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
//    players 3-4 get ours; an outfit picked at the chest (flag
//    outfit_player<N> >= 0) wins over both. Players 3-4's spawn events are
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
#include "../options/options_settings.h"
#include "../pddi/intercept.h"
#include "../ui/scrooby.h"
#include "more_players.h"

REXCVAR_DEFINE_BOOL(carbon_colours, true, "CrashMoM",
                    "Crash in colours at the chest (red, yellow, green, blue, purple, pink: players 3-4 start "
                    "green and purple; player 2 stays the original white Carbon Crash); false = only the "
                    "white Carbon Crash");
REXCVAR_DEFINE_INT32(outfit_player1, -1, "CrashMoM",
                     "Player 1's outfit (picked at the chest in Crash's house): -1 as the game decides, "
                     "0 Crash, 1 Greeny, 2 Bluey, 3 Reddy, 4 Snipe, 5 Ratcicle, 6 Magmadon, 7 Skeleton, "
                     "8 Spike, 9 Coco, 10 Old School, 11 Carbon Crash, 12-17 Crash in red / blue / green / "
                     "purple / yellow / pink")
    .range(-1, 17);
REXCVAR_DEFINE_INT32(outfit_player2, -1, "CrashMoM", "Player 2's outfit (see outfit_player1)")
    .range(-1, 17);
REXCVAR_DEFINE_INT32(outfit_player3, -1, "CrashMoM", "Player 3's outfit (see outfit_player1)")
    .range(-1, 17);
REXCVAR_DEFINE_INT32(outfit_player4, -1, "CrashMoM", "Player 4's outfit (see outfit_player1)")
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
extern "C" REX_FUNC(__imp__sub_822E7880);  // the inventory's per-change update
extern "C" REX_FUNC(__imp__sub_8236ACB8);
extern "C" REX_FUNC(__imp__sub_820D8908);  // a section's texture by name key (r3 section, r4 64-bit key)
extern "C" REX_FUNC(__imp__sub_822B6FB8);  // CCharacterObject: draws its model (r3 the object)
extern "C" REX_FUNC(__imp__sub_822B6DB0);  // CCharacterObject slot 11: draws its +28 drawable  // name key from a string (r3 key, r4 string)

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

const char* FlagName(int p) {
  static const char* names[kMaxPlayers] = {"outfit_player1", "outfit_player2", "outfit_player3",
                                           "outfit_player4"};
  return names[p];
}
// The game's number for an outfit (Carbon = Crash's model).
uint32_t GameSkin(int outfit) { return IsCarbon(outfit) ? 0u : uint32_t(outfit); }
bool IsCoco(uint32_t skin) { return skin == 9 || skin == 10; }
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
// The outfit a player picked at the chest (settings.toml), -1 = none. The
// pick is kept for every save, but it only counts while the loaded save has
// it unlocked: a pick of Coco made in a late save, then a New Game or an
// early save, played Coco before the story gives her (2026-10-17). There the
// player wears what the save says; the pick comes back in saves that have it.
int Picked(int p) {
  int picked = -1;
  switch (p) {
    case 0: picked = REXCVAR_GET(outfit_player1); break;
    case 1: picked = REXCVAR_GET(outfit_player2); break;
    case 2: picked = REXCVAR_GET(outfit_player3); break;
    case 3: picked = REXCVAR_GET(outfit_player4); break;
    default: return -1;
  }
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

int OutfitOf(int p) {
  if (p < 0 || p >= kMaxPlayers) return 0;
  if (p == g_wearing_player) return g_wearing_outfit;
  const int picked = Picked(p);
  if (picked >= 0) return picked;
  // As the game decides: player 1 = its skin; others: Crash's model =
  // Carbon (player 2 the original white); 3-4, the port's, a coloured Crash
  // in their marker's colour (green, purple).
  uint32_t skin = p >= 2 ? g_game_skins[p - 2] : Read32(0x8259B1C0 + 4 * p);
  if (skin > 10) skin = 0;
  if (p == 0 || skin != 0) return int(skin);
  if (p >= 2 && REXCVAR_GET(carbon_colours) && g_colours_added) return p == 2 ? kCrashGreen : kCrashPurple;
  return kCarbon;
}

}  // namespace outfits

using namespace outfits;

// --- 1. Who wears what --------------------------------------------------------

extern "C" REX_FUNC(sub_8229F8B8) {
  const uint32_t lr = uint32_t(ctx.lr);
  int p = int(ctx.r3.u32);
  if (lr == kAskOpening || lr == kAskPageTurn) {
    ctx.r3.u64 = uint32_t(OutfitOf(g_page_player));  // the screen sees Carbon as 11
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
  }
}

extern "C" REX_FUNC(sub_8229F8A0) {
  const int p = int(ctx.r3.u32);
  if (p == g_applying) {
    // A pick at the chest: kept as the player's outfit (settings.toml).
    options_settings::SetFlagToSave(FlagName(p), std::to_string(g_applying_outfit));
    options_settings::SaveChanged();
    REXLOG_INFO("Outfits: player {} wears outfit {}", p + 1, g_applying_outfit);
  }
  if (p >= 2 && p < kMaxPlayers) {
    g_game_skins[p - 2] = ctx.r4.u32;
    return;
  }
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
  // A floating button's coin: its owner's B (input/prompt_icons.h).
  if (!g_hair_from) {
    uint32_t from = 0, to = 0;
    if (prompt_icons::WorldButtonSwap(base, object, from, to)) g_hair_from = from, g_hair_to = to;
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
  __imp__sub_820E0858(ctx, base);
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
  const uint32_t shown = Read32(kSelection);
  r5.u64 = IsCoco(GameSkin(int(std::min<uint32_t>(shown, uint32_t(kLastOutfit))))) ? kPreviewCocoMoves
                                                                               : kPreviewCrashMoves;
  r6.u64 = IsCarbon(int(shown)) ? 1u : 0u;
}
extern "C" REX_FUNC(sub_820E0C98) {
  const uint32_t screen = ctx.r3.u32;
  const uint32_t page = Read32(screen + kScreenPage);
  const uint32_t shown = Read32(kSelection);
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
    CallGame(__imp__sub_822665B0, ctx, base, front_end, uint32_t(p), new_skin);
    g_applying = -1;
    g_wearing_player = -1;
    REXLOG_INFO("Outfits: player {} picked outfit {} (game skin {} -> {})", p + 1, outfit, old_skin,
                new_skin);
  }
  CallGame(__imp__sub_822E7880, ctx, base, Read32(game + kInventoryOf));
}
