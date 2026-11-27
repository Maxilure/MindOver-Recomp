// =============================================================================
// net/aim_sync.h -- ONLINE MASK AIMING IN WORLD SPACE (2026-11-27)
//
// A mask (and a jacked titan's aim) shoots along a ray: the aiming behaviour's
// Update (sub_822AC668) reads its player's crosshair spot on the 640 x 480
// canvas (sub_822AD0A0), turns it into a WORLD segment through THIS PC's camera
// (sub_8224C6C0 -> sub_82265C80 unproject: near and far point) and casts it
// (sub_8224C7F8: r3 world, r4 start, r5 end, r6 query, r7 result; result +0 =
// the hit's fraction of the segment, return = hit). The hit spot (result +16)
// is what the mask turns to and fires at.
// Online, before: the owner's crosshair SPOT was sent (state_sync reticle x/y)
// and re-cast through the other PC's own camera = a different world point
// (every PC has its own camera), so the copy aimed somewhere else. And with
// mouse aiming off on the receiving PC it wasn't applied at all.
// Now: the owner sends the world SEGMENT its aim cast used; the other PCs cast
// that same segment through their own scenery/targets for that player's aim.
// Same world line on every PC = the same target.
// Message kMsgAim (unreliable, each frame the owner's aim runs):
//   [0] 16, [1] slot, [2..25] 6 x f32 LE: start xyz, end xyz.
// Used for 0.3 s after it arrives; older = the copy's own (old) aim.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <rex/ppc/context.h>

namespace net::aim_sync {

constexpr uint8_t kMsgAim = 16;
constexpr size_t kAimSize = 26;

// From the aim ray wrapper (ui/wii_cursor.cpp sub_8224C6C0), FIRST: true =
// handled here (our player's cast captured + queued, or a remote player's cast
// done with its owner's segment); false = run the original as before.
bool HandleAimRay(PPCContext& ctx, uint8_t* base);

// Is a remote player's own aim running right now (a segment from their PC
// within the last 0.3 s)? Their aim runs exactly while their crosshair is
// shown there: a copy's crosshair is shown / hidden by this (reticle_idle).
bool RemoteAimFresh(int p);

// Network side (state_sync, under its lock).
std::vector<std::vector<uint8_t>> TakeOutgoing();
void OnReceive(const uint8_t* data, size_t size);
void Reset();

}  // namespace net::aim_sync
