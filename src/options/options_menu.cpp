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
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <string>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../data/fight_tree.h"
#include "../guest_memory.h"
#include "../ui/game_font.h"
#include "../achievements/achievements.h"
#include "../achievements/achievements_screen.h"
#include "../players/more_players.h"
#include "../players/players_setup.h"
#include "../ui/quit_game.h"
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
// ...except the ACHIEVEMENTS LIST (achievements_screen.h): it is drawn at
// the game's own size, the full glass panel of the pause menu (the
// request may still be pending when the in-game screen's Enter scales).
bool ListMode();
float DrawScale() { return ListMode() ? 1.0f : kPageScale; }
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
  // THE ACHIEVEMENTS SCREEN (achievements/achievements_screen.h): this
  // screen in its second mode. The selected grid cell (kept between visits),
  // the pointer's move count last seen, the main menu page's title.
  bool achievements = false;
  int cell = 0;
  uint32_t pointer_moves = 0;
  uint32_t title = 0;          // the main menu page's "Options" title (OptTitle)
  uint32_t paused_title = 0;   // the in-game page's "P1 Paused" (the game's own text)
  // Its elements (options_page.cpp THE ACHIEVEMENTS LIST): the heading and
  // the cards; none when the pictures couldn't be made. ach_top = the
  // achievement on the first card (the list scrolls one card at a time).
  struct AchCard {
    uint32_t glass = 0, ring = 0, icon = 0, name = 0, state = 0, desc = 0;
  };
  std::vector<AchCard> ach_cards;
  uint32_t ach_heading = 0, ach_up = 0, ach_down = 0;
  int ach_top = 0;
  // THE PLAYERS SCREEN (players/players_setup.h): this screen in its third
  // mode. Its four cards (options_page.cpp AddPlayersCards; none when the
  // device pictures couldn't be made), the selected card (kept between
  // visits), and what the cards were last drawn for (redrawn on a change).
  bool players = false;
  struct PlyCard {
    uint32_t back = 0, glass = 0, aura = 0, ring = 0, pic = 0, title = 0, info = 0, style = 0, left = 0, right = 0;
  };
  std::vector<PlyCard> ply_cards;
  // Our copy of the panel's hinge, drawn over the cards (options_page.cpp
  // PlyHinge), and whether the game's own one (under player 1's card) is
  // hidden yet this visit (it goes on ply_hidden: back at Exit).
  uint32_t ply_hinge = 0;
  bool ply_hinge_swapped = false;
  // THE PRESS INDICATOR (players_setup.h PollPresses): when each card's
  // device last pressed something (steady-clock seconds; < 0 = not lately).
  std::array<double, 4> ply_flash{-1, -1, -1, -1};
  // THE CORNER GLOW only for a player who's IN (in a level: playing; in the
  // main menu: has a device), fading in / out over kAuraFade when that
  // changes: its alpha now (0-1), and when it was last stepped (< 0 = the
  // screen just opened: no fade, straight to what it should be).
  std::array<float, 4> ply_aura{};
  double ply_aura_time = -1;
  std::array<uint32_t, options_page::kPlyPrompts> ply_prompt{};
  std::array<int, options_page::kPlyPrompts> ply_prompt_action{};  // what a click on each does
  int ply_sel = 0;
  int ply_choosing = -1;    // the card whose controller is being chosen (-1 none)
  // A SELECTED CARD (A on it: ply_choosing) has two rows: its controller
  // (left / right = the controllers connected, A = take the one shown) and,
  // DOWN from there, its BUTTONS LINE (left / right / A = the player's button
  // pictures; up = back to the controller). B leaves the card. (2026-10-29:
  // first LB / RB on a card, then a Buttons stop in the cards' own up / down
  // order: both replaced at the player's request.) True = the Buttons row.
  bool ply_on_style = false;
  int ply_choice = 0;       // the controller on offer (index into players_setup::Controllers())
  bool ply_hold_b = false;  // the B that cancelled a choice: hidden from the game until let go
  std::vector<uint32_t> ply_hidden;  // the game's prompts / title hidden while the screen is up
  std::string ply_drawn;
};
Screen g;
bool ListMode() {
  return g.achievements || g.players || achievements_screen::OpenNextPending() || players_setup::OpenNextPending();
}
// The Options screen's other modes (Achievements, Players): no rows, tabs
// or side card.
bool OtherMode() { return g.achievements || g.players; }
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
// The Players screen's chooser is open (or the B that cancelled it is still
// held): the game doesn't see B (FilterPad), the screen does (its
// own reading, SeePad, comes first). Holding back only the screen's decision
// wasn't enough: the screen's own action heard that B and slid away, and the
// front end stayed in the Options state with nothing on screen.
std::atomic<bool> g_players_take_b{false};

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
bool Slides() { return !OtherMode() && int(Rows().size()) > kRows; }
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
  // The Achievements list's cards (as many as the page has).
  g.ach_cards.clear();
  for (int k = 0;; ++k) {
    Screen::AchCard c;
    c.glass = FindPicture(ctx, base, g.page, fmt::format("AchCard{}", k));
    if (!c.glass) break;
    c.ring = FindPicture(ctx, base, g.page, fmt::format("AchCardRing{}", k));
    c.icon = FindPicture(ctx, base, g.page, fmt::format("AchCardIcon{}", k));
    c.name = FindText(ctx, base, g.page, fmt::format("AchCardName{}", k));
    c.state = FindText(ctx, base, g.page, fmt::format("AchCardState{}", k));
    c.desc = FindText(ctx, base, g.page, fmt::format("AchCardDesc{}", k));
    g.ach_cards.push_back(c);
  }
  // The Players screen's cards and prompt row.
  g.ply_cards.clear();
  for (int k = 0; k < 4; ++k) {
    Screen::PlyCard c;
    c.glass = FindPicture(ctx, base, g.page, fmt::format("PlyCard{}", k));
    if (!c.glass) break;
    c.back = FindPicture(ctx, base, g.page, fmt::format("PlyBack{}", k));
    c.aura = FindPicture(ctx, base, g.page, fmt::format("PlyAura{}", k));
    c.ring = FindPicture(ctx, base, g.page, fmt::format("PlyRing{}", k));
    c.pic = FindPicture(ctx, base, g.page, fmt::format("PlyPic{}", k));
    c.title = FindText(ctx, base, g.page, fmt::format("PlyTitle{}", k));
    c.info = FindText(ctx, base, g.page, fmt::format("PlyInfo{}", k));
    c.style = FindText(ctx, base, g.page, fmt::format("PlyStyle{}", k));
    c.left = FindPicture(ctx, base, g.page, fmt::format("PlyLeft{}", k));
    c.right = FindPicture(ctx, base, g.page, fmt::format("PlyRight{}", k));
    g.ply_cards.push_back(c);
  }
  g.ply_hinge = FindPicture(ctx, base, g.page, "PlyHinge");
  for (int i = 0; i < options_page::kPlyPrompts; ++i) {
    g.ply_prompt[size_t(i)] = FindText(ctx, base, g.page, fmt::format("PlyPrompt{}", i));
  }
  g.ach_heading = FindText(ctx, base, g.page, "AchHeading");
  g.ach_up = FindPicture(ctx, base, g.page, "AchUp");
  g.ach_down = FindPicture(ctx, base, g.page, "AchDown");
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

void RefreshAchievements(Env& env);  // the Achievements screen's (below)
void RefreshPlayers(Env& env);       // the Players screen's (below)

// Everything that depends on the tab, the values and the selection.
void Refresh(Env& env) {
  if (g.achievements) {
    RefreshAchievements(env);
    return;
  }
  if (g.players) {
    RefreshPlayers(env);
    return;
  }
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

// ---------------------------------------------------------------------------
// THE ACHIEVEMENTS SCREEN (achievements/achievements_screen.h)
// ---------------------------------------------------------------------------
// THE LIST (options_page.cpp THE ACHIEVEMENTS LIST): cards of 540 x 78
// page units from x 50; texts at line 1 (name left, state right) and line 2
// (the description, small print). Their boxes in the data are full size;
// the small ones are scaled around their box's centre, so their boxes are
// moved to keep the edge they're read from (left / right) in place.
constexpr float kAchTextLeft = options_page::kAchTextLeft, kAchTextRight = options_page::kAchTextRight;
constexpr float kAchStateScale = 0.8f, kAchDescScale = 0.72f, kAchDescMinScale = 0.55f;
// Colours (0xAARRGGBB): the menus' own (options_page.cpp kGreen / kYellow /
// kCyan); the selected card's glass solid, the others see-through like the
// save list's (#dc alpha) and a little more; locked: picture and texts
// darkened.
constexpr uint32_t kAchGreen = 0xFF00FF00, kAchYellow = 0xFFFFF110, kAchCyan = 0xFF00FFF7;
constexpr uint32_t kAchGlassSelected = 0xFFFFFFFF, kAchGlass = 0xA0FFFFFF;
constexpr uint32_t kEarnedTint = 0xFFFFFFFF, kLockedTint = 0xFF4C4C4C;
constexpr uint32_t kLockedName = 0xFF5FA05F, kLockedState = 0xFFB0B0B0;

void ShowAchievementElements(uint8_t* base, bool shown) {
  for (const Screen::AchCard& c : g.ach_cards) {
    for (const uint32_t e : {c.glass, c.ring, c.icon, c.name, c.state, c.desc}) scrooby::SetVisible(base, e, shown);
  }
  scrooby::SetVisible(base, g.ach_heading, shown);
  scrooby::SetVisible(base, g.ach_up, shown);
  scrooby::SetVisible(base, g.ach_down, shown);
}

// The Options screen's SIDE CARD, hidden while the list is up and back
// after (LayOutCard picks which of its glass shapes shows). The glass
// panel, cogs, hinge and title stay: the list sits in the game's own panel.
void ShowOptionsFurniture(PPCContext& ctx, uint8_t* base, bool shown) {
  (void)ctx;
  for (const uint32_t e : g.card_glass) {
    if (!shown) scrooby::SetVisible(base, e, false);
  }
  for (const uint32_t e : {g.card_value, g.desc, g.warn, g.guide}) {
    if (e) scrooby::SetVisible(base, e, shown);
  }
}

// THE PANEL CENTRED FOR THE LIST: the Options screen's pieces sit moved left
// by kGlassShift (room for its side card): for the list they go back to
// the middle (+110), the list's arrows beside the cards on the right. In
// game the shared glass page isn't moved at all (MoveGlassForCard skips it);
// here: the in-game title ("P1 Paused"), and in the main menu the page's own
// panel, cogs, hinge and title. Every element's place as found is kept and
// put back at Exit.
struct Moved {
  uint32_t element, x, y;  // the +92 / +96 words as found
};
std::vector<Moved> g_moved;
void Remember(const uint8_t* base, uint32_t e) {
  if (e) g_moved.push_back({e, Read32(base, e + kElementX), Read32(base, e + kElementY)});
}
void CentreForList(PPCContext& ctx, uint8_t* base) {
  g_moved.clear();
  std::vector<uint32_t> shifted = {g.paused_title, g.title};
  if (g.main_menu) {
    for (const char* name : {"OptionsMenu_Panel", "cog1", "OptCog2", "OptionsMenu_PanelHinge"}) {
      shifted.push_back(scrooby::FindPicture(ctx, base, g.page, name));
    }
  }
  for (const uint32_t e : shifted) {
    if (!e) continue;
    Remember(base, e);
    MoveX(base, e, -options_page::kGlassShift);
  }
  // The list's own arrows (AchUp / AchDown), sized and turned like the
  // Options list's markers.
  scrooby::SetScale(ctx, base, g.ach_up, kPageArrowScale);
  scrooby::Rotate(ctx, base, g.ach_up, 180.0f);
  scrooby::SetScale(ctx, base, g.ach_down, kPageArrowScale);
}
void PutBackAfterList(uint8_t* base) {
  for (const Moved& m : g_moved) {
    for (const auto& [field, word] : {std::pair{kElementX, m.x}, std::pair{kElementY, m.y}}) {
      uint8_t* p = GuestPtr(base, m.element + field);
      for (int b = 0; b < 4; ++b) p[b] = uint8_t(word >> (24 - 8 * b));
    }
  }
  g_moved.clear();
}

std::u16string Utf16(std::string_view s) {
  std::u16string out;
  for (size_t i = 0; i < s.size();) {
    const uint8_t c = uint8_t(s[i]);
    uint32_t cp = c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (extra) cp = c & (0x3F >> extra);
    ++i;
    for (int k = 0; k < extra && i < s.size(); ++k, ++i) cp = (cp << 6) | (uint8_t(s[i]) & 0x3F);
    if (cp < 0x10000) out.push_back(char16_t(cp));
  }
  return out;
}

// "12 Oct 2026" (the player's own time zone).
std::u16string DateText(int64_t unix_seconds) {
  const std::time_t t = std::time_t(unix_seconds);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char text[32];
  std::strftime(text, sizeof(text), "%d %b %Y", &tm);
  std::string s = text;
  if (s.size() > 1 && s[0] == '0') s.erase(0, 1);
  return std::u16string(s.begin(), s.end());
}

// A text's box placed so that, drawn at `scale` around the box's centre,
// its LEFT (right_edge false) or RIGHT edge lands at x; `y` = the box's
// bottom as in the data.
void PlaceScaled(PPCContext& ctx, uint8_t* base, uint32_t text, float x, float y, float w, float scale,
                 bool right_edge) {
  if (!text) return;
  const float box_x = right_edge ? x - w + (1 - scale) * w / 2 : x - (1 - scale) * w / 2;
  SetBox(base, text, box_x, y, w, 36.0f);
  scrooby::SetScale(ctx, base, text, scale);
}

void RefreshAchievements(Env& env) {
  PPCContext& ctx = env.ctx;
  uint8_t* base = env.base;
  // No rows, no tabs, no page text: the cards are the screen.
  for (int r = 0; r < kRows; ++r) {
    scrooby::SetSelectable(base, scrooby::Item(base, g.menu, r), false);
    scrooby::SetVisible(base, g.label[r], false);
    scrooby::SetVisible(base, g.value[r], false);
    for (int k = 0; k < kStars; ++k) scrooby::SetVisible(base, g.star[r][k], false);
  }
  for (const uint32_t e : {g.tab_prev, g.tab_text, g.tab_next, g.lb, g.rb, g.page_text}) {
    scrooby::SetVisible(base, e, false);
  }
  g_wheel_list = true;  // the wheel scrolls the cards (UpdateAchievements)
  ShowOptionsFurniture(ctx, base, false);
  // The heading in the tabs' line, under the game's "P1 Paused" (in the main
  // menu the panel's own title says "Achievements": Enter).
  scrooby::SetText(ctx, base, g.ach_heading, u"Achievements");
  scrooby::SetVisible(base, g.ach_heading, g.title == 0);
  const std::vector<achievements::Info> list = achievements::List();
  const int cards = int(g.ach_cards.size());
  const int count = int(list.size());
  if (!count || !cards) return;
  g.cell = std::clamp(g.cell, 0, count - 1);
  // The window follows the selection.
  if (g.cell < g.ach_top) g.ach_top = g.cell;
  if (g.cell >= g.ach_top + cards) g.ach_top = g.cell - cards + 1;
  g.ach_top = std::clamp(g.ach_top, 0, std::max(0, count - cards));
  for (int k = 0; k < cards; ++k) {
    const Screen::AchCard& c = g.ach_cards[size_t(k)];
    const int i = g.ach_top + k;
    const bool used = i < count;
    for (const uint32_t e : {c.glass, c.ring, c.icon, c.name, c.state, c.desc}) scrooby::SetVisible(base, e, used);
    if (!used) continue;
    const achievements::Info& a = list[size_t(i)];
    const bool selected = i == g.cell;
    scrooby::SetColour(ctx, base, c.glass, selected ? kAchGlassSelected : kAchGlass);
    const int frame = achievements::SpriteFrame(a.id);
    scrooby::SetVisible(base, c.icon, frame >= 0);
    if (frame >= 0) scrooby::SetFrame(ctx, base, c.icon, uint32_t(frame));
    scrooby::SetColour(ctx, base, c.icon, a.unlocked ? kEarnedTint : kLockedTint);
    // Texts: line 1 = name + state, line 2 = the description (the game's
    // "how to earn it" text while locked), shrunk if it's too long.
    const float card_top = float(options_page::kAchCardsTop - options_page::kAchCardStep * k);
    const std::u16string name = Utf16(a.name);
    scrooby::SetText(ctx, base, c.name, name);
    scrooby::SetColour(ctx, base, c.name, selected ? kAchYellow : a.unlocked ? kAchGreen : kLockedName);
    const std::u16string state = a.unlocked ? u"Earned " + DateText(a.unlocked_at) : std::u16string(u"Locked");
    // The name gets the line up to the state (a gap of 16): smaller when it
    // doesn't fit ("Would You Like Fries With That?" ran into the date).
    {
      const float room = kAchTextRight - TextWidth(state, kAchStateScale) - 16 - kAchTextLeft;
      const float w = TextWidth(name, 1.0f);
      const float scale = w > room && w > 0 ? std::max(0.6f, room / w) : 1.0f;
      PlaceScaled(ctx, base, c.name, kAchTextLeft, card_top - 38, kAchTextRight - kAchTextLeft, scale, false);
    }
    scrooby::SetText(ctx, base, c.state, state);
    scrooby::SetColour(ctx, base, c.state, a.unlocked ? kAchYellow : kLockedState);
    PlaceScaled(ctx, base, c.state, kAchTextRight, card_top - 38, 260, kAchStateScale, true);
    const std::u16string desc = Utf16(a.unlocked ? a.description : a.locked_description);
    const float room = kAchTextRight - kAchTextLeft;
    float scale = kAchDescScale;
    const float w = TextWidth(desc, 1.0f);
    if (w * scale > room) scale = std::max(kAchDescMinScale, room / w);
    scrooby::SetText(ctx, base, c.desc, desc);
    scrooby::SetColour(ctx, base, c.desc, kAchCyan);
    PlaceScaled(ctx, base, c.desc, kAchTextLeft, card_top - 66, room, scale, false);
  }
  // The list's arrows beside the cards: more above / below.
  const bool more = count > cards;
  scrooby::SetVisible(base, g.page_up, false);
  scrooby::SetVisible(base, g.page_down, false);
  scrooby::SetVisible(base, g.ach_up, more);
  scrooby::SetVisible(base, g.ach_down, more);
  if (more) {
    scrooby::SetColour(ctx, base, g.ach_up, g.ach_top > 0 ? kArrowOn : kArrowFaded);
    scrooby::SetColour(ctx, base, g.ach_down, g.ach_top + cards < count ? kArrowOn : kArrowFaded);
  }
  // No X / Y actions here.
  if (g.x_showing) {
    scrooby::SetText(ctx, base, g.x_button, u"");
    scrooby::SetText(ctx, base, g.x_text, u"");
    g.x_showing = false;
  }
  if (g.y_showing) {
    scrooby::SetText(ctx, base, g.y_button, u"");
    scrooby::SetText(ctx, base, g.y_text, u"");
    g.y_showing = false;
  }
}

// Every Update while the Achievements screen is up: the selection (up /
// down one card, left / right four, through the game's own menu buttons;
// the mouse by hovering a card, its wheel scrolls; the list's arrows
// clicked). One sound per move, the game's menu sounds: up "up_right",
// down "down_left" (the menu's own up / down, which would sound too, is
// told "no move": TurnPage).
void UpdateAchievements(PPCContext& ctx, uint8_t* base, uint32_t fe, int owner) {
  // No "Select A" (A does nothing here); the screen's own tracks may write
  // the prompt bar again, so every frame. The glass layer too (in game the
  // screen action finds it after our Enter).
  scrooby::SetText(ctx, base, g.select_button, u"");
  scrooby::SetText(ctx, base, g.select_text, u"");
  ShowOptionsFurniture(ctx, base, false);
  if (!fe || !(*GuestPtr(base, fe + kMenuFlags) & 0x40)) return;  // sliding
  const int count = int(achievements::List().size());
  const int cards = int(g.ach_cards.size());
  if (!count || !cards) return;
  int cell = g.cell, top = g.ach_top;
  if (Pressed(ctx, base, 45)) cell = std::max(0, cell - 1);
  if (Pressed(ctx, base, 16)) cell = std::min(count - 1, cell + 1);
  if (Pressed(ctx, base, kButtonLeft)) cell = std::max(0, cell - cards);
  if (Pressed(ctx, base, kButtonRight)) cell = std::min(count - 1, cell + cards);
  // The mouse: the card under the pointer, once it moves; the wheel and
  // the arrows scroll (the selection stays on a card on screen).
  const uint32_t layer = MainMenuLayer(base);
  const uint32_t moves = mouse_menus::PointerMoves();
  if (moves != g.pointer_moves) {
    g.pointer_moves = moves;
    for (int k = 0; k < cards && top + k < count; ++k) {
      if (mouse_menus::PointerOver(base, g.ach_cards[size_t(k)].glass, g.page, layer)) cell = top + k;
    }
  }
  int scroll = 0;
  if (auto* driver = kbm::KeyboardMouseDriver::Get()) scroll = driver->TakeListWheel();
  if (mouse_menus::TakeClickOver(base, g.ach_up, g.page, layer)) scroll = -1;
  if (mouse_menus::TakeClickOver(base, g.ach_down, g.page, layer)) scroll = +1;
  if (scroll) {
    top = std::clamp(top + scroll, 0, std::max(0, count - cards));
    cell = std::clamp(cell, top, top + cards - 1);
  }
  // The main menu's copy: B = back (as the Options rows' code does).
  const uint16_t pad = g_pad[size_t(owner)].load(std::memory_order_relaxed);
  const uint16_t pressed = pad & ~g.seen;
  g.seen = pad;
  if (g.main_menu && (pressed & kPadB) && !CloseMainMenuOptions(ctx, base)) {
    *GuestPtr(base, fe + kResultFlags) |= kFlagCancel;
  }
  if (cell != g.cell || top != g.ach_top) {
    if (cell != g.cell) {
      scrooby::PlaySound(ctx, base, cell < g.cell ? scrooby::Sound::kChange : scrooby::Sound::kMove);
    }
    g.cell = cell;
    g.ach_top = top;
    Env env{ctx, base, owner};
    RefreshAchievements(env);
  }
}

// ---------------------------------------------------------------------------
// THE PLAYERS SCREEN (players/players_setup.h)
// ---------------------------------------------------------------------------
// The design of 2026-10-29: four cards two by two in the game's glass
// panel, each "Player N" (the player's colour; yellow and pulsing when
// selected), the device picture, the device's name (or "Press any button to
// join", blinking). Player 1's card shows the device player 1 used last.
// A opens the CHOOSER on the selected card (left / right = every controller
// connected, A gives it to that player, B cancels); X / Backspace removes
// the player's controller; B / Esc goes back. The
// prompt row under the panel shows only what can be done. Texts are
// written only when they change (each change allocates on the menus' heap);
// the pulse and blink are a scale / an alpha, every frame.
constexpr uint32_t kPlyYellow = 0xFFFDED06, kPlyGreen = 0xFF00F800, kPlyCyan = 0xFF00F6EF;
constexpr float kPlyInfoScale = 0.72f, kPlyPromptScale = 0.9f, kPlyMinScale = 0.42f;
constexpr float kPlyStyleScale = 0.56f;  // the button pictures line: small print
constexpr float kPlyNumberScale = 1.7f;  // the player's number in the card's corner
constexpr char16_t kGlyphB = u'²';  // the font's B button picture
constexpr int kPlyNone = -1;

// The buttons a controller that just joined holds: hidden from the menus'
// reading on sockets 1 and `user` until let go (defined with FilterPad).
void HideHeld(int user, uint16_t buttons);
uint16_t HiddenOn(int user);

// A text that fits `room` units at most at `scale` (smaller, down to
// kPlyMinScale, when it doesn't). (TextWidth's units are the Options
// screen's, drawn at kPageScale; this screen is drawn at full size: names
// measured that way ran ~15 % past the cards' edge.)
float FitScale(std::u16string_view text, float room, float scale) {
  const float w = TextWidth(text, 1.0f) / kPageScale;
  return w * scale > room && w > 0 ? std::max(kPlyMinScale, room / w) : scale;
}

void ShowPlayersElements(uint8_t* base, bool shown) {
  for (const Screen::PlyCard& c : g.ply_cards) {
    for (const uint32_t e : {c.back, c.glass, c.aura, c.ring, c.pic, c.title, c.info, c.style, c.left, c.right}) {
      scrooby::SetVisible(base, e, shown);
    }
  }
  for (const uint32_t e : g.ply_prompt) scrooby::SetVisible(base, e, shown);
  scrooby::SetVisible(base, g.ply_hinge, shown);
}

// While the screen is up the game's own prompts (FE_Buttons: Back...) and
// the page title ("P1 Paused" / the main menu's "Options") give way to the
// screen's prompt row; put back as they were at Exit.
void HideGamePrompts(uint8_t* base, bool hide) {
  std::vector<uint32_t> elements(g.prompts.begin(), g.prompts.end());
  elements.push_back(g.paused_title);
  elements.push_back(g.title);
  if (hide) {
    g.ply_hidden.clear();
    g.ply_hinge_swapped = false;
    for (const uint32_t e : elements) {
      if (e && scrooby::Visible(base, e)) {
        g.ply_hidden.push_back(e);
        scrooby::SetVisible(base, e, false);
      }
    }
  } else {
    for (const uint32_t e : g.ply_hidden) scrooby::SetVisible(base, e, true);
    g.ply_hidden.clear();
    g.ply_hinge_swapped = false;
  }
}

// The game's own hinge hidden while ours (over the cards) shows: both drawn
// = its soft edges twice as dark. Main menu: on our page; in game: on the
// shared glass page (the screen action's +28), known only once the in-game
// screen's Enter ran (before or after ours), so asked at every refresh until
// found. Put back with the prompts (ply_hidden).
void SwapHinge(PPCContext& ctx, uint8_t* base) {
  if (g.ply_hinge_swapped || !g.ply_hinge) return;
  uint32_t original = 0;
  if (g.main_menu) {
    original = scrooby::FindPicture(ctx, base, g.page, "OptionsMenu_PanelHinge");
  } else if (g_screen_action) {
    if (const uint32_t page = Read32(base, g_screen_action + 28)) original = scrooby::FindPicture(ctx, base, page, "Hinge");
  }
  if (!original) return;
  g.ply_hinge_swapped = true;
  if (scrooby::Visible(base, original)) {
    g.ply_hidden.push_back(original);
    scrooby::SetVisible(base, original, false);
  }
}

// The device a card shows: player 1's in use (keys or its controller), the
// first controller of the others; null = none.
const players_setup::ShownDevice* ShownDevice(const players_setup::Box& b, int p) {
  if (b.devices.empty()) return nullptr;
  if (p == 0 && b.has_pad && !prompt_icons::KeysLookFor(0)) {
    for (const players_setup::ShownDevice& d : b.devices) {
      if (!d.keyboard) return &d;
    }
  }
  return &b.devices[0];
}

struct PlyPrompt {
  char16_t glyph;
  std::u16string label;
  int action;  // what a click on it does (kPlyAct*)
};
enum { kPlyActChoose, kPlyActRemove, kPlyActStyle, kPlyActBack };

// Does card k show its BUTTONS LINE (players_setup.h PadIcons)? With a
// controller only, and not while it shows player 1's keyboard (keys have no
// button pictures to choose).
bool StyleLineShown(const players_setup::Box& b, int k) {
  const players_setup::ShownDevice* d = ShownDevice(b, k);
  return b.has_pad && !(d && d->keyboard);
}

// The prompts for the selection as it is.
std::vector<PlyPrompt> PlayersPrompts(const std::array<players_setup::Box, 4>& boxes, size_t controllers) {
  const char16_t remove = prompt_icons::KeysLookFor(0) ? kGlyphY : kGlyphX;  // Backspace on keys
  std::vector<PlyPrompt> out;
  const players_setup::Box& sel = boxes[size_t(g.ply_sel)];
  if (g.ply_choosing != kPlyNone && g.ply_on_style) {
    out.push_back({kGlyphA, u"Change Buttons", kPlyActStyle});
    out.push_back({kGlyphB, u"Back", kPlyActBack});
  } else if (g.ply_choosing != kPlyNone) {
    out.push_back({kGlyphA, u"Select Controller", kPlyActChoose});
    out.push_back({kGlyphB, u"Back", kPlyActBack});
  } else {
    if (controllers && sel.state != players_setup::State::kUnavailable) {
      out.push_back({kGlyphA, u"Select", kPlyActChoose});
    }
    if (sel.has_pad) out.push_back({remove, u"Remove", kPlyActRemove});
    out.push_back({kGlyphB, u"Back", kPlyActBack});
  }
  return out;
}

// The selected card's arrows (the map's arrow, turned): `inset` units in
// from the card's sides, centred `centre` units below its top, at `scale`
// of their size. Turned again round the new place each time (the turn is a
// transform built round the box: a moved box alone left the arrows where
// the old turn put them).
constexpr float kPlyLineArrowScale = 0.75f;  // beside the Buttons line
void PlaceArrows(PPCContext& ctx, uint8_t* base, const Screen::PlyCard& c, int k, float inset, float centre,
                 float scale) {
  const float cx = float(options_page::kPlyCardX[k % 2]), ctop = float(options_page::kPlyCardTop[k / 2]);
  const float side = float(options_page::kPlyArrowSide) * scale, y = ctop - centre - side / 2;
  // Smaller, the turned arrows don't shrink round their centres: the left
  // one (+90) sat (1 - scale) x its full size lower and ~0.4 of that further
  // out, the right one (-90) ~0.6 of it further out (measured on photos at
  // 0.75: 7 units down; gaps to the text 17 vs 30 px after the first fix).
  // Moved back so both sit level, the same distance from the text.
  const float shift = float(options_page::kPlyArrowSide) * (1.0f - scale);
  SetBox(base, c.left, cx + inset + 0.4f * shift, y + shift, side, side);
  SetBox(base, c.right, cx + float(options_page::kPlyCardW) - inset - side - 0.6f * shift, y, side, side);
  scrooby::SetScale(ctx, base, c.left, scale);
  scrooby::Rotate(ctx, base, c.left, 90.0f);
  scrooby::SetScale(ctx, base, c.right, scale);
  scrooby::Rotate(ctx, base, c.right, -90.0f);
}

void RefreshPlayers(Env& env) {
  PPCContext& ctx = env.ctx;
  uint8_t* base = env.base;
  SwapHinge(ctx, base);
  const auto boxes = players_setup::Boxes();
  const auto controllers = players_setup::Controllers();
  if (g.ply_choosing != kPlyNone && controllers.empty()) g.ply_choosing = kPlyNone;  // all unplugged
  if (!controllers.empty()) g.ply_choice = std::clamp(g.ply_choice, 0, int(controllers.size()) - 1);
  const std::vector<PlyPrompt> prompts = PlayersPrompts(boxes, controllers.size());
  // What the screen would say (a change = redraw).
  if (g.ply_on_style && (g.ply_choosing == kPlyNone || !boxes[size_t(g.ply_choosing)].has_pad)) {
    g.ply_on_style = false;  // no card selected / its controller gone: no Buttons row
  }
  std::string drawn = fmt::format("{}|{}|{}|{}|{}|", g.ply_sel, g.ply_choosing, g.ply_choice, prompt_icons::KeysLookFor(0),
                                  g.ply_on_style);
  for (int k = 0; k < 4; ++k) {
    const players_setup::ShownDevice* d = ShownDevice(boxes[size_t(k)], k);
    drawn += fmt::format("{}:{}:{}:{};", d ? d->key : "", d ? d->picture : -1, boxes[size_t(k)].has_pad,
                         players_setup::PadIcons(k));
  }
  for (const players_setup::Controller& c : controllers) drawn += fmt::format("{}={},", c.key, c.player);
  if (drawn == g.ply_drawn) return;
  g.ply_drawn = drawn;
  // No rows, tabs or page texts: the cards are the screen.
  for (int r = 0; r < kRows; ++r) {
    scrooby::SetSelectable(base, scrooby::Item(base, g.menu, r), false);
    scrooby::SetVisible(base, g.label[r], false);
    scrooby::SetVisible(base, g.value[r], false);
    for (int k = 0; k < kStars; ++k) scrooby::SetVisible(base, g.star[r][k], false);
  }
  for (const uint32_t e : {g.tab_prev, g.tab_text, g.tab_next, g.lb, g.rb, g.page_text, g.page_up, g.page_down}) {
    scrooby::SetVisible(base, e, false);
  }
  ShowAchievementElements(base, false);
  ShowOptionsFurniture(ctx, base, false);
  scrooby::SetVisible(base, g.ply_hinge, true);  // ours over the cards (SwapHinge hid the game's)
  g_wheel_list = true;  // the wheel does nothing here (not the D-pad)
  for (int k = 0; k < 4; ++k) {
    const Screen::PlyCard& c = g.ply_cards[size_t(k)];
    const players_setup::Box& b = boxes[size_t(k)];
    const players_setup::ShownDevice* d = ShownDevice(b, k);
    const bool selected = k == g.ply_sel, choosing = k == g.ply_choosing;
    // The selected card's rows: offering = its controller row (the
    // controller on offer shown), style_focus = its Buttons row.
    const bool offering = choosing && !g.ply_on_style, style_focus = choosing && g.ply_on_style;
    for (const uint32_t e : {c.back, c.glass, c.pic, c.title, c.info}) scrooby::SetVisible(base, e, true);
    scrooby::SetVisible(base, c.ring, selected);
    if (selected) scrooby::SetFrame(ctx, base, c.ring, choosing ? uint32_t(1 + k) : 0u);  // choosing: their colour
    // The arrows: beside the picture while choosing a controller, beside the
    // Buttons line while it has the focus (same arrows, moved).
    scrooby::SetVisible(base, c.left, offering || style_focus);
    scrooby::SetVisible(base, c.right, offering || style_focus);
    // (Beside the picture: placed here; beside the Buttons line: below, once
    // the line's width is known.)
    if (offering) PlaceArrows(ctx, base, c, k, options_page::kPlyArrowInset, float(options_page::kPlyPicCentre[k / 2]), 1.0f);
    // The player's NUMBER, big, in their colour (the selected card is told by
    // its ring and the number's pulse).
    scrooby::SetText(ctx, base, c.title, Utf16(fmt::format("{}", k + 1)));
    scrooby::SetColour(ctx, base, c.title, players_setup::kPlayerColour[k]);
    std::u16string info;
    uint32_t colour = kPlyGreen;
    int picture = d ? d->picture : players_setup::kPicEmpty;
    if (offering) {
      // The controller on offer: its picture (in this player's colour), its
      // name, and whose it is now.
      const players_setup::Controller& o = controllers[size_t(g.ply_choice)];
      picture = o.picture;
      info = Utf16(o.player == k   ? o.name + " (now)"
                   : o.player >= 0 ? fmt::format("{} (Player {})", o.name, o.player + 1)
                                   : o.name);
      colour = kPlyYellow;
    } else if (d) {
      info = Utf16(d->name);
    } else {
      info = u"Press any button to join";
      colour = kPlyCyan;
    }
    scrooby::SetFrame(ctx, base, c.pic, uint32_t(picture));
    scrooby::SetText(ctx, base, c.info, info);
    scrooby::SetColour(ctx, base, c.info, colour);
    const float x = float(options_page::kPlyCardX[k % 2]), top = float(options_page::kPlyCardTop[k / 2]);
    const float room = float(options_page::kPlyCardW - 16);
    SetBox(base, c.info, x, top - options_page::kPlyInfoTop[k / 2] - 36.0f, float(options_page::kPlyCardW), 36.0f);
    scrooby::SetScale(ctx, base, c.info, FitScale(info, room, kPlyInfoScale));
    // The button pictures line (players_setup.h PadIcons), with a controller
    // only (LB / RB change it; the chooser shows the controller on offer),
    // and not while the card shows player 1's keyboard (its keys have no
    // button pictures to choose).
    // (A selected card shows it whenever it has a controller: the row to go
    // down to, even while it shows player 1's keyboard.)
    const bool style_shown = choosing ? b.has_pad : StyleLineShown(b, k);
    scrooby::SetVisible(base, c.style, style_shown);
    if (style_shown) {
      // With the focus: the style's NAME only, between the arrows ("Buttons:"
      // dropped), and the arrows at FIXED places: just outside the widest
      // name ("PlayStation 5"), so they stay put while the names change (at
      // first they hugged each name: "Switch" pulled them in, 2026-10-29).
      // The arrows 0.75x.
      const char* name = players_setup::StyleName(players_setup::PadIcons(k));
      const std::u16string style = style_focus ? Utf16(name) : Utf16(fmt::format("Buttons: {}", name));
      scrooby::SetText(ctx, base, c.style, style);
      scrooby::SetColour(ctx, base, c.style, style_focus ? kPlyYellow : kPlyCyan);
      SetBox(base, c.style, x, top - options_page::kPlyStyleTop[k / 2] - 36.0f, float(options_page::kPlyCardW), 36.0f);
      const float arrow = kPlyLineArrowScale * float(options_page::kPlyArrowSide);
      if (style_focus) {
        static const std::u16string kWidest = u"PlayStation 5";
        const float scale = FitScale(kWidest, room - 2.0f * (arrow + 6.0f), kPlyStyleScale);
        scrooby::SetScale(ctx, base, c.style, scale);
        const float half = TextWidth(kWidest, 1.0f) / kPageScale * scale / 2.0f;
        const float inset = float(options_page::kPlyCardW) / 2.0f - half - 6.0f - arrow;
        PlaceArrows(ctx, base, c, k, inset, float(options_page::kPlyStyleTop[k / 2]) + 18.0f, kPlyLineArrowScale);
      } else {
        scrooby::SetScale(ctx, base, c.style, FitScale(style, room, kPlyStyleScale));
      }
    }
  }
  // The prompt row, centred under the panel: "<glyph> <label>", 24 apart.
  float widths[options_page::kPlyPrompts] = {};
  float total = 0;
  const int n = std::min(int(prompts.size()), options_page::kPlyPrompts);
  std::array<std::u16string, options_page::kPlyPrompts> texts;
  for (int i = 0; i < n; ++i) {
    texts[size_t(i)] = std::u16string(1, prompt_icons::MenuGlyph(prompts[size_t(i)].glyph)) + u" " +
                       prompts[size_t(i)].label;
    widths[i] = TextWidth(texts[size_t(i)], 1.0f) / kPageScale * kPlyPromptScale;
    total += widths[i] + (i ? 24.0f : 0.0f);
  }
  float left = 320.0f - total / 2;
  for (int i = 0; i < options_page::kPlyPrompts; ++i) {
    const uint32_t t = g.ply_prompt[size_t(i)];
    scrooby::SetVisible(base, t, i < n);
    g.ply_prompt_action[size_t(i)] = i < n ? prompts[size_t(i)].action : -1;
    if (i >= n) continue;
    scrooby::SetText(ctx, base, t, texts[size_t(i)]);
    // A box as wide as the text (the mouse clicks it), drawn scaled round
    // its centre: the box is the scaled width's / the scale.
    const float w = widths[i] / kPlyPromptScale;
    SetBox(base, t, left + widths[i] / 2 - w / 2, options_page::kPlyPromptTop - 36.0f, w, 36.0f);
    scrooby::SetScale(ctx, base, t, kPlyPromptScale);
    left += widths[i] + 24.0f;
  }
}

// A: open the chooser on the selected card (on its own controller, else the
// first one), or give the controller on offer to that player.
void PlayersAct(PPCContext& ctx, uint8_t* base, int action) {
  const auto boxes = players_setup::Boxes();
  const auto controllers = players_setup::Controllers();
  if (action == kPlyActChoose && g.ply_on_style && g.ply_choosing != kPlyNone) action = kPlyActStyle;
  if (action == kPlyActChoose) {
    if (g.ply_choosing == kPlyNone) {
      if (controllers.empty() || boxes[size_t(g.ply_sel)].state == players_setup::State::kUnavailable) return;
      g.ply_choosing = g.ply_sel;
      g.ply_choice = 0;
      for (int i = 0; i < int(controllers.size()); ++i) {
        if (controllers[size_t(i)].player == g.ply_sel) {
          g.ply_choice = i;
          break;
        }
      }
      scrooby::PlaySound(ctx, base, scrooby::Sound::kChange);
    } else {
      const int p = g.ply_choosing;
      g.ply_choosing = kPlyNone;
      g.ply_on_style = false;
      if (g.ply_choice < int(controllers.size()) &&
          players_setup::AssignController(ctx, base, p, controllers[size_t(g.ply_choice)].key)) {
        scrooby::PlaySound(ctx, base, scrooby::Sound::kChange);
      } else {
        scrooby::PlaySound(ctx, base, scrooby::Sound::kMove);
      }
    }
  } else if (action == kPlyActStyle || action == -kPlyActStyle) {
    // The selected player's button pictures, one style on (right / A / a
    // click on the line) or back (left), wrapping: Automatic, Xbox 360 ..
    // Switch.
    if (!boxes[size_t(g.ply_sel)].has_pad) return;
    const int step = action > 0 ? 1 : -1;
    players_setup::SetPadIcons(g.ply_sel, (players_setup::PadIcons(g.ply_sel) + step + 6) % 6);
    scrooby::PlaySound(ctx, base, step < 0 ? scrooby::Sound::kMove : scrooby::Sound::kChange);
  } else if (action == kPlyActRemove && g.ply_choosing == kPlyNone) {
    if (players_setup::RemoveControllers(ctx, base, g.ply_sel)) {
      scrooby::PlaySound(ctx, base, scrooby::Sound::kChange);
    }
  }
}

// The chooser: the next / previous controller (wrapping round).
void PlayersStep(PPCContext& ctx, uint8_t* base, int direction) {
  const int count = int(players_setup::Controllers().size());
  if (g.ply_choosing == kPlyNone || count < 2) return;
  g.ply_choice = ((g.ply_choice + direction) % count + count) % count;
  scrooby::PlaySound(ctx, base, direction < 0 ? scrooby::Sound::kMove : scrooby::Sound::kChange);
}

void UpdatePlayers(PPCContext& ctx, uint8_t* base, uint32_t fe, int owner) {
  scrooby::SetText(ctx, base, g.select_button, u"");
  scrooby::SetText(ctx, base, g.select_text, u"");
  ShowOptionsFurniture(ctx, base, false);
  Env env{ctx, base, owner};
  if (g.ply_cards.size() < 4) return;
  // Joins first: a controller playing as nobody that presses anything.
  uint16_t joined_buttons = 0;
  const int joined = players_setup::PollJoins(ctx, base, &joined_buttons);
  if (joined >= 0) {
    // Its press isn't also a menu press (outside play it answers on socket 1).
    HideHeld(0, joined_buttons);
    HideHeld(joined, joined_buttons);
    g.seen |= joined_buttons;
    if (g.ply_choosing == kPlyNone) g.ply_sel = joined;
    scrooby::PlaySound(ctx, base, scrooby::Sound::kChange);
  }
  // The pulse of the selected name (1.08 <-> 1.02 over 0.9 s) and the blink
  // of the empty cards' line (1 <-> 0.35 over 1.2 s), every frame.
  {
    const double t = double(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count()) /
                     1000.0;
    const float pulse = 1.05f + 0.03f * float(std::cos(t * 2 * 3.14159265 / 0.9));
    const float blink = 0.675f + 0.325f * float(std::cos(t * 2 * 3.14159265 / 1.2));
    const auto boxes = players_setup::Boxes();
    for (int k = 0; k < 4; ++k) {
      const Screen::PlyCard& c = g.ply_cards[size_t(k)];
      scrooby::SetScale(ctx, base, c.title, kPlyNumberScale * (k == g.ply_sel ? pulse : 1.0f));
      const bool empty = boxes[size_t(k)].devices.empty() && k != g.ply_choosing;
      scrooby::SetAlpha(ctx, base, c.info, empty ? uint8_t(255 * blink) : 0xFF);
    }
    // THE CORNER GLOW (see Screen::ply_aura): in = the player has a device
    // (joined on this screen: a controller pressed to join, or chosen; player
    // 1's keyboard always), whether or not they're in the level yet; fades
    // over kAuraFade. (First tried: only players playing in the level, in
    // menus those with a device: most cards stayed bare in the pause menu.)
    {
      constexpr double kAuraFade = 0.5;
      const double dt = g.ply_aura_time < 0 ? 1e9 : std::max(0.0, t - g.ply_aura_time);
      g.ply_aura_time = t;
      for (int k = 0; k < 4; ++k) {
        const bool in = !boxes[size_t(k)].devices.empty();
        float& a = g.ply_aura[size_t(k)];
        const float step = float(dt / kAuraFade);
        a = in ? std::min(1.0f, a + step) : std::max(0.0f, a - step);
        const uint32_t e = g.ply_cards[size_t(k)].aura;
        scrooby::SetVisible(base, e, a > 0.0f);
        if (a > 0.0f) scrooby::SetAlpha(ctx, base, e, uint8_t(255 * (a * a * (3 - 2 * a))));  // eased
      }
    }
    // THE PRESS INDICATOR: a card whose device pressed something (its
    // controller, player 1's keys, the chooser's offered controller): its
    // ring flashes in the player's colour, fading out over kFlash (on the
    // selected card the yellow ring turns their colour meanwhile). Only the
    // card's ring: a first version also popped the controller picture (1.15x),
    // not wanted. Drawn every frame over what
    // RefreshPlayers set; when a flash ends that redraw is asked for again.
    constexpr double kFlash = 0.4;
    const auto controllers = players_setup::Controllers();
    const bool offering =
        g.ply_choosing != kPlyNone && !g.ply_on_style && g.ply_choice < int(controllers.size());
    const uint32_t pressed =
        players_setup::PollPresses(offering ? controllers[size_t(g.ply_choice)].key : std::string(),
                                   offering ? g.ply_choosing : -1);
    for (int k = 0; k < 4; ++k) {
      double& since = g.ply_flash[size_t(k)];
      if (pressed & (1u << k)) since = t;
      if (since < 0) continue;
      const Screen::PlyCard& c = g.ply_cards[size_t(k)];
      const double age = t - since;
      if (age >= kFlash) {  // over: back to the card as drawn
        since = -1;
        scrooby::SetAlpha(ctx, base, c.ring, 0xFF);
        g.ply_drawn.clear();
        continue;
      }
      const float left = float(1.0 - age / kFlash);  // 1 -> 0
      scrooby::SetVisible(base, c.ring, true);
      scrooby::SetFrame(ctx, base, c.ring, uint32_t(1 + k));
      scrooby::SetAlpha(ctx, base, c.ring, k == g.ply_sel ? 0xFF : uint8_t(255 * left));
    }
  }
  if (!fe || !(*GuestPtr(base, fe + kMenuFlags) & 0x40)) {  // sliding
    RefreshPlayers(env);
    return;
  }
  int sel = g.ply_sel;
  bool on_style = g.ply_on_style;
  const bool left = Pressed(ctx, base, kButtonLeft), right = Pressed(ctx, base, kButtonRight);
  const bool up = Pressed(ctx, base, 45), down = Pressed(ctx, base, 16);
  if (g.ply_choosing != kPlyNone && !on_style) {
    // The selected card's controller row: left / right = the controller on
    // offer; DOWN = its Buttons row (with a controller).
    if (left) PlayersStep(ctx, base, -1);
    if (right) PlayersStep(ctx, base, +1);
    // (The bottom row is drawn upside down: its Buttons line sits ABOVE the
    // picture, so there it's UP.)
    const bool toward_line = g.ply_choosing < 2 ? down : up;
    if (toward_line && players_setup::Boxes()[size_t(g.ply_choosing)].has_pad) on_style = true;
  } else if (g.ply_choosing != kPlyNone) {
    // Its Buttons row: left / right = the previous / next style; UP = back
    // to the controller row (offering its own controller again).
    if (left || right) PlayersAct(ctx, base, right ? kPlyActStyle : -kPlyActStyle);
    if (g.ply_choosing < 2 ? up : down) {
      on_style = false;
      const auto controllers = players_setup::Controllers();
      for (int i = 0; i < int(controllers.size()); ++i) {
        if (controllers[size_t(i)].player == g.ply_choosing) g.ply_choice = i;
      }
    }
  } else {
    if (up || down) sel ^= 2;  // the other row
    if (left || right) sel ^= 1;
  }
  // The mouse: the card under the pointer once it moves; a click on a card
  // selects it (on the selected one = A; on another while choosing = cancel
  // and select it), on the chooser's arrows = step, on a prompt = do it.
  const uint32_t layer = MainMenuLayer(base);
  const uint32_t moves = mouse_menus::PointerMoves();
  // HOVER PICKS THE ROW (2026-10-29): over a card's Buttons line = that card
  // selected on its Buttons row (nothing changes until a click / A / left /
  // right); over the selected card's controller (picture or name) = back to
  // its controller row; over another card while none is selected = that
  // card. Only when the pointer moved.
  if (moves != g.pointer_moves) {
    g.pointer_moves = moves;
    const auto boxes = players_setup::Boxes();
    for (int k = 0; k < 4; ++k) {
      const Screen::PlyCard& c = g.ply_cards[size_t(k)];
      if (boxes[size_t(k)].has_pad && scrooby::Visible(base, c.style) &&
          mouse_menus::PointerOver(base, c.style, g.page, layer)) {
        sel = k;
        on_style = true;
        g.ply_choosing = k;
        break;
      }
      if (g.ply_choosing == k && on_style &&
          (mouse_menus::PointerOver(base, c.pic, g.page, layer) ||
           mouse_menus::PointerOver(base, c.info, g.page, layer))) {
        on_style = false;  // the controller row, offering its own controller again
        const auto controllers = players_setup::Controllers();
        for (int i = 0; i < int(controllers.size()); ++i) {
          if (controllers[size_t(i)].player == k) g.ply_choice = i;
        }
        break;
      }
      if (g.ply_choosing == kPlyNone && mouse_menus::PointerOver(base, c.glass, g.page, layer)) sel = k;
    }
  }
  int clicked = -1;
  if (g.ply_choosing != kPlyNone) {
    const Screen::PlyCard& c = g.ply_cards[size_t(g.ply_choosing)];
    // The arrows: the controller on offer, or (on the Buttons row) the
    // previous / next style (the arrows stood there but only the keys and
    // a click on the line itself worked; 2026-10-29).
    for (const int dir : {-1, +1}) {
      // (A little room round the small arrows: a click just beside one
      // counts; only on the selected card, where nothing else is there.)
      const uint32_t arrow = dir < 0 ? c.left : c.right;
      if (!mouse_menus::TakeClickOver(base, arrow, g.page, layer) &&
          !(layer == 0 && mouse_menus::TakeClickNear(base, arrow, 22.0f, 22.0f, g.page))) {
        continue;
      }
      if (on_style) {
        PlayersAct(ctx, base, dir > 0 ? kPlyActStyle : -kPlyActStyle);
      } else {
        PlayersStep(ctx, base, dir);
      }
    }
  }
  for (int k = 0; k < 4; ++k) {
    const Screen::PlyCard& c = g.ply_cards[size_t(k)];
    // A click on a card's Buttons line: that card selected, its Buttons row,
    // the next style.
    if (scrooby::Visible(base, c.style) && mouse_menus::TakeClickOver(base, c.style, g.page, layer) &&
        players_setup::Boxes()[size_t(k)].has_pad) {
      sel = k;
      on_style = true;
      g.ply_sel = k;
      g.ply_choosing = k;
      g.ply_on_style = true;
      clicked = kPlyActStyle;
      continue;
    }
    if (!mouse_menus::TakeClickOver(base, c.glass, g.page, layer)) continue;
    if (k == g.ply_choosing && on_style) continue;  // its Buttons row: a click beside the arrows does nothing
    if (k == g.ply_sel && !on_style) {
      clicked = kPlyActChoose;
    } else {
      g.ply_choosing = kPlyNone;
      sel = k;
      on_style = false;
    }
  }
  for (int i = 0; i < options_page::kPlyPrompts; ++i) {
    if (g.ply_prompt_action[size_t(i)] >= 0 &&
        mouse_menus::TakeClickOver(base, g.ply_prompt[size_t(i)], g.page, layer)) {
      clicked = g.ply_prompt_action[size_t(i)];
    }
  }
  if (auto* driver = kbm::KeyboardMouseDriver::Get()) driver->TakeListWheel();  // (no use here)
  if (sel != g.ply_sel || on_style != g.ply_on_style) {
    scrooby::PlaySound(ctx, base, scrooby::Sound::kMove);
    g.ply_sel = sel;
    g.ply_on_style = on_style;
  }
  // (Minus a joining controller's press, still hidden: HideHeld.)
  const uint16_t pad = g_pad[size_t(owner)].load(std::memory_order_relaxed) & ~HiddenOn(owner);
  const uint16_t pressed = pad & ~g.seen;
  g.seen = pad;
  if (!(pad & kPadB)) g.ply_hold_b = false;  // B let go: the game may hear B again
  if ((pressed & kPadA) || clicked == kPlyActChoose) {
    PlayersAct(ctx, base, kPlyActChoose);
  } else if ((pressed & (kPadX | kPadY)) || clicked == kPlyActRemove) {
    PlayersAct(ctx, base, kPlyActRemove);
  } else if (clicked == kPlyActStyle) {
    PlayersAct(ctx, base, kPlyActStyle);
  } else if ((pressed & kPadB) || clicked == kPlyActBack) {
    if (g.ply_choosing != kPlyNone) {
      g.ply_choosing = kPlyNone;  // leave the card (a controller on offer not taken); the screen stays
      g.ply_on_style = false;
      g.ply_hold_b = (pad & kPadB) != 0;
      scrooby::PlaySound(ctx, base, scrooby::Sound::kMove);
    } else if (g.main_menu && !CloseMainMenuOptions(ctx, base)) {
      *GuestPtr(base, fe + kResultFlags) |= kFlagCancel;
    } else if (!g.main_menu && clicked == kPlyActBack) {
      if (auto* driver = kbm::KeyboardMouseDriver::Get()) driver->Pulse(kPadB);  // the game's own back
    }
  }
  g_players_take_b = g.ply_choosing != kPlyNone || g.ply_hold_b;
  RefreshPlayers(env);
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

namespace {
// A controller that just joined in the Players screen moves to a socket only
// at the next poll, so for a moment its held buttons aren't on the socket
// yet: "let go" mustn't be decided then (the joining B came through a frame
// later as Back and closed the screen). Not cleared before this time (ms).
std::array<std::atomic<int64_t>, 4> g_hidden_until{};
int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
void HideHeld(int user, uint16_t buttons) {
  if (user < 0 || user >= int(g_hidden.size()) || !buttons) return;
  g_hidden[size_t(user)] |= buttons;
  g_hidden_until[size_t(user)] = NowMs() + 300;
}
// The buttons hidden on socket `user` (the Players screen's own reading
// skips them too: its reading comes before the filter).
uint16_t HiddenOn(int user) {
  return user >= 0 && user < int(g_hidden.size()) ? g_hidden[size_t(user)].load(std::memory_order_relaxed) : 0;
}
}  // namespace

void HideHeldButtons(int user, uint16_t buttons) { HideHeld(user, buttons); }

void FilterPad(uint32_t user, uint8_t* gamepad) {
  if (user >= g_hidden.size() || !gamepad) return;
  if (g_players_take_b.load(std::memory_order_relaxed)) gamepad[0] &= uint8_t(~(kPadB >> 8));  // (Players screen)
  const uint16_t buttons = uint16_t(gamepad[0] << 8 | gamepad[1]);
  if (Capturing()) {
    g_hidden[user] = buttons;
    std::memset(gamepad, 0, 12);
    return;
  }
  uint16_t hidden = g_hidden[user].load(std::memory_order_relaxed);
  if (!hidden) return;
  if (NowMs() >= g_hidden_until[user].load(std::memory_order_relaxed)) {
    hidden &= buttons;  // let go = seen again from the next press on
    g_hidden[user] = hidden;
  }
  const uint16_t out = buttons & ~hidden;
  gamepad[0] = uint8_t(out >> 8);
  gamepad[1] = uint8_t(out);
}

int TurnPage(uint32_t menu, int direction, uint8_t* base) {
  // The Achievements screen: the menu's own up / down neither moves nor
  // sounds (UpdateAchievements moves the grid and plays one sound).
  if (g.action && menu == g.menu && OtherMode()) return -1;
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
      scrooby::CallF(__imp__sub_82371198, ctx, base, e, DrawScale());
    }
  }
}
}  // namespace

void MoveGlassForCard(PPCContext& ctx, uint8_t* base, uint32_t action) {
  g_screen_action = action;
  // The Achievements list keeps the panel centred (its Enter may run before
  // or after the menu's: the request is still pending, or already taken).
  if (ListMode()) {
    ShrinkInGame(ctx, base, action);
    return;
  }
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
  // THE ACHIEVEMENTS SCREEN (achievements_screen.h): this screen in its
  // second mode when the main / pause menu asked for it.
  g.achievements = achievements_screen::TakeOpenNext();
  // THE PLAYERS SCREEN (players/players_setup.h): the third mode, when the
  // main / pause menu's "Players" asked (and the page has its cards).
  g.players = players_setup::TakeOpenNext() && !g.achievements;
  g.ply_drawn.clear();
  g.selection = -1;
  g.focused = -1;
  g.top = 0;
  g.pending_select = -1;
  g.x_showing = true;  // the first Refresh writes the corners either way
  g.y_showing = true;
  const int owner = Owner(base);
  g.seen = g_pad[size_t(owner)].load(std::memory_order_relaxed);
  FindElements(ctx, base);
  if (g.players && g.ply_cards.size() < 4) {  // the page has no cards: Options instead
    g.players = false;
    REXLOG_WARN("Players: the page has no player cards: Options opens instead");
  }
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
    // (The Achievements list keeps the panel centred: no shift.)
    const float shift = OtherMode() ? 0.0f : float(options_page::kGlassShift);
    const float cx = ReadF(base, e + kElementX) / unit + shift + w / 2;
    const float cy = ReadF(base, e + kElementY) / unit + h / 2;
    WriteF(base, e + kElementX, (320.0f + DrawScale() * (cx - 320.0f) - w / 2) * unit);
    WriteF(base, e + kElementY, (240.0f + DrawScale() * (cy - 240.0f) - h / 2) * unit);
    scrooby::SetScale(ctx, base, e, DrawScale());
  }
  SetUpFixedTexts(ctx, base);
  // The elements the Achievements list hides come back for Options; the
  // main menu page's title says which screen this is.
  for (const uint32_t e : {g.tab_prev, g.tab_next, g.lb, g.rb}) scrooby::SetVisible(base, e, !OtherMode());
  scrooby::SetVisible(base, g.tab_text, true);
  if (!g.achievements) ShowAchievementElements(base, false);  // (shown by RefreshAchievements)
  if (!g.players) ShowPlayersElements(base, false);            // (shown by RefreshPlayers)
  g.title = main_menu ? scrooby::FindText(ctx, base, g.page, "OptTitle") : 0;
  g.paused_title = main_menu ? 0 : scrooby::FindText(ctx, base, g.page, "Paused");
  if (g.title && g.achievements) {
    scrooby::SetText(ctx, base, g.title, u"Achievements");
  } else if (g.title) {  // the game's own translated "Options" (options_page.cpp: the PS2 item's string)
    scrooby::Call(__imp__sub_82371CE0, ctx, base, g.title, scrooby::GuestAscii("GameStart_MainMenu_Options"));
  }
  if (OtherMode()) CentreForList(ctx, base);
  // The list hides the Options screen's own furniture: back for Options.
  if (!OtherMode()) ShowOptionsFurniture(ctx, base, true);
  if (g.players) {
    players_setup::Opened();
    g.ply_choosing = -1;
    g.ply_flash = {-1, -1, -1, -1};
    g.ply_on_style = false;
    g.ply_aura_time = -1;
    g.ply_hold_b = false;
    HideGamePrompts(base, true);
    // The chooser's arrows: the map's arrow (it points down as drawn: the
    // Options list turns it 180 for "up") turned to point left / right.
    for (const Screen::PlyCard& c : g.ply_cards) {
      scrooby::SetScale(ctx, base, c.left, 1.0f);
      scrooby::Rotate(ctx, base, c.left, 90.0f);
      scrooby::SetScale(ctx, base, c.right, 1.0f);
      scrooby::Rotate(ctx, base, c.right, -90.0f);
    }
  }
  if (OtherMode()) {  // A does nothing here: no "Select A"
    scrooby::SetText(ctx, base, g.select_button, u"");
    scrooby::SetText(ctx, base, g.select_text, u"");
    if (const uint32_t fe = FrontEnd(base)) {
      // (as from the main menu: a pick left over must not close it)
      *GuestPtr(base, fe + kResultFlags) &= uint8_t(~(kFlagCancel | kFlagAccept));
    }
  }
  scrooby::Select(ctx, base, g.menu, FirstSelectableOnScreen());
  Env env{ctx, base, owner};
  Refresh(env);
  g_open = true;
  REXLOG_INFO("{}: screen opened ({}) by player {} (tab {})",
              g.players ? "Players" : g.achievements ? "Achievements" : "Options",
              main_menu ? "main menu" : "in game", owner + 1, g.tab);
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
  if (g.achievements) {
    UpdateAchievements(ctx, base, fe, owner);
    ctx = saved;
    return;
  }
  if (g.players) {
    UpdatePlayers(ctx, base, fe, owner);
    ctx = saved;
    return;
  }
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
    if (g.achievements) {
      ShowAchievementElements(base, false);
      ShowOptionsFurniture(ctx, base, true);
      PutBackAfterList(base);
      g.achievements = false;
    }
    if (g.players) {
      ShowPlayersElements(base, false);
      HideGamePrompts(base, false);
      g.ply_choosing = -1;
      g.ply_hold_b = false;
      g_players_take_b = false;
      ShowOptionsFurniture(ctx, base, true);
      PutBackAfterList(base);
      g.players = false;
      players_setup::Closed();
    }
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
  // The glass layer (panel, cogs, hinge) is the pause menu's too: whatever
  // the Achievements list did with it, it's shown again.
  if (g_screen_action) {
    if (const uint32_t layer = Read32(base, g_screen_action + 32)) scrooby::SetVisible(base, layer, true);
  }
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
// Options, Players, Credits, Quit Game (Calibration gave its place; Players = the Options screen's
// third mode, players/players_setup.h; Quit = ui/quit_game.h); 2 and 3 = exit 77, the PS2 menu's ExitOptions -> the
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
  // "Quit Game"'s question up (or just answered No): stay, no pick counts
  // (ui/quit_game.h).
  if (quit_game::HoldDecision(ctx, base)) {
    ctx.r3.s64 = -1;
    return;
  }
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
  // "Players" (players/players_setup.h) = the Options screen in its third
  // mode: Options' exit, plus the request.
  constexpr int32_t kExits[] = {kExitNewGame, kExitLoadGame, kExitOptions, kExitOptions, kExitCredits};
  static_assert(kExits[options_page::kMainMenuOptionsItem] == kExitOptions);
  static_assert(kExits[options_page::kMainMenuPlayersItem] == kExitOptions);
  // The last item, "Quit Game", has no exit: its pick never closes the
  // screen (ui/quit_game.cpp takes it back and asks); should one get here,
  // stay.
  if (index == options_page::kMainMenuQuitItem) {
    ctx.r3.s64 = -1;
    return;
  }
  if (index < 0 || index >= int(std::size(kExits))) {
    __imp__sub_8211B6C8(ctx, base);  // as the game: the flags' other answers
    return;
  }
  ctx.r3.s64 = kExits[index];
  if (index == options_page::kMainMenuPlayersItem) {
    if (options_page::PlayersCardsAvailable()) {
      players_setup::OpenNext();
      REXLOG_INFO("Options: main menu -> Players");
    } else {
      ctx.r3.s64 = -1;  // no cards on the page: stay
    }
  } else if (index == options_page::kMainMenuOptionsItem) {
    REXLOG_INFO("Options: main menu -> Options");
  }
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
// The page's smaller size, after a slide step (or once at Enter). The
// Players screen in the main menu: 0.88 x, so its prompt row (under the
// glass panel) clears the main menu's purple frame, which is drawn over the
// screen's bottom edge (at full size the row was hidden under it).
constexpr float kPlayersMainMenuScale = 0.88f;
void ShrinkMainMenuPage(PPCContext& ctx, uint8_t* base, uint32_t action) {
  if (const uint32_t layer = Read32(base, action + kOptLayer)) {
    const bool players = g.players || players_setup::OpenNextPending();
    scrooby::CallF(__imp__sub_82371198, ctx, base, layer, players ? kPlayersMainMenuScale : DrawScale());
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
