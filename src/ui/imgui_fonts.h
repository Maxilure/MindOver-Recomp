// =============================================================================
// ui/imgui_fonts.h -- letters beyond Latin-1 in the ImGui windows
// =============================================================================
//
// WHY: the developer windows (F1 Tools, F5 Cheats, F6 Controls, the
// renderer banners) are ImGui, and their texts are translated like the rest
// of the port (language/texts.h). The SDK's ImGui font is a small pixel font
// with only Basic Latin + Latin-1 (imgui_drawer.cpp): Russian, and letters
// like ł ř ő, drew as "?" boxes.
//
// HOW: ReXApp's font hook (OnConfigureFonts, after the SDK's own font) MERGES
// a system font into it for Latin Extended-A, Greek and Cyrillic only: the
// pixel font keeps every letter it already has. The system font is the
// first that exists of a short list (Linux: Noto Sans / DejaVu Sans;
// Windows: Segoe UI). None found = a log line, and the boxes stay.
// =============================================================================
#pragma once

struct ImFontAtlas;

namespace imgui_fonts {

// From CrashMomApp::OnConfigureFonts.
void AddExtraLetters(ImFontAtlas* atlas);

}  // namespace imgui_fonts
