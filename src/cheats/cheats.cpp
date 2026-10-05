// =============================================================================
// cheats/cheats.cpp -- see cheats.h (what each cheat does and how we found it:
// docs/findings/27-cheat-menu.md)
// =============================================================================
#include "cheats.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <map>
#include <set>

#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../players/more_players.h"
#include "free_camera.h"
#include "spawn.h"

namespace cheats {
namespace {

// --- Game addresses (findings/27) --------------------------------------------

// The game manager ("CUberManager"; the scripts' GetGameState, HideHud, ...):
// *kUberGlobal. Its byte +168 holds the pause bits: 0x04 = paused for
// screenshots (frozen, see cheats.h), 0x02 = run one step, 0x10 / 0x20 = the
// game's own pause states (pause menu, sound paused).
constexpr uint32_t kUberGlobal = 0x8259B190;
constexpr uint32_t kUberFlags = 168;
constexpr uint8_t kFlagStep = 0x02, kFlagFrozen = 0x04;
// The stats manager = *(uber + 96); EStatInt id N at +420 + 4 N.
constexpr uint32_t kUberStats = 96;
constexpr uint32_t kStatInts = 420;
constexpr int kStatTotalMojo = 3;        // EStatInt_MojoCount
constexpr int kStatCrashMojo = 4;        // EStatInt_MojoCountCrashUpgrade
constexpr int kStatCrashLevel = 102;     // EStatInt_CrashUpgradeLevel
constexpr int kStatIntCount = 183;
// What Crash's level script waits for: the threshold of the last checked
// "WAIT_CrashMojoUpgradeValue" requirement (sub_822A1458 stores it here; the
// previous one at +4). Stat 4 >= this = the script's next upgrade happens.
constexpr uint32_t kCrashNextPrice = 0x8259B1CC;

// Each player's co-op state (ECoOpPlayerState; more_players grew the table
// to 4 in place): 0 not joined, 1 mask, 2 in the game on foot / in a titan,
// 3 entering the mask (findings/26).
constexpr uint32_t kCoOpState = 0x8259B11C;
constexpr uint32_t kStateNotJoined = 0, kStateMask = 1, kStateEnteringMask = 3;

// Actor member (a titan's actor): its CUpgradeableBehaviour*. (Each actor's
// damageable is found through its update instead: Crash keeps the pointer at
// +0x12C, titans elsewhere.)
constexpr uint32_t kActorUpgradeable = 0x180;

// CUpgradeableBehaviour (vtable 0x82032344, constructor sub_82177888).
constexpr uint32_t kUpgradeableVtable = 0x82032344;
constexpr uint32_t kUpgSteps = 56;      // 5 x 16 bytes: mojo price, type, value, done byte (+12)
constexpr uint32_t kUpgMojoStat = 44;   // EStatInt id of its mojo count
constexpr uint32_t kUpgLevelStat = 48;  // EStatInt id of its level (105 = never upgrades)
constexpr uint32_t kUpgStep = 140;      // the step it is on (0-4)
constexpr uint32_t kUpgNextPrice = 144; // that step's price
constexpr int kUpgStepCount = 5;

// CDamageableBehaviour (vtable 0x8202D2E4): floats behind two pointers.
constexpr uint32_t kDamageableVtable = 0x8202D2E4;
constexpr uint32_t kDmgHitpoints = 64, kDmgMaxHitpoints = 68;

// The frame function's call of CTimeManager::GetScale returns here
// (sub_8227C5D8, `bl 0x822FFA40` at 0x8227C698): our once-per-frame moment.
constexpr uint32_t kFrameFunctionScaleReturn = 0x8227C69C;

// --- Guest memory -------------------------------------------------------------

uint8_t* Guest(uint32_t address) {
  return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(address);
}
uint32_t Read32(uint32_t address) {
  const uint8_t* p = Guest(address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Write32(uint32_t address, uint32_t value) {
  uint8_t* p = Guest(address);
  p[0] = uint8_t(value >> 24); p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8); p[3] = uint8_t(value);
}
float ReadFloat(uint32_t address) {
  const uint32_t bits = Read32(address);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

// Is [address, address + size) committed, readable guest memory? Pointers we
// follow out of objects are checked first: a stray read of an unmapped page
// kills the game (the SDK's fault handler can't recover it).
bool Readable(uint32_t address, uint32_t size) {
  if (address < 0x1000) return false;
  auto* memory = rex::system::kernel_memory();
  auto* heap = memory->LookupHeap(address);
  return heap && heap->QueryRangeAccess(address, address + size - 1) != rex::memory::PageAccess::kNoAccess;
}

// The object at `address` if it is readable and has vtable `vtable`, else 0.
uint32_t ObjectWithVtable(uint32_t address, uint32_t vtable, uint32_t size) {
  return address && Readable(address, size) && Read32(address) == vtable ? address : 0;
}

uint32_t StatsManager() {
  const uint32_t uber = Read32(kUberGlobal);
  return uber ? Read32(uber + kUberStats) : 0;
}
int ReadStat(uint32_t stats, int id) {
  return id >= 0 && id < kStatIntCount ? int32_t(Read32(stats + kStatInts + 4 * id)) : 0;
}
void WriteStat(uint32_t stats, int id, int value) {
  if (id >= 0 && id < kStatIntCount) Write32(stats + kStatInts + 4 * id, uint32_t(value));
}

// A titan's name from its level stat id (the EStatInt_*UpgradeLevel names:
// the game's internal names, which aren't always the names players know).
std::string TitanName(int level_stat) {
  switch (level_stat) {
    case 103: return "Battler";
    case 104: return "Crunch (boss)";
    case 105: return "Cortex (boss)";
    case 106: return "Grizzly";
    case 107: return "Parafox";
    case 108: return "Phantom";
    case 109: return "Ratcicle";
    case 110: return "Roller";
    case 111: return "Scorporilla";
    case 112: return "Shellephant";
    case 113: return "Shurtle";
    case 114: return "Sludge";
    case 115: return "Spike";
    case 116: return "Stinky";
    case 117: return "TK";
    case 118: return "Yuktopus";
    case 134: return "Parafox (hero)";
    case 135: return "Phantom (hero)";
    case 136: return "Ratcicle (hero)";
    case 137: return "Shurtle (hero)";
    case 138: return "Sludge (hero)";
    case 139: return "Spike (hero)";
    default: return "Titan";
  }
}

// --- State --------------------------------------------------------------------

struct Requests {
  int level_up[kPlayers] = {};   // how many single level ups are waiting
  bool max_level[kPlayers] = {};
  int add_mojo[kPlayers] = {};
  bool refill[kPlayers] = {};
  bool kill[kPlayers] = {};
  float hurt[kPlayers] = {};  // debug FIFO only: damage through the game's own path
  bool free_jack[kPlayers] = {};
  bool step = false;
};

std::mutex g_mutex;          // guards g_requests and g_snapshot
Requests g_requests;
Snapshot g_snapshot;
std::atomic<bool> g_god{false};
std::atomic<bool> g_frozen{false};
std::atomic<float> g_speed{1.0f};
std::atomic<bool> g_hud_hidden{false};
// A cheat that changes progress was used this session: achievements stay
// locked from then on (see the achievement hook at the end).
std::atomic<bool> g_cheated{false};
void MarkCheated() {
  if (!g_cheated.exchange(true)) {
    REXLOG_INFO("Cheats: progress cheat used: achievements are off until the game restarts");
  }
}

// Damageables that belonged to a player's character at their last update
// (game thread only). God mode refuses them any loss.
std::set<uint32_t> g_player_damageables;
// Damageables just killed by the cheat -> frames left during which god mode
// leaves them alone (else its refill, the next frame, would undo the kill).
std::map<uint32_t, int> g_killed;
// Actor -> its damageable, from the damageables' per-frame updates (game
// thread only): filled during a frame, handed over at the next frame's start
// (so it never holds actors that are gone).
std::map<uint32_t, uint32_t> g_damageable_filling, g_damageable_of;

// Calls a game function from inside one of our hooks (game's main thread),
// with r3-r6 / f1 set; our hook's registers are put back afterwards. Returns r3.
using GuestFunction = void (*)(PPCContext&, uint8_t*);
uint32_t CallGame(GuestFunction function, PPCContext& ctx, uint8_t* base, uint32_t r3,
                  uint32_t r4 = 0, uint32_t r5 = 0, uint32_t r6 = 0, double f1 = 0.0) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  ctx.r6.u64 = r6;
  ctx.f1.f64 = f1;
  function(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

// Is `actor` one of the players' characters or jacked titans?
bool IsPlayerBody(uint32_t actor) {
  if (!actor) return false;
  for (int p = 0; p < kPlayers; ++p) {
    if (actor == more_players::CharacterOfPlayer(p) || actor == more_players::TitanOfPlayer(p)) {
      return true;
    }
  }
  return false;
}

}  // namespace
}  // namespace cheats

// The game functions we call (generated; __imp__ = the original even when we
// override the name).
extern "C" REX_FUNC(__imp__sub_82177D38);  // CUpgradeableBehaviour: buy the steps its mojo covers
extern "C" REX_FUNC(__imp__sub_821349E0);  // CDamageableBehaviour: change hitpoints by f1
extern "C" REX_FUNC(__imp__sub_822FFA40);  // CTimeManager::GetScale
extern "C" REX_FUNC(__imp__sub_82132F88);  // CDamageableBehaviour::Update (slot 4)
extern "C" REX_FUNC(sub_821349E0);         // the same, through our god-mode wrapper (FIFO "hurt")
extern "C" REX_FUNC(__imp__sub_821AE998);  // Crash (controller): start the "free jack" power-up (r4 = actor)

namespace cheats {
namespace {

// --- The work, on the game's main thread --------------------------------------

// Player p's titan's upgrade object (0 when on foot / none).
uint32_t UpgradeableOf(uint32_t titan) {
  if (!titan || !Readable(titan + kActorUpgradeable, 4)) return 0;
  return ObjectWithVtable(Read32(titan + kActorUpgradeable), kUpgradeableVtable, 160);
}
uint32_t DamageableOf(uint32_t actor) {
  const auto it = g_damageable_of.find(actor);
  return it == g_damageable_of.end() ? 0 : ObjectWithVtable(it->second, kDamageableVtable, 80);
}
bool StepDone(uint32_t upg, int step) {
  return *Guest(upg + kUpgSteps + 16 * uint32_t(step) + 12) != 0;
}

uint32_t CoOpState(int p) { return Read32(kCoOpState + 4 * uint32_t(p)); }
bool IsMask(int p) {
  const uint32_t state = CoOpState(p);
  return state == kStateMask || state == kStateEnteringMask;
}

// Player p's body for the cheats: its titan, else its Crash; 0 when the
// player isn't in or rides as a mask (a mask's Crash is hidden: its health
// isn't the player's).
uint32_t BodyOf(int p, uint32_t* titan_out) {
  const uint32_t titan = more_players::TitanOfPlayer(p);
  if (titan_out) *titan_out = titan;
  if (CoOpState(p) == kStateNotJoined || IsMask(p)) return 0;
  return titan ? titan : more_players::CharacterOfPlayer(p);
}

// Fills one player's entry of the snapshot.
PlayerInfo Describe(uint32_t stats, int p) {
  PlayerInfo info;
  if (CoOpState(p) == kStateNotJoined) return info;
  info.present = true;
  if (IsMask(p)) {
    // Upgrades are shared: a mask player's cheats act on Crash's.
    info.mask = true;
    info.name = "Mask";
    info.level = ReadStat(stats, kStatCrashLevel);
    info.mojo = ReadStat(stats, kStatCrashMojo);
    info.next_price = int32_t(Read32(kCrashNextPrice));
    return info;
  }
  uint32_t titan = 0;
  const uint32_t body = BodyOf(p, &titan);
  if (!body) return info;
  info.titan = titan != 0;
  if (const uint32_t upg = titan ? UpgradeableOf(titan) : 0) {
    const int level_stat = int32_t(Read32(upg + kUpgLevelStat));
    const int step = std::clamp(int32_t(Read32(upg + kUpgStep)), 0, kUpgStepCount - 1);
    info.name = TitanName(level_stat);
    info.level = ReadStat(stats, level_stat);
    info.max_level = kUpgStepCount;
    info.mojo = ReadStat(stats, int32_t(Read32(upg + kUpgMojoStat)));
    info.fully_upgraded = StepDone(upg, step);
    info.next_price = info.fully_upgraded ? 0 : int32_t(Read32(upg + kUpgNextPrice));
  } else if (titan) {
    info.name = "Titan (no upgrades)";
  } else {
    info.name = "on foot";  // (no character name: players look different, findings/27)
    info.level = ReadStat(stats, kStatCrashLevel);
    info.mojo = ReadStat(stats, kStatCrashMojo);
    info.next_price = int32_t(Read32(kCrashNextPrice));
  }
  if (const uint32_t dmg = DamageableOf(body)) {
    const uint32_t hp = Read32(dmg + kDmgHitpoints), max = Read32(dmg + kDmgMaxHitpoints);
    if (Readable(hp, 4) && Readable(max, 4)) {
      info.hitpoints = ReadFloat(hp);
      info.max_hitpoints = ReadFloat(max);
    }
  }
  return info;
}

// One titan level up: its mojo stat topped up to the next price, then the
// game's own routine buys the step (show = the upgrade screen, as after a
// pickup). Returns false when there's nothing left to buy.
bool LevelUpTitan(PPCContext& ctx, uint8_t* base, uint32_t stats, uint32_t titan, bool show) {
  const uint32_t upg = UpgradeableOf(titan);
  if (!upg) return false;
  const int step = std::clamp(int32_t(Read32(upg + kUpgStep)), 0, kUpgStepCount - 1);
  if (StepDone(upg, step)) return false;
  const int mojo_stat = int32_t(Read32(upg + kUpgMojoStat));
  const int price = int32_t(Read32(upg + kUpgNextPrice));
  const int have = ReadStat(stats, mojo_stat);
  if (have < price) {
    WriteStat(stats, mojo_stat, price);
    WriteStat(stats, kStatTotalMojo, ReadStat(stats, kStatTotalMojo) + (price - have));
  }
  CallGame(__imp__sub_82177D38, ctx, base, upg, titan, 0, show ? 1 : 0);
  return true;
}

// Crash: stat 4 up to what his level script waits for (it does the rest).
// Returns false when the script waits for nothing new (already there).
bool LevelUpCrash(uint32_t stats) {
  const int price = int32_t(Read32(kCrashNextPrice));
  const int have = ReadStat(stats, kStatCrashMojo);
  if (price <= 0 || have >= price) return false;
  WriteStat(stats, kStatCrashMojo, price);
  WriteStat(stats, kStatTotalMojo, ReadStat(stats, kStatTotalMojo) + (price - have));
  return true;
}

// Mojo as if picked up: the body's own count (Crash's or the titan's) and
// the total; for a titan its upgrade check runs like after a pickup.
void AddMojo(PPCContext& ctx, uint8_t* base, uint32_t stats, uint32_t titan, int amount) {
  WriteStat(stats, kStatTotalMojo, ReadStat(stats, kStatTotalMojo) + amount);
  if (const uint32_t upg = UpgradeableOf(titan)) {
    const int mojo_stat = int32_t(Read32(upg + kUpgMojoStat));
    WriteStat(stats, mojo_stat, ReadStat(stats, mojo_stat) + amount);
    CallGame(__imp__sub_82177D38, ctx, base, upg, titan, 0, 1);
  } else if (!titan) {
    WriteStat(stats, kStatCrashMojo, ReadStat(stats, kStatCrashMojo) + amount);
  }
}

void RefillDamageable(PPCContext& ctx, uint8_t* base, uint32_t dmg) {
  const uint32_t hp = Read32(dmg + kDmgHitpoints), max = Read32(dmg + kDmgMaxHitpoints);
  if (!Readable(hp, 4) || !Readable(max, 4)) return;
  const float missing = ReadFloat(max) - ReadFloat(hp);
  if (missing > 0.0f) {
    CallGame(__imp__sub_821349E0, ctx, base, dmg, 0, 0, 0, double(missing));
  }
}
void Refill(PPCContext& ctx, uint8_t* base, uint32_t body) {
  if (const uint32_t dmg = DamageableOf(body)) RefillDamageable(ctx, base, dmg);
}

// KILL: hitpoints to 0 through the damage function's original (god mode's
// wrapper doesn't see it); the game does the rest as for any death. Tested
// 2026-10-05: on foot Crash plays his death, the screen fades, he comes back
// at the checkpoint with full health; in a titan the titan dies the game's way
// (it collapses, Crash is thrown out, mojo drops) and Crash goes on on foot (a
// second kill then kills him). God mode keeps its hands off for 3 s.
void Kill(PPCContext& ctx, uint8_t* base, int p, uint32_t body, bool titan) {
  const uint32_t dmg = DamageableOf(body);
  if (!dmg) {
    REXLOG_INFO("Cheats: player {} kill: no health found", p + 1);
    return;
  }
  const uint32_t hp = Read32(dmg + kDmgHitpoints), max = Read32(dmg + kDmgMaxHitpoints);
  if (!Readable(hp, 4) || !Readable(max, 4)) return;
  g_killed[dmg] = 180;
  CallGame(__imp__sub_821349E0, ctx, base, dmg, 0, 0, 0,
           -double(ReadFloat(hp) + ReadFloat(max) + 1.0f));
  REXLOG_INFO("Cheats: player {} killed ({})", p + 1, titan ? "their titan" : "on foot");
}

// FREE JACK: the game's power-up (the "Free jack" pickup, c_freeJack): the
// player may jack a titan without beating it first. The pickup (Crash's
// message handler sub_821ACAD8, at 0x821ACF30) calls sub_821AE998(Crash's
// controller, actor): it starts the HLFreeJack effect and sets the timer at
// controller +304 to ~2e31 s, i.e. until a jack uses it up (sub_821AEA70 ends
// it). The controller is the actor's +0x1C, class "Crash" (vtable 0x82035294).
constexpr uint32_t kActorController = 0x1C;
constexpr uint32_t kCrashControllerVtable = 0x82035294;
void GiveFreeJack(PPCContext& ctx, uint8_t* base, int p, uint32_t titan) {
  const uint32_t crash = more_players::CharacterOfPlayer(p);
  if (titan || !crash || CoOpState(p) != 2 || !Readable(crash + kActorController, 4)) {
    REXLOG_INFO("Cheats: player {} free jack: only on foot", p + 1);
    return;
  }
  const uint32_t controller = ObjectWithVtable(Read32(crash + kActorController), kCrashControllerVtable, 320);
  if (!controller) {
    REXLOG_INFO("Cheats: player {} free jack: no Crash controller found", p + 1);
    return;
  }
  CallGame(__imp__sub_821AE998, ctx, base, controller, crash);
  REXLOG_INFO("Cheats: player {} free jack on", p + 1);
}

// Crash's "max level": one level up per frame until his script stops asking
// for more (its next requirement appears a frame or more after the last).
int g_crash_max_frames[kPlayers] = {};

// Once per game frame (see the GetScale hook below).
void Tick(PPCContext& ctx, uint8_t* base) {
  const uint32_t uber = Read32(kUberGlobal);
  if (!uber) return;
  // Last frame's damageable updates -> who has which. A frame without any
  // (the game paused, e.g. behind a "Level Up!" screen) keeps the last list.
  if (!g_damageable_filling.empty()) {
    g_damageable_of.swap(g_damageable_filling);
    g_damageable_filling.clear();
  }

  // Freeze: keep the game's bit in step with our switch.
  uint8_t& flags = *Guest(uber + kUberFlags);
  const bool frozen = g_frozen.load();
  if (bool(flags & kFlagFrozen) != frozen) {
    flags = frozen ? (flags | kFlagFrozen) : (flags & ~(kFlagFrozen | kFlagStep));
  }

  Requests requests;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    requests = g_requests;
    g_requests = Requests{};
  }
  const bool in_level = more_players::InPlay();
  const uint32_t stats = StatsManager();

  if (requests.step && frozen) flags |= kFlagStep;

  if (in_level && stats) {
    for (int p = 0; p < kPlayers; ++p) {
      if (CoOpState(p) == kStateNotJoined) {
        g_crash_max_frames[p] = 0;
        continue;
      }
      // A mask has no body: its level cheats go to Crash's (shared) upgrades.
      uint32_t titan = 0;
      const uint32_t body = BodyOf(p, &titan);
      if (!body) titan = 0;
      if (requests.add_mojo[p] > 0) {
        AddMojo(ctx, base, stats, titan, requests.add_mojo[p]);
        REXLOG_INFO("Cheats: player {} +{} mojo", p + 1, requests.add_mojo[p]);
      }
      for (int i = 0; i < requests.level_up[p]; ++i) {
        const bool done = titan ? LevelUpTitan(ctx, base, stats, titan, true) : LevelUpCrash(stats);
        REXLOG_INFO("Cheats: player {} level up ({}): {}", p + 1, titan ? "titan" : "Crash",
                    done ? "done" : "nothing left to buy");
      }
      if (requests.max_level[p]) {
        if (titan) {
          int bought = 0;
          // Every step but the last bought silently, the last one shown.
          while (bought < kUpgStepCount && LevelUpTitan(ctx, base, stats, titan, false)) ++bought;
          REXLOG_INFO("Cheats: player {} titan max level: {} steps bought", p + 1, bought);
        } else {
          g_crash_max_frames[p] = 600;  // ~10 s of frames, renewed by each upgrade
          REXLOG_INFO("Cheats: player {} Crash max level: started", p + 1);
        }
      }
      // Crash's chain: top up each new threshold as his script posts it (the
      // next one comes once the player closed the last "Level Up!" screen).
      // Ends ~10 s after the last upgrade, or when he jacks a titan.
      if (g_crash_max_frames[p] > 0) {
        --g_crash_max_frames[p];
        if (titan) {
          g_crash_max_frames[p] = 0;
        } else if (LevelUpCrash(stats)) {
          g_crash_max_frames[p] = 600;
          REXLOG_INFO("Cheats: player {} Crash max level: one more upgrade", p + 1);
        }
      }
      if (requests.refill[p] && body) Refill(ctx, base, body);
      if (requests.kill[p] && body) Kill(ctx, base, p, body, titan != 0);
      if (requests.free_jack[p]) GiveFreeJack(ctx, base, p, titan);
      if (requests.hurt[p] > 0.0f && body) {
        if (const uint32_t dmg = DamageableOf(body)) {
          CallGame(sub_821349E0, ctx, base, dmg, 0, 0, 0, -double(requests.hurt[p]));
          REXLOG_INFO("Cheats: player {} hurt by {:.0f} (test)", p + 1, requests.hurt[p]);
        }
      }
    }
  }

  for (auto it = g_killed.begin(); it != g_killed.end();) {
    it = --it->second <= 0 ? g_killed.erase(it) : std::next(it);
  }

  // Spawns waiting (spawn.h).
  spawn::Tick(ctx, base, in_level);

  // The menu's copy.
  Snapshot snapshot;
  snapshot.in_level = in_level;
  if (stats) {
    snapshot.total_mojo = ReadStat(stats, kStatTotalMojo);
    if (in_level) {
      for (int p = 0; p < kPlayers; ++p) snapshot.players[p] = Describe(stats, p);
    }
  }
  snapshot.hud_hidden = g_hud_hidden.load();
  std::lock_guard<std::mutex> lock(g_mutex);
  g_snapshot = std::move(snapshot);
}

}  // namespace

// --- API ----------------------------------------------------------------------

Snapshot GetSnapshot() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_snapshot;
}
void RequestLevelUp(int player) {
  MarkCheated();
  std::lock_guard<std::mutex> lock(g_mutex);
  if (player >= 0 && player < kPlayers) ++g_requests.level_up[player];
}
void RequestMaxLevel(int player) {
  MarkCheated();
  std::lock_guard<std::mutex> lock(g_mutex);
  if (player >= 0 && player < kPlayers) g_requests.max_level[player] = true;
}
void RequestAddMojo(int player, int amount) {
  MarkCheated();
  std::lock_guard<std::mutex> lock(g_mutex);
  if (player >= 0 && player < kPlayers) g_requests.add_mojo[player] += amount;
}
void RequestKill(int player) {
  MarkCheated();
  std::lock_guard<std::mutex> lock(g_mutex);
  if (player >= 0 && player < kPlayers) g_requests.kill[player] = true;
}
void RequestRefillHealth(int player) {
  MarkCheated();
  std::lock_guard<std::mutex> lock(g_mutex);
  if (player >= 0 && player < kPlayers) g_requests.refill[player] = true;
}
void SetHudHidden(bool hidden) {
  g_hud_hidden = hidden;
  REXLOG_INFO("Cheats: HUD {}", hidden ? "hidden" : "shown");
}
bool HudHidden() { return g_hud_hidden.load(); }
bool AchievementsBlocked() { return g_cheated.load(); }
void NoteProgressCheat() { MarkCheated(); }
void RequestFreeJack(int player) {
  MarkCheated();
  std::lock_guard<std::mutex> lock(g_mutex);
  if (player >= 0 && player < kPlayers) g_requests.free_jack[player] = true;
}
void RequestStepFrame() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_requests.step = true;
}
void SetGodMode(bool on) {
  if (on) MarkCheated();
  g_god = on;
  REXLOG_INFO("Cheats: god mode {}", on ? "on" : "off");
}
bool GodMode() { return g_god.load(); }
void SetGameSpeed(float speed) { g_speed = std::clamp(speed, 0.1f, 4.0f); }
float GameSpeed() { return g_speed.load(); }
void SetFrozen(bool on) {
  g_frozen = on;
  REXLOG_INFO("Cheats: game {}", on ? "frozen" : "running");
}
bool Frozen() { return g_frozen.load(); }

bool DebugCommand(std::string_view command) {
  const std::string_view original = command;
  // Words: verb, then up to two arguments.
  std::string_view words[3];
  int count = 0;
  while (!command.empty() && count < 3) {
    const size_t start = command.find_first_not_of(' ');
    if (start == std::string_view::npos) break;
    command.remove_prefix(start);
    const size_t end = std::min(command.find(' '), command.size());
    words[count++] = command.substr(0, end);
    command.remove_prefix(end);
  }
  if (!count) return false;
  const std::string_view verb = words[0];
  const bool on = words[1] == "on";
  const int player = count > 1 ? std::atoi(std::string(words[1]).c_str()) - 1 : 0;
  const bool player_ok = player >= 0 && player < kPlayers;
  if (verb == "god") SetGodMode(on);
  else if (verb == "freeze") SetFrozen(on);
  else if (verb == "step") RequestStepFrame();
  else if (verb == "speed" && count > 1) SetGameSpeed(std::strtof(std::string(words[1]).c_str(), nullptr));
  else if (verb == "hud") SetHudHidden(!on);
  else if (verb == "levelup" && player_ok) RequestLevelUp(player);
  else if (verb == "maxlevel" && player_ok) RequestMaxLevel(player);
  else if (verb == "mojo" && player_ok && count > 2) {
    RequestAddMojo(player, std::atoi(std::string(words[2]).c_str()));
  } else if (verb == "refill" && player_ok) RequestRefillHealth(player);
  else if (verb == "freejack" && player_ok) RequestFreeJack(player);
  else if (verb == "kill" && player_ok) RequestKill(player);
  else if (verb == "hurt" && player_ok && count > 2) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_requests.hurt[player] += std::strtof(std::string(words[2]).c_str(), nullptr);
  }
  else if (verb == "freecam") free_camera::SetEnabled(on);
  else if (verb == "freecam_keys") free_camera::SetKeysMoveCamera(on);
  else if (verb == "freecam_reset") free_camera::ResetToGameCamera();
  else if (verb == "spawn") {
    const size_t at = original.find("spawn");
    return spawn::DebugCommand(original.substr(at + 5));
  }
  else if (verb == "info") {
    // The menu's numbers, in the log.
    const Snapshot snap = GetSnapshot();
    REXLOG_INFO("Cheats: in level {}, all mojo {}, HUD {}, speed {:.2f}, frozen {}, god {}",
                snap.in_level, snap.total_mojo, snap.hud_hidden ? "hidden" : "shown", GameSpeed(),
                Frozen(), GodMode());
    for (int p = 0; p < kPlayers; ++p) {
      const PlayerInfo& i = snap.players[p];
      if (!i.present) continue;
      REXLOG_INFO("Cheats: player {} = {}{}: level {} (max {}), mojo {} / next {}{}, health {:.1f} / {:.1f}",
                  p + 1, i.name, i.titan ? " (titan)" : "", i.level, i.max_level, i.mojo,
                  i.next_price, i.fully_upgraded ? ", fully upgraded" : "", i.hitpoints,
                  i.max_hitpoints);
    }
  } else return false;
  return true;
}

}  // namespace cheats

// -----------------------------------------------------------------------------
// The wrapped game functions (strong definitions of the generated weak ones)
// -----------------------------------------------------------------------------

// CTimeManager::GetScale (r3 = time manager, returns f1): times our speed.
// Called by the frame function once per frame (and by five others, which then
// see the same scale, as with the game's own slow motion); that call also
// runs our per-frame work.
extern "C" REX_FUNC(sub_822FFA40) {
  const bool frame_function = uint32_t(ctx.lr) == cheats::kFrameFunctionScaleReturn;
  __imp__sub_822FFA40(ctx, base);
  const double scale = ctx.f1.f64;
  if (frame_function) {
    cheats::Tick(ctx, base);
  }
  ctx.f1.f64 = scale * double(cheats::g_speed.load());
}

// CDamageableBehaviour::Update (slot 4: r3 = this, r4 = actor, f1 = dt):
// remembers which damageables are players', refills them in god mode.
extern "C" REX_FUNC(sub_82132F88) {
  const uint32_t self = ctx.r3.u32, actor = ctx.r4.u32;
  __imp__sub_82132F88(ctx, base);
  using namespace cheats;
  g_damageable_filling[actor] = self;
  if (IsPlayerBody(actor)) {
    g_player_damageables.insert(self);
    if (g_god.load() && !g_killed.count(self)) RefillDamageable(ctx, base, self);
  } else {
    g_player_damageables.erase(self);
  }
}

// CDamageableBehaviour: change hitpoints (r3 = this, f1 = change, negative =
// damage). God mode: a player's character loses nothing.
extern "C" REX_FUNC(sub_821349E0) {
  using namespace cheats;
  if (g_god.load() && ctx.f1.f64 < 0.0 && g_player_damageables.count(ctx.r3.u32)) {
    return;
  }
  __imp__sub_821349E0(ctx, base);
}

// The achievement writer (r3 = the achievement manager): takes the next queued
// unlock (28 bytes, a queue ending at +168), copies it to +180 and starts the
// Xbox's XUserWriteAchievements with it (0x82300670; the manager polls the
// write's overlapped at +136 before the next one: sub_82291BB8). After a
// progress cheat we only take the unlock off the queue: nothing is written, so
// the achievement isn't earned (the game's own pop-up still shows, from its
// stats). How we found it: "Max level" on a titan unlocked "fully upgraded"
// (2026-10-05 test run).
extern "C" REX_FUNC(__imp__sub_82292938);
extern "C" REX_FUNC(sub_82292938) {
  using namespace cheats;
  if (!g_cheated.load()) {
    __imp__sub_82292938(ctx, base);
    return;
  }
  const uint32_t manager = ctx.r3.u32;
  Write32(manager + 168, Read32(manager + 168) - 28);
  REXLOG_INFO("Cheats: an achievement unlock was not written (cheats were used this session)");
}
