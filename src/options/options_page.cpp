// =============================================================================
// options/options_page.cpp -- see options_page.h
// =============================================================================
#include "options_page.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <rex/logging.h>

#include "../data/data_patcher.h"
#include "../data/fight_tree.h"
#include "../data/pure3d.h"
#include "../achievements/achievements.h"
#include "../players/players_setup.h"

namespace options_page {
namespace {

using pure3d::Bytes;
using pure3d::Node;

constexpr std::string_view kHostProject = "InGame.prj";
constexpr std::string_view kPageName = "InGame_Options_XENON.pag";
constexpr std::string_view kMenuName = "InGameOptionsMenu";
constexpr std::string_view kMapPageName = "InGame_Map.pag";


constexpr uint32_t kChunkLayer = 0x18020;
constexpr uint32_t kChunkMenu = 0x18010;
constexpr uint32_t kChunkItem = 0x18011;
constexpr uint32_t kChunkPicture = 0x18022;
constexpr uint32_t kChunkText = 0x18023;
constexpr uint32_t kChunkString = 0x1800B;

// Colours as the data stores them: bytes B G R A (findings/30 s.4).
constexpr uint32_t kGreen = 0xFF00FF00;   // menu items (#00FF00)
constexpr uint32_t kYellow = 0xFFFFF110;  // the selected item (#FFF110)
constexpr uint32_t kCyan = 0xFF00FFF7;    // titles, messages, prompts (#00FFF7)
constexpr uint32_t kFadedGreen = 0x8000FF00;  // the tabs beside the current one: half see-through

// Text justification codes (horizontal): 0 left, 1 right, 4 centre.
constexpr uint32_t kLeft = 0, kRight = 1, kCentre = 4;
// Vertical: every text in the game's pages has 2 (top: a text's lines hang
// from its box's top).
constexpr uint32_t kTop = 2;

// The card's glass pictures, the game's own three shapes (found by name from
// any page: the small and square ones are in package b4c85fe7, loaded all
// the time; the large one in every in-game menus package and the main
// menus' package): 4:1 (the save list's panels), 2:1 (the in-game glass
// panel), 1:1 (the question box). options_menu.cpp shows the one closest to
// the card's shape of the moment.
constexpr std::string_view kCardSprites[3] = {"FE_Common_GlassPanel_Rectangle_Small.tga",
                                              "FE_Common_GlassPanel_Rectangle_Large.tga",
                                              "FE_Common_GlassPanel_Square.tga"};
constexpr int kCardAspect[3] = {4, 2, 1};  // width : height of each
// A picture element's draw mode (the u32 after its box): 1 = stretched to its
// box, 0 = at its picture's own size, hanging from the box's top.
constexpr uint32_t kStretchToBox = 1;
constexpr uint32_t kGlassColour = 0xC8FFFFFF;  // white, 78 % opaque: the in-game glass panel's
constexpr int kShift = kGlassShift;             // options_page.h

// ---------------------------------------------------------------------------
// The layout, in Scrooby units (640 x 480, y up, (x, y) = bottom-left).
// TWO GLASS PANELS (2026-10-11): the game's own glass panel (500 x 354, at
// x 70-570 in the original) moves LEFT by kShift, and a glass CARD stands
// beside it on the right (options_page.h kCard*): the selected row's value,
// description, warning and picture. Together they are centred on the 16:9
// picture (x -107-747: past the 4:3 canvas x 0-640, drawn by the game at
// 1280 x 720 always; the map screen's strip uses it too, x -150-790), the
// same margin left and right; the main menu's purple frame leaves that room
// too. The original glass page (shared with the pause menu) is moved by
// options_menu.cpp while the screen is up; the main menu's copy is drawn by
// this page (backdrop).
// The game's own "Paused" title sits at y ~356, its "Back" prompt at y 42-69.
// ---------------------------------------------------------------------------
constexpr int kTabY = 312;                          // the row of tabs
// Three tab names: the current one in the middle (yellow), the previous and
// next ones (wrapping around) beside it, faded: there is more to either side.
constexpr int kTabCentreX[3] = {190 + kShift, 320 + kShift, 450 + kShift};  // previous, current, next
constexpr int kTabWidth = 130;
constexpr int kShoulderCentreX[2] = {105 + kShift, 540 + kShift};  // LB / RB pictures
constexpr int kRowTopY = 270;                       // row 0's y
constexpr int kRowStep = 30;                        // rows are this far apart (the pause menu's: 35)
// A ROW = the name at the glass's left (left aligned) and the value at its
// right (right aligned), the glass's whole width between them (2026-10-11,
// a playtest: centred columns with arrows beside the value left odd gaps).
// No arrows: A (or left / right) changes the value; the selected row jumps.
constexpr int kRowLeft = 100 + kShift, kRowRight = kRowValueRight;  // glass x 70-570, 30 inside
constexpr int kLabelX = kRowLeft, kLabelWidth = 250;
constexpr int kValueWidth = kRowValueWidth, kValueX = kRowRight - kValueWidth;
constexpr int kStarSize = 32;                       // the star pictures (shrunk by options_menu.cpp)
constexpr int kStarStep = 24;                       // 24 apart, the last one ending at the row's right
constexpr int kStarAboveRow = 3;
constexpr int kStarsX = kRowRight - kStarSize - kStarStep * (kStars - 1);
constexpr int kArrowSize = 32;                      // the map's arrow picture (page markers)
constexpr int kLineHeight = 36;                     // a text line's box (the game's rows)
// PAGES (a tab with more than kRows rows): in the button prompts' line
// (Back left, Select right; the middle is free), centred on the glass:
// "1/2" with a small arrow each side (the map's arrow, turned and shrunk
// by options_menu.cpp), the one with nothing that way faded. (First beside
// the glass, then a row of its own under the rows: too much room.)
constexpr int kGlassCentreX = 320 + kShift;
constexpr int kPageMarkersY = 38;                   // the text's box (prompts: y 42-69)
constexpr int kPageArrowY = 40;
constexpr int kPageArrowDX = 34;                    // each arrow's centre from the text's
constexpr int kPageTextWidth = 60;
// THE CARD (options_page.h kCard*): its elements are made here at some
// place; options_menu.cpp sizes the glass and places the texts while the
// screen is up (the card fits its contents).
//   OptCardSmall / OptCardLarge / OptCardSquare  the glass (one shown)
//   OptCardValue  the selected value (or the row's name), yellow
//   OptGuide      the Brightness row's guide (the original's calibration
//                 grey scale; a picture is drawn at its own size, 320 x 64
//                 units here, shrunk by options_menu.cpp)
//   OptDesc       the description (cyan, small print), lines hang from the top
//   OptWarn       the warning (yellow, small print)
constexpr int kGuideWidth = 320, kGuideHeight = 36;

std::atomic<bool> g_patched{false};

// ---------------------------------------------------------------------------
// Element fields (offsets from the end of the element's name; findings/30
// s.1 and tools/scrooby_dump.py): u32 version, i32 x, y, w, h, u8, then for
// texts u32 horizontal / vertical justify, u32 colour (B G R A), ...
// ---------------------------------------------------------------------------
void SetBox(Node& n, int x, int y, int w, int h) {
  const size_t e = pure3d::NameEnd(n);
  pure3d::SetNodeLe32(n, e + 4, uint32_t(x));
  pure3d::SetNodeLe32(n, e + 8, uint32_t(y));
  pure3d::SetNodeLe32(n, e + 12, uint32_t(w));
  pure3d::SetNodeLe32(n, e + 16, uint32_t(h));
}
void SetJustify(Node& text, uint32_t horizontal) {
  pure3d::SetNodeLe32(text, pure3d::NameEnd(text) + 21, horizontal);
}
void SetVerticalJustify(Node& text, uint32_t vertical) {
  pure3d::SetNodeLe32(text, pure3d::NameEnd(text) + 25, vertical);
}
// A one-frame picture element showing another picture (by name): its data
// = name, u32 version, x, y, w, h, u32 frames, u8, u32 justify, u32 colour,
// u32, f32, u32 count, then `count` picture names (tools/scrooby_dump.py).
// Returns false if it isn't a one-name picture.
bool SetPictureSprite(Node& picture, std::string_view sprite) {
  const size_t count_at = pure3d::NameEnd(picture) + 41;
  if (picture.data.size() < count_at + 5 || pure3d::NodeLe32(picture, count_at) != 1) return false;
  const size_t name_at = count_at + 4;
  const size_t name_end = name_at + 1 + picture.data[name_at];
  if (name_end > picture.data.size()) return false;
  Bytes data(picture.data.begin(), picture.data.begin() + long(name_at));
  const Bytes name = pure3d::PaddedString(sprite);
  data.insert(data.end(), name.begin(), name.end());
  data.insert(data.end(), picture.data.begin() + long(name_end), picture.data.end());
  picture.data = std::move(data);
  return true;
}
// The same for a picture with several frames: `sprites` in order (frame 0
// first; the page's code picks one with scrooby::SetFrame).
bool SetPictureSprites(Node& picture, const std::vector<std::string>& sprites) {
  const size_t count_at = pure3d::NameEnd(picture) + 41;
  if (picture.data.size() < count_at + 5 || pure3d::NodeLe32(picture, count_at) != 1 || sprites.empty()) {
    return false;
  }
  const size_t name_at = count_at + 4;
  const size_t name_end = name_at + 1 + picture.data[name_at];
  if (name_end > picture.data.size()) return false;
  Bytes data(picture.data.begin(), picture.data.begin() + long(count_at));
  pure3d::PutLe32(data, uint32_t(sprites.size()));
  for (const std::string& s : sprites) {
    const Bytes name = pure3d::PaddedString(s);
    data.insert(data.end(), name.begin(), name.end());
  }
  data.insert(data.end(), picture.data.begin() + long(name_end), picture.data.end());
  picture.data = std::move(data);
  return true;
}
void SetTextColour(Node& text, uint32_t bgra_le) {
  // stored B G R A: as a little-endian u32 that's 0xAARRGGBB.
  pure3d::SetNodeLe32(text, pure3d::NameEnd(text) + 29, bgra_le);
}

// The first direct child with that chunk id (and name, when given).
Node* Child(Node& n, uint32_t id, std::string_view name = {}) {
  for (Node& k : n.children) {
    if (k.id == id && (name.empty() || pure3d::NodeName(k) == name)) return &k;
  }
  return nullptr;
}

// A text copy with a new name, box, justification and colour, showing the
// bank's "_Empty" string (the menu code writes its real string).
Node MakeText(const Node& text_template, const Node& empty_string, std::string_view name, int x,
              int y, int w, int h, uint32_t justify, uint32_t colour) {
  Node t = text_template;
  pure3d::SetNodeName(t, name);
  SetBox(t, x, y, w, h);
  SetJustify(t, justify);
  SetTextColour(t, colour);
  t.children = {empty_string};
  return t;
}

// What a page is built from: copies of the game's own elements (the in-game
// Options page's Dialogue row, star and "Paused" title, the map's arrow),
// plus what differs between the in-game page and the main menu's.
struct Parts {
  Node menu;          // the menu (its header; rows are added)
  Node row;           // a menu item: look {label text} + value {value text}
  Node label, value;  // the row's texts
  Node empty;         // the string every text starts with ("_Empty" of a loaded bank)
  Node title;         // the title text, kept as is
  Node star, arrow;   // pictures
  Node guide;         // the Brightness guide (the calibration screen's grey scale)
  std::vector<Node> backdrop;  // drawn first (the main menu's glass panel, cogs, hinge)
  // The Achievements screen's sprites (achievements.h MenuSprites): one per
  // achievement in list order, and the ring. Empty = no grid.
  std::vector<std::string> ach_icons;
  std::string ach_ring;
  // The Players screen's device pictures (players_setup.h MenuSprites), in
  // Picture order: one picture element shows any of them (SetFrame).
  std::vector<std::string> ply_pics;
  // The glass panel's hinge (teal bracket + wrench, top left), copied onto
  // our page as PlyHinge AFTER the cards: the original sits on the glass
  // page, drawn before ours, so player 1's card covered it. In game: the
  // Hinge of InGame_NV_GlassPanel.pag; main menu: the PS2 Options page's
  // OptionsMenu_PanelHinge. Empty = no copy.
  Node ply_hinge;
};

// THE ACHIEVEMENTS LIST (achievements_screen.h; the contract with
// options_menu.cpp, which fills, tints and scrolls them): kAchCards glass
// CARDS stacked down the page, like the save list's panels (FE_GameSlot:
// FE_Common_GlassPanel_Rectangle_Small stretched to 388 x 111, a picture,
// texts) but wider and lower, one achievement each (page units, y up):
//   AchHeading       "Achievements", above the cards (the screen's title)
//   AchUp / AchDown  the list's arrows, beside the first / last card
//   AchCard<k>       the glass (kAchCardW x kAchCardH, from x kAchCardX)
//   AchCardRing<k>   a dark frame round the picture
//   AchCardIcon<k>   the picture, one FRAME per achievement (SetFrame)
//   AchCardName<k>   its name (line 1, left)
//   AchCardState<k>  "Earned 29 Sep 2026" / "Locked" (line 1, right)
//   AchCardDesc<k>   its text (line 2, small print)
// The texts' boxes here are where they sit at full size; options_menu.cpp
// scales the small ones and moves them so they keep their left / right edge.
// (Sizes and places: options_page.h THE ACHIEVEMENTS LIST.)
constexpr int kAchRingSide = 66;
constexpr int kAchTextX = kAchTextLeft;
constexpr uint32_t kAchRingColour = 0xA0000000;   // the pictures' frames: black, ~63 %
constexpr uint32_t kAchGlassColour = 0xDCFFFFFF;  // as the save list's panels (#ffffffdc)

// The sprites themselves (top-level chunks for RebuildProject's `extra`),
// and their names into `parts`.
std::vector<Bytes> AchievementSprites(Parts* parts) {
  std::vector<Bytes> chunks;
  for (const achievements::MenuSprite& s : achievements::MenuSprites()) {
    chunks.push_back(s.chunk);
    if (s.id) parts->ach_icons.push_back(s.name);
    else parts->ach_ring = s.name;
  }
  if (parts->ach_ring.empty()) parts->ach_icons.clear();
  return chunks;
}

// The Players screen's pictures (top-level chunks); `parts` notes that the
// cards can be built.
std::atomic<bool> g_players_cards{false};
std::vector<Bytes> PlayersSprites(Parts* parts) {
  std::vector<Bytes> chunks;
  for (const players_setup::Sprite& s : players_setup::MenuSprites()) chunks.push_back(s.chunk);
  parts->ply_pics.clear();
  if (!chunks.empty()) {
    parts->ply_pics.push_back(players_setup::CardBackName());
    g_players_cards = true;
  }
  return chunks;
}

// THE PLAYERS SCREEN (players/players_setup.h; the contract with
// options_menu.cpp, which fills and shows them; places options_page.h):
//   PlyBack<k>      the card's opaque teal backing (players_setup.h)
//   PlyCard<k>      the game's glass over it, at 60 % (the sheen)
//   PlyRing<k>      the selection ring round it (frames: yellow = selected,
//                   players 1-4's colours = the controller being moved)
//   PlyPic<k>       the device picture (frames: players_setup.h Picture)
//   PlyAura<k>      the glow of the player's colour from the card's outer
//                   corner (over the glass; players_setup.h AuraName)
//   PlyTitle<k>     the player's number, big, in that corner
//   PlyInfo<k>      the device's name / "Press any button to join" /
//                   whose a controller is while choosing (centred, small)
//   PlyStyle<k>     its button pictures ("Buttons: PlayStation 5"; only with
//                   a controller: LB / RB change them; centred, small)
//   PlyLeft<k> / PlyRight<k>  the chooser's arrows beside the picture (the
//                   map's arrow, turned by options_menu.cpp)
//   PlyPrompt<i>    the prompt row under the panel ("<A> Move Controller")
//   PlyHinge        the panel's hinge again, over the cards (Parts::ply_hinge;
//                   at the hinge's own place: the Players screen keeps the
//                   panel centred, so (57, 310) in both menus)
constexpr uint32_t kPlyGlassColour = 0x99FFFFFF;  // the glass over the backing (60 %)
void AddPlayersCards(const Parts& p, std::vector<Node>& elements) {
  if (p.ply_pics.empty()) return;
  auto picture = [&](std::string name, const std::vector<std::string>& sprites, int x, int y, int w, int h,
                     uint32_t colour) {
    Node n = p.arrow;
    pure3d::SetNodeName(n, name);
    if (!(sprites.size() == 1 ? SetPictureSprite(n, sprites[0]) : SetPictureSprites(n, sprites))) return false;
    SetBox(n, x, y, w, h);
    pure3d::SetNodeLe32(n, pure3d::NameEnd(n) + 29, colour);
    pure3d::SetNodeLe32(n, pure3d::NameEnd(n) + 20, kStretchToBox);
    elements.push_back(std::move(n));
    return true;
  };
  for (int k = 0; k < 4; ++k) {
    const int x = kPlyCardX[k % 2], top = kPlyCardTop[k / 2], bottom = top - kPlyCardH;
    const int m = kPlyRingMargin;
    if (!picture(fmt::format("PlyBack{}", k), {players_setup::CardBackName()}, x, bottom, kPlyCardW, kPlyCardH,
                 0xFFFFFFFF) ||
        !picture(fmt::format("PlyCard{}", k), {std::string(kCardSprites[0])}, x, bottom, kPlyCardW, kPlyCardH,
                 kPlyGlassColour) ||
        !picture(fmt::format("PlyAura{}", k), {players_setup::AuraName(k)}, x, bottom, kPlyCardW, kPlyCardH,
                 0xFFFFFFFF) ||
        !picture(fmt::format("PlyRing{}", k), players_setup::RingFrames(), x - m, bottom - m, kPlyCardW + 2 * m,
                 kPlyCardH + 2 * m, 0xFFFFFFFF) ||
        !picture(fmt::format("PlyPic{}", k), players_setup::PictureFrames(k), x + (kPlyCardW - kPlyPicSide) / 2,
                 top - kPlyPicCentre[k / 2] - kPlyPicSide / 2, kPlyPicSide, kPlyPicSide, 0xFFFFFFFF)) {
      return;
    }
    for (int side = 0; side < 2; ++side) {
      Node a = p.arrow;
      pure3d::SetNodeName(a, side == 0 ? fmt::format("PlyLeft{}", k) : fmt::format("PlyRight{}", k));
      const int ax = side == 0 ? x + kPlyArrowInset : x + kPlyCardW - kPlyArrowInset - kPlyArrowSide;
      SetBox(a, ax, top - kPlyPicCentre[k / 2] - kPlyArrowSide / 2, kPlyArrowSide, kPlyArrowSide);
      elements.push_back(std::move(a));
    }
    {  // the number: in the outer corner (options_menu.cpp sizes it)
      const int cx = k % 2 == 0 ? x + kPlyNumberInset : x + kPlyCardW - kPlyNumberInset;
      elements.push_back(MakeText(p.label, p.empty, fmt::format("PlyTitle{}", k), cx - 30,
                                  top - kPlyNumberCentreY[k / 2] - kLineHeight / 2, 60, kLineHeight, kCentre, kGreen));
    }
    elements.push_back(MakeText(p.label, p.empty, fmt::format("PlyInfo{}", k), x, top - kPlyInfoTop[k / 2] - kLineHeight,
                                kPlyCardW, kLineHeight, kCentre, kGreen));
    elements.push_back(MakeText(p.label, p.empty, fmt::format("PlyStyle{}", k), x, top - kPlyStyleTop[k / 2] - kLineHeight,
                                kPlyCardW, kLineHeight, kCentre, kCyan));
  }
  for (int i = 0; i < kPlyPrompts; ++i) {
    elements.push_back(MakeText(p.label, p.empty, fmt::format("PlyPrompt{}", i), 0, kPlyPromptTop - kLineHeight,
                                200, kLineHeight, kCentre, kCyan));
  }
  if (p.ply_hinge.id == kChunkPicture) {
    Node hinge = p.ply_hinge;
    pure3d::SetNodeName(hinge, "PlyHinge");
    SetBox(hinge, 57, 310, 100, 150);
    elements.push_back(std::move(hinge));
  }
}

// The in-game Options page's parts (`page` = InGame_Options_XENON.pag,
// `map` = InGame_Map.pag of the same project).
bool CutInGameParts(Node& page, const Node& map, Parts* parts, std::string* error) {
  Node* layer = Child(page, kChunkLayer);
  Node* menu = layer ? Child(*layer, kChunkMenu, kMenuName) : nullptr;
  Node* paused = layer ? Child(*layer, kChunkText, "Paused") : nullptr;
  Node* star = layer ? Child(*layer, kChunkPicture, "DialogueStar1") : nullptr;
  if (!menu || !paused || !star || menu->children.size() < 4) {
    *error = "the Options page isn't laid out as expected";
    return false;
  }
  // The row template: the Dialogue row (item -> look {label} + value {text}).
  const Node& row = menu->children[0];
  if (row.children.size() != 2 || row.children[0].children.size() != 1 ||
      row.children[1].children.size() != 1) {
    *error = "the Options rows aren't label + value";
    return false;
  }
  parts->label = row.children[0].children[0];
  parts->value = row.children[1].children[0];
  if (parts->label.id != kChunkText || parts->value.id != kChunkText ||
      parts->value.children.empty() || parts->value.children[0].id != kChunkString) {
    *error = "the Options row texts aren't as expected";
    return false;
  }
  parts->empty = parts->value.children[0];  // ingame "_Empty"
  parts->row = row;
  parts->menu = *menu;
  parts->menu.children.clear();
  parts->title = *paused;  // the screen action writes "P1 Paused" into it
  parts->star = *star;
  // The arrow picture, out of the map screen's page.
  for (const Node& l : map.children) {
    for (const Node& e : l.children) {
      if (e.id == kChunkPicture && pure3d::NodeName(e) == "TitleArrowLeft") parts->arrow = e;
    }
  }
  if (parts->arrow.id != kChunkPicture) {
    *error = "no map arrow picture";
    return false;
  }
  return true;
}

// The page's elements (the contract with options_menu.cpp: options_page.h).
std::vector<Node> BuildElements(const Parts& p) {
  std::vector<Node> elements = p.backdrop;
  // The card's glass first (under its texts): copies of the map's arrow
  // showing the glass pictures instead, each at its own shape.
  constexpr const char* kCardNames[3] = {"OptCardSmall", "OptCardLarge", "OptCardSquare"};
  for (int i = 0; i < 3; ++i) {
    Node card = p.arrow;
    pure3d::SetNodeName(card, kCardNames[i]);
    if (SetPictureSprite(card, kCardSprites[i])) {
      const int h = kCardWidth / kCardAspect[i];
      SetBox(card, kCardX, kCardCentreY - h / 2, kCardWidth, h);
      pure3d::SetNodeLe32(card, pure3d::NameEnd(card) + 29, kGlassColour);
      // Stretched to its box: the field after the box is 1 on the game's
      // glass panels and cogs (drawn at their box's size), 0 on the map's
      // arrow and the calibration grey scale (drawn at their picture's own
      // size: a 256 x 256 glass drew 257 units wide in a 210 box; photo,
      // 2026-10-11). tools/scrooby_dump.py calls it "frames".
      pure3d::SetNodeLe32(card, pure3d::NameEnd(card) + 20, kStretchToBox);
      elements.push_back(std::move(card));
    }
  }
  // The Achievements list (hidden unless the screen is in that mode).
  if (!p.ach_icons.empty()) {
    auto picture = [&](std::string name, const std::vector<std::string>& sprites, int x, int y, int w, int h,
                       uint32_t colour) {
      Node n = p.arrow;
      pure3d::SetNodeName(n, name);
      if (!(sprites.size() == 1 ? SetPictureSprite(n, sprites[0]) : SetPictureSprites(n, sprites))) return;
      SetBox(n, x, y, w, h);
      pure3d::SetNodeLe32(n, pure3d::NameEnd(n) + 29, colour);
      pure3d::SetNodeLe32(n, pure3d::NameEnd(n) + 20, kStretchToBox);
      elements.push_back(std::move(n));
    };
    elements.push_back(MakeText(p.label, p.empty, "AchHeading", 320 - 150, kAchHeadingY, 300, kLineHeight, kCentre,
                                kYellow));
    // The list's own arrows (the map's arrow, as the Options list's markers),
    // beside the first and last card: more above / below.
    {
      const int last_bottom = kAchCardsTop - kAchCardStep * (kAchCards - 1) - kAchCardH;
      for (const auto& [name, y] : {std::pair{"AchUp", kAchCardsTop - kArrowSize}, std::pair{"AchDown", last_bottom}}) {
        Node a = p.arrow;
        pure3d::SetNodeName(a, name);
        SetBox(a, kAchArrowX, y, kArrowSize, kArrowSize);
        elements.push_back(std::move(a));
      }
    }
    for (int k = 0; k < kAchCards; ++k) {
      const int top = kAchCardsTop - kAchCardStep * k, bottom = top - kAchCardH;
      picture(fmt::format("AchCard{}", k), {std::string(kCardSprites[0])}, kAchCardX, bottom, kAchCardW, kAchCardH,
              kAchGlassColour);
      const int icon_y = bottom + (kAchCardH - kAchIconSide) / 2;
      const int ring_off = (kAchRingSide - kAchIconSide) / 2;
      picture(fmt::format("AchCardIcon{}", k), p.ach_icons, kAchIconX, icon_y, kAchIconSide, kAchIconSide, 0xFFFFFFFF);
      picture(fmt::format("AchCardRing{}", k), {p.ach_ring}, kAchIconX - ring_off, icon_y - ring_off, kAchRingSide,
              kAchRingSide, kAchRingColour);
      elements.push_back(MakeText(p.label, p.empty, fmt::format("AchCardName{}", k), kAchTextX, top - 38,
                                  kAchTextRight - kAchTextX, kLineHeight, kLeft, kGreen));
      elements.push_back(MakeText(p.label, p.empty, fmt::format("AchCardState{}", k), kAchTextRight - 260, top - 38,
                                  260, kLineHeight, kRight, kYellow));
      elements.push_back(MakeText(p.label, p.empty, fmt::format("AchCardDesc{}", k), kAchTextX, top - 66,
                                  kAchTextRight - kAchTextX, kLineHeight, kLeft, kCyan));
    }
  }
  // The Players screen's cards (hidden unless the screen is in that mode).
  AddPlayersCards(p, elements);
  // The menu: kRows rows, each a copy of the Dialogue row.
  Node menu = p.menu;
  for (int r = 0; r < kRows; ++r) {
    const int y = kRowTopY - kRowStep * r;
    Node item = p.row;
    pure3d::SetNodeName(item, fmt::format("OptRow{}", r));
    item.children[0].children[0] = MakeText(p.label, p.empty, fmt::format("OptLabel{}", r),
                                            kLabelX, y, kLabelWidth, kLineHeight, kLeft, kGreen);
    item.children[1].children[0] = MakeText(p.value, p.empty, fmt::format("OptValue{}", r),
                                            kValueX, y, kValueWidth, kLineHeight, kRight, kGreen);
    menu.children.push_back(std::move(item));
  }
  elements.push_back(std::move(menu));
  // The title moves with the glass.
  {
    Node title = p.title;
    const size_t e = pure3d::NameEnd(title);
    SetBox(title, int(pure3d::NodeLe32(title, e + 4)) + kShift, int(pure3d::NodeLe32(title, e + 8)),
           int(pure3d::NodeLe32(title, e + 12)), int(pure3d::NodeLe32(title, e + 16)));
    elements.push_back(std::move(title));
  }
  constexpr const char* kTabNames[3] = {"OptTabPrev", "OptTab", "OptTabNext"};
  for (int t = 0; t < 3; ++t) {
    elements.push_back(MakeText(p.label, p.empty, kTabNames[t], kTabCentreX[t] - kTabWidth / 2,
                                kTabY, kTabWidth, kLineHeight, kCentre,
                                t == 1 ? kYellow : kFadedGreen));
  }
  for (int s = 0; s < 2; ++s) {
    elements.push_back(MakeText(p.label, p.empty, s == 0 ? "OptTabLB" : "OptTabRB",
                                kShoulderCentreX[s] - 25, kTabY, 50, kLineHeight, kCentre, kGreen));
  }
  for (int r = 0; r < kRows; ++r) {
    for (int k = 0; k < kStars; ++k) {
      Node s = p.star;
      pure3d::SetNodeName(s, fmt::format("OptStar{}_{}", r, k));
      SetBox(s, kStarsX + kStarStep * k, kRowTopY - kRowStep * r + kStarAboveRow, kStarSize, kStarSize);
      elements.push_back(std::move(s));
    }
  }
  // The page markers, in the prompts' line.
  for (int side = 0; side < 2; ++side) {
    Node a = p.arrow;
    pure3d::SetNodeName(a, side == 0 ? "OptPageUp" : "OptPageDown");
    const int centre = kGlassCentreX + (side == 0 ? -kPageArrowDX : kPageArrowDX);
    SetBox(a, centre - kArrowSize / 2, kPageArrowY, kArrowSize, kArrowSize);
    elements.push_back(std::move(a));
  }
  elements.push_back(MakeText(p.label, p.empty, "OptPage", kGlassCentreX - kPageTextWidth / 2,
                              kPageMarkersY, kPageTextWidth, kLineHeight, kCentre, kCyan));
  // The card's contents.
  constexpr int kInsideX = kCardX + kCardPadding, kInsideWidth = kCardWidth - 2 * kCardPadding;
  elements.push_back(MakeText(p.label, p.empty, "OptCardValue", kInsideX, kCardCentreY,
                              kInsideWidth, kLineHeight, kCentre, kYellow));
  if (p.guide.id == kChunkPicture) {
    Node g = p.guide;
    pure3d::SetNodeName(g, "OptGuide");
    SetBox(g, kCardX + kCardWidth / 2 - kGuideWidth / 2, kCardCentreY, kGuideWidth, kGuideHeight);
    elements.push_back(std::move(g));
  }
  for (const auto& [name, colour] : {std::pair{"OptDesc", kCyan}, std::pair{"OptWarn", kYellow}}) {
    Node t = MakeText(p.label, p.empty, name, kInsideX, kCardCentreY, kInsideWidth, kLineHeight,
                      kCentre, colour);
    SetVerticalJustify(t, kTop);
    elements.push_back(std::move(t));
  }
  return elements;
}

// The project `project_name` of a package with some of its pages changed:
// `edit(page name, page node)` returns false to fail, and is only called for
// the pages in `pages`. `extra` = top-level chunks put before the project
// (pictures it needs). Everything else is copied byte for byte. Returns
// false if the project or one of the pages isn't there.
using PageEdit = std::function<bool(std::string_view name, Node& page)>;
bool RebuildProject(const Bytes& original, std::string_view project_name,
                    const std::vector<std::string_view>& pages, const PageEdit& edit,
                    const std::vector<Bytes>& extra, Bytes* patched) {
  using pure3d::Chunk;
  Chunk root;
  std::vector<Chunk> top;
  if (!pure3d::Parse(original, &root, &top)) return false;
  Bytes body;
  size_t edited = 0;
  bool project_found = false;
  for (const Chunk& c : top) {
    const bool host = !project_found && c.id == pure3d::kChunkProject &&
                      pure3d::NameOf(original, c) == project_name;
    if (!host) {
      const Bytes chunk = pure3d::Copy(original, c);
      body.insert(body.end(), chunk.begin(), chunk.end());
      continue;
    }
    project_found = true;
    for (const Bytes& e : extra) body.insert(body.end(), e.begin(), e.end());
    Bytes project_children;
    for (const Chunk& k : pure3d::ChildrenOf(original, c)) {
      Bytes chunk;
      const std::string_view name = pure3d::NameOf(original, k);
      if (k.id == pure3d::kChunkPage && std::find(pages.begin(), pages.end(), name) != pages.end()) {
        Node page = pure3d::ToNode(original, k);
        if (!edit(name, page)) return false;
        chunk = pure3d::ToBytes(page);
        ++edited;
      } else {
        chunk = pure3d::Copy(original, k);
      }
      project_children.insert(project_children.end(), chunk.begin(), chunk.end());
    }
    const Bytes project = pure3d::WithChildren(original, c, project_children);
    body.insert(body.end(), project.begin(), project.end());
  }
  if (!project_found || edited != pages.size()) return false;
  *patched = pure3d::WithChildren(original, root, body);
  return true;
}

// The in-game Options page and the map page of an in-game menus package.
bool FindInGamePages(const Bytes& d, Node* options, Node* map) {
  pure3d::Chunk root;
  std::vector<pure3d::Chunk> top;
  if (!pure3d::Parse(d, &root, &top)) return false;
  for (const pure3d::Chunk& c : top) {
    if (c.id != pure3d::kChunkProject || pure3d::NameOf(d, c) != kHostProject) continue;
    for (const pure3d::Chunk& k : pure3d::ChildrenOf(d, c)) {
      if (k.id != pure3d::kChunkPage) continue;
      if (pure3d::NameOf(d, k) == kPageName) *options = pure3d::ToNode(d, k);
      if (pure3d::NameOf(d, k) == kMapPageName) *map = pure3d::ToNode(d, k);
    }
  }
  return options->id == pure3d::kChunkPage && map->id == pure3d::kChunkPage;
}

// The calibration screen's grey scale (page GameStart_VideoCalibration of
// the main menus' package): its picture element, and the picture itself
// (for packages that don't have it).
constexpr std::string_view kMenusPackage = "package\\cdd70a8c.p3d";
constexpr std::string_view kGuideSprite = "FE_calibration_greyscale.tga";
bool CutGuide(const Bytes& menus, Node* picture, Bytes* sprite) {
  pure3d::Chunk root;
  std::vector<pure3d::Chunk> top;
  if (!pure3d::Parse(menus, &root, &top)) return false;
  for (const pure3d::Chunk& c : top) {
    if (c.id == pure3d::kChunkSprite && pure3d::NameOf(menus, c) == kGuideSprite) {
      *sprite = pure3d::Copy(menus, c);
    }
    if (c.id != pure3d::kChunkProject) continue;
    for (const pure3d::Chunk& k : pure3d::ChildrenOf(menus, c)) {
      if (k.id != pure3d::kChunkPage || pure3d::NameOf(menus, k) != "GameStart_VideoCalibration.pag") {
        continue;
      }
      Node page = pure3d::ToNode(menus, k);
      if (Node* layer = Child(page, kChunkLayer)) {
        if (Node* p = Child(*layer, kChunkPicture, "BrightnessScale")) *picture = *p;
      }
    }
  }
  return picture->id == kChunkPicture && !sprite->empty();
}

// ---------------------------------------------------------------------------
// In game: every in-game menus package (one per level group)
// ---------------------------------------------------------------------------

// THE PAUSE MENU'S "Achievements" (2026-10-25, achievements_screen.h): the
// two pause pages the pause state shows (InGame_Pause, one player:
// Resume, Missions, Tutorials, Options, Save, Quit; InGame_Pause2Player:
// ... Drop Out before Options) get a copy of OptionsItem right after it,
// renamed AchievementsItem (its words go in at run time: the in-game text
// bank has none). The items are spread over the same span as before (the
// first at y 280, the last at 70 at most): 35 apart (the game's) when they
// fit, closer when not (two players: 8 items, 30 apart).
constexpr std::string_view kPausePages[] = {"InGame_Pause.pag", "InGame_Pause2Player.pag"};
// DROP OUT LEAVES THE PAUSE MENU (2026-10-30, players/pause_drop_out.h): the
// two-player pause pages lose their "Drop Out" item (DropOutMenuItem, 4th);
// a player drops out with Back (Select) instead. The page without Save (the
// game's other pause screen, InGame_Pause_NoSave.scr) only loses the item:
// it gets no extras. The decisions still count Drop Out's place
// (achievements_screen.cpp ShiftPauseIndex puts it back in their numbers).
constexpr std::string_view kPauseNoSave2Player = "InGame_Pause2Player_NoSave.pag";
constexpr std::string_view kDropOutItem = "DropOutMenuItem";
std::atomic<bool> g_drop_out_removed{false}, g_drop_out_removed_no_save{false};
// THE ACHIEVEMENT CARD, ON THE MESSAGE BAR'S PAGE (InGame_MessageBar.pag:
// the game's pop-up at the bottom; achievements.cpp puts an earned
// achievement in the bar's queue). The bar's own wide glass and texts are
// hidden while one of ours is up and this card shows instead, so it is drawn
// by the game and slides with the bar's page (canvas units, y up; at 720p
// x = 160 + 1.5 u, y = 720 - 1.5 u: 520 x 100 pixels, 30 above the bottom):
//   AchPopGlass  the save list's glass (FE_Common_GlassPanel_Rectangle_Small),
//                white at 78 %, x 147-493, y 20-87
//   AchBarIcon   the achievement's picture, one FRAME each (SetFrame), 45 x 45
//   AchBarRing   its dark frame, 49 x 49
//   AchPopTitle  "Achievement Unlocked" (cyan, small)
//   AchPopName   its name (yellow)
// The texts are placed and sized by achievements.cpp (it shrinks long names).
// (The bar's own glass: 600 x 180 at (20, -45), its lower part under the
// screen's edge; title at y 90, message at y 55.)
constexpr std::string_view kMessageBarPage = "InGame_MessageBar.pag";
constexpr int kPopX = 147, kPopY = 20, kPopW = 346, kPopH = 67;
constexpr int kBarIconSide = 45, kBarRingSide = 49, kBarIconX = kPopX + 11, kBarIconY = kPopY + (kPopH - 45) / 2;
constexpr uint32_t kPopGlassColour = 0xC8FFFFFF;  // white at 78 %, as the in-game glass panel
constexpr int kPauseTopY = 280, kPauseLowestY = 70, kPauseStep = 35;
std::atomic<bool> g_pause_patched{false};

// Spreads a pause menu's items from y `top` down (35 apart, the game's
// step, or closer when they don't fit above y 70).
void SpacePauseItems(std::vector<Node>& items, int top) {
  int count = 0;
  for (const Node& n : items) count += n.id == kChunkItem;
  const int step = count > 1 ? std::min(kPauseStep, (top - kPauseLowestY) / (count - 1)) : kPauseStep;
  int i = 0;
  for (Node& item : items) {
    if (item.id != kChunkItem || item.children.empty() || item.children[0].children.empty()) continue;
    Node& text = item.children[0].children[0];
    const size_t e = pure3d::NameEnd(text);
    SetBox(text, int(pure3d::NodeLe32(text, e + 4)), top - step * i, int(pure3d::NodeLe32(text, e + 12)),
           int(pure3d::NodeLe32(text, e + 16)));
    ++i;
  }
}

// Takes "Drop Out" out of a two-player pause page's menu (true = it was there).
bool RemoveDropOut(Node& page) {
  Node* layer = Child(page, kChunkLayer);
  Node* menu = layer ? Child(*layer, kChunkMenu, "PauseMenu") : nullptr;
  if (!menu) return false;
  auto& items = menu->children;
  const auto drop = std::find_if(items.begin(), items.end(), [](const Node& n) {
    return n.id == kChunkItem && pure3d::NodeName(n) == kDropOutItem;
  });
  if (drop == items.end()) return false;
  items.erase(drop);
  return true;
}

bool AddPauseAchievements(Node& page) {
  Node* layer = Child(page, kChunkLayer);
  Node* menu = layer ? Child(*layer, kChunkMenu, "PauseMenu") : nullptr;
  if (!menu) return false;
  auto& items = menu->children;
  const auto options = std::find_if(items.begin(), items.end(), [](const Node& n) {
    return n.id == kChunkItem && pure3d::NodeName(n) == "OptionsItem";
  });
  if (options == items.end() || options->children.empty() || options->children[0].children.empty()) return false;
  // Drop Out goes (two players' page: pause_drop_out.h); Options moves up one.
  const bool drop_out = RemoveDropOut(page);
  // "Players" first, then "Achievements" (both after Options).
  Node players = *options;
  pure3d::SetNodeName(players, "PlayersItem");
  pure3d::SetNodeName(players.children[0].children[0], "PlayersValue");
  Node copy = *options;
  pure3d::SetNodeName(copy, "AchievementsItem");
  pure3d::SetNodeName(copy.children[0].children[0], "AchievementsValue");
  const auto at = items.insert(options + 1, players);
  items.insert(at + 1, copy);
  SpacePauseItems(items, kPauseTopY);
  if (drop_out) g_drop_out_removed = true;
  return true;
}

bool PatchInGameMenus(const Bytes& original, Bytes* patched) {
  Node options, map;
  if (!FindInGamePages(original, &options, &map)) return false;  // another package
  Parts parts;
  std::string error;
  if (!CutInGameParts(options, map, &parts, &error)) {
    REXLOG_WARN("Options: the game's own Options screen stays ({})", error);
    return false;
  }
  // The Brightness guide, from the main menus' package (cut once a run).
  static Node guide;
  static Bytes guide_sprite;
  static bool guide_cut = false;
  if (!guide_cut) {
    guide_cut = true;
    Bytes menus;
    if (!data_patcher::ReadArchiveFile(kMenusPackage, &menus, &error) ||
        !CutGuide(menus, &guide, &guide_sprite)) {
      REXLOG_WARN("Options: no Brightness guide picture in game");
      guide = Node();
      guide_sprite.clear();
    }
  }
  parts.guide = guide;
  std::vector<Bytes> extra;
  if (!guide_sprite.empty()) extra.push_back(guide_sprite);
  for (Bytes& b : AchievementSprites(&parts)) extra.push_back(std::move(b));
  for (Bytes& b : PlayersSprites(&parts)) extra.push_back(std::move(b));
  // The pause pages that this package has (RebuildProject wants every page
  // it's given; a package without one keeps its pause menu as it was).
  std::vector<std::string_view> pages{kPageName};
  {
    pure3d::Chunk root;
    std::vector<pure3d::Chunk> top;
    pure3d::Parse(original, &root, &top);
    for (const pure3d::Chunk& c : top) {
      if (c.id != pure3d::kChunkProject || pure3d::NameOf(original, c) != kHostProject) continue;
      for (const pure3d::Chunk& k : pure3d::ChildrenOf(original, c)) {
        if (k.id == pure3d::kChunkPage && pure3d::NameOf(original, k) == "InGame_NV_GlassPanel.pag") {
          Node glass_page = pure3d::ToNode(original, k);
          for (Node& layer : glass_page.children) {  // (2nd layer: BackgroundLayer, GlassPanelLayer)
            if (layer.id != kChunkLayer) continue;
            if (Node* h = Child(layer, kChunkPicture, "Hinge")) parts.ply_hinge = *h;
          }
        }
        for (std::string_view p : kPausePages) {
          if (k.id == pure3d::kChunkPage && pure3d::NameOf(original, k) == p) pages.push_back(p);
        }
        if (k.id == pure3d::kChunkPage && pure3d::NameOf(original, k) == kPauseNoSave2Player) {
          pages.push_back(kPauseNoSave2Player);
        }
        if (!parts.ach_icons.empty() && k.id == pure3d::kChunkPage && pure3d::NameOf(original, k) == kMessageBarPage) {
          pages.push_back(kMessageBarPage);
        }
      }
    }
  }
  const bool ok = RebuildProject(
      original, kHostProject, pages,
      [&](std::string_view name, Node& page) {
        if (name == kMessageBarPage) {
          Node* layer = Child(page, kChunkLayer);
          if (!layer) return true;
          auto picture = [&](const char* element, const std::vector<std::string>& sprites, int x, int y, int side,
                             uint32_t colour) {
            Node n = parts.arrow;
            pure3d::SetNodeName(n, element);
            if (!(sprites.size() == 1 ? SetPictureSprite(n, sprites[0]) : SetPictureSprites(n, sprites))) return;
            SetBox(n, x, y, side, side);
            pure3d::SetNodeLe32(n, pure3d::NameEnd(n) + 29, colour);
            pure3d::SetNodeLe32(n, pure3d::NameEnd(n) + 20, kStretchToBox);
            layer->children.push_back(std::move(n));
          };
          // A picture element stretched to its box (the glass is 4:1, the box
          // ~5:1: picture() takes a square side, so the glass is made here).
          {
            Node glass = parts.arrow;
            pure3d::SetNodeName(glass, "AchPopGlass");
            if (SetPictureSprite(glass, kCardSprites[0])) {
              SetBox(glass, kPopX, kPopY, kPopW, kPopH);
              pure3d::SetNodeLe32(glass, pure3d::NameEnd(glass) + 29, kPopGlassColour);
              pure3d::SetNodeLe32(glass, pure3d::NameEnd(glass) + 20, kStretchToBox);
              layer->children.push_back(std::move(glass));
            }
          }
          const int off = (kBarRingSide - kBarIconSide) / 2;
          picture("AchBarIcon", parts.ach_icons, kBarIconX, kBarIconY, kBarIconSide, 0xFFFFFFFF);
          picture("AchBarRing", {parts.ach_ring}, kBarIconX - off, kBarIconY - off, kBarRingSide, kAchRingColour);
          const int text_x = kBarIconX + kBarIconSide + 11, text_w = kPopX + kPopW - 11 - text_x;
          layer->children.push_back(
              MakeText(parts.label, parts.empty, "AchPopTitle", text_x, kPopY + 36, text_w, kLineHeight, kLeft, kCyan));
          layer->children.push_back(
              MakeText(parts.label, parts.empty, "AchPopName", text_x, kPopY + 8, text_w, kLineHeight, kLeft, kYellow));
          return true;
        }
        if (name == kPauseNoSave2Player) {
          // Only Drop Out goes; the rest keep the game's spacing from the top.
          Node* layer = Child(page, kChunkLayer);
          Node* menu = layer ? Child(*layer, kChunkMenu, "PauseMenu") : nullptr;
          if (menu && RemoveDropOut(page)) {
            SpacePauseItems(menu->children, 270);  // the page's first item (Resume) sits at y 270
            g_drop_out_removed_no_save = true;
          }
          return true;
        }
        if (name == kPausePages[0] || name == kPausePages[1]) {
          if (!AddPauseAchievements(page)) {
            REXLOG_WARN("Achievements: {} isn't as expected: no Achievements item there", name);
            return true;  // the page as it was: the rest of the patch still counts
          }
          g_pause_patched = true;
          return true;
        }
        Node* layer = Child(page, kChunkLayer);
        if (!layer) return false;
        layer->children = BuildElements(parts);
        return true;
      },
      extra, patched);
  if (ok && !g_patched.exchange(true)) {
    REXLOG_INFO("Options: the port's Options page replaces InGame_Options_XENON in the in-game menus");
  }
  return ok;
}

// ---------------------------------------------------------------------------
// The main menu (package cdd70a8c, GameStart.prj)
// ---------------------------------------------------------------------------
// * GameStart_Start_Xenon.pag: the main menu's list gets "Options" as its
//   third item: a copy of the PS2 main menu's OptionsItem (its text is the
//   game's own translated "Options", GameStart_MainMenu_Options).
// * GameStart_OptionsPS2.pag: the PS2 Options page (unused on the Xbox; the
//   front end's OptionsScreen state 96 shows it) becomes our page, with the
//   glass panel, cogs and hinge of the in-game screen (its own pictures,
//   moved), its menu keeping the name OptionsMenu (the state's track names
//   it) and a title "Options" (the same translated text).
// The row parts come from an in-game menus package (read out of
// default.rcf): the main menu package has every picture but the stars,
// which are copied in.

constexpr std::string_view kMainMenuProject = "GameStart.prj";
constexpr std::string_view kMainMenuPage = "GameStart_Start_Xenon.pag";
constexpr std::string_view kMainMenuPS2Page = "GameStart_Start_PS2.pag";
constexpr std::string_view kMainOptionsPage = "GameStart_OptionsPS2.pag";
constexpr std::string_view kMainOptionsMenu = "OptionsMenu";
constexpr std::string_view kInGamePackage = "package\\7a8185b0.p3d";  // any in-game menus package
// The main menu's items, as the game lays out its four: 50 units apart from
// y 295. Calibration (the original's TV brightness test screen) gives its
// place to Options, its grey scale becomes the Brightness row's guide
// (2026-10-10). "Quit Game" last (2026-10-25, ui/quit_game.h: a PC game
// closes from its own menu; the Xbox had the console's Guide button). Five
// items at the original's 50 units would end at y 95, close to the prompt
// bar: 45 apart keeps the list's top where it was. (An "Achievements" item
// was here for a few days; it lives in the pause menu only now: the
// achievements are earned in play.)
// Six items since "Players" (2026-10-29), 40 apart; since EXTRAS (2026-10-30,
// ui/main_menu_extras.h) two sets of at most five, shown one at a time: 45
// apart again, each set from the top.
constexpr int kMainItemTopY = 295, kMainItemStep = 45;
constexpr std::string_view kCalibrationItem = "CalibrationItem";

std::atomic<bool> g_main_menu_patched{false};

bool PatchMainMenu(const Bytes& original, Bytes* patched) {
  using pure3d::Chunk;
  // Only the main menu package (the one with GameStart.prj).
  {
    Chunk root;
    std::vector<Chunk> top;
    if (!pure3d::Parse(original, &root, &top)) return false;
    if (std::none_of(top.begin(), top.end(), [&](const Chunk& c) {
          return c.id == pure3d::kChunkProject && pure3d::NameOf(original, c) == kMainMenuProject;
        })) {
      return false;
    }
  }
  auto fail = [](const std::string& why) {
    REXLOG_WARN("Options: no Options in the main menu ({})", why);
    return false;
  };
  // The row parts and the star pictures, from an in-game menus package.
  Bytes ingame;
  std::string error;
  if (!data_patcher::ReadArchiveFile(kInGamePackage, &ingame, &error)) return fail(error);
  Node options, map;
  if (!FindInGamePages(ingame, &options, &map)) return fail("the in-game menus aren't as expected");
  Parts parts;
  if (!CutInGameParts(options, map, &parts, &error)) return fail(error);
  std::vector<Bytes> stars;
  {
    Chunk root;
    std::vector<Chunk> top;
    pure3d::Parse(ingame, &root, &top);
    for (const Chunk& c : top) {
      const std::string_view name = pure3d::NameOf(ingame, c);
      if (c.id == pure3d::kChunkSprite && (name == "FE_star_open.tga" || name == "FE_star_closed.tga")) {
        stars.push_back(pure3d::Copy(ingame, c));
      }
    }
  }
  if (stars.size() != 2) return fail("no star pictures");

  // From the main menu package itself: the PS2 main menu's Options item, and
  // the pictures + a "_Empty" of the PS2 Options page (its texts use the
  // menus' own text bank, which is loaded here; the in-game one isn't).
  Node ps2_item;
  {
    Chunk root;
    std::vector<Chunk> top;
    pure3d::Parse(original, &root, &top);
    for (const Chunk& c : top) {
      if (c.id != pure3d::kChunkProject || pure3d::NameOf(original, c) != kMainMenuProject) continue;
      for (const Chunk& k : pure3d::ChildrenOf(original, c)) {
        if (k.id != pure3d::kChunkPage || pure3d::NameOf(original, k) != kMainMenuPS2Page) continue;
        Node page = pure3d::ToNode(original, k);
        if (Node* layer = Child(page, kChunkLayer)) {
          if (Node* menu = Child(*layer, kChunkMenu)) {
            if (Node* item = Child(*menu, kChunkItem, "OptionsItem")) ps2_item = *item;
          }
        }
      }
    }
  }
  if (ps2_item.id != kChunkItem) return fail("no PS2 Options item");
  Node main_guide;
  Bytes unused_sprite;  // already in this package
  CutGuide(original, &main_guide, &unused_sprite);
  // The Achievements screen's pictures (achievements.h MenuSprites).
  for (Bytes& b : AchievementSprites(&parts)) stars.push_back(std::move(b));
  for (Bytes& b : PlayersSprites(&parts)) stars.push_back(std::move(b));

  const bool ok = RebuildProject(
      original, kMainMenuProject, {kMainMenuPage, kMainOptionsPage},
      [&](std::string_view name, Node& page) {
        Node* layer = Child(page, kChunkLayer);
        if (!layer) return false;
        if (name == kMainMenuPage) {
          Node* menu = Child(*layer, kChunkMenu);
          if (!menu || menu->children.size() != 4) return false;  // the original's four
          auto& items = menu->children;
          const auto calibration = std::find_if(items.begin(), items.end(), [](const Node& n) {
            return pure3d::NodeName(n) == kCalibrationItem;
          });
          if (calibration == items.end()) return false;
          items.erase(calibration);
          items.insert(items.begin() + kMainMenuOptionsItem, ps2_item);
          // The order (options_page.h): New Game, Load Game, Options, EXTRAS,
          // QUIT GAME | PLAYERS, Credits. The new items are copies of
          // Options' (Extras, Players) or Credits' (Quit) item, renamed
          // (siblings keep unique names); their words go in at run time
          // (achievements_screen.cpp, quit_game.cpp, main_menu_extras.cpp:
          // the menus' text bank has none of these strings).
          if (items.size() != 4) return false;  // New, Load, Options, Credits
          const Node credits = items[3];
          items.pop_back();
          auto copy = [&](const Node& from, const char* item_name, const char* text_name) {
            Node n = from;
            pure3d::SetNodeName(n, item_name);
            if (n.children.empty() || n.children[0].children.empty()) return false;
            pure3d::SetNodeName(n.children[0].children[0], text_name);
            items.push_back(std::move(n));
            return true;
          };
          if (!copy(ps2_item, "ExtrasItem", "ExtrasItemText") || !copy(credits, "QuitItem", "QuitItemText") ||
              !copy(ps2_item, "PlayersItem", "PlayersItemText")) {
            return false;
          }
          items.push_back(credits);
          if (int(items.size()) != kMainMenuItemCount) return false;
          // Evenly apart (each item's text: look -> text), each set CENTRED on
          // the same middle (the first set's: its third item, y 205).
          for (size_t i = 0; i < menu->children.size(); ++i) {
            Node& item = menu->children[i];
            if (item.children.empty() || item.children[0].children.empty()) return false;
            Node& text = item.children[0].children[0];
            const size_t e = pure3d::NameEnd(text);
            const bool top = int(i) < kMainMenuTopItems;
            const int row = top ? int(i) : int(i) - kMainMenuTopItems;
            const int count = top ? kMainMenuTopItems : kMainMenuItemCount - kMainMenuTopItems;
            const int middle = kMainItemTopY - kMainItemStep * (kMainMenuTopItems - 1) / 2;
            const int y = middle + kMainItemStep * (count - 1) / 2 - kMainItemStep * row;
            SetBox(text, int(pure3d::NodeLe32(text, e + 4)), y,
                   int(pure3d::NodeLe32(text, e + 12)), int(pure3d::NodeLe32(text, e + 16)));
          }
          return true;
        }
        // The PS2 Options page -> ours.
        Node* panel = Child(*layer, kChunkPicture, "OptionsMenu_Panel");
        Node* cog = Child(*layer, kChunkPicture, "cog1");
        Node* hinge = Child(*layer, kChunkPicture, "OptionsMenu_PanelHinge");
        Node* menu = Child(*layer, kChunkMenu, kMainOptionsMenu);
        if (!panel || !cog || !hinge || !menu || menu->children.empty()) return false;
        // A "_Empty" of the menus' bank: the Sound row's third value.
        Node* sound = Child(*menu, kChunkItem, "SoundMenu");
        if (!sound || sound->children.size() < 2 || sound->children[1].children.empty() ||
            sound->children[1].children[0].children.size() < 3) {
          return false;
        }
        Parts p = parts;
        p.guide = main_guide;
        p.empty = sound->children[1].children[0].children[2];
        // The menu keeps the page's own header (its name: the track finds it).
        p.menu = *menu;
        p.menu.children.clear();
        // The title: the in-game title's place, the translated "Options".
        p.title.children = {ps2_item.children[0].children[0].children[0]};
        pure3d::SetNodeName(p.title, "OptTitle");
        // The in-game screen's glass panel page, rebuilt from this page's own
        // pictures (InGame_NV_GlassPanel: panel 500 x 354 at (70, 34), 78 %
        // opaque; cogs at (98, 448) and (39, 395); hinge at (57, 310)).
        // All moved left by kShift, as options_menu.cpp moves the in-game
        // page (the card stands on the right).
        Node glass = *panel;
        SetBox(glass, 70 + kShift, 34, 500, 354);
        pure3d::SetNodeLe32(glass, pure3d::NameEnd(glass) + 29, kGlassColour);
        Node cog_a = *cog, cog_b = *cog;
        // cog_a keeps the PS2 page's name "cog1": the screen's slide-in
        // (options_menu.cpp, the game's own code) turns it, found by that
        // name; the panel and hinge keep theirs too, and the layer its
        // "OptionsMenu" (the slide moves the layer).
        pure3d::SetNodeName(cog_a, "cog1");
        pure3d::SetNodeName(cog_b, "OptCog2");
        SetBox(cog_a, 98 + kShift, 448, 90, 90);
        SetBox(cog_b, 39 + kShift, 395, 90, 90);
        p.ply_hinge = *hinge;
        Node hinge_copy = *hinge;
        SetBox(hinge_copy, 57 + kShift, 310, 100, 150);
        p.backdrop = {glass, cog_a, cog_b, hinge_copy};
        layer->children = BuildElements(p);
        return true;
      },
      stars, patched);
  if (!ok) return fail("the main menu pages aren't as expected");
  if (!g_main_menu_patched.exchange(true)) {
    REXLOG_INFO("Options: \"Options\" added to the main menu (its page replaces GameStart_OptionsPS2)");
  }
  return true;
}

// ---------------------------------------------------------------------------
// THE SELECTED ROW PULSES ("jumps"), like every selected item in the game
// ---------------------------------------------------------------------------
// The pulse is the menu's own update (Scrooby Menu vtable slot 5,
// sub_8237A070: the selected item's label and value scaled by a sine of a
// timer at menu +148), which CMenuAction's Update (sub_820D2A00, at
// 0x820D2A44) calls only when byte +41 of its FEMenu TRACK is set. That
// comes from the FEMenu item in the state's action list in Frontend.bfig
// (type 0x7B641ABE: u32, f32, three strings (screen, page, menu: u32
// length + bytes padded to 4), then four u32): the THIRD u32 is 1 in the
// pause menu's and the main menu's items (their selections pulse) and 0 in
// the in-game Options' Xbox branch (node 958) and the main menu's
// OptionsScreen (96): the original Options rows never pulsed. Both set to 1
// (read 2026-10-11; first tried: adding an FEMenuCursor track as the pause
// states have, but its CMenuCursorAction (vtable 0x82023B4C) does nothing on
// the Xbox: Enter = Update = sub_820E77F0, "return 1").
constexpr uint32_t kTrackMenu = 0x7B641ABE;
constexpr size_t kMenuItemPulseField = 2;  // of the four u32 after the strings
struct PulseFor {
  int node;
  std::string_view node_name;
};
constexpr PulseFor kPulses[] = {{958, "ContentXenon"}, {96, "OptionsScreen"}};
std::atomic<bool> g_pulse_patched{false};

uint32_t Be32At(const Bytes& b, size_t p) {
  return uint32_t(b[p]) << 24 | uint32_t(b[p + 1]) << 16 | uint32_t(b[p + 2]) << 8 | b[p + 3];
}

bool PatchMenuPulse(const Bytes& original, Bytes* patched) {
  fight_tree::Tree tree;
  if (!tree.Load(original)) return false;
  for (const PulseFor& n : kPulses) {
    if (n.node > tree.Count() || tree.NameOf(n.node) != n.node_name) {
      REXLOG_WARN("Options: the selection doesn't pulse (node {} isn't {})", n.node, n.node_name);
      return false;
    }
    Bytes record = tree.Record(n.node);
    size_t p = 8;
    while (p + 4 <= record.size() && Be32At(record, p) != kTrackMenu) p += 4;
    if (p + 12 > record.size()) return false;
    p += 12;  // type, u32, f32
    for (int str = 0; str < 3 && p + 4 <= record.size(); ++str) {
      p += 4 + ((size_t(Be32At(record, p)) + 3) & ~size_t(3));
    }
    const size_t field = p + 4 * kMenuItemPulseField;
    if (field + 4 > record.size()) return false;
    record[field] = record[field + 1] = record[field + 2] = 0;
    record[field + 3] = 1;
    tree.Replace(n.node, record);
  }
  *patched = tree.Save();
  if (!g_pulse_patched.exchange(true)) {
    REXLOG_INFO("Options: the selected row pulses (FEMenu tracks of front end nodes 958 and 96)");
  }
  return true;
}


}  // namespace

void Register() {
  data_patcher::Register("options page", data_patcher::kCategoryFrontend, "", PatchInGameMenus);
  data_patcher::Register("options selection pulse", data_patcher::kCategoryFightTree, "Frontend",
                         PatchMenuPulse);
  data_patcher::Register("main menu options", data_patcher::kCategoryFrontend, "cdd70a8c",
                         PatchMainMenu);
}

bool Available() { return g_patched.load(); }
bool MainMenuAvailable() { return g_main_menu_patched.load(); }
bool PauseAchievementsAvailable() { return g_pause_patched.load(); }
bool PauseDropOutRemoved() { return g_drop_out_removed.load(); }
bool PauseNoSaveDropOutRemoved() { return g_drop_out_removed_no_save.load(); }
bool PlayersCardsAvailable() { return g_players_cards.load(); }

}  // namespace options_page
