// =============================================================================
// net/mask_sync.h -- ONLINE MASKS BY ANNOUNCEMENT, NOT BY B (2026-11-27)
//
// Before: turning into a mask (and back) online was the OWNER'S B BUTTON fed
// into the other PCs' copy of that player. Each copy then decided for itself
// whether that B meant "mask" (or a jack, a totem, a villager...), held back by
// a gate that waited for the owner's position update to say "mask", plus FAKE B
// PULSES every 16 frames when the two PCs still disagreed. In busy moments
// (a late or lost update, B next to a titan or a totem, a mask host that
// differs between PCs) the copies did other things than their owners.
//
// Now the owner's PC ANNOUNCES each change of its player's co-op state (one
// small reliable message: "P2 is entering a mask / is a mask on player N / is
// on foot again"), and the other PCs make their copy follow with the game's OWN
// forced switches, the ones dig spots and Drop Out already use:
//   * into a mask = sub-state 5 (BEING_FORCED_INTO_MASK): the copy's fight tree
//     takes CoOpBecomeMask by itself (sub_820C0EF0, findings/26 s.31 / s.63);
//     the host it rides = the owner's host (more_players' "other player"
//     sub_82235B10 asks WantedHost);
//   * back on foot = sub-state 4 (forced out of the mask: what Drop Out does to
//     its riders, findings/26 s.60).
// B is no longer fed to other players' copies at all (state_sync), and a
// copy turns into a mask ONLY when its owner said so (BlockRemoteMaskEntry).
// The owner's mask fight-tree states (995 enter, 999 mask, 1037 leave; Crash.bfig)
// are not forced on copies any more (action_sync skips them): the forced
// switch plays them itself. Found by tracing a mask round trip on two PCs:
// notes (private) online-coop.md "MASKS BY ANNOUNCEMENT".
// Switch: online_mask_events (default on; off = the old B replay).
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, online_mask_events);

struct PPCContext;

namespace net::mask_sync {

// Owner -> Host -> others (reliable, in the same queue as the fight-tree
// states so the order holds): [0] 18, [1] slot, [2] the owner's co-op state now
// (1 mask, 2 on foot, 3 entering a mask), [3] the player whose Crash carries the
// mask (0xFF = not known yet: at "entering" the host isn't attached),
// [4..7] u32 LE sequence (per owner).
constexpr uint8_t kMsgMask = 18;
constexpr size_t kMaskSize = 8;

bool On();
// Game thread, once per frame (session_sync TickFrame): send our player's
// changes, make the other players' copies follow theirs.
void Tick(PPCContext& ctx, uint8_t* base);
// Network side: a message from another player's PC.
void OnReceive(const uint8_t* data, size_t size);
// Player p's owner says they are (turning into) a mask.
bool WantsMask(int p);
// The player whose Crash p's mask should ride (owner's), -1 = any.
int WantedHost(int p);
// Fight-tree states of the mask round trip, played by the forced switch
// on copies (never forced by action_sync).
bool IsMaskTransitionNode(int node);
// Player p's PC left: their announcement no longer holds (the copy is
// neither kept a mask nor kept from one by it).
void Forget(int p);
// Connection gone / new session: forget everything.
void Reset();

}  // namespace net::mask_sync
