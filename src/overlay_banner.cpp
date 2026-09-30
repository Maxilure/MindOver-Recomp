// overlay_banner.cpp -- see overlay_banner.h.
#include "overlay_banner.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <utility>

#include <imgui.h>

#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

namespace overlay_banner {
namespace {

using Clock = std::chrono::steady_clock;

// How long a message stays, and how long its fade in / fade out take.
constexpr float kShowSeconds = 1.5f;
constexpr float kFadeInSeconds = 0.12f;
constexpr float kFadeOutSeconds = 0.4f;

class BannerDialog : public rex::ui::ImGuiDialog {
 public:
  explicit BannerDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

  void Set(std::string title, std::string detail) {
    std::lock_guard<std::mutex> lock(mutex_);
    title_ = std::move(title);
    detail_ = std::move(detail);
    since_ = Clock::now();
  }

  // Repaint continuously only while a message is up (see the header).
  bool WantsContinuousRepaint() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return since_.has_value();
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    std::string title, detail;
    float age = 0.0f;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!since_) {
        return;
      }
      age = std::chrono::duration<float>(Clock::now() - *since_).count();
      if (age >= kShowSeconds) {
        since_.reset();  // done: back to drawing nothing (and no repaints)
        return;
      }
      title = title_;
      detail = detail_;
    }
    // Fade envelope: quick in, slower out.
    float alpha = 1.0f;
    if (age < kFadeInSeconds) {
      alpha = age / kFadeInSeconds;
    } else if (age > kShowSeconds - kFadeOutSeconds) {
      alpha = (kShowSeconds - age) / kFadeOutSeconds;
    }
    alpha = std::clamp(alpha, 0.0f, 1.0f);

    // Top centre, sized to the text (AlwaysAutoResize), pivot = its top middle.
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 24.0f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.75f * alpha);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_AlwaysAutoResize;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f, 10.0f));
    if (ImGui::Begin("##crashmom_banner", nullptr, flags)) {
      // Title twice the normal size (the SDK's font is small), detail as is.
      // ImGui 1.92 renders fonts at any size on demand: PushFont(nullptr,
      // size) = the current font, crisp at that size.
      ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.0f);
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.25f, alpha));  // Crash orange-yellow
      ImGui::TextUnformatted(title.c_str());
      ImGui::PopStyleColor();
      ImGui::PopFont();
      if (!detail.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.9f, 0.9f, alpha));
        ImGui::TextUnformatted(detail.c_str());
        ImGui::PopStyleColor();
      }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
  }

 private:
  mutable std::mutex mutex_;
  std::string title_, detail_;
  std::optional<Clock::time_point> since_;  // empty = nothing showing
};

// The one dialog, kept for the whole session: the SDK's drawer doesn't own
// its dialogs, and the process ends right after the drawer goes (nothing
// calls Show() then: the renderer, its only caller at runtime, goes first).
std::mutex g_mutex;
BannerDialog* g_dialog = nullptr;
// A message shown before the dialog exists (the renderer announces the
// starting picture in OnPostSetup, which the SDK runs BEFORE OnCreateDialogs):
// kept and shown once the dialog is created.
std::optional<std::pair<std::string, std::string>> g_pending;

}  // namespace

void Create(rex::ui::ImGuiDrawer* drawer) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (drawer && !g_dialog) {
    g_dialog = new BannerDialog(drawer);  // registers itself with the drawer
    if (g_pending) {
      g_dialog->Set(std::move(g_pending->first), std::move(g_pending->second));
      g_pending.reset();
    }
  }
}

void Show(std::string title, std::string detail) {
  // Also in the log: a record of every switch, and play.sh's live console.
  REXLOG_INFO("Banner: {}{}{}", title, detail.empty() ? "" : " -- ", detail);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_dialog) {
    g_dialog->Set(std::move(title), std::move(detail));
  } else {
    g_pending.emplace(std::move(title), std::move(detail));
  }
}

}  // namespace overlay_banner
