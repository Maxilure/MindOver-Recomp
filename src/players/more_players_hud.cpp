// =============================================================================
// players/more_players_hud.cpp -- a HUD for players 3 and 4 (findings/26 s.18)
// =============================================================================
//
// WHAT: the original HUD has one corner per player: player 1 top left, player
// 2 top right (portrait, health and special bars, mojo count, the combo
// multiplier, and "Join Game" while the player isn't in). With three or four
// local players, player 3 gets the same set bottom left and player 4 bottom
// right. With two players nothing changes (none of this runs).
//
// HOW THE GAME BUILDS IT (found 2026-10-04):
//   * The HUD controller (constructor sub_8226A690, 24 bytes: +8 another
//     element, +12 / +16 the two displays, +20 a 0/1 setting) makes ONE
//     CHealthDisplay per player (vtable 0x8203AFC4, 284 bytes, constructor
//     sub_822678F8(this, player): the player number at +144). Its methods loop
//     over the two displays (virtual slots 0 delete, 1 set up, 2, 3 update with
//     the frame time, 4 draw, 5 show / hide, 6 "wants to draw").
//   * A display's set-up (slot 1, sub_82267B18) finds its parts BY NAME, from
//     two-entry tables of name objects indexed by +144: the "Join Game" page
//     InGame_CoOp_PlayerN (texts Message, Button), and the texts
//     MojoMultiplierPlayerN, MojoMultiplierFXPlayerN, MojoCountPlayerN of the
//     page InGame. Then it sets its BASE POSITION: x at +252 (player 1 left,
//     player 2 right, from the screen width), y at +256 (456, the top: y counts
//     up from the bottom of a 640 x 480 screen) and a mirror factor at +260
//     (+1 player 1, -1 player 2). Everything it draws (bars, portrait, icons)
//     and the texts it moves are placed relative to these three numbers.
//
// WHAT THIS MODULE DOES:
//   1. A data patch (data/data_patcher.h) adds to every in-game menus package
//      (each one holding InGame.prj): copies of player 1's texts named
//      ...Player3 and of player 2's named ...Player4 in the page InGame, the
//      pages InGame_CoOp_Player3 / 4 (copies of 1 / 2, moved down) and those two
//      pages in the screen InGame.scr. Copies keep their sizes: only a digit
//      changes in each name.
//   2. The controller makes displays for players 3-4 and runs every one of its
//      loops over them too (wrappers below).
//   3. Their set-up gets the ...Player3 / 4 names (4 midasm hooks) and a base
//      position at the bottom: player 3 = player 1's x and mirror, player 4 =
//      player 2's, y from the layout below.
//   4. THE LAYOUT: players 3-4's HUD is players 1-2's MIRRORED top to bottom,
//      computed from player 1's base y and the measured shape of the HUD
//      (section "Layout"), not hand-tuned offsets.
// =============================================================================
#include "more_players_hud.h"
#include "more_players.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "cheats/cheats.h"
#include "data/data_patcher.h"
#include "data/pure3d.h"

extern "C" REX_FUNC(__imp__sub_8227E890);  // the game's operator new (r3 = size)
extern "C" REX_FUNC(__imp__sub_822678F8);  // CHealthDisplay constructor (this, player)
extern "C" REX_FUNC(__imp__sub_8236ACB8);  // name object (r3 = 8 bytes) from a C string (r4)

namespace more_players_hud {
namespace {

constexpr float kTopY = 456.0f;              // players 1-2's base y (sub_82265BB0)
constexpr float kJoinTextY = 385.0f;         // "Join Game" texts of players 1-2 (pages)
constexpr uint32_t kDisplaySize = 284;       // sub_8226A690 allocates 284 per display
constexpr uint32_t kDisplayPlayer = 144, kDisplayX = 252, kDisplayY = 256, kDisplayMirror = 260;
constexpr uint32_t kControllerDisplays = 12;  // controller +12 / +16

// --- guest memory helpers ------------------------------------------------------
uint8_t* Guest(uint32_t a) { return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(a); }
uint32_t Read32(uint32_t a) {
  const uint8_t* p = Guest(a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Write32(uint32_t a, uint32_t v) {
  uint8_t* p = Guest(a);
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
float ReadFloat(uint32_t a) { const uint32_t b = Read32(a); float f; std::memcpy(&f, &b, 4); return f; }
void WriteFloat(uint32_t a, float f) { uint32_t b; std::memcpy(&b, &f, 4); Write32(a, b); }

uint32_t CallGame(void (*function)(PPCContext&, uint8_t*), PPCContext& ctx, uint8_t* base,
                  uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = r3; ctx.r4.u64 = r4; ctx.r5.u64 = r5;
  function(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}
// A virtual method of a game object (vtable at +0, slot n), r4 / f1 as given.
uint32_t CallVirtual(PPCContext& ctx, uint8_t* base, uint32_t object, int slot, uint32_t r4 = 0,
                     double f1 = 0) {
  const uint32_t target = Read32(Read32(object) + 4 * slot);
  PPCFunc* function = rex::runtime::ResolveIndirectFunction(target);
  if (!function) return 0;
  const PPCContext saved = ctx;
  ctx.r3.u64 = object; ctx.r4.u64 = r4; ctx.f1.f64 = f1;
  function(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

// --- the names of players 3-4's parts -------------------------------------------
// Kinds, in the order of their hooks in the set-up.
enum Kind { kCoOpPage, kMultiplier, kMultiplierFx, kMojoCount, kCounterButton, kKinds };
constexpr const char* kNameFormat[kKinds] = {"InGame_CoOp_Player%d", "MojoMultiplierPlayer%d",
                                             "MojoMultiplierFXPlayer%d", "MojoCountPlayer%d",
                                             "CounterButtonPlayer%d"};
// Guest memory: per kind and player 3-4 an 8-byte name object, then the strings.
uint32_t g_names = 0;  // 0 = not made yet
bool g_names_made = false;
uint32_t NameObject(int kind, int player) { return g_names + 8 * (2 * kind + (player - 2)); }

void MakeNames(PPCContext& ctx, uint8_t* base) {
  if (g_names_made) return;
  g_names = rex::system::kernel_memory()->SystemHeapAlloc(512);
  if (!g_names) return;
  std::memset(Guest(g_names), 0, 512);
  uint32_t text = g_names + 8 * 2 * kKinds;
  for (int kind = 0; kind < kKinds; ++kind) {
    for (int player = 2; player < 4; ++player) {
      char s[40];
      std::snprintf(s, sizeof(s), kNameFormat[kind], player + 1);
      std::memcpy(Guest(text), s, std::strlen(s) + 1);
      CallGame(__imp__sub_8236ACB8, ctx, base, NameObject(kind, player), text);
      text += uint32_t(std::strlen(s) + 1 + 3) & ~3u;
    }
  }
  g_names_made = true;
}

// --- the displays of players 3-4, per HUD controller ----------------------------
std::map<uint32_t, std::vector<uint32_t>> g_extra;  // controller -> displays of players 3..N
uint32_t g_player1_display = 0;                     // for the layout (player 1's base y)
uint32_t g_player2_display = 0;                     // for player 4's x (the last controller made)

// --- Layout ----------------------------------------------------------------------
// Players 1-2's HUD hangs from the TOP of the screen: portrait, the bars beside
// its upper half (health, then the titan's special bar under it), the combo
// multiplier over the portrait's lower edge and the mojo count under it.
// Players 3-4 sit at the BOTTOM, so they get the same HUD mirrored top to
// bottom, each part around the PORTRAIT'S CENTRE, keeping its own inside
// order (digits stay upright, icons the right way up):
//   * the portrait as far from the bottom edge as player 1's is from the top;
//   * the bars as far above the portrait's lower edge as player 1's are
//     under its upper edge, the special bar above health;
//   * the count above the portrait, as far from it as player 1's is below,
//     the multiplier over the portrait's upper edge.
// (A first version used fixed shifts with the special bar lifted above
// health: its icon met the health icon, the count sat on the portrait.)
//
// Shapes, measured on 1280 x 720 captures of player 1's HUD (2026-10-05;
// 1 HUD unit = 1.5 px vertically), as distances BELOW the display's base y
// (+256; y counts up from the bottom of a 480-unit screen):
constexpr float kScreenHeight = 480.0f;
constexpr float kPortraitTop = 12.0f, kPortraitBottom = 71.33f;  // the ring, outer edge
// mirroring a part spanning [a, b] below the base around the portrait's
// centre gives [S - b, S - a]: it moves by S - (a + b) (down if positive)
constexpr float kMirrorSum = kPortraitTop + kPortraitBottom;
// the bars WITH their icons (the cross / bolt stick out past the bars; the
// bolt starts where the cross ends):
constexpr float kHealthTop = 9.33f, kHealthBottom = 26.67f;
constexpr float kSpecialTop = 26.67f, kSpecialBottom = 46.67f;
// the texts' glyphs: count 130-151 px; the multiplier's anchor is 25 units
// (115 - 90, the set-up's constants 0x82506C20 / 0x82506C10) above it
constexpr float kCountTop = 86.67f, kCountBottom = 100.67f;
constexpr float kMultiplierTop = kCountTop - 25.0f, kMultiplierBottom = kCountBottom - 25.0f;

// Players 3-4's base y: player 1's mirrored (the portrait's top gap becomes
// its bottom gap). Player 1's live value when its display exists (the game
// computes it at set-up: sub_82265BB0), else the usual 456.
float BottomBaseY() {
  const float top = g_player1_display ? ReadFloat(g_player1_display + kDisplayY) : kTopY;
  return kScreenHeight - top + kMirrorSum;
}
// How far down a part spanning [top, bottom] below the base moves when mirrored.
constexpr float MirrorShift(float top, float bottom) { return kMirrorSum - (top + bottom); }

std::vector<uint32_t>* ExtraOf(uint32_t controller) {
  auto it = g_extra.find(controller);
  return it == g_extra.end() ? nullptr : &it->second;
}

}  // namespace

// =============================================================================
// 1. The data patch
// =============================================================================
namespace {
using pure3d::Bytes;
using pure3d::Chunk;

// A copy of a chunk with the last character of its name changed (names keep
// their length, so every size stays the same).
Bytes Renamed(const Bytes& d, const Chunk& c, char digit) {
  Bytes out = pure3d::Copy(d, c);
  const size_t length = out[12];
  const std::string_view name(reinterpret_cast<const char*>(&out[13]), length);
  const size_t end = name.find('\0') == std::string_view::npos ? length : name.find('\0');
  // the digit sits right before ".pag" for pages, at the end for texts
  size_t at = std::string_view(name.data(), end).rfind(".pag");
  at = at == std::string_view::npos ? end - 1 : at - 1;
  out[13 + at] = uint8_t(digit);
  return out;
}

// A text chunk's y (the third u32 after its name: ?, x, y, w, h), little-endian.
void SetTextY(Bytes& chunk, int32_t y) {
  const size_t at = 12 + 1 + chunk[12] + 8;
  for (int i = 0; i < 4; ++i) chunk[at + i] = uint8_t(uint32_t(y) >> (8 * i));
}

// The page `page` with each child of its first layer passed through `edit`
// (which appends what it wants to `layer_children`).
template <typename Edit>
Bytes EditLayer(const Bytes& d, const Chunk& page, Edit edit) {
  Bytes page_children;
  for (const Chunk& layer : pure3d::ChildrenOf(d, page)) {
    if (layer.id != 0x18020) {
      const Bytes copy = pure3d::Copy(d, layer);
      page_children.insert(page_children.end(), copy.begin(), copy.end());
      continue;
    }
    Bytes layer_children;
    for (const Chunk& element : pure3d::ChildrenOf(d, layer)) edit(element, layer_children);
    const Bytes new_layer = pure3d::WithChildren(d, layer, layer_children);
    page_children.insert(page_children.end(), new_layer.begin(), new_layer.end());
  }
  return pure3d::WithChildren(d, page, page_children);
}

bool PatchHud(const Bytes& host, Bytes* out) {
  Chunk root;
  std::vector<Chunk> top;
  if (!pure3d::Parse(host, &root, &top)) return false;
  const auto project = std::find_if(top.begin(), top.end(), [&](const Chunk& c) {
    return c.id == pure3d::kChunkProject && pure3d::NameOf(host, c) == "InGame.prj";
  });
  if (project == top.end()) return false;
  const float bottom = BottomBaseY();
  const int32_t join_y = int32_t(bottom - (kTopY - kJoinTextY));
  Bytes children;
  bool page_done = false, coop_done = false, screen_done = false;
  for (const Chunk& k : pure3d::ChildrenOf(host, *project)) {
    const std::string_view name = pure3d::NameOf(host, k);
    Bytes chunk;
    if (k.id == pure3d::kChunkPage && name == "InGame.pag") {
      // the texts of players 1-2 + copies for players 3 (of 1's) and 4 (of 2's)
      chunk = EditLayer(host, k, [&](const Chunk& e, Bytes& layer) {
        const Bytes copy = pure3d::Copy(host, e);
        layer.insert(layer.end(), copy.begin(), copy.end());
        const std::string_view text = pure3d::NameOf(host, e);
        for (const char* stem : {"MojoCountPlayer", "MojoMultiplierPlayer", "MojoMultiplierFXPlayer",
                                 "CounterButtonPlayer"}) {
          for (char from : {'1', '2'}) {
            if (text == std::string(stem) + from) {
              const Bytes extra = Renamed(host, e, from == '1' ? '3' : '4');
              layer.insert(layer.end(), extra.begin(), extra.end());
            }
          }
        }
      });
      page_done = true;
    } else if (k.id == pure3d::kChunkScreen && name == "InGame.scr") {
      // + "InGame_CoOp_Player3.pag", "...4.pag" (count at name field + 4)
      chunk = pure3d::Copy(host, k);
      const size_t count_at = 12 + 1 + chunk[12] + 4;
      uint32_t count = pure3d::Le32(&chunk[count_at]);
      for (char digit : {'3', '4'}) {
        const std::string page = std::string("InGame_CoOp_Player") + digit + ".pag";
        chunk.push_back(0x18);  // field length 24: the name, NUL padded
        for (size_t i = 0; i < 24; ++i) chunk.push_back(i < page.size() ? uint8_t(page[i]) : 0);
        ++count;
      }
      for (int i = 0; i < 4; ++i) chunk[count_at + i] = uint8_t(count >> (8 * i));
      const uint32_t size = uint32_t(chunk.size());
      for (int i = 0; i < 4; ++i) chunk[4 + i] = chunk[8 + i] = uint8_t(size >> (8 * i));
      screen_done = true;
    } else {
      chunk = pure3d::Copy(host, k);
    }
    children.insert(children.end(), chunk.begin(), chunk.end());
    // after each player 1-2 "Join Game" page: its copy for player 3 / 4, moved down
    for (char from : {'1', '2'}) {
      if (k.id == pure3d::kChunkPage && name == std::string("InGame_CoOp_Player") + from + ".pag") {
        Bytes renamed = Renamed(host, k, from == '1' ? '3' : '4');
        Chunk copy_chunk{0, k.id, k.data_size, k.total_size};
        const Bytes moved = EditLayer(renamed, copy_chunk, [&](const Chunk& e, Bytes& layer) {
          Bytes element = pure3d::Copy(renamed, e);
          if (e.id == 0x18023) SetTextY(element, join_y);  // texts Message, Button
          layer.insert(layer.end(), element.begin(), element.end());
        });
        children.insert(children.end(), moved.begin(), moved.end());
        coop_done = true;
      }
    }
  }
  if (!page_done || !coop_done || !screen_done) {
    REXLOG_WARN("More players: the in-game HUD package isn't as expected (page {}, join pages {}, screen {})",
                page_done, coop_done, screen_done);
    return false;
  }
  Bytes body;
  for (const Chunk& c : top) {
    const Bytes chunk = &c == &*project ? pure3d::WithChildren(host, c, children) : pure3d::Copy(host, c);
    body.insert(body.end(), chunk.begin(), chunk.end());
  }
  *out = pure3d::WithChildren(host, root, body);
  return true;
}
}  // namespace

void Register() {
  if (more_players::LocalPlayerCount() <= 2) return;
  data_patcher::Register("HUD for players 3-4", data_patcher::kCategoryFrontend, "", PatchHud);
}

}  // namespace more_players_hud

using namespace more_players_hud;

// =============================================================================
// 2. The controller's displays of players 3-4
// =============================================================================
extern "C" REX_FUNC(__imp__sub_8226A690);
extern "C" REX_FUNC(__imp__sub_8226A770);
extern "C" REX_FUNC(__imp__sub_8226A800);
extern "C" REX_FUNC(__imp__sub_8226A8A8);
extern "C" REX_FUNC(__imp__sub_8226A908);
extern "C" REX_FUNC(__imp__sub_8226A998);
extern "C" REX_FUNC(__imp__sub_8226AA48);
extern "C" REX_FUNC(__imp__sub_8226AC58);
extern "C" REX_FUNC(__imp__sub_82268550);
extern "C" REX_FUNC(__imp__sub_82269450);
extern "C" REX_FUNC(__imp__sub_82269460);
extern "C" REX_FUNC(__imp__sub_82269470);

// Constructor (r3 = controller): the original makes players 1-2's displays.
extern "C" REX_FUNC(sub_8226A690) {
  const uint32_t controller = ctx.r3.u32;
  __imp__sub_8226A690(ctx, base);
  const int n = more_players::LocalPlayerCount();
  if (n <= 2) return;
  MakeNames(ctx, base);
  g_player1_display = Read32(controller + kControllerDisplays);
  g_player2_display = Read32(controller + kControllerDisplays + 4);
  std::vector<uint32_t>& extra = g_extra[controller];
  for (int p = 2; p < n; ++p) {
    const uint32_t display = CallGame(__imp__sub_8227E890, ctx, base, kDisplaySize);
    if (!display) break;
    CallGame(__imp__sub_822678F8, ctx, base, display, uint32_t(p));
    extra.push_back(display);
  }
  REXLOG_INFO("More players: HUD for {} more players", extra.size());
  ctx.r3.u64 = controller;
}
// Destructor body: players 3-4's displays go too (slot 0 with "delete" = 1).
extern "C" REX_FUNC(sub_8226A770) {
  const uint32_t controller = ctx.r3.u32;
  if (auto* extra = ExtraOf(controller)) {
    for (uint32_t display : *extra) CallVirtual(ctx, base, display, 0, 1);
    g_extra.erase(controller);
  }
  __imp__sub_8226A770(ctx, base);
}
// Set up (slot 1 of each) + show (slot 5 with 1).
extern "C" REX_FUNC(sub_8226A800) {
  const uint32_t controller = ctx.r3.u32;
  __imp__sub_8226A800(ctx, base);
  if (auto* extra = ExtraOf(controller)) {
    for (uint32_t d : *extra) CallVirtual(ctx, base, d, 1);
    for (uint32_t d : *extra) CallVirtual(ctx, base, d, 5, 1);
    more_players_frontend::SetUpInGame(ctx, base);  // their counter prompts (the page is in now)
  }
}
// Slot 2 of each.
extern "C" REX_FUNC(sub_8226A8A8) {
  const uint32_t controller = ctx.r3.u32;
  __imp__sub_8226A8A8(ctx, base);
  if (auto* extra = ExtraOf(controller)) for (uint32_t d : *extra) CallVirtual(ctx, base, d, 2);
}
// Update (slot 3 with the frame time f1), while the controller is on (+4 bit 0x80).
extern "C" REX_FUNC(sub_8226A908) {
  const uint32_t controller = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  __imp__sub_8226A908(ctx, base);
  if (!(*Guest(controller + 4) & 0x80)) return;
  if (auto* extra = ExtraOf(controller)) for (uint32_t d : *extra) CallVirtual(ctx, base, d, 3, 0, dt);
}
// Draw: slot 4 of each that says yes to slot 6. Nothing while the cheat menu
// hides the HUD (cheats/cheats.h).
extern "C" REX_FUNC(sub_8226A998) {
  if (cheats::HudHidden()) return;
  const uint32_t controller = ctx.r3.u32;
  __imp__sub_8226A998(ctx, base);
  if (auto* extra = ExtraOf(controller)) {
    for (uint32_t d : *extra) {
      if (CallVirtual(ctx, base, d, 6) & 0xFF) CallVirtual(ctx, base, d, 4);
    }
  }
}
// Show / hide (slot 5 with r4).
extern "C" REX_FUNC(sub_8226AA48) {
  const uint32_t controller = ctx.r3.u32, show = ctx.r4.u32;
  __imp__sub_8226AA48(ctx, base);
  if (auto* extra = ExtraOf(controller)) for (uint32_t d : *extra) CallVirtual(ctx, base, d, 5, show);
}
// sub_82268550 on each.
extern "C" REX_FUNC(sub_8226AC58) {
  const uint32_t controller = ctx.r3.u32;
  __imp__sub_8226AC58(ctx, base);
  if (auto* extra = ExtraOf(controller)) for (uint32_t d : *extra) CallGame(__imp__sub_82268550, ctx, base, d);
}

// The three "flash" methods with a player number (r4; -1 = every display):
// players 3-4 use their own display (more_players.cpp skipped them before).
namespace more_players_hud {
void FlashExtra(uint32_t controller, int32_t player, void (*flash)(PPCContext&, uint8_t*),
                PPCContext& ctx, uint8_t* base) {
  auto* extra = ExtraOf(controller);
  if (!extra) return;
  for (size_t i = 0; i < extra->size(); ++i) {
    if (player == -1 || player == int32_t(i) + 2) CallGame(flash, ctx, base, (*extra)[i]);
  }
}
}  // namespace more_players_hud
extern "C" REX_FUNC(__imp__sub_8226AB18);
extern "C" REX_FUNC(__imp__sub_8226AB78);
extern "C" REX_FUNC(__imp__sub_8226ABD8);
extern "C" REX_FUNC(sub_8226AB18) {
  const uint32_t c = ctx.r3.u32; const int32_t p = int32_t(ctx.r4.u32);
  if (p < 2) __imp__sub_8226AB18(ctx, base);
  if (p == -1 || p >= 2) FlashExtra(c, p, __imp__sub_82269450, ctx, base);
}
extern "C" REX_FUNC(sub_8226AB78) {
  const uint32_t c = ctx.r3.u32; const int32_t p = int32_t(ctx.r4.u32);
  if (p < 2) __imp__sub_8226AB78(ctx, base);
  if (p == -1 || p >= 2) FlashExtra(c, p, __imp__sub_82269460, ctx, base);
}
extern "C" REX_FUNC(sub_8226ABD8) {
  const uint32_t c = ctx.r3.u32; const int32_t p = int32_t(ctx.r4.u32);
  if (p < 2) __imp__sub_8226ABD8(ctx, base);
  if (p == -1 || p >= 2) FlashExtra(c, p, __imp__sub_82269470, ctx, base);
}

// =============================================================================
// 3. The set-up of players 3-4's displays (midasm hooks in sub_82267B18)
// =============================================================================
// Right before the name object of `kind` is used: r10 = player * 8, r3 = the
// table + player * 8. Players 3-4: our name objects.
static void NameFor(int kind, PPCRegister& offset, PPCRegister& name) {
  const uint32_t player = offset.u32 / 8;
  if (player >= 2 && player < 4 && g_names_made) name.u64 = NameObject(kind, int(player));
}
void MorePlayersHudNameCoOp(PPCRegister& o, PPCRegister& n) { NameFor(kCoOpPage, o, n); }
void MorePlayersHudNameMultiplier(PPCRegister& o, PPCRegister& n) { NameFor(kMultiplier, o, n); }
void MorePlayersHudNameMultiplierFx(PPCRegister& o, PPCRegister& n) { NameFor(kMultiplierFx, o, n); }
void MorePlayersHudNameCount(PPCRegister& o, PPCRegister& n) { NameFor(kMojoCount, o, n); }

// After the base position is set (0x8226802C, r31 = the display), before the
// texts are placed from it: players 3-4 move to the bottom corners.
void MorePlayersHudPosition(PPCRegister& display) {
  const uint32_t d = display.u32;
  const uint32_t player = Read32(d + kDisplayPlayer);
  if (player < 2 || player >= 4) return;
  if (player == 3 && g_player2_display) {  // player 2's x and mirror (player 3 keeps player 1's)
    WriteFloat(d + kDisplayX, ReadFloat(g_player2_display + kDisplayX));
    WriteFloat(d + kDisplayMirror, ReadFloat(g_player2_display + kDisplayMirror));
  }
  WriteFloat(d + kDisplayY, BottomBaseY());
}

// =============================================================================
// 4. Players 3-4's parts mirrored (see "Layout" at the top): the bars drawn
//    lower, the mojo count / multiplier texts above the portrait.
// =============================================================================
extern "C" REX_FUNC(__imp__sub_82267B18);  // CHealthDisplay set-up (slot 1)
extern "C" REX_FUNC(__imp__sub_82370A58);  // Scrooby element: transform matrix = identity
extern "C" REX_FUNC(__imp__sub_82371198);  // Scrooby element: scale f1 around its centre
extern "C" REX_FUNC(__imp__sub_82269940);  // display draw part: the health bar
extern "C" REX_FUNC(__imp__sub_82269DA0);  // display draw part: the special (titan ultimate) bar

namespace more_players_hud {
namespace {
bool IsBottom(uint32_t display) {
  const uint32_t p = Read32(display + kDisplayPlayer);
  return p >= 2 && p < 4;
}
// The bars, mirrored one by one around the portrait's centre: the special
// bar (sub_82269DA0) above, health (sub_82269940) below, each a part's
// shift (its bar with its icon). Health then sits at the same place with or
// without a special bar: nothing moves when a titan is jacked or pocketed.
// (Earlier: the two moved as one block, health still on top, and the block
// depended on whether the player rode a titan, which changes at another
// moment of the jack / pocket animation than the bars: they jumped.)
// The DRAW ORDER is mirrored too: the game draws health, then the special
// bar, so player 1's lower icon (the bolt) lies over the upper one (the
// cross) where they touch; players 3-4 draw the special bar first and
// health after, their lower icon (the cross) on top. Both parts are called
// only from the display's draw (sub_82269480, 0x82269820 / 0x82269828, one
// right after the other): for players 3-4 the health call does nothing and
// the special call draws both.
void DrawPartLowered(void (*part)(PPCContext&, uint8_t*), PPCContext& ctx, uint8_t* base,
                     float top, float bottom) {
  const uint32_t d = ctx.r3.u32;
  const PPCContext saved = ctx;
  const float y = ReadFloat(d + kDisplayY);
  WriteFloat(d + kDisplayY, y - MirrorShift(top, bottom));
  part(ctx, base);
  WriteFloat(d + kDisplayY, y);
  ctx = saved;
}
}  // namespace
}  // namespace more_players_hud

extern "C" REX_FUNC(sub_82269940) {
  if (!IsBottom(ctx.r3.u32)) __imp__sub_82269940(ctx, base);  // players 3-4: from sub_82269DA0
}
extern "C" REX_FUNC(sub_82269DA0) {
  if (!IsBottom(ctx.r3.u32)) {
    __imp__sub_82269DA0(ctx, base);
    return;
  }
  DrawPartLowered(__imp__sub_82269DA0, ctx, base, kSpecialTop, kSpecialBottom);
  DrawPartLowered(__imp__sub_82269940, ctx, base, kHealthTop, kHealthBottom);
}

// THE LEVEL-UP RING upside down. The ring around the portrait that fills up
// toward the next level is drawn by sub_8226A118 (r3 = display): it passes
// the portrait's centre (f1, f2), half sizes (f3, f4), the progress 0-1
// (f5 = display +208) and the texture (+120) to sub_8225F2B8, which draws a
// PIE: a fan of triangles from the centre through the 9 points (centre +-
// size) clockwise from the TOP (y up), cut at the progress. Player 1's combo
// multiplier covers the ring's bottom, the pie's half-way point; mirrored,
// players 3-4's multiplier covers the top, where the pie STARTS, hiding the
// first part of the progress (playtest, 2026-10-05). So their pie is mirrored
// too: a negative vertical half size puts each point below the centre
// instead of above, and the pie fills from the bottom, its half-way point
// under the multiplier as for player 1.
extern "C" REX_FUNC(__imp__sub_8226A118);
extern "C" REX_FUNC(__imp__sub_8225F2B8);
namespace more_players_hud {
namespace {
bool g_mirror_ring = false;  // set around a player 3-4 ring draw (game's main thread)
}  // namespace
}  // namespace more_players_hud
extern "C" REX_FUNC(sub_8226A118) {
  g_mirror_ring = IsBottom(ctx.r3.u32);
  __imp__sub_8226A118(ctx, base);
  g_mirror_ring = false;
}
extern "C" REX_FUNC(sub_8225F2B8) {
  if (g_mirror_ring) ctx.f4.f64 = -ctx.f4.f64;
  __imp__sub_8225F2B8(ctx, base);
}

// Set-up: then players 3-4's texts (multiplier +36, its effect +48, mojo count
// +84) move up above the portrait (y counts up).
//
// The move goes into the element's POSITION (+92 x / +96 y, what the set-up's
// sub_82370E20 writes: HUD units x the constant at 0x8204701C), NOT its
// transform matrix (+16). It used the matrix at first (sub_82370EE8, a
// relative move), but the game RESETS that matrix (sub_82370A58) every time
// it pulses one of these texts: the mojo count when mojo is collected
// (0x822684F4), the multiplier and its effect (0x82269364 / 0x822692F0). So
// the first mojo picked up dropped player 3-4's count back under the
// portrait (a playtest report: "sometimes it doesn't happen"; found
// 2026-10-05 by listing the callers of the reset in CHealthDisplay's code).
// Only the set-up writes the position, so this move stays.
constexpr uint32_t kElementY = 96;
constexpr uint32_t kHudUnitScale = 0x8204701C;  // sub_82370E20's units -> element position
constexpr uint32_t kCountRestScale = 0x8201F594;  // 0.9: the count's size at rest
extern "C" REX_FUNC(sub_82267B18) {
  const uint32_t d = ctx.r3.u32;
  __imp__sub_82267B18(ctx, base);
  if (!IsBottom(d)) return;
  const float multiplier_up = -MirrorShift(kMultiplierTop, kMultiplierBottom);
  const float count_up = -MirrorShift(kCountTop, kCountBottom);
  for (const auto& [field, up] : {std::pair{36u, multiplier_up}, std::pair{48u, multiplier_up},
                                  std::pair{84u, count_up}}) {
    const uint32_t element = Read32(d + field);
    if (element) {
      WriteFloat(element + kElementY, ReadFloat(element + kElementY) + up * ReadFloat(kHudUnitScale));
    }
  }
  // The set-up also shrinks the count to 0.9 around its CENTRE (reset
  // sub_82370A58, then sub_82371198 with the constant at 0x8201F594), and
  // that centre was taken before the move above: the count moved only 0.9 x
  // as far (14 px short) until its first pulse rebuilt the scale around the
  // new place. Done again here, as the set-up does it.
  if (const uint32_t count = Read32(d + 84)) {
    CallGame(__imp__sub_82370A58, ctx, base, count);
    const PPCContext saved = ctx;
    ctx.r3.u64 = count;
    ctx.f1.f64 = ReadFloat(kCountRestScale);
    __imp__sub_82371198(ctx, base);
    ctx = saved;
  }
}

// =============================================================================
// 5. Players 3-4's "counter now" prompt (CCounterOpportunityDisplay, made by
//    more_players_frontend.cpp). Its set-up (slot 1, sub_8225E3E8) finds its
//    text CounterButtonPlayerN from a two-entry name table (0x825A58A0) by its
//    player (+8), then places itself like the HUD: x +24 (player 1's left, player
//    2's right), y +28, mirror +32 (both paths meet at 0x8225E4E4). Without the
//    name, player 3's prompt found no text: null + 0x74 (2026-10-04).
// =============================================================================
void MorePlayersHudNameCounter(PPCRegister& o, PPCRegister& n) { NameFor(kCounterButton, o, n); }
void MorePlayersCounterPosition(PPCRegister& prompt) {
  const uint32_t d = prompt.u32;
  const uint32_t player = Read32(d + 8);
  if (player < 2 || player >= 4) return;
  const uint32_t front_end = Read32(Read32(0x8259B190) + 52);
  if (player == 3) {  // player 2's x and mirror (its prompt, front end +8340 + 40, is set up first)
    const uint32_t p2 = front_end + 8340 + 40;
    WriteFloat(d + 24, ReadFloat(p2 + 24));
    WriteFloat(d + 32, ReadFloat(p2 + 32));
  }
  // as far below the top as the HUD moves: y' = y - (456 - bottom)
  WriteFloat(d + 28, ReadFloat(d + 28) - (kTopY - BottomBaseY()));
}
