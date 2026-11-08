// =============================================================================
// ui/voodoo_card.h -- a voodoo doll turning on the message card
// =============================================================================
//
// WHAT: when the game's bottom bar says a voodoo doll was picked up ("07.
// Crash Grab / Optional mission partially complete / 1/13") and the bar's
// messages use the achievement card (setting message_bar_card,
// achievements.cpp), that doll's 3D model turns on the card's left, where an
// achievement's picture sits, standing out above the card's top edge like
// the dolls placed in the levels.
//
// The doll kinds: 7 templates in default.rcf, "Collectables:c_voodoo<Kind>",
// one per doll mission (achievements.cpp DollOf): 0 Whumpa, 1 Ice, 2 Nina,
// 3 Desert, 4 Junk, 5 Phantom, 6 Crash.
// =============================================================================
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace voodoo_card {

// Puts doll `doll` (0..6) on the card, drawn in place of the card's icon
// element `icon` (AchBarIcon). False = no doll (its kind's model isn't loaded
// in this level): the card's words take the picture's room. From
// achievements.cpp when a doll message comes up (game thread).
bool Show(PPCContext& ctx, uint8_t* base, int doll, uint32_t icon);
// No doll on the card (any other message, or the bar gone).
void Hide();
// The doll on the card, -1 = none.
int Showing();

// Every front-end frame (game thread): lets the doll's section go once the
// doll has left the card.
void FrontEndFrame(PPCContext& ctx, uint8_t* base);


}  // namespace voodoo_card
