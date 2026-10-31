// =============================================================================
// ui/imgui_fonts.cpp -- see imgui_fonts.h
// =============================================================================
#include "imgui_fonts.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <imgui.h>
#include <rex/logging.h>

namespace imgui_fonts {

namespace {

// Where a sans font with Cyrillic usually is.
std::vector<std::filesystem::path> Candidates() {
  std::vector<std::filesystem::path> out;
#if defined(_WIN32)
  const char* windir = std::getenv("WINDIR");
  const std::filesystem::path fonts = std::filesystem::path(windir ? windir : "C:\\Windows") / "Fonts";
  out.push_back(fonts / "segoeui.ttf");
  out.push_back(fonts / "arial.ttf");
#else
  for (const char* p : {
           "/usr/share/fonts/noto/NotoSans-Regular.ttf",                // Arch
           "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",       // Debian / Ubuntu
           "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",         // Fedora
           "/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf",        // Fedora (variable)
           "/usr/share/fonts/TTF/DejaVuSans.ttf",                       // Arch
           "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",           // Debian / Ubuntu
           "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",         // Fedora
       }) {
    out.emplace_back(p);
  }
#endif
  return out;
}

}  // namespace

void AddExtraLetters(ImFontAtlas* atlas) {
  // The SDK's pixel font is 10 px; a slightly bigger outline font matches its
  // letter height best.
  static const ImWchar kRanges[] = {
      0x0100, 0x017F,  // Latin Extended-A (ł ř ő ...)
      0x0370, 0x03FF,  // Greek
      0x0400, 0x04FF,  // Cyrillic
      0x2010, 0x2026,  // dashes, quotes, ellipsis
      0,
  };
  std::error_code ec;
  for (const auto& path : Candidates()) {
    if (!std::filesystem::is_regular_file(path, ec)) continue;
    ImFontConfig config;
    config.MergeMode = true;
    config.OversampleH = config.OversampleV = 1;
    config.PixelSnapH = true;
    if (atlas->AddFontFromFileTTF(path.string().c_str(), 12.0f, &config, kRanges)) {
      REXLOG_INFO("ImGui: extra letters (Cyrillic, Latin Extended) from {}", path.string());
      return;
    }
  }
  REXLOG_WARN("ImGui: no system font with Cyrillic found: those letters draw as boxes in the tool windows");
}

}  // namespace imgui_fonts
