// =============================================================================
// players/players_setup.h -- the PLAYERS screen's workings: who plays with
// which device, press anything to join, each player's button pictures
// =============================================================================
//
// WHAT (2026-10-29): a screen in the game's own look (the Options screen's
// third mode, options/options_menu.cpp; reached from the main menu and the
// pause menu: options_page.cpp adds the "Players" items) with four boxes,
// one per player. This file is what the boxes MEAN; options_menu.cpp draws
// them.
//
// THE RULES:
//   * PLAYER 1 ALWAYS HAS THE KEYBOARD AND MOUSE, and may have a controller
//     as well (both work at once, input/keyboard_mouse.h merges them; the
//     button pictures follow whichever was used last). Nothing here moves
//     the keyboard, and a controller moved onto player 1 never pushes the
//     keyboard away (input/players.h SetChoice).
//   * PRESS ANY BUTTON TO JOIN: a controller that plays as nobody (Off, or a
//     player number the levels have no room for) and presses any button or
//     trigger while the screen is up takes the first player of 2, 3, 4
//     (--local_players permitting) that has no device. (The startup rule is
//     unchanged: with nothing chosen, the first controller is player 1's, so
//     one person on a controller plays at once.) Such a controller answers
//     on no socket, so the game never sees it: its state is asked for
//     directly (players.h ProbeState).
//   * FIRST COME, FIRST SERVED (2026-10-30, replacing a chooser where A on a
//     card picked that player's controller from a list): a controller takes
//     a player only by pressing a button (above). Nobody picks devices for
//     anyone; the cards just show who has what.
//   * X = MOVE (the "pick up and drop" of the first version, back): the
//     selected card's controller is picked up (its ring in the player's
//     colour), the D-pad / stick / mouse point at another card, A drops it
//     there. An empty card = the controller is that player now; a card with
//     a controller = the two SWAP (their button pictures go with them). B
//     (or X again) puts it back. Player 1's
//     keyboard never moves (a controller moved onto player 1 joins it).
//   * Y / Backspace = REMOVE the player's controller(s) (nobody: any
//     controller pressing a button then takes the free player, the one
//     removed included). In a level a player left without a device drops
//     out at once (the pause menu's own Drop Out; lost_controller.h
//     DropOut); the last player in the game keeps theirs (refused). A move
//     follows the same rule.
//   * Each card shows its controller as Xbox / PlayStation 5 / PlayStation 4
//     / Switch by the maker and model (prompt_icons.h DetectedPadStyle), the
//     same detection that picks each player's button pictures. A on a card
//     with a controller = its BUTTON PICTURES (PadIcons below): left / right
//     step the styles while the card shows that style's A B X Y (the face
//     buttons' pictures); A keeps the one shown, B goes back to the old
//     one. The card then names it ("PlayStation 5"; "Automatic" = by the
//     controller's maker).
//   * JOINING THE GAME is the players' own: a controller given to player 3
//     here doesn't put player 3 in the game; they join with the game's own
//     button (START / B in play), as before. (A first version answered the
//     game's "START pressed?" for them once play resumed: not wanted.)
// EVERYTHING IS GLOBAL (one setup for every save): controls.toml's
// `player.<device>` lines (players.h).
// =============================================================================
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct PPCContext;

namespace players_setup {

constexpr int kPlayers = 4;

// The players' colours (the screen's design, 2026-10-29: red, blue, green,
// purple; 0xAARRGGBB).
constexpr uint32_t kPlayerColour[kPlayers] = {0xFFFF5A3C, 0xFF3BA7FF, 0xFF3CD23C, 0xFFB45AF0};

// The device pictures, in the frame order of each card's picture element
// (options_page.cpp; PictureFrames): Xelu's keyboard and mouse as drawn, his
// controller outlines (CC0, no logos: assets/prompts/controllers/) FILLED
// in that card's player colour with a navy edge, and a faint controller for
// an empty card (baked by tools/make_player_pictures.py into
// assets/prompts/controllers/players/).
enum Picture { kPicKeyboard, kPicXbox, kPicPs5, kPicPs4, kPicSwitch, kPicEmpty, kPictureCount };

struct ShownDevice {
  std::string key, name;  // "pad:<guid>", SDL's name ("PS5 Controller")
  bool keyboard = false;
  int picture = kPicXbox;
};

enum class State {
  kMenus,        // not in a level (main menu): who plays is all there is to say
  kPlaying,      // in the game
  kNotPlaying,   // has a device but isn't in the game (START joins)
  kEmpty,        // no device: press anything to join
  kUnavailable,  // levels have no room (--local_players)
};

struct Box {
  State state = State::kEmpty;
  std::vector<ShownDevice> devices;  // the keyboard first, then controllers
  bool has_pad = false;
  // The first controller's kind (DetectedPadStyle: 1 Xbox 360 / unknown,
  // 2 Xbox Series, 3 PlayStation 5, 4 PlayStation 4, 5 Switch), 0 = none.
  int detected_style = 0;
};
std::array<Box, kPlayers> Boxes();

// "Xbox 360", "Xbox Series", "PlayStation 5", "PlayStation 4", "Switch"
// (style numbers as --controller_icons; 0 = "Automatic").
const char* StyleName(int style);

// EACH PLAYER'S BUTTON PICTURES (2026-10-29; since 2026-10-30 A on a card,
// with a preview): the Controller Icons style, numbered as StyleName (0 Automatic =
// by the controller's maker). Player 1's IS the Options row Controller Icons
// (flag controller_icons, saved to user/settings.toml); players 2-4's are
// controls.toml's icons.player2-4 (input/bindings.h). Game main thread.
int PadIcons(int p);
void SetPadIcons(int p, int style);

// The Options screen opens as the Players screen next (the main / pause
// menu's "Players" was picked); its Enter takes the request.
void OpenNext();
bool TakeOpenNext();
bool OpenNextPending();

// The screen opened / closed (the game main thread). Opened: the buttons
// held right now don't join anyone (a press needs letting go first).
void Opened();
void Closed();

// Every update while the screen is up: a new press on a controller that
// plays as nobody joins it (rules above). Returns the player it joined as
// (-1 none), with that controller's buttons in *buttons (the screen hides
// them from its own reading: the press that joined isn't also an A / B).
int PollJoins(PPCContext& ctx, uint8_t* base, uint16_t* buttons);
// Is the Players screen up? (Its own update polls the joins then.)
bool ScreenOpen();
// IN A LEVEL, OUTSIDE THE SCREEN (2026-10-29): every frame, a controller that
// plays as nobody pressing any button takes the first free player of 2-4
// too (PollJoins), so its own "Join Game" corner (with its button) shows;
// the press itself is hidden from the game until let go (it doesn't join
// them: they press START / B for that, as always). From the HUD's per-frame
// update (players/more_players_hud.cpp). Game main thread.
void PollJoinsInPlay(PPCContext& ctx, uint8_t* base);
// Does player p (1-3) have a controller (a device that plays as them)? The
// co-op corners show "Join Game" only for those (input/prompt_icons.cpp).
bool HasController(int p);

// THE PRESS INDICATOR (2026-10-29): several controllers may share a name
// ("Xbox Controller"), so a card shows when ITS device presses something.
// Every update while the screen is up: a bit per card (1 << player) whose
// device went down since the last call: a controller of that player (any
// button, or a trigger past 64), the keyboard and mouse for player 1 (any
// key, mouse button or wheel notch: the driver's last_key_down). A device's
// first look counts as held (opening the screen with a button down is no
// press).
uint32_t PollPresses();

// MOVE: player `from`'s controller (the one its card shows) to player `to`
// (rules above: an empty player takes it, a player with a controller swaps).
// CanMoveTo: may a controller go to player `to` at all (the levels have room
// for them)? REMOVE: player p's controllers off. False = nothing to do or
// refused (rules above).
bool CanMoveTo(int to);
bool MoveController(PPCContext& ctx, uint8_t* base, int from, int to);
bool RemoveControllers(PPCContext& ctx, uint8_t* base, int p);

// The screen's pictures as menu sprites (one 0x19005 chunk each) for the
// pages' data patch (options_page.cpp). Empty if the pictures couldn't be
// read. Names: card k's device pictures in Picture order (PictureFrames),
// the selection rings (RingFrames: yellow = selected, then players 1-4's
// colours = a press on that card, and the controller being moved), the cards' backing (an opaque
// teal gradient under the game's glass, as the design asks).
struct Sprite {
  std::string name;
  std::vector<uint8_t> chunk;
};
const std::vector<Sprite>& MenuSprites();
std::vector<std::string> PictureFrames(int p);
std::vector<std::string> RingFrames();
std::string CardBackName();
// Player p's corner glow (their colour from the card's outer corner of the
// screen: top left, top right, bottom left, bottom right for players 1-4).
std::string AuraName(int p);

// Console / FIFO "players [list]": the boxes into the log.
bool DebugCommand(std::string_view command);

}  // namespace players_setup
