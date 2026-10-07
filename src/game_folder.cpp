// =============================================================================
// game_folder.cpp -- see game_folder.h (the folder layout, first-start copies)
// =============================================================================

#include "game_folder.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <SDL3/SDL_messagebox.h>
#include <fmt/format.h>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>

namespace game_folder {
namespace fs = std::filesystem;

namespace {

fs::path g_root;              // the game folder
fs::path g_user;              // <game folder>/user
fs::path g_saves;             // the saves (user/saves or --user_data_root)
std::vector<std::string> g_notes;  // what Configure did, logged once logging is up

// Is `path` inside `folder` (or the folder itself)? Both made absolute and
// normalized first, so "out/build/../.." tricks compare correctly.
bool IsInside(const fs::path& path, const fs::path& folder) {
  std::error_code ec;
  const fs::path p = fs::weakly_canonical(path, ec);
  const fs::path f = fs::weakly_canonical(folder, ec);
  auto pi = p.begin();
  for (auto fi = f.begin(); fi != f.end(); ++fi, ++pi) {
    if (fi->empty()) continue;  // trailing separator of the folder
    if (pi == p.end() || *pi != *fi) return false;
  }
  return true;
}

// The three rules of game_folder.h, in order.
fs::path FindGameFolder() {
  const fs::path exe_dir = rex::filesystem::GetExecutableFolder();
  if (exe_dir.filename() == "program") {
    return exe_dir.parent_path();  // 1. installed
  }
#ifdef CRASHMOM_SOURCE_DIR
  // CMakeLists.txt passes the source tree's path at build time. Only used
  // while the exe still lives inside it: a build copied elsewhere falls
  // through to rule 3.
  const fs::path source = CRASHMOM_SOURCE_DIR;
  if (IsInside(exe_dir, source)) {
    return source;  // 2. built from source
  }
#endif
  return exe_dir;  // 3. anywhere else
}

// Can we create files in `folder`? (Creates it first.) A real test write:
// permission bits alone don't tell (read-only mounts, Windows ACLs).
bool Writable(const fs::path& folder) {
  std::error_code ec;
  fs::create_directories(folder, ec);
  const fs::path probe = folder / ".write_test";
  {
    std::ofstream out(probe, std::ios::binary);
    if (!out || !(out << "ok")) return false;
  }
  fs::remove(probe, ec);
  return true;
}

// Copies a folder tree, KEEPING each file's modification date (std::filesystem
// ::copy doesn't): the save list orders saves by when they were last written,
// so fresh dates would reshuffle it. `skip` = top-level names left out.
// Returns the number of files copied, or -1 on an error.
int CopyTree(const fs::path& from, const fs::path& to, const std::vector<std::string>& skip) {
  std::error_code ec;
  int files = 0;
  fs::create_directories(to, ec);
  for (auto it = fs::recursive_directory_iterator(from, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    const fs::path rel = fs::relative(it->path(), from, ec);
    if (it.depth() == 0) {
      bool skipped = false;
      for (const auto& name : skip) skipped |= rel == name;
      if (skipped) {
        if (it->is_directory()) it.disable_recursion_pending();
        continue;
      }
    }
    const fs::path dest = to / rel;
    if (it->is_directory(ec)) {
      fs::create_directories(dest, ec);
    } else if (it->is_regular_file(ec)) {
      fs::copy_file(it->path(), dest, fs::copy_options::skip_existing, ec);
      if (ec) return -1;
      fs::last_write_time(dest, fs::last_write_time(it->path(), ec), ec);
      ++files;
    }
    if (ec) return -1;
  }
  return ec ? -1 : files;
}

// One file from the old place (next to the exe) if the new one doesn't exist.
void CopyOldFile(const fs::path& old_file, const fs::path& new_file) {
  std::error_code ec;
  if (fs::exists(new_file, ec) || !fs::exists(old_file, ec)) return;
  fs::copy_file(old_file, new_file, ec);
  g_notes.push_back(ec ? fmt::format("could not copy {} to {}: {}", old_file.string(),
                                     new_file.string(), ec.message())
                       : fmt::format("first start: copied {} to {} (the old file stays)",
                                     old_file.string(), new_file.string()));
}

}  // namespace

void Configure(rex::PathConfig& paths) {
  g_root = FindGameFolder();
  g_user = g_root / "user";

  if (!Writable(g_user)) {
    // No quiet fallback (game_folder.h). Logging isn't up yet, so a real
    // message box: SDL's (it works before SDL is initialized). The SDK's
    // rex::ShowSimpleMessageBox only prints to stderr on Linux, invisible to
    // a player who started the game from a file manager. stderr gets it too.
    const std::string message =
        fmt::format("Mind over Recomp can't save in its folder:\n\n{}\n\n"
                    "Move the game's folder somewhere you can write to (for example "
                    "Documents or another drive) and start it again.",
                    g_user.string());
    std::fprintf(stderr, "[ERROR] %s\n", message.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Mind over Recomp", message.c_str(),
                             nullptr);
    std::exit(1);
  }

  // Saves: the SDK's user data folder, unless --user_data_root chose one
  // (test runs point it at a COPY of the saves on purpose).
  if (REXCVAR_GET(user_data_root).empty()) {
    const fs::path saves = g_user / "saves";
    std::error_code ec;
    if (!fs::exists(saves, ec)) {
      // First start: bring the saves over from the old system folder (the
      // SDK's default, computed the same way it does). Its "cache" is left
      // behind: the new cache/ refills itself.
      const fs::path old_saves = rex::filesystem::GetUserFolder() / "crash_mom";
      if (fs::is_directory(old_saves, ec)) {
        const int copied = CopyTree(old_saves, saves, {"cache"});
        g_notes.push_back(
            copied < 0 ? fmt::format("COULD NOT copy the saves from {} to {}: check both "
                                     "folders (the originals are untouched)",
                                     old_saves.string(), saves.string())
                       : fmt::format("first start: copied {} files of saves from {} to {} "
                                     "(the originals stay there as a backup)",
                                     copied, old_saves.string(), saves.string()));
      }
      fs::create_directories(saves, ec);
    }
    paths.user_data_root = saves;
  }
  g_saves = paths.user_data_root;

  // Shader cache + patched data: rebuildable, so outside user/.
  if (REXCVAR_GET(cache_root).empty()) {
    paths.cache_root = g_root / "cache";
  }

  // Every changed setting (the SDK loads it right after this hook; F4 saves it).
  paths.config_path = g_user / "settings.toml";

  // First start: the files that used to sit next to the exe.
  const fs::path exe_dir = rex::filesystem::GetExecutableFolder();
  CopyOldFile(exe_dir / "crash_mom.toml", paths.config_path);
  CopyOldFile(exe_dir / "controls.toml", g_user / "controls.toml");
}

void ConfigureLogging(rex::LogConfig& config) {
  if (!g_user.empty()) config.log_dir = g_user / "logs";
}

void LogWhatHappened() {
  REXLOG_INFO("Game folder: {} (your files: {})", g_root.string(), g_user.string());
  for (const auto& note : g_notes) {
    REXLOG_INFO("Game folder: {}", note);
  }
  REXLOG_INFO("Game folder: saves in {}", g_saves.string());
}

fs::path UserFolder() { return g_user; }

}  // namespace game_folder
