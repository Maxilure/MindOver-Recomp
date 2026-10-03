// =============================================================================
// saves/save_library.cpp -- see save_library.h
// =============================================================================
#include "save_library.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include "../input/keyboard_mouse.h"
#include "../overlay_banner.h"
#include "save_files.h"

// The originals of the functions we override, and the ones we call.
extern "C" REX_FUNC(__imp__sub_82258DE8);  // slot -> file name
extern "C" REX_FUNC(__imp__sub_82259858);  // CSaveGameManager::LoadGame(slot)
extern "C" REX_FUNC(__imp__sub_822598D8);  // CSaveGameManager::SaveGame(slot)
extern "C" REX_FUNC(__imp__sub_82259B70);  // CSaveGameManager::AutoSaveGame()
extern "C" REX_FUNC(__imp__sub_82259758);  // CSaveGameManager::SelectCard(device) = the scan
extern "C" REX_FUNC(__imp__sub_822596D8);  // CSaveGameManager::EndAccess()
extern "C" REX_FUNC(__imp__sub_820CC188);  // CGameSlotScreenAction::Enter (fills the panels)
extern "C" REX_FUNC(__imp__sub_820CCA38);  // CGameSlotScreenAction::Update
extern "C" REX_FUNC(__imp__sub_820CCB08);  // CGameSlotScreenAction::Exit
extern "C" REX_FUNC(__imp__sub_821239C8);  // GetMenuIndex(_, page name, menu name)
extern "C" REX_FUNC(__imp__sub_8211BF88);  // front end: the Load / Save list's decision (menu)
extern "C" REX_FUNC(__imp__sub_8211DAC0);  // front end: the Save list's decision (in game)
extern "C" REX_FUNC(__imp__sub_8211B7D8);  // front end: the name entry screen's decision
extern "C" REX_FUNC(__imp__sub_8211B8E8);  // front end: the difficulty screen's decision
extern "C" REX_FUNC(__imp__sub_8211BE58);  // front end: ReadingCard's decision (menu)
extern "C" REX_FUNC(__imp__sub_8211C2D8);  // front end: slot 1's overwrite question (menu)
extern "C" REX_FUNC(__imp__sub_8211DD28);  // front end: slot 1's overwrite question (in game)
extern "C" REX_FUNC(__imp__sub_821241E8);  // FindPage(layer, page name)
extern "C" REX_FUNC(__imp__sub_82371A50);  // page: find a menu (group) by name
extern "C" REX_FUNC(__imp__sub_82371A90);  // page: find a picture by name
extern "C" REX_FUNC(__imp__sub_82371AD0);  // page: find a text element by name
extern "C" REX_FUNC(__imp__sub_82373360);  // text element: set its string (UTF-16, copied)
extern "C" REX_FUNC(__imp__sub_82370EE8);  // element: move by (dx, dy) Scrooby units
extern "C" REX_FUNC(__imp__sub_8235AA10);  // the current date into a save header's date field
extern "C" REX_FUNC(__imp__sub_822E10C8);  // progress object -> completion %
extern "C" REX_FUNC(__imp__sub_82270608);  // the game settings object (difficulty at +240)
extern "C" REX_FUNC(__imp__sub_8237A4C0);  // Scrooby Menu::SetSelection(index)
extern "C" REX_FUNC(__imp__sub_8237A550);  // Scrooby Menu::MovePrevious (up)
extern "C" REX_FUNC(__imp__sub_8237A668);  // Scrooby Menu::MoveNext (down)
extern "C" REX_FUNC(__imp__sub_82261800);  // front end: play a sound (bank, name)
extern "C" REX_FUNC(__imp__sub_824742F0);  // the game's XInputGetState wrapper

REXCVAR_DEFINE_BOOL(save_library, true, "CrashMoM",
                    "The Load / Save Game screen as one scrolling list of any number of saves "
                    "(most recently played first, \"Create New Save\" when saving), New Game "
                    "saves by itself, rename (X / F2) and delete (Y). "
                    "false = the game's own three slots");

namespace save_library {
namespace {

// -----------------------------------------------------------------------------
// The game's objects we read (findings/24)
// -----------------------------------------------------------------------------

// The game object; +80 = CSaveGameManager, +52 = the front end's state.
constexpr uint32_t kGameGlobal = 0x8259B190;
constexpr uint32_t kGameSaveManager = 80;
constexpr uint32_t kGameFrontEnd = 52;
// Front-end flag byte: a screen sets bit 3 when the player picks an entry
// (the slot screen's Update, state 5, at 0x820CCAC4); the compiled front-end
// tree reads it (0x8211C030) and then asks which slot (GetMenuIndex).
constexpr uint32_t kFrontEndFlags = 8593;
constexpr uint8_t kFlagSelected = 0x08;

// CSaveGameManager.
constexpr uint32_t kMgrSaveMode = 4;      // s32 ESaveMode: 2 SAVE, 3 AUTOSAVE, 4 LOAD
constexpr uint32_t kMgrState = 8;         // s32 request state: 0 idle, 8-10 scan, 11 load, 12-14 save
constexpr uint32_t kMgrHeader = 16;       // the game in progress's header: the 104 bytes its saves start with
constexpr uint32_t kMgrHeaderName = 36;   // the game in progress's name (header +20, 32 UTF-16 chars)
constexpr uint32_t kMgrSlotTable = 120;   // -> 3 entries of 104 bytes (the files' first 104 bytes)
constexpr uint32_t kMgrLastSlot = 139;    // s8, the slot the game in progress was loaded / saved
constexpr uint32_t kEntrySize = 104;
constexpr uint32_t kEntryFormat = 8;      // s32: the save format word, 0 = no file, -1 = corrupt
constexpr uint32_t kEntryDate = 12;       // u16 year, u8 month, day, hour, minute, second (UTC)
constexpr uint32_t kEntryLevel = 84;      // s32 the level it was saved in (picks the location picture)
constexpr uint32_t kEntryPlayTime = 88;   // float, seconds
constexpr uint32_t kEntryPercent = 92;    // s32
constexpr uint32_t kEntryDifficulty = 96; // s32 0 Mild, 1 Tricky, 2 Bonkers
// Where SaveGame takes those from (sub_822598D8): the game object's current
// level byte, its progress object (+96: play time at +20), the settings
// object (sub_82270608(): difficulty at +240).
constexpr uint32_t kGameLevel = 18;
constexpr uint32_t kGameProgress = 96;
constexpr uint32_t kProgressPlayTime = 20;
constexpr uint32_t kSettingsDifficulty = 240;
constexpr uint32_t kEntryName = 20;       // 32 UTF-16 chars, empty = empty slot
constexpr uint32_t kNameChars = 32;
// The format word every save of this game carries (= its 20,174-byte block
// size; what sub_82259D88 returns and the scan compares).
constexpr uint32_t kSaveFormat = 0x4ECE;

// The strings the front end passes to GetMenuIndex for this screen.
constexpr uint32_t kSlotPageName = 0x82023394;  // "FE_GameSlot"
constexpr uint32_t kSlotMenuName = 0x820233A0;  // "GameSlotMenu"

// Scrooby (Radical's UI library, pure3d::frontend): a Menu keeps its items
// in a vector at +112 (begin) / +116 (end), the selected index (s8) at +152,
// bit 0x80 of +153 = the cursor wraps (this menu's case). While wrapping,
// MoveNext / MovePrevious (0x8237A668 / 0x8237A550) step on to the next item
// whose +137 has bit 0x80 SET (= selectable) and still return 1 when none is
// (the move sound plays, the cursor stays). First version had the bit
// backwards ("skip"), cleared it on every shown panel, and the cursor never
// moved (2026-10-03, found in the disassembly: lbz 137 / rlwinm 0,0,24 /
// bne = found). Any drawable element (text, picture) is visible while bit
// 0x80 of +110 is set (the slot screen's Enter shows / hides a panel's
// picture that way).
constexpr uint32_t kMenuItemsBegin = 112;
constexpr uint32_t kMenuIndex = 152;
constexpr uint32_t kMenuItemFlags = 137;
constexpr uint8_t kMenuItemSelectable = 0x80;
constexpr uint32_t kElementFlags = 110;
constexpr uint8_t kElementVisible = 0x80;
// Where an element is: +92 / +96 = its x / y as a fraction of the screen
// WIDTH (floats; sub_82370E68 reads them x 640, the menu uses that to move
// its cursor sprite from item to item). sub_82370EE8(element, dx, dy) moves
// an element by whole units of that 640-wide screen (a translation
// multiplied into its transform at +16, then the "changed" bit 0x40 of
// +110).
constexpr uint32_t kElementY = 96;
constexpr float kScroobyUnits = 640.0f;

// The slot screen (CGameSlotScreenAction). Its Enter ends by restarting the
// screen's opening: the top 6 bits of +124 = the screen's state (2 opening,
// 3 the list taking input, 4 closing, 5 picked: the Update's switch at
// 0x820CCA58), +112 = the opening's fade, 0 -> 1, which the opening state
// (sub_820CCCF0) hands to the page's alpha (sub_82370888); and it clears the
// front end's "screen ready" bit (+8592 bit 0x40, set again once the fade
// ends).
constexpr uint32_t kScreenState = 124;
constexpr uint32_t kScreenStateMask = 0xFC000000;
constexpr uint32_t kScreenFade = 112;
constexpr uint32_t kFrontEndReadyFlags = 8592;
constexpr uint8_t kFlagScreenReady = 0x40;

constexpr int kPanels = 3;

// The front end's fight tree (fighttrees/Frontend.bfig, findings/24 section
// 6.4): nodes are numbered in file order, each record starts with its
// parent's number, exits name a target state. A state's compiled decision
// function returns the exit to take. The nodes we use:
constexpr int32_t kExitIntoNameEntry = 112;  // DifficultyScreen's ExitBack -> NameEntryScreen (93)
constexpr int32_t kExitIntoSlotList = 159;   // ReadingCard's ExitHasValidSaveFiles -> GameSlotScreen (160)
constexpr int32_t kExitIntoGameSlotList = 385;  // in game: ReadingCard's ExitOperationDone -> GameSlotScreen (386)
constexpr int32_t kNameEntryDone = 94;       // NameEntryScreen's ExitDone (-> difficulty)
constexpr int32_t kNameEntryBack = 95;       // NameEntryScreen's ExitBack (-> main menu)
constexpr int32_t kDifficultyFirst = 109;    // DifficultyScreen's ExitEasy / ExitNormal / ExitHard:
constexpr int32_t kDifficultyLast = 111;     //   ResetSaveFileHeader, the difficulty, save mode, -> 113
constexpr int32_t kReadingNoSaves = 155;     // ReadingCard (153): ExitNoValidSaveFiles
// Slot 1's ExitAvailableSlot -> SaveGameScreen (289); its script runs
// SetAutoSaveGame(true); SaveGame(0). New Game's own save takes it straight
// from ReadingCard, the list never showing.
constexpr int32_t kExitSaveSlot1 = 185;
// The overwrite question (OverWriteMessage: page FE_TRG_Message_Frontend /
// _InGame, text "ConfirmOverwrite_", a No / Yes menu with No first), reused
// to ask about deleting. Entered through slot 1's save branch (the question
// itself doesn't depend on the slot).
constexpr int32_t kExitIntoQuestion = 170;      // menu: ExitSlotNotEmpty -> OverWriteMessage (171)
constexpr int32_t kQuestionNo = 175;            // its ExitCancel (-> GameSlotScreen 160)
constexpr int32_t kQuestionYes = 176;           // its ExitOverWrite (-> SaveGameScreen 289)
constexpr int32_t kExitIntoGameQuestion = 394;  // in game: ExitSlotNotEmpty -> OverWriteMessage (395)
constexpr int32_t kGameQuestionNo = 399;        // its ExitCancel (-> GameSlotScreen 386)
constexpr int32_t kGameQuestionYes = 400;       // its ExitOverWrite (-> AutoSaveGameScreen 326)
// The New Game name entry's text: one global UTF-16 buffer of 32 characters
// (64 bytes, up to the next global), what the screen shows and edits.
constexpr uint32_t kNameEntryBuffer = 0x825A06F8;

// Big-endian guest memory.
uint32_t Read32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = base + address;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Write32(uint8_t* base, uint32_t address, uint32_t value) {
  uint8_t* p = base + address;
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}
int8_t ReadS8(const uint8_t* base, uint32_t address) { return int8_t(base[address]); }
std::u16string ReadName(const uint8_t* base, uint32_t address) {
  std::u16string name;
  for (uint32_t i = 0; i < kNameChars; ++i) {
    const char16_t c = char16_t(base[address + 2 * i] << 8 | base[address + 2 * i + 1]);
    if (c == 0) break;
    name.push_back(c);
  }
  return name;
}
void WriteName(uint8_t* base, uint32_t address, std::u16string_view name) {
  for (uint32_t i = 0; i < kNameChars; ++i) {
    const char16_t c = i < name.size() ? name[i] : u'\0';
    base[address + 2 * i] = uint8_t(c >> 8);
    base[address + 2 * i + 1] = uint8_t(c & 0xFF);
  }
}

uint32_t Game(const uint8_t* base) { return Read32(base, kGameGlobal); }
uint32_t Manager(const uint8_t* base) {
  const uint32_t game = Game(base);
  return game ? Read32(base, game + kGameSaveManager) : 0;
}
int32_t ManagerState(const uint8_t* base, uint32_t manager) {
  return int32_t(Read32(base, manager + kMgrState));
}
uint8_t FrontEndFlags(const uint8_t* base) {
  const uint32_t game = Game(base);
  const uint32_t front_end = game ? Read32(base, game + kGameFrontEnd) : 0;
  return front_end ? base[front_end + kFrontEndFlags] : 0;
}

// UTF-16 <-> UTF-8 for the ImGui box (names are short, from the game's own
// character sets or our box: the Basic Multilingual Plane is enough).
std::string ToUtf8(std::u16string_view text) {
  std::string out;
  for (char16_t c : text) {
    if (c < 0x80) {
      out.push_back(char(c));
    } else if (c < 0x800) {
      out.push_back(char(0xC0 | (c >> 6)));
      out.push_back(char(0x80 | (c & 0x3F)));
    } else {
      out.push_back(char(0xE0 | (c >> 12)));
      out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
      out.push_back(char(0x80 | (c & 0x3F)));
    }
  }
  return out;
}
// Our box only lets through ASCII (see NameFilter), so this is 1:1.
std::u16string AsciiToUtf16(const char* text) {
  std::u16string out;
  for (; *text; ++text) out.push_back(char16_t(uint8_t(*text)));
  return out;
}

// -----------------------------------------------------------------------------
// Calling game functions from a hook
// -----------------------------------------------------------------------------

using GuestFunction = void (*)(PPCContext&, uint8_t*);

// Calls a game function with r3-r5 set, from inside one of our hooks (on the
// game's main thread, at the hooked function's entry or after its original
// returned: r1 is the caller's stack, so the callee builds its frame below
// it, as a normal call would). The hook's own registers are put back
// afterwards; returns r3.
uint32_t CallGame(GuestFunction function, PPCContext& ctx, uint8_t* base, uint32_t r3,
                  uint32_t r4 = 0, uint32_t r5 = 0) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  function(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------

// What the rename box was opened for, and what came back.
struct DialogRequest {
  int number = 0;           // the save's file number (1, 2, 3, ...)
  std::u16string name;      // its name now
};
struct DialogResult {
  enum Action { kNone, kRename } action = kNone;
  int number = 0;
  std::u16string name;      // kRename: the new name
};

// Shared between the game's main thread and the UI thread.
std::mutex g_mutex;
DialogRequest g_open_for;             // set by the game thread, read by the box
bool g_open_request = false;          // game thread -> box: open now
bool g_dialog_open = false;           // the box is showing (or closing)
DialogResult g_result;                // box -> game thread
// A controller drives the box too (A confirms, B cancels): game thread -> box.
enum PadCommand { kPadNone = 0, kPadConfirm = 1, kPadCancel = 2 };
int g_pad_command = kPadNone;
bool g_force_close = false;           // the screen went away: close the box
// While the box is up (and until the buttons that closed it are let go) the
// game sees a neutral controller: its XInputGetState wrapper is wrapped
// below. (Keys and mouse are paused by the keyboard driver's menu mode.)
std::atomic<bool> g_block_game_input{false};
std::atomic<bool> g_screen_showing{false};
// F2 / Delete on the keyboard (UI thread -> game thread).
enum KeyRequest { kKeyNone = 0, kKeyRename = 1, kKeyDelete = 2 };
std::atomic<int> g_key_request{kKeyNone};

// A list item: a save's file number, or "Create New Save".
constexpr int kCreateNew = -1;

// Game main thread only.
struct GameState {
  // The list on screen, built when the screen's scan starts (SelectCard).
  std::vector<int> items;   // file numbers, most recently played first; kCreateNew first when saving
  int top = 0;              // the item in the top panel
  int new_number = 1;       // the file "Create New Save" writes
  bool save_mode = false;
  // The file of the game in progress (0 = none yet): saves of "the last
  // slot" go there. And the file of the load / save request in flight.
  int game_number = 0;
  int op_number = 0;
  bool in_autosave = false;
  // The slot screen, while it shows.
  uint32_t screen = 0;
  uint32_t slot_menu = 0;      // its Scrooby menu (GameSlotMenu)
  int last_index = -1;         // the menu cursor at the last update (-1 = not known)
  bool refilling = false;      // inside our own call of the screen's Enter
  bool scroll_pending = false; // the window moved: fill the panels at the next update
  bool refresh_pending = false;  // the list changed (a delete): rebuild, fill
  bool cursor_pending = false;   // a new visit (the scan ran): put the cursor in place
  bool preview_shown = false;    // "Create New Save" shows the game in progress (FillPreview)
  // Centring (fewer than three panels showing): the page we moved the
  // panels on, how far they are moved now, and the distance from one panel
  // to the next on that page (Scrooby units, read once per page).
  uint32_t shift_page = 0;
  int shift_applied = 0;
  uint32_t step_page = 0;
  int panel_step = 0;
  uint16_t buttons_down = 0;   // A / B / X / Y held at the last update (for presses)
  bool prompts_pending = false;  // set our prompts at the screen's next update
  // Our prompts: 1 shown (the cursor on a save), 0 cleared ("Create New
  // Save"), -1 not set yet this visit (the game's own "Storage Device Y" is
  // up then: cleared even when the cursor starts on "Create New Save").
  int prompts_shown = -1;
  // A slot picked on the screen (its "selected" flag), until a load / save
  // uses it or the save session ends (EndAccess). NOT cleared when the
  // screen closes and opens again: the overwrite prompt ("Are you sure you
  // wish to overwrite this save file?") does exactly that between the pick
  // and the save (traced 2026-10-02: pick, Exit, Enter, prompt, ...).
  bool picked = false;
  int picked_number = 0;
  int picked_slot = -1;
  // Is the slot list itself in charge, or a question over it? The overwrite
  // prompt keeps the screen running underneath (dimmed), but the front end
  // then asks the PROMPT's exits, not the list's. The list's decision
  // functions (wrapped below) stamp the update they ran in.
  uint32_t update_count = 0;
  uint32_t exits_seen = 0;
  // New Game: the difficulty was picked, its save is still to be created.
  bool new_game = false;
  // Renaming with the game's own name entry screen (X): asked by Update,
  // taken by the list's decision (into the name screen), finished by the
  // name screen's decision (back to the list).
  struct {
    bool requested = false;   // take the exit into the name screen next
    bool active = false;      // the name screen is renaming this save
    int32_t way_back = 0;     // the exit back to the list it came from (159 / 385)
    // What the name buffer held before (New Game's typed name, kept there
    // until the difficulty screen copies it into the new game).
    std::u16string buffer_before;
    int number = 0;
    std::u16string name;
  } game_rename;
  // Deleting with the game's own question (Y / Delete).
  struct {
    bool requested = false;
    bool active = false;
    int number = 0;
    int text_sets = 0;        // our wording, set over the first frames
  } delete_question;
} g;

// The slot list has the player's attention (its exits were asked this
// update or the one before: the front end runs before or after the screen).
bool ListActive() { return g.update_count - g.exits_seen <= 1; }

// -----------------------------------------------------------------------------
// The list and the window
// -----------------------------------------------------------------------------

int ItemCount() { return int(g.items.size()); }
// The item in panel `panel` (kCreateNew, a file number, or 0 = none).
int PanelItem(int panel) {
  const int i = g.top + panel;
  return panel >= 0 && panel < kPanels && i >= 0 && i < ItemCount() ? g.items[size_t(i)] : 0;
}
// The file behind panel `panel` (0 = none: "CrashMOM GameSlot 0" never exists).
int PanelFile(int panel) {
  const int item = PanelItem(panel);
  return item == kCreateNew ? g.new_number : std::max(item, 0);
}
int VisiblePanels() { return std::clamp(ItemCount() - g.top, 0, kPanels); }

// The panel showing save `number`, -1 if it's out of the window.
int PanelOf(int number) {
  for (int panel = 0; panel < kPanels; ++panel) {
    if (PanelItem(panel) == number) {
      return panel;
    }
  }
  return -1;
}

void ClampTop() {
  g.top = std::clamp(g.top, 0, std::max(ItemCount() - kPanels, 0));
}

// Builds the list: every save, most recently played first, after "Create
// New Save" when saving. `keep_top`: stay where the window was (a refresh),
// else start at the top (a new visit).
void BuildList(bool save_mode, bool keep_top) {
  g.save_mode = save_mode;
  g.items.clear();
  if (save_mode) {
    g.items.push_back(kCreateNew);
  }
  for (const save_files::SaveInfo& save : save_files::ListSaves()) {
    g.items.push_back(save.number);
  }
  g.new_number = save_files::FreeNumber();
  if (!keep_top) {
    g.top = 0;
    g.cursor_pending = true;
  }
  ClampTop();
}

// The file a slot's file operation uses (the file-name hook asks).
int FileForRequest(const uint8_t* base, int slot) {
  const uint32_t manager = Manager(base);
  const int32_t state = manager ? ManagerState(base, manager) : 0;
  switch (state) {
    case 8:   // the scan, slot 0, 1, 2
    case 9:   // the scan's start when the device was in use
      return PanelFile(slot);
    case 10:  // after a scan: the game in progress's own slot is read again
      return g.game_number;
    case 11:  // LoadGame
    case 12:  // SaveGame
    case 13:  // SaveGame: the write
    case 14:  // SaveGame: removing the old file before writing
      return g.op_number;
    default:
      return g.game_number ? g.game_number : PanelFile(slot);
  }
}

// Fills the slot table for the window straight from the files: what the
// game's scan copies (each save block's first 104 bytes; the format word
// checked: a mismatch = corrupt, name cleared and -1), no file = an empty
// entry. Instant, unlike the scan's three requests.
void FillTable(uint8_t* base, uint32_t manager) {
  const uint32_t table = Read32(base, manager + kMgrSlotTable);
  if (!table) {
    return;
  }
  for (int panel = 0; panel < kPanels; ++panel) {
    uint8_t* entry = base + table + uint32_t(panel) * kEntrySize;
    std::array<uint8_t, kEntrySize> block{};
    const int file = PanelFile(panel);
    if (file > 0 && save_files::ReadBlockStart(file, block.data(), block.size())) {
      std::memcpy(entry, block.data(), block.size());
      if (Read32(base, table + uint32_t(panel) * kEntrySize + kEntryFormat) != kSaveFormat) {
        WriteName(base, table + uint32_t(panel) * kEntrySize + kEntryName, u"");
        Write32(base, table + uint32_t(panel) * kEntrySize + kEntryFormat, uint32_t(-1));
      }
    } else {
      std::memset(entry, 0, kEntrySize);
    }
  }
}

// "Create New Save" previews the save it would write: the game in
// progress's header (the manager's own copy, +16: the same 104 bytes every
// save starts with), brought up to date the way SaveGame (sub_822598D8)
// does right before it writes: the date (sub_8235AA10), the current level
// (+84, which picks the location picture), the play time (+88, the live
// timer: float at *(game+96)+20), the % (+92, sub_822E10C8 of the same
// object), the difficulty (+96, sub_82270608()+240). The manager's own
// header isn't touched. Only while the screen's Enter fills the panels: the
// entry gets its old bytes back right after, so the game's own checks still
// see an empty slot (IsSlotEmpty: no overwrite question for a new save).
struct Preview {
  int panel = -1;
  std::array<uint8_t, kEntrySize> before{};
};
Preview FillPreview(PPCContext& ctx, uint8_t* base, uint32_t manager) {
  Preview preview;
  const uint32_t table = Read32(base, manager + kMgrSlotTable);
  const int panel = g.save_mode ? PanelOf(kCreateNew) : -1;
  if (!table || panel < 0 || ReadName(base, manager + kMgrHeaderName).empty()) {
    return preview;  // nothing to show (no game in progress: our plain label)
  }
  const uint32_t entry = table + uint32_t(panel) * kEntrySize;
  std::memcpy(preview.before.data(), base + entry, kEntrySize);
  std::memcpy(base + entry, base + manager + kMgrHeader, kEntrySize);
  Write32(base, entry + kEntryFormat, kSaveFormat);
  CallGame(__imp__sub_8235AA10, ctx, base, entry + kEntryDate);
  const uint32_t game = Game(base);
  Write32(base, entry + kEntryLevel, uint32_t(int32_t(int8_t(base[game + kGameLevel]))));
  if (const uint32_t progress = Read32(base, game + kGameProgress)) {
    Write32(base, entry + kEntryPlayTime, Read32(base, progress + kProgressPlayTime));
    Write32(base, entry + kEntryPercent, CallGame(__imp__sub_822E10C8, ctx, base, progress));
  }
  if (const uint32_t settings = CallGame(__imp__sub_82270608, ctx, base, 0)) {
    Write32(base, entry + kEntryDifficulty, Read32(base, settings + kSettingsDifficulty));
  }
  preview.panel = panel;
  return preview;
}
void ClearPreview(uint8_t* base, uint32_t manager, const Preview& preview) {
  const uint32_t table = Read32(base, manager + kMgrSlotTable);
  if (preview.panel >= 0 && table) {
    std::memcpy(base + table + uint32_t(preview.panel) * kEntrySize, preview.before.data(),
                kEntrySize);
  }
  g.preview_shown = preview.panel >= 0;
}

// -----------------------------------------------------------------------------
// Pages and elements
// -----------------------------------------------------------------------------

// Strings handed to the game's functions must sit in guest memory: one
// block from the runtime's system heap, filled as needed, never freed.
uint32_t GuestString(const uint8_t* bytes, size_t size) {
  static std::unordered_map<std::string, uint32_t> cache;
  static uint32_t block = 0, used = 0;
  constexpr uint32_t kBlockSize = 1024;
  const std::string key(reinterpret_cast<const char*>(bytes), size);
  if (const auto it = cache.find(key); it != cache.end()) {
    return it->second;
  }
  if (!block || used + size > kBlockSize) {
    block = rex::system::kernel_memory()->SystemHeapAlloc(kBlockSize);
    used = 0;
    if (!block) {
      return 0;
    }
  }
  const uint32_t address = block + used;
  std::memcpy(rex::system::kernel_memory()->TranslateVirtual(address), bytes, size);
  used += uint32_t((size + 3) & ~size_t(3));
  cache.emplace(key, address);
  return address;
}
uint32_t GuestAscii(const char* text) {
  return GuestString(reinterpret_cast<const uint8_t*>(text), std::strlen(text) + 1);
}
uint32_t GuestUtf16(std::u16string_view text) {
  std::string bytes;
  for (char16_t c : text) {
    bytes.push_back(char(c >> 8));  // the guest's byte order
    bytes.push_back(char(c & 0xFF));
  }
  bytes.append(2, '\0');
  return GuestString(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
}

// A Scrooby page by name (the front end's own lookup, as GetMenuIndex does
// it: layer 4, then layer 0), 0 if it isn't loaded.
uint32_t FindPage(PPCContext& ctx, uint8_t* base, const char* page_name) {
  const uint32_t name = GuestAscii(page_name);
  const uint32_t page = CallGame(__imp__sub_821241E8, ctx, base, 4, name);
  return page ? page : CallGame(__imp__sub_821241E8, ctx, base, 0, name);
}

// Sets the string of a text element (no-op if it's missing).
void SetText(PPCContext& ctx, uint8_t* base, uint32_t text_element, std::u16string_view text) {
  if (text_element) {
    CallGame(__imp__sub_82373360, ctx, base, text_element, GuestUtf16(text));
  }
}

// Sets the string of `element` on `page_name` (no-op if either isn't loaded).
void SetPromptText(PPCContext& ctx, uint8_t* base, const char* page_name, const char* element,
                   std::u16string_view text) {
  const uint32_t page = FindPage(ctx, base, page_name);
  const uint32_t text_element =
      page ? CallGame(__imp__sub_82371AD0, ctx, base, page, GuestAscii(element)) : 0;
  if (!text_element) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_WARN("Saves: element {}/{} not found", page_name, element);
    }
    return;
  }
  SetText(ctx, base, text_element, text);
}

void SetVisible(uint8_t* base, uint32_t element, bool visible) {
  if (element) {
    base[element + kElementFlags] = visible ? (base[element + kElementFlags] | kElementVisible)
                                            : (base[element + kElementFlags] & ~kElementVisible);
  }
}

// The elements that make up panel `panel` (0-2) on the slot page: its
// background, location picture and five texts (0 for any that's missing).
std::array<uint32_t, 7> PanelElements(PPCContext& ctx, uint8_t* base, uint32_t page, int panel) {
  std::array<uint32_t, 7> elements{};
  size_t n = 0;
  for (const char* what : {"BG", "Art"}) {
    elements[n++] = CallGame(__imp__sub_82371A90, ctx, base, page,
                             GuestAscii(fmt::format("Slot{}_{}", panel + 1, what).c_str()));
  }
  for (const char* what : {"Name", "Date", "Time", "Percent", "Difficulty"}) {
    elements[n++] = CallGame(__imp__sub_82371AD0, ctx, base, page,
                             GuestAscii(fmt::format("Slot{}_{}", panel + 1, what).c_str()));
  }
  return elements;
}

float ReadFloat(const uint8_t* base, uint32_t address) {
  const uint32_t bits = Read32(base, address);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// The distance from one panel to the next (panel 1's background to panel
// 2's), read the first time a page is seen, before anything was moved.
int PanelStep(PPCContext& ctx, uint8_t* base, uint32_t page) {
  if (g.step_page == page && g.panel_step != 0) {
    return g.panel_step;
  }
  const uint32_t bg1 = PanelElements(ctx, base, page, 0)[0];
  const uint32_t bg2 = PanelElements(ctx, base, page, 1)[0];
  if (!bg1 || !bg2) {
    return 0;
  }
  const float y1 = ReadFloat(base, bg1 + kElementY) * kScroobyUnits;
  const float y2 = ReadFloat(base, bg2 + kElementY) * kScroobyUnits;
  g.step_page = page;
  g.panel_step = int(std::lround(y2 - y1));
  REXLOG_INFO("Saves: slot panels at y {:.1f}, {:.1f}: {} units apart", y1, y2, g.panel_step);
  return g.panel_step;
}

// Moves the three panels by `dy` (Scrooby units).
void MovePanels(PPCContext& ctx, uint8_t* base, uint32_t page, int dy) {
  for (int panel = 0; panel < kPanels; ++panel) {
    for (const uint32_t element : PanelElements(ctx, base, page, panel)) {
      if (element) {
        CallGame(__imp__sub_82370EE8, ctx, base, element, 0, uint32_t(dy));
      }
    }
  }
}

// Fewer than three panels showing: they move to the middle of the screen
// (one = where panel 2 is, two = half a panel down) instead of sitting at
// the top with empty space below. The game's layout is put back when the
// screen closes (RestorePanels): the page outlives the screen.
void CenterPanels(PPCContext& ctx, uint8_t* base, uint32_t page) {
  if (g.shift_page != page) {
    g.shift_page = page;
    g.shift_applied = 0;
  }
  const int step = PanelStep(ctx, base, page);
  if (step == 0) {
    return;
  }
  const int visible = VisiblePanels();
  const int want = visible > 0 && visible < kPanels ? step * (kPanels - visible) / 2 : 0;
  if (want != g.shift_applied) {
    MovePanels(ctx, base, page, want - g.shift_applied);
    g.shift_applied = want;
  }
}
void RestorePanels(PPCContext& ctx, uint8_t* base) {
  if (g.shift_applied == 0) {
    return;
  }
  const uint32_t page = FindPage(ctx, base, "FE_GameSlot");
  if (page && page == g.shift_page) {
    MovePanels(ctx, base, page, -g.shift_applied);
  }
  g.shift_applied = 0;
}

// After the panels were filled (the game's Enter): hide the panels with
// nothing to show (their menu items skipped by the cursor), "Create New
// Save" in its panel's name, the cursor on a visible panel, the panels
// centred.
void DecoratePanels(PPCContext& ctx, uint8_t* base) {
  const uint32_t page = FindPage(ctx, base, "FE_GameSlot");
  if (!page) {
    return;
  }
  g.slot_menu = CallGame(__imp__sub_82371A50, ctx, base, page, kSlotMenuName);
  for (int panel = 0; panel < kPanels; ++panel) {
    const int item = PanelItem(panel);
    const bool shown = item != 0;
    auto text = [&](const char* what) {
      return CallGame(__imp__sub_82371AD0, ctx, base, page,
                      GuestAscii(fmt::format("Slot{}_{}", panel + 1, what).c_str()));
    };
    auto picture = [&](const char* what) {
      return CallGame(__imp__sub_82371A90, ctx, base, page,
                      GuestAscii(fmt::format("Slot{}_{}", panel + 1, what).c_str()));
    };
    for (const char* what : {"Name", "Date", "Time", "Percent", "Difficulty"}) {
      SetVisible(base, text(what), shown);
    }
    SetVisible(base, picture("BG"), shown);
    if (!shown) {
      SetVisible(base, picture("Art"), false);  // shown panels: as the game's Enter set it
    }
    // "Create New Save": the title over the preview of the game in progress
    // (FillPreview), or, with no game in progress, where the game writes
    // "Empty" for an empty slot: the panel's middle line, the Date element
    // (Enter 0x820CC910: element 16 + slot gets GameSlot_Empty, Name an
    // empty string), so it looks like the game's own empty slot.
    if (item == kCreateNew) {
      SetText(ctx, base, text(g.preview_shown ? "Name" : "Date"), u"Create New Save");
    }
    // The menu item: selectable only while the panel shows.
    if (g.slot_menu) {
      const uint32_t items = Read32(base, g.slot_menu + kMenuItemsBegin);
      const uint32_t menu_item = items ? Read32(base, items + uint32_t(panel) * 4) : 0;
      if (menu_item) {
        base[menu_item + kMenuItemFlags] =
            shown ? (base[menu_item + kMenuItemFlags] | kMenuItemSelectable)
                  : (base[menu_item + kMenuItemFlags] & ~kMenuItemSelectable);
      }
    }
  }
  // The cursor at a new visit: saving -> on the game in progress's save
  // (overwriting it is the usual pick, "Create New Save" is one up; none
  // yet -> "Create New Save"), loading -> the most recently played save. The
  // menu object stays loaded between visits and keeps its index, so it would
  // open wherever the last visit left it (seen 2026-10-03: Load Game picked
  // panel 3, the in-game Save Game list then opened on panel 3 = another
  // save).
  if (g.slot_menu && g.cursor_pending) {
    const int game_panel = g.game_number > 0 ? PanelOf(g.game_number) : -1;
    const int want = g.save_mode && game_panel >= 0 ? game_panel : 0;
    CallGame(__imp__sub_8237A4C0, ctx, base, g.slot_menu, uint32_t(want));
  }
  // The cursor on a visible panel (the list shrank: a delete).
  if (g.slot_menu) {
    const int index = ReadS8(base, g.slot_menu + kMenuIndex);
    const int visible = VisiblePanels();
    if (visible > 0 && index >= visible) {
      CallGame(__imp__sub_8237A4C0, ctx, base, g.slot_menu, uint32_t(visible - 1));
    }
  }
  CenterPanels(ctx, base, page);
}

// Fills the panels again from the slot table: the screen's own Enter (it
// releases the elements it held before taking them again; its swipe sound
// is kept quiet: the wrapped sound player below), then our decorations.
// The Enter also restarts the screen's opening (kScreenState above): the
// page faded in again at every slide (a visible flash, 2026-10-03). The
// state, the fade and the front end's "ready" bit are put back as they were.
void Refill(PPCContext& ctx, uint8_t* base, uint32_t screen) {
  const uint32_t state = Read32(base, screen + kScreenState) & kScreenStateMask;
  const uint32_t fade = Read32(base, screen + kScreenFade);
  const uint32_t game = Game(base);
  const uint32_t front_end = game ? Read32(base, game + kGameFrontEnd) : 0;
  const uint8_t ready = front_end ? base[front_end + kFrontEndReadyFlags] & kFlagScreenReady : 0;
  const uint32_t manager = Manager(base);
  const Preview preview = manager ? FillPreview(ctx, base, manager) : Preview{};
  g.refilling = true;
  CallGame(__imp__sub_820CC188, ctx, base, screen);
  g.refilling = false;
  if (manager) {
    ClearPreview(base, manager, preview);
  }
  Write32(base, screen + kScreenState,
          (Read32(base, screen + kScreenState) & ~kScreenStateMask) | state);
  Write32(base, screen + kScreenFade, fade);
  if (front_end) {
    base[front_end + kFrontEndReadyFlags] =
        uint8_t((base[front_end + kFrontEndReadyFlags] & ~kFlagScreenReady) | ready);
  }
  DecoratePanels(ctx, base);
}

// -----------------------------------------------------------------------------
// The screen's button prompts
// -----------------------------------------------------------------------------
//
// The prompts are TEXT in the game's font (Titans_Small), whose private
// characters U+00A5-00BE draw the 360 buttons (findings/23 section 1.2):
// U+00B3 = X, U+00B4 = Y. They live on the Scrooby page FE_Buttons (package
// b4c85fe7.p3d in default.rcf): four prompt slots, each a ...Button (the
// glyph) and a ...Text element. On this screen: LowerLeft "Back B",
// LowerRight "Select A", UpperRight "Storage Device Y" (the Xbox's storage
// device chooser, meaningless on a PC; its exit is blocked below) and
// UpperLeft empty. Ours: UpperRight "Rename X", UpperLeft "Delete Y", while
// the cursor is on a save (not on "Create New Save").
// WHEN: from the screen's first update on, not in its Enter. Every front-end
// state also starts a prompts action (vtable 0x82024A08, Enter 0x820DFBD8)
// that sets all eight FE_Buttons texts (empty first, then its own: Back,
// Select) and it starts after the screen: texts set in Enter were wiped.
void SetPrompts(PPCContext& ctx, uint8_t* base, bool on) {
  SetPromptText(ctx, base, "FE_Buttons", "UpperLeftText", on ? u"Delete" : u"");
  SetPromptText(ctx, base, "FE_Buttons", "UpperLeftButton", on ? u"´" : u"");
  SetPromptText(ctx, base, "FE_Buttons", "UpperRightText", on ? u"Rename" : u"");
  SetPromptText(ctx, base, "FE_Buttons", "UpperRightButton", on ? u"³" : u"");
  g.prompts_shown = on ? 1 : 0;
}

// A, B, X and Y pressed since the last update (player 1's controller, or the
// keys bound to them: the prompts show the controller's buttons, as the
// game's own do). Read from the input system directly, so the game's muted
// view of the controller (g_block_game_input) doesn't hide them.
uint16_t ButtonsPressed() {
  auto* input =
      static_cast<rex::input::InputSystem*>(rex::Runtime::instance()->input_system());
  if (!input) {
    return 0;
  }
  rex::input::X_INPUT_STATE state{};
  input->GetState(0, &state);
  const uint16_t down =
      uint16_t(state.gamepad.buttons) &
      (rex::input::X_INPUT_GAMEPAD_A | rex::input::X_INPUT_GAMEPAD_B |
       rex::input::X_INPUT_GAMEPAD_X | rex::input::X_INPUT_GAMEPAD_Y);
  const uint16_t pressed = down & ~g.buttons_down;
  g.buttons_down = down;
  return pressed;
}

// -----------------------------------------------------------------------------
// Rename and delete
// -----------------------------------------------------------------------------

// The save in panel `panel`: its file number and name (0 / empty: "Create
// New Save", a hidden panel or a corrupt save).
int SaveInPanel(const uint8_t* base, uint32_t manager, int panel, std::u16string* name) {
  const int item = PanelItem(panel);
  const uint32_t table = Read32(base, manager + kMgrSlotTable);
  if (item <= 0 || !table) {
    return 0;
  }
  const uint32_t entry = table + uint32_t(panel) * kEntrySize;
  if (int32_t(Read32(base, entry + kEntryFormat)) <= 0) {
    return 0;
  }
  *name = ReadName(base, entry + kEntryName);
  return name->empty() ? 0 : item;
}

// F2 on a save (or X where the game's name screen can't be used): open the
// rename box for it.
void OpenDialog(const uint8_t* base, uint32_t manager, int panel) {
  std::u16string name;
  const int number = SaveInPanel(base, manager, panel, &name);
  if (!number) {
    return;
  }
  std::lock_guard lock(g_mutex);
  g_open_for = DialogRequest{number, name};
  g_open_request = true;
  REXLOG_INFO("Saves: rename box for save {} ({})", number, ToUtf8(name));
}

// X on a save: rename it with the game's own name entry screen. The list's
// decision takes the jump at its next run (sub_8211BF88 / sub_8211DAC0
// below). If the screen's page isn't loaded, our own box instead: jumping
// there would make its Enter look up elements of a page that isn't there.
// The page (GameStart_NameEntry) is in the front end's package
// (package/cdd70a8c.p3d, the "Fe_Frontend" inventory section): loaded in the
// menus, and in game ONLY in level L0 (Crash's house): the front end's
// section setup for gameplay (sub_82260FF0, state 5) adds Fe_Frontend only
// when the current level is "L0" (traced 2026-10-03: renaming in the house
// works, outside on Wumpa Island the page is gone).
void RequestGameRename(PPCContext& ctx, uint8_t* base, uint32_t manager, int panel) {
  std::u16string name;
  const int number = SaveInPanel(base, manager, panel, &name);
  if (!number) {
    return;
  }
  if (!FindPage(ctx, base, "GameStart_NameEntry")) {
    REXLOG_INFO("Saves: the name entry page isn't loaded here (in game it is only in Crash's house): our rename box instead");
    OpenDialog(base, manager, panel);
    return;
  }
  g.game_rename.requested = true;
  g.game_rename.number = number;
  g.game_rename.name = name;
  REXLOG_INFO("Saves: renaming save {} ({}) with the game's name screen", number, ToUtf8(name));
}

// In a list's decision (r3 = its choice): X was pressed on a save, and the
// list has nothing else to do this frame -> into the game's name entry
// screen, with the save's name in its text. The exit taken is the
// difficulty screen's Back, the only way into the name screen that doesn't
// start a new game; the engine follows any exit's target, child of the
// current state or not (tested 2026-10-02). `way_back` = the exit into the
// list it came from.
void TakeGameRenameJump(PPCContext& ctx, uint8_t* base, int32_t way_back) {
  if (!g.game_rename.requested || int32_t(ctx.r3.u32) != -1) {
    return;
  }
  g.game_rename.requested = false;
  g.game_rename.active = true;
  g.game_rename.way_back = way_back;
  g.game_rename.buffer_before = ReadName(base, kNameEntryBuffer);
  WriteName(base, kNameEntryBuffer, g.game_rename.name);
  ctx.r3.u64 = uint32_t(kExitIntoNameEntry);
}

// A new name for save `number`: the file, the slot table's entry (if that
// save is in the window), and the game in progress's own copy of its name
// (it writes that name with every save, or the old one would come back).
bool RenameSave(uint8_t* base, uint32_t manager, int number, const std::u16string& name) {
  std::string error;
  if (!save_files::Rename(number, name, &error)) {
    REXLOG_WARN("Saves: renaming save {} failed: {}", number, error);
    overlay_banner::Show("Rename failed", error);
    return false;
  }
  REXLOG_INFO("Saves: save {} renamed to \"{}\"", number, ToUtf8(name));
  const int panel = PanelOf(number);
  const uint32_t table = Read32(base, manager + kMgrSlotTable);
  if (table && panel >= 0) {
    WriteName(base, table + uint32_t(panel) * kEntrySize + kEntryName, name);
  }
  if (number == g.game_number && ReadS8(base, manager + kMgrLastSlot) >= 0) {
    WriteName(base, manager + kMgrHeaderName, name);
  }
  return true;
}

// Y / Delete on a save: ask with the game's own question (the list's
// decision takes the jump at its next run).
void RequestDeleteQuestion(const uint8_t* base, uint32_t manager, int panel) {
  std::u16string name;
  const int number = SaveInPanel(base, manager, panel, &name);
  if (!number) {
    return;
  }
  g.delete_question.requested = true;
  g.delete_question.number = number;
  REXLOG_INFO("Saves: asking whether to delete save {} ({})", number, ToUtf8(name));
}

// In a list's decision: into the overwrite question, asking about deleting.
void TakeDeleteJump(PPCContext& ctx, int32_t into_question) {
  if (!g.delete_question.requested || int32_t(ctx.r3.u32) != -1) {
    return;
  }
  g.delete_question.requested = false;
  g.delete_question.active = true;
  g.delete_question.text_sets = 0;
  ctx.r3.u64 = uint32_t(into_question);
}

// Moves save `number` out of the list ("Deleted saves", save_files.h).
void DeleteSave(int number) {
  std::filesystem::path moved_to;
  std::string error;
  if (!save_files::MoveToDeleted(number, &moved_to, &error)) {
    REXLOG_WARN("Saves: deleting save {} failed: {}", number, error);
    overlay_banner::Show("Delete failed", error);
    return;
  }
  REXLOG_INFO("Saves: save {} moved to {}{}", number, moved_to.string(),
              number == g.game_number ? " (the game in progress's save)" : "");
  if (number == g.game_number) {
    g.game_number = 0;  // its next save goes through "Create New Save" or a pick
  }
}

// In the question's decision (r3 = its choice): while it asks about
// deleting, our wording in its message, and both answers lead back to the
// list: Yes after moving the save away (the list is rebuilt then), never the
// overwrite.
void HandleDeleteQuestion(PPCContext& ctx, uint8_t* base, int32_t no, int32_t yes,
                          bool in_game) {
  if (!g.delete_question.active) {
    return;
  }
  // The question's own message is set when it starts (its track: text
  // "ConfirmOverwrite_"); ours goes over it in its first frames.
  if (g.delete_question.text_sets < 10) {
    ++g.delete_question.text_sets;
    const char* page = "FE_TRG_Message_Frontend";
    if (in_game) {
      page = FindPage(ctx, base, "FE_TRG_Message_InGame") ? "FE_TRG_Message_InGame"
                                                           : "FE_TRG_Message";
    }
    SetPromptText(ctx, base, page, "Message",
                  u"Are you sure you wish to delete this save file? "
                  u"It will be moved to the Deleted saves folder.");
  }
  const int32_t exit = int32_t(ctx.r3.u32);
  if (exit != no && exit != yes) {
    return;
  }
  g.delete_question.active = false;
  if (exit == yes) {
    DeleteSave(g.delete_question.number);
    g.refresh_pending = true;
  }
  ctx.r3.u64 = uint32_t(no);
}

// The box's answer, applied on the game thread while the manager is idle.
void ApplyResult(PPCContext& ctx, uint8_t* base, uint32_t manager, uint32_t screen) {
  DialogResult result;
  {
    std::lock_guard lock(g_mutex);
    if (g_result.action == DialogResult::kNone) {
      return;
    }
    result = g_result;
    g_result = DialogResult{};
  }
  if (result.action == DialogResult::kRename &&
      RenameSave(base, manager, result.number, result.name)) {
    Refill(ctx, base, screen);
  }
}

// -----------------------------------------------------------------------------
// The rename box (UI thread)
// -----------------------------------------------------------------------------

// What a name may hold: what the game's font is sure to have (letters,
// digits, space, a little punctuation), at most kMaxNameLength characters.
int NameFilter(ImGuiInputTextCallbackData* data) {
  const ImWchar c = data->EventChar;
  const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == ' ' || c == '-' || c == '_' || c == '.' || c == '!' || c == '?' ||
                  c == '\'' || c == '&';
  return ok ? 0 : 1;
}

class SaveDialog : public rex::ui::ImGuiDialog {
 public:
  explicit SaveDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

  bool WantsContinuousRepaint() const override {
    std::lock_guard lock(g_mutex);
    return g_dialog_open || g_open_request;
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    std::lock_guard lock(g_mutex);
    if (g_open_request) {
      g_open_request = false;
      if (!g_dialog_open) {
        Open();
      }
    }
    if (!g_dialog_open) {
      return;
    }
    // Closing: keep the game's keyboard paused until Enter / Esc are let go,
    // or the Enter that confirmed the box would reach the game as "A" and
    // pick the slot (or the save's overwrite prompt).
    if (closing_) {
      if (!ImGui::IsKeyDown(ImGuiKey_Enter) && !ImGui::IsKeyDown(ImGuiKey_KeypadEnter) &&
          !ImGui::IsKeyDown(ImGuiKey_Escape)) {
        Close();
      }
      return;
    }
    if (g_force_close) {
      g_force_close = false;
      Finish(DialogResult{});
      return;
    }
    // The controller's A / B (sent by the game thread, which reads it).
    const int command = std::exchange(g_pad_command, int(kPadNone));
    if (command == kPadConfirm) {
      if (buffer_[0]) {
        Finish(DialogResult{DialogResult::kRename, request_.number, AsciiToUtf16(buffer_)});
      }
      return;
    }
    if (command == kPadCancel) {
      Finish(DialogResult{});
      return;
    }

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.95f);
    if (ImGui::Begin("Rename save###crashmom_save_box", nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoSavedSettings)) {
      DrawRename();
    }
    ImGui::End();
  }

 private:
  // g_mutex held.
  void Open() {
    request_ = g_open_for;
    const std::string name = ToUtf8(request_.name);
    std::memset(buffer_, 0, sizeof(buffer_));
    std::strncpy(buffer_, name.c_str(), save_files::kMaxNameLength);
    closing_ = false;
    g_pad_command = kPadNone;
    g_force_close = false;
    focus_ = true;
    g_dialog_open = true;
    // Keys and mouse off for the game while the box is up (like the
    // Controls menu); the controller: g_block_game_input.
    if (kbm::KeyboardMouseDriver* keyboard = kbm::KeyboardMouseDriver::Get()) {
      keyboard->SetMenuOpen(true);
    }
  }
  void Finish(DialogResult result) {
    g_result = std::move(result);
    closing_ = true;
  }
  void Close() {
    g_dialog_open = false;
    closing_ = false;
    if (kbm::KeyboardMouseDriver* keyboard = kbm::KeyboardMouseDriver::Get()) {
      keyboard->SetMenuOpen(false);
    }
  }

  void DrawRename() {
    ImGui::Text("Rename \"%s\"", ToUtf8(request_.name).c_str());
    ImGui::TextDisabled("Type up to %zu letters, digits, spaces. Controller: A rename, B cancel",
                        save_files::kMaxNameLength);
    if (focus_) {
      ImGui::SetKeyboardFocusHere();
      focus_ = false;
    }
    const bool enter = ImGui::InputText(
        "##name", buffer_, save_files::kMaxNameLength + 1,
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCharFilter,
        NameFilter);
    const bool has_name = buffer_[0] != 0;
    if (!has_name) ImGui::BeginDisabled();
    const bool rename = ImGui::Button("Rename (Enter)") || (enter && has_name);
    if (!has_name) ImGui::EndDisabled();
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel (Esc)") || ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (rename) {
      Finish(DialogResult{DialogResult::kRename, request_.number, AsciiToUtf16(buffer_)});
    } else if (cancel) {
      Finish(DialogResult{});
    }
  }

  DialogRequest request_;
  char buffer_[save_files::kMaxNameLength + 1] = {};
  bool closing_ = false;
  bool focus_ = false;
};

std::unique_ptr<SaveDialog> g_dialog;

bool DialogBusy() {
  std::lock_guard lock(g_mutex);
  return g_dialog_open || g_open_request || g_result.action != DialogResult::kNone;
}

// Our hooks on the slot menu act only while the list has the player.
bool OwnsSlotMenu(uint32_t menu) {
  return REXCVAR_GET(save_library) && menu && menu == g.slot_menu && g.screen != 0;
}

}  // namespace

void Create(rex::ui::ImGuiDrawer* drawer) {
  if (REXCVAR_GET(save_library)) {
    g_dialog = std::make_unique<SaveDialog>(drawer);
  }
}

void RequestDialog(bool delete_save) {
  if (g_screen_showing.load()) {
    g_key_request = delete_save ? kKeyDelete : kKeyRename;
  }
}

}  // namespace save_library

// -----------------------------------------------------------------------------
// The wrapped game functions (strong definitions of the generated weak ones)
// -----------------------------------------------------------------------------

// Slot -> file name: r3 = the storage handler, r4 = slot 0-2, r5 = the
// 32-byte buffer. The original formats "%s GameSlot %d" with r4 + 1.
extern "C" REX_FUNC(sub_82258DE8) {
  using namespace save_library;
  const int32_t slot = int32_t(ctx.r4.u32);
  if (REXCVAR_GET(save_library) && slot >= 0 && slot < kPanels) {
    ctx.r4.u64 = uint32_t(FileForRequest(base, slot) - 1);  // file 0: "GameSlot 0", never there
  }
  __imp__sub_82258DE8(ctx, base);
}

// SelectCard (r3 = manager, r4 = device): the scan of the three slots, which
// the front end runs when the Load / Save screen is about to open. The list
// is built here, from the top.
extern "C" REX_FUNC(sub_82259758) {
  using namespace save_library;
  if (REXCVAR_GET(save_library)) {
    const int32_t mode = int32_t(Read32(base, ctx.r3.u32 + kMgrSaveMode));
    BuildList(/*save_mode=*/mode == 2 || mode == 3, /*keep_top=*/false);
    const int saves = ItemCount() - (g.save_mode ? 1 : 0);
    REXLOG_INFO("Saves: {} list, {} save{}", g.save_mode ? "save" : "load", saves,
                saves == 1 ? "" : "s");
  }
  __imp__sub_82259758(ctx, base);
}

// LoadGame: r3 = manager, r4 = slot. Always a slot picked on the screen.
extern "C" REX_FUNC(sub_82259858) {
  using namespace save_library;
  if (REXCVAR_GET(save_library)) {
    const int slot = int(int32_t(ctx.r4.u32));
    g.op_number = g.picked && g.picked_slot == slot ? g.picked_number : PanelFile(slot);
    g.game_number = g.op_number;
    g.picked = false;
    save_files::MarkPlayed(g.op_number);
    REXLOG_INFO("Saves: loading save {}", g.op_number);
  }
  __imp__sub_82259858(ctx, base);
}

// SaveGame: r3 = manager, r4 = slot. A slot picked on the screen goes to the
// pick's file ("Create New Save": a new file); everything else (saves to
// "the last slot", the method calling itself to write after removing the
// old file) to the game in progress's file.
extern "C" REX_FUNC(sub_822598D8) {
  using namespace save_library;
  if (REXCVAR_GET(save_library)) {
    const int slot = int(int32_t(ctx.r4.u32));
    if (!g.in_autosave && g.picked && g.picked_slot == slot) {
      g.op_number = g.picked_number;
      g.game_number = g.op_number;
    } else {
      g.op_number = g.game_number ? g.game_number : PanelFile(slot);
    }
    g.picked = false;
    REXLOG_INFO("Saves: saving to save {}{}", g.op_number, g.in_autosave ? " (AutoSaveGame)" : "");
  }
  __imp__sub_822598D8(ctx, base);
}

// AutoSaveGame: saves to the last slot (+139) through SaveGame.
extern "C" REX_FUNC(sub_82259B70) {
  using namespace save_library;
  g.in_autosave = true;
  __imp__sub_82259B70(ctx, base);
  g.in_autosave = false;
}

// The slot screen's Enter (r3 = the CGameSlotScreenAction), unless it's our
// own refill. The front end also closes and reopens the screen around its
// overwrite prompt, so this isn't always a new visit: the pick and the
// window stay (the pick ends with the save session, EndAccess below).
extern "C" REX_FUNC(sub_820CC188) {
  using namespace save_library;
  const bool visit = REXCVAR_GET(save_library) && !g.refilling;
  if (visit) {
    g.screen = ctx.r3.u32;
    g.last_index = -1;
    g.prompts_pending = true;
    g.prompts_shown = -1;
    g_screen_showing = true;
    g_key_request = kKeyNone;
  }
  const uint32_t manager = visit ? Manager(base) : 0;
  const Preview preview = manager ? FillPreview(ctx, base, manager) : Preview{};
  __imp__sub_820CC188(ctx, base);
  if (manager) {
    ClearPreview(base, manager, preview);
  }
  if (visit) {
    DecoratePanels(ctx, base);
  }
}

// The slot screen's Update (r3 = this, f1/f2 = time): the window's refills,
// the prompts, X / Y, then the screen's own update; afterwards, did it
// report a pick?
extern "C" REX_FUNC(sub_820CCA38) {
  using namespace save_library;
  if (!REXCVAR_GET(save_library)) {
    __imp__sub_820CCA38(ctx, base);
    return;
  }
  const uint32_t screen = ctx.r3.u32;
  const uint32_t manager = Manager(base);
  if (manager && g.screen == screen) {
    ++g.update_count;
    const bool list_active = ListActive();
    const bool idle = ManagerState(base, manager) == 0;
    if (g.prompts_pending && list_active) {
      g.prompts_pending = false;
      DecoratePanels(ctx, base);  // again: actions that started after Enter
      g.cursor_pending = false;
    }
    if (idle) {
      ApplyResult(ctx, base, manager, screen);
    }
    // Back from the delete question with a save gone: the list again.
    if (g.refresh_pending && idle && list_active) {
      g.refresh_pending = false;
      BuildList(g.save_mode, /*keep_top=*/true);
      FillTable(base, manager);
      Refill(ctx, base, screen);
    }
    // The window slid (the menu's move, wrapped below): the new panels.
    if (g.scroll_pending && idle) {
      g.scroll_pending = false;
      FillTable(base, manager);
      Refill(ctx, base, screen);
    }
    const int index = int(int32_t(CallGame(__imp__sub_821239C8, ctx, base, screen,
                                           kSlotPageName, kSlotMenuName)));
    // Rename / Delete prompts on a save, not on "Create New Save".
    if (!g.prompts_pending && list_active) {
      const bool on_save = PanelItem(index) > 0;
      if (int(on_save) != g.prompts_shown) {
        SetPrompts(ctx, base, on_save);
      }
    }
    const bool busy = !idle || DialogBusy() || !list_active || g.scroll_pending ||
                      g.refresh_pending;
    const uint16_t pressed = ButtonsPressed();
    const bool dialog = DialogBusy();
    if (dialog) {
      const int command = (pressed & rex::input::X_INPUT_GAMEPAD_A)   ? kPadConfirm
                          : (pressed & rex::input::X_INPUT_GAMEPAD_B) ? kPadCancel
                                                                      : kPadNone;
      if (command != kPadNone) {
        std::lock_guard lock(g_mutex);
        g_pad_command = command;
      }
    }
    g_block_game_input = dialog || (g_block_game_input && g.buttons_down != 0);
    const int key = g_key_request.exchange(kKeyNone);
    const bool want_game_rename = (pressed & rex::input::X_INPUT_GAMEPAD_X) != 0;
    const bool want_rename = key == kKeyRename;
    const bool want_delete = (pressed & rex::input::X_INPUT_GAMEPAD_Y) || key == kKeyDelete;
    if (want_game_rename && !busy && index >= 0) {
      RequestGameRename(ctx, base, manager, index);
    } else if (want_delete && !busy && index >= 0) {
      RequestDeleteQuestion(base, manager, index);
    } else if (want_rename && !busy && index >= 0) {
      OpenDialog(base, manager, index);
    }
    g.last_index = index;
  }
  const bool picked_before = FrontEndFlags(base) & kFlagSelected;
  __imp__sub_820CCA38(ctx, base);
  if (!picked_before && (FrontEndFlags(base) & kFlagSelected) && g.last_index >= 0) {
    const int item = PanelItem(g.last_index);
    if (item != 0) {
      g.picked = true;
      g.picked_number = item == kCreateNew ? g.new_number : item;
      g.picked_slot = g.last_index;
      REXLOG_INFO("Saves: picked {}", item == kCreateNew
                                           ? fmt::format("Create New Save (save {})", g.new_number)
                                           : fmt::format("save {}", item));
    }
  }
}

// The slot screen's Exit (also around the overwrite prompt, see Enter).
extern "C" REX_FUNC(sub_820CCB08) {
  using namespace save_library;
  __imp__sub_820CCB08(ctx, base);
  if (REXCVAR_GET(save_library) && !g.refilling) {
    g.screen = 0;
    g.slot_menu = 0;
    g_screen_showing = false;
    g.prompts_pending = false;
    SetPrompts(ctx, base, false);
    RestorePanels(ctx, base);
    g_block_game_input = false;
    std::lock_guard lock(g_mutex);
    if (g_dialog_open || g_open_request) {
      g_open_request = false;
      g_force_close = true;
    }
  }
}

// The Scrooby menu's MoveNext (down) / MovePrevious (up): r3 = the menu,
// returns 1 if the selection moved. On the slot list: at the bottom visible
// panel, down slides the window (the cursor stays, the next save appears in
// it); at the top panel, up slides back; at the list's ends nothing moves
// (no wrap). Inside, the menu's own move (hidden panels' items aren't
// selectable, so it steps over them).
extern "C" REX_FUNC(sub_8237A668) {
  using namespace save_library;
  const uint32_t menu = ctx.r3.u32;
  if (OwnsSlotMenu(menu)) {
    if (!ListActive() || DialogBusy() || g.scroll_pending || g.refresh_pending) {
      ctx.r3.u64 = 0;
      return;
    }
    const int index = ReadS8(base, menu + kMenuIndex);
    if (index >= VisiblePanels() - 1) {
      const bool more = g.top + kPanels < ItemCount();
      if (more) {
        ++g.top;
        g.scroll_pending = true;
      }
      ctx.r3.u64 = more ? 1 : 0;
      return;
    }
  }
  __imp__sub_8237A668(ctx, base);
}
extern "C" REX_FUNC(sub_8237A550) {
  using namespace save_library;
  const uint32_t menu = ctx.r3.u32;
  if (OwnsSlotMenu(menu)) {
    if (!ListActive() || DialogBusy() || g.scroll_pending || g.refresh_pending) {
      ctx.r3.u64 = 0;
      return;
    }
    if (ReadS8(base, menu + kMenuIndex) <= 0) {
      const bool more = g.top > 0;
      if (more) {
        --g.top;
        g.scroll_pending = true;
      }
      ctx.r3.u64 = more ? 1 : 0;
      return;
    }
  }
  __imp__sub_8237A550(ctx, base);
}

// The front end's sound player: quiet while we refill the panels (the
// screen's Enter plays the menu's "back" swipe each time it runs).
extern "C" REX_FUNC(sub_82261800) {
  using namespace save_library;
  if (g.refilling) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82261800(ctx, base);
}

// The front end's exit choices for the Load / Save list (the compiled fight
// tree: a function per state returns the exit to take, -1 = stay). Their
// last check is "button 14 (Y) pressed -> Storage Device": exits 235 (save
// mode) / 236 (load mode) on the menu's list, 444 on the in-game list.
// Blocked: Y means Delete here. Then our jumps (rename, delete).
extern "C" REX_FUNC(sub_8211BF88) {
  using namespace save_library;
  g.exits_seen = g.update_count;
  __imp__sub_8211BF88(ctx, base);
  if (!REXCVAR_GET(save_library)) {
    return;
  }
  const int32_t exit = int32_t(ctx.r3.u32);
  if (exit == 235 || exit == 236) {
    ctx.r3.u64 = uint32_t(-1);
  }
  TakeGameRenameJump(ctx, base, kExitIntoSlotList);
  TakeDeleteJump(ctx, kExitIntoQuestion);
}
extern "C" REX_FUNC(sub_8211DAC0) {
  using namespace save_library;
  g.exits_seen = g.update_count;
  __imp__sub_8211DAC0(ctx, base);
  if (!REXCVAR_GET(save_library)) {
    return;
  }
  if (int32_t(ctx.r3.u32) == 444) {
    ctx.r3.u64 = uint32_t(-1);
  }
  TakeGameRenameJump(ctx, base, kExitIntoGameSlotList);
  TakeDeleteJump(ctx, kExitIntoGameQuestion);
}

// EndAccess (r3 = manager): the end of a save session (the front end calls
// it when the Load / Save screens are left, and a few times before a session
// starts; never between a pick and its load / save: traced 2026-10-02). A
// pick that wasn't used is forgotten (so a later save of "the last slot"
// can't land on a file that was only looked at).
extern "C" REX_FUNC(sub_822596D8) {
  using namespace save_library;
  if (REXCVAR_GET(save_library)) {
    g.picked = false;
  }
  __imp__sub_822596D8(ctx, base);
}

// The difficulty screen's decision (node 107): Mild / Tricky / Bonkers (109-
// 111) start a new game (ResetSaveFileHeader etc.); Back (112) leaves.
extern "C" REX_FUNC(sub_8211B8E8) {
  using namespace save_library;
  __imp__sub_8211B8E8(ctx, base);
  const int32_t exit = int32_t(ctx.r3.u32);
  if (exit >= kDifficultyFirst && exit <= kDifficultyLast) {
    g.new_game = true;
  } else if (exit == kExitIntoNameEntry) {
    g.new_game = false;
  }
}

// ReadingCard's decision (node 153, the menu's): once the scan is done it
// opens the list (159) or says there are no saves (155). For a new game it
// creates the save instead: slot 1's "available slot" exit (185, whose
// script saves slot 0) with slot 0 = "Create New Save" = a new file. The
// list never shows.
extern "C" REX_FUNC(sub_8211BE58) {
  using namespace save_library;
  __imp__sub_8211BE58(ctx, base);
  const int32_t exit = int32_t(ctx.r3.u32);
  if (!REXCVAR_GET(save_library) || !g.new_game ||
      (exit != kExitIntoSlotList && exit != kReadingNoSaves)) {
    return;
  }
  g.new_game = false;
  if (!g.save_mode || g.items.empty() || g.items[0] != kCreateNew) {
    return;  // not the list we built for saving: leave the game's way alone
  }
  g.top = 0;
  g.picked = true;
  g.picked_number = g.new_number;
  g.picked_slot = 0;
  REXLOG_INFO("Saves: New Game: creating save {}", g.new_number);
  ctx.r3.u64 = uint32_t(kExitSaveSlot1);
}

// The game's XInputGetState wrapper (r3 = player, r4 = XINPUT_STATE: u32
// packet number + the 12-byte gamepad), only called by its input manager's
// poll (sub_8235D188). While the rename box has the controller, the game
// gets a connected but untouched pad.
extern "C" REX_FUNC(sub_824742F0) {
  using namespace save_library;
  const uint32_t state = ctx.r4.u32;
  __imp__sub_824742F0(ctx, base);
  if (g_block_game_input.load(std::memory_order_relaxed) && ctx.r3.u32 == 0 && state) {
    std::memset(base + state + 4, 0, 12);
  }
}

// The name entry screen's decision (node 93): 94 = Done, 95 = Back, -1 =
// stay. While it renames a save, both lead back to the list it came from
// (exit 159 / 385; the slot table still holds the window: nothing frees it
// on the way), Done after renaming the save.
extern "C" REX_FUNC(sub_8211B7D8) {
  using namespace save_library;
  __imp__sub_8211B7D8(ctx, base);
  if (!g.game_rename.active) {
    return;
  }
  const int32_t exit = int32_t(ctx.r3.u32);
  if (exit != kNameEntryDone && exit != kNameEntryBack) {
    return;
  }
  g.game_rename.active = false;
  if (exit == kNameEntryDone) {
    const std::u16string name = ReadName(base, kNameEntryBuffer);
    const uint32_t manager = Manager(base);
    if (manager && !name.empty() && name != g.game_rename.name) {
      RenameSave(base, manager, g.game_rename.number, name);
    }
  }
  WriteName(base, kNameEntryBuffer, g.game_rename.buffer_before);
  ctx.r3.u64 = uint32_t(g.game_rename.way_back);
}

// The overwrite question's decision for slot 1 (the one the delete jump
// enters): menu 171 (175 No, 176 Yes, 173 card gone), in game 395 (399, 400,
// 397).
extern "C" REX_FUNC(sub_8211C2D8) {
  using namespace save_library;
  __imp__sub_8211C2D8(ctx, base);
  HandleDeleteQuestion(ctx, base, kQuestionNo, kQuestionYes, false);
}
extern "C" REX_FUNC(sub_8211DD28) {
  using namespace save_library;
  __imp__sub_8211DD28(ctx, base);
  HandleDeleteQuestion(ctx, base, kGameQuestionNo, kGameQuestionYes, true);
}

// The game's name entry screen: room for one more character? Its "type the
// selected key" (sub_820D6618) refuses a character once the name has 9
// (0x820D663C cmpwi r11,9 on wcslen of the buffer; 0x820D6640 bge -> the
// error sound). This midasm hook runs before that bge (crash_mom_manifest
// .toml): true jumps past it, so names take up to 16 characters like the
// rename box, also at New Game (its copy of the name, ResetSaveFileHeader
// sub_82259408, keeps 32).
bool CrashMomNameEntryRoom(PPCRegister& r11) {
  return REXCVAR_GET(save_library) &&
         int32_t(r11.u32) < int32_t(save_files::kMaxNameLength);
}
