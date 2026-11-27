// =============================================================================
// net/world_sync.cpp -- Phase 2: host-authoritative enemies, titans & combat
// =============================================================================
//
// ENTITY IDENTITY (1-to-1 NetID + nearest-neighbor same-hash binding):
//   Multiple pooled enemies/titans/crates can share the exact same Radical
//   Engine name hash at CActor + 8 (measured on Wumpa Island: 28 active
//   damageables across 9 distinct hashes). To keep identical-template actors
//   from colliding:
//     * Host assigns each active non-player actor a monotonic u32 `net_id`
//       and sends (net_id, name_hash, pos, vel, yaw, hp) at 20 Hz.
//     * Client binds each new `net_id` 1-to-1 to the nearest unassigned local
//       actor with the same `name_hash`, locking `net_id <-> local_actor` for
//       the actor's lifetime (released automatically if the actor goes stale).
//
// GUEST OFFSETS USED (all verified in docs/findings/22, 27, 40, 55, 69):
//   CActor + 8              u32    actor name hash (template/placement hash)
//   CActor + 44 (+0x2C)     ptr    4x4 big-endian world matrix
//   CActor + 308 (+0x134)   ptr    CPhysicsBehaviour (vtable 0x82032FC4)
//     +88  actor, +104 vel[3], +140 pos[3], +0xDC stuck timer
//   CDamageableBehaviour (vtable 0x8202D2E4):
//     +64  ptr -> float hitpoints (big-endian)
//   __imp__sub_821349E0(r3 = dmg, f1 = delta_hp) -- Radical's hitpoint mutator
// =============================================================================
#include "world_sync.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../cheats/spawn.h"
#include "../guest_memory.h"
#include "../players/more_players.h"
#include "state_sync.h"

extern "C" REX_FUNC(__imp__sub_821349E0);  // CDamageableBehaviour: change hitpoints by f1

namespace net {
void SendToHost(const uint8_t* data, size_t size, bool reliable);
void BroadcastFromHost(const uint8_t* data, size_t size, bool reliable);
bool DebugTraceOn();

namespace world_sync {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kActorNameHash = 8;
constexpr uint32_t kActorMatrix = 44;
constexpr uint32_t kActorPhysics = 308;
constexpr uint32_t kPhysicsVtable = 0x82032FC4;
constexpr uint32_t kPhysicsActor = 88;
constexpr uint32_t kPhysicsVelocity = 104;
constexpr uint32_t kPhysicsPosition = 140;
constexpr uint32_t kPhysicsStuckTimer = 0xDC;
constexpr uint32_t kDmgHitpoints = 64;

constexpr int kWorldSendHz = 20;
constexpr size_t kMaxEntitiesPerPacket = 28;  // 28 * 36 + 6 = 1014 bytes (< 1200 MTU)
constexpr size_t kEntityWireBytes = 36;       // net_id(4)+hash(4)+pos(12)+vel(8,xz)+yaw(4)+hp(4)
constexpr float kSnapDistSq = 24.0f * 24.0f;  // smooth catch-up instead of snapping during titan sprints
constexpr float kBlendRate = 18.0f;
constexpr auto kStaleAfter = std::chrono::milliseconds(600);

thread_local bool g_applying_net_hp = false;
thread_local bool g_applying_net_spawn = false;

bool ReadU32(uint8_t* base, uint32_t addr, uint32_t& out) {
  if (!addr || !guest_memory::Readable(addr, 4)) return false;
  const uint8_t* p = GuestPtr(base, addr);
  out = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  return true;
}

bool ReadF32(uint8_t* base, uint32_t addr, float& out) {
  uint32_t bits = 0;
  if (!ReadU32(base, addr, bits)) return false;
  std::memcpy(&out, &bits, 4);
  return std::isfinite(out);
}

void WriteF32(uint8_t* base, uint32_t addr, float v) {
  if (!addr || !guest_memory::Readable(addr, 4) || !std::isfinite(v)) return;
  uint32_t bits = 0;
  std::memcpy(&bits, &v, 4);
  uint8_t* p = GuestPtr(base, addr);
  p[0] = uint8_t(bits >> 24);
  p[1] = uint8_t(bits >> 16);
  p[2] = uint8_t(bits >> 8);
  p[3] = uint8_t(bits);
}

void PutLE32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v & 0xFFu);
  p[1] = uint8_t((v >> 8) & 0xFFu);
  p[2] = uint8_t((v >> 16) & 0xFFu);
  p[3] = uint8_t((v >> 24) & 0xFFu);
}
void PutF32(uint8_t* p, float v) {
  uint32_t b = 0;
  std::memcpy(&b, &v, 4);
  PutLE32(p, b);
}
uint32_t GetLE32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
float GetF32(const uint8_t* p) {
  uint32_t b = GetLE32(p);
  float v = 0.0f;
  std::memcpy(&v, &b, 4);
  return v;
}

bool IsPlayerActor(uint32_t actor) {
  if (!actor) return false;
  for (int p = 0; p < kMaxPlayers; ++p) {
    if (actor == more_players::CharacterOfPlayer(p) || actor == more_players::TitanOfPlayer(p)) {
      return true;
    }
  }
  return false;
}

uint32_t FindPhysicsOf(uint8_t* base, uint32_t actor) {
  if (!actor) return 0;
  uint32_t phys = 0, vtbl = 0, owner = 0;
  if (ReadU32(base, actor + kActorPhysics, phys) && phys &&
      ReadU32(base, phys, vtbl) && vtbl == kPhysicsVtable &&
      ReadU32(base, phys + kPhysicsActor, owner) && owner == actor) {
    return phys;
  }
  return net::LookupCachedPhysics(base, actor);
}

bool ReadDamageableHP(uint8_t* base, uint32_t dmg, float& out_hp) {
  uint32_t hp_ptr = 0;
  if (!ReadU32(base, dmg + kDmgHitpoints, hp_ptr) || !hp_ptr) return false;
  return ReadF32(base, hp_ptr, out_hp);
}

struct LocalEntity {
  uint32_t net_id = 0;  // Host: assigned ID; Client: bound remote net_id (0 = unbound)
  uint32_t actor = 0;
  uint32_t dmg = 0;
  uint32_t hash = 0;
  float pos[3] = {0, 0, 0};
  float vel[3] = {0, 0, 0};
  float yaw = 0.0f;
  float hp = 0.0f;
  bool has_pos = false;
  Clock::time_point last_seen{};
};

struct HostEntityState {
  uint32_t net_id = 0;
  uint32_t hash = 0;
  uint32_t bound_actor = 0;  // Client: local guest CActor bound to this net_id (0 = unbound)
  float pos[3] = {0, 0, 0};
  float vel[2] = {0, 0};
  float yaw = 0.0f;
  float hp = 0.0f;
  Clock::time_point recv_time{};
};

struct PendingDamage {
  uint32_t net_id = 0;
  float delta = 0.0f;
};

struct WorldSyncState {
  std::mutex mutex;
  uint32_t next_net_id = 1;
  std::unordered_map<uint32_t, LocalEntity> local_by_actor;
  std::unordered_map<uint32_t, uint32_t> dmg_to_actor;
  // Client: authoritative entities from host, keyed by unique `net_id`.
  std::unordered_map<uint32_t, HostEntityState> remote_by_id;
  // Host: pending client damage requests keyed by `net_id`.
  std::vector<PendingDamage> pending_damage;
  // PLAYERS' TITANS (2026-11-27): actor -> last time it was ridden by a player
  // or sat in the pocket slot. Those are never synced as enemies (see
  // OnDamageableUpdate); a titan someone just left stays out for kOwnedGrace.
  std::unordered_map<uint32_t, Clock::time_point> player_titans;

  uint32_t snapshot_seq = 0;
  Clock::time_point last_broadcast{};
  uint64_t snapshots_sent = 0;
  uint64_t snapshots_recv = 0;
  uint64_t damage_reqs_sent = 0;
  uint64_t damage_reqs_applied = 0;
  uint64_t entity_physics_applied = 0;
};

WorldSyncState g_ws;

void ApplyHitpointDelta(PPCContext& ctx, uint8_t* base, uint32_t dmg, float delta) {
  if (!dmg || !std::isfinite(delta) || std::fabs(delta) < 1e-4f) return;
  const PPCContext saved = ctx;
  ctx.r3.u64 = dmg;
  ctx.f1.f64 = double(delta);
  g_applying_net_hp = true;
  __imp__sub_821349E0(ctx, base);
  g_applying_net_hp = false;
  ctx = saved;
}

// Client helper: bind `ent` (currently unbound) to the nearest unbound HostEntityState
// with the exact same `name_hash`. Called with g_ws.mutex held.
void TryBindClientEntityLocked(LocalEntity& ent, Clock::time_point now) {
  if (ent.net_id != 0 || !ent.has_pos) return;
  HostEntityState* best = nullptr;
  float best_dist_sq = std::numeric_limits<float>::max();
  for (auto& [id, hs] : g_ws.remote_by_id) {
    if (hs.bound_actor != 0 || hs.hash != ent.hash) continue;
    if (now - hs.recv_time > kStaleAfter) continue;
    const float dx = hs.pos[0] - ent.pos[0];
    const float dy = hs.pos[1] - ent.pos[1];
    const float dz = hs.pos[2] - ent.pos[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 < best_dist_sq) {
      best_dist_sq = d2;
      best = &hs;
    }
  }
  if (best) {
    best->bound_actor = ent.actor;
    ent.net_id = best->net_id;
  }
}

}  // namespace

void Reset() {
  std::lock_guard lock(g_ws.mutex);
  g_ws.next_net_id = 1;
  g_ws.local_by_actor.clear();
  g_ws.dmg_to_actor.clear();
  g_ws.remote_by_id.clear();
  g_ws.pending_damage.clear();
  g_ws.player_titans.clear();
  g_ws.snapshot_seq = 0;
  g_ws.last_broadcast = {};
  g_ws.snapshots_sent = 0;
  g_ws.snapshots_recv = 0;
  g_ws.damage_reqs_sent = 0;
  g_ws.damage_reqs_applied = 0;
  g_ws.entity_physics_applied = 0;
}

// A titan a player rode or pocketed in the last kOwnedGrace stays a PLAYER's
// titan here: pocketing, taking out, swapping and dismount/remount flicker it
// in and out of "ridden", and in between it must not join the enemy list.
constexpr auto kOwnedGrace = std::chrono::seconds(10);
constexpr uint32_t kPocketSlot = 0x825B0008 + 32;  // game object: THE pocketed titan (one slot)

void OnDamageableUpdate(PPCContext& ctx, uint8_t* base, uint32_t dmg, uint32_t actor) {
  if (!net::Active() || !more_players::InPlay() || more_players::Loading()) return;
  if (!dmg || !actor) return;
  // PLAYERS' TITANS ARE NOT ENEMIES (2026-11-27 playtest): when a player's
  // Ratcicle was left riderless for a moment (pocket, take-out), it entered
  // this list; the client bound its copy to the host's entity, steered it like
  // an enemy, and player 2's hits on it went to the host as enemy damage and
  // landed on player 1's own titan (70 -> 66). A player's titan is synced by
  // state_sync / session_sync (owner, position, health) and nowhere else.
  {
    const auto now = Clock::now();
    uint32_t pocketed = 0;
    ReadU32(base, kPocketSlot, pocketed);
    std::lock_guard lock(g_ws.mutex);
    const bool owned_now = IsPlayerActor(actor) || actor == pocketed;
    if (owned_now) g_ws.player_titans[actor] = now;
    const auto it = g_ws.player_titans.find(actor);
    if (it != g_ws.player_titans.end()) {
      if (owned_now || now - it->second < kOwnedGrace) {
        // Forget it as an entity, both ways (host: no snapshot, no damage
        // requests applied; client: unbound, never steered or intercepted).
        const auto e = g_ws.local_by_actor.find(actor);
        if (e != g_ws.local_by_actor.end()) {
          for (auto& [id, hs] : g_ws.remote_by_id) {
            if (hs.bound_actor == actor) hs.bound_actor = 0;
          }
          g_ws.local_by_actor.erase(e);
        }
        g_ws.dmg_to_actor.erase(dmg);
        return;
      }
      g_ws.player_titans.erase(it);  // left alone long enough: an ordinary titan again
    }
  }

  uint32_t hash = 0;
  if (!ReadU32(base, actor + kActorNameHash, hash) || !hash) return;

  float hp = 0.0f;
  if (!ReadDamageableHP(base, dmg, hp)) return;

  const auto now = Clock::now();
  LocalEntity sample;
  sample.actor = actor;
  sample.dmg = dmg;
  sample.hash = hash;
  sample.hp = hp;
  sample.last_seen = now;

  const uint32_t phys = FindPhysicsOf(base, actor);
  if (phys) {
    sample.has_pos = ReadF32(base, phys + kPhysicsPosition, sample.pos[0]) &&
                     ReadF32(base, phys + kPhysicsPosition + 4, sample.pos[1]) &&
                     ReadF32(base, phys + kPhysicsPosition + 8, sample.pos[2]);
    ReadF32(base, phys + kPhysicsVelocity, sample.vel[0]);
    ReadF32(base, phys + kPhysicsVelocity + 4, sample.vel[1]);
    ReadF32(base, phys + kPhysicsVelocity + 8, sample.vel[2]);
  }
  uint32_t mat = 0;
  if (ReadU32(base, actor + kActorMatrix, mat) && mat) {
    if (!sample.has_pos) {
      sample.has_pos = ReadF32(base, mat + 48, sample.pos[0]) &&
                       ReadF32(base, mat + 52, sample.pos[1]) &&
                       ReadF32(base, mat + 56, sample.pos[2]);
    }
    float fx = 0.0f, fz = 1.0f;
    if (ReadF32(base, mat + 32, fx) && ReadF32(base, mat + 40, fz)) {
      if (fx * fx + fz * fz > 1e-6f) sample.yaw = std::atan2(fx, fz);
    }
  }

  float hp_delta_to_apply = 0.0f;
  {
    std::lock_guard lock(g_ws.mutex);
    auto& ent = g_ws.local_by_actor[actor];
    const uint32_t prev_net_id = ent.net_id;
    ent = sample;
    ent.net_id = prev_net_id;
    g_ws.dmg_to_actor[dmg] = actor;

    if (net::IsHost()) {
      if (ent.net_id == 0) ent.net_id = g_ws.next_net_id++;
      for (auto it = g_ws.pending_damage.begin(); it != g_ws.pending_damage.end();) {
        if (it->net_id == ent.net_id) {
          hp_delta_to_apply += it->delta;
          g_ws.damage_reqs_applied++;
          it = g_ws.pending_damage.erase(it);
        } else {
          ++it;
        }
      }
    } else {
      TryBindClientEntityLocked(ent, now);
      if (ent.net_id != 0) {
        const auto it = g_ws.remote_by_id.find(ent.net_id);
        if (it != g_ws.remote_by_id.end() && now - it->second.recv_time < kStaleAfter) {
          const float diff = it->second.hp - hp;
          if (std::fabs(diff) > 0.25f) {
            hp_delta_to_apply = diff;
          }
        }
      }
    }
  }

  if (std::fabs(hp_delta_to_apply) > 1e-4f) {
    ApplyHitpointDelta(ctx, base, dmg, hp_delta_to_apply);
  }
}

bool IsSyncedEnemy(uint32_t actor) {
  if (!actor) return false;
  std::lock_guard lock(g_ws.mutex);
  return g_ws.local_by_actor.count(actor) != 0;
}

bool InterceptHitpointsChange(uint8_t* /*base*/, uint32_t dmg, float delta) {
  if (g_applying_net_hp || !net::Active() || net::IsHost()) return false;
  if (!more_players::InPlay() || more_players::Loading()) return false;

  uint32_t net_id = 0;
  {
    std::lock_guard lock(g_ws.mutex);
    const auto it_a = g_ws.dmg_to_actor.find(dmg);
    if (it_a == g_ws.dmg_to_actor.end()) return false;
    const auto it_e = g_ws.local_by_actor.find(it_a->second);
    if (it_e == g_ws.local_by_actor.end() || it_e->second.net_id == 0) return false;
    net_id = it_e->second.net_id;
  }

  if (delta < -1e-4f) {
    uint8_t msg[9];
    msg[0] = kMsgDamageReq;
    PutLE32(msg + 1, net_id);
    PutF32(msg + 5, delta);
    SendToHost(msg, sizeof(msg), /*reliable=*/true);
    {
      std::lock_guard lock(g_ws.mutex);
      g_ws.damage_reqs_sent++;
    }
    if (DebugTraceOn()) {
      REXLOG_INFO("OnlineTrace: TX kMsgDamageReq net_id={} delta={:.1f}", net_id, delta);
    }
  }
  return true;
}

void PrepareRemoteEntityPhysics(uint8_t* base, uint32_t physics_behaviour, float dt) {
  if (!net::Active() || net::IsHost() || !more_players::InPlay() || more_players::Loading()) {
    return;
  }
  uint32_t actor = 0;
  if (!ReadU32(base, physics_behaviour + kPhysicsActor, actor) || !actor) return;
  if (IsPlayerActor(actor)) return;

  HostEntityState target;
  {
    std::lock_guard lock(g_ws.mutex);
    const auto it_e = g_ws.local_by_actor.find(actor);
    if (it_e == g_ws.local_by_actor.end() || it_e->second.net_id == 0) return;
    const auto it_h = g_ws.remote_by_id.find(it_e->second.net_id);
    if (it_h == g_ws.remote_by_id.end()) return;
    if (Clock::now() - it_h->second.recv_time > kStaleAfter) return;
    target = it_h->second;
  }

  float cur_pos[3] = {0, 0, 0};
  const bool have_cur = ReadF32(base, physics_behaviour + kPhysicsPosition, cur_pos[0]) &&
                        ReadF32(base, physics_behaviour + kPhysicsPosition + 4, cur_pos[1]) &&
                        ReadF32(base, physics_behaviour + kPhysicsPosition + 8, cur_pos[2]);

  float new_pos[3] = {target.pos[0], target.pos[1], target.pos[2]};
  if (have_cur) {
    const float dx = target.pos[0] - cur_pos[0];
    const float dy = target.pos[1] - cur_pos[1];
    const float dz = target.pos[2] - cur_pos[2];
    const float dist_sq = dx * dx + dy * dy + dz * dz;
    if (dist_sq < kSnapDistSq) {
      const float clamped_dt = std::clamp(dt, 0.001f, 0.1f);
      const float dist = std::sqrt(dist_sq);
      const float adaptive_rate = kBlendRate + std::clamp((dist - 1.5f) * 1.2f, 0.0f, 18.0f);
      const float alpha = 1.0f - std::exp(-adaptive_rate * clamped_dt);
      new_pos[0] = cur_pos[0] + dx * alpha;
      new_pos[1] = (std::fabs(dy) < 0.35f) ? cur_pos[1] : (cur_pos[1] + dy * alpha);
      new_pos[2] = cur_pos[2] + dz * alpha;
    }
  }

  for (int i = 0; i < 3; ++i) {
    WriteF32(base, physics_behaviour + kPhysicsPosition + 4 * i, new_pos[i]);
  }
  WriteF32(base, physics_behaviour + kPhysicsVelocity, target.vel[0]);
  WriteF32(base, physics_behaviour + kPhysicsVelocity + 8, target.vel[1]);
  WriteF32(base, physics_behaviour + kPhysicsStuckTimer, 0.0f);

  std::lock_guard lock(g_ws.mutex);
  g_ws.entity_physics_applied++;
}

void FinishRemoteEntityPhysics(uint8_t* base, uint32_t physics_behaviour) {
  if (!net::Active() || net::IsHost() || !more_players::InPlay() || more_players::Loading()) {
    return;
  }
  uint32_t actor = 0;
  if (!ReadU32(base, physics_behaviour + kPhysicsActor, actor) || !actor) return;
  if (IsPlayerActor(actor)) return;

  float yaw = 0.0f;
  {
    std::lock_guard lock(g_ws.mutex);
    const auto it_e = g_ws.local_by_actor.find(actor);
    if (it_e == g_ws.local_by_actor.end() || it_e->second.net_id == 0) return;
    const auto it_h = g_ws.remote_by_id.find(it_e->second.net_id);
    if (it_h == g_ws.remote_by_id.end()) return;
    if (Clock::now() - it_h->second.recv_time > kStaleAfter) return;
    yaw = it_h->second.yaw;
  }

  WriteF32(base, physics_behaviour + kPhysicsStuckTimer, 0.0f);
  uint32_t mat = 0;
  if (ReadU32(base, actor + kActorMatrix, mat) && mat) {
    const float s = std::sin(yaw);
    const float c = std::cos(yaw);
    WriteF32(base, mat + 0, c);
    WriteF32(base, mat + 4, 0.0f);
    WriteF32(base, mat + 8, -s);
    WriteF32(base, mat + 32, s);
    WriteF32(base, mat + 36, 0.0f);
    WriteF32(base, mat + 40, c);
  }
}

void TickHostBroadcast(uint8_t* /*base*/) {
  if (!net::Active() || !net::IsHost() || !more_players::InPlay() || more_players::Loading()) {
    return;
  }
  const auto now = Clock::now();
  std::vector<LocalEntity> active;
  uint32_t seq = 0;
  {
    std::lock_guard lock(g_ws.mutex);
    if (now - g_ws.last_broadcast < std::chrono::milliseconds(1000 / kWorldSendHz)) return;
    g_ws.last_broadcast = now;

    for (auto it = g_ws.local_by_actor.begin(); it != g_ws.local_by_actor.end();) {
      if (now - it->second.last_seen > kStaleAfter) {
        g_ws.dmg_to_actor.erase(it->second.dmg);
        it = g_ws.local_by_actor.erase(it);
      } else {
        if (it->second.has_pos && it->second.net_id != 0) active.push_back(it->second);
        ++it;
      }
    }
    if (active.empty()) return;
    seq = ++g_ws.snapshot_seq;
    g_ws.snapshots_sent++;
  }

  for (size_t offset = 0; offset < active.size(); offset += kMaxEntitiesPerPacket) {
    const size_t count = std::min(kMaxEntitiesPerPacket, active.size() - offset);
    std::vector<uint8_t> pkt(6 + count * kEntityWireBytes);
    pkt[0] = kMsgWorldSnapshot;
    pkt[1] = uint8_t(count);
    PutLE32(pkt.data() + 2, seq);
    for (size_t i = 0; i < count; ++i) {
      const LocalEntity& e = active[offset + i];
      uint8_t* w = pkt.data() + 6 + i * kEntityWireBytes;
      PutLE32(w + 0, e.net_id);
      PutLE32(w + 4, e.hash);
      PutF32(w + 8, e.pos[0]);
      PutF32(w + 12, e.pos[1]);
      PutF32(w + 16, e.pos[2]);
      PutF32(w + 20, e.vel[0]);
      PutF32(w + 24, e.vel[2]);
      PutF32(w + 28, e.yaw);
      PutF32(w + 32, e.hp);
    }
    BroadcastFromHost(pkt.data(), pkt.size(), /*reliable=*/false);
  }
}

void HandlePacket(const uint8_t* data, size_t size, bool is_host) {
  if (!data || size == 0) return;
  const uint8_t type = data[0];

  if (type == kMsgWorldSnapshot && !is_host) {
    if (size < 6) return;
    const size_t count = data[1];
    if (size < 6 + count * kEntityWireBytes) return;
    const auto now = Clock::now();
    std::lock_guard lock(g_ws.mutex);
    g_ws.snapshots_recv++;
    for (size_t i = 0; i < count; ++i) {
      const uint8_t* r = data + 6 + i * kEntityWireBytes;
      const uint32_t net_id = GetLE32(r + 0);
      const uint32_t hash = GetLE32(r + 4);
      const float px = GetF32(r + 8), py = GetF32(r + 12), pz = GetF32(r + 16);
      const float vx = GetF32(r + 20), vz = GetF32(r + 24);
      const float yaw = GetF32(r + 28), hp = GetF32(r + 32);
      if (!net_id || !hash || !std::isfinite(px) || !std::isfinite(py) || !std::isfinite(pz) ||
          !std::isfinite(hp)) {
        continue;
      }
      auto& st = g_ws.remote_by_id[net_id];
      st.net_id = net_id;
      st.hash = hash;
      st.pos[0] = px;
      st.pos[1] = py;
      st.pos[2] = pz;
      st.vel[0] = std::isfinite(vx) ? vx : 0.0f;
      st.vel[1] = std::isfinite(vz) ? vz : 0.0f;
      st.yaw = std::isfinite(yaw) ? yaw : 0.0f;
      st.hp = hp;
      st.recv_time = now;
    }
    return;
  }

  if (type == kMsgDamageReq && is_host) {
    if (size < 9) return;
    const uint32_t net_id = GetLE32(data + 1);
    const float delta = GetF32(data + 5);
    if (!net_id || !std::isfinite(delta) || delta >= 0.0f) return;
    {
      std::lock_guard lock(g_ws.mutex);
      g_ws.pending_damage.push_back({net_id, delta});
    }
    if (DebugTraceOn()) {
      REXLOG_INFO("OnlineTrace: RX kMsgDamageReq net_id={} delta={:.1f}", net_id, delta);
    }
    return;
  }

  if (type == kMsgCheatSpawn) {
    if (size < 6) return;
    const int sender_slot = int(data[1]);
    const int player = int(data[2]);
    const int count = int(data[3]);
    const bool knocked_out = data[4] != 0;
    const size_t len = data[5];
    if (len == 0 || 6 + len > size) return;
    if (sender_slot == net::LocalSlot()) return;  // ignore our own relayed spawn
    const std::string tmpl(reinterpret_cast<const char*>(data + 6), len);
    if (is_host) {
      BroadcastFromHost(data, size, /*reliable=*/true);
    }
    REXLOG_INFO("Online: RX cheat spawn '{}' x{} (ko={}) at P{}", tmpl, count, knocked_out,
                player + 1);
    g_applying_net_spawn = true;
    spawn::Request(tmpl, player, count, knocked_out, nullptr, /*as_cheat=*/false);
    g_applying_net_spawn = false;
  }
}

void BroadcastCheatSpawn(const std::string& template_name, int player, int count,
                         bool knocked_out) {
  if (g_applying_net_spawn || !net::Active() || template_name.empty() ||
      template_name.size() > 200) {
    return;
  }
  std::vector<uint8_t> msg(6 + template_name.size());
  msg[0] = kMsgCheatSpawn;
  msg[1] = uint8_t(std::clamp(net::LocalSlot(), 0, kMaxPlayers - 1));
  msg[2] = uint8_t(std::clamp(player, 0, kMaxPlayers - 1));
  msg[3] = uint8_t(std::clamp(count, 1, 20));
  msg[4] = knocked_out ? 1 : 0;
  msg[5] = uint8_t(template_name.size());
  std::memcpy(msg.data() + 6, template_name.data(), template_name.size());
  if (net::IsHost()) {
    BroadcastFromHost(msg.data(), msg.size(), /*reliable=*/true);
  } else {
    SendToHost(msg.data(), msg.size(), /*reliable=*/true);
  }
  REXLOG_INFO("Online: TX cheat spawn '{}' x{} (ko={}) at P{}", template_name, count, knocked_out,
              player + 1);
}

// The `online` console command's enemy lines: every synced enemy/titan seen
// in the last second here, with its id, position and health (tests: double
// damage = the host's health drop vs the hits sent). Called with g_ws.mutex held.
std::string EnemyList() {
  std::string out;
  const auto now = Clock::now();
  for (const auto& [actor, e] : g_ws.local_by_actor) {
    if (now - e.last_seen > std::chrono::seconds(1)) continue;
    out += fmt::format("    enemy {:08X} net_id={} hash={:08X} hp={:.1f} at ({:.1f}, {:.1f}, {:.1f})\n", actor,
                       e.net_id, e.hash, e.hp, e.pos[0], e.pos[1], e.pos[2]);
  }
  return out;
}

std::string DebugStatus() {
  std::lock_guard lock(g_ws.mutex);
  size_t bound = 0;
  for (const auto& [id, hs] : g_ws.remote_by_id) {
    if (hs.bound_actor != 0) ++bound;
  }
  return fmt::format(
      "  world_sync: local={} remote={} bound={} | snap_tx={} snap_rx={} | "
      "dmg_tx={} dmg_applied={} | entity_phys={}\n",
      g_ws.local_by_actor.size(), g_ws.remote_by_id.size(), bound, g_ws.snapshots_sent,
      g_ws.snapshots_recv, g_ws.damage_reqs_sent, g_ws.damage_reqs_applied,
      g_ws.entity_physics_applied) + EnemyList();
}

}  // namespace world_sync
}  // namespace net
