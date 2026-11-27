// =============================================================================
// net/world_sync.h -- Phase 2: host-authoritative enemies, titans & combat
// =============================================================================
//
// WHAT: extends Phase 1's player state sync to cover the shared world:
//   * Enemies & titans (host-authoritative):
//     - Host tracks every active non-player CDamageableBehaviour actor updated
//       each frame (sub_82132F88), identifies each by its Radical Engine name
//       hash (CActor + 8, findings/27 & 69 -- never a guest heap pointer), and
//       streams compact snapshots (~20 Hz, unreliable UDP): position, velocity,
//       facing yaw, and current hitpoints.
//     - Clients suppress local enemy AI (sub_820B8688 in cheats.cpp), smoothly
//       blend enemy/titan physics & facing before CPhysicsBehaviour::Update
//       (sub_8217D0A0), and apply host's authoritative HP changes through
//       __imp__sub_821349E0 so health bars, knockdowns, and deaths stay in sync.
//   * Combat / Damage RPCs (reliable):
//     - When a client hits an enemy or titan locally (sub_821349E0 with delta < 0),
//       the client suppresses the local HP mutation and sends a reliable
//       kMsgDamageReq(entity_hash, delta_hp) to the host.
//     - The host applies the damage authoritatively via __imp__sub_821349E0 and
//       broadcasts the updated HP in the next snapshot.
//   * Player HP & Death Sync:
//     - Each player's owner PC tracks their own HP; remote PCs mirror it via
//       __imp__sub_821349E0 so death animations and 5-second respawn timers
//       trigger together on every screen.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct PPCContext;

namespace net {

// Protocol message IDs handled by world_sync (dispatched from state_sync.cpp).
constexpr uint8_t kMsgWorldSnapshot = 10;  // Host -> Clients (unreliable @ 20 Hz)
constexpr uint8_t kMsgDamageReq = 11;      // Client -> Host (reliable): hit an enemy/titan
constexpr uint8_t kMsgCheatSpawn = 12;     // Any -> Host -> All (reliable): F5 cheat spawn

namespace world_sync {

// Reset all tracked actors and pending RPCs (called on session start / leave).
void Reset();

// Called from cheats/spawn.cpp when a player spawns an enemy/titan/pickup via
// F5 Cheats so every connected PC spawns the same actor(s) too.
void BroadcastCheatSpawn(const std::string& template_name, int player, int count, bool knocked_out);

// Called from CDamageableBehaviour::Update (sub_82132F88 in cheats.cpp) for
// every active damageable actor each frame (players, enemies, titans).
// On Host: records the actor for the next world snapshot and applies any
// queued client damage requests to it.
// On Client: records the actor's local address/hash and reconciles its HP
// toward the host's authoritative HP via __imp__sub_821349E0.
void OnDamageableUpdate(PPCContext& ctx, uint8_t* base, uint32_t dmg, uint32_t actor);

// Called from sub_821349E0 (CDamageableBehaviour::ChangeHitpoints) in cheats.cpp.
// Returns true if the local HP change should be SUPPRESSED (e.g. a client hit
// an enemy locally and forwarded a kMsgDamageReq to the host instead).
bool InterceptHitpointsChange(uint8_t* base, uint32_t dmg, float delta);

// Called from ground_physics.cpp around CPhysicsBehaviour::Update (sub_8217D0A0)
// for non-player actors on clients: smoothly blends enemy/titan positions &
// facing toward the host's interpolated snapshots.
void PrepareRemoteEntityPhysics(uint8_t* base, uint32_t physics_behaviour, float dt);
void FinishRemoteEntityPhysics(uint8_t* base, uint32_t physics_behaviour);

// Called once per frame from state_sync::TickNetworkLocked:
// On Host: serializes and broadcasts kMsgWorldSnapshot at ~20 Hz.
void TickHostBroadcast(uint8_t* base);

// Handle incoming kMsgWorldSnapshot (client) or kMsgDamageReq (host).
void HandlePacket(const uint8_t* data, size_t size, bool is_host);

// Host: true if `actor` is an enemy/titan in this PC's synced list (not a
// player's, not a prop) -- an attacker whose hits on a remote player count.
bool IsSyncedEnemy(uint32_t actor);

// online_guest_enemy_ai: enemies run their own AI on guests too (and are
// only reined in when they drift). Never while online_enemy_puppets is on.
bool GuestEnemyAi();
// online_enemy_puppets: guests' enemies are driven by the host's AI inputs.
bool EnemyPuppets();
// Enemies attack on guests' screens (either mode): each PC's enemies hurt
// that PC's own player, the host forwards nothing.
bool GuestEnemiesAttack();

// ENEMY INPUTS (net/enemy_input.cpp). Host: the synced enemy whose input
// object owns this value channel (its net_id, `index` 0-17), 0 = none.
uint32_t HostEnemyOfChannel(uint32_t channel, int& index);
// Host: the synced enemy owning this input object (its net_id), 0 = none.
uint32_t HostEnemyOfInput(uint32_t input);
// Guest: true if this input object is a bound (puppet) enemy's.
bool IsPuppetEnemyInput(uint32_t input);
// Guest: the input object of the enemy bound to net_id, 0 = none.
uint32_t PuppetInputOf(uint32_t net_id);
// Guest: true if this value channel belongs to a bound (puppet) enemy.
bool IsPuppetEnemyChannel(uint32_t channel);
// Guest: the address of channel `index` of the enemy bound to net_id, 0 = none.
uint32_t PuppetChannelOf(uint32_t net_id, int index);

// Append Phase 2 enemy/titan sync summary to the `online` debug console command.
std::string DebugStatus();

}  // namespace world_sync
}  // namespace net
