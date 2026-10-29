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
//   upgraded" achievement. With cheats enabled the achievement writer
//   (sub_82292938) only drops its queue entry (rules at AchievementsBlocked).
//   FREEZE / STEP. Leftovers of the developers' "PauseGameForScreenshots" and
//   "SingleStepFrame" script commands: bits 0x04 / 0x02 of the game manager's
//   byte +168. With 0x04 the frame function steps 1e-7 s per frame (the
//   picture keeps rendering, nothing moves, the camera manager still runs);
//   0x02 on top = one 1/60 s step, then the game clears it.
//   UNLOCKS (2026-10-23, findings/27 s.10). The game keeps four "unlocked"
//   lists as plain globals, each a run of ids ending at -1, filled from the
//   save when it loads, reset by New Game and written back by every save:
//     outfits      0x825A72A8, 11 slots, ids 0-10 (EPlayerSkin order: Crash,
//                  Greeny, Bluey, Reddy, Parafox, Ratcicle, Shurtle, Skeleton,
//                  Spike, Coco, Old School Coco); test sub_8229F858
//     concept art  0x825982C8, 12 slots, ids 0-11 (EConceptArt_kidsart..last)
//     enemy info   0x825982F8, 20 slots, ids 0-19 (EEnemyInfo_CHIRATTA..CORTEX)
//     tutorials    0x825983D0, 34 slots, ids 0-33 (ETutorialType_MOJO..
//                  YUKTOPUS_SPECIAL; the list = tutorials already SEEN, for
//                  the review screen: showing one adds it, nothing skips a
//                  tutorial because it's in the list (sub_822956C0))
//   The level scripts' DO_Unlock* actions (CActionUnlockSkin slot 2
//   sub_8229F6D8, ...ConceptArt sub_8229EC98, ...EnemyInfo sub_8229EEF0,
//   ...Tutorial sub_8229FB70) each put ONE id in the first free slot; we do
//   the same for every missing id (ids are contiguous: checked against the
//   script enum registration, every value 0..N-1 present). The game's skin
//   unlock has an extra step for Coco (9): it also MAKES PLAYER 2 COCO and
//   respawns; the cheat leaves that out (players keep what they wear; Coco is
//   picked at the chest) but sets the same "Coco unlocked" byte 0x825AB1C8
//   (only saved / loaded, nothing else reads it). Progress cheat: kept by the
//   next save, achievements off.
//   HIDE HUD. Everything of the HUD is skipped at its draw (ui/hud_hide.h:
//   since 2026-10-29 also the mojo count and multiplier, combo meters,
//   counter prompts, markers, the HUD pages' texts); the same switch as F2
//   (which is not a cheat; this switch doesn't keep cheats on across a load).
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
  std::string name;         // the titan's internal name ("Roller"), "Mask" or "on foot"
  int level = 0;            // 0 = not upgraded yet
  int max_level = 0;        // 5 for titans; 0 = unknown (Crash: his scripts decide)
  int mojo = 0;             // mojo collected toward this character's upgrades
  int next_price = 0;       // mojo the next upgrade needs (0 = none known)
  bool fully_upgraded = false;
  // Crash (on foot / mask): every MOJO upgrade bought (his script's last
  // threshold met, cheats.cpp kCrashLastPrice); the rest come with the story.
  bool mojo_upgrades_done = false;
  float hitpoints = 0.0f, max_hitpoints = 0.0f;
};

struct Snapshot {
  bool in_level = false;  // a level is played (cheats only work then)
  int total_mojo = 0;     // EStatInt_MojoCount (every mojo ever collected)
  bool hud_hidden = false;
  // Unlocked / total per UnlockKind (in a level only).
  int unlocked[4] = {}, unlock_total[4] = {};
  PlayerInfo players[kPlayers];
};

// The latest copy (any thread).
Snapshot GetSnapshot();

// One-shot actions, done at the game's next frame (any thread). `player` = 0-3.
void RequestLevelUp(int player);   // one upgrade for the character it plays now
void RequestMaxLevel(int player);  // every upgrade left (Crash: one per frame)
void RequestAddMojo(int player, int amount);  // like picking up that much mojo
void RequestRefillHealth(int player);
// Kills what the player is on now: in a titan the titan dies (Crash is thrown
// out), on foot Crash dies (the game's death: back at the checkpoint).
// Works with god mode on.
void RequestKill(int player);
// The game's "free jack" power-up: jack any titan without beating it first
// (until the next jack). On foot only.
void RequestFreeJack(int player);
void RequestStepFrame();           // with Freeze on: one 1/60 s step

// UNLOCKS: everything of a kind becomes unlocked in the loaded game (kept
// once the game saves). In a level only.
enum class UnlockKind { kOutfits, kConceptArt, kEnemyInfo, kTutorials, kCount };
const char* UnlockName(UnlockKind kind);  // "Outfits", ...
void RequestUnlockAll(UnlockKind kind);

// Switches (any thread).
void SetGodMode(bool on);
bool GodMode();
// NO AI: enemies (and enemy titans) stop thinking: the AI manager's per-frame
// update (CAIManager slot 10, sub_820B8688) is skipped, as the game itself
// skips it while frozen. Not a progress cheat (beating idle enemies counts).
void SetNoAi(bool on);
bool NoAi();
void SetGameSpeed(float speed);  // 1 = normal; 0.1 - 4
float GameSpeed();
void SetFrozen(bool on);
bool Frozen();
void SetHudHidden(bool hidden);
bool HudHidden();

// CHEATS AND ACHIEVEMENTS (2026-10-29). Before, only "progress" cheats (level
// ups, mojo, god mode...) turned achievements off, and only until the game
// restarted; some cheats slipped through. Now the rule is simple and the
// player is told:
//   * OPENING THE CHEATS MENU (or any console / FIFO cheat, a spawn) = cheats
//     ENABLED: a notification on the game's own message bar ("Cheats
//     Enabled", achievements.h ShowNotice) and no achievement is earned...
//   * ...until a save is LOADED (or New Game starts) with no cheat switch
//     left on (god mode, no AI, speed, freeze, hidden HUD, free camera) and
//     the menu closed: then they count again;
//   * a SAVE WRITTEN while cheats are on is MARKED "made with cheats" (a flag
//     in the save file's trailer, saves/save_files.h): loading it later keeps
//     achievements off for as long as that game is played, and every save of
//     it carries the mark on. A notification says so when it loads.
// Besides fairness, the mark helps bug reports: a strange state in a save
// may come from a mix of cheats (the log says so at every load).
// True while achievements aren't earned (sub_82292938 asks;
// achievements/achievements.cpp).
bool AchievementsBlocked();
// Why, for the menus ("" when not blocked).
const char* BlockReason();
// The game in progress carries the "made with cheats" mark.
bool SaveMarked();
// Cheats enabled (the Cheats menu opened, a spawn, a console cheat).
void NoteCheatsUsed();
void NoteProgressCheat();  // (the same; kept for spawn.cpp)
// The save library (saves/save_library.cpp): a save was loaded (number,
// whether its file carries the mark) or New Game began (0, false).
void OnGameStarted(bool from_marked_save, int save_number);
// The PC save drive wrote the game's save with the mark (or not).
void OnSaveWritten(bool marked);

// Debug FIFO (debug_input_script.h): "cheat <command>" for test runs without
// clicking the menu. Commands: god on|off, freeze on|off, step, speed <x>,
// hud on|off, levelup <player 1-4>, maxlevel <p>, mojo <p> <amount>, refill <p>,
// freejack <p>, kill <p>, unlock outfits|art|enemies|tutorials|all, hurt <p> <amount> (test damage through the game's own path),
// freecam on|off, freecam_keys on|off, freecam_reset, info (logs the
// snapshot), spawn <template> [player] [count] [ko] (spawn.h). False if unknown. ("cheat menu" = F5, handled by the FIFO.)
bool DebugCommand(std::string_view command);

}  // namespace cheats
