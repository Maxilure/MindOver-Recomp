// =============================================================================
// input_record.cpp -- the recording file and the replay (input_record.h)
// =============================================================================

#include "input_record.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <fmt/format.h>
#include <rex/logging.h>

#include "data/fight_tree.h"

REXCVAR_DEFINE_BOOL(debug_input_record, true, "CrashMoM",
                    "Record every controller state the game reads into <log name>-inputs.txt "
                    "next to the session log (to replay a playtest's presses: debug console)");

namespace input_record {
namespace {

using Clock = std::chrono::steady_clock;
const Clock::time_point g_start = Clock::now();

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_start).count();
}

constexpr int kPlayers = 4;
constexpr uint32_t kNotConnected = 0x48F;  // ERROR_DEVICE_NOT_CONNECTED

// One controller state: the 12 gamepad bytes (big-endian, as the game reads
// them) or "off".
struct Pad {
  bool connected = false;
  std::array<uint8_t, 12> bytes{};
  bool operator==(const Pad&) const = default;
};

// --- recording (game thread only) ---------------------------------------------

std::FILE* g_file = nullptr;
bool g_tried_open = false;
Pad g_last[kPlayers];
bool g_have_last[kPlayers] = {};
uint32_t g_fe_exits = 0;
int64_t g_last_flush_ms = 0;

uint16_t U16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

void Open() {
  g_tried_open = true;
  const std::string log = rex::cvar::GetFlagByName("log_file");
  if (log.empty()) return;  // no session log: nowhere to put it
  std::filesystem::path path(log);
  path.replace_filename(path.stem().string() + "-inputs.txt");
  g_file = std::fopen(path.string().c_str(), "w");
  if (!g_file) {
    REXLOG_WARN("Input record: can't write {}", path.string());
    return;
  }
  std::fprintf(g_file,
               "# Mind over Recomp input recording (input_record.h): what the game read per player\n"
               "# <ms since launch> <player> <buttons> <LT> <RT> <LX> <LY> <RX> <RY> | <ms> <player> off"
               " | <ms> fe <front end state>\n");
  REXLOG_INFO("Input record: {}", path.string());
}

void Record(uint32_t player, const Pad& pad) {
  if (!g_tried_open) Open();
  if (!g_file) return;
  const int64_t now = NowMs();
  // The front end's position first (an exit since the last line).
  const auto fe = fight_tree::CurrentFrontEnd();
  if (fe.exits != g_fe_exits) {
    g_fe_exits = fe.exits;
    std::fprintf(g_file, "%lld fe %d\n", (long long)now, fe.state);
  }
  if (g_have_last[player] && g_last[player] == pad) return;
  g_have_last[player] = true;
  g_last[player] = pad;
  if (!pad.connected) {
    std::fprintf(g_file, "%lld %u off\n", (long long)now, player + 1);
  } else {
    const uint8_t* b = pad.bytes.data();
    std::fprintf(g_file, "%lld %u %04X %u %u %d %d %d %d\n", (long long)now, player + 1, U16(b),
                 b[2], b[3], int16_t(U16(b + 4)), int16_t(U16(b + 6)), int16_t(U16(b + 8)),
                 int16_t(U16(b + 10)));
  }
  if (now - g_last_flush_ms > 1000) {  // a crash loses at most a second
    std::fflush(g_file);
    g_last_flush_ms = now;
  }
}

// --- replay (console thread starts/stops, game thread plays) -----------------

struct Entry {
  int64_t ms;
  Pad pad;
};

std::mutex g_replay_mutex;
struct Replay {
  bool running = false;
  std::string path;
  std::vector<Entry> entries[kPlayers];  // per player, in time order
  size_t cursor[kPlayers] = {};          // the entry in force
  int64_t from_ms = 0, to_ms = 0;        // recording times
  int64_t started_ms = 0;                // our time at from_ms
} g_replay;
std::atomic<bool> g_replay_running{false};

void SetBE16(uint8_t* p, int v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}

void Load(const std::string& path, std::vector<Entry> (&out)[kPlayers]) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("can't read " + path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream words(line);
    long long ms;
    std::string who;
    if (!(words >> ms >> who) || who == "fe") continue;
    const int player = std::atoi(who.c_str()) - 1;
    if (player < 0 || player >= kPlayers) continue;
    Entry e{ms, {}};
    std::string buttons;
    words >> buttons;
    if (buttons != "off") {
      int lt, rt, lx, ly, rx, ry;
      if (!(words >> lt >> rt >> lx >> ly >> rx >> ry)) continue;
      e.pad.connected = true;
      uint8_t* b = e.pad.bytes.data();
      SetBE16(b, int(std::stoul(buttons, nullptr, 16)));
      b[2] = uint8_t(lt);
      b[3] = uint8_t(rt);
      SetBE16(b + 4, lx);
      SetBE16(b + 6, ly);
      SetBE16(b + 8, rx);
      SetBE16(b + 10, ry);
    }
    out[player].push_back(e);
  }
}

}  // namespace

void BeforeFilters(uint32_t player, uint8_t* gamepad, uint32_t& result) {
  if (player >= kPlayers || !g_replay_running.load(std::memory_order_relaxed)) return;
  std::lock_guard lock(g_replay_mutex);
  Replay& r = g_replay;
  if (!r.running) return;
  const int64_t t = r.from_ms + (NowMs() - r.started_ms);
  if (t > r.to_ms) {
    r.running = false;
    g_replay_running = false;
    REXLOG_INFO("Input replay: finished ({} ms .. {} ms of {})", r.from_ms, r.to_ms, r.path);
    return;
  }
  const auto& entries = r.entries[player];
  if (entries.empty()) return;  // the recording doesn't speak for this player
  size_t& c = r.cursor[player];
  while (c + 1 < entries.size() && entries[c + 1].ms <= t) ++c;
  if (entries[c].ms > t) {
    // Before this player's first line in range: leave them alone.
    return;
  }
  const Pad& pad = entries[c].pad;
  if (pad.connected) {
    std::memcpy(gamepad, pad.bytes.data(), 12);
    result = 0;
  } else {
    std::memset(gamepad, 0, 12);
    result = kNotConnected;
  }
}

void AfterFilters(uint32_t player, const uint8_t* gamepad, uint32_t result) {
  if (player >= kPlayers || !REXCVAR_GET(debug_input_record)) return;
  Pad pad;
  pad.connected = result == 0;
  if (pad.connected) std::memcpy(pad.bytes.data(), gamepad, 12);
  Record(player, pad);
}

std::string StartReplay(const std::string& path, int64_t from_ms, int64_t to_ms) {
  Replay r;
  Load(path, r.entries);
  int64_t first = INT64_MAX, last = INT64_MIN;
  int players = 0;
  for (const auto& list : r.entries) {
    if (list.empty()) continue;
    ++players;
    first = std::min(first, list.front().ms);
    last = std::max(last, list.back().ms);
  }
  if (!players) throw std::runtime_error("no controller lines in " + path);
  r.from_ms = from_ms < 0 ? first : from_ms;
  r.to_ms = to_ms < 0 ? last + 500 : to_ms;
  if (r.to_ms <= r.from_ms) throw std::runtime_error("to_ms must be after from_ms");
  // Each player's cursor: the last line at or before from_ms (the state in
  // force when the part starts), else their first line.
  for (int p = 0; p < kPlayers; ++p) {
    const auto& list = r.entries[p];
    size_t c = 0;
    while (c + 1 < list.size() && list[c + 1].ms <= r.from_ms) ++c;
    r.cursor[p] = c;
  }
  r.path = path;
  r.running = true;
  r.started_ms = NowMs();
  const std::string reply = fmt::format("replaying {} ms .. {} ms ({:.1f} s) of {}, {} player(s)\n",
                                        r.from_ms, r.to_ms, (r.to_ms - r.from_ms) / 1000.0, path,
                                        players);
  {
    std::lock_guard lock(g_replay_mutex);
    g_replay = std::move(r);
    g_replay_running = true;
  }
  REXLOG_INFO("Input replay: {}", reply.substr(0, reply.size() - 1));
  return reply;
}

std::string StopReplay() {
  std::lock_guard lock(g_replay_mutex);
  const bool was = g_replay.running;
  g_replay.running = false;
  g_replay_running = false;
  if (was) REXLOG_INFO("Input replay: stopped");
  return was ? "stopped\n" : "no replay running\n";
}

std::string ReplayStatus() {
  std::lock_guard lock(g_replay_mutex);
  const Replay& r = g_replay;
  if (!r.running) return "no replay running\n";
  return fmt::format("replaying {} at {} ms (to {} ms)\n", r.path,
                     r.from_ms + (NowMs() - r.started_ms), r.to_ms);
}

bool ReplayRunning() { return g_replay_running.load(); }

}  // namespace input_record
