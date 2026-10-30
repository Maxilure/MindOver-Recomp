// =============================================================================
// ui/hud_hide.cpp -- see hud_hide.h
// =============================================================================
#include "hud_hide.h"

#include <array>
#include <atomic>
#include <chrono>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../achievements/achievements.h"
#include "../cheats/cheats.h"
#include "hud_scale.h"
#include "scrooby.h"

namespace hud_hide {
namespace {

// The HUD screen's pages (findings/30 s.6), plus the port's players 3-4
// corners (players/more_players_hud.cpp). NOT InGame_MessageBar: it says the
// HUD is hidden, and the game's messages keep showing there.
constexpr const char* kPages[] = {"InGame",
                                  "InGame_Announcement",
                                  "InGame_CoOp_Player1",
                                  "InGame_CoOp_Player2",
                                  "InGame_CoOp_Player3",
                                  "InGame_CoOp_Player4",
                                  "InGame_CollectableCounter"};
constexpr size_t kPageCount = sizeof(kPages) / sizeof(kPages[0]);
// Found pages (0 = none), read by the page draw on the same (game) thread.
std::array<std::atomic<uint32_t>, kPageCount> g_pages{};
std::chrono::steady_clock::time_point g_last_find{};

bool IsHudPage(uint32_t page) {
  for (const auto& p : g_pages) {
    if (p.load(std::memory_order_relaxed) == page) return true;
  }
  return false;
}

}  // namespace

// The HUD size (hud_scale.h): "Join Game" pages = their player's corner, the
// collectable counter = the top centre, the others (InGame: its per-player
// texts are scaled one by one; announcements) untouched.
int WhereOfPage(uint32_t page) {
  for (size_t i = 0; i < kPageCount; ++i) {
    if (g_pages[i].load(std::memory_order_relaxed) != page) continue;
    if (i >= 2 && i <= 5) return int(i) - 2;          // InGame_CoOp_Player1-4
    if (i == 6) return hud_scale::kTopCentre;         // InGame_CollectableCounter
    return hud_scale::kNone;
  }
  return hud_scale::kNone;
}

void Toggle() {
  const bool hide = !cheats::HudHidden();
  cheats::SetHudHidden(hide);  // the switch only: no cheat is enabled by this
  if (hide) {
    // The key the bind has now (F2 unless changed in settings.toml).
    std::string key = rex::cvar::GetFlagByName("bind_hide_hud");
    if (key.empty()) key = "F2";
    const std::string line = "Press " + key + " to show it again";
    achievements::ShowNotice(u"HUD Hidden", std::u16string(line.begin(), line.end()));
  } else {
    achievements::DropWaitingNotice(u"HUD Hidden");  // not shown yet: no longer true
  }
  REXLOG_INFO("HUD: {} (key, not a cheat)", hide ? "hidden" : "shown");
}

void Frame(PPCContext& ctx, uint8_t* base) {
  const auto now = std::chrono::steady_clock::now();
  if (now - g_last_find < std::chrono::seconds(1)) return;
  g_last_find = now;
  for (size_t i = 0; i < kPageCount; ++i) g_pages[i] = scrooby::FindPage(ctx, base, kPages[i]);
}

}  // namespace hud_hide

namespace {
// A big-endian guest word (TranslateVirtual: right on every system).
uint32_t Read32(uint32_t a) {
  const uint8_t* p = rex::system::kernel_memory()->TranslateVirtual<const uint8_t*>(a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
}  // namespace

// --- The draws skipped while the HUD is hidden (hud_hide.h) -------------------

// Parent@frontend slot 6 (pages, groups, layers): only the HUD's pages.
extern "C" REX_FUNC(__imp__sub_82371270);
extern "C" REX_FUNC(sub_82371270) {
  const uint32_t page = ctx.r3.u32;
  if (cheats::HudHidden() && hud_hide::IsHudPage(ctx.r3.u32)) return;
  hud_scale::Scope scale(ctx, base, page == 0 ? hud_scale::kNone : hud_hide::WhereOfPage(page));
  __imp__sub_82371270(ctx, base);
}
// CComboCounter / CCounterOpportunityDisplay / CPlayerIdentifierArrow draw (slot 4).
extern "C" REX_FUNC(__imp__sub_8225CF30);
extern "C" REX_FUNC(sub_8225CF30) {
  if (cheats::HudHidden()) return;
  // HUD size: the combo meter's player at +3148.
  hud_scale::Scope scale(ctx, base, int32_t(Read32(ctx.r3.u32 + 3148)));
  __imp__sub_8225CF30(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_8225E658);
extern "C" REX_FUNC(sub_8225E658) {
  if (cheats::HudHidden()) return;
  // HUD size: the prompt's player at +8; its Y (a text on the page InGame,
  // drawn by the page) at +16.
  const int player = int32_t(Read32(ctx.r3.u32 + 8));
  hud_scale::NoteText(Read32(ctx.r3.u32 + 16), player);
  hud_scale::Scope scale(ctx, base, player);
  __imp__sub_8225E658(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_8226D358);
extern "C" REX_FUNC(sub_8226D358) {
  if (cheats::HudHidden()) return;
  __imp__sub_8226D358(ctx, base);
}
// CStoredActorDisplay draw (slot 4, sub_8226E688): the POCKETED mutant's
// portrait and bars (the titan kept for later with RB), placed apart from the
// players' HUD: still on screen with the first version of this (2026-10-29,
// screenshot from play). CTimerDisplay draw (slot 4, sub_8226EBF8: a tail
// call into sub_8226ECE0) = a timed challenge's clock. CHealthDisplay draw
// (slot 4, sub_822685F0): normally only called by the HUD controller's draw
// (already skipped); covered too in case anything else draws one.
extern "C" REX_FUNC(__imp__sub_8226E688);
extern "C" REX_FUNC(sub_8226E688) {
  if (cheats::HudHidden()) return;
  hud_scale::Scope scale(ctx, base, hud_scale::kTopCentre);
  __imp__sub_8226E688(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_8226EBF8);
extern "C" REX_FUNC(sub_8226EBF8) {
  if (cheats::HudHidden()) return;
  hud_scale::Scope scale(ctx, base, hud_scale::kTopCentre);
  __imp__sub_8226EBF8(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_822685F0);
extern "C" REX_FUNC(sub_822685F0) {
  if (cheats::HudHidden()) return;
  // HUD size: the display's player at +144; its texts on the page InGame
  // (multiplier +36, its effect +48, mojo count +84) go with it.
  const uint32_t d = ctx.r3.u32;
  const int player = int32_t(Read32(d + 144));
  for (uint32_t field : {36u, 48u, 84u}) hud_scale::NoteText(Read32(d + field), player);
  hud_scale::Scope scale(ctx, base, player);
  __imp__sub_822685F0(ctx, base);
}
