// =============================================================================
// folders.cpp -- see folders.h
// =============================================================================
#include "folders.h"

#include <cstdlib>
#include <system_error>

#include <unistd.h>

namespace fs = std::filesystem;

namespace folders {
namespace {

// The launcher's own folder (/proc/self/exe = the running executable).
fs::path LauncherFolder() {
  std::error_code ec;
  const fs::path self = fs::read_symlink("/proc/self/exe", ec);
  return ec ? fs::current_path() : self.parent_path();
}

// A folder's layout, if it is a game folder (see folders.h).
bool Fill(const fs::path& root, Folders& out) {
  std::error_code ec;
  if (fs::is_regular_file(root / "program" / "crash_mom", ec)) {
    out.exe = root / "program" / "crash_mom";
    out.from_source = false;
  } else if (fs::is_regular_file(root / "crash_mom_manifest.toml", ec)) {
    // The build folder the docs (docs/01-building.md) and tools/play.sh use.
    out.exe = root / "out" / "build" / "linux-amd64-relwithdebinfo" / "crash_mom";
    out.from_source = true;
  } else {
    return false;
  }
  out.root = root;
  out.disc = root / "game";
  out.user = root / "user";
  return true;
}

}  // namespace

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
  const char* home = std::getenv("HOME");
  if (home && *home) {
    const std::string prefix = std::string(home) + "/";
    if (text.starts_with(prefix)) {
      return "~/" + text.substr(prefix.size());
    }
  }
  return text;
}

}  // namespace folders
