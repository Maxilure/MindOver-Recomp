// =============================================================================
// task_runner.h -- runs a list of commands one after another, for the Setup tab
// =============================================================================
//
// A setup step is a few commands (e.g. "configure, recompile, configure again,
// compile"). The runner starts them in order on a thread of its own, each as a
// child process (platform.h: Cancel stops it and everything it started; and
// it ends by itself if the launcher goes away), with stdout + stderr read line by line into a list the tab
// shows live and a log file (user/logs/setup-<date>_<time>.log, one per run).
// A command that fails (exit code != 0, or a signal) stops the list.
//
// PROGRESS: each command can say how to read its progress from its output:
//   kNinja       "[123/456] Building ..." lines (CMake + Ninja builds)
//   kCountLines  lines starting with `count_prefix`, out of `count_total`
//                (the disc extractor prints one line per file)
//   kNone        no progress: the bar shows the step only
// Overall progress = (commands done + this one's fraction) / commands.
// =============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "platform.h"

namespace task_runner {

enum class Progress { kNone, kNinja, kCountLines };

struct Command {
  std::string title;                    // "Compiling the game"
  std::vector<std::string> argv;        // argv[0] = program (searched in PATH)
  std::filesystem::path work_folder;
  Progress progress = Progress::kNone;
  std::string count_prefix;             // kCountLines
  int count_total = 0;                  // kCountLines
  // Optional: the whole environment to run it in (Windows: Visual Studio's
  // x64 build environment); null = the launcher's own.
  std::shared_ptr<const platform::Environment> environment;
  // Optional: run in-process instead of a child (quick checks between
  // commands, e.g. counting the disc's files). Returns false + message on
  // failure. When set, argv is ignored.
  std::function<bool(std::string* message)> function;
};

enum class State { kIdle, kRunning, kDone, kFailed, kCancelled };

struct Snapshot {
  State state = State::kIdle;
  std::string title;                    // the whole job ("Build the game")
  std::string step;                     // the command running now
  double fraction = 0;                  // 0..1, overall
  std::string detail;                   // "1234 / 4567" or ""
  std::string error;                    // why it failed
  std::filesystem::path log;
  uint64_t version = 0;                 // grows with every new line / change
  std::vector<std::string> lines;       // the output (last kKeptLines)
};

class Runner {
 public:
  ~Runner();

  // Starts `commands` (false if a job is still running). Output also goes to
  // `log_file`. `on_done(success)` runs on the runner's thread at the end.
  bool Start(std::string title, std::vector<Command> commands,
             const std::filesystem::path& log_file,
             std::function<void(bool success)> on_done = {});

  void Cancel();
  bool Running();

  // Copies the state into `view` if it changed (lines appended when possible).
  bool Copy(Snapshot& view);
  void Forget();  // kDone / kFailed / kCancelled -> kIdle

 private:
  void Run(std::vector<Command> commands, std::filesystem::path log_file,
           std::function<void(bool)> on_done);
  bool RunOne(const Command& command, size_t index, size_t count, std::ofstream& log);
  void AddLine(std::string line, std::ofstream& log);  // takes mutex_

  std::mutex mutex_;
  Snapshot state_;
  size_t dropped_ = 0;
  std::deque<std::string> lines_;
  std::thread thread_;
  std::mutex child_mutex_;
  platform::Child* child_ = nullptr;  // the running command's process (under child_mutex_)
  std::atomic<bool> cancel_{false};
};

// (Kept for the callers' convenience: platform.h does the work.)
inline std::filesystem::path FindProgram(const std::string& name) {
  return platform::FindProgram(name);
}
inline std::string Capture(const std::vector<std::string>& argv, bool* ok,
                           const std::filesystem::path& work_folder = {}, int timeout_ms = 10000,
                           const platform::Environment* environment = nullptr) {
  return platform::Capture(argv, ok, work_folder, timeout_ms, environment);
}

}  // namespace task_runner
