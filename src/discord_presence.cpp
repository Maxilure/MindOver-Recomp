#include "discord_presence.h"

#ifdef _WIN32
#include "guest_memory.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace discord_presence {
namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// GAMESTATE (Xbox context 1) from the disc's XDBF. sub_8227B3B8 sends
// game+0x178 as the context value during gameplay (game+8 == 5).
constexpr std::array<std::string_view, 15> kAreas = {
    "In Game", "Main Menu", "Starting Game", "Title Screen", "Watching Movie",
    "Wumpa Island", "Ratcicle Kingdom", "Frozen Coast", "Wasteland",
    "Evil Public School", "Junkyard", "Mount Grimly", "Space Head",
    "Wumpa Island", "Crash's House",
};
constexpr std::array<std::string_view, 3> kDifficulties = {"Mild", "Tricky", "Bonkers"};
constexpr std::string_view kApplicationId = "1569078085200904435";
constexpr uint32_t kGameGlobal = 0x8259B190;
constexpr uint32_t kSettings = 0x825B0008;

struct Snapshot {
  unsigned context = 2;
  unsigned difficulty = kDifficulties.size();
  int seconds = -1;
};

std::atomic<bool> enabled{false};
std::mutex snapshot_mutex;
Snapshot latest;
std::jthread worker;

uint32_t Read32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, GuestPtr(base, address), sizeof(value));
  return std::byteswap(value);
}

}  // namespace

void Sample() {
  if (!enabled.load(std::memory_order_relaxed)) return;

  // Only this game-thread hook touches guest memory. The IPC worker sees
  // host snapshots and can wait on the Discord client without stalling a frame.
  static auto next_sample = Clock::time_point{};
  const auto now = Clock::now();
  if (now < next_sample) return;
  next_sample = now + 250ms;

  const auto* base = rex::system::kernel_memory()->virtual_membase();
  const uint32_t game = Read32(base, kGameGlobal);
  if (!game) return;

  Snapshot current;
  switch (Read32(base, game + 8)) {
    case 1:
      current.context = 4;
      break;
    case 4:
    case 6:
      current.context = 1;
      break;
    case 5: {
      const unsigned area = *GuestPtr(base, game + 0x178);
      current.context = area >= 5 && area < kAreas.size() ? area : 0;
      // The live progress and settings fields used by the save screen.
      current.difficulty = Read32(base, kSettings + 240);
      if (const uint32_t progress = Read32(base, game + 96)) {
        const float seconds = std::bit_cast<float>(Read32(base, progress + 20));
        if (seconds >= 0 && seconds < 1'000'000'000) {
          current.seconds = static_cast<int>(seconds);
        }
      }
      break;
    }
  }

  std::lock_guard lock(snapshot_mutex);
  latest = current;
}

namespace {
std::string Activity(int64_t session_start) {
  Snapshot current;
  {
    std::lock_guard lock(snapshot_mutex);
    current = latest;
  }
  std::string state;
  if (current.difficulty < kDifficulties.size()) {
    state = std::format("Difficulty: {}", kDifficulties[current.difficulty]);
  }
  if (current.seconds >= 0) {
    if (!state.empty()) state += " | ";
    const int seconds = current.seconds;
    state += std::format("Played {:02}:{:02}:{:02}", seconds / 3600,
                         seconds / 60 % 60, seconds % 60);
  }

  // The Discord client uses the registered application's icon.
  auto activity = std::format(R"({{"details":"{}")", kAreas[current.context]);
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
  enabled.store(true, std::memory_order_relaxed);
  worker = std::jthread(Run);
}

void Stop() {
  enabled.store(false, std::memory_order_relaxed);
  if (worker.joinable()) {
    worker.request_stop();
    worker.join();
  }
}
}  // namespace discord_presence
#else
namespace discord_presence {
void Start() {}
void Sample() {}
void Stop() {}
}  // namespace discord_presence
#endif
