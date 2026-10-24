// =============================================================================
// players/outfits.cpp -- see outfits.h (docs/findings/46)
// =============================================================================
//
// HOW (all game addresses verified with xexdis / Ghidra, 2026-10-16):
//
// 1. WHO WEARS WHAT. sub_8229F8B8(p) / sub_8229F8A0(p, skin) read / write the
//    game's 2-entry table 0x8259B1C0; every reader goes through them (the
//    player spawn event sub_8229C550 asks for its player's template, the
//    save loader sub_8229F9A8 sets players 1-2 from the save, the skin
//    screen asks for the current one). Overridden here for 4 players:
//    players 1-2 keep the game's table (so saves stay as they were),
//    players 3-4 get ours; an outfit picked at the chest (kept per save,
//    see Picked) wins over both. Players 3-4's spawn events are
//    built AS player 2 (more_players.cpp): BuildingSpawnFor() says for whom.
//
// 2. CARBON = number 11 (kCarbon). Toward the game it is skin 0 (Crash's
//    model); only the screen and our texture choice see 11. The lookups that
//    take a skin number answer 11 like 0: name -> "Crash" (sub_8229F7F8,
//    the preview's template), base character (sub_8229F8D0), unlocked
//    (sub_8229F858: always).
//
// 3. THE TEXTURE SET. CCharacterRenderBehaviour blends between its two
//    texture sets by +0xE4 (the script's SetCharacterSkin, sub_82186200):
//    1 = set 1 (Crash_Player1.bmp, orange), 0 = set 2 (Crash_Player2.bmp,
//    Carbon); its Update (slot 4, sub_821858C0, r3 = it, r4 = the actor)
//    turns that into the blend every frame (sub_822AFC30; a 2 s fade only
//    when the script asks). The game sets +0xE4 once from the player number;
//    we set it before every Update (and the one-time set-up, slot 7
//    sub_82185E60) of a player's character and of the chest's preview.
//    (First tried in slot 7 alone: it runs ONCE, so a pick showed only after
//    the character was made again.)
//
// 4. THE SCREEN (CSkinScreenAction, vtable 0x82024D9C). this +0x84 = the
//    page's player, 0 or 1, which also picks the page's LAYOUT (player 2's is
//    mirrored) and list; +0x6c = the list's length; +0x70 = the shown entry.
//    We keep +0x84 = the layout (player & 1) and the real player here
//    (g_page_player); the list = ours for every page (sub_820E15A8 entry,
//    sub_820E1428 position of the current outfit, +0x6c after sub_820E14B8).
//    Opening: midasm OutfitsChestOpen (0x820E0088, r3 = the opener, the
//    front end's +0x2230). Back: midasm OutfitsChestNextPage (0x820E18FC, r4
//    = the game's 1 - page) -> the next player on foot; the front end's
//    "player choosing" (sub_82263630, +0x2230) gets the real one.
//
// 5. "WEAR SKIN" (the front end's SelectSkin, sub_82123880) did nothing above
//    player 2 and only let player 2 change character (Crash <-> Coco). Ours
//    (same steps, any player): Crash-type <-> Coco = respawn the players
//    (sub_8227C5A0, the game's way: every player is rebuilt from their
//    outfit); another Crash model = swap it on the spot (the game's own
//    chain on the actor); then the game's store (sub_822665B0: loads the new
//    outfit's package, lets go of the old one), which lands in our setter.
// =============================================================================

#include "outfits.h"
#include "../input/prompt_icons.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <fstream>
#include <map>
#include <sstream>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../data/data_patcher.h"
#include "../data/pure3d.h"
#include "../saves/save_files.h"
#include "../pddi/intercept.h"
#include "../native/recorder.h"
#include "../ui/scrooby.h"
#include "more_players.h"
#include "outfit_picker.h"
#include "outfit_previews.h"

REXCVAR_DEFINE_BOOL(carbon_colours, true, "CrashMoM",
                    "Crash in colours at the chest (red, yellow, green, blue, purple, pink: players 3-4 start "
                    "green and purple; player 2 stays the original white Carbon Crash); false = only the "
                    "white Carbon Crash");
REXCVAR_DEFINE_BOOL(outfit_store_fix, true, "CrashMoM",
                    "The outfit store gives back the package it holds and keeps picks out of the game's "
                    "own table (2026-10-24); false = the earlier behaviour (for comparing)");
// RETIRED (2026-10-24): the picks were global settings until they moved into
// each save file (2026-10-22, see "THE PICKS ARE PER SAVE" below). For a while
// older saves still started from these; now nothing reads them: a save
// without picks of its own wears what the game decides. They stay defined
// only so settings files that still hold them load without a warning; the
// launcher's Settings tab leaves them out (settings_list.cpp).
REXCVAR_DEFINE_INT32(outfit_player1, -1, "CrashMoM",
                     "No longer used (outfits are kept per save since 2026-10-22)")
    .range(-1, 17);
REXCVAR_DEFINE_INT32(outfit_player2, -1, "CrashMoM", "No longer used (see outfit_player1)")
    .range(-1, 17);
REXCVAR_DEFINE_INT32(outfit_player3, -1, "CrashMoM", "No longer used (see outfit_player1)")
    .range(-1, 17);
REXCVAR_DEFINE_INT32(outfit_player4, -1, "CrashMoM", "No longer used (see outfit_player1)")
    .range(-1, 17);

extern "C" REX_FUNC(__imp__sub_8229F8B8);  // player r3's skin
extern "C" REX_FUNC(__imp__sub_8229F8A0);  // player r3's skin = r4
extern "C" REX_FUNC(__imp__sub_8229F7F8);  // skin r3 -> its name ("Crash_Var01")
extern "C" REX_FUNC(__imp__sub_8229F8D0);  // skin r3 -> its base character template
extern "C" REX_FUNC(__imp__sub_8229F858);  // is skin r3 unlocked
extern "C" REX_FUNC(__imp__sub_82185E60);  // CCharacterRenderBehaviour slot 7: set up (r3 it, r4 actor)
extern "C" REX_FUNC(__imp__sub_821858C0);  // CCharacterRenderBehaviour slot 4: Update (r3 it, r4 actor)
extern "C" REX_FUNC(__imp__sub_820E0858);  // skin screen Update (vtable slot 5, per frame)
extern "C" REX_FUNC(__imp__sub_820E0C98);  // skin screen: the preview's random idle animation
extern "C" REX_FUNC(__imp__sub_820E1228);  // skin screen: show entry r4 (name, text, preview)
extern "C" REX_FUNC(__imp__sub_820E15A8);  // skin screen: entry r5 of page r4's list
extern "C" REX_FUNC(__imp__sub_820E1428);  // skin screen: where the current outfit is
extern "C" REX_FUNC(__imp__sub_820E14B8);  // skin screen: the page's groups + list length
extern "C" REX_FUNC(__imp__sub_82263630);  // front end: the player choosing = r4
extern "C" REX_FUNC(__imp__sub_8227C5A0);  // game: respawn the players (r3 = game)
extern "C" REX_FUNC(__imp__sub_820B0E98);  // the outfit swap chain (SelectSkin's own)
extern "C" REX_FUNC(__imp__sub_822E3FA8);
extern "C" REX_FUNC(__imp__sub_822E4680);
extern "C" REX_FUNC(__imp__sub_820B09C8);
extern "C" REX_FUNC(__imp__sub_820B0DF8);
extern "C" REX_FUNC(__imp__sub_822665B0);  // front end: player r4 wears r5 (+ packages)
extern "C" REX_FUNC(sub_822665B0);  // ours: section 6 (gives back what it holds)
extern "C" REX_FUNC(__imp__sub_822E7880);  // the inventory's per-change update
extern "C" REX_FUNC(__imp__sub_8236ACB8);
extern "C" REX_FUNC(__imp__sub_820D8908);  // a section's texture by name key (r3 section, r4 64-bit key)
extern "C" REX_FUNC(__imp__sub_822B6FB8);  // CCharacterObject: draws its model (r3 the object)
extern "C" REX_FUNC(__imp__sub_822B6DB0);  // CCharacterObject slot 11: draws its +28 drawable  // name key from a string (r3 key, r4 string)
extern "C" REX_FUNC(__imp__sub_8229F810);  // skin r3 -> its package's name
extern "C" REX_FUNC(__imp__sub_8236AC90);  // a name handle (r3 the handle, r4 the name)
extern "C" REX_FUNC(__imp__sub_823C9A70);  // lets a name handle go (r3)

namespace outfits {
namespace {

constexpr int kMaxPlayers = 4;
constexpr uint32_t kGameGlobal = 0x8259B190;      // the game object
constexpr uint32_t kFrontEndOf = 52;              // game +52: the front end
constexpr uint32_t kInventoryOf = 100;            // game +100: the inventory manager
constexpr uint32_t kChooser = 0x2230;             // front end: the player at the skin screen
constexpr uint32_t kCoOpState = 0x8259B11C;       // per player: 2 = in game, on foot
constexpr uint32_t kSelection = 0x825A0BA4;       // the shown entry: +0 skin, +12 template hash
constexpr uint32_t kPlayer1List = 0x825A0AC8;     // the game's lists (20-byte entries:
constexpr uint32_t kPlayer2List = 0x825A0C20;     //  skin, name text, description text, 0, 0)
constexpr uint32_t kCrashName = 0x82025A8C;       // "Crash" (the name table's skin 0)
constexpr uint32_t kRenderSkinSet = 0xE4;         // CCharacterRenderBehaviour: 1 orange, 0 Carbon
constexpr uint32_t kScreenPage = 0x84, kScreenCount = 0x6C;
// The preview's animation set, per page (0x825A0A78 + 8 x page): Crash's,
// Coco's (page 2's, except for Carbon Crash, who gets Crash's).
constexpr uint32_t kPreviewCrashMoves = 0x825A0A78, kPreviewCocoMoves = 0x825A0A80;
constexpr uint32_t kEntryBytes = 20;
// The page's "Goto Player N" prompts (one per layout), text objects; a text
// keeps its UTF-16 string's address at +156.
constexpr uint32_t kScreenGotoPrompts[2] = {0x38, 0x3C};
constexpr uint32_t kTextString = 156;
// The skin screen's own call sites that ask sub_8229F8B8 for "the page's
// player" (return addresses): opening (0x820E06B8) and Back (0x820E192C).
constexpr uint32_t kAskOpening = 0x820E06BC, kAskPageTurn = 0x820E1930;
constexpr uint32_t kChooserPageTurn = 0x820E1908;  // its sub_82263630 call after Back

// --- CRASH IN COLOURS + EVERY OUTFIT'S HAIR (findings/46 s.3) ---
// Crash's own package (the path builder's category 10, "b06ecd92") holds
// his two texture sets: Crash_Player1.bmp (orange) and Crash_Player2.bmp
// (Carbon), 512 x 512 DXT1, no mip levels, on his BODY material
// (crash_body_shader); his HAIR (the spikes on his head and down his back) is
// a material of its own, crash_hair_color_shader, with ONE texture,
// crash_fur_color.tga (256 x 256, 8-bit with a 256-entry 32-bit palette:
// dark red-brown spikes with orange roots), which the game never changes:
// its Carbon Crash has orange roots on white fur.
//
// A data patch adds, made from the player's own disc data:
//   - per colour a BODY texture "Crash_Player2_<colour>.bmp", recoloured
//     from the ORANGE texture (blue jeans, red shoes, as in the short): the
//     orange fur, the cream chest / muzzle / hands and the dark brown
//     (eyebrows, tattoos) each get the colour's own shade. (The first
//     version tinted the white Carbon texture, where fur and chest are
//     nearly the same cream: the fur's colour ran into the chest.)
//   - per colour, and for the white Carbon, a HAIR texture
//     "crash_fur_<colour>.tga" (only the palette changes).
// Each such player's Crash then names its body texture (the render
// behaviour's +0x9C key, read when it sets up, slot 7); the hair is put on
// the shared hair material for the length of that Crash's draw (s.3 below).
constexpr const char* kCrashPackage = "b06ecd92";
constexpr const char* kOrangeTexture = "Crash_Player1.bmp";
constexpr const char* kCarbonTexture = "Crash_Player2.bmp";
constexpr const char* kFurTexture = "crash_fur_color.tga";
constexpr uint32_t kRenderCarbonName = 0x9C;  // CCharacterRenderBehaviour: set 2's texture name key
constexpr uint32_t kRenderObject = 0x98;      // CCharacterRenderBehaviour: its CCharacterObject
constexpr uint32_t kSimpleTexture = 92;       // xnSimpleShader: its texture (native/guest.h)
// A colour = hue (degrees), saturation, value of each part, picked from
// stills of the short (its video's colours are washed out: the original
// Crash there measures s 0.68 v 0.81 where his texture is s 0.88 v 0.95,
// so the stills' readings were scaled up by that much) and checked on the
// character in the game.
struct Hsv {
  float h, s, v;
};
struct CrashColour {
  int outfit;
  const char* id;           // texture names
  const char16_t* name;     // the chest's title
  const char16_t* text;     // the chest's description
  Hsv body, chest, brow, tattoo, hair;
};
// In the chest's order. Tattoos: the yellow Crash's arm stripes are blue-violet
// in the short; the others' are a deeper shade of their fur.
constexpr CrashColour kColours[] = {
    {kCrashRed, "red", u"Red Crash", u"Exactly the same in every way but red",
     {9, .66f, .84f}, {12, .34f, 1.0f}, {5, .70f, .40f}, {352, .66f, .42f}, {7, .70f, .56f}},
    {kCrashYellow, "yellow", u"Yellow Crash", u"Exactly the same in every way but yellow",
     {48, .56f, .90f}, {52, .28f, 1.0f}, {28, .66f, .40f}, {248, .42f, .66f}, {30, .62f, .54f}},
    {kCrashGreen, "green", u"Green Crash", u"Exactly the same in every way but green",
     {104, .45f, .76f}, {92, .24f, .94f}, {105, .55f, .34f}, {125, .60f, .33f}, {110, .58f, .50f}},
    {kCrashBlue, "blue", u"Blue Crash", u"Exactly the same in every way but blue",
     {228, .40f, .92f}, {224, .17f, 1.0f}, {225, .40f, .33f}, {236, .55f, .42f}, {224, .40f, .50f}},
    {kCrashPurple, "purple", u"Purple Crash", u"Exactly the same in every way but purple",
     {284, .40f, .80f}, {290, .18f, .98f}, {285, .45f, .33f}, {270, .55f, .36f}, {282, .45f, .50f}},
    {kCrashPink, "pink", u"Pink Crash", u"Exactly the same in every way but pink",
     {350, .36f, 1.0f}, {350, .15f, 1.0f}, {350, .50f, .46f}, {335, .60f, .50f}, {352, .52f, .70f}},
};
constexpr int kColourCount = int(sizeof(kColours) / sizeof(kColours[0]));
// The white Carbon's hair: roots in its fur's cream, spikes a dusty brown.
constexpr CrashColour kCarbonHair = {kCarbon, "carbon", u"", u"",
                                     {48, .36f, .91f}, {}, {}, {}, {36, .33f, .54f}};
const CrashColour* ColourOf(int outfit) {
  for (const CrashColour& c : kColours) {
    if (c.outfit == outfit) return &c;
  }
  return outfit == kCarbon ? &kCarbonHair : nullptr;
}
std::atomic<bool> g_colours_added{false};  // the patch ran this session
uint32_t g_render_of[4] = {};  // each player's CCharacterRenderBehaviour (seen in its Update)
// The hair (s.3b): per CCharacterObject, its inventory section (the set-up's
// r5) and its outfit (the render behaviour's last Update).
std::unordered_map<uint32_t, uint32_t> g_section_of;
std::unordered_map<uint32_t, int> g_outfit_of;

std::string BodyName(const CrashColour& c) { return std::string("Crash_Player2_") + c.id + ".bmp"; }
std::string FurName(const CrashColour& c) { return std::string("crash_fur_") + c.id + ".tga"; }

uint32_t g_game_skins[2] = {0, 0};  // players 3-4's game numbers (0 = Crash -> Carbon)
uint32_t g_list = 0;                // our list in guest memory (kEntryBytes each)
uint32_t g_list_count = 0;
int g_page_player = 0;              // the skin screen's real player (its +0x84 = layout)
int g_applying = -1;                // SelectSkin: the player whose pick is being stored
int g_applying_outfit = 0;
// During Wear Skin: the player and outfit being put on, already counted as
// theirs (the model swap sets the character up again BEFORE the game's
// store records the pick; a first version swapped with the old outfit).
int g_wearing_player = -1, g_wearing_outfit = 0;
// When the skin screen last ran its Update (steady clock, ms): while recent,
// a Crash that isn't a player's is the screen's preview.
std::atomic<int64_t> g_screen_ms{-100000};
// (Section 5b: the chest's previews across a respawn.)
uint32_t g_skin_screen = 0;           // the skin screen seen in its last Update
bool g_preview_held = false;          // a respawn is under way: no preview build
uint32_t g_held_sets = 0;             // PlayerSetsMade() when it began
uint32_t g_held_old_character = 0;    // the respawning player's character then
int g_held_player = -1;
int64_t g_held_since = 0;
int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
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

using GuestFunction = void (*)(PPCContext&, uint8_t*);
uint32_t CallGame(GuestFunction f, PPCContext& ctx, uint8_t* base, uint32_t r3, uint32_t r4 = 0,
                  uint32_t r5 = 0) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = r3; ctx.r4.u64 = r4; ctx.r5.u64 = r5;
  f(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

// The game's number for an outfit (Carbon = Crash's model).
uint32_t GameSkin(int outfit) { return IsCarbon(outfit) ? 0u : uint32_t(outfit); }
bool IsCoco(uint32_t skin) { return skin == 9 || skin == 10; }
// What the ORIGINAL can put in its table (0x8259B1C0) for players 1-2: player
// 1 is always a Crash (0 orange .. 8), player 2 the Carbon Crash (0) or the
// story's Coco (9, 10 Old School). (2026-10-24) Until today a pick at the
// chest also went into that table, so a player 1 Coco pick ended up in the
// game's own part of every save made after it (saves seen with player 1 = 9).
// Picks live only in each save's trailer now; a table value the original
// can't have counts as the game's default (0). The save files keep their bytes.
bool OriginalSkinFor(int p, uint32_t skin) { return p == 0 ? skin <= 8 : (skin == 0 || IsCoco(skin)); }
// Is a game skin unlocked in the loaded save? The game's own test
// (sub_8229F858, read here without a call): the unlock list 0x825A72A8, 11
// skin numbers ending at -1, filled from the save when it loads (save offset
// 13080) and emptied by New Game. Crash's model (0, Carbon's too) is always
// his.
constexpr uint32_t kUnlockList = 0x825A72A8, kUnlockListEnd = 0x825A72D4;
bool SkinUnlocked(uint32_t skin) {
  if (skin == 0) return true;
  for (uint32_t a = kUnlockList; a < kUnlockListEnd; a += 4) {
    const uint32_t v = Read32(a);
    if (v == 0xFFFFFFFFu) return false;
    if (v == skin) return true;
  }
  return false;
}
// THE PICKS ARE PER SAVE (2026-10-22), IN THE SAVE FILE ITSELF. The save
// file is the game's 0x4EED bytes followed by a small trailer of ours (the
// layout is in saves/save_files.h): "MORE", a version, and one signed byte
// per player (-1 = no pick, as the game decides). The game never sees it:
// the PC save drive keeps it apart and writes it back after the game's bytes
// (saves/pc_save_drive.cpp calls SaveTrailer when a file is written).
//   * a save is LOADED  -> its trailer becomes the current picks. A save
//     without one (every save from before this existed) has no picks: the
//     game decides (until 2026-10-24 it took the old global settings
//     outfit_player1..4, which made one old pick follow every older save);
//   * New Game          -> no picks (the story's own outfits);
//   * a pick at the chest -> the current picks; they reach the file with the
//     next save, like the rest of the progress;
//   * a save is WRITTEN (Create New Save, overwrite) -> the trailer holds the
//     current picks.
// Backups, deleted saves and copies of a save carry it along (it's the file).
// (Before any save is loaded -- title, menus, a New Game -- nobody has a pick.)
std::array<int, kMaxPlayers> g_picks = {-1, -1, -1, -1};
std::mutex g_picks_mutex;

std::array<int, kMaxPlayers>& CurrentPicks() { return g_picks; }

// The outfit a player wears by choice, -1 = none. It only counts while the
// loaded save has it unlocked: a pick of Coco made in a late save, then a
// New Game or an early save, played Coco before the story gives her
// (2026-10-17). There the player wears what the save says.
int Picked(int p) {
  if (p < 0 || p >= kMaxPlayers) return -1;
  std::lock_guard guard(g_picks_mutex);
  const int picked = CurrentPicks()[p];
  if (picked < 0 || picked > kLastOutfit) return -1;
  return SkinUnlocked(GameSkin(picked)) ? picked : -1;
}

// The game's own number for player p (no pick): its table / ours.
uint32_t StoredSkin(int p, PPCContext& ctx, uint8_t* base) {
  if (p >= 2) return g_game_skins[p - 2];
  return CallGame(__imp__sub_8229F8B8, ctx, base, uint32_t(p));
}

int PlayerOfActor(uint32_t actor) {
  for (int p = 0; p < kMaxPlayers; ++p) {
    if (actor && more_players::CharacterOfPlayer(p) == actor) return p;
  }
  return -1;
}

// Our list: Crash, Carbon Crash, player 1's other 8, then Coco and Old
// School (player 2's), copied from the game's lists (filled at boot).
void BuildList(bool again = false) {
  if (g_list && !again) return;
  if (!g_list) g_list = rex::system::kernel_memory()->SystemHeapAlloc(24 * kEntryBytes);
  if (!g_list) return;
  g_list_count = 0;
  auto copy = [](uint32_t from, uint32_t skin_override) {
    const uint32_t to = g_list + g_list_count * kEntryBytes;
    std::memcpy(Guest(to), Guest(from), kEntryBytes);
    if (skin_override != ~0u) Write32(to, skin_override);
    ++g_list_count;
  };
  copy(kPlayer1List, ~0u);                   // Crash
  copy(kPlayer2List, uint32_t(kCarbon));     // Carbon Crash (player 2's name + text)
  if (REXCVAR_GET(carbon_colours) && g_colours_added) {  // (names set when shown)
    for (const CrashColour& c : kColours) copy(kPlayer2List, uint32_t(c.outfit));
  }
  for (uint32_t i = 1; i < 9; ++i) copy(kPlayer1List + i * kEntryBytes, ~0u);
  copy(kPlayer2List + kEntryBytes, ~0u);     // Coco
  copy(kPlayer2List + 2 * kEntryBytes, ~0u); // Old School
  static uint32_t logged = 0;
  if (logged != g_list_count) REXLOG_INFO("Outfits: {} outfits on every page of the chest", g_list_count);
  logged = g_list_count;
}

// --- the recolouring ---
// One colour of Crash's ORANGE textures -> the same part of a coloured Crash.
// Works on colour alone (the same function for every pixel / DXT1 end
// colour, so blends between parts stay smooth):
//   - only Crash's own fur range takes part: hue 6-56 degrees (the shoes'
//     and mouth's reds, the jeans, the green eyes stay), saturation above
//     ~0.2 (the teeth, eyes' whites, the shoes' white and grey stay), and not
//     the paw pads (hue above 40 AND darker than 0.65);
//   - DARK (value 0.62 -> 0.40: eyebrows, tattoos, the hair's spikes) ->
//     the colour's brow / tattoo / hair shade;
//   - CREAM (saturation 0.80 -> 0.58: the chest, muzzle, fingers; the orange
//     fur is 0.88-0.92) -> the colour's chest;
//   - the rest = the fur -> the colour's body;
// each part keeps the texture's own shading (value / the part's usual value).
// Measured on the disc's texture: fur h 22-27 s 0.88-0.92 v 0.94; chest h 36-37
// s 0.54-0.58 v 1.0; eyebrows h 14-15 s 0.9 v 0.33-0.46; paw pads h 44-45 s
// 0.29-0.38 v 0.21-0.59; shoes h 355-359.
float Ramp(float x, float a, float b) { return std::clamp((x - a) / (b - a), 0.0f, 1.0f); }
void HsvToRgb(const Hsv& c, float out[3]) {
  const float h = std::fmod(c.h, 360.0f) / 60.0f, f = h - std::floor(h);
  const float p = c.v * (1 - c.s), q = c.v * (1 - c.s * f), t = c.v * (1 - c.s * (1 - f));
  const float table[6][3] = {{c.v, t, p}, {q, c.v, p}, {p, c.v, t}, {p, q, c.v}, {t, p, c.v}, {c.v, p, q}};
  const int i = int(h) % 6;
  for (int k = 0; k < 3; ++k) out[k] = table[i][k];
}
enum class Part { kBody, kTattoo, kHair };  // which dark shade: eyebrows / tattoos / hair spikes
void Recolour(float rgb[3], const CrashColour& c, Part part) {
  const float r = rgb[0], g = rgb[1], b = rgb[2];
  const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
  const float v = mx, s = mx > 0.0f ? (mx - mn) / mx : 0.0f;
  float h = 0.0f;
  if (mx > mn) {
    if (mx == r) h = 60.0f * std::fmod((g - b) / (mx - mn) + 6.0f, 6.0f);
    else if (mx == g) h = 60.0f * ((b - r) / (mx - mn) + 2.0f);
    else h = 60.0f * ((r - g) / (mx - mn) + 4.0f);
  }
  const float band = Ramp(h, 6, 12) * (1 - Ramp(h, 56, 62)) *
                     (1 - Ramp(h, 40, 44) * (1 - Ramp(v, .62f, .72f))) * Ramp(s, .12f, .22f);
  if (band <= 0.0f) return;
  const float dark = std::clamp((0.62f - v) / 0.22f, 0.0f, 1.0f);
  const float cream = part == Part::kHair ? 0.0f : std::clamp((0.80f - s) / 0.22f, 0.0f, 1.0f) * (1 - dark);
  const float fur = 1 - dark - cream;
  float body[3], chest[3], deep[3];
  HsvToRgb(c.body, body);
  HsvToRgb(c.chest, chest);
  HsvToRgb(part == Part::kHair ? c.hair : part == Part::kTattoo ? c.tattoo : c.brow, deep);
  for (int k = 0; k < 3; ++k) {
    const float want = fur * body[k] * (v / 0.95f) + cream * chest[k] * v + dark * deep[k] * (v / 0.40f);
    rgb[k] = band * std::clamp(want, 0.0f, 1.0f) + (1 - band) * rgb[k];
  }
}

// The orange texture's tattoo patch (the swirls of his back and shoulder),
// in pixels: its dark colours take the tattoo shade, other dark colours
// (eyebrows, the head's dark fur) the brow shade.
bool InTattoo(int x, int y) { return x >= 275 && x < 410 && y >= 15 && y < 135; }

uint16_t Recolour565(uint16_t c, const CrashColour& colour, Part part) {
  float rgb[3] = {float((c >> 11) & 31) / 31.0f, float((c >> 5) & 63) / 63.0f, float(c & 31) / 31.0f};
  Recolour(rgb, colour, part);
  const auto q = [](float x, int top) { return uint16_t(std::lround(std::clamp(x, 0.0f, 1.0f) * top)); };
  return uint16_t(q(rgb[0], 31) << 11 | q(rgb[1], 63) << 5 | q(rgb[2], 31));
}

// A DDS file of DXT1 blocks (128-byte header) recoloured. DXT1: every 4 x 4
// block keeps 2 colours (RGB 565) and 2-bit choices between them and their
// mixes; only the 2 colours change, and if their order flips (it picks the
// block's mode) they swap and the choices are renumbered.
bool RecolourDxt1(std::vector<uint8_t>& dds, const CrashColour& colour) {
  if (dds.size() < 128 || std::memcmp(&dds[0], "DDS ", 4) != 0 || std::memcmp(&dds[84], "DXT1", 4) != 0) {
    return false;
  }
  const uint32_t width = pure3d::Le32(&dds[16]);  // dwWidth (the header's 5th word)
  const uint32_t blocks_per_row = std::max(1u, (width + 3) / 4);
  for (size_t o = 128, i = 0; o + 8 <= dds.size(); o += 8, ++i) {
    const int x = int(4 * (i % blocks_per_row)), y = int(4 * (i / blocks_per_row));
    const Part part = InTattoo(x, y) ? Part::kTattoo : Part::kBody;
    const uint16_t c0 = uint16_t(dds[o] | dds[o + 1] << 8), c1 = uint16_t(dds[o + 2] | dds[o + 3] << 8);
    uint32_t pick = uint32_t(dds[o + 4]) | uint32_t(dds[o + 5]) << 8 | uint32_t(dds[o + 6]) << 16 |
                    uint32_t(dds[o + 7]) << 24;
    uint16_t n0 = Recolour565(c0, colour, part), n1 = Recolour565(c1, colour, part);
    if (c0 > c1) {  // 4-colour block: must stay n0 > n1
      if (n0 == n1) {
        pick = 0;
      } else if (n0 < n1) {
        std::swap(n0, n1);
        pick ^= 0x55555555u;  // 0<->1, 2<->3
      }
    } else if (n0 > n1) {  // 3-colour block (+ transparent): must stay n0 <= n1
      std::swap(n0, n1);
      uint32_t renumbered = 0;
      for (int k = 0; k < 16; ++k) {
        uint32_t v = (pick >> (2 * k)) & 3;
        if (v < 2) v ^= 1;  // 0<->1; the mix (2) and transparent (3) stay
        renumbered |= v << (2 * k);
      }
      pick = renumbered;
    }
    dds[o] = uint8_t(n0); dds[o + 1] = uint8_t(n0 >> 8);
    dds[o + 2] = uint8_t(n1); dds[o + 3] = uint8_t(n1 >> 8);
    for (int k = 0; k < 4; ++k) dds[o + 4 + k] = uint8_t(pick >> (8 * k));
  }
  return true;
}

// The hair's TGA (colour-mapped, type 1, 32-bit palette entries B G R A):
// only the palette changes.
bool RecolourTga(std::vector<uint8_t>& tga, const CrashColour& colour) {
  if (tga.size() < 18 || tga[1] != 1 || tga[2] != 1 || tga[7] != 32) return false;
  const size_t palette = 18 + tga[0];
  const size_t entries = size_t(tga[5]) | size_t(tga[6]) << 8;
  if (palette + 4 * entries > tga.size()) return false;
  for (size_t i = 0; i < entries; ++i) {
    uint8_t* e = &tga[palette + 4 * i];
    float rgb[3] = {e[2] / 255.0f, e[1] / 255.0f, e[0] / 255.0f};
    Recolour(rgb, colour, Part::kHair);
    for (int k = 0; k < 3; ++k) e[2 - k] = uint8_t(std::lround(rgb[k] * 255.0f));
  }
  return true;
}

// A Pure3D name field: length (with the NUL, rounded up to 4) + bytes.
data_patcher::Bytes NameField(const std::string& name) {
  const size_t length = (name.size() + 1 + 3) & ~size_t(3);
  data_patcher::Bytes out = {uint8_t(length)};
  out.insert(out.end(), name.begin(), name.end());
  out.resize(1 + length, 0);
  return out;
}

// A texture node (0x19000 > image 0x19001 > image data 0x19002: u32 size +
// the file's bytes) copied under `name`, its file changed by `change`.
// False if not laid out as expected.
bool TextureCopy(const pure3d::Node& source, const std::string& name,
                 const std::function<bool(std::vector<uint8_t>&)>& change, pure3d::Node* out) {
  *out = source;
  auto rename = [&](pure3d::Node& n) {
    data_patcher::Bytes data = NameField(name);
    data.insert(data.end(), n.data.begin() + pure3d::NameEnd(n), n.data.end());
    n.data = std::move(data);
  };
  rename(*out);
  if (out->children.size() != 1 || out->children[0].id != 0x19001) return false;
  pure3d::Node& image = out->children[0];
  rename(image);
  if (image.children.size() != 1 || image.children[0].id != 0x19002) return false;
  pure3d::Node& bytes = image.children[0];
  if (bytes.data.size() < 4) return false;
  const uint32_t size = pure3d::Le32(bytes.data.data());
  if (4 + size_t(size) > bytes.data.size()) return false;
  std::vector<uint8_t> file(bytes.data.begin() + 4, bytes.data.begin() + 4 + size);
  if (!change(file)) return false;
  std::copy(file.begin(), file.end(), bytes.data.begin() + 4);
  return true;
}

// The node list that holds the named texture (Crash's textures sit side by
// side at the package's top), or null.
pure3d::Node* ListWith(pure3d::Node& n, const char* texture, size_t* at) {
  for (size_t i = 0; i < n.children.size(); ++i) {
    if (n.children[i].id == 0x19000 && pure3d::NodeName(n.children[i]) == texture) {
      *at = i;
      return &n;
    }
    if (pure3d::Node* found = ListWith(n.children[i], texture, at)) return found;
  }
  return nullptr;
}

bool PatchCrashPackage(const data_patcher::Bytes& d, data_patcher::Bytes* out) {
  pure3d::Chunk root;
  std::vector<pure3d::Chunk> top;
  if (!pure3d::Parse(d, &root, &top)) return false;
  pure3d::Node tree = pure3d::ToNode(d, root);
  size_t orange_at = 0, fur_at = 0, carbon_at = 0;
  pure3d::Node* orange_list = ListWith(tree, kOrangeTexture, &orange_at);
  pure3d::Node* fur_list = ListWith(tree, kFurTexture, &fur_at);
  pure3d::Node* carbon_list = ListWith(tree, kCarbonTexture, &carbon_at);
  if (!orange_list || !fur_list || !carbon_list) {
    REXLOG_WARN("Outfits: Crash's package doesn't hold his textures as expected: no coloured Crashes");
    return false;
  }
  const pure3d::Node orange = orange_list->children[orange_at];
  const pure3d::Node fur = fur_list->children[fur_at];
  std::vector<pure3d::Node> bodies, hairs;
  for (const CrashColour& c : kColours) {
    pure3d::Node body, hair;
    if (!TextureCopy(orange, BodyName(c), [&](std::vector<uint8_t>& f) { return RecolourDxt1(f, c); },
                     &body) ||
        !TextureCopy(fur, FurName(c), [&](std::vector<uint8_t>& f) { return RecolourTga(f, c); }, &hair)) {
      REXLOG_WARN("Outfits: Crash's {} textures aren't laid out as expected: no coloured Crashes", c.id);
      return false;
    }
    bodies.push_back(std::move(body));
    hairs.push_back(std::move(hair));
  }
  pure3d::Node carbon_hair;
  if (!TextureCopy(fur, FurName(kCarbonHair),
                   [&](std::vector<uint8_t>& f) { return RecolourTga(f, kCarbonHair); }, &carbon_hair)) {
    return false;
  }
  hairs.push_back(std::move(carbon_hair));
  // Bodies after the Carbon texture, hair after the hair (the later insert
  // first when both sit in the same list, so the earlier index holds).
  auto insert = [](pure3d::Node* list, size_t at, std::vector<pure3d::Node>& nodes) {
    list->children.insert(list->children.begin() + at + 1, nodes.begin(), nodes.end());
  };
  if (carbon_list == fur_list && fur_at > carbon_at) {
    insert(fur_list, fur_at, hairs);
    insert(carbon_list, carbon_at, bodies);
  } else {
    insert(carbon_list, carbon_at, bodies);
    size_t at = 0;
    fur_list = ListWith(tree, kFurTexture, &at);
    insert(fur_list, at, hairs);
  }
  *out = pure3d::ToBytes(tree);
  g_colours_added = true;
  REXLOG_INFO("Outfits: {} coloured Crashes and {} hair textures added to Crash's package", bodies.size(),
              hairs.size());
  return true;
}

}  // namespace

// (section 3b, the hair)
void RegisterHair();
void HairUpdate(uint32_t render, uint32_t actor);

void Register() {
  data_patcher::Register("Crash in colours + every outfit's hair", 10, kCrashPackage, PatchCrashPackage);
  RegisterHair();
}

int ColourIndex(int outfit) {
  for (int i = 0; i < kColourCount; ++i) {
    if (kColours[i].outfit == outfit) return i;
  }
  return -1;
}
int ColourCount() { return kColourCount; }
float ColourHue(int index) { return index >= 0 && index < kColourCount ? kColours[index].body.h : 0.0f; }

int ListIndexOf(int outfit) {
  BuildList();
  for (uint32_t i = 0; i < g_list_count; ++i) {
    if (int(Read32(g_list + i * kEntryBytes)) == outfit) return int(i);
  }
  return -1;
}
bool OutfitAvailable(int outfit) {
  if (outfit < 0 || outfit > kLastOutfit || ListIndexOf(outfit) < 0) return false;
  if (outfit >= kFirstColour && !g_colours_added) return false;
  return SkinUnlocked(GameSkin(outfit));
}
int ShownOutfit() { return int(Read32(kSelection)); }
uint32_t ShownTemplateKey() { return kSelection + 12; }

int OutfitOf(int p) {
  if (p < 0 || p >= kMaxPlayers) return 0;
  if (p == g_wearing_player) return g_wearing_outfit;
  const int picked = Picked(p);
  if (picked >= 0) return picked;
  // As the game decides: player 1 = its skin; others: Crash's model =
  // Carbon (player 2 the original white); 3-4, the port's, a coloured Crash
  // in their marker's colour (green, purple).
  uint32_t skin = p >= 2 ? g_game_skins[p - 2] : Read32(0x8259B1C0 + 4 * p);
  if (p < 2 && !OriginalSkinFor(p, skin)) skin = 0;  // (a pick that leaked into the table)
  if (skin > 10) skin = 0;
  if (p == 0 || skin != 0) return int(skin);
  if (p >= 2 && REXCVAR_GET(carbon_colours) && g_colours_added) return p == 2 ? kCrashGreen : kCrashPurple;
  return kCarbon;
}

// --- Per-save picks (see Picked) ---------------------------------------------

void OnSaveLoaded(int number) {
  // (Read the file before taking our lock: the save drive's thread holds the
  // files' lock and then asks for ours when it writes.)
  std::array<int8_t, kMaxPlayers> stored{};
  const bool has_trailer = save_files::ReadOutfitTrailer(number, &stored);
  std::lock_guard guard(g_picks_mutex);
  if (has_trailer) {
    for (int p = 0; p < kMaxPlayers; ++p) g_picks[p] = stored[p];
    REXLOG_INFO("Outfits: save {} has its own picks ({} {} {} {})", number, g_picks[0], g_picks[1],
                g_picks[2], g_picks[3]);
  } else {
    // A save from before the picks were per save: the game decides.
    g_picks = {-1, -1, -1, -1};
    REXLOG_INFO("Outfits: save {} has no picks of its own: the game decides", number);
  }
}
void OnNewGame() {
  std::lock_guard guard(g_picks_mutex);
  g_picks = {-1, -1, -1, -1};
}
std::array<int8_t, kMaxPlayers> CurrentPickBytes() {
  std::lock_guard guard(g_picks_mutex);
  std::array<int, kMaxPlayers>& now = CurrentPicks();
  std::array<int8_t, kMaxPlayers> out{};
  for (int p = 0; p < kMaxPlayers; ++p) out[p] = int8_t(std::clamp(now[p], -1, kLastOutfit));
  return out;
}

}  // namespace outfits

using namespace outfits;

// --- 1. Who wears what --------------------------------------------------------

// --- 1b. The cutscenes' Crash (bug report 2026-10-21, save "test11") ---
// A cutscene actor flagged "the player" (its +0x44 bit 0x10) gets its MODEL
// from player 1's skin package: sub_8223E8A0 asks sub_8229F8B8(0) at
// 0x8223E93C, turns the skin into a package name (sub_8229F810) and finds
// that section; the actor itself is built from the cutscene's CRASH
// template (sub_8223F0B8). In the game player 1 is always a Crash, so the
// package always fit. With player 1 in Coco's outfit the Crash template got
// Coco's package: its character object stayed null and the model set-up
// read it (sub_820B0DF8 -> sub_8210AE80 -> sub_822B7BE0, a read of
// 0x00000194; crash-2026-10-21_180345.txt). Crash models (Purple, Carbon...)
// were fine. HOW WE FOUND IT: the crash log's game stack + xexdis of
// sub_8223E8A0 (the li r3,0 / bl sub_8229F8B8 pair).
// Our answer there: the first player (1, then 2-4) whose outfit is a Crash
// model AND whose package is loaded; else plain Crash (0) if his is loaded.
constexpr uint32_t kAskCutscenePlayer = 0x8223E940;  // return address of that call
// Is skin `skin`'s package loaded (the same lookup sub_8223E8A0 makes)?
bool SkinPackageLoaded(PPCContext& ctx, uint8_t* base, uint32_t skin) {
  static uint32_t handle = 0;  // guest room for the 4-byte name handle
  if (!handle) handle = rex::system::kernel_memory()->SystemHeapAlloc(16);
  if (!handle) return false;
  const uint32_t name = CallGame(__imp__sub_8229F810, ctx, base, skin);
  CallGame(__imp__sub_8236AC90, ctx, base, handle, name);
  const uint32_t inventory = CallGame(__imp__sub_822E3FA8, ctx, base, 0, handle);
  const uint32_t section = CallGame(__imp__sub_822E4680, ctx, base, inventory, handle);
  CallGame(__imp__sub_823C9A70, ctx, base, handle);
  return section != 0;
}
uint32_t CutsceneCrashSkin(PPCContext& ctx, uint8_t* base) {
  for (int p = 0; p < kMaxPlayers; ++p) {
    if (p > 0 && !menu_input::PlayerPlays(p)) continue;
    const uint32_t skin = GameSkin(OutfitOf(p));
    if (!IsCoco(skin) && SkinPackageLoaded(ctx, base, skin)) return skin;
  }
  if (!SkinPackageLoaded(ctx, base, 0)) {
    // Every player is Coco and Crash's own package isn't in memory: the
    // game's crash would follow. Logged so a report shows it.
    REXLOG_WARN("Outfits: a cutscene wants Crash but no Crash package is loaded (every player is Coco?)");
  }
  return 0;
}

extern "C" REX_FUNC(sub_8229F8B8) {
  const uint32_t lr = uint32_t(ctx.lr);
  int p = int(ctx.r3.u32);
  if (lr == kAskOpening || lr == kAskPageTurn) {
    ctx.r3.u64 = uint32_t(OutfitOf(g_page_player));  // the screen sees Carbon as 11
    return;
  }
  if (lr == kAskCutscenePlayer) {
    const uint32_t skin = CutsceneCrashSkin(ctx, base);
    static uint32_t logged = ~0u;
    if (skin != logged) REXLOG_INFO("Outfits: the cutscenes' Crash wears skin {}", skin);
    logged = skin;
    ctx.r3.u64 = skin;
    return;
  }
  const int building = more_players::BuildingSpawnFor();
  if (p == 1 && building >= 2) p = building;  // players 3-4's spawn events ask as player 2
  if (p < 0 || p >= kMaxPlayers) {
    __imp__sub_8229F8B8(ctx, base);
    return;
  }
  const int picked = Picked(p);
  if (picked >= 0) {
    ctx.r3.u64 = GameSkin(picked);
  } else if (p >= 2) {
    ctx.r3.u64 = g_game_skins[p - 2];
  } else {
    ctx.r3.u64 = uint32_t(p);
    __imp__sub_8229F8B8(ctx, base);
    if (!OriginalSkinFor(p, ctx.r3.u32)) ctx.r3.u64 = 0;  // (a pick that leaked into the table)
  }
}

extern "C" REX_FUNC(sub_8229F8A0) {
  const int p = int(ctx.r3.u32);
  if (p == g_applying) {
    // A pick at the chest: kept as the player's outfit (this save's picks).
    {
      std::lock_guard guard(g_picks_mutex);
      CurrentPicks()[p] = g_applying_outfit;  // reaches the file with the next save
    }
    REXLOG_INFO("Outfits: player {} wears outfit {}", p + 1, g_applying_outfit);
  }
  if (p >= 2 && p < kMaxPlayers) {
    g_game_skins[p - 2] = ctx.r4.u32;
    return;
  }
  // A pick the original couldn't make for this player stays in the picks
  // only: the game's table (and so the save's own bytes) keeps its value.
  if (REXCVAR_GET(outfit_store_fix) && p == g_applying && !OriginalSkinFor(p, ctx.r4.u32)) return;
  __imp__sub_8229F8A0(ctx, base);
}

// --- 2. Carbon in the skin-number lookups ---------------------------------------

extern "C" REX_FUNC(sub_8229F7F8) {
  if (IsCarbon(int(ctx.r3.u32))) {
    ctx.r3.u64 = kCrashName;
    return;
  }
  __imp__sub_8229F7F8(ctx, base);
}
extern "C" REX_FUNC(sub_8229F8D0) {
  if (IsCarbon(int(ctx.r3.u32))) ctx.r3.u64 = 0;
  __imp__sub_8229F8D0(ctx, base);
}
extern "C" REX_FUNC(sub_8229F858) {
  if (IsCarbon(int(ctx.r3.u32))) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_8229F858(ctx, base);
}

// --- 3. The texture set, per frame ---------------------------------------------

namespace {
// Player / preview characters: the texture set of their outfit.
void ChooseSkinSet(uint32_t render, uint32_t actor) {
  const int p = PlayerOfActor(actor);
  int outfit = -1;
  if (p >= 0) {
    outfit = OutfitOf(p);
  } else if (const int o = outfit_previews::OutfitForActor(actor); o >= 0) {
    outfit = o;  // PER-PLAYER PREVIEWS (outfit_previews.h): that preview's outfit
  } else if (NowMs() - g_screen_ms.load() < 250 && Read32(kSelection) <= uint32_t(kLastOutfit)) {
    // Not a player, while the skin screen is up: its preview, wearing the
    // shown entry (-1 = a locked one: left alone).
    outfit = int(Read32(kSelection));
  }
  if (outfit >= 0 && !IsCoco(GameSkin(outfit))) {
    Write32(render + kRenderSkinSet, IsCarbon(outfit) ? 0u : 1u);
  }
}
}  // namespace

// Set up (once, when the character appears) and Update (every frame: turns
// +0xE4 into the blend between the two texture sets, sub_822AFC30).
namespace {
// A texture name's key (8 bytes in guest memory, made once by the game's own
// sub_8236ACB8), 0 if out of memory.
uint32_t NameKey(PPCContext& ctx, uint8_t* base, const std::string& name) {
  static std::unordered_map<std::string, uint32_t> keys;
  if (auto it = keys.find(name); it != keys.end()) return it->second;
  const uint32_t key = rex::system::kernel_memory()->SystemHeapAlloc(uint32_t(8 + name.size() + 1));
  if (!key) return 0;
  std::memcpy(Guest(key + 8), name.c_str(), name.size() + 1);
  CallGame(__imp__sub_8236ACB8, ctx, base, key, key + 8);
  keys[name] = key;
  return key;
}
// The body texture an outfit of the Carbon slot names: the white Carbon's
// own, or the colour's copy.
uint32_t CarbonKey(PPCContext& ctx, uint8_t* base, int outfit) {
  const CrashColour* c = outfit == kCarbon ? nullptr : ColourOf(outfit);
  return NameKey(ctx, base, c ? BodyName(*c) : std::string(kCarbonTexture));
}
// The outfit a character is set up / drawn for: its player's, or (not a
// player, the skin screen up) the preview's shown one; -1 = none of ours.
int OutfitForActor(uint32_t actor) {
  const int p = PlayerOfActor(actor);
  if (p >= 0) return OutfitOf(p);
  // PER-PLAYER PREVIEWS (outfit_previews.h): each preview wears its own outfit.
  if (const int o = outfit_previews::OutfitForActor(actor); o >= 0) return o;
  if (NowMs() - g_screen_ms.load() < 250 && Read32(kSelection) <= uint32_t(kLastOutfit)) {
    return int(Read32(kSelection));
  }
  return -1;
}
}  // namespace

extern "C" REX_FUNC(sub_82185E60) {
  const uint32_t render = ctx.r3.u32, actor = ctx.r4.u32;
  ChooseSkinSet(render, actor);
  // Carbon's texture (set 2) is bound HERE, once: the outfit's (white or a
  // coloured copy; the copies only when the patch made them this session).
  const int outfit = OutfitForActor(actor);
  if (IsCarbon(outfit) && g_colours_added && Read32(render + kRenderCarbonName) != 0) {
    if (const uint32_t key = CarbonKey(ctx, base, outfit)) {
      std::memcpy(Guest(render + kRenderCarbonName), Guest(key), 8);
    }
  }
  const uint32_t section = ctx.r5.u32;
  __imp__sub_82185E60(ctx, base);
  if (const uint32_t object = Read32(render + kRenderObject)) g_section_of[object] = section;
}
extern "C" REX_FUNC(sub_821858C0) {
  const int p = PlayerOfActor(ctx.r4.u32);
  if (p >= 0) g_render_of[p] = ctx.r3.u32;  // (Wear Skin: which Carbon it has bound)
  ChooseSkinSet(ctx.r3.u32, ctx.r4.u32);
  HairUpdate(ctx.r3.u32, ctx.r4.u32);
  __imp__sub_821858C0(ctx, base);
}

// --- 3b. The hair -------------------------------------------------------------------
// Crash's hair is its own material (crash_hair_color_shader, an
// xnSimpleShader), ONE object shared by every Crash of the package, so its
// texture can't simply be set per character. Per character, though, the
// game draws a character's whole model at once: sub_822B6FB8 / sub_822B6DB0,
// r3 = its CCharacterObject (= the render behaviour's +0x98). For that span, the hair material's texture (+92,
// read by its draw setup, xnSimpleShader::SetupDraw 0x82441EA8, for every
// draw that uses it) is the outfit's hair copy, and it's given back right
// after (+92 holds a reference: what's there when the material goes must be
// its own texture). Both pictures see it (the set-up is the game's).
// Textures are found the way the render behaviour's set-up finds its body
// textures: sub_820D8908(the set-up's section r5, the name's 64-bit key) ->
// a tTexture, its pddiTexture at +0x18 (as sub_822AF180 reads it).
namespace {
// During one Crash's Display: the hair material's texture `from` -> `to`
// (pddiTextures, tTexture +0x18), and the material it was changed on.
uint32_t g_hair_from = 0, g_hair_to = 0, g_hair_shader = 0;

uint32_t FindTexture(PPCContext& ctx, uint8_t* base, uint32_t section, uint32_t key) {
  if (!section || !key) return 0;
  const PPCContext saved = ctx;
  ctx.r3.u64 = section;
  ctx.r4.u64 = uint64_t(Read32(key)) << 32 | Read32(key + 4);
  __imp__sub_820D8908(ctx, base);
  const uint32_t texture = ctx.r3.u32;
  ctx = saved;
  return texture ? Read32(texture + 0x18) : 0;
}

void HairSetupDraw(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t shader = ctx.r3.u32;

  if (g_hair_to && Read32(shader + kSimpleTexture) == g_hair_from) {
    Write32(shader + kSimpleTexture, g_hair_to);
    g_hair_shader = shader;
  }
  // A floating button's coin: its owner's copy of the B, for this set-up only.
  uint32_t coin_from = 0, coin_to = 0;
  if (prompt_icons::WorldButtonSwap(base, shader, native::recorder::CurrentMeshContext(), coin_from,
                                    coin_to)) {
    // Stays until the frame ends (prompt_icons WorldButtonRestore): the native
    // renderer reads the material's texture at the draw, after this set-up.
    if (coin_to != coin_from) Write32(shader + kSimpleTexture, coin_to);
    original(ctx, base);
    return;
  }
  original(ctx, base);
}
}  // namespace

void outfits::HairUpdate(uint32_t render, uint32_t actor) {
  if (const uint32_t object = Read32(render + kRenderObject)) g_outfit_of[object] = OutfitForActor(actor);
}

void outfits::RegisterHair() { pddi::SetHandler(pddi::kFn_82441EA8, HairSetupDraw); }

namespace {
// One Crash drawn by `draw` (the original display) with its outfit's hair.
int g_hair_depth = 0;  // (the draws nest: slot 8 calls the model draw)
void DrawWithHair(PPCContext& ctx, uint8_t* base, void (*draw)(PPCContext&, uint8_t*)) {
  if (g_hair_depth > 0) {
    draw(ctx, base);
    return;
  }
  ++g_hair_depth;
  const uint32_t object = ctx.r3.u32;
  g_hair_from = g_hair_to = g_hair_shader = 0;
  if (g_colours_added) {
    const auto outfit = g_outfit_of.find(object);
    const auto section = g_section_of.find(object);
    const CrashColour* c = outfit != g_outfit_of.end() ? ColourOf(outfit->second) : nullptr;
    if (c && section != g_section_of.end()) {
      g_hair_from = FindTexture(ctx, base, section->second, NameKey(ctx, base, kFurTexture));
      g_hair_to = FindTexture(ctx, base, section->second, NameKey(ctx, base, FurName(*c)));
      if (!g_hair_from || !g_hair_to) g_hair_from = g_hair_to = 0;
    }
  }
  draw(ctx, base);
  if (g_hair_shader) Write32(g_hair_shader + kSimpleTexture, g_hair_from);
  g_hair_from = g_hair_to = g_hair_shader = 0;
  --g_hair_depth;
}
}  // namespace
// The two functions that draw a CCharacterObject's model (r3 = the object),
// whichever pass asks (call stacks at the hair's set-up, logged 2026-10-17):
// sub_822B6FB8 (from its vtable 0x82041734 slots 6 and 8) and sub_822B6DB0
// (slot 11, draws its +28 drawable).
extern "C" REX_FUNC(sub_822B6FB8) { DrawWithHair(ctx, base, __imp__sub_822B6FB8); }
extern "C" REX_FUNC(sub_822B6DB0) { DrawWithHair(ctx, base, __imp__sub_822B6DB0); }

// --- 4. The screen ----------------------------------------------------------------

// The player Back turns the page to (the next one on foot), -1 = none.
int NextPagePlayer() {
  for (int step = 1; step < kMaxPlayers; ++step) {
    const int p = (g_page_player + step) % kMaxPlayers;
    if (Read32(kCoOpState + 4 * p) == 2) return p;
  }
  return -1;
}

// "Goto Player 2" / "Goto Player 1": the game's two texts name the other of
// TWO players; with four, the digit becomes the real next player.
void NameNextPage(PPCContext& ctx, uint8_t* base, uint32_t screen) {
  const int next = NextPagePlayer();
  if (next < 0) return;
  for (uint32_t field : kScreenGotoPrompts) {
    const uint32_t text = Read32(screen + field);
    const uint32_t string = text ? Read32(text + kTextString) : 0;
    if (!string) continue;
    std::u16string value;
    for (uint32_t i = 0; i < 64; ++i) {
      const char16_t c = char16_t(Read32(string + 2 * i) >> 16);
      if (!c) break;
      value.push_back(c);
    }
    const size_t digit = value.find_last_of(u"1234");
    if (digit == std::u16string::npos) continue;
    const char16_t want = char16_t(u'1' + next);
    if (value[digit] == want) continue;
    value[digit] = want;
    scrooby::SetText(ctx, base, text, value);
  }
}

extern "C" REX_FUNC(sub_820E0858) {
  g_screen_ms = NowMs();
  const uint32_t screen = ctx.r3.u32;
  g_skin_screen = screen;  // (section 5b: the preview across a respawn)
  const double dt = ctx.f1.f64;  // (the Update's frame time)
  __imp__sub_820E0858(ctx, base);
  outfit_picker::AfterUpdate(ctx, base, screen);  // the co-op picker (outfit_picker.h)
  outfit_previews::Tick(ctx, base, screen, dt);   // one model per player (outfit_previews.h)
  NameNextPage(ctx, base, screen);
}

// Opening: r3 = the player at the chest; the page's +0x84 = its layout.
void OutfitsChestOpen(PPCRegister& r3) {
  BuildList(true);  // (carbon_colours may have changed)
  g_page_player = int(r3.u32) & 3;
  r3.u64 = uint32_t(g_page_player & 1);
}

// Back: r4 = the game's other page (1 - page); ours = the next player on foot.
void OutfitsChestNextPage(PPCRegister& r4, PPCRegister& r31) {
  (void)r31;
  const int next = NextPagePlayer();
  if (next >= 0) g_page_player = next;
  r4.u64 = uint32_t(g_page_player & 1);
}

extern "C" REX_FUNC(sub_82263630) {
  if (uint32_t(ctx.lr) == kChooserPageTurn) ctx.r4.u64 = uint32_t(g_page_player);
  __imp__sub_82263630(ctx, base);
}

// THE PREVIEW (findings/46 s.4): the screen builds it for the PAGE: its
// animation set (sub_820E0A30: page 1 = Crash's, page 2 = Coco's except for
// Carbon) and its random idle (sub_820E0C98: a list per page). With every
// outfit on every page that has to follow the shown OUTFIT: Coco on page 1
// got Crash's moves = no animation for her (null read in sub_82351340).
// At the controller's creation (bl sub_8211AA50): r5 = the animation set,
// r6 = the preview's "player" (its texture set: we pick that per frame).
void OutfitsPreviewCharacter(PPCRegister& r5, PPCRegister& r6) {
  // (PER-PLAYER PREVIEWS: one of ours being built = its own outfit.)
  const int building = outfit_previews::BuildingOutfit();
  const uint32_t shown = building >= 0 ? uint32_t(building) : Read32(kSelection);
  r5.u64 = IsCoco(GameSkin(int(std::min<uint32_t>(shown, uint32_t(kLastOutfit))))) ? kPreviewCocoMoves
                                                                               : kPreviewCrashMoves;
  r6.u64 = IsCarbon(int(shown)) ? 1u : 0u;
}
extern "C" REX_FUNC(sub_820E0C98) {
  const uint32_t screen = ctx.r3.u32;
  const uint32_t page = Read32(screen + kScreenPage);
  // (PER-PLAYER PREVIEWS: one of our shadow screens = its own outfit.)
  const int own = outfit_previews::OutfitOfScreen(screen);
  const uint32_t shown = own >= 0 ? uint32_t(own) : Read32(kSelection);
  Write32(screen + kScreenPage,
          IsCoco(GameSkin(int(std::min<uint32_t>(shown, uint32_t(kLastOutfit))))) ? 1u : 0u);
  __imp__sub_820E0C98(ctx, base);
  Write32(screen + kScreenPage, page);
}


// The coloured Crashes share Carbon Crash's name and text in the game's
// bank: after the screen shows one, its own. (Title = this +0x24; the
// description = +0x28 / +0x2C, one per layout: both set.)
extern "C" REX_FUNC(sub_820E1228) {
  const uint32_t screen = ctx.r3.u32, index = ctx.r4.u32;
  __imp__sub_820E1228(ctx, base);
  if (!g_list || index >= g_list_count) return;
  const int outfit = int(Read32(g_list + index * kEntryBytes));
  const CrashColour* c = outfit == kCarbon ? nullptr : ColourOf(outfit);
  if (!c) return;
  scrooby::SetText(ctx, base, Read32(screen + 0x24), c->name);
  if (Read32(kSelection) != uint32_t(outfit)) return;  // (a locked entry shows "Locked")
  scrooby::SetText(ctx, base, Read32(screen + 0x28), c->text);
  scrooby::SetText(ctx, base, Read32(screen + 0x2C), c->text);
}

extern "C" REX_FUNC(sub_820E15A8) {
  BuildList();
  if (!g_list) {
    __imp__sub_820E15A8(ctx, base);
    return;
  }
  const uint32_t i = std::min(ctx.r5.u32, g_list_count - 1);
  ctx.r3.u64 = g_list + i * kEntryBytes;
}

extern "C" REX_FUNC(sub_820E1428) {
  BuildList();
  const uint32_t shown = Read32(kSelection);
  for (uint32_t i = 0; i < g_list_count; ++i) {
    if (Read32(g_list + i * kEntryBytes) == shown) {
      ctx.r3.u64 = i;
      return;
    }
  }
  ctx.r3.u64 = 0;
}

extern "C" REX_FUNC(sub_820E14B8) {
  const uint32_t screen = ctx.r3.u32;
  __imp__sub_820E14B8(ctx, base);
  BuildList();
  if (g_list_count) Write32(screen + kScreenCount, g_list_count);
}

// --- 5b. The chest's preview across a respawn -----------------------------------
//
// THE COCO PICK CRASH (2026-10-24). A Crash <-> Coco pick respawns the players
// (sub_8227C5A0: sub_8229CB78 + game +0xA8 bit 0x80 = the players' section is
// made again, like a small level reload: every actor in the zones is let go,
// sub_822EFEA0 -> sub_820B0C58 + Release). The chest's 3D preview is such an
// actor: its build (sub_820E0A30) puts it in the zone manager's own zone
// (sub_822E6B60, r3 = game +100). In the game that never mattered: Wear Skin
// is the screen's exit (front end exit 850 in the same frame as the respawn),
// so the screen and its preview were gone. The outfit picker keeps the
// screen open while others choose, so the screen's per-frame code
// (sub_820E1C70) went on animating a preview the respawn had freed: reads of
// 0, 0x20, 0x24 in sub_82350F88 / sub_8237AB58 from 0x820E1E40, 1.5-10 s after
// the pick (whenever the next idle animation started). Crash models were fine
// (no respawn: their swap happens on the spot). Found from the crash reports'
// stacks, a trace of the preview's builds and animation changes, and the
// 2026-10-21 log of the same pick before the picker (exit 850 = screen gone).
//
// The fix: right before the respawn the preview is destroyed the screen's own
// way (sub_820E1180: actor +92, controller +96 let go, +92 = 0; the outfit
// picker does the same to change the shown outfit), and the screen may not
// build a new one (sub_820E0A30 skipped; its per-frame code just asks again
// next frame) until the level has made its players again
// (more_players::PlayerSetsMade() moved on) and the player's new character
// exists. Then the screen builds a fresh preview of the shown outfit.
extern "C" REX_FUNC(__imp__sub_820E1180);  // skin screen: destroy its preview character
extern "C" REX_FUNC(__imp__sub_820E0A30);  // skin screen: build its preview (r4 = the section)

namespace outfits {
void PreviewAwayForRespawn(PPCContext& ctx, uint8_t* base, int p) {
  const uint32_t screen = g_skin_screen;
  if (!screen || NowMs() - g_screen_ms.load() > 250) return;  // (no screen up)
  if (Read32(screen + 0x5C)) CallGame(__imp__sub_820E1180, ctx, base, screen);
  g_preview_held = true;
  g_held_sets = more_players::PlayerSetsMade();
  g_held_old_character = more_players::CharacterOfPlayer(p);
  g_held_player = p;
  g_held_since = NowMs();
  outfit_previews::LetGoForRespawn(ctx, base);  // (the picker's per-player previews too)
  REXLOG_INFO("Outfits: the chest's preview is let go for the respawn (screen {:08X})", screen);
}
bool PreviewHeld() {
  if (!g_preview_held) return false;
  const uint32_t now_character = more_players::CharacterOfPlayer(g_held_player);
  const bool remade = more_players::PlayerSetsMade() != g_held_sets && now_character &&
                      now_character != g_held_old_character;
  if (!remade && NowMs() - g_held_since < 10000) return true;
  g_preview_held = false;
  REXLOG_INFO("Outfits: the players are back{}: the chest's preview may be built again",
              remade ? "" : " (gave up waiting after 10 s)");
  return false;
}
}  // namespace outfits

extern "C" REX_FUNC(sub_820E0A30) {
  if (PreviewHeld()) return;  // (the screen asks again next frame)
  __imp__sub_820E0A30(ctx, base);
}

// --- 5c. The respawn happens where the players are -----------------------------
//
// TELEPORTED TO THE TIKI / TV / BOOKS (2026-10-24). After using the save tiki,
// the TV or the books in Crash's house, a Crash <-> Coco pick at the chest
// put the players back at that object, not at the chest. The respawn makes the
// players' spawn events again (sub_8229C550), and each event's position comes
// from, in order: a fixed position in the global options block
// 0x825A7374 (sub_822C8358(); +0xC4 byte = on, +0xC8/+0xCC/+0xD0 = x, y, z;
// the zone manager's start, sub_822E69E0, reads the same), a named spawn point
// (+0xD4), else the level's CURRENT spawn point (sub_8227BC40) per player
// (sub_82279260). Those objects make themselves the current spawn point (their
// screens come back through a reload); the chest doesn't, since in the game a
// pick at the chest never reloaded for player 1. Found by reading
// sub_8229C550 after the reports.
//
// The fix: for the respawn of a chest pick, the fixed position is set to where
// the picking player stands (their character's world matrix), and switched off
// again (the byte and the old position put back) once the level has made its
// players again (more_players::PlayerSetsMade() moved on) and one more second
// has passed (the zone manager's start reads it too). Nothing else in the game
// sets that block in play, so other respawns (deaths, level changes) keep the
// spawn points.
namespace outfits {
namespace {
constexpr uint32_t kStartOptions = 0x825A7374;  // sub_822C8358()
constexpr uint32_t kFixedOn = kStartOptions + 0xC4, kFixedPosition = kStartOptions + 0xC8;
struct FixedSpawn {
  std::atomic<bool> active{false};
  uint8_t old_on = 0;
  uint32_t old_position[3] = {};
  uint32_t sets = 0;              // PlayerSetsMade() at the respawn
  int64_t since = 0;              // when it was set
  int64_t made_at = 0;            // when the players were made again (0 = not yet)
} g_fixed;

// A character's position: Crash +44 -> its world matrix, translation at +48
// (findings/26 s.12).
bool PositionOfCharacter(uint32_t character, float out[3]) {
  if (character < 0x40000000) return false;
  const uint32_t matrix = Read32(character + 44);
  if (matrix < 0x40000000) return false;
  for (int i = 0; i < 3; ++i) {
    const uint32_t bits = Read32(matrix + 48 + 4 * i);
    std::memcpy(&out[i], &bits, 4);
    if (!std::isfinite(out[i])) return false;
  }
  return true;
}

// (Frame-end listener, the game's main thread.) Off again once it was used.
void FixedSpawnFrameEnd(void*) {
  if (!g_fixed.active.load()) return;
  const int64_t now = NowMs();
  if (!g_fixed.made_at && more_players::PlayerSetsMade() != g_fixed.sets) g_fixed.made_at = now;
  const bool used = g_fixed.made_at && now - g_fixed.made_at > 1000;
  if (!used && now - g_fixed.since < 20000) return;
  *Guest(kFixedOn) = g_fixed.old_on;
  for (int i = 0; i < 3; ++i) Write32(kFixedPosition + 4 * i, g_fixed.old_position[i]);
  g_fixed.active = false;
  REXLOG_INFO("Outfits: the respawn's spawn spot is back to the level's{}", used ? "" : " (gave up after 20 s)");
}
}  // namespace

void RespawnWhereTheyStand(int p) {
  static bool listening = false;
  if (!listening) {
    pddi::AddFrameEndListener(&FixedSpawnFrameEnd, nullptr);
    listening = true;
  }
  float at[3];
  if (!PositionOfCharacter(more_players::CharacterOfPlayer(p), at)) return;
  if (!g_fixed.active.load()) {  // (the level's own values, kept once)
    g_fixed.old_on = *Guest(kFixedOn);
    for (int i = 0; i < 3; ++i) g_fixed.old_position[i] = Read32(kFixedPosition + 4 * i);
  }
  *Guest(kFixedOn) = 1;
  for (int i = 0; i < 3; ++i) {
    uint32_t bits;
    std::memcpy(&bits, &at[i], 4);
    Write32(kFixedPosition + 4 * i, bits);
  }
  g_fixed.sets = more_players::PlayerSetsMade();
  g_fixed.since = NowMs();
  g_fixed.made_at = 0;
  g_fixed.active = true;
  REXLOG_INFO("Outfits: the respawn puts the players where player {} stands ({:.1f} {:.1f} {:.1f})", p + 1, at[0],
              at[1], at[2]);
}
}  // namespace outfits

// --- 5. Wear Skin ---------------------------------------------------------------

extern "C" REX_FUNC(sub_82123880) {
  const uint32_t game = Read32(kGameGlobal);
  const uint32_t front_end = Read32(game + kFrontEndOf);
  const int p = int(Read32(front_end + kChooser));
  const int outfit = int(Read32(kSelection));
  const uint32_t actor = p >= 0 && p < kMaxPlayers ? more_players::CharacterOfPlayer(p) : 0;
  if (actor && outfit >= 0 && outfit <= kLastOutfit) {
    const uint32_t old_skin = GameSkin(OutfitOf(p));
    const uint32_t new_skin = GameSkin(outfit);
    g_wearing_player = p;
    g_wearing_outfit = outfit;
    // A Carbon whose texture isn't the one this Crash bound when it was set
    // up (white <-> a colour, or one colour to another): it binds at set-up
    // only, so the model is swapped in place (onto Crash's own) to set it up
    // again. (A respawn did it too, but with partners on foot it brought
    // player 1 back as a mask.)
    bool rebind = false;
    if (IsCarbon(outfit) && g_colours_added && g_render_of[p]) {
      const uint32_t key = CarbonKey(ctx, base, outfit);
      rebind = key && std::memcmp(Guest(g_render_of[p] + kRenderCarbonName), Guest(key), 8) != 0;
    }
    if (IsCoco(old_skin) != IsCoco(new_skin)) {
      // Crash <-> Coco: a different character; the game respawns the players,
      // each from their outfit (as it does for player 2).
      PreviewAwayForRespawn(ctx, base, p);  // (section 5b)
      RespawnWhereTheyStand(p);              // (section 5c)
      CallGame(__imp__sub_8227C5A0, ctx, base, game, uint32_t(p));
    } else if (old_skin != new_skin || rebind) {
      // Another model of the same character: SelectSkin's own swap on the actor.
      // (sub_822E3FA8 = the inventory manager; sub_822E4680 -> sub_822E4000 =
      // its section named *r4: the new outfit's package.)
      const uint32_t name = kSelection + 12;
      CallGame(__imp__sub_820B0E98, ctx, base, actor);
      uint32_t inventory = CallGame(__imp__sub_822E3FA8, ctx, base, 0, name);
      uint32_t section = CallGame(__imp__sub_822E4680, ctx, base, inventory, name);
      CallGame(__imp__sub_820B09C8, ctx, base, actor, section);
      inventory = CallGame(__imp__sub_822E3FA8, ctx, base, 0, name);
      const uint32_t a = CallGame(__imp__sub_822E4680, ctx, base, inventory, name);
      const PPCContext saved = ctx;
      ctx.r3.u64 = actor; ctx.r4.u64 = a; ctx.r5.u64 = 8;
      __imp__sub_820B0DF8(ctx, base);
      ctx = saved;
    }
    // (Carbon <-> orange Crash: same model, the draw picks the texture set.)
    g_applying = p;
    g_applying_outfit = outfit;
    CallGame(sub_822665B0, ctx, base, front_end, uint32_t(p), new_skin);  // ours (section 6)
    g_applying = -1;
    g_wearing_player = -1;
    REXLOG_INFO("Outfits: player {} picked outfit {} (game skin {} -> {})", p + 1, outfit, old_skin,
                new_skin);
  }
  CallGame(__imp__sub_822E7880, ctx, base, Read32(game + kInventoryOf));
}

// --- 6. The front end's outfit packages: give back what was taken --------------
//
// THE QUIT -> LOAD GAME CRASH (2026-10-24). Every outfit's model lives in its
// own package (Crash's, Coco's, ...), counted by the inventory: a package is
// unloaded when its last reference goes. The front end holds one reference
// per player, for what that player wears:
//   * taken once at boot: sub_8225FED8 (front-end package step 3, part 0)
//     asks sub_8229F8B8(p) for players 1-2 and takes each package;
//   * moved by the store sub_822665B0(front end, p, skin): it asks
//     sub_8229F8B8(p) for the OLD outfit, sets the new one (sub_8229F8A0),
//     asks again for the NEW one, takes the new package and gives back the
//     OLD one (sub_822E44B8 / sub_822E4650 by name handle).
// In the game the getter's answer only changes through the setter, so "ask
// again for the old one" always named the package held. Ours also answers a
// player's PICK (per save, and only while the save has that outfit
// unlocked), which changes when a save is loaded: the picks and the unlock
// list arrive before the loader's stores (sub_8229F9A8). Traced: at boot the
// front end took Crash for player 1; loading a save whose pick is Coco, the
// store gave back COCO (the getter already said Coco) and took Coco; a Quit
// Game + Load Game reset (stores of skin 0 from sub_8229F960) gave Coco back
// AGAIN = one release too many: Coco's package was unloaded while player 1
// still wore it, and the next "take Coco" read the half-freed section's
// dependency list (null + 4 in sub_822E28E0, crash-2026-10-23_225531.txt).
// Found with the crash report's game stack + a trace of every store (player,
// skin, the getter's answer before / after, the game's table).
//
// The fix: we remember, per player, which outfit's package the front end
// HOLDS (g_front_end_holds, by the getter's answer when it was taken), and
// the store gives back exactly that one. Players 3-4 (the port's; the store
// is called for them by our chest code) start holding nothing: the first
// store only takes. The store's steps are otherwise the game's.
extern "C" REX_FUNC(__imp__sub_822E44B8);  // inventory: take a reference (r3 inventory, r4 name handle)
extern "C" REX_FUNC(__imp__sub_822E4650);  // inventory: give one back (same)
extern "C" REX_FUNC(__imp__sub_8225FED8);  // front-end packages: step r4, part r5

namespace outfits {
namespace {
constexpr uint32_t kNone = 0xFFFFFFFFu;
uint32_t g_front_end_holds[kMaxPlayers] = {kNone, kNone, kNone, kNone};  // skin numbers

// Takes (take = true) or gives back a reference on skin's package.
void SkinPackageReference(PPCContext& ctx, uint8_t* base, uint32_t skin, bool take) {
  static uint32_t handle = 0;  // guest room for the 4-byte name handle
  if (!handle) handle = rex::system::kernel_memory()->SystemHeapAlloc(16);
  if (!handle) return;
  const uint32_t name = CallGame(__imp__sub_8229F810, ctx, base, skin);
  CallGame(__imp__sub_8236AC90, ctx, base, handle, name);
  const uint32_t inventory = CallGame(__imp__sub_822E3FA8, ctx, base, 0, handle);
  CallGame(take ? __imp__sub_822E44B8 : __imp__sub_822E4650, ctx, base, inventory, handle);
  CallGame(__imp__sub_823C9A70, ctx, base, handle);
}
}  // namespace
}  // namespace outfits

// Boot: the front end's first references (players 1-2). Recorded right after,
// with the same answer the original just used.
extern "C" REX_FUNC(sub_8225FED8) {
  const PPCContext call = ctx;
  __imp__sub_8225FED8(ctx, base);
  if (call.r4.u32 != 3 || call.r5.u32 != 0) return;
  const PPCContext after = ctx;
  for (int p = 0; p < 2; ++p) g_front_end_holds[p] = CallGame(sub_8229F8B8, ctx, base, uint32_t(p));
  ctx = after;
  REXLOG_INFO("Outfits: the front end holds the packages of skins {} and {} (players 1-2)", g_front_end_holds[0],
              g_front_end_holds[1]);
}

// The store, rewritten (same steps; gives back what it holds).
extern "C" REX_FUNC(sub_822665B0) {
  const PPCContext call = ctx;
  const uint32_t p = call.r4.u32, skin = call.r5.u32;
  if (p >= uint32_t(kMaxPlayers) || !REXCVAR_GET(outfit_store_fix)) {
    __imp__sub_822665B0(ctx, base);
    return;
  }
  // (A player 1-2 store before the boot step was seen: the game's own rule.)
  uint32_t held = g_front_end_holds[p];
  if (held == kNone && p < 2) held = CallGame(sub_8229F8B8, ctx, base, p);
  CallGame(sub_8229F8A0, ctx, base, p, skin);  // ours: picks + players 3-4
  const uint32_t now = CallGame(sub_8229F8B8, ctx, base, p);
  SkinPackageReference(ctx, base, now, true);  // new first: a same-package store never drops to 0
  if (held != kNone) SkinPackageReference(ctx, base, held, false);
  g_front_end_holds[p] = now;
  if (held != now) {
    REXLOG_INFO("Outfits: player {} stored skin {}: the front end now holds skin {}'s package (gave back {})", p + 1,
                skin, now, held == kNone ? std::string("nothing") : std::to_string(held));
  }
  ctx = call;
}
