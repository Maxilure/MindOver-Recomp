// =============================================================================
// options/options_menu.cpp -- see options_menu.h
// =============================================================================
#include "options_menu.h"

#include "../input/keyboard_mouse.h"
#include "../input/mouse_menus.h"
#include "../input/prompt_icons.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <string>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../data/fight_tree.h"
#include "../guest_memory.h"
#include "../ui/game_font.h"
#include "../ui/scrooby.h"
#include "options_page.h"
#include "options_settings.h"

extern "C" REX_FUNC(__imp__sub_820D2828);  // CMenuAction::Enter
extern "C" REX_FUNC(__imp__sub_820D2A00);  // CMenuAction::Update
extern "C" REX_FUNC(__imp__sub_820D2F08);  // CMenuAction::Exit
// IsButtonPressed WITH our owner rules (players/menu_input.cpp hooks it):
// the menu's owner answers, as for every other in-game menu.
extern "C" REX_FUNC(sub_822652B0);
extern "C" REX_FUNC(__imp__sub_821241E8);
extern "C" REX_FUNC(__imp__sub_82371198);  // element: scale around its centre (f1), composed
extern "C" REX_FUNC(__imp__sub_82371CE0);  // text element: string from the text list by key
extern "C" REX_FUNC(__imp__sub_82370A58);  // element: reset its transform
extern "C" REX_FUNC(__imp__sub_820CE2C8);  // CInGameOptionsScreenAction: a slide step
extern "C" REX_FUNC(__imp__sub_8225F758);  // ShowConfirmMessage (front end, r4 text name, f1 timer)
extern "C" REX_FUNC(__imp__sub_823574E8);  // make heap r3 current, returns the previous
extern "C" REX_FUNC(__imp__sub_8211AF40);  // the front end's decision dispatcher (the game's)
extern "C" REX_FUNC(__imp__sub_82370FD0);  // element: scale (f1 x, f2 y, f3 z) around its centre  // FindPage(layer, page name): a loaded Scrooby page

// The main menu copy's "OptionsMenu" layer (0 in game): defined with the
// main menu's code below.
uint32_t MainMenuLayer(const uint8_t* base);

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
constexpr uint16_t kPadY = 0x8000;
constexpr char16_t kGlyphX = u'\u00B3';  // the font's X button picture
constexpr char16_t kGlyphY = u'\u00B4';  // the font's Y button picture
constexpr char16_t kGlyphA = u'\u00B1';  // the font's A button picture
constexpr const char* kSelectTextKey = "FE_Button_Text_Select";  // the prompts' "Select" (text list key)

// The stars' colours, as the original screen sets them (sub_820D2F80): lit =
// the colour word at 0x825078E4, unlit = translucent white.
constexpr uint32_t kStarLitColour = 0x825078E4;
constexpr uint32_t kStarUnlitColour = 0x4BFFFFFF;

// The description / warning / selected tab's size (findings/30 s.4: menu
// rows are Titans_Small at full size; the explanation reads as small print).
constexpr float kSmallPrint = 0.72f;
constexpr float kGuideScale = 0.55f;  // the Brightness guide (options_page.cpp): 176 units wide, inside the card
// The tabs: the ones beside smaller than the rows, the current one larger,
// like a selected menu item.
constexpr float kTab = 0.8f;
constexpr float kSelectedTab = 0.97f;

// THE WHOLE SCREEN IS DRAWN AT 0.85 x, in game and in the main menu (the
// first version filled the main menu's frame edge to edge and was shrunk
// there only; one size for both since 2026-10-11): the screens' actions
// scale the page and glass LAYERS around the screen's centre after every
// step of their slides (each step resets them); the prompts follow.
constexpr float kPageScale = 0.85f;
constexpr float kPageArrowScale = 0.6f;          // the page markers' arrows
// A greyed-out row's name and value (Row::unavailable): see-through like
// the tab names beside the current one (options_page.cpp kFadedGreen).
constexpr uint8_t kRowSolid = 0xFF, kRowGreyed = 0x60;
constexpr uint32_t kArrowOn = 0xFFFFFFFF, kArrowFaded = 0x50FFFFFF;  // white; ~30 % (nothing that way)
constexpr float kStarScale = 0.8f;  // the volume stars on a row

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
  int top = 0;          // the tab's first row on screen: the list SLIDES, one row at a time
  int pending_select = -1;  // the list slid inside the menu's own move: select this screen row after
  int selection = -1;   // the row the texts were made for
  int focused = -1;     // the row whose focus() was told "on"
  uint16_t seen = 0;    // the owner's buttons at the last Update
  bool main_menu = false;  // the main menu's copy (B is ours to answer; no Music preview)
  // Elements, found once per visit.
  uint32_t tab_prev = 0, tab_text = 0, tab_next = 0;  // the three tab names (options_page.h)
  std::array<uint32_t, kRows> label{}, value{};
  std::array<std::array<uint32_t, kStars>, kRows> star{};
  uint32_t desc = 0, warn = 0, lb = 0, rb = 0, guide = 0;
  uint32_t card_value = 0;                // the card (options_page.h)
  std::array<uint32_t, 3> card_glass{};   // its glass: 4:1, 2:1, 1:1
  uint32_t page_up = 0, page_down = 0, page_text = 0;  // the page markers under the rows
  // The game's button prompts (page FE_Buttons, always loaded): the lower
  // right corner = "Select A" (A changes the value; the in-game Options
  // state's own prompts were Back only), the upper right one = a row's X
  // action ("Dual Mode"), when it has one.
  uint32_t select_button = 0, select_text = 0;
  uint32_t x_button = 0, x_text = 0;
  bool x_showing = false;
  // The upper left corner: the Keys tab's Y ("Remove Key").
  uint32_t y_button = 0, y_text = 0;
  bool y_showing = false;
  // THE KEYS TAB (2026-10-15): the action whose key is being asked for
  // (-1 = none) and which of its two keys; the card's line about the last
  // change (e.g. a key taken from another action), for the row it was made on.
  int capture_action = -1, capture_slot = 0;
  std::u16string key_message;
  int key_message_row = -1;  // index into the tab's rows
  // A row's question (Row::confirm) in the game's Yes / No box: the row
  // (index into the tab's rows) and the selection to keep meanwhile.
  int asking_row = -1, asking_selection = 0;
  // The question is a row's X action's (Row::x_confirm), not its A action's.
  bool asking_x = false;
  // A row's name colour as the data has it (unselected green), found at
  // Enter: a slot that showed a heading gets it back (below).
  uint32_t row_colour = 0;
  // All eight prompts of FE_Buttons, moved with the glass while the screen
  // is up (their x positions as found, put back at Exit).
  std::array<uint32_t, 8> prompts{};
  std::array<uint32_t, 8> prompts_x{}, prompts_y{};  // the +92 / +96 words as found
};
Screen g;
std::atomic<bool> g_open{false};
std::atomic<bool> g_wheel_list{false};  // WheelScrollsList: open and the tab slides (Refresh)
// The in-game screen's CInGameOptionsScreenAction while it's up (its +20
// page and +32 glass layer: hidden under a question), 0 otherwise.
uint32_t g_screen_action = 0;
// The titles' cyan (findings/30 s.5): a heading row's colour.
constexpr uint32_t kHeadingColour = 0xFF00F6EF;
constexpr uint32_t kElementColour = 80;  // an element's colour word (0xAARRGGBB; scrooby.h)
// The key a capture got (UI thread writes, game thread reads): an Input,
// or one of these.
constexpr int kCaptureNone = -3, kCaptureWaiting = -2, kCaptureCancelled = -1;
std::atomic<int> g_capture_result{kCaptureNone};
std::atomic<bool> g_capturing{false};  // read by the controller filter (Capturing())

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

// ---------------------------------------------------------------------------
// Element places, sizes and the card's text (options_page.h: THE CARD)
// ---------------------------------------------------------------------------
// A Scrooby element (read 2026-10-11 with the debug console, on a text whose
// box was known): +92 / +96 x / y of its box's bottom-left as a fraction of
// the 640-unit width (floats; sub_82370E20 writes units x the constant at
// 0x8204701C), +88 / +90 its width / height in units (u16), +16 its
// transform (a scale around the centre is built from the box at the call).
// A picture is stretched to that size (the card's glass); a text's lines
// hang from its box's top (vertical justify 2) and are centred across it.
constexpr uint32_t kElementSize = 88;
constexpr uint32_t kElementX = 92, kElementY = 96;
constexpr uint32_t kUnitToPosition = 0x8204701C;  // Scrooby units -> element position

float ReadF(const uint8_t* base, uint32_t a) {
  const uint32_t v = Read32(base, a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void WriteF(uint8_t* base, uint32_t a, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  uint8_t* p = GuestPtr(base, a);
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (24 - 8 * i));
}
void Write16(uint8_t* base, uint32_t a, uint16_t v) {
  uint8_t* p = GuestPtr(base, a);
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}
void MoveX(uint8_t* base, uint32_t element, int units) {
  WriteF(base, element + kElementX,
         ReadF(base, element + kElementX) + float(units) * ReadF(base, kUnitToPosition));
}
// Places an element's box (units, y up, bottom-left corner).
void SetBox(uint8_t* base, uint32_t element, float x, float y, float w, float h) {
  if (!element) return;
  const float unit = ReadF(base, kUnitToPosition);
  WriteF(base, element + kElementX, x * unit);
  WriteF(base, element + kElementY, y * unit);
  Write16(base, element + kElementSize, uint16_t(std::lround(std::max(1.0f, w))));
  Write16(base, element + kElementSize + 2, uint16_t(std::lround(std::max(1.0f, h))));
}

// THE GAME'S FONT, to measure text (ui/game_font.h: Titans_Small, the font
// of every text on the page). Its pixels -> Scrooby units at size 1:
// calibrated on a photo, 2026-10-11 (a row label at size 1, "Frame-rate
// Cap", and the lines of the small print).
constexpr float kUnitsPerFontPixel = 0.5f;
game_font::Font g_font;
bool g_font_tried = false;
const game_font::Font* Font() {
  if (!g_font_tried) {
    g_font_tried = true;
    if (g_font.Load("Titans_Small")) {
      REXLOG_INFO("Options: card text measured with the game's font (\"Frame-rate Cap\" = {:.0f} px, "
                  "line {:.0f} px)", g_font.Measure(u"Frame-rate Cap"), g_font.cell_height());
    }
  }
  return g_font.loaded() ? &g_font : nullptr;
}
// A line's width in units at `scale` (without the font: ~12 units a letter).
float TextWidth(std::u16string_view text, float scale) {
  const game_font::Font* f = Font();
  return (f ? f->Measure(text) * kUnitsPerFontPixel : 12.0f * float(text.size())) * scale;
}
// A line's height in units at `scale` (the font's cell).
float LineHeight(float scale) {
  const game_font::Font* f = Font();
  return (f ? f->cell_height() * kUnitsPerFontPixel : 29.5f) * scale;
}

// Scrooby texts don't wrap by themselves: lines are broken at spaces so
// none is wider than `width` units at `scale` (measured letter by letter
// with the game's font: wide letters like W and M take more room than a
// count of letters would give them). A word wider than the line stays whole.
std::u16string Wrap(const std::u16string& text, float width, float scale, int* lines) {
  std::u16string out, line;
  *lines = 0;
  if (text.empty()) return out;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find(u' ', start);
    if (end == std::u16string::npos) end = text.size();
    const std::u16string word = text.substr(start, end - start);
    const std::u16string longer = line.empty() ? word : line + u' ' + word;
    if (!line.empty() && TextWidth(longer, scale) > width) {
      out += line + u'\n';
      ++*lines;
      line = word;
    } else {
      line = longer;
    }
    start = end + 1;
  }
  ++*lines;
  return out + line;
}

const std::vector<Row>& Rows() { return options_settings::Tabs()[size_t(g.tab)].rows; }

// A SLIDING LIST (2026-10-15; was pages of kRows): a tab shows kRows rows
// at a time, from its row g.top on; the screen's row r is the tab's row
// First() + r. Stepping past the window's edge slides it by ONE row (like the
// save list), so the mouse wheel goes through the rows one by one.
int First() { return g.top; }
bool Slides() { return int(Rows().size()) > kRows; }
int LastTop() { return std::max(0, int(Rows().size()) - kRows); }
bool Selectable(int i) {
  const auto& rows = Rows();
  return i >= 0 && i < int(rows.size()) && rows[size_t(i)].kind != Kind::kHeader;
}
int VisibleCount() { return std::min(kRows, int(Rows().size()) - First()); }
// The first screen row the cursor may stand on (a tab can START with a
// heading since the 2026-10-24 sort: Gameplay's "Co-op"). Used wherever the
// menu puts the cursor at the top: opening the screen, changing tab.
int FirstSelectableOnScreen() {
  for (int r = 0; r < VisibleCount(); ++r) {
    if (Selectable(First() + r)) return r;
  }
  return 0;
}
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
    if (r == kRows - 1 && g.label[r]) g.row_colour = Read32(base, g.label[r] + kElementColour);
    g.value[r] = item ? Read32(base, item + kItemValue) : 0;
    for (int k = 0; k < kStars; ++k) {
      g.star[r][k] = FindPicture(ctx, base, g.page, fmt::format("OptStar{}_{}", r, k));
    }
  }
  g.desc = FindText(ctx, base, g.page, "OptDesc");
  g.card_value = FindText(ctx, base, g.page, "OptCardValue");
  g.card_glass = {FindPicture(ctx, base, g.page, "OptCardSmall"),
                  FindPicture(ctx, base, g.page, "OptCardLarge"),
                  FindPicture(ctx, base, g.page, "OptCardSquare")};
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
  g.select_button = FindText(ctx, base, buttons, "LowerRightButton");
  g.select_text = FindText(ctx, base, buttons, "LowerRightText");
  g.x_button = FindText(ctx, base, buttons, "UpperRightButton");
  g.x_text = FindText(ctx, base, buttons, "UpperRightText");
  g.y_button = FindText(ctx, base, buttons, "UpperLeftButton");
  g.y_text = FindText(ctx, base, buttons, "UpperLeftText");
  constexpr const char* kPrompts[8] = {"LowerLeftButton",  "LowerLeftText",  "LowerRightButton",
                                       "LowerRightText",   "UpperLeftButton", "UpperLeftText",
                                       "UpperRightButton", "UpperRightText"};
  for (size_t i = 0; i < g.prompts.size(); ++i) g.prompts[i] = FindText(ctx, base, buttons, kPrompts[i]);
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
  scrooby::SetText(ctx, base, g.lb, std::u16string(1, prompt_icons::MenuGlyph(kGlyphLB)));
  scrooby::SetText(ctx, base, g.rb, std::u16string(1, prompt_icons::MenuGlyph(kGlyphRB)));
  // The stars, smaller than the volume screen's (rows are closer here).
  for (int r = 0; r < kRows; ++r) {
    for (int k = 0; k < kStars; ++k) scrooby::SetScale(ctx, base, g.star[r][k], kStarScale);
  }
  // The page arrows: the same down arrow, smaller, the upper one turned over.
  scrooby::SetScale(ctx, base, g.page_up, kPageArrowScale);
  scrooby::Rotate(ctx, base, g.page_up, 180.0f);
  scrooby::SetScale(ctx, base, g.page_down, kPageArrowScale);
  scrooby::SetScale(ctx, base, g.page_text, kSmallPrint);
  // (The card's texts and picture: LayOutCard, at every change.)
}

// THE CARD, fitted to what it says (options_page.h: the card is centred on
// the glass's centre; its width is fixed, its height = its contents):
//   padding, the value (yellow, shrunk if it's wider than the card), the
//   picture (the Brightness guide), the description, the warning, padding;
//   a gap between any two.
// The glass shown is the game's shape nearest the card's (4:1 / 2:1 / 1:1,
// switched at the geometric middles 2.83 and 1.41), stretched to fit: at
// most 1.41 x one way, what the game itself does to its own glass panel
// (a 2:1 picture drawn 500 x 354).
constexpr float kCardValueScale = 1.0f;  // the value: as big as a row
constexpr float kTextBoxHeight = 36.0f;  // the card texts' boxes in the data (options_page.cpp kLineHeight)
constexpr float kCardPadY = 12.0f, kCardGap = 8.0f;
// The guide picture, drawn at kGuideScale x its own size (320 x 64 units): 35
// units tall; its box sits 10 units under the drawn picture's centre
// (measured on a photo, 2026-10-11: a picture hangs from its box's top and
// shrinks around the box's centre).
constexpr float kGuideHeight = 35.0f, kGuideBoxBelowCentre = 10.0f, kGuideBoxHeight = 36.0f;

void LayOutCard(PPCContext& ctx, uint8_t* base, const std::u16string& heading, bool picture,
                const std::u16string& description, const std::u16string& warning) {
  using options_page::kCardPadding;
  using options_page::kCardWidth;
  using options_page::kCardX;
  const float inside_x = float(kCardX + kCardPadding);
  const float inside_w = float(kCardWidth - 2 * kCardPadding);
  // The texts and their line counts.
  const float value_w = TextWidth(heading, kCardValueScale);
  const float value_scale = value_w > inside_w ? kCardValueScale * inside_w / value_w : kCardValueScale;
  int desc_lines = 0, warn_lines = 0;
  const std::u16string desc = Wrap(description, inside_w, kSmallPrint, &desc_lines);
  const std::u16string warn = Wrap(warning, inside_w, kSmallPrint, &warn_lines);
  scrooby::SetText(ctx, base, g.card_value, heading);
  scrooby::SetText(ctx, base, g.desc, desc);
  scrooby::SetText(ctx, base, g.warn, warn);
  scrooby::SetVisible(base, g.guide, picture);
  scrooby::SetVisible(base, g.desc, desc_lines > 0);
  scrooby::SetVisible(base, g.warn, warn_lines > 0);
  // Heights, top to bottom.
  const float value_h = LineHeight(value_scale);
  const float small_h = LineHeight(kSmallPrint);
  float height = 2 * kCardPadY + value_h;
  if (picture) height += kCardGap + kGuideHeight;
  if (desc_lines) height += kCardGap + desc_lines * small_h;
  if (warn_lines) height += kCardGap + warn_lines * small_h;
  const float top = float(options_page::kCardCentreY) + height / 2;
  // A text's lines hang from the top of its box as the DATA made it (one
  // line, kTextBoxHeight: a height written at +90 doesn't move them; found
  // on a photo, 2026-10-11: each line past the first pushed the text ~20
  // units low), and shrink around the box's centre. So every text keeps a
  // one-line box, placed for its FIRST line's top: after the shrink, top =
  // centre + scale x half the box.
  auto place = [&](uint32_t text, float block_top, int /*lines*/, float scale) {
    const float half = kTextBoxHeight / 2;
    const float centre = block_top - scale * half;
    SetBox(base, text, inside_x, centre - half, inside_w, kTextBoxHeight);
    scrooby::SetScale(ctx, base, text, scale);
  };
  float y = top - kCardPadY;
  place(g.card_value, y, 1, value_scale);
  y -= value_h;
  if (picture && g.guide) {
    y -= kCardGap;
    const float centre = y - kGuideHeight / 2;
    const float box_y = centre - kGuideBoxBelowCentre;
    SetBox(base, g.guide, float(kCardX) + float(kCardWidth) / 2 - 160.0f, box_y, 320.0f, kGuideBoxHeight);
    scrooby::SetScale(ctx, base, g.guide, kGuideScale);
    y -= kGuideHeight;
  }
  if (desc_lines) {
    y -= kCardGap;
    place(g.desc, y, desc_lines, kSmallPrint);
    y -= desc_lines * small_h;
  }
  if (warn_lines) {
    y -= kCardGap;
    place(g.warn, y, warn_lines, kSmallPrint);
  }
  // The glass. A picture too is drawn at its box size AS THE DATA MADE IT
  // (a size written at +88 / +90 isn't drawn: photo, 2026-10-11), so its
  // box stays the picture's own shape, centred on the card's centre, and
  // the TRANSFORM stretches it to the card's height: sub_82370FD0(element,
  // f1 x, f2 y, f3 z) = a scale around the box's centre (the uniform one,
  // sub_82371198, is that with x = y).
  const float aspect = float(kCardWidth) / height;
  const int shape = aspect > 2.83f ? 0 : aspect > 1.41f ? 1 : 2;
  constexpr int kAspects[3] = {4, 2, 1};
  for (int i = 0; i < 3; ++i) {
    scrooby::SetVisible(base, g.card_glass[size_t(i)], i == shape);
  }
  if (const uint32_t glass = g.card_glass[size_t(shape)]) {
    const float own_h = float(kCardWidth / kAspects[shape]);
    const float centre = top - height / 2;
    SetBox(base, glass, float(kCardX), centre - own_h / 2, float(kCardWidth), own_h);
    scrooby::SetScale(ctx, base, glass, 1.0f);  // a fresh transform
    scrooby::CallF(__imp__sub_82370FD0, ctx, base, glass, 1.0, height / own_h, 1.0);
  }
}

// THE PULSE: only the selected row's VALUE pulses (the menu would pulse the
// name too: CMenuAction's Update undoes that every frame, the name only
// turns yellow). The menu pulses a text around the centre of its box
// (sub_8237A070 -> sub_82371198), and the value's box (250 units, right
// aligned) is wider than its text: it grew out past the glass's right edge.
// So the value's box is made exactly its text (measured with the game's
// font), ending where the row ends: x = right end - width (+92), width
// (+88). A right-aligned text is placed by the box's run-time x AND width
// (a first try widened the box only, keeping its x: the values moved right
// by the extra width; photo, 2026-10-11).
void CentrePulse(uint8_t* base, uint32_t text, const std::u16string& s) {
  if (!text) return;
  const float w = std::max(1.0f, TextWidth(s, 1.0f));
  const float unit = ReadF(base, kUnitToPosition);
  WriteF(base, text + kElementX, (float(options_page::kRowValueRight) - w) * unit);
  Write16(base, text + kElementSize, uint16_t(std::lround(w)));
}

// Row::unavailable's reason, empty = usable.
std::u16string Unavailable(Env& env, const Row& row) {
  return row.unavailable ? row.unavailable(env) : std::u16string();
}

// Greyed-out rows see-through, the others solid. Every frame (Update): the
// menu recolours its items when the selection moves, which may bring the
// alpha back.
void ApplyGreying(Env& env) {
  for (int r = 0; r < kRows; ++r) {
    const Row* row = RowOnScreen(r);
    if (!row) continue;
    if (row->kind == Kind::kHeader) {  // (the menu recolours its items: every frame)
      scrooby::SetColour(env.ctx, env.base, g.label[r], kHeadingColour);
      continue;
    }
    // The slot showed the heading on another page: the menu only recolours
    // an item when the selection moves, so the cyan stayed on whichever row
    // took the slot ("Jump", "Map": playtest, 2026-10-15). Back to the rows'
    // own colour (a selected row's yellow is the menu's and differs).
    if (g.row_colour && g.label[r] &&
        (Read32(env.base, g.label[r] + kElementColour) & 0xFFFFFF) == (kHeadingColour & 0xFFFFFF)) {
      scrooby::SetColour(env.ctx, env.base, g.label[r], g.row_colour);
    }
    const uint8_t alpha = Unavailable(env, *row).empty() ? kRowSolid : kRowGreyed;
    scrooby::SetAlpha(env.ctx, env.base, g.label[r], alpha);
    scrooby::SetAlpha(env.ctx, env.base, g.value[r], alpha);
  }
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
    sel = FirstSelectableOnScreen();
    scrooby::Select(ctx, base, g.menu, sel);
  }
  const uint32_t lit = Read32(base, kStarLitColour);
  for (int r = 0; r < kRows; ++r) {
    const Row* row = RowOnScreen(r);
    const bool used = row != nullptr;
    scrooby::SetSelectable(base, scrooby::Item(base, g.menu, r), used && row->kind != Kind::kHeader);
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
    if (row) {
      scrooby::SetText(ctx, base, g.label[r], row->label);
      std::u16string value;
      if (row->kind == Kind::kChoice) {
        const auto values = row->values(env);
        const int i = std::clamp(row->get(env), 0, int(values.size()) - 1);
        value = values.empty() ? u"" : values[size_t(i)];
      } else if (row->kind == Kind::kKey) {
        // The keys as pictures (the names are on the card), else as names.
        if (row->key_action == g.capture_action) {
          value = u"Press a Key";
        } else {
          value = options_settings::KeyPictures(row->key_action);
          if (value.empty()) value = options_settings::KeysText(row->key_action);
        }
      }
      scrooby::SetText(ctx, base, g.value[r], value);
      CentrePulse(base, g.value[r], value);
    }
  }
  // The list's markers: only for a tab longer than the screen; "row / rows"
  // (the selected row among the rows that can be selected: headings don't
  // count) and both arrows, faded where there are no more rows that way.
  const bool slides = Slides();
  g_wheel_list = slides;
  scrooby::SetVisible(base, g.page_up, slides);
  scrooby::SetVisible(base, g.page_down, slides);
  scrooby::SetVisible(base, g.page_text, slides);
  if (slides) {
    scrooby::SetColour(ctx, base, g.page_up, g.top > 0 ? kArrowOn : kArrowFaded);
    scrooby::SetColour(ctx, base, g.page_down, g.top < LastTop() ? kArrowOn : kArrowFaded);
    int position = 0, count = 0;
    for (int r = 0; r < int(Rows().size()); ++r) {
      if (!Selectable(r)) continue;
      ++count;
      if (r <= First() + sel) position = count;
    }
    const std::string text = fmt::format("{}/{}", position, count);
    scrooby::SetText(ctx, base, g.page_text, std::u16string(text.begin(), text.end()));
  }
  // The card: the selected row's value (its name for star and action rows,
  // which have no words for their value), picture, description and warning.
  const Row& row = *RowOnScreen(sel);
  const int current = row.get ? row.get(env) : 0;
  std::u16string heading = row.label;
  if (row.kind == Kind::kChoice) {
    const auto values = row.values(env);
    if (!values.empty()) heading = values[size_t(std::clamp(current, 0, int(values.size()) - 1))];
  }
  // A greyed-out row: its reason is the warning.
  const std::u16string unavailable = Unavailable(env, row);
  std::u16string description = row.describe ? row.describe(env, current) : u"";
  std::u16string warning = !unavailable.empty() ? unavailable : row.warn ? row.warn(env, current) : u"";
  if (row.kind == Kind::kKey) {
    // The keys as the heading; while one is asked for, what to do.
    const bool asking = row.key_action == g.capture_action;
    heading = asking ? std::u16string(u"Press a Key") : options_settings::KeysText(row.key_action);
    if (asking) {
      // Esc as its picture (a card slot, prompt_icons.h), its name without the slots.
      const kbm::Input esc = kbm::ParseInput("Escape");
      const char16_t esc_glyph = prompt_icons::KeyGlyph(esc, prompt_icons::SlotSize::kCard);
      const std::string esc_name = kbm::InputLabel(esc);
      const char16_t b_glyph = prompt_icons::ButtonGlyph("B", prompt_icons::SlotSize::kCard);
      description = u"Press the new " + std::u16string(g.capture_slot ? u"second " : u"") +
                    u"key or mouse button for " + row.label + u". " +
                    (esc_glyph ? std::u16string(1, esc_glyph) : std::u16string(esc_name.begin(), esc_name.end())) +
                    u" or " + (b_glyph ? std::u16string(1, b_glyph) : std::u16string(u"B")) + u": cancel.";
      warning.clear();
    } else if (g.key_message_row == First() + sel && unavailable.empty()) {
      warning = g.key_message;
    }
  }
  LayOutCard(ctx, base, heading, row.guide, description, warning);
  ApplyGreying(env);
  // The row's X action in the upper right corner (blank unless the row
  // offers something).
  // A key row: X = a second key, Y = remove one (not while a key is asked
  // for: then only Esc means anything).
  const bool key_row = row.kind == Kind::kKey && unavailable.empty() && g.capture_action < 0;
  const bool x = key_row || (row.x_action && row.x_shown && row.x_shown(env, current));
  if (x != g.x_showing || x) {
    scrooby::SetText(ctx, base, g.x_button, x ? std::u16string(1, prompt_icons::MenuGlyph(kGlyphX)) : u"");
    scrooby::SetText(ctx, base, g.x_text, !x ? u"" : key_row ? u"Second Key" : row.x_label);
    g.x_showing = x;
  }
  if (key_row != g.y_showing || key_row) {
    scrooby::SetText(ctx, base, g.y_button,
                     key_row ? std::u16string(1, prompt_icons::MenuGlyph(kGlyphY)) : u"");
    scrooby::SetText(ctx, base, g.y_text, key_row ? u"Remove Key" : u"");
    g.y_showing = key_row;
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
  g.key_message_row = -1;
  g.top = 0;
  scrooby::Select(env.ctx, env.base, g.menu, FirstSelectableOnScreen());
  scrooby::PlaySound(env.ctx, env.base,
                     delta < 0 ? scrooby::Sound::kMove : scrooby::Sound::kChange);
  Refresh(env);
}

// Left / right on the selected row; returns whether the value changed. With
// `wrap` (A), past the last value comes the first again.
bool ChangeValue(Env& env, int delta, bool wrap = false) {
  const Row* on_screen = RowOnScreen(scrooby::Selection(env.base, g.menu));
  if (!on_screen) return false;
  const Row& row = *on_screen;
  if (row.kind == Kind::kAction || !row.get || !row.set) return false;
  if (!Unavailable(env, row).empty()) return false;  // greyed out: stays as it is
  const int now = row.get(env);
  const int last = row.kind == Kind::kStars ? kStars : int(row.values(env).size()) - 1;
  const int next = wrap ? (now + delta + last + 1) % (last + 1) : std::clamp(now + delta, 0, last);
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

// ---------------------------------------------------------------------------
// The Keys tab: asking for a key (2026-10-15)
// ---------------------------------------------------------------------------
// A (or a click) on an action's row asks for its first key, X for a second
// one: the keyboard driver's capture hands the next key / mouse button /
// wheel notch to us instead of the game (Esc = cancel), and while it waits
// the controllers say nothing to the game (Capturing(): the XInputGetState
// wrapper), so a pad's B cancels instead of leaving the screen. The new key
// goes in with Bindings::Assign, which takes it from any other action it was
// on in an overlapping context (one key, one job): the card says so. The
// keys are in effect at once (SetBindings) and written to controls.toml.

void StartCapture(Env& env, int action, int slot) {
  kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  if (!driver || action < 0) return;
  g.capture_action = action;
  g.capture_slot = slot;
  g.key_message_row = -1;
  g_capture_result = kCaptureWaiting;
  g_capturing = true;
  driver->Capture([](kbm::Input input) {
    g_capture_result = input == kbm::kNoInput ? kCaptureCancelled : int(input);
  });
  scrooby::PlaySound(env.ctx, env.base, scrooby::Sound::kChange);
  REXLOG_INFO("Options: asking for {}'s {} key", kbm::Info(kbm::Action(action)).id, slot ? "second" : "first");
  Refresh(env);
}

// Writes a changed set of keys: in effect at once, and to the file.
void KeepKeys(const kbm::Bindings& b) {
  if (kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get()) driver->SetBindings(b);
  kbm::SaveBindings(b, kbm::ControlsFilePath());
}

// The menus' own actions (Pause, Menu: ..., the D-pad = arrow keys) aren't
// listed and their keys can't be taken: the menu action of an overlapping
// context that holds `key`, or kActionCount if none does. (A key on a menu
// action only, e.g. Enter, can still be a PLAY key: Jump on Enter keeps
// Enter = confirm in menus; contexts, input/bindings.h.)
kbm::Action MenuKeyOwner(const kbm::Bindings& b, int action, kbm::Input key) {
  for (int a = 0; a < kbm::kActionCount; ++a) {
    const kbm::ActionInfo& info = kbm::Info(kbm::Action(a));
    if (a == action || info.group != kbm::Group::kMenus || a == kbm::kMap) continue;
    if (!kbm::ContextsOverlap(info.context, kbm::Info(kbm::Action(action)).context)) continue;
    if (b.Has(kbm::Action(a), key)) return kbm::Action(a);
  }
  return kbm::kActionCount;
}

// The capture's end: `result` = the key (an Input) or kCaptureCancelled.
void EndCapture(Env& env, int result) {
  kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  const int action = g.capture_action;
  g.capture_action = -1;
  g_capturing = false;
  g_capture_result = kCaptureNone;
  if (driver) driver->CancelCapture();  // (nothing waiting after a key arrived)
  g.key_message_row = First() + scrooby::Selection(env.base, g.menu);
  if (result < 0 || !driver || action < 0) {
    g.key_message = u"Not changed.";
    REXLOG_INFO("Options: key change cancelled");
  } else if (const kbm::Action owner = MenuKeyOwner(driver->bindings(), action, kbm::Input(result));
             owner < kbm::kActionCount) {
    // A key the menus need (the arrows, Enter, Escape, Backspace, Q / E...):
    // taking it would leave a menu without its key. Not changed.
    const std::string label = kbm::InputLabel(kbm::Input(result));
    g.key_message = std::u16string(label.begin(), label.end()) + u" is the menus' " +
                    options_settings::ActionName(owner) + u": pick another key.";
    REXLOG_INFO("Options: {} refused for {} (the menus' {})", kbm::InputName(kbm::Input(result)),
                kbm::Info(kbm::Action(action)).id, kbm::Info(owner).id);
  } else {
    kbm::Bindings b = driver->bindings();
    const kbm::Input key = kbm::Input(result);
    const kbm::Action lost = b.Assign(kbm::Action(action), g.capture_slot, key);
    KeepKeys(b);
    const std::u16string name = options_settings::ActionName(action);
    const std::string label = kbm::InputLabel(key);
    const std::u16string key_text(label.begin(), label.end());
    g.key_message = lost < kbm::kActionCount
                        ? key_text + u" was " + options_settings::ActionName(lost) +
                              u"'s key: it's " + name + u"'s now."
                        : std::u16string();
    REXLOG_INFO("Options: {} key {} = {}{}", kbm::Info(kbm::Action(action)).id, g.capture_slot + 1,
                kbm::InputName(key),
                lost < kbm::kActionCount ? std::string(" (taken from ") + kbm::Info(lost).id + ")" : "");
  }
  scrooby::PlaySound(env.ctx, env.base, scrooby::Sound::kChange);
  Refresh(env);
}

// Y on an action's row: its second key goes, or its first when it has one
// key only.
void RemoveKey(Env& env, int action) {
  kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  if (!driver || action < 0) return;
  kbm::Bindings b = driver->bindings();
  const int slot = b.keys[size_t(action)][1] != kbm::kNoInput ? 1 : 0;
  const kbm::Input key = b.keys[size_t(action)][size_t(slot)];
  g.key_message_row = First() + scrooby::Selection(env.base, g.menu);
  if (key == kbm::kNoInput) {
    g.key_message = u"No key to remove.";
  } else {
    b.Assign(kbm::Action(action), slot, kbm::kNoInput);
    if (slot == 1 && b.keys[size_t(action)][0] == kbm::kNoInput) {
      std::swap(b.keys[size_t(action)][0], b.keys[size_t(action)][1]);
    }
    KeepKeys(b);
    const std::string label = kbm::InputLabel(key);
    g.key_message = std::u16string(label.begin(), label.end()) + u" removed.";
    REXLOG_INFO("Options: {} key {} removed", kbm::Info(kbm::Action(action)).id, kbm::InputName(key));
  }
  scrooby::PlaySound(env.ctx, env.base, scrooby::Sound::kMove);
  Refresh(env);
}

// ---------------------------------------------------------------------------
// A row's question (Row::confirm: Default Keys), in the game's own Yes / No
// box, the one "Are you sure you want to quit?" uses: the front end's object
// at +172 (players/lost_controller.cpp has the map): ShowConfirmMessage
// sub_8225F758(front end, text name, f1 timer -1 = none) shows it, its text
// element (front end +192) gets our words (heap 7 current, as the box's own
// code does: scrooby::SetText), its update reads left / right / A from the
// menu's owner, and on an answer sets +44 bit 0x40 = Yes and hides itself
// (+4 bit 0x40 clear). While it asks, the Options state's decision (in game
// 956, main menu 96) is held (B would leave the screen under the box) and
// the rows keep their selection.
constexpr uint32_t kBox = 172, kBoxFlags = 4, kBoxAnswer = 44, kBoxTextElement = 192;
constexpr uint8_t kBoxShowing = 0x40, kBoxYes = 0x40;
constexpr uint32_t kAnyTextName = 0x82023570;  // "InGame_Pause_Paused": a name the box can look up
constexpr uint32_t kFrontEndHeap = 7;

// The page's own content hidden under the box (it's see-through: the rows
// and the card read through the question). Refresh shows the rows and the
// card again; the rest comes back here.
void ShowContent(uint8_t* base, bool shown) {
  for (int r = 0; r < kRows; ++r) {
    scrooby::SetVisible(base, g.label[r], shown);
    scrooby::SetVisible(base, g.value[r], shown);
    for (int k = 0; k < kStars; ++k) {
      if (!shown) scrooby::SetVisible(base, g.star[r][k], false);
    }
  }
  for (const uint32_t e : {g.tab_prev, g.tab_text, g.tab_next, g.lb, g.rb, g.desc, g.warn, g.card_value,
                           g.page_up, g.page_down, g.page_text, g.guide}) {
    if (e) scrooby::SetVisible(base, e, shown);
  }
  for (const uint32_t e : g.card_glass) {
    if (e && !shown) scrooby::SetVisible(base, e, false);
  }
  // The rest of the screen too (an empty glass panel behind the question
  // looked broken: playtest photo, 2026-10-15), as the pause menu is gone
  // under "Are you sure you want to quit?": in game the screen's page and
  // its glass layer (panel, cogs, hinge, "P1 Paused"), in the main menu the
  // "OptionsMenu" layer; the prompt bar's Back / Select.
  if (g_screen_action) {
    for (const uint32_t field : {20u, 32u}) {
      if (const uint32_t e = Read32(base, g_screen_action + field)) scrooby::SetVisible(base, e, shown);
    }
  }
  if (const uint32_t layer = MainMenuLayer(base)) scrooby::SetVisible(base, layer, shown);
  for (const uint32_t e : g.prompts) {
    if (e) scrooby::SetVisible(base, e, shown);
  }
}

void Ask(Env& env, int row_index, const std::u16string& question) {
  const uint32_t fe = FrontEnd(env.base);
  if (!fe) return;
  *GuestPtr(env.base, fe + kBox + kBoxAnswer) &= uint8_t(~kBoxYes);
  const uint32_t heap = scrooby::Call(__imp__sub_823574E8, env.ctx, env.base, kFrontEndHeap);
  {
    const PPCContext saved = env.ctx;
    env.ctx.r3.u64 = fe;
    env.ctx.r4.u64 = kAnyTextName;
    env.ctx.f1.f64 = -1.0;
    __imp__sub_8225F758(env.ctx, env.base);
    env.ctx = saved;
  }
  scrooby::Call(__imp__sub_823574E8, env.ctx, env.base, heap);
  scrooby::SetText(env.ctx, env.base, Read32(env.base, fe + kBoxTextElement), question);
  g.asking_row = row_index;
  g.asking_selection = scrooby::Selection(env.base, g.menu);
  ShowContent(env.base, false);
  REXLOG_INFO("Options: asking \"{}\"", std::string(question.begin(), question.end()));
}

// While asking: -1 = still asking, 0 = No, 1 = Yes.
int Answer(uint8_t* base) {
  const uint32_t fe = FrontEnd(base);
  if (!fe) return 0;
  if (*GuestPtr(base, fe + kBox + kBoxFlags) & kBoxShowing) return -1;
  return (*GuestPtr(base, fe + kBox + kBoxAnswer) & kBoxYes) ? 1 : 0;
}

}  // namespace

bool Capturing() { return g_capturing.load(std::memory_order_relaxed); }

namespace {
// Controller buttons held when a capture ended: hidden from the game until
// let go (the B that cancelled would otherwise close the screen at once).
std::array<std::atomic<uint16_t>, 4> g_hidden{};
}  // namespace

void FilterPad(uint32_t user, uint8_t* gamepad) {
  if (user >= g_hidden.size() || !gamepad) return;
  const uint16_t buttons = uint16_t(gamepad[0] << 8 | gamepad[1]);
  if (Capturing()) {
    g_hidden[user] = buttons;
    std::memset(gamepad, 0, 12);
    return;
  }
  uint16_t hidden = g_hidden[user].load(std::memory_order_relaxed);
  if (!hidden) return;
  hidden &= buttons;  // let go = seen again from the next press on
  g_hidden[user] = hidden;
  const uint16_t out = buttons & ~hidden;
  gamepad[0] = uint8_t(out >> 8);
  gamepad[1] = uint8_t(out);
}

int TurnPage(uint32_t menu, int direction, uint8_t* base) {
  if (!g.action || menu != g.menu || !Slides()) return 0;  // short lists: the menu's own move
  const int count = int(Rows().size());
  const int current = First() + scrooby::Selection(base, g.menu);
  // The next row that can be selected that way (headings are skipped).
  int next = current + direction;
  while (next >= 0 && next < count && !Selectable(next)) next += direction;
  if (next < 0 || next >= count) {
    // Past an end: around to the other end, as the game's menus wrap.
    // (Searching on in the SAME direction: a tab can start with a heading,
    // and searching back from row 0 found nothing = no slide, heading hidden.)
    next = direction > 0 ? 0 : count - 1;
    while (next >= 0 && next < count && !Selectable(next)) next += direction;
    if (next < 0 || next >= count) return 0;
    g.top = direction > 0 ? 0 : LastTop();
  } else if (next >= First() && next < First() + kRows) {
    return 0;  // on screen: the menu's own move
  } else if (direction > 0) {
    g.top = std::min(LastTop(), next - kRows + 1);  // slides down: the row comes in at the bottom
  } else {
    g.top = next;  // slides up: the row comes in at the top...
    // ...with a heading just above it shown too.
    while (g.top > 0 && !Selectable(g.top - 1)) --g.top;
    // ...but not the blank spacer before that heading (options_settings
    // Build): the window wouldn't start with an empty line.
    if (Rows()[size_t(g.top)].label.empty() && g.top < next) ++g.top;
  }
  g.pending_select = next - g.top;
  return 1;
}

// The list's arrows clicked, or the mouse wheel: the window slides one row;
// the cursor stays on its row if that's still on screen, else on the nearest
// row that is (the menu's cursor is one of the rows on screen).
void SlideBy(Env& env, int delta) {
  const int top = std::clamp(g.top + delta, 0, LastTop());
  if (top == g.top) return;
  const int current = First() + scrooby::Selection(env.base, g.menu);
  g.top = top;
  int keep = current;
  if (keep < g.top) {  // went off the top: the first selectable row on screen
    keep = g.top;
    while (keep < g.top + kRows - 1 && !Selectable(keep)) ++keep;
  } else if (keep >= g.top + kRows) {  // off the bottom: the last one
    keep = g.top + kRows - 1;
    while (keep > g.top && !Selectable(keep)) --keep;
  }
  g.pending_select = keep - g.top;
  scrooby::PlaySound(env.ctx, env.base, scrooby::Sound::kMove);
}

// The Options states' decisions (in game 956, main menu 96): held while a
// row's question is in the box (its B must not close the screen under it).
constexpr int32_t kOptionsStates[] = {956, 96};

void Register() {
  options_page::Register();
  for (const int32_t state : kOptionsStates) {
    fight_tree::SetFrontEndDecision(state, [](PPCContext& ctx, uint8_t* base) -> int32_t {
      if (g.asking_row >= 0) return -1;  // stay
      __imp__sub_8211AF40(ctx, base);
      return int32_t(ctx.r3.u32);
    });
  }
}

void SeePad(uint32_t user, const uint8_t* gamepad) {
  if (user < g_pad.size()) {
    g_pad[user].store(uint16_t(gamepad[0] << 8 | gamepad[1]), std::memory_order_relaxed);
  }
}

bool Open() { return g_open.load(std::memory_order_relaxed); }
bool InMainMenu() { return Open() && g.main_menu; }
bool WheelScrollsList() { return Open() && g_wheel_list.load(std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// THE GLASS MOVES LEFT (the card stands on the right: options_page.h)
// ---------------------------------------------------------------------------
// The in-game screen draws the game's glass panel page InGame_NV_GlassPanel
// (panel, two cogs, hinge), which the PAUSE menu shares: the page is loaded
// once and lives on between screens, so it's moved here for this screen
// only and put back at its Exit.
// CInGameOptionsScreenAction (vtable 0x82023594: Enter sub_820CDAD0, Update
// sub_820CDCC0, Exit sub_820CDD60; read 2026-10-11) keeps that page at +28,
// its layer at +32 (the slide-in: reset + move of the LAYER every step,
// sub_820CE2C8) and the two cogs at +40 / +44 (every slide step RESETS their
// transform and turns them again, sub_82370A58 + sub_823710C8). A relative
// move (sub_82370EE8, the transform) would be lost on the cogs at the next
// step, so the move goes into each element's POSITION instead (+92 x, as the
// HUD fix for players 3-4 does: more_players_hud.cpp): a fraction of the
// screen width (sub_82370E20 writes units x the constant at 0x8204701C), and
// the turn is built around it.
namespace {
constexpr uint32_t kScreenGlassPage = 28;         // CInGameOptionsScreenAction
constexpr uint32_t kScreenCog1 = 40, kScreenCog2 = 44;
std::array<uint32_t, 4> g_moved_glass{};  // panel, hinge, cog, cog (0 = not moved)
constexpr uint32_t kScreenPage = 20, kScreenGlassLayer = 32;
// The screen drawn at kPageScale: its glass layer and its page.
void ShrinkInGame(PPCContext& ctx, uint8_t* base, uint32_t action) {
  if (!options_page::Available()) return;
  for (const uint32_t field : {kScreenGlassLayer, kScreenPage}) {
    if (const uint32_t e = Read32(base, action + field)) {
      scrooby::CallF(__imp__sub_82371198, ctx, base, e, kPageScale);
    }
  }
}
}  // namespace

void MoveGlassForCard(PPCContext& ctx, uint8_t* base, uint32_t action) {
  g_screen_action = action;
  if (!options_page::Available() || g_moved_glass[0]) return;
  const uint32_t page = Read32(base, action + kScreenGlassPage);
  if (!page) return;
  g_moved_glass = {scrooby::FindPicture(ctx, base, page, "GlassPanel"),
                   scrooby::FindPicture(ctx, base, page, "Hinge"),
                   Read32(base, action + kScreenCog1), Read32(base, action + kScreenCog2)};
  if (!g_moved_glass[0]) {  // not the page we know: leave it alone
    g_moved_glass = {};
    REXLOG_WARN("Options: the glass panel page isn't as expected: not moved for the card");
    return;
  }
  for (const uint32_t e : g_moved_glass) {
    if (e) MoveX(base, e, options_page::kGlassShift);
  }
  ShrinkInGame(ctx, base, action);
  const uint8_t* l = GuestPtr(base, Read32(base, action + kScreenGlassLayer));
  const uint8_t* pg = GuestPtr(base, Read32(base, action + kScreenPage));
  REXLOG_INFO("Options: in-game glass layer box {}x{}, page box {}x{}", int(l[88] << 8 | l[89]),
              int(l[90] << 8 | l[91]), int(pg[88] << 8 | pg[89]), int(pg[90] << 8 | pg[91]));
}

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
  g.top = 0;
  g.pending_select = -1;
  g.x_showing = true;  // the first Refresh writes the corners either way
  g.y_showing = true;
  const int owner = Owner(base);
  g.seen = g_pad[size_t(owner)].load(std::memory_order_relaxed);
  FindElements(ctx, base);
  // "Select A" in the lower right corner: the font's A picture and the
  // game's own text (by the key the main menus' prompts use, so it's in the
  // game's language; sub_82371CE0 = a text's string from the text list).
  scrooby::SetText(ctx, base, g.select_button, std::u16string(1, prompt_icons::MenuGlyph(kGlyphA)));
  if (g.select_text) {
    scrooby::Call(__imp__sub_82371CE0, ctx, base, g.select_text, scrooby::GuestAscii(kSelectTextKey));
  }
  // The button prompts (Back, the X action...) go with the glass (their x
  // as found is put back at Exit: FE_Buttons serves every screen).
  // The page is drawn smaller (kPageScale, around the screen's centre (320,
  // 240): its layer's box is the whole 640 x 480), so the prompts go where
  // the smaller glass puts its corners.
  for (size_t i = 0; i < g.prompts.size(); ++i) {
    const uint32_t e = g.prompts[i];
    if (!e) continue;
    g.prompts_x[i] = Read32(base, e + kElementX);
    g.prompts_y[i] = Read32(base, e + kElementY);
    // Each prompt is shrunk like the page too, around its own centre, and
    // that CENTRE goes where the page's scale puts it (only the positions
    // moved at first: buttons and words kept their size, came closer
    // together and overlapped; a playtest photo, 2026-10-11).
    const float unit = ReadF(base, kUnitToPosition);
    const uint8_t* size = GuestPtr(base, e + kElementSize);
    const float w = float(size[0] << 8 | size[1]), h = float(size[2] << 8 | size[3]);
    const float cx = ReadF(base, e + kElementX) / unit + float(options_page::kGlassShift) + w / 2;
    const float cy = ReadF(base, e + kElementY) / unit + h / 2;
    WriteF(base, e + kElementX, (320.0f + kPageScale * (cx - 320.0f) - w / 2) * unit);
    WriteF(base, e + kElementY, (240.0f + kPageScale * (cy - 240.0f) - h / 2) * unit);
    scrooby::SetScale(ctx, base, e, kPageScale);
  }
  SetUpFixedTexts(ctx, base);
  scrooby::Select(ctx, base, g.menu, FirstSelectableOnScreen());
  Env env{ctx, base, owner};
  Refresh(env);
  g_open = true;
  REXLOG_INFO("Options: screen opened ({}) by player {} (tab {})", main_menu ? "main menu" : "in game",
              owner + 1, g.tab);
}

// The main menu copy's "OptionsMenu" layer (drawn at kMainMenuScale), 0 in
// game; defined with the main menu's code below. For the mouse's hit tests.
uint32_t MainMenuLayer(const uint8_t* base);

namespace {
// The mouse on a volume row's STARS (input/mouse_menus.h): the pointer moving
// over a star selects its row; a click on star k (1-5) sets the volume to k,
// and a click on the first star of a row already at 1 mutes it (0).
uint32_t g_star_moves = 0;
void StarMouse(PPCContext& ctx, uint8_t* base, int owner) {
  const uint32_t layer = MainMenuLayer(base);
  int star_row = -1, star = -1;
  for (int r = 0; r < kRows && star_row < 0; ++r) {
    const Row* row = RowOnScreen(r);
    if (!row || row->kind != Kind::kStars) continue;
    for (int k = 0; k < kStars; ++k) {
      if (mouse_menus::PointerOver(base, g.star[r][k], g.page, layer)) {
        star_row = r;
        star = k;
        break;
      }
    }
  }
  const uint32_t moves = mouse_menus::PointerMoves();
  const bool moved = moves != g_star_moves;
  g_star_moves = moves;
  if (star_row < 0) return;
  Env env{ctx, base, owner};
  if (mouse_menus::TakeClickOver(base, g.star[star_row][star], g.page, layer)) {
    if (scrooby::Selection(base, g.menu) != star_row) scrooby::Select(ctx, base, g.menu, star_row);
    const Row& row = *RowOnScreen(star_row);
    if (row.get && row.set && Unavailable(env, row).empty()) {
      const int now = row.get(env);
      const int level = (star == 0 && now == 1) ? 0 : star + 1;
      if (level != now) row.set(env, level);  // the row plays its own sound
    }
    Refresh(env);
  } else if (moved && scrooby::Selection(base, g.menu) != star_row) {
    scrooby::Select(ctx, base, g.menu, star_row);
    scrooby::PlaySound(ctx, base, scrooby::Sound::kMove);
  }
}
}  // namespace

extern "C" REX_FUNC(sub_820D2A00) {
  const uint32_t action = ctx.r3.u32;
  // The mouse in menus: hover / click on this menu's items (input/mouse_menus.h);
  // ours with the main menu copy's shrunk layer too.
  int mouse_row = -1;  // a row clicked with the mouse this frame
  if (g.action && action == g.action) {
    if (g.capture_action < 0) {  // asking for a key: the mouse is the key's
      StarMouse(ctx, base, Owner(base));
      mouse_row = mouse_menus::HoverAndTakeClick(ctx, base, g.menu, g.page, MainMenuLayer(base));
    }
  } else {
    mouse_menus::OnMenuUpdate(ctx, base, action);
  }
  if (!g.action || action != g.action) {
    __imp__sub_820D2A00(ctx, base);
    return;
  }
  const uint32_t fe = FrontEnd(base);
  const int owner = Owner(base);
  // The tabs answer to clicks: the previous tab's name or its key hint = LB,
  // the next one's = RB (input/mouse_menus.h).
  // The list's arrows answer to clicks: the list slides one row that way.
  if (g.capture_action < 0 && g.asking_row < 0 && Slides()) {
    const uint32_t layer = MainMenuLayer(base);
    const int turn = mouse_menus::TakeClickOver(base, g.page_up, g.page, layer)     ? -1
                     : mouse_menus::TakeClickOver(base, g.page_down, g.page, layer) ? +1
                                                                                     : 0;
    if (turn) {
      Env env{ctx, base, Owner(base)};
      SlideBy(env, turn);  // the new rows and the cursor: below (pending_select)
    }
  }
  // The mouse wheel scrolls the list (keyboard_mouse.cpp sends its notches
  // here while WheelScrollsList): one row a notch; nothing at the ends.
  if (auto* driver = kbm::KeyboardMouseDriver::Get()) {
    const int notches = driver->TakeListWheel();
    if (notches && g.capture_action < 0 && g.asking_row < 0 && Slides()) {
      Env env{ctx, base, Owner(base)};
      const int top = g.top;
      SlideBy(env, std::clamp(notches, -kRows, kRows));
      if (g.top != top) {
        REXLOG_INFO("Options: the wheel scrolled the list to row {} (of {})", g.top + 1, int(Rows().size()));
      }
    }
  }
  if (auto* driver = kbm::KeyboardMouseDriver::Get(); driver && g.capture_action < 0) {
    if (mouse_menus::TakeClickOver(base, g.tab_prev, g.page, MainMenuLayer(base)) || mouse_menus::TakeClickOver(base, g.lb, g.page, MainMenuLayer(base))) {
      driver->Pulse(kPadLB);
    } else if (mouse_menus::TakeClickOver(base, g.tab_next, g.page, MainMenuLayer(base)) || mouse_menus::TakeClickOver(base, g.rb, g.page, MainMenuLayer(base))) {
      driver->Pulse(kPadRB);
    }
  }
  RunWithoutSpecialCode(__imp__sub_820D2A00, ctx, base);
  const PPCContext saved = ctx;  // our calls below must not change what Update returned
  // A question in the box: the rows under it don't move.
  if (g.asking_row >= 0 && scrooby::Selection(base, g.menu) != g.asking_selection) {
    scrooby::Select(ctx, base, g.menu, g.asking_selection);
  }
  // The selected row's name: yellow, not pulsing (the menu's pulse, run
  // just now, undone: CentrePulse).
  {
    const int sel = scrooby::Selection(base, g.menu);
    if (sel >= 0 && sel < kRows && g.label[size_t(sel)]) {
      scrooby::Call(__imp__sub_82370A58, ctx, base, g.label[size_t(sel)]);
    }
  }
  if (!fe || !(*GuestPtr(base, fe + kMenuFlags) & 0x40)) return;
  Env env{ctx, base, owner};
  if (g.pending_select >= 0) {
    // The list slid in the menu's own up / down (TurnPage) or by an arrow:
    // the new rows first, then the cursor on its row.
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
  // A question in the box: its answer, nothing else meanwhile.
  if (g.asking_row >= 0) {
    const int answer = Answer(base);
    if (answer >= 0) {
      const int row_index = g.asking_row;
      g.asking_row = -1;
      const auto& rows = Rows();
      const bool x = g.asking_x;
      g.asking_x = false;
      if (answer == 1 && x && row_index < int(rows.size()) && rows[size_t(row_index)].x_action) {
        rows[size_t(row_index)].x_action(env);
      } else if (answer == 1 && !x && row_index < int(rows.size()) && rows[size_t(row_index)].activate) {
        rows[size_t(row_index)].activate(env);
        g.key_message_row = -1;
    g.asking_row = -1;
      }
      REXLOG_INFO("Options: the question was answered {}", answer == 1 ? "Yes" : "No");
      ShowContent(base, true);
      g.selection = -1;
      Refresh(env);
    }
    ctx = saved;
    return;
  }
  // Asking for a key: it arrives (UI thread), or the controller's B
  // cancels; nothing else happens meanwhile.
  if (g.capture_action >= 0) {
    const int result = g_capture_result.load();
    if (result != kCaptureWaiting) {
      EndCapture(env, result);
    } else if (pressed & kPadB) {
      EndCapture(env, kCaptureCancelled);
    }
    ctx = saved;
    return;
  }
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
  // The main menu's copy: B = back: the panel slides out first (the
  // screen's action, below), then the state's decision sees the cancel
  // flag; without its slide, the flag at once.
  if (g.main_menu && (pressed & kPadB) && !CloseMainMenuOptions(ctx, base)) {
    *GuestPtr(base, fe + kResultFlags) |= kFlagCancel;
  }
  // The Keys tab: X = a second key, Y = remove one.
  if (const Row* row = RowOnScreen(scrooby::Selection(base, g.menu));
      row && row->kind == Kind::kKey && Unavailable(env, *row).empty()) {
    if (pressed & kPadX) {
      StartCapture(env, row->key_action, 1);
      ctx = saved;
      return;
    }
    if (pressed & kPadY) {
      RemoveKey(env, row->key_action);
      ctx = saved;
      return;
    }
  }
  if (pressed & kPadX) {
    const Row* row = RowOnScreen(scrooby::Selection(base, g.menu));
    if (row && row->x_action && row->x_shown && row->x_shown(env, row->get ? row->get(env) : 0)) {
      const std::u16string question = row->x_confirm ? row->x_confirm(env) : std::u16string();
      scrooby::PlaySound(ctx, base, scrooby::Sound::kChange);
      if (!question.empty()) {
        Ask(env, First() + scrooby::Selection(base, g.menu), question);
        g.asking_x = true;
        ctx = saved;
        return;
      }
      row->x_action(env);
    }
  }
  // A: an action row (Rebind Keys) does its thing; any other row takes its
  // next value, the first again after the last (no arrows to aim at:
  // left / right still step either way and stop at the ends).
  // A, or a mouse click on a row (input/mouse_menus.h: handled here at once,
  // no pretend A press, so quick clicks all count).
  if ((pressed & kPadA) || mouse_row >= 0) {
    const Row* row = RowOnScreen(scrooby::Selection(base, g.menu));
    if (row && row->kind == Kind::kKey) {
      if (Unavailable(env, *row).empty()) {
        StartCapture(env, row->key_action, 0);
        ctx = saved;
        return;
      }
    } else if (row && row->kind == Kind::kAction) {
      if (row->activate && Unavailable(env, *row).empty()) {
        if (!row->confirm.empty()) {
          Ask(env, First() + scrooby::Selection(base, g.menu), row->confirm);
        } else {
          row->activate(env);
          refresh = true;
        }
        scrooby::PlaySound(ctx, base, scrooby::Sound::kChange);
      }
    } else if (row) {
      refresh |= ChangeValue(env, +1, /*wrap=*/true);
    }
  }
  if (refresh || scrooby::Selection(base, g.menu) != g.selection) {
    Refresh(env);
  } else {
    ApplyGreying(env);
  }
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
    if (g.y_showing) {
      scrooby::SetText(ctx, base, g.y_button, u"");
      scrooby::SetText(ctx, base, g.y_text, u"");
      g.y_showing = false;
    }
    if (g.capture_action >= 0) {  // (the screen can't close while asking: just in case)
      if (auto* driver = kbm::KeyboardMouseDriver::Get()) driver->CancelCapture();
      g.capture_action = -1;
      g_capturing = false;
      g_capture_result = kCaptureNone;
    }
    g.key_message_row = -1;
    if (!g.main_menu) {  // in game the pause menu sets its own corners again
      scrooby::SetText(ctx, base, g.select_button, u"");
      scrooby::SetText(ctx, base, g.select_text, u"");
    }
    for (size_t i = 0; i < g.prompts.size(); ++i) {
      if (!g.prompts[i]) continue;
      for (const auto& [field, word] : {std::pair{kElementX, g.prompts_x[i]}, std::pair{kElementY, g.prompts_y[i]}}) {
        uint8_t* p = GuestPtr(base, g.prompts[i] + field);
        for (int b = 0; b < 4; ++b) p[b] = uint8_t(word >> (24 - 8 * b));
      }
      scrooby::SetScale(ctx, base, g.prompts[i], 1.0f);  // its own size again
      g.prompts[i] = 0;
    }
    g.action = 0;
    g_open = false;
    options_settings::SaveChanged();
    REXLOG_INFO("Options: screen closed");
  }
  __imp__sub_820D2F08(ctx, base);
}

// The in-game screen's Exit: the glass page back where the pause menu has
// it, BEFORE the original (it lets go of the cogs it holds). The cogs' last
// turn was built around the moved place: their transform is reset (the
// pause menu's own slide turns them again).
extern "C" REX_FUNC(__imp__sub_820CDD60);  // CInGameOptionsScreenAction: Exit
extern "C" REX_FUNC(sub_820CDD60) {
  g_screen_action = 0;
  if (g_moved_glass[0]) {
    for (size_t i = 0; i < g_moved_glass.size(); ++i) {
      const uint32_t e = g_moved_glass[i];
      if (!e) continue;
      MoveX(base, e, -options_page::kGlassShift);
      if (i >= 2) scrooby::SetScale(ctx, base, e, 1.0f);  // the cogs: a fresh transform
    }
    g_moved_glass = {};
  }
  __imp__sub_820CDD60(ctx, base);
}

// The in-game screen's slide step (sub_820CE2C8: resets + moves the glass
// layer +32 and the options page +20, resets + turns the cogs): after it,
// both drawn at kPageScale. Its Enter's set-up (sub_820CDEC8) resets the
// layer too: MoveGlassForCard (after the Enter) scales once there.
extern "C" REX_FUNC(sub_820CE2C8) {
  const uint32_t action = ctx.r3.u32;
  __imp__sub_820CE2C8(ctx, base);
  ShrinkInGame(ctx, base, action);
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
extern "C" REX_FUNC(sub_821239C8);  // GetMenuIndex(_, page name, menu name): through the
                                    // mouse's wrapper (input/mouse_menus.cpp: a clicked item)

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
  sub_821239C8(ctx, base);
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
// name and index: with our page in place of that page, its Enter / Exit are
// rewritten here, keeping everything a screen needs and its SLIDE (read
// 2026-10-10 / 11):
//   Enter: sub_8210C068(action +12, action) (the action takes its track's
//          updates), track object +124 = 0.0, the page GameStart_OptionsPS2
//          (+20; name hash at 0x825A0828, sub_820C5AD0(4, &hash)), its layer
//          "OptionsMenu" (+28, hash 0x825A0838, sub_82371990(page, hash)),
//          two pictures (+32 / +36, hashes 0x825A0850 / 0x825A0848: the
//          panel's cog and hinge names, kept by our page: options_page.cpp),
//          the screen GameStart_Options (hash 0x825A0840, sub_820C5A28(4,
//          &hash)) shown with sub_82262730(front end, screen), result flags
//          cleared, state +40 = 2 (OPENING) and the sounds "back" +
//          "arm_wipe" (FE_SFX). (Skipped: the PS2 widescreen row's value.)
//   Update, by state +40: 2 = sub_820D9038 (the opening step: menus wait
//          (+8592 bit 0x40 off), the interpolator at +44 runs toward "in",
//          the layer +28 is reset and moved by its value, the cog +36 turned;
//          once there: state 3, menus take input), 3 = open (the PS2 rows'
//          code: skipped, CMenuAction drives ours), 4 / 5 = sub_820D9360
//          (the closing step, toward "out"; once there the result flag: 4 =
//          cancel, 5 = accept). Every step resets the layer's transform: the
//          page's smaller size (kMainMenuScale) goes on after each.
//   Exit:  sub_8210C068(action +12, 0), front end +8592 |= 0x40 (menus take
//          input again).
// B (options_menu's CMenuAction Update) = state 4: the panel slides out, then
// the state's decision sees the cancel flag (as the original did).
extern "C" REX_FUNC(__imp__sub_820D8B10);
extern "C" REX_FUNC(__imp__sub_820D8D68);
extern "C" REX_FUNC(__imp__sub_820D8F20);
extern "C" REX_FUNC(__imp__sub_820D9038);  // COptionsScreenAction: opening step (f1 = the frame's time)
extern "C" REX_FUNC(__imp__sub_820D9360);  // COptionsScreenAction: closing step
extern "C" REX_FUNC(__imp__sub_8210C068);  // track object: which action takes its updates
extern "C" REX_FUNC(__imp__sub_820C5A28);  // a Scrooby screen by (layer, &name hash)
extern "C" REX_FUNC(__imp__sub_820C5AD0);  // a Scrooby page by (layer, &name hash)
extern "C" REX_FUNC(__imp__sub_82262730);  // front end: show a screen
extern "C" REX_FUNC(__imp__sub_82371990);  // page: a layer by name hash (r4, 64 bits)
extern "C" REX_FUNC(__imp__sub_823719A0);  // page: a picture by name hash (r4, 64 bits)
extern "C" REX_FUNC(__imp__sub_820C5B08);  // holder = page (counted reference)
extern "C" REX_FUNC(__imp__sub_820C9DE0);  // holder = layer
extern "C" REX_FUNC(__imp__sub_820C5B78);  // holder = picture
extern "C" REX_FUNC(__imp__sub_82261800);  // front end: play a sound (bank, name, f1-f3)

namespace {
constexpr uint32_t kActionTrackObject = 12;
constexpr uint32_t kTrackObjectFade = 124;
constexpr uint32_t kMainOptionsScreenKey = 0x825A0840;  // hash of "GameStart_Options"
constexpr uint32_t kMainOptionsPageKey = 0x825A0828;    // hash of its page
constexpr uint32_t kMainOptionsLayerKey = 0x825A0838;   // hash of "OptionsMenu" (layer)
constexpr uint32_t kMainOptionsPic32Key = 0x825A0850, kMainOptionsPic36Key = 0x825A0848;
constexpr uint32_t kMenusTakeInput = 0x40;               // front end +8592
// COptionsScreenAction fields.
constexpr uint32_t kOptPage = 20, kOptLayer = 28, kOptPic32 = 32, kOptPic36 = 36, kOptState = 40;
constexpr uint32_t kStateOpening = 2, kStateOpen = 3, kStateClosingCancel = 4;
// The sounds of its Enter: FE_SFX "back" and "arm_wipe", at (f1, f2, f3) =
// the floats at 0x8201EE28, 0x82046720, 0x8201EE28 (as the original passes).
constexpr uint32_t kSoundBank = 0x820222E4, kSoundBack = 0x820222DC, kSoundArmWipe = 0x820222EC;
constexpr uint32_t kSoundF1 = 0x8201EE28, kSoundF2 = 0x82046720;
uint32_t g_main_action = 0;  // the main menu's Options action while it's up

// The main menu's Options page is drawn smaller than the in-game one: it
// filled the purple frame's opening edge to edge (a playtest: "way too
// big"). A scale of the page's LAYER around its centre, put on after every
// slide step (each resets the layer) and kept once open.
constexpr float kMainMenuScale = kPageScale;

uint32_t ByHash(PPCContext& ctx, uint8_t* base, scrooby::GuestFunction find, uint32_t page,
                uint32_t hash_global) {
  if (!page) return 0;
  const PPCContext saved = ctx;
  ctx.r3.u64 = page;
  ctx.r4.u64 = Read64(base, hash_global);
  find(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}
void Hold(PPCContext& ctx, uint8_t* base, scrooby::GuestFunction assign, uint32_t holder,
          uint32_t object) {
  scrooby::Call(assign, ctx, base, holder, object);
}
void Sound(PPCContext& ctx, uint8_t* base, uint32_t fe, uint32_t name) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = fe;
  ctx.r4.u64 = kSoundBank;
  ctx.r5.u64 = name;
  ctx.f1.f64 = ReadF(base, kSoundF1);
  ctx.f2.f64 = ReadF(base, kSoundF2);
  ctx.f3.f64 = ReadF(base, kSoundF1);
  __imp__sub_82261800(ctx, base);
  ctx = saved;
}
}  // namespace

uint32_t MainMenuLayer(const uint8_t* base) {
  return g_main_action ? Read32(base, g_main_action + kOptLayer) : 0;
}

namespace {
// The page's smaller size, after a slide step (or once at Enter).
void ShrinkMainMenuPage(PPCContext& ctx, uint8_t* base, uint32_t action) {
  if (const uint32_t layer = Read32(base, action + kOptLayer)) {
    scrooby::CallF(__imp__sub_82371198, ctx, base, layer, kMainMenuScale);
  }
}
}  // namespace

namespace options_menu {
bool CloseMainMenuOptions(PPCContext& ctx, uint8_t* base) {
  if (!g_main_action || Read32(base, g_main_action + kOptLayer) == 0) return false;
  if (Read32(base, g_main_action + kOptState) != kStateOpen) return true;  // sliding: wait
  uint8_t* state = GuestPtr(base, g_main_action + kOptState);
  state[0] = state[1] = state[2] = 0;
  state[3] = uint8_t(kStateClosingCancel);
  if (const uint32_t fe = FrontEnd(base)) {
    *GuestPtr(base, fe + kMenuFlags) &= uint8_t(~kMenusTakeInput);
    // The slide out with its sounds, as the slide in (a playtest: the exit
    // was silent; the original's B starts the same slide with them).
    Sound(ctx, base, fe, kSoundBack);
    Sound(ctx, base, fe, kSoundArmWipe);
  }
  return true;
}
}  // namespace options_menu

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
  // The page, its layer and the two pictures, as the original finds them.
  const uint32_t page = scrooby::Call(__imp__sub_820C5AD0, ctx, base, 4, kMainOptionsPageKey);
  Hold(ctx, base, __imp__sub_820C5B08, action + kOptPage, page);
  const uint32_t layer = ByHash(ctx, base, __imp__sub_82371990, page, kMainOptionsLayerKey);
  const uint32_t pic32 = ByHash(ctx, base, __imp__sub_823719A0, page, kMainOptionsPic32Key);
  const uint32_t pic36 = ByHash(ctx, base, __imp__sub_823719A0, page, kMainOptionsPic36Key);
  Hold(ctx, base, __imp__sub_820C9DE0, action + kOptLayer, layer);
  Hold(ctx, base, __imp__sub_820C5B78, action + kOptPic32, pic32);
  Hold(ctx, base, __imp__sub_820C5B78, action + kOptPic36, pic36);
  if (pic32) scrooby::Call(__imp__sub_82370A58, ctx, base, pic32);
  const uint32_t fe = FrontEnd(base);
  const uint32_t screen = scrooby::Call(__imp__sub_820C5A28, ctx, base, 4, kMainOptionsScreenKey);
  if (fe && screen) scrooby::Call(__imp__sub_82262730, ctx, base, fe, screen);
  if (fe) *GuestPtr(base, fe + kResultFlags) &= uint8_t(~(kFlagCancel | kFlagAccept));
  // The slide needs all three (the steps use them unchecked); without them
  // the page just shows (state 3, menus take input).
  const bool slide = page && layer && pic36;
  uint8_t* state = GuestPtr(base, action + kOptState);
  state[0] = state[1] = state[2] = 0;
  state[3] = uint8_t(slide ? kStateOpening : kStateOpen);
  if (slide && fe) {
    Sound(ctx, base, fe, kSoundBack);
    Sound(ctx, base, fe, kSoundArmWipe);
  }
  if (!slide) {
    REXLOG_WARN("Options: main menu page without its slide (page {:08X} layer {:08X} cog {:08X})",
                page, layer, pic36);
  } else {
    const uint8_t* l = GuestPtr(base, layer);
    REXLOG_INFO("Options: main menu layer box {}x{} at ({:.1f}, {:.1f})", int(l[88] << 8 | l[89]),
                int(l[90] << 8 | l[91]), ReadF(base, layer + 92) * 640, ReadF(base, layer + 96) * 640);
  }
  g_main_action = action;
  ctx.r3.u64 = 1;
}
extern "C" REX_FUNC(sub_820D8D68) {
  if (!options_page::MainMenuAvailable()) {
    __imp__sub_820D8D68(ctx, base);
    return;
  }
  const uint32_t action = ctx.r3.u32;
  const uint32_t state = Read32(base, action + kOptState);
  if (state == kStateOpening || state == kStateClosingCancel || state == kStateClosingCancel + 1) {
    const PPCContext saved = ctx;  // (r3 = action, f1 = the frame's time: as the original passes)
    if (state == kStateOpening) {
      __imp__sub_820D9038(ctx, base);
    } else {
      __imp__sub_820D9360(ctx, base);
    }
    ctx = saved;
    ShrinkMainMenuPage(ctx, base, action);
  }
  ctx.r3.u64 = 1;
}
extern "C" REX_FUNC(sub_820D8F20) {
  if (!options_page::MainMenuAvailable()) {
    __imp__sub_820D8F20(ctx, base);
    return;
  }
  const uint32_t action = ctx.r3.u32;
  if (action == g_main_action) g_main_action = 0;
  if (const uint32_t track = Read32(base, action + kActionTrackObject)) {
    scrooby::Call(__imp__sub_8210C068, ctx, base, track, 0);
  }
  if (const uint32_t fe = FrontEnd(base)) *GuestPtr(base, fe + kMenuFlags) |= kMenusTakeInput;
}
