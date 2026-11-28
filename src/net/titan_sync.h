// =============================================================================
// net/titan_sync.h -- ONLINE TITANS: THE OWNER SAYS WHAT HAPPENED (2026-11-28)
//
// History: first the owner's accepted RB / B presses were replayed (each copy
// decided what they meant: the same B got off on one PC and jacked on
// another); then (2026-11-27) the owner announced its STATE by titan net id
// and the copies were stepped toward it. That second version failed with
// titans that have NO net id (one ridden since a save load never was an enemy
// entity: "pocket yes (id 0)"): the copy couldn't match, fell back to "get
// off", whose B question JACKED a knocked-out titan nearby, the repair fought
// that, unlinked the rider mid-animation and the guest crashed (read of 0x34
// in sub_820CD6B8 under sub_8212EE90). 2026-11-28 two-PC test, reproducible.
//
// THE ORIGINAL (single-player watch of 0x825B0020 ridden + 0x825B0028 pocket):
// one RB with a titan pocketed = ONE swap, ridden and pocket trade places in
// one frame (sometimes with one frame of "nothing ridden" between). The
// pocket is a two-place stack shared by everyone (0x825A5474 / 0x825A5480):
// pocket = sub_8216DAC0 -> sub_8216D508 -> sub_8216D1D0 (pushes, sets +32);
// take out = sub_8216DB00 -> sub_8216D668 -> sub_8216D548 -> sub_8216D6B0
// (takes the older entry). The swap script does both, then mounts Crash.
//
// NOW: the owner's PC watches its own ridden / pocket values; once they held
// 2 frames (the original's 1-frame in-between doesn't count) it names WHAT
// HAPPENED, as steps: POCKET, TAKE OUT, SWAP, GET OFF, MOUNT <titan id> (a
// jack; jacking while riding = POCKET then MOUNT). Every other PC replays each
// step through the game's own question for it, checks the result, in order:
//   * POCKET / SWAP = the copy's RB-riding question answered yes once (ENEMY
//     map 6 / event 37); the game pockets with an empty pocket, swaps with a
//     full one, exactly as the owner's did (the shared pocket matches);
//   * TAKE OUT = the RB-on-foot question (PLAYER map 5 / event 14);
//   * GET OFF = the titan's own dismount question (lost_controller's
//     CheckInputEvent hook). A copy NEVER picks a titan to jack
//     (CheckOpportunity sub_82157058 answers "none" for other PCs' players),
//     so that B can only mean get off;
//   * MOUNT = the game's own mount of our copy of that titan (only titans
//     that were enemies have ids, and those are exactly the jacked ones).
// A step whose start doesn't fit (riding where the owner was on foot...) is
// skipped and logged; one that shows no result in 3 s is dropped. Then the
// LATE CHECK compares riding / not riding and pocket full / empty (never ids)
// after 4 s quiet and makes one step to fix it. session_sync's old titan
// repair (spawning copies, forced unlinks) stays off while this is on.
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
// [10] bit 0 rides a titan, bit 1 the pocket holds one, [11..14] u32 LE seq,
// [15] the step (0 none = state only, 1 pocket, 2 take out, 3 swap, 4 get off,
// 5 mount), [16..19] u32 LE the mounted titan's net id (mount).
constexpr uint8_t kMsgTitan = 19;
constexpr size_t kTitanSize = 20;

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
// Another PC's player (their Crash copy here): never picks a titan to jack.
bool IsOtherPcsPlayer(uint32_t actor);

}  // namespace net::titan_sync
