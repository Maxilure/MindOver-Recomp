// =============================================================================
// net/state_sync.cpp -- host-authoritative state synchronization (Phase 1)
// =============================================================================
//
// GUEST OFFSETS USED (all verified in docs/findings/22, 26, 29):
//   0x8259B11C              u32[4] ECoOpPlayerState (0=out, 1=mask, 2=in game, 3=entering mask)
//   CActor + 44 (+0x2C)     ptr    4x4 big-endian world matrix:
//                                    row 0 (+0..+12)  = right
//                                    row 1 (+16..+28) = up
//                                    row 2 (+32..+44) = forward
//                                    row 3 (+48..+60) = position (x, y, z)
//   CActor + 308 (+0x134)   ptr    CPhysicsBehaviour (vtable 0x82032FC4):
//                                    +88  ptr to owning CActor
//                                    +104 float[3] velocity (vx, vy, vz)
//                                    +140 float[3] position (x, y, z)
//
// All guest memory accesses go through GuestPtr() + guest_memory::Readable()
// so Windows' 4 KB shift on 0xE0000000+ works identically to Linux.
// =============================================================================
#include "state_sync.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../cheats/cheats.h"     // PlayerHealth / SetPlayerHealthFromNetwork
#include "../data/fight_tree.h"  // CurrentFrontEnd: GameRunning vs a real level load
#include "../guest_memory.h"
#include "../input/players.h"
#include "../players/coop_camera.h"
#include "../players/more_players.h"
#include "session_sync.h"
#include "transport.h"
#include "enemy_input.h"
#include "world_sync.h"
#include "action_sync.h"
#include "mask_sync.h"
#include "titan_sync.h"
#include "../players/lost_controller.h"
#include "aim_sync.h"

REXCVAR_DEFINE_BOOL(online_host, false, "CrashMoM",
                    "Online co-op (state sync): host a LAN session on --online_port as Player 1");
REXCVAR_DEFINE_STRING(online_join, "", "CrashMoM",
                      "Online co-op (state sync): connect to a host IP/hostname on --online_port");
REXCVAR_DEFINE_INT32(online_port, 37373, "CrashMoM", "Online co-op: UDP port (default 37373)")
    .range(1024, 65535);
REXCVAR_DEFINE_INT32(online_rate, 60, "CrashMoM",
                     "Online co-op: player snapshot send rate in Hz (default 60)")
    .range(10, 60);
REXCVAR_DEFINE_BOOL(debug_online_trace, false, "CrashMoM",
                    "Online co-op: log spins, mask state changes, and sync events to the game log");

namespace net {
namespace {

using Clock = std::chrono::steady_clock;

// Guest constants (findings/22, 26, 29).
constexpr uint32_t kCoOpStateTable = 0x8259B11C;
constexpr uint32_t kActorMatrix = 44;
constexpr uint32_t kActorPhysics = 308;
constexpr uint32_t kPhysicsVtable = 0x82032FC4;
constexpr uint32_t kPhysicsActor = 88;
constexpr uint32_t kPhysicsVelocity = 104;
constexpr uint32_t kPhysicsPosition = 140;
constexpr uint32_t kPhysicsStuckTimer = 0xDC;  // "floating, stuck" 5 s respawn timer (findings/55)

constexpr uint32_t kXErrorSuccess = 0;
constexpr uint32_t kXErrorNotConnected = 1167;  // ERROR_DEVICE_NOT_CONNECTED

// XInput button bitmasks (in big-endian u16 at gamepad[0..1]).
constexpr uint16_t kBtnStart = 0x0010;
constexpr uint16_t kBtnBack = 0x0020;
constexpr uint16_t kBtnB = 0x2000;

// Protocol message IDs (first byte of payload).
enum MsgType : uint8_t {
  kMsgHello = 1,        // C -> H (reliable): request a slot
  kMsgWelcome = 2,      // H -> C (reliable): [1] slot, [2] connected_mask
  kMsgRoster = 3,       // H -> C (reliable): [1] connected_mask
  kMsgPlayerState = 4,  // Owner -> H -> others (unreliable @ 30 Hz)
  kMsgPing = 5,         // bidirectional (unreliable @ 1 Hz): [1..8] timestamp_us
  kMsgPong = 6,         // reply to Ping: [1..8] echoed timestamp_us
  kMsgSpin = 7,         // Owner -> H -> others (immediate unreliable): [1] slot
  kMsgPlayerHit = 9,    // Host -> all (reliable): [1] slot [2..5] f32 damage = an enemy's hit on that player
  kMsgPocket = 8,       // Owner -> H -> others (reliable): [1] slot [2] event = one accepted titan pocket press
};

// Wire layout for kMsgPlayerState (58 bytes):
//   +0  u8       type = kMsgPlayerState
//   +1  u8       slot (0..3)
//   +2  u8       coop_state (0..4)
//   +3  u8       flags (bit 0 = has_actor, bit 1 = riding_titan, bit 2 = spin_pulse,
//                       bit 3 = spin_hold, bit 4 = has_reticle, bit 5 = on_ground)
//   +4  u32      seq
//   +8  f32[3]   pos (x, y, z)
//   +20 f32[3]   vel (vx, vy, vz)
//   +32 f32      yaw (radians, atan2(fwd.x, fwd.z))
//   +36 u8[12]   gamepad (big-endian XINPUT_GAMEPAD)
//   +48 f32[2]   reticle (x, y on 640x480 canvas)
//   +56 s8       level_id (loaded level ID on owner's PC, -1 if not in level)
//   +57 u8       digging_byte (0x8259B124 + slot per-player digging flag)
//   +58 f32      hitpoints of the owner's body (valid with kFlagHasHealth)
// (58-byte packets of an older build are still read: no health in them.)
constexpr size_t kPlayerStateWireSize = 62;
constexpr size_t kPlayerStateOldWireSize = 58;
constexpr int32_t kFrontEndGameRunning = 489;  // Frontend.bfig "GameRunning"
constexpr uint32_t kDiggingFlagTable = 0x8259B124;
constexpr uint8_t kFlagHasActor = 1u << 0;
constexpr uint8_t kFlagRidingTitan = 1u << 1;
constexpr uint8_t kFlagSpinPulse = 1u << 2;
constexpr uint8_t kFlagSpinHold = 1u << 3;
constexpr uint8_t kFlagHasReticle = 1u << 4;
constexpr uint8_t kFlagOnGround = 1u << 5;
constexpr uint8_t kFlagInMenu = 1u << 6;
// HEALTH (2026-11-27): each player's OWN PC decides their health (a hit counts
// where it lands on the victim's screen); every other PC mirrors that number.
// Before, nobody else ever heard it: the attacker saw only a push, while the
// victim lost health on their screen alone.
constexpr uint8_t kFlagHasHealth = 1u << 7;

// Interpolation buffer tuning (cubic Hermite + adaptive smooth catch-up).
constexpr double kInterpDelaySec = 0.025;        // ~25 ms (~1.5 snapshots at 60 Hz, crisp jumps/attacks)
constexpr double kMaxExtrapSec = 0.14;           // dead-reckon up to 140 ms with velocity damping
constexpr float kSnapDistanceSq = 28.0f * 28.0f; // > 28 units: real teleport only (never snaps during titan sprints)
constexpr float kBlendRate = 20.0f;              // base convergence rate (ramps smoothly under lag spikes)

struct PlayerSnapshot {
  uint32_t seq = 0;
  uint8_t coop_state = 0;
  uint8_t flags = 0;
  int8_t level_id = -1;
  uint8_t digging_byte = 0;
  float hitpoints = 0.0f;  // with kFlagHasHealth
  float pos[3] = {0, 0, 0};
  float vel[3] = {0, 0, 0};
  float yaw = 0.0f;
  uint8_t gamepad[12] = {};
  float reticle_x = 320.0f;
  float reticle_y = 240.0f;
  Clock::time_point recv_time;
};

struct RemoteSlot {
  bool connected = false;
  PeerId peer = kInvalidPeer;  // used on host to map slot -> client peer
  uint32_t latest_seq = 0;
  bool has_snapshot = false;
  std::deque<PlayerSnapshot> history;  // newest at back, up to 8 entries
  uint32_t join_pulse_frames = 0;
  uint32_t mask_pulse_frames = 0;
  bool pending_spin = false;
  // Titan pocket/swap presses the owner's game accepted, not yet used by the
  // puppet here (TakeRemotePocket). Counted, not a bool: two quick accepted
  // swaps (1.25 s apart) must both happen. Old ones expire (kPocketLifetime).
  // ONLY for our own slot: enemy damage the host saw land on us (kMsgPlayerHit),
  // applied on the game thread by ApplyRemoteHealth.
  float incoming_hit = 0.0f;
  // [0] = riding event 37 (pocket / swap), [1] = on-foot event 14 (take out).
  std::array<int, 4> pending_pockets{};  // by kind: PocketKind() (37 / 14 / 23 / 2)
  std::array<Clock::time_point, 4> pocket_at{};
  bool spin_holding = false;
};

struct SessionState {
  std::recursive_mutex mutex;
  std::unique_ptr<ITransport> transport;
  std::atomic<bool> active{false};
  std::atomic<bool> is_host{false};
  std::atomic<int> local_slot{-1};
  std::atomic<uint32_t> connected_mask{0};
  PeerId host_peer = 1;  // UdpTransport assigns PeerId 1 to the host on clients

  std::array<RemoteSlot, kMaxPlayers> slots{};
  std::array<uint8_t, 12> cached_local_gamepad{};
  // Latch short 1-frame button taps between 30 Hz snapshots so Jump/Attack/B
  // presses at 60+ fps are never dropped between snapshot ticks.
  uint16_t latched_local_buttons = 0;
  bool latched_local_spin = false;
  bool local_spin_holding = false;
  bool local_on_ground = true;
  bool has_local_reticle = false;
  float local_reticle_x = 320.0f;
  float local_reticle_y = 240.0f;
  uint32_t local_seq = 0;
  uint32_t local_join_pulse_frames = 0;

  Clock::time_point last_send_time{};
  Clock::time_point last_hello_time{};
  Clock::time_point last_ping_time{};
  double rtt_ms = 0.0;
  uint64_t packets_sent = 0;
  uint64_t packets_recv = 0;
  uint64_t remote_physics_applied = 0;
};

SessionState g_net;

// Big-endian guest helpers (safe against unmapped memory).
bool ReadGuestU32(uint8_t* base, uint32_t addr, uint32_t& out) {
  if (!addr || !guest_memory::Readable(addr, 4)) return false;
  const uint8_t* p = GuestPtr(base, addr);
  out = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  return true;
}

bool ReadGuestF32(uint8_t* base, uint32_t addr, float& out) {
  uint32_t bits = 0;
  if (!ReadGuestU32(base, addr, bits)) return false;
  std::memcpy(&out, &bits, 4);
  return std::isfinite(out);
}

void WriteGuestF32(uint8_t* base, uint32_t addr, float value) {
  if (!addr || !guest_memory::Readable(addr, 4) || !std::isfinite(value)) return;
  uint32_t bits = 0;
  std::memcpy(&bits, &value, 4);
  uint8_t* p = GuestPtr(base, addr);
  p[0] = uint8_t(bits >> 24);
  p[1] = uint8_t(bits >> 16);
  p[2] = uint8_t(bits >> 8);
  p[3] = uint8_t(bits);
}

// Little-endian wire helpers.
void PutLE32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v & 0xFFu);
  p[1] = uint8_t((v >> 8) & 0xFFu);
  p[2] = uint8_t((v >> 16) & 0xFFu);
  p[3] = uint8_t((v >> 24) & 0xFFu);
}
void PutLE64(uint8_t* p, uint64_t v) {
  PutLE32(p, uint32_t(v & 0xFFFFFFFFu));
  PutLE32(p + 4, uint32_t(v >> 32));
}
void PutF32(uint8_t* p, float v) {
  uint32_t bits = 0;
  std::memcpy(&bits, &v, 4);
  PutLE32(p, bits);
}
uint32_t GetLE32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t GetLE64(const uint8_t* p) { return uint64_t(GetLE32(p)) | (uint64_t(GetLE32(p + 4)) << 32); }
float GetF32(const uint8_t* p) {
  uint32_t bits = GetLE32(p);
  float v = 0.0f;
  std::memcpy(&v, &bits, 4);
  return v;
}

bool SeqNewer32(uint32_t a, uint32_t b) { return a != b && uint32_t(a - b) < 0x80000000u; }

float LerpAngle(float a, float b, float t) {
  constexpr float kPi = 3.14159265358979323846f;
  float diff = std::fmod(b - a + kPi, 2.0f * kPi);
  if (diff < 0.0f) diff += 2.0f * kPi;
  diff -= kPi;
  return a + diff * t;
}

uint32_t FindPhysicsOf(uint8_t* base, uint32_t actor) {
  if (!actor) return 0;
  uint32_t phys = 0, vtbl = 0, owner = 0;
  if (ReadGuestU32(base, actor + kActorPhysics, phys) && phys &&
      ReadGuestU32(base, phys, vtbl) && vtbl == kPhysicsVtable &&
      ReadGuestU32(base, phys + kPhysicsActor, owner) && owner == actor) {
    return phys;
  }
  // Titans and enemies do NOT store CPhysicsBehaviour at actor +308 like Crash
  // does (findings/55). Fall back to the live actor -> physics map populated by
  // CPhysicsBehaviour::Update (sub_8217D0A0).
  return LookupCachedPhysics(base, actor);
}

uint32_t ReadCoOpState(uint8_t* base, int slot) {
  if (slot < 0 || slot >= kMaxPlayers) return 0;
  uint32_t st = 0;
  ReadGuestU32(base, kCoOpStateTable + uint32_t(slot) * 4, st);
  return st;
}

void SerializePlayerState(uint8_t* out, uint8_t slot, const PlayerSnapshot& snap) {
  out[0] = kMsgPlayerState;
  out[1] = slot;
  out[2] = snap.coop_state;
  out[3] = snap.flags;
  PutLE32(out + 4, snap.seq);
  for (int i = 0; i < 3; ++i) PutF32(out + 8 + 4 * i, snap.pos[i]);
  for (int i = 0; i < 3; ++i) PutF32(out + 20 + 4 * i, snap.vel[i]);
  PutF32(out + 32, snap.yaw);
  std::memcpy(out + 36, snap.gamepad, 12);
  PutF32(out + 48, snap.reticle_x);
  PutF32(out + 52, snap.reticle_y);
  out[56] = uint8_t(snap.level_id);
  out[57] = snap.digging_byte;
  PutF32(out + 58, snap.hitpoints);
}

bool DeserializePlayerState(const uint8_t* data, size_t size, uint8_t& slot, PlayerSnapshot& out) {
  if (size < kPlayerStateOldWireSize || data[0] != kMsgPlayerState) return false;
  slot = data[1];
  if (slot >= kMaxPlayers) return false;
  out.coop_state = data[2];
  out.flags = data[3];
  out.seq = GetLE32(data + 4);
  for (int i = 0; i < 3; ++i) {
    out.pos[i] = GetF32(data + 8 + 4 * i);
    out.vel[i] = GetF32(data + 20 + 4 * i);
    if (!std::isfinite(out.pos[i]) || !std::isfinite(out.vel[i])) return false;
  }
  out.yaw = GetF32(data + 32);
  if (!std::isfinite(out.yaw)) out.yaw = 0.0f;
  std::memcpy(out.gamepad, data + 36, 12);
  out.reticle_x = GetF32(data + 48);
  out.reticle_y = GetF32(data + 52);
  if (!std::isfinite(out.reticle_x) || !std::isfinite(out.reticle_y)) {
    out.flags &= ~kFlagHasReticle;
    out.reticle_x = 320.0f;
    out.reticle_y = 240.0f;
  }
  out.level_id = int8_t(data[56]);
  out.digging_byte = data[57];
  if (size >= kPlayerStateWireSize) {
    out.hitpoints = GetF32(data + 58);
    if (!std::isfinite(out.hitpoints)) out.flags &= ~kFlagHasHealth;
  } else {
    out.flags &= ~kFlagHasHealth;
  }
  return true;
}

// Recompute connected_mask on host and broadcast kMsgRoster to all clients.
void HostBroadcastRosterLocked() {
  uint32_t mask = 1u;  // Host is always slot 0.
  for (int s = 1; s < kMaxPlayers; ++s) {
    if (g_net.slots[s].connected) mask |= (1u << s);
  }
  g_net.connected_mask = mask;
  uint8_t msg[2] = {kMsgRoster, uint8_t(mask)};
  g_net.transport->Broadcast(msg, sizeof(msg), /*reliable=*/true);
}

int HostFindSlotByPeerLocked(PeerId peer) {
  for (int s = 1; s < kMaxPlayers; ++s) {
    if (g_net.slots[s].connected && g_net.slots[s].peer == peer) return s;
  }
  return -1;
}

void HandlePacketLocked(const PeerEvent& ev, Clock::time_point now) {
  if (ev.type == PeerEvent::Type::kDisconnected) {
    if (g_net.is_host) {
      const int s = HostFindSlotByPeerLocked(ev.peer);
      if (s > 0) {
        REXLOG_INFO("Online: Player {} (peer {}) disconnected", s + 1, ev.peer);
        g_net.slots[s] = RemoteSlot{};
        mask_sync::Forget(s);  // their last "mask" no longer holds the copy
        titan_sync::Forget(s);
        lost_controller::NetworkLeft(s);  // their copy drops out (lost_controller.h ONLINE)
        HostBroadcastRosterLocked();
      }
    } else {
      REXLOG_WARN("Online: disconnected from host");
      g_net.local_slot = -1;
      g_net.connected_mask = 0;
      for (auto& sl : g_net.slots) sl = RemoteSlot{};
    }
    return;
  }

  if (ev.type != PeerEvent::Type::kPacket || !ev.data || ev.size == 0) return;
  g_net.packets_recv++;
  const uint8_t msg_type = ev.data[0];

  switch (msg_type) {
    case kMsgHello: {
      if (!g_net.is_host) break;
      int slot = HostFindSlotByPeerLocked(ev.peer);
      if (slot < 0) {
        for (int s = 1; s < kMaxPlayers; ++s) {
          if (!g_net.slots[s].connected) {
            slot = s;
            break;
          }
        }
      }
      if (slot < 0) {
        REXLOG_WARN("Online: rejected peer {} (session full)", ev.peer);
        break;
      }
      g_net.slots[slot].connected = true;
      g_net.slots[slot].peer = ev.peer;
      HostBroadcastRosterLocked();

      uint8_t welcome[3] = {kMsgWelcome, uint8_t(slot), uint8_t(g_net.connected_mask)};
      g_net.transport->Send(ev.peer, welcome, sizeof(welcome), /*reliable=*/true);
      REXLOG_INFO("Online: assigned Player {} to peer {} (roster=0x{:X})", slot + 1, ev.peer,
                  g_net.connected_mask.load());
      break;
    }

    case kMsgWelcome: {
      if (g_net.is_host || ev.size < 3) break;
      const int assigned = int(ev.data[1]);
      if (assigned < 1 || assigned >= kMaxPlayers) break;
      g_net.local_slot = assigned;
      g_net.connected_mask = ev.data[2];
      for (int s = 0; s < kMaxPlayers; ++s) {
        g_net.slots[s].connected = (g_net.connected_mask & (1u << s)) != 0;
      }
      REXLOG_INFO("Online: welcomed by host as Player {} (roster=0x{:X})", assigned + 1,
                  g_net.connected_mask.load());
      break;
    }

    case kMsgRoster: {
      if (g_net.is_host || ev.size < 2) break;
      g_net.connected_mask = ev.data[1];
      for (int s = 0; s < kMaxPlayers; ++s) {
        const bool now_conn = (g_net.connected_mask & (1u << s)) != 0;
        if (!now_conn && g_net.slots[s].connected) {
          g_net.slots[s] = RemoteSlot{};
          mask_sync::Forget(s);
          titan_sync::Forget(s);
          lost_controller::NetworkLeft(s);
        } else {
          g_net.slots[s].connected = now_conn;
        }
      }
      REXLOG_INFO("Online: roster updated (0x{:X})", g_net.connected_mask.load());
      break;
    }

    case kMsgPlayerState: {
      uint8_t slot = 0;
      PlayerSnapshot snap;
      if (!DeserializePlayerState(ev.data, ev.size, slot, snap)) break;
      if (int(slot) == g_net.local_slot) break;  // ignore echoed self-state

      // Host validates sender owns `slot` and relays to all other connected clients.
      if (g_net.is_host) {
        if (slot == 0 || !g_net.slots[slot].connected || g_net.slots[slot].peer != ev.peer) {
          break;
        }
        g_net.transport->Broadcast(ev.data, ev.size, /*reliable=*/false, /*exclude=*/ev.peer);
      }

      RemoteSlot& rs = g_net.slots[slot];
      rs.connected = true;
      if (rs.has_snapshot && !SeqNewer32(snap.seq, rs.latest_seq)) break;  // out-of-order/old
      snap.recv_time = now;
      rs.latest_seq = snap.seq;
      rs.has_snapshot = true;
      // The OLD spin channel (pulse + hold flag -> game_mods TakeButtonSpin /
      // Holding) is off while copies obey their owner's states
      // (action_sync.h): its pulse waited unasked (the driven copy makes no
      // decisions) and fired a SECOND spin once the copy decided again (2026-11-27
      // playtest: simple spinning while walking).
      const bool old_spin = !REXCVAR_GET(online_action_sync);
      if (old_spin && (snap.flags & kFlagSpinPulse)) rs.pending_spin = true;
      rs.spin_holding = old_spin && (snap.flags & kFlagSpinHold) != 0;
      rs.history.push_back(snap);
      while (rs.history.size() > 8) rs.history.pop_front();
      break;
    }

    case kMsgPing: {
      if (ev.size < 9) break;
      uint8_t pong[9];
      pong[0] = kMsgPong;
      std::memcpy(pong + 1, ev.data + 1, 8);
      g_net.transport->Send(ev.peer, pong, sizeof(pong), /*reliable=*/false);
      break;
    }

    case kMsgPong: {
      if (ev.size < 9) break;
      const uint64_t sent_us = GetLE64(ev.data + 1);
      const uint64_t now_us = uint64_t(
          std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
      if (now_us >= sent_us) {
        const double sample_ms = double(now_us - sent_us) * 0.001;
        g_net.rtt_ms = (g_net.rtt_ms == 0.0) ? sample_ms : (g_net.rtt_ms * 0.8 + sample_ms * 0.2);
      }
      break;
    }

    case kMsgSpin: {
      if (ev.size < 2) break;
      const uint8_t slot = ev.data[1];
      if (slot >= kMaxPlayers || int(slot) == g_net.local_slot) break;
      if (g_net.is_host) {
        if (slot == 0 || !g_net.slots[slot].connected || g_net.slots[slot].peer != ev.peer) break;
        g_net.transport->Broadcast(ev.data, ev.size, /*reliable=*/false, /*exclude=*/ev.peer);
      }
      if (!REXCVAR_GET(online_action_sync)) g_net.slots[slot].pending_spin = true;  // see the snapshot
      if (REXCVAR_GET(debug_online_trace)) {
        REXLOG_INFO("OnlineTrace: RX kMsgSpin for P{}", slot + 1);
      }
      break;
    }

    case kMsgPocket: {
      if (ev.size < 3) break;
      const uint8_t slot = ev.data[1];
      if (slot >= kMaxPlayers || int(slot) == g_net.local_slot) break;
      if (PocketKind(ev.data[2]) < 0) break;
      if (g_net.is_host) {
        if (slot == 0 || !g_net.slots[slot].connected || g_net.slots[slot].peer != ev.peer) break;
        g_net.transport->Broadcast(ev.data, ev.size, /*reliable=*/true, /*exclude=*/ev.peer);
      }
      RemoteSlot& rs = g_net.slots[slot];
      const size_t e = size_t(PocketKind(ev.data[2]));
      if (titan_sync::On()) {
        // Titans by outcome (titan_sync.h): the press is a HOLD only: the copy
        // decides for itself meanwhile (no forced states: a pocket keeps rider
        // and titan linked), and hears nothing until the owner's announced
        // result picks its answer (GrantRemotePress).
        rs.pocket_at[e] = now;  // (session_sync's repair keeps out meanwhile)
        action_sync::AllowOwnDecisions(slot, 1200);
        if (REXCVAR_GET(debug_online_trace)) {
          REXLOG_INFO("OnlineTrace: RX titan press hold for P{} (event {})", slot + 1, ev.data[2]);
        }
        break;
      }
      rs.pending_pockets[e] = std::min(rs.pending_pockets[e] + 1, 2);
      rs.pocket_at[e] = now;
      action_sync::AllowOwnDecisions(slot, 700);  // the copy decides the pocket itself (action_sync.h)
      if (REXCVAR_GET(debug_online_trace)) {
        REXLOG_INFO("OnlineTrace: RX kMsgPocket for P{} (event {})", slot + 1, ev.data[2]);
      }
      break;
    }

    case kMsgPlayerHit: {
      if (ev.size < 6) break;
      const uint8_t slot = ev.data[1];
      if (slot >= kMaxPlayers) break;
      if (g_net.is_host) break;  // the host only sends these
      const float dmg = GetF32(ev.data + 2);
      if (int(slot) != g_net.local_slot || !std::isfinite(dmg) || dmg >= 0.0f) break;
      g_net.slots[slot].incoming_hit += std::max(dmg, -1000.0f);
      if (REXCVAR_GET(debug_online_trace)) {
        REXLOG_INFO("OnlineTrace: RX kMsgPlayerHit for P{} ({:.1f})", slot + 1, dmg);
      }
      break;
    }

    case kMsgEnemyInput: {
      if (!g_net.is_host) enemy_input::HandlePacket(ev.data, ev.size);
      break;
    }

    case aim_sync::kMsgAim: {  // an owner's mask aim segment (aim_sync.h)
      if (ev.size < aim_sync::kAimSize) break;
      const uint8_t slot = ev.data[1];
      if (slot >= kMaxPlayers || int(slot) == g_net.local_slot) break;
      if (g_net.is_host) {
        if (slot == 0 || !g_net.slots[slot].connected || g_net.slots[slot].peer != ev.peer) break;
        g_net.transport->Broadcast(ev.data, ev.size, /*reliable=*/false, /*exclude=*/ev.peer);
      }
      aim_sync::OnReceive(ev.data, ev.size);
      break;
    }

    case action_sync::kMsgEnemyState: {  // the host's enemy states (guests only)
      if (g_net.is_host) break;
      action_sync::OnReceiveEnemy(ev.data, ev.size);
      break;
    }

    case mask_sync::kMsgMask: {  // an owner's co-op state change: mask / on foot (mask_sync.h)
      if (ev.size < mask_sync::kMaskSize) break;
      const uint8_t slot = ev.data[1];
      if (slot >= kMaxPlayers || int(slot) == g_net.local_slot) break;
      if (g_net.is_host) {
        if (slot == 0 || !g_net.slots[slot].connected || g_net.slots[slot].peer != ev.peer) break;
        g_net.transport->Broadcast(ev.data, ev.size, /*reliable=*/true, /*exclude=*/ev.peer);
      }
      mask_sync::OnReceive(ev.data, ev.size);
      break;
    }

    case titan_sync::kMsgTitan: {  // an owner's titans: ridden + pocketed (titan_sync.h)
      if (ev.size < titan_sync::kTitanSize) break;
      const uint8_t slot = ev.data[1];
      if (slot >= kMaxPlayers || int(slot) == g_net.local_slot) break;
      if (g_net.is_host) {
        if (slot == 0 || !g_net.slots[slot].connected || g_net.slots[slot].peer != ev.peer) break;
        g_net.transport->Broadcast(ev.data, ev.size, /*reliable=*/true, /*exclude=*/ev.peer);
      }
      titan_sync::OnReceive(ev.data, ev.size);
      break;
    }

    case action_sync::kMsgTreeState: {  // an owner's fight-tree state (action_sync.h)
      if (ev.size < action_sync::kTreeStateSize) break;
      const uint8_t slot = ev.data[1];
      if (slot >= kMaxPlayers || int(slot) == g_net.local_slot) break;
      if (g_net.is_host) {
        if (slot == 0 || !g_net.slots[slot].connected || g_net.slots[slot].peer != ev.peer) break;
        g_net.transport->Broadcast(ev.data, ev.size, /*reliable=*/true, /*exclude=*/ev.peer);
      }
      action_sync::OnReceive(ev.data, ev.size);
      break;
    }

    case kMsgWorldSnapshot:
    case kMsgDamageReq:
    case kMsgEntityHp:
    case kMsgCheatSpawn: {
      world_sync::HandlePacket(ev.data, ev.size, g_net.is_host);
      break;
    }

    case kMsgAreaTransition:
    case kMsgCutsceneSync:
    case kMsgTitanOwnership:
    case kMsgPickupEvent:
    case kMsgStatsSync:
    case kMsgRequestHostSave:
    case kMsgHostSaveChunk:
    case kMsgWorldInteract:
    case kMsgOutfitSync: {
      session_sync::HandlePacket(ev.data, ev.size, g_net.is_host, ev.peer);
      break;
    }

    default:
      break;
  }
}

// Compute velocity-assisted cubic Hermite interpolated target state for remote
// slot `slot` at `now` so curved paths and stops stay smooth without corners or jitter.
bool SampleRemoteStateLocked(int slot, Clock::time_point now, float out_pos[3], float out_vel[3],
                             float& out_yaw) {
  const RemoteSlot& rs = g_net.slots[slot];
  if (!rs.connected || !rs.has_snapshot || rs.history.empty()) return false;
  const PlayerSnapshot& latest = rs.history.back();
  if (!(latest.flags & kFlagHasActor)) return false;

  const auto target_time =
      now - std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(kInterpDelaySec));

  // Find bracketing pair [a, b] where a.recv_time <= target_time <= b.recv_time.
  if (rs.history.size() >= 2 && target_time < latest.recv_time) {
    for (size_t i = rs.history.size() - 1; i > 0; --i) {
      const PlayerSnapshot& b = rs.history[i];
      const PlayerSnapshot& a = rs.history[i - 1];
      if (a.recv_time <= target_time && target_time <= b.recv_time) {
        const double span = std::chrono::duration<double>(b.recv_time - a.recv_time).count();
        const float dt_seg = float(std::clamp(span, 0.0, 0.25));
        const float t =
            (span > 1e-6)
                ? std::clamp(float(std::chrono::duration<double>(target_time - a.recv_time).count() /
                                   span),
                             0.0f, 1.0f)
                : 1.0f;
        // Cubic Hermite basis functions (unless segment is a teleport > 20 u).
        const float seg_dx = b.pos[0] - a.pos[0];
        const float seg_dy = b.pos[1] - a.pos[1];
        const float seg_dz = b.pos[2] - a.pos[2];
        const bool use_hermite =
            dt_seg > 1e-4f && (seg_dx * seg_dx + seg_dy * seg_dy + seg_dz * seg_dz) < 400.0f;
        const float t2 = t * t;
        const float t3 = t2 * t;
        const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
        const float h10 = t3 - 2.0f * t2 + t;
        const float h01 = -2.0f * t3 + 3.0f * t2;
        const float h11 = t3 - t2;
        for (int k = 0; k < 3; ++k) {
          if (use_hermite) {
            out_pos[k] = h00 * a.pos[k] + h10 * (a.vel[k] * dt_seg) + h01 * b.pos[k] +
                         h11 * (b.vel[k] * dt_seg);
          } else {
            out_pos[k] = a.pos[k] + (b.pos[k] - a.pos[k]) * t;
          }
          out_vel[k] = a.vel[k] + (b.vel[k] - a.vel[k]) * t;
        }
        out_yaw = LerpAngle(a.yaw, b.yaw, t);
        return true;
      }
    }
    // Older than oldest snapshot: return oldest.
    const PlayerSnapshot& first = rs.history.front();
    std::memcpy(out_pos, first.pos, 12);
    std::memcpy(out_vel, first.vel, 12);
    out_yaw = first.yaw;
    return true;
  }

  // Latest snapshot is older than target_time: damped dead-reckoning so brief
  // jitter never overshoots or snaps back.
  const double extrap = std::clamp(
      std::chrono::duration<double>(target_time - latest.recv_time).count(), 0.0, kMaxExtrapSec);
  const float damp = float(std::exp(-4.0 * extrap));
  for (int k = 0; k < 3; ++k) {
    out_pos[k] = latest.pos[k] + latest.vel[k] * float(extrap) * damp;
    out_vel[k] = latest.vel[k] * damp;
  }
  out_yaw = latest.yaw;
  return true;
}

void SendLocalSnapshotLocked(uint8_t* base, Clock::time_point now) {
  if (g_net.local_slot < 0 || !g_net.transport || !g_net.transport->IsOpen()) return;
  const int hz = std::clamp(REXCVAR_GET(online_rate), 10, 60);
  const auto period = std::chrono::microseconds(1000000 / hz);
  if (now - g_net.last_send_time < period) return;
  g_net.last_send_time = now;

  PlayerSnapshot snap;
  snap.seq = ++g_net.local_seq;
  snap.coop_state = uint8_t(ReadCoOpState(base, g_net.local_slot));
  snap.level_id = int8_t(session_sync::CurrentLevelId(base));
  if (base && guest_memory::Readable(kDiggingFlagTable + uint32_t(g_net.local_slot), 1)) {
    snap.digging_byte = *GuestPtr(base, kDiggingFlagTable + uint32_t(g_net.local_slot));
  }
  // While our local player has an in-game menu / confirm box / outfit picker up
  // (or during the brief button-release window right as it closes), send neutral
  // controls so menu navigation never moves or attacks on remote screens!
  if (session_sync::ShouldBlockLocalGameplayInput(base)) {
    std::memset(snap.gamepad, 0, 12);
    g_net.latched_local_buttons = 0;
    g_net.latched_local_spin = false;
    g_net.local_spin_holding = false;
  } else {
    std::memcpy(snap.gamepad, g_net.cached_local_gamepad.data(), 12);
    const uint16_t merged_btns =
        ((uint16_t(snap.gamepad[0]) << 8) | snap.gamepad[1]) | g_net.latched_local_buttons;
    snap.gamepad[0] = uint8_t(merged_btns >> 8);
    snap.gamepad[1] = uint8_t(merged_btns & 0xFFu);
    g_net.latched_local_buttons = 0;
  }
  if (session_sync::IsLocalMenuOpen(base)) snap.flags |= kFlagInMenu;
  if (g_net.latched_local_spin) {
    snap.flags |= kFlagSpinPulse;
    g_net.latched_local_spin = false;
  }
  if (g_net.local_spin_holding) snap.flags |= kFlagSpinHold;
  if (g_net.local_on_ground) snap.flags |= kFlagOnGround;
  if (g_net.has_local_reticle) {
    snap.flags |= kFlagHasReticle;
    snap.reticle_x = g_net.local_reticle_x;
    snap.reticle_y = g_net.local_reticle_y;
  }

  // A loading screen while the front end is in GameRunning (489) is NOT a level
  // load: the world runs on (e.g. a black fade drawn under the loading screen,
  // 2026-11-27: the old check sent no position for 15 minutes and the other PC
  // showed this player at 0,0,0). Real loads leave 489; the receiver also
  // ignores snapshots from another level (level_id).
  const bool real_load =
      more_players::Loading() && fight_tree::CurrentFrontEnd().state != kFrontEndGameRunning;
  if (more_players::InPlay() && !real_load) {
    const uint32_t titan = more_players::TitanOfPlayer(g_net.local_slot);
    const uint32_t body = titan ? titan : more_players::CharacterOfPlayer(g_net.local_slot);
    if (body) {
      uint32_t mat = 0;
      const uint32_t phys = FindPhysicsOf(base, body);
      bool got_pos = false;
      if (phys) {
        got_pos = ReadGuestF32(base, phys + kPhysicsPosition, snap.pos[0]) &&
                  ReadGuestF32(base, phys + kPhysicsPosition + 4, snap.pos[1]) &&
                  ReadGuestF32(base, phys + kPhysicsPosition + 8, snap.pos[2]);
        ReadGuestF32(base, phys + kPhysicsVelocity, snap.vel[0]);
        ReadGuestF32(base, phys + kPhysicsVelocity + 4, snap.vel[1]);
        ReadGuestF32(base, phys + kPhysicsVelocity + 8, snap.vel[2]);
      }
      if (ReadGuestU32(base, body + kActorMatrix, mat) && mat) {
        if (!got_pos) {
          got_pos = ReadGuestF32(base, mat + 48, snap.pos[0]) &&
                    ReadGuestF32(base, mat + 52, snap.pos[1]) &&
                    ReadGuestF32(base, mat + 56, snap.pos[2]);
        }
        float fx = 0.0f, fz = 1.0f;
        if (ReadGuestF32(base, mat + 32, fx) && ReadGuestF32(base, mat + 40, fz)) {
          if (fx * fx + fz * fz > 1e-6f) snap.yaw = std::atan2(fx, fz);
        }
      }
      // Guard against uninitialized origin (0,0,0) right as a level finishes loading.
      const bool non_zero = (snap.pos[0] * snap.pos[0] + snap.pos[1] * snap.pos[1] +
                             snap.pos[2] * snap.pos[2]) > 1e-4f;
      if (got_pos && non_zero) {
        snap.flags |= kFlagHasActor;
        if (titan) snap.flags |= kFlagRidingTitan;
      }
      float hp = 0.0f, max_hp = 0.0f;
      if (cheats::PlayerHealth(g_net.local_slot, hp, max_hp)) {
        snap.hitpoints = hp;
        snap.flags |= kFlagHasHealth;
      }
    }
  }

  uint8_t wire[kPlayerStateWireSize];
  SerializePlayerState(wire, uint8_t(g_net.local_slot), snap);

  if (g_net.is_host) {
    g_net.transport->Broadcast(wire, sizeof(wire), /*reliable=*/false);
  } else {
    g_net.transport->Send(g_net.host_peer, wire, sizeof(wire), /*reliable=*/false);
  }
  g_net.packets_sent++;
}

void TickNetworkLocked(uint8_t* base) {
  if (!g_net.active || !g_net.transport || !g_net.transport->IsOpen()) return;
  const auto now = Clock::now();

  g_net.transport->Poll([&](const PeerEvent& ev) { HandlePacketLocked(ev, now); });

  // Client: send Hello periodically until welcomed with a slot.
  if (!g_net.is_host && g_net.local_slot < 0) {
    if (now - g_net.last_hello_time >= std::chrono::milliseconds(250)) {
      uint8_t hello[1] = {kMsgHello};
      g_net.transport->Send(g_net.host_peer, hello, sizeof(hello), /*reliable=*/true);
      g_net.last_hello_time = now;
      g_net.packets_sent++;
    }
    return;
  }

  // Heartbeat / RTT measurement (1 Hz).
  if (now - g_net.last_ping_time >= std::chrono::seconds(1)) {
    g_net.last_ping_time = now;
    uint8_t ping[9];
    ping[0] = kMsgPing;
    const uint64_t now_us = uint64_t(
        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
    PutLE64(ping + 1, now_us);
    if (g_net.is_host) {
      g_net.transport->Broadcast(ping, sizeof(ping), /*reliable=*/false);
    } else {
      g_net.transport->Send(g_net.host_peer, ping, sizeof(ping), /*reliable=*/false);
    }
  }

  // Our mask's aim segments (aim_sync.h): unreliable, newest wins.
  for (const auto& msg : aim_sync::TakeOutgoing()) {
    if (g_net.is_host) {
      g_net.transport->Broadcast(msg.data(), msg.size(), /*reliable=*/false);
    } else {
      g_net.transport->Send(g_net.host_peer, msg.data(), msg.size(), /*reliable=*/false);
    }
    g_net.packets_sent++;
  }

  // Our player's fight-tree states (action_sync.h): reliable, in the order taken.
  for (const auto& msg : action_sync::TakeOutgoing()) {
    if (g_net.is_host) {
      g_net.transport->Broadcast(msg.data(), msg.size(), /*reliable=*/true);
    } else {
      g_net.transport->Send(g_net.host_peer, msg.data(), msg.size(), /*reliable=*/true);
    }
    g_net.packets_sent++;
  }

  SendLocalSnapshotLocked(base, now);
}

void SetBeButtons(uint8_t* gamepad_be12, uint16_t buttons) {
  gamepad_be12[0] = uint8_t(buttons >> 8);
  gamepad_be12[1] = uint8_t(buttons & 0xFFu);
}

uint16_t GetBeButtons(const uint8_t* gamepad_be12) {
  return (uint16_t(gamepad_be12[0]) << 8) | gamepad_be12[1];
}

}  // namespace

void Install() {
  const uint16_t port = uint16_t(std::clamp(REXCVAR_GET(online_port), 1024, 65535));
  if (REXCVAR_GET(online_host)) {
    StartHost(port);
  } else if (!REXCVAR_GET(online_join).empty()) {
    StartJoin(REXCVAR_GET(online_join), port);
  }
}

void Shutdown() { Leave(); }

bool StartHost(uint16_t port) {
  {
    std::lock_guard lock(g_net.mutex);
    if (g_net.transport) g_net.transport->Close();
    g_net.slots = {};
    g_net.transport = CreateUdpTransport();
    if (!g_net.transport->Host(port)) {
      REXLOG_ERROR("Online: failed to host on port {}: {}", port, g_net.transport->LastError());
      g_net.transport.reset();
      g_net.active = false;
      return false;
    }
    g_net.active = true;
    g_net.is_host = true;
    g_net.local_slot = 0;
    g_net.connected_mask = 1u;
    g_net.slots[0].connected = true;
    g_net.rtt_ms = 0.0;
    g_net.packets_sent = 0;
    g_net.packets_recv = 0;
    g_net.remote_physics_applied = 0;
    world_sync::Reset();
    enemy_input::Reset();
    session_sync::Reset();
    mask_sync::Reset();
    titan_sync::Reset();
  }
  if (auto* pa = kbm::PlayerAssignment::Get()) pa->AnnounceIfChanged();
  return true;
}

bool StartJoin(std::string_view address, uint16_t port) {
  {
    std::lock_guard lock(g_net.mutex);
    if (g_net.transport) g_net.transport->Close();
    g_net.slots = {};
    g_net.transport = CreateUdpTransport();
    if (!g_net.transport->Connect(address, port)) {
      REXLOG_ERROR("Online: failed to connect to {}:{}: {}", address, port,
                   g_net.transport->LastError());
      g_net.transport.reset();
      g_net.active = false;
      return false;
    }
    g_net.active = true;
    g_net.is_host = false;
    g_net.local_slot = -1;  // assigned upon kMsgWelcome
    g_net.connected_mask = 0;
    g_net.last_hello_time = {};
    g_net.rtt_ms = 0.0;
    g_net.packets_sent = 0;
    g_net.packets_recv = 0;
    g_net.remote_physics_applied = 0;
    world_sync::Reset();
    enemy_input::Reset();
    session_sync::Reset();
    mask_sync::Reset();
    titan_sync::Reset();
  }
  if (auto* pa = kbm::PlayerAssignment::Get()) pa->AnnounceIfChanged();
  return true;
}

void Leave() {
  {
    std::lock_guard lock(g_net.mutex);
    if (g_net.transport) {
      g_net.transport->Close();
      g_net.transport.reset();
    }
    g_net.active = false;
    g_net.is_host = false;
    g_net.local_slot = -1;
    g_net.connected_mask = 0;
    g_net.slots = {};
    world_sync::Reset();
    action_sync::Reset();
    aim_sync::Reset();
    mask_sync::Reset();
    titan_sync::Reset();
    enemy_input::Reset();
    session_sync::Reset();
  }
  // Immediately restore normal local multi-controller routing (P1..P4) on leave!
  if (auto* pa = kbm::PlayerAssignment::Get()) pa->AnnounceIfChanged();
}

bool Active() { return g_net.active.load(std::memory_order_relaxed); }

bool IsHost() {
  return g_net.active.load(std::memory_order_relaxed) &&
         g_net.is_host.load(std::memory_order_relaxed);
}

int LocalSlot() {
  return g_net.active.load(std::memory_order_relaxed)
             ? g_net.local_slot.load(std::memory_order_relaxed)
             : -1;
}

uint32_t ConnectedMask() {
  return g_net.active.load(std::memory_order_relaxed)
             ? g_net.connected_mask.load(std::memory_order_relaxed)
             : 0;
}

bool HasVirtualController(uint32_t user) {
  if (user >= kMaxPlayers) return false;
  std::lock_guard lock(g_net.mutex);
  if (!g_net.active) return false;
  // Expose all connected slots (and our own slot) as present pads so the game
  // creates controller objects for remote players and lets them spawn/join.
  return (g_net.connected_mask & (1u << user)) != 0 || int(user) == g_net.local_slot;
}

void FilterControllerState(uint32_t user, uint8_t* gamepad_be12, uint32_t& xinput_result,
                           uint8_t* base) {
  if (user >= kMaxPlayers || !gamepad_be12) return;
  // Phase 2: broadcast host's active enemy/titan snapshots outside g_net.mutex
  // (world_sync acquires its own mutex and calls BroadcastFromHost).
  if (user == 0) {
    world_sync::TickHostBroadcast(base);
  }
  std::lock_guard lock(g_net.mutex);
  if (!g_net.active) return;

  // Drive network poll + snapshot send once per frame (on socket 0).
  if (user == 0) {
    TickNetworkLocked(base);
  }

  // Outside a level (title/main menu/loading), let normal local input work.
  if (!more_players::InPlay() || g_net.local_slot < 0) return;

  // Capture local player's input from socket 0 (keyboard + primary pad).
  if (user == 0) {
    if (xinput_result == kXErrorSuccess) {
      std::memcpy(g_net.cached_local_gamepad.data(), gamepad_be12, 12);
      g_net.latched_local_buttons |= GetBeButtons(gamepad_be12);
    } else {
      g_net.cached_local_gamepad.fill(0);
    }
  }

  // Local player's own slot:
  if (int(user) == g_net.local_slot) {
    std::memcpy(gamepad_be12, g_net.cached_local_gamepad.data(), 12);
    xinput_result = kXErrorSuccess;
    // On a client (slot 1..3), if our Crash hasn't joined this level yet,
    // pulse START automatically so our player spawns into the world.
    if (g_net.local_slot > 0 && ReadCoOpState(base, g_net.local_slot) == 0 &&
        !more_players::Loading()) {
      ++g_net.local_join_pulse_frames;
      if ((g_net.local_join_pulse_frames % 16) < 4) {
        SetBeButtons(gamepad_be12, GetBeButtons(gamepad_be12) | kBtnStart);
      }
    } else {
      g_net.local_join_pulse_frames = 0;
    }
    return;
  }

  // Remote player slot:
  RemoteSlot& rs = g_net.slots[user];
  if (!rs.connected) {
    std::memset(gamepad_be12, 0, 12);
    xinput_result = kXErrorNotConnected;
    return;
  }

  xinput_result = kXErrorSuccess;
  std::memset(gamepad_be12, 0, 12);

  // If the remote peer is in-game (mask or on foot) and hasn't joined on this
  // PC yet, pulse START so Radical's co-op system spawns their character.
  const uint32_t local_state = ReadCoOpState(base, int(user));
  if (rs.has_snapshot && !rs.history.empty()) {
    const PlayerSnapshot& latest = rs.history.back();
    // Player 1 too (was `user > 0`): the host's player 1 who died or dropped
    // out and came back (as a mask or on foot) never rejoined on the other PCs
    // (2026-11-27 test: "P1 says co-op state 1" heard, copy stayed not joined).
    // START from a not-joined socket 0 is the game's own rejoin (findings/26 s.31).
    if (local_state == 0 && latest.coop_state >= 1 && !more_players::Loading()) {
      ++rs.join_pulse_frames;
      if ((rs.join_pulse_frames % 16) < 4) {
        SetBeButtons(gamepad_be12, kBtnStart);
      }
      return;
    }
    rs.join_pulse_frames = 0;
    // Feed remote player's real controls to drive their Radical Engine fight
    // tree (including real B presses to jack titans and RB presses to pocket/
    // un-pocket titans), masking only START/BACK so remote pauses never pause
    // our local game.
    // Only use a fallback B pulse if mask <-> on-foot state stays mismatched
    // for > 25 frames (~0.4 s) after a dropped packet -- NEVER fake B for
    // titan mounting/un-pocketing (which accidentally triggered nearby Tikis).
    // Authoritative alive confirmation: if the remote player's own PC reports
    // them alive in gameplay (coop_state == 2), keep their alive byte at
    // 0x825B0008 + 36 + user set to 1 so local desyncs never trigger death rules.
    if (latest.coop_state == 2 && base && guest_memory::Readable(0x825B0008u + 36u + user, 1)) {
      *GuestPtr(base, 0x825B0008u + 36u + user) = 1;
    }
    std::memcpy(gamepad_be12, latest.gamepad, 12);
    // Align remote left-stick direction to THIS PC's independent camera so
    // Crash and ridden titans accelerate straight along their real world
    // velocity at full stick magnitude (full run animation instead of fighting
    // a rotated camera and getting stuck in slow walk/turn blend).
    if (local_state == 2 && latest.coop_state == 2) {
      const int16_t raw_lx =
          int16_t((uint16_t(gamepad_be12[4]) << 8) | uint16_t(gamepad_be12[5]));
      const int16_t raw_ly =
          int16_t((uint16_t(gamepad_be12[6]) << 8) | uint16_t(gamepad_be12[7]));
      const float raw_mag =
          std::min(1.0f, std::hypot(float(raw_lx) / 32767.0f, float(raw_ly) / 32767.0f));
      const float horiz_speed = std::hypot(latest.vel[0], latest.vel[2]);
      // Boost stick extent past Radical's 0.25 deadzone when moving fast or
      // riding a titan so the fight tree plays the full-speed run animation!
      const float speed_mag = (latest.flags & kFlagRidingTitan)
                                  ? std::clamp(horiz_speed / 4.5f, 0.0f, 1.0f)
                                  : std::clamp(horiz_speed / 6.0f, 0.0f, 1.0f);
      const float mag = (raw_mag > 0.65f) ? 1.0f : std::max(raw_mag, speed_mag);
      float cam_fx = 0.0f, cam_fz = 1.0f;
      bool have_cam = coop_camera::GetForwardXZ(cam_fx, cam_fz);
      if (!have_cam && base) {
        // Fallback: read active camera forward directly from *(*(0x8259B190)+76)+444
        // (+228 = fwd.x, +236 = fwd.z), identical to sub_822718E8.
        uint32_t uber = 0, cam_mgr = 0, active_cam = 0;
        float fx = 0.0f, fz = 0.0f;
        if (ReadGuestU32(base, 0x8259B190u, uber) && uber &&
            ReadGuestU32(base, uber + 76, cam_mgr) && cam_mgr &&
            ReadGuestU32(base, cam_mgr + 444, active_cam) && active_cam &&
            ReadGuestF32(base, active_cam + 228, fx) &&
            ReadGuestF32(base, active_cam + 236, fz)) {
          const float len = std::hypot(fx, fz);
          if (len > 1e-4f) {
            cam_fx = fx / len;
            cam_fz = fz / len;
            have_cam = true;
          }
        }
      }
      if (mag > 0.12f && have_cam) {
        float mx = std::sin(latest.yaw);
        float mz = std::cos(latest.yaw);
        if (horiz_speed > 0.5f) {
          mx = latest.vel[0] / horiz_speed;
          mz = latest.vel[2] / horiz_speed;
        }
        // Radical left-handed XZ: screen right = Cross((0,1,0), (cam_fx,0,cam_fz)) = (cam_fz, -cam_fx).
        const float new_lx = std::clamp((mx * cam_fz - mz * cam_fx) * mag, -1.0f, 1.0f);
        const float new_ly = std::clamp((mx * cam_fx + mz * cam_fz) * mag, -1.0f, 1.0f);
        const int16_t out_lx = int16_t(std::lround(new_lx * 32767.0f));
        const int16_t out_ly = int16_t(std::lround(new_ly * 32767.0f));
        gamepad_be12[4] = uint8_t(uint16_t(out_lx) >> 8);
        gamepad_be12[5] = uint8_t(uint16_t(out_lx) & 0xFFu);
        gamepad_be12[6] = uint8_t(uint16_t(out_ly) >> 8);
        gamepad_be12[7] = uint8_t(uint16_t(out_ly) & 0xFFu);
      }
    }
    uint16_t btns = GetBeButtons(gamepad_be12) & ~uint16_t(kBtnStart | kBtnBack);
    if (mask_sync::On()) {
      // MASKS BY ANNOUNCEMENT (mask_sync.h): no B for other players' copies at
      // all. Its jobs online are all announced or answered elsewhere: the mask
      // round trip (mask_sync), titan get-off / jack-from-a-titan (session_sync's
      // CheckInputEvent answers, kMsgPocket), the on-foot jack (fight-tree
      // states + the mount by net id). A replayed B could only do the copy's
      // OWN thing with it (a mask, a totem, a villager) at the wrong moment.
      SetBeButtons(gamepad_be12, uint16_t(btns & ~kBtnB));
      rs.mask_pulse_frames = 0;
      return;
    }
    const bool remote_wants_mask = (latest.coop_state == 1 || latest.coop_state == 3);
    const bool mask_mismatch =
        (local_state == 2 && remote_wants_mask) ||
        (local_state == 1 && latest.coop_state == 2);
    if (mask_mismatch) {
      ++rs.mask_pulse_frames;
      // Entering mask: our BlockRemoteMaskEntry gate holds off B until the
      // owner's coop_state flips to 3 or 1, so once confirmed, pulse B quickly
      // (~6 frames) to mirror the real mask transition. Leaving mask waits ~12
      // frames in case the real B press is still in flight.
      const int delay = remote_wants_mask ? 6 : 12;
      if (rs.mask_pulse_frames > delay && (rs.mask_pulse_frames % 16) < 3) {
        btns |= kBtnB;
      }
    } else {
      rs.mask_pulse_frames = 0;
    }
    SetBeButtons(gamepad_be12, btns);
  }
}

namespace {
std::mutex g_phys_cache_mutex;
std::unordered_map<uint32_t, uint32_t> g_actor_to_physics;
}  // namespace

void NotePhysicsBehaviour(uint8_t* base, uint32_t physics_behaviour) {
  if (!base || !physics_behaviour) return;
  uint32_t actor = 0;
  if (!ReadGuestU32(base, physics_behaviour + kPhysicsActor, actor) || !actor) return;
  std::lock_guard lock(g_phys_cache_mutex);
  if (g_actor_to_physics.size() > 512) g_actor_to_physics.clear();
  g_actor_to_physics[actor] = physics_behaviour;
}

uint32_t LookupCachedPhysics(uint8_t* base, uint32_t actor) {
  if (!base || !actor) return 0;
  uint32_t phys = 0;
  {
    std::lock_guard lock(g_phys_cache_mutex);
    const auto it = g_actor_to_physics.find(actor);
    if (it == g_actor_to_physics.end()) return 0;
    phys = it->second;
  }
  uint32_t vtbl = 0, owner = 0;
  if (!guest_memory::Readable(phys, 244)) return 0;
  if (!ReadGuestU32(base, phys, vtbl) || vtbl != kPhysicsVtable) return 0;
  if (!ReadGuestU32(base, phys + kPhysicsActor, owner) || owner != actor) return 0;
  return phys;
}

bool IsPlayerInMenu(int player, uint8_t* base) {
  if (!g_net.active.load(std::memory_order_relaxed) || player < 0 || player >= kMaxPlayers) {
    return false;
  }
  const int local = g_net.local_slot.load(std::memory_order_relaxed);
  if (player == local) {
    return base ? session_sync::IsLocalMenuOpen(base) : session_sync::IsLocalMenuOpenCached();
  }
  std::lock_guard lock(g_net.mutex);
  const RemoteSlot& rs = g_net.slots[player];
  if (!rs.connected || !rs.has_snapshot || rs.history.empty()) return false;
  return (rs.history.back().flags & kFlagInMenu) != 0;
}

bool AnyPlayerInMenu(uint8_t* base) {
  if (!g_net.active.load(std::memory_order_relaxed)) return false;
  if (base ? session_sync::IsLocalMenuOpen(base) : session_sync::IsLocalMenuOpenCached()) {
    return true;
  }
  std::lock_guard lock(g_net.mutex);
  const int local = g_net.local_slot.load(std::memory_order_relaxed);
  for (int p = 0; p < kMaxPlayers; ++p) {
    if (p == local || !g_net.slots[p].connected) continue;
    const RemoteSlot& rs = g_net.slots[p];
    if (rs.has_snapshot && !rs.history.empty() && (rs.history.back().flags & kFlagInMenu)) {
      return true;
    }
  }
  return false;
}

void PrepareRemotePhysics(uint8_t* base, uint32_t physics_behaviour, float dt) {
  std::lock_guard lock(g_net.mutex);
  if (!g_net.active || g_net.local_slot < 0 || !more_players::InPlay() || more_players::Loading()) {
    return;
  }

  uint32_t actor = 0;
  if (!ReadGuestU32(base, physics_behaviour + kPhysicsActor, actor) || !actor) return;

  // Identify whether `actor` belongs to a connected remote player slot.
  int remote_slot = -1;
  for (int p = 0; p < kMaxPlayers; ++p) {
    if (p == g_net.local_slot || !g_net.slots[p].connected) continue;
    const uint32_t titan = more_players::TitanOfPlayer(p);
    const uint32_t body = titan ? titan : more_players::CharacterOfPlayer(p);
    if (body == actor) {
      remote_slot = p;
      break;
    }
  }
  if (remote_slot < 0) return;

  // Only drive physics on an on-foot (or riding-titan) remote player. If either
  // PC has this player in mask state (coop_state != 2), let Radical's mask
  // attacher position them instead of dragging an on-foot Crash into the sky.
  const uint32_t local_state = ReadCoOpState(base, remote_slot);
  const RemoteSlot& rs = g_net.slots[remote_slot];
  if (local_state != 2 || !rs.has_snapshot || rs.history.empty() ||
      rs.history.back().coop_state != 2) {
    return;
  }

  float target_pos[3] = {0, 0, 0};
  float target_vel[3] = {0, 0, 0};
  float target_yaw = 0.0f;
  if (!SampleRemoteStateLocked(remote_slot, Clock::now(), target_pos, target_vel, target_yaw)) {
    return;
  }

  const PlayerSnapshot& latest = rs.history.back();
  const int local_lvl = session_sync::CurrentLevelId(base);
  // Cross-area & transition/cutscene guard: if the remote player is in a
  // different loaded level/area (or a transition/cutscene just ran), NEVER
  // drag their puppet into unloaded void geometry!
  if (session_sync::InTransitionOrCutsceneGrace() ||
      (latest.level_id >= 0 && local_lvl >= 0 && int(latest.level_id) != local_lvl)) {
    WriteGuestF32(base, physics_behaviour + kPhysicsStuckTimer, 0.0f);
    return;
  }

  const bool remote_on_ground = (latest.flags & kFlagOnGround) != 0;

  // Current physics position.
  float cur_pos[3] = {0, 0, 0};
  const bool have_cur = ReadGuestF32(base, physics_behaviour + kPhysicsPosition, cur_pos[0]) &&
                        ReadGuestF32(base, physics_behaviour + kPhysicsPosition + 4, cur_pos[1]) &&
                        ReadGuestF32(base, physics_behaviour + kPhysicsPosition + 8, cur_pos[2]);

  // When airborne (jumping / jump-attacking), use the latest snapshot's Y and
  // vertical velocity directly so the 25 ms interpolation delay never pins a
  // jumping Crash/Titan back to the ground.
  const float desired_y = remote_on_ground ? target_pos[1] : latest.pos[1];
  float new_pos[3] = {target_pos[0], desired_y, target_pos[2]};
  if (have_cur) {
    const float dx = target_pos[0] - cur_pos[0];
    const float dy = desired_y - cur_pos[1];
    const float dz = target_pos[2] - cur_pos[2];
    const float dist_sq = dx * dx + dy * dy + dz * dz;
    if (dist_sq < kSnapDistanceSq) {
      const float clamped_dt = std::clamp(dt, 0.001f, 0.1f);
      // Adaptive smooth convergence: gentle 16/s under normal play, ramping up
      // smoothly to 38/s under larger lag gaps so players glide back into step
      // instead of abruptly teleporting!
      const float dist = std::sqrt(dist_sq);
      const float rate = kBlendRate + std::clamp((dist - 1.5f) * 1.4f, 0.0f, 22.0f);
      const float alpha = 1.0f - std::exp(-rate * clamped_dt);
      new_pos[0] = cur_pos[0] + dx * alpha;
      // Uneven-ground bump handling:
      //   * Stepping UP (dy > 0.01f): lift Y immediately to `desired_y` so a
      //     remote Crash/Titan sweeps cleanly OVER rising bumps and steps instead
      //     of colliding horizontally into the bump face, stopping, and snapping!
      //   * Flat / downhill (dy <= 0.01f): blend gently while grounded so
      //     Radical's ground sweep keeps smooth slope contact without micro-falls.
      if (remote_on_ground && dy > 0.01f && dy < 2.2f) {
        new_pos[1] = (dy > 0.05f) ? desired_y : (cur_pos[1] + dy * std::max(alpha, 0.85f));
      } else if (remote_on_ground && std::fabs(dy) < 1.25f) {
        new_pos[1] = cur_pos[1] + dy * (alpha * 0.45f);
      } else {
        new_pos[1] = cur_pos[1] + dy * alpha;
      }
      new_pos[2] = cur_pos[2] + dz * alpha;
    }
  }

  // Write blended position and owner velocity BEFORE CPhysicsBehaviour::Update
  // so Radical's own ground sweep resolves floor contact cleanly. When
  // grounded, leave vy to Radical's ground physics so downhill slopes never
  // inject a downward falling velocity.
  for (int i = 0; i < 3; ++i) {
    WriteGuestF32(base, physics_behaviour + kPhysicsPosition + 4 * i, new_pos[i]);
  }
  WriteGuestF32(base, physics_behaviour + kPhysicsVelocity, target_vel[0]);
  if (!remote_on_ground) {
    WriteGuestF32(base, physics_behaviour + kPhysicsVelocity + 4, latest.vel[1]);
  }
  WriteGuestF32(base, physics_behaviour + kPhysicsVelocity + 8, target_vel[2]);
  WriteGuestF32(base, physics_behaviour + kPhysicsStuckTimer, 0.0f);
  g_net.remote_physics_applied++;
}

void FinishRemotePhysics(uint8_t* base, uint32_t physics_behaviour) {
  std::lock_guard lock(g_net.mutex);
  if (!g_net.active || g_net.local_slot < 0 || !more_players::InPlay() || more_players::Loading()) {
    return;
  }

  uint32_t actor = 0;
  if (!ReadGuestU32(base, physics_behaviour + kPhysicsActor, actor) || !actor) return;

  int remote_slot = -1;
  for (int p = 0; p < kMaxPlayers; ++p) {
    if (p == g_net.local_slot || !g_net.slots[p].connected) continue;
    const uint32_t titan = more_players::TitanOfPlayer(p);
    const uint32_t body = titan ? titan : more_players::CharacterOfPlayer(p);
    if (body == actor) {
      remote_slot = p;
      break;
    }
  }
  if (remote_slot < 0) return;

  // Only drive physics on an on-foot (or riding-titan) remote player. If either
  // PC has this player in mask state (coop_state != 2), let Radical's mask
  // attacher position them instead of dragging an on-foot Crash into the sky.
  const uint32_t local_state = ReadCoOpState(base, remote_slot);
  const RemoteSlot& rs = g_net.slots[remote_slot];
  if (local_state != 2 || !rs.has_snapshot || rs.history.empty() ||
      rs.history.back().coop_state != 2) {
    return;
  }

  // Never let the local "floating, stuck" 5 s safety timer respawn a remote
  // puppet; the owner PC is authoritative for their own deaths/respawns.
  WriteGuestF32(base, physics_behaviour + kPhysicsStuckTimer, 0.0f);

  const PlayerSnapshot& latest = rs.history.back();
  const int local_lvl = session_sync::CurrentLevelId(base);
  if (session_sync::InTransitionOrCutsceneGrace() ||
      (latest.level_id >= 0 && local_lvl >= 0 && int(latest.level_id) != local_lvl)) {
    return;
  }

  float target_pos[3] = {0, 0, 0};
  float target_vel[3] = {0, 0, 0};
  float target_yaw = 0.0f;
  if (!SampleRemoteStateLocked(remote_slot, Clock::now(), target_pos, target_vel, target_yaw)) {
    return;
  }

  // Apply owner facing yaw to the actor's 4x4 world matrix (rows 0 & 2).
  uint32_t mat = 0;
  if (ReadGuestU32(base, actor + kActorMatrix, mat) && mat) {
    const float s = std::sin(target_yaw);
    const float c = std::cos(target_yaw);
    WriteGuestF32(base, mat + 0, c);
    WriteGuestF32(base, mat + 4, 0.0f);
    WriteGuestF32(base, mat + 8, -s);
    WriteGuestF32(base, mat + 32, s);
    WriteGuestF32(base, mat + 36, 0.0f);
    WriteGuestF32(base, mat + 40, c);
  }
}

void NoteLocalSpin(int player) {
  std::lock_guard lock(g_net.mutex);
  if (!g_net.active || player != g_net.local_slot) return;
  // Send ONE immediate 2-byte spin event right now (do NOT also latch into the
  // next 30 Hz snapshot, which caused remote Crash to queue a second spin!).
  if (g_net.transport && g_net.transport->IsOpen()) {
    uint8_t msg[2] = {kMsgSpin, uint8_t(player)};
    if (g_net.is_host) {
      g_net.transport->Broadcast(msg, sizeof(msg), /*reliable=*/false);
    } else {
      g_net.transport->Send(g_net.host_peer, msg, sizeof(msg), /*reliable=*/false);
    }
    g_net.packets_sent++;
    if (REXCVAR_GET(debug_online_trace)) {
      REXLOG_INFO("OnlineTrace: TX kMsgSpin for P{}", player + 1);
    }
  }
}

// A pocket the puppet never got to use (it was mid-animation, a cutscene
// started) must not fire seconds later out of nowhere.
constexpr auto kPocketLifetime = std::chrono::milliseconds(1500);

void GrantRemotePress(int player, uint8_t event) {
  if (player < 0 || player >= kMaxPlayers || PocketKind(event) < 0) return;
  {
    std::lock_guard lock(g_net.mutex);
    if (!g_net.active || player == g_net.local_slot) return;
    RemoteSlot& rs = g_net.slots[player];
    const size_t e = size_t(PocketKind(event));
    rs.pending_pockets[e] = std::min(rs.pending_pockets[e] + 1, 2);
    rs.pocket_at[e] = Clock::now();
  }
  action_sync::AllowOwnDecisions(player, 700);
}

void NoteLocalPocket(int player, uint8_t event) {
  std::lock_guard lock(g_net.mutex);
  if (!g_net.active || player != g_net.local_slot) return;
  if (!g_net.transport || !g_net.transport->IsOpen()) return;
  // Reliable: a lost swap would leave the titans different on two screens
  // until session_sync's titan check repairs them (~2 s).
  uint8_t msg[3] = {kMsgPocket, uint8_t(player), event};
  if (g_net.is_host) {
    g_net.transport->Broadcast(msg, sizeof(msg), /*reliable=*/true);
  } else {
    g_net.transport->Send(g_net.host_peer, msg, sizeof(msg), /*reliable=*/true);
  }
  g_net.packets_sent++;
  if (REXCVAR_GET(debug_online_trace)) {
    REXLOG_INFO("OnlineTrace: TX kMsgPocket for P{} (event {})", player + 1, event);
  }
}

Clock::time_point LastRemoteTitanPress(int player) {
  if (player < 0 || player >= kMaxPlayers) return {};
  std::lock_guard lock(g_net.mutex);
  Clock::time_point last{};
  for (const auto& t : g_net.slots[player].pocket_at) last = std::max(last, t);
  return last;
}

int PocketKind(uint8_t event) {
  switch (event) {
    case 37: return 0;  // ENEMY map 6: riding RB = pocket / swap
    case 14: return 1;  // PLAYER map 5: on foot RB = take the pocketed one out
    case 23: return 2;  // ENEMY map 6: riding B = get off, or jack the next titan (pockets the old one)
    case 2: return 3;   // GENERAL map 4: the same B, asked through the titan's controller
    default: return -1;
  }
}

bool TakeRemotePocket(int player, uint8_t event) {
  if (player < 0 || player >= kMaxPlayers) return false;
  std::lock_guard lock(g_net.mutex);
  if (!g_net.active || player == g_net.local_slot) return false;
  RemoteSlot& rs = g_net.slots[player];
  if (PocketKind(event) < 0) return false;
  const size_t e = size_t(PocketKind(event));
  if (rs.pending_pockets[e] <= 0) return false;
  if (Clock::now() - rs.pocket_at[e] > kPocketLifetime) {
    rs.pending_pockets[e] = 0;
    return false;
  }
  rs.pending_pockets[e]--;
  if (REXCVAR_GET(debug_online_trace)) {
    REXLOG_INFO("OnlineTrace: P{} puppet used an accepted pocket press (event {})", player + 1, event);
  }
  return true;
}

void ApplyRemoteHealth(PPCContext& ctx, uint8_t* base) {
  struct Want {
    int player;
    float hitpoints;
  };
  Want wants[kMaxPlayers];
  int count = 0;
  float my_hit = 0.0f;
  int my_slot = -1;
  {
    std::lock_guard lock(g_net.mutex);
    if (!g_net.active || !more_players::InPlay()) return;
    if (g_net.local_slot >= 0 && g_net.local_slot < kMaxPlayers) {
      my_slot = g_net.local_slot;
      my_hit = g_net.slots[my_slot].incoming_hit;
      g_net.slots[my_slot].incoming_hit = 0.0f;
    }
    for (int p = 0; p < kMaxPlayers; ++p) {
      if (p == g_net.local_slot) continue;
      const RemoteSlot& rs = g_net.slots[p];
      if (!rs.connected || !rs.has_snapshot || rs.history.empty()) continue;
      const PlayerSnapshot& last = rs.history.back();
      if (!(last.flags & kFlagHasHealth) || !(last.flags & kFlagHasActor)) continue;
      if (Clock::now() - last.recv_time > std::chrono::milliseconds(500)) continue;
      // Same body on both PCs only: while a titan mount / dismount / pocket
      // is still being matched here, the owner's titan health must not land
      // on our copy of their Crash (or the other way round).
      const bool rides_here = more_players::TitanOfPlayer(p) != 0;
      if (rides_here != ((last.flags & kFlagRidingTitan) != 0)) continue;
      if (ReadCoOpState(base, p) != 2 || last.coop_state != 2) continue;  // on foot / riding only
      wants[count++] = {p, last.hitpoints};
    }
  }
  if (session_sync::InTransitionOrCutsceneGrace()) return;
  // An enemy's hit the host saw land on us: our own health, through the
  // game's change routine (health bar, death); no hurt animation (the enemy
  // swung on the host's screen only).
  if (my_slot >= 0 && my_hit < 0.0f) {
    float hp = 0.0f, max_hp = 0.0f;
    if (cheats::PlayerHealth(my_slot, hp, max_hp)) {
      cheats::SetPlayerHealthFromNetwork(ctx, base, my_slot, hp + my_hit);
    }
  }
  // Outside the lock: the game's routine runs our hooks (and maybe a death).
  for (int i = 0; i < count; ++i) {
    cheats::SetPlayerHealthFromNetwork(ctx, base, wants[i].player, wants[i].hitpoints);
  }
}

namespace {
thread_local int t_hit_depth = 0;
thread_local bool t_remote_hitter_on_enemy = false;
thread_local int t_remote_victim = -1;
thread_local bool t_forward = false;
thread_local float t_victim_damage = 0.0f;
thread_local int t_by = -1;
}  // namespace

OnlineHitScope::OnlineHitScope(bool remote_hitter_on_enemy, int remote_victim, bool forward, int by) {
  if (t_hit_depth++ > 0) return;  // a message inside a message: the outer one counts
  t_by = by;
  t_remote_hitter_on_enemy = remote_hitter_on_enemy;
  t_remote_victim = remote_victim;
  t_forward = forward;
  t_victim_damage = 0.0f;
}

OnlineHitScope::~OnlineHitScope() {
  if (--t_hit_depth > 0) return;
  const bool send = t_forward && t_remote_victim >= 0 && t_victim_damage < -1e-4f;
  const int victim = t_remote_victim;
  const float damage = t_victim_damage;
  t_remote_hitter_on_enemy = false;
  t_remote_victim = -1;
  t_forward = false;
  t_victim_damage = 0.0f;
  t_by = -1;
  if (!send) return;
  std::lock_guard lock(g_net.mutex);
  if (!g_net.active || !g_net.is_host || !g_net.transport || !g_net.transport->IsOpen()) return;
  uint8_t msg[6] = {kMsgPlayerHit, uint8_t(victim)};
  PutF32(msg + 2, damage);
  g_net.transport->Broadcast(msg, sizeof(msg), /*reliable=*/true);
  g_net.packets_sent++;
  if (REXCVAR_GET(debug_online_trace)) {
    REXLOG_INFO("OnlineTrace: TX kMsgPlayerHit for P{} ({:.1f}): an enemy hit them here", victim + 1, damage);
  }
}

bool HitByRemotePlayerOnEnemy() { return t_hit_depth > 0 && t_remote_hitter_on_enemy; }
bool HitByNonLocalOnThisPc() { return t_hit_depth > 0 && t_by != g_net.local_slot; }

void NoteRemotePlayerDamage(float delta) {
  if (t_hit_depth > 0 && t_remote_victim >= 0 && delta < 0.0f) t_victim_damage += delta;
}

void SetLocalSpinHolding(int player, bool holding) {
  std::lock_guard lock(g_net.mutex);
  if (g_net.active && player == g_net.local_slot) {
    g_net.local_spin_holding = holding;
  }
}

bool TakeRemoteSpin(int player) {
  if (player < 0 || player >= kMaxPlayers) return false;
  std::lock_guard lock(g_net.mutex);
  if (!g_net.active || player == g_net.local_slot) return false;
  RemoteSlot& rs = g_net.slots[player];
  if (!rs.pending_spin) return false;
  rs.pending_spin = false;
  return true;
}

bool RemoteSpinHolding(int player) {
  if (player < 0 || player >= kMaxPlayers) return false;
  std::lock_guard lock(g_net.mutex);
  if (!g_net.active || player == g_net.local_slot) return false;
  return g_net.slots[player].spin_holding;
}

void SetLocalReticle(int player, float x, float y) {
  std::lock_guard lock(g_net.mutex);
  if (!g_net.active || player != g_net.local_slot) return;
  if (!std::isfinite(x) || !std::isfinite(y)) return;
  g_net.has_local_reticle = true;
  g_net.local_reticle_x = x;
  g_net.local_reticle_y = y;
}

bool GetRemoteReticle(int player, float& out_x, float& out_y) {
  if (player < 0 || player >= kMaxPlayers) return false;
  std::lock_guard lock(g_net.mutex);
  if (!g_net.active || player == g_net.local_slot) return false;
  const RemoteSlot& rs = g_net.slots[player];
  if (!rs.connected || !rs.has_snapshot || rs.history.empty()) return false;
  const PlayerSnapshot& latest = rs.history.back();
  if (!(latest.flags & kFlagHasReticle)) return false;
  out_x = latest.reticle_x;
  out_y = latest.reticle_y;
  return true;
}

bool BlockRemoteMaskEntry(int player) {
  if (!g_net.active.load(std::memory_order_relaxed)) return false;
  if (player < 0 || player >= kMaxPlayers) return false;
  if (player == g_net.local_slot.load(std::memory_order_relaxed)) return false;
  std::lock_guard lock(g_net.mutex);
  if (!g_net.active || player == g_net.local_slot) return false;
  const RemoteSlot& rs = g_net.slots[player];
  if (!rs.connected) return false;
  // Masks by announcement (mask_sync.h): only when the owner's PC said so.
  if (mask_sync::On()) return !mask_sync::WantsMask(player);
  // Block mask entry on other PCs unless the owner's own PC has actually
  // transitioned into state 3 (entering mask) or 1 (mask).
  if (!rs.has_snapshot || rs.history.empty()) return true;
  const uint8_t remote_state = rs.history.back().coop_state;
  return (remote_state != 1 && remote_state != 3);
}

void NoteLocalGroundState(uint8_t* base, uint32_t physics_behaviour, bool on_ground) {
  if (!g_net.active.load(std::memory_order_relaxed)) return;
  const int slot = g_net.local_slot.load(std::memory_order_relaxed);
  if (slot < 0 || !more_players::InPlay() || more_players::Loading()) return;
  uint32_t actor = 0;
  if (!ReadGuestU32(base, physics_behaviour + kPhysicsActor, actor) || !actor) return;
  const uint32_t titan = more_players::TitanOfPlayer(slot);
  const uint32_t body = titan ? titan : more_players::CharacterOfPlayer(slot);
  if (actor != body) return;
  std::lock_guard lock(g_net.mutex);
  g_net.local_on_ground = on_ground;
}

int RemoteOnGroundOverride(uint8_t* base, uint32_t physics_behaviour) {
  if (!g_net.active.load(std::memory_order_relaxed)) return -1;
  const int local = g_net.local_slot.load(std::memory_order_relaxed);
  if (local < 0 || !more_players::InPlay() || more_players::Loading()) return -1;
  uint32_t actor = 0;
  if (!ReadGuestU32(base, physics_behaviour + kPhysicsActor, actor) || !actor) return -1;

  int remote_slot = -1;
  for (int p = 0; p < kMaxPlayers; ++p) {
    if (p == local) continue;
    const uint32_t titan = more_players::TitanOfPlayer(p);
    const uint32_t body = titan ? titan : more_players::CharacterOfPlayer(p);
    if (body == actor) {
      remote_slot = p;
      break;
    }
  }
  if (remote_slot < 0) return -1;

  std::lock_guard lock(g_net.mutex);
  const RemoteSlot& rs = g_net.slots[remote_slot];
  if (!rs.connected || !rs.has_snapshot || rs.history.empty()) return -1;
  if (ReadCoOpState(base, remote_slot) != 2 || rs.history.back().coop_state != 2) return -1;
  return (rs.history.back().flags & kFlagOnGround) ? 1 : 0;
}

void SendToHost(const uint8_t* data, size_t size, bool reliable) {
  std::lock_guard lock(g_net.mutex);
  if (!g_net.active || g_net.is_host || !g_net.transport || !g_net.transport->IsOpen()) return;
  g_net.transport->Send(g_net.host_peer, data, size, reliable);
  g_net.packets_sent++;
}

void BroadcastFromHost(const uint8_t* data, size_t size, bool reliable) {
  std::lock_guard lock(g_net.mutex);
  if (!g_net.active || !g_net.is_host || !g_net.transport || !g_net.transport->IsOpen()) return;
  g_net.transport->Broadcast(data, size, reliable);
  g_net.packets_sent++;
}

void SendToPeerFromHost(uint32_t peer, const uint8_t* data, size_t size, bool reliable) {
  std::lock_guard lock(g_net.mutex);
  if (!g_net.active || !g_net.is_host || !g_net.transport || !g_net.transport->IsOpen()) return;
  g_net.transport->Send(peer, data, size, reliable);
  g_net.packets_sent++;
}

bool DebugTraceOn() { return REXCVAR_GET(debug_online_trace); }

bool PuppetOnFoot(int p) {
  if (p < 0 || p >= kMaxPlayers || !more_players::InPlay() || more_players::Loading()) return false;
  // Their PC said "mask": the copy's own tree must be free to take the forced
  // switch's CoOpBecomeMask (a driven copy decides nothing; mask_sync.h).
  if (mask_sync::WantsMask(p)) return false;
  if (session_sync::InTransitionOrCutsceneGrace()) return false;
  uint8_t* base = rex::system::kernel_memory()->virtual_membase();
  std::lock_guard lock(g_net.mutex);
  if (!g_net.active || p == g_net.local_slot) return false;
  const RemoteSlot& rs = g_net.slots[p];
  if (!rs.connected || !rs.has_snapshot || rs.history.empty()) return false;
  const PlayerSnapshot& latest = rs.history.back();
  if (latest.coop_state != 2 || ReadCoOpState(base, p) != 2) return false;
  const int here = session_sync::CurrentLevelId(base);
  return latest.level_id < 0 || here < 0 || int(latest.level_id) == here;
}

std::string DebugStatus(uint8_t* base) {
  std::lock_guard lock(g_net.mutex);
  if (!g_net.active) return "online: off\n";
  const int ls = g_net.local_slot.load();
  const uint32_t cm = g_net.connected_mask.load();
  std::string out = fmt::format(
      "online: {} | local_slot={} (P{}) | roster=0x{:X} | rtt={:.1f}ms | tx={} rx={} | "
      "puppet_updates={}\n",
      g_net.is_host ? "HOST" : "CLIENT", ls, ls >= 0 ? ls + 1 : 0, cm, g_net.rtt_ms,
      g_net.packets_sent, g_net.packets_recv, g_net.remote_physics_applied);
  for (int p = 0; p < kMaxPlayers; ++p) {
    if (!(g_net.connected_mask & (1u << p)) && p != g_net.local_slot) continue;
    const uint32_t st = base ? ReadCoOpState(base, p) : 0;
    const RemoteSlot& rs = g_net.slots[p];
    if (p == g_net.local_slot) {
      out += fmt::format("  P{} [LOCAL]  coop_state={}\n", p + 1, st);
    } else if (rs.has_snapshot && !rs.history.empty()) {
      const auto& s = rs.history.back();
      out += fmt::format("  P{} [REMOTE] coop_state={} (remote={}) seq={} pos=({:.2f},{:.2f},{:.2f}) "
                         "yaw={:.2f}\n",
                         p + 1, st, s.coop_state, s.seq, s.pos[0], s.pos[1], s.pos[2], s.yaw);
    } else {
      out += fmt::format("  P{} [REMOTE] coop_state={} (no snapshot yet)\n", p + 1, st);
    }
  }
  out += world_sync::DebugStatus();
  out += enemy_input::DebugStatus();
  out += session_sync::DebugStatus() + "\n";
  return out;
}

}  // namespace net
