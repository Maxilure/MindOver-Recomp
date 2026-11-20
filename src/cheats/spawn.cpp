// =============================================================================
// cheats/spawn.cpp -- see spawn.h
// =============================================================================
#include "spawn.h"

#include "../guest_memory.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../players/more_players.h"
#include "cheats.h"
#include "free_camera.h"

// Game functions we call.
extern "C" REX_FUNC(__imp__sub_82357020);  // hash of a string (r3 = string, r4 = seed 0)
extern "C" REX_FUNC(__imp__sub_82168620);  // create an actor from a template at a position
extern "C" REX_FUNC(__imp__sub_822E3FC8);  // inventory: does the section named *r4 exist (r3 = manager)
extern "C" REX_FUNC(__imp__sub_822E4D88);  // inventory: is that (existing) section ready (loaded)
extern "C" REX_FUNC(__imp__sub_822E44B8);  // inventory: take a reference on a section (loads it if needed)
extern "C" REX_FUNC(__imp__sub_822E4650);  // inventory: give the reference back (unloads at 0)
extern "C" REX_FUNC(__imp__sub_8213E6D0);  // the next fight tree's START STATE (r3 = name object)
extern "C" REX_FUNC(__imp__sub_8236ACB8);  // name object (r3 = 8 bytes) from a C string (r4)
extern "C" REX_FUNC(__imp__sub_820B6C68);  // CActionPlatform message (r3 = 40 bytes, r4 = code)
extern "C" REX_FUNC(__imp__sub_820B6CE0);  // platform message NAME object (r3) -> its code (4..21, -1 unknown)
extern "C" REX_FUNC(__imp__sub_820B0B70);  // hand an actor message to an actor (r3 actor, r4 message)
extern "C" REX_FUNC(__imp__sub_823C9A70);  // a message's name part, destroyed
extern "C" REX_FUNC(__imp__sub_820B8090);  // an actor message, destroyed
// Paths (the level scripts' WorldManager:CreatePath / AddPathPoint / ...):
extern "C" REX_FUNC(__imp__sub_8236AC40);  // name object (r3, 16 bytes) from a C string (r4)
extern "C" REX_FUNC(__imp__sub_822FA4C0);  // WorldManager: the path of that name (r4 name object), or 0
extern "C" REX_FUNC(__imp__sub_8227E890);  // the game's `new` (r3 bytes)
extern "C" REX_FUNC(__imp__sub_823574E8);  // set the current heap (r3), returns the old one
extern "C" REX_FUNC(__imp__sub_822FB960);  // path constructor (r3 memory, r4 name object): room for 100 points
extern "C" REX_FUNC(__imp__sub_822FA2C8);  // WorldManager: attach path r5 to the actor named r4 (C string)
extern "C" REX_FUNC(__imp__sub_822FBB90);  // path: type (r4) -> +60
extern "C" REX_FUNC(__imp__sub_822FBB98);  // path: speed kind (r4) + speed (f1) -> +64, +72
extern "C" REX_FUNC(__imp__sub_822FBBA8);  // path: rotation type (r4) -> +68
extern "C" REX_FUNC(__imp__sub_822FBB30);  // path: add a point (r4 -> 3 floats); returns the 24-byte point
extern "C" REX_FUNC(__imp__sub_822EAB78);  // point: duration (f1) -> +12
extern "C" REX_FUNC(__imp__sub_82240598);  // point: wait type (r4) -> +20
extern "C" REX_FUNC(__imp__sub_822FA510);  // WorldManager +8 map: the slot for name object r4 (made if new)

namespace spawn {
namespace {

constexpr uint32_t kActorVtable = 0x820211A0;  // CActor
constexpr int kDone = 1 << 20;                  // ToStun::frames: finished
constexpr uint32_t kActorController = 0x1C;  // actor -> its behaviour controller (the AI, for NPCs)
constexpr uint32_t kActorWorldMatrix = 44;  // actor -> its world matrix (rows of 4 floats; +48 = position)
constexpr uint32_t kJackingVtable = 0x8202EF0C;
constexpr uint32_t kStunState = 1104;  // CJackingBehaviour: 2 = stunned (ready to jack)
constexpr uint32_t kStunned = 2;
constexpr uint32_t kStunMeter = 40;    // CJackingBehaviour: -> the meter (a float; = the titan's health)
constexpr uint32_t kCoOpState = 0x8259B11C;  // per player: 0 not joined, 1 / 3 mask, 2 in game
constexpr int kMaxCount = 10;
// The inventory (loaded assets, in sections keyed by a name hash): manager.
constexpr uint32_t kInventoryManager = 0x8259B264;
// The level's pools of ready-made actors (sub_820B3998): pointers to pools at
// kPools, their count at kPoolCount. A pool: +4 template name (C string), +8
// how many, +12 its entries (8 bytes: +0 the actor, +4 byte "in use").
constexpr uint32_t kPools = 0x82597C70, kPoolCount = 0x825AAD54;

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
void WriteFloat(uint32_t address, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  Write32(address, bits);
}
// Committed pages only (GitHub issue #25: guest_memory.cpp).
bool Readable(uint32_t address, uint32_t size) { return guest_memory::Readable(address, size); }

using GuestFunction = void (*)(PPCContext&, uint8_t*);
// Calls a game function from inside our per-frame hook (game's main thread),
// registers put back afterwards; returns r3.
uint32_t CallGame(GuestFunction function, PPCContext& ctx, uint8_t* base, uint32_t r3,
                  uint32_t r4 = 0, uint32_t r5 = 0, uint32_t r6 = 0, uint32_t r7 = 0,
                  uint32_t r8 = 0, double f1 = 0.0) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = r3; ctx.r4.u64 = r4; ctx.r5.u64 = r5;
  ctx.r6.u64 = r6; ctx.r7.u64 = r7; ctx.r8.u64 = r8;
  ctx.f1.f64 = f1;
  function(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

// Our scratch block in guest memory (the game's functions take pointers):
// +0 template name (128 bytes), +192 the actor's name object (the template
// name's hash, 4 bytes), +208 position (4 floats).
constexpr uint32_t kScratchTemplate = 0, kScratchNameObject = 192,
                   kScratchPosition = 208, kScratchStartState = 224, kScratchStartText = 232,
                   kScratchSize = 256;
uint32_t g_scratch = 0;

// --- State --------------------------------------------------------------------

struct Pending {
  std::string template_name;
  int player = 0;
  int count = 1;
  bool knocked_out = false;
  bool loaded_for_it = false;  // we loaded its section (don't try again)
  // AT A SPOT (2026-11-19, console only: test props such as a rope, which
  // hangs DOWN from its spot): the first actor goes exactly here (no
  // "ahead of the player", no lift), the others in a row beside it.
  bool at_spot = false;
  float at[3] = {};
  // A PATH for the new actor (MakePath), attached as soon as it exists.
  std::vector<std::array<float, 3>> path;
  int path_type = 1, path_wait = 0;
  float path_speed = 2.0f, path_duration = 0.0f;
};
// A spawn waiting for its section to load (see LOADING in Spawn).
struct Loading {
  Pending request;
  uint32_t key = 0;  // the section's name hash
  int frames = 0;
};
// A spawned titan to knock out and keep down until it stays down.
struct ToStun {
  uint32_t actor = 0;
  int frames = 0;          // since the spawn
  int stunned_frames = 0;  // in a row it has been down
};

std::mutex g_mutex;  // guards g_pending and g_result
uint32_t g_last_spawned = 0;  // the last actor Spawn made (under g_mutex)
std::vector<Pending> g_pending;
std::string g_result;
std::vector<ToStun> g_to_stun;  // game thread only
std::vector<Loading> g_loading;  // game thread only
void SetResult(std::string text) {
  REXLOG_INFO("Cheats: spawn: {}", text);
  std::lock_guard<std::mutex> lock(g_mutex);
  g_result = std::move(text);
}

// A member of `object` (first `size` bytes) pointing at an object with
// vtable `vtable`, 0 if none. Every pointer is checked readable first.
uint32_t FindMember(uint32_t object, uint32_t size, uint32_t vtable) {
  if (!Readable(object, size)) return 0;
  for (uint32_t offset = 4; offset < size; offset += 4) {
    const uint32_t p = Read32(object + offset);
    if (p >= 0x40000000 && Readable(p, 4) && Read32(p) == vtable) return p;
  }
  return 0;
}

// The fight-tree state a "knocked out" spawn starts in (see START STATE in
// Spawn).
std::string KnockedOutState(const std::string& template_name) {
  const bool boss = template_name.find("Boss") != std::string::npos;
  return boss ? "Stunned" : "StartJackable";
}

// A titan's jacking behaviour: among the actor's members, or its AI
// controller's (actor +0x1C, e.g. "Strong" : NPCBaseJackable for a Ratcicle):
// a fresh Ratcicle had none in the actor itself.
uint32_t FindJacking(uint32_t actor) {
  if (const uint32_t jacking = FindMember(actor, 0x600, kJackingVtable)) return jacking;
  const uint32_t controller = Readable(actor + kActorController, 4) ? Read32(actor + kActorController) : 0;
  return controller ? FindMember(controller, 0x800, kJackingVtable) : 0;
}

// Where player p's body (titan or Crash) stands, and the directions to place
// spawns: "forward" = AWAY FROM THE CAMERA (level), so they appear beyond the
// player on screen. (The body's own facing put them behind the camera when a
// jacked titan faced it: first test, 2026-10-05.)
bool BodyPlace(int p, float position[3], float forward[3], float right[3]) {
  const uint32_t state = Read32(kCoOpState + 4 * uint32_t(p));
  if (state != 2) return false;  // not in, or a mask
  const uint32_t titan = more_players::TitanOfPlayer(p);
  const uint32_t body = titan ? titan : more_players::CharacterOfPlayer(p);
  if (!body || !Readable(body + kActorWorldMatrix, 4)) return false;
  const uint32_t matrix = Read32(body + kActorWorldMatrix);
  if (!Readable(matrix, 64)) return false;
  for (int i = 0; i < 3; ++i) {
    forward[i] = ReadFloat(matrix + 32 + 4 * i);    // row 2: the body's facing (fallback)
    position[i] = ReadFloat(matrix + 48 + 4 * i);   // row 3
  }
  const free_camera::Pose camera = free_camera::GameCameraPose();
  if (camera.direction[0] != 0.0f || camera.direction[2] != 0.0f) {
    for (int i = 0; i < 3; ++i) forward[i] = camera.direction[i];
  }
  // Right = up x forward (y up; matches the free camera's right).
  right[0] = forward[2];
  right[1] = 0.0f;
  right[2] = -forward[0];
  // Level both directions (spawn on the player's height).
  for (float* v : {forward, right}) {
    v[1] = 0.0f;
    const float length = std::sqrt(v[0] * v[0] + v[2] * v[2]);
    if (length < 1e-3f) return false;
    v[0] /= length;
    v[2] /= length;
  }
  return true;
}

// Does a pool of `template_name` have a free actor (what the creation takes
// first)? A read-only copy of sub_820B3998's search.
bool PoolHasFree(const std::string& template_name) {
  const int pools = int32_t(Read32(kPoolCount));
  for (int i = 0; i < pools && i < 4096; ++i) {
    const uint32_t pool = Read32(kPools + 4 * uint32_t(i));
    if (!Readable(pool, 16)) continue;
    const uint32_t name = Read32(pool + 4);
    if (!Readable(name, 1) || template_name != reinterpret_cast<const char*>(Guest(name))) continue;
    const int count = int32_t(Read32(pool + 8));
    const uint32_t entries = Read32(pool + 12);
    if (count <= 0 || !Readable(entries, uint32_t(count) * 8)) return false;
    for (int k = 0; k < count; ++k) {
      if (*Guest(entries + 8 * uint32_t(k) + 4) == 0) return true;
    }
    return false;
  }
  return false;
}

// THE GAME'S HANDLE TABLE (2026-11-12, a player's crash report: ~200
// Characters:Chiratta spawned 10 at a time in a few seconds, then a read of
// 0x12 in sub_824542D8 under the creation's scripts): the manager at
// [0x825973D0] hands out 24-byte entries (named handles: the scripts, sounds
// and behaviours of every actor take some) from ONE fixed free list at
// +0x1138 (+0 head, +4 tail, +8 count; pop = sub_823534D0, entries from +0x1144,
// ~6,200 of them, 14-bit handles). sub_824542D8 pops one and uses it WITHOUT a
// null check: the table full = the crash. The levels never come close; a
// spawn cheat in a loop does, and enemies take far more entries than a
// titan. So the spawn checks the free count first and stops while there's
// still room for the level itself (kHandleReserve) and for one more actor of
// this kind (its cost measured at the previous spawn of it; a first spawn
// guesses kHandleFirstGuess).
constexpr uint32_t kHandleManagerGlobal = 0x825973D0;
constexpr uint32_t kHandleFreeCount = 0x1140;
constexpr uint32_t kHandleReserve = 800;
constexpr uint32_t kHandleFirstGuess = 300;
std::map<std::string, uint32_t> g_handle_cost;  // template -> entries one spawn took (most seen)

// The table's free entries, or -1 if the manager isn't there.
int64_t FreeHandles() {
  const uint32_t manager = Read32(kHandleManagerGlobal);
  if (!manager || !Readable(manager + kHandleFreeCount, 4)) return -1;
  return int64_t(Read32(manager + kHandleFreeCount));
}

// One request: `count` actors in a row in front of the player.
//
// THE NAME MATTERS: the creation picks the inventory section holding the new
// actor's assets (its model, its collision shape...) BY THE ACTOR'S NAME. The
// game names actors after their template (every large mojo has the same name
// hash; a titan brought into the next level too), so we do the same. A first
// version named them "CheatSpawn1".. and the new actor's physics found no
// collision shape: a null read in sub_822531E8 (from sub_8217F808), the game
// stuck (2026-10-05, caught with tools/gdb/catch_fault). For the same reason
// we refuse a template whose section isn't loaded (and has no free pooled
// actor): the game would build it without its assets and crash the same way.
void Spawn(PPCContext& ctx, uint8_t* base, const Pending& request) {
  float position[3], forward[3], right[3];
  if (!BodyPlace(request.player, position, forward, right)) {
    SetResult(fmt::format("player {} has no body in the level (not in, or a mask)", request.player + 1));
    return;
  }
  // Villagers crash the game when made this way (a null read in their fight
  // tree's set-up, sub_820C1578, under the creation; test 2026-10-05 with
  // Villagers:RatcicleKid, section ready): the level's own spawn events give
  // them something we don't.
  if (request.template_name.rfind("Villagers:", 0) == 0) {
    SetResult("villagers can't be spawned (they crash the game this way)");
    return;
  }
  if (!g_scratch) g_scratch = rex::system::kernel_memory()->SystemHeapAlloc(kScratchSize);
  if (!g_scratch || request.template_name.size() >= 127) return;
  std::memcpy(Guest(g_scratch + kScratchTemplate), request.template_name.c_str(),
              request.template_name.size() + 1);
  // Characters (titans are big) further ahead and wider apart than pickups.
  const bool character = request.template_name.rfind("Characters:", 0) == 0;
  const float ahead = character ? (request.knocked_out ? 10.0f : 6.0f) : 3.0f;
  const float spacing = character ? 5.0f : 1.5f;
  Write32(g_scratch + kScratchNameObject,
          CallGame(__imp__sub_82357020, ctx, base, g_scratch + kScratchTemplate, 0));
  const uint32_t inventory = Read32(kInventoryManager);
  int made = 0;
  bool full = false;  // stopped by the handle table (above)
  uint32_t last_actor = 0;  // for the log (console tests: `who` on it)
  for (int i = 0; i < request.count; ++i) {
    // The creation's own test (sub_821686C0): the section exists AND is
    // ready ("ready" on a missing section would read through a null pointer).
    const bool exists = inventory && (CallGame(__imp__sub_822E3FC8, ctx, base, inventory,
                                               g_scratch + kScratchNameObject) & 0xFF) != 0;
    const bool ready = exists && (CallGame(__imp__sub_822E4D88, ctx, base, inventory,
                                           g_scratch + kScratchNameObject) & 0xFF) != 0;
    const bool pooled = PoolHasFree(request.template_name);
    if (i == 0) {
      REXLOG_INFO("Cheats: spawn: {} (hash {:08X}): section {}, pooled actor free: {}",
                  request.template_name, Read32(g_scratch + kScratchNameObject),
                  ready ? "ready" : exists ? "not ready" : "none", pooled ? "yes" : "no");
    }
    // LOADING: the section exists for every group of the game's packages
    // (GlobalPackages: one group per template, named like it, e.g.
    // "Characters:Znu" -> package/Characters_Znu.p3d + its model and
    // animations); "not ready" = not loaded in this level. Taking a reference
    // on it (sub_822E44B8 -> sub_822E24D0: count +1, and an unloaded section
    // is asked to load, sub_822E20E0(section, 1)) loads it in the background,
    // as the level loader does for a titan brought along (sub_822E5E00 at
    // 0x822E5EEC). Tick spawns once it's ready, then gives our reference back
    // (the new actor holds its own, as the creation takes one).
    if (i == 0 && exists && !ready && !pooled && !request.loaded_for_it && inventory) {
      CallGame(__imp__sub_822E44B8, ctx, base, inventory, g_scratch + kScratchNameObject);
      Loading loading;
      loading.request = request;
      loading.request.loaded_for_it = true;
      loading.key = Read32(g_scratch + kScratchNameObject);
      g_loading.push_back(loading);
      SetResult(fmt::format("loading {} into this level...", request.template_name));
      return;
    }
    if (!ready && !pooled) break;
    const int64_t free_before = FreeHandles();
    const auto cost_it = g_handle_cost.find(request.template_name);
    const uint32_t cost = cost_it != g_handle_cost.end() ? cost_it->second : kHandleFirstGuess;
    if (free_before >= 0 && free_before < int64_t(kHandleReserve + 2 * cost)) {
      REXLOG_WARN("Cheats: spawn: the game's handle table is almost full ({} free, one {} takes ~{}): stopped",
                  free_before, request.template_name, cost);
      full = true;
      break;
    }
    const float side = (float(i) - float(request.count - 1) * 0.5f) * spacing;
    for (int k = 0; k < 3; ++k) {
      WriteFloat(g_scratch + kScratchPosition + 4 * k,
                 request.at_spot ? request.at[k] + right[k] * side
                                 : position[k] + forward[k] * ahead + right[k] * side + (k == 1 ? 0.5f : 0.0f));
    }
    WriteFloat(g_scratch + kScratchPosition + 12, 1.0f);
    // START STATE: a "knocked out" titan is created already down, ready to
    // jack, the way the levels respawn one (DO_SpawnEnemy's "StartJackable",
    // CActionSpawnEnemy sub_8229BF40): sub_8213E6D0 copies a state name into
    // the global 0x825A4FB8, the fight tree built during the creation starts
    // in that state, and an empty name is put back right after.
    const std::string start_state = request.knocked_out ? KnockedOutState(request.template_name) : "";
    if (!start_state.empty()) {
      std::memcpy(Guest(g_scratch + kScratchStartText), start_state.c_str(), start_state.size() + 1);
      CallGame(__imp__sub_8236ACB8, ctx, base, g_scratch + kScratchStartState, g_scratch + kScratchStartText);
      CallGame(__imp__sub_8213E6D0, ctx, base, g_scratch + kScratchStartState);
    }
    const uint32_t actor = CallGame(__imp__sub_82168620, ctx, base, g_scratch + kScratchTemplate,
                                    g_scratch + kScratchNameObject, 0, g_scratch + kScratchPosition,
                                    0, 8);
    if (!start_state.empty()) {  // back to "no start state", as the level scripts do
      Write32(g_scratch + kScratchStartState, 0);
      Write32(g_scratch + kScratchStartState + 4, 0);
      CallGame(__imp__sub_8213E6D0, ctx, base, g_scratch + kScratchStartState);
    }
    if (!actor) break;
    const int64_t free_after = FreeHandles();
    if (free_before >= 0 && free_after >= 0 && free_after < free_before) {
      uint32_t& seen = g_handle_cost[request.template_name];
      seen = std::max(seen, uint32_t(free_before - free_after));
    }
    ++made;
    last_actor = actor;
    if (actor && i == 0 && !request.path.empty()) {
      MakePath(actor, request.path, request.path_type, 0, request.path_speed, request.path_wait, request.path_duration);
    }
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_last_spawned = actor;
    }
    // (The start state does it now; the meter route below stays for a
    // template without a start state.)
    if (request.knocked_out && start_state.empty()) g_to_stun.push_back({actor, 0, 0});
  }
  if (full) {
    SetResult(made ? fmt::format("{} x {} spawned, then stopped: too many things in the level (the game's "
                                 "table is almost full; kill some first)", made, request.template_name)
                   : std::string("too many things in the level (the game's table is almost full): kill some first"));
  } else if (!made) {
    SetResult(fmt::format("{} can't be spawned here (not in the game's packages, or it didn't load)",
                          request.template_name));
  } else {
    SetResult(fmt::format("{} x {} spawned{} (last actor {:08X})", made, request.template_name,
                          request.knocked_out ? ", knocked out" : "", last_actor));
  }
}

// "KNOCKED OUT": one hit away from down. How we got there (2026-10-05 tests,
// Roller on Wumpa Island and Ratcicle in Ratcicle Kingdom):
//   * the stun meter can be emptied directly (sub_8214EBF8) or by the actor
//     message "stun" (CActionStun, kind 37, built by sub_820B7C20, sent with
//     sub_820B0B70; a negative amount empties it, handler sub_8214CFC0 at
//     0x8214D094). The state then reads 2 = stunned (IsStunned), but the titan
//     kept fighting: its AI only goes down from a HIT (also tried: the "hit
//     reaction" message kind 28 = sub_820B7440, and the awareness message;
//     no knockdown). And a stun message to a titan already at state 2 fills
//     the meter again (0x8214EC58).
//   * what works: after the titan's arrival (1.5 s; the arrival fills the
//     meter), keep the meter at 0.5 (the jacking behaviour's +40 points at
//     it). It shows the stun bar over its head; the first hit knocks it down
//     with the game's own stars and jack prompt (B).
// The titan is followed until it's down (then the game takes over: it gets up
// again after a while if nobody jacks it), for 60 s at most, and dropped at
// once if a player jacked it (a jacked titan's meter is its health) or it
// isn't an actor any more (gone; its memory may be reused).
void StunPending(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  (void)base;
  for (ToStun& s : g_to_stun) {
    ++s.frames;
    bool jacked = false;
    for (int p = 0; p < 4; ++p) jacked = jacked || more_players::TitanOfPlayer(p) == s.actor;
    if (jacked || !Readable(s.actor, 4) || Read32(s.actor) != kActorVtable || s.frames > 3600) {
      s.frames = kDone;
      continue;
    }
    const uint32_t jacking = FindJacking(s.actor);
    if (!jacking) {
      if (s.frames >= 600) {
        SetResult("spawned, but it can't be knocked out (not a titan?)");
        s.frames = kDone;
      }
      continue;
    }
    if (Read32(jacking + kStunState) == kStunned) {
      if (++s.stunned_frames >= 30) s.frames = kDone;  // down: the game takes over
      continue;
    }
    s.stunned_frames = 0;
    const uint32_t meter = Read32(jacking + kStunMeter);
    if (s.frames >= 90 && Readable(meter, 4) && ReadFloat(meter) > 0.5f) {
      WriteFloat(meter, 0.5f);  // one hit away from down
    }
  }
  std::erase_if(g_to_stun, [](const ToStun& s) { return s.frames == kDone; });
}

// Our reference on a section we loaded, given back (game thread).
void ReleaseSection(PPCContext& ctx, uint8_t* base, uint32_t key) {
  const uint32_t inventory = Read32(kInventoryManager);
  if (!inventory || !g_scratch) return;
  Write32(g_scratch + kScratchNameObject, key);
  CallGame(__imp__sub_822E4650, ctx, base, inventory, g_scratch + kScratchNameObject);
}

// Sections being loaded for a spawn: spawn when ready, give up after 20 s.
void CheckLoading(PPCContext& ctx, uint8_t* base) {
  const uint32_t inventory = Read32(kInventoryManager);
  for (Loading& l : g_loading) {
    ++l.frames;
    Write32(g_scratch + kScratchNameObject, l.key);
    const bool ready = inventory && (CallGame(__imp__sub_822E4D88, ctx, base, inventory,
                                              g_scratch + kScratchNameObject) & 0xFF) != 0;
    if (ready) {
      REXLOG_INFO("Cheats: spawn: {} loaded in {:.1f} s", l.request.template_name, l.frames / 60.0);
      Spawn(ctx, base, l.request);
      ReleaseSection(ctx, base, l.key);
      l.frames = kDone;
    } else if (l.frames > 1200) {
      ReleaseSection(ctx, base, l.key);
      SetResult(fmt::format("{} didn't load in 20 s: not spawned", l.request.template_name));
      l.frames = kDone;
    }
  }
  std::erase_if(g_loading, [](const Loading& l) { return l.frames == kDone; });
}

// Leaving the level: references we still hold go back (else the section
// would stay loaded for good).
void ReleaseLoading(PPCContext& ctx, uint8_t* base, const char* why) {
  for (const Loading& l : g_loading) {
    ReleaseSection(ctx, base, l.key);
    SetResult(fmt::format("{} not spawned: {}", l.request.template_name, why));
  }
  g_loading.clear();
}

std::vector<Category> MakeCatalogue() {
  auto titan = [](const char* name) { return Entry{name, std::string("Characters:") + name, true}; };
  // a titan whose template name isn't the one players know
  auto titan_as = [](const char* label, const char* name) {
    return Entry{label, std::string("Characters:") + name, true};
  };
  auto character = [](const char* label, const char* name) {
    return Entry{label, std::string("Characters:") + name, false};
  };
  auto pickup = [](const char* label, const char* name) {
    return Entry{label, std::string("Collectables:") + name, false};
  };
  return {
      {"Titans",
       {titan("Ratcicle"), titan("Spike"), titan("Roller"), titan("Battler"), titan("TK"),
        titan("Shurtle"), titan("Scorporilla"), titan("Phantom"), titan("Stinky"),
        titan("Parafox"), titan("Sludge"), titan("Yuktopus"), titan("RatcicleHero"),
        titan("SpikeHero"), titan("ShurtleHero"), titan("SludgeHero"), titan("PhantomHero"),
        titan("ParafoxHero"),
        // the two boss titans (their upgrade levels are stats 104 / 105, so
        // they're jackable titans like the others; names from default.rcf)
        titan_as("Crunch (boss)", "CrunchBoss"), titan_as("Cortex (boss)", "CortexBoss")}},
      {"Enemies",
       {character("Znu", "Znu"), character("Slappy (Slap-E)", "Slappy"),
        character("Bratgirl", "Bratgirl"), character("Monkey", "Monkey"),
        character("Chiratta", "Chiratta")}},
      // PICKUPS (2026-11-07, asked for in a playtest): template names from
      // default.rcf ("Collectables:..."). They appear in front of the player
      // (walk forward to take them).
      {"Pickups",
       {pickup("Mojo (small)", "c_mojoS"), pickup("Mojo (medium)", "c_mojoM"),
        pickup("Mojo (large)", "c_mojoXL"), pickup("Mojo multiplier", "c_mojoMultiplier"),
        pickup("Health", "c_health")}},
  };
}

}  // namespace

const std::vector<Category>& Catalogue() {
  static const std::vector<Category> catalogue = MakeCatalogue();
  return catalogue;
}

void RequestWithPath(const std::string& template_name, const float at[3], const std::vector<std::array<float, 3>>& path,
                     int type, float speed, int wait, float duration) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Pending request{template_name, 0, 1, false};
  request.at_spot = true;
  std::copy(at, at + 3, request.at);
  request.path = path;
  request.path_type = type;
  request.path_speed = speed;
  request.path_wait = wait;
  request.path_duration = duration;
  g_pending.push_back(request);
}

void Request(const std::string& template_name, int player, int count, bool knocked_out, const float* at,
             bool as_cheat) {
  if (template_name.empty() || player < 0 || player > 3) return;
  // A spawned titan to jack counts as progress: achievements off (not for the
  // test course's own objects).
  if (as_cheat) cheats::NoteProgressCheat();
  std::lock_guard<std::mutex> lock(g_mutex);
  Pending request{template_name, player, std::clamp(count, 1, kMaxCount), knocked_out};
  if (at) {
    request.at_spot = true;
    std::copy(at, at + 3, request.at);
  }
  g_pending.push_back(request);
}

std::string LastResult() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_result;
}

// PLATFORM MESSAGES (2026-11-19, the test course's moving platforms): what a
// level script's DO_PlatformMessage(<platform>, "NEXT_WAIT_POINT" | "PAUSE" |
// "CONTINUE" | ...) does (CActionPlatformMessage, vtable 0x8203FB5C, run
// sub_82296B58): an actor message of kind 0x1A (a CActionPlatform, vtable
// 0x820217EC; built by sub_820B6C68(message, code), code at +0x10) handed to
// the platform actor by sub_820B0B70(actor, message), then its parts
// destroyed (sub_823C9A70 on +0x14, sub_820B8090). The code is a small
// number, made from the name by sub_820B6CE0 (function-local name objects
// built by sub_8236ACB8, compared in turn): NEXT_WAIT_POINT 4, PAUSE 5,
// CONTINUE 6, ROTATION_SPEED1 7, ROTATION_SPEED2 8, RESET 9,
// IS_CRASH_ATTACHED 10, IS_NPC_ATTACHED 13, ROT_SPEED 14, SOUND 15,
// GO_NORTH..GO_DOWN 16-21, anything else -1. The path follower's message
// slot (sub_82158DE8, vtable 0x8202FA1C slot 5) switches on it: 4 -> +0x50 = 1,
// 5 -> 2, 9 -> 3, 6 -> sub_822922E8, 16-21 -> their moves. (The first
// version sent the name's HASH word as the code: the follower's switch
// matched nothing, PAUSE and CONTINUE changed nothing.) Queued from any
// thread, sent in Tick.
struct PlatformMessage {
  uint32_t actor;
  std::string name;
};
std::vector<PlatformMessage> g_platform_messages;  // under g_mutex

void SendPlatformMessages(PPCContext& ctx, uint8_t* base) {
  std::vector<PlatformMessage> list;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    list.swap(g_platform_messages);
  }
  if (list.empty()) return;
  static uint32_t scratch = 0;  // 64-byte message + 8-byte name object + 32-byte name text
  if (!scratch) scratch = rex::system::kernel_memory()->SystemHeapAlloc(128);
  if (!scratch) return;
  for (const PlatformMessage& m : list) {
    if (m.name.size() >= 31 || !m.actor || !Readable(m.actor, 64)) continue;
    std::memset(Guest(scratch), 0, 128);
    std::memcpy(Guest(scratch + 96), m.name.c_str(), m.name.size() + 1);
    CallGame(__imp__sub_8236ACB8, ctx, base, scratch + 64, scratch + 96);
    const uint32_t code = CallGame(__imp__sub_820B6CE0, ctx, base, scratch + 64);
    CallGame(__imp__sub_823C9A70, ctx, base, scratch + 64);  // the name object, destroyed
    if (code == 0xFFFFFFFFu) {
      REXLOG_WARN("Cheats: platform message {}: not a name the game knows", m.name);
      continue;
    }
    CallGame(__imp__sub_820B6C68, ctx, base, scratch, code);
    CallGame(__imp__sub_820B0B70, ctx, base, m.actor, scratch);
    CallGame(__imp__sub_823C9A70, ctx, base, scratch + 0x14);
    CallGame(__imp__sub_820B8090, ctx, base, scratch);
    REXLOG_INFO("Cheats: platform message {} (code {}) sent to {:08X}", m.name, code, m.actor);
  }
}

// PATHS FOR SPAWNED PLATFORMS (2026-11-19, findings/69 s.9). A moving
// platform rides a PATH the level's props script builds right after making
// it (Desert, L4_R1_MovingPlatform2):
//   WorldManager:CreatePath('.:L4_R1_MovingPlatform2:L4_R1_MovingPlatform2Path1')
//   SetPathType(1) SetPathSpeed(0, 10) SetPathRotType(0)
//   per point: AddPathPoint(name, i, {x, y, z}) SetPathPointDuration(3)
//              SetPathPointWaitType(3) SetPathPointAdditionalSpeed(0) CompletePoint()
// The script methods (registered by sub_822F98B8) are thin: CreatePath
// (sub_822FA1D0) splits the name at its last two colons (sub_822FA0B8: actor =
// the segment before the last), looks the path up (sub_822FA4C0), else makes
// one (`new` 0x4C, sub_822FB960; vtable 0x82046CFC, slot 0 = add a reference)
// and attaches it under the ACTOR'S NAME (sub_822FA2C8: WorldManager +8 map,
// keyed by sub_8236ACB8(name)). The others write the path (+60 type, +64/+72
// speed kind / speed, +68 rotation type) or the point (sub_822FBB30: 24 bytes,
// xyz, +12 duration, +16 additional speed, +20 wait type, default 1).
// WorldManager = *(*(0x8259B190) + 120). The path goes under the actor's own
// name object (below), not through CreatePath's name splitting. Wait types seen in the levels:
// 0 (800 points), 1 (25), 3 (165, the "wait for NEXT_WAIT_POINT" platforms).
struct PathRequest {
  uint32_t actor = 0;  // the actor (a spawned platform)
  std::string actor_name;  // for the log
  std::vector<std::array<float, 3>> points;
  int type = 1, speed_kind = 0, wait = 0;
  float speed = 2.0f, duration = 0.0f;
};
std::vector<PathRequest> g_paths;  // under g_mutex
// actor -> its CPathFollowingBehaviour (NotePathFollower; game's main thread).
std::map<uint32_t, uint32_t> g_followers;

void MakePaths(PPCContext& ctx, uint8_t* base) {
  std::vector<PathRequest> list;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    list.swap(g_paths);
  }
  if (list.empty()) return;
  static uint32_t scratch = 0;  // +0 name object (16), +16 path name, +64 actor name, +192 a point, +232 key (16)
  if (!scratch) scratch = rex::system::kernel_memory()->SystemHeapAlloc(256);
  const uint32_t game = Read32(0x8259B190);
  const uint32_t world = game ? Read32(game + 120) : 0;
  if (!scratch || !world) return;
  for (const PathRequest& r : list) {
    if (!r.actor || !Readable(r.actor, 64) || r.points.empty() || r.points.size() > 100) continue;
    const std::string path_name = fmt::format("TestCoursePath{:08X}", r.actor);  // one per platform
    std::memset(Guest(scratch), 0, 256);
    std::memcpy(Guest(scratch + 16), path_name.c_str(), path_name.size() + 1);
    CallGame(__imp__sub_8236AC40, ctx, base, scratch, scratch + 16);
    uint32_t path = CallGame(__imp__sub_822FA4C0, ctx, base, world, scratch);
    const bool existed = path != 0;
    // The game's `new` asserts while no heap is chosen (heap 0; sub_823574E8
    // sets the current one, returns the old): heap 1, as the path's own
    // constructor uses for its points, for the path AND the attach (its map
    // insertion allocates too: the first tries asserted there).
    const uint32_t old_heap = CallGame(__imp__sub_823574E8, ctx, base, 1);
    if (!path) {
      path = CallGame(__imp__sub_8227E890, ctx, base, 0x4C);
      if (path) CallGame(__imp__sub_822FB960, ctx, base, path, scratch);
    }
    CallGame(__imp__sub_823C9A70, ctx, base, scratch);
    if (path && !existed) {
      // ATTACHED UNDER THE ACTOR'S OWN NAME, built the way its path follower
      // builds it every frame until it finds one (CPathFollowingBehaviour
      // Update sub_82158830 -> sub_82158758: sub_8236ABC0(actor +8) ->
      // sub_8236ABA8 -> sub_822FA460 = the +8 map). First try: the attach with
      // the template NAME string (sub_822FA2C8 -> sub_8236ACB8) made another
      // key (a spawned actor's +8 isn't its template's hash): never found.
      // (sub_8236ABC0 returns the name's TWO words in one 64-bit register and
      // sub_8236ABA8 stores them: the key is just the actor's 8-byte name at
      // +8, copied here. A first version took sub_8236ABC0's result through
      // CallGame, which keeps 32 bits: the first word was lost, the follower's
      // lookup never matched.)
      Write32(scratch + 232, Read32(r.actor + 8));
      Write32(scratch + 236, Read32(r.actor + 12));
      const uint32_t slot = CallGame(__imp__sub_822FA510, ctx, base, world + 8, scratch + 232);
      if (slot && !Read32(slot)) Write32(slot, path);
      CallGame(__imp__sub_823C9A70, ctx, base, scratch + 232);
      // The follower looks ONCE, at its first update, then clears its "look
      // for a path" byte (+77, at 0x821587F0) found or not: the platform was
      // already running. Set again, it looks on its next update and finds this.
      if (const auto it = g_followers.find(r.actor); it != g_followers.end()) {
        *Guest(it->second + 77) = 1;
      } else {
        REXLOG_WARN("Cheats: path: {:08X} has no path follower seen yet (not a path platform?)", r.actor);
      }
      Write32(path + 4, Read32(path + 4) + 1);  // CreatePath's reference (vtable slot 0)
    }
    CallGame(__imp__sub_823574E8, ctx, base, old_heap);
    if (!path) continue;
    CallGame(__imp__sub_822FBB90, ctx, base, path, uint32_t(r.type));
    CallGame(__imp__sub_822FBB98, ctx, base, path, uint32_t(r.speed_kind), 0, 0, 0, 0, r.speed);
    CallGame(__imp__sub_822FBBA8, ctx, base, path, 0);
    for (const auto& p : r.points) {
      for (int k = 0; k < 3; ++k) WriteFloat(scratch + 192 + 4 * uint32_t(k), p[k]);
      const uint32_t point = CallGame(__imp__sub_822FBB30, ctx, base, path, scratch + 192);
      if (!point) continue;
      CallGame(__imp__sub_822EAB78, ctx, base, point, 0, 0, 0, 0, 0, r.duration);
      CallGame(__imp__sub_82240598, ctx, base, point, uint32_t(r.wait));
      WriteFloat(point + 16, 0.0f);  // SetPathPointAdditionalSpeed(0)
    }
    REXLOG_INFO("Cheats: path {:08X} ({} points, type {}, speed {} {:.1f}, wait {}) for {}{}", path, r.points.size(),
                r.type, r.speed_kind, r.speed, r.wait, r.actor_name, existed ? " (it had one: points added)" : "");
  }
}

void Tick(PPCContext& ctx, uint8_t* base, bool in_level) {
  std::vector<Pending> pending;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    pending.swap(g_pending);
  }
  if (!in_level) {
    g_to_stun.clear();  // a level change: those actors are gone
    ReleaseLoading(ctx, base, "left the level");
    if (!pending.empty()) SetResult("not in a level");
    return;
  }
  for (const Pending& request : pending) Spawn(ctx, base, request);
  SendPlatformMessages(ctx, base);
  MakePaths(ctx, base);
  CheckLoading(ctx, base);
  StunPending(ctx, base);
}

bool DebugCommand(std::string_view arguments) {
  // spawn <template> [player 1-4] [count] [ko] [at <x> <y> <z>]   (template "Characters:Roller", or just "Roller")
  std::vector<std::string> words;
  size_t at = 0;
  while (at < arguments.size()) {
    const size_t start = arguments.find_first_not_of(' ', at);
    if (start == std::string_view::npos) break;
    const size_t end = std::min(arguments.find(' ', start), arguments.size());
    words.emplace_back(arguments.substr(start, end - start));
    at = end;
  }
  if (words.empty()) return false;
  // "at x y z" (anywhere after the template): the exact spot.
  float spot[3] = {};
  bool at_spot = false;
  for (size_t i = 1; i + 3 < words.size(); ++i) {
    if (words[i] != "at") continue;
    for (int k = 0; k < 3; ++k) spot[k] = std::strtof(words[i + 1 + k].c_str(), nullptr);
    at_spot = true;
    words.erase(words.begin() + long(i), words.begin() + long(i) + 4);
    break;
  }
  const int player = words.size() > 1 ? std::atoi(words[1].c_str()) - 1 : 0;
  const int count = words.size() > 2 ? std::atoi(words[2].c_str()) : 1;
  const bool knocked_out = words.size() > 3 && words[3] == "ko";
  // A bare name ("Phantom") = a character ("Characters:Phantom"): the
  // template names carry their group, and "Phantom" alone matches no section
  // ("can't be spawned here"), an easy slip at the console.
  if (words[0].find(':') == std::string::npos) words[0] = "Characters:" + words[0];
  Request(words[0], player, count, knocked_out, at_spot ? spot : nullptr);
  return true;
}

void SendPlatformMessage(uint32_t actor, const std::string& name) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_platform_messages.push_back({actor, name});
}

void MakePath(uint32_t actor, const std::vector<std::array<float, 3>>& points, int type, int speed_kind, float speed,
              int wait, float duration) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_paths.push_back({actor, fmt::format("{:08X}", actor), points, type, speed_kind, wait, speed, duration});
}

void NotePathFollower(uint32_t follower, uint32_t actor) {
  if (actor) g_followers[actor] = follower;
}

uint32_t LastSpawned() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_last_spawned;
}

}  // namespace spawn

// -----------------------------------------------------------------------------
// BOSS TITANS OUTSIDE THEIR ARENA (Crunch, Cortex: Characters:CrunchBoss /
// CortexBoss). Both faults below are in CSoundDialogueBehaviour (vtable
// 0x820370A4: the boss's voice lines), whose sounds live in the boss level:
// outside it the boss is simply silent.
// 1. While such an actor is built, its set-up (slot 7, sub_8222CBB8) walks
// up to four object NAMES of its own (+72, 8 bytes each; empty = the name at
// 0x825A7564), looks each one up in the level's named-object registry
// (sub_822DB870 -> registry 0x825A7574) and links to what it finds
// (sub_822D9D20(this +104, object), from 0x8222CC20). It never checks the
// lookup: outside the boss's own level those objects don't exist, the link
// was made to null and sub_822D9F48 read null + 0xC (a fault loop; caught
// with tools/gdb/catch_fault on a cheat spawn, 2026-10-05). A link to
// nothing is now skipped (the original could only crash there).
// -----------------------------------------------------------------------------
extern "C" REX_FUNC(__imp__sub_822D9D20);
extern "C" REX_FUNC(sub_822D9D20) {
  if (ctx.r4.u32 == 0) {
    REXLOG_INFO("Cheats: spawn: a named object the actor links to isn't in this level (skipped)");
    return;
  }
  __imp__sub_822D9D20(ctx, base);
}

// 2. The same set-up stores another lookup at +136 (0x8222CC84), null there
// too; the update (slot 4, sub_8222CD80, at 0x8222CEEC) then calls
// sub_8222CFC8, the only reader of +136, which read null + 8 as soon as the
// boss was jacked (caught the same way). Nothing to play: nothing done.
extern "C" REX_FUNC(__imp__sub_8222CFC8);
extern "C" REX_FUNC(sub_8222CFC8) {
  const uint8_t* p = GuestPtr(base, ctx.r3.u32 + 136);
  if ((uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]) == 0) return;
  __imp__sub_8222CFC8(ctx, base);
}
