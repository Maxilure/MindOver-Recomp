#include "discord_presence.h"

#ifdef _WIN32
#include "gameplay/game_state.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <chrono>
#include <format>
#include <string>
#include <string_view>
#include <thread>

#include <rex/logging.h>

namespace discord_presence {
namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr std::array<std::string_view, 15> kAreas = {
    "In Game", "Main Menu", "Starting Game", "Title Screen", "Watching Movie",
    "Wumpa Island", "Ratcicle Kingdom", "Frozen Coast", "Wasteland",
    "Evil Public School", "Junkyard", "Mount Grimly", "Space Head",
    "Wumpa Island", "Crash's House",
};
constexpr std::array<std::string_view, 3> kDifficulties = {"Mild", "Tricky", "Bonkers"};
constexpr std::string_view kApplicationId = "1569078085200904435";
std::jthread worker;

std::string_view Details(const game_state::Snapshot& current) {
  using game_state::Phase;
  switch (current.phase) {
    case Phase::kStarting: return "Starting Game";
    case Phase::kFrontEnd: return "Main Menu";
    case Phase::kLoading: return "Loading";
    case Phase::kMovie: return "Watching Movie";
    case Phase::kInGame:
      if (current.area_id && *current.area_id >= 5 && *current.area_id < kAreas.size()) {
        return kAreas[*current.area_id];
      }
      return "In Game";
  }
  return "In Game";
}

std::string Activity(int64_t session_start) {
  const auto current = game_state::Current();
  std::string state;
  if (current.difficulty && *current.difficulty < kDifficulties.size()) {
    state = std::format("Difficulty: {}", kDifficulties[*current.difficulty]);
  }
  if (current.completion) {
    if (!state.empty()) state += " | ";
    state += std::format("Completion: {}%", *current.completion);
  }

  // The Discord client uses the registered application's icon.
  auto activity = std::format(R"({{"details":"{}")", Details(current));
  activity += std::format(R"(,"timestamps":{{"start":{}}})", session_start);
  if (!state.empty()) activity += std::format(R"(,"state":"{}")", state);
  return activity + '}';
}

class Pipe {
 public:
  ~Pipe() {
    if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
    if (event_) CloseHandle(event_);
  }

  bool Open(int index) {
    event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const auto path = std::format(LR"(\\.\pipe\discord-ipc-{})", index);
    pipe_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    return event_ && pipe_ != INVALID_HANDLE_VALUE;
  }

  bool Send(uint32_t opcode, std::string_view payload) {
    const std::array<uint32_t, 2> header{opcode, static_cast<uint32_t>(payload.size())};
    std::string packet(reinterpret_cast<const char*>(header.data()), sizeof(header));
    packet += payload;
    return Transfer(packet.data(), static_cast<DWORD>(packet.size()), true);
  }

  bool Available(DWORD& bytes) {
    return PeekNamedPipe(pipe_, nullptr, 0, nullptr, &bytes, nullptr) != FALSE;
  }

  bool Receive(uint32_t& opcode, std::string& payload) {
    std::array<uint32_t, 2> header{};
    if (!Transfer(header.data(), sizeof(header), false) || header[1] > 64 * 1024) return false;
    opcode = header[0];
    payload.resize(header[1]);
    return Transfer(payload.data(), header[1], false);
  }

 private:
  bool Transfer(void* buffer, DWORD size, bool write) {
    auto* bytes = static_cast<char*>(buffer);
    while (size) {
      OVERLAPPED operation{};
      operation.hEvent = event_;
      ResetEvent(event_);
      DWORD done = 0;
      const BOOL completed = write ? WriteFile(pipe_, bytes, size, nullptr, &operation)
                                   : ReadFile(pipe_, bytes, size, nullptr, &operation);
      if (!completed) {
        if (GetLastError() != ERROR_IO_PENDING) return false;
        if (WaitForSingleObject(event_, 300) != WAIT_OBJECT_0) {
          CancelIoEx(pipe_, &operation);
          GetOverlappedResult(pipe_, &operation, &done, TRUE);
          return false;
        }
      }
      if (!GetOverlappedResult(pipe_, &operation, &done, FALSE) || !done) return false;
      bytes += done;
      size -= done;
    }
    return true;
  }

  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  HANDLE event_ = nullptr;
};

bool SendActivity(Pipe& pipe, std::string_view activity, unsigned nonce) {
  return pipe.Send(1, std::format(
      R"({{"cmd":"SET_ACTIVITY","args":{{"pid":{},"activity":{}}},"nonce":"{}"}})",
      GetCurrentProcessId(), activity, nonce));
}

void Run(std::stop_token stop) {
  // Keep the Discord timer anchored to process launch, including after reconnects.
  FILETIME created, exited, kernel, user;
  auto session_start = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
    constexpr int64_t kSecondsFromWindowsToUnixEpoch = 11'644'473'600;
    const uint64_t ticks = (uint64_t{created.dwHighDateTime} << 32) | created.dwLowDateTime;
    session_start = static_cast<int64_t>(ticks / 10'000'000) - kSecondsFromWindowsToUnixEpoch;
  }
  while (!stop.stop_requested()) {
    for (int index = 0; index < 10 && !stop.stop_requested(); ++index) {
      Pipe pipe;
      if (!pipe.Open(index) ||
          !pipe.Send(0, std::format(R"({{"v":1,"client_id":"{}"}})", kApplicationId))) continue;

      uint32_t opcode;
      std::string response;
      if (!pipe.Receive(opcode, response) || opcode != 1 ||
          response.find("\"READY\"") == std::string::npos) continue;

      REXLOG_INFO("Discord Rich Presence: connected to the desktop client");
      std::string sent;
      unsigned nonce = 0;
      auto next_send = Clock::now();
      while (!stop.stop_requested()) {
        DWORD available = 0;
        if (!pipe.Available(available)) break;
        if (available >= 8) {
          if (!pipe.Receive(opcode, response) || opcode == 2) break;
          if (opcode == 3 && !pipe.Send(4, response)) break;
          if (opcode == 1 && response.find("\"ERROR\"") != std::string::npos) {
            REXLOG_WARN("Discord Rich Presence: the client rejected an activity update");
          }
        }
        const auto now = Clock::now();
        if (now >= next_send) {
          auto activity = Activity(session_start);
          if (activity != sent) {
            if (!SendActivity(pipe, activity, ++nonce)) break;
            sent = std::move(activity);
            next_send = now + 15s;
          }
        }
        std::this_thread::sleep_for(200ms);
      }
      if (stop.stop_requested()) SendActivity(pipe, "null", ++nonce);
      break;
    }
    for (int i = 0; i < 15 && !stop.stop_requested(); ++i) {
      std::this_thread::sleep_for(200ms);
    }
  }
}
}  // namespace

void Start() {
  if (worker.joinable()) return;
  worker = std::jthread(Run);
}

void Stop() {
  if (worker.joinable()) {
    worker.request_stop();
    worker.join();
  }
}
}  // namespace discord_presence
#else
namespace discord_presence {
void Start() {}
void Stop() {}
}  // namespace discord_presence
#endif
