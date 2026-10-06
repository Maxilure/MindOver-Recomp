// =============================================================================
// players/more_players.cpp -- see more_players.h (and findings/26)
// =============================================================================
#include "more_players.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <map>
#include <vector>
#include <set>
#include <string>
#include <tuple>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_INT32(local_players, 2, "CrashMoM",
                     "How many local players a level makes room for: 2 (the original) to 4. "
                     "Players 3-4 are spawned like player 2 (in progress: no join for them yet)");

// The originals of the functions rewritten or wrapped below.
extern "C" REX_FUNC(__imp__sub_82266130);  // front end: controller of player r4
extern "C" REX_FUNC(__imp__sub_82266150);  // front end: player r4's controller = r5
extern "C" REX_FUNC(__imp__sub_82266178);  // front end: every player's controller = -1
extern "C" REX_FUNC(__imp__sub_82275210);  // (what the setter calls when a controller is removed)
extern "C" REX_FUNC(__imp__sub_8229F8B8);  // player r3's character
extern "C" REX_FUNC(__imp__sub_8229F8A0);  // player r3's character = r4
extern "C" REX_FUNC(__imp__sub_82283670);  // level: create the spawn event of player r4
extern "C" REX_FUNC(__imp__sub_8227E890);  // the game's operator new (r3 = size)
extern "C" REX_FUNC(__imp__sub_8229C550);  // CActionSpawnPlayer constructor (this, player, r5, name)
extern "C" REX_FUNC(__imp__sub_82287860);  // level: add an event (r3 = level, r4 = event)
extern "C" REX_FUNC(__imp__sub_820B1D10);  // reference-counted pointer store (r3 = slot, r4 = new)
extern "C" REX_FUNC(__imp__sub_82265FB8);  // HaveBothPlayersJoinedGame (r3 = front end)
extern "C" REX_FUNC(__imp__sub_82248EA8);  // the game's state (r3 = game; 5 = playing)
extern "C" REX_FUNC(__imp__sub_822747F8);  // wire player r4's character r6 to its controller
extern "C" REX_FUNC(__imp__sub_821B6E40);  // give controller r4 the input map r3
extern "C" REX_FUNC(__imp__sub_822392A0);  // mask holder (CMaskAttacherBehaviour): attach mask r4
extern "C" REX_FUNC(__imp__sub_82239418);  // mask holder: let go of its mask

REXCVAR_DEFINE_BOOL(debug_coop_trace, false, "CrashMoM",
                    "Debug: log every change of the players' co-op states, each join "
                    "decision and controller assignment (findings/26)");

namespace more_players {
namespace {

constexpr int kMaxPlayers = 4;

// --- Guest addresses of the original tables ------------------------------------
constexpr uint32_t kState = 0x8259B11C;           // u32 [2] -> [4] in place
constexpr uint32_t kOldFlags = 0x8259B124;        // u8 [2]    (moved)
constexpr uint32_t kOldByte126 = 0x8259B126;      // u8        (moved)
constexpr uint32_t kOldByte127 = 0x8259B127;      // u8        (moved)
constexpr uint32_t kOldPointer128 = 0x8259B128;   // u32       (moved)
constexpr uint32_t kOldStartState = 0x824F3C30;   // u32 [2]   (moved)
constexpr uint32_t kSubState = 0x824F3C38;        // u32 [2] -> [4] in place
constexpr uint32_t kOldFloat3C44 = 0x824F3C44;    // float     (moved)
constexpr uint32_t kResetByte = 0x824F3966;       // u8 the reset function sets to 1
constexpr uint32_t kGameGlobal = 0x8259B190;      // the game object
constexpr uint32_t kGameFrontEndControllerList = 56;  // game +56: the controllers (sub_82275210 gets it)
constexpr uint32_t kGameFrontEnd = 52;             // game +52: the front end manager
constexpr uint32_t kFrontEndControllers = 8524;   // front end: players 1-2's controllers (s32 x2)
constexpr uint32_t kControllersArray = 8;         // controllers +8: one object per controller (x4)
constexpr uint32_t kControllerCharacter = 504;    // controller object: the character it drives
constexpr uint32_t kActorIndexBase = 4 * 4;       // (p + 4) * 4: the object's list starts at +16
constexpr uint32_t kSpawnEventSize = 92;          // sub_82283670 allocates 92 bytes
constexpr uint32_t kEventPlayer = 88;             // spawn event: player index
constexpr uint32_t kEventNameHashLow = 48;        // spawn event: low word of the actor's name hash
constexpr uint32_t kEventX = 20;                  // spawn event: position x (float)
constexpr uint32_t kListBytes = 1028;             // one per-player list (count + 256 items)

// --- Our block in guest memory (allocated once by Install) -------------------
// +0 flags[4] (u8), +4 byte 126, +5 byte 127, +8 pointer 128, +12 start state[4],
// +28 the float that was at 0x824F3C44, +32 players 3-4's characters (u32 x2,
// reference-counted like the game object's +16 list), +40 players 3-4's jacked
// titans (u32 x2, reference-counted like the object's +24 list), +48 players
// 3-4's carried-actor names (64 bytes x2, like the object's +108), +176 players
// 3-4's mojo multipliers (u32 x2, like front end +8572), +256 the
// per-player lists of players 3-4: list A (was 0x825A6280) x2, then list B (was
// 0x825A6A88) x2.
constexpr uint32_t kBlockFlags = 0, kBlockByte126 = 4, kBlockByte127 = 5;
constexpr uint32_t kBlockPointer128 = 8, kBlockStartState = 12, kBlockFloat = 28;
constexpr uint32_t kBlockActors = 32, kBlockTitans = 40, kBlockNames = 48, kBlockListA = 256;
constexpr uint32_t kNameBytes = 64;               // game object +108 + 64 * p
constexpr uint32_t kBlockMultipliers = 176;       // players 3-4's mojo multipliers (u32 x2)
constexpr uint32_t kBlockListB = kBlockListA + 2 * kListBytes;
constexpr uint32_t kBlockSize = kBlockListB + 2 * kListBytes;

uint32_t g_block = 0;           // guest address of our block (0 = not installed)
int32_t g_controllers[kMaxPlayers - 2] = {-1, -1};  // players 3-4's controllers
uint32_t g_characters[kMaxPlayers - 2] = {0, 0};     // players 3-4's characters (0 = Carbon Crash)
int g_building_spawn_for = -1;  // while players 3-4's spawn event is built as player 2: which player

// THE HOST OF EACH PLAYER'S MASK, by PLAYER NUMBER (not by object: a level
// change creates new Crashes): g_host_player[rider] = the player it rides, -1 =
// none. Set by every attach of a joined player, cleared when that player leaves
// the mask (B); a level change keeps it, so the next level puts the mask back on
// the same player (wanted: player 3 riding player 2 rode player 1 after a level
// change, because every level start attaches each player-2-style Crash to
// player 1).
int g_host_player[kMaxPlayers] = {-1, -1, -1, -1};

// RIDERS LEFT WITHOUT A HOST: a Crash carrying masks turns into a mask itself;
// its holder is disabled (slot 16) and lets go of every rider. Before, they froze
// in place. Now they wait here (keyed by their old host's Crash) and follow it:
// when it attaches to its own new host, they attach there too.
std::map<uint32_t, std::vector<uint32_t>> g_orphans;

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

int LocalPlayers() { return std::clamp(REXCVAR_GET(local_players), 2, kMaxPlayers); }

void TracePositions();  // (below: needs the characters)

// --debug_coop_trace: logs the four co-op states whenever one changed. Called
// from hooks the game runs often (the character-list guards).
void TraceStates() {
  if (!REXCVAR_GET(debug_coop_trace)) return;
  static uint32_t last[kMaxPlayers] = {~0u, ~0u, ~0u, ~0u};
  uint32_t now[kMaxPlayers];
  bool changed = false;
  for (int p = 0; p < kMaxPlayers; ++p) {
    now[p] = Read32(kState + 4 * p);
    changed |= now[p] != last[p];
  }
  if (!changed) return;
  std::memcpy(last, now, sizeof(last));
  REXLOG_INFO("Co-op: states {} {} {} {} (0 not joined, 1 mask, 2 in game, 3 entering mask)",
              now[0], now[1], now[2], now[3]);
  TracePositions();
}

uint32_t FrontEnd() { return Read32(Read32(kGameGlobal) + kGameFrontEnd); }

// Player p's controller (0-3), -1 = none.
int32_t ControllerOf(int p) {
  if (p < 2) return int32_t(Read32(FrontEnd() + kFrontEndControllers + 4 * p));
  return p < kMaxPlayers ? g_controllers[p - 2] : -1;
}

// Calls a game function with r3-r6; the caller's registers are put back.
uint32_t CallGame(void (*function)(PPCContext&, uint8_t*), PPCContext& ctx, uint8_t* base,
                  uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0, uint32_t r6 = 0) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = r3; ctx.r4.u64 = r4; ctx.r5.u64 = r5; ctx.r6.u64 = r6;
  function(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

}  // namespace

void Install() {
  if (g_block) return;
  g_block = rex::system::kernel_memory()->SystemHeapAlloc(kBlockSize);
  if (!g_block) {
    REXLOG_ERROR("More players: no guest memory for the moved tables");
    return;
  }
  std::memset(Guest(g_block), 0, kBlockSize);
  // What moves: copy the current values (the image's initial data).
  *Guest(g_block + kBlockFlags + 0) = *Guest(kOldFlags + 0);
  *Guest(g_block + kBlockFlags + 1) = *Guest(kOldFlags + 1);
  *Guest(g_block + kBlockByte126) = *Guest(kOldByte126);
  *Guest(g_block + kBlockByte127) = *Guest(kOldByte127);
  Write32(g_block + kBlockPointer128, Read32(kOldPointer128));
  Write32(g_block + kBlockStartState + 0, Read32(kOldStartState + 0));
  Write32(g_block + kBlockStartState + 4, Read32(kOldStartState + 4));
  Write32(g_block + kBlockFloat, Read32(kOldFloat3C44));
  // The per-player lists of players 3-4: empty (count -1, like the game's init).
  for (uint32_t list : {kBlockListA, kBlockListA + kListBytes, kBlockListB,
                        kBlockListB + kListBytes}) {
    Write32(g_block + list, 0xFFFFFFFF);
  }
  // Players 3-4's mojo multipliers start at 1 (the game's reset value).
  Write32(g_block + kBlockMultipliers + 0, 1);
  Write32(g_block + kBlockMultipliers + 4, 1);
  // What grows in place: players 3-4's entries start like player 2's.
  Write32(kState + 8, 0);
  Write32(kState + 12, 0);
  Write32(kSubState + 8, Read32(kSubState + 4));
  Write32(kSubState + 12, Read32(kSubState + 4));
  REXLOG_INFO("More players: co-op tables have room for 4 players (moved to {:08X}); "
              "local players: {}", g_block, LocalPlayers());
}

int LocalPlayerCount() { return LocalPlayers(); }

bool InPlay() {
  // Game state (the game global's +8, sub_82248EA8): 5 = a level is played
  // (its pause and in-game menus included). Read from input threads too: a
  // plain read of guest memory, 0 before the game made its global.
  const uint32_t game = Read32(kGameGlobal);
  return game != 0 && Read32(game + 8) == 5;
}

}  // namespace more_players

using namespace more_players;

// =============================================================================
// Midasm hooks (crash_mom_manifest.toml): point a register at the moved data.
// Each runs right BEFORE the named instruction.
// =============================================================================

// The per-player flag bytes (were 0x8259B124): the register holding the
// array's address, right after the game computed it (6 sites).
void MorePlayersFlagsBase(PPCRegister& r) { r.u64 = g_block + kBlockFlags; }

// sub_82235DC8: "addi r31 = &ptr128 ; stw r30,-20184(r11)" -> both at ours.
void MorePlayersPointer128Store(PPCRegister& r11, PPCRegister& r31) {
  r31.u64 = g_block + kBlockPointer128;
  r11.u64 = g_block + kBlockPointer128 + 20184;  // -20184(r11) = ours
}

// "lwz rD,-20184(r11)" loads of ptr128 (7 script-helper sites): r11 moves.
void MorePlayersPointer128Load(PPCRegister& r11) {
  r11.u64 = g_block + kBlockPointer128 + 20184;
}

// sub_82270608: "stb r11,-20185(r10)" = byte 127 -> ours.
void MorePlayersByte127Store(PPCRegister& r10) { r10.u64 = g_block + kBlockByte127 + 20185; }

// The starting-state array's address, right after the game computed it.
void MorePlayersStartStateBase(PPCRegister& r) { r.u64 = g_block + kBlockStartState; }

// The float that sat at 0x824F3C44 (now sub-state[3]): its users reach it as
// 15428(base), so the base register moves to ours - 15428.
void MorePlayersFloatBase(PPCRegister& r) { r.u64 = g_block + kBlockFloat - 15428; }

// The spawn reads player p's list at list + p * 1028 (r_offset = p * 1028):
// players 3-4 read ours instead.
void MorePlayersListA(PPCRegister& offset, PPCRegister& list) {
  if (offset.u32 >= 2 * kListBytes) list.u64 = g_block + kBlockListA - 2 * kListBytes;
}
void MorePlayersListB(PPCRegister& offset, PPCRegister& list) {
  if (offset.u32 >= 2 * kListBytes) list.u64 = g_block + kBlockListB - 2 * kListBytes;
}

// The game object's player characters (+16 list; manifest: 30 readers + the
// writer). "player < 2" guards let players up to --local_players through:
// Most readers: players 3-4 only once they joined (the original game never
// lets anything see a player 3; a hidden, not-joined player 3 found by "the
// other player" lookups pulled player 2's mask to its spawn point).
bool MorePlayersActorGuard(PPCRegister& player) {
  TraceStates();
  const int32_t p = int32_t(player.u32);
  if (p < 2) return p >= 0;
  const bool pass = p < LocalPlayers() && Read32(kState + 4 * p) != 0;
  {  // --debug_coop_trace: the first 40 decisions for players 3-4
    static int logged = 0;
    if (REXCVAR_GET(debug_coop_trace) && logged < 40) {
      ++logged;
      REXLOG_INFO("Co-op: guard asked for player {}: {}", p + 1, pass ? "pass" : "no");
    }
  }
  return pass;
}
// The join's own lookups and the writer: every local player.
bool MorePlayersActorGuardAlways(PPCRegister& player) {
  return int32_t(player.u32) >= 0 && int32_t(player.u32) < LocalPlayers();
}
// ... a read of (p + 4) * 4 just happened: players 3-4 get ours.
void MorePlayersActorRead(PPCRegister& index, PPCRegister& value) {
  if (index.u32 >= kActorIndexBase + 8 && index.u32 < kActorIndexBase + 16) {
    value.u64 = Read32(g_block + kBlockActors + index.u32 - kActorIndexBase - 8);
  }
}
// ... the writer's slot address (object + (p + 4) * 4): players 3-4 store in ours.
void MorePlayersActorSlot(PPCRegister& index, PPCRegister& slot) {
  if (index.u32 >= kActorIndexBase + 8 && index.u32 < kActorIndexBase + 16) {
    slot.u64 = g_block + kBlockActors + index.u32 - kActorIndexBase - 8;
    if (REXCVAR_GET(debug_coop_trace)) {
      REXLOG_INFO("Co-op: player {}'s character slot written (now {:08X})",
                  (index.u32 - kActorIndexBase) / 4 + 1, Read32(slot.u32));
    }
  }
}

// ... a store "stwx rS,index,base" (Crash leaving the level, sub_821AC768,
// clears its slot): players 3-4's base becomes ours minus their index.
void MorePlayersActorClearBase(PPCRegister& index, PPCRegister& base_register) {
  if (index.u32 >= kActorIndexBase + 8 && index.u32 < kActorIndexBase + 16) {
    base_register.u64 = g_block + kBlockActors - (kActorIndexBase + 8);
    if (REXCVAR_GET(debug_coop_trace)) {
      REXLOG_INFO("Co-op: player {}'s character leaves the level (slot cleared)",
                  (index.u32 - kActorIndexBase) / 4 + 1);
    }
  }
}

// The game object's JACKED TITANS (+24 / +28: the titan each player is riding,
// 0 = on foot; written by CJackingBehaviour, sub_8214D660 / sub_8214D778, and by
// Crash leaving the level, sub_821AC768). Found 2026-10-04 after a level change
// with player 3 or 4 on foot faulted: code reads them as (p + 6) * 4 behind a
// "player < 2" check, and where our character-list guard had already let a
// player 3-4 through (sub_822420D8) the read landed on the object's +32 / +36
// (another pointer / the per-player bytes 01010101). Players 3-4's titans live in
// our block (+40); every guard lets every local player through (an empty slot
// = "not in a titan", the same path as a failed guard at all 14 sites).
constexpr uint32_t kTitanIndexBase = 6 * 4;       // (p + 6) * 4: the object's +24 list
bool MorePlayersTitanGuard(PPCRegister& player) {
  return int32_t(player.u32) >= 0 && int32_t(player.u32) < LocalPlayers();
}
// ... a read of (p + 6) * 4 just happened: players 3-4 get ours.
void MorePlayersTitanRead(PPCRegister& index, PPCRegister& value) {
  if (index.u32 >= kTitanIndexBase + 8 && index.u32 < kTitanIndexBase + 16) {
    value.u64 = Read32(g_block + kBlockTitans + index.u32 - kTitanIndexBase - 8);
  }
}
// ... "stwx rS,index,base" (a titan slot cleared): players 3-4's base = ours
// minus their index (the base register is dead after the store at both sites).
void MorePlayersTitanStoreBase(PPCRegister& index, PPCRegister& base_register) {
  if (index.u32 >= kTitanIndexBase + 8 && index.u32 < kTitanIndexBase + 16) {
    base_register.u64 = g_block + kBlockTitans - (kTitanIndexBase + 8);
  }
}
// ... CJackingBehaviour's "slot = object + (p + 6) * 4" right before the
// reference-counted store sub_820B1D10(r3 = slot, r4 = titan).
void MorePlayersTitanSlot(PPCRegister& index, PPCRegister& slot) {
  if (index.u32 >= kTitanIndexBase + 8 && index.u32 < kTitanIndexBase + 16) {
    slot.u64 = g_block + kBlockTitans + index.u32 - kTitanIndexBase - 8;
  }
  if (REXCVAR_GET(debug_coop_trace)) {
    REXLOG_INFO("Co-op: player {} jacks a titan", (index.u32 - kTitanIndexBase) / 4 + 1);
  }
}

// "FOR EACH PLAYER" LOOPS over the game object's lists (findings/26 s.16): a
// counter rO = base + 4p walks the characters (+16) and titans (+24) up to 2
// players: "lwzx rD,rO,r3" or "add rX,rO,r3 ; lwz rD,+8/-8(rX)". 17 loops
// (cutscene messages to every player, trigger volumes, enemy targeting checks,
// rumble, unlocks, the nearest player, ...) now run for every local player:
// their guards open (MorePlayersTitanGuard), each read gives players 3-4 ours,
// and the loop's end jumps back to its top while players remain. Base 16: rO
// = 16 + 4p (lwzx = character, +8 = titan); base 24: rO = 24 + 4p (lwzx =
// titan, -8 = character).
uint32_t OurCharacterOrTitan(bool titan, uint32_t offset, uint32_t base, uint32_t original) {
  if (offset < base + 8 || offset >= base + 4 * kMaxPlayers) return original;
  return Read32(g_block + (titan ? kBlockTitans : kBlockActors) + offset - base - 8);
}
void MorePlayersLoopChar16(PPCRegister& o, PPCRegister& v) { v.u64 = OurCharacterOrTitan(false, o.u32, 16, v.u32); }
void MorePlayersLoopTitan16(PPCRegister& o, PPCRegister& v) { v.u64 = OurCharacterOrTitan(true, o.u32, 16, v.u32); }
void MorePlayersLoopChar24(PPCRegister& o, PPCRegister& v) { v.u64 = OurCharacterOrTitan(false, o.u32, 24, v.u32); }
void MorePlayersLoopTitan24(PPCRegister& o, PPCRegister& v) { v.u64 = OurCharacterOrTitan(true, o.u32, 24, v.u32); }
// The loop's end ("cmpwi rO,24|32 ; blt top"), rO already advanced: true = go
// round again (for players 1-2 exactly what the original does).
bool MorePlayersLoopEnd16(PPCRegister& o) { return o.u32 < 16 + 4u * uint32_t(LocalPlayers()); }
bool MorePlayersLoopEnd24(PPCRegister& o) { return o.u32 < 24 + 4u * uint32_t(LocalPlayers()); }
// A loop whose counter IS the player number ("cmpwi rP,2 ; blt top"): true =
// back to the top while players remain (the level loader's carried-titan
// packages, sub_822E5E00: findings/26 s.22).
bool MorePlayersLoopEndPlayer(PPCRegister& p) { return int32_t(p.u32) < LocalPlayers(); }

// The join (front end): r18 = the controller that pressed START, r31 = the
// player it joins as (the game picked 1 = player 2 if free). Returns true to
// skip this controller.
// SOCKET N = PLAYER N (2026-10-04, findings/26 s.21): the controller in socket
// N joins as player N, never as "the first free player". The original took
// any free socket for player 2 (and ours for players 3-4 the first free one),
// so the game's player 2 could be socket 4: the Controls menu's "Player 2"
// (input/players.h) then drove someone else, and moving devices there
// "didn't work reliably". A socket beyond --local_players, or whose player
// already plays, is skipped.
bool MorePlayersJoinPick(PPCRegister& controller, PPCRegister& player) {
  const int c = int(int32_t(controller.u32));
  const bool can_join = c >= 1 && c < LocalPlayers() && Read32(kState + 4 * c) == 0;
  if (REXCVAR_GET(debug_coop_trace)) {
    if (can_join) {
      REXLOG_INFO("Co-op: controller {} pressed START: joins as player {}", c, c + 1);
    } else {
      REXLOG_INFO("Co-op: controller {} pressed START: no join (player {} {})", c, c + 1,
                  c >= LocalPlayers() ? "is beyond --local_players" : "already plays");
    }
  }
  if (!can_join) return true;
  player.u64 = uint32_t(c);
  return false;
}

// --debug_coop_trace: what each mask update sees (CMaskAttachableBehaviour
// update sub_82237778, after it read its owner player's co-op state; it gets
// there only while the mask is ATTACHED, flag +144 bit 0x80). Found 2026-10-04:
// with a player 3, only player 3's mask ever got here: player 1 seems to carry
// ONE mask, and the last player-2-style Crash spawned takes it (findings/26 s.10).
void MorePlayersDebugMaskState(PPCRegister& r3, PPCRegister& r8, PPCRegister& r30, PPCRegister& r31) {
  static std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>> seen;
  if (REXCVAR_GET(debug_coop_trace) && seen.insert({r3.u32, r8.u32, r30.u32, r31.u32}).second) {
    REXLOG_INFO("Co-op: mask {:08X} of actor {:08X}: player {} state {}", r31.u32, r30.u32,
                int32_t(r3.u32) + 1, r8.u32);
  }
}

// =============================================================================
// Rewritten functions (small getters / setters / the reset)
// =============================================================================

// Returns the pointer that was at 0x8259B128.
extern "C" REX_FUNC(sub_82234AA0) { ctx.r3.u64 = Read32(g_block + kBlockPointer128); }

// Byte 126: get / set.
extern "C" REX_FUNC(sub_822635E8) { ctx.r3.u64 = *Guest(g_block + kBlockByte126); }
extern "C" REX_FUNC(sub_822635F8) { *Guest(g_block + kBlockByte126) = uint8_t(ctx.r4.u32); }

// Byte 127: get.
extern "C" REX_FUNC(sub_82270678) { ctx.r3.u64 = *Guest(g_block + kBlockByte127); }

// Player r3's starting state.
extern "C" REX_FUNC(sub_82235C18) {
  const uint32_t player = std::min<uint32_t>(ctx.r3.u32, kMaxPlayers - 1);
  ctx.r3.u64 = Read32(g_block + kBlockStartState + 4 * player);
}

// "Reset co-op": every player not joined, player 1 starting in game. The
// original (sub_82234B58) wrote exactly these values for two players.
extern "C" REX_FUNC(sub_82234B58) {
  for (int p = 0; p < kMaxPlayers; ++p) {
    Write32(kSubState + 4 * p, 9);
    Write32(kState + 4 * p, 0);
    *Guest(g_block + kBlockFlags + p) = 0;
    Write32(g_block + kBlockStartState + 4 * p, p == 0 ? 2 : 0);
  }
  *Guest(kResetByte) = 1;
  std::fill(std::begin(g_host_player), std::end(g_host_player), -1);  // nobody rides anyone
  Write32(g_block + kBlockMultipliers + 0, 1);  // mojo multipliers back to 1
  Write32(g_block + kBlockMultipliers + 4, 1);
  g_orphans.clear();
}

// GetCurrentNumPlayers (sub_82270590): how many players are IN GAME (state 2).
extern "C" REX_FUNC(sub_82270590) {
  uint32_t n = 0;
  for (int p = 0; p < kMaxPlayers; ++p) n += Read32(kState + 4 * p) == 2;
  ctx.r3.u64 = n;
}

// Controllers: the front end keeps players 1-2's (+8524); 3-4's are here.
// g_lookup_shift: while the loading screen's paws run their second pass for
// players 3-4 (more_players_loading.cpp), "player 0 / 1" means player 3 / 4.
namespace more_players {
thread_local int g_lookup_shift = 0;
void ShiftControllerLookup(int shift) { g_lookup_shift = shift; }
}  // namespace more_players
extern "C" REX_FUNC(sub_82266130) {
  const int32_t player = int32_t(ctx.r4.u32) + more_players::g_lookup_shift;
  ctx.r4.u64 = uint32_t(player);
  if (player >= 2 && player < kMaxPlayers) {
    ctx.r3.u64 = uint32_t(g_controllers[player - 2]);
    return;
  }
  __imp__sub_82266130(ctx, base);
}
extern "C" REX_FUNC(sub_82266150) {
  const int32_t player = int32_t(ctx.r4.u32);
  // Socket N = player N (see MorePlayersJoinPick): the title's START makes the
  // socket that pressed it player 1's; outside play every device answers on
  // socket 1 (input/players.h), so that's socket 1 already, and this keeps it
  // so if anything else asks for another one.
  if (player >= 0 && player < kMaxPlayers && int32_t(ctx.r5.u32) >= 0 &&
      int32_t(ctx.r5.u32) != player) {
    REXLOG_INFO("Co-op: player {} was given controller {}: socket {} instead (socket N = player N)",
                player + 1, int32_t(ctx.r5.u32), player);
    ctx.r5.u64 = uint32_t(player);
  }
  if (REXCVAR_GET(debug_coop_trace)) {
    REXLOG_INFO("Co-op: player {} controller = {}", player + 1, int32_t(ctx.r5.u32));
  }
  if (player >= 2 && player < kMaxPlayers) {
    g_controllers[player - 2] = int32_t(ctx.r5.u32);
    if (int32_t(ctx.r5.u32) == -1) {  // as the original: tell the game's controller list
      ctx.r3.u64 = Read32(Read32(kGameGlobal) + kGameFrontEndControllerList);
      __imp__sub_82275210(ctx, base);
    }
    return;
  }
  __imp__sub_82266150(ctx, base);
}
extern "C" REX_FUNC(sub_82266178) {
  __imp__sub_82266178(ctx, base);
  g_controllers[0] = g_controllers[1] = -1;
}

// HaveBothPlayersJoinedGame (r3 = front end). The original: in play (game
// state 5) players 1 and 2 both have a co-op state other than "not joined",
// else both have a controller. Its 13 callers ask two different questions,
// identical with two players but not with more:
//   * the join (sub_82264988, call at 0x82265064): "is there room for one
//     more?" -> with more than two local players: have ALL of them joined;
//   * the other 12, all in the front end's compiled fight tree: the pause
//     screens (PauseScreen 868 decision sub_82121B48, NoSavePauseScreen 901
//     sub_82121EA8, DemoPauseScreen 936 sub_821222D8) and their TwoPlayer /
//     OnePlayer branch checks (sub_82122E38/E80 = nodes 870/871, F38/F80 =
//     903/904, 23010/23058 = 937/938): "is this a co-op game?" -> the
//     TwoPlayer branch = the pause page WITH "Drop Out" (InGame_Pause2Player).
//     With more than two: at least TWO players in game.
// Found 2026-10-06: with --local_players=4 and fewer than four joined, nobody's
// pause menu offered Drop Out (players 1-2 included), because those 12 heard
// the join's "all of them".
constexpr uint32_t kJoinCallReturn = 0x82265068;  // return address of the join's call
extern "C" REX_FUNC(sub_82265FB8) {
  const int n = LocalPlayers();
  if (n <= 2) {
    __imp__sub_82265FB8(ctx, base);
    return;
  }
  const bool join = uint32_t(ctx.lr) == kJoinCallReturn;
  const bool playing = CallGame(__imp__sub_82248EA8, ctx, base, Read32(kGameGlobal)) == 5;
  int count = 0;
  for (int p = 0; p < n; ++p) {
    count += (playing ? Read32(kState + 4 * p) != 0 : ControllerOf(p) != -1) ? 1 : 0;
  }
  ctx.r3.u64 = (join ? count == n : count >= 2) ? 1 : 0;
}

// Wires player r4's character r6 (and input map r5) to its controller (r3 =
// the controllers). For a player without a controller the original takes
// "the one the OTHER player doesn't use", a two-player rule: players 3-4 take
// their own number's controller if nobody has it, else the first free one.
extern "C" REX_FUNC(sub_822747F8) {
  const int p = int(int32_t(ctx.r4.u32));
  if (p < 2 || p >= kMaxPlayers) {
    __imp__sub_822747F8(ctx, base);
    return;
  }
  int32_t c = ControllerOf(p);
  if (c < 0) {
    auto used = [](int32_t pad) {
      for (int q = 0; q < kMaxPlayers; ++q) {
        if (ControllerOf(q) == pad) return true;
      }
      return false;
    };
    for (int32_t pad : {int32_t(p), 0, 1, 2, 3}) {
      if (!used(pad)) { c = pad; break; }
    }
    if (c < 0) return;  // four controllers taken: leave it unwired
  }
  if (REXCVAR_GET(debug_coop_trace)) {
    REXLOG_INFO("Co-op: player {}'s character {:08X} wired to controller {}", p + 1, ctx.r6.u32, c);
  }
  const uint32_t controller = Read32(Read32(ctx.r3.u32 + kControllersArray) + 4 * c);
  Write32(controller + kControllerCharacter, ctx.r6.u32);
  CallGame(__imp__sub_821B6E40, ctx, base, ctx.r5.u32, controller);
}

// Which player is this character? (sub_8226FD20: r3 = game object, r4 =
// character -> 0/1 by the object's +16 list, else -1.) Part 1 of the game's
// "player number of an actor" (sub_8226FCD8, 26 callers; then the +24 list,
// then +32). Players 3-4's characters are in our block.
extern "C" REX_FUNC(sub_8226FD20) {
  const uint32_t object = ctx.r3.u32, character = ctx.r4.u32;
  int32_t player = -1;
  if (character) {
    for (int p = 0; p < LocalPlayers() && player < 0; ++p) {
      const uint32_t slot = p < 2 ? object + 16 + 4 * p : g_block + kBlockActors + 4 * (p - 2);
      if (Read32(slot) == character) player = p;
    }
  }
  ctx.r3.u64 = uint32_t(player);
  {  // --debug_coop_trace: each distinct answer once
    static std::set<std::pair<uint32_t, int32_t>> seen;
    if (REXCVAR_GET(debug_coop_trace) && seen.insert({character, player}).second) {
      REXLOG_INFO("Co-op: which player is {:08X}? {} (asked from {:08X})", character, player + 1,
                  uint32_t(ctx.lr));
    }
  }
}

// Player r4's body (sub_8226FC88, r3 = game object): its jacked titan if it
// rides one, else its character; 0 for anyone else. The original: players 1-2.
extern "C" REX_FUNC(sub_8226FC88) {
  const uint32_t object = ctx.r3.u32;
  const int32_t p = int32_t(ctx.r4.u32);
  uint32_t body = 0;
  if (p >= 0 && p < kMaxPlayers) {
    const uint32_t titan = p < 2 ? Read32(object + 24 + 4 * p) : Read32(g_block + kBlockTitans + 4 * (p - 2));
    body = titan ? titan : p < 2 ? Read32(object + 16 + 4 * p) : Read32(g_block + kBlockActors + 4 * (p - 2));
  }
  ctx.r3.u64 = body;
}

// Whose jacked titan is r4? (sub_8226FD60, r3 = game object; part 2 of
// sub_8226FCD8 "player number of an actor") -> player, else -1.
extern "C" REX_FUNC(sub_8226FD60) {
  const uint32_t object = ctx.r3.u32, titan = ctx.r4.u32;
  int32_t player = -1;
  for (int p = 0; titan && p < kMaxPlayers && player < 0; ++p) {
    const uint32_t slot = p < 2 ? object + 24 + 4 * p : g_block + kBlockTitans + 4 * (p - 2);
    if (Read32(slot) == titan) player = p;
  }
  ctx.r3.u64 = uint32_t(player);
}

// Player r4's CARRIED-ACTOR NAME (game object +108 + 64 * p): written before a
// level change (sub_8229CB78), read when the player spawns ON FOOT in the next
// level (sub_8229C6F8 at 0x8229C954): a non-empty name spawns that actor by
// script (a titan brought along). For a player 3 the original's address is
// object +236.., which holds other fields (+240 a word, +244 / +248 the level and
// entry of the last level change): a garbage "name", and the spawn script
// faulted (read of guest 0x11, Lua code; found 2026-10-04 with a gdb backtrace
// at the access-violation callback after a level change with player 3 on foot).
// Players 3-4's names are in our block.
uint32_t NameAddress(uint32_t object, uint32_t player) {
  return player < 2 ? object + 108 + kNameBytes * player
                    : g_block + kBlockNames + kNameBytes * std::min<uint32_t>(player - 2, 1);
}
extern "C" REX_FUNC(sub_822705F0) { ctx.r3.u64 = NameAddress(ctx.r3.u32, ctx.r4.u32); }
extern "C" REX_FUNC(__imp__sub_82357E00);  // bounded string copy (r3 = to, r4 = from, r5 = size)
extern "C" REX_FUNC(sub_822705D0) {
  const uint32_t to = NameAddress(ctx.r3.u32, ctx.r4.u32), from = ctx.r5.u32;
  ctx.r3.u64 = to; ctx.r4.u64 = from; ctx.r5.u64 = kNameBytes;
  __imp__sub_82357E00(ctx, base);
}

// Characters (0x8259B1C0, per player): the number of the character a player
// plays: 0 = Crash (with a player number other than 1 the game swaps in
// player 2's texture: Carbon Crash, the white variant), 9 = Coco, who takes
// player 2's place later in the story (seen in a save from the Ice Prison
// way). Players 3-4 keep their own, Carbon Crash (0) until something sets it
// (a first version copied player 2's: two Cocos). Outfits picked in Crash's
// house set it for players 1-2 (sub_8229F8A0).
extern "C" REX_FUNC(sub_8229F8B8) {
  const uint32_t player = ctx.r3.u32;
  if (player >= 2 && player < kMaxPlayers) {
    ctx.r3.u64 = g_characters[player - 2];
  } else if (player == 1 && g_building_spawn_for >= 2) {
    ctx.r3.u64 = g_characters[g_building_spawn_for - 2];  // the event is built as player 2
  } else {
    __imp__sub_8229F8B8(ctx, base);
  }
  {  // --debug_coop_trace: each player's character number, once per value
    static std::set<std::pair<uint32_t, uint32_t>> seen;
    if (REXCVAR_GET(debug_coop_trace) && seen.insert({player, ctx.r3.u32}).second) {
      REXLOG_INFO("Co-op: player {}'s character = {}", player + 1, ctx.r3.u32);
    }
  }
}
extern "C" REX_FUNC(sub_8229F8A0) {
  if (ctx.r3.u32 >= 2) {
    if (ctx.r3.u32 < kMaxPlayers) g_characters[ctx.r3.u32 - 2] = ctx.r4.u32;
    return;
  }
  __imp__sub_8229F8A0(ctx, base);
}

// A level creates its players' spawn events (r3 = level, r4 = player, r5, r6 =
// name). After player 2's: players 3..N, built as player 2 and renumbered.
extern "C" REX_FUNC(sub_82283670) {
  const PPCContext call = ctx;
  if (call.r4.u32 == 0 && g_block) {
    g_orphans.clear();  // (riders of the old level's Crashes)
    // A level's players are being created: let go of the previous level's
    // players 3-4 (the game object's list does the same for 1-2).
    for (uint32_t slot = 0; slot < 8; slot += 4) {
      if (REXCVAR_GET(debug_coop_trace)) {
        const uint32_t c = Read32(g_block + kBlockActors + slot);
        REXLOG_INFO("Co-op: new level: player {}'s old character {:08X} (vtable {:08X})", slot / 4 + 3, c,
                    c ? Read32(c) : 0);
      }
      for (uint32_t list : {kBlockActors, kBlockTitans}) {  // (their titans too)
        if (Read32(g_block + list + slot)) {
          CallGame(__imp__sub_820B1D10, ctx, base, g_block + list + slot, 0);
        }
      }
    }
  }
  __imp__sub_82283670(ctx, base);
  if (call.r4.u32 != 1 || LocalPlayers() <= 2 || !g_block) return;
  const PPCContext after = ctx;
  for (int p = 2; p < LocalPlayers(); ++p) {
    ctx = call;
    ctx.r3.u64 = kSpawnEventSize;
    __imp__sub_8227E890(ctx, base);
    const uint32_t event = ctx.r3.u32;
    if (!event) break;
    ctx = call;
    ctx.r3.u64 = event;
    ctx.r4.u64 = 1;  // as player 2: its spawn point and name; the character is player p's
    g_building_spawn_for = p;  // (the constructor asks sub_8229F8B8 for player 2's character)
    __imp__sub_8229C550(ctx, base);
    g_building_spawn_for = -1;
    Write32(event + kEventPlayer, uint32_t(p));
    Write32(event + kEventNameHashLow, Read32(event + kEventNameHashLow) ^ uint32_t(p));
    float x;
    uint32_t bits = Read32(event + kEventX);
    std::memcpy(&x, &bits, 4);
    x += 1.5f * float(p - 1);  // a little to the side of player 2's spot
    std::memcpy(&bits, &x, 4);
    Write32(event + kEventX, bits);
    ctx = call;
    ctx.r4.u64 = event;
    __imp__sub_82287860(ctx, base);
    REXLOG_INFO("More players: player {} spawn event added ({:08X})", p + 1, event);
  }
  ctx = after;
}

// =============================================================================
// Masks (findings/26 section 12; in progress). The holder (CMaskAttacherBehaviour,
// vtable 0x8203801C, one per Crash) keeps ONE rider at +52.
// =============================================================================

extern "C" REX_FUNC(__imp__sub_82238F18);  // mask holder: message handler (r4 = owner, r5 = message)
extern "C" REX_FUNC(__imp__sub_82237960);  // mask (CMaskAttachableBehaviour): message handler
extern "C" REX_FUNC(__imp__sub_82235B10);  // co-op: "the other player's" Crash (r4 = mine)
extern "C" REX_FUNC(__imp__sub_82239250);  // mask holder, slot 16: disabled (let go)

namespace more_players {
namespace {

// Extra riders per holder (guest address of the CMaskAttacherBehaviour): the
// original's one rider stays at holder +52; up to two more live here. Each
// pointer carries the reference the holder's store gave it (sub_820B1D10).
constexpr uint32_t kHolderRider = 52;   // holder: its (main) rider, the rider's Crash
constexpr uint32_t kHolderOwner = 28;   // holder: the Crash carrying it
constexpr int kMaxExtraRiders = 2;      // three masks per Crash (the design for more players)
constexpr float kExtraRiderSpacing = 0.45f;  // extra masks: this far to each side (game units)
std::map<uint32_t, std::vector<uint32_t>> g_extra_riders;
uint32_t g_leaving_rider = 0;           // set by the detach branch's hook, used once
std::map<uint32_t, uint32_t> g_host_of;  // rider's Crash -> the holder carrying it
bool g_releasing_all = false;   // inside a holder's "disabled" (slot 16): every rider goes
bool g_attaching_extra = false;  // inside an extra rider's attach: the original's own
                                 // "let go of the current rider" call must stay a no-op

// Keeps g_host_of in step with a holder after an attach / detach: its main
// rider and extras are carried by it; riders it no longer has are not.
void SyncHosts(uint32_t holder) {
  for (auto it = g_host_of.begin(); it != g_host_of.end();) {
    it = it->second == holder ? g_host_of.erase(it) : std::next(it);
  }
  if (const uint32_t main_rider = Read32(holder + kHolderRider)) g_host_of[main_rider] = holder;
  if (auto it = g_extra_riders.find(holder); it != g_extra_riders.end()) {
    for (uint32_t rider : it->second) g_host_of[rider] = holder;
  }
}

// A character's world position: Crash +44 points to its world matrix (4x4
// floats, translation in the last row, +48). Found 2026-10-04: the mask holder
// builds the attach point from it (sub_822392A0: owner +44 into sub_824241D0);
// checked: right after spawning, the three players' translations were their
// spawn events' positions (364/366/367.5, 3-4, -278).
constexpr uint32_t kCharacterWorldMatrix = 44;
bool PositionOf(uint32_t character, float out[3]) {
  if (!character) return false;
  const uint32_t matrix = Read32(character + kCharacterWorldMatrix);
  if (matrix < 0x40000000) return false;
  for (int i = 0; i < 3; ++i) {
    const uint32_t b = Read32(matrix + 48 + 4 * i);
    std::memcpy(&out[i], &b, 4);
  }
  return true;
}

// Player p's Crash: the game object's list for players 1-2, ours for 3-4.
constexpr uint32_t kGameObjectCharacters = 0x825B0008 + 16;  // sub_82270608's singleton +16
uint32_t CharacterOf(int p) {
  return p < 2 ? Read32(kGameObjectCharacters + 4 * p)
               : Read32(g_block + kBlockActors + 4 * (p - 2));
}

std::vector<uint32_t>* ExtrasOf(uint32_t holder) {
  auto it = g_extra_riders.find(holder);
  return it == g_extra_riders.end() ? nullptr : &it->second;
}

// Which player a Crash is (the game object's list / ours), -1 = none.
int PlayerOf(uint32_t character) {
  for (int p = 0; character && p < kMaxPlayers; ++p) {
    if (CharacterOf(p) == character) return p;
  }
  return -1;
}

// A Crash's mask holder: Crash's message handler (this = Crash + 0x40) keeps it
// at +260 (0x821AD868 "lwz r3,260(r31)"), so Crash + 324; checked against the
// holder's own owner (+28) before use.
uint32_t HolderOf(uint32_t character) {
  if (!character) return 0;
  const uint32_t holder = Read32(character + 0x40 + 260);
  return holder >= 0x40000000 && Read32(holder + kHolderOwner) == character ? holder : 0;
}


}  // namespace
}  // namespace more_players

// "The other player's" Crash (sub_82235B10, r4 = mine), asked when a player
// turns into a mask (HideAndAttachToOtherPlayer: the mask rides it), leaves
// the mask (the detach message goes to it) and by the co-op update. The
// original: player 1's if I'm not player 1, else player 2's. With more players
// (the design for more players): a mask that rides someone -> its HOST (a first version
// answered "nearest" here too: player 3's leave-mask message went to player 2,
// who wasn't carrying it, and player 3 stayed on player 1's holder); anyone
// else -> the NEAREST Crash on foot (co-op state 2) other than mine; the
// original's answer when there is none.
extern "C" REX_FUNC(sub_82235B10) {
  const uint32_t mine = ctx.r4.u32;
  float me[3];
  uint32_t best = 0;
  float best_distance = 0;
  if (auto it = g_host_of.find(mine); LocalPlayers() > 2 && it != g_host_of.end()) {
    best = Read32(it->second + kHolderOwner);  // my host
  } else if (LocalPlayers() > 2 && PositionOf(mine, me)) {
    for (int p = 0; p < LocalPlayers(); ++p) {
      const uint32_t other = CharacterOf(p);
      float at[3];
      if (!other || other == mine || Read32(kState + 4 * p) != 2 || !PositionOf(other, at)) continue;
      const float d = (at[0] - me[0]) * (at[0] - me[0]) + (at[1] - me[1]) * (at[1] - me[1]) +
                      (at[2] - me[2]) * (at[2] - me[2]);
      if (!best || d < best_distance) { best = other; best_distance = d; }
    }
  }
  const uint32_t lr = uint32_t(ctx.lr);
  if (!best) {
    __imp__sub_82235B10(ctx, base);
  } else {
    ctx.r3.u64 = best;
  }
  if (REXCVAR_GET(debug_coop_trace)) {  // each distinct (caller, mine, answer) once
    static std::set<std::tuple<uint32_t, uint32_t, uint32_t>> seen;
    if (seen.insert({lr, mine, ctx.r3.u32}).second) {
      REXLOG_INFO("Co-op: other player of {:08X} (asked from {:08X}): {:08X}{}", mine, lr, ctx.r3.u32,
                  !best ? std::string(" (original rule)")
                  : best_distance == 0 ? std::string(", my host")
                  : fmt::format(", the nearest Crash on foot ({:.1f} away)", std::sqrt(best_distance)));
    }
  }
}

// Crash's mask messages (manifest: attach and detach branches of sub_821ACAD8).
void MorePlayersMaskMessage(PPCRegister& message, PPCRegister& crash) {
  const uint32_t kind = Read32(message.u32 + 16), rider = Read32(message.u32 + 24);
  if (kind == 5) g_leaving_rider = rider;
  if (REXCVAR_GET(debug_coop_trace)) {
    REXLOG_INFO("Co-op: Crash {:08X} gets mask message {} ({}) for rider {:08X}", crash.u32, kind,
                kind == 4 ? "attach" : kind == 5 ? "detach" : "?", rider);
  }
}

// ---// Attach mask r4 (a rider's Crash) to holder r3. With a rider already on, the
// original first lets go of it (one rider per Crash); here the newcomer
// becomes an extra rider instead (up to three in all): the original attaches
// it with the main slot briefly empty, then the main rider goes back.
extern "C" REX_FUNC(sub_822392A0) {
  uint32_t holder = ctx.r3.u32;
  const uint32_t mask = ctx.r4.u32, lr = uint32_t(ctx.lr);
  // A joined player whose mask rode someone before (a level change, findings/26
  // s.15) goes back to that player, if that player is on foot.
  const int rider_player = PlayerOf(mask);
  if (LocalPlayers() > 2 && rider_player >= 0 && Read32(kState + 4 * rider_player) != 0) {
    const int q = g_host_player[rider_player];
    const uint32_t wanted = q >= 0 && q != rider_player && Read32(kState + 4 * q) == 2
                                ? HolderOf(CharacterOf(q)) : 0;
    if (wanted && wanted != holder) {
      if (REXCVAR_GET(debug_coop_trace)) {
        REXLOG_INFO("Co-op: player {}'s mask goes back to player {} (holder {:08X} instead of {:08X})",
                    rider_player + 1, q + 1, wanted, holder);
      }
      holder = wanted;
      ctx.r3.u64 = holder;
    }
  }
  const uint32_t main_rider = Read32(holder + kHolderRider);
  std::vector<uint32_t>& extras = g_extra_riders[holder];
  const bool already = std::find(extras.begin(), extras.end(), mask) != extras.end();
  const char* what = "main";
  if (LocalPlayers() <= 2 || !main_rider || !mask || main_rider == mask) {
    __imp__sub_822392A0(ctx, base);
  } else if (already) {
    ctx.r3.u64 = 1;
    what = "already extra";
  } else if (int(extras.size()) < kMaxExtraRiders) {
    Write32(holder + kHolderRider, 0);          // (raw: the main rider keeps its reference)
    g_attaching_extra = true;  // (the original first lets go of the current rider: none now)
    __imp__sub_822392A0(ctx, base);             // attaches the mask, holder +52 = it (+ref)
    g_attaching_extra = false;
    extras.push_back(Read32(holder + kHolderRider));
    Write32(holder + kHolderRider, main_rider);  // (raw again: the extra keeps its reference)
    what = "extra";
  } else {
    ctx.r3.u64 = 0;  // full: three masks already
    what = "refused (full)";
  }
  SyncHosts(holder);
  const uint32_t attached = ctx.r3.u32;
  if (REXCVAR_GET(debug_coop_trace)) {
    REXLOG_INFO("Co-op: holder {:08X} (owner {:08X}) attach {:08X} as {}: main {:08X}, {} extra (from {:08X})",
                holder, Read32(holder + kHolderOwner), mask, what, Read32(holder + kHolderRider),
                extras.size(), lr);
  }
  // Who rides whom, by player number (kept across levels; joined players only:
  // hidden, not-joined Crashes ride player 1 at every level start).
  const int host_player = PlayerOf(Read32(holder + kHolderOwner));
  if (rider_player >= 0 && host_player >= 0 && Read32(kState + 4 * rider_player) != 0 &&
      g_host_of.count(mask)) {
    g_host_player[rider_player] = host_player;
  }
  // This mask carried riders on foot a moment ago: they follow it here.
  if (auto it = g_orphans.find(mask); it != g_orphans.end()) {
    const std::vector<uint32_t> riders = std::move(it->second);
    g_orphans.erase(it);
    for (uint32_t rider : riders) {
      if (REXCVAR_GET(debug_coop_trace)) {
        REXLOG_INFO("Co-op: rider {:08X} follows its host {:08X} to holder {:08X}", rider, mask, holder);
      }
      CallGame(sub_822392A0, ctx, base, holder, rider);
    }
  }
  ctx.r3.u64 = attached;
}

// Holder r3 lets go of its rider. The original has one; with extras, the one
// that asked (Crash's detach message, remembered by MorePlayersMaskMessage)
// leaves: an extra one by putting it in the main slot for the call, the main
// one as before, and then the first extra takes the main slot.
extern "C" REX_FUNC(sub_82239418) {
  if (g_attaching_extra) {  // the attach's own call, main slot empty for it: as the original
    __imp__sub_82239418(ctx, base);
    return;
  }
  const uint32_t holder = ctx.r3.u32, lr = uint32_t(ctx.lr);
  const uint32_t leaving = g_leaving_rider;
  g_leaving_rider = 0;
  std::vector<uint32_t>* extras = ExtrasOf(holder);
  const uint32_t main_rider = Read32(holder + kHolderRider);
  const bool carried = leaving == main_rider ||
                       (extras && std::find(extras->begin(), extras->end(), leaving) != extras->end());
  if (leaving && !g_releasing_all && !carried) {
    // A detach message for a rider this holder doesn't carry: the original would
    // let go of its (other) rider. Seen 2026-10-04: player 4 rode nobody after its
    // host turned into a mask, its B sent the detach to the nearest Crash (player
    // 1), and player 2's mask fell off player 1. Nothing to do here.
    if (REXCVAR_GET(debug_coop_trace)) {
      REXLOG_INFO("Co-op: holder {:08X} (owner {:08X}) asked to let go of {:08X}, which it doesn't carry: ignored",
                  holder, Read32(holder + kHolderOwner), leaving);
    }
    ctx.r3.u64 = 0;
    return;
  }
  if (leaving && !g_releasing_all) {
    if (const int r = PlayerOf(leaving); r >= 0) g_host_player[r] = -1;  // it leaves the mask
  }
  if (g_releasing_all) {
    // The holder is disabled. If its Crash is turning into a mask (co-op state
    // 3), its riders will follow it (g_orphans, used by the attach above); if it
    // leaves the level, they leave too (the next level forgets g_orphans).
    const uint32_t owner = Read32(holder + kHolderOwner);
    const int owner_player = PlayerOf(owner);
    if (owner_player >= 0 && Read32(kState + 4 * owner_player) == 3) {
      std::vector<uint32_t>& orphans = g_orphans[owner];
      if (main_rider) orphans.push_back(main_rider);
      if (extras) orphans.insert(orphans.end(), extras->begin(), extras->end());
    }
  }
  if (g_releasing_all && extras) {
    // The holder is being disabled (its Crash leaves the level, ...): the main
    // rider leaves as in the original, then each extra one the same way, and
    // nobody moves up (a first version promoted an extra rider into a holder
    // that was going away, during Quit Game).
    const PPCContext call = ctx;
    __imp__sub_82239418(ctx, base);
    for (uint32_t rider : *extras) {
      Write32(holder + kHolderRider, rider);  // raw: its reference moves to the slot
      ctx = call;
      __imp__sub_82239418(ctx, base);         // lets go of it (+ releases it)
    }
    extras->clear();
  } else if (!extras || extras->empty()) {
    __imp__sub_82239418(ctx, base);
  } else if (auto it = std::find(extras->begin(), extras->end(), leaving); leaving && it != extras->end()) {
    Write32(holder + kHolderRider, leaving);    // raw: its reference moves to the slot
    extras->erase(it);
    __imp__sub_82239418(ctx, base);             // lets go of it (+ releases it)
    Write32(holder + kHolderRider, main_rider);  // raw: the main rider back
  } else {
    __imp__sub_82239418(ctx, base);             // the main rider leaves
    if (!Read32(holder + kHolderRider)) {
      Write32(holder + kHolderRider, extras->front());  // raw: its reference moves along
      extras->erase(extras->begin());
    }
  }
  SyncHosts(holder);
  if (REXCVAR_GET(debug_coop_trace) && main_rider) {
    REXLOG_INFO("Co-op: holder {:08X} (owner {:08X}) detach {:08X}: main {:08X} -> {:08X}, {} extra (from {:08X})",
                holder, Read32(holder + kHolderOwner), leaving, main_rider,
                Read32(holder + kHolderRider), extras ? extras->size() : 0, lr);
  }
}

// The holder's slot 16 (sub_82239250 = "let go", r4 = 1), called when the
// behaviour is disabled (sub_82102F28), e.g. when its Crash leaves the level:
// every rider leaves.
extern "C" REX_FUNC(sub_82239250) {
  g_releasing_all = true;
  __imp__sub_82239250(ctx, base);
  g_releasing_all = false;
}

// The holder's message handler (r3 = holder, r4 = owner, r5 = message): the
// per-frame position messages (type 39 subtype 0 and type 25 subtype 4) go to
// its main rider; send the same to each extra rider.
extern "C" REX_FUNC(sub_82238F18) {
  const PPCContext call = ctx;
  __imp__sub_82238F18(ctx, base);
  const uint32_t holder = call.r3.u32, type = Read32(call.r5.u32 + 4);
  if (type != 39 && type != 25) return;
  std::vector<uint32_t>* extras = ExtrasOf(holder);
  if (!extras || extras->empty()) return;
  const PPCContext after = ctx;
  const uint32_t main_rider = Read32(holder + kHolderRider);
  if (!main_rider) return;  // (the handler only sends while it has a rider)
  for (uint32_t rider : *extras) {
    Write32(holder + kHolderRider, rider);
    ctx = call;
    __imp__sub_82238F18(ctx, base);
  }
  Write32(holder + kHolderRider, main_rider);
  ctx = after;
}

// The mask's message handler (r3 = mask behaviour, r4 = its Crash, r5 =
// message). Position messages (type 56, subtype 2/3) carry the attach point's
// matrix (64 bytes at message +20, copied to *(mask +36)): extra riders sit
// to the side of the main one, along the matrix's first row.
extern "C" REX_FUNC(sub_82237960) {
  const PPCContext call = ctx;
  __imp__sub_82237960(ctx, base);
  const uint32_t message = call.r5.u32, rider = call.r4.u32;
  if (Read32(message + 4) != 56) return;
  const uint32_t kind = Read32(message + 16);
  if (kind != 2 && kind != 3) return;
  for (auto& [holder, extras] : g_extra_riders) {
    for (size_t k = 0; k < extras.size(); ++k) {
      if (extras[k] != rider) continue;
      const uint32_t matrix = Read32(call.r3.u32 + 36);
      auto f = [&](uint32_t o) { uint32_t b = Read32(matrix + o); float v; std::memcpy(&v, &b, 4); return v; };
      auto set = [&](uint32_t o, float v) { uint32_t b; std::memcpy(&b, &v, 4); Write32(matrix + o, b); };
      const float side = (k == 0 ? 1.0f : -1.0f) * kExtraRiderSpacing;
      for (int c = 0; c < 3; ++c) set(48 + 4 * c, f(48 + 4 * c) + side * f(4 * c));
      return;
    }
  }
}

// =============================================================================
// "May I turn into a mask?" (sub_82235CB8, r3 = my co-op behaviour, my Crash at
// +32; one caller, Crash's fight-tree helper sub_821A8440). The original, for two
// players: nobody is entering a mask (state 3) and no sub-state 7 (players 1-2),
// the other player is on screen (sub_82235978, itself two-player gated), and
// PLAYER 1 AND PLAYER 2 ARE BOTH ON FOOT. Found 2026-10-04: with player 2 a
// mask, players 3-4 could never turn into one (their B did nothing). For every
// local player: nobody entering a mask, no sub-state 7, the original's on-screen
// check, I am on foot, and my host (sub_82235B10: the nearest Crash on foot) is
// on foot with room for one more mask (three per Crash). A Crash that carries
// masks may turn into one: its riders follow it (g_orphans above).
// =============================================================================
extern "C" REX_FUNC(__imp__sub_82235CB8);
extern "C" REX_FUNC(__imp__sub_82235978);  // false = the other player may be the host (on screen)
extern "C" REX_FUNC(sub_82235CB8) {
  const int n = LocalPlayers();
  if (n <= 2) {
    __imp__sub_82235CB8(ctx, base);
    return;
  }
  const uint32_t me_behaviour = ctx.r3.u32, me = Read32(me_behaviour + 32);
  bool may = true;
  for (int p = 0; p < n && may; ++p) {
    may = Read32(kState + 4 * p) != 3 && Read32(kSubState + 4 * p) != 7;
  }
  const int my_player = PlayerOf(me);
  may = may && my_player >= 0 && Read32(kState + 4 * my_player) == 2;
  uint32_t host = 0;
  if (may) {
    host = CallGame(sub_82235B10, ctx, base, me_behaviour, me);
    const int host_player = PlayerOf(host);
    const uint32_t holder = HolderOf(host);
    const std::vector<uint32_t>* extras = holder ? ExtrasOf(holder) : nullptr;
    const int riders = holder ? (Read32(holder + kHolderRider) != 0) + (extras ? int(extras->size()) : 0) : 0;
    may = host_player >= 0 && host_player != my_player && Read32(kState + 4 * host_player) == 2 &&
          holder && riders < 1 + kMaxExtraRiders &&
          (CallGame(__imp__sub_82235978, ctx, base, me_behaviour, host) & 0xFF) == 0;
  }
  ctx.r3.u64 = may ? 1 : 0;
  if (REXCVAR_GET(debug_coop_trace)) {  // each distinct answer once per (me, host)
    static std::set<std::tuple<uint32_t, uint32_t, bool>> seen;
    if (seen.insert({me, host, may}).second) {
      REXLOG_INFO("Co-op: may player {} turn into a mask (host {:08X})? {}", my_player + 1, host,
                  may ? "yes" : "no");
    }
  }
}

// =============================================================================
// "THE OTHER PLAYER" BY NUMBER (findings/26 s.24). CCoOpBehaviour (r3 = it, my
// player at +36, my Crash at +32) asks sub_82235B00 for the other player's
// NUMBER: "1 if I'm player 1, else 0" -- player 2 for player 1, player 1 for
// everyone else. Found 2026-10-05 while giving players 3-4 their own drop-out
// countdown: four script-reachable co-op functions still tied players 3-4 to
// player 1 or 2 through it (method names from the registration at 0x82235E70..):
//   IsOtherPlayerDigging          sub_82234AE0  digging byte of "the other"
//   DeatchFromOtherPlayerAndUnhide sub_82235258 leave a mask: at the host's
//                                               height if the host digs
//   IsOkayToLeaveMaskState        sub_82235A80  no while "the other" is DYING (7)
//   Drop Out (front end 48/7)     sub_822357B0  below
//   ForceOtherPlayerToBecomeMask  sub_82235B70  below (rewritten)
// With more than two players "the other" = my partner by sub_82235B10: the
// Crash carrying me if I'm a mask, else the nearest Crash on foot -- the same
// partner the mask code already uses. Inside Drop Out it is one of my riders.
// =============================================================================
extern "C" REX_FUNC(__imp__sub_82235B00);
extern "C" REX_FUNC(__imp__sub_822357B0);
extern "C" REX_FUNC(__imp__sub_82235B70);

namespace more_players {
namespace {
constexpr uint32_t kBehaviourCrash = 32, kBehaviourPlayer = 36;  // CCoOpBehaviour
// Sub-states (registration at 0x8223619C): 0 JOINING_GAME, 1 DEAD, 2 DISPLAYING_NO_EXIT_ERROR,
// 3 LEAVING_MASK, 5 BEING_FORCED_INTO_MASK, 7 DYING; unnamed: 4 (forced out of the mask: Drop
// Out's rider), 6 (dropped out), 8 (a mask dropping out), 9 (nothing going on).
constexpr uint32_t kSubForcedOutOfMask = 4, kSubForcedIntoMask = 5, kSubDroppedOut = 6;
uint32_t g_dropping_out = 0;  // the CCoOpBehaviour inside its Drop Out (sub_822357B0)

// The players riding Crash `crash` as masks (state 1 mask / 3 entering one).
std::vector<int> RidersOf(uint32_t crash) {
  std::vector<int> riders;
  const uint32_t holder = HolderOf(crash);
  if (!holder) return riders;
  for (int q = 0; q < LocalPlayers(); ++q) {
    const uint32_t c = CharacterOf(q), s = Read32(kState + 4 * q);
    if (!c || c == crash || (s != 1 && s != 3)) continue;
    auto it = g_host_of.find(c);
    if ((it != g_host_of.end() && it->second == holder) || Read32(holder + kHolderRider) == c) riders.push_back(q);
  }
  return riders;
}
}  // namespace
}  // namespace more_players

extern "C" REX_FUNC(sub_82235B00) {
  if (LocalPlayers() <= 2) {
    __imp__sub_82235B00(ctx, base);
    return;
  }
  const uint32_t behaviour = ctx.r3.u32;
  const int me = int(Read32(behaviour + kBehaviourPlayer));
  int other = -1;
  if (g_dropping_out == behaviour) {
    // Drop Out's "the other" (only on its "someone rides me / nobody else plays"
    // path): my first rider, else a player not in the game (= nothing to do).
    const std::vector<int> riders = RidersOf(Read32(behaviour + kBehaviourCrash));
    if (!riders.empty()) other = riders.front();
    for (int q = 0; q < LocalPlayers() && other < 0; ++q) {
      if (q != me && Read32(kState + 4 * q) == 0) other = q;
    }
  } else {
    other = PlayerOf(CallGame(sub_82235B10, ctx, base, behaviour, Read32(behaviour + kBehaviourCrash)));
  }
  if (other < 0 || other == me) {
    __imp__sub_82235B00(ctx, base);
  } else {
    ctx.r3.u64 = uint32_t(other);
  }
}

// DROP OUT (pause menu; the front end sends 48/7 to the player's Crash,
// sub_82266040; CCoOpBehaviour's handler sub_822357B0). The original: a mask (or
// one entering) -> sub-state 8. On foot: if players 1 AND 2 are both in game ->
// path A (my partner gets a "where to stand" message if needed, I leave: state 0,
// sub-state 5); else path B: "the other" (it rides me) is forced out of its mask
// (sub-state 4), then I leave the same way. For more players (midasm hooks on
// the two state reads at 0x822357EC / 0x822357F8 + this wrapper): path A when
// nobody rides me and someone else is in game, path B otherwise; path B's
// "other" = my first rider (sub_82235B00 above), the remaining riders get
// sub-state 4 here. Before: player 3 dropping out with player 2 a mask set
// PLAYER 1's sub-state to 4 and left player 3's own riders on a vanished Crash.
extern "C" REX_FUNC(sub_822357B0) {
  const uint32_t behaviour = ctx.r3.u32;
  const int me = int(Read32(behaviour + kBehaviourPlayer));
  const uint32_t my_state = me >= 0 && me < kMaxPlayers ? Read32(kState + 4 * me) : 0;
  if (LocalPlayers() <= 2 || my_state == 1 || my_state == 3) {
    __imp__sub_822357B0(ctx, base);
    return;
  }
  const std::vector<int> riders = RidersOf(Read32(behaviour + kBehaviourCrash));
  g_dropping_out = behaviour;
  __imp__sub_822357B0(ctx, base);
  g_dropping_out = 0;
  for (int q : riders) {  // path B freed the first; every rider of mine leaves its mask
    if (Read32(kSubState + 4 * q) != kSubForcedOutOfMask) Write32(kSubState + 4 * q, kSubForcedOutOfMask);
  }
  if (REXCVAR_GET(debug_coop_trace)) {
    REXLOG_INFO("Co-op: player {} drops out ({} rider(s) forced out of their masks)", me + 1, riders.size());
  }
}

// Midasm hooks before "cmpwi r11,2" at 0x822357F0 / 0x822357FC (r11 = player 1's
// / player 2's state just read; r31 = the behaviour): with more players both
// answer "in game" (2) for path A, else 0 (path B).
void MorePlayersDropOutPath(PPCRegister& r11, PPCRegister& r31) {
  if (LocalPlayers() <= 2) return;
  const uint32_t behaviour = r31.u32;
  const int me = int(Read32(behaviour + kBehaviourPlayer));
  bool someone_else_plays = false;
  for (int q = 0; q < LocalPlayers(); ++q) someone_else_plays |= q != me && Read32(kState + 4 * q) == 2;
  const bool path_a = someone_else_plays && RidersOf(Read32(behaviour + kBehaviourCrash)).empty();
  r11.u64 = path_a ? 2 : 0;
}

// ForceOtherPlayerToBecomeMask (script method; Crash.bfig calls it right before
// InteractWithInteractable). The original: when players 1 AND 2 are both in game
// and neither is being forced into a mask / dropped out (sub-states 5 / 6), "the
// other" gets sub-state 5 (its Crash turns into a mask on the caller). For more
// players: when I'm in game and not 5 / 6, EVERY other player in game who isn't
// 5 / 6 gets 5 (three masks fit on one Crash).
extern "C" REX_FUNC(sub_82235B70) {
  if (LocalPlayers() <= 2) {
    __imp__sub_82235B70(ctx, base);
    return;
  }
  const int me = int(Read32(ctx.r3.u32 + kBehaviourPlayer));
  if (me < 0 || me >= kMaxPlayers || Read32(kState + 4 * me) != 2) return;
  auto busy = [](int q) {
    const uint32_t s = Read32(kSubState + 4 * q);
    return s == kSubForcedIntoMask || s == kSubDroppedOut;
  };
  if (busy(me)) return;
  for (int q = 0; q < LocalPlayers(); ++q) {
    if (q == me || Read32(kState + 4 * q) != 2 || busy(q)) continue;
    Write32(kSubState + 4 * q, kSubForcedIntoMask);
    if (REXCVAR_GET(debug_coop_trace)) REXLOG_INFO("Co-op: player {} forces player {} into a mask", me + 1, q + 1);
  }
}

// The HUD controller's three "flash" methods with a player number
// (sub_8226AB18 / AB78 / ABD8): more_players_hud.cpp (players 3-4's displays). Found
// 2026-10-04: for player 3 the original used the controller's +20 (0 / 1) as
// the display: a mojo froze the game ("write of guest 0x00000110").

// THE COMBO METERS (CComboCounter, front end +1852 + 3152 * p, two of them):
// players 3-4's are in more_players_frontend.cpp (with the lock-on arrows,
// counter prompts and reticles). Found 2026-10-04: a mojo collected by player 3
// or 4 updated "player 3's meter" = other front end fields.

// THE MOJO MULTIPLIERS (findings/26 s.17): front end +8572 + 4p (index (2143 +
// p) * 4), two of them; +8580.. are other front end fields (bytes at +8580,
// +8584.., +8588.., flags +8592). Crash's collect code (sub_821ACAD8) multiplies
// every mojo by its player's multiplier: for player 3 it read +8580 as one.
// That was the 50 / 100 million mojos and the endless "Level Up!" screens
// (found 2026-10-04 after the tracker skip above did not stop them). Players
// 3-4's multipliers live in our block; the 4 sites: raise (sub_82263690,
// then the game's sub_822E1010 as the original), reset to 1 (sub_822636B8),
// Crash's collect read and the HUD display's read (hooks).
constexpr uint32_t kMultiplierIndexBase = 2143 * 4;  // byte offset of player 1's
extern "C" REX_FUNC(__imp__sub_82263690);
extern "C" REX_FUNC(__imp__sub_822E1010);
extern "C" REX_FUNC(sub_82263690) {  // r3 = front end, r4 = player: multiplier + 1
  const int32_t p = int32_t(ctx.r4.u32);
  if (p < 2 || p >= kMaxPlayers) {
    __imp__sub_82263690(ctx, base);
    return;
  }
  const uint32_t slot = g_block + kBlockMultipliers + 4 * (p - 2);
  Write32(slot, Read32(slot) + 1);
  // as the original's tail call: r3 = *(game + 96), r4 = 0
  ctx.r3.u64 = Read32(Read32(kGameGlobal) + 96);
  ctx.r4.u64 = 0;
  __imp__sub_822E1010(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_822636B8);
extern "C" REX_FUNC(sub_822636B8) {  // r3 = front end, r4 = player: multiplier = 1
  const int32_t p = int32_t(ctx.r4.u32);
  if (p < 2 || p >= kMaxPlayers) {
    __imp__sub_822636B8(ctx, base);
    return;
  }
  Write32(g_block + kBlockMultipliers + 4 * (p - 2), 1);
}
// ... a read "lwzx rD,index,front end" just happened (index = (2143 + p) * 4).
void MorePlayersMultiplierRead(PPCRegister& index, PPCRegister& value) {
  if (index.u32 >= kMultiplierIndexBase + 8 && index.u32 < kMultiplierIndexBase + 16) {
    value.u64 = Read32(g_block + kBlockMultipliers + index.u32 - kMultiplierIndexBase - 8);
  }
}

// Player p's character, for the other modules (more_players.h).
uint32_t more_players::CharacterOfPlayer(int p) {
  return p >= 0 && p < kMaxPlayers ? CharacterOf(p) : 0;
}

// The game object's +24 list (players 1-2) / our block (3-4), as sub_8226FC88 reads it.
uint32_t more_players::TitanOfPlayer(int p) {
  if (p < 0 || p >= kMaxPlayers) return 0;
  return p < 2 ? Read32(0x825B0008 + 24 + 4 * p) : Read32(g_block + kBlockTitans + 4 * (p - 2));
}

// --debug_coop_trace: where every player's Crash is (with each state change).
// Found the NaN position of the safety net above this way.
namespace more_players {
namespace {
void TracePositions() {
  std::string line;
  for (int p = 0; p < LocalPlayers(); ++p) {
    float at[3];
    if (PositionOf(CharacterOf(p), at)) line += fmt::format("  P{} ({:.1f}, {:.1f}, {:.1f})", p + 1, at[0], at[1], at[2]);
    else line += fmt::format("  P{} -", p + 1);
  }
  REXLOG_INFO("Co-op: positions{}", line);
}
}  // namespace
}  // namespace more_players

// DEATHS WITH THREE OR FOUR PLAYERS (findings/26 s.23). What happens when a
// player dies is written in the scripts (script/objectives/objectives.lua),
// four objectives, each "WAIT_NumPlayers(n) AND WAIT_PlayerIsDead(p), then
// DO_ResetPlayer(p, drop)":
//   OnePlayerOutOfPack_P1_Dies / _P2_Dies   n = 1: fade out, back at the
//                                           checkpoint (drop = false)
//   TwoPlayersOutOfPack_P1_Dies / _P2_Dies  n = 2: the dead player drops out
//                                           (drop = true: back to the mask,
//                                           Join Game), the other plays on
// "n" = players IN GAME (state 2, sub_82270590). With three or four in game no
// objective matched, and players 3-4 have none: a dead player lay on the ground
// for good (found 2026-10-05 with the cheat menu's Kill: players 2-4 stayed
// down, states 2 2 2 2; with two players player 2 dropped out as it should).
// Three answers make the scripts' own rules cover four players:
//   1. WAIT_NumPlayers(2) is "two OR MORE in game" (only these objectives and
//      one more n = 1 rule use WAIT_NumPlayers).
//   2. WAIT_PlayerIsDead(player 2) is "one of players 2-4 in game is dead";
//      we remember which.
//   3. DO_ResetPlayer(player 2) resets the one remembered.
// "Dead" = the game object's alive byte +36 + player (CRequirementActorIsDead
// sub_822A01E8 reads it; Crash's code writes it by his own player number, so
// players 3-4's are at +38 / +39, which nothing else uses).
namespace more_players {
namespace {
constexpr uint32_t kGameObject = 0x825B0008;   // sub_82270608's singleton
constexpr uint32_t kGameObjectAlive = 36;      // byte per player: 0 = dead
int g_dead_standing_for_two = -1;  // the player "player 2" means for the reset (game thread)
}  // namespace
}  // namespace more_players

// CRequirementNumPlayers check (r3 = it, its +8 = n): count == n; for n = 2
// "two or more".
extern "C" REX_FUNC(__imp__sub_822A3178);
extern "C" REX_FUNC(sub_822A3178) {
  const uint32_t wanted = Read32(ctx.r3.u32 + 8);
  if (wanted != 2 || LocalPlayers() <= 2) {
    __imp__sub_822A3178(ctx, base);
    return;
  }
  uint32_t n = 0;
  for (int p = 0; p < kMaxPlayers; ++p) n += Read32(kState + 4 * p) == 2;
  ctx.r3.u64 = n >= 2 ? 1 : 0;
}

// CRequirementActorIsDead check (WAIT_PlayerIsDead; r3 = it, its +20 = the
// player, -1 = an actor by name): for player 2, any of players 2-4 in game.
extern "C" REX_FUNC(__imp__sub_822A01E8);
extern "C" REX_FUNC(sub_822A01E8) {
  if (int32_t(Read32(ctx.r3.u32 + 20)) != 1 || LocalPlayers() <= 2) {
    __imp__sub_822A01E8(ctx, base);
    return;
  }
  int dead = -1;
  for (int p = 1; p < LocalPlayers() && dead < 0; ++p) {
    if (Read32(kState + 4 * p) == 2 && *Guest(kGameObject + kGameObjectAlive + p) == 0) dead = p;
  }
  if (dead >= 0) {
    g_dead_standing_for_two = dead;
    if (REXCVAR_GET(debug_coop_trace)) {
      REXLOG_INFO("Co-op: player {} is dead: the scripts' player-2 death rule takes it", dead + 1);
    }
  }
  ctx.r3.u64 = dead >= 0 ? 1 : 0;
}

// CActionResetPlayer's work (DO_ResetPlayer; r3 = it, its +8 = the player):
// "player 2" = the one the death rule found.
extern "C" REX_FUNC(__imp__sub_82297928);
extern "C" REX_FUNC(sub_82297928) {
  const uint32_t action = ctx.r3.u32;
  const int dead = g_dead_standing_for_two;
  if (Read32(action + 8) != 1 || dead <= 1) {
    __imp__sub_82297928(ctx, base);
    return;
  }
  g_dead_standing_for_two = -1;
  if (REXCVAR_GET(debug_coop_trace)) REXLOG_INFO("Co-op: reset of player 2 goes to player {}", dead + 1);
  Write32(action + 8, uint32_t(dead));
  __imp__sub_82297928(ctx, base);
  Write32(action + 8, 1);
}

// SAFETY NET: a character's "set position" (physics behaviour, sub_8217F528:
// r3 = the physics behaviour, r5 = the message, position at message +68,
// snapped by the world sub_822E6AA0) that ends with a NaN position keeps the
// previous one. Found 2026-10-04 (cause TBD): at the save totem in the first
// level after Crash's house, player 3's hidden Crash, riding as a mask, got a
// NaN position this way; leaving the mask it stayed NaN (invisible, picking up
// mission items, holding the co-op camera so player 1 couldn't walk).
extern "C" REX_FUNC(__imp__sub_8217F528);
extern "C" REX_FUNC(sub_8217F528) {
  using namespace more_players;
  const uint32_t physics = ctx.r3.u32;
  const uint32_t actor = Read32(physics + 88);
  const uint32_t matrix = actor ? Read32(actor + 44) : 0;
  uint8_t before[64], fields[12];
  const bool ok = matrix >= 0x40000000;
  if (ok) {
    std::memcpy(before, Guest(matrix), 64);
    std::memcpy(fields, Guest(physics + 128), 12);
  }
  __imp__sub_8217F528(ctx, base);
  if (!ok) return;
  bool bad = false;
  for (uint32_t o = 48; o < 60 && !bad; o += 4) bad = (Read32(matrix + o) & 0x7F800000) == 0x7F800000;
  if (bad) {
    std::memcpy(Guest(matrix), before, 64);
    std::memcpy(Guest(physics + 128), fields, 12);
    static int logged = 0;
    if (logged++ < 10) REXLOG_WARN("Co-op: character {:08X} was set to a NaN position: kept the previous one", actor);
  }
}

