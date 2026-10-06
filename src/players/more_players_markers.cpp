// =============================================================================
// players/more_players_markers.cpp -- pictures of their own for players 3-4's
// markers over their heads (findings/26 s.29)
// =============================================================================
//
// WHAT THE GAME DOES (found 2026-10-06):
//   The marker over a player's head (CPlayerIdentifierArrow, vtable
//   0x8203B21C, the front end's +1720) is a banner with a stylized digit
//   ("1" / "2") plus a little arrow, tinted in the player's colour. Its
//   set-up (slot 1, sub_8226CF10) finds its pictures BY NAME: a name is an
//   8-byte key (Radical's hashed name, built at startup by sub_8236ACB8(key,
//   "HUD_coop_player_one.tga")), two keys at 0x825A5B20 (one, two) indexed
//   by the player (+8), the arrow's at 0x825A5B18; the lookup sub_820D88D0(4,
//   &key) searches the loaded data. The draw (slot 4) uses +12 (its own
//   banner) and +20 (the arrow). (+16, the other number, is never drawn.)
//
//   The pictures live in the 16 in-game menu packages (package/<hex>.p3d,
//   the ones holding InGame.prj; the path builder's category 6) as Pure3D
//   textures:
//     0x19000 texture "HUD_coop_player_one.tga" (name, version 0x36B0, 32 x 64, 8 bpp, ...)
//       0x19001 image (same name and numbers)
//         0x19002 image data: u32 size + a whole TGA file (type 1: 8-bit
//                 palettized, 256 x 32-bit palette, top-left origin)
//   The banners are white (alpha 254) with a black symbol and frame at alpha
//   100 (about 39%: the world shows through); the tint colours the white.
//
// WHAT THIS MODULE DOES (with --local_players 3 or 4):
//   * Reads the pictures for players 3-4: the port's own, drawn for it
//     (assets/markers/HUD_coop_player_three.png, ..._four.png, copied next to
//     the exe at build time), unless the player has a copy of their own in
//     <user data>/markers (same names: that one wins). 32 x 64, white +
//     black + greys. Opacity follows the game's
//     banners: a pixel's alpha comes from its brightness (black 100, white
//     254, greys between; nearly transparent pixels, alpha < 50, become
//     fully transparent), so a picture drawn with a solid black works.
//   * A data patch (data/data_patcher.h) adds to every in-game menus package
//     that holds player 1's banner two more textures, built like it:
//     "HUD_coop_player_three.tga" / "..._four.tga", their TGA made from the
//     PNG (a palette of the picture's grey + alpha pairs).
//   * Builds their name keys with the game's own sub_8236ACB8, checks the
//     texture is loaded (sub_820D88D0), and the set-up's lookup of player
//     3 / 4's banner (midasm hook at 0x8226CF3C) takes that key. A missing or
//     unreadable PNG, or a level whose package has no copy: player 1 / 2's
//     banner as before (tinted green / purple, more_players_frontend.cpp).
// =============================================================================
#include "more_players.h"

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "data/data_patcher.h"
#include "game_folder.h"
#include "data/pure3d.h"

// stb_image (from the SDK's source tree), private to this file.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

extern "C" REX_FUNC(__imp__sub_8226CF10);  // CPlayerIdentifierArrow set-up (r3 this)
extern "C" REX_FUNC(__imp__sub_8236ACB8);  // name key from a string (r3 key, r4 string)
extern "C" REX_FUNC(__imp__sub_820D88D0);  // find loaded data (r3 section 4, r4 &key)

namespace more_players_markers {
namespace {

using Bytes = data_patcher::Bytes;
using pure3d::Chunk;

constexpr uint32_t kTexture = 0x19000, kImage = 0x19001, kImageData = 0x19002;
constexpr int kWidth = 32, kHeight = 64;
const char* const kPlayerOne = "HUD_coop_player_one.tga";
const char* const kNames[2] = {"HUD_coop_player_three.tga", "HUD_coop_player_four.tga"};
const char* const kFiles[2] = {"HUD_coop_player_three.png", "HUD_coop_player_four.png"};

Bytes g_tga[2];               // players 3-4's pictures as the game's TGA (empty = none)
uint32_t g_keys = 0;          // guest: 2 x 8-byte name keys, then the 2 strings
bool g_use_ours[2] = {};      // the set-up's lookup takes our key (texture found)

uint8_t* Guest(uint32_t a) { return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(a); }
uint32_t Read32(uint32_t a) {
  const uint8_t* p = Guest(a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint32_t CallGame(void (*function)(PPCContext&, uint8_t*), PPCContext& ctx, uint8_t* base,
                  uint32_t r3, uint32_t r4) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = r3; ctx.r4.u64 = r4;
  function(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

// A PNG -> the TGA the game's textures hold (see the header). Empty if the
// file is missing or not 32 x 64.
Bytes LoadPicture(const std::filesystem::path& path) {
  int w = 0, h = 0, n = 0;
  stbi_uc* rgba = stbi_load(path.string().c_str(), &w, &h, &n, 4);
  if (!rgba) return {};
  if (w != kWidth || h != kHeight) {
    REXLOG_WARN("Markers: {} is {} x {}, it has to be {} x {}: not used", path.string(), w, h,
                kWidth, kHeight);
    stbi_image_free(rgba);
    return {};
  }
  // Grey + alpha per pixel (alpha from brightness), then a palette of the
  // distinct pairs. Brightness is 0-255 and alpha follows it, so there are
  // at most 257 pairs (+ transparent): the 2 lowest greys share an entry
  // if needed (invisible difference).
  std::vector<uint16_t> pixels(kWidth * kHeight);  // grey << 8 | alpha
  for (int i = 0; i < kWidth * kHeight; ++i) {
    const stbi_uc* p = rgba + 4 * i;
    if (p[3] < 50) { pixels[i] = 0; continue; }
    int grey = (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000;
    if (grey == 0) grey = 1;  // keeps index 0 for "transparent"
    const int alpha = (100 * 255 + 154 * grey + 127) / 255;
    pixels[i] = uint16_t(grey << 8 | alpha);
  }
  stbi_image_free(rgba);
  std::vector<uint16_t> palette = {0};  // entry 0 = transparent
  std::vector<uint8_t> index(kWidth * kHeight);
  for (int i = 0; i < kWidth * kHeight; ++i) {
    size_t k = 0;
    while (k < palette.size() && palette[k] != pixels[i]) ++k;
    if (k == palette.size()) {
      if (palette.size() == 256) k = 255;  // (can't happen with greys; just in case)
      else palette.push_back(pixels[i]);
    }
    index[i] = uint8_t(k);
  }
  // The TGA, laid out like the game's: 18-byte header, 256 BGRA entries, pixels.
  Bytes tga = {0, 1, 1, 0, 0, 0, 1, 32, 0, 0, 0, 0, kWidth, 0, kHeight, 0, 8, 0x20};
  for (size_t k = 0; k < 256; ++k) {
    const uint16_t e = k < palette.size() ? palette[k] : 0;
    const uint8_t grey = uint8_t(e >> 8), alpha = uint8_t(e);
    tga.insert(tga.end(), {grey, grey, grey, alpha});
  }
  tga.insert(tga.end(), index.begin(), index.end());
  return tga;
}

// A Pure3D name field: length byte (with the NUL, rounded up to 4) + the
// bytes, NUL padded (as the game's files have it).
Bytes NameField(const std::string& name) {
  const size_t length = (name.size() + 1 + 3) & ~size_t(3);
  Bytes out = {uint8_t(length)};
  out.insert(out.end(), name.begin(), name.end());
  out.resize(1 + length, 0);
  return out;
}
// A chunk: 12-byte header, data, children.
Bytes MakeChunk(uint32_t id, const Bytes& data, const Bytes& children) {
  Bytes out;
  pure3d::PutLe32(out, id);
  pure3d::PutLe32(out, uint32_t(12 + data.size()));
  pure3d::PutLe32(out, uint32_t(12 + data.size() + children.size()));
  out.insert(out.end(), data.begin(), data.end());
  out.insert(out.end(), children.begin(), children.end());
  return out;
}
// A chunk's data after its name field (the numbers that follow the name).
Bytes AfterName(const Bytes& d, const Chunk& c) {
  const size_t start = c.offset + 12 + 1 + d[c.offset + 12];
  return Bytes(d.begin() + start, d.begin() + c.offset + c.data_size);
}

// Player 1's texture chunk -> one with `name` and `tga` (same numbers: same
// size and format). Empty if it isn't laid out as expected.
Bytes TextureLike(const Bytes& d, const Chunk& texture, const std::string& name, const Bytes& tga) {
  const auto images = pure3d::ChildrenOf(d, texture);
  if (images.size() != 1 || images[0].id != kImage) return {};
  const auto datas = pure3d::ChildrenOf(d, images[0]);
  if (datas.size() != 1 || datas[0].id != kImageData) return {};
  Bytes image_data;
  pure3d::PutLe32(image_data, uint32_t(tga.size()));
  image_data.insert(image_data.end(), tga.begin(), tga.end());
  Bytes image_fields = NameField(name), texture_fields = NameField(name);
  const Bytes image_rest = AfterName(d, images[0]), texture_rest = AfterName(d, texture);
  image_fields.insert(image_fields.end(), image_rest.begin(), image_rest.end());
  texture_fields.insert(texture_fields.end(), texture_rest.begin(), texture_rest.end());
  const Bytes image = MakeChunk(kImage, image_fields, MakeChunk(kImageData, image_data, {}));
  return MakeChunk(kTexture, texture_fields, image);
}

// The data patch: right after player 1's banner, players 3-4's.
bool PatchPackage(const Bytes& d, Bytes* out) {
  Chunk root;
  std::vector<Chunk> top;
  if (!pure3d::Parse(d, &root, &top)) return false;
  Bytes body;
  bool added = false;
  for (const Chunk& c : top) {
    if (c.id == kTexture && (pure3d::NameOf(d, c) == kNames[0] || pure3d::NameOf(d, c) == kNames[1])) {
      return false;  // (already has them)
    }
    const Bytes copy = pure3d::Copy(d, c);
    body.insert(body.end(), copy.begin(), copy.end());
    if (c.id == kTexture && pure3d::NameOf(d, c) == kPlayerOne) {
      for (int i = 0; i < 2; ++i) {
        if (g_tga[i].empty()) continue;
        const Bytes texture = TextureLike(d, c, kNames[i], g_tga[i]);
        if (texture.empty()) return false;
        body.insert(body.end(), texture.begin(), texture.end());
        added = true;
      }
    }
  }
  if (!added) return false;
  *out = pure3d::WithChildren(d, root, body);
  return true;
}

}  // namespace

void Register() {
  if (more_players::LocalPlayerCount() <= 2) return;
  // The player's own copy first (user/markers in the game folder,
  // game_folder.h), then the port's (assets/markers next to the exe).
  std::vector<std::filesystem::path> folders;
  folders.push_back(game_folder::UserFolder() / "markers");
  folders.push_back(rex::filesystem::GetExecutableFolder() / "assets" / "markers");
  int found = 0;
  for (int i = 0; i < 2; ++i) {
    for (const auto& folder : folders) {
      std::error_code ec;
      if (!std::filesystem::exists(folder / kFiles[i], ec)) continue;
      g_tga[i] = LoadPicture(folder / kFiles[i]);
      if (!g_tga[i].empty()) {
        REXLOG_INFO("Markers: player {}'s picture: {}", i + 3, (folder / kFiles[i]).string());
        ++found;
        break;
      }
    }
  }
  if (!found) {
    REXLOG_INFO("Markers: no pictures found (players 3-4 use the 1 / 2 banners)");
    return;
  }
  data_patcher::Register("markers for players 3-4", data_patcher::kCategoryFrontend, "", PatchPackage);
}

}  // namespace more_players_markers

using namespace more_players_markers;

// Set-up: before the original looks its banner up, players 3-4's key (built
// once) is checked against the loaded data.
extern "C" REX_FUNC(sub_8226CF10) {
  const uint32_t marker = ctx.r3.u32;
  const uint32_t player = Read32(marker + 8);
  if (player >= 2 && player < 4 && !g_tga[player - 2].empty()) {
    if (!g_keys) {
      g_keys = rex::system::kernel_memory()->SystemHeapAlloc(16 + 2 * 32);
      for (int i = 0; i < 2; ++i) {
        const uint32_t text = g_keys + 16 + 32 * uint32_t(i);
        std::memset(Guest(text), 0, 32);
        std::memcpy(Guest(text), kNames[i], std::strlen(kNames[i]));
        CallGame(__imp__sub_8236ACB8, ctx, base, g_keys + 8 * uint32_t(i), text);
      }
    }
    const uint32_t key = g_keys + 8 * (player - 2);
    const bool found = CallGame(__imp__sub_820D88D0, ctx, base, 4, key) != 0;
    if (found != g_use_ours[player - 2]) {
      REXLOG_INFO("Markers: player {}'s own picture {}", player + 1,
                  found ? "in use" : "not loaded here: the 1 / 2 banner");
    }
    g_use_ours[player - 2] = found;
  }
  __imp__sub_8226CF10(ctx, base);
}

// The set-up's banner lookup (0x8226CF3C: r11 = player * 8, r3 = the name
// key's address in the two-entry table 0x825A5B20). Players 3-4: their own
// key when their texture is loaded, else player 1 / 2's entry.
void MorePlayersMarkerPicture(PPCRegister& offset, PPCRegister& name) {
  const uint32_t p = offset.u32 / 8;
  if (p < 2 || p >= 4) return;
  if (g_use_ours[p - 2]) name.u64 = g_keys + 8 * (p - 2);
  else name.u64 = name.u32 - 16;  // player 1 / 2's entry
}
