// =============================================================================
// world_hash.cpp -- the game world's fingerprint per frame (world_hash.h)
// =============================================================================
#include "debug/world_hash.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>

#include <fmt/format.h>
#include <rex/memory.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "fixed_step.h"
#include "guest_memory.h"
#include "input_record.h"
#include "pddi/intercept.h"
#include "players/more_players.h"

namespace world_hash {
namespace {

// The writable part of the executable image (after the code, which ends at
// 0x824D0000): the game's globals. Image = 0x82000000 + 0x650000.
constexpr uint32_t kDataStart = 0x824D0000;
constexpr uint32_t kDataEnd = 0x82650000;
constexpr uint32_t kPageSize = 0x1000;
constexpr uint32_t kPages = (kDataEnd - kDataStart) / kPageSize;  // 384

// The random generator's state: two 32-bit words (findings/66, world_hash.h).
constexpr uint32_t kRandomState = 0x825A14F4;

// Physics block of a body (debug_console.cpp PhysicsOf: actor +308 -> a
// CPhysicsBehaviour whose +88 points back; +104 velocity, +140 position).
constexpr uint32_t kActorPhysics = 308, kPhysicsVtable = 0x82032FC4, kPhysicsActor = 88;
constexpr uint32_t kPhysicsVelocity = 104, kPhysicsPosition = 140;

uint8_t* Membase() { return rex::system::kernel_memory()->virtual_membase(); }

uint32_t Load32(uint32_t address) {
  const uint8_t* p = GuestPtr(Membase(), address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// A fast 64-bit mix over 8-byte words (not cryptographic: it only has to make
// two different pages very unlikely to give the same number).
uint64_t HashBytes(const uint8_t* data, size_t size, uint64_t h = 0x9E3779B97F4A7C15ull) {
  size_t i = 0;
  for (; i + 8 <= size; i += 8) {
    uint64_t w;
    std::memcpy(&w, data + i, 8);
    h ^= w * 0xC2B2AE3D27D4EB4Full;
    h = (h << 31 | h >> 33) * 0x9E3779B185EBCA87ull;
  }
  for (; i < size; ++i) h = (h ^ data[i]) * 0x100000001B3ull;
  h ^= h >> 29;
  h *= 0xBF58476D1CE4E5B9ull;
  return h ^ (h >> 32);
}

struct Recorder {
  std::FILE* file = nullptr;
  std::string path;
  int64_t max_frames = 0;    // 0 = until `worldhash stop`
  int64_t frames = 0;
  uint64_t count = 0;        // frame column without a fixed step
  int64_t dump_frame = -1;   // write the data section at this frame (frame column)
  bool dumped = false;
  std::mutex mutex;
  uint64_t pages[kPages];
};
std::mutex g_mutex;  // guards g_recorder (the pointer)
std::unique_ptr<Recorder> g_recorder;

// Players: the body (titan or Crash), its physics position + velocity, as the
// raw big-endian bytes (any difference in any bit counts).
uint64_t PlayersHash(uint32_t& present) {
  uint64_t h = 0x84222325CBF29CE4ull;
  present = 0;
  for (int p = 0; p < 4; ++p) {
    const uint32_t titan = more_players::TitanOfPlayer(p);
    const uint32_t body = titan ? titan : more_players::CharacterOfPlayer(p);
    if (!body || !guest_memory::Readable(body + kActorPhysics, 4)) continue;
    present |= 1u << p;
    uint8_t buffer[4 + 12 + 12] = {};
    std::memcpy(buffer, &body, 4);
    const uint32_t physics = Load32(body + kActorPhysics);
    if (physics && guest_memory::Readable(physics, kPhysicsPosition + 12) &&
        Load32(physics) == kPhysicsVtable && Load32(physics + kPhysicsActor) == body) {
      std::memcpy(buffer + 4, GuestPtr(Membase(), physics + kPhysicsPosition), 12);
      std::memcpy(buffer + 16, GuestPtr(Membase(), physics + kPhysicsVelocity), 12);
    }
    h = HashBytes(buffer, sizeof(buffer), h);
  }
  return h;
}

void OnFrameEnd(void* user) {
  Recorder& r = *static_cast<Recorder*>(user);
  std::lock_guard lock(r.mutex);
  if (!r.file || (r.max_frames && r.frames >= r.max_frames)) return;
  if (!more_players::InPlay()) return;  // menus / loading: no world to compare
  ++r.count;
  ++r.frames;
  const int64_t frame =
      fixed_step::Fps() ? int64_t(fixed_step::Frame()) : int64_t(r.count);
  const int64_t replay_frame = input_record::ReplayFrame();

  const uint8_t* data = GuestPtr(Membase(), kDataStart);
  uint64_t all = 0x2545F4914F6CDD1Dull;
  for (uint32_t i = 0; i < kPages; ++i) {
    r.pages[i] = HashBytes(data + i * kPageSize, kPageSize);
    all = HashBytes(reinterpret_cast<const uint8_t*>(&r.pages[i]), 8, all);
  }
  uint32_t present = 0;
  const uint64_t players = PlayersHash(present);
  const uint32_t rng_x = Load32(kRandomState), rng_y = Load32(kRandomState + 4);
  const uint32_t zero = 0;

  std::fwrite(&frame, 8, 1, r.file);
  std::fwrite(&replay_frame, 8, 1, r.file);
  std::fwrite(&rng_x, 4, 1, r.file);
  std::fwrite(&rng_y, 4, 1, r.file);
  std::fwrite(&present, 4, 1, r.file);
  std::fwrite(&zero, 4, 1, r.file);
  std::fwrite(&players, 8, 1, r.file);
  std::fwrite(&all, 8, 1, r.file);
  std::fwrite(r.pages, 8, kPages, r.file);

  // The optional dump: the whole data section as the game stores it
  // (big-endian), for a word-by-word comparison of two runs.
  const int64_t dump_key = replay_frame >= 0 ? replay_frame : frame;
  if (!r.dumped && r.dump_frame >= 0 && dump_key >= r.dump_frame) {
    r.dumped = true;
    const std::string dump_path = fmt::format("{}.frame{}.bin", r.path, dump_key);
    if (std::FILE* d = std::fopen(dump_path.c_str(), "wb")) {
      std::fwrite(data, 1, kDataEnd - kDataStart, d);
      std::fclose(d);
    }
  }
}

std::string Stop() {
  std::unique_ptr<Recorder> recorder;
  {
    std::lock_guard lock(g_mutex);
    recorder = std::move(g_recorder);
  }
  if (!recorder) return "no world hash running\n";
  pddi::RemoveFrameEndListener(&OnFrameEnd, recorder.get());  // waits for a call in progress
  std::fclose(recorder->file);
  return fmt::format("world hash stopped: {} frames in {}\n", recorder->frames, recorder->path);
}

int64_t Number(std::string_view text, const char* what) {
  char* end = nullptr;
  const std::string s(text);
  const long long v = std::strtoll(s.c_str(), &end, 10);
  if (s.empty() || *end) throw std::runtime_error(fmt::format("{}: not a number", what));
  return v;
}

}  // namespace

std::string Command(const std::vector<std::string_view>& w) {
  if (w.size() == 1) {
    std::lock_guard lock(g_mutex);
    if (!g_recorder) return "no world hash running\n";
    std::lock_guard recorder_lock(g_recorder->mutex);
    return fmt::format("world hash into {}: {} frames\n", g_recorder->path, g_recorder->frames);
  }
  if (w[1] == "stop") return Stop();
  Stop();  // one at a time
  auto r = std::make_unique<Recorder>();
  r->path = std::string(w[1]);
  for (size_t i = 2; i < w.size(); ++i) {
    if (w[i] == "dump" && i + 1 < w.size()) {
      r->dump_frame = Number(w[++i], "dump frame");
    } else {
      r->max_frames = Number(w[i], "frames");
    }
  }
  r->file = std::fopen(r->path.c_str(), "wb");
  if (!r->file) throw std::runtime_error("can't write " + r->path);
  const uint32_t version = 1, pages = kPages, first = kDataStart, page_size = kPageSize;
  const uint64_t zero = 0;
  std::fwrite("MORWHASH", 1, 8, r->file);
  std::fwrite(&version, 4, 1, r->file);
  std::fwrite(&pages, 4, 1, r->file);
  std::fwrite(&first, 4, 1, r->file);
  std::fwrite(&page_size, 4, 1, r->file);
  std::fwrite(&zero, 8, 1, r->file);
  std::string reply = fmt::format(
      "world hash every frame into {}{}{}{}\n", r->path,
      fixed_step::Fps() ? fmt::format(" (fixed step 1/{} s)", fixed_step::Fps())
                        : std::string(" (REAL TIME: runs won't line up)"),
      r->max_frames ? fmt::format(", {} frames", r->max_frames) : std::string(),
      r->dump_frame >= 0 ? fmt::format(", data dump at frame {}", r->dump_frame) : std::string());
  pddi::AddFrameEndListener(&OnFrameEnd, r.get());
  std::lock_guard lock(g_mutex);
  g_recorder = std::move(r);
  return reply;
}

}  // namespace world_hash
