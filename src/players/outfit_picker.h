// =============================================================================
// players/outfit_picker.h -- the co-op outfit picker at the chest (players 1-4)
// =============================================================================
//
// THE ORIGINAL (findings/46): the chest in Crash's house opens the front
// end's SkinSelectionScreen (fight-tree state 847, CSkinScreenAction): a
// full-screen menu (the purple frame and blue swirl = CNVBackgroundAction)
// with ONE player's page at a time, one outfit at a time, a 3D preview.
//
// OURS (2026-10-23): every joined player picks at the same time, each on a
// CARD of their own (2-4 cards in a row, centred; 1 player = the game's page
// re-arranged), with three choices: Character (Crash / Coco), Outfit (that
// character's looks) and Colour (the coloured Crashes, on his Normal look).
// See outfit_picker.cpp.
// =============================================================================
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace outfit_picker {

// Registers the data patches (call before the game runs: CrashMomApp).
void Register();

// The picker is on (setting outfit_picker): the chest's list then also holds
// the coloured looks (outfits.cpp BuildList), which only the picker offers.
bool Enabled();

// Right after the skin screen's own Update (outfits.cpp's wrapper of
// sub_820E0858): opens the picker, reads every player, draws the panels.
void AfterUpdate(PPCContext& ctx, uint8_t* base, uint32_t screen);

// The game's view of controller `user` (its XInputGetState wrapper,
// saves/save_library.cpp; `pad` = the 12-byte gamepad, guest byte order):
// quiet while the picker is open, and after it closes until that
// controller's buttons are let go (the button that closed it would reach the
// game as a fresh press / release).
void FilterPad(uint32_t user, uint8_t* pad);

// The chest's screen is up, or the picker closed less than a second ago:
// nobody turns into a mask then (more_players.cpp BelongsToAMenu)
// (B released = "turn into a mask" with a partner on screen; the game's own
// chest screen, closed with B in a two-player game, left that player a
// mask). Asked by players/more_players.cpp (sub_82235CB8).
bool JustClosed();

// The picker is open right now (from its first frame until it closes; any
// thread). A menu for the keyboard (menu_input::MenuOnScreen): a Crash <->
// Coco pick respawns the players while the picker stays open, and the
// respawn clears the front end's "a menu is on screen" bit (+8594 0x80), so
// the keys went back to their play jobs and a keyboard player could no
// longer Wear (Enter) or keep (Escape): a soft lock (2026-11-01: player 1 on
// keys browsing Coco, player 4 picked Old Skool Coco).
bool IsOpen();

}  // namespace outfit_picker
