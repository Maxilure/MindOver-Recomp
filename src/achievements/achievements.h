// =============================================================================
// achievements/achievements.h -- the game's Xbox 360 achievements, kept by
// the port: one global record, a notification in the game's own look
// =============================================================================
//
// WHAT THE PLAYER GETS (2026-10-25, findings/51):
//   * the game's own 48 achievements (names, texts, pictures),
//     earned exactly when the original earns them;
//   * ONE record for the whole game, whatever save is played (as on the
//     Xbox, where achievements belong to the player, not to a save):
//     <saves folder>/achievements.toml;
//   * a notification when one is earned: a glass CARD at the bottom centre
//     (its picture, "Achievement Unlocked", its name), drawn by the game: it
//     is made of elements on the game's own message bar page
//     (InGame_MessageBar, the pop-up that slides up for voodoo dolls and
//     objectives) and rides that bar's queue, so it slides like the game's
//     messages and waits its turn behind them and through cutscenes; with
//     the game's reward jingle (stinger_reward). No gamerscore anywhere: the
//     Xbox profile's number means nothing on a PC.
//
// WHERE THE ACHIEVEMENTS COME FROM: the player's own default.xex carries
// the title's data (an XDBF block, the "SPA"): every achievement's id, name,
// description, locked description, picture id and gamerscore, in every
// language the game shipped. The SDK reads it at start-up (KernelState::
// LoadAchievementsData, in the language of --user_language); we take the list
// from there (rex::system::AchievementInfo) and the pictures (PNGs inside
// the XDBF) into the menus' own sprites (MenuSprites below). Nothing is copied into
// the port: all of it comes from the disc at run time.
//
// HOW THE GAME UNLOCKS ONE (read 2026-10-05 for the cheats, 2026-10-25):
// Radical's achievement manager queues 28-byte entries (the queue's end
// pointer at manager +168); its writer sub_82292938 takes the last one,
// copies it to +180 and calls the XAPI wrapper 0x82300670 =
// XUserWriteAchievements(count [manager +204], entries manager +180,
// overlapped manager +136). An entry starts with an XUSER_ACHIEVEMENT:
// +0 user index, +4 ACHIEVEMENT ID. The manager polls the overlapped before
// writing the next (sub_82291BB8); with nothing started it simply moves on
// (the cheats' "blocked" path proved it). The writer is REPLACED here: the
// entry comes off the queue, the id goes into our record, nothing reaches
// the emulated Xbox (no XUserWriteAchievements: one Xbox service fewer).
//
// THE OLD RECORD: before this, the SDK kept unlocks in
// <saves folder>/achievements/565507FA.toml (Windows FILETIME per id). The
// first start with this code COPIES those into the new file (the old file
// stays as it is: a backup, never written again by us).
//
// Progress cheats (cheats.h AchievementsBlocked) still stop unlocks.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/ppc/context.h>

REXCVAR_DECLARE(bool, achievement_popups);
REXCVAR_DECLARE(bool, achievement_sound);

namespace achievements {

// One achievement as the game's title data describes it.
struct Info {
  uint32_t id = 0;
  std::string name;              // "Crash Course" (UTF-8)
  std::string description;       // what it was earned for
  std::string locked_description;  // what to do, while locked (may say "Secret")
  uint32_t gamerscore = 0;      // in the title data; not shown (logs only)
  uint32_t image_id = 0;         // its picture in the XDBF
  bool unlocked = false;
  int64_t unlocked_at = 0;       // Unix seconds (0 = not unlocked)
};

// The whole list in the title data's order, with the record's state (empty
// until the SDK has read the title data).
std::vector<Info> List();

// Earned / total, and gamerscore earned / total.
void Count(int* earned, int* total, int* score, int* max_score);

// Records an unlock (game thread or console). New = queued for the
// notification and saved; already earned = nothing. False for an id the
// title data doesn't know.
bool Unlock(uint32_t id, const char* why);

// Every front-end update (players/more_players_frontend.cpp, sub_82261C40):
// the next waiting notification starts here (and its jingle, which needs the
// game's thread).
void FrontEndFrame(PPCContext& ctx, uint8_t* base);

// THE ACHIEVEMENTS SCREEN's pictures (achievements_screen.h) as SPRITES of
// the game's menu pages (ui/menu_pictures.h): "AchIcon<id>.tga" per
// achievement (its picture from the title data, 64 x 64) and "AchRing.tga"
// (a square outline of the port's own: the cells' frames, tinted by the
// page). The menus' data patch (options/options_page.cpp) adds them to the
// packages and builds the grid's picture elements from them. Made once
// (the title data must have been read: empty before), any thread.
struct MenuSprite {
  uint32_t id = 0;     // the achievement (0 = the ring)
  std::string name;    // the sprite's name, what a picture element shows
  std::vector<uint8_t> chunk;  // the whole 0x19005 chunk
};
const std::vector<MenuSprite>& MenuSprites();
// An achievement's picture as a FRAME of the multi-picture elements (its
// place among MenuSprites' pictures: one whose picture couldn't be read is
// left out, so it isn't the list index). -1 = no picture.
int SpriteFrame(uint32_t id);


// Debug FIFO / console: "achievement <command>":
//   list            every achievement in the log (id, gamerscore, state, name)
//   show <id>       the notification only (nothing recorded)
//   unlock <id>     as if the game earned it (recorded + notification)
bool DebugCommand(std::string_view command);

}  // namespace achievements
