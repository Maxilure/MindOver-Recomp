// =============================================================================
// task_runner.cpp -- see task_runner.h
// =============================================================================
#include "task_runner.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace task_runner {
namespace {

constexpr size_t kKeptLines = 20000;

// "[123/456] ..." -> 123, 456 (Ninja's progress prefix). False if not one.
bool NinjaProgress(const std::string& line, int* done, int* total) {
  if (line.size() < 5 || line[0] != '[') {
    return false;
  }
  return std::sscanf(line.c_str(), "[%d/%d]", done, total) == 2 && *total > 0;
}

// Known failures whose tool message doesn't say what to do: a line of the
// output -> a plain explanation added to the error. Empty if not one.
//   "manifest 'build.ninja' still dirty after 100 tries": Ninja regenerates
//   build.ninja whenever one of its inputs is NEWER than it. A file dated in
//   the future (the PC's clock was wrong when the files were written, or is
//   wrong now) stays "newer" forever. Seen on a Windows test PC whose clock
//   had the right time of day but the wrong time zone; syncing the clock in
//   Windows' Date & time settings fixed it.
std::string KnownProblemHint(const std::string& line) {
  if (line.find("still dirty after") != std::string::npos) {
    return "The computer's clock looks wrong (files seem to come from the future). Set the date, time and "
           "time zone correctly (Windows: Settings > Time & language > Date & time > Sync now), then run "
           "this step again.";
  }
  return {};
}

std::string Joined(const std::vector<std::string>& args) {
  std::string text;
  for (const auto& arg : args) {
    if (!text.empty()) {
      text += ' ';
    }
    text += arg.find(' ') != std::string::npos ? "\"" + arg + "\"" : arg;
  }
  return text;
}

}  // namespace

Runner::~Runner() {
  Cancel();
  if (thread_.joinable()) {
    thread_.join();
  }
}

bool Runner::Start(std::string title, std::vector<Command> commands, const fs::path& log_file,
                   std::function<void(bool)> on_done) {
  {
    std::lock_guard lock(mutex_);
    if (state_.state == State::kRunning) {
      return false;
    }
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  {
    std::lock_guard lock(mutex_);
    state_ = {};
    state_.state = State::kRunning;
    state_.title = std::move(title);
    state_.log = log_file;
    lines_.clear();
    dropped_ = 0;
    ++state_.version;
  }
  cancel_ = false;
  thread_ = std::thread(&Runner::Run, this, std::move(commands), log_file, std::move(on_done));
  return true;
}

void Runner::AddLine(std::string line, std::ofstream& log) {
  log << line << '\n';
  log.flush();
  std::lock_guard lock(mutex_);
  lines_.push_back(std::move(line));
  if (lines_.size() > kKeptLines) {
    lines_.pop_front();
    ++dropped_;
  }
  ++state_.version;
}

void Runner::Run(std::vector<Command> commands, fs::path log_file,
                 std::function<void(bool)> on_done) {
  std::error_code ec;
  fs::create_directories(log_file.parent_path(), ec);
  std::ofstream log(log_file, std::ios::trunc);
  bool ok = true;
  for (size_t i = 0; i < commands.size() && ok; ++i) {
    ok = RunOne(commands[i], i, commands.size(), log);
    // Cancel stops what's LEFT: a last step that finished well is done (an
    // update's swap pressed "Cancel" on while it worked, swapped everything,
    // and was then called cancelled: the update was offered again).
    if (cancel_ && (!ok || i + 1 < commands.size())) {
      ok = false;
    }
  }
  {
    std::lock_guard lock(mutex_);
    state_.state = ok ? State::kDone : cancel_ ? State::kCancelled : State::kFailed;
    state_.cancellable = false;
    if (ok) {
      state_.fraction = 1;
      state_.detail.clear();
    }
    ++state_.version;
  }
  AddLine(ok ? "== Done." : cancel_ ? "== Cancelled." : "== Failed: " + state_.error, log);
  if (on_done) {
    on_done(ok);
  }
}

bool Runner::RunOne(const Command& command, size_t index, size_t count, std::ofstream& log) {
  {
    std::lock_guard lock(mutex_);
    state_.step = command.title;
    state_.fraction = double(index) / double(count);
    state_.detail.clear();
    state_.cancellable = !command.function;
    ++state_.version;
  }
  AddLine("== " + command.title, log);
  if (command.function) {
    std::string message;
    const bool ok = command.function(&message);
    if (!message.empty()) {
      AddLine(message, log);
    }
    if (!ok) {
      std::lock_guard lock(mutex_);
      state_.error = message.empty() ? command.title + " failed" : message;
    }
    return ok;
  }
  AddLine("$ " + Joined(command.argv), log);

  platform::Child child;
  std::string start_error;
  platform::ChildOptions options{command.argv, command.work_folder, /*die_with_launcher=*/true,
                                 command.environment.get()};
  if (!child.Start(options, &start_error)) {
    std::lock_guard lock(mutex_);
    state_.error = start_error;
    return false;
  }
  {
    std::lock_guard lock(child_mutex_);
    child_ = &child;
  }
  if (cancel_) {
    child.RequestStop();  // cancelled while it was starting
  }
  int counted = 0;
  std::string hint;  // KnownProblemHint of a line, added to the error
  std::string pending;
  char buffer[16384];
  for (;;) {
    const int got = child.Read(buffer, sizeof(buffer), 250);
    if (got < 0) {
      break;  // the output closed: the command has ended
    }
    if (got == 0) {
      continue;
    }
    // Windows tools end lines with CR LF: the CR goes.
    for (int i = 0; i < got; ++i) {
      if (buffer[i] != '\r') pending += buffer[i];
    }
    size_t start = 0, newline;
    while ((newline = pending.find('\n', start)) != std::string::npos) {
      std::string line = pending.substr(start, newline - start);
      start = newline + 1;
      // Progress from the line (task_runner.h).
      double part = -1;
      std::string detail;
      int done = 0, total = 0;
      if (command.progress == Progress::kNinja && NinjaProgress(line, &done, &total)) {
        part = double(done) / total;
        detail = std::to_string(done) + " / " + std::to_string(total);
      } else if (command.progress == Progress::kCountLines && command.count_total > 0 &&
                 line.starts_with(command.count_prefix)) {
        ++counted;
        part = std::min(1.0, double(counted) / command.count_total);
        detail = std::to_string(counted) + " / " + std::to_string(command.count_total);
      }
      if (part >= 0) {
        std::lock_guard lock(mutex_);
        state_.fraction = (double(index) + part) / double(count);
        state_.detail = detail;
      }
      if (hint.empty()) {
        hint = KnownProblemHint(line);
      }
      AddLine(std::move(line), log);
    }
    pending.erase(0, start);
  }
  if (!pending.empty()) {
    AddLine(pending, log);
  }
  const platform::Ended ended = child.Wait();
  {
    std::lock_guard lock(child_mutex_);
    child_ = nullptr;
  }
  const bool ok = !ended.crashed && !ended.killed && ended.exit_code == 0;
  if (!ok) {
    std::lock_guard lock(mutex_);
    state_.error = (ended.crashed || ended.killed)
                       ? command.title + ": stopped (" + ended.description + ")"
                       : command.title + " failed (exit code " + std::to_string(ended.exit_code) + ")";
    if (!hint.empty()) {
      state_.error += ". " + hint;
    }
  }
  return ok;
}

void Runner::Cancel() {
  cancel_ = true;
  std::lock_guard lock(child_mutex_);
  if (child_) {
    child_->RequestStop();
    // Still there after 5 s: force it (checked against the same command).
    platform::Child* target = child_;
    std::thread([this, target] {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      std::lock_guard again(child_mutex_);
      if (child_ == target) {
        child_->ForceStop();
      }
    }).detach();
  }
}

bool Runner::Running() {
  std::lock_guard lock(mutex_);
  return state_.state == State::kRunning;
}

bool Runner::Copy(Snapshot& view) {
  std::lock_guard lock(mutex_);
  if (view.version == state_.version) {
    return false;
  }
  std::vector<std::string> lines = std::move(view.lines);
  const size_t had = lines.size();
  const bool same_job = view.log == state_.log && view.version != 0;
  view = state_;
  // Append only the new lines when it's the same job and nothing was dropped.
  if (same_job && dropped_ == 0 && had <= lines_.size()) {
    lines.insert(lines.end(), lines_.begin() + ptrdiff_t(had), lines_.end());
  } else {
    lines.assign(lines_.begin(), lines_.end());
  }
  view.lines = std::move(lines);
  return true;
}

void Runner::Forget() {
  std::lock_guard lock(mutex_);
  if (state_.state != State::kRunning) {
    state_.state = State::kIdle;
    ++state_.version;
  }
}

}  // namespace task_runner
