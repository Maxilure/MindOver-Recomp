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
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <mutex>
#include <set>
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
#include "../players/dummy_players.h"
#include "../players/players_setup.h"

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
REXCVAR_DEFINE_STRING(debug_fake_pad_styles, "", "CrashMoM",
                      "Debug: the button pictures of --debug_fake_pads' controllers 2, 3, 4 (with "
                      "Controller Icons = automatic), comma-separated: xbox-series, ps5, ps4, switch, 360 "
                      "(e.g. \"ps5,switch,xbox-series\": every test player in a look of their own)");

// Point a Scrooby text at a borrowed string (0 = its text bank default);
// ui/scrooby.cpp's wrapper frees an owned copy first.
extern "C" REX_FUNC(sub_823731D0);

// Pure3D's TextureFontLoader::LoadObject (vtable 0x8200E420 slot 5): r4 =
// where to store the font it builds.
extern "C" REX_FUNC(__imp__sub_823C5DA0);

REXCVAR_DECLARE(bool, simple_spinning);  // gameplay/game_mods.cpp
REXCVAR_DECLARE(bool, keep_spinning);

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
void Store32(uint8_t* base, uint32_t a, uint32_t v) {
  uint8_t* p = GuestPtr(base, a);
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

// --- The prompt characters we redraw -----------------------------------------

enum class Kind : uint8_t {
  kKey,    // one key: `action`'s
  kStick,  // four keys from `action` on (up, down, left, right in Action order)
  kSpin,   // `action`'s key and a circling arrow
  kSlot,   // a KEY SLOT: whatever key KeyGlyph() lent it (the Options rows)
  kPlayerSlot,  // a PLAYER SLOT: one player's picture of a prompt (PlayerGlyph, below)
};
struct Prompt {
  char16_t character;
  // The disc glyph whose picture this one starts as: itself, except for the
  // two MENU glyphs our font patch adds (copies of the A and B pictures).
  char16_t source;
  const char* name;  // the 360 control, for the log
  Kind kind;
  Action action;
  // Added to the small font only (Titans_Small): the font has room for 64
  // pages (below, "64 PAGES"), the large one for fewer of ours.
  bool small_only = false;
};
// The menus' own A, B, X, Y, LB, RB (prompt_icons.h "Menus"): characters of
// the Unicode private use area (no game text contains them), numbered after
// the glyphs they copy.
constexpr char16_t kMenuA = u'\uE0B1', kMenuB = u'\uE0B2', kMenuX = u'\uE0B3',
                   kMenuY = u'\uE0B4', kMenuLB = u'\uE0BE', kMenuRB = u'\uE0B8';
constexpr Prompt kBasePrompts[] = {
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
    // BIG SLOTS (2026-10-15): the same, as copies of the LEFT STICK's picture
    // (its 128 x 128 page): a key drawn there is as big as the stick. For the
    // spin rows' "OR Q" under "stick + arrow" (playtest: an A-sized Q looked
    // small next to the stick).
    {u'\uE110', u'\u00B9', "big key slot 0", Kind::kSlot, kbm::kJump},
    {u'\uE111', u'\u00B9', "big key slot 1", Kind::kSlot, kbm::kJump},
    {u'\uE112', u'\u00B9', "big key slot 2", Kind::kSlot, kbm::kJump},
    {u'\uE113', u'\u00B9', "big key slot 3", Kind::kSlot, kbm::kJump},
    // CARD SLOTS (2026-10-15): copies of the A picture's page (64 x 64, which
    // sits on the text line like a letter), the picture FILLING the page's
    // width (key, button or stick alike): for pictures inside the Options
    // cards' small print, where a stick squeezed into the A's own 49-pixel
    // circle was too small to make out (playtest). (A first version copied
    // the stick's 128 x 96 page: much too big, and it hung below the line.)
    {u'\uE114', u'\u00B1', "card slot 0", Kind::kSlot, kbm::kJump, true},
    {u'\uE115', u'\u00B1', "card slot 1", Kind::kSlot, kbm::kJump, true},
    {u'\uE116', u'\u00B1', "card slot 2", Kind::kSlot, kbm::kJump, true},
    {u'\uE117', u'\u00B1', "card slot 3", Kind::kSlot, kbm::kJump, true},
    {u'\uE118', u'\u00B1', "card slot 4", Kind::kSlot, kbm::kJump, true},
    {u'\uE119', u'\u00B1', "card slot 5", Kind::kSlot, kbm::kJump, true},
    // PLAYER SLOTS (2026-10-17): one player's own picture of a prompt glyph,
    // lent out by PlayerGlyph() (below, "EACH PLAYER'S OWN PICTURES"); copies
    // of the A picture's page. Small font only.
    {u'\uE120', u'\u00B1', "player slot 0", Kind::kPlayerSlot, kbm::kJump, true},
    {u'\uE121', u'\u00B1', "player slot 1", Kind::kPlayerSlot, kbm::kJump, true},
    {u'\uE122', u'\u00B1', "player slot 2", Kind::kPlayerSlot, kbm::kJump, true},
    {u'\uE123', u'\u00B1', "player slot 3", Kind::kPlayerSlot, kbm::kJump, true},
    {u'\uE124', u'\u00B1', "player slot 4", Kind::kPlayerSlot, kbm::kJump, true},
    {u'\uE125', u'\u00B1', "player slot 5", Kind::kPlayerSlot, kbm::kJump, true},
    {u'\uE126', u'\u00B1', "player slot 6", Kind::kPlayerSlot, kbm::kJump, true},
    {u'\uE127', u'\u00B1', "player slot 7", Kind::kPlayerSlot, kbm::kJump, true},
    {u'\uE128', u'\u00B1', "player slot 8", Kind::kPlayerSlot, kbm::kJump, true},
    {u'\uE129', u'\u00B1', "player slot 9", Kind::kPlayerSlot, kbm::kJump, true},
};
constexpr size_t kSmallSlots = 16, kBigSlots = 4, kCardSlots = 6;
constexpr size_t kSlotCount = kSmallSlots + kBigSlots + kCardSlots;
constexpr size_t kFirstBig = kSmallSlots, kFirstCard = kSmallSlots + kBigSlots;  // slot numbers
// A card slot's picture: the page's whole width (a stick, wider than tall,
// gets all 64), at most this many rows, centred where the A's own circle is
// (49 rows): a little larger than the game's buttons, still inside a line.
constexpr int kCardSlotRows = 54;
// How big a big slot's key is, against the stick's picture (playtest: 1.0 =
// too big, the A-sized slot = too small).
constexpr float kBigSlotScale = 0.65f;
constexpr char16_t kFirstSlot = u'\uE100';
constexpr size_t kPlayerSlots = 10;
constexpr char16_t kFirstPlayerSlot = u'\uE120';
constexpr size_t kBaseAdded = 6 + kSlotCount + kPlayerSlots;  // menu glyphs, key slots, player slots
constexpr size_t kDiscPrompts = std::size(kBasePrompts) - kBaseAdded;

// 64 PAGES (2026-10-17): a font's pages can't go past 64. The font's draw
// (sub_823C4FA0 and the glyph batching around 0x823C4324) keeps per-page
// tables in fixed globals of 64 entries each (0x8257E6E0 glyph counts,
// 0x8257E7E0, 0x8257F0E0, 0x8257F1E0 vertex lists: 256 bytes apart), and
// clears / fills them for EVERY page of the font: a 65th page writes into
// the next table. Found when a full copy of the prompt glyphs per player
// (19 x 3 more pages) drew nothing past page 64 (and wrote over those
// tables). The disc's fonts have 22 (Titans_Small) and 36 (Titans_Large)
// pages; ours: Small 22 + 6 menu glyphs + 26 key slots + 10 player slots =
// 64, Large 36 + 6 + 20 key slots (no card slots, no player slots) = 62.
constexpr uint32_t kMaxFontPages = 64;

// The prompts a font gets: the disc's, then the ones our font patch adds (in
// this order, after the disc's pages); the large font skips small_only ones.
std::vector<const Prompt*> PromptsFor(bool large) {
  std::vector<const Prompt*> v;
  for (const Prompt& p : kBasePrompts) {
    if (!(large && p.small_only)) v.push_back(&p);
  }
  return v;
}
size_t AddedPrompts(bool large) { return PromptsFor(large).size() - kDiscPrompts; }
// Which font by its disc page count (Titans_Large 36, Titans_Small 22).
bool IsLargeFont(uint32_t disc_pages) { return disc_pages > 30; }
// The world's B (State::textures_loaded): the B button, Jack on the keyboard.
constexpr Prompt kWorldB{0, u'\u00B2', "world B", Kind::kKey, kbm::kJack};
constexpr std::string_view kWorldPackage = "package\\bca1bc0b.p3d";
constexpr std::string_view kWorldTexture = "FE_360_b_button_64.tga";
// WORLD B: THE BUTTON'S MATERIAL (2026-10-15). The B is drawn on a 12-sided
// coin mesh (THE BUTTON'S SHAPE, below) by the material "HUD_button_circle_m"
// (chunk 0x11000 in the same package): opaque, ALPHA TEST OFF (ATST 0), so
// every pixel is solid and a see-through one is drawn as its colour: black.
// The red ball's dark rim on screen is exactly that (with the B's picture
// emptied, a black 12-gon remained; filled with magenta, a magenta 12-gon).
// The data patch switches the material's alpha test ON (ATST 1; its compare
// ACMP 4 = greater, threshold ACTH 0.5): our pictures keep their transparency
// and their see-through parts are left out.
constexpr std::string_view kWorldMaterial = "HUD_button_circle_m";
constexpr uint32_t kCategoryJoinedAssets = 10;  // data_patcher category of its package
constexpr std::string_view kWorldMesh = "evil_circle_geoShape";  // THE BUTTON'S SHAPE (below)
constexpr uint32_t kChunkMesh = 0x10000, kChunkPrimGroup = 0x10020, kChunkPositions = 0x10005, kChunkUvs = 0x10007;
constexpr uint32_t kChunkShader = 0x11000, kChunkShaderInt = 0x11003;

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
          const Prompt& prompt = kBasePrompts[k];
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

// A texture of the world's B as the disc has it (in kWorldPackage).
// THE BUTTON'S SHAPE (2026-10-15). The B's mesh, "evil_circle_geoShape", is
// a 12-sided coin (radius 0.36, 0.27 thick, the front a shallow dome) whose
// texture coordinates are a flat projection (u = 0.5 + 1.261 x, v = 0.5 -
// 1.261 y): on screen a 12-sided window onto the 64 x 64 picture, radius 0.454
// of it. A key picture is square, so the data patch makes the coin's faces
// SQUARE (each rim corner moved out to the square: those at 0 / 90 / 180 /
// 270 degrees to the edge middles, the pairs at 30 / 60 degrees etc. both to
// their quadrant's corner; texture coordinates by the same projection), and
// bakes the 12-sided window into the game's own B picture instead: its
// pixels outside that window get a see-through palette colour (with the
// material's alpha test on, they're left out), every other colour is made
// opaque (the material drew them all opaque: the dark rim around the ball is
// "see-through" black inside the window). So the game's B looks exactly as
// before, and a key is a whole square key.
constexpr float kCoinWindow = 0.454f;  // the 12-sided window's corner radius, fraction of the picture
// The world B's copies (PatchWorldButton): 0-3 = players 1-4's, 4 = the jack
// symbol's (a stunned titan out of everyone's reach: WHO CAN JACK IT).
constexpr int kJackSymbol = 4;
constexpr float kJackSymbolScale = 1.7f;  // the jack symbol's coin, drawn this much bigger (CoinGroup::scale)

// Bakes the window into the game's B: `indices` w x h (either row order: the
// window is symmetric), `palette` BGRA x `colours`. The see-through colour =
// the first palette entry no pixel uses. False if there's none.
bool BakeWorldButton(uint8_t* indices, uint32_t w, uint32_t h, uint8_t* palette, size_t colours) {
  std::array<bool, 256> used{};
  for (size_t i = 0; i < size_t(w) * h; ++i) used[indices[i]] = true;
  size_t clear = colours;
  for (size_t i = 0; i < colours && clear == colours; ++i) {
    if (!used[i]) clear = i;
  }
  if (clear == colours) return false;
  for (size_t i = 0; i < colours; ++i) palette[4 * i + 3] = 255;
  std::fill(palette + 4 * clear, palette + 4 * clear + 4, uint8_t(0));
  // Inside the 12-gon (corners at 0, 30, 60... degrees): within its inner
  // radius along each edge's normal (15, 45, 75... degrees), half a pixel to spare.
  const float r = kCoinWindow * float(w) * std::cos(15.0f * 3.14159265f / 180.0f) + 0.5f;
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      const float dx = x + 0.5f - w / 2.0f, dy = y + 0.5f - h / 2.0f;
      bool inside = true;
      for (int k = 0; k < 6 && inside; ++k) {
        const float a = (15.0f + 30.0f * float(k)) * 3.14159265f / 180.0f;
        inside = std::abs(dx * std::cos(a) + dy * std::sin(a)) <= r;
      }
      if (!inside) indices[size_t(y) * w + x] = uint8_t(clear);
    }
  }
  return true;
}

std::optional<DiscPage> ReadWorldTexture(std::string_view name) {
  pure3d::Bytes d;
  std::string error;
  pure3d::Chunk root;
  std::vector<pure3d::Chunk> top;
  if (!data_patcher::ReadArchiveFile(std::string(kWorldPackage), &d, &error) || !pure3d::Parse(d, &root, &top)) {
    REXLOG_WARN("Prompt pictures: can't read {} ({})", kWorldPackage, error);
    return std::nullopt;
  }
  for (const pure3d::Chunk& c : top) {
    if (c.id != kChunkTexture || pure3d::NameOf(d, c) != name) continue;
    DiscPage dp;
    if (DecodeTga(d, c, dp)) {
      // As the data patch serves it (THE BUTTON'S SHAPE): baked.
      if (dp.palette.size() >= 4 * size_t(dp.colours) && BakeWorldButton(dp.indices.data(), dp.width, dp.height, dp.palette.data(), dp.colours)) {
        return dp;
      }
    }
  }
  REXLOG_WARN("Prompt pictures: no {} in {}", name, kWorldPackage);
  return std::nullopt;
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

constexpr uint32_t kTrimAll = 1000;  // Trim: every empty border

// The picture of one key, its drawn part only (Xelu's 100 px keys leave
// ~13 px of empty border), at `size` % of Xelu's size: Fit places it where
// the original picture is drawn (THE SIZE OF A PICTURE).
Picture KeyPicture(Input input, uint32_t size, const KeySet& set) {
  const std::string file = PictureFile(input, set);
  if (!file.empty() && !Asset(set.folder, file).empty()) {
    const Picture cut = Trim(Asset(set.folder, file), kTrimAll);
    return Resize(cut, uint32_t(cut.width * size / 100), uint32_t(cut.height * size / 100));
  }
  const Picture cut = Trim(Labelled(ShortLabel(input), 100, set), kTrimAll);
  return Resize(cut, uint32_t(cut.width * size / 100), uint32_t(cut.height * size / 100));
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

// Each button's prompt glyph on the disc (its 360 picture), by PadButton.
constexpr char16_t kButtonGlyphs[kPadButtons] = {u'\u00B1', u'\u00B2', u'\u00B3', u'\u00B4',
                                                u'\u00B5', u'\u00B6', u'\u00B7', u'\u00B8',
                                                u'\u00BD', u'\u00BE', u'\u00B9', u'\u00A5'};

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
  // A debug fake controller ("debug-pad-2".."4"): the style --debug_fake_pad_styles
  // gives it (tests of per-player pictures with every player in another look).
  if (key.rfind("debug-pad-", 0) == 0 && key.size() == 11) {
    // A dummy that pretends to be a real model (F5 Dummy players): detect from
    // that model's GUID, exactly as for a plugged-in pad.
    if (const std::string pretend = dummy_players::DetectorKey(key.back() - '2'); !pretend.empty()) {
      return StyleOfPad(pretend);
    }
    std::string_view list = REXCVAR_GET(debug_fake_pad_styles);
    for (int n = key.back() - '2'; n > 0 && !list.empty(); --n) {
      const size_t comma = list.find(',');
      list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
    }
    const std::string_view name = list.substr(0, list.find(','));
    for (int i = 0; i < int(std::size(kPadSets)); ++i) {
      if (name == kPadSets[i].folder) return i;
    }
    return kOriginalStyle;  // "360", empty or unknown: the game's own
  }
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
// A slot holds a key ("K <input>": KeyGlyph) or a controller button
// ("B <PadButton>": ButtonGlyph, drawn in the Controller Icons style).
struct Slot {
  std::string what;
  uint64_t asked = 0;
};
std::array<Slot, kSlotCount> g_slots;
uint64_t g_slot_clock = 0;
uint32_t g_slot_revision = 0, g_slot_drawn = UINT32_MAX;

// What one player's prompts show (LookOf).
struct Look {
  bool keys = false;               // keyboard keys (else the controller's buttons)
  bool dark = false;               // the key set: Xelu's Dark (else Light)
  int pad_style = kOriginalStyle;  // the controller set (kPadSets; -1 = the game's own)
  std::string Name() const { return keys ? (dark ? "D" : "L") : "P" + std::to_string(pad_style); }
};

// The player slots' pictures (PlayerGlyph), under g.mutex: a look and the
// prompt glyph it shows (a disc glyph or a menu one; 0 = not lent yet), when
// it was last asked for (the least recently asked is lent out next).
struct PlayerSlot {
  std::string key;  // Look::Name() + the glyph: what the slot holds
  Look look;
  char16_t glyph = 0;
  uint64_t asked = 0;
};
std::array<PlayerSlot, kPlayerSlots> g_player_slot;

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
    case Kind::kSpin:
      // Just the circling arrow (2026-10-15): with the stick picture before it
      // ("¹¼" = WASD + arrow) it says "circle", the original's gesture. The
      // Spin key only spins with Simple Spinning (gameplay/game_mods.h), and
      // then the spin rows get a line of their own with it (SpinRows below).
      return "spin";
    case Kind::kSlot:
    case Kind::kPlayerSlot:
      break;  // (Describe answers for slots itself)
  }
  return {};
}

// What a page should show for the device in use: "" = the game's own
// picture; keys: "L key 32" / "D key 32" (the set's letter first); a
// controller set: "P <set> <button>".
std::string Describe(const Prompt& p, const kbm::Bindings& b, bool keyboard, bool dark, int pad_style) {
  if (p.kind == Kind::kPlayerSlot) {  // one player's picture of a glyph, in their look
    const PlayerSlot& slot = g_player_slot[size_t(p.character - kFirstPlayerSlot)];
    for (size_t k = 0; slot.glyph && k < kDiscPrompts + 6; ++k) {  // the disc's glyphs + the menu ones
      if (kBasePrompts[k].character != slot.glyph) continue;
      const std::string d = Describe(kBasePrompts[k], b, slot.look.keys, slot.look.dark, slot.look.pad_style);
      if (!d.empty()) return d;
      // "" = the game's own picture, which for a slot (a copy of the A page)
      // would be the A: the game's 360 picture of THIS button instead (the
      // playtest's "A Back / A Select" for a player with a 360-style pad).
      const int button = PadButtonOf(kBasePrompts[k].source);
      return button < 0 ? std::string() : "O " + std::to_string(button);
    }
    return {};
  }
  if (p.kind == Kind::kSlot) {  // the Options rows and card, whatever the device in use
    const std::string& what = g_slots[size_t(p.character - kFirstSlot)].what;
    if (what.size() < 3) return {};
    // A big slot: drawn stretched for its narrowed glyph ("W ", see Draw).
    const size_t slot = size_t(p.character - kFirstSlot);
    const std::string wide = slot >= kFirstBig && slot < kFirstCard ? "W " : "";
    if (what[0] == 'K') return wide + (dark ? "D " : "L ") + "key " + what.substr(2);
    // A controller button: the style's picture, or the game's own ("O").
    return wide + (pad_style == kOriginalStyle ? "O " + what.substr(2)
                                               : "P " + std::to_string(pad_style) + " " + what.substr(2));
  }
  if (keyboard) {
    const std::string keys = DescribeKeys(p, b);
    if (keys.empty()) return keys;
    return std::string(&p == &kWorldB ? "C " : "") + (dark ? "D " : "L ") + keys;
  }
  const int button = PadButtonOf(p.source);
  if (pad_style == kOriginalStyle || button < 0) return {};
  return "P " + std::to_string(pad_style) + " " + std::to_string(button);
}

// THE SIZE OF A PICTURE (playtest 2026-10-15: every replacement looked
// bigger than the game's own buttons). Each page's original picture covers
// only part of the page: the 360 face buttons 49 of 64 pixels (x / y 7-55),
// the sticks 93 x 72 of their 128 x 128 page, the world's B nearly all of
// its 64. A replacement is cut to its own drawn part (Trim) and fitted, its
// shape kept, into the box the ORIGINAL covers (GuestPage::ink, measured
// from the disc's page): drawn part to drawn part, so the sizes match.
struct Box {
  int x = 0, y = 0, w = 0, h = 0;  // picture pixels, rows top first
};

// `pic` fitted into `box` (centred, shape kept), drawn over `page`.
void Fit(Picture& page, const Picture& pic, const Box& box) {
  if (pic.empty() || box.w <= 0 || box.h <= 0) return;
  const float k = std::min(float(box.w) / float(pic.width), float(box.h) / float(pic.height));
  const uint32_t w = std::max(1u, uint32_t(std::lround(pic.width * k)));
  const uint32_t h = std::max(1u, uint32_t(std::lround(pic.height * k)));
  Over(page, Resize(pic, w, h), box.x + (box.w - int(w)) / 2, box.y + (box.h - int(h)) / 2);
}

// The fonts as the disc has them (read once fonts are loaded; State below,
// under its mutex: Draw runs inside OnFrameEnd).
const std::vector<DiscFont>& DiscFonts();

// Draws what Describe() described, at the page's size.
Picture Draw(const std::string& described, uint32_t width, uint32_t height, Box ink) {
  Picture page;
  page.width = width;
  page.height = height;
  page.rgba.assign(size_t(width) * height * 4, 0);
  if (described.size() < 2) return page;
  if (described[0] == 'W') {
    // A big key slot: drawn at its shape into kBigSlotScale of the box's
    // width, then stretched across the whole box: the font squeezes this
    // page into kBigSlotScale of the stick's width (its record, narrowed by
    // the font patch), which gives the picture its shape back.
    const Box narrow{ink.x, ink.y, int(float(ink.w) * kBigSlotScale), ink.h};
    const Picture drawn = Draw(described.substr(2), width, height, narrow);
    for (uint32_t y = 0; y < height; ++y) {
      for (int x = ink.x; x < int(width); ++x) {
        const int from = ink.x + int(float(x - ink.x) * kBigSlotScale);
        std::memcpy(&page.rgba[4 * (size_t(y) * width + uint32_t(x))],
                    &drawn.rgba[4 * (size_t(y) * width + uint32_t(from))], 4);
      }
    }
    return page;
  }
  if (described[0] == 'C') {
    // The world's B with a key: a whole square key over the coin's square
    // face (THE BUTTON'S SHAPE), which shows the picture from 0.046 to 0.954
    // of its width: the key fills that.
    const int margin = int(std::lround((0.5f - kCoinWindow) * float(width)));
    const Box face{margin, margin, int(width) - 2 * margin, int(height) - 2 * margin};
    return Draw(described.substr(2), width, height, face);
  }
  if (described[0] == 'J') {
    // THE JACK SYMBOL (a stunned titan nobody is close enough to jack yet):
    // our own picture over the coin's square face, like a key ('C').
    const int margin = int(std::lround((0.5f - kCoinWindow) * float(width)));
    const Box face{margin, margin, int(width) - 2 * margin, int(height) - 2 * margin};
    Fit(page, Trim(Asset("../icons", "jack.png"), kTrimAll), face);
    return page;
  }
  if (described[0] == 'O') {
    // A controller button as the game draws it (its 360 picture from the
    // disc's font, the page of that button's glyph), fitted to the page with
    // its shape kept (a stick's picture is the top 3/4 of a 128 x 128 page).
    const int button = std::atoi(described.c_str() + 2);
    const std::vector<DiscFont>& disc = DiscFonts();
    if (button < 0 || button >= kPadButtons || disc.empty()) return page;
    const auto at = disc[0].page_of.find(kButtonGlyphs[button]);
    if (at == disc[0].page_of.end()) return page;
    const DiscPage& dp = disc[0].pages.at(at->second);
    const bool stick = button == kPadLeftStick || button == kPadRightStick;
    Picture pic;
    pic.width = dp.width;
    pic.height = stick ? dp.height * 3 / 4 : dp.height;
    pic.rgba.resize(size_t(pic.width) * pic.height * 4);
    for (uint32_t i = 0; i < pic.width * pic.height; ++i) {
      const uint8_t* bgra = &dp.palette[4 * size_t(std::min<uint32_t>(dp.indices[i], dp.colours - 1))];
      pic.rgba[4 * size_t(i) + 0] = bgra[2];
      pic.rgba[4 * size_t(i) + 1] = bgra[1];
      pic.rgba[4 * size_t(i) + 2] = bgra[0];
      pic.rgba[4 * size_t(i) + 3] = bgra[3];
    }
    Fit(page, Trim(pic, kTrimAll), ink);
    return page;
  }
  if (described[0] == 'P') {
    // A controller button: Xelu's picture over the square part of the page
    // (a stick's page is 128 x 128 with 96 rows used), cut like the keys.
    int style = 0, button = 0;
    std::sscanf(described.c_str() + 2, "%d %d", &style, &button);
    if (style < 0 || style >= int(std::size(kPadSets)) || button < 0 || button >= kPadButtons) return page;
    const PadSet& set = kPadSets[style];
    const Picture& pic = Asset(set.folder, std::string(set.files[button]) + ".png");
    if (pic.empty()) return page;
    Fit(page, Trim(pic, kTrimAll), ink);
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
    // One key where the 360 button picture was.
    Fit(page, KeyPicture(keys[0], 100, set), ink);
  } else if (kind == "stick" && keys.size() == 4) {
    // Up above left / down / right, the usual WASD shape, where the stick
    // picture was.
    const uint32_t size = uint32_t(std::min(ink.w / 3, ink.h / 2));
    const int x0 = ink.x + (ink.w - 3 * int(size)) / 2, y0 = ink.y + (ink.h - 2 * int(size)) / 2;
    // Each key in its cell, a little apart (their empty borders are cut off).
    auto cell = [&](int cx, int cy) {
      const int gap = std::max(1, int(size) / 16);
      return Box{x0 + cx * int(size) + gap, y0 + cy * int(size) + gap, int(size) - 2 * gap, int(size) - 2 * gap};
    };
    Fit(page, KeyPicture(keys[0], 100, set), cell(1, 0));  // up
    Fit(page, KeyPicture(keys[2], 100, set), cell(0, 1));  // left
    Fit(page, KeyPicture(keys[1], 100, set), cell(1, 1));  // down
    Fit(page, KeyPicture(keys[3], 100, set), cell(2, 1));  // right
  } else if (kind == "spin" && keys.empty()) {
    // Xelu's circling arrow alone, STRETCHED to twice its width over the
    // whole "rotate" page (128 x 64): while keys are shown the font draws this
    // page into a square (NarrowRotate below: the round arrow's width, no gap
    // after it), which squeezes it back round (playtest, 2026-10-15: centred
    // at full width it left a big gap).
    const Picture arrow = Trim(Asset(kArrowFolder, "Directional_Arrow_Full_Circle.png"), kTrimAll);
    Picture wide;
    wide.width = arrow.width * 2;
    wide.height = arrow.height;
    wide.rgba.resize(size_t(wide.width) * wide.height * 4);
    for (uint32_t y = 0; y < wide.height; ++y) {
      for (uint32_t x = 0; x < wide.width; ++x) {
        std::memcpy(&wide.rgba[4 * (size_t(y) * wide.width + x)], &arrow.rgba[4 * (size_t(y) * arrow.width + x / 2)], 4);
      }
    }
    Fit(page, wide, ink);
  } else if (kind == "spin" && keys.size() == 1) {
    // The Spin key on the left half, Xelu's circling arrow on the right
    // (the "rotate" picture is 128 x 64: two squares).
    const int half = ink.w / 2;
    Fit(page, KeyPicture(keys[0], 100, set), Box{ink.x, ink.y, half, ink.h});
    Fit(page, Trim(Asset(kArrowFolder, "Directional_Arrow_Full_Circle.png"), kTrimAll),
        Box{ink.x + half + half / 16, ink.y + ink.h / 16, half * 7 / 8, ink.h * 7 / 8});
  }
  return page;
}

// --- The pages in the game's memory -----------------------------------------------

struct GuestPage {
  const Prompt* prompt = nullptr;
  uint32_t font = 0, texture = 0, xn = 0;  // to check it's still the same page
  uint32_t index_fetch1 = 0;               // the D3D texture's fetch word 1 (address)
  uint32_t width = 0, height = 0, used_height = 0;
  Box ink;  // where the original picture is drawn (THE SIZE OF A PICTURE)
  uint32_t index_address = 0, row_pitch = 0;
  uint8_t index_xor = 0;     // the endian swap of single bytes (address ^ this)
  bool flipped = false;      // guest row y = disc row height - 1 - y
  uint32_t palette_address = 0;
  int palette_byte[4] = {};  // where B, G, R, A of an entry sit in its 4 guest bytes
  std::vector<uint8_t> original_rows;     // the guest's index rows (row_pitch x height)
  std::vector<uint8_t> original_palette;  // the guest's 1 KB palette
  std::string shown;                      // Describe() of what's in memory ("" = original)
  std::string name;                       // "Titans_Small A" (log)
  int world_player = -1;                  // a copy of the world's B: whose (0-3), 4 = the jack symbol; -1 = the game's own
};

struct State {
  std::mutex mutex;
  std::vector<uint32_t> fonts_loaded;  // from the loader hook, not checked yet
  // THE WORLD'S B (2026-10-15): the red B button floating over a save totem,
  // a stunned titan, things to use (texture FE_360_b_button_64.tga in
  // package bca1bc0b, a 64 x 64 8-bit palettized picture like the prompt
  // pages). Textures as the texture loader builds them, not checked yet; the
  // disc's picture; the texture's vtable (to notice it was freed).
  std::vector<uint32_t> textures_loaded;
  bool world_read = false;
  std::optional<DiscPage> world_disc;
  uint32_t world_vtable = 0;
  std::vector<DiscFont> disc;
  bool disc_read = false;
  std::vector<GuestPage> pages;
  // Cache: Describe() + size -> the 256-colour picture.
  std::map<std::string, Paletted> drawn;
  // What the pages show (to notice a change cheaply).
  uint32_t revision = UINT32_MAX;
  std::string look;  // player 1's Look::Name() (the game's own glyphs), to notice a change
} g;
std::atomic<bool> g_keys_shown{false};  // KeysShown()
const std::vector<DiscFont>& DiscFonts() { return g.disc; }

// steady_clock ns of the last controller input, per socket (= per player in
// play; outside play every device answers on socket 0 = player 1).
std::array<std::atomic<int64_t>, 4> g_pad_activity{};
// The game's fonts in memory -> their names (mouse_menus.h measures texts).
std::mutex g_font_names_mutex;
std::map<uint32_t, std::string> g_font_names;
// A font with the menu glyphs was found (our font patch served and checked):
// only then may the prompt bar use them (else they'd draw nothing).
std::atomic<bool> g_menu_glyphs{false};
// The small font's player slots were found (PlayerGlyph lends them).
std::atomic<bool> g_player_slots{false};

// PLAYER p's LOOK (0-3): keys or the controller's buttons, and which set.
// p's devices = what answers on socket p (input/players.h): the keyboard if
// it plays as p, p's controller(s); of those, the one used last (the
// keyboard's last key against p's last controller input). Player 1's look =
// the game's own glyphs (shared texts: hints, tutorials, the message bar).
// Outside play every device answers as player 1, so any device counts there.
// Each player's Controller Icons setting (0 Automatic .. 5 Switch, as
// --controller_icons): player 1's = that flag, players 2-4's = controls.toml's
// icons.player2-4 (bindings.h Settings::pad_icons; the Players screen sets
// them), copied out of the driver's bindings whenever those change (a copy
// of all the bindings is too dear for every LookOf).
std::array<std::atomic<int>, 4> g_pad_icons{};
std::atomic<uint32_t> g_pad_icons_revision{UINT32_MAX};
int PadIconsSettingOf(uint32_t p) {
  if (p == 0 || p >= 4) return int(REXCVAR_GET(controller_icons));
  if (const kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get()) {
    const uint32_t revision = driver->bindings_revision();
    if (g_pad_icons_revision.exchange(revision) != revision) {
      const kbm::Bindings b = driver->bindings();
      for (size_t q = 1; q < 4; ++q) g_pad_icons[q] = b.settings.pad_icons[q];
    }
  }
  return g_pad_icons[p].load();
}

Look LookOf(uint32_t p) {
  Look l;
  const kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  const kbm::PlayerAssignment* assignment = kbm::PlayerAssignment::Get();
  const int32_t setting = REXCVAR_GET(button_prompts);
  l.dark = REXCVAR_GET(keyboard_icons) == 1;
  const bool keyboard = driver && assignment && assignment->KeyboardAnswersFor(p);
  if (driver && setting != 2 && p < g_pad_activity.size()) {
    l.keys = setting == 1 || (keyboard && driver->last_key_down() >= g_pad_activity[p].load());
  }
  // The controller's style: chosen, or (Automatic) the maker and model of p's
  // controller (PadKeyFor socket p).
  const int32_t pad_setting = PadIconsSettingOf(p);
  if (pad_setting >= 2 && pad_setting < 2 + int(std::size(kPadSets))) {
    l.pad_style = pad_setting - 2;
  } else if (pad_setting == 0 && assignment) {
    l.pad_style = StyleOfPad(assignment->PadKeyFor(p));
  }
  return l;
}

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

// Checks one prompt texture of the game against its disc picture `dp` and
// records where its pixels and palette are (CheckFont, CheckWorldTexture).
// `font` = the font it's a page of (0: a texture of its own). False = not as
// expected (logged; left alone).
bool MakePage(uint8_t* base, uint32_t font, uint32_t texture, const Prompt* prompt, const DiscPage& dp,
              const std::string& name, GuestPage* out, bool quiet = false) {
  GuestPage& gp = *out;
  gp.prompt = prompt;
  gp.name = name;
  gp.font = font;
  gp.texture = texture;
  gp.xn = gp.texture ? Load32(base, gp.texture + kTexturePddi) : 0;
  const uint32_t d3d = gp.xn ? Load32(base, gp.xn + kXnD3DTexture) : 0;
  if (!d3d || Load32(base, gp.xn + kXnPaletteMode) != 1) {
    if (!quiet) REXLOG_WARN("Prompt pictures: {} isn't a palettized texture: left alone", gp.name);
    return false;
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
    if (!quiet) REXLOG_WARN("Prompt pictures: {} isn't laid out as expected (fetch {:08X} {:08X} {:08X}, "
                "palette {:08X} {:08X}): left alone",
                gp.name, f[0], f[1], f[2], pf[0], pf[1]);
    return false;
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
    if (!quiet) REXLOG_WARN("Prompt pictures: {}'s pixels aren't the disc's: left alone", gp.name);
    return false;
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
    if (!quiet) REXLOG_WARN("Prompt pictures: {}'s palette isn't the disc's: left alone", gp.name);
    return false;
  }
  // The part of the page the glyph shows: the stick pictures use the
  // top 3/4 (their glyph's v starts at 0.25), every other the whole page.
  gp.used_height = prompt->kind == Kind::kStick ? gp.height * 3 / 4 : gp.height;
  // Where the original picture is drawn (THE SIZE OF A PICTURE): the disc
  // page's opaque part, inside the rows the glyph shows.
  {
    int x0 = int(gp.width), y0 = int(gp.used_height), x1 = -1, y1 = -1;
    for (uint32_t y = 0; y < gp.used_height && y < dp.height; ++y) {
      for (uint32_t x = 0; x < gp.width && x < dp.width; ++x) {
        const uint8_t index = dp.indices[size_t(y) * dp.width + x];
        if (index >= dp.colours || dp.palette[4 * size_t(index) + 3] <= 16) continue;
        x0 = std::min(x0, int(x));
        y0 = std::min(y0, int(y));
        x1 = std::max(x1, int(x));
        y1 = std::max(y1, int(y));
      }
    }
    gp.ink = x1 < 0 ? Box{0, 0, int(gp.width), int(gp.used_height)} : Box{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    // A BIG key slot (a copy of the stick's page): a key at the full stick
    // size was too big (playtest), an A-sized one too small: kBigSlotScale
    // of the stick's height, centred on it (the stick sits on the line the
    // same way, so the key stays level with its row). The full width stays:
    // the font draws this page into kBigSlotScale of the stick's width (the
    // font patch narrows the record, no gap after the key) and the key is
    // drawn stretched to match ("W" in Describe / Draw).
    const size_t slot = prompt && prompt->kind == Kind::kSlot ? size_t(prompt->character - kFirstSlot) : 0;
    if (prompt && prompt->kind == Kind::kSlot && slot >= kFirstBig && slot < kFirstCard) {
      const int h = int(float(gp.ink.h) * kBigSlotScale);
      gp.ink = Box{gp.ink.x, gp.ink.y + (gp.ink.h - h) / 2, gp.ink.w, h};
    }
    // A CARD slot: the A's box, a little larger: the page's width and
    // kCardSlotRows rows, centred on the A's circle.
    if (prompt && prompt->kind == Kind::kSlot && slot >= kFirstCard) {
      const int rows = std::min(kCardSlotRows, int(gp.used_height));
      const int top = std::clamp(gp.ink.y + gp.ink.h / 2 - rows / 2, 0, int(gp.used_height) - rows);
      gp.ink = Box{0, top, int(gp.width), rows};
    }
  }
  gp.original_rows.resize(size_t(gp.row_pitch) * gp.height);
  for (size_t i = 0; i < gp.original_rows.size(); ++i) {
    gp.original_rows[i] = *GuestPtr(base, gp.index_address + uint32_t(i));
  }
  gp.original_palette.assign(raw, raw + 1024);
  return true;
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
    const bool large = IsLargeFont(disc.page_count);
    const bool patched = page_count == disc.page_count + AddedPrompts(large);
    if (page_count != disc.page_count && !patched) continue;
    std::vector<GuestPage> found;
    const std::vector<const Prompt*> prompts = PromptsFor(large);
    for (size_t k = 0; k < prompts.size(); ++k) {
      const Prompt* prompt = prompts[k];
      const auto source = disc.page_of.find(prompt->source);
      if (source == disc.page_of.end() || (k >= kDiscPrompts && !patched)) continue;
      // Each page must hold its source's picture (an added page = a copy).
      const DiscPage& dp = disc.pages.at(source->second);
      const uint32_t index =
          k < kDiscPrompts ? source->second : disc.page_count + uint32_t(k - kDiscPrompts);
      GuestPage gp;
      if (!MakePage(base, font, Load32(base, array + 4 * index), prompt, dp, disc.name + " " + prompt->name, &gp)) {
        continue;
      }
      found.push_back(std::move(gp));
    }
    REXLOG_INFO("Prompt pictures: {} found ({} prompt pictures checked against the disc{})",
                disc.name, found.size(), patched ? ", menu glyphs included" : "");
    for (GuestPage& gp : found) g.pages.push_back(std::move(gp));
    if (patched) g_menu_glyphs = true;
    if (patched && !large) g_player_slots = true;
    {
      std::lock_guard<std::mutex> names(g_font_names_mutex);
      g_font_names[font] = disc.name;
    }
    return;
  }
}

// Is the page still the one we checked (the font not unloaded / replaced)?
bool StillThere(uint8_t* base, const GuestPage& gp) {
  if (gp.font) {
    if (Load32(base, gp.font) != kTextureFontVtable) return false;
    const uint32_t array = Load32(base, gp.font + kFontPages);
    bool listed = false;
    for (uint32_t i = 0, n = Load32(base, gp.font + kFontPageCount); i < n && i < 256; ++i) {
      listed = listed || Load32(base, array + 4 * i) == gp.texture;
    }
    if (!listed) return false;
  } else if (Load32(base, gp.texture) != g.world_vtable) {
    return false;  // (a texture of its own: freed / reused for something else)
  }
  if (Load32(base, gp.texture + kTexturePddi) != gp.xn) return false;
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
        std::to_string(gp.used_height) + " ink " + std::to_string(gp.ink.x) + "," + std::to_string(gp.ink.y) +
        "," + std::to_string(gp.ink.w) + "," + std::to_string(gp.ink.h);
    auto it = g.drawn.find(key);
    if (it == g.drawn.end()) {
      it = g.drawn.emplace(key, Quantize(Draw(what, gp.width, gp.height, gp.ink))).first;
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

// A texture the loader built: the world's B? Only a 64 x 64 palettized one
// is looked at closely (MakePage compares every pixel with the disc's),
// quietly: nearly every texture is something else. Once found, it's a page
// like the prompts' (drawn below with the B button's / Jack's picture).
void CheckWorldTexture(uint8_t* base, uint32_t texture) {
  if (!g.world_disc || !texture) return;
  const uint32_t xn = Load32(base, texture + kTexturePddi);
  const uint32_t d3d = xn ? Load32(base, xn + kXnD3DTexture) : 0;
  if (!d3d || Load32(base, xn + kXnPaletteMode) != 1) return;
  const uint32_t size = Load32(base, d3d + kD3DFetch + 8);
  if ((size & 0x1FFF) + 1 != g.world_disc->width || ((size >> 13) & 0x1FFF) + 1 != g.world_disc->height) return;
  int world_found = 0;  // world B textures already known (the original first, then the copies)
  for (const GuestPage& gp : g.pages) {
    if (gp.texture == texture) return;  // known already
    if (!gp.font && StillThere(base, gp)) ++world_found;
  }
  GuestPage gp;
  if (!MakePage(base, 0, texture, &kWorldB, *g.world_disc, "world B", &gp, /*quiet=*/true)) return;
  // The palette's byte order: this red ball's palette has identical blue and
  // green columns, so MakePage's check can't tell those two apart (our
  // pictures would come out with blue and green swapped). The font pages
  // are k_8_8_8_8 palettes laid out the same way: their order.
  for (const GuestPage& font_page : g.pages) {
    if (font_page.font) {
      std::copy(std::begin(font_page.palette_byte), std::end(font_page.palette_byte), std::begin(gp.palette_byte));
      break;
    }
  }
  // UPSIDE DOWN: the game DRAWS this texture upside down (a B looks the same
  // either way up, so nobody could tell): an "A" written the disc's way came
  // out as "∀" (playtest photo). So the pictures go in the other way up.
  gp.flipped = !gp.flipped;
  g.world_vtable = Load32(base, texture);
  gp.world_player = world_found - 1;  // the original, players 1-4's copies, the jack symbol (PatchWorldButton)
  if (gp.world_player > kJackSymbol) return;
  if (gp.world_player == kJackSymbol) gp.name = "world jack symbol";
  else if (gp.world_player >= 0) gp.name = "world B player " + std::to_string(gp.world_player + 1);
  REXLOG_INFO("Prompt pictures: the world's B button found (texture {:08X}){}", texture,
              gp.world_player == kJackSymbol ? " = the jack symbol's copy"
              : gp.world_player >= 0         ? " = player " + std::to_string(gp.world_player + 1) + "'s copy"
                                             : "");
  g.pages.push_back(std::move(gp));
  g.revision = UINT32_MAX;  // drawn at once
}

// THE ROTATE GLYPH'S WIDTH (2026-10-15). The "rotate" picture ("¼", after
// the stick in every spin prompt) is 128 x 64: the game's own arrow is that
// wide. The keyboard's version is Xelu's ROUND arrow, drawn in the page's
// left square; at full width the empty right half left a gap before the next
// character. While keys are shown, the font's record for "¼" is made a
// square: width and advance halved (the gap with them). A picture glyph is
// drawn as its WHOLE page into that width (the texture's u range is ignored:
// halving it changed nothing, measured), so the keys' page holds the arrow
// stretched twice as wide. With the controller's buttons the record gets its
// numbers back (the game's own wide arrow). Records: TextureFont
// +72 count, +76 the records (after a 20-byte header; 40 bytes each, as the file's 0x22001 chunk:
// +28 width, +32 advance, +36 character as a u16), big-endian.
constexpr uint32_t kFontGlyphCount = 72, kFontGlyphs = 76, kGlyphSize = 40;
// The records begin 20 bytes after +76's pointer (measured: Titans_Small's
// first record, the newline's, at +20; then one every 40 bytes, in the order
// of the sorted character list at +84).
constexpr uint32_t kGlyphHeader = 20;
constexpr uint32_t kGlyphWidth = 28, kGlyphAdvance = 32, kGlyphChar = 36;
struct RotateGlyph {
  uint32_t record = 0;
  uint32_t width = 0, advance = 0;  // the game's own, as raw words
};
// By font and character (the game's ¼ and players 2-4's copies of it); main
// thread (g.mutex).
std::map<std::pair<uint32_t, char16_t>, RotateGlyph> g_rotate;

float WordFloat(uint32_t w) { float f; std::memcpy(&f, &w, 4); return f; }
uint32_t FloatWord(float f) { uint32_t w; std::memcpy(&w, &f, 4); return w; }

void NarrowRotate(uint8_t* base, uint32_t font, char16_t character, bool narrow) {
  auto it = g_rotate.find({font, character});
  if (it == g_rotate.end()) {
    RotateGlyph r;
    const uint32_t count = Load32(base, font + kFontGlyphCount), records = Load32(base, font + kFontGlyphs);
    for (uint32_t i = 0; records && i < count && i < 1024; ++i) {
      const uint32_t at = records + kGlyphHeader + kGlyphSize * i;
      // The loaded record holds its character as a u16 in the word's top half
      // (measured: "00 20 00 00" for the space).
      if (Load32(base, at + kGlyphChar) >> 16 != character) continue;
      r = {at, Load32(base, at + kGlyphWidth), Load32(base, at + kGlyphAdvance)};
      break;
    }
    if (!r.record) {
      REXLOG_WARN("Prompt pictures: the rotate glyph {:04X} wasn't found in font {:08X}", uint32_t(character), font);
    }
    it = g_rotate.emplace(std::make_pair(font, character), r).first;
  }
  const RotateGlyph& r = it->second;
  if (!r.record) return;
  const float width = WordFloat(r.width), advance = WordFloat(r.advance);
  Store32(base, r.record + kGlyphWidth, narrow ? FloatWord(width / 2) : r.width);
  Store32(base, r.record + kGlyphAdvance, narrow ? FloatWord(advance - width / 2) : r.advance);
}

// THE FLOATING B'S OWNER (2026-10-17). The B over a save totem, a stunned
// titan, a thing to use is ONE texture shared by every such icon, so it can
// only show one player's picture at a time: the player it appeared for.
// The icons are CIconBehaviour (vtable 0x8203248C; +28 its object, +116 its
// drawable, +120 shown), switched by sub_82179130(icon, show). When an icon
// turns ON, the playing player NEAREST to its object (their Crash, or the
// titan they ride) owns it; the B shows the look of the owner of the icon
// shown FIRST among those still up ("whoever comes near first": player 4 on
// an Xbox pad walks up to a totem = Xbox B, even if player 1 on the keyboard
// arrives a moment later). No icon up = player 1's look.
constexpr uint32_t kIconVtable = 0x8203248C;
constexpr uint32_t kIconObject = 28, kIconDrawable = 116, kIconShown = 120;
struct IconOwner {
  uint32_t icon = 0;
  int player = 0;
  // How far its owner stood from its object when it appeared: the game's own
  // range for it, as near as we can tell (side-by-side coins of an icon the
  // game keeps no "who may use it" list for, e.g. a stunned titan's B).
  float range = 0;
};
std::mutex g_icons_mutex;
std::vector<IconOwner> g_icons;  // in the order they were shown

// Player p's body (the titan they ride, else their Crash) position.
bool BodyAt(uint8_t* base, int p, float out[3]) {
  const uint32_t titan = more_players::TitanOfPlayer(p);
  const uint32_t actor = titan ? titan : more_players::CharacterOfPlayer(p);
  if (!actor) return false;
  const uint32_t matrix = Load32(base, actor + 44);  // world matrix, position at +48
  if (matrix < 0x40000000) return false;
  for (int i = 0; i < 3; ++i) {
    const uint32_t w = Load32(base, matrix + 48 + 4 * i);
    std::memcpy(&out[i], &w, 4);
  }
  return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

// Who the floating B is for now (OnFrameEnd; drops icons that went away).
int WorldButtonOwner(uint8_t* base) {
  std::lock_guard<std::mutex> lock(g_icons_mutex);
  std::erase_if(g_icons, [base](const IconOwner& o) {
    return Load32(base, o.icon) != kIconVtable || !*GuestPtr(base, o.icon + kIconShown);
  });
  return g_icons.empty() ? 0 : g_icons.front().player;
}

void RefreshWorldXn();  // FORGET THE LAST MATERIAL (below)

// Once per game frame (main thread).
void OnFrameEnd(void*) {
  auto* memory = rex::system::kernel_memory();
  if (!memory) return;
  uint8_t* base = memory->virtual_membase();
  kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  std::lock_guard<std::mutex> lock(g.mutex);
  WorldButtonRestore(base);

  if (!g.fonts_loaded.empty()) {
    if (!g.disc_read) {
      g.disc_read = true;
      g.disc = ReadDiscFonts();
    }
    for (uint32_t font : g.fonts_loaded) {
      // A new font at this address: its records are new too.
      std::erase_if(g_rotate, [font](const auto& entry) { return entry.first.first == font; });
      CheckFont(base, font);
    }
    g.fonts_loaded.clear();
    // New pages show the game's own pictures: draw them below even if
    // nothing else changed (the second font arrives a frame or two after the
    // first; without this its pages kept the 360 buttons until the next
    // device switch).
    g.revision = UINT32_MAX;
  }
  if (!g.textures_loaded.empty()) {
    if (!g.world_read) {
      g.world_read = true;
      g.world_disc = ReadWorldTexture(kWorldTexture);
    }
    for (const uint32_t texture : g.textures_loaded) CheckWorldTexture(base, texture);
    g.textures_loaded.clear();
    RefreshWorldXn();
  }
  if (g.pages.empty()) return;

  // Player 1's look: the game's own glyphs (every shared text). Players
  // 2-4's pictures live in the player slots, each with its own look.
  const Look l = LookOf(0);
  // The game's own B: the look of the first icon's owner (the fallback for
  // a coin that WorldButtonSwap can't match to an icon). Each player's copy
  // is painted below and drawn on the coins that player owns.
  const Look world = LookOf(uint32_t(WorldButtonOwner(base)));
  const uint32_t revision = driver ? driver->bindings_revision() : 0;
  g_keys_shown = l.keys;
  std::string look = l.Name() + " " + world.Name();
  for (uint32_t p = 0; p < 4; ++p) look += " " + LookOf(p).Name();  // the B copies' looks
  if (revision == g.revision && look == g.look && g_slot_drawn == g_slot_revision) return;
  g_slot_drawn = g_slot_revision;
  if (look != g.look || g.revision == UINT32_MAX) {
    REXLOG_INFO("Prompt pictures: {}", l.keys ? (l.dark ? "keyboard and mouse keys (dark)" : "keyboard and mouse keys")
                                       : l.pad_style == kOriginalStyle ? std::string("controller buttons (the game's own)")
                                       : std::string("controller buttons: ") + kPadSets[l.pad_style].name);
    if (world.Name() != l.Name()) REXLOG_INFO("Prompt pictures: the floating B in another player's look ({})", world.Name());
  }
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
    // The floating B: its owner's look (THE FLOATING B'S OWNER above).
    const Look own = gp.world_player >= 0 && gp.world_player < kJackSymbol ? LookOf(uint32_t(gp.world_player)) : Look{};
    const Look& use = gp.prompt != &kWorldB ? l : gp.world_player >= 0 ? own : world;
    // The jack symbol's copy: the same picture whatever anyone plays with.
    const std::string want = gp.prompt == &kWorldB && gp.world_player == kJackSymbol
                                 ? std::string("J jack")
                                 : Describe(*gp.prompt, bindings, use.keys, use.dark, use.pad_style);
    if (want != gp.shown) Show(base, gp, want);
    if (gp.font && gp.prompt->kind == Kind::kSpin) NarrowRotate(base, gp.font, gp.prompt->character, l.keys);
    ++i;
  }
  RefreshWorldXn();  // (a page may have gone)
}

// --- The font patch: six more prompt glyphs for the menus, and 16 key slots -------
// (prompt_icons.h "Menus"). In package\b4c85fe7.p3d, for each of the two fonts:
//   * the A, B, X, Y, LB, RB pictures' texture chunks (0x19000) copied,
//     renamed after the next page numbers ("..._texture_36" on), inserted
//     after the last page: the loader numbers pages in file order;
//   * the font's page count (the last u32 of its own data) + 6 (+ 16 key
//     slots since 2026-10-15: copies of the A picture, U+E100..E10F, + 4 big
//     ones, copies of the left stick's, U+E110..E113);
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
  const bool large = IsLargeFont(page_count);
  const std::vector<const Prompt*> prompts = PromptsFor(large);
  const size_t added_count = AddedPrompts(large);
  if (page_count + added_count > kMaxFontPages) return false;  // "64 PAGES"
  std::vector<std::vector<uint8_t>> records(added_count);
  for (uint32_t i = 0; i < glyph_count; ++i) {
    const uint8_t* r = &glyphs->data[4 + 40 * i];
    for (size_t k = 0; k < added_count; ++k) {
      if (pure3d::Le32(r + 36) == prompts[kDiscPrompts + k]->source) records[k].assign(r, r + 40);
    }
  }
  std::vector<pure3d::Node> added;
  for (size_t k = 0; k < added_count; ++k) {
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
    const uint32_t picture_page = page | 0x80000000u, character = prompts[kDiscPrompts + k]->character;
    for (int b = 0; b < 4; ++b) {
      r[b] = uint8_t(picture_page >> (8 * b));
      r[36 + b] = uint8_t(character >> (8 * b));
    }
    // A big key slot draws into kBigSlotScale of the stick's width (and
    // advances by that much less): no empty space after its key.
    if (k >= 6 + kFirstBig && k < 6 + kFirstCard) {
      float width_f, advance_f;
      const uint32_t w = pure3d::Le32(&r[28]), a = pure3d::Le32(&r[32]);
      std::memcpy(&width_f, &w, 4);
      std::memcpy(&advance_f, &a, 4);
      const float narrowed = width_f * kBigSlotScale, advance_n = advance_f - (width_f - narrowed);
      uint32_t wn, an;
      std::memcpy(&wn, &narrowed, 4);
      std::memcpy(&an, &advance_n, 4);
      for (int b = 0; b < 4; ++b) {
        r[28 + b] = uint8_t(wn >> (8 * b));
        r[32 + b] = uint8_t(an >> (8 * b));
      }
    }
    InsertSorted(*glyphs, 40, 36, r);
    // The picture list's entry.
    if (pictures && pictures->data.size() >= 4) {
      const uint32_t n = pure3d::NodeLe32(*pictures, 0);
      for (uint32_t i = 0; i < n && 4 + 8 * size_t(i + 1) <= pictures->data.size(); ++i) {
        if (pure3d::NodeLe32(*pictures, 4 + 8 * i) != prompts[kDiscPrompts + k]->source) continue;
        std::vector<uint8_t> entry(pictures->data.begin() + 4 + 8 * i,
                                   pictures->data.begin() + 4 + 8 * (i + 1));
        for (int b = 0; b < 4; ++b) entry[b] = uint8_t(character >> (8 * b));
        InsertSorted(*pictures, 8, 0, entry);
        break;
      }
    }
  }
  pure3d::SetNodeLe32(font, count_at, page_count + uint32_t(added_count));
  font.children.insert(font.children.begin() + std::ptrdiff_t(pages.back() + 1), added.begin(),
                       added.end());
  return true;
}

// THE BUTTON'S SHAPE: the coin's faces made square (one prim group:
// positions 0x10005 = u32 count + x y z floats, texture coordinates 0x10007
// = u32 count + u32 channel + u v floats). The texture projection is read
// from the coin itself (a rim corner's u / x and v / y).
bool SquareCoin(pure3d::Node& group) {
  pure3d::Node* positions = nullptr;
  pure3d::Node* uvs = nullptr;
  for (pure3d::Node& c : group.children) {
    if (c.id == kChunkPositions) positions = &c;
    if (c.id == kChunkUvs) uvs = &c;
  }
  if (!positions || !uvs || positions->data.size() < 4 || uvs->data.size() < 8) return false;
  const uint32_t n = pure3d::NodeLe32(*positions, 0);
  if (pure3d::NodeLe32(*uvs, 0) != n || positions->data.size() < 4 + 12 * size_t(n) ||
      uvs->data.size() < 8 + 8 * size_t(n)) {
    return false;
  }
  auto get = [](const pure3d::Node& node, size_t at) {
    float f;
    std::memcpy(&f, &node.data[at], 4);
    return f;
  };
  auto put = [](pure3d::Node& node, size_t at, float f) { std::memcpy(&node.data[at], &f, 4); };
  // The projection: from the rim corner furthest along x and along y.
  float ku = 0, kv = 0, radius = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const float x = get(*positions, 4 + 12 * i), y = get(*positions, 8 + 12 * i);
    const float u = get(*uvs, 8 + 8 * i), v = get(*uvs, 12 + 8 * i);
    if (std::abs(x) > 0.1f && std::abs(y) < 1e-3f) ku = (u - 0.5f) / x;
    if (std::abs(y) > 0.1f && std::abs(x) < 1e-3f) kv = (v - 0.5f) / y;
    radius = std::max(radius, std::hypot(x, y));
  }
  if (ku == 0 || kv == 0 || radius < 0.01f) return false;
  for (uint32_t i = 0; i < n; ++i) {
    float x = get(*positions, 4 + 12 * i), y = get(*positions, 8 + 12 * i);
    if (std::hypot(x, y) < 0.5f * radius) continue;  // a face's centre
    // Rim corners: on an axis = an edge's middle (stays); between = the corner.
    if (std::abs(x) > 1e-3f * radius && std::abs(y) > 1e-3f * radius) {
      x = std::copysign(radius, x);
      y = std::copysign(radius, y);
    }
    put(*positions, 4 + 12 * i, x);
    put(*positions, 8 + 12 * i, y);
    put(*uvs, 8 + 8 * i, 0.5f + ku * x);
    put(*uvs, 12 + 8 * i, 0.5f + kv * y);
  }
  return true;
}

// WORLD B: THE BUTTON'S MATERIAL (above): the material's alpha test on;
// THE BUTTON'S SHAPE: the B baked, the coin square. Package bca1bc0b.
bool PatchWorldButton(const pure3d::Bytes& original, pure3d::Bytes* patched) {
  pure3d::Chunk root;
  std::vector<pure3d::Chunk> top;
  if (!pure3d::Parse(original, &root, &top)) return false;
  pure3d::Bytes children;
  int done = 0;
  for (const pure3d::Chunk& c : top) {
    const std::string_view name = pure3d::NameOf(original, c);
    if (c.id == kChunkShader && name == kWorldMaterial) {
      pure3d::Node shader = pure3d::ToNode(original, c);
      for (pure3d::Node& param : shader.children) {
        // An int parameter: four-letter name, then a u32 (little-endian).
        if (param.id == kChunkShaderInt && param.data.size() >= 8 &&
            std::string_view(reinterpret_cast<const char*>(param.data.data()), 4) == "ATST") {
          pure3d::SetNodeLe32(param, 4, 1);
          ++done;
        }
      }
      const pure3d::Bytes bytes = pure3d::ToBytes(shader);
      children.insert(children.end(), bytes.begin(), bytes.end());
    } else if (c.id == kChunkTexture && name == kWorldTexture) {
      pure3d::Node texture = pure3d::ToNode(original, c);
      for (pure3d::Node& image : texture.children) {
        for (pure3d::Node& pixels : image.children) {
          if (pixels.id != kChunkImageData || pixels.data.size() < 4 + 18) continue;
          uint8_t* tga = pixels.data.data() + 4;
          const size_t colours = size_t(tga[5] | tga[6] << 8), at = 18 + tga[0];
          const uint32_t w = tga[12] | tga[13] << 8, h = tga[14] | tga[15] << 8;
          if (tga[1] != 1 || tga[7] != 32 || tga[16] != 8 || at + 4 * colours + size_t(w) * h > pixels.data.size() - 4) continue;
          if (BakeWorldButton(tga + at + 4 * colours, w, h, tga + at, colours)) ++done;
        }
      }
      const pure3d::Bytes bytes = pure3d::ToBytes(texture);
      children.insert(children.end(), bytes.begin(), bytes.end());
      // EACH PLAYER'S B (2026-10-17): four copies, "..._p1" to "_p4", right
      // after it. Nothing in the game uses them; the package loads them like
      // any texture, CheckWorldTexture finds them after the original (same
      // pixels) and paints each in its player's look. MEANT for each icon's
      // coin to be drawn with its owner's copy (WorldButtonSwap, through the
      // outfits' per-object texture swap), but that swap never sees the coin
      // yet (OnFrameEnd: drawn by Pure3D's render loop): not visible so far.
      // THE JACK SYMBOL (2026-10-24): a fifth copy, "_p5", painted with our
      // own picture (assets/icons/jack.png): what a stunned titan shows while
      // no player is close enough to jack it (WHO CAN JACK IT, below).
      const std::string base_name(name);
      for (int p = 1; p <= 5; ++p) {
        pure3d::Node copy = texture;
        const std::string dot = base_name.substr(base_name.rfind('.'));
        RenamePage(copy, base_name, base_name.substr(0, base_name.rfind('.')) + "_p" + std::to_string(p) + dot);
        const pure3d::Bytes copy_bytes = pure3d::ToBytes(copy);
        children.insert(children.end(), copy_bytes.begin(), copy_bytes.end());
      }
    } else if (c.id == kChunkMesh && name == kWorldMesh) {
      pure3d::Node mesh = pure3d::ToNode(original, c);
      for (pure3d::Node& group : mesh.children) {
        if (group.id == kChunkPrimGroup && SquareCoin(group)) ++done;
      }
      const pure3d::Bytes bytes = pure3d::ToBytes(mesh);
      children.insert(children.end(), bytes.begin(), bytes.end());
    } else {
      const pure3d::Bytes bytes = pure3d::Copy(original, c);
      children.insert(children.end(), bytes.begin(), bytes.end());
    }
  }
  if (done < 3) {
    REXLOG_WARN("Prompt pictures: {} isn't as expected: the world's B stays as it was", kWorldPackage);
    return false;
  }
  *patched = pure3d::WithChildren(original, root, children);
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
  // (bca1bc0b is a "joined_assets" package, type 9 in levels/GlobalPackages
  // .p3d: the path builder's category = type + 1 = 10, not the menus' 6.)
  data_patcher::Register("world button alpha test", kCategoryJoinedAssets, "bca1bc0b", &PatchWorldButton);
}

void Install() { pddi::AddFrameEndListener(&OnFrameEnd, nullptr); }

std::string FontNameAt(uint32_t font) {
  std::lock_guard<std::mutex> lock(g_font_names_mutex);
  const auto it = g_font_names.find(font);
  return it != g_font_names.end() ? it->second : std::string();
}

bool KeysShown() { return g_keys_shown.load(); }

namespace {
// Lends a slot to `what` (Slot): the one already holding it, else the least
// recently asked.
char16_t LendSlot(const std::string& what, SlotSize size) {
  std::lock_guard<std::mutex> lock(g.mutex);
  // Small slots 0-15 (A-sized), big ones 16-19 (stick-sized), card ones 20-25.
  const size_t first = size == SlotSize::kBig ? kFirstBig : size == SlotSize::kCard ? kFirstCard : 0;
  const size_t end = size == SlotSize::kBig ? kFirstCard : size == SlotSize::kCard ? kSlotCount : kFirstBig;
  size_t slot = first;
  for (size_t i = first; i < end; ++i) {
    if (g_slots[i].what == what) {
      slot = i;
      break;
    }
    if (g_slots[i].asked < g_slots[slot].asked) slot = i;  // least recently asked
  }
  if (g_slots[slot].what != what) {
    g_slots[slot].what = what;
    ++g_slot_revision;
  }
  g_slots[slot].asked = ++g_slot_clock;
  return char16_t(kFirstSlot + slot);
}
}  // namespace

// EACH PLAYER'S OWN PICTURES (2026-10-17). The game's prompt glyphs are one
// set of pages, so they show one device at a time: with players on
// different devices, whoever pressed something last repainted every prompt
// on screen, everyone's. Now the game's own glyphs follow PLAYER 1's devices
// only (shared texts), and a text that belongs to player 2-4 gets PLAYER
// SLOTS: copies of the A page holding that player's picture of the glyph
// (their keyboard key or their controller's button, in their look). A slot
// is lent per look and glyph, not per player: two players on PlayStation
// pads share their Circle. A player whose look is player 1's needs no slot
// (the game's own glyph shows the same picture). 10 slots (the 64-page
// limit, "64 PAGES"); the least recently asked is lent out next, and the
// texts using slots are rewritten every frame (FrontEndFrame / HudFrame), so
// a slot taken over by another picture is noticed at once.
namespace {
char16_t PlayerGlyph(const Look& look, char16_t glyph) {
  std::lock_guard<std::mutex> lock(g.mutex);
  const std::string key = look.Name() + ":" + std::to_string(uint32_t(glyph));
  size_t slot = 0;
  for (size_t i = 0; i < kPlayerSlots; ++i) {
    if (g_player_slot[i].key == key) {
      slot = i;
      break;
    }
    if (g_player_slot[i].asked < g_player_slot[slot].asked) slot = i;
  }
  PlayerSlot& ps = g_player_slot[slot];
  if (ps.key != key) {
    ps.key = key;
    ps.look = look;
    ps.glyph = glyph;
    ++g_slot_revision;  // drawn at the next frame end
  }
  ps.asked = ++g_slot_clock;
  return char16_t(kFirstPlayerSlot + slot);
}

// A prompt glyph a player slot can show: the disc's (but the rotate arrow,
// whose record the keys narrow) and the menu ones.
bool SlotGlyph(char16_t c) {
  for (size_t k = 0; k < kDiscPrompts + 6; ++k) {
    if (kBasePrompts[k].character == c) return kBasePrompts[k].kind != Kind::kSpin;
  }
  return false;
}

// What a player slot character stands for (its glyph), else `c` itself.
char16_t BaseGlyph(char16_t c) {
  if (c < kFirstPlayerSlot || c >= kFirstPlayerSlot + kPlayerSlots) return c;
  std::lock_guard<std::mutex> lock(g.mutex);
  const char16_t glyph = g_player_slot[c - kFirstPlayerSlot].glyph;
  return glyph ? glyph : c;
}

// A text as player p's (0-3): its prompt glyphs in p's pictures (player
// slots), or the game's own glyphs for player 1 / a player who looks like
// player 1 / no slots in the font.
std::u16string ForPlayer(std::u16string text, int p) {
  for (char16_t& c : text) c = BaseGlyph(c);
  if (p <= 0 || p > 3 || !g_player_slots.load(std::memory_order_relaxed)) return text;
  const Look look = LookOf(uint32_t(p));
  if (look.Name() == LookOf(0).Name()) return text;
  for (char16_t& c : text) {
    if (SlotGlyph(c)) c = PlayerGlyph(look, c);
  }
  return text;
}
}  // namespace

char16_t KeyGlyph(Input key, SlotSize size) {
  if (key == kbm::kNoInput || !g_menu_glyphs.load()) return 0;
  return LendSlot("K " + std::to_string(key), size);
}

std::u16string TextForPlayer(std::u16string text, int p) { return ForPlayer(std::move(text), p); }

char16_t ButtonGlyph(std::string_view pad, SlotSize size) {
  if (!g_menu_glyphs.load()) return 0;
  // The 360 control names of input/bindings.h (ActionInfo::pad).
  static const std::map<std::string_view, int> kNames = {
      {"A", kPadA},       {"B", kPadB},         {"X", kPadX},          {"Y", kPadY},
      {"Back", kPadBack}, {"Start", kPadStart}, {"RT", kPadRT},        {"RB", kPadRB},
      {"LT", kPadLT},     {"LB", kPadLB},       {"Left stick", kPadLeftStick},
      {"Left stick circles", kPadLeftStick},    {"Right stick", kPadRightStick},
  };
  const auto it = kNames.find(pad);
  return it == kNames.end() ? 0 : LendSlot("B " + std::to_string(it->second), size);
}

char16_t MenuGlyph(char16_t c) {
  if (!g_menu_glyphs.load(std::memory_order_relaxed)) return c;
  for (size_t k = kDiscPrompts; k < kDiscPrompts + 6; ++k) {  // the six menu glyphs
    if (kBasePrompts[k].source == c) return kBasePrompts[k].character;
  }
  return c;
}

// -----------------------------------------------------------------------------
// SPIN ROWS (2026-10-15). The game writes spins as "¹¼" (left stick + rotate
// arrow: circle the stick), in the Tutorials pages (InGame_Tutorial.pag, rows
// Text0-Text3), the hints in play (InGame.pag HintMessage) and the prompt
// beside the HUD (InGame_Announcement.pag AnnouncementText: dig spots). Their strings
// come straight from the text bank (they don't pass through SetString), so
// they're looked at every front-end frame. With Simple Spinning on, such a
// text gets a SECOND LINE that spins with the button: "¹¼" there becomes
// the Spin key (keyboard) or LB (controller), whichever the prompts show;
// the line starts with "OR" and the button is drawn as big as the stick (a
// big key slot); with Keep Spinning on, the Old Skool row (spin, then Light
// attack again and again: "¹¼,³,³,³ . . .") becomes "OR Hold <button>". A
// text with words gets "Or press <button>" (text-sized) instead. Simple Spinning off = the game's text, back as
// it was.
// -----------------------------------------------------------------------------
namespace {

struct SpunText {
  std::u16string original;  // the game's text
  std::u16string shown;     // what we set (original + our line)
};
std::map<uint32_t, SpunText> g_spun;  // by text element; game main thread

std::u16string ReadText(uint8_t* base, uint32_t text) {
  std::u16string value;
  const uint32_t string = text ? Load32(base, text + kTextString) : 0;
  for (uint32_t i = 0; string && i < 256; ++i) {
    const uint8_t* c = GuestPtr(base, string + 2 * i);
    const char16_t ch = char16_t(c[0] << 8 | c[1]);
    if (!ch) break;
    value.push_back(ch);
  }
  return value;
}

// The second line for a text with "¹¼" in it, or "" for none.
std::u16string SpinLine(const std::u16string& text) {
  if (!REXCVAR_GET(simple_spinning)) return {};
  // The button as big as the stick picture it stands for (a big slot) for
  // the picture rows; text-sized in a sentence.
  const bool words = std::any_of(text.begin(), text.end(), [](char16_t c) {
    return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z');
  });
  char16_t button = 0;
  if (KeysShown()) {
    if (const kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get()) {
      const Input key = FirstKey(driver->bindings(), kbm::kSpin);
      if (key != kbm::kNoInput) button = KeyGlyph(key, words ? SlotSize::kText : SlotSize::kBig);
    }
  } else {
    button = ButtonGlyph("LB", words ? SlotSize::kText : SlotSize::kBig);
  }
  if (!button) return {};
  if (words) return u"Or press " + std::u16string(1, button);
  // "OR" first: the two lines are two ways to do the same move (playtest).
  const std::u16string spin = u"\u00B9\u00BC";
  const bool old_skool = text.find(u"\u00B3,\u00B3,\u00B3") != std::u16string::npos;
  if (old_skool && REXCVAR_GET(keep_spinning)) return u"OR Hold " + std::u16string(1, button);
  std::u16string line = text;
  for (size_t at = line.find(spin); at != std::u16string::npos; at = line.find(spin, at + 1)) {
    line.replace(at, spin.size(), 1, button);
  }
  return u"OR " + line;
}

// One text: its spin line (above) and its prompt characters as player
// `player`'s (0-3; ForPlayer: the texts that belong to one player).
void SpinRow(PPCContext& ctx, uint8_t* base, uint32_t text, int player = 0) {
  if (!text) return;
  const std::u16string now = ReadText(base, text);
  auto it = g_spun.find(text);
  // Our own text from last time = its original is still the game's; anything
  // else = the game wrote a new one (in the game's characters; ForPlayer(.., 0)
  // only matters for a text of ours another player's copy wrote).
  const std::u16string original =
      it != g_spun.end() && now == it->second.shown ? it->second.original : ForPlayer(now, 0);
  const bool spin = original.find(u"\u00B9\u00BC") != std::u16string::npos;
  const std::u16string line = spin ? SpinLine(original) : std::u16string();
  const std::u16string wanted = ForPlayer(line.empty() ? original : original + u"\n" + line, player);
  if (wanted != now) scrooby::SetText(ctx, base, text, wanted);
  if (wanted == original) {
    if (it != g_spun.end()) g_spun.erase(it);
  } else {
    g_spun[text] = {original, wanted};
  }
}

void SpinRows(PPCContext& ctx, uint8_t* base, int owner) {
  // The Tutorials pages: opened from a pause menu, they belong to its owner.
  if (const uint32_t page = scrooby::FindPage(ctx, base, "InGame_Tutorial")) {
    for (const char* name : {"Text0", "Text1", "Text2", "Text3"}) {
      SpinRow(ctx, base, scrooby::FindText(ctx, base, page, name), owner);
    }
  }
  if (const uint32_t page = scrooby::FindPage(ctx, base, "InGame")) {
    SpinRow(ctx, base, scrooby::FindText(ctx, base, page, "HintMessage"));
  }
  // The prompt beside the HUD (dig spots and the like: "¹¼" under the mojo
  // count) = the announcement page's one text, placed at run time (found at a
  // dig spot: "¹¼", shown; the co-op pages' Button text never held a spin).
  if (const uint32_t page = scrooby::FindPage(ctx, base, "InGame_Announcement")) {
    SpinRow(ctx, base, scrooby::FindText(ctx, base, page, "AnnouncementText"));
  }
}

// EACH PLAYER'S TEXTS (2026-10-17): the texts that belong to one player get
// that player's prompt characters (ForPlayer): the co-op corner (pages
// InGame_CoOp_Player1-4, texts Message and Button: "Join Game" and its
// button, the countdown...) and the counter prompt beside each HUD
// (InGame.pag CounterButtonPlayer1-4; players 3-4's pages / texts are
// players/more_players_hud.cpp's copies). Looked at every front-end frame
// AND after every HUD update (HudFrame): while the corners show "Join Game"
// (the first ~10 s of a level), the HUD (sub_8226A998 -> sub_82269480) sets
// their button from the text bank EVERY frame, after the front end's update
// (found with the console's `who` on the text's string word +156).
void PlayerTexts(PPCContext& ctx, uint8_t* base) {
  const uint32_t ingame = scrooby::FindPage(ctx, base, "InGame");
  for (int p = 0; p < 4; ++p) {
    const std::string digit = std::to_string(p + 1);
    if (const uint32_t page = scrooby::FindPage(ctx, base, ("InGame_CoOp_Player" + digit).c_str())) {
      for (const char* name : {"Message", "Button"}) SpinRow(ctx, base, scrooby::FindText(ctx, base, page, name), p);
    }
    if (ingame) SpinRow(ctx, base, scrooby::FindText(ctx, base, ingame, ("CounterButtonPlayer" + digit).c_str()), p);
  }
}

// THE COUNTDOWN UNDER OUR PICTURE (2026-10-23). A dead player's corner
// read "Please Wait" + a button picture instead of "Please Wait 5 .. 1". The
// corner (sub_82269480, once per display per frame) shows the countdown by
// formatting the seconds into the display's own buffer (+8) and pointing the
// Button text at it, but ONLY when the text points at nothing yet
// (sub_823721D8 = its +152; 0x822696B0). Otherwise ("Join Game") it points
// the text back to 0 and sets the button from the text bank. PlayerTexts
// above gives the Button text a COPY of its player's picture after every HUD
// update, so when a countdown began the text still held our copy: the game
// skipped its number and our picture stayed. Found with the console's `who`
// on the text's +152 (writes: the corner's reset at 0x822696F4, our copy via
// sub_82373360). Now the corner gets the game's own state back first: our
// copy is dropped (pointed to 0, freed: scrooby.cpp TEXT OWNERSHIP FIX), the
// corner decides as on the 360, then PlayerTexts converts what it chose
// (a number has no prompt characters: left alone).
void GiveBackToGame(PPCContext& ctx, uint8_t* base, uint32_t text) {
  if (!text) return;
  const auto it = g_spun.find(text);
  if (it == g_spun.end()) return;
  // Only our own copy (owned, +179 bit 0x20, still what we set): never the
  // game's number buffer or a string it pointed at since.
  if ((*GuestPtr(base, text + 179) & 0x20) && ReadText(base, text) == it->second.shown) {
    scrooby::Call(sub_823731D0, ctx, base, text, 0);
  }
  g_spun.erase(it);
}

}  // namespace

void HudFrame(PPCContext& ctx, uint8_t* base) {
  if (g_menu_glyphs.load(std::memory_order_relaxed)) PlayerTexts(ctx, base);
}

void FrontEndFrame(PPCContext& ctx, uint8_t* base) {
  // Whose menu is up (who paused...; menu_input.cpp): its prompts are theirs.
  const int owner =
      g_menu_glyphs.load(std::memory_order_relaxed) && menu_input::MenuOnScreen()
          ? std::max(0, menu_input::CurrentMenuOwner(ctx, base))
          : 0;
  SpinRows(ctx, base, owner);
  if (!g_menu_glyphs.load(std::memory_order_relaxed)) return;
  PlayerTexts(ctx, base);
  // The menus' prompt bar: page FE_Buttons (always loaded, in the front end
  // and in game), whose BUTTON texts hold just the glyph ("±"), apart from
  // their labels ("Select"): their play glyphs become the menu ones, in the
  // menu OWNER's pictures (player slots: player 2 paused = player 2's). Found
  // every frame (four lookups by name): pages come and go with the packages.
  const uint32_t page = scrooby::FindPage(ctx, base, "FE_Buttons");
  if (!page) return;
  static constexpr const char* kButtons[] = {"LowerLeftButton", "LowerRightButton",
                                             "UpperLeftButton", "UpperRightButton"};
  for (const char* name : kButtons) {
    const uint32_t text = scrooby::FindText(ctx, base, page, name);
    const uint32_t string = text ? Load32(base, text + kTextString) : 0;
    if (!string) continue;
    std::u16string now, value;
    for (uint32_t i = 0; i < 64; ++i) {
      const uint8_t* c = GuestPtr(base, string + 2 * i);
      const char16_t ch = char16_t(c[0] << 8 | c[1]);
      if (!ch) break;
      now.push_back(ch);
      value.push_back(MenuGlyph(BaseGlyph(ch)));
    }
    // The menu owner's pictures (player 2-4 paused: their buttons).
    value = ForPlayer(value, owner);
    if (value != now) scrooby::SetText(ctx, base, text, value);
  }
}

void SeePad(uint32_t user, const uint8_t* gamepad, uint16_t ours) {
  // Guest X_INPUT_GAMEPAD: u16 buttons, u8 LT, u8 RT, s16 LX, LY, RX, RY (big-endian).
  const uint16_t buttons = uint16_t(gamepad[0] << 8 | gamepad[1]);
  auto stick = [&](int i) { return int16_t(uint16_t(gamepad[4 + 2 * i] << 8 | gamepad[5 + 2 * i])); };
  // Take away what the keyboard pressed (it's merged into its player's
  // state): what's left came from a controller. Sticks count only while the
  // keyboard leaves them alone, well past XInput's dead zone (7,849), so a
  // drifting stick doesn't count.
  // Our own filters' buttons (the START a key press becomes on the title or
  // for a movie's Skip prompt) aren't a controller either.
  uint16_t kb_buttons = ours;
  bool kb_sticks = false, kb_triggers = false;
  if (kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get()) {
    const rex::input::X_INPUT_GAMEPAD kb = driver->last_pad();
    kb_buttons |= kb.buttons;
    kb_sticks = kb.thumb_lx != 0 || kb.thumb_ly != 0 || kb.thumb_rx != 0 || kb.thumb_ry != 0;
    kb_triggers = kb.left_trigger != 0 || kb.right_trigger != 0;
  }
  bool pad = (buttons & ~kb_buttons) != 0;
  if (!kb_triggers && (gamepad[2] > 64 || gamepad[3] > 64)) pad = true;
  if (!kb_sticks) {
    for (int i = 0; i < 4; ++i) pad = pad || std::abs(int(stick(i))) > 12000;
  }
  if (pad) {
    if (user < g_pad_activity.size()) g_pad_activity[user] = Clock::now().time_since_epoch().count();
  }
}

}  // namespace prompt_icons

// TextureLoader::LoadObject (vtable 0x8200D710 slot 5): every texture as
// it's built, through r4 like the font loader below; the frame end looks for
// the world's B among them (prompt_icons State::textures_loaded).
extern "C" REX_FUNC(__imp__sub_823AFB00);
namespace prompt_icons {
// A floating button's coin is being set up (outfits.cpp HairSetupDraw: r3 =
// its xnSimpleShader, `xn_context` = the mesh draw's context). Which texture
// does it get, and how many coins are there?
//
// The game's one B texture is shared by every icon, but the draw's world
// matrix (VS c12-c15, already set when the material's set-up runs) is exactly
// the icon's own matrix (icon +32, translation at +80), so the nearest SHOWN
// icon is the one being drawn. Its coins: every player who may use it (the
// game's own list, WHO MAY USE IT below; plus the owner), one coin per DIFFERENT
// look: a keyboard player and a controller player at the same totem get a
// key and a B side by side; players sharing a look share one coin. Each
// coin's texture is its look's player's copy of the B (PatchWorldButton).
// The shader gets the first coin's now (set-up reads it; the native renderer
// reads +92 at the draw, so it stays until the frame ends, WorldButtonRestore);
// the native renderer draws the others (CurrentCoinGroup, recorder.cpp).
// Returns false if `shader` isn't the world B's material.
namespace {
CoinGroup g_coins;
uint32_t g_coin_shader = 0, g_coin_original = 0;
constexpr float kCoinRange = 12.0f;  // world units from the icon's object (last resort)
}  // namespace

// WHO MAY USE IT (2026-10-22). A player gets a coin only while the GAME
// would let them use the thing: the interactable on the icon's object keeps
// its own list of the bodies close enough. CInteractableBehaviour (vtable
// 0x8202EA6C, object at +28 like every behaviour): +32 an inline list of up
// to 4 entries (8 bytes: the body (an ObjPtr: the actor), a countdown f32),
// +64 the list's end. sub_82149C60 adds or refreshes a body each time its
// interactor reports in (message 18 / 2); the Update (sub_82149808) counts
// the countdowns down and drops the expired ones. So "in the list" = in
// range right now, by the game's own rule. Found 2026-10-22: a player far
// across the lawn still got a coin at a totem from the 12-unit guess.
// The interactables are remembered as their Update runs (main thread).
constexpr uint32_t kInteractableVtable = 0x8202EA6C;
std::mutex g_interactables_mutex;
std::vector<uint32_t> g_interactables;

void NoteInteractable(uint32_t it) {
  std::lock_guard<std::mutex> lock(g_interactables_mutex);
  if (std::find(g_interactables.begin(), g_interactables.end(), it) == g_interactables.end()) {
    g_interactables.push_back(it);
  }
}

// The interactable on `object` (0 = none), forgetting the ones that went away.
uint32_t InteractableOf(uint8_t* base, uint32_t object) {
  std::lock_guard<std::mutex> lock(g_interactables_mutex);
  std::erase_if(g_interactables, [base](uint32_t it) { return Load32(base, it) != kInteractableVtable; });
  for (uint32_t it : g_interactables) {
    if (Load32(base, it + 28) == object) return it;
  }
  return 0;
}

// Is player p's body (their Crash, or the titan they ride) in the list?
bool InInteractableList(uint8_t* base, uint32_t it, int p) {
  const uint32_t crash = more_players::CharacterOfPlayer(p), titan = more_players::TitanOfPlayer(p);
  const uint32_t end = Load32(base, it + 64);
  if (end < it + 32 || end > it + 32 + 4 * 8) return false;
  for (uint32_t e = it + 32; e < end; e += 8) {
    const uint32_t body = Load32(base, e);
    if (body && (body == crash || body == titan)) return true;
  }
  return false;
}

// WHO CAN JACK IT (2026-10-24). A stunned titan has no interactable list:
// its floating B means "this titan can be jacked" and shows from any
// distance (it went up 72.8 units from the nearest player in a playtest;
// the old guess, "within the owner's distance at the time + 15%", then gave
// the whole area a coin). The game's own rule, found with the debug console
// (findings/37 s.7):
//   * a stunned titan OPENS AN OPPORTUNITY (COpportunityBehaviour, vtable
//     0x8202F904; message 24, subtype 0 = open sub_82156128, 1 = close,
//     2 = check sub_82157058) named after its fight tree: StrongJackable,
//     BearJackable, ...;
//   * the open opportunities live in one table at 0x825A5418: a binary tree
//     (root = the word at the table, sentinel = table + 4; node +0 left, +4
//     right, +0x10 the name's hash (h * 31 + c), +0x14 a list: entry +0 next,
//     +8 the COpportunity: +0xC the actor offering it, +0x10 the one actor it
//     is for, 0 = anyone);
//   * on the Jack button, Crash's fight tree (Crashlua.blua, the function at
//     line 7306) checks each name with its own range; the check (sub_82157058)
//     takes an offer whose actor is within that range across the ground
//     (x, z) AND up or down (|y|), from Crash to the offering actor.
// So player p can jack titan T right now = T offers one of these names and
// p's Crash, on foot, is within its range. Measured before the script was
// read: a Ratcicle (Strong) jacks at 6.0, not at 6.5; Scorporilla at 8.
constexpr uint32_t kOpportunityTable = 0x825A5418;
constexpr uint32_t kCoOpState = 0x8259B11C;  // u32 per player: 2 = in the game (more_players.h)

constexpr uint32_t NameHash(std::string_view name) {
  uint32_t h = 0;
  for (const char c : name) h = h * 31 + uint8_t(c);
  return h;
}
struct JackCheck {
  uint32_t key;
  float range;
};
// In the script's order, with its ranges.
constexpr JackCheck kJackChecks[] = {
    {NameHash("BossJackable"), 8.5f},       {NameHash("CaptainJackable"), 8.5f},
    {NameHash("BearJackable"), 6.5f},       {NameHash("StrongJackable"), 6.0f},
    {NameHash("ProjectileJackable"), 6.0f}, {NameHash("JackableTurretJackableByCrash"), 8.0f},
};

// The list of the opportunities open under `key` (0 = none): a walk down the
// tree like the game's lookup (keys compared as unsigned numbers).
uint32_t OpportunityList(uint8_t* base, uint32_t key) {
  const uint32_t sentinel = kOpportunityTable + 4;
  uint32_t node = Load32(base, kOpportunityTable);
  for (int depth = 0; depth < 64 && node != sentinel && node >= 0x40000000u; ++depth) {
    const uint32_t at = Load32(base, node + 0x10);
    if (key == at) return Load32(base, node + 0x14);
    node = Load32(base, node + (key < at ? 0 : 4));
  }
  return 0;
}

// Body position (an actor's world matrix: +44, translation at +48).
bool ActorAt(uint8_t* base, uint32_t actor, float out[3]) {
  const uint32_t matrix = actor ? Load32(base, actor + 44) : 0;
  if (matrix < 0x40000000u) return false;
  for (int i = 0; i < 3; ++i) {
    const uint32_t w = Load32(base, matrix + 48 + 4 * i);
    std::memcpy(&out[i], &w, 4);
  }
  return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

// Bit p set = player p could jack `titan` now. *jackable = the titan offers a
// jack at all (it is down, waiting for a rider).
uint32_t JackReach(uint8_t* base, uint32_t titan, bool* jackable) {
  *jackable = false;
  float at[3];
  if (!ActorAt(base, titan, at)) return 0;
  uint32_t reach = 0;
  for (const JackCheck& check : kJackChecks) {
    uint32_t entry = OpportunityList(base, check.key);
    for (int n = 0; n < 64 && entry >= 0x40000000u; ++n, entry = Load32(base, entry)) {
      const uint32_t offer = Load32(base, entry + 8);
      if (offer < 0x40000000u || Load32(base, offer + 0xC) != titan) continue;
      *jackable = true;
      const uint32_t only_for = Load32(base, offer + 0x10);
      for (int p = 0; p < 4; ++p) {
        // On foot: in the game (not a mask, not joining) and riding no titan.
        if (!menu_input::PlayerPlays(p) || Load32(base, kCoOpState + 4 * uint32_t(p)) != 2 ||
            more_players::TitanOfPlayer(p)) {
          continue;
        }
        const uint32_t crash = more_players::CharacterOfPlayer(p);
        float body[3];
        if (!crash || (only_for && only_for != crash) || !ActorAt(base, crash, body)) continue;
        const float dx = at[0] - body[0], dy = at[1] - body[1], dz = at[2] - body[2];
        if (std::fabs(dy) <= check.range && dx * dx + dz * dz <= check.range * check.range) reach |= 1u << p;
      }
    }
  }
  return reach;
}

const CoinGroup& CurrentCoinGroup() { return g_coins; }

// FORGET THE LAST MATERIAL (2026-10-23). Two floating buttons up at once
// (two players at two things in Crash's house: the chest and the tiki face)
// showed ONE player's picture on both. xnContext::DrawPrimBuffer (0x82430878)
// applies a material's pass through 0x8243C0D0 / 0x8243C198, which compare the
// material's key (+12) with the last one applied (global 0x824EE830): the
// same material again = its set-up (vtable slot 14, where WorldButtonSwap
// picks the coin's texture) is SKIPPED for a one-pass material. Every coin
// shares the one B material and the game draws them back to back, so the
// second coin kept the first's texture (and the native renderer its coin
// group). Found from the log: one "floating button ... players near" line
// per frame for two icons up. The fix: before a coin's DrawPrimBuffer the
// global is set to -1, as the game's own reset does (0x8243C250), so each
// coin gets its own set-up (and its owner's picture) in BOTH renderers.
constexpr uint32_t kLastMaterialKey = 0x824EE830;
namespace {
// The game's B + players 1-4's copies + the jack symbol's (0 = none).
std::array<std::atomic<uint32_t>, kJackSymbol + 2> g_world_xn{};

void RefreshWorldXn() {  // with g.mutex held
  uint32_t xn[kJackSymbol + 2] = {};
  for (const GuestPage& gp : g.pages) {
    if (gp.font || gp.prompt != &kWorldB || gp.world_player > kJackSymbol) continue;
    xn[gp.world_player + 1] = gp.xn;
  }
  for (int i = 0; i < kJackSymbol + 2; ++i) g_world_xn[i].store(xn[i], std::memory_order_relaxed);
}
}  // namespace

bool IsCoinMaterial(uint8_t* base, uint32_t material) {
  if (!material || !g_world_xn[0].load(std::memory_order_relaxed)) return false;
  if (Load32(base, material) != 0x82017E64u) return false;  // xnSimpleShader's vtable
  const uint32_t texture = Load32(base, material + 92);
  for (const auto& xn : g_world_xn) {
    if (texture && xn.load(std::memory_order_relaxed) == texture) {
      Store32(base, kLastMaterialKey, 0xFFFFFFFFu);
      return true;
    }
  }
  return false;
}

bool WorldButtonSwap(uint8_t* base, uint32_t shader, uint32_t xn_context, uint32_t& from_xn,
                     uint32_t& to_xn) {
  if (!xn_context) return false;
  const uint32_t current = Load32(base, shader + 92);
  const uint32_t dev = Load32(base, xn_context + 400);  // xnContext +400 = the D3D device
  if (dev < 0x40000000) return false;
  float at[3];
  for (int i = 0; i < 3; ++i) {
    const uint32_t w = Load32(base, dev + 1920 + 16 * 15 + 4 * i);  // VS c15 = world translation
    std::memcpy(&at[i], &w, 4);
  }
  std::lock_guard<std::mutex> lock(g.mutex);
  const GuestPage* original = nullptr;
  uint32_t copy[4] = {};
  for (const GuestPage& gp : g.pages) {
    if (gp.font || gp.prompt != &kWorldB) continue;
    if (gp.world_player < 0) original = &gp;
    else if (gp.world_player < 4) copy[gp.world_player] = gp.xn;
  }
  if (!original) return false;
  bool ours = original->xn == current;
  for (uint32_t c : copy) ours = ours || (c && c == current);
  if (!ours) return false;
  from_xn = current;
  to_xn = original->xn;
  g_coins = CoinGroup{};
  g_coin_shader = shader;
  g_coin_original = original->xn;
  // The nearest shown icon: this coin's.
  int owner = -1;
  uint32_t icon_object = 0;
  float icon_range = 0;
  float best = 0.25f;  // squared distance in world units: the same matrix to the last digit
  {
    std::lock_guard<std::mutex> icons(g_icons_mutex);
    for (const IconOwner& o : g_icons) {
      float d2 = 0;
      for (int i = 0; i < 3; ++i) {
        const uint32_t w = Load32(base, o.icon + 32 + 48 + 4 * i);
        float v;
        std::memcpy(&v, &w, 4);
        d2 += (v - at[i]) * (v - at[i]);
      }
      if (d2 < best) {
        best = d2, owner = o.player, icon_range = o.range;
        icon_object = Load32(base, o.icon + kIconObject);
      }
    }
  }
  if (owner < 0) return true;  // not an icon we know: the game's own B
  // Who stands near it.
  float centre[3] = {at[0], at[1], at[2]};
  if (icon_object) {
    if (const uint32_t m = Load32(base, icon_object + 44); m > 0x40000000) {
      for (int i = 0; i < 3; ++i) {
        const uint32_t w = Load32(base, m + 48 + 4 * i);
        std::memcpy(&centre[i], &w, 4);
      }
    }
  }
  const uint32_t interactable = icon_object ? InteractableOf(base, icon_object) : 0;
  if (!interactable) {
    // Once per object (two such buttons up at once alternated one "last
    // logged" value = this line every frame: 2026-10-24 log).
    static std::set<uint32_t> logged;
    if (logged.size() > 256) logged.clear();
    if (logged.insert(icon_object).second) {
      REXLOG_INFO("Prompt pictures: a floating button ({:08X}) without an interactable: coins within {:.2f}",
                  icon_object, icon_range);
    }
  }
  // A stunned titan: the game's own jack rule (WHO CAN JACK IT).
  bool jackable = false;
  const uint32_t jack_reach = !interactable && icon_object ? JackReach(base, icon_object, &jackable) : 0;
  std::string seen[4];
  int count = 0;
  uint32_t players_near = 0;  // bit p: player p may use it (for the log)
  for (int p = 0; p < 4 && count < 4; ++p) {
    float body[3];
    bool near = false;
    if (!menu_input::PlayerPlays(p)) {
    } else if (interactable) {
      near = InInteractableList(base, interactable, p);
    } else if (jackable) {
      near = (jack_reach >> p) & 1;
    } else {
      // No list: within the range the game showed it at (its owner's
      // distance then, 15% to spare for steps and turns).
      const float range = icon_range > 0 ? icon_range * 1.15f : kCoinRange;
      near = BodyAt(base, p, body) &&
             (body[0] - centre[0]) * (body[0] - centre[0]) + (body[1] - centre[1]) * (body[1] - centre[1]) +
                     (body[2] - centre[2]) * (body[2] - centre[2]) <=
                 range * range;
    }
    if (!near) continue;  // the owner too: they may have walked off while another stays
    players_near |= 1u << p;
    const std::string look = LookOf(uint32_t(p)).Name();
    bool known = false;
    for (int i = 0; i < count; ++i) known = known || seen[i] == look;
    if (known || !copy[p]) continue;
    seen[count] = look;
    g_coins.xn[count++] = copy[p];
  }
  if (count == 0) {
    // Nobody close enough to jack a stunned titan: the jack symbol ("this
    // one can be jacked", from anywhere, as the original's B meant). Else
    // (nobody in an interactable's list for a frame): the owner's, as before.
    const uint32_t jack = g_world_xn[kJackSymbol + 1].load(std::memory_order_relaxed);
    if (jackable && jack) {
      g_coins.xn[count++] = jack;
      g_coins.scale = kJackSymbolScale;
    }
    else if (copy[owner]) g_coins.xn[count++] = copy[owner];
  }
  g_coins.count = count;
  if (count > 0) to_xn = g_coins.xn[0];
  {  // who may use it: logged when that changes
    static std::map<uint32_t, uint32_t> last_near;  // per icon object (several can be up at once)
    if (last_near.size() > 64) last_near.clear();
    auto [it, fresh] = last_near.try_emplace(icon_object, players_near);
    if (fresh || it->second != players_near) {
      it->second = players_near;
      std::string who;
      for (int p = 0; p < 4; ++p) {
        if (players_near & (1u << p)) who += fmt::format("{}{}", who.empty() ? "" : " ", p + 1);
      }
      REXLOG_INFO("Prompt pictures: floating button {:08X}: players near [{}], {} coin(s){}", icon_object, who,
                  count, jackable ? (players_near ? " (titan: in jacking reach)" : " (titan: jack symbol)") : "");
    }
  }
  return true;
}

// The frame's end: the world B's material gets its own texture back (it holds
// a reference: what's there when the material goes must be its own).
void WorldButtonRestore(uint8_t* base) {
  g_coins = CoinGroup{};
  if (!g_coin_shader) return;
  if (Load32(base, g_coin_shader) == 0x82017E64u && Load32(base, g_coin_shader + 92) != g_coin_original) {
    Store32(base, g_coin_shader + 92, g_coin_original);
  }
  g_coin_shader = 0;
}
bool KeysLookFor(int p) { return p >= 0 && p < 4 && LookOf(uint32_t(p)).keys; }
int PadIconsSetting(int p) { return p >= 0 && p < 4 ? PadIconsSettingOf(uint32_t(p)) : 0; }

int DetectedPadStyle(const std::string& key) {
  const int style = StyleOfPad(key);
  return style == kOriginalStyle ? 1 : style + 2;
}

}  // namespace prompt_icons

// CInteractableBehaviour Update (vtable slot 4): remembered for WHO MAY USE IT.
extern "C" REX_FUNC(__imp__sub_82149808);
extern "C" REX_FUNC(sub_82149808) {
  prompt_icons::NoteInteractable(ctx.r3.u32);
  __imp__sub_82149808(ctx, base);
}

// CIconBehaviour: show / hide (r4) its floating button (THE FLOATING B'S
// OWNER): an icon that turns on gets the nearest playing player as owner.
extern "C" REX_FUNC(__imp__sub_82179130);
extern "C" REX_FUNC(sub_82179130) {
  using namespace prompt_icons;
  const uint32_t icon = ctx.r3.u32;
  const bool was = icon && *GuestPtr(base, icon + kIconShown);
  __imp__sub_82179130(ctx, base);
  if (!icon || was || !*GuestPtr(base, icon + kIconShown)) return;
  const uint32_t object = Load32(base, icon + kIconObject);
  const uint32_t matrix = object ? Load32(base, object + 44) : 0;
  if (matrix < 0x40000000) return;
  float at[3];
  for (int i = 0; i < 3; ++i) {
    const uint32_t w = Load32(base, matrix + 48 + 4 * i);
    std::memcpy(&at[i], &w, 4);
  }
  int owner = 0;
  float best = INFINITY;
  for (int p = 0; p < more_players::LocalPlayerCount(); ++p) {
    float body[3];
    if (!menu_input::PlayerPlays(p) || !BodyAt(base, p, body)) continue;
    const float dx = body[0] - at[0], dy = body[1] - at[1], dz = body[2] - at[2];
    const float d = dx * dx + dy * dy + dz * dz;
    if (d < best) best = d, owner = p;
  }
  std::lock_guard<std::mutex> lock(g_icons_mutex);
  g_icons.push_back({icon, owner, std::isfinite(best) ? std::sqrt(best) : 0.0f});
  REXLOG_INFO("Prompt pictures: a floating button appeared ({:08X}), for player {} ({:.2f} away)", icon,
              owner + 1, g_icons.back().range);
}

extern "C" REX_FUNC(sub_823AFB00) {
  const uint32_t out = ctx.r4.u32;
  __imp__sub_823AFB00(ctx, base);
  const uint32_t texture = out ? prompt_icons::Load32(base, out) : 0;
  if (texture) {
    std::lock_guard<std::mutex> lock(prompt_icons::g.mutex);
    if (prompt_icons::g.textures_loaded.size() < 4096) prompt_icons::g.textures_loaded.push_back(texture);
  }
}

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

// The HUD display's co-op corner ("Join Game" / "Please Wait N", r3 = the
// display; +20 its Button text): THE COUNTDOWN UNDER OUR PICTURE.
extern "C" REX_FUNC(__imp__sub_82269480);
extern "C" REX_FUNC(sub_82269480) {
  const uint32_t display = ctx.r3.u32;
  if (prompt_icons::g_menu_glyphs.load(std::memory_order_relaxed) && display) {
    prompt_icons::GiveBackToGame(ctx, base, prompt_icons::Load32(base, display + 20));
  }
  __imp__sub_82269480(ctx, base);
  // "JOIN GAME" ONLY FOR PLAYERS WITH A CONTROLLER (2026-10-29): the corner
  // offered every player not in the game "Join Game", controller or not
  // (with players 3-4 too). Players 2-4 without one (players_setup.h
  // HasController) get no corner: its Message (+16) and Button (+20) texts
  // hidden right after the game set them (it sets their shown bit, +110
  // bit 0x80, every frame, so nothing needs putting back). A player in the
  // game (co-op state != 0) keeps theirs (the countdown after a death...).
  // Any button on a free controller gives it a player (PollJoinsInPlay):
  // their corner appears, in their own button.
  if (display) {
    const int32_t p = int32_t(prompt_icons::Load32(base, display + 0x90));
    if (p >= 1 && p < 4 && prompt_icons::Load32(base, 0x8259B11C + 4 * uint32_t(p)) == 0 &&
        !players_setup::HasController(p)) {
      for (const uint32_t field : {16u, 20u}) {
        if (const uint32_t text = prompt_icons::Load32(base, display + field)) scrooby::SetVisible(base, text, false);
      }
    }
  }
}
