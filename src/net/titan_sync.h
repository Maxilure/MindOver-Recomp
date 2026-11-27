// =============================================================================
// net/titan_sync.h -- ONLINE TITANS BY OUTCOME, NOT BY PRESSES (2026-11-27)
//
// Before: pocketing (RB riding), taking a titan out (RB on foot), swapping
// (RB with one pocketed), getting off (B riding) and jacking the next titan
// while riding (B riding next to a knocked-out one) went to the other PCs as
// "their game accepted RB / B" (kMsgPocket). Each copy then heard that press
// and DECIDED for itself what it meant: with a knocked-out titan in reach on
// one PC and not on the other, the same B got off here and jacked there. A
// jack on foot reached the others only through session_sync's titan repair,
// 0.3 s or more later, and the repair spawned second titans when it misread.
//
// Now the owner's PC ANNOUNCES the result: which titan its player rides and
// which titan the pocket holds (host net ids, world_sync), on every change and
// every 2 s. The game itself changes these one step at a time (jacking B while
// riding A = A pocketed, then B mounted, frames apart; a swap = the ridden one
// pocketed, then the other taken out), so every action is a sequence of four
// steps the other PCs make with the game's own code:
//   * MOUNT a titan that isn't in our pocket = the game's own mount of our copy
//     of it (CActionSpawnPlayer's two messages, session_sync);
//   * TAKE OUT the titan our pocket holds = the copy's own RB-on-foot question
//     answered yes once (PLAYER map 5 / event 14): the game's take-out;
//   * POCKET the ridden titan (the owner's pocket now holds it) = the copy's
//     RB-riding question answered yes once (ENEMY map 6 / event 37);
//   * GET OFF (ridden titan neither ridden nor pocketed there) = the titan's
//     own dismount question answered (lost_controller's CheckInputEvent hook).
// Each answer is given only when it leads to the announced result; the riding
// B is never replayed (its two meanings were the ambiguity). The pocket is ONE
// slot for everyone, as in the original: the game object +32 (CActor*).
// session_sync's titan repair stays as the late fallback (spawn after 3 s).
// Found: who writes the pocket slot (console `who 0x825B0028`, save 10 on a
// Ratcicle): pocket = script method sub_8216DAC0 -> sub_8216D508 ->
// sub_8216D1D0 (stores the actor, sub_820B1D10); take out = sub_8216DB00 ->
// sub_8216D668 -> sub_8216D548 -> sub_8216D6B0 (clears it). Both run from the
// fight trees' Lua, i.e. from those two questions' answers.
// Switch: online_titan_events (default on; off = the old press replay).
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, online_titan_events);

struct PPCContext;

namespace net::titan_sync {

// Owner -> Host -> others (reliable, in the fight-tree states' queue):
// [0] 19, [1] slot, [2..5] u32 LE ridden titan's host net id (0 = none / not a
// synced entity), [6..9] u32 LE pocketed titan's net id (0 = empty / unknown),
// [10] bit 0 rides a titan, bit 1 the pocket holds one, [11..14] u32 LE seq.
constexpr uint8_t kMsgTitan = 19;
constexpr size_t kTitanSize = 15;

bool On();
// Game thread, once per frame (session_sync TickFrame).
void Tick(PPCContext& ctx, uint8_t* base);
// Network side: an owner's announcement.
void OnReceive(const uint8_t* data, size_t size);
// Matching player p's titans right now (a change in the last 1.5 s, or a step
// under way): session_sync's repair keeps its hands off.
bool Busy(int p);
// Player p's PC left / new session.
void Forget(int p);
void Reset();

}  // namespace net::titan_sync
