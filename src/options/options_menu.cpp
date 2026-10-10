// =============================================================================
// options/options_menu.cpp -- see options_menu.h
// =============================================================================
#include "options_menu.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <string>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../guest_memory.h"
#include "../ui/scrooby.h"
#include "options_page.h"
#include "options_settings.h"

extern "C" REX_FUNC(__imp__sub_820D2828);  // CMenuAction::Enter
extern "C" REX_FUNC(__imp__sub_820D2A00);  // CMenuAction::Update
extern "C" REX_FUNC(__imp__sub_820D2F08);  // CMenuAction::Exit
// IsButtonPressed WITH our owner rules (players/menu_input.cpp hooks it):
// the menu's owner answers, as for every other in-game menu.
extern "C" REX_FUNC(sub_822652B0);
extern "C" REX_FUNC(__imp__sub_821241E8);  // FindPage(layer, page name): a loaded Scrooby page

namespace options_menu {
namespace {

using options_page::kRows;
using options_page::kStars;
using options_settings::Env;
using options_settings::Kind;
using options_settings::Row;

constexpr uint32_t kGameGlobal = 0x8259B190;  // the game (front end at +52)
constexpr uint32_t kOwner = 8536;             // front end: the menu's owner (-1 = none)
constexpr uint32_t kMenuFlags = 8592;         // front end: bit 0x40 = menus take input
// The hash of "InGameOptionsMenu" (64-bit, filled at start-up by 0x824A1ED0):
// CMenuAction's special code runs for a menu with this name.
constexpr uint32_t kOptionsMenuKey = 0x825A0598;
// The hash of "OptionsMenu" (0x824A1E98): the PS2 Options page's menu, our
// page's menu in the main menu (options_page.h).
constexpr uint32_t kMainOptionsMenuKey = 0x825A0580;
// Front end +8593: result flags (bit 2 = cancel: OptionsScreen's decision
// sub_8211B818 takes ExitBack 106 -> the main menu; bit 3 = accept).
constexpr uint32_t kResultFlags = 8593;
constexpr uint8_t kFlagCancel = 0x04, kFlagAccept = 0x08;
// CMenuAction: +8 its track (the state's FEMenu data: +20 the page's name
// hash, +28 the menu's), +16 the page, +20 the menu (Scrooby objects, found
// by those hashes in Enter).
constexpr uint32_t kActionTrack = 8;
constexpr uint32_t kTrackMenuHash = 28;
constexpr uint32_t kActionPage = 16;
constexpr uint32_t kActionMenu = 20;
// A Scrooby menu item: +140 its look (the label text), +144 its value text.
constexpr uint32_t kItemLook = 140;
constexpr uint32_t kItemValue = 144;

// IsButtonPressed's buttons for left / right in menus (CMenuAction
// sub_820D2A00 asks these: 27 -> previous value + "down_left", 40 -> next).
constexpr uint32_t kButtonLeft = 27;
constexpr uint32_t kButtonRight = 40;

// XINPUT_GAMEPAD buttons.
constexpr uint16_t kPadBack = 0x0020;
constexpr uint16_t kPadLB = 0x0100;
constexpr uint16_t kPadRB = 0x0200;
constexpr uint16_t kPadA = 0x1000;
constexpr uint16_t kPadB = 0x2000;
constexpr uint16_t kPadX = 0x4000;
constexpr char16_t kGlyphX = u'\u00B3';  // the font's X button picture

// The stars' colours, as the original screen sets them (sub_820D2F80): lit =
// the colour word at 0x825078E4, unlit = translucent white.
constexpr uint32_t kStarLitColour = 0x825078E4;
constexpr uint32_t kStarUnlitColour = 0x4BFFFFFF;

// The description / warning / selected tab's size (findings/30 s.4: menu
// rows are Titans_Small at full size; the explanation reads as small print).
constexpr float kSmallPrint = 0.72f;
constexpr float kGuideScale = 0.6f;  // the Brightness guide (options_page.cpp)
// The tabs: the ones beside smaller than the rows, the current one larger,
// like a selected menu item.
constexpr float kTab = 0.8f;
constexpr float kSelectedTab = 0.97f;

// The selector arrows: the map's down arrow turned by this to point left
// (the right one the other way).
constexpr float kArrowLeftDegrees = 90.0f;  // (-90 pointed right: checked on a photo)

// Button pictures of the font (findings/23 s.1.2).
constexpr char16_t kGlyphLB = u'¾';
constexpr char16_t kGlyphRB = u'¸';

// The controller states the game read last (SeePad), per user.
std::array<std::atomic<uint16_t>, 4> g_pad{};

// The open screen (game main thread only).
struct Screen {
  uint32_t action = 0;  // our CMenuAction while the screen is up
  uint32_t page = 0, menu = 0;
  int tab = 0;          // kept between visits
  int row_page = 0;     // which kRows rows of the tab are shown (pages)
  int pending_select = -1;  // a page turned inside the menu's own move: select this after
  int selection = -1;   // the row the texts were made for
  int focused = -1;     // the row whose focus() was told "on"
  uint16_t seen = 0;    // the owner's buttons at the last Update
  bool main_menu = false;  // the main menu's copy (B is ours to answer; no Music preview)
  // Elements, found once per visit.
  uint32_t tab_prev = 0, tab_text = 0, tab_next = 0;  // the three tab names (options_page.h)
  std::array<uint32_t, kRows> label{}, value{};
  std::array<std::array<uint32_t, kStars>, kRows> star{};
  std::array<uint32_t, kRows> arrow_left{}, arrow_right{};
  uint32_t desc = 0, warn = 0, lb = 0, rb = 0, guide = 0;
  uint32_t page_up = 0, page_down = 0, page_text = 0;  // the page markers beside the glass
  // The game's button prompts (page FE_Buttons, always loaded): the lower
  // right corner = a row's X action ("Dual Mode"), when it has one.
  uint32_t x_button = 0, x_text = 0;
  bool x_showing = false;
};
Screen g;
std::atomic<bool> g_open{false};

uint32_t Read32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = GuestPtr(base, a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint64_t Read64(const uint8_t* base, uint32_t a) {
  return uint64_t(Read32(base, a)) << 32 | Read32(base, a + 4);
}
void Write64(uint8_t* base, uint32_t a, uint64_t v) {
  uint8_t* p = GuestPtr(base, a);
  for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (56 - 8 * i));
}
uint32_t FrontEnd(const uint8_t* base) {
  const uint32_t game = Read32(base, kGameGlobal);
  return game ? Read32(base, game + 52) : 0;
}
int Owner(const uint8_t* base) {
  const uint32_t fe = FrontEnd(base);
  const int owner = fe ? int(int32_t(Read32(base, fe + kOwner))) : -1;
  return owner >= 0 && owner < 4 ? owner : 0;
}

// Our page's menu: a track naming InGameOptionsMenu, with our page in the
// game. Asked BEFORE Enter (which only then finds the menu itself).
// In the main menu: the menu named OptionsMenu on the patched main menu page.
bool IsOurs(const uint8_t* base, uint32_t action, bool* main_menu) {
  if (!action) return false;
  const uint32_t track = Read32(base, action + kActionTrack);
  if (!track) return false;
  const uint64_t menu = Read64(base, track + kTrackMenuHash);
  const uint64_t in_game = Read64(base, kOptionsMenuKey);
  const uint64_t main = Read64(base, kMainOptionsMenuKey);
  if (options_page::Available() && in_game && menu == in_game) {
    *main_menu = false;
    return true;
  }
  if (options_page::MainMenuAvailable() && main && menu == main) {
    *main_menu = true;
    return true;
  }
  return false;
}

// Runs a CMenuAction method with the InGameOptionsMenu key hidden, so its
// special code (stars and Invert Axis by row number) skips our menu.
void RunWithoutSpecialCode(void (*original)(PPCContext&, uint8_t*), PPCContext& ctx,
                           uint8_t* base) {
  const uint64_t key = Read64(base, kOptionsMenuKey);
  Write64(base, kOptionsMenuKey, 0);
  original(ctx, base);
  Write64(base, kOptionsMenuKey, key);
}

bool Pressed(PPCContext& ctx, uint8_t* base, uint32_t button) {
  const uint32_t fe = FrontEnd(base);
  if (!fe) return false;
  const PPCContext saved = ctx;
  ctx.r3.u64 = fe;
  ctx.r4.u64 = button;
  ctx.r5.u64 = 0;
  ctx.r6.u64 = 0xFFFFFFFFu;  // anyone (= the menu's owner, menu_input.cpp)
  ctx.r7.u64 = 0;
  ctx.r8.u64 = 0;
  sub_822652B0(ctx, base);
  const bool pressed = (ctx.r3.u32 & 0xFF) != 0;
  ctx = saved;
  return pressed;
}

// Scrooby texts don't wrap by themselves: lines are broken at spaces so
// none is longer than this many characters (at kSmallPrint: ~12 px a
// character at 720p, the panel's inside is ~680 px wide; measured on a
// photo, 2026-10-10).
constexpr size_t kSmallPrintChars = 54;

std::u16string Wrap(const std::u16string& text, size_t width) {
  std::u16string out, line;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find(u' ', start);
    if (end == std::u16string::npos) end = text.size();
    const std::u16string word = text.substr(start, end - start);
    if (!line.empty() && line.size() + 1 + word.size() > width) {
      out += line + u'\n';
      line.clear();
    }
    line += (line.empty() ? u"" : u" ") + word;
    start = end + 1;
  }
  return out + line;
}

const std::vector<Row>& Rows() { return options_settings::Tabs()[size_t(g.tab)].rows; }

// PAGES: a tab shows kRows rows at a time; the screen's row r is the tab's
// row First() + r.
int PageCount() { return std::max(1, (int(Rows().size()) + kRows - 1) / kRows); }
int First() { return g.row_page * kRows; }
int VisibleCount() { return std::min(kRows, int(Rows().size()) - First()); }
// The tab's row under the cursor (screen row `sel`), or null.
const Row* RowOnScreen(int sel) {
  const auto& rows = Rows();
  const int i = First() + sel;
  return sel >= 0 && sel < VisibleCount() && i < int(rows.size()) ? &rows[size_t(i)] : nullptr;
}
// The focused row (an index into the tab's rows) hears "off".
void Unfocus(Env& env) {
  const auto& rows = Rows();
  if (g.focused >= 0 && g.focused < int(rows.size()) && rows[size_t(g.focused)].focus) {
    rows[size_t(g.focused)].focus(env, false);
  }
  g.focused = -1;
}

void FindElements(PPCContext& ctx, uint8_t* base) {
  using scrooby::FindPicture;
  using scrooby::FindText;
  g.tab_prev = FindText(ctx, base, g.page, "OptTabPrev");
  g.tab_text = FindText(ctx, base, g.page, "OptTab");
  g.tab_next = FindText(ctx, base, g.page, "OptTabNext");
  for (int r = 0; r < kRows; ++r) {
    // A menu item's texts aren't found by name in the page: the item holds
    // them (+140 what is drawn, +144 its value; both Text objects, read
    // 2026-10-10 with the debug console).
    const uint32_t item = scrooby::Item(base, g.menu, r);
    g.label[r] = item ? Read32(base, item + kItemLook) : 0;
    g.value[r] = item ? Read32(base, item + kItemValue) : 0;
    for (int k = 0; k < kStars; ++k) {
      g.star[r][k] = FindPicture(ctx, base, g.page, fmt::format("OptStar{}_{}", r, k));
    }
    g.arrow_left[r] = FindPicture(ctx, base, g.page, fmt::format("OptArrowL{}", r));
    g.arrow_right[r] = FindPicture(ctx, base, g.page, fmt::format("OptArrowR{}", r));
  }
  g.desc = FindText(ctx, base, g.page, "OptDesc");
  g.guide = FindPicture(ctx, base, g.page, "OptGuide");
  g.warn = FindText(ctx, base, g.page, "OptWarn");
  g.lb = FindText(ctx, base, g.page, "OptTabLB");
  g.rb = FindText(ctx, base, g.page, "OptTabRB");
  g.page_up = FindPicture(ctx, base, g.page, "OptPageUp");
  g.page_down = FindPicture(ctx, base, g.page, "OptPageDown");
  g.page_text = FindText(ctx, base, g.page, "OptPage");
  // FE_Buttons, found as the game's GetMenuIndex does (layer 4, then 0).
  uint32_t buttons = scrooby::Call(__imp__sub_821241E8, ctx, base, 4, scrooby::GuestAscii("FE_Buttons"));
  if (!buttons) buttons = scrooby::Call(__imp__sub_821241E8, ctx, base, 0, scrooby::GuestAscii("FE_Buttons"));
  g.x_button = FindText(ctx, base, buttons, "LowerRightButton");
  g.x_text = FindText(ctx, base, buttons, "LowerRightText");
  REXLOG_INFO("Options: page {:08X} menu {:08X} ({} items), label0 {:08X} value0 {:08X} star0 {:08X} "
              "item0 {:08X} desc {:08X}",
              g.page, g.menu, scrooby::ItemCount(base, g.menu), g.label[0], g.value[0], g.star[0][0],
              scrooby::Item(base, g.menu, 0), g.desc);
}

// The texts that never change while the screen is up.
void SetUpFixedTexts(PPCContext& ctx, uint8_t* base) {
  scrooby::SetScale(ctx, base, g.tab_prev, kTab);
  scrooby::SetScale(ctx, base, g.tab_text, kSelectedTab);
  scrooby::SetScale(ctx, base, g.tab_next, kTab);
  scrooby::SetText(ctx, base, g.lb, std::u16string(1, kGlyphLB));
  scrooby::SetText(ctx, base, g.rb, std::u16string(1, kGlyphRB));
  // The arrows are the map screen's DOWN arrow: turned to point left /
  // right (from a fresh transform: the page lives on between visits).
  for (int r = 0; r < kRows; ++r) {
    scrooby::SetScale(ctx, base, g.arrow_left[r], 1.0f);
    scrooby::Rotate(ctx, base, g.arrow_left[r], kArrowLeftDegrees);
    scrooby::SetScale(ctx, base, g.arrow_right[r], 1.0f);
    scrooby::Rotate(ctx, base, g.arrow_right[r], -kArrowLeftDegrees);
  }
  // The page arrows: the same down arrow, the upper one turned over.
  scrooby::SetScale(ctx, base, g.page_up, 1.0f);
  scrooby::Rotate(ctx, base, g.page_up, 180.0f);
  scrooby::SetScale(ctx, base, g.page_down, 1.0f);
  scrooby::SetScale(ctx, base, g.page_text, kSmallPrint);
  scrooby::SetScale(ctx, base, g.guide, kGuideScale);
  scrooby::SetScale(ctx, base, g.desc, kSmallPrint);
  scrooby::SetScale(ctx, base, g.warn, kSmallPrint);
}

// Everything that depends on the tab, the values and the selection.
void Refresh(Env& env) {
  PPCContext& ctx = env.ctx;
  uint8_t* base = env.base;
  const auto& tabs = options_settings::Tabs();
  const int count = int(tabs.size());
  scrooby::SetText(ctx, base, g.tab_prev, tabs[size_t((g.tab + count - 1) % count)].name);
  scrooby::SetText(ctx, base, g.tab_text, tabs[size_t(g.tab)].name);
  scrooby::SetText(ctx, base, g.tab_next, tabs[size_t((g.tab + 1) % count)].name);
  const int visible = VisibleCount();
  int sel = scrooby::Selection(base, g.menu);
  if (sel < 0 || sel >= visible) {
    sel = 0;
    scrooby::Select(ctx, base, g.menu, 0);
  }
  const uint32_t lit = Read32(base, kStarLitColour);
  for (int r = 0; r < kRows; ++r) {
    const Row* row = RowOnScreen(r);
    const bool used = row != nullptr;
    scrooby::SetSelectable(base, scrooby::Item(base, g.menu, r), used);
    scrooby::SetVisible(base, g.label[r], used);
    scrooby::SetVisible(base, g.value[r], used);
    const bool stars = row && row->kind == Kind::kStars;
    const int level = stars ? row->get(env) : 0;
    for (int k = 0; k < kStars; ++k) {
      const uint32_t s = g.star[r][k];
      scrooby::SetVisible(base, s, stars);
      if (!stars) continue;
      scrooby::SetFrame(ctx, base, s, k < level ? 0 : 1);
      scrooby::SetColour(ctx, base, s, k < level ? lit : kStarUnlitColour);
    }
    // The selector's arrows: on the selected row only, where it can still move.
    bool can_left = false, can_right = false;
    if (row) {
      scrooby::SetText(ctx, base, g.label[r], row->label);
      std::u16string value;
      if (row->kind == Kind::kChoice) {
        const auto values = row->values(env);
        const int i = std::clamp(row->get(env), 0, int(values.size()) - 1);
        value = values.empty() ? u"" : values[size_t(i)];
        can_left = i > 0;
        can_right = i + 1 < int(values.size());
      } else if (stars) {
        can_left = level > 0;
        can_right = level < kStars;
      }
      scrooby::SetText(ctx, base, g.value[r], value);
    }
    scrooby::SetVisible(base, g.arrow_left[r], r == sel && can_left);
    scrooby::SetVisible(base, g.arrow_right[r], r == sel && can_right);
  }
  // The page markers: only for a tab with more than one page; an arrow where
  // there are more rows that way, and "page / pages".
  const int pages = PageCount();
  scrooby::SetVisible(base, g.page_up, pages > 1 && g.row_page > 0);
  scrooby::SetVisible(base, g.page_down, pages > 1 && g.row_page + 1 < pages);
  scrooby::SetVisible(base, g.page_text, pages > 1);
  if (pages > 1) {
    const std::string text = fmt::format("{}/{}", g.row_page + 1, pages);
    scrooby::SetText(ctx, base, g.page_text, std::u16string(text.begin(), text.end()));
  }
  // The selected row's description and warning.
  const Row& row = *RowOnScreen(sel);
  scrooby::SetVisible(base, g.guide, row.guide);
  const int current = row.get ? row.get(env) : 0;
  scrooby::SetText(ctx, base, g.desc,
                   row.describe ? Wrap(row.describe(env, current), kSmallPrintChars) : u"");
  const std::u16string warning = row.warn ? row.warn(env, current) : u"";
  scrooby::SetText(ctx, base, g.warn, warning);
  scrooby::SetVisible(base, g.warn, !warning.empty());
  // The row's X action in the lower right corner (the main menu's copy had
  // "Select A" there: blank unless the row offers something).
  const bool x = row.x_action && row.x_shown && row.x_shown(env, current);
  if (x != g.x_showing || x) {
    scrooby::SetText(ctx, base, g.x_button, x ? std::u16string(1, kGlyphX) : u"");
    scrooby::SetText(ctx, base, g.x_text, x ? row.x_label : u"");
    g.x_showing = x;
  }
  // The Music row plays its preview while it has the cursor (in game: the
  // main menu has its own music playing).
  const int focus = First() + sel;
  if (focus != g.focused && !g.main_menu) {
    Unfocus(env);
    if (row.focus) row.focus(env, true);
    g.focused = focus;
  }
  g.selection = sel;
}

void SwitchTab(Env& env, int delta) {
  // Leaving the tab: its focused row hears "off" first.
  Unfocus(env);
  const int count = int(options_settings::Tabs().size());
  g.tab = (g.tab + delta + count) % count;
  g.row_page = 0;
  scrooby::Select(env.ctx, env.base, g.menu, 0);
  scrooby::PlaySound(env.ctx, env.base,
                     delta < 0 ? scrooby::Sound::kMove : scrooby::Sound::kChange);
  Refresh(env);
}

// Left / right on the selected row; returns whether the value changed.
bool ChangeValue(Env& env, int delta) {
  const Row* on_screen = RowOnScreen(scrooby::Selection(env.base, g.menu));
  if (!on_screen) return false;
  const Row& row = *on_screen;
  if (row.kind == Kind::kAction || !row.get || !row.set) return false;
  const int now = row.get(env);
  const int last = row.kind == Kind::kStars ? kStars : int(row.values(env).size()) - 1;
  const int next = std::clamp(now + delta, 0, last);
  if (next == now) return false;
  row.set(env, next);
  // Volume rows play their own sound (options_settings.cpp); the others the
  // menu's, as a value list in the game does.
  if (row.kind == Kind::kChoice) {
    scrooby::PlaySound(env.ctx, env.base,
                       delta < 0 ? scrooby::Sound::kMove : scrooby::Sound::kChange);
  }
  return true;
}

}  // namespace

bool TurnPage(uint32_t menu, int direction, uint8_t* base) {
  if (!g.action || menu != g.menu) return false;
  const int pages = PageCount();
  if (pages <= 1) return false;  // the menu's own wrap-around
  const int sel = scrooby::Selection(base, g.menu);
  if (direction > 0 && sel >= VisibleCount() - 1) {
    g.row_page = (g.row_page + 1) % pages;
    g.pending_select = 0;
    return true;
  }
  if (direction < 0 && sel <= 0) {
    g.row_page = (g.row_page + pages - 1) % pages;
    g.pending_select = VisibleCount() - 1;
    return true;
  }
  return false;
}

void Register() { options_page::Register(); }

void SeePad(uint32_t user, const uint8_t* gamepad) {
  if (user < g_pad.size()) {
    g_pad[user].store(uint16_t(gamepad[0] << 8 | gamepad[1]), std::memory_order_relaxed);
  }
}

bool Open() { return g_open.load(std::memory_order_relaxed); }

}  // namespace options_menu

using namespace options_menu;

extern "C" REX_FUNC(sub_820D2828) {
  const uint32_t action = ctx.r3.u32;
  bool main_menu = false;
  if (!IsOurs(base, action, &main_menu)) {
    __imp__sub_820D2828(ctx, base);
    return;
  }
  RunWithoutSpecialCode(__imp__sub_820D2828, ctx, base);
  g.action = action;
  g.main_menu = main_menu;
  if (main_menu) {
    // A cancel / accept left over from the main menu must not end this screen.
    if (const uint32_t fe = FrontEnd(base)) {
      *GuestPtr(base, fe + kResultFlags) &= uint8_t(~(kFlagCancel | kFlagAccept));
    }
  }
  g.page = Read32(base, action + kActionPage);
  g.menu = Read32(base, action + kActionMenu);
  g.selection = -1;
  g.focused = -1;
  g.row_page = 0;
  g.pending_select = -1;
  g.x_showing = true;  // the first Refresh writes the corner either way
  const int owner = Owner(base);
  g.seen = g_pad[size_t(owner)].load(std::memory_order_relaxed);
  FindElements(ctx, base);
  SetUpFixedTexts(ctx, base);
  scrooby::Select(ctx, base, g.menu, 0);
  Env env{ctx, base, owner};
  Refresh(env);
  g_open = true;
  REXLOG_INFO("Options: screen opened ({}) by player {} (tab {})", main_menu ? "main menu" : "in game",
              owner + 1, g.tab);
}

extern "C" REX_FUNC(sub_820D2A00) {
  const uint32_t action = ctx.r3.u32;
  if (!g.action || action != g.action) {
    __imp__sub_820D2A00(ctx, base);
    return;
  }
  const uint32_t fe = FrontEnd(base);
  const int owner = Owner(base);
  RunWithoutSpecialCode(__imp__sub_820D2A00, ctx, base);
  const PPCContext saved = ctx;  // our calls below must not change what Update returned
  if (!fe || !(*GuestPtr(base, fe + kMenuFlags) & 0x40)) return;
  Env env{ctx, base, owner};
  if (g.pending_select >= 0) {
    // A page turned in the menu's own up / down (TurnPage): the new page's rows
    // first, then the cursor on its first / last row.
    g.selection = -1;
    scrooby::Select(ctx, base, g.menu, 0);
    Refresh(env);
    scrooby::Select(ctx, base, g.menu, g.pending_select);
    g.pending_select = -1;
    Refresh(env);
  }
  const uint16_t pad = g_pad[size_t(owner)].load(std::memory_order_relaxed);
  const uint16_t pressed = pad & ~g.seen;
  g.seen = pad;
  bool refresh = false;
  if (pressed & kPadLB) {
    SwitchTab(env, -1);
  } else if (pressed & (kPadRB | kPadBack)) {
    SwitchTab(env, +1);
  }
  if (Pressed(ctx, base, kButtonLeft)) {
    refresh |= ChangeValue(env, -1);
  } else if (Pressed(ctx, base, kButtonRight)) {
    refresh |= ChangeValue(env, +1);
  }
  // The main menu's copy: B = back (the PS2 screen's action, which said so,
  // doesn't run: options_menu.h); the state's decision takes it from there.
  if (g.main_menu && (pressed & kPadB)) {
    *GuestPtr(base, fe + kResultFlags) |= kFlagCancel;
  }
  if (pressed & kPadX) {
    const Row* row = RowOnScreen(scrooby::Selection(base, g.menu));
    if (row && row->x_action && row->x_shown && row->x_shown(env, row->get ? row->get(env) : 0)) {
      row->x_action(env);
      scrooby::PlaySound(ctx, base, scrooby::Sound::kChange);
    }
  }
  if (pressed & kPadA) {
    const Row* row = RowOnScreen(scrooby::Selection(base, g.menu));
    if (row && row->kind == Kind::kAction && row->activate) row->activate(env);
  }
  if (refresh || scrooby::Selection(base, g.menu) != g.selection) Refresh(env);
  ctx = saved;
}

extern "C" REX_FUNC(sub_820D2F08) {
  const uint32_t action = ctx.r3.u32;
  if (g.action && action == g.action) {
    Env env{ctx, base, Owner(base)};
    Unfocus(env);
    if (g.x_showing) {  // the corner back to blank (the next screen sets its own)
      scrooby::SetText(ctx, base, g.x_button, u"");
      scrooby::SetText(ctx, base, g.x_text, u"");
      g.x_showing = false;
    }
    g.action = 0;
    g_open = false;
    options_settings::SaveChanged();
    REXLOG_INFO("Options: screen closed");
  }
  __imp__sub_820D2F08(ctx, base);
}

// =============================================================================
// THE MAIN MENU'S "Options" (options_page.h): its decision and the PS2 screen
// =============================================================================
// The main menu's decision (MainMenuScreen, node 68: sub_8211B6C8) turns the
// selected item into an exit when the accept flag is up: GetMenuIndex
// (sub_821239C8, page GameStart_Start_Xenon, menu StartMenu) 0-3 -> 86 New
// Game, 87 Load Game, 88 Credits, 89 Calibration. Ours: New Game, Load Game,
// Options, Credits (Calibration gave its place); 2 = exit 77, the PS2 menu's ExitOptions -> the
// OptionsScreen state 96 (the engine follows any exit's target, not only a
// child of the current state: saves/rename_screen.cpp).
extern "C" REX_FUNC(__imp__sub_8211B6C8);  // MainMenuScreen's decision
extern "C" REX_FUNC(__imp__sub_821239C8);  // GetMenuIndex(_, page name, menu name)

namespace {
constexpr uint32_t kMainMenuPageName = 0x82023674;  // "GameStart_Start_Xenon"
constexpr uint32_t kMainMenuMenuName = 0x820239D8;  // "StartMenu"
constexpr int32_t kExitNewGame = 86, kExitLoadGame = 87, kExitCredits = 88;
constexpr int32_t kExitOptions = 77;  // the PS2 main menu's ExitOptions -> OptionsScreen 96
}  // namespace

extern "C" REX_FUNC(sub_8211B6C8) {
  const uint32_t fe = FrontEnd(base);
  if (!options_page::MainMenuAvailable() || !fe ||
      !(*GuestPtr(base, fe + kResultFlags) & kFlagAccept)) {
    __imp__sub_8211B6C8(ctx, base);
    return;
  }
  const PPCContext saved = ctx;
  ctx.r4.u64 = kMainMenuPageName;
  ctx.r5.u64 = kMainMenuMenuName;
  __imp__sub_821239C8(ctx, base);
  const int index = int(ctx.r3.s32);
  ctx = saved;
  // (Calibration, 89, is no longer in the list: options_page.cpp.)
  constexpr int32_t kExits[] = {kExitNewGame, kExitLoadGame, kExitOptions, kExitCredits};
  static_assert(kExits[options_page::kMainMenuOptionsItem] == kExitOptions);
  if (index < 0 || index >= int(std::size(kExits))) {
    __imp__sub_8211B6C8(ctx, base);  // as the game: the flags' other answers
    return;
  }
  ctx.r3.s64 = kExits[index];
  if (index == options_page::kMainMenuOptionsItem) REXLOG_INFO("Options: main menu -> Options");
}

// The PS2 Options screen's own action (COptionsScreenAction, vtable
// 0x82024404: Enter sub_820D8B10, Update sub_820D8D68, Exit sub_820D8F20)
// works the PS2 rows (widescreen, sound, vibration, progressive scan) by
// name and index: with our page in place of that page only the parts every
// screen needs are kept (read 2026-10-10 from its Enter / Exit):
//   Enter: sub_8210C068(action +12, action) (the action takes its track's
//          updates), track object +124 = 0.0, the screen GameStart_Options
//          (name hash at 0x825A0840, looked up by sub_820C5A28(4, &hash))
//          shown with sub_82262730(front end, screen), result flags cleared;
//   Exit:  sub_8210C068(action +12, 0), front end +8592 |= 0x40 (menus take
//          input again).
// CMenuAction drives the menu meanwhile (the hooks above).
extern "C" REX_FUNC(__imp__sub_820D8B10);
extern "C" REX_FUNC(__imp__sub_820D8D68);
extern "C" REX_FUNC(__imp__sub_820D8F20);
extern "C" REX_FUNC(__imp__sub_8210C068);  // track object: which action takes its updates
extern "C" REX_FUNC(__imp__sub_820C5A28);  // a Scrooby screen by (layer, &name hash)
extern "C" REX_FUNC(__imp__sub_82262730);  // front end: show a screen

namespace {
constexpr uint32_t kActionTrackObject = 12;
constexpr uint32_t kTrackObjectFade = 124;
constexpr uint32_t kMainOptionsScreenKey = 0x825A0840;  // hash of "GameStart_Options"
constexpr uint32_t kMenusTakeInput = 0x40;               // front end +8592
}  // namespace

extern "C" REX_FUNC(sub_820D8B10) {
  if (!options_page::MainMenuAvailable()) {
    __imp__sub_820D8B10(ctx, base);
    return;
  }
  const uint32_t action = ctx.r3.u32;
  const uint32_t track = Read32(base, action + kActionTrackObject);
  if (track) {
    scrooby::Call(__imp__sub_8210C068, ctx, base, track, action);
    std::memset(GuestPtr(base, track + kTrackObjectFade), 0, 4);  // 0.0f
  }
  const uint32_t fe = FrontEnd(base);
  const uint32_t screen = scrooby::Call(__imp__sub_820C5A28, ctx, base, 4, kMainOptionsScreenKey);
  if (fe && screen) scrooby::Call(__imp__sub_82262730, ctx, base, fe, screen);
  if (fe) *GuestPtr(base, fe + kResultFlags) &= uint8_t(~(kFlagCancel | kFlagAccept));
  ctx.r3.u64 = 1;
}
extern "C" REX_FUNC(sub_820D8D68) {
  if (options_page::MainMenuAvailable()) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_820D8D68(ctx, base);
}
extern "C" REX_FUNC(sub_820D8F20) {
  if (!options_page::MainMenuAvailable()) {
    __imp__sub_820D8F20(ctx, base);
    return;
  }
  const uint32_t action = ctx.r3.u32;
  if (const uint32_t track = Read32(base, action + kActionTrackObject)) {
    scrooby::Call(__imp__sub_8210C068, ctx, base, track, 0);
  }
  if (const uint32_t fe = FrontEnd(base)) *GuestPtr(base, fe + kMenuFlags) |= kMenusTakeInput;
}
