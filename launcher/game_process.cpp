// =============================================================================
// game_process.cpp -- see game_process.h
// =============================================================================
#include "game_process.h"

#include <cstring>
#include <ctime>
#include <fstream>


namespace fs = std::filesystem;

namespace game_process {
namespace {

// Lines kept for the Game log tab. A session logs a few thousand lines a
// minute at most (bursts of one repeated warning while a save loads); the
// oldest are dropped past this, the file keeps everything.
constexpr size_t kKeptLines = 50000;

// "play-2026-10-06_1712.log" (seconds added if that name is taken: two quick
// starts within one minute).
fs::path SessionLogPath(const fs::path& logs) {
  const std::time_t now = std::time(nullptr);
  const std::tm local = platform::LocalTime(now);
  char name[64];
  std::strftime(name, sizeof(name), "play-%Y-%m-%d_%H%M.log", &local);
  std::error_code ec;
  if (fs::exists(logs / name, ec)) {
    std::strftime(name, sizeof(name), "play-%Y-%m-%d_%H%M%S.log", &local);
  }
  return logs / name;
}

// A log line: "[date time] [level] [category] [thread] message"; the message
// (after the 4th "] ") tells one warning from another.
std::string MessageOf(const std::string& line) {
  size_t at = 0;
  for (int i = 0; i < 4 && at != std::string::npos; ++i) {
    at = line.find("] ", at);
    if (at != std::string::npos) {
      at += 2;
    }
  }
  return at == std::string::npos ? line : line.substr(at);
}

// Splits `pending` into whole lines (handing each to `add`); a last partial
// line stays in `pending` until its newline arrives.
template <typename Add>
void TakeLines(std::string& pending, Add&& add) {
  size_t start = 0, newline;
  while ((newline = pending.find('\n', start)) != std::string::npos) {
    add(pending.substr(start, newline - start));
    start = newline + 1;
  }
  pending.erase(0, start);
}

}  // namespace

bool Game::Start(const fs::path& exe, const std::vector<std::string>& args,
                 const fs::path& work_folder, const fs::path& user_folder, std::string* error) {
  {
    std::lock_guard lock(mutex_);
    if (state_ == State::kRunning) {
      *error = "The game is already running.";
      return false;
    }
  }
  if (watcher_.joinable()) {
    watcher_.join();  // the previous session's watcher has finished by now
  }

  std::error_code ec;
  fs::create_directories(user_folder / "logs", ec);
  const fs::path log_file = SessionLogPath(user_folder / "logs");

  platform::ChildOptions options;
  options.argv.push_back(exe.string());
  options.argv.insert(options.argv.end(), args.begin(), args.end());
  options.argv.push_back("--log_file=" + log_file.string());
  options.work_folder = work_folder;
  options.die_with_launcher = false;  // closing the launcher never closes the game
  auto child = std::make_unique<platform::Child>();
  started_ = std::chrono::steady_clock::now();
  if (!child->Start(options, error)) {
    return false;
  }
  {
    std::lock_guard lock(mutex_);
    state_ = State::kRunning;
    result_ = {};
    child_ = std::move(child);
    stop_requested_ = false;
    user_folder_ = user_folder;
    log_file_ = log_file;
    lines_.clear();
    dropped_ = 0;
    errors_ = warnings_ = 0;
    error_kinds_.clear();
    warning_kinds_.clear();
    fps_.clear();
    ++version_;
  }
  watcher_ = std::thread(&Game::Watch, this);
  return true;
}

void Game::AddLine(std::string line) {
  if (line.find("] [error] ") != std::string::npos) {
    ++errors_;
    error_kinds_.insert(MessageOf(line));
  } else if (line.find("] [warning] ") != std::string::npos) {
    ++warnings_;
    warning_kinds_.insert(MessageOf(line));
  }
  if (const size_t at = line.find("frame_rate: at exit, "); at != std::string::npos) {
    fps_ = line.substr(at + 21);
  }
  lines_.push_back(std::move(line));
  if (lines_.size() > kKeptLines) {
    lines_.pop_front();
    ++dropped_;
  }
  ++version_;
}

// The watcher thread: terminal output and the log file's new lines, merged,
// until the game closes its end of the pipe (= it ended); then its exit status.
void Game::Watch() {
  platform::Child* child = child_.get();  // stays until the next Start (after this thread)
  std::ofstream copy(user_folder_ / "logs" / "last-launch-output.txt",
                     std::ios::trunc | std::ios::binary);
  std::ifstream log;               // opened once the game has created the file
  std::string terminal_pending, log_pending;
  char buffer[65536];

  // Whatever the log file grew by since the last call.
  auto read_log = [&] {
    if (!log.is_open()) {
      log.open(log_file_, std::ios::binary);
      if (!log.is_open()) {
        return;
      }
    }
    for (;;) {
      log.read(buffer, sizeof(buffer));
      const std::streamsize got = log.gcount();
      if (got <= 0) {
        break;
      }
      log_pending.append(buffer, size_t(got));
    }
    log.clear();  // at the end for now: clear EOF so the next read continues
    std::erase(log_pending, '\r');  // Windows line ends
    std::lock_guard lock(mutex_);
    TakeLines(log_pending, [&](std::string line) { AddLine(std::move(line)); });
  };

  for (;;) {
    const int got = child->Read(buffer, sizeof(buffer), 250);
    if (got < 0) {
      break;  // the game closed its end: it has ended
    }
    if (got > 0) {
      copy.write(buffer, got);
      copy.flush();
      terminal_pending.append(buffer, size_t(got));
      std::erase(terminal_pending, '\r');
      std::lock_guard lock(mutex_);
      TakeLines(terminal_pending,
                [&](std::string line) { AddLine("terminal: " + std::move(line)); });
    }
    read_log();
  }
  const platform::Ended ended = child->Wait();
  read_log();  // the last lines it wrote
  Finish(ended);
}

void Game::Finish(const platform::Ended& ended) {
  std::lock_guard lock(mutex_);
  Result result;
  result.stopped_by_launcher = stop_requested_;
  result.raw = ended.raw;
  result.ended_by = ended.description;
  result.exit_code = ended.exit_code;
  result.crashed = ended.crashed && !result.stopped_by_launcher;
  result.killed = ended.killed && !result.stopped_by_launcher;
  result.played = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - started_);
  result.log = log_file_;
  result.fps = fps_;
  result_ = std::move(result);
  state_ = State::kEnded;
  ++version_;
}

bool Game::CopyLog(LogView& view) {
  std::lock_guard lock(mutex_);
  if (view.version == version_) {
    return false;
  }
  // Only the new lines when the view is the same session's and nothing it
  // holds has been dropped since; else everything.
  const size_t first_new = view.lines.size() + view.dropped;
  if (view.file == log_file_ && first_new >= dropped_ && first_new - dropped_ <= lines_.size() &&
      view.dropped == dropped_) {
    view.lines.insert(view.lines.end(), lines_.begin() + ptrdiff_t(first_new - dropped_),
                      lines_.end());
  } else {
    view.lines.assign(lines_.begin(), lines_.end());
  }
  view.file = log_file_;
  view.dropped = dropped_;
  view.errors = errors_;
  view.warnings = warnings_;
  view.error_kinds = int(error_kinds_.size());
  view.warning_kinds = int(warning_kinds_.size());
  view.version = version_;
  return true;
}

void Game::Stop() {
  platform::Child* child;
  {
    std::lock_guard lock(mutex_);
    if (state_ != State::kRunning || !child_) {
      return;
    }
    child = child_.get();
    stop_requested_ = true;
  }
  child->RequestStop();  // TERM / closing its window; the game ends in ~0.5 s
  // Still there after 5 s: force it. Checked against the same game under the
  // lock, so a later session is never hit. (The Game object lives as long as
  // the process: main.cpp never deletes it.)
  std::thread([this, child] {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    std::lock_guard lock(mutex_);
    if (state_ == State::kRunning && child_.get() == child) {
      child->ForceStop();
    }
  }).detach();
}

State Game::state() {
  std::lock_guard lock(mutex_);
  return state_;
}

Result Game::result() {
  std::lock_guard lock(mutex_);
  return result_;
}

void Game::Forget() {
  std::lock_guard lock(mutex_);
  if (state_ == State::kEnded) {
    state_ = State::kIdle;
  }
}

bool AnotherGameRunning() { return platform::ProcessRunning("crash_mom"); }

}  // namespace game_process
