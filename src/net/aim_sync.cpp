// =============================================================================
// net/aim_sync.cpp -- online mask aiming in world space (see the .h).
// =============================================================================
#include "aim_sync.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../players/reticle_idle.h"
#include "action_sync.h"  // online_action_sync: the same switch
#include "state_sync.h"

extern "C" REX_FUNC(__imp__sub_8224C6C0);  // screen-spot ray (original)
extern "C" REX_FUNC(__imp__sub_8224C7F8);  // segment cast (original)

namespace net::aim_sync {
namespace {

using Clock = std::chrono::steady_clock;
constexpr int kPlayers = 4;
constexpr uint32_t kAimCallReturn = 0x822AC988;   // sub_822AC668's call of sub_8224C6C0 (0x822AC984)
constexpr auto kFresh = std::chrono::milliseconds(300);
constexpr uint32_t kQueryLength = 0x20;           // query +0x20: fraction used when nothing is hit
constexpr uint32_t kResultSpot = 0x10;            // result +16: the hit spot (x, y, z)

uint8_t* Guest(uint32_t a) { return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(a); }
float ReadF(uint32_t a) {
  const uint8_t* p = Guest(a);
  const uint32_t v = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void WriteF(uint32_t a, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  uint8_t* p = Guest(a);
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
void PutF(uint8_t* p, float f) { std::memcpy(p, &f, 4); }  // little-endian host = LE wire
float GetF(const uint8_t* p) { float f; std::memcpy(&f, p, 4); return f; }

struct Segment {
  bool valid = false;
  float v[6] = {};  // start xyz, end xyz
  Clock::time_point when;
};

std::mutex g_mutex;
std::array<Segment, kPlayers> g_remote{};
std::vector<std::vector<uint8_t>> g_outgoing;
bool g_capturing = false;   // our player's aim cast is running: the segment cast records it
Segment g_captured;

bool Finite6(const float* v) {
  for (int i = 0; i < 6; ++i) if (!std::isfinite(v[i]) || std::fabs(v[i]) > 1.0e6f) return false;
  return true;
}

}  // namespace

bool HandleAimRay(PPCContext& ctx, uint8_t* base) {
  if (uint32_t(ctx.lr) != kAimCallReturn || !net::Active() || !REXCVAR_GET(online_action_sync)) return false;
  const int p = reticle_idle::AimingPlayer();
  const int local = net::LocalSlot();
  if (p < 0 || p >= kPlayers || local < 0) return false;

  if (p == local) {
    // Our aim: the original, with its segment captured on the way.
    g_capturing = true;
    g_captured.valid = false;
    const uint32_t result = ctx.r6.u32;
    __imp__sub_8224C6C0(ctx, base);
    g_capturing = false;
    static uint32_t n = 0;
    if (net::DebugTraceOn() && n++ % 10 == 0) {
      REXLOG_INFO("OnlineTrace: P{} aim (own) at ({:.2f}, {:.2f}, {:.2f}) t {:.4f} hit+12 {:08X} seg ({:.2f} {:.2f} {:.2f})->({:.2f} {:.2f} {:.2f})",
                  p + 1, ReadF(result + kResultSpot), ReadF(result + kResultSpot + 4), ReadF(result + kResultSpot + 8),
                  ReadF(result), *reinterpret_cast<uint32_t*>(Guest(result + 12)), g_captured.v[0], g_captured.v[1],
                  g_captured.v[2], g_captured.v[3], g_captured.v[4], g_captured.v[5]);
    }
    if (g_captured.valid && Finite6(g_captured.v)) {
      std::vector<uint8_t> msg(kAimSize, 0);
      msg[0] = kMsgAim;
      msg[1] = uint8_t(local);
      for (int i = 0; i < 6; ++i) PutF(msg.data() + 2 + 4 * i, g_captured.v[i]);
      std::lock_guard lock(g_mutex);
      if (g_outgoing.size() < 8) g_outgoing.push_back(std::move(msg));  // never pile up
      static uint32_t sent = 0;
      if (net::DebugTraceOn() && sent++ % 60 == 0) REXLOG_INFO("OnlineTrace: P{} aim segment queued (x{})", p + 1, sent);
    }
    return true;
  }

  // A remote player's aim: their owner's world segment, if fresh.
  Segment seg;
  {
    std::lock_guard lock(g_mutex);
    seg = g_remote[p];
  }
  if (!seg.valid || Clock::now() - seg.when > kFresh) {
    static uint32_t miss = 0;
    if (net::DebugTraceOn() && miss++ % 60 == 0) {
      REXLOG_INFO("OnlineTrace: P{} aim: no fresh owner segment (valid {}, age {} ms, x{})", p + 1, seg.valid,
                  seg.valid ? std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - seg.when).count() : -1,
                  miss);
    }
    return false;
  }
  static uint32_t room = 0;  // start (16 B) + end (16 B), guest memory, made once
  if (!room) room = rex::system::kernel_memory()->SystemHeapAlloc(32);
  if (!room) return false;
  for (int i = 0; i < 3; ++i) {
    WriteF(room + 4 * i, seg.v[i]);
    WriteF(room + 16 + 4 * i, seg.v[3 + i]);
  }
  const uint32_t world = ctx.r3.u32, query = ctx.r5.u32, result = ctx.r6.u32;
  const PPCContext saved = ctx;
  ctx.r3.u64 = world;
  ctx.r4.u64 = room;
  ctx.r5.u64 = room + 16;
  ctx.r6.u64 = query;
  ctx.r7.u64 = result;
  __imp__sub_8224C7F8(ctx, base);
  const bool hit = (ctx.r3.u32 & 0xFF) != 0;
  ctx = saved;
  // The hit spot as the original computes it (sub_8224C6C0's tail): start +
  // DIRECTION x t, the direction normalised (sub_82424880), t = the hit's
  // DISTANCE (result +0), or the query's length (+0x20) if nothing was hit.
  // (Checked against an owner's own aim: start + 9.7967 x unit dir = its spot.)
  const float t = hit ? ReadF(result) : ReadF(query + kQueryLength);
  float d[3] = {seg.v[3] - seg.v[0], seg.v[4] - seg.v[1], seg.v[5] - seg.v[2]};
  const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  if (len > 1e-6f) for (float& c : d) c /= len;
  for (int i = 0; i < 3; ++i) WriteF(result + kResultSpot + 4 * i, seg.v[i] + d[i] * t);
  ctx.r3.u64 = hit ? 1 : 0;
  static uint32_t n = 0;
  if (net::DebugTraceOn() && n++ % 10 == 0) {
    REXLOG_INFO("OnlineTrace: P{} aim (owner's segment) at ({:.2f}, {:.2f}, {:.2f}) hit {} t {:.4f} seg ({:.2f} {:.2f} {:.2f})->({:.2f} {:.2f} {:.2f})",
                p + 1, ReadF(result + kResultSpot), ReadF(result + kResultSpot + 4), ReadF(result + kResultSpot + 8),
                hit, t, seg.v[0], seg.v[1], seg.v[2], seg.v[3], seg.v[4], seg.v[5]);
  }
  return true;
}

bool RemoteAimFresh(int p) {
  if (p < 0 || p >= kPlayers) return false;
  std::lock_guard lock(g_mutex);
  return g_remote[p].valid && Clock::now() - g_remote[p].when <= kFresh;
}

std::vector<std::vector<uint8_t>> TakeOutgoing() {
  std::lock_guard lock(g_mutex);
  std::vector<std::vector<uint8_t>> out;
  out.swap(g_outgoing);
  return out;
}

void OnReceive(const uint8_t* data, size_t size) {
  if (size < kAimSize || data[1] >= kPlayers) return;
  Segment s;
  for (int i = 0; i < 6; ++i) s.v[i] = GetF(data + 2 + 4 * i);
  if (!Finite6(s.v)) return;
  s.valid = true;
  s.when = Clock::now();
  std::lock_guard lock(g_mutex);
  g_remote[data[1]] = s;
  static uint32_t got = 0;
  if (net::DebugTraceOn() && got++ % 60 == 0) REXLOG_INFO("OnlineTrace: P{} aim segment received (x{})", data[1] + 1, got);
}

void Reset() {
  std::lock_guard lock(g_mutex);
  g_remote = {};
  g_outgoing.clear();
}

}  // namespace net::aim_sync

// The segment cast (r3 world, r4 start, r5 end, r6 query, r7 result): while
// our player's aim cast runs (HandleAimRay above), its start and end are kept.
extern "C" REX_FUNC(sub_8224C7F8) {
  using namespace net::aim_sync;
  if (g_capturing) {
    for (int i = 0; i < 3; ++i) {
      g_captured.v[i] = ReadF(ctx.r4.u32 + 4 * i);
      g_captured.v[3 + i] = ReadF(ctx.r5.u32 + 4 * i);
    }
    g_captured.valid = true;
  }
  __imp__sub_8224C7F8(ctx, base);
}
