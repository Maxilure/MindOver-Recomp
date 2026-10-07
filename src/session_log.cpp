// =============================================================================
// session_log.cpp -- see session_log.h
// =============================================================================
#include "session_log.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>  // CommandLineToArgvW
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")  // RegGetValueW
#else
#include <sys/utsname.h>
#endif

#ifndef CRASHMOM_VERSION
#define CRASHMOM_VERSION "(unknown version)"
#endif

REXCVAR_DEFINE_BOOL(event_logs, true, "CrashMoM",
                    "Write the game's events into the session log (co-op, menus, screens, sounds, "
                    "frame rate): what a problem report needs. Cheap; false = only the basics");
REXCVAR_DEFINE_INT32(logs_budget_mb, 300, "CrashMoM",
                     "Keep the logs folder (user/logs) under this many MB: the oldest logs are "
                     "deleted at start (0 = never delete)");

namespace session_log {
namespace {

namespace fs = std::filesystem;

// The event logs session_log.h lists (all default to true in their own files).
constexpr const char* kEventFlags[] = {
    "debug_log_fps",           // frame_rate.cpp
    "debug_audio_trace",       // audio_trace.cpp
    "debug_coop_trace",        // players/more_players.cpp
    "debug_menu_input_trace",  // players/menu_input.cpp
    "debug_frontend_trace",    // data/fight_tree.cpp
};

// --- facts about the machine ---------------------------------------------------

#if defined(_WIN32)

std::string Utf8(const wchar_t* text) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) return {};
  std::string out(size_t(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
  return out;
}

// "Windows 11 build 26200" (RtlGetVersion: GetVersionEx lies to programs
// without a compatibility manifest). Build 22000+ = Windows 11.
std::string System() {
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  OSVERSIONINFOW info{};
  info.dwOSVersionInfoSize = sizeof(info);
  if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
    if (auto get = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))) {
      get(&info);
    }
  }
  if (info.dwMajorVersion == 0) return "Windows (version unknown)";
  const int shown = info.dwMajorVersion == 10 && info.dwBuildNumber >= 22000 ? 11 : info.dwMajorVersion;
  return "Windows " + std::to_string(shown) + " build " + std::to_string(info.dwBuildNumber);
}

std::string CpuName() {
  wchar_t name[256] = {};
  DWORD size = sizeof(name);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                   L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, name, &size) != ERROR_SUCCESS) {
    return "?";
  }
  return Utf8(name);
}

// Total and free RAM in GB.
std::pair<double, double> Ram() {
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (!GlobalMemoryStatusEx(&status)) return {0, 0};
  return {status.ullTotalPhys / 1073741824.0, status.ullAvailPhys / 1073741824.0};
}

// The command line without the program itself.
std::string Arguments() {
  int count = 0;
  LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
  std::string out;
  for (int i = 1; args && i < count; ++i) {
    out += (out.empty() ? "" : " ") + Utf8(args[i]);
  }
  if (args) LocalFree(args);
  return out;
}

#else

// The value of `key` ("PRETTY_NAME=" / "model name") in a text file, or "".
std::string ValueIn(const char* file, const std::string& key, char separator) {
  std::ifstream in(file);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind(key, 0) != 0) continue;
    const size_t at = line.find(separator);
    if (at == std::string::npos) continue;
    std::string value = line.substr(at + 1);
    value.erase(0, value.find_first_not_of(" \t\""));
    while (!value.empty() && (value.back() == '"' || value.back() == ' ')) value.pop_back();
    return value;
  }
  return {};
}

// "Linux 7.2.9-1-cachyos (CachyOS) x86_64, desktop Hyprland (wayland)".
std::string System() {
  utsname name{};
  std::string out = uname(&name) == 0 ? std::string(name.sysname) + " " + name.release : "Linux";
  const std::string distro = ValueIn("/etc/os-release", "PRETTY_NAME=", '=');
  if (!distro.empty()) out += " (" + distro + ")";
  out += std::string(" ") + name.machine;
  const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
  const char* session = std::getenv("XDG_SESSION_TYPE");
  if (desktop || session) {
    out += std::string(", desktop ") + (desktop ? desktop : "?") + " (" + (session ? session : "?") + ")";
  }
  return out;
}

std::string CpuName() {
  const std::string name = ValueIn("/proc/cpuinfo", "model name", ':');
  return name.empty() ? "?" : name;
}

std::pair<double, double> Ram() {
  // /proc/meminfo: "MemTotal:  16303428 kB".
  const double total = std::atof(ValueIn("/proc/meminfo", "MemTotal:", ':').c_str());
  const double free = std::atof(ValueIn("/proc/meminfo", "MemAvailable:", ':').c_str());
  return {total / 1048576.0, free / 1048576.0};
}

std::string Arguments() {
  // NUL-separated; the first is the program.
  std::ifstream in("/proc/self/cmdline", std::ios::binary);
  std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::string out;
  size_t start = all.find('\0');
  while (start != std::string::npos && start + 1 < all.size()) {
    const size_t end = all.find('\0', start + 1);
    out += (out.empty() ? "" : " ") + all.substr(start + 1, end - start - 1);
    start = end;
  }
  return out;
}

#endif

// --- the clean-up -------------------------------------------------------------------

// Deletes the oldest log files until the folder fits the budget (see the header).
void CleanUp(const fs::path& logs) {
  const int budget_mb = REXCVAR_GET(logs_budget_mb);
  if (budget_mb <= 0) return;
  const uint64_t budget = uint64_t(budget_mb) * 1024 * 1024;

  struct File {
    fs::path path;
    uint64_t size;
    fs::file_time_type time;
  };
  std::vector<File> files;
  uint64_t total = 0;
  std::error_code ec;
  const fs::path own = fs::path(std::string(REXCVAR_GET(log_file))).filename();
  const auto recent = fs::file_time_type::clock::now() - std::chrono::seconds(30);
  for (const auto& entry : fs::directory_iterator(logs, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    const std::string ext = entry.path().extension().string();
    if (ext != ".log" && ext != ".txt") continue;  // only logs and crash reports
    File file{entry.path(), entry.file_size(ec), entry.last_write_time(ec)};
    total += file.size;
    // Never this session's log, nor anything written in the last 30 s (the
    // launcher's copy of the terminal output, a log being opened right now).
    if (entry.path().filename() == own || file.time > recent) continue;
    files.push_back(std::move(file));
  }
  if (total <= budget) return;
  std::sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.time < b.time; });
  int deleted = 0;
  uint64_t freed = 0;
  for (const File& file : files) {
    if (total <= budget) break;
    if (fs::remove(file.path, ec)) {
      total -= file.size;
      freed += file.size;
      ++deleted;
    }
  }
  REXLOG_INFO("Session: deleted the {} oldest logs ({:.1f} MB) to keep {} under {} MB "
              "(--logs_budget_mb)", deleted, freed / 1048576.0, logs.string(), budget_mb);
}

}  // namespace

void Start(const fs::path& logs) {
  REXLOG_INFO("Session: Mind over Recomp {} (built {} {})", CRASHMOM_VERSION, __DATE__, __TIME__);
  REXLOG_INFO("Session: system {}", System());
  const auto [ram_total, ram_free] = Ram();
  REXLOG_INFO("Session: CPU {} ({} threads), RAM {:.1f} GB ({:.1f} GB free)", CpuName(),
              std::thread::hardware_concurrency(), ram_total, ram_free);
  const std::string arguments = Arguments();
  REXLOG_INFO("Session: started with {}", arguments.empty() ? "(no options)" : arguments);

  // The off switch: every event log nobody set on its own goes off.
  if (REXCVAR_GET(event_logs)) {
    REXLOG_INFO("Session: event logs on (frame rate, sounds, co-op, menus, screens); "
                "event_logs = false turns them off");
  } else {
    for (const char* flag : kEventFlags) {
      // (By where the value came from: "--debug_log_fps=true" equals the
      // default, yet it was asked for.)
      if (rex::cvar::GetFlagSource(flag) == rex::cvar::Source::kDefault) {
        rex::cvar::SetFlagByName(flag, "false");
      }
    }
    REXLOG_INFO("Session: event logs off (event_logs = false)");
  }

  // (Not in the launcher's --list_settings run: it lasts a moment, logs to
  // nowhere, and the session started right after does the clean-up visibly.)
  if (!logs.empty() && rex::cvar::GetFlagByName("list_settings").empty()) CleanUp(logs);
}

void LogSettings() {
  // Settings the PLAYER changed: from the settings file, the command line or
  // a REX_* variable (the "started with" line shows which came from the
  // command line). Left out: values our own code sets (kRuntime, e.g. the
  // render-target path) and the per-session ones the launcher passes.
  std::string changed;
  for (const std::string& name : rex::cvar::ListModifiedFlags()) {
    if (name == "log_file" || name == "game_data_root" || name == "load_save") continue;
    const rex::cvar::Source source = rex::cvar::GetFlagSource(name);
    if (source == rex::cvar::Source::kDefault || source == rex::cvar::Source::kRuntime) continue;
    changed += (changed.empty() ? "" : ", ") + name + " = " + rex::cvar::GetFlagByName(name);
  }
  REXLOG_INFO("Session: settings changed from the defaults: {}", changed.empty() ? "none" : changed);
}

}  // namespace session_log
