// =============================================================================
// saves/save_screen_waits.cpp -- see save_screen_waits.h
// =============================================================================
#include "save_screen_waits.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string_view>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../data/data_patcher.h"
#include "../data/fight_tree.h"
#include "../guest_memory.h"
#include "../pddi/intercept.h"
#include "../ui/scrooby.h"

REXCVAR_DEFINE_BOOL(save_screen_waits, false, "CrashMoM",
                    "Keep the Xbox 360's minimum times on the save screens (\"Accessing ... save game "
                    "files\" 4 s, \"Saving\" 3 s, loading a save 3 s); off = move on as soon as the "
                    "save files are read / written");

REXCVAR_DEFINE_BOOL(save_success_screens, false, "CrashMoM",
                    "Show the Xbox 360's \"Load successful.\" / \"Save successful.\" screens (each waits "
                    "for A); off = straight on, as if A had been pressed");

REXCVAR_DEFINE_INT32(save_message_delay_ms, 1500, "CrashMoM",
                     "How long the save screens' \"Reading save files...\" / \"Saving...\" message and "
                     "its dimmed background wait before showing (a quick read or save then shows "
                     "nothing at all); 0 = right away, like the Xbox");

extern "C" REX_FUNC(__imp__sub_8211AF40);  // the front end's decision dispatcher (data/fight_tree.cpp)

namespace {

// Front-end states (fight tree node numbers, save_screen_waits.h) whose
// decision waits for a minimum time in state.
constexpr int32_t kWaitingStates[] = {
    4, 8,                      // boot: CheckMemoryCard, ReadingCard
    113, 125, 153, 278, 289,   // menus: AccessMemoryCard, DeviceSelectorOpen, ReadingCard,
                               //        LoadGameScreen, SaveGameScreen
    359, 366, 384, 326,        // in game: AccessMemoryCard, ShowDeviceSelector, ReadingCard,
                               //          AutoSaveGameScreen
};

// Front-end states that show a storage message ("Wait..." text ids in the
// tree: WaitChecking = reading the saves, WaitSaving / WaitAutoSaving,
// WaitLoading) on the message page. 4 (boot) has none; 127 / 368
// StateBeginAccess show it without a minimum time.
constexpr int32_t kMessageStates[] = {8, 113, 125, 127, 153, 278, 289, 326, 359, 366, 368, 384};

// The time the state has been up, as each decision reads it (r3 = the front
// end's state machine): lwz r11,176(r3); lwz r9,28(r11); lfs f13,132(r9).
constexpr uint32_t kMachineToTimer = 176;
constexpr uint32_t kTimerToClock = 28;
constexpr uint32_t kClockTimeInState = 132;  // float, seconds

// What the decision sees instead: longer than any of the minimums (3 s).
constexpr float kLongEnough = 3600.0f;

uint32_t Load32(uint8_t* base, uint32_t address) {
  const uint8_t* p = GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  uint8_t* p = GuestPtr(base, address);
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

// The address of the time-in-state float, 0 if a pointer on the way is null.
uint32_t TimeInStateAddress(uint8_t* base, uint32_t machine) {
  if (!machine) return 0;
  const uint32_t timer = Load32(base, machine + kMachineToTimer);
  if (!timer) return 0;
  const uint32_t clock = Load32(base, timer + kTimerToClock);
  if (!clock) return 0;
  return clock + kClockTimeInState;
}

// --- The message, a moment late -------------------------------------------
// Without the waits, reading the saves takes a few frames: the message page
// (its full-screen black "Background" polygon fading in to dim the picture,
// and its "Message" text) blinked by. So both stay hidden (Scrooby element
// visibility, +110 bit 0x80, scrooby::SetVisible) until the storage work
// has gone on for --save_message_delay_ms; only a slow read or save shows
// them. The page is the game's own: FE_TRG_Message_InGame in game,
// FE_TRG_Message_Frontend / FE_TRG_Message in the menus (findings/30). A
// "route" = the message states asked one after another (in game 359 -> 366
// -> 368 -> 359 -> 384, a few frames each); it ends when none of them has
// been asked for kRouteGap (checked at every frame end; a single frame
// between two states mustn't end it, or the restore would flash the page):
// then the elements are shown again, for the screens that use the same page
// next (the overwrite question, error messages).
constexpr const char* kMessagePages[] = {"FE_TRG_Message_InGame", "FE_TRG_Message_Frontend",
                                         "FE_TRG_Message"};

constexpr auto kRouteGap = std::chrono::milliseconds(100);

struct MessageHold {
  bool in_route = false;
  std::chrono::steady_clock::time_point route_start;
  std::chrono::steady_clock::time_point last_ask;
  uint8_t* base = nullptr;
  std::array<uint32_t, 6> hidden{};  // elements we hid (0 = none)
} g_hold;

void ShowHidden() {
  for (uint32_t& element : g_hold.hidden) {
    if (element) scrooby::SetVisible(g_hold.base, element, true);
    element = 0;
  }
}

// In a message state's decision (every frame while it's up).
void HoldMessage(PPCContext& ctx, uint8_t* base) {
  const auto now = std::chrono::steady_clock::now();
  if (!g_hold.in_route) {
    g_hold.in_route = true;
    g_hold.route_start = now;
  }
  g_hold.last_ask = now;
  g_hold.base = base;
  const auto delay = std::chrono::milliseconds(std::max(0, REXCVAR_GET(save_message_delay_ms)));
  if (now - g_hold.route_start >= delay) {
    ShowHidden();  // still busy: the message shows, as on the Xbox
    return;
  }
  size_t slot = 0;
  for (const char* name : kMessagePages) {
    const uint32_t page = scrooby::FindPage(ctx, base, name);
    if (!page) continue;
    for (const uint32_t element : {scrooby::FindPolygon(ctx, base, page, "Background"),
                                   scrooby::FindText(ctx, base, page, "Message")}) {
      if (element && slot < g_hold.hidden.size()) {
        scrooby::SetVisible(base, element, false);
        g_hold.hidden[slot++] = element;
      }
    }
  }
}

// Frame end: no message state for kRouteGap = the route is over.
void OnFrameEnd(void*) {
  if (!g_hold.in_route || std::chrono::steady_clock::now() - g_hold.last_ask < kRouteGap) return;
  g_hold.in_route = false;
  ShowHidden();
}

bool IsMessageState(int32_t state) {
  return std::find(std::begin(kMessageStates), std::end(kMessageStates), state) !=
         std::end(kMessageStates);
}
bool IsWaitingState(int32_t state) {
  return std::find(std::begin(kWaitingStates), std::end(kWaitingStates), state) !=
         std::end(kWaitingStates);
}

// The state's node number: the low half of the node's word +28 (the
// dispatcher: lwz r11,28(r4); extsh).
int32_t StateNumber(uint8_t* base, uint32_t node) {
  return node ? int16_t(Load32(base, node + 28) & 0xFFFF) : -1;
}

// The game's own decision for the state, asked as if its minimum time had
// passed; the message held back a moment. ctx holds the dispatcher's
// arguments (r3 machine, r4 node...).
int32_t AskWithoutTheWait(PPCContext& ctx, uint8_t* base) {
  const int32_t state = StateNumber(base, ctx.r4.u32);
  if (IsMessageState(state)) {
    HoldMessage(ctx, base);
  }
  const uint32_t time = REXCVAR_GET(save_screen_waits) || !IsWaitingState(state)
                            ? 0
                            : TimeInStateAddress(base, ctx.r3.u32);
  uint32_t real = 0;
  if (time) {
    real = Load32(base, time);
    uint32_t bits;
    std::memcpy(&bits, &kLongEnough, 4);
    Store32(base, time, bits);
  }
  __imp__sub_8211AF40(ctx, base);  // -> the state's compiled decision
  if (time) {
    Store32(base, time, real);  // the state's clock goes on as before
  }
  return int32_t(ctx.r3.u32);
}

// --- The "successful" screens ----------------------------------------------
// "Load successful." / "Save successful." wait for A: their decisions ask the
// menus' IsButtonPressed (sub_822652B0) for A and return one exit when it's
// down, -1 until then. Ours asks the game's decision first (its other exits,
// e.g. the attract demo's, keep working) and, when it would go on waiting,
// takes the exit A takes:
//   282 LoadComplete (sub_8211CE30) -> 283 ExitToLevel
//   293 SaveComplete (sub_8211CF90, New Game's first save) -> 295 ExitFINAL
//       (-> FadeOut 296, the fade into the level: a real transition, kept)
//   329 AutoSaveSuccessful (sub_8211D3F8, saving in game) -> 331
//       ExitEndAccessFirst while the save manager's mode is 2 (save), else
//       332 ExitToInGame (both -> 486 InGame), as its two A branches do.
constexpr int32_t kLoadComplete = 282, kExitToLevel = 283;
constexpr int32_t kSaveComplete = 293, kExitFinal = 295;
constexpr int32_t kAutoSaveSuccessful = 329, kExitEndAccessFirst = 331, kExitToInGame = 332;

// The save manager's mode (findings/24: CSaveGameManager = *(0x8259B190)+80,
// +4 = 0 none 1 bootup 2 save 3 autosave 4 load); -1 if there's none.
constexpr uint32_t kGameGlobal = 0x8259B190;
int32_t SaveMode(uint8_t* base) {
  const uint32_t game = Load32(base, kGameGlobal);
  const uint32_t manager = game ? Load32(base, game + 80) : 0;
  return manager ? int32_t(Load32(base, manager + 4)) : -1;
}

int32_t SkipSuccessScreen(PPCContext& ctx, uint8_t* base) {
  const int32_t state = StateNumber(base, ctx.r4.u32);
  __imp__sub_8211AF40(ctx, base);  // -> the state's compiled decision
  const int32_t exit = int32_t(ctx.r3.u32);
  if (exit >= 0 || REXCVAR_GET(save_success_screens)) {
    return exit;
  }
  switch (state) {
    case kLoadComplete: return kExitToLevel;
    case kSaveComplete: return kExitFinal;
    case kAutoSaveSuccessful: return SaveMode(base) == 2 ? kExitEndAccessFirst : kExitToInGame;
    default: return exit;
  }
}

// --- The wording ---------------------------------------------------------
// The messages are strings of the menu packages' text banks (findings/30:
// chunk 0x1800D, one sub-chunk per language): UTF-16 big-endian, each ended
// by a 0 character, packed one after another and found through an offset
// table. Their English forms (the title's M / O are special characters of
// the font, so only the ends are matched):
//   "Accessing Crash: Mind Over Mutant save game files. Please do not turn
//    off the Xbox 360 console."           (before the save lists)
//   "Saving Crash: Mind Over Mutant game, please do not turn off the Xbox
//    360 console."                        (saving, menus)
//   "Saving content.  Please do not turn off your console."   (saving, in game)
//   "Loading content.  Please do not turn off your console."  (loading a save)
// in package/cdd70a8c.p3d (main menus) and the in-game menus' packages
// (7a8185b0, 7a88cba0, 97597d1b, c1e387c7). The PC text goes over the old one
// IN PLACE, followed by 0 characters up to the old end: every offset and
// chunk size stays as it was. Other languages are left alone. (The "Please
// do not touch the Power Button or RESET" forms are other consoles' and
// never shown here.)

// The endings that make a string one of these messages.
constexpr std::u16string_view kConsoleEndings[] = {
    u"Please do not turn off the Xbox 360 console.",
    u"please do not turn off the Xbox 360 console.",
    u"Please do not turn off your console.",
};

// The PC text for a message, by its first word (empty = not ours).
std::u16string_view PcWording(std::u16string_view message) {
  if (message.starts_with(u"Accessing")) return u"Reading save files...";
  if (message.starts_with(u"Saving")) return u"Saving...";
  if (message.starts_with(u"Loading")) return u"Loading...";
  return {};
}

// UTF-16 big-endian character at byte offset `at`.
char16_t CharAt(const data_patcher::Bytes& data, size_t at) {
  return char16_t(data[at] << 8 | data[at + 1]);
}

// Data patch for every menu package: swaps the messages' wording. False =
// none in this package (the game gets its own file).
bool PatchWording(const data_patcher::Bytes& original, data_patcher::Bytes* patched) {
  *patched = original;
  data_patcher::Bytes& data = *patched;
  int swapped = 0;
  for (const std::u16string_view ending : kConsoleEndings) {
    // The ending as big-endian bytes.
    data_patcher::Bytes pattern;
    for (const char16_t c : ending) {
      pattern.push_back(uint8_t(c >> 8));
      pattern.push_back(uint8_t(c));
    }
    auto hit = data.begin();
    while ((hit = std::search(hit, data.end(), pattern.begin(), pattern.end())) != data.end()) {
      const size_t end = size_t(hit - data.begin()) + pattern.size();  // its 0 character
      hit += pattern.size();
      if (end + 2 > data.size() || CharAt(data, end) != 0) continue;
      // Back to the string's first character: right after the previous 0.
      size_t start = end - pattern.size();
      while (start >= 2 && CharAt(data, start - 2) != 0) start -= 2;
      std::u16string message;
      for (size_t at = start; at < end; at += 2) message += CharAt(data, at);
      const std::u16string_view pc = PcWording(message);
      if (pc.empty() || pc.size() >= message.size()) continue;
      size_t at = start;
      for (const char16_t c : pc) {
        data[at] = uint8_t(c >> 8);
        data[at + 1] = uint8_t(c);
        at += 2;
      }
      std::fill(data.begin() + ptrdiff_t(at), data.begin() + ptrdiff_t(end), uint8_t(0));
      ++swapped;
    }
  }
  return swapped > 0;
}

}  // namespace

namespace save_screen_waits {

void Install() {
  for (const int32_t state : kWaitingStates) {
    fight_tree::SetFrontEndDecision(state, AskWithoutTheWait);
  }
  for (const int32_t state : kMessageStates) {
    fight_tree::SetFrontEndDecision(state, AskWithoutTheWait);  // same one (127 / 368: no timer)
  }
  pddi::AddFrameEndListener(&OnFrameEnd, nullptr);
  for (const int32_t state : {kLoadComplete, kSaveComplete, kAutoSaveSuccessful}) {
    fight_tree::SetFrontEndDecision(state, SkipSuccessScreen);
  }
  data_patcher::Register("save messages' PC wording", data_patcher::kCategoryFrontend, "",
                         PatchWording);
}

}  // namespace save_screen_waits
