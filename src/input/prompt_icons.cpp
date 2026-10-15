// =============================================================================
// input/prompt_icons.cpp -- see prompt_icons.h
// =============================================================================
#include "prompt_icons.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/thread/mutex.h>

#include "../data/data_patcher.h"
#include "../data/pure3d.h"
#include "../guest_memory.h"
#include "../pddi/intercept.h"
#include "../players/more_players.h"
#include "../ui/game_font.h"
#include "../ui/scrooby.h"
#include "bindings.h"
#include "keyboard_mouse.h"
#include "players.h"

// stb_image (from the SDK's source tree), private to this file.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

REXCVAR_DEFINE_INT32(button_prompts, 0, "CrashMoM",
                     "Button prompts: 0 = the device used last, 1 = keyboard and mouse keys, "
                     "2 = controller buttons");
REXCVAR_DEFINE_INT32(keyboard_icons, 0, "CrashMoM",
                     "Keyboard and mouse prompt pictures: 0 = light keys, 1 = dark keys");
REXCVAR_DEFINE_INT32(controller_icons, 0, "CrashMoM",
                     "Controller prompt pictures: 0 = automatic (by the controller in use: "
                     "PlayStation 5 / 4, Switch, Xbox One / Series; any other = the game's own), "
                     "1 = the game's own Xbox 360 buttons, 2 = Xbox Series, 3 = PlayStation 5, "
                     "4 = PlayStation 4, 5 = Switch");

// Pure3D's TextureFontLoader::LoadObject (vtable 0x8200E420 slot 5): r4 =
// where to store the font it builds.
extern "C" REX_FUNC(__imp__sub_823C5DA0);

namespace prompt_icons {
namespace {

using kbm::Action;
using kbm::Input;
using Clock = std::chrono::steady_clock;

// --- Game addresses (prompt_icons.h, findings/37) ------------------------------

constexpr uint32_t kTextureFontVtable = 0x8200E394;  // pure3d::TextureFont
constexpr uint32_t kFontPageCount = 60;              // u32
constexpr uint32_t kFontPages = 64;                  // -> pure3d::Texture*[count]
constexpr uint32_t kTexturePddi = 24;                // pure3d::Texture -> pddiTexture (xnTexture)
// xnTexture (native/guest.h, namespace texture): its D3D texture, the ring of
// 16 palettes with the index of the one in use, and "8-bit palettized".
constexpr uint32_t kXnD3DTexture = 20, kXnPalettes = 24, kXnPaletteIndex = 88,
                   kXnPaletteMode = 276;
constexpr uint32_t kD3DFetch = 28;  // a D3D texture's GPU fetch constant (6 words)
// A Scrooby text element: +156 = the string it shows (UTF-16 big-endian).
constexpr uint32_t kTextString = 156;

// GPU fetch constant fields (xenos::xe_gpu_texture_fetch_t): word 0 bits
// 22-30 pitch (/32 texels), 31 tiled; word 1 bits 0-5 format, 6-7 endian,
// 12-31 base address (4 KB units); word 2 = width - 1 | (height - 1) << 13.
constexpr uint32_t kFormat8 = 2, kFormat8888 = 6;

uint32_t Load32(uint8_t* base, uint32_t a) {
  const uint8_t* p = GuestPtr(base, a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// --- The prompt characters we redraw -----------------------------------------

enum class Kind : uint8_t {
  kKey,    // one key: `action`'s
  kStick,  // four keys from `action` on (up, down, left, right in Action order)
  kSpin,   // `action`'s key and a circling arrow
  kSlot,   // a KEY SLOT: whatever key KeyGlyph() lent it (the Options rows)
};
struct Prompt {
  char16_t character;
  // The disc glyph whose picture this one starts as: itself, except for the
  // two MENU glyphs our font patch adds (copies of the A and B pictures).
  char16_t source;
  const char* name;  // the 360 control, for the log
  Kind kind;
  Action action;
};
// The menus' own A, B, X, Y, LB, RB (prompt_icons.h "Menus"): characters of
// the Unicode private use area (no game text contains them), numbered after
// the glyphs they copy.
constexpr char16_t kMenuA = u'\uE0B1', kMenuB = u'\uE0B2', kMenuX = u'\uE0B3',
                   kMenuY = u'\uE0B4', kMenuLB = u'\uE0BE', kMenuRB = u'\uE0B8';
constexpr Prompt kPrompts[] = {
    {u'\u00B1', u'\u00B1', "A", Kind::kKey, kbm::kJump},
    {u'\u00B2', u'\u00B2', "B", Kind::kKey, kbm::kJack},
    {u'\u00B3', u'\u00B3', "X", Kind::kKey, kbm::kLightAttack},
    {u'\u00B4', u'\u00B4', "Y", Kind::kKey, kbm::kHeavyAttack},
    {u'\u00B5', u'\u00B5', "Back", Kind::kKey, kbm::kMap},
    {u'\u00B6', u'\u00B6', "Start", Kind::kKey, kbm::kPause},
    {u'\u00B7', u'\u00B7', "RT", Kind::kKey, kbm::kBlock},
    {u'\u00B8', u'\u00B8', "RB", Kind::kKey, kbm::kPocket},
    {u'\u00BD', u'\u00BD', "LT", Kind::kKey, kbm::kTitanSpecial},
    {u'\u00BE', u'\u00BE', "LB", Kind::kKey, kbm::kLeftBumper},
    {u'\u00B9', u'\u00B9', "left stick", Kind::kStick, kbm::kMoveUp},
    {u'\u00A5', u'\u00A5', "right stick", Kind::kStick, kbm::kRightStickUp},
    {u'\u00BC', u'\u00BC', "rotate", Kind::kSpin, kbm::kSpin},
    // Added by our font patch (the last pages of each font, in this order).
    {kMenuA, u'\u00B1', "menu A", Kind::kKey, kbm::kMenuConfirm},
    {kMenuB, u'\u00B2', "menu B", Kind::kKey, kbm::kMenuBack},
    {kMenuX, u'\u00B3', "menu X", Kind::kKey, kbm::kMenuX},
    {kMenuY, u'\u00B4', "menu Y", Kind::kKey, kbm::kMenuY},
    {kMenuLB, u'\u00BE', "menu LB", Kind::kKey, kbm::kMenuTabLeft},
    {kMenuRB, u'\u00B8', "menu RB", Kind::kKey, kbm::kMenuTabRight},
    // KEY SLOTS (2026-10-15): pictures of ANY key, lent out by KeyGlyph() for
    // the Options screen's key rows (copies of the A picture until then).
    {u'\uE100', u'\u00B1', "key slot 0", Kind::kSlot, kbm::kJump},
    {u'\uE101', u'\u00B1', "key slot 1", Kind::kSlot, kbm::kJump},
    {u'\uE102', u'\u00B1', "key slot 2", Kind::kSlot, kbm::kJump},
    {u'\uE103', u'\u00B1', "key slot 3", Kind::kSlot, kbm::kJump},
    {u'\uE104', u'\u00B1', "key slot 4", Kind::kSlot, kbm::kJump},
    {u'\uE105', u'\u00B1', "key slot 5", Kind::kSlot, kbm::kJump},
    {u'\uE106', u'\u00B1', "key slot 6", Kind::kSlot, kbm::kJump},
    {u'\uE107', u'\u00B1', "key slot 7", Kind::kSlot, kbm::kJump},
    {u'\uE108', u'\u00B1', "key slot 8", Kind::kSlot, kbm::kJump},
    {u'\uE109', u'\u00B1', "key slot 9", Kind::kSlot, kbm::kJump},
    {u'\uE10A', u'\u00B1', "key slot 10", Kind::kSlot, kbm::kJump},
    {u'\uE10B', u'\u00B1', "key slot 11", Kind::kSlot, kbm::kJump},
    {u'\uE10C', u'\u00B1', "key slot 12", Kind::kSlot, kbm::kJump},
    {u'\uE10D', u'\u00B1', "key slot 13", Kind::kSlot, kbm::kJump},
    {u'\uE10E', u'\u00B1', "key slot 14", Kind::kSlot, kbm::kJump},
    {u'\uE10F', u'\u00B1', "key slot 15", Kind::kSlot, kbm::kJump},
};
constexpr size_t kSlotCount = 16;
constexpr char16_t kFirstSlot = u'\uE100';
constexpr size_t kAddedPrompts = 6 + kSlotCount;  // the menu glyphs + the key slots, at the end of kPrompts
constexpr size_t kDiscPrompts = std::size(kPrompts) - kAddedPrompts;

// --- The pages as the disc has them ------------------------------------------

constexpr std::string_view kPackage = "package\\b4c85fe7.p3d";
constexpr uint32_t kChunkTextureFont = 0x22000, kChunkGlyphs = 0x22001;
constexpr uint32_t kChunkTexture = 0x19000, kChunkImage = 0x19001, kChunkImageData = 0x19002;

struct DiscPage {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> indices;  // rows top to bottom (as the TGA's descriptor says)
  std::vector<uint8_t> palette;  // B, G, R, A per entry
  uint32_t colours = 0;          // palette entries in the file
};
struct DiscFont {
  std::string name;
  uint32_t page_count = 0;
  std::map<char16_t, uint32_t> page_of;  // disc prompt character -> its page
  std::map<uint32_t, DiscPage> pages;    // only the prompt pages
};

const pure3d::Chunk* ChildWith(const std::vector<pure3d::Chunk>& list, uint32_t id) {
  for (const pure3d::Chunk& c : list) {
    if (c.id == id) return &c;
  }
  return nullptr;
}

// A texture chunk's TGA (game_font.cpp reads the same; here the indices and
// palette themselves, not just the alpha).
bool DecodeTga(const pure3d::Bytes& d, const pure3d::Chunk& texture, DiscPage& out) {
  // (The child lists are kept in locals: ChildWith points into them.)
  const std::vector<pure3d::Chunk> in_texture = pure3d::ChildrenOf(d, texture);
  const pure3d::Chunk* image = ChildWith(in_texture, kChunkImage);
  if (!image) return false;
  const std::vector<pure3d::Chunk> in_image = pure3d::ChildrenOf(d, *image);
  const pure3d::Chunk* data = ChildWith(in_image, kChunkImageData);
  if (!data || data->data_size < 16 + 18) return false;
  const uint8_t* tga = &d[data->offset + 16];
  const size_t size = std::min<size_t>(pure3d::Le32(&d[data->offset + 12]), data->data_size - 16);
  if (size < 18 || tga[1] != 1 || tga[2] != 1 || tga[7] != 32 || tga[16] != 8) return false;
  out.colours = uint32_t(tga[5] | tga[6] << 8);
  out.width = uint32_t(tga[12] | tga[13] << 8);
  out.height = uint32_t(tga[14] | tga[15] << 8);
  const bool top_down = (tga[17] & 0x20) != 0;
  const size_t palette_at = 18 + tga[0];
  const size_t pixels_at = palette_at + 4 * out.colours;
  if (out.colours == 0 || out.colours > 256 ||
      pixels_at + size_t(out.width) * out.height > size) {
    return false;
  }
  out.palette.assign(tga + palette_at, tga + palette_at + 4 * out.colours);
  out.palette.resize(256 * 4, 0);
  out.indices.resize(size_t(out.width) * out.height);
  for (uint32_t y = 0; y < out.height; ++y) {
    const uint32_t row = top_down ? y : out.height - 1 - y;
    std::memcpy(&out.indices[size_t(y) * out.width], tga + pixels_at + size_t(row) * out.width,
                out.width);
  }
  return true;
}

// Both fonts' prompt pages from the player's own disc data (never saved).
std::vector<DiscFont> ReadDiscFonts() {
  std::vector<DiscFont> fonts;
  pure3d::Bytes d;
  std::string error;
  pure3d::Chunk root;
  std::vector<pure3d::Chunk> top;
  if (!data_patcher::ReadArchiveFile(kPackage, &d, &error) || !pure3d::Parse(d, &root, &top)) {
    REXLOG_WARN("Prompt pictures: can't read {} ({})", kPackage, error);
    return fonts;
  }
  for (const pure3d::Chunk& c : top) {
    if (c.id != kChunkTextureFont || c.data_size < 17) continue;
    DiscFont font;
    const size_t length = d[c.offset + 16];
    font.name.assign(reinterpret_cast<const char*>(&d[c.offset + 17]),
                     std::min<size_t>(length, c.data_size - 17));
    font.name = font.name.substr(0, font.name.find('\0'));
    std::vector<const pure3d::Chunk*> textures;
    const std::vector<pure3d::Chunk> parts = pure3d::ChildrenOf(d, c);
    for (const pure3d::Chunk& p : parts) {
      if (p.id == kChunkTexture) textures.push_back(&p);
    }
    font.page_count = uint32_t(textures.size());
    for (const pure3d::Chunk& p : parts) {
      if (p.id != kChunkGlyphs || p.data_size < 16) continue;
      const size_t count = pure3d::Le32(&d[p.offset + 12]);
      for (size_t i = 0; i < count && 16 + 40 * (i + 1) <= p.data_size; ++i) {
        const uint8_t* r = &d[p.offset + 16 + 40 * i];
        const uint32_t page = pure3d::Le32(r), character = pure3d::Le32(r + 36);
        if (!(page & 0x80000000u)) continue;  // a letter, not a picture
        const uint32_t index = page & 0xFFFF;
        for (size_t k = 0; k < kDiscPrompts; ++k) {
          const Prompt& prompt = kPrompts[k];
          if (prompt.character != character || index >= textures.size()) continue;
          DiscPage dp;
          if (DecodeTga(d, *textures[index], dp)) {
            font.page_of[prompt.character] = index;
            font.pages[index] = std::move(dp);
          }
        }
      }
    }
    if (!font.pages.empty()) fonts.push_back(std::move(font));
  }
  return fonts;
}

// --- Pictures ------------------------------------------------------------------

// RGBA, 8 bits a channel, rows top to bottom, NOT premultiplied.
struct Picture {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;
  bool empty() const { return rgba.empty(); }
};

// assets/prompts/<folder>: keyboard-mouse (Light), keyboard-mouse-dark,
// xbox-series, ps5, ps4, switch (assets/prompts/README.md).
std::filesystem::path AssetFolder(const std::string& folder) {
  return rex::filesystem::GetExecutableFolder() / "assets" / "prompts" / folder;
}

// One PNG of an asset folder, decoded once and kept.
const Picture& Asset(const std::string& folder, const std::string& file) {
  static std::map<std::string, Picture> cache;
  auto [it, added] = cache.try_emplace(folder + "/" + file);
  if (added) {
    const std::filesystem::path path = AssetFolder(folder) / file;
    int w = 0, h = 0, n = 0;
    if (stbi_uc* px = stbi_load(path.string().c_str(), &w, &h, &n, 4)) {
      it->second.width = uint32_t(w);
      it->second.height = uint32_t(h);
      it->second.rgba.assign(px, px + size_t(w) * h * 4);
      stbi_image_free(px);
    } else {
      REXLOG_WARN("Prompt pictures: can't read {}", path.string());
    }
  }
  return it->second;
}

// Shrinks (or grows) a picture to w x h: each output pixel = the average of
// the source area under it (premultiplied by alpha, so transparent pixels
// don't darken the edges). Good for the 100 px pictures -> 42-64 px.
Picture Resize(const Picture& src, uint32_t w, uint32_t h) {
  Picture out;
  out.width = w;
  out.height = h;
  out.rgba.assign(size_t(w) * h * 4, 0);
  if (src.empty() || !w || !h) return out;
  const double sx = double(src.width) / w, sy = double(src.height) / h;
  for (uint32_t y = 0; y < h; ++y) {
    const double y0 = y * sy, y1 = (y + 1) * sy;
    for (uint32_t x = 0; x < w; ++x) {
      const double x0 = x * sx, x1 = (x + 1) * sx;
      double acc[4] = {0, 0, 0, 0}, area = 0;
      for (uint32_t yy = uint32_t(y0); yy < src.height && yy < y1; ++yy) {
        const double wy = std::min(y1, yy + 1.0) - std::max(y0, double(yy));
        for (uint32_t xx = uint32_t(x0); xx < src.width && xx < x1; ++xx) {
          const double wx = std::min(x1, xx + 1.0) - std::max(x0, double(xx));
          const double weight = wx * wy;
          const uint8_t* p = &src.rgba[(size_t(yy) * src.width + xx) * 4];
          const double a = p[3] / 255.0;
          acc[0] += p[0] * a * weight;
          acc[1] += p[1] * a * weight;
          acc[2] += p[2] * a * weight;
          acc[3] += p[3] * weight;
          area += weight;
        }
      }
      uint8_t* o = &out.rgba[(size_t(y) * w + x) * 4];
      if (area <= 0 || acc[3] <= 0) continue;
      const double alpha = acc[3] / area;  // 0..255
      for (int c = 0; c < 3; ++c) {
        o[c] = uint8_t(std::clamp(acc[c] / area / (alpha / 255.0) + 0.5, 0.0, 255.0));
      }
      o[3] = uint8_t(std::clamp(alpha + 0.5, 0.0, 255.0));
    }
  }
  return out;
}

// `src` drawn over `dst` at (x, y) ("over" blending).
void Over(Picture& dst, const Picture& src, int x, int y) {
  for (uint32_t sy = 0; sy < src.height; ++sy) {
    const int dy = y + int(sy);
    if (dy < 0 || dy >= int(dst.height)) continue;
    for (uint32_t sx = 0; sx < src.width; ++sx) {
      const int dx = x + int(sx);
      if (dx < 0 || dx >= int(dst.width)) continue;
      const uint8_t* s = &src.rgba[(size_t(sy) * src.width + sx) * 4];
      uint8_t* d = &dst.rgba[(size_t(dy) * dst.width + dx) * 4];
      const float sa = s[3] / 255.0f, da = d[3] / 255.0f;
      const float oa = sa + da * (1 - sa);
      if (oa <= 0) continue;
      for (int c = 0; c < 3; ++c) {
        d[c] = uint8_t(std::clamp((s[c] * sa + d[c] * da * (1 - sa)) / oa + 0.5f, 0.0f, 255.0f));
      }
      d[3] = uint8_t(std::clamp(oa * 255.0f + 0.5f, 0.0f, 255.0f));
    }
  }
}

// The two keyboard sets (--keyboard_icons): Xelu's Light and Dark keys. The
// circling arrow beside the Spin key is the Light folder's in both.
struct KeySet {
  const char* folder;
  const char* suffix;  // "_Key_Light.png"
  const char* blank;   // the empty key, for keys the set has no picture of
  uint8_t letter;      // the grey of Xelu's letters on that blank key
};
constexpr KeySet kLightKeys{"keyboard-mouse", "_Key_Light.png", "Blank_White_Normal.png", 64};
constexpr KeySet kDarkKeys{"keyboard-mouse-dark", "_Key_Dark.png", "Blank_Black_Normal.png", 225};
constexpr const char* kArrowFolder = "keyboard-mouse";

// Xelu's picture of a key (file in the asset folder), or "" if the set has
// none (then a blank key with the key's name is drawn: Labelled()).
// Names = bindings.cpp's config names. Xelu's file names are the CHARACTER on
// the key cap: "Slash" shows "\", "Question" the "/?" key, "Mark_Left/Right"
// the "<" ">" of the comma / period keys, "Tilda" the backtick key.
std::string PictureFile(Input input, const KeySet& set) {
  const std::string name = kbm::InputName(input);
  if (name.size() == 1) return name + set.suffix;  // A-Z, 0-9
  if (name.rfind("Numpad", 0) == 0 && name.size() == 7) {
    return name.substr(6) + set.suffix;  // Numpad0-9 = the digit
  }
  static const std::map<std::string, std::string> kFiles = {
      {"LMB", "Mouse_Left"},          {"RMB", "Mouse_Right"},
      {"MMB", "Mouse_Middle"},        {"WheelUp", "Mouse_Middle"},
      {"WheelDown", "Mouse_Middle"},  {"Space", "Space"},
      {"Return", "Enter"},            {"Escape", "Esc"},
      {"Tab", "Tab"},                 {"Backspace", "Backspace"},
      {"Shift", "Shift"},             {"Control", "Ctrl"},
      {"Alt", "Alt"},                 {"CapsLock", "Caps_Lock"},
      {"Up", "Arrow_Up"},             {"Down", "Arrow_Down"},
      {"Left", "Arrow_Left"},         {"Right", "Arrow_Right"},
      {"Insert", "Insert"},           {"Delete", "Del"},
      {"Home", "Home"},               {"End", "End"},
      {"PageUp", "Page_Up"},          {"PageDown", "Page_Down"},
      {"Backtick", "Tilda"},          {"Minus", "Minus"},
      {"Plus", "Plus"},               {"LBracket", "Bracket_Left"},
      {"RBracket", "Bracket_Right"},  {"Backslash", "Slash"},
      {"Semicolon", "Semicolon"},     {"Quote", "Quote"},
      {"Comma", "Mark_Left"},         {"Period", "Mark_Right"},
      {"Slash", "Question"},          {"NumpadPlus", "Plus"},
      {"NumpadMinus", "Minus"},       {"NumpadStar", "Asterisk"},
      {"NumLock", "Num_Lock"},        {"PrintScreen", "Print_Screen"},
  };
  if (name.size() >= 2 && name[0] == 'F' && std::isdigit(static_cast<unsigned char>(name[1]))) {
    return name + set.suffix;  // F1-F12 (F13+ have no picture: a label)
  }
  const auto it = kFiles.find(name);
  return it != kFiles.end() ? it->second + set.suffix : std::string();
}

// A short name for a blank key: the rest of the keys and the side mouse
// buttons ("Pause", "M4", "Num ." ...).
std::string ShortLabel(Input input) {
  const std::string name = kbm::InputName(input);
  if (name == "Mouse4") return "M4";
  if (name == "Mouse5") return "M5";
  if (name == "NumpadSlash") return "Num/";
  if (name == "NumpadPeriod") return "Num.";
  if (name == "ScrollLock") return "Scrl";
  if (name == "Menu") return "Menu";
  return name.empty() ? "?" : name;
}

// The game's own font (Titans_Small), for the labels of blank keys.
game_font::Font* LabelFont() {
  static game_font::Font font;
  static bool tried = false;
  if (!tried) {
    tried = true;
    font.Load("Titans_Small");
  }
  return font.loaded() ? &font : nullptr;
}

// Xelu's blank key with `text` written on it in the game's font, dark like
// Xelu's letters, size x size.
Picture Labelled(const std::string& text, uint32_t size, const KeySet& set) {
  Picture key = Resize(Asset(set.folder, set.blank), size, size);
  game_font::Font* font = LabelFont();
  if (!font || key.empty()) return key;
  std::u16string line(text.begin(), text.end());
  const float width = font->Measure(line);
  if (width <= 0) return key;
  // The key cap is ~64% of the picture: the text fits 52% wide, its capitals
  // ~28% tall (one letter looks like Xelu's own; long names shrink).
  const float scale = std::min(size * 0.52f / width, size * 0.28f / font->cap_height());
  float pen = (size - width * scale) / 2;
  const float cell = font->cell_height() * scale;
  const float top = (size - cell) / 2 - size * 0.04f;  // Xelu's caps sit a bit high
  for (char16_t c : line) {
    const game_font::Glyph* g = font->Find(c);
    if (!g) continue;
    const game_font::PageLayers& layers = font->Layers(g->page);
    // Each output pixel inside the glyph's box samples the page's fill layer
    // (nearest texel: the text is small, the quantizer blurs little).
    const float x0 = pen + g->left * scale, y0 = top + g->top * scale;
    const float w = g->width * scale, h = g->height * scale;
    for (int y = int(y0); y < int(std::ceil(y0 + h)); ++y) {
      for (int x = int(x0); x < int(std::ceil(x0 + w)); ++x) {
        if (x < 0 || y < 0 || x >= int(size) || y >= int(size)) continue;
        const float u = g->u0 + (g->u1 - g->u0) * ((x + 0.5f - x0) / w);
        const float v = g->v0 + (g->v1 - g->v0) * ((y + 0.5f - y0) / h);
        const uint32_t tx = std::min(layers.width - 1, uint32_t(std::max(0.0f, u * layers.width)));
        const uint32_t ty =
            std::min(layers.height - 1, uint32_t(std::max(0.0f, v * layers.height)));
        const uint8_t a = layers.fill[(size_t(ty) * layers.width + tx) * 4 + 3];
        uint8_t* d = &key.rgba[(size_t(y) * size + x) * 4];
        // Xelu's letter colour (dark grey) over the cap, by the glyph's alpha.
        for (int ch = 0; ch < 3; ++ch) d[ch] = uint8_t(d[ch] + (set.letter - d[ch]) * a / 255);
      }
    }
    pen += g->advance * scale;
  }
  return key;
}

// The middle of a picture, `margin` pixels cut off each side.
Picture Crop(const Picture& src, uint32_t margin) {
  if (src.empty() || 2 * margin >= src.width || 2 * margin >= src.height) return src;
  Picture out;
  out.width = src.width - 2 * margin;
  out.height = src.height - 2 * margin;
  out.rgba.resize(size_t(out.width) * out.height * 4);
  for (uint32_t y = 0; y < out.height; ++y) {
    std::memcpy(&out.rgba[size_t(y) * out.width * 4],
                &src.rgba[(size_t(y + margin) * src.width + margin) * 4], size_t(out.width) * 4);
  }
  return out;
}

// Up to `margin` pixels off each side, but never into the picture: as many
// as its emptiest-side border has (alpha 0). Xelu's letter keys leave ~13 px,
// but the wide keys and some buttons reach the edge (Space 2 px, Shift / Ctrl
// / Backspace / Enter 4, the mouse and LT 4, the Switch sticks 5): a plain
// 6 px cut clipped them (playtest, 2026-10-15). The same cut on every side
// keeps the shape.
Picture Trim(const Picture& src, uint32_t margin) {
  if (src.empty()) return src;
  uint32_t x0 = src.width, y0 = src.height, x1 = 0, y1 = 0;
  for (uint32_t y = 0; y < src.height; ++y) {
    for (uint32_t x = 0; x < src.width; ++x) {
      if (src.rgba[(size_t(y) * src.width + x) * 4 + 3] == 0) continue;
      x0 = std::min(x0, x);
      y0 = std::min(y0, y);
      x1 = std::max(x1, x + 1);
      y1 = std::max(y1, y + 1);
    }
  }
  if (x1 <= x0) return src;  // empty
  margin = std::min({margin, x0, y0, src.width - x1, src.height - y1});
  return Crop(src, margin);
}

// The picture of one key, size x size. Xelu's 100 px keys leave ~13 px of
// empty border; the 360 buttons fill ~77% of their page. Cutting 6 px off
// each side makes a letter key about as big as the button it replaces
// (the wide keys, Space / Enter / Shift, only reach the sides).
Picture KeyPicture(Input input, uint32_t size, const KeySet& set) {
  const std::string file = PictureFile(input, set);
  if (!file.empty() && !Asset(set.folder, file).empty()) {
    return Resize(Trim(Asset(set.folder, file), 6), size, size);
  }
  return Resize(Trim(Labelled(ShortLabel(input), 100, set), 6), size, size);
}

// --- Controller sets (--controller_icons) ---------------------------------------
// Xelu's buttons for the other controllers, by the 360 button each prompt
// glyph stands for (positions, not letters: the 360's A is the BOTTOM
// button, which is Cross on a PlayStation and B on a Switch). The "rotate"
// arrow keeps the game's own picture (it isn't a button).
enum PadButton { kPadA, kPadB, kPadX, kPadY, kPadBack, kPadStart, kPadRT, kPadRB, kPadLT, kPadLB,
                 kPadLeftStick, kPadRightStick, kPadButtons };
struct PadSet {
  const char* folder;
  const char* name;  // the log's
  const char* files[kPadButtons];
};
// Style numbers: --controller_icons 2..5 = kPadSets[0..3].
constexpr PadSet kPadSets[] = {
    {"xbox-series", "Xbox Series",
     {"XboxSeriesX_A", "XboxSeriesX_B", "XboxSeriesX_X", "XboxSeriesX_Y", "XboxSeriesX_View",
      "XboxSeriesX_Menu", "XboxSeriesX_RT", "XboxSeriesX_RB", "XboxSeriesX_LT", "XboxSeriesX_LB",
      "XboxSeriesX_Left_Stick", "XboxSeriesX_Right_Stick"}},
    {"ps5", "PlayStation 5",
     {"PS5_Cross", "PS5_Circle", "PS5_Square", "PS5_Triangle", "PS5_Share", "PS5_Options", "PS5_R2",
      "PS5_R1", "PS5_L2", "PS5_L1", "PS5_Left_Stick", "PS5_Right_Stick"}},
    {"ps4", "PlayStation 4",
     {"PS4_Cross", "PS4_Circle", "PS4_Square", "PS4_Triangle", "PS4_Share", "PS4_Options", "PS4_R2",
      "PS4_R1", "PS4_L2", "PS4_L1", "PS4_Left_Stick", "PS4_Right_Stick"}},
    {"switch", "Switch",
     {"Switch_B", "Switch_A", "Switch_Y", "Switch_X", "Switch_Minus", "Switch_Plus", "Switch_RT",
      "Switch_RB", "Switch_LT", "Switch_LB", "Switch_Left_Stick", "Switch_Right_Stick"}},
};
constexpr int kOriginalStyle = -1;  // the game's own 360 pictures

// Which button a prompt glyph (its disc source) shows, -1 = none (rotate).
int PadButtonOf(char16_t source) {
  switch (source) {
    case u'\u00B1': return kPadA;
    case u'\u00B2': return kPadB;
    case u'\u00B3': return kPadX;
    case u'\u00B4': return kPadY;
    case u'\u00B5': return kPadBack;
    case u'\u00B6': return kPadStart;
    case u'\u00B7': return kPadRT;
    case u'\u00B8': return kPadRB;
    case u'\u00BD': return kPadLT;
    case u'\u00BE': return kPadLB;
    case u'\u00B9': return kPadLeftStick;
    case u'\u00A5': return kPadRightStick;
    default: return -1;
  }
}

// AUTOMATIC: the controller's maker and model, from its SDL GUID (the
// players' device key "pad:<32 hex digits>[:n]"): bytes 4-5 = USB vendor,
// 8-9 = product, little-endian. Sony 054C: DualSense 0CE6 / DualSense Edge
// 0DF2 = PS5, the rest (DualShock 4 05C4 / 09CC...) = PS4. Nintendo 057E =
// Switch. Microsoft 045E: the 360 pads (028E, 028F, 0719) = the game's own
// pictures, the rest (Xbox One / Series) = Xbox Series. Anyone else (a
// generic or third-party pad) = the game's own 360 look.
int StyleOfPad(const std::string& key) {
  if (key.rfind("pad:", 0) != 0 || key.size() < 4 + 20) return kOriginalStyle;
  auto byte = [&](size_t i) { return uint32_t(std::stoul(key.substr(4 + 2 * i, 2), nullptr, 16)); };
  uint32_t vendor = 0, product = 0;
  try {
    vendor = byte(4) | byte(5) << 8;
    product = byte(8) | byte(9) << 8;
  } catch (...) {
    return kOriginalStyle;
  }
  switch (vendor) {
    case 0x054C: return product == 0x0CE6 || product == 0x0DF2 ? 1 : 2;
    case 0x057E: return 3;
    case 0x045E: return product == 0x028E || product == 0x028F || product == 0x0719 ? kOriginalStyle : 0;
    default: return kOriginalStyle;
  }
}

// --- 256 colours ---------------------------------------------------------------
// The page is 8-bit palettized: our picture must fit 256 colours. Xelu's keys
// are a few greys (and the mouse's red), so resized ones have a few hundred
// colours at most. MEDIAN CUT: put every colour in one box, split the box
// with the most spread at its median until there are 255 boxes, take each
// box's average; entry 0 = transparent.

struct Paletted {
  std::vector<uint8_t> indices;  // one per pixel
  std::array<uint8_t, 256 * 4> palette{};  // R, G, B, A
};

Paletted Quantize(const Picture& pic) {
  struct Colour {
    uint8_t c[4];
    uint32_t count;
  };
  std::unordered_map<uint32_t, uint32_t> counts;
  for (size_t i = 0; i < pic.rgba.size(); i += 4) {
    if (pic.rgba[i + 3] < 4) continue;  // = transparent
    uint32_t key;
    std::memcpy(&key, &pic.rgba[i], 4);
    ++counts[key];
  }
  std::vector<Colour> colours;
  colours.reserve(counts.size());
  for (const auto& [key, count] : counts) {
    Colour c;
    std::memcpy(c.c, &key, 4);
    c.count = count;
    colours.push_back(c);
  }
  // Boxes = ranges of `colours`.
  struct Box {
    size_t begin, end;
  };
  std::vector<Box> boxes;
  if (!colours.empty()) boxes.push_back({0, colours.size()});
  auto spread = [&](const Box& b, int& channel) {
    int best = -1;
    for (int ch = 0; ch < 4; ++ch) {
      uint8_t lo = 255, hi = 0;
      for (size_t i = b.begin; i < b.end; ++i) {
        lo = std::min(lo, colours[i].c[ch]);
        hi = std::max(hi, colours[i].c[ch]);
      }
      if (hi - lo > best) {
        best = hi - lo;
        channel = ch;
      }
    }
    return best;
  };
  while (boxes.size() < 255) {
    // The box with the widest spread that can still split.
    int pick = -1, pick_channel = 0, pick_spread = 0;
    for (size_t i = 0; i < boxes.size(); ++i) {
      if (boxes[i].end - boxes[i].begin < 2) continue;
      int channel = 0;
      const int s = spread(boxes[i], channel);
      if (s > pick_spread) {
        pick = int(i);
        pick_spread = s;
        pick_channel = channel;
      }
    }
    if (pick < 0) break;  // every colour has a box of its own
    Box b = boxes[size_t(pick)];
    std::sort(colours.begin() + b.begin, colours.begin() + b.end,
              [&](const Colour& x, const Colour& y) { return x.c[pick_channel] < y.c[pick_channel]; });
    // Split where half the PIXELS (not colours) are on each side.
    uint64_t total = 0, half = 0;
    for (size_t i = b.begin; i < b.end; ++i) total += colours[i].count;
    size_t split = b.begin + 1;
    for (size_t i = b.begin; i < b.end - 1; ++i) {
      half += colours[i].count;
      split = i + 1;
      if (half * 2 >= total) break;
    }
    boxes[size_t(pick)] = {b.begin, split};
    boxes.push_back({split, b.end});
  }
  Paletted out;
  // Entry 0: transparent, the dark grey of Xelu's outlines (what filtering
  // blends the edges toward).
  out.palette[0] = out.palette[1] = out.palette[2] = 48;
  out.palette[3] = 0;
  std::unordered_map<uint32_t, uint8_t> index_of;
  for (size_t b = 0; b < boxes.size(); ++b) {
    double sum[4] = {0, 0, 0, 0}, n = 0;
    for (size_t i = boxes[b].begin; i < boxes[b].end; ++i) {
      for (int ch = 0; ch < 4; ++ch) sum[ch] += double(colours[i].c[ch]) * colours[i].count;
      n += colours[i].count;
    }
    for (int ch = 0; ch < 4; ++ch) out.palette[(b + 1) * 4 + ch] = uint8_t(sum[ch] / n + 0.5);
    for (size_t i = boxes[b].begin; i < boxes[b].end; ++i) {
      uint32_t key;
      std::memcpy(&key, colours[i].c, 4);
      index_of[key] = uint8_t(b + 1);
    }
  }
  out.indices.resize(pic.rgba.size() / 4, 0);
  for (size_t i = 0; i < out.indices.size(); ++i) {
    if (pic.rgba[4 * i + 3] < 4) continue;
    uint32_t key;
    std::memcpy(&key, &pic.rgba[4 * i], 4);
    out.indices[i] = index_of[key];
  }
  return out;
}

// The key slots' keys (KeyGlyph), under g.mutex: the key, when it was last
// asked for (the least recently asked slot is lent out next) and a revision
// (a change = the pages are drawn again).
struct Slot {
  Input key = kbm::kNoInput;
  uint64_t asked = 0;
};
std::array<Slot, kSlotCount> g_slots;
uint64_t g_slot_clock = 0;
uint32_t g_slot_revision = 0, g_slot_drawn = UINT32_MAX;

// --- What a page should show -----------------------------------------------------

// The first key of `action` (kNoInput: none). Bindings come from the driver.
Input FirstKey(const kbm::Bindings& b, Action action) {
  for (Input in : b.keys[size_t(action)]) {
    if (in != kbm::kNoInput) return in;
  }
  return kbm::kNoInput;
}

// A description of the picture a page should show, "" = the game's own.
// Equal descriptions = equal pictures (the cache key, with the size).
std::string DescribeKeys(const Prompt& p, const kbm::Bindings& b) {
  switch (p.kind) {
    case Kind::kKey: {
      const Input in = FirstKey(b, p.action);
      return in == kbm::kNoInput ? std::string() : "key " + std::to_string(in);
    }
    case Kind::kStick: {
      std::string d = "stick";
      for (int i = 0; i < 4; ++i) {
        const Input in = FirstKey(b, Action(p.action + i));
        if (in == kbm::kNoInput) return {};  // a direction without a key: the stick picture
        d += " " + std::to_string(in);
      }
      return d;
    }
    case Kind::kSpin: {
      const Input in = FirstKey(b, p.action);
      return in == kbm::kNoInput ? std::string() : "spin " + std::to_string(in);
    }
  }
  return {};
}

// What a page should show for the device in use: "" = the game's own
// picture; keys: "L key 32" / "D key 32" (the set's letter first); a
// controller set: "P <set> <button>".
std::string Describe(const Prompt& p, const kbm::Bindings& b, bool keyboard, bool dark, int pad_style) {
  if (p.kind == Kind::kSlot) {  // always a key (the Options rows), in the key set chosen
    const Input key = g_slots[size_t(p.character - kFirstSlot)].key;
    return key == kbm::kNoInput ? std::string() : std::string(dark ? "D " : "L ") + "key " + std::to_string(key);
  }
  if (keyboard) {
    const std::string keys = DescribeKeys(p, b);
    return keys.empty() ? keys : std::string(dark ? "D " : "L ") + keys;
  }
  const int button = PadButtonOf(p.source);
  if (pad_style == kOriginalStyle || button < 0) return {};
  return "P " + std::to_string(pad_style) + " " + std::to_string(button);
}

// Draws what Describe() described, at the page's size.
Picture Draw(const std::string& described, uint32_t width, uint32_t height, uint32_t used_height) {
  Picture page;
  page.width = width;
  page.height = height;
  page.rgba.assign(size_t(width) * height * 4, 0);
  if (described.size() < 2) return page;
  if (described[0] == 'P') {
    // A controller button: Xelu's picture over the square part of the page
    // (a stick's page is 128 x 128 with 96 rows used), cut like the keys.
    int style = 0, button = 0;
    std::sscanf(described.c_str() + 2, "%d %d", &style, &button);
    if (style < 0 || style >= int(std::size(kPadSets)) || button < 0 || button >= kPadButtons) return page;
    const PadSet& set = kPadSets[style];
    const Picture& pic = Asset(set.folder, std::string(set.files[button]) + ".png");
    if (pic.empty()) return page;
    const uint32_t size = std::min(width, used_height);
    Over(page, Resize(Trim(pic, 6), size, size), int(width - size) / 2, int(used_height - size) / 2);
    return page;
  }
  const KeySet& set = described[0] == 'D' ? kDarkKeys : kLightKeys;
  const std::string what = described.substr(2);
  std::vector<Input> keys;
  const size_t space = what.find(' ');
  const std::string kind = what.substr(0, space);
  for (size_t at = space; at != std::string::npos;) {
    const size_t next = what.find(' ', at + 1);
    keys.push_back(Input(std::stoul(what.substr(at + 1, next - at - 1))));
    at = next;
  }
  if (kind == "key" && keys.size() == 1) {
    // One key over the whole (square) page, like the 360 button pictures.
    const uint32_t size = std::min(width, used_height);
    Over(page, KeyPicture(keys[0], size, set), int(width - size) / 2, int(used_height - size) / 2);
  } else if (kind == "stick" && keys.size() == 4) {
    // Up above left / down / right, the usual WASD shape, inside the part
    // of the page the glyph shows (the stick pictures use the top 96 rows of
    // 128: their glyph's v starts at 0.25 from the bottom).
    const uint32_t size = std::min(width / 3, used_height / 2);
    const int x0 = int(width - 3 * size) / 2, y0 = int(used_height - 2 * size) / 2;
    Over(page, KeyPicture(keys[0], size, set), x0 + int(size), y0);                  // up
    Over(page, KeyPicture(keys[2], size, set), x0, y0 + int(size));                  // left
    Over(page, KeyPicture(keys[1], size, set), x0 + int(size), y0 + int(size));      // down
    Over(page, KeyPicture(keys[3], size, set), x0 + 2 * int(size), y0 + int(size));  // right
  } else if (kind == "spin" && keys.size() == 1) {
    // The Spin key on the left half, Xelu's circling arrow on the right
    // (the "rotate" picture is 128 x 64: two squares).
    const uint32_t size = std::min(width / 2, used_height);
    Over(page, KeyPicture(keys[0], size, set), 0, int(used_height - size) / 2);
    const uint32_t arrow = size * 7 / 8;
    Over(page, Resize(Asset(kArrowFolder, "Directional_Arrow_Full_Circle.png"), arrow, arrow),
         int(width / 2 + (width / 2 - arrow) / 2), int(used_height - arrow) / 2);
  }
  return page;
}

// --- The pages in the game's memory -----------------------------------------------

struct GuestPage {
  const Prompt* prompt = nullptr;
  uint32_t font = 0, texture = 0, xn = 0;  // to check it's still the same page
  uint32_t index_fetch1 = 0;               // the D3D texture's fetch word 1 (address)
  uint32_t width = 0, height = 0, used_height = 0;
  uint32_t index_address = 0, row_pitch = 0;
  uint8_t index_xor = 0;     // the endian swap of single bytes (address ^ this)
  bool flipped = false;      // guest row y = disc row height - 1 - y
  uint32_t palette_address = 0;
  int palette_byte[4] = {};  // where B, G, R, A of an entry sit in its 4 guest bytes
  std::vector<uint8_t> original_rows;     // the guest's index rows (row_pitch x height)
  std::vector<uint8_t> original_palette;  // the guest's 1 KB palette
  std::string shown;                      // Describe() of what's in memory ("" = original)
  std::string name;                       // "Titans_Small A" (log)
};

struct State {
  std::mutex mutex;
  std::vector<uint32_t> fonts_loaded;  // from the loader hook, not checked yet
  std::vector<DiscFont> disc;
  bool disc_read = false;
  std::vector<GuestPage> pages;
  // Cache: Describe() + size -> the 256-colour picture.
  std::map<std::string, Paletted> drawn;
  // What the pages show (to notice a change cheaply).
  int device = -1;  // 0 controller, 1 keyboard
  uint32_t revision = UINT32_MAX;
  std::string look;  // the sets in use ("L", "D", "P1"...), to notice a change
  bool dark = false;  // the key set (the key slots use it whatever the device)
} g;
std::atomic<uint32_t> g_pad_user{0};  // the socket a controller was used on last
std::atomic<bool> g_keys_shown{false};  // KeysShown()

std::atomic<int64_t> g_pad_activity{0};  // steady_clock ns of the last controller input
// The game's fonts in memory -> their names (mouse_menus.h measures texts).
std::mutex g_font_names_mutex;
std::map<uint32_t, std::string> g_font_names;
// A font with the menu glyphs was found (our font patch served and checked):
// only then may the prompt bar use them (else they'd draw nothing).
std::atomic<bool> g_menu_glyphs{false};

// Writes host bytes into guest memory that may be graphics memory: the SDK's
// watchers (the emulated GPU's texture cache, our native renderer's write
// watch) hear about it first, as when the game's own code writes there.
void WriteGuest(uint8_t* base, uint32_t address, const uint8_t* data, uint32_t size) {
  if (auto* memory = rex::system::kernel_memory()) {
    memory->TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                           address, size, true, false);
  }
  std::memcpy(GuestPtr(base, address), data, size);
}

// Checks one font the loader built against the disc's fonts and records its
// prompt pages. False = not ready / not one of ours (dropped either way).
void CheckFont(uint8_t* base, uint32_t font) {
  if (Load32(base, font) != kTextureFontVtable) return;
  const uint32_t page_count = Load32(base, font + kFontPageCount);
  const uint32_t array = Load32(base, font + kFontPages);
  if (!array || page_count == 0 || page_count > 256) return;
  // Which disc font: the one with as many pages (36 Large, 22 Small), or two
  // more when our font patch was served (the menu glyphs' pages, at the end).
  for (const DiscFont& disc : g.disc) {
    const bool patched = page_count == disc.page_count + kAddedPrompts;
    if (page_count != disc.page_count && !patched) continue;
    std::vector<GuestPage> found;
    for (size_t k = 0; k < std::size(kPrompts); ++k) {
      const Prompt* prompt = &kPrompts[k];
      const auto source = disc.page_of.find(prompt->source);
      if (source == disc.page_of.end() || (k >= kDiscPrompts && !patched)) continue;
      // Each page must hold its source's picture (an added page = a copy).
      const DiscPage& dp = disc.pages.at(source->second);
      const uint32_t index =
          k < kDiscPrompts ? source->second : disc.page_count + uint32_t(k - kDiscPrompts);
      GuestPage gp;
      gp.prompt = prompt;
      gp.name = disc.name + " " + prompt->name;
      gp.font = font;
      gp.texture = Load32(base, array + 4 * index);
      gp.xn = gp.texture ? Load32(base, gp.texture + kTexturePddi) : 0;
      const uint32_t d3d = gp.xn ? Load32(base, gp.xn + kXnD3DTexture) : 0;
      if (!d3d || Load32(base, gp.xn + kXnPaletteMode) != 1) {
        REXLOG_WARN("Prompt pictures: {} isn't a palettized texture: left alone", gp.name);
        continue;
      }
      const uint32_t palette =
          Load32(base, gp.xn + kXnPalettes + 4 * (Load32(base, gp.xn + kXnPaletteIndex) & 15));
      uint32_t f[6], pf[6];
      for (int i = 0; i < 6; ++i) {
        f[i] = Load32(base, d3d + kD3DFetch + 4 * i);
        pf[i] = palette ? Load32(base, palette + kD3DFetch + 4 * i) : 0;
      }
      gp.index_fetch1 = f[1];
      gp.width = (f[2] & 0x1FFF) + 1;
      gp.height = ((f[2] >> 13) & 0x1FFF) + 1;
      const bool tiled = (f[0] >> 31) != 0, palette_tiled = (pf[0] >> 31) != 0;
      if ((f[1] & 0x3F) != kFormat8 || tiled || !palette || (pf[1] & 0x3F) != kFormat8888 ||
          palette_tiled || gp.width != dp.width || gp.height != dp.height) {
        REXLOG_WARN("Prompt pictures: {} isn't laid out as expected (fetch {:08X} {:08X} {:08X}, "
                    "palette {:08X} {:08X}): left alone",
                    gp.name, f[0], f[1], f[2], pf[0], pf[1]);
        continue;
      }
      // Linear rows: pitch in 32-texel units, rows padded to 256 bytes.
      gp.row_pitch = ((((f[0] >> 22) & 0x1FF) << 5) + 255) & ~255u;
      gp.index_address = f[1] & 0xFFFFF000u;
      const uint32_t endian = (f[1] >> 6) & 3;  // 0 none, 1 8in16, 2 8in32, 3 16in32
      gp.index_xor = endian == 1 ? 1 : endian == 2 ? 3 : endian == 3 ? 2 : 0;
      gp.palette_address = pf[1] & 0xFFFFF000u;
      // The pixels must be the disc's (either row order).
      auto texel = [&](uint32_t x, uint32_t y) {
        return *GuestPtr(base, (gp.index_address + y * gp.row_pitch + x) ^ gp.index_xor);
      };
      bool match[2] = {true, true};
      for (uint32_t y = 0; y < gp.height; ++y) {
        for (uint32_t x = 0; x < gp.width; ++x) {
          const uint8_t want = dp.indices[size_t(y) * gp.width + x];
          match[0] = match[0] && texel(x, y) == want;
          match[1] = match[1] && texel(x, gp.height - 1 - y) == want;
        }
      }
      if (!match[0] && !match[1]) {
        REXLOG_WARN("Prompt pictures: {}'s pixels aren't the disc's: left alone", gp.name);
        continue;
      }
      gp.flipped = !match[0];
      // The palette: find where each of the disc's B, G, R, A sits in the
      // guest's 4 bytes per entry (the GPU's byte order + D3D's swizzle).
      const uint8_t* raw = GuestPtr(base, gp.palette_address);
      bool palette_ok = true;
      for (int c = 0; c < 4 && palette_ok; ++c) {
        gp.palette_byte[c] = -1;
        for (int b = 0; b < 4 && gp.palette_byte[c] < 0; ++b) {
          bool same = true;
          for (uint32_t i = 0; i < dp.colours && same; ++i) {
            same = raw[4 * i + b] == dp.palette[4 * i + c];
          }
          // A byte already taken by another channel can't be this one too.
          for (int k = 0; k < c && same; ++k) same = gp.palette_byte[k] != b;
          if (same) gp.palette_byte[c] = b;
        }
        palette_ok = gp.palette_byte[c] >= 0;
      }
      if (!palette_ok) {
        REXLOG_WARN("Prompt pictures: {}'s palette isn't the disc's: left alone", gp.name);
        continue;
      }
      // The part of the page the glyph shows: the stick pictures use the
      // top 3/4 (their glyph's v starts at 0.25), every other the whole page.
      gp.used_height = prompt->kind == Kind::kStick ? gp.height * 3 / 4 : gp.height;
      gp.original_rows.resize(size_t(gp.row_pitch) * gp.height);
      for (size_t i = 0; i < gp.original_rows.size(); ++i) {
        gp.original_rows[i] = *GuestPtr(base, gp.index_address + uint32_t(i));
      }
      gp.original_palette.assign(raw, raw + 1024);
      found.push_back(std::move(gp));
    }
    REXLOG_INFO("Prompt pictures: {} found ({} prompt pictures checked against the disc{})",
                disc.name, found.size(), patched ? ", menu glyphs included" : "");
    for (GuestPage& gp : found) g.pages.push_back(std::move(gp));
    if (patched) g_menu_glyphs = true;
    {
      std::lock_guard<std::mutex> names(g_font_names_mutex);
      g_font_names[font] = disc.name;
    }
    return;
  }
}

// Is the page still the one we checked (the font not unloaded / replaced)?
bool StillThere(uint8_t* base, const GuestPage& gp) {
  if (Load32(base, gp.font) != kTextureFontVtable) return false;
  const uint32_t array = Load32(base, gp.font + kFontPages);
  bool listed = false;
  for (uint32_t i = 0, n = Load32(base, gp.font + kFontPageCount); i < n && i < 256; ++i) {
    listed = listed || Load32(base, array + 4 * i) == gp.texture;
  }
  if (!listed || Load32(base, gp.texture + kTexturePddi) != gp.xn) return false;
  const uint32_t d3d = Load32(base, gp.xn + kXnD3DTexture);
  return d3d && Load32(base, d3d + kD3DFetch + 4) == gp.index_fetch1;
}

// Puts `what` (Describe()) into the page's memory; "" = the game's own picture.
void Show(uint8_t* base, GuestPage& gp, const std::string& what) {
  if (what.empty()) {
    WriteGuest(base, gp.index_address, gp.original_rows.data(), uint32_t(gp.original_rows.size()));
    WriteGuest(base, gp.palette_address, gp.original_palette.data(), 1024);
  } else {
    const std::string key =
        what + " @" + std::to_string(gp.width) + "x" + std::to_string(gp.height) + "/" +
        std::to_string(gp.used_height);
    auto it = g.drawn.find(key);
    if (it == g.drawn.end()) {
      it = g.drawn.emplace(key, Quantize(Draw(what, gp.width, gp.height, gp.used_height))).first;
    }
    const Paletted& p = it->second;
    // Rows into a copy of the guest's storage (padding bytes kept), in the
    // guest's byte order and row order.
    std::vector<uint8_t> rows = gp.original_rows;
    for (uint32_t y = 0; y < gp.height; ++y) {
      const uint32_t gy = gp.flipped ? gp.height - 1 - y : y;
      for (uint32_t x = 0; x < gp.width; ++x) {
        rows[(gy * gp.row_pitch + x) ^ gp.index_xor] = p.indices[size_t(y) * gp.width + x];
      }
    }
    // Palette: our R, G, B, A -> the disc's B, G, R, A order -> guest bytes.
    std::array<uint8_t, 1024> palette{};
    for (int i = 0; i < 256; ++i) {
      const uint8_t bgra[4] = {p.palette[4 * i + 2], p.palette[4 * i + 1], p.palette[4 * i + 0],
                               p.palette[4 * i + 3]};
      for (int c = 0; c < 4; ++c) palette[4 * i + gp.palette_byte[c]] = bgra[c];
    }
    WriteGuest(base, gp.index_address, rows.data(), uint32_t(rows.size()));
    WriteGuest(base, gp.palette_address, palette.data(), 1024);
  }
  gp.shown = what;
}

// Once per game frame (main thread).
void OnFrameEnd(void*) {
  auto* memory = rex::system::kernel_memory();
  if (!memory) return;
  uint8_t* base = memory->virtual_membase();
  kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  std::lock_guard<std::mutex> lock(g.mutex);

  if (!g.fonts_loaded.empty()) {
    if (!g.disc_read) {
      g.disc_read = true;
      g.disc = ReadDiscFonts();
    }
    for (uint32_t font : g.fonts_loaded) CheckFont(base, font);
    g.fonts_loaded.clear();
    // New pages show the game's own pictures: draw them below even if
    // nothing else changed (the second font arrives a frame or two after the
    // first; without this its pages kept the 360 buttons until the next
    // device switch).
    g.revision = UINT32_MAX;
  }
  if (g.pages.empty()) return;

  // Keys or the controller's buttons?
  int device = 0;
  const int32_t setting = REXCVAR_GET(button_prompts);
  if (driver && setting != 2) {
    device = setting == 1 || driver->last_key_down() >= g_pad_activity.load() ? 1 : 0;
  }
  g_keys_shown = device == 1;
  const uint32_t revision = driver ? driver->bindings_revision() : 0;
  // The sets: Light / Dark keys, the controller's style (Automatic: the
  // model of the controller used last, PadKeyFor its socket).
  const bool dark = REXCVAR_GET(keyboard_icons) == 1;
  int pad_style = kOriginalStyle;
  const int32_t pad_setting = REXCVAR_GET(controller_icons);
  if (pad_setting >= 2 && pad_setting < 2 + int(std::size(kPadSets))) {
    pad_style = pad_setting - 2;
  } else if (pad_setting == 0) {
    if (const kbm::PlayerAssignment* assignment = kbm::PlayerAssignment::Get()) {
      pad_style = StyleOfPad(assignment->PadKeyFor(g_pad_user.load()));
    }
  }
  const std::string look = device ? (dark ? "D" : "L") : "P" + std::to_string(pad_style);
  if (device == g.device && revision == g.revision && look == g.look && g_slot_drawn == g_slot_revision &&
      dark == g.dark) {
    return;
  }
  g_slot_drawn = g_slot_revision;
  g.dark = dark;
  if (device != g.device || look != g.look) {
    REXLOG_INFO("Prompt pictures: {}", device ? (dark ? "keyboard and mouse keys (dark)" : "keyboard and mouse keys")
                                       : pad_style == kOriginalStyle ? std::string("controller buttons (the game's own)")
                                       : std::string("controller buttons: ") + kPadSets[pad_style].name);
  }
  g.device = device;
  g.revision = revision;
  g.look = look;

  const kbm::Bindings bindings = driver ? driver->bindings() : kbm::Bindings{};
  for (size_t i = 0; i < g.pages.size();) {
    GuestPage& gp = g.pages[i];
    if (!StillThere(base, gp)) {
      REXLOG_INFO("Prompt pictures: {} is gone (font unloaded): forgotten", gp.name);
      g.pages.erase(g.pages.begin() + std::ptrdiff_t(i));
      continue;
    }
    const std::string want = Describe(*gp.prompt, bindings, device == 1, dark, pad_style);
    if (want != gp.shown) Show(base, gp, want);
    ++i;
  }
}

// --- The font patch: six more prompt glyphs for the menus, and 16 key slots -------
// (prompt_icons.h "Menus"). In package\b4c85fe7.p3d, for each of the two fonts:
//   * the A, B, X, Y, LB, RB pictures' texture chunks (0x19000) copied,
//     renamed after the next page numbers ("..._texture_36" on), inserted
//     after the last page: the loader numbers pages in file order;
//   * the font's page count (the last u32 of its own data) + 6 (+ 16 key
//     slots since 2026-10-15: copies of the A picture, U+E100..E10F);
//   * six (+ 16) glyph records (0x22001: u32 count, 40 bytes each) copied from the
//     sources' (kPrompts), with the new page (bit 31 = a picture) and the menu
//     character, inserted
//     IN CHARACTER ORDER: the font looks characters up by binary search over
//     the table as the file has it (0x823C6764), sorted, ending with U+FFFF
//     (first try: appended after U+FFFF = never found, nothing drawn);
//   * the same two characters in the 0x22006 list (u32 count, then u32
//     character + u32 value, one per picture glyph; its value copied), also
//     in order.

// Inserts `record` (`size` bytes, its u32 character at `char_at`) into a
// table of `count` records starting at data offset 4, before the first
// record with a bigger character.
void InsertSorted(pure3d::Node& table, size_t size, size_t char_at, const std::vector<uint8_t>& record) {
  const uint32_t count = pure3d::NodeLe32(table, 0);
  const uint32_t character = pure3d::Le32(&record[char_at]);
  size_t at = 4 + size * count;
  for (uint32_t i = 0; i < count; ++i) {
    if (pure3d::NodeLe32(table, 4 + size * i + char_at) > character) {
      at = 4 + size * i;
      break;
    }
  }
  table.data.insert(table.data.begin() + std::ptrdiff_t(at), record.begin(), record.end());
  pure3d::SetNodeLe32(table, 0, count + 1);
}

// Renames a page's texture chunk (and its image, which carries the same
// name) to `name`. Pure3D names: u8 length + bytes, NUL padded.
void RenamePage(pure3d::Node& texture, std::string_view old_name, std::string_view name) {
  pure3d::SetNodeName(texture, name);
  for (pure3d::Node& child : texture.children) {
    if (pure3d::NodeName(child) == old_name) pure3d::SetNodeName(child, name);
  }
}

bool PatchFont(pure3d::Node& font) {
  std::vector<size_t> pages;
  pure3d::Node* glyphs = nullptr;
  pure3d::Node* pictures = nullptr;
  for (size_t i = 0; i < font.children.size(); ++i) {
    pure3d::Node& c = font.children[i];
    if (c.id == kChunkTexture) pages.push_back(i);
    if (c.id == kChunkGlyphs) glyphs = &c;
    if (c.id == 0x22006) pictures = &c;
  }
  if (pages.empty() || !glyphs || glyphs->data.size() < 4 || font.data.size() < 4) return false;
  const size_t count_at = font.data.size() - 4;
  const uint32_t page_count = pure3d::NodeLe32(font, count_at);
  const uint32_t glyph_count = pure3d::NodeLe32(*glyphs, 0);
  if (page_count != pages.size() || glyphs->data.size() < 4 + 40 * size_t(glyph_count)) return false;
  // ± and ²'s glyph records.
  std::vector<uint8_t> records[kAddedPrompts];
  for (uint32_t i = 0; i < glyph_count; ++i) {
    const uint8_t* r = &glyphs->data[4 + 40 * i];
    for (size_t k = 0; k < kAddedPrompts; ++k) {
      if (pure3d::Le32(r + 36) == kPrompts[kDiscPrompts + k].source) records[k].assign(r, r + 40);
    }
  }
  std::vector<pure3d::Node> added;
  for (size_t k = 0; k < kAddedPrompts; ++k) {
    if (records[k].empty()) return false;
    const uint32_t source_page = pure3d::Le32(&records[k][0]) & 0xFFFF;
    if (source_page >= pages.size()) return false;
    pure3d::Node copy = font.children[pages[source_page]];
    const std::string old_name(pure3d::NodeName(copy));
    const uint32_t page = page_count + uint32_t(k);
    // "Titans_final_64_texture_8" -> "Titans_final_64_texture_22".
    RenamePage(copy, old_name, old_name.substr(0, old_name.rfind('_') + 1) + std::to_string(page));
    added.push_back(std::move(copy));
    // Its glyph record: the new page and character.
    std::vector<uint8_t>& r = records[k];
    const uint32_t picture_page = page | 0x80000000u, character = kPrompts[kDiscPrompts + k].character;
    for (int b = 0; b < 4; ++b) {
      r[b] = uint8_t(picture_page >> (8 * b));
      r[36 + b] = uint8_t(character >> (8 * b));
    }
    InsertSorted(*glyphs, 40, 36, r);
    // The picture list's entry.
    if (pictures && pictures->data.size() >= 4) {
      const uint32_t n = pure3d::NodeLe32(*pictures, 0);
      for (uint32_t i = 0; i < n && 4 + 8 * size_t(i + 1) <= pictures->data.size(); ++i) {
        if (pure3d::NodeLe32(*pictures, 4 + 8 * i) != kPrompts[kDiscPrompts + k].source) continue;
        std::vector<uint8_t> entry(pictures->data.begin() + 4 + 8 * i,
                                   pictures->data.begin() + 4 + 8 * (i + 1));
        for (int b = 0; b < 4; ++b) entry[b] = uint8_t(character >> (8 * b));
        InsertSorted(*pictures, 8, 0, entry);
        break;
      }
    }
  }
  pure3d::SetNodeLe32(font, count_at, page_count + uint32_t(kAddedPrompts));
  font.children.insert(font.children.begin() + std::ptrdiff_t(pages.back() + 1), added.begin(),
                       added.end());
  return true;
}

bool PatchFonts(const pure3d::Bytes& original, pure3d::Bytes* patched) {
  pure3d::Chunk root;
  std::vector<pure3d::Chunk> top;
  if (!pure3d::Parse(original, &root, &top)) return false;
  pure3d::Bytes children;
  int fonts = 0;
  for (const pure3d::Chunk& c : top) {
    if (c.id == kChunkTextureFont) {
      pure3d::Node font = pure3d::ToNode(original, c);
      if (!PatchFont(font)) {
        REXLOG_WARN("Prompt pictures: a font in {} isn't as expected: menu glyphs not added", kPackage);
        return false;
      }
      const pure3d::Bytes bytes = pure3d::ToBytes(font);
      children.insert(children.end(), bytes.begin(), bytes.end());
      ++fonts;
    } else {
      const pure3d::Bytes bytes = pure3d::Copy(original, c);
      children.insert(children.end(), bytes.begin(), bytes.end());
    }
  }
  if (fonts == 0) return false;
  *patched = pure3d::WithChildren(original, root, children);
  return true;
}

}  // namespace

void Register() {
  data_patcher::Register("menu button glyphs", data_patcher::kCategoryFrontend, "b4c85fe7",
                         &PatchFonts);
}

void Install() { pddi::AddFrameEndListener(&OnFrameEnd, nullptr); }

std::string FontNameAt(uint32_t font) {
  std::lock_guard<std::mutex> lock(g_font_names_mutex);
  const auto it = g_font_names.find(font);
  return it != g_font_names.end() ? it->second : std::string();
}

bool KeysShown() { return g_keys_shown.load(); }

char16_t KeyGlyph(Input key) {
  if (key == kbm::kNoInput || !g_menu_glyphs.load()) return 0;
  std::lock_guard<std::mutex> lock(g.mutex);
  size_t slot = 0;
  for (size_t i = 0; i < g_slots.size(); ++i) {
    if (g_slots[i].key == key) {
      slot = i;
      break;
    }
    if (g_slots[i].asked < g_slots[slot].asked) slot = i;  // least recently asked
  }
  if (g_slots[slot].key != key) {
    g_slots[slot].key = key;
    ++g_slot_revision;
  }
  g_slots[slot].asked = ++g_slot_clock;
  return char16_t(kFirstSlot + slot);
}

char16_t MenuGlyph(char16_t c) {
  if (!g_menu_glyphs.load(std::memory_order_relaxed)) return c;
  for (size_t k = kDiscPrompts; k < std::size(kPrompts); ++k) {
    if (kPrompts[k].source == c) return kPrompts[k].character;
  }
  return c;
}

void FrontEndFrame(PPCContext& ctx, uint8_t* base) {
  if (!g_menu_glyphs.load(std::memory_order_relaxed)) return;
  // The menus' prompt bar: page FE_Buttons (always loaded, in the front end
  // and in game), whose BUTTON texts hold just the glyph ("±"), apart from
  // their labels ("Select"): their play glyphs become the menu ones. Found
  // every frame (four lookups by name): pages come and go with the packages.
  const uint32_t page = scrooby::FindPage(ctx, base, "FE_Buttons");
  if (!page) return;
  static constexpr const char* kButtons[] = {"LowerLeftButton", "LowerRightButton",
                                             "UpperLeftButton", "UpperRightButton"};
  for (const char* name : kButtons) {
    const uint32_t text = scrooby::FindText(ctx, base, page, name);
    const uint32_t string = text ? Load32(base, text + kTextString) : 0;
    if (!string) continue;
    std::u16string value;
    bool changed = false;
    for (uint32_t i = 0; i < 64; ++i) {
      const uint8_t* c = GuestPtr(base, string + 2 * i);
      char16_t ch = char16_t(c[0] << 8 | c[1]);
      if (!ch) break;
      const char16_t menu = MenuGlyph(ch);
      changed = changed || menu != ch;
      value.push_back(menu);
    }
    if (changed) scrooby::SetText(ctx, base, text, value);
  }
}

void SeePad(uint32_t user, const uint8_t* gamepad) {
  // Guest X_INPUT_GAMEPAD: u16 buttons, u8 LT, u8 RT, s16 LX, LY, RX, RY (big-endian).
  const uint16_t buttons = uint16_t(gamepad[0] << 8 | gamepad[1]);
  auto stick = [&](int i) { return int16_t(uint16_t(gamepad[4 + 2 * i] << 8 | gamepad[5 + 2 * i])); };
  // Take away what the keyboard pressed (it's merged into its player's
  // state): what's left came from a controller. Sticks count only while the
  // keyboard leaves them alone, well past XInput's dead zone (7,849), so a
  // drifting stick doesn't count.
  uint16_t kb_buttons = 0;
  bool kb_sticks = false, kb_triggers = false;
  if (kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get()) {
    const rex::input::X_INPUT_GAMEPAD kb = driver->last_pad();
    kb_buttons = kb.buttons;
    kb_sticks = kb.thumb_lx != 0 || kb.thumb_ly != 0 || kb.thumb_rx != 0 || kb.thumb_ry != 0;
    kb_triggers = kb.left_trigger != 0 || kb.right_trigger != 0;
  }
  bool pad = (buttons & ~kb_buttons) != 0;
  if (!kb_triggers && (gamepad[2] > 64 || gamepad[3] > 64)) pad = true;
  if (!kb_sticks) {
    for (int i = 0; i < 4; ++i) pad = pad || std::abs(int(stick(i))) > 12000;
  }
  if (pad) {
    g_pad_activity = Clock::now().time_since_epoch().count();
    g_pad_user = user;
  }
}

}  // namespace prompt_icons

// TextureFontLoader::LoadObject: the game's fonts, as they're loaded.
extern "C" REX_FUNC(sub_823C5DA0) {
  const uint32_t out = ctx.r4.u32;
  __imp__sub_823C5DA0(ctx, base);
  const uint32_t font = out ? prompt_icons::Load32(base, out) : 0;
  if (font) {
    std::lock_guard<std::mutex> lock(prompt_icons::g.mutex);
    prompt_icons::g.fonts_loaded.push_back(font);
  }
}

