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
#include "fixed_step.h"
#include "guest_memory.h"
#include "players/more_players.h"

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
  // IN GAME FRAMES (the fixed step was on when the replay started,
  // fixed_step.h): an entry recorded at ms m takes effect at "tick"
  // ceil((m - from_ms) / 33.3 ms), a 30 Hz grid. Game frame f (counted from
  // the replay's first pass) sees the entries with tick x fps <= f x 30. 30,
  // 60, 90, 180 fps are all multiples of 30, so every run gets each change at
  // exactly the same GAME time, whatever its frame rate. (A press shorter
  // than a tick moves to the grid with its release; fine for movement.)
  int32_t fps = 0;                       // 0 = real-time replay
  int64_t base_frame = -1;               // fixed_step::Frame() at the first read
  bool aligned = true;                   // inputs as a 30 fps frame acts on them (below)
  // START ON A WORLD FRAME: a replay started by a console command begins on
  // whatever frame the command lands, a frame or two apart between runs;
  // the world (animations, the spawn sequence) is then at another phase
  // when the first press comes. At 180 fps that parted two runs of the
  // same replay at the first press (2026-10-09: Crash's body popped into
  // place one frame apart). on_level = start on the frame the level turns
  // playable instead, with the fixed step on from launch (--fixed_step).
  bool on_level = false;
  // on_spawn: better still. "Playable" comes a fixed 166 frames after the
  // loading screen goes, but the loading screen goes whenever the loading
  // THREAD is done: 10 or 11 frames after player 1's Crash was created
  // (measured, 180 fps, two runs of the same save). His animations run from
  // his creation, so replays anchored on "playable" met him one frame apart
  // in his idle animation, and his first step came a frame later in one run.
  // on_spawn starts kSpawnLead seconds of game time after the frame his
  // Crash first exists (after "playable" in every run seen), so the replay
  // and Crash share the same clock.
  bool on_spawn = false;
  int64_t spawn_frame = -1;              // fixed-step frame Crash first existed
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

// How long after player 1's Crash is created an on_spawn replay starts:
// playable came 176-177 frames (0.98 s) after it at 180 fps; 1.5 s leaves room.
constexpr double kSpawnLead = 1.5;

// The level is PLAYABLE (the debug console's `wait level`): front end in
// GameRunning (489) and its fade-in timer (front end = uber +52, timer +144,
// 1 -> 0) done.
bool LevelPlayable(uint8_t* base) {
  if (!more_players::InPlay() || fight_tree::CurrentFrontEnd().state != 489) return false;
  auto load32 = [base](uint32_t address) {
    uint32_t v;
    std::memcpy(&v, GuestPtr(base, address), 4);
    return __builtin_bswap32(v);
  };
  const uint32_t uber = load32(0x8259B190);
  if (!uber) return false;
  const uint32_t front_end = load32(uber + 52);
  if (!front_end) return false;
  const uint32_t fade = load32(front_end + 144);
  float f;
  std::memcpy(&f, &fade, 4);
  return f <= 0.0f;
}

// The 30 Hz grid tick a recording time takes effect at (game-frame replays):
// ceil((ms - from) x 30 / 1000); everything at or before `from` = tick 0.
int64_t TickOf(int64_t ms, int64_t from) {
  const int64_t d = ms - from;
  return d <= 0 ? 0 : (d * 30 + 999) / 1000;
}

// The guest memory base (from the pad reads), for OnPass.
std::atomic<uint8_t*> g_base{nullptr};

// The game polls the pads from sub_822744F0: when its millisecond clock is
// more than 16 ms past the last poll (object [uber+56], last poll ms at
// +76). At 30 fps that's every frame, at 180 every 3-4 frames, and the
// rhythm runs on from boot, through a loading screen of a varying number of
// frames: two runs met player 1's Crash at different steps of it and read
// the same stick change a frame apart (2026-10-09). A replay that starts on
// an exact frame resets it there: "last poll" 17 ms ago = a poll this frame.
constexpr uint32_t kUber = 0x8259B190, kUberInput = 56, kInputLastPollMs = 76;
void RestartPadPollRhythm() {
  uint8_t* base = g_base.load();
  const uint32_t now_ms = fixed_step::ClockMs();
  if (!base || !now_ms) return;
  auto load32 = [base](uint32_t a) {
    uint32_t v;
    std::memcpy(&v, GuestPtr(base, a), 4);
    return __builtin_bswap32(v);
  };
  const uint32_t uber = load32(kUber);
  const uint32_t input = uber ? load32(uber + kUberInput) : 0;
  if (!input) return;
  const uint32_t last = __builtin_bswap32(now_ms - 17);
  std::memcpy(GuestPtr(base, input + kInputLastPollMs), &last, 4);
}

// Every main-loop pass while the fixed step is on (fixed_step::SetPassListener):
// an on_spawn replay notes the frame player 1's Crash first exists. Checked
// every frame: the game doesn't read the pads every frame.
void OnPass() {
  if (!g_replay_running.load(std::memory_order_relaxed)) return;
  std::lock_guard lock(g_replay_mutex);
  Replay& r = g_replay;
  if (!r.running || !r.on_spawn) return;
  if (r.spawn_frame >= 0) {
    // The replay's first frame: the pads' poll rhythm starts here too.
    if (int64_t(fixed_step::Frame()) == r.spawn_frame + int64_t(kSpawnLead * r.fps)) {
      RestartPadPollRhythm();
    }
    return;
  }
  if (!more_players::InPlay() || !more_players::CharacterOfPlayer(0)) return;
  r.spawn_frame = int64_t(fixed_step::Frame());
  REXLOG_INFO("Input replay: player 1's Crash exists at fixed-step frame {}", r.spawn_frame);
}

}  // namespace

void BeforeFilters(uint32_t player, uint8_t* gamepad, uint32_t& result, uint8_t* base) {
  g_base.store(base, std::memory_order_relaxed);
  if (player >= kPlayers || !g_replay_running.load(std::memory_order_relaxed)) return;
  std::lock_guard lock(g_replay_mutex);
  Replay& r = g_replay;
  if (!r.running) return;
  if (r.fps) {
    // Game-frame replay. Turned off / changed fixed step = the clock this
    // replay counts in is gone: stop rather than replay at a wrong pace.
    if (fixed_step::Fps() != r.fps) {
      r.running = false;
      g_replay_running = false;
      REXLOG_WARN("Input replay: stopped (the fixed step changed under it)");
      return;
    }
    if (r.base_frame < 0) {
      if (r.on_spawn) {
        if (r.spawn_frame < 0) return;  // OnPass sets it, on the very frame
        // An exact frame number, not "the first controller read from then
        // on": the game doesn't read the pads every frame (two runs started
        // 2 frames apart that way, 2026-10-09).
        const int64_t start = r.spawn_frame + int64_t(kSpawnLead * r.fps);
        if (int64_t(fixed_step::Frame()) < start) return;
        r.base_frame = start;
      } else {
        if (r.on_level && !LevelPlayable(base)) return;
        r.base_frame = int64_t(fixed_step::Frame());
      }
      REXLOG_INFO("Input replay: started at fixed-step frame {}{}", r.base_frame,
                  r.on_spawn   ? fmt::format(" ({} s after player 1's Crash was created)", kSpawnLead)
                  : r.on_level ? std::string(" (the level's first playable frame)")
                               : std::string());
    }
    const int64_t frame = int64_t(fixed_step::Frame()) - r.base_frame;
    // The newest grid tick this frame has reached (tick x fps <= frame x 30).
    // ALIGNED (default): the game acts on a controller state in the frame
    // AFTER the one that read it, so a 30 fps run moves 1/30 s after a
    // change, a 180 fps run 1/180 s after: every turn of a long replay came
    // ~28 ms earlier at 180 and the paths drifted apart (several units in
    // 20 s), hiding the physics differences we compare for. Measured on a
    // scripted walk, 2026-10-09: the 180 run's first movement ended 1 grid
    // tick earlier. So a run above 30 gets each change (fps/30 - 1) frames
    // late: the frame that ACTS on it starts at the same game time as the
    // 30 fps run's. `replay ... raw` = no delay (the real high-fps feel).
    const int64_t delay = r.aligned && r.fps > 30 ? r.fps / 30 - 1 : 0;
    const int64_t tick = frame < delay ? -1 : (frame - delay) * 30 / r.fps;
    if (tick < 0) return;  // the first changes aren't due yet: leave the pads alone
    if (TickOf(r.to_ms, r.from_ms) < tick) {
      r.running = false;
      g_replay_running = false;
      REXLOG_INFO("Input replay: finished at game frame {} ({} ms .. {} ms of {})", frame,
                  r.from_ms, r.to_ms, r.path);
      return;
    }
    const auto& entries = r.entries[player];
    if (entries.empty()) return;
    size_t& c = r.cursor[player];
    while (c + 1 < entries.size() && TickOf(entries[c + 1].ms, r.from_ms) <= tick) ++c;
    if (TickOf(entries[c].ms, r.from_ms) > tick) return;
    const Pad& pad = entries[c].pad;
    if (pad.connected) {
      std::memcpy(gamepad, pad.bytes.data(), 12);
      result = 0;
    } else {
      std::memset(gamepad, 0, 12);
      result = kNotConnected;
    }
    return;
  }
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

std::string StartReplay(const std::string& path, int64_t from_ms, int64_t to_ms, bool raw,
                        bool on_level, bool on_spawn) {
  Replay r;
  r.aligned = !raw;
  r.on_level = on_level;
  r.on_spawn = on_spawn;
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
  r.fps = fixed_step::Fps();
  fixed_step::SetPassListener(&OnPass);
  const std::string reply = fmt::format(
      "replaying {} ms .. {} ms ({:.1f} s) of {}, {} player(s){}\n", r.from_ms, r.to_ms,
      (r.to_ms - r.from_ms) / 1000.0, path, players,
      r.fps ? fmt::format(", in GAME FRAMES (fixed step 1/{} s, 30 Hz input grid{})", r.fps,
                          r.fps > 30 ? (r.aligned ? ", aligned to 30 fps reactions"
                                                  : ", raw: reacting sooner than 30 fps")
                                     : "")
            : std::string(", in real time"));
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
  if (r.fps) {
    const int64_t frame = r.base_frame < 0 ? 0 : int64_t(fixed_step::Frame()) - r.base_frame;
    return fmt::format("replaying {} in game frames: frame {} = {} ms of the recording (to {} ms)\n",
                       r.path, frame, r.from_ms + frame * 1000 / r.fps, r.to_ms);
  }
  return fmt::format("replaying {} at {} ms (to {} ms)\n", r.path,
                     r.from_ms + (NowMs() - r.started_ms), r.to_ms);
}

bool ReplayRunning() { return g_replay_running.load(); }

int64_t ReplayFrame() {
  if (!g_replay_running.load(std::memory_order_relaxed)) return -1;
  std::lock_guard lock(g_replay_mutex);
  const Replay& r = g_replay;
  if (!r.running || !r.fps || r.base_frame < 0) return -1;
  return int64_t(fixed_step::Frame()) - r.base_frame;
}

}  // namespace input_record
