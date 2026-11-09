// =============================================================================
// platform_win.cpp -- platform.h on Windows
// =============================================================================
//
// Child processes: CreateProcessW with
//   - the output on an anonymous pipe (stdout + stderr), stdin = NUL, and
//     ONLY those two handles inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST:
//     nothing of the launcher's leaks in);
//   - CREATE_NO_WINDOW: tools like cmake / git don't flash a console window;
//   - started suspended, put into a JOB, then resumed: the job holds the
//     process and everything it starts (cmake -> ninja -> clang), so a stop
//     ends them all (TerminateJobObject); build steps' jobs also have
//     KILL_ON_JOB_CLOSE, so they end with the launcher (die_with_launcher).
//   - a .cmd / .bat runs through cmd.exe /d /s /c.
//   - FULL SPEED (2026-11-08): Windows 11 runs the work of apps it thinks are
//     in the background (a minimized window; processes without any window,
//     like cmake -> ninja -> clang) on its power-saving "efficiency" footing
//     (EcoQoS). Players saw builds crawl or seem to stop while the installer
//     was minimized. Every process in a build step's job is told to opt out
//     (FullSpeed below), the new ones as they appear (a watcher thread per
//     step, once a second).
// Text: the launcher works in UTF-8; Windows calls get UTF-16 (wide).
// =============================================================================
#include "platform.h"

#if defined(_WIN32)

#define NOMINMAX
#include <windows.h>
// (after windows.h:)
#include <tlhelp32.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>
#include <cstdlib>
#include <cstring>  // _stricmp
#include <cwctype>

namespace fs = std::filesystem;

namespace platform {
namespace {

std::wstring Wide(const std::string& text) {
  if (text.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
  std::wstring out(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n);
  return out;
}

std::string Narrow(const std::wstring& text) {
  if (text.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr,
                                    nullptr);
  std::string out(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n, nullptr, nullptr);
  return out;
}

std::wstring LowerW(std::wstring s) {
  std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
  return s;
}

// One argument for a Windows command line (the rules the C runtime parses
// back: quotes around it when needed, backslashes before a quote doubled).
void AppendQuoted(std::wstring& line, const std::wstring& arg) {
  if (!line.empty()) line += L' ';
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    line += arg;
    return;
  }
  line += L'"';
  for (auto it = arg.begin();; ++it) {
    size_t backslashes = 0;
    while (it != arg.end() && *it == L'\\') {
      ++it;
      ++backslashes;
    }
    if (it == arg.end()) {
      line.append(backslashes * 2, L'\\');
      break;
    }
    if (*it == L'"') {
      line.append(backslashes * 2 + 1, L'\\');
      line += *it;
    } else {
      line.append(backslashes, L'\\');
      line += *it;
    }
  }
  line += L'"';
}

std::wstring EnvironmentVariable(const wchar_t* name) {
  const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
  if (n == 0) return {};
  std::wstring value(n, L'\0');
  GetEnvironmentVariableW(name, value.data(), n);
  value.resize(n - 1);
  return value;
}

}  // namespace

fs::path SelfPath() {
  std::wstring buffer(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
  buffer.resize(n);
  return fs::path(buffer);
}

fs::path HomeDir() { return fs::path(EnvironmentVariable(L"USERPROFILE")); }

std::string ExeName(const std::string& name) { return name + ".exe"; }

std::string GetEnv(const char* name) { return Narrow(EnvironmentVariable(Wide(name).c_str())); }

namespace {

// One value of a registry key's environment list ("" if absent), with
// %VARIABLES% expanded (PATH entries are often REG_EXPAND_SZ like
// "%USERPROFILE%\AppData\Local\Microsoft\WindowsApps").
std::wstring RegistryEnvironmentValue(HKEY root, const wchar_t* key, const wchar_t* name) {
  DWORD bytes = 0;
  const DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND;
  if (RegGetValueW(root, key, name, flags, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes == 0) {
    return {};
  }
  std::wstring raw(bytes / sizeof(wchar_t) + 1, L'\0');
  if (RegGetValueW(root, key, name, flags, nullptr, raw.data(), &bytes) != ERROR_SUCCESS) {
    return {};
  }
  raw.resize(wcslen(raw.c_str()));
  const DWORD n = ExpandEnvironmentStringsW(raw.c_str(), nullptr, 0);
  if (n == 0) return raw;
  std::wstring expanded(n, L'\0');
  ExpandEnvironmentStringsW(raw.c_str(), expanded.data(), n);
  expanded.resize(wcslen(expanded.c_str()));
  return expanded;
}

constexpr const wchar_t* kMachineEnvironment =
    L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";
constexpr const wchar_t* kUserEnvironment = L"Environment";

// "a;b;;c" -> {a, b, c}
std::vector<std::wstring> SplitPath(const std::wstring& path) {
  std::vector<std::wstring> parts;
  size_t start = 0;
  while (start <= path.size()) {
    const size_t end = path.find(L';', start);
    std::wstring part = path.substr(start, end == std::wstring::npos ? end : end - start);
    if (!part.empty()) parts.push_back(std::move(part));
    if (end == std::wstring::npos) break;
    start = end + 1;
  }
  return parts;
}

}  // namespace

void RefreshEnvironment() {
  // PATH: the system's, then the user's (Windows' own order), then whatever
  // this process had on top (a folder a caller added on purpose).
  std::vector<std::wstring> parts =
      SplitPath(RegistryEnvironmentValue(HKEY_LOCAL_MACHINE, kMachineEnvironment, L"Path"));
  for (std::wstring& part : SplitPath(RegistryEnvironmentValue(HKEY_CURRENT_USER, kUserEnvironment, L"Path"))) {
    parts.push_back(std::move(part));
  }
  if (parts.empty()) {
    return;  // the registry couldn't be read: keep what we have
  }
  std::vector<std::wstring> seen;
  for (const std::wstring& part : parts) seen.push_back(LowerW(part));
  for (const std::wstring& part : SplitPath(EnvironmentVariable(L"PATH"))) {
    if (std::find(seen.begin(), seen.end(), LowerW(part)) == seen.end()) {
      parts.push_back(part);
      seen.push_back(LowerW(part));
    }
  }
  std::wstring joined;
  for (const std::wstring& part : parts) {
    if (!joined.empty()) joined += L';';
    joined += part;
  }
  SetEnvironmentVariableW(L"PATH", joined.c_str());
  // The Vulkan SDK's installer sets VULKAN_SDK (system-wide): where glslc is.
  for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
    const std::wstring vulkan = RegistryEnvironmentValue(
        root, root == HKEY_LOCAL_MACHINE ? kMachineEnvironment : kUserEnvironment, L"VULKAN_SDK");
    if (!vulkan.empty()) {
      SetEnvironmentVariableW(L"VULKAN_SDK", vulkan.c_str());
      break;
    }
  }
}

// Is there a program file here? A plain file, OR an "app execution alias":
// how Store / MSIX apps put a command on PATH (winget.exe = App Installer's,
// in %LOCALAPPDATA%\Microsoft\WindowsApps). An alias is a reparse point that
// only CreateProcess knows how to follow: std::filesystem's is_regular_file
// fails on it with error 1920 "The file cannot be accessed by the system".
// That made the installer say winget was missing on PCs that HAD it, even
// after the player installed App Installer by hand (a tester's report,
// 2026-11-06; reproduced on the Windows test PC: winget there works, but
// is_regular_file said no). The file's own attributes answer either way.
static bool IsProgramFile(const fs::path& file) {
  const DWORD attributes = GetFileAttributesW(file.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// FindProgram's search; `path` = the PATH to walk (";"-separated).
static fs::path FindProgramOnPath(const std::string& name, const std::wstring& path) {
  if (name.empty()) return {};
  const fs::path given(Wide(name));
  const bool has_ext = given.has_extension();
  auto try_file = [&](const fs::path& base) -> fs::path {
    if (has_ext && IsProgramFile(base)) return base;
    for (const wchar_t* ext : {L".exe", L".cmd", L".bat"}) {
      fs::path candidate = base;
      candidate += ext;
      if (IsProgramFile(candidate)) return candidate;
    }
    return {};
  };
  if (name.find_first_of("/\\:") != std::string::npos) {
    return try_file(given);
  }
  // Windows' aliases folder, looked at last even when PATH lacks it (the
  // player's PATH was edited, or App Installer arrived after log-on and
  // Windows hasn't added the folder yet). Only aliases live there; the
  // Store's fake "python" is one too, but FindPython runs what it finds and
  // refuses anything that doesn't answer "Python 3".
  auto try_aliases = [&]() -> fs::path {
    const std::wstring local = EnvironmentVariable(L"LOCALAPPDATA");
    if (local.empty()) return {};
    return try_file(fs::path(local) / L"Microsoft" / L"WindowsApps" / given);
  };
  size_t start = 0;
  while (start <= path.size()) {
    const size_t end = path.find(L';', start);
    const std::wstring dir = path.substr(start, end == std::wstring::npos ? end : end - start);
    if (!dir.empty()) {
      if (fs::path found = try_file(fs::path(dir) / given); !found.empty()) return found;
    }
    if (end == std::wstring::npos) break;
    start = end + 1;
  }
  return try_aliases();
}

fs::path FindProgram(const std::string& name) {
  return FindProgramOnPath(name, EnvironmentVariable(L"PATH"));
}

// The program a child runs: looked up on the PATH of the environment the child
// will GET first, then on ours.
//
// GitHub issues #10 / #11 (2026-11-08, 0.2.1): "Couldn't find cmake (is it
// installed?)" right at the SDK's configure step, while the tools page said
// CMake was OK. Those PCs had only Visual Studio's own CMake (its C++ tools
// carry one, outside PATH): the tools check accepts it and puts its folder on
// the BUILD environment's PATH (setup.cpp BuildEnvironment), but the lookup
// here only walked the installer's own PATH, so "cmake" was never found and
// the step never started. Found from a player's setup log (the failure is the
// first "cmake --preset" line, nothing ran) + reading the two lookups side by
// side. Same for Visual Studio's Ninja and an off-PATH Python.
static fs::path FindProgramFor(const ChildOptions& options) {
  const std::string name = options.argv.empty() ? std::string() : options.argv[0];
  if (options.environment) {
    for (const auto& [key, value] : *options.environment) {
      if (_stricmp(key.c_str(), "PATH") != 0) continue;
      if (fs::path found = FindProgramOnPath(name, Wide(value)); !found.empty()) return found;
    }
  }
  return FindProgram(name);
}

double MemAvailableGb() {
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  return GlobalMemoryStatusEx(&status) ? double(status.ullAvailPhys) / 1e9 : 0;
}

int Cores() { return int(std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS))); }

// The .ico file's layout (ICONDIR + ICONDIRENTRY) and the resource's
// (GRPICONDIR + GRPICONDIRENTRY: the same fields, but the picture's resource
// id instead of its file offset). Each picture becomes one RT_ICON resource
// (its bytes as they are in the file: PNG-compressed pictures are fine in an
// exe since Vista), the group RT_GROUP_ICON 1 lists them.
bool EmbedIcon(const fs::path& exe, const fs::path& ico, std::string* error) {
  std::ifstream in(ico, std::ios::binary);
  std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto u16 = [&](size_t at) { return uint16_t(file[at] | file[at + 1] << 8); };
  auto u32 = [&](size_t at) { return uint32_t(u16(at) | uint32_t(u16(at + 2)) << 16); };
  if (file.size() < 6 || u16(0) != 0 || u16(2) != 1 || u16(4) == 0 || file.size() < 6 + 16 * size_t(u16(4))) {
    *error = "not an icon file: " + ico.string();
    return false;
  }
  const int count = u16(4);
  HANDLE update = BeginUpdateResourceW(exe.wstring().c_str(), FALSE);
  if (!update) {
    *error = "couldn't open " + exe.string() + " (error " + std::to_string(GetLastError()) + ")";
    return false;
  }
  std::vector<uint8_t> group = {0, 0, 1, 0, uint8_t(count), uint8_t(count >> 8)};
  bool ok = true;
  for (int i = 0; i < count && ok; ++i) {
    const size_t e = 6 + 16 * size_t(i);
    const uint32_t bytes = u32(e + 8), offset = u32(e + 12);
    if (size_t(offset) + bytes > file.size()) {
      ok = false;
      break;
    }
    const WORD id = WORD(i + 1);
    ok = UpdateResourceW(update, MAKEINTRESOURCEW(3) /* RT_ICON */, MAKEINTRESOURCEW(id), MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
                         file.data() + offset, bytes) != 0;
    // width, height, colours, reserved, planes, bit count, bytes (12 B as
    // in the file), then the WORD resource id.
    group.insert(group.end(), file.begin() + long(e), file.begin() + long(e) + 12);
    group.push_back(uint8_t(id));
    group.push_back(uint8_t(id >> 8));
  }
  if (ok) {
    ok = UpdateResourceW(update, MAKEINTRESOURCEW(14) /* RT_GROUP_ICON */, MAKEINTRESOURCEW(1), MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
                         group.data(), DWORD(group.size())) != 0;
  }
  if (!EndUpdateResourceW(update, ok ? FALSE : TRUE) || !ok) {
    *error = "couldn't write the icon into " + exe.string() + " (error " + std::to_string(GetLastError()) + ")";
    return false;
  }
  return true;
}

double FreeDiskGb(const fs::path& where) {
  ULARGE_INTEGER free{};
  return GetDiskFreeSpaceExW(where.wstring().c_str(), &free, nullptr, nullptr)
             ? double(free.QuadPart) / 1e9
             : -1;
}

bool LibraryAvailable(const char*, const char* windows_name) {
  if (HMODULE lib = LoadLibraryExW(Wide(windows_name).c_str(), nullptr,
                                   LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_SEARCH_SYSTEM32)) {
    FreeLibrary(lib);
    return true;
  }
  return false;
}

bool ProcessRunning(const std::string& name) {
  const std::wstring wanted = LowerW(Wide(ExeName(name)));
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  bool found = false;
  for (BOOL ok = Process32FirstW(snapshot, &entry); ok && !found;
       ok = Process32NextW(snapshot, &entry)) {
    found = LowerW(entry.szExeFile) == wanted;
  }
  CloseHandle(snapshot);
  return found;
}

std::tm LocalTime(std::time_t t) {
  std::tm local{};
  localtime_s(&local, &t);
  return local;
}

FontFiles SystemFonts() {
  const fs::path fonts = fs::path(EnvironmentVariable(L"WINDIR")) / "Fonts";
  std::error_code ec;
  auto pick = [&](const char* file) {
    const fs::path p = fonts / file;
    return fs::is_regular_file(p, ec) ? p : fs::path();
  };
  return {pick("segoeui.ttf"), pick("segoeuib.ttf"), pick("consola.ttf")};
}

Environment CurrentEnvironment() {
  Environment env;
  wchar_t* block = GetEnvironmentStringsW();
  for (const wchar_t* p = block; p && *p; p += wcslen(p) + 1) {
    const std::wstring entry = p;
    const size_t eq = entry.find(L'=', 1);  // "=C:=C:\..." entries start with '='
    if (eq != std::wstring::npos && entry[0] != L'=') {
      env[Narrow(entry.substr(0, eq))] = Narrow(entry.substr(eq + 1));
    }
  }
  FreeEnvironmentStringsW(block);
  return env;
}

// --- child processes ----------------------------------------------------------

struct Child::State {
  HANDLE process = nullptr;
  HANDLE job = nullptr;
  HANDLE read = nullptr;
  DWORD pid = 0;
  bool waited = false;
  std::thread speed_watcher;  // FullSpeed for every process of the job (header)
};

namespace {

// Opts one process out of Windows' power throttling (EcoQoS): ControlMask
// names the throttling kinds we decide, StateMask 0 = "off" for all of them
// (Microsoft's documented way to ask for full speed, "HighQoS").
void FullSpeed(HANDLE process) {
  PROCESS_POWER_THROTTLING_STATE state{};
  state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
#ifdef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
  state.ControlMask |= PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
#endif
  state.StateMask = 0;
  SetProcessInformation(process, ProcessPowerThrottling, &state, sizeof(state));
}

// Until `process` ends: every process in `job` (the step's tool and all it
// starts: ninja's compilers come and go) is put on full speed once.
void WatchJobSpeed(HANDLE job, HANDLE process) {
  std::vector<ULONG_PTR> seen;
  std::vector<uint8_t> buffer(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + 1024 * sizeof(ULONG_PTR));
  do {
    auto* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buffer.data());
    if (QueryInformationJobObject(job, JobObjectBasicProcessIdList, list, DWORD(buffer.size()), nullptr) ||
        GetLastError() == ERROR_MORE_DATA) {
      for (DWORD i = 0; i < list->NumberOfProcessIdsInList; ++i) {
        const ULONG_PTR pid = list->ProcessIdList[i];
        if (std::find(seen.begin(), seen.end(), pid) != seen.end()) continue;
        seen.push_back(pid);
        if (HANDLE p = OpenProcess(PROCESS_SET_INFORMATION, FALSE, DWORD(pid))) {
          FullSpeed(p);
          CloseHandle(p);
        }
      }
      if (seen.size() > 4096) seen.erase(seen.begin(), seen.begin() + 2048);  // pids get reused
    }
  } while (WaitForSingleObject(process, 1000) == WAIT_TIMEOUT);
  CloseHandle(job);
  CloseHandle(process);
}

}  // namespace

void KeepFullSpeed() { FullSpeed(GetCurrentProcess()); }

void KeepAwake(bool on) {
  // ES_SYSTEM_REQUIRED: no sleep by itself while set (ES_CONTINUOUS = until
  // cleared). The display may still go dark: that doesn't slow a build.
  SetThreadExecutionState(on ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED) : ES_CONTINUOUS);
}

Child::Child() : state_(std::make_unique<State>()) {}

Child::~Child() {
  if (state_->speed_watcher.joinable()) state_->speed_watcher.join();  // ends with the process
  if (state_->read) CloseHandle(state_->read);
  if (state_->process) CloseHandle(state_->process);
  if (state_->job) CloseHandle(state_->job);  // KILL_ON_JOB_CLOSE jobs end their steps here
}

bool Child::Start(const ChildOptions& options, std::string* error) {
  const fs::path program = FindProgramFor(options);
  if (program.empty()) {
    *error = "Couldn't find " + (options.argv.empty() ? std::string("?") : options.argv[0]) +
             " (is it installed?)";
    return false;
  }
  // The command line (a .cmd / .bat goes through cmd.exe).
  std::wstring line;
  std::wstring application = program.wstring();
  const std::wstring ext = LowerW(program.extension().wstring());
  const bool script = ext == L".cmd" || ext == L".bat";
  std::wstring inner;
  AppendQuoted(inner, program.wstring());
  for (size_t i = 1; i < options.argv.size(); ++i) AppendQuoted(inner, Wide(options.argv[i]));
  if (script) {
    application = EnvironmentVariable(L"ComSpec");
    line = L"cmd.exe /d /s /c \"" + inner + L"\"";
  } else {
    line = inner;
  }

  // Output pipe: the child's end inheritable, ours not. stdin: NUL.
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  HANDLE read = nullptr, write = nullptr;
  if (!CreatePipe(&read, &write, &inherit, 0)) {
    *error = "Couldn't make a pipe";
    return false;
  }
  SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                           OPEN_EXISTING, 0, nullptr);

  // Only those two handles reach the child.
  HANDLE handles[2] = {write, nul};
  SIZE_T attr_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
  std::vector<uint8_t> attr_storage(attr_size);
  auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_storage.data());
  InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size);
  UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                            sizeof(HANDLE) * (nul != INVALID_HANDLE_VALUE ? 2 : 1), nullptr,
                            nullptr);
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdOutput = write;
  startup.StartupInfo.hStdError = write;
  startup.StartupInfo.hStdInput = nul != INVALID_HANDLE_VALUE ? nul : nullptr;
  startup.lpAttributeList = attrs;

  // Environment block: "KEY=value\0...\0\0", UTF-16, sorted (Windows wants that).
  std::wstring env_block;
  if (options.environment) {
    std::vector<std::wstring> entries;
    for (const auto& [key, value] : *options.environment) entries.push_back(Wide(key + "=" + value));
    std::sort(entries.begin(), entries.end(),
              [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    for (const auto& entry : entries) {
      env_block += entry;
      env_block += L'\0';
    }
    env_block += L'\0';
  }
  const std::wstring work = options.work_folder.wstring();
  PROCESS_INFORMATION info{};
  const BOOL ok = CreateProcessW(
      application.c_str(), line.data(), nullptr, nullptr, TRUE,
      CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
          EXTENDED_STARTUPINFO_PRESENT | CREATE_NEW_PROCESS_GROUP,
      options.environment ? env_block.data() : nullptr, work.empty() ? nullptr : work.c_str(),
      &startup.StartupInfo, &info);
  DeleteProcThreadAttributeList(attrs);
  CloseHandle(write);
  if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
  if (!ok) {
    CloseHandle(read);
    *error = "Couldn't start " + Narrow(program.wstring()) + " (error " +
             std::to_string(GetLastError()) + ")";
    return false;
  }
  // The job (see the header), then let it run.
  state_->job = CreateJobObjectW(nullptr, nullptr);
  if (state_->job && options.die_with_launcher) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(state_->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
  }
  if (state_->job) AssignProcessToJobObject(state_->job, info.hProcess);
  FullSpeed(info.hProcess);
  ResumeThread(info.hThread);
  // Build steps: their children on full speed too, as they start (header).
  if (state_->job && options.die_with_launcher) {
    HANDLE job_copy = nullptr, process_copy = nullptr;
    const HANDLE me = GetCurrentProcess();
    if (DuplicateHandle(me, state_->job, me, &job_copy, 0, FALSE, DUPLICATE_SAME_ACCESS) &&
        DuplicateHandle(me, info.hProcess, me, &process_copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
      state_->speed_watcher = std::thread(WatchJobSpeed, job_copy, process_copy);
    } else {
      if (job_copy) CloseHandle(job_copy);
    }
  }
  CloseHandle(info.hThread);
  state_->process = info.hProcess;
  state_->pid = info.dwProcessId;
  state_->read = read;
  return true;
}

int Child::Read(char* buffer, size_t size, int timeout_ms) {
  if (!state_->read) return -1;
  const ULONGLONG until = GetTickCount64() + ULONGLONG(std::max(timeout_ms, 0));
  for (;;) {
    DWORD available = 0;
    if (!PeekNamedPipe(state_->read, nullptr, 0, nullptr, &available, nullptr)) {
      CloseHandle(state_->read);  // broken pipe: the writer is gone
      state_->read = nullptr;
      return -1;
    }
    if (available > 0) {
      DWORD got = 0;
      if (!ReadFile(state_->read, buffer, DWORD(std::min<size_t>(size, available)), &got, nullptr) ||
          got == 0) {
        CloseHandle(state_->read);
        state_->read = nullptr;
        return -1;
      }
      return int(got);
    }
    if (GetTickCount64() >= until) return 0;
    Sleep(10);
  }
}

Ended Child::Wait() {
  Ended ended;
  if (!state_->process || state_->waited) return ended;
  WaitForSingleObject(state_->process, INFINITE);
  state_->waited = true;
  DWORD code = 0;
  GetExitCodeProcess(state_->process, &code);
  ended.raw = code;
  if (code >= 0xC0000000u && code != 0xC000013Au /* Ctrl+C / closed console */) {
    ended.crashed = true;
    const char* what = "a crash";
    switch (code) {
      case 0xC0000005: what = "access violation"; break;
      case 0xC00000FD: what = "stack overflow"; break;
      case 0xC000001D: what = "illegal instruction"; break;
      case 0xC0000094: what = "integer divide by zero"; break;
      case 0xC0000409: what = "fail fast / stack buffer overrun"; break;
      case 0xC0000374: what = "heap corruption"; break;
      case 0xC0000135: what = "a DLL is missing"; break;
      case 0xC0000142: what = "a DLL failed to start"; break;
    }
    char text[64];
    std::snprintf(text, sizeof(text), "0x%08lX (%s)", code, what);
    ended.description = text;
  } else {
    ended.exit_code = int(code);
  }
  return ended;
}

void Child::RequestStop() {
  if (!state_->process || state_->waited) return;
  // A windowed program (the game): close its windows like the X button does.
  struct Find {
    DWORD pid;
    int closed = 0;
  } find{state_->pid};
  EnumWindows(
      [](HWND window, LPARAM param) -> BOOL {
        auto* f = reinterpret_cast<Find*>(param);
        DWORD owner = 0;
        GetWindowThreadProcessId(window, &owner);
        if (owner == f->pid && IsWindowVisible(window)) {
          PostMessageW(window, WM_CLOSE, 0, 0);
          ++f->closed;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&find));
  if (find.closed == 0) ForceStop();  // no window (a build tool): end it
}

void Child::ForceStop() {
  if (!state_->process || state_->waited) return;
  if (state_->job) {
    TerminateJobObject(state_->job, 1);
  } else {
    TerminateProcess(state_->process, 1);
  }
}

bool Child::running() const { return state_->process && !state_->waited; }
int Child::id() const { return int(state_->pid); }

std::string Capture(const std::vector<std::string>& argv, bool* ok, const fs::path& work_folder,
                    int timeout_ms, const Environment* environment) {
  *ok = false;
  Child child;
  std::string error;
  if (!child.Start({argv, work_folder, true, environment}, &error)) return {};
  std::string out;
  char buffer[4096];
  const ULONGLONG until = GetTickCount64() + ULONGLONG(timeout_ms);
  for (;;) {
    const ULONGLONG now = GetTickCount64();
    if (now >= until) {
      child.ForceStop();
      break;
    }
    const int got = child.Read(buffer, sizeof(buffer), int(std::min<ULONGLONG>(until - now, 200)));
    if (got < 0) break;
    out.append(buffer, size_t(std::max(got, 0)));
  }
  const Ended ended = child.Wait();
  *ok = !ended.crashed && !ended.killed && ended.exit_code == 0;
  return out;
}

void Restart(const fs::path& program, const std::vector<std::string>& args) {
  std::wstring line;
  AppendQuoted(line, program.wstring());
  for (const auto& arg : args) AppendQuoted(line, Wide(arg));
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info{};
  if (CreateProcessW(program.wstring().c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr,
                     program.parent_path().wstring().c_str(), &startup, &info)) {
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    std::_Exit(0);  // the new launcher takes over
  }
}

}  // namespace platform

#endif  // _WIN32
