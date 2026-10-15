// =============================================================================
// input/mouse_menus.cpp -- see mouse_menus.h
// =============================================================================
#include "mouse_menus.h"

#include <cstring>
#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <string>

#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "../guest_memory.h"
#include "../ui/game_font.h"
#include "../ui/scrooby.h"
#include "prompt_icons.h"
#include "keyboard_mouse.h"

namespace mouse_menus {
namespace {

using namespace rex::input;  // X_INPUT_GAMEPAD_*

constexpr uint32_t kActionMenu = 20;           // CMenuAction +20: its Scrooby menu
constexpr uint32_t kActionPage = 16;           // CMenuAction +16: the page it's on
constexpr uint32_t kItemLook = 140;            // menu item +140: the text drawn for it
constexpr uint32_t kItemFlags = 137;           // bit 0x80 = selectable
constexpr uint32_t kElementSize = 88;          // u16 width, u16 height (canvas units)
constexpr uint32_t kElementX = 92, kElementY = 96;  // floats, x 1/640-width units
constexpr uint32_t kUnitToPosition = 0x8204701C;    // canvas units -> element position
constexpr uint32_t kTextString = 156;               // a text element's string (UTF-16 BE)
constexpr uint32_t kTextFont = 144;                 // a text element's font (pure3d::TextureFont)
constexpr uint32_t kTextJustify = 108;              // byte: 0 left, 1 right, else centred
constexpr uint32_t kElementMatrix = 16;             // 4 x 4 floats; [0] = its x scale
constexpr uint32_t kItemValue = 144;                // menu item +144: its value text (Options rows), or 0
// Drawn width of a text, in canvas units, per font pixel of the font's own
// advances: measured on the main menu (Titans_Large: "Load Game" 346 font px
// -> 247 picture px) and the pause menu (Titans_Small: "Options" 170 -> 121),
// both 0.475 (a selected item is drawn 1.2x: its element scale).
constexpr float kUnitsPerFontPixel = 0.475f;
// A little room around a text's ink (canvas units) so the edges of letters
// count.
constexpr float kTextMargin = 4.0f;
// AFTER A CLICK (two playtests found the problem: a click on Options, then
// the pointer moving on over Load Game, opened Load Game). The game doesn't
// act on the A press at once: the screen slides out first, and only at the
// end does its decision ask which item is selected (GetMenuIndex,
// sub_821239C8). Two things together:
//   1. While the front end isn't taking menu input (+8592 bit 0x40 clear: a
//      screen sliding in or out), the mouse selects nothing and clicks
//      nothing, as a controller's buttons do nothing then, and the clicked
//      item stays highlighted.
//   2. The click is REMEMBERED (menu + item): while it's pending, the
//      game's question "which item is selected in that menu?" gets the
//      clicked item, wherever the pointer went.
// The memory ends once the press has reached the game (an ~80 ms press) and
// the front end still takes input kClickSettle later (the screen stayed: a
// value changed on the Options screen, so the next click works at once), or
// once the next screen takes input after a slide; at most kClickMemory.
// (Earlier tries: a fixed 1 s hold fixed the slide but swallowed the next
// click on the Options screen; a ~150 ms hold let the slide's end read the
// moved selection.)
constexpr auto kClickSettle = std::chrono::milliseconds(60);
constexpr auto kClickMemory = std::chrono::milliseconds(5000);
// The game's question box ("Are you sure you want to quit?", the totem's
// "Save?"): the front end's object at +172 (players/lost_controller.cpp):
// +4 flags (0x40 showing), +16 its Yes / No menu. While it shows, the menu
// of the screen under it (the pause menu) still updates every frame: it
// must not take the mouse (a click on "Yes" there pressed A on the pause
// menu's item under the pointer: found in the first test).
constexpr uint32_t kGameGlobal = 0x8259B190, kFrontEnd = 52;
constexpr uint32_t kBox = 172, kBoxFlags = 4, kBoxMenu = 16;
constexpr uint8_t kBoxShowing = 0x40;
// Front end +8592 bit 0x40 = "menus take input": cleared by the game while a
// screen slides in or out, set again once the next one is up (the Options
// screen's own slide uses it too: options_menu.cpp kMenuFlags).
constexpr uint32_t kMenuFlags = 8592;
constexpr uint8_t kMenusTakeInput = 0x40;

uint32_t Read32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = GuestPtr(base, a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
float ReadF(const uint8_t* base, uint32_t a) {
  const uint32_t v = Read32(base, a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
uint16_t Read16(const uint8_t* base, uint32_t a) {
  const uint8_t* p = GuestPtr(base, a);
  return uint16_t(p[0] << 8 | p[1]);
}

// The pointer's last move seen by the hover code (hover only follows a
// pointer that moved, so a resting one never fights the arrow keys).
uint32_t g_seen_moves = 0;

// The pointer in canvas units (640 x 480, y up; the canvas is drawn 4:3 in
// the middle of the 16:9 picture: picture x = (160 + 1.5 x) / 1280,
// y = (720 - 1.5 y) / 720). False without a pointer.
bool PointerCanvas(float* cx, float* cy, uint32_t* moves) {
  kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  float px = 0, py = 0;
  if (!driver || !driver->Pointer(&px, &py, moves)) return false;
  *cx = (px * 1280.0f - 160.0f) / 1.5f;
  *cy = (1.0f - py) * 480.0f;
  return true;
}

struct Rect {
  float x0 = 0, y0 = 0, x1 = -1, y1 = -1;  // canvas units, y up; empty if x1 < x0
  bool empty() const { return x1 < x0; }
  bool contains(float x, float y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
  Rect united(const Rect& o) const {
    if (empty()) return o;
    if (o.empty()) return *this;
    return {std::min(x0, o.x0), std::min(y0, o.y0), std::max(x1, o.x1), std::max(y1, o.y1)};
  }
};

// The game's fonts, read from the disc once each (ui/game_font.h).
const game_font::Font* FontNamed(const std::string& name) {
  static std::map<std::string, std::unique_ptr<game_font::Font>> fonts;
  auto& font = fonts[name];
  if (!font) {
    font = std::make_unique<game_font::Font>();
    font->Load(name);
  }
  return font->loaded() ? font.get() : nullptr;
}

// An element's box (shown elements only; empty otherwise).
Rect Box(const uint8_t* base, uint32_t element) {
  if (!element || !scrooby::Visible(const_cast<uint8_t*>(base), element)) return {};
  const float unit = ReadF(base, kUnitToPosition);
  if (!(unit > 0)) return {};
  const float x = ReadF(base, element + kElementX) / unit, y = ReadF(base, element + kElementY) / unit;
  const float w = Read16(base, element + kElementSize), h = Read16(base, element + kElementSize + 2);
  return {x, y, x + w, y + h};
}

// What a TEXT element covers across: its string measured with its font,
// placed by its justification inside its box (menu boxes are often far wider
// than their words: the main menu's are 400 units). Falls back to the box
// for a font we don't know or a picture.
Rect TextArea(const uint8_t* base, uint32_t element) {
  Rect box = Box(base, element);
  if (box.empty() || Read32(base, element) != 0x8200BE2C) return box;  // not a Text
  const uint32_t string = Read32(base, element + kTextString);
  const game_font::Font* font = FontNamed(prompt_icons::FontNameAt(Read32(base, element + kTextFont)));
  if (!string || !font) return box;
  std::u16string text;
  for (uint32_t i = 0; i < 128; ++i) {
    const uint8_t* c = GuestPtr(base, string + 2 * i);
    const char16_t ch = char16_t(c[0] << 8 | c[1]);
    if (!ch) break;
    text.push_back(ch);
  }
  if (text.empty()) return {};
  const float scale = ReadF(base, element + kElementMatrix);  // the selected item's 1.2x
  const float width =
      std::min(box.x1 - box.x0, font->Measure(text) * kUnitsPerFontPixel * (scale > 0 ? scale : 1.0f));
  const uint8_t justify = *GuestPtr(base, element + kTextJustify);
  Rect area = box;
  if (justify == 0) {
    area.x1 = box.x0 + width;
  } else if (justify == 1) {
    area.x0 = box.x1 - width;
  } else {
    const float mid = (box.x0 + box.x1) / 2;
    area.x0 = mid - width / 2;
    area.x1 = mid + width / 2;
  }
  area.x0 -= kTextMargin;
  area.x1 += kTextMargin;
  return area;
}

// Where a page draws an area: a Scrooby page (and its layer) can be moved
// and scaled as a whole (the Options screen draws its page at 0.85x around
// the screen's centre, and slides it in): its 4 x 4 transform at +16 (row
// vectors: x scale [0], y scale [5], translation [12] / [13] in element
// position units, i.e. canvas units x the constant at 0x8204701C). Read
// every frame (it moves during the slide). Measured on the in-game Options
// page: scale 0.85, translation y 0.05625 = 36 units = 240 x (1 - 0.85).
Rect OnPage(const uint8_t* base, Rect r, uint32_t page) {
  if (!page || r.empty()) return r;
  const float unit = ReadF(base, kUnitToPosition);
  const float sx = ReadF(base, page + kElementMatrix), sy = ReadF(base, page + kElementMatrix + 20);
  const float tx = ReadF(base, page + kElementMatrix + 48), ty = ReadF(base, page + kElementMatrix + 52);
  if (!(unit > 0) || !(sx > 0) || !(sy > 0)) return r;
  return {r.x0 * sx + tx / unit, r.y0 * sy + ty / unit, r.x1 * sx + tx / unit, r.y1 * sy + ty / unit};
}

bool Inside(const uint8_t* base, uint32_t element, float cx, float cy, uint32_t page = 0,
            uint32_t layer = 0) {
  return OnPage(base, OnPage(base, TextArea(base, element), layer), page).contains(cx, cy);
}

// Does the front end take menu input right now (no screen sliding)?
bool MenusTakeInput(const uint8_t* base) {
  const uint32_t game = Read32(base, kGameGlobal);
  const uint32_t fe = game ? Read32(base, game + kFrontEnd) : 0;
  return !fe || (*GuestPtr(base, fe + kMenuFlags) & kMenusTakeInput) != 0;
}

// The remembered click (see "AFTER A CLICK").
struct Click {
  bool active = false;
  uint32_t menu = 0;
  int item = -1;
  std::chrono::steady_clock::time_point made{}, press_out{};
  bool slid = false;  // the front end stopped taking input since (a slide began)
} g_click;

uint32_t QuestionMenu(const uint8_t* base);  // below

// Ages the remembered click (every front-end update and menu update).
void AgeClick(const uint8_t* base) {
  if (!g_click.active) return;
  const auto now = std::chrono::steady_clock::now();
  kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  if (now - g_click.made > kClickMemory) {
    g_click.active = false;
    return;
  }
  if (driver && driver->PulseBusy()) return;  // the press hasn't gone out yet
  if (g_click.press_out == std::chrono::steady_clock::time_point{}) g_click.press_out = now;
  const bool input = MenusTakeInput(base) || QuestionMenu(base) != 0;  // a box takes its own
  if (!input) {
    g_click.slid = true;  // a slide: remembered until the next screen is up
  } else if (g_click.slid || now - g_click.press_out > kClickSettle) {
    g_click.active = false;  // the next screen is up / the screen stayed
  }
}
bool Locked() { return g_click.active; }

// The 360 button a prompt glyph stands for (the play glyphs and our menu
// glyphs, input/prompt_icons.h), 0 if none.
uint16_t ButtonOfGlyph(char16_t c) {
  switch (c) {
    case u'±': case u'': return X_INPUT_GAMEPAD_A;
    case u'²': case u'': return X_INPUT_GAMEPAD_B;
    case u'³': case u'': return X_INPUT_GAMEPAD_X;
    case u'´': case u'': return X_INPUT_GAMEPAD_Y;
    case u'µ': return X_INPUT_GAMEPAD_BACK;
    case u'¶': return X_INPUT_GAMEPAD_START;
    case u'¸': case u'': return X_INPUT_GAMEPAD_RIGHT_SHOULDER;
    case u'¾': case u'': return X_INPUT_GAMEPAD_LEFT_SHOULDER;
    default: return 0;
  }
}

// The question box's Yes / No menu while the box shows, else 0.
uint32_t QuestionMenu(const uint8_t* base) {
  const uint32_t game = Read32(base, kGameGlobal);
  const uint32_t fe = game ? Read32(base, game + kFrontEnd) : 0;
  if (!fe || !(*GuestPtr(base, fe + kBox + kBoxFlags) & kBoxShowing)) return 0;
  return Read32(base, fe + kBox + kBoxMenu);
}

}  // namespace

uint32_t PointerMoves() {
  float cx, cy;
  uint32_t moves = 0;
  PointerCanvas(&cx, &cy, &moves);
  return moves;
}

bool PointerOver(uint8_t* base, uint32_t element, uint32_t page, uint32_t layer) {
  float cx, cy;
  uint32_t moves;
  return PointerCanvas(&cx, &cy, &moves) && Inside(base, element, cx, cy, page, layer);
}

bool TakeClickOver(uint8_t* base, uint32_t element, uint32_t page, uint32_t layer) {
  kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  float px = 0, py = 0;
  if (!driver || !driver->PeekMenuClick(&px, &py)) return false;
  // Where the click was made (see PeekMenuClick).
  if (!Inside(base, element, (px * 1280.0f - 160.0f) / 1.5f, (1.0f - py) * 480.0f, page, layer)) {
    return false;
  }
  return driver->TakeMenuClick();
}

namespace {
// The shared body: hover, and a click on an item. press_a: the click presses
// A (the game's menus); else it's only reported (the port's own menus).
int HoverAndClickImpl(PPCContext& ctx, uint8_t* base, uint32_t menu, const uint32_t* targets,
                      uint32_t page, uint32_t layer, bool press_a);
}  // namespace

void HoverAndClick(PPCContext& ctx, uint8_t* base, uint32_t menu, const uint32_t* targets,
                   uint32_t page, uint32_t layer) {
  HoverAndClickImpl(ctx, base, menu, targets, page, layer, true);
}

int HoverAndTakeClick(PPCContext& ctx, uint8_t* base, uint32_t menu, uint32_t page, uint32_t layer) {
  return HoverAndClickImpl(ctx, base, menu, nullptr, page, layer, false);
}

namespace {
int HoverAndClickImpl(PPCContext& ctx, uint8_t* base, uint32_t menu, const uint32_t* targets,
                      uint32_t page, uint32_t layer, bool press_a) {
  kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  float cx, cy;
  uint32_t moves;
  if (!driver || !menu || !PointerCanvas(&cx, &cy, &moves)) return -1;
  // A question box on top: only its own menu answers.
  const uint32_t question = QuestionMenu(base);
  if (question && menu != question) return -1;
  // The front end's "menus take input" (a screen sliding: no); a question box
  // answers on its own while it shows (the flag stays clear under it).
  const bool takes_input = MenusTakeInput(base) || menu == question;
  // The item at a canvas point (-1: none).
  const int count = scrooby::ItemCount(base, menu);
  auto item_at = [&](float x, float y) {
    for (int i = 0; i < count; ++i) {
      const uint32_t item = scrooby::Item(base, menu, i);
      if (!item || !(*GuestPtr(base, item + kItemFlags) & 0x80)) continue;  // not selectable
      Rect area;
      if (targets && targets[i]) {
        area = Box(base, targets[i]);  // a picture standing for the item (a save panel's glass)
      } else {
        // The item's text, and its value text when it has one (the Options
        // rows: name on the left, value on the right): the whole row between.
        area = TextArea(base, Read32(base, item + kItemLook));
        if (const uint32_t value = Read32(base, item + kItemValue);
            value && Read32(base, value) == 0x8200BE2C) {
          area = area.united(TextArea(base, value));
        }
      }
      if (OnPage(base, OnPage(base, area, layer), page).contains(x, y)) return i;
    }
    return -1;
  };
  const int hovered = item_at(cx, cy);
  AgeClick(base);
  float kx = 0, ky = 0;
  const bool click_waiting = driver->PeekMenuClick(&kx, &ky);
  const int clicked = click_waiting ? item_at((kx * 1280.0f - 160.0f) / 1.5f, (1.0f - ky) * 480.0f) : -1;
  // A slide (no menu input) or a click still pending: the clicked item stays
  // selected, nothing else follows the pointer. A new click on the SAME item
  // while the screen still takes input (it didn't slide: a value changed on
  // the Options screen) counts at once; other clicks are dropped.
  if (press_a && (Locked() || !takes_input)) {
    if (g_click.active && menu == g_click.menu && g_click.item >= 0 &&
        scrooby::Selection(base, menu) != g_click.item) {
      scrooby::Select(ctx, base, menu, g_click.item);
    }
    g_seen_moves = moves;
    if (click_waiting && driver->TakeMenuClick() && g_click.active && !g_click.slid &&
        takes_input && menu == g_click.menu && clicked == g_click.item) {
      driver->Pulse(X_INPUT_GAMEPAD_A);
      g_click.made = std::chrono::steady_clock::now();
      g_click.press_out = {};
    }
    return -1;
  }
  // The port's own menu (no A press, it changes the value itself): a click
  // is simply reported, at once, each one.
  if (!press_a) {
    const bool moved_here = moves != g_seen_moves;
    g_seen_moves = moves;
    if (moved_here && hovered >= 0 && hovered != scrooby::Selection(base, menu)) {
      scrooby::Select(ctx, base, menu, hovered);
      scrooby::PlaySound(ctx, base, scrooby::Sound::kMove);
    }
    if (clicked >= 0 && driver->TakeMenuClick()) {
      if (clicked != scrooby::Selection(base, menu)) scrooby::Select(ctx, base, menu, clicked);
      return clicked;
    }
    return -1;
  }
  const bool moved = moves != g_seen_moves;
  g_seen_moves = moves;
  if (moved && hovered >= 0 && hovered != scrooby::Selection(base, menu)) {
    scrooby::Select(ctx, base, menu, hovered);
    scrooby::PlaySound(ctx, base, scrooby::Sound::kMove);
  }
  // A left click chooses the item it was MADE on (selected first, then A).
  if (clicked >= 0 && driver->TakeMenuClick()) {
    if (clicked != scrooby::Selection(base, menu)) scrooby::Select(ctx, base, menu, clicked);
    driver->Pulse(X_INPUT_GAMEPAD_A);
    g_click = Click{};
    g_click.active = true;
    g_click.menu = menu;
    g_click.item = clicked;
    g_click.made = std::chrono::steady_clock::now();
  }
  return -1;
}
}  // namespace


void OnMenuUpdate(PPCContext& ctx, uint8_t* base, uint32_t action) {
  if (!action || QuestionMenu(base)) return;  // a question box is on top
  HoverAndClick(ctx, base, Read32(base, action + kActionMenu), nullptr,
                Read32(base, action + kActionPage));
}

void FrontEndFrame(PPCContext& ctx, uint8_t* base) {
  AgeClick(base);
  // A question box: its Yes / No answer to the pointer (and nothing under it).
  if (const uint32_t menu = QuestionMenu(base)) {
    HoverAndClick(ctx, base, menu, nullptr);
    return;
  }
  kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  if (!driver || !driver->PeekMenuClick()) return;
  const uint32_t page = scrooby::FindPage(ctx, base, "FE_Buttons");
  if (!page) return;
  static constexpr const char* kCorners[4] = {"LowerLeft", "LowerRight", "UpperLeft", "UpperRight"};
  for (const char* corner : kCorners) {
    const uint32_t button = scrooby::FindText(ctx, base, page, std::string(corner) + "Button");
    const uint32_t label = scrooby::FindText(ctx, base, page, std::string(corner) + "Text");
    if (!button || !scrooby::Visible(base, button)) continue;
    const uint32_t string = Read32(base, button + kTextString);
    if (!string) continue;
    const uint8_t* c = GuestPtr(base, string);
    const uint16_t pad = ButtonOfGlyph(char16_t(c[0] << 8 | c[1]));
    if (!pad) continue;
    // The prompt = its button picture and its label ("Back", "Select").
    if (TakeClickOver(base, button) || TakeClickOver(base, label)) {
      driver->Pulse(pad);
      return;
    }
  }
}

}  // namespace mouse_menus

// GetMenuIndex (r4 = page name, r5 = menu name -> r3 = the selected item):
// the question a front-end decision asks when a screen's slide ends. For the
// menu of a remembered click, the clicked item (see "AFTER A CLICK").
extern "C" REX_FUNC(__imp__sub_821239C8);
extern "C" REX_FUNC(sub_821239C8) {
  const uint32_t page_name = ctx.r4.u32, menu_name = ctx.r5.u32;
  __imp__sub_821239C8(ctx, base);
  using namespace mouse_menus;
  if (!g_click.active || !page_name || !menu_name) return;
  const auto text = [&](uint32_t a) {
    std::string out;
    for (uint32_t i = 0; i < 64; ++i) {
      const char c = char(*GuestPtr(base, a + i));
      if (!c) break;
      out.push_back(c);
    }
    return out;
  };
  const PPCContext saved = ctx;
  const uint32_t page = scrooby::FindPage(ctx, base, text(page_name));
  const uint32_t menu = page ? scrooby::FindMenu(ctx, base, page, text(menu_name)) : 0;
  ctx = saved;
  if (menu && menu == g_click.menu && int32_t(ctx.r3.u32) != g_click.item) {
    REXLOG_INFO("Mouse: {} asked for its selection: the clicked item {} (not {})", text(menu_name),
                g_click.item, int32_t(ctx.r3.u32));
    ctx.r3.u64 = uint64_t(int64_t(g_click.item));
  }
}
