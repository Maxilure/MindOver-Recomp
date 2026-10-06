// =============================================================================
// folders.cpp -- see folders.h
// =============================================================================
#include "folders.h"

#include "platform.h"

#include <cstdlib>
#include <system_error>


namespace fs = std::filesystem;

namespace folders {
namespace {

// The launcher's own folder.
fs::path LauncherFolder() {
  const fs::path self = platform::SelfPath();
  return self.empty() ? fs::current_path() : self.parent_path();
}

// The build preset's folder for this system (CMakePresets.json).
#if defined(_WIN32)
constexpr const char* kBuildPreset = "win-amd64-relwithdebinfo";
#else
constexpr const char* kBuildPreset = "linux-amd64-relwithdebinfo";
#endif

// A folder's layout, if it is a game folder (see folders.h).
bool Fill(const fs::path& root, Folders& out) {
  std::error_code ec;
  const std::string exe = platform::ExeName("crash_mom");
  if (fs::is_regular_file(root / "source" / "crash_mom_manifest.toml", ec)) {
    // A release: built in source/, played from program/ (Setup copies it).
    out.source = root / "source";
    out.exe = root / "program" / exe;
    out.from_source = true;
    out.release = true;
  } else if (fs::is_regular_file(root / "crash_mom_manifest.toml", ec)) {
    // A developer clone: played straight from the build folder the docs
    // (docs/01-building.md) and tools/play.sh use.
    out.source = root;
    out.exe = root / "out" / "build" / kBuildPreset / exe;
    out.from_source = true;
    out.release = false;
  } else if (fs::is_regular_file(root / "program" / exe, ec)) {
    out.source.clear();
    out.exe = root / "program" / exe;
    out.from_source = false;
    out.release = false;
  } else {
    return false;
  }
  out.root = root;
  out.disc = root / "game";
  out.user = root / "user";
  return true;
}

}  // namespace

fs::path Folders::BuiltExe() const {
  return source.empty() ? fs::path()
                        : source / "out" / "build" / kBuildPreset / platform::ExeName("crash_mom");
}

bool Folders::ExeExists() const {
  std::error_code ec;
  return !exe.empty() && fs::is_regular_file(exe, ec);
}

bool Folders::DiscExists() const {
  std::error_code ec;
  return !disc.empty() && fs::is_regular_file(disc / "default.xex", ec);
}

Folders Find(const fs::path& override_root) {
  Folders found;
  if (!override_root.empty()) {
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(override_root, ec);
    Fill(ec ? override_root : root, found);
    return found;
  }
  for (fs::path dir = LauncherFolder(); !dir.empty(); dir = dir.parent_path()) {
    if (Fill(dir, found)) {
      return found;
    }
    if (dir == dir.root_path()) {
      break;
    }
  }
  return found;
}

std::string Pretty(const fs::path& path) {
  const std::string text = path.string();
  const std::string home = platform::HomeDir().string();
  if (!home.empty()) {
    const std::string prefix = home + std::string(1, fs::path::preferred_separator);
    if (text.starts_with(prefix)) {
#if defined(_WIN32)
      return text;  // Windows users read full paths (no "~")
#else
      return "~/" + text.substr(prefix.size());
#endif
    }
  }
  return text;
}

}  // namespace folders
