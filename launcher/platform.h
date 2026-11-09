// =============================================================================
// platform.h -- everything the launcher does differently on Linux and Windows
// =============================================================================
//
// The rest of the launcher is shared code; it reaches the operating system
// only through this file (platform_posix.cpp on Linux, platform_win.cpp on
// Windows):
//
//   CHILD PROCESSES (the game, build steps, quick tool calls): started with
//   their output (stdout + stderr) on a pipe, in a folder, optionally with
//   their own environment (Windows builds run in Visual Studio's x64
//   environment). Stopping one stops what it started too:
//     Linux    its own session / process group; TERM, then KILL
//     Windows  a "job" holding it and its children; WM_CLOSE to the game's
//              windows (a normal close), else TerminateJobObject
//   die_with_launcher (build steps): if the launcher goes away, so do they
//     Linux    PR_SET_PDEATHSIG     Windows  job with KILL_ON_JOB_CLOSE
//
//   HOW A PROCESS ENDED: a normal exit (code), a CRASH (Linux: SEGV, ABRT,
//   BUS, ILL, FPE, TRAP, SYS; Windows: an NTSTATUS error code such as
//   0xC0000005 access violation, 0xC00000FD stack overflow), or ended from
//   outside (Linux: KILL, TERM, ...).
//
//   PLACES AND FACTS: the launcher's own file, the user's home, PATH lookup
//   (Windows also tries .exe / .cmd / .bat), free RAM and disk, CPU cores,
//   whether a program or a library is present, the desktop's fonts, a
//   restart of the launcher (after an update).
// =============================================================================
#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace platform {

// --- places --------------------------------------------------------------------

std::filesystem::path SelfPath();   // the running launcher's own file
std::filesystem::path HomeDir();    // ~ / %USERPROFILE%
// The first `name` on PATH ("cmake"; Windows adds .exe / .cmd / .bat), or a
// path that's given directly; empty if none.
std::filesystem::path FindProgram(const std::string& name);
// A program's file name: "crash_mom" -> "crash_mom.exe" on Windows.
std::string ExeName(const std::string& name);

// An environment variable of THIS process ("" when unset). On Windows read
// through the wide API, so it sees what RefreshEnvironment set (the C
// runtime's getenv keeps its own copy from the start).
std::string GetEnv(const char* name);

// WINDOWS: re-reads PATH (and VULKAN_SDK) from the registry, where installers
// write them, into this process. A program keeps the PATH it was started
// with, so tools installed while the installer is open (winget, the Visual
// Studio Installer) stayed "not found" until it was restarted, sometimes
// until the player logged out: the most likely reason testers reported
// that the setup "never works" (found reading setup.cpp, 2026-11-05). The
// new PATH = the system's + the user's (as Windows builds it at log-on),
// then any folders this process had that neither lists (kept, not lost).
// Linux: nothing to do.
void RefreshEnvironment();

// THE GAME'S ICON (2026-11-05): writes an .ico file (tools/make_icon.py: the
// game's own portrait of Crash as a mask, from the player's disc) into an exe
// as its icon resource (RT_ICON per picture + RT_GROUP_ICON 1). Windows shows
// an exe's first icon group in Explorer, the Start menu and the Apps list,
// and SDL gives it to the game's window (the taskbar). Windows: UpdateResource;
// Linux: nothing (true). False + `error` when the exe couldn't be changed
// (e.g. another program has it open): the game just has no icon then.
bool EmbedIcon(const std::filesystem::path& exe, const std::filesystem::path& ico, std::string* error);

// --- facts ---------------------------------------------------------------------

double MemAvailableGb();
int Cores();
double FreeDiskGb(const std::filesystem::path& where);  // -1 = unknown
bool LibraryAvailable(const char* linux_name, const char* windows_name);
bool ProcessRunning(const std::string& name);  // a process with this exe name ("crash_mom")
std::tm LocalTime(std::time_t t);

struct FontFiles {
  std::filesystem::path regular, bold, mono;  // any may be empty
};
FontFiles SystemFonts();

// --- child processes ----------------------------------------------------------

using Environment = std::map<std::string, std::string>;

struct ChildOptions {
  std::vector<std::string> argv;       // argv[0]: a program name (PATH) or a path
  std::filesystem::path work_folder;   // empty = the launcher's
  bool die_with_launcher = false;      // build steps (see the header)
  const Environment* environment = nullptr;  // the WHOLE environment, or null = inherit
};

// WINDOWS: tells Windows never to slow THIS process down to save power.
// Windows 11 puts the work of apps that are minimized or hidden on its
// "efficiency" footing (EcoQoS: slow cores, low clocks); a build in a
// minimized installer could crawl or seem to stop (players' reports,
// 2026-11). Build steps' children get the same (Child::Start). Linux: no-op.
void KeepFullSpeed();

// WINDOWS: while `on`, the PC doesn't go to sleep by itself (a long build
// left alone overnight, then asleep for hours, looked like a 5-hour
// install). The screen may still turn off. Linux: no-op.
void KeepAwake(bool on);

struct Ended {
  bool crashed = false;   // a crash (see the header)
  bool killed = false;    // ended from outside (Linux signals other than crash ones)
  int exit_code = 0;      // the exit code (when neither)
  uint32_t raw = 0;       // signal number (Linux) or the exit code / NTSTATUS (Windows)
  std::string description;  // "SIGSEGV (Segmentation fault)", "0xC0000005 (access violation)"
};

class Child {
 public:
  Child();
  ~Child();
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;

  bool Start(const ChildOptions& options, std::string* error);
  // Up to `size` bytes of output: > 0 read, 0 = nothing within timeout_ms,
  // -1 = the output closed (the process ended or closed it).
  int Read(char* buffer, size_t size, int timeout_ms);
  Ended Wait();              // waits for the end (call once)
  void RequestStop();        // a polite stop (TERM / WM_CLOSE)
  void ForceStop();          // KILL / TerminateJobObject
  bool running() const;
  int id() const;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

// Runs `argv` quietly and returns its output; `*ok` = started and exited 0
// within `timeout_ms` (else stopped).
std::string Capture(const std::vector<std::string>& argv, bool* ok,
                    const std::filesystem::path& work_folder = {}, int timeout_ms = 10000,
                    const Environment* environment = nullptr);

// This process's environment (for building a modified one).
Environment CurrentEnvironment();

// Replaces this launcher with a fresh start of `program` + `args` (after an
// update). Returns only on failure.
void Restart(const std::filesystem::path& program, const std::vector<std::string>& args);

}  // namespace platform
