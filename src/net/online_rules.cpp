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
