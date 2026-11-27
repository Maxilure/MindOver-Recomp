// =============================================================================
// net/action_sync.h -- ONLINE PLAYERS BY ACTIONS, NOT BUTTONS (2026-11-27)
//
// Before: every PC fed the other players' BUTTONS to its copy of their Crash,
// and each copy decided for itself what those buttons meant (which enemy an
// attack homes on, whether a jump continues, ...). Copies parted ways: attacks
// pointed elsewhere, a copy kept jumping while its owner had died.
//
// Now: the OWNER's PC sends every fight-tree STATE its player's body enters
// (one small reliable message per state: Jump, AttackHeavyAWindup,
// AttackHeavyA, SpinAttack, GetHit..., Dead, ...), and the other PCs put their
// copy into that same state with the game's own "go to node" (sub_820C0DF8,
// what scripts use). The copy then makes NO decisions of its own: its tree's
// decision is switched off (tree +0x9C bit 0x80, the game's own "no decision"
// flag) and any other state change is refused. So:
//   * chains (heavy A -> B -> C, light A -> B -> C) = each step is its own
//     state, sent when the owner takes it;
//   * HELD moves (a heavy windup charged as long as the button is held) = the
//     copy stays in the windup until the owner's next state arrives;
//   * spins (simple-spin presses, repeated spin presses, stick circles that
//     keep spinning) = every ENTRY is sent, a re-entry of the same state too
//     (sequence number), so the copy spins exactly as often.
// Position, facing and health keep coming from the snapshots (state_sync).
// Only while the player is on foot / riding on BOTH PCs, in the same area,
// outside cutscenes (masks, drop-outs and cutscenes keep the old way).
// Found + tested: notes (private) online-coop.md s.11; probe debug/tree_probe.cpp.
// Switch: online_action_sync (default on).
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, online_action_sync);

namespace net::action_sync {

constexpr uint8_t kMsgTreeState = 14;  // Owner -> Host -> others (reliable):
// [0] 14, [1] slot, [2] body (0 = character, 1 = titan), [3] how: 0 = sub_820C1578,
//   1 = sub_820C1BB8 (a MASTER tree changing with its slave: a ridden titan's tree
//   carries the rider's tree at +0x98 and moves both at once),
// [4..7] u32 LE sequence (per owner, +1 per state entry), [8..9] u16 LE state node,
// [10..11] u16 LE VARIANT node (tree +0x4C, 0 = none), [12..13] u16 LE EXIT taken (0 = none),
// [14..15] u16 LE second variant (tree +0x50, = the slave's variant; 0 = none).
// Variant + exit (2026-11-27 playtest round 2): a state picks its variant by a
// compiled test of the character's situation (tree +0x2C via sub_820C0ED8): the
// copy, put in "Jump" from wherever it stood, picked the LEDGE jump; the exit's
// own actions (blend time, effects) ran only on the owner (spin effects missing).
constexpr size_t kTreeStateSize = 16;

// ENEMIES (online_enemy_states, world_sync.h): host -> guests (reliable):
// [0] 17, [1..4] u32 LE host net id, [5..8] u32 LE sequence (host-wide),
// [9..16] state, variant, exit, second variant (u16 LE each), [17] how (as [3] above).
// Guests' enemy AI is off (cheats.cpp sub_820B8688); each enemy copy is put
// into the host's states and makes no decisions (like a player's copy).
constexpr uint8_t kMsgEnemyState = 17;
constexpr size_t kEnemyStateSize = 18;
void OnReceiveEnemy(const uint8_t* data, size_t size);

// --- game thread (fight tree hooks) -----------------------------------------
// After a state change (sub_820C1578): our player's body -> queue a message.
void OnStateEntered(uint32_t tree, uint32_t exit, bool with_slave = false);
// Before a state change: true = a copy we drive refuses it (we didn't ask).
bool BlockStateChange(uint32_t tree);

// --- network side (state_sync, under its lock) --------------------------------
// Queue another reliable message in OUR order (mask_sync: a co-op state
// change must reach the others in order with the fight-tree states around it).
void QueueMessage(std::vector<uint8_t> msg);
// Messages waiting to be sent (moved out).
std::vector<std::vector<uint8_t>> TakeOutgoing();
// A message for a remote player's copy.
void OnReceive(const uint8_t* data, size_t size);
// Let player p's copy make its OWN decisions for `ms` (an accepted pocket
// press: the game's pocket / take-out runs through the copy's decision on the
// replayed press, state_sync kMsgPocket). Not driven meanwhile.
void AllowOwnDecisions(int p, int ms);
// Let an ENEMY copy (actor) make its own decisions for `ms`: OUR player is
// jacking it (B on foot at the game's jack target). Being jacked is the titan's
// own decision; a copy driven by the host's states made none, so the jack
// started and aborted (2026-11-27 playtest: re-jacking a titan just got off,
// which the host already drives as an enemy again).
void FreeEnemyCopy(uint32_t actor, int ms);
// Connection gone / new session: forget everything.
void Reset();

}  // namespace net::action_sync
