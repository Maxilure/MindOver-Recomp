// =============================================================================
// desktop_entry.cpp -- see desktop_entry.h
// =============================================================================
#include "desktop_entry.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace desktop_entry {
namespace {

constexpr const char* kFileName = "crash_mom_launcher.desktop";

// A path as one Exec argument: in double quotes, with ", `, $ and \ escaped
// by a backslash; and % doubled (the spec's field codes start with %).
std::string ExecQuote(const std::string& path) {
  std::string out = "\"";
  for (char c : path) {
    if (c == '"' || c == '`' || c == '$' || c == '\\') {
      out += '\\';
    }
    out += c;
    if (c == '%') {
      out += '%';
    }
  }
  return out + "\"";
}

// The Exec line the entry should have for this launcher.
std::string ExecLine(const fs::path& launcher) {
  return "Exec=" + ExecQuote(launcher.string());
}

// Tells the desktop the menu changed, where the tool exists (most menus
// notice new files by themselves; this makes sure). Quiet, waited for.
void RefreshMenus(const fs::path& folder) {
  const pid_t pid = fork();
  if (pid == 0) {
    const int null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
      dup2(null_fd, STDOUT_FILENO);
      dup2(null_fd, STDERR_FILENO);
    }
    close_range(3, ~0U, 0);
    execlp("update-desktop-database", "update-desktop-database", folder.c_str(),
           static_cast<char*>(nullptr));
    _exit(127);
  }
  if (pid > 0) {
    int status = 0;
    waitpid(pid, &status, 0);
  }
}

}  // namespace

fs::path EntryPath() {
  const char* data_home = std::getenv("XDG_DATA_HOME");
  fs::path base;
  if (data_home && *data_home) {
    base = data_home;
  } else {
    const char* home = std::getenv("HOME");
    base = fs::path(home ? home : ".") / ".local" / "share";
  }
  return base / "applications" / kFileName;
}

fs::path IconFile(const fs::path& game_folder) {
  std::error_code ec;
  for (const fs::path& candidate :
       {game_folder / "assets" / "icon" / "crash_mom.png",
        game_folder / "program" / "assets" / "icon" / "crash_mom.png"}) {
    if (fs::is_regular_file(candidate, ec)) {
      return candidate;
    }
  }
  return {};
}

Status Check(const fs::path& launcher) {
  std::ifstream in(EntryPath());
  if (!in) {
    return Status::kMissing;
  }
  const std::string wanted = ExecLine(launcher);
  std::string line;
  while (std::getline(in, line)) {
    if (line == wanted) {
      return Status::kCurrent;
    }
  }
  return Status::kOutdated;  // ours, but for a launcher somewhere else
}

bool Write(const fs::path& launcher, const fs::path& game_folder, std::string* error) {
  const fs::path entry = EntryPath();
  std::error_code ec;
  fs::create_directories(entry.parent_path(), ec);
  const fs::path icon = IconFile(game_folder);

  std::ostringstream text;
  text << "[Desktop Entry]\n"
       << "Type=Application\n"
       << "Version=1.5\n"
       << "Name=Crash: Mind over Mutant\n"
       << "GenericName=Game\n"
       << "Comment=PC port (alpha): play, continue a save, settings\n"
       << ExecLine(launcher) << "\n"
       << "Path=" << game_folder.string() << "\n"
       << "Icon=" << (icon.empty() ? std::string("applications-games") : icon.string()) << "\n"
       << "Terminal=false\n"
       << "Categories=Game;ActionGame;\n"
       << "Keywords=crash;bandicoot;mind;mutant;\n"
       << "StartupWMClass=crash_mom_launcher\n"
       << "Actions=continue;\n"
       << "\n"
       << "[Desktop Action continue]\n"
       << "Name=Continue last save\n"
       << ExecLine(launcher) << " --play=last\n";

  const fs::path temp = entry.string() + ".tmp";
  {
    std::ofstream out(temp, std::ios::trunc);
    out << text.str();
    if (!out) {
      *error = "Couldn't write " + temp.string();
      return false;
    }
  }
  fs::rename(temp, entry, ec);
  if (ec) {
    *error = "Couldn't write " + entry.string() + ": " + ec.message();
    return false;
  }
  RefreshMenus(entry.parent_path());
  return true;
}

bool AddOnFirstStart(const fs::path& launcher, const fs::path& game_folder,
                     const fs::path& user_folder) {
  const fs::path state = user_folder / "launcher.toml";
  {
    std::ifstream in(state);
    std::string line;
    while (std::getline(in, line)) {
      if (line.starts_with("menu_entry_added") && line.find("true") != std::string::npos) {
        return false;  // done before (the player may have removed it since)
      }
    }
  }
  std::string error;
  const bool added = Check(launcher) == Status::kMissing && Write(launcher, game_folder, &error);
  // Noted even when it was already there or failed: first start = one try.
  std::error_code ec;
  fs::create_directories(user_folder, ec);
  std::ofstream out(state, std::ios::app);
  out << "# The launcher's own notes (not game settings).\n"
      << "menu_entry_added = true  # added to the applications menu at its first start\n";
  return added;
}

bool Remove(std::string* error) {
  const fs::path entry = EntryPath();
  std::error_code ec;
  fs::remove(entry, ec);
  if (ec) {
    *error = "Couldn't remove " + entry.string() + ": " + ec.message();
    return false;
  }
  RefreshMenus(entry.parent_path());
  return true;
}

}  // namespace desktop_entry
