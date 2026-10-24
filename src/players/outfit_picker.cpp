// =============================================================================
// players/outfit_picker.cpp -- see outfit_picker.h
// =============================================================================
//
// HOW IT IS BUILT (2026-10-23; the look rebuilt the same day after a design
// round: one CARD per player, side by side)
//
// The screen stays the game's own: the chest opens front end state 847
// (SkinSelectionScreen), whose CSkinScreenAction draws the menu's purple frame
// and blue swirl (CNVBackgroundAction) and a 3D PREVIEW of the shown outfit.
// (A first try took the backdrop away to show the room and the players' own
// characters: the game draws no world under this screen at all, the picture
// behind was black; the preview is drawn by the menu itself.) So:
//
//   * THE LOOK (the chosen design: 1920 x 1080 mockups, one per player count):
//       1 player   = the game's own page re-arranged: a glass panel on the
//                    left (Crash / Coco faces, the outfit's name, the eight
//                    colour circles in two rows of four), the 3D model on the
//                    right on an orange pad, the prompt bar's "B Back" /
//                    "Wear A";
//       2-4 players = a CARD per player in a row, centred: "P1" in the
//                    player's colour at the top left, the model standing on
//                    an oval PAD in the player's colour above the card's
//                    rows: the faces (the other character greyed), the
//                    outfit's name (selected = yellow, bigger, pulsing, a red
//                    arrow at each side), the colour circles (Crash's Normal
//                    look only; the picked one bigger in a white ring, yellow
//                    while that row is selected), "A Wear" under them;
//                    states READY ("Ready!", "B Change"), LOADING ("Loading"
//                    pulsing where Wear was), LOCKED (the game's lock icon,
//                    no Wear).
//   * THE ELEMENTS: a data patch adds one layer to the screen's page
//     (GameStart_SkinSelection.pag in the menus package cdd70a8c, which the
//     game loads in Crash's house): FOUR cards' worth of elements, copies of
//     the page's own (its square glass, its texts, its lock icon, its hinge),
//     plus pictures the page didn't have: the faces (the HUD's MugshotCrash /
//     MugshotCoco, textures of the in-game menus' package 7a8185b0, wrapped
//     as menu sprites) and a CIRCLE (the game has no round picture: drawn by
//     this file, a white disc and a disc with a dark rim, tinted per element
//     for the pads, the colour circles and the ring). The red arrows and the
//     A / B are the game font's own button characters.
//     Nothing is laid out for a player count in the data: every frame each
//     element is MOVED (its box, +92 / +96) and STRETCHED (its transform)
//     to where the mockup has it for this many players. One set of elements
//     instead of one per layout keeps the page small (the menus' memory).
//   * The game's own elements of the page (its single panel, title, arrows,
//     description) are hidden while ours are up; the FE_Buttons prompts are
//     ours too (1 player: Back / Wear, more: none, each card has its own).
//   * INPUT: every joined player's controller (and the keyboard on its
//     socket) is read here, straight from the input system; the GAME gets
//     quiet controllers while the picker is open (its own Update keeps
//     running for the preview and its loading, but presses nothing).
//   * THE MODELS: 1 player = the screen's own 3D model (a new pick = the
//     old model destroyed, the new one built by the screen once its package
//     is in: ShowInPreview), on the right. 2-4 players = one model per card
//     (players/outfit_previews.h), each wearing its player's pick, standing
//     on its card's pad. (The screen's own selection still follows the
//     player who moved last: the wear queue uses its package.)
//   * A = wear it: queued; the preview shows it, its package is waited for
//     (the inventory section, as the F5 spawn menu does), then the screen's
//     own Wear Skin for that player (outfits.cpp section 5). The player is
//     then READY. B = ready -> not ready; not ready -> leave as they are
//     (ready, nothing changed). Locked looks can be looked at, not worn.
//   * Everyone joined is ready and nothing waits = the screen closes: our
//     decision for state 847 takes its ExitBack (852 -> 486, the game).
// =============================================================================
#include "outfit_picker.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../data/data_patcher.h"
#include "../data/fight_tree.h"
#include "../data/pure3d.h"
#include "../input/keyboard_mouse.h"
#include "../input/players.h"
#include "../input/prompt_icons.h"
#include "../ui/scrooby.h"
#include "more_players.h"
#include "outfit_previews.h"
#include "outfits.h"

REXCVAR_DEFINE_BOOL(outfit_picker, true, "CrashMoM",
                    "The chest in Crash's house: every player picks their outfit at the same time on a card "
                    "of their own (false = the game's own one-player-at-a-time screen)");

REXCVAR_DEFINE_STRING(outfit_picker_tune, "", "CrashMoM",
                      "(Developer) the outfit picker's model placement: 'px_per_x,px_per_y,feet_y0,size' "
                      "overrides the measured numbers (tuning only)");

extern "C" REX_FUNC(sub_820E1228);      // skin screen: show entry r4 (outfits.cpp's wrapper)
extern "C" REX_FUNC(__imp__sub_820E1180);  // skin screen: destroy its preview character
extern "C" REX_FUNC(__imp__sub_822D9998);  // music manager: mode (r4: 1 = the level's music, 3 = a menu's)
extern "C" REX_FUNC(sub_82123880);      // Wear Skin (outfits.cpp: any player)
extern "C" REX_FUNC(__imp__sub_822E3FC8);  // inventory: does the section named *r4 exist (r3 = manager)
extern "C" REX_FUNC(__imp__sub_822E4D88);  // inventory: is that section ready (loaded)
extern "C" REX_FUNC(__imp__sub_8211AF40);  // the front end's decision dispatcher (the game's own)
extern "C" REX_FUNC(__imp__sub_82370FD0);  // element: scale (f1 x, f2 y, f3 z) around its box's centre

namespace outfit_picker {
namespace {

using Bytes = data_patcher::Bytes;
using pure3d::Node;

constexpr int kMaxPlayers = 4;
constexpr int kSkinState = 847;          // front end: SkinSelectionScreen (fig_tree.py)
constexpr int32_t kSkinExitBack = 852;   // its ExitBack -> 486 (the game)
constexpr uint32_t kCoOpState = 0x8259B11C;  // per player: 0 not joined, 1 mask, 2 on foot, 3 entering a mask
constexpr uint32_t kGameGlobal = 0x8259B190;
constexpr uint32_t kFrontEndOf = 52;
constexpr uint32_t kChooser = 0x2230;    // front end: the player Wear Skin is for
constexpr uint32_t kScreenShown = 0x70;  // CSkinScreenAction: the shown entry
constexpr uint32_t kInventoryManager = 0x8259B264;
// THE PREVIEW'S PLACE: the screen builds its preview character at +92 and
// sets its matrix (actor +44: a 4 x 4 of floats, rows at +0 / +16 / +32, the
// position at +48) from a table per page layout (0x82505E7C + 12 x page:
// (1, 0.1, 0) = right of the middle for player 1's page, (-1, 0.1, 0) = left
// for player 2's mirrored one; read in sub_820E0A30 at 0x820E0B54-0x820E0B68).
// Ours is written every frame: above the card of whoever moved last.
constexpr uint32_t kScreenPreview = 92;
constexpr uint32_t kMusicManager = 0x8259B240;  // the music manager (sub_822D9998's r3)
// THE OPENING'S SLIDE (read 2026-10-23 with a per-frame log of the screen's
// fields): CSkinScreenAction +24 = its title panel's LAYER (MenuLayer), +32 =
// the description panel's (CrashLayer; +28 CocoLayer, parked at 1.33). The
// front end's tracks move the layers, not the elements: their transform's x
// (+16 matrix, translation x at +64, in 640-unit widths: 1 = 1440 pixels at
// 1080p) runs -1.33 -> +0.1 -> 0 over ~0.6 s for the title (in from the
// left, overshooting) and the mirror image for the description (from the
// right). The game builds its 3D model only after that (+92 set ~frame 29).
constexpr uint32_t kScreenTitleLayer = 24;
constexpr uint32_t kLayerX = 64;
constexpr float kLayerWidthPx = 1440.0f;

// --- The data: chunk ids, colours ------------------------------------------------
constexpr std::string_view kProject = "GameStart.prj";
constexpr std::string_view kPageFile = "GameStart_SkinSelection.pag";
constexpr std::string_view kPage = "GameStart_SkinSelection";
constexpr std::string_view kInGamePackage = "package\\7a8185b0.p3d";  // the HUD's faces live there
constexpr uint32_t kChunkLayer = 0x18020, kChunkPicture = 0x18022, kChunkText = 0x18023;
constexpr uint32_t kChunkTexture = 0x19000, kChunkSprite = 0x19005, kChunkImage = 0x19001,
                   kChunkImageData = 0x19002;
// Colours as the data stores them (B G R A bytes = 0xAARRGGBB little-endian),
// also what scrooby::SetColour takes. The menus' own: item green, selected
// yellow, title cyan; the players' marker colours; the colour circles'.
constexpr uint32_t kGreen = 0xFF00FF00, kYellow = 0xFFFFF110, kCyan = 0xFF00FFF7, kWhite = 0xFFFFFFFF;
constexpr uint32_t kGlass = 0xDBFFFFFF;    // the cards' glass: white, 86 % opaque (the page's own is 78 %)
constexpr uint32_t kFaceDim = 0x8C9A9A9A;  // the character not picked: grey, see-through
constexpr uint32_t kPlayerColour[kMaxPlayers] = {0xFFFF8C1A, 0xFF2F8CFF, 0xFF3CD23C, 0xFFB45AF0};
constexpr uint8_t kPadAlpha = 0x8C;        // the pads: 55 % opaque
constexpr uint32_t kLeft = 0, kCentre = 4;  // a text's horizontal justify

// --- Element names (one set of four cards: "OP0Glass".."OP3Lock") -------------
constexpr int kSwatches = 8;
std::string Name(int card, std::string_view part) { return fmt::format("OP{}{}", card, part); }

// The data boxes (Scrooby units) every element is laid out with; the code
// stretches them from there (a picture is drawn at its DATA size, a size
// written at run time isn't drawn: findings/31 s.7 -> the transform).
constexpr float kGlassW = 256, kGlassH = 230;   // the page's own square glass (copied as it is)
constexpr float kDisc = 32;                     // circles and faces: 32 x 32
constexpr float kLockW = 50, kLockH = 79;       // the page's lock icon (copied as it is)
// The lock picture's own shape: its sprite (LockedIcon.tga, 64 x 128 pixels)
// draws a 63 x 100 part (the sprite's crop fields), so it's drawn 0.63 wide
// per 1 tall (the mockup's 72 x 144 box stretched it: playtest 2026-10-23).
constexpr float kLockAspect = 63.0f / 100.0f;
constexpr float kLockTall = 110.0f;             // screen pixels at the mockup's model size 1
constexpr float kHingeW = 80, kHingeH = 120;    // the page's hinge (copied as it is)
constexpr float kCogSize = 90;                  // the page's gear ("cog", copied as it is)
constexpr float kTextW = 300, kTextH = 36;      // centred texts (a line of the game's rows)
constexpr float kTagW = 60;                     // "P1": left-aligned

// --- Generated pictures (own art: a circle, drawn here) ------------------------
// A sprite is chunk 0x19005 (name, u32 640, u32 480 = the screen it was made
// for, a byte, u32 crop w / h (0 = whole), u32 image count 1, u32 0) holding
// an image 0x19001 (name, u32 14000, width, height, bits 8, palettized 1,
// alpha 1, format 2) holding its data 0x19002 (u32 size, then a TGA: 8-bit
// colour indices + a 256-colour BGRA palette, rows top to bottom).
constexpr int kCircleSize = 48;  // pixels (the largest circle drawn is ~60 screen pixels at 720p)

// A white disc (index = coverage: palette i = white with alpha i), or one
// with a dark navy rim (indices 0..127 = the rim colour fading out at the
// edge, 128..255 = rim -> white across the inner edge, solid). Tinted per
// element (the game multiplies a picture by its element's colour), so the
// rim stays dark on every colour circle, as the mockup's outlines.
Bytes CircleTga(bool rim) {
  constexpr int n = kCircleSize;
  constexpr uint8_t kNavy[3] = {0x14, 0x2F, 0x5E};  // R G B (the menus' text outline)
  Bytes t(18 + 4 * 256 + n * n, 0);
  t[1] = 1;   // colour map
  t[2] = 1;   // uncompressed, colour-mapped
  t[5] = 0x00; t[6] = 0x01;  // 256 entries
  t[7] = 32;  // BGRA entries
  t[12] = n; t[14] = n;
  t[16] = 8;
  t[17] = 0x20;  // rows top to bottom
  uint8_t* pal = &t[18];
  for (int i = 0; i < 256; ++i) {
    uint8_t r = 255, g = 255, b = 255, a = uint8_t(i);
    if (rim) {
      if (i < 128) {
        r = kNavy[0]; g = kNavy[1]; b = kNavy[2];
        a = uint8_t(i * 255 / 127);
      } else {
        const float k = float(i - 128) / 127.0f;
        r = uint8_t(std::lround(kNavy[0] + (255 - kNavy[0]) * k));
        g = uint8_t(std::lround(kNavy[1] + (255 - kNavy[1]) * k));
        b = uint8_t(std::lround(kNavy[2] + (255 - kNavy[2]) * k));
        a = 255;
      }
    }
    pal[4 * i + 0] = b; pal[4 * i + 1] = g; pal[4 * i + 2] = r; pal[4 * i + 3] = a;
  }
  uint8_t* px = pal + 4 * 256;
  const float c = (n - 1) / 2.0f, outer = n / 2.0f - 0.5f, inner = outer - n * 0.09f;
  for (int y = 0; y < n; ++y) {
    for (int x = 0; x < n; ++x) {
      const float r = std::hypot(x - c, y - c);
      const float cover = std::clamp(outer - r + 0.5f, 0.0f, 1.0f);   // inside the disc
      const float fill = std::clamp(inner - r + 0.5f, 0.0f, 1.0f);    // inside the rim
      uint8_t v;
      if (!rim) v = uint8_t(std::lround(cover * 255));
      else if (fill > 0) v = uint8_t(128 + std::lround(fill * 127));
      else v = uint8_t(std::lround(cover * 127));
      px[y * n + x] = v;
    }
  }
  return t;
}

// A sprite chunk holding `image` (a 0x19001 node) under `name`; the header
// fields copied from `model` (one of the package's own sprites).
Node SpriteOf(const Node& model, std::string_view name, Node image) {
  Node s;
  s.id = kChunkSprite;
  s.data = model.data;
  pure3d::SetNodeName(s, name);
  pure3d::SetNodeName(image, name);
  s.children = {std::move(image)};
  return s;
}
// Our circle as a sprite (image fields copied from the model's image).
Node CircleSprite(const Node& model, std::string_view name, bool rim) {
  Node image = model.children.at(0);
  const size_t e = pure3d::NameEnd(image);
  pure3d::SetNodeLe32(image, e + 4, kCircleSize);
  pure3d::SetNodeLe32(image, e + 8, kCircleSize);
  Node pixels;
  pixels.id = kChunkImageData;
  const Bytes tga = CircleTga(rim);
  pure3d::PutLe32(pixels.data, uint32_t(tga.size()));
  pixels.data.insert(pixels.data.end(), tga.begin(), tga.end());
  image.children = {std::move(pixels)};
  return SpriteOf(model, name, std::move(image));
}
constexpr std::string_view kCircleSprite = "OP_Circle.tga";
constexpr std::string_view kRimSprite = "OP_CircleRim.tga";
constexpr std::string_view kFaceSprite[2] = {"OP_MugshotCrash.tga", "OP_MugshotCoco.tga"};
constexpr std::string_view kFaceTexture[2] = {"MugshotCrash.tga", "MugshotCoco.tga"};

// --- Building the layer -------------------------------------------------------
// Element fields after the name (findings/30, tools/scrooby_dump.py): u32
// version, i32 x, y, w, h, u8; texts then u32 horizontal / vertical justify,
// u32 colour; pictures u32, u32, u32 colour, u32, u32, u32 frame count and
// the frames' sprite names (one here: the last field).
void SetBox(Node& n, float x, float y, float w, float h) {
  const size_t e = pure3d::NameEnd(n);
  pure3d::SetNodeLe32(n, e + 4, uint32_t(int32_t(std::lround(x))));
  pure3d::SetNodeLe32(n, e + 8, uint32_t(int32_t(std::lround(y))));
  pure3d::SetNodeLe32(n, e + 12, uint32_t(int32_t(std::lround(w))));
  pure3d::SetNodeLe32(n, e + 16, uint32_t(int32_t(std::lround(h))));
}
void SetJustify(Node& text, uint32_t horizontal) { pure3d::SetNodeLe32(text, pure3d::NameEnd(text) + 21, horizontal); }
void SetColourField(Node& element, uint32_t colour) { pure3d::SetNodeLe32(element, pure3d::NameEnd(element) + 29, colour); }
bool SetSpriteName(Node& picture, std::string_view sprite) {
  const size_t frames = pure3d::NameEnd(picture) + 41;  // the frame count
  if (picture.data.size() < frames + 4 || pure3d::NodeLe32(picture, frames) != 1) return false;
  picture.data.resize(frames + 4);
  const Bytes s = pure3d::PaddedString(sprite);
  picture.data.insert(picture.data.end(), s.begin(), s.end());
  return true;
}
Node* Child(Node& n, uint32_t id, std::string_view name) {
  for (Node& k : n.children) {
    if (k.id == id && pure3d::NodeName(k) == name) return &k;
  }
  return nullptr;
}
Node* Find(Node& page, uint32_t id, std::string_view name) {
  for (Node& layer : page.children) {
    if (Node* n = Child(layer, id, name)) return n;
  }
  return nullptr;
}

std::atomic<bool> g_page_patched{false}, g_tree_patched{false};

// The page with our layer added (on top: appended last). Element order =
// drawing order: glass, pad, faces, circles (the ring under the picked one),
// texts, arrows, lock.
bool PatchPage(Node& page) {
  Node* glass = Find(page, kChunkPicture, "CrashPanel");
  Node* text = Find(page, kChunkText, "Player1");
  Node* lock = Find(page, kChunkPicture, "CrashLockedIcon");
  Node* hinge = Find(page, kChunkPicture, "MenuHinge");
  Node* cog = Find(page, kChunkPicture, "cog");
  Node* menu_layer = nullptr;
  for (Node& l : page.children) {
    if (l.id == kChunkLayer && pure3d::NodeName(l) == "MenuLayer") menu_layer = &l;
  }
  if (!glass || !text || !lock || !hinge || !cog || !menu_layer || text->children.size() != 1) return false;
  const Node empty = text->children[0];  // the menus' bank "_Empty": our code writes the strings
  Node layer = *menu_layer;
  pure3d::SetNodeName(layer, "OutfitPickerLayer");
  layer.children.clear();
  bool ok = true;
  auto add_picture = [&](const Node& from, const std::string& name, std::string_view sprite, float w, float h) {
    Node p = from;
    pure3d::SetNodeName(p, name);
    if (!sprite.empty()) ok = ok && SetSpriteName(p, sprite);
    SetBox(p, 0, 0, w, h);
    SetColourField(p, kWhite);
    layer.children.push_back(std::move(p));
  };
  auto add_text = [&](const std::string& name, float w, uint32_t justify) {
    Node t = *text;
    pure3d::SetNodeName(t, name);
    SetBox(t, 0, 0, w, kTextH);
    SetJustify(t, justify);
    SetColourField(t, kCyan);
    t.children = {empty};
    layer.children.push_back(std::move(t));
  };
  for (int k = 0; k < kMaxPlayers; ++k) {
    add_picture(*glass, Name(k, "Glass"), "", kGlassW, kGlassH);
    add_picture(*lock, Name(k, "Pad"), kCircleSprite, kDisc, kDisc);
    add_picture(*lock, Name(k, "Face0"), kFaceSprite[0], kDisc, kDisc);
    add_picture(*lock, Name(k, "Face1"), kFaceSprite[1], kDisc, kDisc);
    add_picture(*lock, Name(k, "Ring"), kCircleSprite, kDisc, kDisc);
    add_picture(*lock, Name(k, "Hover"), kCircleSprite, kDisc, kDisc);  // (the mouse's circle)
    for (int s = 0; s < kSwatches; ++s) add_picture(*lock, Name(k, fmt::format("Swatch{}", s)), kRimSprite, kDisc, kDisc);
    add_text(Name(k, "Tag"), kTagW, kLeft);
    add_text(Name(k, "Outfit"), kTextW, kCentre);
    add_text(Name(k, "Ready"), kTextW, kCentre);
    add_text(Name(k, "Prompt"), kTextW, kCentre);
    add_text(Name(k, "ArrowL"), kTagW, kCentre);
    add_text(Name(k, "ArrowR"), kTagW, kCentre);
    add_picture(*lock, Name(k, "Lock"), "", kLockW, kLockH);
  }
  // (1 player) the gear and the hinge of the game's title panel, over the
  // glass as in the original (its MenuLayer draws cog, panel, hinge).
  add_picture(*cog, "OPCog2", "", kCogSize, kCogSize);  // (the second gear, under the first)
  add_picture(*cog, "OPCog", "", kCogSize, kCogSize);
  add_picture(*hinge, "OPHinge", "", kHingeW, kHingeH);
  if (!ok) return false;
  page.children.push_back(std::move(layer));
  return true;
}

// The faces: the in-game package's textures, their image wrapped as sprites.
bool CutFaces(const Node& model, std::vector<Bytes>* sprites) {
  Bytes ingame;
  std::string error;
  if (!data_patcher::ReadArchiveFile(kInGamePackage, &ingame, &error)) return false;
  pure3d::Chunk root;
  std::vector<pure3d::Chunk> top;
  if (!pure3d::Parse(ingame, &root, &top)) return false;
  int found = 0;
  for (int f = 0; f < 2; ++f) {
    for (const pure3d::Chunk& c : top) {
      if (c.id != kChunkTexture || pure3d::NameOf(ingame, c) != kFaceTexture[f]) continue;
      Node texture = pure3d::ToNode(ingame, c);
      for (Node& image : texture.children) {
        if (image.id != kChunkImage) continue;
        sprites->push_back(pure3d::ToBytes(SpriteOf(model, kFaceSprite[f], image)));
        ++found;
        break;
      }
      break;
    }
  }
  return found == 2;
}

// The same steps as options_page.cpp's RebuildProject, for one page; our
// sprites go in right before the project (pages find them by name).
bool PatchPackage(const Bytes& original, Bytes* patched) {
  using pure3d::Chunk;
  Chunk root;
  std::vector<Chunk> top;
  if (!pure3d::Parse(original, &root, &top)) return false;
  // A sprite of the package's own to copy header fields from (the cog).
  Node model;
  for (const Chunk& c : top) {
    if (c.id == kChunkSprite && pure3d::NameOf(original, c) == "cog.tga") model = pure3d::ToNode(original, c);
  }
  if (model.id != kChunkSprite || model.children.empty() || model.children[0].id != kChunkImage) return false;
  std::vector<Bytes> sprites = {pure3d::ToBytes(CircleSprite(model, kCircleSprite, false)),
                                pure3d::ToBytes(CircleSprite(model, kRimSprite, true))};
  if (!CutFaces(model, &sprites)) {
    REXLOG_WARN("Outfit picker: the faces aren't in {}: the chest keeps the game's screen", kInGamePackage);
    return false;
  }
  Bytes body;
  bool edited = false;
  for (const Chunk& c : top) {
    if (c.id != pure3d::kChunkProject || pure3d::NameOf(original, c) != kProject) {
      const Bytes chunk = pure3d::Copy(original, c);
      body.insert(body.end(), chunk.begin(), chunk.end());
      continue;
    }
    for (const Bytes& s : sprites) body.insert(body.end(), s.begin(), s.end());
    Bytes children;
    for (const Chunk& k : pure3d::ChildrenOf(original, c)) {
      Bytes chunk;
      if (k.id == pure3d::kChunkPage && pure3d::NameOf(original, k) == kPageFile) {
        Node page = pure3d::ToNode(original, k);
        if (!PatchPage(page)) {
          REXLOG_WARN("Outfit picker: {} isn't laid out as expected: the chest keeps the game's screen", kPageFile);
          return false;
        }
        chunk = pure3d::ToBytes(page);
        edited = true;
      } else {
        chunk = pure3d::Copy(original, k);
      }
      children.insert(children.end(), chunk.begin(), chunk.end());
    }
    const Bytes project = pure3d::WithChildren(original, c, children);
    body.insert(body.end(), project.begin(), project.end());
  }
  if (!edited) return false;
  *patched = pure3d::WithChildren(original, root, body);
  if (!g_page_patched.exchange(true)) REXLOG_INFO("Outfit picker: player cards added to the chest's page");
  return true;
}

// State 847 is checked to be the one we drive (its decision becomes ours);
// the tree itself is left as it is.
bool CheckTree(const Bytes& original, Bytes* patched) {
  (void)patched;
  fight_tree::Tree tree;
  if (!tree.Load(original)) return false;
  const bool ok = kSkinState <= tree.Count() && tree.NameOf(kSkinState) == "SkinSelectionScreen" &&
                  kSkinExitBack <= tree.Count() && tree.NameOf(kSkinExitBack) == "ExitBack" &&
                  tree.TargetOf(kSkinExitBack) == 486;
  if (!ok) REXLOG_WARN("Outfit picker: front end node {} isn't SkinSelectionScreen: picker off", kSkinState);
  g_tree_patched = ok;
  return false;  // nothing changed
}

// --- What a player can pick ---------------------------------------------------
// Character -> its looks (port outfit numbers, outfits.h) -> for Crash's own
// look, a colour (the fur: orange is the original, white the original Carbon).
// Every look and colour can be LOOKED AT; only unlocked ones can be worn.
enum Character { kCrash = 0, kCoco = 1 };
struct Look {
  int outfit;  // for Crash's own look: the orange one (colours replace it)
  const char16_t* name;
};
constexpr Look kCrashLooks[] = {{0, u"Normal"},     {1, u"Greeny"},    {2, u"Bluey"},
                                {3, u"Reddy"},      {4, u"Snipe"},     {5, u"Ratcicle"},
                                {6, u"Magmadon"},   {7, u"Skeleton"},  {8, u"Spike"}};
constexpr Look kCocoLooks[] = {{9, u"Coco"}, {10, u"Old School"}};
struct Colour {
  int outfit;
  uint32_t tint;  // the circle's colour (the mockup's swatches)
};
// The mockup's order: orange, white, red, yellow, green, blue, purple, pink.
constexpr Colour kColours[kSwatches] = {
    {0, 0xFFF08A24},                     {outfits::kCarbon, 0xFFF2F2EE},     {outfits::kCrashRed, 0xFFD8312A},
    {outfits::kCrashYellow, 0xFFF2D230}, {outfits::kCrashGreen, 0xFF5BC236}, {outfits::kCrashBlue, 0xFF4A7BE0},
    {outfits::kCrashPurple, 0xFF9A5AD8}, {outfits::kCrashPink, 0xFFF28CC0}};
constexpr int kCrashLookCount = int(std::size(kCrashLooks)), kCocoLookCount = int(std::size(kCocoLooks));

struct Pick {
  int character = kCrash, look = 0, colour = 0;
};
int LookCount(int character) { return character == kCoco ? kCocoLookCount : kCrashLookCount; }
const Look& LookOf(const Pick& k) { return k.character == kCoco ? kCocoLooks[k.look] : kCrashLooks[k.look]; }
bool HasColour(const Pick& k) { return k.character == kCrash && k.look == 0; }
int OutfitOfPick(const Pick& k) { return HasColour(k) ? kColours[k.colour].outfit : LookOf(k).outfit; }
Pick PickOfOutfit(int outfit) {
  Pick k;
  for (int i = 0; i < kCocoLookCount; ++i) {
    if (kCocoLooks[i].outfit == outfit) return {kCoco, i, 0};
  }
  for (int i = 0; i < kSwatches; ++i) {
    if (kColours[i].outfit == outfit) return {kCrash, 0, i};
  }
  for (int i = 0; i < kCrashLookCount; ++i) {
    if (kCrashLooks[i].outfit == outfit) return {kCrash, i, 0};
  }
  return k;
}
bool Available(const Pick& k) { return outfits::OutfitAvailable(OutfitOfPick(k)); }

// The rows: 0 character, 1 outfit, 2 colour (Crash's Normal look only).
constexpr int kRows = 3;
constexpr int kRowCharacter = 0, kRowOutfit = 1, kRowColour = 2;

// The next value of `row` in direction d (wraps). Switching the character
// lands on its first unlocked look (its first look when none is).
void Step(Pick& k, int row, int d) {
  if (row == kRowCharacter) {
    k.character = 1 - k.character;
    k.look = 0;
    for (int i = 0; i < LookCount(k.character) && !Available(k); ++i) k.look = (k.look + 1) % LookCount(k.character);
    if (!Available(k)) k.look = 0;
    return;
  }
  const int count = row == kRowOutfit ? LookCount(k.character) : kSwatches;
  int& v = row == kRowOutfit ? k.look : k.colour;
  v = ((v + d) % count + count) % count;
}

// --- The picker's state (game main thread) -----------------------------------
using Clock = std::chrono::steady_clock;
struct Player {
  bool joined = false;
  Pick pick;
  int row = kRowOutfit;
  bool ready = false;
  bool waiting = false;      // in the wear queue
  uint16_t held = 0;         // buttons held at the last update (edges)
  int stick_dir = 0;         // -1 / 0 / 1 per axis direction held (vertical*3 + horizontal)
  Clock::time_point repeat_at{};
};
// What a click can hit (THE MOUSE, below). kRow = a whole row's band: not
// clickable, it only says which row the pointer is on (the outfit row's
// arrows show there).
enum HitKind { kHitNone = 0, kHitFace, kHitSwatch, kHitArrowL, kHitArrowR, kHitWear, kHitChange, kHitBack, kHitRow };
struct HitBox {
  int kind, index;   // index: the face / circle number, the arrows' row, the row
  float x0, y0, x1, y1;
};

// The elements of one card (and the page's), found once per page.
struct Card {
  uint32_t glass = 0, pad = 0, face[2] = {}, ring = 0, hover = 0, swatch[kSwatches] = {};
  uint32_t tag = 0, outfit = 0, ready = 0, prompt = 0, arrow_l = 0, arrow_r = 0, lock = 0;
};
struct State {
  bool open = false;
  uint32_t screen = 0;
  int frames = 0;
  int count = 1;             // joined players (the layout)
  int focus = 0;             // whose pick the preview shows
  Player players[kMaxPlayers];
  std::deque<int> queue;     // players waiting to wear their pick
  Clock::time_point queue_since{};
  Clock::time_point opened_at{};
  bool close = false;        // the decision takes ExitBack at its next ask
  std::vector<std::pair<uint32_t, std::u16string>> shown;  // texts set (only changes are written)
  // THE MOUSE (the keyboard's player only, on their own card): what can be
  // clicked, as last drawn (screen pixels at 1080p); what the pointer is over.
  int mouse_player = -1;     // -1 = the keyboard plays nobody here
  bool mouse_on = false;     // the pointer moved since that player last used keys / a pad
  uint32_t mouse_moves = 0;  // the driver's pointer move counter, last seen
  std::vector<struct HitBox> hits;
  int hover_kind = 0, hover_index = -1, hover_row = -1;
  uint32_t bar_colour[2] = {};  // the prompt bar's Back / Wear text colours as found (1 player)
  // THE SLIDE (see Slide below): the opening's layer positions, frame by
  // frame; settled = the opening is over; exit_step >= 0 = closing.
  std::vector<float> curve;
  bool settled = false;
  int exit_step = -1;
  float slide = 0;           // this frame's offset of a LEFT-side panel, screen pixels
  uint32_t page = 0;         // the page the elements below belong to
  uint32_t hinge = 0, cog = 0, cog2 = 0;
  Card cards[kMaxPlayers];
} g;
std::atomic<bool> g_mute{false};
std::atomic<int64_t> g_closed_ms{-100000};
std::atomic<bool> g_quiet_until_up[kMaxPlayers] = {};  // after closing, per controller
int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

uint8_t* Guest(uint32_t a) { return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(a); }
uint32_t Read32(uint32_t a) {
  const uint8_t* p = Guest(a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Write32(uint32_t a, uint32_t v) {
  uint8_t* p = Guest(a);
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
float ReadF(uint32_t a) {
  const uint32_t v = Read32(a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void WriteF(uint32_t a, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  Write32(a, v);
}

// A text's string, written only when it changes.
void Put(PPCContext& ctx, uint8_t* base, uint32_t text, std::u16string_view value) {
  if (!text) return;
  for (auto& [t, s] : g.shown) {
    if (t == text) {
      if (s == value) return;
      s = value;
      scrooby::SetText(ctx, base, text, value);
      return;
    }
  }
  g.shown.emplace_back(text, std::u16string(value));
  scrooby::SetText(ctx, base, text, value);
}
std::u16string U16(std::string_view s) { return std::u16string(s.begin(), s.end()); }

// --- Input ----------------------------------------------------------------------
// Player p's buttons and left stick, from the input system (not the game's
// muted view), plus the keyboard if it plays on that socket.
struct Pad {
  uint16_t buttons = 0;
  float x = 0, y = 0;
};
Pad ReadPad(int p) {
  Pad pad;
  auto* input = static_cast<rex::input::InputSystem*>(rex::Runtime::instance()->input_system());
  if (input) {
    rex::input::X_INPUT_STATE state{};
    if (input->GetState(uint32_t(p), &state) == 0) {
      pad.buttons = uint16_t(state.gamepad.buttons);
      pad.x = float(int16_t(state.gamepad.thumb_lx)) / 32767.0f;
      pad.y = float(int16_t(state.gamepad.thumb_ly)) / 32767.0f;
    }
  }
  const auto* assignment = kbm::PlayerAssignment::Get();
  if (auto* driver = kbm::KeyboardMouseDriver::Get(); driver && assignment && assignment->KeyboardAnswersFor(uint32_t(p))) {
    const rex::input::X_INPUT_GAMEPAD k = driver->last_pad();
    pad.buttons |= uint16_t(k.buttons);
    if (std::fabs(float(int16_t(k.thumb_lx))) > std::fabs(pad.x * 32767.0f)) pad.x = float(int16_t(k.thumb_lx)) / 32767.0f;
    if (std::fabs(float(int16_t(k.thumb_ly))) > std::fabs(pad.y * 32767.0f)) pad.y = float(int16_t(k.thumb_ly)) / 32767.0f;
  }
  return pad;
}

constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kLeftButton = 0x0004, kRightButton = 0x0008;
constexpr uint16_t kA = 0x1000, kB = 0x2000;
constexpr float kStickOn = 0.55f;
constexpr auto kRepeatFirst = std::chrono::milliseconds(380), kRepeatNext = std::chrono::milliseconds(140);

// Directions this frame (a press, or a held one repeating): +1 / -1 per axis.
void Directions(Player& pl, const Pad& pad, int* dx, int* dy) {
  int h = 0, v = 0;
  if ((pad.buttons & kLeftButton) || pad.x < -kStickOn) h = -1;
  if ((pad.buttons & kRightButton) || pad.x > kStickOn) h = 1;
  if ((pad.buttons & kUp) || pad.y > kStickOn) v = -1;   // up = the row above (row - 1)
  if ((pad.buttons & kDown) || pad.y < -kStickOn) v = 1;
  const int dir = v * 3 + h;
  const Clock::time_point now = Clock::now();
  *dx = *dy = 0;
  if (dir == 0) {
    pl.stick_dir = 0;
    return;
  }
  if (dir != pl.stick_dir) {
    pl.stick_dir = dir;
    pl.repeat_at = now + kRepeatFirst;
  } else if (now >= pl.repeat_at) {
    pl.repeat_at = now + kRepeatNext;
  } else {
    return;
  }
  *dx = h;
  *dy = v;
}

// --- The preview and the wear queue --------------------------------------------
bool PackageReady(PPCContext& ctx, uint8_t* base) {
  const uint32_t inventory = Read32(kInventoryManager);
  const uint32_t key = outfits::ShownTemplateKey();
  if (!inventory) return false;
  if ((scrooby::Call(__imp__sub_822E3FC8, ctx, base, inventory, key) & 0xFF) == 0) return true;  // no section: nothing to wait for
  return (scrooby::Call(__imp__sub_822E4D88, ctx, base, inventory, key) & 0xFF) != 0;
}

void ShowInPreview(PPCContext& ctx, uint8_t* base, int outfit) {
  const int index = outfits::ListIndexOf(outfit);
  if (index < 0 || !g.screen || outfits::ShownOutfit() == outfit) return;
  Write32(g.screen + kScreenShown, uint32_t(index));
  // As the screen's own input does when the player picks another entry
  // (sub_820E15F8, read 2026-10-23): first sub_820E1180 = DESTROY the preview
  // (its actor +92, controller +96, animation +100; +92 = 0), then "show
  // entry" sub_820E1228 (r5 = 0: only the texts it writes differ) = let go of
  // the old outfit's package (sub_822E4650) and ask for the new one
  // (sub_822E44B8). The screen's per-frame code (sub_820E1C70 from
  // 0x820E1CE8) then BUILDS a new model (sub_820E0A30) as soon as that
  // package is ready. (The first version called "show entry" with r5 = 1
  // thinking it built the model: it never did; the old model stayed, with
  // only the per-frame hair colour following: Greeny looked like Normal, Red
  // Crash like Carbon with red hair.)
  scrooby::Call(__imp__sub_820E1180, ctx, base, g.screen);
  scrooby::Call(sub_820E1228, ctx, base, g.screen, uint32_t(index), 0);
}

// The first queued player's pick: in the preview, its package loaded, then worn.
void RunQueue(PPCContext& ctx, uint8_t* base) {
  if (g.queue.empty()) return;
  const int p = g.queue.front();
  Player& pl = g.players[p];
  const int outfit = OutfitOfPick(pl.pick);
  auto done = [&](const char* how) {
    pl.waiting = false;
    g.queue.pop_front();
    g.queue_since = Clock::now();
    REXLOG_INFO("Outfit picker: player {} {} outfit {}", p + 1, how, outfit);
  };
  if (outfit == outfits::OutfitOf(p)) {
    done("keeps");
    return;
  }
  if (outfits::ShownOutfit() != outfit) {
    g.focus = p;
    ShowInPreview(ctx, base, outfit);
    return;
  }
  if (!PackageReady(ctx, base)) {
    if (Clock::now() - g.queue_since > std::chrono::seconds(15)) {
      pl.ready = false;
      done("timed out loading");
    }
    return;
  }
  const uint32_t front_end = Read32(Read32(kGameGlobal) + kFrontEndOf);
  Write32(front_end + kChooser, uint32_t(p));
  scrooby::Call(sub_82123880, ctx, base, 0);
  done("wears");
}

// =============================================================================
// THE LAYOUT (the mockup's numbers: 1920 x 1080 screen pixels, y DOWN)
// =============================================================================
// Scrooby's canvas is 640 x 480 units, y UP, drawn 4:3 in the middle of the
// 16:9 picture (findings/30): at 1080p one unit = 2.25 pixels and x = 0 is
// pixel 240. So every place below is written in the mockup's pixels and
// turned into units here.
float UnitX(float px) { return (px - 240.0f) / 2.25f; }
float UnitY(float py) { return (1080.0f - py) / 2.25f; }
float Units(float px) { return px / 2.25f; }

// A row of cards (2-4 players), as the mockup's rowLayout(): card width,
// the gap between cards, the model's size (1 = the 1-player page's), the
// font size (CSS pixels of the mockup's font), the face size, the row
// pitch, the circles' size and gap, the arrows' size and their gap to the
// faces. Cards: top 470, bottom 968; the model's feet 150 below the top.
struct RowLayout {
  float w, gap, model, font, face, row, swatch, swatch_gap, arrow, arrow_gap;
};
constexpr RowLayout kRowLayouts[3] = {
    {600, 90, 1.25f, 62, 76, 84, 44, 14, 46, 18},  // 2 players
    {470, 50, 1.05f, 58, 70, 80, 38, 11, 42, 14},  // 3
    {375, 26, 0.90f, 50, 62, 74, 32, 8, 38, 10},   // 4
};
constexpr float kCardTop = 470, kCardBottom = 968, kFeetDrop = 150;
constexpr float kOneModelSize = 1.6f;  // the mockup's 1-player model (its scale; the cards' are relative to it)

// TEXT SIZE: the mockup sets the game's font by CSS pixel size; the game's
// text at scale 1 (Titans_Small, a menu row) is about this many of them.
// TEXT PLACE: the mockup gives each text line's TOP; the game's text hangs
// from its box's top and the box is scaled around its centre, so a line's
// box centre = top + this x the font size.
constexpr float kFontAtScale1 = 62.0f;
constexpr float kLineMiddle = 0.5f;

// Where the 3D model stands: screen pixels -> the preview's world position.
// Measured from photos with the model at known places (2026-10-23, the tune
// flag below via the console's `flag`): world x 0 = the screen's middle,
// x -0.958 -> 270 px left of it; feet at world y 0.1 (the game's own height)
// -> screen y 825, y -0.233 -> 915, y 0.767 -> 645; the model at scale 1 is
// ~460 px tall (shoes to hair), the mockup's 1-player model ~400; its shoes
// end ~17 px below its origin.
struct ModelMap {
  float px_per_x = 282.0f;  // screen pixels per world unit, across
  float px_per_y = 270.0f;  // ... and up
  float feet_y0 = 808.0f;   // the shoes' screen y at world y = 0.1 (the game's own height)
  float size = 0.87f;       // the model's scale at the 1-player size
};
ModelMap Map() {
  ModelMap m;
  const std::string tune = REXCVAR_GET(outfit_picker_tune);
  if (!tune.empty()) {
    float v[4] = {m.px_per_x, m.px_per_y, m.feet_y0, m.size};
    size_t at = 0;
    for (int i = 0; i < 4 && at <= tune.size(); ++i) {
      const size_t comma = tune.find(',', at);
      const std::string part = tune.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
      if (!part.empty()) v[i] = std::strtof(part.c_str(), nullptr);
      if (comma == std::string::npos) break;
      at = comma + 1;
    }
    m = {v[0], v[1], v[2], v[3]};
  }
  return m;
}

// --- Placing elements ------------------------------------------------------------
// Element words (options_menu.cpp, read 2026-10-11): +92 / +96 the box's
// bottom-left x / y as a fraction of the 640-unit width (units x the
// constant at 0x8204701C), +88 / +90 its width / height (u16 units), +16 its
// transform; a scale is built around the box's centre at the call.
constexpr uint32_t kElementSize = 88, kElementX = 92, kElementY = 96;
constexpr uint32_t kUnitToPosition = 0x8204701C;

void Hide(uint8_t* base, uint32_t e) { scrooby::SetVisible(base, e, false); }

// A picture centred on (cx, cy), drawn w x h (screen pixels); its data box
// is own_w x own_h units.
void PlacePicture(PPCContext& ctx, uint8_t* base, uint32_t e, float cx, float cy, float w, float h, float own_w,
                  float own_h, uint32_t colour) {
  if (!e) return;
  const float unit = ReadF(kUnitToPosition);
  WriteF(e + kElementX, (UnitX(cx) - own_w / 2) * unit);
  WriteF(e + kElementY, (UnitY(cy) - own_h / 2) * unit);
  scrooby::SetScale(ctx, base, e, 1.0f);  // a fresh transform around the new box
  scrooby::CallF(__imp__sub_82370FD0, ctx, base, e, Units(w) / own_w, Units(h) / own_h, 1.0);
  scrooby::SetColour(ctx, base, e, colour);
  scrooby::SetVisible(base, e, true);
}
void PlaceDisc(PPCContext& ctx, uint8_t* base, uint32_t e, float cx, float cy, float w, float h, uint32_t colour) {
  PlacePicture(ctx, base, e, cx, cy, w, h, kDisc, kDisc, colour);
}

// A text whose line's top is at `top` (screen pixels), centred on cx (or,
// left-aligned, starting at cx), the mockup's font size `font`; `box` = the
// width its data box was made with (units). `turn` = degrees (an arrow).
void PlaceText(PPCContext& ctx, uint8_t* base, uint32_t e, std::u16string_view s, float cx, float top, float font,
               uint32_t colour, bool left = false, float box = kTextW, float turn = 0) {
  if (!e) return;
  const float scale = font / kFontAtScale1;
  const float cy = top + kLineMiddle * font;
  // Scaled around the box's centre: a left-aligned text's start moves in by
  // (1 - scale) x half the box; the box is moved out by as much.
  const float x = left ? UnitX(cx) - box / 2 * (1 - scale) : UnitX(cx) - box / 2;
  const float unit = ReadF(kUnitToPosition);
  WriteF(e + kElementX, x * unit);
  WriteF(e + kElementY, (UnitY(cy) - kTextH / 2) * unit);
  scrooby::SetScale(ctx, base, e, scale);
  if (turn != 0) scrooby::Rotate(ctx, base, e, turn);
  Put(ctx, base, e, s);
  scrooby::SetColour(ctx, base, e, colour);
  scrooby::SetVisible(base, e, true);
}

// The red arrows (PlaceArrowsFor, below): the font's U+00BB is a filled red
// arrow pointing RIGHT (U+00BA is an outlined one pointing left:
// findings/31; photo 2026-10-23), so the left arrow is U+00BB turned half a
// turn.

// The mockup's pulse: 1 -> 1.06 -> 1 each second.
float Pulse() {
  const double t = std::chrono::duration<double>(Clock::now() - g.opened_at).count();
  return 1.0f + 0.03f * float(1.0 - std::cos(t * 2.0 * 3.14159265358979));
}

// A player's prompt with the button pictures of THEIR device (prompt_icons:
// player slots; ± = A, ² = B as the menus draw them).
std::u16string ForPlayer(std::u16string text, int p) {
  for (char16_t& c : text) c = prompt_icons::MenuGlyph(c);
  return prompt_icons::TextForPlayer(std::move(text), p);
}

void HideCard(uint8_t* base, const Card& c) {
  for (uint32_t e : {c.glass, c.pad, c.face[0], c.face[1], c.ring, c.hover, c.tag, c.outfit, c.ready, c.prompt, c.arrow_l,
                     c.arrow_r, c.lock}) {
    Hide(base, e);
  }
  for (uint32_t e : c.swatch) Hide(base, e);
}

// --- The mouse's spots ---------------------------------------------------------------
// Drawing records the mouse player's clickable spots (centre + size, screen
// pixels); the next frame's AfterUpdate finds the one under the pointer.
void AddHit(int p, int kind, int index, float cx, float cy, float w, float h) {
  if (p != g.mouse_player) return;
  g.hits.push_back({kind, index, cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2});
}
// Is this spot of player p's the one under the pointer (= highlighted)?
bool Hovered(int p, int kind, int index = -1) {
  return p == g.mouse_player && g.mouse_on && g.hover_kind == kind && (index < 0 || g.hover_index == index);
}
// The row whose arrows show: the mouse player's pointer row (the outfit row
// has no other spot to click), else the selected row.
int ArrowRow(int p, const Player& pl) {
  return p == g.mouse_player && g.mouse_on && g.hover_row >= 0 ? g.hover_row : pl.row;
}
// The hover ring behind a circle, or hidden.
void PlaceHoverRing(PPCContext& ctx, uint8_t* base, const Card& c, int p, float sx[kSwatches], float sy[kSwatches],
                    float d) {
  for (int s2 = 0; s2 < kSwatches; ++s2) {
    if (Hovered(p, kHitSwatch, s2)) {
      PlaceDisc(ctx, base, c.hover, sx[s2], sy[s2], d + 16, d + 16, 0xC8FFF110);  // see-through yellow
      return;
    }
  }
  Hide(base, c.hover);
}
// Hovered arrows grow (the one about to be clicked).
float ArrowSize(int p, int row, bool left, float size) {
  return Hovered(p, left ? kHitArrowL : kHitArrowR, row) ? size * 1.3f : size;
}
void PlaceArrowsFor(PPCContext& ctx, uint8_t* base, const Card& c, int p, int row, float cx, float cy, float dx,
                    float size) {
  const float l = ArrowSize(p, row, true, size), r = ArrowSize(p, row, false, size);
  PlaceText(ctx, base, c.arrow_l, u"\u00BB", cx - dx, cy - kLineMiddle * l, l, kWhite, false, kTagW, 180.0f);
  PlaceText(ctx, base, c.arrow_r, u"\u00BB", cx + dx, cy - kLineMiddle * r, r, kWhite, false, kTagW);
  AddHit(p, kHitArrowL, row, cx - dx, cy, size * 1.4f, size * 1.4f);
  AddHit(p, kHitArrowR, row, cx + dx, cy, size * 1.4f, size * 1.4f);
}

// --- The game's own elements ------------------------------------------------------
constexpr const char* kGameElements[] = {"cog", "MenuPanel", "CrashMenuText", "CocoMenuText", "MenuHinge",
                                         "LeftArrow", "RightArrow", "CrashPanel", "cog2", "CrashDescription",
                                         "CrashHinge", "CrashLockedIcon", "CrashLoading", "cog3", "CocoPanel",
                                         "CocoDescription", "CocoHinge", "CocoLockedIcon", "CocoLoading",
                                         "Player1", "Player2"};

// The elements, found again only when the page object changes.
bool FindElements(PPCContext& ctx, uint8_t* base, uint32_t page) {
  if (g.page == page) return true;
  g.page = page;
  g.shown.clear();
  auto pic = [&](const std::string& n) { return scrooby::FindPicture(ctx, base, page, n); };
  auto txt = [&](const std::string& n) { return scrooby::FindText(ctx, base, page, n); };
  g.hinge = pic("OPHinge");
  g.cog = pic("OPCog");
  g.cog2 = pic("OPCog2");
  bool ok = g.hinge && g.cog && g.cog2;
  for (int k = 0; k < kMaxPlayers; ++k) {
    Card& c = g.cards[k];
    c.glass = pic(Name(k, "Glass"));
    c.pad = pic(Name(k, "Pad"));
    c.face[0] = pic(Name(k, "Face0"));
    c.face[1] = pic(Name(k, "Face1"));
    c.ring = pic(Name(k, "Ring"));
    c.hover = pic(Name(k, "Hover"));
    for (int s = 0; s < kSwatches; ++s) c.swatch[s] = pic(Name(k, fmt::format("Swatch{}", s)));
    c.tag = txt(Name(k, "Tag"));
    c.outfit = txt(Name(k, "Outfit"));
    c.ready = txt(Name(k, "Ready"));
    c.prompt = txt(Name(k, "Prompt"));
    c.arrow_l = txt(Name(k, "ArrowL"));
    c.arrow_r = txt(Name(k, "ArrowR"));
    c.lock = pic(Name(k, "Lock"));
    ok = ok && c.glass && c.pad && c.face[0] && c.face[1] && c.ring && c.hover && c.swatch[kSwatches - 1] && c.tag &&
         c.outfit && c.ready && c.prompt && c.arrow_l && c.arrow_r && c.lock;
  }
  if (!ok) REXLOG_WARN("Outfit picker: some of the cards' elements weren't found on the page");
  return ok;
}

// The prompt bar (FE_Buttons): 1 player = the game page's Back / Wear;
// more = empty (each card has its own). The mouse's hover turns Back / Wear
// yellow (their own colour, kept from the first look, otherwise).
void PromptBar(PPCContext& ctx, uint8_t* base, bool one, bool wear, bool hover_back = false,
               bool hover_wear = false) {
  const uint32_t buttons = scrooby::FindPage(ctx, base, "FE_Buttons");
  if (!buttons) return;
  struct P {
    const char* name;
    std::u16string_view one;
  };
  const P prompts[] = {{"LowerLeftText", u"Back"}, {"LowerLeftButton", u"\u00B2"},
                       {"LowerRightText", wear ? u"Wear" : u""}, {"LowerRightButton", wear ? u"\u00B1" : u""},
                       {"UpperLeftText", u""},  {"UpperLeftButton", u""},
                       {"UpperRightText", u""}, {"UpperRightButton", u""}};
  for (const P& q : prompts) {
    if (uint32_t e = scrooby::FindText(ctx, base, buttons, q.name)) Put(ctx, base, e, one ? q.one : u"");
  }
  const char* words[2] = {"LowerLeftText", "LowerRightText"};
  const bool hovered[2] = {hover_back, hover_wear};
  for (int i = 0; i < 2; ++i) {
    const uint32_t e = scrooby::FindText(ctx, base, buttons, words[i]);
    if (!e) continue;
    if (!g.bar_colour[i]) g.bar_colour[i] = Read32(e + 80);  // (its own, before any hover)
    scrooby::SetColour(ctx, base, e, hovered[i] ? kYellow : g.bar_colour[i]);
  }
}
// The bar's own colours back (the picker closes).
void RestoreBar(PPCContext& ctx, uint8_t* base) {
  const uint32_t buttons = scrooby::FindPage(ctx, base, "FE_Buttons");
  const char* words[2] = {"LowerLeftText", "LowerRightText"};
  for (int i = 0; i < 2 && buttons; ++i) {
    const uint32_t e = scrooby::FindText(ctx, base, buttons, words[i]);
    if (e && g.bar_colour[i]) scrooby::SetColour(ctx, base, e, g.bar_colour[i]);
  }
}

// =============================================================================
// DRAWING
// =============================================================================
// The 3D model above card `cx` with its feet at `feet` (screen pixels),
// `size` relative to the 1-player model: its matrix, every frame. A LOCKED
// look isn't shown: the model stands far outside the picture and the card's
// lock icon takes its place (the mockup's black silhouette needs a model of
// our own: players/outfit_previews.*, later).
void PlaceModel(float cx, float feet, float size, bool locked) {
  const uint32_t preview = Read32(g.screen + kScreenPreview);
  if (preview < 0x40000000) return;
  const uint32_t matrix = Read32(preview + 44);
  if (matrix < 0x40000000) return;
  const ModelMap m = Map();
  const float scale = size * m.size;
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) WriteF(matrix + 16 * r + 4 * c, r == c ? scale : 0.0f);
  }
  WriteF(matrix + 48, locked ? 1000.0f : (cx - 960.0f) / m.px_per_x);
  WriteF(matrix + 52, 0.1f + (m.feet_y0 - feet) / m.px_per_y);
  WriteF(matrix + 56, 0.0f);
}

// THE 1-PLAYER PAGE (the mockup's first screen): a 600 x 520 glass at
// (420, 330) with the hinge at its top left; the faces, the name, the
// circles in two rows of four; the model at x 1320 on its pad, feet at 860.
void DrawOne(PPCContext& ctx, uint8_t* base, int p) {
  const Player& pl = g.players[p];
  const Card& c = g.cards[0];
  for (int k = 1; k < kMaxPlayers; ++k) HideCard(base, g.cards[k]);
  // The panel comes in from the left with the title layer's slide, the model
  // side (pad, lock) from the right (the description layer's, mirrored).
  const float px = 420 + g.slide, py = 330, pw = 600, ph = 520, cx = px + pw / 2;
  const float mx = 1320 - g.slide;
  const bool locked = !Available(pl.pick);
  const bool browsing = !pl.ready && !pl.waiting;
  PlacePicture(ctx, base, c.glass, cx, py + ph / 2, pw, ph, kGlassW, kGlassH, kGlass);
  // The hinge and the gear at the panel's top-left corner, at the game's own
  // size and where the original page has them on its title panel
  // (MenuPanel (141, 330) top 404: MenuHinge (131, 347) 80 x 120, cog (107,
  // 412) 90 x 90 = hinge 10 units left of the panel and 57 below its top,
  // cog 34 left, 8 above). The gear MOVES as the game moves its own (the
  // hidden "cog": its transform copied every frame).
  {
    const float left = UnitX(px), top = UnitY(py);
    auto sx = [](float u) { return 240.0f + 2.25f * u; };
    auto sy = [](float u) { return 1080.0f - 2.25f * u; };
    // The game's gear PULSES (read with the console: its transform's scale
    // swings 1.11 <-> 1.20 about every 0.6 s, angle 0; the Options screen's
    // turn during slides): its scale and angle are copied every frame.
    float scale = 1.0f, angle = 0.0f;
    if (const uint32_t game_cog = scrooby::FindPicture(ctx, base, g.page, "cog")) {
      const float m00 = ReadF(game_cog + 16), m01 = ReadF(game_cog + 20);
      const float k = std::hypot(m00, m01);
      if (std::isfinite(k) && k > 0.2f && k < 5.0f) {
        scale = k;
        angle = std::atan2(m01, m00) * 57.2957795f;
      }
    }
    PlacePicture(ctx, base, g.cog, sx(left - 34 + kCogSize / 2), sy(top + 8 + kCogSize / 2),
                 kCogSize * 2.25f * scale, kCogSize * 2.25f * scale, kCogSize, kCogSize, kWhite);
    if (angle != 0) scrooby::Rotate(ctx, base, g.cog, angle);
    // A SECOND GEAR (2026-10-23), up and to the right of the
    // first, its teeth meshing with it and its top running up behind the
    // purple frame (the frame page is drawn over ours), as the menus' gears
    // usually vanish behind the frame (the original title panel's own gear sits
    // at y 412-502, past the frame's inner edge ~450). It pulses opposite to
    // the first (one grows while the other shrinks: 1.11 + 1.20 - scale).
    const float scale2 = std::clamp(2.31f - scale, 0.5f, 2.0f);
    PlacePicture(ctx, base, g.cog2, sx(left - 34 + kCogSize / 2 + 40), sy(440), kCogSize * 2.25f * scale2,
                 kCogSize * 2.25f * scale2, kCogSize, kCogSize, kWhite);
    if (angle != 0) scrooby::Rotate(ctx, base, g.cog2, -angle);
    PlacePicture(ctx, base, g.hinge, sx(left - 10 + kHingeW / 2), sy(top - 57 + kHingeH / 2), kHingeW * 2.25f,
                 kHingeH * 2.25f, kHingeW, kHingeH, kWhite);
  }
  Hide(base, c.tag);
  Hide(base, c.ready);
  const int arrow_row = ArrowRow(p, pl);
  // Faces: 100 px, 60 px either side of the middle.
  const float face = 100, face_y = py + 70 + face / 2;
  AddHit(p, kHitRow, kRowCharacter, cx, face_y, pw, face + 30);
  for (int f = 0; f < 2; ++f) {
    const bool on = pl.pick.character == f;
    const bool hover = Hovered(p, kHitFace, f);
    const float grow = (on && browsing && pl.row == kRowCharacter ? 1.2f : 1.0f) * (hover ? 1.15f : 1.0f);
    const float fx = cx + (f == 0 ? -110.0f : 110.0f);
    PlaceDisc(ctx, base, c.face[f], fx, face_y, face * grow, face * grow, on || hover ? kWhite : kFaceDim);
    if (browsing) AddHit(p, kHitFace, f, fx, face_y, face, face);
  }
  // The name.
  const bool name_selected = browsing && pl.row == kRowOutfit;
  const float font = 67;
  AddHit(p, kHitRow, kRowOutfit, cx, py + 220 + font * 0.5f, pw, font + 30);
  PlaceText(ctx, base, c.outfit, LookOf(pl.pick).name, cx, py + 220 - (name_selected ? font * 0.1f : 0.0f),
            font * (name_selected ? 1.2f * Pulse() : 1.0f), name_selected ? kYellow : kGreen);
  // The circles: 54 px, 22 apart, 4 x 2.
  const float d = 54, gap = 22, total = 4 * d + 3 * gap, top = py + 340;
  if (HasColour(pl.pick)) {
    AddHit(p, kHitRow, kRowColour, cx, top + d + gap / 2, pw, 2 * d + gap + 20);
    float sxs[kSwatches], sys[kSwatches];
    for (int s = 0; s < kSwatches; ++s) {
      const int col = s % 4, row = s / 4;
      const float sx = cx - total / 2 + col * (d + gap) + d / 2, sy = top + row * (d + gap) + d / 2;
      sxs[s] = sx;
      sys[s] = sy;
      const bool on = s == pl.pick.colour;
      PlaceDisc(ctx, base, c.swatch[s], sx, sy, on ? d + 10 : d, on ? d + 10 : d, kColours[s].tint);
      if (on) {
        const uint32_t ring = browsing && pl.row == kRowColour ? kYellow : kWhite;
        PlaceDisc(ctx, base, c.ring, sx, sy, d + 22, d + 22, ring);
      }
      if (browsing) AddHit(p, kHitSwatch, s, sx, sy, d + gap, d + gap);
    }
    PlaceHoverRing(ctx, base, c, p, sxs, sys, d);
  } else {
    for (uint32_t e : c.swatch) Hide(base, e);
    Hide(base, c.ring);
    Hide(base, c.hover);
  }
  // The arrows beside the selected row (the mouse's row while it points).
  if (!browsing) {
    Hide(base, c.arrow_l);
    Hide(base, c.arrow_r);
  } else if (arrow_row == kRowCharacter) {
    PlaceArrowsFor(ctx, base, c, p, kRowCharacter, cx, face_y, 110 + face / 2 + 50, 48);
  } else if (arrow_row == kRowOutfit) {
    PlaceArrowsFor(ctx, base, c, p, kRowOutfit, cx, py + 220 + font * 0.5f, pw / 2 - 24 - 14, 48);
  } else if (p != g.mouse_player) {  // (keyboard + mouse: the circles are clicked, no arrows)
    PlaceArrowsFor(ctx, base, c, p, kRowColour, cx, top + d + gap / 2, total / 2 + 48, 48);
  } else {
    Hide(base, c.arrow_l);
    Hide(base, c.arrow_r);
  }
  // "Loading" under the circles while the pick loads.
  if (pl.waiting) {
    PlaceText(ctx, base, c.prompt, u"Loading", cx, py + ph - 70, 50 * Pulse(), kCyan);
  } else {
    Hide(base, c.prompt);
  }
  // The model on the right, on an orange pad; locked: the lock over it.
  PlaceDisc(ctx, base, c.pad, mx, 860, 300, 52, (kPlayerColour[p] & 0x00FFFFFF) | uint32_t(kPadAlpha) << 24);
  PlaceModel(1320, 860, 1.0f, locked || !g.settled || g.exit_step >= 0);  // (after the slide, gone at the close)
  if (locked) {
    const float h = kLockTall * kOneModelSize;
    PlacePicture(ctx, base, c.lock, mx, 860 - 380 * kOneModelSize * 0.62f + 72 * kOneModelSize,
                 h * kLockAspect, h, kLockW, kLockH, kWhite);
  } else {
    Hide(base, c.lock);
  }
  // The prompt bar: "B Back" bottom left, "Wear A" bottom right (the
  // mockup's places); the mouse can click them too.
  const bool wear = browsing && !locked;
  PromptBar(ctx, base, g.exit_step < 0, wear, Hovered(p, kHitBack), wear && Hovered(p, kHitWear));
  if (browsing) AddHit(p, kHitBack, -1, 482, 957, 180, 80);
  if (wear) AddHit(p, kHitWear, -1, 1450, 957, 240, 80);
}

// THE CARDS (2-4 players: the mockup's card()).
void DrawCard(PPCContext& ctx, uint8_t* base, int slot, int p, const RowLayout& L, float x) {
  const Player& pl = g.players[p];
  const Card& c = g.cards[slot];
  const float w = L.w, cx = x + w / 2, s = L.model, fs = L.font;
  const float feet = kCardTop + kFeetDrop;
  const bool locked = !Available(pl.pick);
  const bool browsing = !pl.ready && !pl.waiting;
  PlacePicture(ctx, base, c.glass, cx, (kCardTop + kCardBottom) / 2, w, kCardBottom - kCardTop, kGlassW, kGlassH,
               kGlass);
  PlaceText(ctx, base, c.tag, U16(fmt::format("P{}", p + 1)), x + 22, kCardTop + 18, fs * 0.9f, kPlayerColour[p],
            true, kTagW);
  const float pad_w = 300 * s * 0.72f;
  PlaceDisc(ctx, base, c.pad, cx, feet, pad_w, 44, (kPlayerColour[p] & 0x00FFFFFF) | uint32_t(kPadAlpha) << 24);
  if (locked) {
    const float ph = 380 * s;
    PlacePicture(ctx, base, c.lock, cx, feet - ph * 0.62f + 72 * s, kLockTall * s * kLockAspect, kLockTall * s,
                 kLockW, kLockH, kWhite);
  } else {
    Hide(base, c.lock);
  }
  float y = feet + 34;
  const float row = L.row;
  if (pl.ready && !pl.waiting) {  // READY: the name, "Ready!", "B Change"
    Hide(base, c.face[0]);
    Hide(base, c.face[1]);
    Hide(base, c.ring);
    Hide(base, c.hover);
    for (uint32_t e : c.swatch) Hide(base, e);
    Hide(base, c.arrow_l);
    Hide(base, c.arrow_r);
    PlaceText(ctx, base, c.outfit, LookOf(pl.pick).name, cx, y + 8, fs, kGreen);
    PlaceText(ctx, base, c.ready, u"Ready!", cx, y + row + 6, fs * 1.45f, kYellow);
    PlaceText(ctx, base, c.prompt, ForPlayer(u"² Change", p), cx, y + row * 2 + 30, fs * 0.8f,
              Hovered(p, kHitChange) ? kYellow : kCyan);
    AddHit(p, kHitChange, -1, cx, y + row * 2 + 30 + fs * 0.4f, w * 0.6f, fs);
    return;
  }
  Hide(base, c.ready);
  // Row 1: the faces (the other character greyed; the picked one bigger
  // while its row is selected).
  const int arrow_row = ArrowRow(p, pl);
  const float m = L.face, gap = m * 0.5f;
  AddHit(p, kHitRow, kRowCharacter, cx, y + m / 2, w, row);
  for (int f = 0; f < 2; ++f) {
    const bool on = pl.pick.character == f;
    const bool hover = Hovered(p, kHitFace, f);
    const float grow = (on && browsing && pl.row == kRowCharacter ? 1.2f : 1.0f) * (hover ? 1.15f : 1.0f);
    const float fx = f == 0 ? cx - gap / 2 - m / 2 : cx + gap / 2 + m / 2;
    PlaceDisc(ctx, base, c.face[f], fx, y + m / 2, m * grow, m * grow, on || hover ? kWhite : kFaceDim);
    if (browsing) AddHit(p, kHitFace, f, fx, y + m / 2, m, m);
  }
  if (browsing && arrow_row == kRowCharacter) {
    PlaceArrowsFor(ctx, base, c, p, kRowCharacter, cx, y + m / 2, gap / 2 + m + L.arrow_gap, L.arrow);
  }
  y += row;
  // Row 2: the outfit's name.
  const bool name_selected = browsing && pl.row == kRowOutfit;
  AddHit(p, kHitRow, kRowOutfit, cx, y + fs * 0.5f, w, row);
  if (browsing && arrow_row == kRowOutfit) {
    PlaceArrowsFor(ctx, base, c, p, kRowOutfit, cx, y + fs * 0.5f, w / 2 - L.arrow * 0.5f - 14, L.arrow);
  }
  if (name_selected) {
    PlaceText(ctx, base, c.outfit, LookOf(pl.pick).name, cx, y - fs * 0.1f, fs * 1.2f * Pulse(), kYellow);
  } else {
    PlaceText(ctx, base, c.outfit, LookOf(pl.pick).name, cx, y, fs, kGreen);
  }
  y += row;
  // Row 3: the colour circles (Crash's Normal look only).
  if (HasColour(pl.pick)) {
    const float d = L.swatch, g2 = L.swatch_gap, total = kSwatches * d + (kSwatches - 1) * g2;
    AddHit(p, kHitRow, kRowColour, cx, y + d / 2, w, row);
    float sxs[kSwatches], sys[kSwatches];
    for (int sw = 0; sw < kSwatches; ++sw) {
      const bool on = sw == pl.pick.colour;
      const float sx = cx - total / 2 + sw * (d + g2) + d / 2, sy = y + d / 2;
      sxs[sw] = sx;
      sys[sw] = sy;
      PlaceDisc(ctx, base, c.swatch[sw], sx, sy, on ? d + 8 : d, on ? d + 8 : d, kColours[sw].tint);
      if (on) {
        const uint32_t ring = browsing && pl.row == kRowColour ? kYellow : kWhite;
        PlaceDisc(ctx, base, c.ring, sx, sy, d + 20, d + 20, ring);
      }
      if (browsing) AddHit(p, kHitSwatch, sw, sx, sy, d + g2, d + g2);
    }
    PlaceHoverRing(ctx, base, c, p, sxs, sys, d);
    if (browsing && arrow_row == kRowColour && p != g.mouse_player) {  // (keyboard + mouse: no arrows)
      PlaceArrowsFor(ctx, base, c, p, kRowColour, cx, y + d / 2, total / 2 + L.arrow * 0.5f + 8, L.arrow * 0.8f);
    }
  } else {
    for (uint32_t e : c.swatch) Hide(base, e);
    Hide(base, c.ring);
    Hide(base, c.hover);
  }
  if (!browsing || (arrow_row == kRowColour && p == g.mouse_player)) {  // (no arrows there)
    Hide(base, c.arrow_l);
    Hide(base, c.arrow_r);
  }
  y += row * 0.85f;
  // The prompt: "Loading" while the pick (or its model) loads, "A Wear" (not
  // on a locked look).
  if (pl.waiting || (!locked && outfit_previews::Active() && outfit_previews::Loading(slot))) {
    PlaceText(ctx, base, c.prompt, u"Loading", cx, y + 6, fs * 0.8f * Pulse(), kCyan);
  } else if (!locked) {
    PlaceText(ctx, base, c.prompt, ForPlayer(u"± Wear", p), cx, y, fs * 0.8f,
              Hovered(p, kHitWear) ? kYellow : kCyan);
    AddHit(p, kHitWear, -1, cx, y + fs * 0.4f, w * 0.6f, fs);
  } else {
    Hide(base, c.prompt);
  }
}

void Draw(PPCContext& ctx, uint8_t* base) {
  g.hits.clear();
  const uint32_t page = scrooby::FindPage(ctx, base, kPage);
  if (!page || !FindElements(ctx, base, page)) return;
  for (const char* name : kGameElements) {
    if (uint32_t e = scrooby::FindText(ctx, base, page, name)) Hide(base, e);
    if (uint32_t e = scrooby::FindPicture(ctx, base, page, name)) Hide(base, e);
  }
  std::vector<int> joined;
  for (int p = 0; p < kMaxPlayers; ++p) {
    if (g.players[p].joined) joined.push_back(p);
  }
  if (joined.size() <= 1) {
    outfit_previews::End();  // (1 player: the game's own model)
    DrawOne(ctx, base, joined.empty() ? 0 : joined[0]);
    return;
  }
  Hide(base, g.hinge);
  Hide(base, g.cog);
  Hide(base, g.cog2);
  PromptBar(ctx, base, false, false);
  const int n = int(joined.size());
  const RowLayout& L = kRowLayouts[n - 2];
  // The models: once the slide is over (as the game's own), gone at the close.
  if (g.settled && g.exit_step < 0) outfit_previews::Begin(n);  // (the game's own single model goes out of sight)
  else outfit_previews::End();
  const float total = n * L.w + (n - 1) * L.gap, x0 = std::round(960.0f - total / 2);
  for (int slot = 0; slot < kMaxPlayers; ++slot) {
    if (slot >= n) {
      HideCard(base, g.cards[slot]);
      continue;
    }
    // Cards on the left half come in from the left (the title layer's
    // slide), the others from the right (mirrored, as the description).
    // The overshoot (the slide past its place before settling) is kept
    // under half the gap: two cards meeting in the middle mustn't overlap.
    const float rest = x0 + slot * (L.w + L.gap);
    const float slide = std::min(g.slide, L.gap / 2 - 2);
    const float x = rest + (rest + L.w / 2 <= 960.0f ? slide : -slide);
    DrawCard(ctx, base, slot, joined[size_t(slot)], L, x);
    // ITS OWN MODEL (players/outfit_previews.h, one per card): the player's
    // pick, above their card, the shoes on the pad; a locked look = none.
    const Player& pl = g.players[joined[size_t(slot)]];
    const ModelMap mm = Map();
    if (Available(pl.pick)) outfit_previews::Show(slot, OutfitOfPick(pl.pick));
    outfit_previews::SetLocked(slot, !Available(pl.pick));
    outfit_previews::Place(slot, (rest + L.w / 2) / 1920.0f, L.model / kOneModelSize * mm.size,
                           0.1f + (mm.feet_y0 - (kCardTop + kFeetDrop)) / mm.px_per_y);
  }
}

// --- What a player can do (keys, a controller, the mouse) ---------------------
// Step `row` by d (the character, the look, the colour), shown in the preview.
void StepRow(PPCContext& ctx, uint8_t* base, int p, int row, int d) {
  Player& pl = g.players[p];
  pl.row = row;
  Step(pl.pick, row, d);
  g.focus = p;
  // A LOCKED look never goes to the preview (its model is hidden, the
  // lock shows): the screen's own "show entry" of a locked Coco switched
  // the preview's idle animations to Coco's while the Crash model stayed:
  // a null read in sub_820E0C98's idle pick (crash, 2026-10-23, 1 player,
  // Character row -> Coco in a save without her).
  if (Available(pl.pick)) ShowInPreview(ctx, base, OutfitOfPick(pl.pick));
  scrooby::PlaySound(ctx, base, scrooby::Sound::kChange);
  if (pl.row == kRowColour && !HasColour(pl.pick)) pl.row = kRowOutfit;
}
// A = wear the pick (queued; a locked look can be looked at, not worn).
void Wear(PPCContext& ctx, uint8_t* base, int p) {
  Player& pl = g.players[p];
  if (!Available(pl.pick)) return;
  pl.ready = true;
  pl.waiting = true;
  if (g.queue.empty()) g.queue_since = Clock::now();
  g.queue.push_back(p);
  scrooby::PlaySound(ctx, base, scrooby::Sound::kChange);
}
// B while choosing = stay as they are (ready); B when ready = choose again.
void Keep(PPCContext& ctx, uint8_t* base, int p) {
  Player& pl = g.players[p];
  pl.pick = PickOfOutfit(outfits::OutfitOf(p));
  pl.ready = true;
  scrooby::PlaySound(ctx, base, scrooby::Sound::kMove);
}
void Unready(PPCContext& ctx, uint8_t* base, int p) {
  g.players[p].ready = false;
  scrooby::PlaySound(ctx, base, scrooby::Sound::kMove);
}

// THE MOUSE (2026-10-23): it belongs to the player the
// keyboard plays as, and only reaches THAT player's card, so it can't change
// anyone else's pick. Pointing at a face, a colour circle, an arrow or the
// Wear / Change (1 player: the bar's Back / Wear) only HIGHLIGHTS it (the
// face grows, a yellow ring behind the circle, the arrow grows, the words
// turn yellow); a LEFT CLICK on it does it. The right click is B and the
// wheel the D-pad already (the keyboard driver's pulses, in the keys' pad).
// The spots are the ones the last frame drew (Draw -> AddHit); a click
// counts where it was MADE (input/keyboard_mouse.h PeekMenuClick).
int HitAt(float x, float y, int* row) {
  int found = -1;
  *row = -1;
  for (size_t i = 0; i < g.hits.size(); ++i) {
    const HitBox& h = g.hits[i];
    if (x < h.x0 || x > h.x1 || y < h.y0 || y > h.y1) continue;
    if (h.kind == kHitRow) *row = h.index;
    else found = int(i);  // (the last drawn wins: arrows over a row's band)
  }
  return found;
}
void Mouse(PPCContext& ctx, uint8_t* base) {
  g.mouse_player = -1;
  const auto* assignment = kbm::PlayerAssignment::Get();
  auto* driver = kbm::KeyboardMouseDriver::Get();
  if (!driver || !assignment) return;
  for (int p = 0; p < kMaxPlayers; ++p) {
    if (g.players[p].joined && assignment->KeyboardAnswersFor(uint32_t(p))) {
      g.mouse_player = p;
      break;
    }
  }
  g.hover_kind = kHitNone;
  g.hover_index = -1;
  g.hover_row = -1;
  float x = 0, y = 0;
  uint32_t moves = 0;
  if (g.mouse_player < 0 || !driver->Pointer(&x, &y, &moves)) return;
  if (moves != g.mouse_moves) {
    g.mouse_moves = moves;
    if (g.frames > 1) g.mouse_on = true;  // (not the pointer as found at the opening)
  }
  if (g.mouse_on) {
    if (const int i = HitAt(x * 1920.0f, y * 1080.0f, &g.hover_row); i >= 0) {
      g.hover_kind = g.hits[size_t(i)].kind;
      g.hover_index = g.hits[size_t(i)].index;
    }
  }
  float cx = 0, cy = 0;
  if (!driver->PeekMenuClick(&cx, &cy)) return;
  int row = -1;
  const int i = HitAt(cx * 1920.0f, cy * 1080.0f, &row);
  if (i < 0) return;  // (not on a spot: left for the click's own timeout)
  driver->TakeMenuClick();
  g.mouse_on = true;
  const HitBox h = g.hits[size_t(i)];
  const int p = g.mouse_player;
  Player& pl = g.players[p];
  if (pl.waiting || pl.stick_dir == 99) return;
  switch (h.kind) {
    case kHitFace:
      if (pl.pick.character != h.index) StepRow(ctx, base, p, kRowCharacter, 1);
      pl.row = kRowCharacter;
      break;
    case kHitSwatch:
      if (HasColour(pl.pick) && pl.pick.colour != h.index) {
        pl.pick.colour = h.index;
        StepRow(ctx, base, p, kRowColour, 0);
      }
      pl.row = kRowColour;
      break;
    case kHitArrowL:
    case kHitArrowR:
      StepRow(ctx, base, p, h.index, h.kind == kHitArrowL ? -1 : 1);
      break;
    case kHitWear:
      if (!pl.ready) Wear(ctx, base, p);
      break;
    case kHitBack:
      if (!pl.ready) Keep(ctx, base, p);
      break;
    case kHitChange:
      if (pl.ready) Unready(ctx, base, p);
      break;
    default:
      break;
  }
}

// The slide this frame: while opening, the title layer's own x (recorded);
// while closing, the recording played backwards (out to the left).
void Slide() {
  const uint32_t layer = Read32(g.screen + kScreenTitleLayer);
  float lx = layer > 0x40000000 ? ReadF(layer + kLayerX) : 0.0f;
  if (!std::isfinite(lx) || std::fabs(lx) > 4) lx = 0;
  if (g.exit_step >= 0) {
    const int i = int(g.curve.size()) - 1 - g.exit_step;
    g.slide = g.curve.empty() ? 0.0f : g.curve[size_t(std::max(i, 0))] * kLayerWidthPx;
    return;
  }
  if (!g.settled) {
    g.curve.push_back(lx);
    // Over: back at 0 after having moved (or nothing moves, or ~8 s).
    if ((lx == 0 && (g.curve.size() > 1 || g.frames > 2)) || g.curve.size() > 240) g.settled = true;
  }
  g.slide = lx * kLayerWidthPx;
}

void Open(PPCContext& ctx, uint8_t* base, uint32_t screen) {
  g = State{};
  g.open = true;
  g.screen = screen;
  g.opened_at = Clock::now();
  int count = 0;
  for (int p = 0; p < kMaxPlayers && p < more_players::LocalPlayerCount(); ++p) {
    Player& pl = g.players[p];
    pl.joined = Read32(kCoOpState + 4 * p) != 0;
    if (!pl.joined) continue;
    ++count;
    pl.pick = PickOfOutfit(outfits::OutfitOf(p));
    pl.row = HasColour(pl.pick) ? kRowColour : kRowOutfit;
    pl.held = ReadPad(p).buttons;  // (the press that opened the chest isn't a press here)
    pl.stick_dir = 99;
  }
  if (count == 0) {  // (nobody? player 1 then)
    g.players[0].joined = true;
    g.players[0].pick = PickOfOutfit(outfits::OutfitOf(0));
    count = 1;
  }
  g.count = count;
  for (int p = 0; p < kMaxPlayers; ++p) {
    if (g.players[p].joined) {
      g.focus = p;
      break;
    }
  }
  g_mute = true;
  if (Available(g.players[g.focus].pick)) ShowInPreview(ctx, base, OutfitOfPick(g.players[g.focus].pick));
  REXLOG_INFO("Outfit picker: open for {} player(s) (screen {:08X})", count, screen);
}

}  // namespace

void Register() {
  if (!REXCVAR_GET(outfit_picker)) return;
  data_patcher::Register("outfit picker cards", data_patcher::kCategoryFrontend, "cdd70a8c", PatchPackage);
  data_patcher::Register("outfit picker check", data_patcher::kCategoryFightTree, "Frontend", CheckTree);
  fight_tree::SetFrontEndDecision(kSkinState, [](PPCContext& ctx, uint8_t* base) -> int32_t {
    if (!g.open) {  // (the game's own screen: its own decision)
      __imp__sub_8211AF40(ctx, base);
      return int32_t(ctx.r3.u32);
    }
    if (!g.close) return -1;
    RestoreBar(ctx, base);
    // THE MUSIC BACK (2026-10-24). The screen's Enter (sub_820E0188, 0x820E07B8)
    // puts the music manager (*(0x8259B240)) in mode 3 = the menu's music;
    // both of its own ways out set mode 1 again = the level's music: B
    // (sub_820E15F8, 0x820E1700) and Wear's "done" (sub_820E0858, 0x820E0924),
    // via sub_822D9998(manager, mode). The picker takes the input itself, so
    // neither ran: the menu music played on in the level until a level change
    // or the next menu. Found from the logs (the level's "explore" music was
    // loaded again only after a respawn pick) and the screen's code.
    if (const uint32_t music = Read32(kMusicManager)) scrooby::Call(__imp__sub_822D9998, ctx, base, music, 1);
    outfit_previews::End();
    g.close = false;
    g.open = false;
    g_mute = false;
    g_closed_ms = NowMs();
    for (auto& q : g_quiet_until_up) q = true;
    REXLOG_INFO("Outfit picker: everyone is ready: closed");
    return kSkinExitBack;
  });
}

void FilterPad(uint32_t user, uint8_t* pad) {
  if (user >= kMaxPlayers || !pad) return;
  if (g_mute.load(std::memory_order_relaxed)) {
    std::memset(pad, 0, 12);
    return;
  }
  if (!g_quiet_until_up[user].load(std::memory_order_relaxed)) return;
  if (pad[0] == 0 && pad[1] == 0) {
    g_quiet_until_up[user] = false;  // let go: the game hears it again
    return;
  }
  pad[0] = pad[1] = 0;  // buttons only (u16 at +0): the sticks are the game's again
}

bool JustClosed() {
  // (Also while the chest's state is the front end's: the B that OPENED the
  // chest is let go in its first frames, before the picker is up; that
  // release made player 1 a mask in two of five test runs.)
  return NowMs() - g_closed_ms.load(std::memory_order_relaxed) < 1000 ||
         fight_tree::CurrentFrontEnd().state == kSkinState;
}

void AfterUpdate(PPCContext& ctx, uint8_t* base, uint32_t screen) {
  if (!REXCVAR_GET(outfit_picker) || !g_page_patched || !g_tree_patched) return;
  if (!g.open || g.screen != screen) Open(ctx, base, screen);
  ++g.frames;
  if (g.close) {
    Draw(ctx, base);
    return;
  }
  Slide();
  const bool input = g.settled && g.exit_step < 0;  // (not while sliding in / out)
  if (input) Mouse(ctx, base);
  for (int p = 0; p < kMaxPlayers; ++p) {
    Player& pl = g.players[p];
    if (!pl.joined) continue;
    const Pad pad = ReadPad(p);
    const uint16_t pressed = pad.buttons & ~pl.held;
    pl.held = pad.buttons;
    if (!input) continue;
    if (pl.stick_dir == 99) {  // (opening: wait until the stick is let go)
      if (std::fabs(pad.x) < kStickOn && std::fabs(pad.y) < kStickOn && !(pad.buttons & 0x000F)) pl.stick_dir = 0;
      continue;
    }
    if (pl.waiting) continue;
    if (pl.ready) {
      if (pressed & kB) Unready(ctx, base, p);
      continue;
    }
    int dx = 0, dy = 0;
    Directions(pl, pad, &dx, &dy);
    if ((dx || dy) && p == g.mouse_player) g.mouse_on = false;  // keys again: the arrows follow the selection
    // 1 PLAYER: the circles are two rows of four: down from the top row =
    // the circle under it, up from the bottom row = the one above.
    if (dy && g.count == 1 && pl.row == kRowColour && HasColour(pl.pick) &&
        ((dy > 0 && pl.pick.colour < 4) || (dy < 0 && pl.pick.colour >= 4))) {
      pl.pick.colour += dy > 0 ? 4 : -4;
      StepRow(ctx, base, p, kRowColour, 0);
      dy = 0;
    }
    if (dy) {  // up / down: the rows (no colour row on a look without colours; no wrap)
      const int last = HasColour(pl.pick) ? kRowColour : kRowOutfit;
      const int row = std::clamp(pl.row + dy, kRowCharacter, last);
      if (row != pl.row) {
        pl.row = row;
        scrooby::PlaySound(ctx, base, scrooby::Sound::kMove);
      }
    }
    if (dx) StepRow(ctx, base, p, pl.row, dx);
    if (pressed & kA) {
      Wear(ctx, base, p);
    } else if (pressed & kB) {
      Keep(ctx, base, p);
    }
  }
  RunQueue(ctx, base);
  // When nobody's pick is being worn, the preview shows the focus player's.
  if (g.queue.empty() && Available(g.players[g.focus].pick)) {
    ShowInPreview(ctx, base, OutfitOfPick(g.players[g.focus].pick));
  }
  bool all = true;
  for (const Player& pl : g.players) {
    if (pl.joined && (!pl.ready || pl.waiting)) all = false;
  }
  // Everyone ready: the models go, the panels slide out (the opening played
  // backwards), then the screen closes (the decision's ExitBack).
  if (all && g.queue.empty() && g.frames > 10 && g.exit_step < 0 && outfits::PreviewHeld()) {
    // The last pick was Crash <-> Coco: the players are being respawned (a
    // small level reload, outfits.cpp section 5b). The game's own Wear Skin
    // closes the screen in that very frame (front-end exit 850); sliding the
    // panels out first left the empty chest frame up through the reload's
    // loading screen (a flash of the menu's background before the game came
    // back, 2026-10-24). So: closed at once, like the original.
    outfit_previews::End();
    g.exit_step = int(g.curve.size());
    g.close = true;
  } else if (all && g.queue.empty() && g.frames > 10 && g.exit_step < 0) {
    g.exit_step = 0;
    outfit_previews::End();
  } else if (g.exit_step >= 0 && ++g.exit_step >= int(g.curve.size())) {
    g.close = true;
  }
  Draw(ctx, base);
}

}  // namespace outfit_picker
