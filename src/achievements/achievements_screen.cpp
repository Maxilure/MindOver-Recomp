// =============================================================================
// achievements/achievements_screen.cpp -- see achievements_screen.h
// =============================================================================
#include "achievements_screen.h"

#include <atomic>
#include <string>
#include <string_view>

#include <rex/logging.h>
#include <rex/ppc/func.h>

#include "../guest_memory.h"
#include "../input/mouse_menus.h"
#include "../options/options_page.h"
#include "../ui/scrooby.h"

extern "C" REX_FUNC(__imp__sub_82121B48);  // the pause menu's decision (node 868)

namespace achievements_screen {
namespace {

std::atomic<bool> g_open_next{false};

// Scrooby menu item: +140 its look (the Text drawn); Text +152 its string.
constexpr uint32_t kItemLook = 140, kTextString = 152;
constexpr std::u16string_view kWords = u"Achievements";

// The pause pages and where their new item is (options_page.cpp puts it
// right after Options).
struct PausePage {
  std::string_view name;
  int item;           // the Achievements item's index
  int32_t options_exit;  // the decision's exit for Options (-> state 956)
};
constexpr PausePage kPausePages[] = {{"InGame_Pause", 4, 894}, {"InGame_Pause2Player", 5, 883}};

// While the pause decision runs: which pause page asked, and whether the
// Achievements item was the one picked.
bool g_in_pause_decision = false;
bool g_picked = false;

uint32_t Read32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = GuestPtr(base, a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

std::string GuestString(const uint8_t* base, uint32_t a) {
  std::string out;
  for (uint32_t i = 0; a && i < 64; ++i) {
    const char c = char(*GuestPtr(base, a + i));
    if (!c) break;
    out.push_back(c);
  }
  return out;
}

// Whether a Text object already says `words` (UTF-16, big-endian).
bool Says(const uint8_t* base, uint32_t text, std::u16string_view words) {
  const uint32_t s = Read32(base, text + kTextString);
  if (!s) return false;
  for (size_t i = 0; i <= words.size(); ++i) {
    const uint8_t* p = GuestPtr(base, s + 2 * uint32_t(i));
    if (uint16_t(p[0] << 8 | p[1]) != (i < words.size() ? uint16_t(words[i]) : 0)) return false;
  }
  return true;
}

// Item `index` of `menu_name` on `page_name` says kWords.
void Word(PPCContext& ctx, uint8_t* base, std::string_view page_name, std::string_view menu_name, int index) {
  const uint32_t page = scrooby::FindPage(ctx, base, page_name);
  const uint32_t menu = page ? scrooby::FindMenu(ctx, base, page, menu_name) : 0;
  if (!menu || scrooby::ItemCount(base, menu) <= index) return;
  const uint32_t item = scrooby::Item(base, menu, index);
  const uint32_t text = item ? Read32(base, item + kItemLook) : 0;
  if (text && !Says(base, text, kWords)) scrooby::SetText(ctx, base, text, kWords);
}

// The GetMenuIndex filter (mouse_menus.h): only while the pause decision
// runs, and only for the pause pages.
int ShiftPauseIndex(uint8_t* base, uint32_t page_name, int index) {
  if (!g_in_pause_decision || !page_name || index < 0) return index;
  const std::string name = GuestString(base, page_name);
  for (const PausePage& p : kPausePages) {
    if (name != p.name) continue;
    if (index == p.item) {
      g_picked = true;
      return p.item - 1;  // Options' index: the decision takes the Options exit
    }
    return index > p.item ? index - 1 : index;
  }
  return index;
}

}  // namespace

void OpenNext() {
  g_open_next = true;
  REXLOG_INFO("Achievements: the screen opens next");
}

bool TakeOpenNext() { return g_open_next.exchange(false); }
bool OpenNextPending() { return g_open_next.load(); }

void FrontEndFrame(PPCContext& ctx, uint8_t* base) {
  if (options_page::PauseAchievementsAvailable()) {
    for (const PausePage& p : kPausePages) Word(ctx, base, p.name, "PauseMenu", p.item);
  }
}

void Register() { mouse_menus::SetMenuIndexFilter(&ShiftPauseIndex); }

}  // namespace achievements_screen

// The pause menu's decision: its own index -> exit mapping, with the
// Achievements item counted out (ShiftPauseIndex) and turned into the
// Options exit + "open as the Achievements screen".
extern "C" REX_FUNC(sub_82121B48) {
  using namespace achievements_screen;
  if (!options_page::PauseAchievementsAvailable()) {
    __imp__sub_82121B48(ctx, base);
    return;
  }
  g_in_pause_decision = true;
  g_picked = false;
  __imp__sub_82121B48(ctx, base);
  g_in_pause_decision = false;
  const int32_t exit = int32_t(ctx.r3.u32);
  if (g_picked && (exit == kPausePages[0].options_exit || exit == kPausePages[1].options_exit)) {
    OpenNext();
  }
  g_picked = false;
}
