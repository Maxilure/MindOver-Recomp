// =============================================================================
// net/network_window.cpp -- the Network window (net/network_window.h)
// =============================================================================

#include "network_window.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>

#include <imgui.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include <fmt/format.h>

#include "net_session.h"

namespace network_window {
namespace {

const ImVec4 kTitleColour(1.0f, 0.85f, 0.25f, 1.0f);  // the other tools' yellow
const ImVec4 kGood(0.45f, 0.95f, 0.45f, 1.0f);
const ImVec4 kBad(1.0f, 0.40f, 0.35f, 1.0f);
const ImVec4 kWarn(1.0f, 0.75f, 0.30f, 1.0f);

class NetworkDialog : public rex::ui::ImGuiDialog {
 public:
  explicit NetworkDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {
    std::snprintf(address_, sizeof(address_), "127.0.0.1");
  }

  void Toggle() {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = !open_;
  }

  // While the frame gate waits the game draws nothing new: keep the numbers
  // moving then. Otherwise the game's frames repaint it.
  bool WantsContinuousRepaint() const override {
    return open_shown_.load() && net::GetStatus().waiting_now;
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    std::lock_guard<std::mutex> lock(mutex_);
    open_shown_ = open_;
    if (!open_) return;
    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(std::min(460.0f, io.DisplaySize.x - 20.0f), 0.0f),
                             ImGuiCond_Appearing);
    ImGui::SetNextWindowBgAlpha(0.90f);
    bool still_open = true;
    if (ImGui::Begin("Network###crashmom_network", &still_open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_AlwaysAutoResize)) {
      DrawContents();
    }
    ImGui::End();
    if (!still_open) open_ = false;
  }

 private:
  void DrawContents() {
    const net::Status st = net::GetStatus();
    ImGui::TextColored(kTitleColour, "Network");
    ImGui::SameLine();
    ImGui::TextDisabled("LAN co-op test: 2 PCs, lockstep");

    if (st.role == net::Role::kNone) {
      ImGui::TextWrapped("Both PCs: same version, same world settings, then load the SAME save "
                         "on both once connected (a session starts when the level starts).");
      ImGui::SetNextItemWidth(90);
      ImGui::InputInt("Port", &port_, 0);
      port_ = std::clamp(port_, 1024, 65535);
      if (ImGui::Button("Host (player 1)")) error_ = net::Host(port_);
      ImGui::SameLine();
      ImGui::SetNextItemWidth(150);
      ImGui::InputText("##address", address_, sizeof(address_));
      ImGui::SameLine();
      if (ImGui::Button("Join (player 2)")) error_ = net::Join(address_, port_);
      if (!error_.empty()) ImGui::TextColored(kBad, "%s", error_.c_str());
      if (!st.last_event.empty()) ImGui::TextDisabled("Last: %s", st.last_event.c_str());
      return;
    }

    ImGui::Text("%s, player %d", st.role == net::Role::kHost ? "Host" : "Guest", st.local_player + 1);
    if (!st.alone) {
      ImGui::SameLine();
      if (ImGui::Button("Leave")) net::Leave();
      ImGui::SetItemTooltip("In a level: you play on alone, the other player drops out on both PCs.");
    } else {
      ImGui::SameLine();
      ImGui::TextColored(kWarn, st.dropped ? "  alone (the other player dropped out)"
                                           : "  alone (the other player stays a while)");
    }
    ImGui::TextWrapped("%s", st.state.c_str());
    if (!st.peer.empty()) {
      ImGui::Text("Other PC: %s", st.peer.c_str());
      if (st.connected) {
        ImGui::SameLine();
        if (st.ping_ms >= 0) {
          ImGui::Text("  ping %.1f ms", st.ping_ms);
        } else {
          ImGui::TextDisabled("  ping ...");
        }
      }
    }
    ImGui::Text("Input delay: %d frames (%.0f ms)", st.delay, st.delay * 1000.0 / 60.0);

    // World settings.
    if (st.peer_settings_known) {
      if (st.peer_settings == st.my_settings) {
        ImGui::TextColored(kGood, "World settings alike (%016llX)", (unsigned long long)st.my_settings);
      } else {
        ImGui::TextColored(kBad, "WORLD SETTINGS DIFFER: here %016llX, there %016llX",
                           (unsigned long long)st.my_settings, (unsigned long long)st.peer_settings);
        ImGui::TextDisabled("Debug console `settings` lists them. The games will drift apart.");
      }
    } else {
      ImGui::TextDisabled("World settings here: %016llX", (unsigned long long)st.my_settings);
    }

    ImGui::Separator();
    if (!st.in_session) {
      ImGui::TextDisabled(st.connected ? "Connected. Load the same save on both PCs."
                                       : "Waiting for the other PC.");
    } else {
      ImGui::Text("Session frame %lld  (%.1f s)", (long long)st.session_frame, st.session_frame / 60.0);
      if (st.waiting_now) {
        ImGui::TextColored(kWarn, "Waiting for the other PC's input...");
      }
      ImGui::Text("Waited at the gate: %lld frames, %.0f ms in all (last %.1f ms)", (long long)st.waits,
                  st.wait_ms_total, st.last_wait_ms);
      if (st.drift_frame >= 0) {
        ImGui::TextColored(kBad, "DRIFTED at frame %lld", (long long)st.drift_frame);
      } else if (st.checked_through >= 0) {
        ImGui::TextColored(kGood, "In sync (checked through frame %lld)", (long long)st.checked_through);
      } else {
        ImGui::TextDisabled("Not compared yet");
      }
      if (ImGui::Button("Dump both sides")) dump_reply_ = net::RequestDump();
      ImGui::SetItemTooltip("Both PCs write the whole world data at the same session frame (2 s from\n"
                            "now) to user/logs/net-dump-player<N>-frame<F>.whash.\n"
                            "Compare: tools/world_hash_compare.py a b --dump-a --dump-b");
      if (!dump_reply_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", dump_reply_.c_str());
      }
    }
    if (!st.last_event.empty()) ImGui::TextDisabled("Last: %s", st.last_event.c_str());
  }

  std::mutex mutex_;
  bool open_ = false;
  std::atomic<bool> open_shown_{false};
  int port_ = 37373;
  char address_[128] = {};
  std::string error_, dump_reply_;
};

NetworkDialog* g_dialog = nullptr;  // kept for the whole session

}  // namespace

void Create(rex::ui::ImGuiDrawer* drawer) {
  if (drawer && !g_dialog) g_dialog = new NetworkDialog(drawer);
}

void Toggle() {
  if (g_dialog) g_dialog->Toggle();
}

}  // namespace network_window
