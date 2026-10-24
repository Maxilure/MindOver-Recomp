// =============================================================================
// players/outfits.h -- every player picks an outfit at the chest (players 1-4)
// =============================================================================
//
// THE GAME (docs/findings/46): the chest in Crash's house (L0_Skins) opens the
// SKIN SCREEN (CSkinScreenAction, a carousel: one outfit at a time with a 3D
// preview, name and description, "Wear Skin"). An outfit = a SKIN number:
//   0 Crash, 1-3 Crash Greeny / Bluey / Reddy (coloured pants and hair tips),
//   4 Snipe, 5 Ratcicle, 6 Magmadon, 7 Skeleton, 8 Spike, 9 Coco, 10 Coco's
//   "Old School"
// each its own character template ("Characters:" + name, table 0x824F3A30)
// with its own package. Per player the game keeps ONE number (2-entry table
// 0x8259B1C0, get sub_8229F8B8 / set sub_8229F8A0). Player 1's page lists 0-8;
// player 2's lists 0 (as CARBON CRASH), 9, 10: Carbon Crash is NOT a model, it
// is Crash's model with the second texture set (Crash_Player2.bmp, the white
// fur), which the game gives every Crash whose player isn't player 1. With two
// players on foot, Back = "Goto Player 2/1" turns the page.
//
// THE PORT:
//   * every player (1-4) has their own outfit, and every page offers ALL of
//     them: Crash, Carbon Crash, Greeny, Bluey, Reddy, Snipe, Ratcicle,
//     Magmadon, Skeleton, Spike, Coco, Old School (locked ones stay locked);
//   * CARBON is an outfit of its own (our number 11, kCarbon): Crash's model
//     with the second texture set, for any player; plain orange Crash is
//     there for players 2-4 too. Crash also comes in six colours (12-17:
//     red, blue, green, purple, yellow, pink, after the many-coloured
//     Crashes of the Crash of the Titans short "Have Another"), for anyone.
//     The texture set is picked per character at draw time
//     (CCharacterRenderBehaviour's draw, +0xE4); the HAIR (its own material,
//     shared by every Crash) follows the outfit too;
//   * Back pages through every player on foot (1 -> 2 -> 3 -> 4 -> 1);
//   * a picked outfit is kept per player AND PER SAVE
//     (in the save file's trailer; a save without picks wears what the game
//     decides: player 1 orange Crash, players 2-4 Carbon Crash or the
//     story's Coco; outfit_player1..4 in settings.toml are retired). Saves keep the game's
//     own two numbers unchanged.
// =============================================================================
#pragma once

#include <array>
#include <cstdint>

#include <rex/cvar.h>

REXCVAR_DECLARE(int32_t, outfit_player1);
REXCVAR_DECLARE(int32_t, outfit_player2);
REXCVAR_DECLARE(int32_t, outfit_player3);
REXCVAR_DECLARE(int32_t, outfit_player4);

namespace outfits {

// Our numbers after the game's 0-10 (Crash's model, toward the game skin 0):
// Carbon Crash (the original white), then Crash in six colours. 12 was an
// orange Carbon until 2026-10-17 (a settings file with 12 now gets red).
constexpr int kCarbon = 11;
constexpr int kCrashRed = 12, kCrashBlue = 13, kCrashGreen = 14, kCrashPurple = 15, kCrashYellow = 16,
              kCrashPink = 17;
constexpr int kFirstColour = kCrashRed, kLastOutfit = kCrashPink;
// Carbon Crash and the coloured Crashes: Crash's model wearing the SECOND
// texture set (the game's Carbon slot), which names the outfit's texture.
inline bool IsCarbon(int outfit) { return outfit >= kCarbon && outfit <= kLastOutfit; }

// Registers the data patch that adds the coloured Crashes (and every outfit's
// hair) to Crash's package, and the hair's draw hook (call before the game
// runs: CrashMomApp).
void Register();

// The coloured Crashes in the chest's order (0-5), for the spin effects
// (players/spin_colours.h): an outfit's index (-1 = not a coloured Crash),
// how many there are, and one's fur hue (degrees).
int ColourIndex(int outfit);
int ColourCount();
float ColourHue(int index);

// Player p's outfit as the port sees it (0-10 the game's skins, 11 Carbon, 12-17 colours).
int OutfitOf(int p);

// FOR THE CO-OP PICKER (players/outfit_picker.h), which drives the chest's
// screen: where an outfit is in the screen's list (-1 = not there: colours
// off), whether the loaded save has it, the outfit the screen shows now (its
// preview) and the address of that entry's template name key (8 bytes: the
// inventory section to wait for), and Wear = the screen's "Wear Skin" for
// player p with the shown outfit (ours, see outfits.cpp section 5).
int ListIndexOf(int outfit);
bool OutfitAvailable(int outfit);
int ShownOutfit();
uint32_t ShownTemplateKey();

// PICKS PER SAVE (2026-10-22), kept in the save file itself (a trailer after
// the game's bytes, see outfits.cpp and saves/save_files.h). Called by the
// save list: a save was loaded / a New Game starts. CurrentPickBytes = what
// the PC save drive writes into the trailer of every save file it writes
// (one signed byte per player, -1 = no pick).
void OnSaveLoaded(int number);
void OnNewGame();
std::array<int8_t, 4> CurrentPickBytes();

// A Crash <-> Coco pick is respawning the players: the chest's previews
// (the screen's own and outfit_previews') may not be built yet (section 5b of
// outfits.cpp). Turns false once the players are made again.
bool PreviewHeld();

}  // namespace outfits
