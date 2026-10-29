// =============================================================================
// input/mouse_screens.cpp -- see mouse_screens.h
// =============================================================================
#include "mouse_screens.h"

#include <chrono>

#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "../data/fight_tree.h"
#include "../guest_memory.h"
#include "../ui/scrooby.h"
#include "keyboard_mouse.h"
#include "mouse_menus.h"

namespace mouse_screens {
namespace {

using namespace rex::input;  // X_INPUT_GAMEPAD_*

// One picture screen: its front-end state, its page and its picture.
struct Screen {
  int32_t state;            // front-end state (fig_tree.py numbers)
  const char* page;         // its Scrooby page (GameStart.prj)
  const char* picture;      // the big picture; nullptr = clicking it does nothing
  const char* name;         // for the log
  bool clicks = true;       // false: the wheel only (the screen has its own mouse)
};
constexpr Screen kScreens[] = {
    {988, "GameStart_MovieSelection", "MovieImageSprite", "TV (movies)"},
    {865, "GameStart_ConceptArt", "MenuSprite", "comic (concept art)"},
    {860, "GameStart_EnemyInfo", nullptr, "tiki face (enemy info)"},
    // The chest = the port's own outfit picker (players/outfit_picker.cpp),
    // with its own mouse (swatches, arrows): only the wheel here.
    {847, "GameStart_SkinSelection", nullptr, "chest (outfits)", false},
};
constexpr int32_t kEnemyInfoState = 860;
// The tiki face's enemy model (a 3D actor, not a page element) stands right
// of the description panel: this canvas box (640 x 480, y up) holds every
// model seen (Ratnician x 413-493 y 100-247, Ratcicle 347-560 x 133-360,
// read from photos) and stays below the portrait row (its panel starts at
// y 298). A click there = A: the game plays that enemy's sound (its own A
// on this screen, 0x820C92D4; the keyboard's is Space / Enter).
constexpr float kModelX0 = 345, kModelY0 = 40, kModelX1 = 575, kModelY1 = 295;

// The tiki face's enemy table (mouse_screens.h): 20 entries of 84 bytes,
// the portrait picture at +76; the selected portrait is drawn at alpha 255.
constexpr uint32_t kEnemyTable = 0x8259FCA8;
constexpr uint32_t kEnemyEntrySize = 84;
constexpr uint32_t kEnemyPortrait = 76;
constexpr int kEnemies = 20;
constexpr uint32_t kElementAlpha = 80;  // a Scrooby element's colour word: alpha = its top byte (SetAlpha sub_82370860)

uint32_t Read32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = GuestPtr(base, a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// The screen whose state the front end is in, nullptr if none.
const Screen* CurrentScreen() {
  const int32_t state = fight_tree::CurrentFrontEnd().state;
  for (const Screen& s : kScreens) {
    if (s.state == state) return &s;
  }
  return nullptr;
}

// The page's elements, found once per visit (pages stay loaded while their
// screen is up; a new visit can be a new load, so we look again then).
struct Found {
  int32_t state = -1;
  uint32_t page = 0, left = 0, right = 0, picture = 0;
} g_found;

// Tiki face: a click on a portrait = steps toward it (see the header), ONE
// press at a time: after a step the screen loads the next enemy and drops
// presses meanwhile (tested: two pulses at once moved one place). So the
// clicked portrait becomes a TARGET, and the next press goes out once the
// last one has moved the selection (or after 1.5 s, given up after 4 s).
struct PortraitWalk {
  int target = -1;          // the clicked enemy (-1 = none)
  int selected_at_press = -1;
  std::chrono::steady_clock::time_point pressed{}, started{};
} g_walk;

// The selected enemy (its portrait drawn opaque) and the clicked one (-1).
int SelectedPortrait(uint8_t* base, int* clicked) {
  int selected = -1;
  for (int i = 0; i < kEnemies; ++i) {
    const uint32_t picture = Read32(base, kEnemyTable + uint32_t(i) * kEnemyEntrySize + kEnemyPortrait);
    if (!picture || !scrooby::Visible(base, picture)) continue;
    if (*GuestPtr(base, picture + kElementAlpha) == 0xFF) selected = i;
    if (clicked && *clicked < 0 && mouse_menus::TakeClickOver(base, picture, g_found.page)) *clicked = i;
  }
  return selected;
}

void WalkToPortrait(uint8_t* base, kbm::KeyboardMouseDriver* driver, bool click_waiting) {
  int clicked = -1;
  const int selected = SelectedPortrait(base, click_waiting ? &clicked : nullptr);
  const auto now = std::chrono::steady_clock::now();
  if (clicked >= 0 && selected >= 0) {
    g_walk = {};
    g_walk.target = clicked;
    g_walk.started = now;
    REXLOG_INFO("Mouse in menus: tiki face portrait {} clicked (selected {})", clicked, selected);
  }
  if (g_walk.target < 0) return;
  if (selected == g_walk.target || selected < 0 || now - g_walk.started > std::chrono::seconds(4)) {
    g_walk.target = -1;
    return;
  }
  const bool moved = selected != g_walk.selected_at_press;
  if (driver->PulseBusy() || (!moved && now - g_walk.pressed < std::chrono::milliseconds(1500))) return;
  driver->Pulse(g_walk.target < selected ? X_INPUT_GAMEPAD_DPAD_LEFT : X_INPUT_GAMEPAD_DPAD_RIGHT);
  g_walk.selected_at_press = selected;
  g_walk.pressed = now;
}

// A waiting left click inside a canvas box (where it was MADE): taken.
bool TakeClickInBox(kbm::KeyboardMouseDriver* driver, float x0, float y0, float x1, float y1) {
  float px = 0, py = 0;
  if (mouse_menus::MouseBlocked() || !driver->PeekMenuClick(&px, &py)) return false;
  const float cx = (px * 1280.0f - 160.0f) / 1.5f, cy = (1.0f - py) * 480.0f;  // mouse_menus.h's mapping
  return cx >= x0 && cx <= x1 && cy >= y0 && cy <= y1 && driver->TakeMenuClick();
}

}  // namespace

void FrontEndFrame(PPCContext& ctx, uint8_t* base) {
  const Screen* screen = CurrentScreen();
  kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  if (!screen || !driver) {
    g_found = {};
    g_walk.target = -1;
    return;
  }
  if (g_found.state != screen->state || !g_found.page) {
    g_found = {};
    g_found.state = screen->state;
    g_found.page = scrooby::FindPage(ctx, base, screen->page);
    if (!g_found.page) return;
    g_found.left = scrooby::FindPicture(ctx, base, g_found.page, "LeftArrow");
    g_found.right = scrooby::FindPicture(ctx, base, g_found.page, "RightArrow");
    g_found.picture = screen->picture ? scrooby::FindPicture(ctx, base, g_found.page, screen->picture) : 0;
    REXLOG_INFO("Mouse in menus: the {} screen (arrows {:08X} / {:08X}, picture {:08X})", screen->name,
                g_found.left, g_found.right, g_found.picture);
  }
  // A row of pictures: the wheel steps through it (mouse_menus.h).
  mouse_menus::ClaimWheel(mouse_menus::Wheel::kLeftRight);
  // Clicks only while the screen takes input (not while it slides in / out,
  // nor while a movie plays: the TV clears the same flag then).
  const bool input = mouse_menus::MenusTakeInput(base) && screen->clicks;
  if (screen->state == kEnemyInfoState) {
    if (!input) g_walk.target = -1;
    else WalkToPortrait(base, driver, driver->PeekMenuClick());
  }
  if (!input || !driver->PeekMenuClick()) return;
  // Hidden arrows (the first / last picture) and pictures never match.
  if (mouse_menus::TakeClickOver(base, g_found.left, g_found.page)) {
    driver->Pulse(X_INPUT_GAMEPAD_DPAD_LEFT);
  } else if (mouse_menus::TakeClickOver(base, g_found.right, g_found.page)) {
    driver->Pulse(X_INPUT_GAMEPAD_DPAD_RIGHT);
  } else if (g_found.picture && mouse_menus::TakeClickOver(base, g_found.picture, g_found.page)) {
    driver->Pulse(X_INPUT_GAMEPAD_A);
  } else if (screen->state == kEnemyInfoState && TakeClickInBox(driver, kModelX0, kModelY0, kModelX1, kModelY1)) {
    driver->Pulse(X_INPUT_GAMEPAD_A);  // the enemy's sound
    REXLOG_INFO("Mouse in menus: tiki face character clicked");
  }
}

}  // namespace mouse_screens
