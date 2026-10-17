// =============================================================================
// players/spin_colours.cpp -- see spin_colours.h (docs/findings/46 s.4)
// =============================================================================
//
// HOW (all game addresses verified with xexdis / Ghidra, 2026-10-17):
//
// 1. THE EFFECTS. In bca1bc0b, an effect = a set of top-level chunks tied by
//    NAME: the effect itself (0x123000, lists its parts by name), its
//    skeleton of attachment points (0x23000), animations (0x121000: "VRTX_",
//    "SPAR_", "SPEM_" ... + the part's name), particle systems (0x1580c:
//    emitters 0x15900 with their own animations), billboards (0x17006),
//    meshes (0x10000) and the glow's material (0x11000 Crash_Spin_m) and
//    texture (0x19000 Crash_Spin.tga: three PNG pictures, 32 / 16 / 8 px).
//    Every chunk whose bytes hold one of the spin names is one of them (the
//    six effects Crash's fight tree starts: the spin attack's
//    cha_fx_crash_spin_attack_fx / _arm_geo / _body_geo / _thigh_geo /
//    _foot_geo, and the spin jump's cha_fx_crash_spin_jump).
//
// 2. THE COPIES. Each such chunk is copied right after itself, once per
//    coloured Crash, its names changed in place to the same length (the
//    copy's references to its own parts follow; shared things such as the
//    spark pictures keep their names), and its colours turned:
//      0x10008 a mesh's vertex colours: u32 count, then u32 colours;
//      0x121109 a colour channel ("COOL" colour over life, "CLR" start
//        colour): u32 version, fourcc, u32 count n, n u16 frames, n u32
//        colours (no padding);
//      0x19002 a picture's bytes: u32 size, then the PNG;
//    colours are B G R A bytes (little-endian ARGB). Warm colours (hue 0-70,
//    saturation above 0.12: the yellows, oranges, browns) move to the
//    Crash's hue, orange (25) on it and yellow (50) half as far beyond:
//    new hue = Crash's + (hue - 25) / 2, saturation kept; then it is made
//    as bright to the eye (luminance) as the original x 1.15, turning toward
//    white past full brightness: at the same value blue and purple look far
//    darker than yellow (the first version did that: dim spins). White,
//    black and the blue parts stay.
//
// 3. THE SPAWN. CCharacterEffectAction's start (sub_820C47B8, r3 = the
//    action): track = action +8, its effect name key at track +0xC (8 bytes,
//    from the name by sub_8236ACB8), the character = *(action +0xC) +0x10.
//    For a player wearing a coloured Crash, a track naming one of the six
//    effects names the copy for the length of the call (put back after: the
//    track is shared by every Crash's tree).
// =============================================================================

#include "spin_colours.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../data/data_patcher.h"
#include "../data/pure3d.h"
#include "../png_writer.h"
#include "more_players.h"
#include "outfits.h"

// stb_image (from the SDK's source tree), private to this file.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

extern "C" REX_FUNC(__imp__sub_820C47B8);  // CCharacterEffectAction: start (spawns the effect)
extern "C" REX_FUNC(__imp__sub_8236ACB8);  // name key from a string (r3 key, r4 string)

namespace spin_colours {
namespace {

constexpr uint32_t kCategoryJoinedAssets = 10;  // data_patcher category of bca1bc0b
constexpr const char* kEffectsPackage = "bca1bc0b";

// The name parts that mark the spin effects' chunks, and their copies'
// (same length; '#' = the copy's number 1-6).
struct Rename {
  const char* from;
  const char* to;
};
constexpr Rename kRenames[] = {
    {"crash_spin", "crsh#_spin"},
    {"Crash_Spin", "Crsh#_Spin"},
    {"crash_coopswitch1", "crsh#_coopswitch1"},
};
// The effects Crash's fight tree starts (the copies' names follow kRenames).
constexpr const char* kEffects[] = {
    "cha_fx_crash_spin_attack_fx",        "cha_fx_crash_spin_attack_arm_geo",
    "cha_fx_crash_spin_attack_body_geo",  "cha_fx_crash_spin_attack_thigh_geo",
    "cha_fx_crash_spin_attack_foot_geo",  "cha_fx_crash_spin_jump",
};
constexpr int kEffectCount = int(sizeof(kEffects) / sizeof(kEffects[0]));

std::atomic<bool> g_made{false};  // the patch ran this session

std::string Renamed(std::string name, int copy) {
  for (const Rename& r : kRenames) {
    for (size_t at = name.find(r.from); at != std::string::npos; at = name.find(r.from, at + 1)) {
      std::string to = r.to;
      to[to.find('#')] = char('0' + copy);
      name.replace(at, to.size(), to);
    }
  }
  return name;
}

bool HasSpinName(const data_patcher::Bytes& bytes) {
  for (const Rename& r : kRenames) {
    const size_t n = std::strlen(r.from);
    if (std::search(bytes.begin(), bytes.end(), r.from, r.from + n) != bytes.end()) return true;
  }
  return false;
}

// Every name in a chunk (and its children) -> the copy's, in place.
void RenameAll(pure3d::Node& n, int copy) {
  for (const Rename& r : kRenames) {
    const size_t len = std::strlen(r.from);
    std::string to = r.to;
    to[to.find('#')] = char('0' + copy);
    for (auto it = std::search(n.data.begin(), n.data.end(), r.from, r.from + len); it != n.data.end();
         it = std::search(it + len, n.data.end(), r.from, r.from + len)) {
      std::copy(to.begin(), to.end(), it);
    }
  }
  for (pure3d::Node& k : n.children) RenameAll(k, copy);
}

// --- colours ---
// How much brighter than the original a turned colour is (luminance).
constexpr float kLift = 1.15f;
// One colour (0-1 RGB) turned toward `hue` if it's warm (section 2 above).
void Turn(float rgb[3], float hue) {
  const float r = rgb[0], g = rgb[1], b = rgb[2];
  const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
  if (mx <= 0.0f || mx == mn) return;
  const float s = (mx - mn) / mx, v = mx;
  float h;
  if (mx == r) h = 60.0f * std::fmod((g - b) / (mx - mn) + 6.0f, 6.0f);
  else if (mx == g) h = 60.0f * ((b - r) / (mx - mn) + 2.0f);
  else h = 60.0f * ((r - g) / (mx - mn) + 4.0f);
  if (s < 0.12f || h > 70.0f) return;
  float nh = std::fmod(hue + (h - 25.0f) * 0.5f + 360.0f, 360.0f) / 60.0f;
  const float f = nh - std::floor(nh);
  const float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
  const float table[6][3] = {{v, t, p}, {q, v, p}, {p, v, t}, {p, q, v}, {t, p, v}, {v, p, q}};
  const int i = int(nh) % 6;
  // As bright to the eye as the original (yellow and orange look far
  // brighter than blue or purple at the same value), and a little more:
  // scaled up to the original's luminance x kLift; what goes past full
  // brightness turns toward white, as a hot glow does.
  const auto luma = [](const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; };
  const float before = luma(rgb) * kLift;
  float out[3] = {table[i][0], table[i][1], table[i][2]};
  const float now = luma(out);
  if (now > 0.0f && before > now) {
    for (float& c : out) c *= before / now;
    const float top = std::max({out[0], out[1], out[2]});
    if (top > 1.0f) {
      for (float& c : out) c /= top;  // the colour at full brightness ...
      const float l = luma(out);       // ... then mixed with white up to the luminance
      const float w = l < 1.0f ? std::clamp((std::min(before, 1.0f) - l) / (1.0f - l), 0.0f, 1.0f) : 0.0f;
      for (float& c : out) c = c + (1.0f - c) * w;
    }
  }
  for (int k = 0; k < 3; ++k) rgb[k] = out[k];
}
// A little-endian ARGB colour (bytes B G R A) at p, turned.
void TurnBgra(uint8_t* p, float hue) {
  float rgb[3] = {p[2] / 255.0f, p[1] / 255.0f, p[0] / 255.0f};
  Turn(rgb, hue);
  for (int k = 0; k < 3; ++k) p[2 - k] = uint8_t(std::lround(std::clamp(rgb[k], 0.0f, 1.0f) * 255.0f));
}
// A PNG file turned (false: not a PNG we can read; left as it is).
bool TurnPng(std::vector<uint8_t>& png, float hue) {
  int w = 0, h = 0, n = 0;
  stbi_uc* rgba = stbi_load_from_memory(png.data(), int(png.size()), &w, &h, &n, 4);
  if (!rgba) return false;
  for (int i = 0; i < w * h; ++i) {
    uint8_t* p = rgba + 4 * i;
    float rgb[3] = {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f};
    Turn(rgb, hue);
    for (int k = 0; k < 3; ++k) p[k] = uint8_t(std::lround(std::clamp(rgb[k], 0.0f, 1.0f) * 255.0f));
  }
  png = EncodePng(rgba, uint32_t(w), uint32_t(h), size_t(w) * 4, true);
  stbi_image_free(rgba);
  return true;
}
// Every colour in a chunk and its children; counts what it turned.
void TurnAll(pure3d::Node& n, float hue, int* turned) {
  if (n.id == 0x10008 && n.data.size() >= 4) {
    const uint32_t count = pure3d::Le32(n.data.data());
    if (n.data.size() == 4 + size_t(count) * 4) {
      for (uint32_t i = 0; i < count; ++i) TurnBgra(&n.data[4 + 4 * i], hue);
      *turned += int(count);
    }
  } else if (n.id == 0x121109 && n.data.size() >= 12) {
    const uint32_t count = pure3d::Le32(&n.data[8]);
    const size_t colours = 12 + 2 * size_t(count);
    if (n.data.size() == colours + 4 * size_t(count)) {
      for (uint32_t i = 0; i < count; ++i) TurnBgra(&n.data[colours + 4 * i], hue);
      *turned += int(count);
    }
  } else if (n.id == 0x19002 && n.data.size() >= 4) {
    const uint32_t size = pure3d::Le32(n.data.data());
    if (4 + size_t(size) <= n.data.size() && size >= 8 && n.data[4] == 0x89 && n.data[5] == 'P') {
      std::vector<uint8_t> png(n.data.begin() + 4, n.data.begin() + 4 + size);
      if (TurnPng(png, hue)) {
        data_patcher::Bytes data;
        pure3d::PutLe32(data, uint32_t(png.size()));
        data.insert(data.end(), png.begin(), png.end());
        n.data = std::move(data);
        ++*turned;
      }
    }
  }
  for (pure3d::Node& k : n.children) TurnAll(k, hue, turned);
}

bool PatchEffects(const data_patcher::Bytes& d, data_patcher::Bytes* out) {
  pure3d::Chunk root;
  std::vector<pure3d::Chunk> top;
  if (!pure3d::Parse(d, &root, &top)) return false;
  pure3d::Node tree = pure3d::ToNode(d, root);
  const int colours = outfits::ColourCount();
  std::vector<pure3d::Node> children;
  int spin_chunks = 0, turned = 0;
  for (pure3d::Node& c : tree.children) {
    const bool spin = HasSpinName(pure3d::ToBytes(c));
    const pure3d::Node original = spin ? c : pure3d::Node{};
    children.push_back(std::move(c));
    if (!spin) continue;
    ++spin_chunks;
    for (int i = 0; i < colours; ++i) {  // the copies right after the original
      pure3d::Node copy = original;
      RenameAll(copy, i + 1);
      TurnAll(copy, outfits::ColourHue(i), &turned);
      children.push_back(std::move(copy));
    }
  }
  if (spin_chunks == 0) {
    REXLOG_WARN("Spin colours: no spin effect in {}: coloured Crashes spin in orange", kEffectsPackage);
    return false;
  }
  tree.children = std::move(children);
  *out = pure3d::ToBytes(tree);
  g_made = true;
  REXLOG_INFO("Spin colours: {} chunks of Crash's spin effects copied for {} colours ({} colours turned)",
              spin_chunks, colours, turned);
  return true;
}

// --- the spawn ---
uint8_t* Guest(uint32_t a) { return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(a); }
uint32_t Read32(uint32_t a) {
  const uint8_t* p = Guest(a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// Name keys (8 bytes each, made by the game's own sub_8236ACB8): the six
// effects, then per copy 1-6 the same six; 0 until made.
uint32_t g_keys = 0;
uint32_t KeyAt(int copy, int effect) { return g_keys + 8 * uint32_t(copy * kEffectCount + effect); }
bool MakeKeys(PPCContext& ctx, uint8_t* base) {
  if (g_keys) return true;
  const int copies = outfits::ColourCount() + 1;
  const uint32_t keys = rex::system::kernel_memory()->SystemHeapAlloc(uint32_t(8 * copies * kEffectCount + 64));
  if (!keys) return false;
  const uint32_t text = keys + uint32_t(8 * copies * kEffectCount);
  for (int c = 0; c < copies; ++c) {
    for (int e = 0; e < kEffectCount; ++e) {
      const std::string name = c == 0 ? std::string(kEffects[e]) : Renamed(kEffects[e], c);
      std::memcpy(Guest(text), name.c_str(), name.size() + 1);
      const PPCContext saved = ctx;
      ctx.r3.u64 = keys + 8 * uint32_t(c * kEffectCount + e);
      ctx.r4.u64 = text;
      __imp__sub_8236ACB8(ctx, base);
      ctx = saved;
    }
  }
  g_keys = keys;
  return true;
}

int PlayerOf(uint32_t actor) {
  for (int p = 0; p < 4; ++p) {
    if (actor && more_players::CharacterOfPlayer(p) == actor) return p;
  }
  return -1;
}

}  // namespace

void Register() {
  data_patcher::Register("spin effects in Crash's colours", kCategoryJoinedAssets, kEffectsPackage,
                         &PatchEffects);
}

}  // namespace spin_colours

using namespace spin_colours;

extern "C" REX_FUNC(sub_820C47B8) {
  const uint32_t action = ctx.r3.u32;
  uint32_t track = 0;
  uint8_t saved_key[8];
  if (g_made && action) {
    const uint32_t owner = Read32(action + 0xC);
    const int p = owner ? PlayerOf(Read32(owner + 0x10)) : -1;
    const int colour = p >= 0 ? outfits::ColourIndex(outfits::OutfitOf(p)) : -1;
    if (colour >= 0 && MakeKeys(ctx, base)) {
      const uint32_t t = Read32(action + 8);
      for (int e = 0; t && e < kEffectCount; ++e) {
        if (std::memcmp(Guest(t + 0xC), Guest(KeyAt(0, e)), 8) == 0) {
          track = t;
          std::memcpy(saved_key, Guest(t + 0xC), 8);
          std::memcpy(Guest(t + 0xC), Guest(KeyAt(colour + 1, e)), 8);
          break;
        }
      }
    }
  }
  __imp__sub_820C47B8(ctx, base);
  if (track) std::memcpy(Guest(track + 0xC), saved_key, 8);
}
