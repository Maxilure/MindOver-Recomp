// =============================================================================
// install.cpp -- see install.h
// =============================================================================
#include "install.h"

#include "folders.h"
#include "platform.h"
#include "texts.h"

#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

#if defined(_WIN32)

#define NOMINMAX
#include <windows.h>
// (after windows.h:)
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cwctype>

namespace install {
namespace {

constexpr const wchar_t* kUninstallRoot = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\";
constexpr const wchar_t* kCMakePackages = L"Software\\Kitware\\CMake\\Packages\\rexglue";

std::wstring LowerW(std::wstring s) {
  std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
  return s;
}

std::wstring Wide(const std::string& text) {
  if (text.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
  std::wstring out(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n);
  return out;
}

// A path Windows takes past 260 characters: "\\?\C:\..." (no "..", "\" only).
std::wstring LongPath(const fs::path& path) {
  std::error_code ec;
  std::wstring p = fs::absolute(path, ec).lexically_normal().wstring();
  std::replace(p.begin(), p.end(), L'/', L'\\');
  while (p.size() > 3 && p.back() == L'\\') p.pop_back();
  if (p.starts_with(L"\\\\?\\")) return p;
  if (p.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + p.substr(2);
  return L"\\\\?\\" + p;
}

// Is `inner` inside `outer` (or the same)? Case-insensitive, by text.
bool Inside(const fs::path& inner, const fs::path& outer) {
  std::error_code ec;
  std::wstring a = LowerW(fs::absolute(inner, ec).lexically_normal().wstring());
  std::wstring b = LowerW(fs::absolute(outer, ec).lexically_normal().wstring());
  std::replace(a.begin(), a.end(), L'/', L'\\');
  std::replace(b.begin(), b.end(), L'/', L'\\');
  while (b.size() > 3 && b.back() == L'\\') b.pop_back();
  return a == b || a.starts_with(b + L"\\");
}

fs::path KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR path = nullptr;
  fs::path result;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) result = path;
  CoTaskMemFree(path);
  return result;
}

fs::path StartMenu() { return KnownFolder(FOLDERID_Programs); }
fs::path Desktop() { return KnownFolder(FOLDERID_Desktop); }

constexpr const wchar_t* kGameShortcut = L"Crash Mind over Mutant.lnk";
constexpr const wchar_t* kInstallerShortcut = L"Mind over Recomp.lnk";
// 0.1.0-alpha's launcher wrote this one (--play=last); the installer has no
// such option, so a leftover pointing into the folder is removed.
constexpr const wchar_t* kOldContinueShortcut = L"Mind over Recomp (continue last save).lnk";

// Writes one .lnk (COM's IShellLink + IPersistFile), as desktop_entry.cpp.
bool WriteShortcut(const fs::path& lnk, const fs::path& target, const fs::path& work,
                   const std::wstring& description, const fs::path& icon) {
  const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  IShellLinkW* link = nullptr;
  bool ok = false;
  if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                 reinterpret_cast<void**>(&link)))) {
    link->SetPath(target.wstring().c_str());
    link->SetWorkingDirectory(work.wstring().c_str());
    link->SetDescription(description.c_str());
    link->SetIconLocation(icon.wstring().c_str(), 0);
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)))) {
      ok = SUCCEEDED(file->Save(lnk.wstring().c_str(), TRUE));
      file->Release();
    }
    link->Release();
  }
  if (SUCCEEDED(init)) CoUninitialize();
  return ok;
}

// The program a .lnk points at (empty if none / unreadable).
fs::path ShortcutTarget(const fs::path& lnk) {
  const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
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
  if (SUCCEEDED(init)) CoUninitialize();
  return result;
}

void SetString(HKEY key, const wchar_t* name, const std::wstring& value) {
  RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                 DWORD((value.size() + 1) * sizeof(wchar_t)));
}

void SetDword(HKEY key, const wchar_t* name, DWORD value) {
  RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

// The size of everything under `dir` in bytes (not following links).
uint64_t SizeOf(const std::wstring& dir) {
  uint64_t total = 0;
  WIN32_FIND_DATAW data;
  HANDLE find = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &data,
                                 FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
  if (find == INVALID_HANDLE_VALUE) return 0;
  do {
    const std::wstring name = data.cFileName;
    if (name == L"." || name == L"..") continue;
    if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      total += SizeOf(dir + L"\\" + name);
    } else {
      total += (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    }
  } while (FindNextFileW(find, &data));
  FindClose(find);
  return total;
}

// Removes one file or folder tree (see install.h: links as links, read-only
// files too, long paths). A file another program holds open (an antivirus
// scan) gets a few tries. Counts removed files into *files.
bool RemoveTree(const std::wstring& path, uint64_t* files,
                const std::function<void(uint64_t)>& progress, std::wstring* failed) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return true;  // already gone
  }
  auto retry = [](auto&& action) {
    for (int attempt = 0; attempt < 5; ++attempt) {
      if (action()) return true;
      const DWORD error = GetLastError();
      if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return true;
      Sleep(100 * (attempt + 1));
    }
    return false;
  };
  bool ok = true;
  if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
    // A junction / link (source\game): remove the LINK, never what it points at.
    SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
    ok = (attributes & FILE_ATTRIBUTE_DIRECTORY)
             ? retry([&] { return RemoveDirectoryW(path.c_str()) != 0; })
             : retry([&] { return DeleteFileW(path.c_str()) != 0; });
  } else if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileExW((path + L"\\*").c_str(), FindExInfoBasic, &data,
                                   FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find != INVALID_HANDLE_VALUE) {
      do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") continue;
        ok = RemoveTree(path + L"\\" + name, files, progress, failed) && ok;
      } while (FindNextFileW(find, &data));
      FindClose(find);
    }
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    ok = retry([&] { return RemoveDirectoryW(path.c_str()) != 0; }) && ok;
  } else {
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);  // git's read-only objects
    ok = retry([&] { return DeleteFileW(path.c_str()) != 0; });
    if (ok && ++*files % 500 == 0 && progress) progress(*files);
  }
  if (!ok && failed->empty()) *failed = path;
  return ok;
}

}  // namespace

fs::path DefaultParent() {
  const fs::path games = fs::path(platform::GetEnv("USERPROFILE")) / "Games";
  return games;
}

std::string WhyNotHere(const fs::path& root, double needed_gb, bool existing_game) {
  std::error_code ec;
  const fs::path absolute = fs::absolute(root, ec);
  if (absolute.wstring().size() > 90) {
    return trs("That path is too long: the build's files nest deep, and Windows stops at 260 "
               "characters. Pick a folder closer to the drive, like C:\\Games.");
  }
  for (const char* name : {"ProgramFiles", "ProgramFiles(x86)", "SystemRoot"}) {
    const std::string forbidden = platform::GetEnv(name);
    if (!forbidden.empty() && Inside(absolute, forbidden)) {
      return trs("The game keeps its saves and settings in its own folder, so it can't live in "
                 "Program Files or Windows. Pick a folder of your own, like C:\\Games.");
    }
  }
  if (fs::exists(absolute, ec) && !existing_game) {
    if (LooksInstalled(absolute)) {
      return trs("The game is already installed there. Start its \"Mind over Recomp\" from the "
                 "Start menu to update or uninstall it.");
    }
    if (!fs::is_directory(absolute, ec) || !fs::is_empty(absolute, ec)) {
      return trf(trs("{0} already exists and isn't empty: pick another place."),
                 {absolute.string()});
    }
  }
  // The nearest folder that exists: free space and "can we write there".
  fs::path existing = absolute;
  while (!existing.empty() && !fs::exists(existing, ec) && existing != existing.root_path()) {
    existing = existing.parent_path();
  }
  const double free_gb = platform::FreeDiskGb(existing);
  if (!existing_game && free_gb >= 0 && free_gb < needed_gb) {
    char text[200];
    std::snprintf(text, sizeof(text),
                  trs("Not enough free space there: the game needs about %.0f GB, the drive has "
                      "%.1f GB free."),
                  needed_gb, free_gb);
    return text;
  }
  // OneDrive: it uploads (and may "free up space" on) files the build and
  // the game write constantly. Documents / Desktop are often inside it.
  for (const char* name : {"OneDrive", "OneDriveConsumer", "OneDriveCommercial"}) {
    const std::string onedrive = platform::GetEnv(name);
    if (!onedrive.empty() && Inside(absolute, onedrive)) {
      return trs("That folder is inside OneDrive, which syncs every file the build writes. Pick "
                 "a folder outside it, like C:\\Games.");
    }
  }
  // NTFS (or ReFS) only: the build links two of its folders (a junction),
  // which exFAT / FAT32 drives (most USB sticks) can't hold.
  {
    wchar_t volume[MAX_PATH + 1] = {};
    wchar_t file_system[64] = {};
    if (GetVolumePathNameW(existing.wstring().c_str(), volume, DWORD(std::size(volume))) &&
        GetVolumeInformationW(volume, nullptr, 0, nullptr, nullptr, nullptr, file_system,
                              DWORD(std::size(file_system)))) {
      const std::wstring fs_name = LowerW(file_system);
      if (fs_name != L"ntfs" && fs_name != L"refs") {
        return trs("That drive isn't formatted NTFS (it's exFAT or FAT32, like most USB sticks), "
                   "and the build needs NTFS. Pick a folder on your PC's own drive.");
      }
    }
  }
  const fs::path probe = existing / ".mind_over_recomp_write_test";
  HANDLE file = CreateFileW(probe.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return trs("Windows doesn't let us write there: pick a folder of your own, like C:\\Games.");
  }
  CloseHandle(file);
  return {};
}

bool Register(const Entry& entry, std::string* error) {
  HKEY key = nullptr;
  const std::wstring path = std::wstring(kUninstallRoot) + Wide(kAppsKey);
  if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                      nullptr) != ERROR_SUCCESS) {
    *error = trs("Couldn't add the game to Windows' list of apps.");
    return false;
  }
  const std::wstring installer = entry.installer.wstring();
  // The name the Apps list shows: the project's, with what it is.
  SetString(key, L"DisplayName", L"Mind over Recomp (Crash Mind over Mutant)");
  SetString(key, L"DisplayVersion", Wide(entry.version));
  SetString(key, L"Publisher", L"Mind over Recomp project");
  SetString(key, L"Comments",
            L"Unofficial PC port of Crash: Mind over Mutant, built from your own disc");
  SetString(key, L"URLInfoAbout", L"https://github.com/Maxilure/MindOver-Recomp");
  SetString(key, L"HelpLink", L"https://github.com/Maxilure/MindOver-Recomp/issues");
  SetString(key, L"InstallLocation", entry.root.wstring());
  SetString(key, L"DisplayIcon", entry.game_exe.wstring() + L",0");
  SetString(key, L"UninstallString", L"\"" + installer + L"\" --uninstall");
  SetString(key, L"QuietUninstallString", L"\"" + installer + L"\" --uninstall --quiet");
  SetDword(key, L"NoModify", 1);
  SetDword(key, L"NoRepair", 1);
  SetDword(key, L"EstimatedSize", DWORD(SizeOf(LongPath(entry.root)) / 1024));  // in KB
  wchar_t date[16];
  SYSTEMTIME now;
  GetLocalTime(&now);
  swprintf(date, std::size(date), L"%04u%02u%02u", now.wYear, now.wMonth, now.wDay);
  SetString(key, L"InstallDate", date);
  RegCloseKey(key);
  return true;
}

void Unregister() {
  const std::wstring path = std::wstring(kUninstallRoot) + Wide(kAppsKey);
  RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
}

fs::path RegisteredRoot() {
  const std::wstring path = std::wstring(kUninstallRoot) + Wide(kAppsKey);
  wchar_t value[4096] = {};
  DWORD bytes = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER, path.c_str(), L"InstallLocation", RRF_RT_REG_SZ, nullptr,
                   value, &bytes) != ERROR_SUCCESS) {
    return {};
  }
  return fs::path(value);
}

bool WriteShortcuts(const Entry& entry, bool desktop, std::string* error) {
  std::error_code ec;
  const fs::path menu = StartMenu();
  fs::create_directories(menu, ec);
  const fs::path game_work = entry.game_exe.parent_path();
  bool ok = WriteShortcut(menu / kGameShortcut, entry.game_exe, game_work,
                          L"Crash: Mind over Mutant (unofficial PC port, Mind over Recomp)",
                          entry.game_exe);
  // The installer has no icon of its own (it ships: no game art in it); its
  // shortcut borrows the one made from the player's disc when there is one
  // (tools/make_icon.py, next to the game exe).
  const fs::path disc_icon = game_work / "crash_mom.ico";
  ok = WriteShortcut(menu / kInstallerShortcut, entry.installer, entry.root,
                     L"Mind over Recomp: update or uninstall Crash Mind over Mutant",
                     fs::is_regular_file(disc_icon, ec) ? disc_icon : entry.installer) && ok;
  if (desktop) {
    ok = WriteShortcut(Desktop() / kGameShortcut, entry.game_exe, game_work,
                       L"Crash: Mind over Mutant (unofficial PC port, Mind over Recomp)",
                       entry.game_exe) && ok;
  }
  // An old launcher's "continue last save" entry into this folder (see above).
  const fs::path old = menu / kOldContinueShortcut;
  if (fs::exists(old, ec) && Inside(ShortcutTarget(old), entry.root)) fs::remove(old, ec);
  if (!ok) *error = trs("Couldn't write the shortcuts.");
  return ok;
}

void RemoveShortcuts(const fs::path& root) {
  std::error_code ec;
  for (const fs::path& lnk : {StartMenu() / kGameShortcut, StartMenu() / kInstallerShortcut,
                              StartMenu() / kOldContinueShortcut, Desktop() / kGameShortcut}) {
    if (fs::exists(lnk, ec) && Inside(ShortcutTarget(lnk), root)) fs::remove(lnk, ec);
  }
}

bool LooksInstalled(const fs::path& root) {
  // The installer + its marker (MarkInstalled) or a built game. NOT just the
  // installer next to source\: that's also the unpacked release .zip, which
  // must open the INSTALL page (2026-11-05: the first test opened "manage"
  // there and built inside the .zip's folder).
  std::error_code ec;
  return fs::is_regular_file(root / kInstallerName, ec) &&
         (fs::is_regular_file(root / kMarkerName, ec) || fs::is_directory(root / "program", ec));
}

void MarkInstalled(const fs::path& root) {
  std::ofstream out(root / kMarkerName, std::ios::trunc);
  out << "# This folder is an install of Mind over Recomp (launcher/install.h).\n"
         "# Delete the whole folder through Windows' Installed apps, not by hand.\n";
}

bool RemoveInstall(const fs::path& root, bool keep_user,
                   const std::function<void(uint64_t)>& progress, std::string* error) {
  if (!LooksInstalled(root)) {
    *error = trf(trs("{0} doesn't look like a Mind over Recomp install: nothing removed."),
                 {root.string()});
    return false;
  }
  // ONLY what the installer, the build and the game make (install.h).
  std::vector<std::wstring> parts = {L"source", L"program", L"game", L"cache", L".update",
                                     L".install", L"program.new", L"previous", L".download",
                                     L"program.bad", L"source.bad", L"READ ME FIRST.txt", Wide(kMarkerName),
                                     Wide(kInstallerName), Wide(kInstallerName) + L".old"};
  if (!keep_user) parts.push_back(L"user");
  const std::wstring base = LongPath(root);
  uint64_t files = 0;
  std::wstring failed;
  bool ok = true;
  for (const std::wstring& part : parts) {
    ok = RemoveTree(base + L"\\" + part, &files, progress, &failed) && ok;
  }
  RemoveDirectoryW(base.c_str());  // only if that left it empty
  if (progress) progress(files);
  if (!ok) {
    *error = trf(trs("Some files couldn't be removed (is the game still running?), e.g. {0}"),
                 {fs::path(failed.starts_with(L"\\\\?\\") ? failed.substr(4) : failed).string()});
  }
  return ok;
}

bool RemovePath(const fs::path& path, std::string* failed) {
  uint64_t files = 0;
  std::wstring stayed;
  const bool ok = RemoveTree(LongPath(path), &files, {}, &stayed);
  if (!ok && failed) *failed = fs::path(stayed.starts_with(L"\\\\?\\") ? stayed.substr(4) : stayed).string();
  return ok;
}

void ForgetCMakePackages(const fs::path& root) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kCMakePackages, 0, KEY_READ | KEY_SET_VALUE, &key) !=
      ERROR_SUCCESS) {
    return;
  }
  std::vector<std::wstring> drop;
  for (DWORD i = 0;; ++i) {
    wchar_t name[256];
    DWORD name_size = DWORD(std::size(name));
    wchar_t data[4096] = {};
    DWORD data_size = sizeof(data);
    DWORD type = 0;
    const LONG result = RegEnumValueW(key, i, name, &name_size, nullptr, &type,
                                      reinterpret_cast<BYTE*>(data), &data_size);
    if (result == ERROR_NO_MORE_ITEMS) break;
    if (result == ERROR_SUCCESS && type == REG_SZ && Inside(fs::path(data), root)) {
      drop.push_back(name);
    }
  }
  for (const std::wstring& name : drop) RegDeleteValueW(key, name.c_str());
  DWORD left = 0;
  RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &left, nullptr,
                   nullptr, nullptr, nullptr);
  RegCloseKey(key);
  if (left == 0) RegDeleteKeyW(HKEY_CURRENT_USER, kCMakePackages);
}

bool RelaunchFromTemp(const std::vector<std::string>& args) {
  wchar_t temp[MAX_PATH + 1] = {};
  GetTempPathW(DWORD(std::size(temp)), temp);
  const fs::path copy = fs::path(temp) / (L"MindOverRecomp-uninstall-" +
                                          std::to_wstring(GetCurrentProcessId()) + L".exe");
  if (!CopyFileW(platform::SelfPath().wstring().c_str(), copy.wstring().c_str(), FALSE)) {
    return false;
  }
  std::wstring line = L"\"" + copy.wstring() + L"\"";
  for (const std::string& arg : args) line += L" \"" + Wide(arg) + L"\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};
  if (!CreateProcessW(copy.wstring().c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      temp, &startup, &info)) {
    DeleteFileW(copy.wstring().c_str());
    return false;
  }
  CloseHandle(info.hThread);
  CloseHandle(info.hProcess);
  return true;
}

bool RunningFromTemp() {
  wchar_t temp[MAX_PATH + 1] = {};
  GetTempPathW(DWORD(std::size(temp)), temp);
  return Inside(platform::SelfPath(), fs::path(temp)) &&
         platform::SelfPath().filename().wstring().starts_with(L"MindOverRecomp-uninstall-");
}

void DeleteSelfAfterExit() {
  // cmd waits ~3 s (ping), then deletes the file; we quit meanwhile.
  std::wstring line = L"cmd.exe /d /c ping 127.0.0.1 -n 4 >nul & del /f /q \"" +
                      platform::SelfPath().wstring() + L"\"";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};
  if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &startup, &info)) {
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
  }
}

bool StartDetached(const fs::path& exe, const fs::path& work_folder) {
  const HINSTANCE result = ShellExecuteW(nullptr, L"open", exe.wstring().c_str(), nullptr,
                                         work_folder.wstring().c_str(), SW_SHOWNORMAL);
  return reinterpret_cast<INT_PTR>(result) > 32;
}

}  // namespace install

#else  // Linux: stubs (install.h)

namespace install {

fs::path DefaultParent() { return fs::path(platform::GetEnv("HOME")) / "Games"; }
std::string WhyNotHere(const fs::path& root, double, bool) {
  std::error_code ec;
  return fs::exists(root, ec) && !fs::is_empty(root, ec) ? "already exists" : "";
}
bool Register(const Entry&, std::string*) { return true; }
void Unregister() {}
fs::path RegisteredRoot() { return {}; }
bool WriteShortcuts(const Entry&, bool, std::string*) { return true; }
void RemoveShortcuts(const fs::path&) {}
bool LooksInstalled(const fs::path& root) {
  std::error_code ec;
  return fs::is_regular_file(root / kInstallerName, ec) && fs::is_regular_file(root / kMarkerName, ec);
}
void MarkInstalled(const fs::path&) {}
bool RemoveInstall(const fs::path&, bool, const std::function<void(uint64_t)>&, std::string* error) {
  *error = "uninstalling is Windows-only";
  return false;
}
void ForgetCMakePackages(const fs::path&) {}
bool RelaunchFromTemp(const std::vector<std::string>&) { return false; }
bool RunningFromTemp() { return false; }
void DeleteSelfAfterExit() {}
bool RemovePath(const fs::path& path, std::string* failed) {
  std::error_code ec;
  fs::remove_all(path, ec);
  if (ec && failed) *failed = path.string();
  return !ec;
}
bool StartDetached(const fs::path&, const fs::path&) { return false; }

}  // namespace install

#endif

// --- both systems ----------------------------------------------------------------

namespace install {

// TAKING OVER A FOLDER THAT HAS A GAME (2026-11-05): the player picks a folder
// that already holds a set-up, built game (reinstalling over a kept folder, an
// install whose Apps entry is gone, an old launcher's folder): the install
// uses it as it is instead of refusing it (installer.cpp). Either the picked
// folder itself or the game's folder inside it ("Crash Mind over Mutant").
// A build = the release layout (source\ with its built exe, or program\ with
// the installed exe): folders::Find decides, as the setup does.
std::filesystem::path ExistingGameAt(const std::filesystem::path& picked) {
  if (picked.empty()) return {};
  std::error_code ec;
  for (const std::filesystem::path& dir : {picked, picked / kFolderName}) {
    if (!std::filesystem::is_directory(dir, ec)) continue;
    const folders::Folders found = folders::Find(dir);
    if (found.root.empty() || !found.release) continue;
    if (std::filesystem::is_regular_file(found.BuiltExe(), ec) || found.ExeExists()) return found.root;
  }
  return {};
}

}  // namespace install
