// =============================================================================
// ui_look.cpp -- see ui_look.h
// =============================================================================
#include "ui_look.h"

#include <string>

#include "platform.h"

namespace ui {

// -----------------------------------------------------------------------------
// Look
// -----------------------------------------------------------------------------

// Colours: dark, with Crash's orange as the accent.


Fonts LoadFonts() {
  ImGuiIO& io = ImGui::GetIO();
  Fonts fonts;
  // The desktop's fonts (Linux: fontconfig's sans / monospace, e.g. Noto
  // Sans; Windows: Segoe UI / Consolas); ImGui's own small pixel font if none.
  const platform::FontFiles files = platform::SystemFonts();
  const std::string regular = files.regular.string();
  const std::string bold = files.bold.string();
  if (!regular.empty()) {
    fonts.regular = io.Fonts->AddFontFromFileTTF(regular.c_str(), 17.0f);
  }
  if (!fonts.regular) {
    fonts.regular = io.Fonts->AddFontDefault();
  }
  if (!bold.empty()) {
    fonts.bold = io.Fonts->AddFontFromFileTTF(bold.c_str(), 17.0f);
  }
  if (!fonts.bold) {
    fonts.bold = fonts.regular;
  }
  const std::string mono = files.mono.string();
  if (!mono.empty()) {
    fonts.mono = io.Fonts->AddFontFromFileTTF(mono.c_str(), 15.0f);
  }
  if (!fonts.mono) {
    fonts.mono = fonts.regular;
  }
  return fonts;
}

void ApplyStyle(float scale) {
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.FontSizeBase = 17.0f;  // unscaled; FontScaleDpi below scales it
  style.WindowPadding = {28, 24};
  style.FramePadding = {12, 7};
  style.ItemSpacing = {10, 9};
  style.CellPadding = {10, 2};
  style.FrameRounding = 6;
  style.ChildRounding = 8;
  style.PopupRounding = 8;
  style.GrabRounding = 6;
  style.WindowBorderSize = 0;
  style.ChildBorderSize = 1;
  ImVec4* c = style.Colors;
  c[ImGuiCol_WindowBg] = {0.09f, 0.09f, 0.11f, 1.0f};
  c[ImGuiCol_ChildBg] = {0.12f, 0.12f, 0.15f, 1.0f};
  c[ImGuiCol_PopupBg] = {0.13f, 0.13f, 0.16f, 1.0f};
  c[ImGuiCol_Border] = {0.22f, 0.22f, 0.27f, 1.0f};
  c[ImGuiCol_FrameBg] = {0.17f, 0.17f, 0.21f, 1.0f};
  c[ImGuiCol_FrameBgHovered] = {0.22f, 0.22f, 0.27f, 1.0f};
  c[ImGuiCol_FrameBgActive] = {0.25f, 0.25f, 0.31f, 1.0f};
  c[ImGuiCol_Button] = {0.20f, 0.20f, 0.25f, 1.0f};
  c[ImGuiCol_ButtonHovered] = {0.27f, 0.27f, 0.33f, 1.0f};
  c[ImGuiCol_ButtonActive] = {0.32f, 0.32f, 0.39f, 1.0f};
  c[ImGuiCol_Header] = {0.96f, 0.55f, 0.13f, 0.22f};
  c[ImGuiCol_HeaderHovered] = {0.96f, 0.55f, 0.13f, 0.32f};
  c[ImGuiCol_HeaderActive] = {0.96f, 0.55f, 0.13f, 0.45f};
  c[ImGuiCol_TableHeaderBg] = {0.15f, 0.15f, 0.19f, 1.0f};
  c[ImGuiCol_TableRowBgAlt] = {1.0f, 1.0f, 1.0f, 0.025f};
  c[ImGuiCol_NavCursor] = kOrange;
  c[ImGuiCol_CheckMark] = kOrange;
  c[ImGuiCol_TextSelectedBg] = {0.96f, 0.55f, 0.13f, 0.35f};
  style.ScaleAllSizes(scale);
  style.FontScaleDpi = scale;
}

// A big orange button (Play / Continue).
bool AccentButton(const char* label, ImVec2 size) {
  ImGui::PushStyleColor(ImGuiCol_Button, kOrange);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kOrangeHover);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, kOrangeActive);
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.08f, 0.06f, 0.04f, 1.0f});
  const bool pressed = ImGui::Button(label, size);
  ImGui::PopStyleColor(4);
  return pressed;
}

void MutedText(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
}

}  // namespace ui
