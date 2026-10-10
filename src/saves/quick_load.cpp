// =============================================================================
// saves/quick_load.cpp -- see quick_load.h
// =============================================================================
#include "quick_load.h"
#include "../guest_memory.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "save_files.h"
#include "save_library.h"

// The originals of the decisions we answer differently.
extern "C" REX_FUNC(__imp__sub_8211B4E0);  // front end: LoadPersistentPostBootUp's decision (node 44)
extern "C" REX_FUNC(__imp__sub_8211B5A8);  // front end: LoadingTitleScreen's decision (node 53)
extern "C" REX_FUNC(__imp__sub_8211CE30);  // front end: LoadComplete's decision (node 282)
// What START on the title screen does (the front end's input update,
// sub_82264988, at 0x8226520C / 0x82265218), called by us instead:
extern "C" REX_FUNC(__imp__sub_82266150);  // front end: player slot r4 = controller r5 (+ (2131 + slot) * 4)
extern "C" REX_FUNC(sub_8227CEA8);  // controller r4 starts playing (sets +352; the PC version, players/who_plays.h)

REXCVAR_DEFINE_STRING(load_save, "", "CrashMoM",
                      "Start straight into a save, skipping the intros, title and menus: "
                      "its number (7), its name (\"ice tower\"; or a unique part of it), or "
                      "\"last\" (the most recently played). Empty = boot normally");

namespace quick_load {
namespace {

// The front end's tree (findings/24 sections 2.4 + 7, findings/25): node
// numbers as the 360 release's Frontend.bfig numbers them.
constexpr int32_t kExitPlayMovie = 50;     // 44's ExitPlayMovie -> 51 Movie (the intro)
constexpr int32_t kExitSkipToLevel = 49;   // 44's SkipToLevel -> 486 (Radical's debug switch)
constexpr int32_t kExitIntoTitle = 52;    // Movie's ExitTitleScreen -> 53; its script: GotoGameState(LEVEL_SELECT)
constexpr int32_t kExitTitleLoaded = 54;  // LoadingTitleScreen's ExitLoadingComplete -> 55 the title
constexpr int32_t kExitLoadGame = 87;      // main menu's (Xenon) ExitLoadGame -> 113 AccessMemoryCard
constexpr int32_t kExitIntoSlotList = 159; // ReadingCard's ExitHasValidSaveFiles -> 160 the list
constexpr int32_t kExitLoadSlot1 = 187;    // slot 1's ExitSlotNotEmpty (load) -> 278 LoadGameScreen
constexpr int32_t kExitToLevel = 283;      // LoadComplete's ExitToLevel -> 486 InGame

// The game object and its front end (findings/24).
constexpr uint32_t kGameGlobal = 0x8259B190;
constexpr uint32_t kGameFrontEnd = 52;
constexpr uint32_t kGameActiveController = 352;  // s32, -1 = nobody pressed START yet
constexpr uint32_t kController = 0;  // the first controller (keyboard + mouse plays as it too)

uint32_t Read32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// Calls a game function with up to three arguments; the caller's registers
// are kept.
void CallGame(void (*function)(PPCContext&, uint8_t*), PPCContext& ctx, uint8_t* base,
              uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  function(ctx, base);
  ctx = saved;
}

// Where the quick load is (game thread only).
enum class Step {
  kOff,        // no --load_save, done, or given up
  kBooting,    // waiting for the boot to reach node 44
  kToTitle,    // took the way into the menus' game state: waiting for its loading
  kToList,     // took "Load Game": waiting for the scan (ReadingCard)
  kLoading,    // the save is being read: waiting for "Load successful"
};
Step g_step = Step::kBooting;
int g_number = 0;  // the save file chosen
const auto g_start = std::chrono::steady_clock::now();

double Seconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
}

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return text;
}

// A save's name, as UTF-8 (the game's names are ASCII in practice; other
// characters become '?', which only matters for matching).
std::string NameOf(int number) {
  constexpr size_t kNameOffset = 20, kNameChars = 32;  // block +20, UTF-16 BE (save_files.h)
  uint8_t block[kNameOffset + 2 * kNameChars] = {};
  if (!save_files::ReadBlockStart(number, block, sizeof(block))) {
    return {};
  }
  std::string name;
  for (size_t i = 0; i < kNameChars; ++i) {
    const char16_t c = char16_t(block[kNameOffset + 2 * i] << 8 | block[kNameOffset + 2 * i + 1]);
    if (c == 0) break;
    name.push_back(c < 0x80 ? char(c) : '?');
  }
  return name;
}

// --load_save -> a save file number (0 = none; the reason is logged).
int Resolve(const std::string& wanted) {
  const std::vector<save_files::SaveInfo> saves = save_files::ListSaves();
  std::string listing;  // for the messages: "7 'ice tower', 2 'start', ..."
  for (const auto& save : saves) {
    listing += (listing.empty() ? "" : ", ") + std::to_string(save.number) + " '" +
               NameOf(save.number) + "'";
  }
  if (saves.empty()) {
    REXLOG_WARN("Quick load: --load_save={}: there are no saves", wanted);
    return 0;
  }
  const std::string key = Lower(wanted);
  if (key == "last") {
    return saves.front().number;  // most recently played first (save_files.h)
  }
  if (!key.empty() && std::all_of(key.begin(), key.end(), ::isdigit)) {
    const int number = std::stoi(key);
    if (save_files::Exists(number)) {
      return number;
    }
    REXLOG_WARN("Quick load: --load_save={}: no save number {} (saves: {})", wanted, number,
                listing);
    return 0;
  }
  // The whole name first, then a part of it (only if exactly one save has it).
  std::vector<int> partial;
  for (const auto& save : saves) {
    const std::string name = Lower(NameOf(save.number));
    if (name == key) {
      return save.number;
    }
    if (name.find(key) != std::string::npos) {
      partial.push_back(save.number);
    }
  }
  if (partial.size() == 1) {
    return partial.front();
  }
  REXLOG_WARN("Quick load: --load_save={}: {} (saves: {})", wanted,
              partial.empty() ? "no save has that name" : "more than one save has that in its name",
              listing);
  return 0;
}

void GiveUp(const char* why) {
  REXLOG_WARN("Quick load: {}; the game goes on its own way", why);
  g_step = Step::kOff;
}

// While booting: is there a save to start into? Decided once, at the first
// boot movie or at node 44 (the profile and its saves are up by then): a
// save that can't be found leaves the whole boot as it is.
bool Chosen() {
  if (g_step != Step::kBooting) {
    return false;
  }
  if (g_number) {
    return true;
  }
  const std::string wanted = REXCVAR_GET(load_save);
  if (wanted.empty()) {
    g_step = Step::kOff;
    return false;
  }
  if (!REXCVAR_GET(save_library)) {
    GiveUp("--load_save needs the save library (--save_library)");
    return false;
  }
  g_number = Resolve(wanted);
  if (!g_number) {
    g_step = Step::kOff;  // Resolve said why
    return false;
  }
  REXLOG_INFO("Quick load: {:.1f} s: save {} '{}' chosen", Seconds(), g_number, NameOf(g_number));
  return true;
}

}  // namespace

int32_t OnReadingCard(int32_t exit) {
  if (g_step != Step::kToList) {
    return -2;
  }
  if (exit < 0) {
    return -2;  // still scanning
  }
  if (exit != kExitIntoSlotList) {
    GiveUp("the save scan found no saves");  // 155 "no valid save files" -> main menu
    return -2;
  }
  if (!save_library::PickForQuickLoad(g_number)) {
    GiveUp("the save library is off (--save_library=false)");
    return -2;
  }
  g_step = Step::kLoading;
  REXLOG_INFO("Quick load: {:.1f} s: loading save {} '{}'", Seconds(), g_number, NameOf(g_number));
  return kExitLoadSlot1;
}

}  // namespace quick_load

// The boot movies (Dolby, Radical, Sierra, then the attract movie): one
// sequencer object plays them one after the other. sub_82188448(object,
// name) plays one; when one ends, its "finished" handler sub_821882B8
// (r3 = object + 28) moves the counter at object +44 on (0 -> radical,
// 1 -> sierra, 2 -> attract; table of names at 0x82503018) and at 3 reports
// the sequence done (its vtable slot 18), which lets the boot go on to node
// 44's decision. While a quick load is pending, no movie plays: the counter
// is set to 3 and the "finished" handler called at once.
extern "C" REX_FUNC(__imp__sub_82188448);  // movie sequencer: play one movie (r4 = name)
extern "C" REX_FUNC(__imp__sub_821882B8);  // movie sequencer: a movie finished
extern "C" REX_FUNC(sub_82188448) {
  using namespace quick_load;
  if (!Chosen()) {
    __imp__sub_82188448(ctx, base);
    return;
  }
  constexpr uint32_t kCounter = 44, kFinishedInterface = 28, kLastMovie = 3;
  const uint32_t object = ctx.r3.u32;
  const uint32_t name = ctx.r4.u32;
  REXLOG_INFO("Quick load: {:.1f} s: boot movie '{}' skipped", Seconds(),
              name ? reinterpret_cast<const char*>(GuestPtr(base, name)) : "?");
  const uint8_t last[4] = {0, 0, 0, kLastMovie};
  std::memcpy(GuestPtr(base, object + kCounter), last, 4);  // big-endian u32
  ctx.r3.u64 = object + kFinishedInterface;
  __imp__sub_821882B8(ctx, base);
}

// LoadPersistentPostBootUp (node 44): asked once the boot movies are over.
// Its answer comes at once (50 = the title's movie state, or 49 with
// Radical's skip-to-level switch). Ours is the movie state's own way out,
// exit 52: its script switches the game into the menus' game state
// (GotoGameState(EGameState_LEVEL_SELECT)), which the save screens need
// (taken straight to "Load Game" without it, the front end waited
// forever, a black screen), and it leads to 53 LoadingTitleScreen.
extern "C" REX_FUNC(sub_8211B4E0) {
  using namespace quick_load;
  __imp__sub_8211B4E0(ctx, base);
  const int32_t exit = int32_t(ctx.r3.u32);
  if ((exit != kExitPlayMovie && exit != kExitSkipToLevel) || !Chosen()) {
    return;
  }
  g_step = Step::kToTitle;
  REXLOG_INFO("Quick load: {:.1f} s: boot done, into the menus' game state", Seconds());
  ctx.r3.u64 = uint32_t(kExitIntoTitle);
}

// LoadingTitleScreen (node 53): 54 once the title's loading is done. Ours is
// the main menu's "Load Game" (87): no title, no main menu. The title's one
// job, START, is done here.
extern "C" REX_FUNC(sub_8211B5A8) {
  using namespace quick_load;
  __imp__sub_8211B5A8(ctx, base);
  if (g_step == Step::kToTitle && int32_t(ctx.r3.u32) == kExitTitleLoaded) {
    // "Press START" first, as the title would: controller 0 becomes player
    // 1 and its profile is signed in. Without it the save screens see no
    // profile (game +352 = -1: AccessMemoryCard's "no profile" exit 115).
    const uint32_t game = Read32(base, kGameGlobal);
    const uint32_t front_end = game ? Read32(base, game + kGameFrontEnd) : 0;
    if (!front_end) {
      GiveUp("the front end isn't there yet");
      return;
    }
    CallGame(__imp__sub_82266150, ctx, base, front_end, 0, kController);
    CallGame(sub_8227CEA8, ctx, base, game, kController);
    if (int32_t(Read32(base, game + kGameActiveController)) < 0) {
      GiveUp("no profile signed in on the first controller");
      return;
    }
    g_step = Step::kToList;
    REXLOG_INFO("Quick load: {:.1f} s: into Load Game", Seconds());
    ctx.r3.u64 = uint32_t(kExitLoadGame);
  }
}

// LoadComplete (node 282): "Load successful", waiting for a button. Ours
// goes on into the level at once.
extern "C" REX_FUNC(sub_8211CE30) {
  using namespace quick_load;
  __imp__sub_8211CE30(ctx, base);
  if (g_step == Step::kLoading) {
    g_step = Step::kOff;
    REXLOG_INFO("Quick load: {:.1f} s: save loaded, starting the level", Seconds());
    ctx.r3.u64 = uint32_t(kExitToLevel);
  }
}
