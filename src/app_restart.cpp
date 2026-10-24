// =============================================================================
// app_restart.cpp -- close the game and start it again (see app_restart.h)
// =============================================================================
#include "app_restart.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")  // CommandLineToArgvW
#else
#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <iterator>
#if defined(__linux__)
#include <linux/close_range.h>
#include <sys/syscall.h>
#endif
#endif

#include <rex/logging.h>

namespace app_restart {
namespace {

// "--load_save=3" or "--load_save 3": the quick load of the first start.
bool IsLoadSave(const std::string& arg) {
  return arg == "--load_save" || arg.rfind("--load_save=", 0) == 0;
}

void FlushLog() {
  if (auto logger = rex::GetLogger()) logger->flush();
}

}  // namespace

#if defined(_WIN32)

void RestartNow() {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) {
    REXLOG_WARN("Restart: couldn't read the command line");
    return;
  }
  // Rebuild it, quoting every argument (Windows' own rules: backslashes
  // before a quote doubled, quotes escaped).
  std::wstring line;
  for (int i = 0; i < argc; ++i) {
    std::wstring a = argv[i];
    const std::string narrow(a.begin(), a.end());
    if (i > 0 && IsLoadSave(narrow)) {
      if (narrow == "--load_save") ++i;  // its value too
      continue;
    }
    if (!line.empty()) line += L' ';
    line += L'"';
    size_t slashes = 0;
    for (wchar_t c : a) {
      if (c == L'\\') {
        ++slashes;
      } else {
        if (c == L'"') line.append(slashes + 1, L'\\');
        slashes = 0;
      }
      line += c;
    }
    line.append(slashes, L'\\');
    line += L'"';
  }
  LocalFree(argv);
  wchar_t exe[MAX_PATH * 4] = {};
  GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  REXLOG_INFO("Restart: starting the game again");
  FlushLog();
  if (!CreateProcessW(exe, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    REXLOG_WARN("Restart: couldn't start the game again (error {})", GetLastError());
    return;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  std::_Exit(0);  // as closing the window does (ReXApp::OnClosing)
}

#else

void RestartNow() {
  // The command line as started: NUL-separated in /proc/self/cmdline.
  std::ifstream in("/proc/self/cmdline", std::ios::binary);
  const std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::vector<std::string> args;
  for (size_t start = 0; start < raw.size();) {
    const size_t end = raw.find('\0', start);
    args.push_back(raw.substr(start, end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  if (args.empty()) {
    REXLOG_WARN("Restart: couldn't read the command line");
    return;
  }
  std::vector<std::string> kept{args[0]};
  for (size_t i = 1; i < args.size(); ++i) {
    if (IsLoadSave(args[i])) {
      if (args[i] == "--load_save") ++i;  // its value too
      continue;
    }
    kept.push_back(args[i]);
  }
  std::vector<char*> argv;
  for (std::string& a : kept) argv.push_back(a.data());
  argv.push_back(nullptr);
  REXLOG_INFO("Restart: starting the game again ({} arguments)", kept.size() - 1);
  FlushLog();
  // Everything but stdin / stdout / stderr closes in the new program: the
  // debug console's socket, the log file (opened again by the new run), the
  // GPU's files.
#if defined(__linux__) && defined(SYS_close_range)
  syscall(SYS_close_range, 3u, ~0u, CLOSE_RANGE_CLOEXEC);
#else
  for (int fd = 3; fd < 1024; ++fd) fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
  execv("/proc/self/exe", argv.data());
  REXLOG_WARN("Restart: couldn't start the game again (execv failed)");
}

#endif

}  // namespace app_restart
