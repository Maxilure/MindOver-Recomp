// =============================================================================
// ui/hud_scale.h -- HUD SIZE: the whole HUD smaller, each part toward its own
// screen corner
// =============================================================================
//
// WHAT (2026-10-30): two settings (Options -> Display -> HUD):
//   * hud_size (30-100 %, default 80 (2026-10-30); 100 = the
//     original): every HUD part drawn smaller, shrunk toward the screen corner it belongs to (player 1 top
//     left, player 2 top right, players 3-4 bottom left / right), so its
//     margin from the edges shrinks with it;
//   * hud_smaller_four_players (default on): while players 3-4 are in the
//     game (more_players::FourPlayerLayout) the HUD is AT MOST 80 %, so
//     four corners fit: a HUD Size already at or under 80 % is kept as it is
//     (a ceiling, not a further shrink). With 1-2 players and 100 % nothing changes at all.
//   GLOBAL settings (user/settings.toml like every Options row), never stored
//   in a save: the same HUD with every save.
//
// WHAT IT COVERS: each player's corner (portrait, health / special bars,
// level-up ring, mojo count, multiplier, the "Join Game" text), the combo
// meters, the "counter now" prompts (star + its Y), the pocketed mutant's
// display (toward the top centre), the collectable counter and a timed
// challenge's clock under it (toward the top RIGHT corner: the original lays
// them out as a column right under player 2's corner, as far from the right
// edge as player 1's portrait is from the left; 2026-10-30, they shrank toward
// the top centre at first, which pulled them toward the middle), the
// announcement prompt ("Attack" + button, a dig spot's prompt: toward the
// screen's left edge at its own height). NOT: the markers over the players'
// heads (they're in the world), the message bar, hints, the mission title
// and menus.
//
// HOW: the game draws all of the HUD through PDDI's MODELVIEW matrix stack
// (type 0; findings/53). The context's slot 15 pushes a copy of the top,
// slot 16 pops it, slot 14 loads one. Every 2D frame (the front end's, each
// Scrooby page's) starts from identity and scales canvas units by 1.5 / 1280
// around the canvas centre, so the stack's OUTPUT is the same for all of them:
// the screen spans x -0.5 .. 0.5, y -0.28 .. 0.28 there. Around a part's draw
// we push, load "the top, then scale by s toward the corner" (worked out in
// that output space, so a text's own units don't matter) and pop afterwards:
// everything the part draws (immediate quads, Scrooby texts with their own
// matrices on top) shrinks toward that corner, in BOTH renderers, without
// touching any of the game's positions (layouts, pulses and the players 3-4
// mirroring keep working as they are).
//
// The pieces are wrapped where they're drawn: CHealthDisplay / combo meter /
// counter prompt / stored actor / timer draws in ui/hud_hide.cpp, Scrooby
// texts (Text slot 6) in ui/marquee.cpp, Scrooby pages (Parent slot 6) in
// ui/hud_hide.cpp. A player's TEXTS on the shared page InGame (mojo count,
// multiplier, counter Y) are told apart by pointer: the displays and prompts
// note theirs when they draw (NoteText).
// =============================================================================
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace hud_scale {

// Where a part shrinks toward: a player's corner (0-3) or the top centre.
constexpr int kTopCentre = 10;
constexpr int kTopRight = 1;       // = player 2's corner
constexpr int kAnnouncement = 11;  // the left edge, at the announcement's height
constexpr int kNone = -1;

// The scale in use now (1 = untouched).
float Scale();

// Pushes the matrix and scales toward `where` when the scale isn't 1 (and
// `where` isn't kNone). Pop only if Push returned true.
bool Push(PPCContext& ctx, uint8_t* base, int where);
void Pop(PPCContext& ctx, uint8_t* base);

// Push / pop around a scope.
class Scope {
 public:
  Scope(PPCContext& ctx, uint8_t* base, int where)
      : ctx_(ctx), base_(base), pushed_(Push(ctx, base, where)) {}
  ~Scope() {
    if (pushed_) Pop(ctx_, base_);
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  PPCContext& ctx_;
  uint8_t* base_;
  bool pushed_;
};

// A player's Scrooby text on a shared page (noted by its display / prompt
// every frame) -> the player, else kNone.
void NoteText(uint32_t text, int player);
int WhereOfText(uint32_t text);

// DEBUG (console "cheat hudtest counter|timer|announce|all", game thread):
// shows the HUD parts that only appear in certain places, through the game's
// own functions: a collectable counter "0/6" (frontend +1628: add
// sub_8225C520, show sub_8225C7E8), a timed challenge's clock (*(frontend
// +260): seconds +12, start sub_8226EC40), the announcement (frontend +72:
// seconds +16, text +12).
void ShowTestParts(PPCContext& ctx, uint8_t* base, int what);
constexpr int kTestCounter = 1, kTestTimer = 2, kTestAnnouncement = 4;

}  // namespace hud_scale
