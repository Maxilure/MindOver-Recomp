// =============================================================================
// game_process.cpp -- see game_process.h
// =============================================================================
#include "game_process.h"

#include <csignal>
#include <cstring>
#include <ctime>
#include <fstream>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

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
  std::tm local{};
  localtime_r(&now, &local);
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

  // argv, built BEFORE fork: between fork and exec the child may only call
  // async-signal-safe functions (no memory allocation).
  std::vector<std::string> argv_strings;
  argv_strings.push_back(exe.string());
  argv_strings.insert(argv_strings.end(), args.begin(), args.end());
  argv_strings.push_back("--log_file=" + log_file.string());
  std::vector<char*> argv;
  for (auto& arg : argv_strings) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);
  const std::string exe_path = exe.string();
  const std::string work_path = work_folder.string();

  int pipe_fds[2];
  if (pipe2(pipe_fds, O_CLOEXEC) != 0) {
    *error = std::string("Couldn't make a pipe: ") + std::strerror(errno);
    return false;
  }
  started_ = std::chrono::steady_clock::now();

  const pid_t pid = fork();
  if (pid < 0) {
    *error = std::string("Couldn't start the game: ") + std::strerror(errno);
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return false;
  }
  if (pid == 0) {
    // THE CHILD. Its own session (and process group): Stop() can signal the
    // whole game, a Ctrl+C in the launcher's terminal doesn't reach it, and
    // closing the launcher doesn't close the game.
    setsid();
    const int null_fd = open("/dev/null", O_RDONLY);
    if (null_fd >= 0) {
      dup2(null_fd, STDIN_FILENO);
    }
    dup2(pipe_fds[1], STDOUT_FILENO);  // dup2 clears close-on-exec on the copies
    dup2(pipe_fds[1], STDERR_FILENO);
    close_range(3, ~0U, 0);  // nothing of the launcher's (window, sockets) leaks in
    if (chdir(work_path.c_str()) != 0) {
      _exit(126);
    }
    execv(exe_path.c_str(), argv.data());
    const char message[] = "launcher: couldn't run the game's executable\n";
    (void)!write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(127);
  }

  close(pipe_fds[1]);
  {
    std::lock_guard lock(mutex_);
    state_ = State::kRunning;
    result_ = {};
    pid_ = pid;
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
  watcher_ = std::thread(&Game::Watch, this, int(pid), pipe_fds[0]);
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
void Game::Watch(int pid, int output_fd) {
  std::ofstream copy(user_folder_ / "logs" / "last-launch-output.txt", std::ios::trunc);
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
    std::lock_guard lock(mutex_);
    TakeLines(log_pending, [&](std::string line) { AddLine(std::move(line)); });
  };

  bool open = true;
  while (open) {
    pollfd poller{output_fd, POLLIN, 0};
    const int ready = poll(&poller, 1, 250);
    if (ready > 0) {
      const ssize_t got = read(output_fd, buffer, sizeof(buffer));
      if (got < 0 && errno == EINTR) {
        continue;
      }
      if (got <= 0) {
        open = false;  // the game closed its end: it has ended
      } else {
        copy.write(buffer, got);
        copy.flush();
        terminal_pending.append(buffer, size_t(got));
        std::lock_guard lock(mutex_);
        TakeLines(terminal_pending,
                  [&](std::string line) { AddLine("terminal: " + std::move(line)); });
      }
    }
    read_log();
  }
  close(output_fd);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  read_log();  // the last lines it wrote
  Finish(status);
}

void Game::Finish(int status) {
  std::lock_guard lock(mutex_);
  Result result;
  result.stopped_by_launcher = stop_requested_;
  if (WIFSIGNALED(status)) {
    result.signal = WTERMSIG(status);
    switch (result.signal) {
      case SIGSEGV: case SIGABRT: case SIGBUS: case SIGILL: case SIGFPE: case SIGTRAP: case SIGSYS:
        result.crashed = true;
        break;
      default:
        result.killed = !result.stopped_by_launcher;
        break;
    }
  } else if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  }
  result.played = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - started_);
  result.log = log_file_;
  result.fps = fps_;
  result_ = std::move(result);
  state_ = State::kEnded;
  pid_ = -1;
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
  int pid;
  {
    std::lock_guard lock(mutex_);
    if (state_ != State::kRunning || pid_ <= 0) {
      return;
    }
    pid = pid_;
    stop_requested_ = true;
  }
  kill(-pid, SIGTERM);  // the whole process group (setsid in Start); ends in ~0.5 s
  // Still there after 5 s: force it. Checked against the same pid under the
  // lock, so a later session is never hit. (The Game object lives as long as
  // the process: main.cpp never deletes it.)
  std::thread([this, pid] {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    std::lock_guard lock(mutex_);
    if (state_ == State::kRunning && pid_ == pid) {
      kill(-pid, SIGKILL);
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

bool AnotherGameRunning() {
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator("/proc", ec)) {
    const std::string name = entry.path().filename().string();
    if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) {
      continue;
    }
    std::ifstream comm(entry.path() / "comm");
    std::string command;
    if (std::getline(comm, command) && command == "crash_mom") {
      return true;
    }
  }
  return false;
}

}  // namespace game_process
