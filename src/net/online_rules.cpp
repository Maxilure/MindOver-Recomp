// =============================================================================
// net/online_rules.cpp -- nothing pauses online (net/online_rules.h)
// =============================================================================

#include "online_rules.h"

#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../data/fight_tree.h"
#include "../guest_memory.h"
#include "../players/more_players.h"
#include "../ui/scrooby.h"
#include "net_session.h"

extern "C" REX_FUNC(__imp__sub_8227C8E8);  // the game manager's pause (r4 = on / off)
extern "C" REX_FUNC(sub_82266130);         // front end: input controller index of player r4 (-1 = none)
extern "C" REX_FUNC(__imp__sub_8225DE68);  // the question box's update (r3 = box, f1 = time)

REXCVAR_DEFINE_BOOL(debug_online_no_menu_lock, false, "CrashMoM",
                    "Debug: online, never lock a menu owner's controller (to test what the lock changes)");


namespace online_rules {
constexpr uint32_t kGameGlobalAddress = 0x8259B190;  // (the box wrapper below)
namespace {

constexpr uint32_t kGameGlobal = 0x8259B190;    // the game global (front end at +52, input manager at +56)
constexpr uint32_t kFrontEnd = 52, kInputManager = 56, kControllers = 8;
constexpr uint32_t kLockByte = 516;             // root input controller: lock count (top byte, > 0 = locked)
constexpr int kPlayers = 4;
constexpr int32_t kGameRunning = 489;  // front end: the level plays, no menu
constexpr uint32_t kGamePaused = 168;   // game: bit 0x20 = paused (sub_8227C8E8)
constexpr uint8_t kPausedBit = 0x20;

uint32_t Read32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, GuestPtr(base, a), 4);
  return __builtin_bswap32(v);
}

// Player p's root input controller (the one sub_82265A60 asks about), 0 = none.
uint32_t RootController(PPCContext& ctx, uint8_t* base, int p) {
  const uint32_t game = Read32(base, kGameGlobal);
  if (!game) return 0;
  const uint32_t fe = Read32(base, game + kFrontEnd), manager = Read32(base, game + kInputManager);
  if (!fe || !manager) return 0;
  const PPCContext saved = ctx;
  ctx.r3.u64 = fe;
  ctx.r4.u64 = uint32_t(p);
  sub_82266130(ctx, base);
  const int32_t index = int32_t(ctx.r3.u32);
  ctx = saved;
  if (index < 0 || index > 3) return 0;
  const uint32_t controllers = Read32(base, manager + kControllers);
  return controllers ? Read32(base, controllers + 4 * uint32_t(index)) : 0;
}

uint32_t g_locked[kPlayers] = {};  // (below)
uint32_t g_roots[kPlayers] = {};   // every player's root controller, refreshed per frame (OnFrame)

// The map's owner online (MenuOwner): set at the exit into the map.
constexpr int32_t kExitToMap = 502, kMapState = 927;
int g_map_owner = -1;  // the controller we added 1 to, per player (0 = none)

void Unlock(uint8_t* base, int p, const char* why) {
  if (!g_locked[p]) return;
  int8_t* lock = reinterpret_cast<int8_t*>(GuestPtr(base, g_locked[p] + kLockByte));
  if (*lock > 0) --*lock;
  REXLOG_INFO("Online: player {}'s controller unlocked ({})", p + 1, why);
  g_locked[p] = 0;
}

}  // namespace

void Init() {
  fight_tree::AddFrontEndExitListener([](PPCContext&, uint8_t*, int32_t from, int32_t exit) {
    if (from != kGameRunning || exit != kExitToMap || !net::InSession()) return;
    g_map_owner = -1;
    for (int p = 0; p < kPlayers && g_map_owner < 0; ++p) {
      uint8_t pad[12];
      if (net::FrameInput(p, pad) && (pad[1] & 0x20)) g_map_owner = p;  // XINPUT_GAMEPAD_BACK
    }
    REXLOG_INFO("Online: the map was opened by player {}", g_map_owner + 1);
  });
}

// The front end's frame sees "paused" while a menu is up (online_rules.h).
namespace {
uint8_t g_fe_paused_old = 0;
bool g_fe_paused_set = false;
}  // namespace

void BeforeFrontEndFrame(uint8_t* base) {
  g_fe_paused_set = false;
  if (!net::InSession() || !more_players::InPlay() ||
      fight_tree::CurrentFrontEnd().state == kGameRunning) {
    return;
  }
  const uint32_t game = Read32(base, kGameGlobal);
  if (!game) return;
  uint8_t* paused = GuestPtr(base, game + kGamePaused);
  g_fe_paused_old = *paused;
  *paused = uint8_t(*paused | kPausedBit);
  g_fe_paused_set = true;
}

void AfterFrontEndFrame(uint8_t* base) {
  if (!g_fe_paused_set) return;
  g_fe_paused_set = false;
  const uint32_t game = Read32(base, kGameGlobal);
  if (!game) return;
  uint8_t* paused = GuestPtr(base, game + kGamePaused);
  *paused = uint8_t((*paused & ~kPausedBit) | (g_fe_paused_old & kPausedBit));
}

// --- Only the owner sees their menu (online_rules.h) ------------------------
namespace {
// The pages every PC keeps drawing (the HUD), found by name once a second.
constexpr const char* kKeptPages[] = {
    "InGame",           "InGame_Announcement", "InGame_CoOp_Player1",       "InGame_CoOp_Player2",
    "InGame_CoOp_Player3", "InGame_CoOp_Player4", "InGame_CollectableCounter", "InGame_LetterBox",
    "InGame_MessageBar", "InGame_Tutorial",    "FE_TRG_Message_InGame"};
constexpr size_t kKeptCount = sizeof(kKeptPages) / sizeof(kKeptPages[0]);
uint32_t g_kept[kKeptCount] = {};
int g_view_owner = -1;     // whose menu is up now (OnFrame), -1 none
int g_kept_depth = -1;     // draw depth of the kept page being drawn, -1 none
int g_depth = 0;           // Parent draw nesting
uint64_t g_hidden_draws = 0;
uint32_t g_forced_visible = 0;   // a HUD page made visible for its draw (EndMenuDraw hides it again)
uint8_t* g_base_for_draw = nullptr;

bool Kept(uint32_t node) {
  for (uint32_t k : g_kept) {
    if (k && k == node) return true;
  }
  return false;
}
}  // namespace

bool HideMenuDraw(uint32_t node) {
  const bool hiding = net::InSession() && g_view_owner >= 0 && g_view_owner != net::LocalPlayer();
  if (hiding && g_kept_depth < 0 && g_depth == 0 && node && !Kept(node)) {
    // A top-level page that isn't the HUD: someone else's menu. (Pages are
    // drawn at the top; their groups / layers come nested below.)
    if (g_hidden_draws++ == 0) {
      REXLOG_INFO("Online: player {}'s menu is up: not drawn here (player {}'s PC)", g_view_owner + 1,
                  net::LocalPlayer() + 1);
    }
    return true;
  }
  if (g_kept_depth < 0 && Kept(node)) {
    g_kept_depth = g_depth;
    // The pause screen HIDES the HUD's pages (for everyone: shared state);
    // the players who keep playing still want theirs: visible for this draw
    // only, put back in EndMenuDraw.
    g_forced_visible = 0;
    if (hiding && !scrooby::Visible(g_base_for_draw, node)) {
      scrooby::SetVisible(g_base_for_draw, node, true);
      g_forced_visible = node;
    }
  }
  ++g_depth;
  return false;
}

void EndMenuDraw(uint32_t node) {
  if (g_depth > 0) --g_depth;
  if (g_kept_depth == g_depth) {
    g_kept_depth = -1;
    if (g_forced_visible && g_forced_visible == node) {
      scrooby::SetVisible(g_base_for_draw, node, false);
      g_forced_visible = 0;
    }
  }
}

int MenuOwner() {
  if (!net::InSession() || fight_tree::CurrentFrontEnd().state != kMapState) return -1;
  return g_map_owner;
}

void OnFrame(PPCContext& ctx, uint8_t* base) {
  if (!net::InSession()) {
    for (int p = 0; p < kPlayers; ++p) Unlock(base, p, "no session");
    return;
  }
  // A menu is up: the game's own flag (+8594 bit 0x80) is set by its PAUSE,
  // which never happens online, so also: in a level, the front end has left
  // GameRunning (489) for one of its in-game menus (pause 868 and the
  // screens behind it, the totem's prompts, ...). Both = world state, alike
  // on every PC.
  for (int p = 0; p < kPlayers; ++p) g_roots[p] = RootController(ctx, base, p);
  const bool menu = menu_input::MenuOnScreen() ||
                    (more_players::InPlay() && fight_tree::CurrentFrontEnd().state != kGameRunning);
  const int owner = menu ? menu_input::CurrentMenuOwner(ctx, base) : -1;
  // Whose menu is on the screen: drawn only on their PC (HideMenuDraw).
  g_view_owner = owner;
  g_base_for_draw = base;
  if (owner < 0) g_hidden_draws = 0;
  static int refind = 0;
  if (owner >= 0 && refind-- <= 0) {
    refind = 60;  // the HUD pages, by name, once a second while a menu is up
    for (size_t i = 0; i < kKeptCount; ++i) g_kept[i] = scrooby::FindPage(ctx, base, kKeptPages[i]);
  }
  for (int p = 0; p < kPlayers; ++p) {
    const uint32_t root = p == owner && !REXCVAR_GET(debug_online_no_menu_lock) ? RootController(ctx, base, p) : 0;
    if (g_locked[p] && g_locked[p] != root) Unlock(base, p, "menu closed");
    if (root && !g_locked[p]) {
      int8_t* lock = reinterpret_cast<int8_t*>(GuestPtr(base, root + kLockByte));
      if (*lock < 127) ++*lock;
      g_locked[p] = root;
      REXLOG_INFO("Online: player {}'s menu is open: their controller locked (the world plays on)", p + 1);
    }
  }
}

uint32_t RootOf(int p) { return p >= 0 && p < kPlayers ? g_roots[p] : 0; }

// Player p's root controller read straight from the tables, without calling
// the game (callable from mid-function hooks, which have no PPCContext):
// players 1-2's controller number = front end +8524 + 4p (what
// sub_82266130 reads), 3-4's = more_players' own table (sub_82266130's
// override); then the input manager's controller list.
uint32_t RootControllerNow(uint8_t* base, int p) {
  const uint32_t game = Read32(base, kGameGlobal);
  if (!game) return 0;
  const uint32_t fe = Read32(base, game + kFrontEnd), manager = Read32(base, game + kInputManager);
  if (!fe || !manager || p < 0 || p > 1 || !guest_memory::Readable(fe + 4 * 2131, 8) ||
      !guest_memory::Readable(manager + kControllers, 4)) {
    return 0;
  }
  const int32_t index = int32_t(Read32(base, fe + 4 * (2131 + uint32_t(p))));
  if (index < 0 || index > 3) return 0;
  const uint32_t controllers = Read32(base, manager + kControllers);
  return controllers ? Read32(base, controllers + 4 * uint32_t(index)) : 0;
}

// FRESH LOOKUP (2026-11-17, own-camera round 10): g_roots is refreshed once
// per front-end frame, but a joining player's controller is made and used
// (the hold logic sub_822718E8 runs) before that refresh: the lookup said
// "nobody", the online hooks stood aside and the controller's held forward
// started as each PC's own camera direction (fingerprint: player 2's held
// forward differed from the join on, with a tilt no sent facing has). Now a
// controller the cache doesn't know is checked against the tables as they
// are right now.
int PlayerOfController(uint8_t* base, uint32_t controller) {
  constexpr uint32_t kParent = 36;  // input controller: its parent (0 = a root)
  for (int depth = 0; controller && depth < 8; ++depth) {
    for (int p = 0; p < kPlayers; ++p) {
      if (g_roots[p] && g_roots[p] == controller) return p;
    }
    for (int p = 0; p < 2; ++p) {
      if (RootControllerNow(base, p) == controller) return p;
    }
    if (!guest_memory::Readable(controller + kParent, 4)) return -1;
    controller = Read32(base, controller + kParent);
  }
  return -1;
}

}  // namespace online_rules

// The game manager's pause (online_rules.h): online, the world never stops.
// (The original's code stays as __imp__sub_8227C8E8 for offline play and for
// the port's own callers.)
extern "C" REX_FUNC(sub_8227C8E8) {
  if (net::InSession()) {
    static bool told[2] = {};
    const bool on = (ctx.r4.u32 & 0xFF) != 0;
    if (!told[on]) {
      told[on] = true;
      REXLOG_INFO("Online: the game asked to {} the world: not online (logged once)", on ? "pause" : "unpause");
    }
    return;
  }
  __imp__sub_8227C8E8(ctx, base);
}

// The question box's own update sees "paused" too, ALWAYS online (not only
// while the front end is out of GameRunning): a box opened by a script in the
// level (the totem's question) runs its first update in the same frame, while
// the front end still reads GameRunning, and closed itself right there
// (BeforeFrontEndFrame hadn't set the bit yet). See online_rules.h.
extern "C" REX_FUNC(sub_8225DE68) {
  uint32_t game = 0;
  if (net::InSession()) {
    std::memcpy(&game, GuestPtr(base, online_rules::kGameGlobalAddress), 4);
    game = __builtin_bswap32(game);
  }
  if (!game) {
    __imp__sub_8225DE68(ctx, base);
    return;
  }
  uint8_t* paused = GuestPtr(base, game + 168);
  const uint8_t old = *paused;
  *paused = uint8_t(old | 0x20);
  __imp__sub_8225DE68(ctx, base);
  *paused = uint8_t((*paused & ~0x20) | (old & 0x20));
}

// ZONES RUN BY THE PLAYERS, NOT BY A CAMERA (2026-11-17, found by the own-
// camera experiment: player 2 bumped into something on one PC only).
// sub_822EFFD8 decides each frame whether a zone RUNS (its objects update):
// always-on flag (+681 0x20), or loaded (+596 == 2) and either this test or
// "the focus player's zone" (sub_822E7A70); the co-op camera code adds "any
// player stands in it" (coop_camera.cpp CoopCameraZoneActive, 0x822F003C).
// THIS test, sub_822F4AA0, asks whether the GAME CAMERA's position
// (VectorCamera +216..+224) is inside the zone: its bounding sphere (+488..
// +500, radius squared) and then its walls (planes at *(+376)); it also sets
// a sticky bit (+681 0x40) in the zone. With a camera per PC each PC would
// run a different set of zones: objects in a zone only one PC's camera is in
// move there and stand still on the other. Online the answer is "no" (and the
// zone isn't touched): a zone runs when it's always on, the focus player's,
// or holds a player, the same on every PC. (A zone only the camera looks into
// stands still until a player walks in: watch for that in play.) Offline:
// the original.
extern "C" REX_FUNC(__imp__sub_822F4AA0);
extern "C" REX_FUNC(sub_822F4AA0) {
  if (net::InSession()) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_822F4AA0(ctx, base);
}

// "AM I OFF SCREEN?" FOR THE FIGHT TREES (2026-11-17, own-camera round 7:
// player 2's turn started one frame earlier on the PC whose camera showed
// them; on the other PC they were off screen). sub_821BA090 (r3 = the
// asking object, its actor at +156) = "the actor's position is NOT in the
// game camera's view" (camera virtual slot 17), the fight-tree condition
// registered beside "IsOffScreenInCoOpMode" (0x821BA54C). Like the co-op
// off-screen test sub_82235978 (players/more_players.cpp): online every
// Crash counts as on screen, the same on every PC.
extern "C" REX_FUNC(__imp__sub_821BA090);
extern "C" REX_FUNC(sub_821BA090) {
  if (net::InSession()) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_821BA090(ctx, base);
}

// ENEMIES CIRCLING A PLAYER ASK "AM I ON SCREEN?" (2026-11-17, own-camera
// callers pass: every game function that asks the game camera's slot 17
// "is this point in view" is one of four: the fight-tree condition above,
// the co-op catch-up (sub_82235978), the camera's focus list (sub_821099C0,
// view side) and this one). sub_820BE878 = a step of CAITaskCombatMoveEncircle
// (r3 = the task, its actor at +152): it asks the camera about the enemy's
// own position (actor +48 matrix row 3) and sets the task's +16 from its +12,
// one more when the enemy is OFF screen. With a camera per PC that number
// (and with it how the enemy moves around the player) would differ per PC.
// Online every enemy counts as on screen (the original's answer while the
// single camera sees it): +16 = +12, r3 = 1 like the camera's "yes".
extern "C" REX_FUNC(__imp__sub_820BE878);
extern "C" REX_FUNC(sub_820BE878) {
  if (net::InSession()) {
    const uint32_t task = ctx.r3.u32;
    if (guest_memory::Readable(task + 12, 8)) {
      std::memcpy(GuestPtr(base, task + 16), GuestPtr(base, task + 12), 4);
    }
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_820BE878(ctx, base);
}

// PROPS ANIMATE WHETHER OR NOT THIS PC SEES THEM (2026-11-17, own-camera
// round 9: a breakable broke one frame later on the PC whose camera didn't
// show it, and player 2's targeting let go of it a frame later there).
// CRenderObject3D's update (slot 5, sub_822B6608, f1 = time step) has an
// "animate only when drawn" rule: with its flag +404 bit 0x40 set and the
// object NOT drawn lately (its draw stamp, sub_8236ABC0(+8), against the
// render frame counter **0x82571B88, low two bits), it only adds the time to
// +400 and returns; the next time it is drawn it advances by all of it. Its
// animation is a StateProp whose frames fire events (a breakable's "break":
// call chain 822B6720 -> 82380CD0 -> 8237F638 -> 8218CEB0 -> 821959AC), so
// WHEN an event fires depended on what this PC's camera drew. Online the flag
// is cleared for the call (and put back after): every object advances every
// frame, on every PC alike. Offline: the original (saves the work for objects
// nobody sees).
extern "C" REX_FUNC(__imp__sub_822B6608);
extern "C" REX_FUNC(sub_822B6608) {
  if (!net::InSession()) {
    __imp__sub_822B6608(ctx, base);
    return;
  }
  uint8_t* flags = GuestPtr(base, ctx.r3.u32 + 404);
  const uint8_t before = *flags;
  *flags = uint8_t(before & ~0x40);
  __imp__sub_822B6608(ctx, base);
  *flags = uint8_t((*flags & ~0x40) | (before & 0x40));
}

// SOMEONE ELSE'S MOJO DOESN'T FLY ON THIS PC (2026-11-19, a design decision
// for online play). A collected mojo (Collectable update sub_82198738) first
// flies into its collector (state 4, a world path: the same on every PC),
// then to the collector's HUD portrait (state 5): every frame it is placed
// 4 units in front of the GAME CAMERA, toward the portrait's corner, and
// shrunk by f10 = 1 - 0.5 x progress (0x82198F7C, progress = sqrt(2 x time),
// done at 1: the HUD count and pulse, sub_8226AB78). Online each PC has its
// own camera and shows only its own player's HUD view, so on player 1's PC a
// mojo player 2 collected flew across player 1's screen toward a corner that
// isn't player 2's. Online, a HUD flight whose collector (r25, the player
// from sub_8226FCD8) isn't this PC's player is drawn at 1/1000 of its size:
// out of sight (a few units from the camera that is below a pixel). Not 0: a
// zero matrix could make a NaN somewhere that reads the actor's size. The
// flight's time, its end and the count are untouched (they never read f10),
// so both PCs still count the mojo on the same frame; the actor's position
// already differed between PCs (own cameras) and nothing in the world reads
// it (checked with the per-frame fingerprints, findings/68). Types (r23,
// collectable +0xB4 +0x20) 3, 4, 7, 8 don't aim at the HUD (path 0x82198F70,
// a point from the stack): left alone. Found by reading the state-5 code
// (xexdis 0x82198D6C-0x82199000). Offline (one shared screen): the original.
void OnlineMojoFlightHidden(PPCRegister& r23, PPCRegister& r25, PPCRegister& f10) {
  if (!net::InSession()) return;
  const int local = net::LocalPlayer();
  const int32_t collector = int32_t(r25.u32);
  const int32_t type = int32_t(r23.u32);
  if (local < 0 || collector < 0 || collector == local) return;
  if (type == 3 || type == 4 || type == 7 || type == 8) return;
  static uint64_t hidden = 0;
  if (hidden++ % 600 == 0) {
    REXLOG_INFO("Online: player {}'s mojo flies to their HUD: not shown on player {}'s PC ({} frames so far)",
                collector + 1, local + 1, hidden);
  }
  f10.f64 = 0.001 * f10.f64;
}
