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
#include "../data/pure3d.h"

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
constexpr uint32_t kRight = 1, kCentre = 4;

// ---------------------------------------------------------------------------
// The layout, in Scrooby units (640 x 480, y up, (x, y) = bottom-left).
// The glass panel behind it spans x 70-570, y 34-388; the game's own
// "Paused" title sits at y ~356, its "Back" prompt at y 42-69.
// ---------------------------------------------------------------------------
constexpr int kTabY = 312;                          // the row of tabs
// Three tab names: the current one in the middle (yellow), the previous and
// next ones (wrapping around) beside it, faded: there is more to either side.
constexpr int kTabCentreX[3] = {190, 320, 450};     // previous, current, next
constexpr int kTabWidth = 130;
constexpr int kShoulderCentreX[2] = {105, 540};     // LB / RB pictures
constexpr int kRowTopY = 270;                       // row 0's y
constexpr int kRowStep = 33;                        // rows are this far apart (the pause menu's: 35)
constexpr int kLabelX = 10, kLabelWidth = 290;      // right aligned: ends at x 300
// The value column is a SELECTOR: the map screen's arrow picture on both
// ends (shown on the selected row, options_menu.cpp turns them to point
// left / right), the value centred between them, the stars too.
constexpr int kSelectorX = 312, kSelectorEnd = 568;  // the value: 192 units ("Frame Everyone" fits)
constexpr int kArrowSize = 32, kArrowAboveRow = 2;
constexpr int kValueX = kSelectorX + kArrowSize, kValueWidth = kSelectorEnd - kArrowSize - kValueX;
constexpr int kStarStep = 30;                       // stars: 32 x 32, 30 apart (as the game's)
constexpr int kStarAboveRow = 5;
constexpr int kStarsX = (kSelectorX + kSelectorEnd) / 2 - (kStarStep * (kStars - 1) + 32) / 2;
constexpr int kLineHeight = 36;                     // a text line's box (the game's rows)
// The description and warning boxes: full width, centred, drawn smaller by
// options_menu.cpp (scaled around their centre, so they stay put).
constexpr int kWarnY = 106, kWarnHeight = 30;
// The Brightness row's guide (the original's calibration grey scale): a
// picture is drawn at its own size (320 x 64 units here), hanging from the
// TOP of its box (checked on a photo, 2026-10-10: the box's size didn't
// matter). Its top at y 140, centred; options_menu.cpp draws it at 0.6 x
// around its centre: between the rows and the description, where a warning
// would go (that row has none).
constexpr int kGuideX = 160, kGuideY = 104, kGuideWidth = 320, kGuideHeight = 36;
// PAGES (a tab with more than kRows rows): the map's arrow beside the glass
// on the right (panel edge x 570), level with the first and the last row,
// pointing up / down (options_menu.cpp turns them), and "1/2" between them.
constexpr int kPageArrowX = 574;
constexpr int kPageTextX = 562, kPageTextWidth = 60;
constexpr int kDescY = 56, kDescHeight = 60;  // two lines clear the prompts (y 42-69)
constexpr int kWideX = 20, kWideWidth = 600;

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
};

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
  // The menu: kRows rows, each a copy of the Dialogue row.
  Node menu = p.menu;
  for (int r = 0; r < kRows; ++r) {
    const int y = kRowTopY - kRowStep * r;
    Node item = p.row;
    pure3d::SetNodeName(item, fmt::format("OptRow{}", r));
    item.children[0].children[0] = MakeText(p.label, p.empty, fmt::format("OptLabel{}", r),
                                            kLabelX, y, kLabelWidth, kLineHeight, kRight, kGreen);
    item.children[1].children[0] = MakeText(p.value, p.empty, fmt::format("OptValue{}", r),
                                            kValueX, y, kValueWidth, kLineHeight, kCentre, kGreen);
    menu.children.push_back(std::move(item));
  }
  elements.push_back(std::move(menu));
  elements.push_back(p.title);
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
      const size_t e = pure3d::NameEnd(s);
      const int w = int(pure3d::NodeLe32(s, e + 12)), h = int(pure3d::NodeLe32(s, e + 16));
      SetBox(s, kStarsX + kStarStep * k, kRowTopY - kRowStep * r + kStarAboveRow, w, h);
      elements.push_back(std::move(s));
    }
    for (int side = 0; side < 2; ++side) {
      Node a = p.arrow;
      pure3d::SetNodeName(a, fmt::format("OptArrow{}{}", side == 0 ? 'L' : 'R', r));
      SetBox(a, side == 0 ? kSelectorX : kSelectorEnd - kArrowSize,
             kRowTopY - kRowStep * r + kArrowAboveRow, kArrowSize, kArrowSize);
      elements.push_back(std::move(a));
    }
  }
  if (p.guide.id == kChunkPicture) {
    Node g = p.guide;
    pure3d::SetNodeName(g, "OptGuide");
    SetBox(g, kGuideX, kGuideY, kGuideWidth, kGuideHeight);
    elements.push_back(std::move(g));
  }
  for (int side = 0; side < 2; ++side) {
    Node a = p.arrow;
    pure3d::SetNodeName(a, side == 0 ? "OptPageUp" : "OptPageDown");
    const int row = side == 0 ? 0 : kRows - 1;
    SetBox(a, kPageArrowX, kRowTopY - kRowStep * row + kArrowAboveRow, kArrowSize, kArrowSize);
    elements.push_back(std::move(a));
  }
  elements.push_back(MakeText(p.label, p.empty, "OptPage", kPageTextX,
                              kRowTopY - kRowStep * (kRows - 1) / 2, kPageTextWidth, kLineHeight,
                              kCentre, kCyan));
  elements.push_back(MakeText(p.label, p.empty, "OptWarn", kWideX, kWarnY, kWideWidth, kWarnHeight,
                              kCentre, kYellow));
  elements.push_back(MakeText(p.label, p.empty, "OptDesc", kWideX, kDescY, kWideWidth, kDescHeight,
                              kCentre, kCyan));
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
  const bool ok = RebuildProject(
      original, kHostProject, {kPageName},
      [&](std::string_view, Node& page) {
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
// y 295. Still four: Calibration (the original's TV brightness test screen)
// gives its place to Options, its grey scale becomes the Brightness row's
// guide (2026-10-10).
constexpr int kMainItemTopY = 295, kMainItemStep = 50;
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

  const bool ok = RebuildProject(
      original, kMainMenuProject, {kMainMenuPage, kMainOptionsPage},
      [&](std::string_view name, Node& page) {
        Node* layer = Child(page, kChunkLayer);
        if (!layer) return false;
        if (name == kMainMenuPage) {
          Node* menu = Child(*layer, kChunkMenu);
          if (!menu || menu->children.size() != 4) return false;
          auto& items = menu->children;
          const auto calibration = std::find_if(items.begin(), items.end(), [](const Node& n) {
            return pure3d::NodeName(n) == kCalibrationItem;
          });
          if (calibration == items.end()) return false;
          items.erase(calibration);
          items.insert(items.begin() + kMainMenuOptionsItem, ps2_item);
          // Evenly apart (each item's text: look -> text).
          for (size_t i = 0; i < menu->children.size(); ++i) {
            Node& item = menu->children[i];
            if (item.children.empty() || item.children[0].children.empty()) return false;
            Node& text = item.children[0].children[0];
            const size_t e = pure3d::NameEnd(text);
            SetBox(text, int(pure3d::NodeLe32(text, e + 4)), kMainItemTopY - kMainItemStep * int(i),
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
        Node glass = *panel;
        SetBox(glass, 70, 34, 500, 354);
        pure3d::SetNodeLe32(glass, pure3d::NameEnd(glass) + 29, 0xC8FFFFFF);
        Node cog_a = *cog, cog_b = *cog;
        pure3d::SetNodeName(cog_a, "OptCog1");
        pure3d::SetNodeName(cog_b, "OptCog2");
        SetBox(cog_a, 98, 448, 90, 90);
        SetBox(cog_b, 39, 395, 90, 90);
        Node hinge_copy = *hinge;
        SetBox(hinge_copy, 57, 310, 100, 150);
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

}  // namespace

void Register() {
  data_patcher::Register("options page", data_patcher::kCategoryFrontend, "", PatchInGameMenus);
  data_patcher::Register("main menu options", data_patcher::kCategoryFrontend, "cdd70a8c",
                         PatchMainMenu);
}

bool Available() { return g_patched.load(); }
bool MainMenuAvailable() { return g_main_menu_patched.load(); }

}  // namespace options_page
