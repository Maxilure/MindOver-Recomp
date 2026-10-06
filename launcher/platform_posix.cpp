// =============================================================================
// platform_posix.cpp -- platform.h on Linux
// =============================================================================
#include "platform.h"

#if !defined(_WIN32)

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace fs = std::filesystem;

namespace platform {

fs::path SelfPath() {
  std::error_code ec;
  const fs::path self = fs::read_symlink("/proc/self/exe", ec);
  return ec ? fs::path() : self;
}

fs::path HomeDir() {
  const char* home = std::getenv("HOME");
  return home && *home ? fs::path(home) : fs::path();
}

std::string ExeName(const std::string& name) { return name; }

fs::path FindProgram(const std::string& name) {
  if (name.empty()) {
    return {};
  }
  std::error_code ec;
  if (name.find('/') != std::string::npos) {
    return fs::is_regular_file(name, ec) ? fs::path(name) : fs::path();
  }
  const char* path = std::getenv("PATH");
  std::stringstream dirs(path ? path : "/usr/local/bin:/usr/bin:/bin");
  std::string dir;
  while (std::getline(dirs, dir, ':')) {
    const fs::path candidate = fs::path(dir.empty() ? "." : dir) / name;
    if (fs::is_regular_file(candidate, ec) && access(candidate.c_str(), X_OK) == 0) {
      return candidate;
    }
  }
  return {};
}

double MemAvailableGb() {
  std::ifstream in("/proc/meminfo");
  std::string key, unit;
  long kb = 0;
  while (in >> key >> kb >> unit) {
    if (key == "MemAvailable:") {
      return double(kb) / 1e6;
    }
  }
  return 0;
}

int Cores() { return int(std::max(1L, sysconf(_SC_NPROCESSORS_ONLN))); }

double FreeDiskGb(const fs::path& where) {
  struct statvfs info{};
  if (statvfs(where.c_str(), &info) != 0) {
    return -1;
  }
  return double(info.f_bavail) * double(info.f_frsize) / 1e9;
}

bool LibraryAvailable(const char* linux_name, const char*) {
  if (void* lib = dlopen(linux_name, RTLD_LAZY)) {
    dlclose(lib);
    return true;
  }
  return false;
}

bool ProcessRunning(const std::string& name) {
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator("/proc", ec)) {
    const std::string pid = entry.path().filename().string();
    if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos) {
      continue;
    }
    std::ifstream comm(entry.path() / "comm");
    std::string command;
    if (std::getline(comm, command) && command == name) {
      return true;
    }
  }
  return false;
}

std::tm LocalTime(std::time_t t) {
  std::tm local{};
  localtime_r(&t, &local);
  return local;
}

namespace {

// A system font file for `pattern` (fontconfig's fc-match), or empty.
fs::path FcMatch(const char* pattern) {
  const std::string command = std::string("fc-match -f '%{file}' '") + pattern + "' 2>/dev/null";
  std::string path;
  if (FILE* pipe = popen(command.c_str(), "r")) {
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe)) {
      path += buffer;
    }
    pclose(pipe);
  }
  std::error_code ec;
  return !path.empty() && fs::is_regular_file(path, ec) ? fs::path(path) : fs::path();
}

}  // namespace

FontFiles SystemFonts() {
  return {FcMatch("sans:style=Regular"), FcMatch("sans:style=Bold"),
          FcMatch("monospace:style=Regular")};
}

Environment CurrentEnvironment() {
  Environment env;
  for (char** e = environ; e && *e; ++e) {
    const std::string entry = *e;
    const size_t eq = entry.find('=');
    if (eq != std::string::npos) {
      env[entry.substr(0, eq)] = entry.substr(eq + 1);
    }
  }
  return env;
}

// --- child processes ----------------------------------------------------------

struct Child::State {
  pid_t pid = -1;
  int fd = -1;
  bool waited = false;
};

Child::Child() : state_(std::make_unique<State>()) {}

Child::~Child() {
  if (state_->fd >= 0) {
    close(state_->fd);
  }
}

bool Child::Start(const ChildOptions& options, std::string* error) {
  const fs::path program = FindProgram(options.argv.empty() ? std::string() : options.argv[0]);
  if (program.empty()) {
    *error = "Couldn't find " + (options.argv.empty() ? std::string("?") : options.argv[0]) +
             " (is it installed?)";
    return false;
  }
  // Everything the child needs, built BEFORE fork (after it, only
  // async-signal-safe calls: no allocation).
  std::vector<std::string> args = options.argv;
  std::vector<char*> argv;
  for (auto& arg : args) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);
  std::vector<std::string> env_strings;
  std::vector<char*> envp;
  if (options.environment) {
    for (const auto& [key, value] : *options.environment) {
      env_strings.push_back(key + "=" + value);
    }
    for (auto& entry : env_strings) {
      envp.push_back(entry.data());
    }
    envp.push_back(nullptr);
  }
  const std::string program_path = program.string();
  const std::string work = options.work_folder.string();
  const bool die_with_launcher = options.die_with_launcher;

  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0) {
    *error = std::string("Couldn't make a pipe: ") + std::strerror(errno);
    return false;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    *error = std::string("Couldn't start ") + program_path + ": " + std::strerror(errno);
    close(fds[0]);
    close(fds[1]);
    return false;
  }
  if (pid == 0) {
    // THE CHILD: its own session (and process group), so a stop reaches
    // everything it starts and a Ctrl+C in the launcher's terminal doesn't.
    setsid();
    if (die_with_launcher) {
      prctl(PR_SET_PDEATHSIG, SIGTERM);  // the launcher gone = this step too
      if (getppid() == 1) {
        _exit(125);  // the launcher was already gone before prctl took hold
      }
    }
    const int null_fd = open("/dev/null", O_RDONLY);
    if (null_fd >= 0) {
      dup2(null_fd, STDIN_FILENO);
    }
    dup2(fds[1], STDOUT_FILENO);  // dup2 clears close-on-exec on the copies
    dup2(fds[1], STDERR_FILENO);
    close_range(3, ~0U, 0);       // nothing of the launcher's (window, sockets) leaks in
    if (!work.empty() && chdir(work.c_str()) != 0) {
      _exit(126);
    }
    if (!envp.empty()) {
      execve(program_path.c_str(), argv.data(), envp.data());
    } else {
      execv(program_path.c_str(), argv.data());
    }
    const char message[] = "launcher: couldn't run the program\n";
    (void)!write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(127);
  }
  close(fds[1]);
  state_->pid = pid;
  state_->fd = fds[0];
  return true;
}

int Child::Read(char* buffer, size_t size, int timeout_ms) {
  if (state_->fd < 0) {
    return -1;
  }
  pollfd poller{state_->fd, POLLIN, 0};
  const int ready = poll(&poller, 1, timeout_ms);
  if (ready <= 0) {
    return ready < 0 && errno != EINTR ? -1 : 0;
  }
  const ssize_t got = read(state_->fd, buffer, size);
  if (got < 0 && errno == EINTR) {
    return 0;
  }
  if (got <= 0) {
    close(state_->fd);
    state_->fd = -1;
    return -1;
  }
  return int(got);
}

Ended Child::Wait() {
  Ended ended;
  if (state_->pid <= 0 || state_->waited) {
    return ended;
  }
  int status = 0;
  while (waitpid(state_->pid, &status, 0) < 0 && errno == EINTR) {
  }
  state_->waited = true;
  if (WIFSIGNALED(status)) {
    const int signal = WTERMSIG(status);
    ended.raw = uint32_t(signal);
    switch (signal) {
      case SIGSEGV: case SIGABRT: case SIGBUS: case SIGILL: case SIGFPE: case SIGTRAP: case SIGSYS:
        ended.crashed = true;
        break;
      default:
        ended.killed = true;
        break;
    }
    const char* abbrev = sigabbrev_np(signal);
    const char* text = sigdescr_np(signal);
    ended.description = abbrev ? std::string("SIG") + abbrev : "signal " + std::to_string(signal);
    if (text) {
      ended.description += std::string(" (") + text + ")";
    }
  } else if (WIFEXITED(status)) {
    ended.exit_code = WEXITSTATUS(status);
    ended.raw = uint32_t(ended.exit_code);
  }
  return ended;
}

void Child::RequestStop() {
  if (state_->pid > 0 && !state_->waited) {
    kill(-state_->pid, SIGTERM);  // the whole process group (setsid in Start)
  }
}

void Child::ForceStop() {
  if (state_->pid > 0 && !state_->waited) {
    kill(-state_->pid, SIGKILL);
  }
}

bool Child::running() const { return state_->pid > 0 && !state_->waited; }
int Child::id() const { return state_->pid; }

std::string Capture(const std::vector<std::string>& argv, bool* ok, const fs::path& work_folder,
                    int timeout_ms, const Environment* environment) {
  *ok = false;
  Child child;
  std::string error;
  if (!child.Start({argv, work_folder, false, environment}, &error)) {
    return {};
  }
  std::string out;
  char buffer[4096];
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - std::chrono::steady_clock::now())
                          .count();
    if (left <= 0) {
      child.ForceStop();
      break;
    }
    const int got = child.Read(buffer, sizeof(buffer), int(std::min<long long>(left, 200)));
    if (got < 0) {
      break;
    }
    out.append(buffer, size_t(std::max(got, 0)));
  }
  const Ended ended = child.Wait();
  *ok = !ended.crashed && !ended.killed && ended.exit_code == 0;
  return out;
}

void Restart(const fs::path& program, const std::vector<std::string>& args) {
  std::vector<std::string> all = {program.string()};
  all.insert(all.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (auto& arg : all) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);
  execv(argv[0], argv.data());
}

}  // namespace platform

#endif  // !_WIN32
