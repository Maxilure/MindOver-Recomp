// =============================================================================
// game_process.h -- starting the game, watching it, telling a crash from a quit
// =============================================================================
//
// One game at a time. Start() runs the game's executable as a child process
// (in its own session, working folder = the game folder) and returns at once.
// A watcher thread then follows the session until the game ends:
//
//   THE GAME'S LOG: the launcher names it itself, one file per session:
//   user/logs/play-<date>_<time>.log (the same naming as tools/play.sh), passed
//   as --log_file. Chosen by the launcher on purpose: a log_file left in
//   user/settings.toml (the SDK's F4 menu saves command-line flags too) would
//   otherwise send every session's log into one old file.
//
//   THE TERMINAL OUTPUT (stdout + stderr): nearly empty in normal play (the
//   game logs to its file), but a crash message can land only here. Read
//   through a pipe and also written to user/logs/last-launch-output.txt.
//
//   Both are merged, as they arrive, into one list of lines the launcher's
//   "Game log" tab shows LIVE (terminal lines start with "terminal: "). The
//   watcher polls the pipe with a 250 ms timeout and reads whatever the log
//   file grew by in between. [error] / [warning] lines are counted as they
//   come (in total and how many DIFFERENT ones: the emulated GPU repeats one
//   warning thousands of times while a save loads).
//
// HOW IT ENDED (platform.h): exit code 0 = a normal quit (closing the window
// ends the game with _Exit(0)); a crash (Linux: SIGSEGV, SIGABRT...; Windows:
// an NTSTATUS like 0xC0000005); ended from outside (Linux: SIGKILL, SIGTERM:
// the out-of-memory killer, a shutdown); any other exit code = it gave up
// (e.g. a message box about a missing folder, then exit 1). The Stop button
// asks politely first (TERM / closing its window), then forces it after 5 s.
// =============================================================================
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "platform.h"

namespace game_process {

enum class State { kIdle, kRunning, kEnded };

struct Result {
  bool crashed = false;          // ended by a CRASH signal (SEGV, ABRT, BUS, ILL, FPE, TRAP, SYS)
  bool killed = false;           // ended by another signal from outside (KILL, TERM...: the
                                 // out-of-memory killer, a shutdown, a `kill`)
  std::string ended_by;          // how (crashed or killed): "SIGSEGV (...)", "0xC0000005 (...)"
  uint32_t raw = 0;              // the signal number (Linux) or the exit code / NTSTATUS
  int exit_code = 0;             // exit code (neither crashed nor killed)
  bool stopped_by_launcher = false;  // the Stop button
  std::chrono::seconds played{0};
  std::filesystem::path log;     // the game's log of this session
  std::string fps;               // the frame-rate summary line at exit, if any
};

// The session's log so far (the "Game log" tab; live while the game runs).
struct LogView {
  uint64_t version = 0;          // grows with every change: copy only when it moved
  std::vector<std::string> lines;
  std::filesystem::path file;
  int errors = 0, warnings = 0;
  int error_kinds = 0, warning_kinds = 0;
  size_t dropped = 0;            // oldest lines no longer kept (the file has them all)
};

class Game {
 public:
  // Starts `exe` with `args` (+ --log_file) in `work_folder`; logs go to
  // `user_folder`/logs. False (+ `error`) if it couldn't start or one is
  // already running.
  bool Start(const std::filesystem::path& exe, const std::vector<std::string>& args,
             const std::filesystem::path& work_folder,
             const std::filesystem::path& user_folder, std::string* error);

  // Asks the game to close (SIGTERM), then forces it (SIGKILL) after 5 s.
  void Stop();

  State state();
  Result result();                          // valid once state() == kEnded
  std::chrono::steady_clock::time_point started() const { return started_; }
  void Forget();                            // kEnded -> kIdle (dismiss the report)

  // Copies the log into `view` if it changed since `view.version`; true if it did.
  bool CopyLog(LogView& view);

 private:
  void Watch();
  void AddLine(std::string line);           // under mutex_
  void Finish(const platform::Ended& ended);

  std::mutex mutex_;
  State state_ = State::kIdle;
  Result result_;
  std::unique_ptr<platform::Child> child_;  // the running game (state_ == kRunning)
  bool stop_requested_ = false;
  std::thread watcher_;
  std::chrono::steady_clock::time_point started_;
  std::filesystem::path user_folder_, log_file_;

  // The merged log (see the header).
  std::deque<std::string> lines_;
  uint64_t version_ = 0;
  size_t dropped_ = 0;
  int errors_ = 0, warnings_ = 0;
  std::set<std::string> error_kinds_, warning_kinds_;
  std::string fps_;
};

// Whether a crash_mom process is running already (started some other way,
// e.g. tools/play.sh): two copies would fight over the saves.
bool AnotherGameRunning();

}  // namespace game_process
