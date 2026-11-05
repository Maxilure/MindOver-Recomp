// =============================================================================
// ui_look.h -- the look of the launcher and the installer (dark, Crash's orange)
// =============================================================================
//
// Both windows are ImGui on SDL's 2D renderer. This is what they share: the
// colours, the desktop's fonts (Segoe UI / Consolas on Windows, fontconfig's
// sans / monospace on Linux; ImGui's own pixel font if none), the style
// (rounded, roomy, scaled by the display's DPI) and two small widgets.
// =============================================================================
#pragma once

#include <imgui.h>

namespace ui {

inline const ImVec4 kOrange{0.96f, 0.55f, 0.13f, 1.00f};
inline const ImVec4 kOrangeHover{1.00f, 0.65f, 0.25f, 1.00f};
inline const ImVec4 kOrangeActive{0.85f, 0.45f, 0.08f, 1.00f};
inline const ImVec4 kMuted{0.62f, 0.64f, 0.70f, 1.00f};
inline const ImVec4 kGood{0.45f, 0.82f, 0.45f, 1.00f};
inline const ImVec4 kBad{1.00f, 0.42f, 0.38f, 1.00f};
inline const ImVec4 kWarn{1.00f, 0.80f, 0.35f, 1.00f};

struct Fonts {
  ImFont* regular = nullptr;
  ImFont* bold = nullptr;
  ImFont* mono = nullptr;  // logs
};

Fonts LoadFonts();             // after ImGui::CreateContext
void ApplyStyle(float scale);  // scale = the display's content scale (DPI)
bool AccentButton(const char* label, ImVec2 size = {0, 0});  // big orange button
void MutedText(const char* text);                            // grey, wrapped

}  // namespace ui
