// =============================================================================
// ui/main_menu_extras.cpp -- see main_menu_extras.h
// =============================================================================
#include "main_menu_extras.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string_view>
#include <unordered_map>

#include <rex/logging.h>

#include <cstring>

#include "../guest_memory.h"
#include "../options/options_page.h"
#include "scrooby.h"
#include "../language/texts.h"

namespace main_menu_extras {
namespace {

constexpr uint32_t kGameGlobal = 0x8259B190;  // the game (front end at +52)
constexpr uint32_t kMenuFlags = 8592;         // front end: bit 0x40 = menus take input
constexpr uint8_t kMenusTakeInput = 0x40;
constexpr uint32_t kResultFlags = 8593;       // front end: menu result flags
constexpr uint8_t kFlagCancel = 0x04, kFlagAccept = 0x08;
constexpr uint32_t kItemLook = 140, kTextString = 152;  // menu item: its text; text: its string
constexpr uint32_t kElementX = 92;            // an element's x (canvas units x the constant below)
constexpr uint32_t kUnitToPosition = 0x8204701C;
// The main menu screen (CMainMenuScreenAction, ui/quit_game.cpp): +36 state,
// 3 open, 4 closing after B, 5 closing after A.
constexpr uint32_t kScreenState = 36;
constexpr uint32_t kStateOpen = 3, kStateBack = 4, kStatePicked = 5;
// Its B (sub_820CEE40: button 12 -> state 4) plays the screen's closing
// "shutter" sound right away (the call at 0x820CEED4): in the Extras set B
// only goes back to the first set, so that one sound is dropped and the
// menu's own move sound plays instead (2026-10-30: it sounded like leaving
// for the title).
constexpr uint32_t kBackSoundReturn = 0x820CEED8;

constexpr std::string_view kMainMenuPage = "GameStart_Start_Xenon";
constexpr std::string_view kMainMenuMenu = "StartMenu";
constexpr std::u16string_view kExtrasWords = trn(u"Extras");  // shown as tr()

// THE SWAP'S TRANSITION (2026-10-30: the new set just popped in): the set
// coming in slides kSlideUnits in from the side (the right going into Extras,
// the left coming back) and fades in, eased out over kSwapTime; the list
// takes no input meanwhile.
using Clock = std::chrono::steady_clock;
constexpr float kSlideUnits = 60.0f;
constexpr auto kSwapTime = std::chrono::milliseconds(220);

bool g_extras = false;  // the Extras set shows
// The Extras item last picked (Players / Credits): back from its screen the
// cursor is there again (the game's menu comes back on its first item).
int g_last_pick = options_page::kMainMenuPlayersItem;
Clock::time_point g_swap_start{};  // {} = no transition running
float g_swap_from = 0;             // +1 = in from the right, -1 = from the left
std::unordered_map<uint32_t, float> g_home_x;  // a text element -> its own x (+92), as the page has it

float ReadF(const uint8_t* base, uint32_t address) {
  const uint8_t* p = GuestPtr(base, address);
  const uint32_t bits = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
void WriteF(uint8_t* base, uint32_t address, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  uint8_t* p = GuestPtr(base, address);
  p[0] = uint8_t(bits >> 24), p[1] = uint8_t(bits >> 16), p[2] = uint8_t(bits >> 8), p[3] = uint8_t(bits);
}
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
// The main menu's list (0 when that page isn't loaded: in game, boot), and
// only with all seven items (our page).
uint32_t MainMenu(PPCContext& ctx, uint8_t* base) {
  if (!options_page::MainMenuAvailable()) return 0;
  const uint32_t page = scrooby::FindPage(ctx, base, kMainMenuPage);
  const uint32_t menu = page ? scrooby::FindMenu(ctx, base, page, kMainMenuMenu) : 0;
  return menu && scrooby::ItemCount(base, menu) == options_page::kMainMenuItemCount ? menu : 0;
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
void Word(PPCContext& ctx, uint8_t* base, uint32_t menu, int index, std::u16string_view words) {
  const uint32_t item = scrooby::Item(base, menu, index);
  const uint32_t text = item ? Read32(base, item + kItemLook) : 0;
  // Written once per page load (the page keeps our string until it's freed).
  if (text && !Says(base, text, words)) scrooby::SetText(ctx, base, text, words);
}
bool InShownSet(int index) {
  return g_extras ? index >= options_page::kMainMenuTopItems : index < options_page::kMainMenuTopItems;
}
// The shown set's items on, the other set's off; the selection inside the
// set; the transition's step (slide + fade) on the shown set.
void ShowSet(PPCContext& ctx, uint8_t* base, uint32_t menu) {
  float t = 1.0f;
  if (g_swap_start != Clock::time_point{}) {
    t = std::chrono::duration<float>(Clock::now() - g_swap_start).count() /
        std::chrono::duration<float>(kSwapTime).count();
    if (t >= 1.0f) {
      t = 1.0f;
      g_swap_start = {};
      if (const uint32_t fe = FrontEnd(base)) *GuestPtr(base, fe + kMenuFlags) |= kMenusTakeInput;
    }
  }
  const float ease = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);  // fast in, settles gently
  const float unit = ReadF(base, kUnitToPosition);
  for (int i = 0; i < options_page::kMainMenuItemCount; ++i) {
    const uint32_t item = scrooby::Item(base, menu, i);
    if (!item) continue;
    const bool shown = InShownSet(i);
    if (const uint32_t text = Read32(base, item + kItemLook)) {
      scrooby::SetVisible(base, text, shown);
      // Its own x, kept the first time it's seen (a new page = new elements).
      const auto home = g_home_x.try_emplace(text, ReadF(base, text + kElementX)).first->second;
      const float dx = shown ? g_swap_from * kSlideUnits * (1.0f - ease) : 0.0f;
      if (unit > 0 && std::isfinite(home)) WriteF(base, text + kElementX, home + dx * unit);
      if (shown) scrooby::SetAlpha(ctx, base, text, uint8_t(std::lround(255.0f * ease)));
    }
    scrooby::SetSelectable(base, item, shown);
  }
  if (!InShownSet(scrooby::Selection(base, menu))) {
    scrooby::Select(ctx, base, menu, g_extras ? g_last_pick : 0);
  }
}
// Swap to the other set, the cursor on `select`; the screen open again, the
// transition started (input back at its end).
void Swap(PPCContext& ctx, uint8_t* base, uint32_t screen, uint32_t menu, bool extras, int select) {
  g_extras = extras;
  g_swap_from = extras ? 1.0f : -1.0f;
  g_swap_start = Clock::now();
  scrooby::Select(ctx, base, menu, select);
  ShowSet(ctx, base, menu);
  Write32(base, screen + kScreenState, kStateOpen);
  if (const uint32_t fe = FrontEnd(base)) {
    // (the screen turned the menus' input off with the press: it stays off
    // through the transition, ShowSet turns it on again)
    *GuestPtr(base, fe + kMenuFlags) &= uint8_t(~kMenusTakeInput);
    *GuestPtr(base, fe + kResultFlags) &= uint8_t(~(kFlagAccept | kFlagCancel));
  }
  REXLOG_INFO("Main menu: {}", extras ? "Extras (Players, Credits)" : "back to the first items");
}

}  // namespace

void FrontEndFrame(PPCContext& ctx, uint8_t* base) {
  const uint32_t menu = MainMenu(ctx, base);
  if (!menu) {
    if (!g_home_x.empty()) g_home_x.clear();  // (the page is gone: its elements too)
    return;
  }
  Word(ctx, base, menu, options_page::kMainMenuExtrasItem, tr(kExtrasWords));
  ShowSet(ctx, base, menu);
}

bool DropSound(uint32_t return_address) {
  if (!g_extras || return_address != kBackSoundReturn) return false;
  REXLOG_INFO("Main menu: B's closing sound dropped (Extras: B only goes back)");
  return true;
}

void AfterScreenUpdate(PPCContext& ctx, uint8_t* base, uint32_t screen, uint32_t before) {
  if (before != kStateOpen) return;
  const uint32_t after = Read32(base, screen + kScreenState);
  if (after != kStatePicked && after != kStateBack) return;
  const uint32_t menu = MainMenu(ctx, base);
  if (!menu) return;
  const int picked = scrooby::Selection(base, menu);
  if (after == kStatePicked && (picked == options_page::kMainMenuPlayersItem ||
                                picked == options_page::kMainMenuCreditsItem)) {
    g_last_pick = picked;  // (leaves the menu as before)
  }
  if (after == kStatePicked && picked == options_page::kMainMenuExtrasItem) {
    Swap(ctx, base, screen, menu, true, options_page::kMainMenuPlayersItem);
  } else if (g_extras && after == kStateBack) {
    // B in the Extras set: the first set again, on Extras (its closing sound
    // was dropped: DropSound; the menu's move sound instead).
    scrooby::PlaySound(ctx, base, scrooby::Sound::kMove);
    Swap(ctx, base, screen, menu, false, options_page::kMainMenuExtrasItem);
  }
}

}  // namespace main_menu_extras
