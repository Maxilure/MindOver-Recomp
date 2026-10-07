// =============================================================================
// desktop_entry.cpp -- see desktop_entry.h
// =============================================================================
#include "desktop_entry.h"

#include "platform.h"

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
// (after windows.h:)
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif

#include <cstdlib>
#include <fstream>
#include <sstream>


namespace fs = std::filesystem;

namespace desktop_entry {
#if !defined(_WIN32)

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
  if (!platform::FindProgram("update-desktop-database").empty()) {
    bool ok = false;
    platform::Capture({"update-desktop-database", folder.string()}, &ok);
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

#endif  // !_WIN32

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

#if !defined(_WIN32)

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
       << "Name=Mind over Recomp\n"
       << "GenericName=Game\n"
       << "Comment=Unofficial PC port of Crash: Mind over Mutant (alpha)\n"
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

#endif  // !_WIN32

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


#if defined(_WIN32)

// --- Windows: Start menu shortcuts --------------------------------------------
//   %APPDATA%\Microsoft\Windows\Start Menu\Programs\
//     Mind over Recomp.lnk                       the launcher
//     Mind over Recomp (continue last save).lnk  the launcher --play=last
// (Windows has no right-click actions on a Start menu entry like Linux's
// .desktop "Actions", so the shortcut to the last save is a second entry.)
// The port is named after the PROJECT, not the game: 0.1.0-alpha's first
// build called the shortcuts "Crash Mind over Mutant ...". Those old names
// are removed whenever the shortcuts are written or removed (kOldNames).

namespace {

fs::path StartMenuPrograms() {
  PWSTR path = nullptr;
  fs::path result;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &path))) {
    result = path;
  }
  CoTaskMemFree(path);
  return result;
}

fs::path ContinuePath() {
  return StartMenuPrograms() / L"Mind over Recomp (continue last save).lnk";
}

// Shortcuts by an earlier name of the port (see the header comment).
constexpr const wchar_t* kOldNames[] = {L"Crash Mind over Mutant.lnk",
                                        L"Crash Mind over Mutant (continue last save).lnk"};

bool OldShortcutsThere() {
  std::error_code ec;
  for (const wchar_t* name : kOldNames) {
    if (fs::exists(StartMenuPrograms() / name, ec)) return true;
  }
  return false;
}

void RemoveOldShortcuts() {
  std::error_code ec;
  for (const wchar_t* name : kOldNames) fs::remove(StartMenuPrograms() / name, ec);
}

// Writes one .lnk (COM's IShellLink + IPersistFile).
bool WriteShortcut(const fs::path& lnk, const fs::path& target, const std::wstring& arguments,
                   const fs::path& work, const std::wstring& description, const fs::path& icon) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  IShellLinkW* link = nullptr;
  bool ok = false;
  if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                 reinterpret_cast<void**>(&link)))) {
    link->SetPath(target.wstring().c_str());
    link->SetArguments(arguments.c_str());
    link->SetWorkingDirectory(work.wstring().c_str());
    link->SetDescription(description.c_str());
    if (!icon.empty()) link->SetIconLocation(icon.wstring().c_str(), 0);
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)))) {
      ok = SUCCEEDED(file->Save(lnk.wstring().c_str(), TRUE));
      file->Release();
    }
    link->Release();
  }
  CoUninitialize();
  return ok;
}

// The program a .lnk points at (empty if none / unreadable).
fs::path ShortcutTarget(const fs::path& lnk) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  fs::path result;
  IShellLinkW* link = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                 reinterpret_cast<void**>(&link)))) {
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)))) {
      if (SUCCEEDED(file->Load(lnk.wstring().c_str(), STGM_READ))) {
        wchar_t path[MAX_PATH * 4] = {};
        if (SUCCEEDED(link->GetPath(path, int(std::size(path)), nullptr, SLGP_RAWPATH))) {
          result = path;
        }
      }
      file->Release();
    }
    link->Release();
  }
  CoUninitialize();
  return result;
}

}  // namespace

fs::path EntryPath() { return StartMenuPrograms() / L"Mind over Recomp.lnk"; }

Status Check(const fs::path& launcher) {
  std::error_code ec;
  if (!fs::exists(EntryPath(), ec)) {
    // Only the old-named shortcuts: "outdated", so the button rewrites them.
    return OldShortcutsThere() ? Status::kOutdated : Status::kMissing;
  }
  const fs::path target = ShortcutTarget(EntryPath());
  return fs::equivalent(target, launcher, ec) ? Status::kCurrent : Status::kOutdated;
}

bool Write(const fs::path& launcher, const fs::path& game_folder, std::string* error) {
  const fs::path icon = IconFile(game_folder);  // a .png can't be a Windows icon: the exe's own
  (void)icon;
  std::error_code ec;
  fs::create_directories(EntryPath().parent_path(), ec);
  RemoveOldShortcuts();
  if (!WriteShortcut(EntryPath(), launcher, L"", game_folder,
                     L"Mind over Recomp (alpha), an unofficial PC port of Crash: Mind over Mutant",
                     launcher)) {
    *error = "Couldn't write the Start menu shortcut.";
    return false;
  }
  WriteShortcut(ContinuePath(), launcher, L"--play=last", game_folder,
                L"Mind over Recomp: straight into the most recently played save", launcher);
  return true;
}

bool RenameIfOld(const fs::path& launcher, const fs::path& game_folder) {
  std::error_code ec;
  if (!OldShortcutsThere() ||
      !fs::equivalent(ShortcutTarget(StartMenuPrograms() / kOldNames[0]), launcher, ec)) {
    return false;
  }
  std::string error;
  return Write(launcher, game_folder, &error);  // removes the old-named ones too
}

bool Remove(std::string* error) {
  std::error_code ec;
  RemoveOldShortcuts();
  fs::remove(EntryPath(), ec);
  if (ec) {
    *error = "Couldn't remove the Start menu shortcut: " + ec.message();
    return false;
  }
  fs::remove(ContinuePath(), ec);
  return true;
}

#else  // Linux

bool RenameIfOld(const fs::path& launcher, const fs::path& game_folder) {
  std::ifstream in(EntryPath());
  if (!in) {
    return false;
  }
  const std::string wanted = ExecLine(launcher);
  bool ours = false, old_name = false;
  std::string line;
  while (std::getline(in, line)) {
    ours = ours || line == wanted;
    old_name = old_name || line == "Name=Crash: Mind over Mutant";
  }
  in.close();
  std::string error;
  return ours && old_name && Write(launcher, game_folder, &error);
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

#endif  // _WIN32

}  // namespace desktop_entry
