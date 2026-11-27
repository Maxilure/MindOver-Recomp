// =============================================================================
// net/session_sync.cpp -- see session_sync.h
// =============================================================================
//
// GUEST OFFSETS & FUNCTIONS USED (verified with xexdis & docs/findings/24,26,27):
//   *0x8259B190 (CUberManager):
//     +8    s32    game state (5 = playing a level)
//     +18   s8     active save level ID
//     +20   s8     current loaded level ID (written by ChangeLevel sub_8227B640)
//     +21   s8     current spawn entry ID
//     +52   ptr    front end (menu flags +8592, +8594 bit 0x80, confirm box +172,
//                             pause owner +8536, mojo multipliers +8572 + 4*p)
//     +80   ptr    CSaveGameManager (+16 104B header, +128 20,174B save buffer)
//     +96   ptr    CStatsManager (+20 play time f32, +420 s32[183] EStatInt)
//     +104  ptr    CTimeManager (+24 bit 0x80 = time paused)
//     +168  u8     pause bits (0x04 screenshot freeze, 0x10/0x20 game/menu pause)
//   0x825B0008 (GameObject, returned by sub_82270608):
//     +32   ptr    pocketed titan CActor*
//     +44   char[64] pocketed titan template name
//     +108  char[64] P1 carried titan name (+172 P2; P3-P4 in more_players block)
//     +240  s32    difficulty (0..2)
//     +244  s32    transition target level ID
//     +248  s32    transition target entry ID
//   0x8259B11C     u32[4] ECoOpPlayerState (0=out, 1=mask, 2=on foot/titan, 3=entering)
// =============================================================================
#include <map>
#include "session_sync.h"
#include "enemy_input.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../achievements/achievements.h"
#include "../cheats/cheats.h"
#include "../cheats/spawn.h"
#include "../data/fight_tree.h"
#include "../guest_memory.h"
#include "../input/keyboard_mouse.h"
#include "../language/texts.h"
#include "../input/prompt_icons.h"
#include "../players/more_players.h"
#include "../players/outfit_picker.h"
#include "../players/outfits.h"
#include "../saves/save_files.h"
#include "state_sync.h"
#include "world_sync.h"
#include "action_sync.h"
#include "mask_sync.h"
#include "titan_sync.h"

// Recompiled game functions we override or invoke on the main guest thread.
extern "C" REX_FUNC(__imp__sub_8211AF40);  // FrontEnd fight-tree decision dispatcher
extern "C" REX_FUNC(__imp__sub_8227C8E8);  // CUberManager::SetPaused(uber, on)
extern "C" REX_FUNC(__imp__sub_8227C950);  // CUberManager::SetPausedWithSound(uber, on, sound)
extern "C" REX_FUNC(__imp__sub_8225DE68);  // CConfirmBox::Update(box, dt)
extern "C" REX_FUNC(__imp__sub_8225F718);  // FrontEnd::SetSavePrompt(fe, on) -> fe + 8592 bit 0x04
extern "C" REX_FUNC(__imp__sub_82295E20);  // CActionOpenMenu::Run(action) -> sub_82263620
extern "C" REX_FUNC(__imp__sub_8217D5A0);  // CPhysicsBehaviour::IsFloatingStuck(phys, dt)
extern "C" REX_FUNC(__imp__sub_820B5668);  // BuildInteractMsg(msg, 2, actor, target, flag, 0)
extern "C" REX_FUNC(__imp__sub_820B56C8);  // DestroyInteractMsg(msg)
extern "C" REX_FUNC(__imp__sub_8227B640);  // CUberManager::ChangeLevel(uber, level, entry)
extern "C" REX_FUNC(__imp__sub_82293D70);  // CActionChangeLevel::Run (door/portal transition)
extern "C" REX_FUNC(__imp__sub_822975A8);  // CActionExitLevel::Run (hub/area exit transition)
extern "C" REX_FUNC(__imp__sub_8229CB78);  // StorePlayersBeforeLevelChange()
extern "C" REX_FUNC(__imp__sub_820B3430);  // CMasterCinematicAction: notify all players/titans
extern "C" REX_FUNC(__imp__sub_821991B0);  // Collectable::SetState(collectable, actor, new_state)
extern "C" REX_FUNC(__imp__sub_8225A7D0);  // CSaveGameManager::PackSaveBuffer(save_mgr)
extern "C" REX_FUNC(__imp__sub_82293850);  // CActionBeginChangeLevel "ready?" (vt slot 1)
extern "C" REX_FUNC(__imp__sub_82293D48);  // CActionChangeLevel "ready?" (vt slot 1)
extern "C" REX_FUNC(__imp__sub_82270608);  // GetGameObject() -> 0x825B0008
extern "C" REX_FUNC(sub_822705D0);         // SetCarriedActorName(game_obj, player, name_ptr)
extern "C" REX_FUNC(__imp__sub_8235AA10);  // FillCurrentSaveDate(date_ptr)
extern "C" REX_FUNC(__imp__sub_822E10C8);  // ComputeCompletionPercent(stats_mgr)
extern "C" REX_FUNC(__imp__sub_8227BA18);  // GetCurrentSaveLevelId(uber)
extern "C" REX_FUNC(__imp__sub_8236ACB8);  // HashString(out_obj, cstr_ptr)
extern "C" REX_FUNC(__imp__sub_8236AC18);  // CopyHashObj(dst, src)
extern "C" REX_FUNC(__imp__sub_823C9A70);  // DestroyHashObj(obj)
extern "C" REX_FUNC(__imp__sub_820B7BC8);  // BuildQueryTemplateMsg(msg, mode, payload)
extern "C" REX_FUNC(__imp__sub_820B7C10);  // DestroyQueryTemplateMsg(msg)
extern "C" REX_FUNC(__imp__sub_820B0B70);  // SendMessageToActor(actor, msg)
extern "C" REX_FUNC(__imp__sub_820B3E18);  // BuildTitanAttachMsg(msg, kind_9, payload)
extern "C" REX_FUNC(__imp__sub_820B3E58);  // DestroyTitanAttachMsg(msg)
extern "C" REX_FUNC(__imp__sub_820B58F0);  // BuildCrashRideMsg(msg, kind_4, crash, titan)
extern "C" REX_FUNC(__imp__sub_820B59E0);  // DestroyCrashRideMsg(msg)
extern "C" REX_FUNC(__imp__sub_820B0C58);  // Actor::SetEnabled(actor, enabled)
extern "C" REX_FUNC(__imp__sub_821AE998);  // Crash::GiveFreeJack(controller, crash)
extern "C" REX_FUNC(__imp__sub_82177D38);  // CUpgradeableBehaviour::CheckUpgrade(upg, titan, 0, show)
extern "C" REX_FUNC(__imp__sub_82272338);  // CInputController::CheckInputEvent(ctrl, map, event, ignore_lock)
extern "C" REX_FUNC(__imp__sub_82271D08);  // CInputController::GetChannelNewestValue(ctrl, channel)
extern "C" REX_FUNC(__imp__sub_82275C90);  // CInputMap::ResetAllEvents(map)

namespace net {
void SendToHost(const uint8_t* data, size_t size, bool reliable);
bool DebugTraceOn();  // state_sync.cpp: --debug_online_trace
void BroadcastFromHost(const uint8_t* data, size_t size, bool reliable);
void SendToPeerFromHost(uint32_t peer, const uint8_t* data, size_t size, bool reliable);

namespace session_sync {
namespace {

using Clock = std::chrono::steady_clock;
// An accepted titan pocket press stays "yes" this long: a fight tree may ask the
// same event more than once in one frame (the hook in CheckInputEvent below).
constexpr auto kPocketSameFrame = std::chrono::milliseconds(40);

// Guest layout constants.
constexpr uint32_t kUberGlobal = 0x8259B190;
constexpr uint32_t kGameObject = 0x825B0008;
constexpr uint32_t kCoOpState = 0x8259B11C;
constexpr uint32_t kUberGameState = 8;
constexpr uint32_t kUberLoadedLevel = 20;
constexpr uint32_t kUberLoadedEntry = 21;
constexpr uint32_t kUberFrontEnd = 52;
constexpr uint32_t kUberSaveMgr = 80;
constexpr uint32_t kUberStats = 96;
constexpr uint32_t kUberTimeMgr = 104;
constexpr uint32_t kUberFlags = 168;
constexpr uint8_t kPauseBitMenu = 0x10;
constexpr uint8_t kPauseBitGame = 0x20;
constexpr uint32_t kTimeMgrFlags = 24;
constexpr uint8_t kTimeMgrPausedBit = 0x80;

constexpr uint32_t kFeConfirmBoxFlags = 172 + 4;
constexpr uint8_t kFeConfirmShowing = 0x40;
constexpr uint32_t kFeOwner = 8536;
constexpr uint32_t kFeMojoMultipliers = 8572;
constexpr uint32_t kFeFlags2 = 8594;
constexpr uint8_t kFeMenuOnScreen = 0x80;
constexpr uint8_t kFeInMainMenus = 0x20;

constexpr uint32_t kStatsPlayTime = 20;
constexpr uint32_t kStatIntsOffset = 420;
constexpr int kStatIntCount = 183;
constexpr int kStatTotalMojo = 3;
constexpr int kStatCrashMojo = 4;

constexpr uint32_t kSaveBlockBytes = 20174;  // 0x4ECE bytes inside the 0x4EED save file
constexpr size_t kSaveChunkMax = 1024;

// Unlock tables (findings/27, cheats.cpp).
struct UnlockTable {
  uint32_t address;
  int count;
};
constexpr UnlockTable kUnlockTables[4] = {
    {0x825A72A8, 11},  // Outfits
    {0x825982C8, 12},  // Concept art
    {0x825982F8, 20},  // Enemy info
    {0x825983D0, 34},  // Tutorials
};
constexpr uint32_t kCocoUnlockedByte = 0x825AB1C8;

// Crash & Titan controller / upgrade offsets (findings/27).
constexpr uint32_t kActorNameHash = 8;
constexpr uint32_t kActorController = 0x1C;
constexpr uint32_t kActorMatrix = 44;
constexpr uint32_t kCrashCtrlVtable = 0x82035294;
constexpr uint32_t kCrashFreeJackTimer = 304;
constexpr uint32_t kJackableUpgradeable = 0x140;
constexpr uint32_t kUpgradeableVtable = 0x82032344;
constexpr uint32_t kUpgMojoStat = 44;
constexpr uint32_t kUpgLevelStat = 48;

enum PickupKind : uint8_t {
  kPickupStatSet = 0,          // client stat increased -> host adopts max/delta
  kPickupUnlockItem = 1,       // unlock list [id_or_list >> 8] item [id_or_list & 0xFF]
  kPickupCollectableTaken = 2, // world Collectable (mojo/doll/powerup) picked up at pos
  kPickupFreeJack = 3,         // Free Jack power-up activated on player `slot`
  kPickupCoco = 4,             // Coco unlocked
};

struct PendingTransition {
  bool pending = false;
  int8_t level = -1;
  int8_t entry = 0;
  uint32_t epoch = 0;
};

struct PendingCollectableHide {
  uint32_t hash = 0;
  float pos[3] = {};
  Clock::time_point expires;
};

struct PendingPickup {
  uint8_t slot = 0;
  uint8_t kind = 0;
  uint16_t id = 0;
  int32_t value = 0;
  uint32_t hash = 0;
  float pos[3] = {};
};

struct PlayerTitanState {
  std::string ridden_template;
  std::string pocket_template;
  float hp = 0.0f;
  int32_t mojo = 0;
  int32_t level = 0;
  Clock::time_point mismatch_since{};
  Clock::time_point last_spawn_attempt{};
  Clock::time_point last_forced_off{};  // Case A: last own-decisions window opened (at most every 3 s)
  // The host net id of the titan they ride (0 = not a synced entity / unknown):
  // the other PCs mount THEIR COPY of that titan instead of spawning one.
  uint32_t ridden_net_id = 0;
  // True once this player's own PC told us what they ride (kMsgTitanOwnership).
  // Until then the titan repair leaves them alone: "nothing heard" is not
  // "rides nothing".
  bool known = false;
};

struct PendingRemoteInteract {
  uint8_t slot = 0;
  uint32_t hash = 0;
  uint32_t flag = 0;
  float pos[3] = {};
};

struct State {
  std::recursive_mutex mutex;
  std::atomic<bool> local_menu_paused{false};
  std::atomic<bool> local_menu_open_cached{false};
  std::atomic<int64_t> menu_button_block_until_ns{0};
  bool prev_menu_open = false;
  bool fe_hooks_registered = false;
  Clock::time_point last_menu_block_banner{};
  // A level exit waiting for open menus (HoldExitForOpenMenu): until when, and
  // where the local player is held meanwhile.
  Clock::time_point exit_waiting_until{};
  bool exit_hold_valid = false;
  uint32_t exit_hold_body = 0;
  float exit_hold_pos[3] = {};
  // Host: a guest's exit request that came while a menu was open (sent on later).
  PendingTransition queued_request{};
  uint8_t queued_initiator = 0;
  // Guest: our last exit request to the host, resent until the host answers.
  PendingTransition sent_request{};
  Clock::time_point sent_request_at{};

  // Area, NPC interaction & cutscene sync.
  uint32_t transition_epoch = 0;
  PendingTransition pending_transition{};
  bool applying_transition = false;
  bool was_loading = false;
  std::atomic<int64_t> grace_until_ns{0};
  int active_interact_player = -1;
  int last_interact_player = -1;
  Clock::time_point last_interact_time{};
  bool local_interact_opened_menu = false;
  bool replaying_remote_interact = false;
  std::vector<PendingRemoteInteract> pending_remote_interacts;
  std::atomic<int32_t> pending_fe_skip_from{-1};
  std::atomic<int32_t> pending_fe_skip_exit{-1};
  // Another PC left a 3D cutscene (543 -> 544): skip ours the game's way.
  bool remote_scene_skip = false;
  Clock::time_point remote_scene_skip_at{};
  bool applying_remote_fe_exit = false;

  // Stats & pickups sync.
  bool stats_baseline_valid = false;
  std::array<int32_t, kStatIntCount> prev_stats{};
  std::array<uint64_t, 4> prev_unlocks{};
  uint8_t prev_coco = 0;
  float prev_free_jack = 0.0f;
  bool pending_stats_apply = false;
  uint32_t stats_seq = 0;
  uint32_t latest_rx_stats_seq = 0;
  std::array<int32_t, kStatIntCount> rx_stats{};
  std::array<uint64_t, 4> rx_unlocks{};
  uint8_t rx_coco = 0;
  Clock::time_point last_stats_broadcast{};
  std::vector<PendingPickup> host_pickup_queue;
  std::vector<PendingCollectableHide> hidden_collectables;

  // Titan ownership, pocket sync & 1.25 s swap debounce.
  std::array<PlayerTitanState, kMaxPlayers> titans{};
  std::string local_last_ridden;
  Clock::time_point local_titan_sent{};  // last kMsgTitanOwnership we sent
  std::string local_last_pocket;
  Clock::time_point next_pocket_allowed{};
  // An accepted pocket press keeps answering yes this long (same frame asks
  // twice): [0] = riding event 37, [1] = on-foot event 14. Local + per remote.
  std::array<Clock::time_point, 4> pocket_yes_until{};
  std::array<std::array<Clock::time_point, 4>, kMaxPlayers> remote_pocket_yes_until{};
  uint32_t dismounting_titan = 0;
  Clock::time_point dismount_until{};
  uint32_t pending_mount_player = ~0u;
  std::string pending_mount_template;

  // Online outfit sync.
  int local_last_outfit = -1;
  Clock::time_point last_outfit_broadcast{};
  std::array<int, kMaxPlayers> remote_target_outfit{-1, -1, -1, -1};

  // Host -> Client save transfer.
  std::vector<uint32_t> save_request_peers;  // peers waiting for Host's live save
  uint16_t host_save_seq = 0;
  uint16_t rx_save_seq = 0;
  uint16_t rx_save_received_bytes = 0;
  std::vector<uint8_t> rx_save_staging;
  std::vector<uint8_t> client_ready_save;  // complete 20,174-byte Host save block
  std::array<int8_t, 4> client_ready_outfits{-1, -1, -1, -1};
  bool client_ready_cheated = false;
  bool client_has_host_save = false;

  // Guest scratch buffer allocated once on the guest system heap.
  uint32_t guest_scratch = 0;
  uint32_t guest_save_scratch = 0;
};

State g_ss;

// --- Safe big-endian guest memory helpers ------------------------------------
uint32_t ReadU32(uint8_t* base, uint32_t addr) {
  if (!addr || !guest_memory::Readable(addr, 4)) return 0;
  const uint8_t* p = GuestPtr(base, addr);
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void WriteU32(uint8_t* base, uint32_t addr, uint32_t v) {
  if (!addr || !guest_memory::Readable(addr, 4)) return;
  uint8_t* p = GuestPtr(base, addr);
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
float ReadF32(uint8_t* base, uint32_t addr) {
  const uint32_t b = ReadU32(base, addr);
  float f = 0.0f;
  std::memcpy(&f, &b, 4);
  return std::isfinite(f) ? f : 0.0f;
}
void WriteF32(uint8_t* base, uint32_t addr, float f) {
  uint32_t b = 0;
  std::memcpy(&b, &f, 4);
  WriteU32(base, addr, b);
}
std::string ReadCString(uint8_t* base, uint32_t addr, size_t max_len = 64) {
  if (!addr || !guest_memory::Readable(addr, uint32_t(max_len))) return {};
  const uint8_t* p = GuestPtr(base, addr);
  std::string s;
  for (size_t i = 0; i < max_len && p[i] != 0; ++i) s.push_back(char(p[i]));
  return s;
}
void WriteCString64(uint8_t* base, uint32_t addr, const std::string& s) {
  if (!addr || !guest_memory::Readable(addr, 64)) return;
  uint8_t* p = GuestPtr(base, addr);
  std::memset(p, 0, 64);
  if (!s.empty()) std::memcpy(p, s.data(), std::min<size_t>(63, s.size()));
}

// Little-endian wire helpers.
void PutLE16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v & 0xFFu);
  p[1] = uint8_t(v >> 8);
}
uint16_t GetLE16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
void PutLE32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v & 0xFFu);
  p[1] = uint8_t((v >> 8) & 0xFFu);
  p[2] = uint8_t((v >> 16) & 0xFFu);
  p[3] = uint8_t(v >> 24);
}
uint32_t GetLE32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
void PutWireF32(uint8_t* p, float v) {
  uint32_t b = 0;
  std::memcpy(&b, &v, 4);
  PutLE32(p, b);
}
float GetWireF32(const uint8_t* p) {
  const uint32_t b = GetLE32(p);
  float v = 0.0f;
  std::memcpy(&v, &b, 4);
  return std::isfinite(v) ? v : 0.0f;
}

uint32_t CallGame(void (*fn)(PPCContext&, uint8_t*), PPCContext& ctx, uint8_t* base,
                  uint32_t r3 = 0, uint32_t r4 = 0, uint32_t r5 = 0, uint32_t r6 = 0) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  ctx.r6.u64 = r6;
  fn(ctx, base);
  const uint32_t ret = ctx.r3.u32;
  ctx = saved;
  return ret;
}

void ExtendGrace(double seconds) {
  const auto until = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                        std::chrono::duration<double>(seconds));
  const int64_t ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(until.time_since_epoch()).count();
  int64_t prev = g_ss.grace_until_ns.load(std::memory_order_relaxed);
  while (ns > prev && !g_ss.grace_until_ns.compare_exchange_weak(prev, ns)) {
  }
}

uint32_t EnsureScratch() {
  if (!g_ss.guest_scratch) {
    g_ss.guest_scratch = rex::system::kernel_memory()->SystemHeapAlloc(1024);
  }
  return g_ss.guest_scratch;
}

// Query any actor's spawn template string (e.g. "Characters:Ratcicle") using
// the exact actor message sub_8229CB78 sends at 0x8229CC64..0x8229CCC4.
std::string QueryActorTemplate(PPCContext& ctx, uint8_t* base, uint32_t actor) {
  if (!actor || !guest_memory::Readable(actor, 64)) return {};
  const uint32_t sc = EnsureScratch();
  if (!sc) return {};
  // Scratch layout:
  //   +0   empty C string ""
  //   +16  temp hash obj (16B)
  //   +32  payload: +32 vtable 0x820311D8, +36 hash copy, +44 out const char*
  //   +96  message object (64B)
  std::memset(GuestPtr(base, sc), 0, 256);
  CallGame(__imp__sub_8236ACB8, ctx, base, sc + 16, sc + 0);
  WriteU32(base, sc + 32, 0x820311D8u);
  CallGame(__imp__sub_8236AC18, ctx, base, sc + 36, sc + 16);
  WriteU32(base, sc + 44, 0);
  CallGame(__imp__sub_823C9A70, ctx, base, sc + 16);
  CallGame(__imp__sub_820B7BC8, ctx, base, sc + 96, 0, sc + 32);
  CallGame(__imp__sub_820B0B70, ctx, base, actor, sc + 96);
  std::string result;
  if (*GuestPtr(base, sc + 96 + 24) != 0) {
    const uint32_t str_ptr = ReadU32(base, sc + 44);
    if (str_ptr) result = ReadCString(base, str_ptr, 64);
  }
  CallGame(__imp__sub_820B7C10, ctx, base, sc + 96);
  CallGame(__imp__sub_823C9A70, ctx, base, sc + 36);
  WriteU32(base, sc + 32, 0x820311D0u);
  return result;
}

// Native mount of `crash` onto `titan` using the exact two-message sequence in
// CActionSpawnPlayer sub_8229C6F8 (0x8229CA9C..0x8229CB10) -- zero fake inputs.
void NativeMountPlayerOnTitan(PPCContext& ctx, uint8_t* base, uint32_t crash, uint32_t titan) {
  if (!crash || !titan) return;
  const uint32_t sc = EnsureScratch();
  if (!sc) return;
  // Scratch +256:
  //   +256 payload (24B): [0]=0x8204036C, [4]=titan, [8]=0, [12]=crash, [16]=0
  //   +304 msg1 (48B)
  //   +368 msg2 (80B)
  std::memset(GuestPtr(base, sc + 256), 0, 256);
  WriteU32(base, sc + 256 + 0, 0x8204036Cu);
  WriteU32(base, sc + 256 + 4, titan);
  WriteU32(base, sc + 256 + 8, 0);
  WriteU32(base, sc + 256 + 12, crash);
  WriteU32(base, sc + 256 + 16, 0);

  CallGame(__imp__sub_820B3E18, ctx, base, sc + 304, 9, sc + 256);
  CallGame(__imp__sub_820B0B70, ctx, base, titan, sc + 304);

  CallGame(__imp__sub_820B58F0, ctx, base, sc + 368, 4, crash, titan);
  uint8_t* flag_byte = GuestPtr(base, sc + 368 + 44);
  *flag_byte &= ~0x40u;
  CallGame(__imp__sub_820B0B70, ctx, base, crash, sc + 368);

  CallGame(__imp__sub_820B59E0, ctx, base, sc + 368);
  CallGame(__imp__sub_820B3E58, ctx, base, sc + 304);
  WriteU32(base, sc + 256 + 0, 0x8202206Cu);
}

uint32_t UpgradeableOfTitan(uint8_t* base, uint32_t titan) {
  if (!titan || !guest_memory::Readable(titan + kActorController, 4)) return 0;
  const uint32_t ctrl = ReadU32(base, titan + kActorController);
  if (!ctrl || !guest_memory::Readable(ctrl + kJackableUpgradeable, 4)) return 0;
  const uint32_t upg = ReadU32(base, ctrl + kJackableUpgradeable);
  if (!upg || !guest_memory::Readable(upg, 160)) return 0;
  return ReadU32(base, upg) == kUpgradeableVtable ? upg : 0;
}

uint64_t ReadUnlockBits(uint8_t* base, int list_idx) {
  const auto& tbl = kUnlockTables[list_idx];
  uint64_t mask = 0;
  for (int i = 0; i < tbl.count; ++i) {
    const uint32_t v = ReadU32(base, tbl.address + 4 * uint32_t(i));
    if (v == 0xFFFFFFFFu) break;
    if (v < 64) mask |= (1ull << v);
  }
  return mask;
}

void ApplyUnlockItem(uint8_t* base, int list_idx, int item_id) {
  if (list_idx < 0 || list_idx >= 4) return;
  const auto& tbl = kUnlockTables[list_idx];
  if (item_id < 0 || item_id >= tbl.count) return;
  for (int i = 0; i < tbl.count; ++i) {
    const uint32_t v = ReadU32(base, tbl.address + 4 * uint32_t(i));
    if (v == uint32_t(item_id)) return;
    if (v == 0xFFFFFFFFu) {
      WriteU32(base, tbl.address + 4 * uint32_t(i), uint32_t(item_id));
      return;
    }
  }
}

void SendPickupToHostOrBroadcast(const PendingPickup& ev) {
  uint8_t pkt[25] = {};
  pkt[0] = kMsgPickupEvent;
  pkt[1] = ev.slot;
  pkt[2] = ev.kind;
  PutLE16(pkt + 3, ev.id);
  PutLE32(pkt + 5, uint32_t(ev.value));
  PutLE32(pkt + 9, ev.hash);
  for (int k = 0; k < 3; ++k) PutWireF32(pkt + 13 + 4 * k, ev.pos[k]);
  if (net::IsHost()) {
    BroadcastFromHost(pkt, sizeof(pkt), /*reliable=*/true);
  } else {
    SendToHost(pkt, sizeof(pkt), /*reliable=*/true);
  }
}

void HostBroadcastFullStats(uint8_t* base, uint32_t stats) {
  if (!stats) return;
  uint8_t pkt[770] = {};
  pkt[0] = kMsgStatsSync;
  const uint32_t seq = ++g_ss.stats_seq;
  PutLE32(pkt + 1, seq);
  for (int i = 0; i < kStatIntCount; ++i) {
    const int32_t val = int32_t(ReadU32(base, stats + kStatIntsOffset + 4 * uint32_t(i)));
    PutLE32(pkt + 5 + 4 * i, uint32_t(val));
  }
  pkt[737] = guest_memory::Readable(kCocoUnlockedByte, 1) ? *GuestPtr(base, kCocoUnlockedByte) : 0;
  for (int u = 0; u < 4; ++u) {
    const uint64_t bits = ReadUnlockBits(base, u);
    PutLE32(pkt + 738 + 8 * u, uint32_t(bits & 0xFFFFFFFFu));
    PutLE32(pkt + 738 + 8 * u + 4, uint32_t(bits >> 32));
  }
  BroadcastFromHost(pkt, sizeof(pkt), /*reliable=*/true);
}

// Pack Host's authoritative 20,174-byte save block on Host's main thread and
// stream it in reliable 1 KB chunks to requesting client peer(s).
void ServiceHostSaveRequests(PPCContext& ctx, uint8_t* base) {
  std::vector<uint32_t> peers;
  {
    std::lock_guard lock(g_ss.mutex);
    if (g_ss.save_request_peers.empty()) return;
    peers.swap(g_ss.save_request_peers);
  }
  const uint32_t uber = ReadU32(base, kUberGlobal);
  const uint32_t save_mgr = uber ? ReadU32(base, uber + kUberSaveMgr) : 0;
  const uint32_t stats = uber ? ReadU32(base, uber + kUberStats) : 0;
  if (!save_mgr || !stats) return;

  if (!g_ss.guest_save_scratch) {
    g_ss.guest_save_scratch = rex::system::kernel_memory()->SystemHeapAlloc(kSaveBlockBytes + 256);
  }
  if (!g_ss.guest_save_scratch) return;

  // Populate CSaveGameManager's header fields exactly like SaveGame (0x82259908..0x82259968),
  // temporarily pointing save_mgr + 128 at our scratch buffer so we never disturb
  // an in-progress local operation on the Host.
  const uint32_t old_buf = ReadU32(base, save_mgr + 128);
  std::memset(GuestPtr(base, g_ss.guest_save_scratch), 0, kSaveBlockBytes);

  CallGame(__imp__sub_8235AA10, ctx, base, save_mgr + 28);
  WriteU32(base, save_mgr + 24, kSaveBlockBytes);  // format word 0x4ECE
  const uint32_t save_lvl = CallGame(__imp__sub_8227BA18, ctx, base, uber);
  WriteU32(base, save_mgr + 100, save_lvl);
  WriteF32(base, save_mgr + 104, ReadF32(base, stats + kStatsPlayTime));
  const uint32_t pct = CallGame(__imp__sub_822E10C8, ctx, base, stats);
  WriteU32(base, save_mgr + 108, pct);
  WriteU32(base, save_mgr + 112, ReadU32(base, kGameObject + 240));
  const uint32_t fe = ReadU32(base, uber + kUberFrontEnd);
  if (fe) WriteU32(base, save_mgr + 116, ReadU32(base, fe + 60));

  // Ensure carried/pocketed state is flushed before serializing GameObject.
  CallGame(__imp__sub_8229CB78, ctx, base);

  WriteU32(base, save_mgr + 128, g_ss.guest_save_scratch);
  CallGame(__imp__sub_8225A7D0, ctx, base, save_mgr);
  WriteU32(base, save_mgr + 128, old_buf);

  const uint16_t seq = ++g_ss.host_save_seq;
  const auto outfits = outfits::CurrentPickBytes();
  const uint8_t flags = cheats::AchievementsBlocked() ? 1u : 0u;
  const uint8_t* raw = GuestPtr(base, g_ss.guest_save_scratch);

  for (uint32_t offset = 0; offset < kSaveBlockBytes; offset += uint32_t(kSaveChunkMax)) {
    const uint16_t chunk_len =
        uint16_t(std::min<uint32_t>(uint32_t(kSaveChunkMax), kSaveBlockBytes - offset));
    std::vector<uint8_t> pkt(14 + chunk_len);
    pkt[0] = kMsgHostSaveChunk;
    PutLE16(pkt.data() + 1, seq);
    PutLE16(pkt.data() + 3, uint16_t(offset));
    PutLE16(pkt.data() + 5, uint16_t(kSaveBlockBytes));
    PutLE16(pkt.data() + 7, chunk_len);
    pkt[9] = flags;
    for (int i = 0; i < 4; ++i) pkt[10 + i] = uint8_t(outfits[size_t(i)]);
    std::memcpy(pkt.data() + 14, raw + offset, chunk_len);
    for (uint32_t peer : peers) {
      SendToPeerFromHost(peer, pkt.data(), pkt.size(), /*reliable=*/true);
    }
  }
  REXLOG_INFO("Online: streamed authoritative Host save ({} bytes, seq {}) to {} client(s)",
              kSaveBlockBytes, seq, peers.size());
}

void TickStatsAndPickups(PPCContext& ctx, uint8_t* base, const Clock::time_point& now) {
  const uint32_t uber = ReadU32(base, kUberGlobal);
  const uint32_t stats = uber ? ReadU32(base, uber + kUberStats) : 0;
  if (!stats || !more_players::InPlay() || more_players::Loading()) {
    g_ss.stats_baseline_valid = false;
    return;
  }

  // 1. Client: apply any authoritative kMsgStatsSync received from Host first,
  //    updating baseline so we never echo Host's own changes back as pickups.
  {
    std::lock_guard lock(g_ss.mutex);
    if (!net::IsHost() && g_ss.pending_stats_apply) {
      g_ss.pending_stats_apply = false;
      for (int i = 0; i < kStatIntCount; ++i) {
        WriteU32(base, stats + kStatIntsOffset + 4 * uint32_t(i), uint32_t(g_ss.rx_stats[size_t(i)]));
        g_ss.prev_stats[size_t(i)] = g_ss.rx_stats[size_t(i)];
      }
      if (guest_memory::Readable(kCocoUnlockedByte, 1)) {
        *GuestPtr(base, kCocoUnlockedByte) = g_ss.rx_coco;
      }
      g_ss.prev_coco = g_ss.rx_coco;
      for (int u = 0; u < 4; ++u) {
        for (int bit = 0; bit < kUnlockTables[u].count; ++bit) {
          if (g_ss.rx_unlocks[size_t(u)] & (1ull << bit)) ApplyUnlockItem(base, u, bit);
        }
        g_ss.prev_unlocks[size_t(u)] = ReadUnlockBits(base, u);
      }
      g_ss.stats_baseline_valid = true;
    }
  }

  // 2. Host: apply queued client pickup events authoritatively.
  bool host_dirty = false;
  if (net::IsHost()) {
    std::vector<PendingPickup> queue;
    {
      std::lock_guard lock(g_ss.mutex);
      queue.swap(g_ss.host_pickup_queue);
    }
    for (const PendingPickup& ev : queue) {
      if (ev.kind == kPickupStatSet && ev.id < kStatIntCount) {
        const uint32_t addr = stats + kStatIntsOffset + 4 * uint32_t(ev.id);
        const int32_t cur = int32_t(ReadU32(base, addr));
        if (ev.value > 0) {
          WriteU32(base, addr, uint32_t(cur + ev.value));
          // If a titan's mojo stat rose, check for titan level-up on any ridden titan using it.
          for (int p = 0; p < kMaxPlayers; ++p) {
            const uint32_t t = more_players::TitanOfPlayer(p);
            const uint32_t upg = UpgradeableOfTitan(base, t);
            if (upg && int32_t(ReadU32(base, upg + kUpgMojoStat)) == int(ev.id)) {
              CallGame(__imp__sub_82177D38, ctx, base, upg, t, 0, 1);
            }
          }
          host_dirty = true;
        }
      } else if (ev.kind == kPickupUnlockItem) {
        ApplyUnlockItem(base, int(ev.id >> 8), int(ev.id & 0xFFu));
        host_dirty = true;
      } else if (ev.kind == kPickupCoco) {
        if (guest_memory::Readable(kCocoUnlockedByte, 1)) *GuestPtr(base, kCocoUnlockedByte) = 1;
        host_dirty = true;
      } else if (ev.kind == kPickupFreeJack && ev.slot < kMaxPlayers) {
        const uint32_t crash = more_players::CharacterOfPlayer(ev.slot);
        const uint32_t ctrl = crash ? ReadU32(base, crash + kActorController) : 0;
        if (ctrl && ReadU32(base, ctrl) == kCrashCtrlVtable) {
          CallGame(__imp__sub_821AE998, ctx, base, ctrl, crash);
        }
      }
    }
  }

  // 3. Detect local positive deltas on stats, unlocks, and Free Jack power-up.
  const int local_slot = std::max(0, net::LocalSlot());
  if (!g_ss.stats_baseline_valid) {
    for (int i = 0; i < kStatIntCount; ++i) {
      g_ss.prev_stats[size_t(i)] = int32_t(ReadU32(base, stats + kStatIntsOffset + 4 * uint32_t(i)));
    }
    for (int u = 0; u < 4; ++u) g_ss.prev_unlocks[size_t(u)] = ReadUnlockBits(base, u);
    g_ss.prev_coco =
        guest_memory::Readable(kCocoUnlockedByte, 1) ? *GuestPtr(base, kCocoUnlockedByte) : 0;
    g_ss.stats_baseline_valid = true;
  } else {
    for (int i = 0; i < kStatIntCount; ++i) {
      const int32_t cur = int32_t(ReadU32(base, stats + kStatIntsOffset + 4 * uint32_t(i)));
      const int32_t delta = cur - g_ss.prev_stats[size_t(i)];
      if (delta != 0) {
        g_ss.prev_stats[size_t(i)] = cur;
        if (net::IsHost()) {
          host_dirty = true;
        } else if (delta > 0) {
          PendingPickup ev{};
          ev.slot = uint8_t(local_slot);
          ev.kind = kPickupStatSet;
          ev.id = uint16_t(i);
          ev.value = delta;
          SendPickupToHostOrBroadcast(ev);
        }
      }
    }
    for (int u = 0; u < 4; ++u) {
      const uint64_t cur = ReadUnlockBits(base, u);
      const uint64_t added = cur & ~g_ss.prev_unlocks[size_t(u)];
      if (added) {
        g_ss.prev_unlocks[size_t(u)] = cur;
        if (net::IsHost()) {
          host_dirty = true;
        } else {
          for (int bit = 0; bit < kUnlockTables[u].count; ++bit) {
            if (added & (1ull << bit)) {
              PendingPickup ev{};
              ev.slot = uint8_t(local_slot);
              ev.kind = kPickupUnlockItem;
              ev.id = uint16_t((u << 8) | bit);
              SendPickupToHostOrBroadcast(ev);
            }
          }
        }
      }
    }
    const uint8_t coco =
        guest_memory::Readable(kCocoUnlockedByte, 1) ? *GuestPtr(base, kCocoUnlockedByte) : 0;
    if (coco != g_ss.prev_coco) {
      g_ss.prev_coco = coco;
      if (net::IsHost()) {
        host_dirty = true;
      } else if (coco) {
        PendingPickup ev{};
        ev.slot = uint8_t(local_slot);
        ev.kind = kPickupCoco;
        SendPickupToHostOrBroadcast(ev);
      }
    }
  }

  // Free Jack power-up detection on our local Crash.
  const uint32_t my_crash = more_players::CharacterOfPlayer(local_slot);
  const uint32_t my_ctrl = my_crash ? ReadU32(base, my_crash + kActorController) : 0;
  if (my_ctrl && ReadU32(base, my_ctrl) == kCrashCtrlVtable) {
    const float fj = ReadF32(base, my_ctrl + kCrashFreeJackTimer);
    if (fj > 1.0f && g_ss.prev_free_jack <= 1.0f) {
      PendingPickup ev{};
      ev.slot = uint8_t(local_slot);
      ev.kind = kPickupFreeJack;
      SendPickupToHostOrBroadcast(ev);
    }
    g_ss.prev_free_jack = fj;
  } else {
    g_ss.prev_free_jack = 0.0f;
  }

  // Host broadcasts full authoritative stats immediately on change + 1 Hz keep-in-step.
  if (net::IsHost() &&
      (host_dirty || now - g_ss.last_stats_broadcast >= std::chrono::milliseconds(1000))) {
    g_ss.last_stats_broadcast = now;
    HostBroadcastFullStats(base, stats);
  }
}

void TickTitanOwnership(PPCContext& ctx, uint8_t* base, const Clock::time_point& now) {
  const int local = net::LocalSlot();
  if (local < 0 || !more_players::InPlay() || more_players::Loading()) return;

  // Clear expired forced dismount.
  if (g_ss.dismounting_titan && now > g_ss.dismount_until) {
    g_ss.dismounting_titan = 0;
  }

  // 1. Broadcast OUR own ridden + pocketed titan whenever either changes.
  //    In-level RB pocketing stores the pocketed CActor* at kGameObject + 32
  //    before StorePlayersBeforeLevelChange writes the string at +44, so query
  //    +32 first and fall back to +44 across loads!
  const uint32_t my_titan = more_players::TitanOfPlayer(local);
  const std::string my_ridden = my_titan ? QueryActorTemplate(ctx, base, my_titan) : std::string();
  const uint32_t pocket_actor = ReadU32(base, kGameObject + 32);
  std::string my_pocket =
      pocket_actor ? QueryActorTemplate(ctx, base, pocket_actor) : std::string();
  if (my_pocket.empty()) {
    my_pocket = ReadCString(base, kGameObject + 44, 64);
  }

  // SENT AGAIN EVERY SECOND, not only on a change (2026-11-27 playtest): a save
  // that starts on a titan announced it ONCE, at the level start, before the
  // other PC was even connected; that PC took "nothing heard" for "rides
  // nothing", and its titan repair pulled player 1 off the Ratcicle there (the
  // titan then dies, as in the original). Reliable and tiny (142 bytes / s).
  const bool changed = my_ridden != g_ss.local_last_ridden || my_pocket != g_ss.local_last_pocket;
  if (changed || now - g_ss.local_titan_sent > std::chrono::seconds(1)) {
    g_ss.local_titan_sent = now;
    g_ss.local_last_ridden = my_ridden;
    g_ss.local_last_pocket = my_pocket;
    {
      std::lock_guard lock(g_ss.mutex);
      g_ss.titans[size_t(local)].ridden_template = my_ridden;
      g_ss.titans[size_t(local)].pocket_template = my_pocket;
    }
    uint8_t pkt[146] = {};
    pkt[0] = kMsgTitanOwnership;
    pkt[1] = uint8_t(local);
    if (!my_ridden.empty()) {
      std::memcpy(pkt + 2, my_ridden.data(), std::min<size_t>(63, my_ridden.size()));
    }
    if (!my_pocket.empty()) {
      std::memcpy(pkt + 66, my_pocket.data(), std::min<size_t>(63, my_pocket.size()));
    }
    PutWireF32(pkt + 130, 100.0f);
    // [142..145] the ridden titan's net id (world_sync), so the other PCs
    // mount their copy of THAT titan (2026-11-27: they spawned a second one).
    const uint32_t ridden_id = my_titan ? net::world_sync::LastNetIdOf(my_titan) : 0;
    for (int i = 0; i < 4; ++i) pkt[142 + i] = uint8_t(ridden_id >> (8 * i));
    if (net::IsHost()) {
      BroadcastFromHost(pkt, sizeof(pkt), /*reliable=*/true);
    } else {
      SendToHost(pkt, sizeof(pkt), /*reliable=*/true);
    }
    if (changed) {
      REXLOG_INFO("Online: P{} titan state -> ridden='{}' pocket='{}'", local + 1, my_ridden,
                  my_pocket);
    }
  }

  // Complete any pending spawned-titan mount from last frame.
  if (g_ss.pending_mount_player < uint32_t(kMaxPlayers)) {
    const int p = int(g_ss.pending_mount_player);
    const uint32_t spawned = spawn::LastSpawned();
    const uint32_t crash = more_players::CharacterOfPlayer(p);
    if (spawned && crash && !more_players::TitanOfPlayer(p)) {
      const std::string sp_tmpl = QueryActorTemplate(ctx, base, spawned);
      if (sp_tmpl == g_ss.pending_mount_template) {
        NativeMountPlayerOnTitan(ctx, base, crash, spawned);
        g_ss.pending_mount_player = ~0u;
        g_ss.pending_mount_template.clear();
      }
    }
  }

  // 2. Reconcile remote players' ridden titans (covers RB pocket-swap, unpocket,
  //    respawn desync, or missed B jack).
  const uint32_t conn_mask = net::ConnectedMask();
  for (int p = 0; p < kMaxPlayers; ++p) {
    if (p == local || !(conn_mask & (1u << p))) continue;
    if (ReadU32(base, kCoOpState + 4 * uint32_t(p)) != 2) continue;

    std::string want_ridden;
    uint32_t want_id = 0;
    Clock::time_point mismatch_since{};
    Clock::time_point last_attempt{};
    {
      std::lock_guard lock(g_ss.mutex);
      if (!g_ss.titans[size_t(p)].known) continue;  // their PC hasn't said yet
      want_ridden = g_ss.titans[size_t(p)].ridden_template;
      want_id = g_ss.titans[size_t(p)].ridden_net_id;
      mismatch_since = g_ss.titans[size_t(p)].mismatch_since;
      last_attempt = g_ss.titans[size_t(p)].last_spawn_attempt;
    }

    const uint32_t cur_titan = more_players::TitanOfPlayer(p);
    const std::string cur_ridden =
        cur_titan ? QueryActorTemplate(ctx, base, cur_titan) : std::string();

    // Same KIND isn't enough: jacking a second Ratcicle while riding one is
    // "Ratcicle" both times. With the owner's titan id known, it must be our
    // copy of THAT titan (2026-11-27 playtest: the other PC kept the old one).
    const uint32_t cur_id = cur_titan ? net::world_sync::LastNetIdOf(cur_titan) : 0;
    const bool other_titan = cur_titan && want_id && cur_id && cur_id != want_id &&
                             net::world_sync::ActorOfNetId(want_id) != 0;
    if (cur_ridden == want_ridden && !other_titan) {
      if (mismatch_since.time_since_epoch().count() != 0) {
        std::lock_guard lock(g_ss.mutex);
        g_ss.titans[size_t(p)].mismatch_since = {};
      }
      // (Antigravity's version wrote OUR pocket name back into the pocket slot
      // here after another player pocketed. The pocket is ONE for everyone, as
      // in the original game (user, 2026-11-27): whoever pockets fills it, on
      // every PC, through the game's own pocketing (kMsgPocket). Removed.)
      continue;
    }

    if (mismatch_since.time_since_epoch().count() == 0) {
      std::lock_guard lock(g_ss.mutex);
      g_ss.titans[size_t(p)].mismatch_since = now;
      continue;
    }

    // Give the game's own B / RB animations time to finish first. Pocketing
    // and taking a titan back out now happen HERE too, from the owner's own
    // decisions (kMsgPocket, CheckInputEvent hook below), and the take-out
    // animation lasts longer than the old 450 ms wait (2026-11-27 playtest:
    // the repair spawned a second Ratcicle 0.47 s after the owner's take-out,
    // then the puppet's own take-out brought the original out too: two
    // Ratcicles, the riderless one treated as an enemy).
    const auto waited = now - mismatch_since;
    // HANDS OFF while the copy's own game is still playing out a replayed titan
    // press (RB pocket / swap / take-out, B get off / jack): those animations
    // take longer than this repair's first step (2026-11-27 playtest: the RB
    // swap was halfway when a 0.3 s mount put player 1 on a titan a second
    // time; the half-linked rider then heard no more presses, the rejack never
    // took, and player 1 was finally dropped there). The repair only steps in
    // when the game didn't sort it out by itself.
    if (now - net::LastRemoteTitanPress(p) < std::chrono::milliseconds(1500)) continue;
    // Titans by outcome (titan_sync.h) is matching this player right now: the
    // repair is the late fallback only.
    if (net::titan_sync::Busy(p)) continue;
    // THE SAME TITAN: on foot here, riding a synced titan there whose copy we
    // have = mount that copy now (the game's own mount), as the owner's jack
    // did. A copy makes no decisions, so it never holds the titan's jack
    // opportunity itself (the jack state alone found nothing to mount), and
    // the spawn below made a SECOND titan (2026-11-27 playtest: "fake" titans).
    if (cur_titan == 0 && want_id && waited >= std::chrono::milliseconds(300) &&
        now - last_attempt > std::chrono::seconds(1)) {
      const uint32_t copy = net::world_sync::ActorOfNetId(want_id);
      const uint32_t crash = more_players::CharacterOfPlayer(p);
      if (copy && crash && guest_memory::Readable(copy, 0x40)) {
        NativeMountPlayerOnTitan(ctx, base, crash, copy);
        {
          std::lock_guard lock(g_ss.mutex);
          g_ss.titans[size_t(p)].last_spawn_attempt = now;
        }
        REXLOG_INFO("Online: P{} mounted on their titan's copy here (net id {}, {:08X})", p + 1, want_id, copy);
        continue;
      }
    }
    if (waited < std::chrono::milliseconds(1500)) continue;

    // Case A: Remote player currently rides a titan locally that they no longer
    // ride on their own PC (pocketed, swapped to a different titan, or dismounted).
        // (A first version KILLED our copy of the old titan here: wrong, the
    // owner's game had POCKETED it (jacking B while riding A pockets A; RB with
    // one pocketed swaps them). Removed: the owner's press is replayed into the
    // copy's own decision instead (kMsgPocket), and this repair is the late
    // fallback: dismount below, then the id mount.)
    if (cur_titan != 0) {
      g_ss.dismounting_titan = cur_titan;
      g_ss.dismount_until = now + std::chrono::milliseconds(600);
      // The forced dismount is answered to the copy's own B decision (lost
      // controller's CheckInputEvent hook): a copy driven by its owner's states
      // makes no decisions, so it never got off (2026-11-27: letting go of a
      // titan stayed mounted on the other PC). Decisions on for that moment.
      // ONCE per 3 s, not every frame: called each frame, it kept the copy
      // deciding for itself as long as the mismatch lasted (2026-11-27
      // playtest: 12 s in which player 2's copy ignored their states, attacked
      // and even re-jacked the titan by itself from the replayed buttons).
      bool open_window = false;
      {
        std::lock_guard lock(g_ss.mutex);
        if (now - g_ss.titans[size_t(p)].last_forced_off > std::chrono::seconds(3)) {
          g_ss.titans[size_t(p)].last_forced_off = now;
          open_window = true;
        }
      }
      if (open_window) {
        net::action_sync::AllowOwnDecisions(p, 800);
        REXLOG_INFO("Online: P{} rides a titan here but not on their PC: forced dismount (own decisions 0.8 s)", p + 1);
      }
      continue;
    }

    // Case B: Remote player is on foot locally, but rides `want_ridden` on their
    // own PC (e.g. swapped from pocket or unpocketed a titan this PC didn't have).
    // Last resort only: a titan of that kind already in this PC's pocket slot
    // (game object +32, the one the puppet pocketed) is about to come out by
    // itself; spawning a copy then makes two. Spawn only after 3 s, or 8 s
    // while the pocket holds that kind.
    const uint32_t pocketed = ReadU32(base, kGameObject + 32);
    const bool same_kind_pocketed =
        pocketed && QueryActorTemplate(ctx, base, pocketed) == want_ridden;
    const auto spawn_after = same_kind_pocketed ? std::chrono::seconds(8) : std::chrono::seconds(3);
    if (waited < spawn_after) continue;
    if (!want_ridden.empty() && now - last_attempt > std::chrono::seconds(2)) {
      {
        std::lock_guard lock(g_ss.mutex);
        g_ss.titans[size_t(p)].last_spawn_attempt = now;
      }
      const uint32_t crash = more_players::CharacterOfPlayer(p);
      const uint32_t mat = crash ? ReadU32(base, crash + kActorMatrix) : 0;
      if (crash && mat && guest_memory::Readable(mat + 48, 12)) {
        float at[3] = {ReadF32(base, mat + 48), ReadF32(base, mat + 52) + 0.3f,
                       ReadF32(base, mat + 56)};
        g_ss.pending_mount_player = uint32_t(p);
        g_ss.pending_mount_template = want_ridden;
        spawn::Request(want_ridden, p, 1, /*knocked_out=*/false, at, /*as_cheat=*/false);
        REXLOG_INFO("Online: spawning & mounting '{}' for remote P{} to sync pocket/swap",
                    want_ridden, p + 1);
      }
    }
  }
}

void EnsureFrontEndHooksRegistered() {
  if (g_ss.fe_hooks_registered) return;
  g_ss.fe_hooks_registered = true;

  // Sync FMV movie skips (state 492 -> exit 494) and 3D NIS cutscene skips
  // (state 543 -> exit 544) natively through FrontEnd's decision dispatcher.
  auto make_movie_or_nis_decision = [](int32_t state_id) {
    return [state_id](PPCContext& ctx, uint8_t* base) -> int32_t {
      if (net::Active() && g_ss.pending_fe_skip_from.load(std::memory_order_relaxed) == state_id) {
        const int32_t exit_id = g_ss.pending_fe_skip_exit.exchange(-1, std::memory_order_relaxed);
        g_ss.pending_fe_skip_from.store(-1, std::memory_order_relaxed);
        if (exit_id >= 0) {
          g_ss.applying_remote_fe_exit = true;
          REXLOG_INFO("Online: applying synced cutscene/movie skip (state {} -> exit {})",
                      state_id, exit_id);
          return exit_id;
        }
      }
      __imp__sub_8211AF40(ctx, base);
      return int32_t(ctx.r3.u32);
    };
  };
  fight_tree::SetFrontEndDecision(492, make_movie_or_nis_decision(492));
  // 3D cutscenes (543) are NOT forced out any more: see AnswerRemoteSceneSkip.

  fight_tree::AddFrontEndExitListener(
      [](PPCContext& /*ctx*/, uint8_t* base, int32_t from, int32_t exit) {
        if (!net::Active() || !more_players::InPlay()) return;
        if (from == 543) {
          std::lock_guard lock(g_ss.mutex);
          g_ss.remote_scene_skip = false;  // our cutscene is over (skipped or ended)
        }
        if (g_ss.applying_remote_fe_exit) {
          g_ss.applying_remote_fe_exit = false;
          return;
        }
        // Broadcast movie/cutscene entries (489 -> 491 FMV, 489 -> 767 3D NIS)
        // and skips (492 -> 494, 543 -> 544) so all players stay in step!
        const bool is_start = (from == 489 && (exit == 491 || exit == 767));
        const bool is_skip = (from == 492 && exit == 494) || (from == 543 && exit == 544);
        if (!is_start && !is_skip) return;
        ExtendGrace(5.0);
        uint8_t pkt[6] = {kMsgCutsceneSync, uint8_t(CurrentLevelId(base))};
        PutLE16(pkt + 2, uint16_t(from));
        PutLE16(pkt + 4, uint16_t(exit));
        if (net::IsHost()) {
          BroadcastFromHost(pkt, sizeof(pkt), /*reliable=*/true);
        } else {
          SendToHost(pkt, sizeof(pkt), /*reliable=*/true);
        }
      });
}

// EXITS WAIT FOR OPEN MENUS (2026-11-27 rework). A level exit in this game is a
// level objective: a trigger volume (CRequirementTriggerVolume) followed by
// CActionBeginChangeLevel (vtable 0x8203F2E8) and CActionChangeLevel (0x8203F36C).
// Every event of an objective is asked "ready?" (vtable slot 1) before it runs
// (slot 2); the objective waits on an event that answers 0. Both level-change
// events answer through sub_822938A8, which already waits while game +0x110 bit
// 0x80 is set and while either player can't take the change. Begin's run
// (sub_82293878) sets game +0x110 bit 0x40 (players held, fade starts);
// ChangeLevel's run (sub_82293D70) loads the level and clears it.
//
// The FIRST online lock skipped ChangeLevel's RUN instead: Begin had already set
// bit 0x40 and started the fade, the run never came back (events run once), so the
// loading screen stayed up forever and the players stayed frozen after the menu
// closed. It also skipped CActionRegisterCheckPoint's run (sub_822975A8): no
// checkpoint. Now the lock answers the game's own "ready?" with "not yet" BEFORE
// Begin, so nothing has started: no fade, no lock, no lost run. The exit fires by
// itself once every menu is closed.
bool HoldExitForOpenMenu(uint8_t* base) {
  if (!net::Active() || !more_players::InPlay() || g_ss.applying_transition) return false;
  if (!net::AnyPlayerInMenu(base)) return false;
  const auto now = Clock::now();
  // One card per wait (a new wait = none in the last 300 ms): the bar queues
  // every card it is given, so a card every few seconds would pile up.
  const bool new_wait = now >= g_ss.exit_waiting_until;
  g_ss.exit_waiting_until = now + std::chrono::milliseconds(300);
  if (new_wait && now - g_ss.last_menu_block_banner > std::chrono::seconds(2)) {
    g_ss.last_menu_block_banner = now;
    int who = -1;
    for (int p = 0; p < kMaxPlayers && who < 0; ++p) {
      if (net::IsPlayerInMenu(p, base)) who = p;
    }
    const std::u16string player(1, char16_t(u'1' + std::max(0, who)));
    achievements::ShowNotice(tr(u"Exit on Hold"),
                             trf(u"Waiting for Player {0} to close their menu.", {player}));
    REXLOG_INFO("Online: level exit waits: player {} has a menu open", who + 1);
  }
  return true;
}

// While an exit waits, the local player (if not in a menu) is held where the wait
// began: no walking out past the exit, no other trigger on the way. The hold ends
// the moment the menus close; the objective then runs its Begin and the level
// changes the game's way.
// Guest: an exit request went to the host (resent by TickFrame until answered).
void NoteExitRequestSent(int8_t level, int8_t entry) {
  std::lock_guard lock(g_ss.mutex);
  g_ss.sent_request = {true, level, entry, 0};
  g_ss.sent_request_at = Clock::now();
}

void HoldPlayerAtWaitingExit(uint8_t* base, const Clock::time_point& now) {
  const int slot = net::LocalSlot();
  const bool waiting = now < g_ss.exit_waiting_until && slot >= 0 && slot < kMaxPlayers &&
                       !IsLocalMenuOpen(base);
  if (!waiting) {
    g_ss.exit_hold_valid = false;
    return;
  }
  const uint32_t titan = more_players::TitanOfPlayer(slot);
  const uint32_t body = titan ? titan : more_players::CharacterOfPlayer(slot);
  uint32_t phys = body ? ReadU32(base, body + 308) : 0;
  if (!phys || ReadU32(base, phys) != 0x82032FC4u || ReadU32(base, phys + 88) != body) {
    phys = net::LookupCachedPhysics(base, body);
  }
  if (!phys || !guest_memory::Readable(phys + 104, 48)) return;
  if (!g_ss.exit_hold_valid || g_ss.exit_hold_body != body) {
    g_ss.exit_hold_valid = true;
    g_ss.exit_hold_body = body;
    for (int i = 0; i < 3; ++i) g_ss.exit_hold_pos[i] = ReadF32(base, phys + 140 + 4 * i);
  }
  WriteF32(base, phys + 140, g_ss.exit_hold_pos[0]);  // x, z: held; y: the game's (ground)
  WriteF32(base, phys + 148, g_ss.exit_hold_pos[2]);
  WriteF32(base, phys + 104, 0.0f);
  WriteF32(base, phys + 112, 0.0f);
}

void TickOutfitsAndRemoteInteracts(PPCContext& ctx, uint8_t* base, const Clock::time_point& now) {
  const int local = net::LocalSlot();
  if (local < 0 || !more_players::InPlay() || more_players::Loading()) return;

  // 1. Broadcast local worn outfit on change (+ 2 s keep-in-step for new peers).
  const int my_outfit = outfits::OutfitOf(local);
  if (outfits::ValidOutfit(my_outfit) &&
      (my_outfit != g_ss.local_last_outfit ||
       now - g_ss.last_outfit_broadcast >= std::chrono::seconds(2))) {
    g_ss.local_last_outfit = my_outfit;
    g_ss.last_outfit_broadcast = now;
    uint8_t pkt[3] = {kMsgOutfitSync, uint8_t(local), uint8_t(int8_t(my_outfit))};
    if (net::IsHost()) {
      BroadcastFromHost(pkt, sizeof(pkt), /*reliable=*/true);
    } else {
      SendToHost(pkt, sizeof(pkt), /*reliable=*/true);
    }
  }

  // 2. Apply remote players' worn outfits once their inventory package streams in.
  const uint32_t conn_mask = net::ConnectedMask();
  for (int p = 0; p < kMaxPlayers; ++p) {
    if (p == local || !(conn_mask & (1u << p))) continue;
    int want_outfit = -1;
    {
      std::lock_guard lock(g_ss.mutex);
      want_outfit = g_ss.remote_target_outfit[size_t(p)];
    }
    if (want_outfit >= 0 && outfits::ValidOutfit(want_outfit) &&
        outfits::OutfitOf(p) != want_outfit) {
      outfits::ApplyRemoteOutfit(ctx, base, p, want_outfit);
    }
  }

  // 3. Replay shared NPC / world interactions triggered by remote players (e.g.
  //    talking to Coco for Mission 1) while suppressing local-only menu popups!
  std::vector<PendingRemoteInteract> interacts;
  {
    std::lock_guard lock(g_ss.mutex);
    interacts.swap(g_ss.pending_remote_interacts);
  }
  if (interacts.empty()) return;

  constexpr uint32_t kInteractableVtable = 0x8202EA6C;
  const auto known = prompt_icons::KnownInteractables();
  const uint32_t sc = EnsureScratch();
  if (!sc) return;

  for (const PendingRemoteInteract& ev : interacts) {
    if (ev.slot >= kMaxPlayers || int(ev.slot) == local) continue;
    const uint32_t remote_crash = more_players::CharacterOfPlayer(ev.slot);
    if (!remote_crash) continue;

    uint32_t best_obj = 0;
    float best_d2 = 25.0f * 25.0f;
    for (uint32_t it : known) {
      if (!guest_memory::Readable(it, 32) || ReadU32(base, it) != kInteractableVtable) continue;
      const uint32_t obj = ReadU32(base, it + 28);
      if (!obj || !guest_memory::Readable(obj + kActorMatrix, 4)) continue;
      const uint32_t obj_hash = ReadU32(base, obj + kActorNameHash);
      if (ev.hash != 0 && obj_hash != ev.hash) continue;
      const uint32_t mat = ReadU32(base, obj + kActorMatrix);
      if (!mat || !guest_memory::Readable(mat + 48, 12)) continue;
      const float dx = ReadF32(base, mat + 48) - ev.pos[0];
      const float dy = ReadF32(base, mat + 52) - ev.pos[1];
      const float dz = ReadF32(base, mat + 56) - ev.pos[2];
      const float d2 = dx * dx + dy * dy + dz * dz;
      if (d2 < best_d2) {
        best_d2 = d2;
        best_obj = obj;
      }
    }
    if (!best_obj) continue;

    REXLOG_INFO("Online: replaying shared world interaction from P{} on object {:08X} (hash {:08X})",
                ev.slot + 1, best_obj, ev.hash);
    ExtendGrace(4.0);
    g_ss.replaying_remote_interact = true;
    g_ss.active_interact_player = int(ev.slot);
    g_ss.last_interact_player = int(ev.slot);
    g_ss.last_interact_time = now;

    std::memset(GuestPtr(base, sc + 640), 0, 128);
    {
      const PPCContext saved = ctx;
      ctx.r3.u64 = sc + 640;
      ctx.r4.u64 = 2;
      ctx.r5.u64 = remote_crash;
      ctx.r6.u64 = best_obj;
      ctx.r7.u64 = ev.flag;
      ctx.r8.u64 = 0;
      __imp__sub_820B5668(ctx, base);
      ctx = saved;
    }
    CallGame(__imp__sub_820B0B70, ctx, base, best_obj, sc + 640);
    CallGame(__imp__sub_820B56C8, ctx, base, sc + 640);

    g_ss.active_interact_player = -1;
    g_ss.replaying_remote_interact = false;
  }
}

}  // namespace

void Reset() {
  std::lock_guard lock(g_ss.mutex);
  g_ss.local_menu_paused = false;
  g_ss.local_menu_open_cached = false;
  g_ss.menu_button_block_until_ns = 0;
  g_ss.prev_menu_open = false;
  g_ss.transition_epoch = 0;
  g_ss.pending_transition = {};
  g_ss.applying_transition = false;
  g_ss.exit_waiting_until = {};
  g_ss.exit_hold_valid = false;
  g_ss.queued_request = {};
  g_ss.sent_request = {};
  g_ss.was_loading = false;
  g_ss.grace_until_ns = 0;
  g_ss.active_interact_player = -1;
  g_ss.last_interact_player = -1;
  g_ss.local_interact_opened_menu = false;
  g_ss.replaying_remote_interact = false;
  g_ss.pending_remote_interacts.clear();
  g_ss.pending_fe_skip_from = -1;
  g_ss.pending_fe_skip_exit = -1;
  g_ss.remote_scene_skip = false;
  g_ss.applying_remote_fe_exit = false;
  g_ss.stats_baseline_valid = false;
  g_ss.pending_stats_apply = false;
  g_ss.host_pickup_queue.clear();
  g_ss.hidden_collectables.clear();
  for (auto& t : g_ss.titans) t = PlayerTitanState{};
  g_ss.local_last_ridden.clear();
  g_ss.local_last_pocket.clear();
  g_ss.next_pocket_allowed = {};
  g_ss.dismounting_titan = 0;
  g_ss.pending_mount_player = ~0u;
  g_ss.pending_mount_template.clear();
  g_ss.local_last_outfit = -1;
  g_ss.last_outfit_broadcast = {};
  g_ss.remote_target_outfit = {-1, -1, -1, -1};
  g_ss.save_request_peers.clear();
  g_ss.rx_save_staging.clear();
  g_ss.client_ready_save.clear();
  g_ss.client_has_host_save = false;
}

// The level change of CActionChangeLevel's run (sub_82293D70), done by us: the
// respawn place (game +244/+248), the players stored (sub_8229CB78), the change
// (sub_8227B640), then game +0x110 bit 0x40 cleared (set by Begin = players held).
void ChangeLevelLikeTheGame(PPCContext& ctx, uint8_t* base, uint32_t uber, int8_t level,
                            int8_t entry) {
  WriteU32(base, kGameObject + 244, uint32_t(int32_t(level)));
  WriteU32(base, kGameObject + 248, uint32_t(int32_t(entry)));
  CallGame(__imp__sub_8229CB78, ctx, base);
  CallGame(__imp__sub_8227B640, ctx, base, uber, uint32_t(int32_t(level)), uint32_t(int32_t(entry)));
  *GuestPtr(base, kGameObject + 0x110) &= uint8_t(~0x40u);
}

void TickFrame(PPCContext& ctx, uint8_t* base) {
  EnsureFrontEndHooksRegistered();
  if (!net::Active()) return;
  net::ApplyRemoteHealth(ctx, base);  // other players' health, as their own PCs say
  net::mask_sync::Tick(ctx, base);    // masks: announce ours, copies follow theirs
  net::titan_sync::Tick(ctx, base);   // titans: announce ours, copies follow theirs
  const auto now = Clock::now();
  const uint32_t uber = ReadU32(base, kUberGlobal);
  if (!uber) return;

  // 1. NON-PAUSING WORLD ENFORCEMENT:
  //    While in a level online, keep uber +168 pause bits (0x10 | 0x20) and
  //    CTimeManager +24 pause bit (0x80) cleared so no menu, Save Totem, Outfit
  //    Chest, Level-Up screen, or script ever freezes the live simulation!
  if (more_players::InPlay()) {
    uint8_t* uber_flags = GuestPtr(base, uber + kUberFlags);
    if (*uber_flags & (kPauseBitMenu | kPauseBitGame)) {
      g_ss.local_menu_paused = true;
      *uber_flags &= ~uint8_t(kPauseBitMenu | kPauseBitGame);
    }
    const uint32_t time_mgr = ReadU32(base, uber + kUberTimeMgr);
    if (time_mgr && guest_memory::Readable(time_mgr + kTimeMgrFlags, 1)) {
      *GuestPtr(base, time_mgr + kTimeMgrFlags) &= ~kTimeMgrPausedBit;
    }
    const bool menu_open = IsLocalMenuOpen(base);
    if (!menu_open) {
      g_ss.local_menu_paused = false;
    }
    // While a local menu/totem is open or on the exact frame it closes, clear
    // sequence progress & happened bits on all non-menu CInputMaps (GENERAL,
    // PLAYER, ENEMY, PROJECTILE, CAMERA) of our local player's root controller
    // so menu button presses never prime or trigger gameplay actions!
    if (menu_open || g_ss.prev_menu_open) {
      const int slot = net::LocalSlot();
      // Keep the local player completely immovable (zero horizontal velocity)
      // while inside any menu (Outfit Chest, Save List, Pause, Upgrade).
      if (menu_open && slot >= 0 && slot < 4) {
        const uint32_t titan = more_players::TitanOfPlayer(slot);
        const uint32_t body = titan ? titan : more_players::CharacterOfPlayer(slot);
        uint32_t phys = body ? ReadU32(base, body + 308) : 0;
        if (!phys || ReadU32(base, phys) != 0x82032FC4u || ReadU32(base, phys + 88) != body) {
          phys = net::LookupCachedPhysics(base, body);
        }
        if (phys) {
          WriteF32(base, phys + 104, 0.0f);
          WriteF32(base, phys + 112, 0.0f);
        }
      }
      const uint32_t input_mgr = ReadU32(base, uber + 56);
      const uint32_t list = input_mgr ? ReadU32(base, input_mgr + 8) : 0;
      const uint32_t root = (slot >= 0 && slot < 4 && list)
                                ? ReadU32(base, list + 4 * uint32_t(slot))
                                : 0;
      if (root && guest_memory::Readable(root + 0x198, 8)) {
        const uint32_t begin = ReadU32(base, root + 0x198);
        const uint32_t end = ReadU32(base, root + 0x19C);
        if (begin && end >= begin && (end - begin) <= 64) {
          for (uint32_t p = begin; p < end; p += 4) {
            const uint32_t map = ReadU32(base, p);
            if (!map || !guest_memory::Readable(map + 0x18, 4)) continue;
            const int32_t type = int32_t(ReadU32(base, map + 0x18));
            if (type != 1 && type != 2) {
              CallGame(__imp__sub_82275C90, ctx, base, map);
            }
          }
        }
      }
    }
    g_ss.prev_menu_open = menu_open;
  } else {
    g_ss.local_menu_paused = false;
    g_ss.local_menu_open_cached = false;
    g_ss.prev_menu_open = false;
  }

  // Track loading screen exits -> extend void-death / snap grace by 2.5 s so
  // players finishing an area load at slightly different instants never die.
  const bool loading = more_players::Loading();
  if (loading) {
    ExtendGrace(2.5);
  } else if (g_ss.was_loading) {
    ExtendGrace(2.5);
  }
  g_ss.was_loading = loading;

  // 2. Execute any pending authoritative area/level transition from Host
  //    (waiting if any connected player currently has a menu open!).
  PendingTransition trans{};
  if (!net::AnyPlayerInMenu(base)) {
    std::lock_guard lock(g_ss.mutex);
    if (g_ss.pending_transition.pending && !loading) {
      trans = g_ss.pending_transition;
      g_ss.pending_transition.pending = false;
    }
  }
  if (trans.pending && more_players::InPlay()) {
    const int cur_lvl = CurrentLevelId(base);
    const int cur_ent = int(int8_t(*GuestPtr(base, uber + kUberLoadedEntry)));
    if (cur_lvl != int(trans.level) || cur_ent != int(trans.entry)) {
      REXLOG_INFO("Online: applying synced area transition -> level {} entry {}", int(trans.level),
                  int(trans.entry));
      ExtendGrace(4.0);
      g_ss.applying_transition = true;
      ChangeLevelLikeTheGame(ctx, base, uber, trans.level, trans.entry);
      g_ss.applying_transition = false;
      world_sync::Reset();
      net::enemy_input::Reset();
      return;
    }
  }

  // 2b. Exits waiting for open menus: hold the local player there; the host sends
  //     on a guest's request that came while a menu was open; a guest resends its
  //     request every 3 s until the host answers (a reliable message can still
  //     arrive while the host is between states).
  HoldPlayerAtWaitingExit(base, now);
  if (!loading && !net::AnyPlayerInMenu(base)) {
    std::lock_guard lock(g_ss.mutex);
    if (net::IsHost() && g_ss.queued_request.pending) {
      const PendingTransition q = g_ss.queued_request;
      g_ss.queued_request.pending = false;
      const uint32_t epoch = ++g_ss.transition_epoch;
      g_ss.pending_transition = {true, q.level, q.entry, epoch};
      uint8_t out[8] = {kMsgAreaTransition, uint8_t(q.level), uint8_t(q.entry),
                        g_ss.queued_initiator};
      PutLE32(out + 4, epoch);
      BroadcastFromHost(out, sizeof(out), /*reliable=*/true);
      REXLOG_INFO("Online: sent on P{}'s held exit -> level {} entry {}",
                  int(g_ss.queued_initiator) + 1, int(q.level), int(q.entry));
    }
    if (!net::IsHost() && g_ss.sent_request.pending && net::LocalSlot() < 0) {
      // The host left after our ChangeLevel asked it (the run already returned,
      // game +0x110 bit 0x40 still set = players held): change level here, the
      // way ChangeLevel's run does, so nobody is left frozen.
      const PendingTransition q = g_ss.sent_request;
      g_ss.sent_request.pending = false;
      REXLOG_WARN("Online: host gone with our exit request out: changing level here ({} / {})",
                  int(q.level), int(q.entry));
      g_ss.applying_transition = true;
      ChangeLevelLikeTheGame(ctx, base, uber, q.level, q.entry);
      g_ss.applying_transition = false;
      return;
    }
    if (!net::IsHost() && g_ss.sent_request.pending &&
        now - g_ss.sent_request_at > std::chrono::seconds(3)) {
      g_ss.sent_request_at = now;
      uint8_t pkt[8] = {kMsgAreaTransition, uint8_t(g_ss.sent_request.level),
                        uint8_t(g_ss.sent_request.entry), uint8_t(std::max(0, net::LocalSlot()))};
      SendToHost(pkt, sizeof(pkt), /*reliable=*/true);
      REXLOG_INFO("Online: exit request resent (level {} entry {})", int(g_ss.sent_request.level),
                  int(g_ss.sent_request.entry));
    }
  }

  // 3. Shared stats / mojo / voodoo doll / power-up sync.
  TickStatsAndPickups(ctx, base, now);

  // 4. Ridden + pocketed titan ownership sync.
  TickTitanOwnership(ctx, base, now);

  // 4b. Online outfit sync + shared NPC/world interactions.
  TickOutfitsAndRemoteInteracts(ctx, base, now);

  // 5. Host: stream live save block to any client that opened Save Game.
  if (net::IsHost()) {
    ServiceHostSaveRequests(ctx, base);
  }

  // Prune expired collectable-hide entries.
  {
    std::lock_guard lock(g_ss.mutex);
    std::erase_if(g_ss.hidden_collectables,
                  [&](const PendingCollectableHide& h) { return now > h.expires; });
  }
}

void HandlePacket(const uint8_t* data, size_t size, bool is_host, uint32_t sender_peer) {
  if (!data || size == 0) return;
  const uint8_t type = data[0];

  switch (type) {
    case kMsgAreaTransition: {
      if (size < 8) break;
      const int8_t level = int8_t(data[1]);
      const int8_t entry = int8_t(data[2]);
      const uint8_t initiator = data[3];
      uint32_t epoch = GetLE32(data + 4);
      if (is_host) {
        std::lock_guard lock(g_ss.mutex);
        if (net::AnyPlayerInMenu(nullptr)) {
          // A menu opened in between (the guest's own exit waited while it saw
          // one): kept and sent on once the menus close (TickFrame), never
          // dropped: the guest's ChangeLevel has already run and waits for it.
          g_ss.queued_request = {true, level, entry, 0};
          g_ss.queued_initiator = initiator;
          break;
        }
        // Client walked into a door/area trigger: Host adopts & broadcasts to all!
        epoch = ++g_ss.transition_epoch;
        g_ss.pending_transition = {true, level, entry, epoch};
        uint8_t out[8] = {kMsgAreaTransition, uint8_t(level), uint8_t(entry), initiator};
        PutLE32(out + 4, epoch);
        BroadcastFromHost(out, sizeof(out), /*reliable=*/true);
      } else {
        std::lock_guard lock(g_ss.mutex);
        g_ss.sent_request.pending = false;  // the host answered
        if (epoch > g_ss.transition_epoch) {
          g_ss.transition_epoch = epoch;
          g_ss.pending_transition = {true, level, entry, epoch};
        }
      }
      ExtendGrace(4.0);
      break;
    }

    case kMsgCutsceneSync: {
      if (size < 6) break;
      if (is_host) {
        BroadcastFromHost(data, size, /*reliable=*/true);
      }
      const int32_t from = int32_t(int16_t(GetLE16(data + 2)));
      const int32_t exit = int32_t(int16_t(GetLE16(data + 4)));
      if (from == 492 && exit == 494) {
        g_ss.pending_fe_skip_exit.store(exit, std::memory_order_relaxed);
        g_ss.pending_fe_skip_from.store(from, std::memory_order_relaxed);
      } else if (from == 543 && exit == 544) {
        // Another PC left the 3D cutscene: skip ours through the game's own
        // Skip prompt (AnswerRemoteSceneSkip), never by forcing the exit.
        std::lock_guard lock(g_ss.mutex);
        g_ss.remote_scene_skip_at = Clock::now();
        g_ss.remote_scene_skip = true;
      }
      ExtendGrace(5.0);
      break;
    }

    case kMsgWorldInteract: {
      if (size < 22) break;
      const uint8_t slot = data[1];
      if (slot >= kMaxPlayers) break;
      if (is_host) {
        BroadcastFromHost(data, size, /*reliable=*/true);
      }
      if (int(slot) == net::LocalSlot()) break;
      PendingRemoteInteract ev{};
      ev.slot = slot;
      ev.hash = GetLE32(data + 2);
      ev.flag = GetLE32(data + 6);
      for (int k = 0; k < 3; ++k) ev.pos[k] = GetWireF32(data + 10 + 4 * k);
      std::lock_guard lock(g_ss.mutex);
      g_ss.pending_remote_interacts.push_back(ev);
      break;
    }

    case kMsgOutfitSync: {
      if (size < 3) break;
      const uint8_t slot = data[1];
      const int outfit = int(int8_t(data[2]));
      if (slot >= kMaxPlayers || !outfits::ValidOutfit(outfit)) break;
      if (is_host) {
        BroadcastFromHost(data, size, /*reliable=*/true);
      }
      if (int(slot) == net::LocalSlot()) break;
      std::lock_guard lock(g_ss.mutex);
      g_ss.remote_target_outfit[slot] = outfit;
      break;
    }

    case kMsgTitanOwnership: {
      if (size < 142) break;
      const uint8_t slot = data[1];
      if (slot >= kMaxPlayers || int(slot) == net::LocalSlot()) break;
      if (is_host) {
        BroadcastFromHost(data, size, /*reliable=*/true);
      }
      std::string ridden, pocket;
      for (size_t i = 0; i < 64 && data[2 + i] != 0; ++i) ridden.push_back(char(data[2 + i]));
      for (size_t i = 0; i < 64 && data[66 + i] != 0; ++i) pocket.push_back(char(data[66 + i]));
      std::lock_guard lock(g_ss.mutex);
      auto& st = g_ss.titans[slot];
      // The same state comes again every second: only a CHANGE restarts the
      // repair's wait (else the repeats would keep it waiting forever).
      const bool changed = !st.known || st.ridden_template != ridden || st.pocket_template != pocket;
      st.known = true;
      st.ridden_template = std::move(ridden);
      st.pocket_template = std::move(pocket);
      st.hp = GetWireF32(data + 130);
      st.ridden_net_id = size >= 146 ? GetLE32(data + 142) : 0;
      st.mojo = int32_t(GetLE32(data + 134));
      st.level = int32_t(GetLE32(data + 138));
      if (changed) {
        st.mismatch_since = {};
        REXLOG_INFO("Online: RX P{} titan ownership -> ridden='{}' pocket='{}'", slot + 1,
                    st.ridden_template, st.pocket_template);
      }
      break;
    }

    case kMsgPickupEvent: {
      if (size < 25) break;
      PendingPickup ev{};
      ev.slot = data[1];
      ev.kind = data[2];
      ev.id = GetLE16(data + 3);
      ev.value = int32_t(GetLE32(data + 5));
      ev.hash = GetLE32(data + 9);
      for (int k = 0; k < 3; ++k) ev.pos[k] = GetWireF32(data + 13 + 4 * k);

      if (is_host) {
        // Relay CollectableTaken & FreeJack to other clients; stats/unlocks go into
        // Host's authoritative queue and broadcast via kMsgStatsSync.
        if (ev.kind == kPickupCollectableTaken || ev.kind == kPickupFreeJack) {
          BroadcastFromHost(data, size, /*reliable=*/true);
        }
        std::lock_guard lock(g_ss.mutex);
        if (ev.kind == kPickupCollectableTaken) {
          g_ss.hidden_collectables.push_back(
              {ev.hash, {ev.pos[0], ev.pos[1], ev.pos[2]}, Clock::now() + std::chrono::seconds(5)});
        } else {
          g_ss.host_pickup_queue.push_back(ev);
        }
      } else {
        if (ev.kind == kPickupCollectableTaken && int(ev.slot) != net::LocalSlot()) {
          std::lock_guard lock(g_ss.mutex);
          g_ss.hidden_collectables.push_back(
              {ev.hash, {ev.pos[0], ev.pos[1], ev.pos[2]}, Clock::now() + std::chrono::seconds(5)});
        }
      }
      break;
    }

    case kMsgStatsSync: {
      if (is_host || size < 770) break;
      const uint32_t seq = GetLE32(data + 1);
      std::lock_guard lock(g_ss.mutex);
      if (seq <= g_ss.latest_rx_stats_seq && (g_ss.latest_rx_stats_seq - seq) < 0x80000000u) break;
      g_ss.latest_rx_stats_seq = seq;
      for (int i = 0; i < kStatIntCount; ++i) {
        g_ss.rx_stats[size_t(i)] = int32_t(GetLE32(data + 5 + 4 * i));
      }
      g_ss.rx_coco = data[737];
      for (int u = 0; u < 4; ++u) {
        const uint64_t lo = GetLE32(data + 738 + 8 * u);
        const uint64_t hi = GetLE32(data + 738 + 8 * u + 4);
        g_ss.rx_unlocks[size_t(u)] = lo | (hi << 32);
      }
      g_ss.pending_stats_apply = true;
      break;
    }

    case kMsgRequestHostSave: {
      if (!is_host || size < 2) break;
      std::lock_guard lock(g_ss.mutex);
      if (std::find(g_ss.save_request_peers.begin(), g_ss.save_request_peers.end(), sender_peer) ==
          g_ss.save_request_peers.end()) {
        g_ss.save_request_peers.push_back(sender_peer);
      }
      break;
    }

    case kMsgHostSaveChunk: {
      if (is_host || size < 14) break;
      const uint16_t seq = GetLE16(data + 1);
      const uint16_t offset = GetLE16(data + 3);
      const uint16_t total = GetLE16(data + 5);
      const uint16_t len = GetLE16(data + 7);
      if (total != kSaveBlockBytes || size < size_t(14 + len) ||
          uint32_t(offset) + uint32_t(len) > kSaveBlockBytes) {
        break;
      }
      std::lock_guard lock(g_ss.mutex);
      if (seq != g_ss.rx_save_seq) {
        g_ss.rx_save_seq = seq;
        g_ss.rx_save_received_bytes = 0;
        g_ss.rx_save_staging.assign(kSaveBlockBytes, 0);
      }
      std::memcpy(g_ss.rx_save_staging.data() + offset, data + 14, len);
      g_ss.rx_save_received_bytes = uint16_t(std::min<uint32_t>(
          kSaveBlockBytes, uint32_t(g_ss.rx_save_received_bytes) + uint32_t(len)));
      if (uint32_t(offset) + uint32_t(len) == kSaveBlockBytes &&
          g_ss.rx_save_received_bytes >= kSaveBlockBytes) {
        g_ss.client_ready_save = g_ss.rx_save_staging;
        g_ss.client_ready_cheated = (data[9] & 1u) != 0;
        for (int i = 0; i < 4; ++i) g_ss.client_ready_outfits[size_t(i)] = int8_t(data[10 + i]);
        g_ss.client_has_host_save = true;
        REXLOG_INFO("Online: received complete Host save snapshot (seq {}, {} bytes)", seq,
                    kSaveBlockBytes);
      }
      break;
    }

    default:
      break;
  }
}

bool IsLocalMenuOpen(uint8_t* base) {
  if (!net::Active() || !base || !more_players::InPlay()) {
    g_ss.local_menu_open_cached.store(false, std::memory_order_relaxed);
    return false;
  }
  const int32_t fe_state = fight_tree::CurrentFrontEnd().state;
  // Normal plain gameplay is FrontEnd state 489 (plus 486 entering 489), while
  // FMV movies are 492 and 3D NIS cutscenes are 543. Every other FrontEnd state
  // while InPlay() (e.g. 386 Save Slot List, 412 Overwrite Confirm, 326/329
  // Saving, Rename Screen, 868/901 Pause, 847 Outfit Chest, 839 Level-Up, 833
  // Save Totem Confirm) is an active UI menu where Crash must not move/attack!
  const bool in_fe_menu = fe_state > 0 && fe_state != 489 && fe_state != 486 &&
                          fe_state != 492 && fe_state != 543;
  const bool kbm_menu =
      kbm::KeyboardMouseDriver::Get() && kbm::KeyboardMouseDriver::Get()->menu_open();
  bool open = outfit_picker::IsOpen() || menu_input::MenuOnScreen() || in_fe_menu || kbm_menu;
  const uint32_t uber = ReadU32(base, kUberGlobal);
  const uint32_t fe = uber ? ReadU32(base, uber + kUberFrontEnd) : 0;
  if (!open && fe && guest_memory::Readable(fe + kFeFlags2, 1)) {
    // fe + 8592 (+0x2190): 0x20 Map, 0x08 Tutorial, 0x04 Save Totem prompt
    // (set immediately on pressing B at a Save Tiki Totem during its opening
    // animation!), 0x02 Mini-game menu, 0x01 Game Complete.
    const uint8_t menu_flags = *GuestPtr(base, fe + 8592);
    const bool in_game_overlay = (menu_flags & 0x2Fu) != 0;
    const bool on_screen = (*GuestPtr(base, fe + kFeFlags2) & (kFeMenuOnScreen | kFeInMainMenus)) != 0;
    const bool confirm = (*GuestPtr(base, fe + kFeConfirmBoxFlags) & kFeConfirmShowing) != 0;
    const bool owner_paused = int32_t(ReadU32(base, fe + kFeOwner)) >= 0;
    open = in_game_overlay || on_screen || confirm || owner_paused;
  }
  g_ss.local_menu_open_cached.store(open, std::memory_order_relaxed);
  if (open) {
    // Hold a short 220 ms post-menu button-drain window so pressing A ("Resume"
    // / "Yes") or B ("Back" / "No") to close a menu never fires a Jump or Jack
    // on the frame play resumes.
    const auto until = Clock::now() + std::chrono::milliseconds(220);
    const int64_t ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(until.time_since_epoch()).count();
    g_ss.menu_button_block_until_ns.store(ns, std::memory_order_relaxed);
  }
  return open;
}

bool IsLocalMenuOpenCached() {
  return net::Active() && g_ss.local_menu_open_cached.load(std::memory_order_relaxed);
}

bool ShouldBlockLocalGameplayInput(uint8_t* base) {
  if (!net::Active() || !more_players::InPlay()) return false;
  if (base && IsLocalMenuOpen(base)) return true;
  if (!base && g_ss.local_menu_open_cached.load(std::memory_order_relaxed)) return true;
  const int64_t until = g_ss.menu_button_block_until_ns.load(std::memory_order_relaxed);
  if (until == 0) return false;
  const int64_t now_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
  return now_ns < until;
}

int CurrentLevelId(uint8_t* base) {
  if (!base || !more_players::InPlay()) return -1;
  const uint32_t uber = ReadU32(base, kUberGlobal);
  if (!uber || !guest_memory::Readable(uber + kUberLoadedLevel, 1)) return -1;
  return int(int8_t(*GuestPtr(base, uber + kUberLoadedLevel)));
}

bool InTransitionOrCutsceneGrace() {
  if (more_players::Loading()) return true;
  const int64_t until = g_ss.grace_until_ns.load(std::memory_order_relaxed);
  if (until == 0) return false;
  const int64_t now_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
  return now_ns < until;
}

// A 3D CUTSCENE SKIPPED ON ANOTHER PC (2026-11-27). Antigravity's first
// version answered the front end's decision for state 543 with exit 544, i.e.
// yanked the front end out of the cutscene. The cutscene's own ending never
// ran: its fade to black had started (front end +140 = +2, +144 = 1) and the
// fade back in belongs to that ending -> the picture stayed black under the
// loading screen for good while the game ran on (playtest: "player 1 in a
// loading screen infinitely"; unstuck by writing +140 = -2).
// Now the game skips by itself, exactly as if someone pressed Skip: the
// cutscene step's playing state sub_82242D50 asks the front end's
// IsButtonPressed (sub_822652B0) two things (findings/68 s.6k, xexdis):
//   * back at 0x82242E98: START while the prompt is hidden -> shows it
//     (+428 bit 0x01), only asked when the scene is skippable (+428 bit 0x08);
//   * back at 0x82242E1C: A while the prompt shows -> the step's skip state 4,
//     with its own fade out and back in.
// Both answer yes until our own cutscene leaves 543 (any reason), at most 4 s.
bool AnswerRemoteSceneSkip(uint32_t lr, uint32_t button) {
  if (!net::Active()) return false;
  const bool start_ask = lr == 0x82242E98u && button == 36;
  const bool a_ask = lr == 0x82242E1Cu && button == 1;
  if (!start_ask && !a_ask) return false;
  std::lock_guard lock(g_ss.mutex);
  if (!g_ss.remote_scene_skip) return false;
  if (Clock::now() - g_ss.remote_scene_skip_at > std::chrono::seconds(4)) {
    g_ss.remote_scene_skip = false;
    return false;
  }
  return true;
}

bool IsRemotePlayerActor(uint32_t actor) {
  if (!net::Active() || !actor) return false;
  const int local = net::LocalSlot();
  for (int p = 0; p < kMaxPlayers; ++p) {
    if (p == local) continue;
    if (actor == more_players::CharacterOfPlayer(p) || actor == more_players::TitanOfPlayer(p)) {
      return true;
    }
  }
  return false;
}

bool ShouldForceDismountTitan(uint32_t titan_actor) {
  return net::Active() && titan_actor != 0 && g_ss.dismounting_titan == titan_actor;
}

void RequestHostSaveSnapshot() {
  if (!net::Active() || net::IsHost()) return;
  const int slot = net::LocalSlot();
  if (slot < 0) return;
  uint8_t pkt[2] = {kMsgRequestHostSave, uint8_t(slot)};
  SendToHost(pkt, sizeof(pkt), /*reliable=*/true);
}

bool GetHostSaveForClient(std::vector<uint8_t>& out_block_20174,
                          std::array<int8_t, 4>& out_outfits,
                          bool& out_cheated) {
  if (!net::Active() || net::IsHost()) return false;
  std::lock_guard lock(g_ss.mutex);
  if (!g_ss.client_has_host_save || g_ss.client_ready_save.size() != kSaveBlockBytes) return false;
  out_block_20174 = g_ss.client_ready_save;
  out_outfits = g_ss.client_ready_outfits;
  out_cheated = g_ss.client_ready_cheated;
  return true;
}

void BeginPlayerInteract(int player) {
  g_ss.active_interact_player = player;
  g_ss.local_interact_opened_menu = false;
  if (player >= 0) {
    g_ss.last_interact_player = player;
    g_ss.last_interact_time = Clock::now();
  }
}

void EndPlayerInteract(PPCContext& /*ctx*/, uint8_t* base, int player, uint32_t /*actor*/,
                       uint32_t target, uint32_t interactor_flag) {
  const bool opened_local_menu = g_ss.local_interact_opened_menu;
  g_ss.active_interact_player = -1;
  g_ss.local_interact_opened_menu = false;
  if (!net::Active() || !more_players::InPlay() || more_players::Loading()) return;
  if (g_ss.replaying_remote_interact || player < 0 || player != net::LocalSlot() || !target) {
    return;
  }
  if (opened_local_menu) {
    // Local-only interactable (e.g. Save Tiki Totem): never broadcast!
    return;
  }
  const uint32_t mat = ReadU32(base, target + kActorMatrix);
  if (!mat || !guest_memory::Readable(mat + 48, 12)) return;
  const uint32_t hash = ReadU32(base, target + kActorNameHash);
  uint8_t pkt[22] = {};
  pkt[0] = kMsgWorldInteract;
  pkt[1] = uint8_t(player);
  PutLE32(pkt + 2, hash);
  PutLE32(pkt + 6, interactor_flag);
  PutWireF32(pkt + 10, ReadF32(base, mat + 48));
  PutWireF32(pkt + 14, ReadF32(base, mat + 52));
  PutWireF32(pkt + 18, ReadF32(base, mat + 56));
  if (net::IsHost()) {
    BroadcastFromHost(pkt, sizeof(pkt), /*reliable=*/true);
  } else {
    SendToHost(pkt, sizeof(pkt), /*reliable=*/true);
  }
  REXLOG_INFO("Online: TX shared world interaction by P{} on {:08X} (hash {:08X})", player + 1,
              target, hash);
}

bool ShouldSuppressMenuOpen() {
  if (!net::Active() || !more_players::InPlay()) return false;
  if (g_ss.replaying_remote_interact) return true;
  if (g_ss.active_interact_player >= 0 && g_ss.active_interact_player != net::LocalSlot()) {
    return true;
  }
  if (g_ss.last_interact_player >= 0 && g_ss.last_interact_player != net::LocalSlot() &&
      Clock::now() - g_ss.last_interact_time < std::chrono::milliseconds(1200)) {
    return true;
  }
  return false;
}

std::string DebugStatus() {
  std::lock_guard lock(g_ss.mutex);
  return fmt::format(
      "  session_sync: epoch={} grace={} host_save={} | local_titan=('{}' / pocket '{}')",
      g_ss.transition_epoch, InTransitionOrCutsceneGrace() ? "yes" : "no",
      g_ss.client_has_host_save ? "ready" : "n/a", g_ss.local_last_ridden, g_ss.local_last_pocket);
}

void MountPlayerOnTitan(PPCContext& ctx, uint8_t* base, int p, uint32_t titan) {
  const uint32_t crash = more_players::CharacterOfPlayer(p);
  if (!crash || !titan || !guest_memory::Readable(titan, 0x40)) return;
  NativeMountPlayerOnTitan(ctx, base, crash, titan);
}

void ForceDismount(int p, uint32_t titan) {
  if (p < 0 || p >= kMaxPlayers || !titan) return;
  g_ss.dismounting_titan = titan;  // lost_controller's CheckInputEvent hook answers its dismount
  g_ss.dismount_until = Clock::now() + std::chrono::milliseconds(600);
  net::action_sync::AllowOwnDecisions(p, 800);  // the copy's titan tree must ask (decides itself)
}

}  // namespace session_sync
}  // namespace net

// =============================================================================
// Engine Hook Overrides (active ONLY when net::Active())
// =============================================================================

// 1. CUberManager::SetPaused (sub_8227C8E8) & SetPausedWithSound (sub_8227C950):
//    Never pause world time, AI, physics, or audio when in an online level!
extern "C" REX_FUNC(sub_8227C8E8) {
  if (net::Active() && more_players::InPlay()) {
    net::session_sync::g_ss.local_menu_paused = (ctx.r4.u32 & 0xFFu) != 0;
    return;
  }
  __imp__sub_8227C8E8(ctx, base);
}

extern "C" REX_FUNC(sub_8227C950) {
  if (net::Active() && more_players::InPlay()) {
    net::session_sync::g_ss.local_menu_paused = (ctx.r4.u32 & 0xFFu) != 0;
    return;
  }
  __imp__sub_8227C950(ctx, base);
}

// 2. CConfirmBox::Update (sub_8225DE68):
//    In a level (game state 5), 0x8225DEC8 auto-closes the Save Totem confirm
//    box unless uber +168 bit 0x20 is set. Present 0x20 ONLY during this UI
//    method so confirm boxes stay open while the world keeps running!
extern "C" REX_FUNC(sub_8225DE68) {
  const uint32_t box = ctx.r3.u32;
  if (net::Active() && more_players::InPlay() && box && guest_memory::Readable(box + 4, 1) &&
      (*GuestPtr(base, box + 4) & 0x40u) != 0) {
    const uint32_t uber = net::session_sync::ReadU32(base, net::session_sync::kUberGlobal);
    if (uber && guest_memory::Readable(uber + net::session_sync::kUberFlags, 1)) {
      uint8_t* flags = GuestPtr(base, uber + net::session_sync::kUberFlags);
      const uint8_t saved = *flags;
      *flags = uint8_t((saved | net::session_sync::kPauseBitGame) &
                       ~net::session_sync::kPauseBitMenu);
      __imp__sub_8225DE68(ctx, base);
      *flags = saved & ~uint8_t(net::session_sync::kPauseBitGame | net::session_sync::kPauseBitMenu);
      return;
    }
  }
  __imp__sub_8225DE68(ctx, base);
}

// 3. Door / Portal / Hub Area Transitions (sub_82293D70, sub_822975A8, sub_8227B640):
//    Sync moving to new areas across all players. An open menu holds exits at the
//    level-change events' "ready?" checks (below), never in these runs: a run that
//    is skipped never comes back (see HoldExitForOpenMenu).
//
//    CActionBeginChangeLevel "ready?" (vtable 0x8203F2E8 slot 1): not yet while a
//    menu is open anywhere.
extern "C" REX_FUNC(sub_82293850) {
  if (net::session_sync::HoldExitForOpenMenu(base)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82293850(ctx, base);
}

//    CActionChangeLevel "ready?" (0x8203F36C slot 1): an exit with no Begin event
//    waits the same way; after a Begin (game +0x110 bit 0x40 set: fade under way,
//    players held) it goes on, so a menu opened in that moment never strands it.
extern "C" REX_FUNC(sub_82293D48) {
  const bool begun = (*GuestPtr(base, net::session_sync::kGameObject + 0x110) & 0x40) != 0;
  if (!begun && net::session_sync::HoldExitForOpenMenu(base)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82293D48(ctx, base);
}

extern "C" REX_FUNC(sub_82293D70) {
  const uint32_t action = ctx.r3.u32;
  if (net::Active() && more_players::InPlay() && !net::session_sync::g_ss.applying_transition &&
      action && guest_memory::Readable(action + 8, 2)) {
    const int8_t level = int8_t(*GuestPtr(base, action + 8));
    const int8_t entry = int8_t(*GuestPtr(base, action + 9));
    net::session_sync::ExtendGrace(4.0);
    if (!net::IsHost() && net::LocalSlot() >= 0) {  // (no host any more: change here)
      // Client walked into door: ask Host to transition everyone together!
      uint8_t pkt[8] = {net::kMsgAreaTransition, uint8_t(level), uint8_t(entry),
                        uint8_t(std::max(0, net::LocalSlot()))};
      net::SendToHost(pkt, sizeof(pkt), /*reliable=*/true);
      net::session_sync::NoteExitRequestSent(level, entry);
      return;
    }
  }
  __imp__sub_82293D70(ctx, base);
}

// CActionRegisterCheckPoint run (vtable 0x8203FC94 slot 2): writes the respawn
// place (game +244 level / +248 entry) and stores the players. It used to send
// that place to the host as an AREA CHANGE from a guest, so every checkpoint a
// guest's level registered (one fires right after most level loads) made the
// host load the level AGAIN at the checkpoint's entry: two loading screens in a
// row ("level 1 entry 1" then "level 1 entry 25", seen 2026-11-27). A checkpoint
// is not a level change: each PC registers its own (the same objective runs on
// every PC); only the cutscene/void grace is kept.
extern "C" REX_FUNC(sub_822975A8) {
  __imp__sub_822975A8(ctx, base);
  if (net::Active() && more_players::InPlay() && !net::session_sync::g_ss.applying_transition) {
    net::session_sync::ExtendGrace(4.0);
  }
}

extern "C" REX_FUNC(sub_8227B640) {
  if (net::Active() && more_players::InPlay() && !net::session_sync::g_ss.applying_transition) {
    const int8_t level = int8_t(ctx.r4.u32);
    const int8_t entry = int8_t(ctx.r5.u32);
    net::session_sync::ExtendGrace(4.0);
    if (net::IsHost()) {
      uint32_t epoch = 0;
      {
        std::lock_guard lock(net::session_sync::g_ss.mutex);
        epoch = ++net::session_sync::g_ss.transition_epoch;
      }
      uint8_t pkt[8] = {net::kMsgAreaTransition, uint8_t(level), uint8_t(entry), 0};
      net::session_sync::PutLE32(pkt + 4, epoch);
      net::BroadcastFromHost(pkt, sizeof(pkt), /*reliable=*/true);
      net::world_sync::Reset();
      net::enemy_input::Reset();
    } else if (net::LocalSlot() >= 0) {  // (no host any more: change here)
      uint8_t pkt[8] = {net::kMsgAreaTransition, uint8_t(level), uint8_t(entry),
                        uint8_t(std::max(0, net::LocalSlot()))};
      net::SendToHost(pkt, sizeof(pkt), /*reliable=*/true);
      net::session_sync::NoteExitRequestSent(level, entry);
      return;  // Wait for Host's kMsgAreaTransition so everyone enters together!
    }
  }
  __imp__sub_8227B640(ctx, base);
}

// 3a. CCameraManager update (vtable 0x820383C4 slot 10, sub_8223A280): MENU MODELS.
//     The chest's outfit preview (also the level-up screen's model and the tiki
//     face's enemy) stands in front of a camera the screen sets up. The original
//     PAUSES the game there (uber +168 bit 0x20), and this update, which runs
//     every camera actor in its list (+0x234..+0x244, sub_820B0A70), does nothing
//     while paused, so the screen's camera stays put. Online the world never
//     pauses (TickFrame clears the bit), so the gameplay camera took over again
//     the next frame: the chest showed no Crash or Coco. Found by showing the
//     pause bit to one reader at a time (CCameraManager's update was the one; the
//     render manager, zone manager and HUD readers were not). While THIS PC's
//     player has such a screen open, the update is skipped the way a pause skips
//     it; the game itself keeps running, and the other PCs are untouched.
extern "C" REX_FUNC(__imp__sub_8223A280);
extern "C" REX_FUNC(sub_8223A280) {
  if (net::Active() && more_players::InPlay()) {
    const int32_t fe_state = fight_tree::CurrentFrontEnd().state;
    if (outfit_picker::IsOpen() || fe_state == 847 /* chest */ ||
        fe_state == 839 /* level up */ || fe_state == 860 /* tiki face */) {
      return;  // (the paused original: no camera update)
    }
  }
  __imp__sub_8223A280(ctx, base);
}

// 3b. CRequirementTriggerVolume "ready?" (vtable 0x82041300 slot 1, sub_822A6188):
//     with --debug_online_trace, each volume's change to "someone is inside" is
//     logged with this PC's player's position at that moment, so a test can find
//     a level exit (or any story trigger) without walking around blind. Answer
//     untouched. (The volume's own place isn't at its +44 matrix: read as 0.)
extern "C" REX_FUNC(__imp__sub_822A6188);
extern "C" REX_FUNC(sub_822A6188) {
  const uint32_t req = ctx.r3.u32;
  __imp__sub_822A6188(ctx, base);
  if (!net::DebugTraceOn() || !req) return;
  static std::unordered_map<uint32_t, bool> inside;  // requirement -> last answer
  const bool now_in = (ctx.r3.u32 & 0xFF) != 0;
  bool& was = inside[req];
  if (now_in && !was) {
    const uint32_t vol = net::session_sync::ReadU32(base, req + 8);
    const int32_t who = int32_t(net::session_sync::ReadU32(base, req + 0x14));
    const int slot = std::max(0, net::LocalSlot());
    const uint32_t body = more_players::CharacterOfPlayer(slot);
    const uint32_t mat = body ? net::session_sync::ReadU32(base, body + 44) : 0;
    float at[3] = {};
    if (mat && guest_memory::Readable(mat + 48, 12)) {
      for (int i = 0; i < 3; ++i) at[i] = net::session_sync::ReadF32(base, mat + 48 + 4 * i);
    }
    REXLOG_INFO("OnlineTrace: trigger volume {:08X} (requirement {:08X}, who {}) entered; P{} "
                "at ({:.2f}, {:.2f}, {:.2f})", vol, req, who, slot + 1, at[0], at[1], at[2]);
  }
  was = now_in;
}

// 4. StorePlayersBeforeLevelChange (sub_8229CB78):
//    Write every connected player's authoritative ridden titan template into
//    their carried-actor slot so all players keep their titans across area loads!
extern "C" REX_FUNC(sub_8229CB78) {
  __imp__sub_8229CB78(ctx, base);
  if (!net::Active()) return;
  const uint32_t sc = net::session_sync::EnsureScratch();
  if (!sc) return;
  std::lock_guard lock(net::session_sync::g_ss.mutex);
  if (!net::session_sync::g_ss.local_last_pocket.empty() &&
      net::session_sync::ReadCString(base, net::session_sync::kGameObject + 44, 64).empty()) {
    net::session_sync::WriteCString64(base, net::session_sync::kGameObject + 44,
                                      net::session_sync::g_ss.local_last_pocket);
  }
  const uint32_t mask = net::ConnectedMask();
  for (int p = 0; p < net::kMaxPlayers; ++p) {
    if (!(mask & (1u << p)) || p == net::LocalSlot()) continue;
    const std::string& tmpl = net::session_sync::g_ss.titans[size_t(p)].ridden_template;
    net::session_sync::WriteCString64(base, sc + 512, tmpl);
    net::session_sync::CallGame(sub_822705D0, ctx, base, net::session_sync::kGameObject,
                                uint32_t(p), sc + 512);
  }
}

// 5. CMasterCinematicAction (sub_820B3430):
//    Extend cutscene grace & notify peers so cutscenes never cause void deaths!
extern "C" REX_FUNC(sub_820B3430) {
  if (net::Active() && more_players::InPlay()) {
    net::session_sync::ExtendGrace(4.0);
    uint8_t pkt[6] = {net::kMsgCutsceneSync,
                      uint8_t(net::session_sync::CurrentLevelId(base))};
    if (net::IsHost()) {
      net::BroadcastFromHost(pkt, sizeof(pkt), /*reliable=*/true);
    } else {
      net::SendToHost(pkt, sizeof(pkt), /*reliable=*/true);
    }
  }
  __imp__sub_820B3430(ctx, base);
}

// 6. Collectable::SetState (sub_821991B0, r3 = collectable, r4 = actor, r5 = new_state):
//    State 0 = collected/hidden! When collected locally, broadcast kPickupCollectableTaken
//    so the orb/voodoo doll/power-up disappears on remote screens; and if already
//    collected by a peer at this spot, hide duplicate orbs smoothly.
extern "C" REX_FUNC(sub_821991B0) {
  const uint32_t collectable = ctx.r3.u32;
  const uint32_t actor = ctx.r4.u32;
  const uint32_t new_state = ctx.r5.u32;
  const uint32_t old_state = collectable ? net::session_sync::ReadU32(base, collectable + 200) : 0;

  __imp__sub_821991B0(ctx, base);

  if (!net::Active() || !more_players::InPlay() || more_players::Loading() || !actor) return;

  // Transition from flying-to-player (4) or active (1/2) -> collected/disabled (0):
  if (new_state == 0 && old_state != 0) {
    const uint32_t mat = net::session_sync::ReadU32(base, actor + net::session_sync::kActorMatrix);
    if (!mat || !guest_memory::Readable(mat + 48, 12)) return;
    const uint32_t hash = net::session_sync::ReadU32(base, actor + net::session_sync::kActorNameHash);
    const float px = net::session_sync::ReadF32(base, mat + 48);
    const float py = net::session_sync::ReadF32(base, mat + 52);
    const float pz = net::session_sync::ReadF32(base, mat + 56);

    // Check if this was suppressed by a remote peer's pickup event (avoid echo).
    {
      std::lock_guard lock(net::session_sync::g_ss.mutex);
      for (const auto& h : net::session_sync::g_ss.hidden_collectables) {
        const float dx = h.pos[0] - px, dy = h.pos[1] - py, dz = h.pos[2] - pz;
        if (h.hash == hash && (dx * dx + dy * dy + dz * dz) < 9.0f) return;
      }
    }

    net::session_sync::PendingPickup ev{};
    ev.slot = uint8_t(std::max(0, net::LocalSlot()));
    ev.kind = net::session_sync::kPickupCollectableTaken;
    ev.hash = hash;
    ev.pos[0] = px;
    ev.pos[1] = py;
    ev.pos[2] = pz;
    net::session_sync::SendPickupToHostOrBroadcast(ev);
  } else if (new_state != 0) {
    // Active collectable: if a remote player already picked up this exact item, hide it!
    const uint32_t mat = net::session_sync::ReadU32(base, actor + net::session_sync::kActorMatrix);
    if (!mat || !guest_memory::Readable(mat + 48, 12)) return;
    const uint32_t hash = net::session_sync::ReadU32(base, actor + net::session_sync::kActorNameHash);
    const float px = net::session_sync::ReadF32(base, mat + 48);
    const float py = net::session_sync::ReadF32(base, mat + 52);
    const float pz = net::session_sync::ReadF32(base, mat + 56);
    bool hide = false;
    {
      std::lock_guard lock(net::session_sync::g_ss.mutex);
      for (const auto& h : net::session_sync::g_ss.hidden_collectables) {
        const float dx = h.pos[0] - px, dy = h.pos[1] - py, dz = h.pos[2] - pz;
        if (h.hash == hash && (dx * dx + dy * dy + dz * dz) < 4.0f) {
          hide = true;
          break;
        }
      }
    }
    if (hide) {
      net::session_sync::CallGame(__imp__sub_820B0C58, ctx, base, actor, 0);
      net::session_sync::WriteU32(base, collectable + 200, 0);
    }
  }
}

namespace {
// Which player slot (0..3) `controller` belongs to: walk up to its root
// CInputController (parent at +0x24), then find that root in the input
// manager's per-socket list `*(uber + 56) + 8`. -1 = none of the four.
int ControllerSlot(uint8_t* base, uint32_t controller) {
  if (!base || !controller) return -1;
  uint32_t root = controller;
  for (int guard = 0; guard < 16; ++guard) {
    if (!guest_memory::Readable(root + 0x24, 4)) return -1;
    const uint32_t parent = net::session_sync::ReadU32(base, root + 0x24);
    if (!parent) break;
    root = parent;
  }
  const uint32_t uber = net::session_sync::ReadU32(base, net::session_sync::kUberGlobal);
  const uint32_t input_mgr = uber ? net::session_sync::ReadU32(base, uber + 56) : 0;
  const uint32_t list = input_mgr ? net::session_sync::ReadU32(base, input_mgr + 8) : 0;
  if (list && guest_memory::Readable(list, 16)) {
    for (int slot = 0; slot < 4; ++slot) {
      if (net::session_sync::ReadU32(base, list + 4 * uint32_t(slot)) == root) return slot;
    }
  }
  // Not a socket's controller: a body's own one. Crash's behaviour controller
  // is actor +0x40 (findings/40), a ridden titan's too; match the root of
  // every player's Crash and titan. (2026-11-27: on a guest PC the host
  // player's puppet riding a titan asked the pocket event through such a
  // controller, the socket match failed, and raw RB presses pocketed.)
  for (int p = 0; p < 4; ++p) {
    for (const uint32_t body : {more_players::TitanOfPlayer(p), more_players::CharacterOfPlayer(p)}) {
      if (!body || !guest_memory::Readable(body + 0x40, 4)) continue;
      uint32_t r = net::session_sync::ReadU32(base, body + 0x40);
      for (int guard = 0; r && guard < 16; ++guard) {
        if (!guest_memory::Readable(r + 0x24, 4)) break;
        const uint32_t parent = net::session_sync::ReadU32(base, r + 0x24);
        if (!parent) break;
        r = parent;
      }
      if (r && r == root) return p;
    }
  }
  if (net::DebugTraceOn()) {
    static uint32_t logged_root = 0;
    if (logged_root != root) {
      logged_root = root;
      REXLOG_INFO("OnlineTrace: controller {:08X} (root {:08X}) belongs to no player", controller, root);
    }
  }
  return -1;
}

// True if `controller` drives THIS PC's local player slot.
bool IsLocalOnlineController(uint8_t* base, uint32_t controller) {
  const int slot = net::LocalSlot();
  return slot >= 0 && slot < 4 && ControllerSlot(base, controller) == slot;
}
}  // namespace

// 7. CInputController::CheckInputEvent (sub_82272338, findings/62 s.2):
//    While an unpaused menu, Save Totem prompt, Map, Upgrade screen, or Outfit
//    Picker is open on this PC (plus a 220 ms button-drain window on close),
//    keep MENU (type 1) and MOVIE (type 2) events working 100% normally while
//    blocking gameplay input maps (GENERAL=4, PLAYER=5, ENEMY/Titan=6,
//    PROJECTILE=7, CAMERA=0) on our local player's controller so menu navigation
//    never makes Crash or a Titan jump, attack, jack, or shoot in the background!
extern "C" REX_FUNC(sub_82272338) {
  if (net::Active() && more_players::InPlay()) {
    const int32_t map_type = int32_t(ctx.r4.u32);
    const int32_t event_id = int32_t(ctx.r5.u32);
    // ENEMY PUPPETS (net/enemy_input.h): a guest's copy of an enemy asks
    // whether a move was pressed. The host's AI presses are replayed for one
    // frame; the copy may ask a little later: a waiting replayed press of
    // that move answers yes (once).
    if (!net::IsHost() && net::world_sync::EnemyPuppets() &&
        net::world_sync::IsPuppetEnemyInput(ctx.r3.u32)) {
      const uint32_t asked_input = ctx.r3.u32;
      __imp__sub_82272338(ctx, base);
      const bool heard = (ctx.r3.u32 & 0xFF) != 0;
      if (net::enemy_input::TakeWaitingPress(asked_input, uint32_t(map_type), uint32_t(event_id), heard) &&
          !heard) {
        ctx.r3.u64 = 1;
        if (net::DebugTraceOn()) {
          REXLOG_INFO("OnlineTrace: puppet input {:08X} heard {}/{} late (replayed press kept)", asked_input,
                      map_type, event_id);
        }
      }
      return;
    }
    if (map_type != 1 && map_type != 2 &&
        net::session_sync::ShouldBlockLocalGameplayInput(base) &&
        IsLocalOnlineController(base, ctx.r3.u32)) {
      ctx.r3.u64 = 0;
      return;
    }
    // TITAN POCKET / SWAP = OWNER DECIDES. Two RB events (console
    // `inputmaps`, findings/62):
    //   * ENEMY map 6, POCKET event 37: riding -> pocket the titan / swap;
    //   * PLAYER map 5, event 14 "CHEER" (down R2 = RB): on foot -> take the
    //     pocketed titan back out (2026-11-27 trace: unpocketing never asked
    //     event 37), else Crash's cheer.
    // Local player: one shared 1.25 s cooldown, so rapid RB mashing gives
    // peers time to finish a mount/pocket transition without orphaning or
    // killing actors; each press the owner's game ACCEPTS = one reliable
    // kMsgPocket(event). Remote player (a puppet here): its controller gets the
    // owner's raw pad state, mashing included, and used to swap on EVERY press:
    // the cooldown only existed on the owner's PC (2026-11-27 playtest: "rapidly
    // swapping on other screens while spam clicking"). Now the puppet's raw RB
    // is never heard for these two: the event answers "yes" once per kMsgPocket.
    // A "yes" stays yes for kPocketSameFrame, so a fight tree that asks the same
    // event twice in one frame hears the same answer both times.
    // (test trace, 2026-11-27: which events does our player's game ACCEPT
    // while riding / jacking; each distinct pair once a second at most)
    if (net::DebugTraceOn() && map_type >= 4 && IsLocalOnlineController(base, ctx.r3.u32)) {
      const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32;
      __imp__sub_82272338(ctx, base);
      const bool yes = (ctx.r3.u32 & 0xFF) != 0;
      ctx.r3.u64 = r3; ctx.r4.u64 = r4; ctx.r5.u64 = r5;
      static std::map<int, net::session_sync::Clock::time_point> last;
      const auto t = net::session_sync::Clock::now();
      if (yes && t - last[map_type * 100 + event_id] > std::chrono::seconds(1)) {
        last[map_type * 100 + event_id] = t;
        REXLOG_INFO("OnlineTrace: {}: our game heard map {} event {}",
                    more_players::TitanOfPlayer(net::LocalSlot()) ? "riding" : "on foot", map_type, event_id);
      }
    }
    // B WHILE RIDING (2026-11-27 playtest, titan swap): the same B gets off the
    // titan, or jacks a stunned titan in reach (the game then POCKETS the one
    // ridden; with the pocket full that one dies). A copy decides nothing, so
    // it is replayed like RB: ENEMY 6 / 23 and GENERAL 4 / 2 (both asked for one
    // press), only while that player rides a titan.
    const int asker = ControllerSlot(base, ctx.r3.u32);
    const bool riding_b = asker >= 0 && asker < 4 && more_players::TitanOfPlayer(asker) &&
                          ((map_type == 6 && event_id == 23) || (map_type == 4 && event_id == 2));
    // OUR B on foot at a jack target that is a host-driven enemy copy here: the
    // copy decides for itself for a moment, so the game's jack goes through
    // (being jacked is the titan's own decision; net/action_sync.h).
    // (on foot the jack B is PLAYER map 5 / event 23: traced 2026-11-27)
    if (!net::IsHost() && map_type == 5 && event_id == 23 && asker >= 0 && asker == net::LocalSlot() &&
        !more_players::TitanOfPlayer(asker)) {
      const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32;
      __imp__sub_82272338(ctx, base);
      const bool pressed = (ctx.r3.u32 & 0xFF) != 0;
      if (pressed) {
        if (const uint32_t target = prompt_icons::JackTarget(base, asker)) {
          net::action_sync::FreeEnemyCopy(target, 3000);
          if (net::DebugTraceOn()) REXLOG_INFO("OnlineTrace: our B jacks {:08X}: its copy decides itself", target);
        }
      }
      return;  // (answered by the original above)
    }
    const bool pocket_event =
        (map_type == 6 && event_id == 37) || (map_type == 5 && event_id == 14) || riding_b;
    if (pocket_event) {
      const int slot = asker;
      const auto now = net::session_sync::Clock::now();
      auto& ss = net::session_sync::g_ss;
      const size_t e = size_t(std::max(0, net::PocketKind(uint8_t(event_id))));
      if (slot >= 0 && slot == net::LocalSlot()) {
        if (now < ss.pocket_yes_until[e]) {
          ctx.r3.u64 = 1;
          return;
        }
        // (the RB anti-mash cooldown is RB's only: the riding B's two events
        // come in the same frame and must both be heard)
        if (!riding_b && now < ss.next_pocket_allowed) {
          ctx.r3.u64 = 0;
          return;
        }
        __imp__sub_82272338(ctx, base);
        if ((ctx.r3.u32 & 0xFFu) != 0) {
          if (!riding_b) ss.next_pocket_allowed = now + std::chrono::milliseconds(1250);
          ss.pocket_yes_until[e] = now + net::session_sync::kPocketSameFrame;
          net::NoteLocalPocket(slot, uint8_t(event_id));
        }
        return;
      }
      if (slot >= 0 && slot < 4) {
        if (now < ss.remote_pocket_yes_until[size_t(slot)][e]) {
          ctx.r3.u64 = 1;
          return;
        }
        const bool yes = net::TakeRemotePocket(slot, uint8_t(event_id));
        if (yes) ss.remote_pocket_yes_until[size_t(slot)][e] = now + net::session_sync::kPocketSameFrame;
        ctx.r3.u64 = yes ? 1 : 0;
        return;
      }
    }
  }
  __imp__sub_82272338(ctx, base);
}

// 8. CInputController::GetChannelNewestValue (sub_82271D08, findings/62 s.2):
//    Stick movement & 360-spin detectors read channels 0..3 through sub_82271D08.
//    While a local menu is open on this PC, let FrontEnd menu stick navigation
//    (sub_82265798 at 0x82265798..0x82265960 and CMenuScreenAction sub_820D0B88
//    at 0x820D0B88..0x820D1200) read the sticks normally, while returning 0.0f
//    to all gameplay locomotion, aiming, and spin callers for our local player!
extern "C" REX_FUNC(sub_82271D08) {
  if (net::Active() && more_players::InPlay() && net::session_sync::IsLocalMenuOpen(base)) {
    const uint32_t lr = uint32_t(ctx.lr);
    const bool front_end_caller =
        (lr >= 0x82265798u && lr < 0x82265960u) || (lr >= 0x820D0B88u && lr < 0x820D1200u);
    if (!front_end_caller && IsLocalOnlineController(base, ctx.r3.u32)) {
      ctx.f1.f64 = 0.0;
      return;
    }
  }
  __imp__sub_82271D08(ctx, base);
}

// 9. Local-Only Interactable Menu Gates (sub_8225F718 Save Totem & sub_82295E20
//    CActionOpenMenu for Outfit Chest / TV / Comic):
//    Never pop open a local UI menu when a REMOTE player interacts with a totem
//    or chest; complete the script action cleanly on remote PCs while letting
//    shared mission NPCs (e.g. Coco) run for everyone!
extern "C" REX_FUNC(sub_8225F718) {
  if (net::Active() && more_players::InPlay() && (ctx.r4.u32 & 0xFFu) != 0) {
    if (net::session_sync::ShouldSuppressMenuOpen()) {
      return;
    }
    net::session_sync::g_ss.local_interact_opened_menu = true;
  }
  __imp__sub_8225F718(ctx, base);
}

extern "C" REX_FUNC(sub_82295E20) {
  const uint32_t action = ctx.r3.u32;
  if (net::Active() && more_players::InPlay() && action && guest_memory::Readable(action + 12, 5)) {
    int target_player = int32_t(net::session_sync::ReadU32(base, action + 12));
    if (net::session_sync::g_ss.last_interact_player >= 0 &&
        net::session_sync::Clock::now() - net::session_sync::g_ss.last_interact_time <
            std::chrono::milliseconds(1200) &&
        (net::session_sync::g_ss.last_interact_player & 1) == target_player) {
      target_player = net::session_sync::g_ss.last_interact_player;
    }
    if (net::session_sync::g_ss.replaying_remote_interact || target_player != net::LocalSlot()) {
      ctx.r3.u64 = 1;  // Complete CActionOpenMenu immediately without opening UI on this PC!
      return;
    }
    net::session_sync::g_ss.local_interact_opened_menu = true;
  }
  __imp__sub_82295E20(ctx, base);
}

// 10. Authoritative Floating / Stuck False-Death Gate (sub_8217D5A0):
//     Radical's CPhysicsBehaviour checks sub_8217D5A0 ("is floating/stuck?")
//     and kills/respawns any actor airborne for 5 seconds. Under network jitter
//     or cross-zone lag, a remote player who is completely fine on their own PC
//     must NEVER be killed locally by another client's floating check!
extern "C" REX_FUNC(sub_8217D5A0) {
  if (net::Active() && more_players::InPlay() && ctx.r3.u32) {
    const uint32_t actor = net::session_sync::ReadU32(base, ctx.r3.u32 + 88);
    if (net::session_sync::IsRemotePlayerActor(actor) ||
        net::session_sync::InTransitionOrCutsceneGrace()) {
      ctx.r3.u64 = 0;
      return;
    }
  }
  __imp__sub_8217D5A0(ctx, base);
}
