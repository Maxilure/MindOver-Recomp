// =============================================================================
// net/online_rules.h -- what changes in the game while an online session runs
// (the online plan's "ONLINE RULES"), part 1: NOTHING PAUSES THE WORLD
// =============================================================================
//
// In a session every PC runs the whole world. A pause would have to stop it on
// every PC at once, for everybody, because one player opened a menu. The rule
// (2026-11-17): online, nothing pauses.
//
// HOW THE GAME PAUSES (found with the debug console's `who` on the paused
// byte): the pause menu's script calls a script method (sub_82123778) that
// calls the game manager's pause, sub_8227C8E8(manager, on): game +168 bit
// 0x20 (the AI, physics, sound... skip their updates while it's set), +104's
// +24 bit 0x80, and the input manager's reset (sub_82274538). The
// lost-controller box (players/lost_controller.*) uses the same function.
// Online it's answered "done" without doing anything: the menu opens, the
// world runs on under it.
//
// THE MENU'S OWNER STANDS STILL. With the world running, the buttons the
// owner presses in the menu would also move their Crash (the menu reads the
// MENU input map, the character the GENERAL / PLAYER / titan maps of the same
// controller). The game already has the tool: a LOCKED controller (the root
// input controller's byte +516 above 0, sub_82272D68) answers "no" to every
// play question; cutscenes lock all of them that way (DO_LockAllControllers,
// AreControllersLocked = sub_82265A60), and the cutscene's own "Skip" prompt
// still reads the MENU map through it. So while player p's menu is on screen
// (menu_input's owner rules: who paused, who used the totem...) we add 1 to
// p's lock, and take it back when the menu goes. Everyone else plays on.
// Both PCs see the same menus on the same frames (same inputs), so the lock
// changes on the same frame everywhere.
// =============================================================================
#pragma once

#include <cstdint>

struct PPCContext;

namespace online_rules {

// Once per game frame (the front end's frame, more_players_frontend.cpp:
// it runs in menus too; game main thread): locks / unlocks the menu owner's controller as above. Out of a
// session: gives back any lock it added.
void OnFrame(PPCContext& ctx, uint8_t* base);

// THE MAP (Back in play: front end 489 -> exit 502 -> state 927) has no owner
// in the original: any player's buttons work it, and it pauses the world. Online
// the world runs on, so the map belongs to the player who OPENED it (whose
// session input has Back down on the frame of that exit): their Crash stands
// still and only their buttons work the map (menu_input asks MenuOwner). The
// original's way offline. -1 = no such owner (offline, or not in the map).
// Game main thread.
int MenuOwner();

// THE MENUS STILL SEE A PAUSE. The front end's code reads "paused" in many
// places: every frame the co-op join check (sub_82264988) copies it into
// front end +8594 bit 0x80 ("a menu is on screen in play"), which the menus'
// D-pad handling and the port's menu-owner rules (menu_input) need, and the
// question box (the totem's "Do you want to save?", update sub_8225DE68)
// closes itself in a level unless the game is paused (its check at
// 0x8225DEB4; found with the console's `who` on its "answered" flag). Online,
// while a menu is up in a level (front end out of GameRunning 489), the
// front end's frame (sub_82261C40) runs with game +168 bit 0x20 set and gets
// it put back after: the menus behave as in the original, the world (AI,
// physics, players, sound) never sees a pause. Game main thread, around the
// front end's frame (more_players_frontend.cpp).
void BeforeFrontEndFrame(uint8_t* base);
void AfterFrontEndFrame(uint8_t* base);

// ONLY THE OWNER SEES THEIR MENU. In lockstep a menu is part of the shared
// game: every PC runs it, so every PC drew it, over a player who keeps playing.
// Online, on every PC but the menu owner's, the in-game menu pages aren't
// DRAWN (the menu still runs everywhere: nothing in the world changes). Kept
// for everyone: the HUD's pages (InGame, the co-op corners, counters,
// announcements, the message bar, tutorials, the letterbox). The Parent draw
// wrapper (ui/hud_hide.cpp, sub_82371270) asks for every page, group and layer
// it draws: true = skip it. Game main thread.
bool HideMenuDraw(uint32_t node);
// The draw of `node` is over (pairs with every HideMenuDraw that said false).
void EndMenuDraw(uint32_t node);

// Registers the front-end exit listener (net::Init).
void Init();

}  // namespace online_rules
