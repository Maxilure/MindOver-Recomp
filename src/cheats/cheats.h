// =============================================================================
// cheats/cheats.h -- testing cheats: level ups, mojo, god mode, game speed,
// freeze, hide the HUD (the free camera is in free_camera.h, the F5 window
// that drives all of this in cheat_menu.h)
// =============================================================================
//
// WHAT: switches and one-shot actions for TESTING (reach a titan's last
// upgrade without grinding mojo, survive a fight to watch something, slow a
// glitch down, take a clean picture). Nothing here is saved by us; what the
// game itself saves afterwards (mojo, levels) stays as the cheat left it.
//
// HOW EACH CHEAT WORKS (findings/27 has the research):
//
//   LEVEL UP / MOJO. The game keeps every mojo count and level in its stats
//   manager (*(0x8259B190)+96): ints at +420 + 4 x id, ids in the order of the
//   EStatInt_* script names (3 = all mojo, 4 = mojo toward Crash's next
//   upgrade, 102 = Crash's level, 103-139 the titans' levels and counts).
//     * CRASH has no upgrade object: his LEVEL SCRIPT waits on
//       "WAIT_CrashMojoUpgradeValue" (CRequirementCrashMojoUpgradeValue,
//       check sub_822A1458: stat 4 >= the requirement's threshold, which it
//       also copies to the global 0x8259B1CC). So a level up = stat 4 topped
//       up to that threshold; the script then runs the upgrade itself (screen,
//       health, everything) like after a real pickup.
//     * A TITAN has a CUpgradeableBehaviour (vtable 0x82032344) at actor
//       +0x180: 5 steps of {mojo price, type, value, done} at +56, its mojo
//       stat id at +44, level stat id at +48, step index +140, next price
//       +144. The game's own routine sub_82177D38(this, actor, -, show) buys
//       every step the titan's mojo covers (that's what a pickup triggers,
//       message 8). We top the mojo stat up to the next price and call it.
//   GOD MODE. Every character's CDamageableBehaviour (vtable 0x8202D2E4, at
//   actor +0x12C) points at its hitpoints (+64) and maximum (+68); all damage
//   and healing goes through sub_821349E0(this, f1 = change). Its per-frame
//   update (slot 4, sub_82132F88, r4 = actor) tells us which ones belong to a
//   player's Crash or titan (sub_8226FCD8 = "player number of an actor");
//   for those the change function drops losses and the update refills.
//   Deaths that skip hitpoints (pits, if they kill directly) aren't covered
//   (not tested yet).
//   GAME SPEED. CTimeManager's scale (sub_822FFA40), the game's own slow-
//   motion factor: the frame function multiplies its step by it, and five
//   other places read it. We multiply what it returns.
//   ACHIEVEMENTS. "Max level" on a titan made the game unlock its "fully
//   upgraded" achievement. After any progress cheat the achievement writer
//   (sub_82292938) only drops its queue entry, until the game restarts.
//   FREEZE / STEP. Leftovers of the developers' "PauseGameForScreenshots" and
//   "SingleStepFrame" script commands: bits 0x04 / 0x02 of the game manager's
//   byte +168. With 0x04 the frame function steps 1e-7 s per frame (the
//   picture keeps rendering, nothing moves, the camera manager still runs);
//   0x02 on top = one 1/60 s step, then the game clears it.
//   HIDE HUD. The HUD controller's draw (sub_8226A998: every player's
//   portrait, health and special bars, mojo icon; players/more_players_hud.cpp
//   wraps it) is skipped. Texts on the HUD's menu page (the mojo count) stay.
//   (The developers' own "HideHud" script command, sub_8227D4B0, flips two
//   bytes nothing in the release build reads: tried, no effect.)
//
// THREADS: the menu (UI thread) only sets switches and queues requests; the
// game's main thread does the work at the start of its next frame (when the
// frame function asks CTimeManager for the scale) and publishes a Snapshot
// for the menu. Requests are only carried out while a level is played.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace cheats {

constexpr int kPlayers = 4;

// What one player's character is doing (copied by the game's thread).
struct PlayerInfo {
  bool present = false;     // joined the level (co-op state not 0)
  bool mask = false;        // rides another player as a mask (no body of its own)
  bool titan = false;       // rides a jacked titan (else on foot = Crash)
  std::string name;         // "Crash" or the titan's internal name ("Roller")
  int level = 0;            // 0 = not upgraded yet
  int max_level = 0;        // 5 for titans; 0 = unknown (Crash: his scripts decide)
  int mojo = 0;             // mojo collected toward this character's upgrades
  int next_price = 0;       // mojo the next upgrade needs (0 = none known)
  bool fully_upgraded = false;
  float hitpoints = 0.0f, max_hitpoints = 0.0f;
};

struct Snapshot {
  bool in_level = false;  // a level is played (cheats only work then)
  int total_mojo = 0;     // EStatInt_MojoCount (every mojo ever collected)
  bool hud_hidden = false;
  PlayerInfo players[kPlayers];
};

// The latest copy (any thread).
Snapshot GetSnapshot();

// One-shot actions, done at the game's next frame (any thread). `player` = 0-3.
void RequestLevelUp(int player);   // one upgrade for the character it plays now
void RequestMaxLevel(int player);  // every upgrade left (Crash: one per frame)
void RequestAddMojo(int player, int amount);  // like picking up that much mojo
void RequestRefillHealth(int player);
// The game's "free jack" power-up: jack any titan without beating it first
// (until the next jack). On foot only.
void RequestFreeJack(int player);
void RequestStepFrame();           // with Freeze on: one 1/60 s step

// Switches (any thread).
void SetGodMode(bool on);
bool GodMode();
void SetGameSpeed(float speed);  // 1 = normal; 0.1 - 4
float GameSpeed();
void SetFrozen(bool on);
bool Frozen();
void SetHudHidden(bool hidden);
bool HudHidden();

// True once a cheat that changes progress (level up, max level, mojo, refill,
// god mode) was used: from then on until the game restarts, achievements the
// game unlocks aren't written (sub_82292938 hook). Freeze, speed, the camera
// and the HUD don't count.
bool AchievementsBlocked();
// Turns achievements off like the progress cheats above (spawn.h uses it).
void NoteProgressCheat();

// Debug FIFO (debug_input_script.h): "cheat <command>" for test runs without
// clicking the menu. Commands: god on|off, freeze on|off, step, speed <x>,
// hud on|off, levelup <player 1-4>, maxlevel <p>, mojo <p> <amount>, refill <p>,
// freejack <p>, hurt <p> <amount> (test damage through the game's own path),
// freecam on|off, freecam_keys on|off, freecam_reset, info (logs the
// snapshot), spawn <template> [player] [count] [ko] (spawn.h). False if unknown. ("cheat menu" = F5, handled by the FIFO.)
bool DebugCommand(std::string_view command);

}  // namespace cheats
