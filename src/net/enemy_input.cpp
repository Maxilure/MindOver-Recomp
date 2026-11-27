// =============================================================================
// net/enemy_input.cpp -- online: the host's enemy AI drives guests' enemies
// (what and why: enemy_input.h)
//
// Wire (kMsgEnemyInput, reliable, host -> guests):
//   +0 u8 type, +1 u8 count, then count x 7 B:
//     +0 u16 LE net_id (world_sync's enemy id)
//     +2 u8 code: 0xFF = an EVENT PRESS: +3 u8 map type, +4 u8 event, +5 2 B 0
//                 0-17 = a VALUE CHANNEL write: +3 f32 LE value
// =============================================================================
#include "enemy_input.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../guest_memory.h"
#include "../players/more_players.h"
#include "state_sync.h"
#include "world_sync.h"

extern "C" REX_FUNC(__imp__sub_82273328);  // add a sample to a value channel (r3 channel, f1 value, r5 time ms)
extern "C" REX_FUNC(__imp__sub_82273E48);  // press an input event (r3 input object, r4 map type, r5 event)
extern "C" REX_FUNC(sub_8235ABC8);         // the game clock, ms (r3)

namespace net {
void BroadcastFromHost(const uint8_t* data, size_t size, bool reliable);
bool DebugTraceOn();

namespace enemy_input {
namespace {

using Clock = std::chrono::steady_clock;
constexpr uint8_t kCodeEvent = 0xFF;

struct Entry {
  uint16_t net_id = 0;
  uint8_t code = 0;  // kCodeEvent or a channel 0-17
  uint8_t map = 0, event = 0;
  float value = 0.0f;
};

struct State {
  std::mutex mutex;
  std::vector<Entry> host_pending;  // host: this frame's presses and writes
  // Guest: what arrived, waiting for our "AI moment" (ApplyGuest).
  std::vector<Entry> guest_events;                                // presses: all at once
  std::unordered_map<uint32_t, std::deque<float>> guest_channels;  // writes: one per channel per frame
  // Guest: replayed presses the copy hasn't heard yet: (input, map, event)
  // -> when replayed. See TakeWaitingPress.
  std::unordered_map<uint64_t, Clock::time_point> waiting_presses;
  uint64_t host_events = 0, host_values = 0, presses_kept = 0;
  uint64_t guest_events_applied = 0, guest_values_applied = 0, guest_dropped_local = 0, guest_unbound = 0;
};
State g;

// Set while WE write into a puppet (the wrappers let it through).
thread_local bool t_applying = false;

constexpr size_t kEntryBytes = 7;
constexpr size_t kMaxPerPacket = 160;  // 2 + 160 x 7 = 1122 B (< ~1200)

uint32_t Key(uint32_t net_id, int channel) { return (net_id << 5) | uint32_t(channel); }
uint64_t PressKey(uint32_t input, uint32_t map, uint32_t event) {
  return uint64_t(input) << 32 | (map & 0xFF) << 8 | (event & 0xFF);
}
// A replayed press waits this long for the copy's next question about it.
constexpr auto kPressWait = std::chrono::milliseconds(400);
void PutLE16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
uint16_t GetLE16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
void PutF32(uint8_t* p, float v) { std::memcpy(p, &v, 4); }  // every PC is little-endian x86
float GetF32(const uint8_t* p) { float v; std::memcpy(&v, p, 4); return v; }

// The ROOT input object (parents at +0x24), as the game's own event code walks.
uint32_t RootOf(uint8_t* base, uint32_t input) {
  for (int guard = 0; input && guard < 16; ++guard) {
    if (!guest_memory::Readable(input + 0x24, 4)) break;
    const uint8_t* p = GuestPtr(base, input + 0x24);
    const uint32_t parent = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    if (!parent) break;
    input = parent;
  }
  return input;
}

}  // namespace

void Reset() {
  std::lock_guard lock(g.mutex);
  g.host_pending.clear();
  g.guest_events.clear();
  g.guest_channels.clear();
  g.host_events = g.host_values = g.presses_kept = 0;
  g.waiting_presses.clear();
  g.guest_events_applied = g.guest_values_applied = g.guest_dropped_local = g.guest_unbound = 0;
}

void FlushHost() {
  std::vector<Entry> pending;
  {
    std::lock_guard lock(g.mutex);
    if (g.host_pending.empty()) return;
    pending.swap(g.host_pending);
  }
  for (size_t at = 0; at < pending.size(); at += kMaxPerPacket) {
    const size_t n = std::min(kMaxPerPacket, pending.size() - at);
    std::vector<uint8_t> msg(2 + n * kEntryBytes, 0);
    msg[0] = kMsgEnemyInput;
    msg[1] = uint8_t(n);
    for (size_t i = 0; i < n; ++i) {
      const Entry& e = pending[at + i];
      uint8_t* w = msg.data() + 2 + i * kEntryBytes;
      PutLE16(w, e.net_id);
      w[2] = e.code;
      if (e.code == kCodeEvent) {
        w[3] = e.map;
        w[4] = e.event;
      } else {
        PutF32(w + 3, e.value);
      }
    }
    BroadcastFromHost(msg.data(), msg.size(), /*reliable=*/true);
  }
}

void HandlePacket(const uint8_t* data, size_t size) {
  if (size < 2 || data[0] != kMsgEnemyInput) return;
  const size_t n = data[1];
  if (size < 2 + n * kEntryBytes) return;
  std::lock_guard lock(g.mutex);
  for (size_t i = 0; i < n; ++i) {
    const uint8_t* r = data + 2 + i * kEntryBytes;
    Entry e;
    e.net_id = GetLE16(r);
    e.code = r[2];
    if (e.code == kCodeEvent) {
      e.map = r[3];
      e.event = r[4];
      g.guest_events.push_back(e);
      if (g.guest_events.size() > 512) g.guest_events.erase(g.guest_events.begin());
    } else if (e.code < 18) {
      const float v = GetF32(r + 3);
      if (!(v == v)) continue;  // NaN
      auto& q = g.guest_channels[Key(e.net_id, e.code)];
      q.push_back(v);
      while (q.size() > 32) q.pop_front();
    }
  }
}

void ApplyGuest(PPCContext& ctx, uint8_t* base) {
  if (!Active() || IsHost() || !world_sync::EnemyPuppets()) return;
  if (!more_players::InPlay() || more_players::Loading()) return;
  std::vector<Entry> events;
  std::vector<std::pair<uint32_t, float>> values;
  {
    std::lock_guard lock(g.mutex);
    events.swap(g.guest_events);
    for (auto it = g.guest_channels.begin(); it != g.guest_channels.end();) {
      if (it->second.empty()) {
        it = g.guest_channels.erase(it);
        continue;
      }
      values.emplace_back(it->first, it->second.front());
      it->second.pop_front();
      ++it;
    }
  }
  if (events.empty() && values.empty()) return;
  const PPCContext saved = ctx;
  uint64_t ev_done = 0, val_done = 0, unbound = 0;
  t_applying = true;
  for (const Entry& e : events) {
    const uint32_t input = world_sync::PuppetInputOf(e.net_id);
    if (!input) {
      ++unbound;
      continue;
    }
    ctx = saved;
    ctx.r3.u64 = input;
    ctx.r4.u64 = e.map;
    ctx.r5.u64 = e.event;
    __imp__sub_82273E48(ctx, base);
    ++ev_done;
    {
      std::lock_guard lock(g.mutex);
      g.waiting_presses[PressKey(input, e.map, e.event)] = Clock::now();
    }
    if (DebugTraceOn()) {
      REXLOG_INFO("OnlineTrace: enemy {} replayed map {} event {}", e.net_id, e.map, e.event);
    }
  }
  if (!values.empty()) {
    ctx = saved;
    sub_8235ABC8(ctx, base);
    const uint32_t now_ms = ctx.r3.u32;
    for (const auto& [key, value] : values) {
      const uint32_t channel = world_sync::PuppetChannelOf(key >> 5, int(key & 31));
      if (!channel) {
        ++unbound;
        continue;
      }
      ctx = saved;
      ctx.r3.u64 = channel;
      ctx.f1.f64 = double(value);
      ctx.r5.u64 = now_ms;
      __imp__sub_82273328(ctx, base);
      ++val_done;
    }
  }
  t_applying = false;
  ctx = saved;
  std::lock_guard lock(g.mutex);
  g.guest_events_applied += ev_done;
  g.guest_values_applied += val_done;
  g.guest_unbound += unbound;
}

std::string DebugStatus() {
  std::lock_guard lock(g.mutex);
  return fmt::format(
      "  enemy_input: puppets={} host: {} presses {} values | guest: {} presses {} values applied, "
      "{} local dropped, {} unbound, {} waiting, {} heard late\n",
      world_sync::EnemyPuppets(), g.host_events, g.host_values, g.guest_events_applied,
      g.guest_values_applied, g.guest_dropped_local, g.guest_unbound, g.guest_events.size(), g.presses_kept);
}

// Host: a write or press on a synced enemy's input object, noted.
void NoteHost(uint32_t net_id, const Entry& e_in) {
  if (!net_id || net_id > 0xFFFF) return;
  Entry e = e_in;
  e.net_id = uint16_t(net_id);
  if (DebugTraceOn() && e.code == kCodeEvent) {
    REXLOG_INFO("OnlineTrace: enemy {} pressed map {} event {}", net_id, e.map, e.event);
  }
  std::lock_guard lock(g.mutex);
  g.host_pending.push_back(e);
  if (e.code == kCodeEvent) {
    ++g.host_events;
  } else {
    ++g.host_values;
  }
}

void NoteGuestDrop() {
  std::lock_guard lock(g.mutex);
  ++g.guest_dropped_local;
}

bool Applying() { return t_applying; }

bool TakeWaitingPress(uint32_t input, uint32_t map, uint32_t event, bool heard_anyway) {
  std::lock_guard lock(g.mutex);
  const auto it = g.waiting_presses.find(PressKey(input, map, event));
  if (it == g.waiting_presses.end()) return false;
  const bool fresh = Clock::now() - it->second <= kPressWait;
  g.waiting_presses.erase(it);
  if (fresh && !heard_anyway) ++g.presses_kept;
  return fresh;
}

uint32_t Root(uint8_t* base, uint32_t input) { return RootOf(base, input); }

}  // namespace enemy_input
}  // namespace net

// THE EVENT PRESS (sub_82273E48: r3 = input object, r4 = map type, r5 =
// event; walks to the root input object, finds the map, sets the event's
// "happened" bit via sub_82275D60). The enemy AI and scripts press their moves
// through it (callers sub_8221F100, sub_8221E588: behaviour-controller
// methods; sub_822C7338). Online only; offline it is the original.
extern "C" REX_FUNC(sub_82273E48) {
  using namespace net::enemy_input;
  if (net::Active() && more_players::InPlay()) {
    const uint32_t input = ctx.r3.u32;
    if (net::IsHost()) {
      uint32_t id = net::world_sync::HostEnemyOfInput(input);
      if (!id) id = net::world_sync::HostEnemyOfInput(Root(base, input));
      if (id) {
        Entry e;
        e.code = 0xFF;
        e.map = uint8_t(ctx.r4.u32);
        e.event = uint8_t(ctx.r5.u32);
        NoteHost(id, e);
      }
    } else if (net::world_sync::EnemyPuppets() && !Applying() &&
               (net::world_sync::IsPuppetEnemyInput(input) ||
                net::world_sync::IsPuppetEnemyInput(Root(base, input)))) {
      NoteGuestDrop();  // only the host's AI presses a puppet's moves
      return;
    }
  }
  __imp__sub_82273E48(ctx, base);
}

// The ONE writer of every value channel (findings/62): r3 = channel, f1 =
// value, r5 = game clock ms. Online only; offline it is the original.
extern "C" REX_FUNC(sub_82273328) {
  using namespace net::enemy_input;
  if (net::Active() && more_players::InPlay()) {
    const uint32_t channel = ctx.r3.u32;
    if (net::IsHost()) {
      int index = 0;
      const uint32_t id = net::world_sync::HostEnemyOfChannel(channel, index);
      if (id) {
        Entry e;
        e.code = uint8_t(index);
        e.value = float(ctx.f1.f64);
        NoteHost(id, e);
      }
    } else if (net::world_sync::EnemyPuppets() && !Applying() &&
               net::world_sync::IsPuppetEnemyChannel(channel)) {
      NoteGuestDrop();
      return;
    }
  }
  __imp__sub_82273328(ctx, base);
}
