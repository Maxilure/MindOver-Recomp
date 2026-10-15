// =============================================================================
// input/name_typing.cpp -- see name_typing.h
// =============================================================================
#include "name_typing.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <string>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../data/data_patcher.h"
#include "../data/pure3d.h"
#include "../guest_memory.h"
#include "../ui/game_font.h"
#include "../ui/scrooby.h"
#include "keyboard_mouse.h"
#include "mouse_menus.h"
#include "prompt_icons.h"

extern "C" REX_FUNC(__imp__sub_820D6618);  // CNameEntryScreenAction: type the selected key
extern "C" REX_FUNC(__imp__sub_820D6450);  // CNameEntryScreenAction: the character under the cursor
extern "C" REX_FUNC(__imp__sub_820D66E0);  // CNameEntryScreenAction: erase the last letter
extern "C" REX_FUNC(__imp__sub_82261800);  // front end: play a menu sound (bank r4, sound r5, f1-f3)

namespace name_typing {
namespace {

// CNameEntryScreenAction (findings/24 s.5): the cursor's row (0-3) and
// column (0-10).
constexpr uint32_t kCursorRow = 308, kCursorColumn = 312;
constexpr int kRows = 4, kColumns = 11;
// The character table the cursor reads (sub_820D6450): pages of 4 x 11
// UTF-16 cells, page = character set (+316) + caps; 0 = no key there.
// Column 0 rows 1-2 and column 10 rows 1-2 are the special keys (character
// set, caps; erase, done), handled by POSITION in the typing update.
constexpr uint32_t kGameGlobal = 0x8259B190, kFrontEnd = 52;

// The game's sounds for this screen (the typing update and sub_820D6618):
// bank "FE_SFX_NameScreen", "letter_scroll" for a cursor move.
constexpr uint32_t kSoundBank = 0x82024224, kSoundScroll = 0x82024214;
// The error sound (the bank at 0x820222E4, "backpackNONONO"): a character
// the font can't draw.
constexpr uint32_t kErrorBank = 0x820222E4, kErrorSound = 0x820240B8;

// Backspace held: the first erase at once, then again after kRepeatDelay,
// every kRepeatEvery (like a text box).
constexpr auto kRepeatDelay = std::chrono::milliseconds(450);
constexpr auto kRepeatEvery = std::chrono::milliseconds(60);
bool g_backspace_down = false;
std::chrono::steady_clock::time_point g_next_erase{};

// While set, sub_820D6450 (the character under the cursor) answers this:
// the typed character goes in through the screen's own typing.
char16_t g_forced = 0;

uint32_t Read32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = GuestPtr(base, a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Write32(uint8_t* base, uint32_t a, uint32_t v) {
  uint8_t* p = GuestPtr(base, a);
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

void Sound(PPCContext& ctx, uint8_t* base, uint32_t bank, uint32_t sound) {
  const uint32_t game = Read32(base, kGameGlobal);
  const uint32_t fe = game ? Read32(base, game + kFrontEnd) : 0;
  if (!fe) return;
  const PPCContext saved = ctx;
  ctx.r3.u64 = fe;
  ctx.r4.u64 = bank;
  ctx.r5.u64 = sound;
  ctx.f1.f64 = 1.0;
  ctx.f2.f64 = 0.0;
  ctx.f3.f64 = 1.0;
  __imp__sub_82261800(ctx, base);
  ctx = saved;
}

// Types `c` through the screen's own "type the selected key".
void Type(PPCContext& ctx, uint8_t* base, uint32_t self, char16_t c) {
  g_forced = c;
  scrooby::Call(__imp__sub_820D6618, ctx, base, self);
  g_forced = 0;
}

// The name field's font (Titans_Large), read from the disc once: which
// characters it can draw.
const game_font::Font* NameFont() {
  static game_font::Font font;
  static bool tried = false;
  if (!tried) {
    tried = true;
    font.Load("Titans_Large");
  }
  return font.loaded() ? &font : nullptr;
}
bool Drawable(char32_t c) {
  if (c == U' ') return true;
  if (c > 0xFFFF || (c >= 0xE000 && c < 0xF900)) return false;  // (our picture glyphs too)
  const game_font::Font* font = NameFont();
  return font && font->Find(char16_t(c)) != nullptr;
}

// --- The mouse on the on-screen keyboard ----------------------------------------
// The keys' elements on page GameStart_NameEntry (findings/24 s.5): a text
// Key_<row>_<column> per cell, placed by the screen's Enter; the four special
// keys are drawn as pictures on small buttons: Key_CharSet_BG (row 1, column
// 0), Key_Caps_BG (2, 0), Key_Backspace_BG (1, 10: our space key) and
// Key_Enter_BG (2, 10).
// A cell: the letters sit ~43 x 46 picture pixels apart = 29 x 31 canvas
// units (photo of the screen, 2026-10-15): half of that either side.
constexpr float kCellHalfW = 14.0f, kCellHalfH = 15.0f;
constexpr float kButtonHalf = 17.0f;  // the special keys' 64-px buttons, drawn ~34 units

struct Keys {
  uint32_t page = 0;
  std::array<std::array<uint32_t, kColumns>, kRows> text{};  // Key_<row>_<column>
  std::array<std::array<uint32_t, kColumns>, kRows> cell{};  // the element a cell is hit by
  std::array<std::array<float, kColumns>, kRows> half{};     // its cell's half size
};
Keys g_keys;
uint32_t g_keys_self = 0;  // the screen they were found for
uint32_t g_seen_moves = 0;

void FindKeys(PPCContext& ctx, uint8_t* base, uint32_t self) {
  g_keys = Keys{};
  g_keys_self = self;
  g_keys.page = scrooby::FindPage(ctx, base, "GameStart_NameEntry");
  if (!g_keys.page) return;
  for (int r = 0; r < kRows; ++r) {
    for (int c = 0; c < kColumns; ++c) {
      g_keys.text[r][c] = scrooby::FindText(ctx, base, g_keys.page, fmt::format("Key_{}_{}", r, c));
      g_keys.cell[r][c] = g_keys.text[r][c];
      g_keys.half[r][c] = kCellHalfW;
    }
  }
  const struct {
    int row, column;
    const char* name;
  } kButtons[] = {{1, 0, "Key_CharSet_BG"}, {2, 0, "Key_Caps_BG"}, {1, 10, "Key_Backspace_BG"},
                  {2, 10, "Key_Enter_BG"}};
  for (const auto& b : kButtons) {
    if (const uint32_t e = scrooby::FindPicture(ctx, base, g_keys.page, b.name)) {
      g_keys.cell[b.row][b.column] = e;
      g_keys.half[b.row][b.column] = kButtonHalf;
    }
  }
}

// The character under cell (r, c) of the page in use (0 = no key there).
char16_t CellCharacter(PPCContext& ctx, uint8_t* base, uint32_t self, int r, int c) {
  const uint32_t row = Read32(base, self + kCursorRow), column = Read32(base, self + kCursorColumn);
  Write32(base, self + kCursorRow, uint32_t(r));
  Write32(base, self + kCursorColumn, uint32_t(c));
  const char16_t ch = char16_t(scrooby::Call(__imp__sub_820D6450, ctx, base, self) & 0xFFFF);
  Write32(base, self + kCursorRow, row);
  Write32(base, self + kCursorColumn, column);
  return ch;
}

// MOVING THE CURSOR, as the typing update does around its own moves
// (0x820D5D00-0x820D5DF4 the old key, 0x820D5E74-0x820D5F54 the new one;
// read 2026-10-15): the old key back to its size (1.0; the caps label 0.5)
// and green (the word at 0x82506BB4), the new one enlarged (1.4; caps 0.65)
// and yellow (0x82506BBC); the two right-hand buttons (erase / space and
// done: pictures at the screen's +48 / +52) show frame 1 while selected.
// Without this the old key stayed highlighted (first test).
constexpr uint32_t kRightButton1 = 48, kRightButton2 = 52;
constexpr uint32_t kGreen = 0x82506BB4, kYellow = 0x82506BBC;
constexpr float kNormal = 1.0f, kCapsNormal = 0.5f, kSelected = 1.4f, kCapsSelected = 0.65f;

void Highlight(PPCContext& ctx, uint8_t* base, uint32_t self, int r, int c, bool on) {
  if (c == 10 && (r == 1 || r == 2)) {
    const uint32_t picture = Read32(base, self + (r == 1 ? kRightButton1 : kRightButton2));
    if (picture) {
      scrooby::SetScale(ctx, base, picture, on ? kSelected : kNormal);
      scrooby::SetFrame(ctx, base, picture, on ? 1 : 0);
    }
    if (!on) return;  // (selecting also styles the cell's own text, as the game does)
  }
  const uint32_t text = g_keys.text[r][c];
  if (!text) return;
  const bool caps = r == 2 && c == 0;
  scrooby::SetScale(ctx, base, text, on ? (caps ? kCapsSelected : kSelected) : (caps ? kCapsNormal : kNormal));
  scrooby::SetColour(ctx, base, text, Read32(base, on ? kYellow : kGreen));
}

void MoveCursor(PPCContext& ctx, uint8_t* base, uint32_t self, int r, int c) {
  const int old_r = int(Read32(base, self + kCursorRow)), old_c = int(Read32(base, self + kCursorColumn));
  if (old_r >= 0 && old_r < kRows && old_c >= 0 && old_c < kColumns) Highlight(ctx, base, self, old_r, old_c, false);
  Write32(base, self + kCursorRow, uint32_t(r));
  Write32(base, self + kCursorColumn, uint32_t(c));
  Highlight(ctx, base, self, r, c, true);
}

// Pointer moves over a key: the cursor goes there (with the scroll sound);
// a click on a key: the cursor there and A (the screen types it, or does its
// special key's job, as for a controller's A).
void Mouse(PPCContext& ctx, uint8_t* base, uint32_t self) {
  kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  if (!driver) return;
  if (g_keys_self != self) FindKeys(ctx, base, self);
  if (!g_keys.page) return;
  const uint32_t moves = mouse_menus::PointerMoves();
  const bool moved = moves != g_seen_moves;
  g_seen_moves = moves;
  for (int r = 0; r < kRows; ++r) {
    for (int c = 0; c < kColumns; ++c) {
      const uint32_t e = g_keys.cell[r][c];
      if (!e) continue;
      const float half = g_keys.half[r][c];
      const bool clicked = mouse_menus::TakeClickNear(base, e, half, half * kCellHalfH / kCellHalfW, g_keys.page);
      if (!clicked && !(moved && mouse_menus::PointerNear(base, e, half, half * kCellHalfH / kCellHalfW, g_keys.page))) {
        continue;
      }
      if (!CellCharacter(ctx, base, self, r, c)) continue;  // no key in this cell (on this page)
      const bool there = Read32(base, self + kCursorRow) == uint32_t(r) &&
                         Read32(base, self + kCursorColumn) == uint32_t(c);
      if (!there) {
        MoveCursor(ctx, base, self, r, c);
        if (!clicked) Sound(ctx, base, kSoundBank, kSoundScroll);
      }
      if (clicked) driver->Pulse(rex::input::X_INPUT_GAMEPAD_A);
      return;
    }
  }
}

// A typed character's key, if the on-screen keyboard's page in use has one:
// the cursor goes there, so it's plain that the typing landed (the space
// has no cell of its own: the space key does).
void ShowOnKeys(PPCContext& ctx, uint8_t* base, uint32_t self, char16_t c) {
  if (g_keys_self != self) FindKeys(ctx, base, self);
  if (c == u' ') {
    MoveCursor(ctx, base, self, 1, 10);
    return;
  }
  for (int r = 0; r < kRows; ++r) {
    for (int col = 0; col < kColumns; ++col) {
      if ((col == 0 || col == 10) && (r == 1 || r == 2)) continue;  // the special keys
      if (CellCharacter(ctx, base, self, r, col) == c) {
        if (Read32(base, self + kCursorRow) != uint32_t(r) || Read32(base, self + kCursorColumn) != uint32_t(col)) {
          MoveCursor(ctx, base, self, r, col);
        }
        return;
      }
    }
  }
}

// --- The prompt bar -----------------------------------------------------------------
// While the keyboard is the device in use, the name screen's prompts show
// the keys that do these jobs here (prompt_icons.h key slots): Backspace,
// Enter (Done) and the left mouse button (Select); on a controller the
// game's X, Start and A. Set when that changes (and once per visit).
int g_prompts_for = -1;  // 1 keys, 0 buttons, -1 not set yet

void SetPrompt(PPCContext& ctx, uint8_t* base, const char* element, std::u16string_view text) {
  static uint32_t buttons_page = 0;
  buttons_page = scrooby::FindPage(ctx, base, "FE_Buttons");
  if (!buttons_page) return;
  scrooby::SetText(ctx, base, scrooby::FindText(ctx, base, buttons_page, element), text);
}

void Prompts(PPCContext& ctx, uint8_t* base) {
  const bool keys = prompt_icons::KeysShown();
  if (int(keys) == g_prompts_for) return;
  g_prompts_for = int(keys);
  auto key = [](kbm::Input input, char16_t otherwise) {
    const char16_t g = prompt_icons::KeyGlyph(input);
    return std::u16string(1, g ? g : otherwise);
  };
  using VK = rex::ui::VirtualKey;
  if (keys) {
    SetPrompt(ctx, base, "UpperLeftButton", key(kbm::Input(VK::kBack), u'³'));
    SetPrompt(ctx, base, "UpperRightButton", key(kbm::Input(VK::kReturn), u'¶'));
    SetPrompt(ctx, base, "LowerRightButton", key(kbm::Input(VK::kLButton), u'±'));
  } else {
    SetPrompt(ctx, base, "UpperLeftButton", u"³");   // X: erase (saves/save_library.cpp)
    SetPrompt(ctx, base, "UpperRightButton", u"¶");  // Start: done
    SetPrompt(ctx, base, "LowerRightButton", u"±");  // A: select (the menus' A: prompt_icons)
  }
}

// --- The space key's picture -------------------------------------------------------
// The on-screen keyboard's erase key (row 1, column 10) types a space now
// (sub_820D66E0 below), so its picture is the game's own left arrow MIRRORED:
// an arrow pointing right, "move on / leave a gap" (chosen in a playtest,
// 2026-10-15; a first version drew a "␣" symbol in code). The pictures are
// sprites FE_NameEntry_Backspace.tga and _Highlight.tga (chunk 0x19005 >
// image 0x19001 > 0x19002: a u32 size, then a TGA: 8-bit palettized, 256
// BGRA colours) in the main menus' package (cdd70a8c) and, copied there by
// the rename route (saves/rename_screen.cpp), the in-game menus' packages.
// Each row of colour indices is reversed: same pixels, same palette.
constexpr uint32_t kChunkSprite = 0x19005, kChunkImage = 0x19001, kChunkImageData = 0x19002;

bool MirrorPicture(pure3d::Bytes& data) {
  // data: u32 size, then the TGA.
  if (data.size() < 4 + 18) return false;
  uint8_t* t = data.data() + 4;
  const size_t tga_size = data.size() - 4;
  const uint32_t id = t[0], cmap_type = t[1], type = t[2];
  const uint32_t cmap_len = t[5] | t[6] << 8, cmap_bits = t[7];
  const uint32_t w = t[12] | t[13] << 8, h = t[14] | t[15] << 8, bpp = t[16];
  if (cmap_type != 1 || type != 1 || cmap_bits != 32 || bpp != 8 || cmap_len == 0 || cmap_len > 256) return false;
  const size_t pixels_at = 18 + id + 4 * size_t(cmap_len);
  if (pixels_at + size_t(w) * h > tga_size || w == 0 || h == 0) return false;
  uint8_t* px = t + pixels_at;
  for (uint32_t y = 0; y < h; ++y) std::reverse(px + size_t(y) * w, px + size_t(y + 1) * w);
  return true;
}

bool PatchSpaceKey(const pure3d::Bytes& original, pure3d::Bytes* patched) {
  pure3d::Chunk root;
  std::vector<pure3d::Chunk> top;
  if (!pure3d::Parse(original, &root, &top)) return false;
  pure3d::Bytes children;
  int drawn = 0;
  for (const pure3d::Chunk& c : top) {
    const std::string_view name = c.id == kChunkSprite ? pure3d::NameOf(original, c) : std::string_view();
    if (name == "FE_NameEntry_Backspace.tga" || name == "FE_NameEntry_Backspace_Highlight.tga") {
      pure3d::Node sprite = pure3d::ToNode(original, c);
      for (pure3d::Node& image : sprite.children) {
        if (image.id != kChunkImage) continue;
        for (pure3d::Node& pixels : image.children) {
          if (pixels.id == kChunkImageData && MirrorPicture(pixels.data)) ++drawn;
        }
      }
      const pure3d::Bytes bytes = pure3d::ToBytes(sprite);
      children.insert(children.end(), bytes.begin(), bytes.end());
    } else {
      const pure3d::Bytes bytes = pure3d::Copy(original, c);
      children.insert(children.end(), bytes.begin(), bytes.end());
    }
  }
  if (drawn == 0) return false;
  *patched = pure3d::WithChildren(original, root, children);
  return true;
}

}  // namespace

void Frame(PPCContext& ctx, uint8_t* base, uint32_t self) {
  kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  if (!driver) return;
  driver->SetTextMode(true);
  for (const char32_t c : driver->TakeTyped()) {
    if (Drawable(c)) {
      Type(ctx, base, self, char16_t(c));
      ShowOnKeys(ctx, base, self, char16_t(c));
    } else {
      REXLOG_INFO("Name typing: U+{:04X} isn't in the game's font: dropped", uint32_t(c));
      Sound(ctx, base, kErrorBank, kErrorSound);
    }
  }
  // Backspace: the screen's own erase (the last letter off, the erase
  // sound; the error sound when the name is empty). The keyboard's state
  // isn't in the SDK's input system (keyboard_mouse.h: the game gets it
  // through its XInputGetState), so it's read here, not as an X press.
  const bool backspace = driver->InputHeld(kbm::Input(rex::ui::VirtualKey::kBack));
  const auto now = std::chrono::steady_clock::now();
  if (backspace && (!g_backspace_down || now >= g_next_erase)) {
    g_next_erase = now + (g_backspace_down ? kRepeatEvery : kRepeatDelay);
    scrooby::Call(__imp__sub_820D66E0, ctx, base, self);
  }
  g_backspace_down = backspace;
  Mouse(ctx, base, self);
  Prompts(ctx, base);
}

void Leave() {
  if (kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get()) driver->SetTextMode(false);
  g_prompts_for = -1;
  g_keys_self = 0;
}

void Register() {
  data_patcher::Register("space key picture", data_patcher::kCategoryFrontend, "", &PatchSpaceKey);
}

}  // namespace name_typing

// The character under the cursor: a typed character while Type() asks.
extern "C" REX_FUNC(sub_820D6450) {
  if (name_typing::g_forced) {
    ctx.r3.u64 = name_typing::g_forced;
    return;
  }
  __imp__sub_820D6450(ctx, base);
}

// The on-screen keyboard's erase key (row 1, column 10; its only caller is
// the typing update, 0x820D5FC8) types a SPACE instead: the keyboard had no
// space, and erasing has Backspace, X and the prompt. (The port's own erase,
// saves/save_library.cpp, calls __imp__sub_820D66E0: still an erase.)
extern "C" REX_FUNC(sub_820D66E0) {
  name_typing::Type(ctx, base, ctx.r3.u32, u' ');
}
