// =============================================================================
// net/enemy_input.h -- online: guests' enemies are PUPPETS of the host's AI
// =============================================================================
//
// WHY (2026-11-27): with every PC running its own enemy AI, an enemy chose its
// own target and attacked at its own moment on each screen. Now only the HOST
// runs the AI; what the AI "presses" travels, and every guest's copy plays the
// same moves at (nearly) the same moment, at the same player, through the
// game's own move system. It is the way players already work online.
//
// HOW THE GAME DRIVES AN ENEMY (research 2026-11-27, Ghidra + RTTI):
//   * The AI = CAIManager / CAIDecisionTreeManager running CAITask kinds
//     (ApproachTargetActor, Attack, Block, Dodge, Flee, Guard, Idle,
//     ControllerInput, ...). Its per-frame update is sub_820B8688.
//   * The enemy's behaviour controller (actor +0x40) is an NPCBase (vtable
//     0x8203584C; NPCBaseJackable 0x820358C4 for titans). Every "was this
//     pressed?" of the enemy's move system (fight tree) is CheckInputEvent
//     sub_8221F1E8 = slot 23 (+0x5C, NPCBase: sub_821B9720 = `**(this+0xB4)`)
//     -> the enemy's own INPUT OBJECT -> sub_82272338, the very code a
//     player's gamepad goes through (findings/62).
//   * So the AI fills a virtual gamepad: the input object's 18 VALUE
//     CHANNELS (+0x2C, 12 B each), every value through ONE writer,
//     sub_82273328 (r3 = channel, f1 = value, r5 = game clock ms; it adds a
//     sample only when the value moved). The input maps turn the samples into
//     events every frame for every input object.
//
// WHAT WE DO:
//   * Host: our sub_82273328 wrapper notes every write to a synced enemy's
//     channel (net_id, channel 0-17, value); once a frame they go out as one
//     reliable kMsgEnemyInput.
//   * Guest (online_enemy_puppets on): the enemy AI still RUNS (with it off,
//     a copy never reached the states in which its move system asks "attack?":
//     replayed presses went unheard), but every move it presses or channel it
//     writes on a copy is dropped, and the host's values are written in order through the game's
//     own writer, at most one change per channel per frame (two changes in one
//     frame would share a time stamp: the writer then overwrites the sample,
//     and a short press would vanish). Position and facing still follow the
//     host's snapshots (world_sync).
//   * Hits: as before, a hit counts on the screen it lands on for that PC's
//     own player; enemy health stays the host's.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct PPCContext;

namespace net {

constexpr uint8_t kMsgEnemyInput = 13;  // Host -> guests (reliable): [1] count, then 7 B each

namespace enemy_input {

void Reset();
// Host: send the frame's channel writes (once per frame, from world_sync).
void FlushHost();
// Guest: a kMsgEnemyInput arrived (network thread).
void HandlePacket(const uint8_t* data, size_t size);
// Guest: write the host's presses and values into the puppets (game thread,
// from the AI update's place: cheats.cpp's sub_820B8688 wrapper).
void ApplyGuest(PPCContext& ctx, uint8_t* base);
std::string DebugStatus();
// Guest: the copy asks "was (map, event) pressed?" (CheckInputEvent). A press
// the host's AI made is replayed for one frame only, but a copy's move system
// asks at its own moments (2026-11-27: most replayed attacks were never
// heard). True = a replayed press of it is waiting (<= 0.4 s): answer yes;
// either way the waiting press is used up. heard_anyway = the game said yes
// itself (counts nothing extra).
bool TakeWaitingPress(uint32_t input, uint32_t map, uint32_t event, bool heard_anyway);

}  // namespace enemy_input
}  // namespace net
