// =============================================================================
// ui/quit_game.cpp -- see quit_game.h
// =============================================================================
#include "quit_game.h"

#include <chrono>
#include <cstdlib>
#include <string_view>

#include <rex/logging.h>
#include <rex/ppc/func.h>

#include "../guest_memory.h"
#include "../options/options_page.h"
#include "scrooby.h"

extern "C" REX_FUNC(__imp__sub_8225F758);  // ShowConfirmMessage (front end, r4 text name, f1 timer)
extern "C" REX_FUNC(__imp__sub_823574E8);  // make heap r3 current, returns the previous
extern "C" REX_FUNC(__imp__sub_820CEB58);  // the main menu screen's Update (f1 = frame time)

namespace quit_game {
namespace {

constexpr uint32_t kGameGlobal = 0x8259B190;  // the game (front end at +52)
constexpr uint32_t kMenuFlags = 8592;         // front end: bit 0x40 = menus take input
constexpr uint8_t kMenusTakeInput = 0x40;
constexpr uint32_t kResultFlags = 8593;       // front end: menu result flags
constexpr uint8_t kFlagCancel = 0x04, kFlagAccept = 0x08;
// The question box (options/options_menu.cpp, players/lost_controller.cpp
// have the whole map): front end +172; +4 bit 0x40 = showing, +44 bit 0x40 =
// answered Yes, front end +192 = its text element.
constexpr uint32_t kBox = 172, kBoxFlags = 4, kBoxAnswer = 44, kBoxTextElement = 192;
constexpr uint8_t kBoxShowing = 0x40, kBoxYes = 0x40;
// A text name the box can look up (it shows that text, then ours replaces it).
constexpr uint32_t kAnyTextName = 0x82023570;  // "InGame_Pause_Paused"
constexpr uint32_t kFrontEndHeap = 7;
// Scrooby menu item: +140 its look (the Text drawn); Text +152 its string.
constexpr uint32_t kItemLook = 140, kTextString = 152;
// The main menu SCREEN's action (vtable 0x82023700; Update sub_820CEB58,
// read 2026-10-25): +36 state, 2 opening (sub_820CECE8), 3 open
// (sub_820CEE40: IsButtonPressed B -> 4, A -> 5, either one clears the
// menus' input bit and plays its sound), 4 / 5 closing (sub_820CEF70: the
// list fades out (+68 alpha), then the result flag: 4 cancel, 5 accept).
// The main menu's decision only sees the pick once that fade is over.
// (Found with the debug console's `who` on front end +8593: the first
// version answered the decision, and after No this fade-out kept raising
// "accept" every frame = the question came back at once.)
constexpr uint32_t kScreenState = 36;
constexpr uint32_t kStateOpen = 3, kStatePicked = 5;

constexpr std::string_view kMainMenuPage = "GameStart_Start_Xenon";
constexpr std::string_view kMainMenuMenu = "StartMenu";
constexpr std::u16string_view kItemWords = u"Quit Game";
constexpr std::u16string_view kQuestion = u"Are you sure you want to quit the game?";

bool g_asking = false;
// After No: the press that answered (A on "No", or B) is still the newest
// press while the box closes; the main menu would take it too (B = leave to
// the title, A = pick "Quit Game" again). Its input stays off a moment longer.
using Clock = std::chrono::steady_clock;
Clock::time_point g_settle_until{};
constexpr auto kSettle = std::chrono::milliseconds(300);

uint32_t Read32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Write32(uint8_t* base, uint32_t address, uint32_t v) {
  uint8_t* p = GuestPtr(base, address);
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

uint32_t FrontEnd(const uint8_t* base) {
  const uint32_t game = Read32(base, kGameGlobal);
  return game ? Read32(base, game + 52) : 0;
}

// The main menu's list (0 when that page isn't loaded: in game, boot).
uint32_t MainMenu(PPCContext& ctx, uint8_t* base) {
  const uint32_t page = scrooby::FindPage(ctx, base, kMainMenuPage);
  return page ? scrooby::FindMenu(ctx, base, page, kMainMenuMenu) : 0;
}

// Whether a Text object already says `words` (its string: UTF-16, big-endian).
bool Says(const uint8_t* base, uint32_t text, std::u16string_view words) {
  const uint32_t s = Read32(base, text + kTextString);
  if (!s) return false;
  for (size_t i = 0; i <= words.size(); ++i) {
    const uint8_t* p = GuestPtr(base, s + 2 * uint32_t(i));
    const uint16_t c = uint16_t(p[0] << 8 | p[1]);
    if (c != (i < words.size() ? uint16_t(words[i]) : 0)) return false;
  }
  return true;
}

// The game's question box, our words in it; the list hidden under it (the
// box is see-through) and deaf to the pad / keys (the box reads its own).
void Ask(PPCContext& ctx, uint8_t* base, uint32_t fe) {
  *GuestPtr(base, fe + kBox + kBoxAnswer) &= uint8_t(~kBoxYes);
  // The box copies its text into the current heap: the front end's (7), as
  // the game's own callers make it.
  const uint32_t heap = scrooby::Call(__imp__sub_823574E8, ctx, base, kFrontEndHeap);
  {
    const PPCContext saved = ctx;
    ctx.r3.u64 = fe;
    ctx.r4.u64 = kAnyTextName;
    ctx.f1.f64 = -1.0;  // no timer: waits for an answer
    __imp__sub_8225F758(ctx, base);
    ctx = saved;
  }
  scrooby::Call(__imp__sub_823574E8, ctx, base, heap);
  scrooby::SetText(ctx, base, Read32(base, fe + kBoxTextElement), kQuestion);
  if (const uint32_t menu = MainMenu(ctx, base)) scrooby::SetVisible(base, menu, false);
  *GuestPtr(base, fe + kMenuFlags) &= uint8_t(~kMenusTakeInput);
  g_asking = true;
  REXLOG_INFO("Quit: main menu -> Quit Game (asking)");
}

// Every frame of the main menu screen while asking / settling.
void Follow(PPCContext& ctx, uint8_t* base, uint32_t fe) {
  if (g_asking) {
    if (*GuestPtr(base, fe + kBox + kBoxFlags) & kBoxShowing) return;  // still asking
    g_asking = false;
    if (*GuestPtr(base, fe + kBox + kBoxAnswer) & kBoxYes) {
      REXLOG_INFO("Quit: the player quit the game from the main menu");
      if (auto logger = rex::GetLogger()) logger->flush();
      std::_Exit(0);  // as closing the window does (ReXApp::OnClosing)
    }
    // No: the list back, "Quit Game" still selected.
    if (const uint32_t menu = MainMenu(ctx, base)) scrooby::SetVisible(base, menu, true);
    g_settle_until = Clock::now() + kSettle;
    REXLOG_INFO("Quit: stays (No)");
    return;
  }
  if (g_settle_until != Clock::time_point{} && Clock::now() >= g_settle_until) {
    g_settle_until = {};
    *GuestPtr(base, fe + kMenuFlags) |= kMenusTakeInput;  // the list answers again
  }
}

bool QuitSelected(PPCContext& ctx, uint8_t* base) {
  const uint32_t menu = MainMenu(ctx, base);
  return menu && scrooby::Selection(base, menu) == options_page::kMainMenuQuitItem;
}

}  // namespace

void FrontEndFrame(PPCContext& ctx, uint8_t* base) {
  if (!options_page::MainMenuAvailable()) return;
  const uint32_t menu = MainMenu(ctx, base);
  if (!menu || scrooby::ItemCount(base, menu) <= options_page::kMainMenuQuitItem) return;
  const uint32_t item = scrooby::Item(base, menu, options_page::kMainMenuQuitItem);
  const uint32_t text = item ? Read32(base, item + kItemLook) : 0;
  // Written once per page load (the page keeps our string until it's freed).
  if (text && !Says(base, text, kItemWords)) scrooby::SetText(ctx, base, text, kItemWords);
}

bool HoldDecision(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  if (!g_asking && g_settle_until == Clock::time_point{}) return false;
  if (const uint32_t fe = FrontEnd(base)) {
    *GuestPtr(base, fe + kResultFlags) &= uint8_t(~(kFlagAccept | kFlagCancel));
  }
  return true;
}

// The screen's Update, after the original ran (see sub_820CEB58 below).
void AfterScreenUpdate(PPCContext& ctx, uint8_t* base, uint32_t screen, uint32_t before) {
  if (!options_page::MainMenuAvailable()) return;
  const uint32_t fe = FrontEnd(base);
  if (!fe) return;
  if (before == kStateOpen && Read32(base, screen + kScreenState) == kStatePicked && !g_asking &&
      QuitSelected(ctx, base)) {
    Write32(base, screen + kScreenState, kStateOpen);
    Ask(ctx, base, fe);
  } else {
    Follow(ctx, base, fe);
  }
}

uint32_t ScreenState(const uint8_t* base, uint32_t screen) { return Read32(base, screen + kScreenState); }

}  // namespace quit_game

// The main menu screen's Update: A on "Quit Game" (state 3 -> 5) is taken
// back (state 3: the list doesn't fade out, nothing is picked) and the
// question asked instead. The mouse's click arrives the same way (a pulsed
// A: input/mouse_menus.h), so it's covered too.
extern "C" REX_FUNC(sub_820CEB58) {
  const uint32_t screen = ctx.r3.u32;
  const uint32_t before = quit_game::ScreenState(base, screen);
  __imp__sub_820CEB58(ctx, base);
  const uint64_t result = ctx.r3.u64;  // (the original always answers 1)
  quit_game::AfterScreenUpdate(ctx, base, screen, before);
  ctx.r3.u64 = result;
}
