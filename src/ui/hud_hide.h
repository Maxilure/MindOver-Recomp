// =============================================================================
// ui/hud_hide.h -- HIDE THE HUD: all of it, on a key (F2) or from the Cheats
// menu
// =============================================================================
//
// WHAT (2026-10-29): with the HUD hidden the picture shows only the game
// world (and menus): no portraits or bars, no mojo count or multiplier, no
// combo meters, no "counter now" prompts, no player markers over heads, no
// hint / mission / announcement texts, no "Join Game" corners, no collectable
// counter. The message bar at the bottom stays: it tells the player the HUD
// is hidden, and a voodoo doll or an achievement still shows there.
//   * F2 (the bind bind_hide_hud) or Tools -> Hide / show the HUD: on / off,
//     with a card on the message bar ("HUD Hidden / Press F2 to show it
//     again"). NOT a cheat: achievements are untouched.
//   * The Cheats menu's "Hide the HUD": the same switch (opening that menu
//     already enabled cheats, cheats.h).
//
// WHY: the first version (findings/27) only skipped the HUD controller's
// draw (sub_8226A998: portraits, health and special bars); the mojo count,
// multiplier and combo meters stayed on screen.
//
// HOW: each piece is skipped AT ITS DRAW, so the game's own state (which
// page it shows, a meter counting hits) carries on untouched and comes back
// as it was:
//   * the HUD controller's draw sub_8226A998 (players/more_players_hud.cpp);
//   * the HUD screen's pages: Scrooby's Page draw is Parent@frontend's slot 6
//     sub_82371270 (shared by groups and layers: only `this` in our list of
//     pages is skipped): InGame (mojo count + multiplier, hints, mission
//     title, counter-button texts), InGame_Announcement,
//     InGame_CoOp_Player1-4, InGame_CollectableCounter. The list is found by
//     name once a second from the HUD's per-frame hook (pages come and go
//     with levels);
//   * slot 4 (draw) of the front end's per-player objects: CComboCounter
//     sub_8225CF30, CCounterOpportunityDisplay sub_8225E658,
//     CPlayerIdentifierArrow sub_8226D358 (players 3-4's copies go through
//     the same functions).
//   * also slot 4 of CStoredActorDisplay sub_8226E688 (the POCKETED mutant's
//     portrait + bars, apart from the HUD: it stayed in the first version),
//     CTimerDisplay sub_8226EBF8 (a timed challenge's clock) and
//     CHealthDisplay sub_822685F0 (in case anything but the HUD draws one).
// =============================================================================
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace hud_hide {

// F2 / the Tools button (UI thread): hides / shows the HUD, with a card on
// the message bar when hiding. Not a cheat.
void Toggle();

// From the HUD controller's per-frame hook (game thread): refreshes the list
// of HUD pages now and then.
void Frame(PPCContext& ctx, uint8_t* base);

}  // namespace hud_hide
