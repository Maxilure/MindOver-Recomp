// =============================================================================
// net/state_sync.h -- host-authoritative state synchronization (Phase 1)
// =============================================================================
//
// WHAT: online co-op for up to 4 players built on state synchronization rather
// than deterministic lockstep.
//
// AUTHORITY MODEL:
//   * Players (owner-authoritative): each PC simulates its own local player
//     character natively (0 ms local input delay, independent FPS, own camera)
//     and sends compact snapshots (~30 Hz, unreliable UDP): position, velocity,
//     facing yaw, co-op state, and current gamepad state (which drives the
//     remote puppet's Radical Engine fight tree for animations/VFX/audio).
//   * Remote players on each PC act as hybrid puppets: their fight tree runs
//     from the owner's replicated controls, while CPhysicsBehaviour::Update
//     (sub_8217D0A0) smoothly blends their position, velocity, and facing to
//     the owner's interpolated snapshots so they never drift or desync.
//   * Host (Player 1, slot 0) assigns slots 1..3, relays player snapshots to
//     other clients, and serves as the referee for shared world state (Phase 2).
//
// FLAGS (off by default -- original game untouched):
//   --online_host          host a LAN session on --online_port as Player 1
//   --online_join=<ip>     join a host at <ip>:--online_port as Player 2..4
//   --online_port=37373    UDP port (default 37373)
//   --online_rate=30       player snapshot rate in Hz (10..60, default 30)
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <rex/cvar.h>

struct PPCContext;

REXCVAR_DECLARE(bool, online_host);
REXCVAR_DECLARE(std::string, online_join);
REXCVAR_DECLARE(int32_t, online_port);
REXCVAR_DECLARE(int32_t, online_rate);

namespace net {

constexpr int kMaxPlayers = 4;

// Called once at startup (CrashMomApp::OnPreLaunchModule). Starts hosting or
// connecting if --online_host or --online_join was passed on the command line.
void Install();

// Clean shutdown (sends disconnect notice and closes the socket).
void Shutdown();

// Start / stop sessions at run time (from debug console or F1 Tools menu).
bool StartHost(uint16_t port);
bool StartJoin(std::string_view address, uint16_t port);
void Leave();

// True while an online session is open (hosting or joining/connected).
bool Active();
bool IsHost();

// Which player slot (0..3) this PC controls locally. 0 on host, 1..3 on client
// once welcomed by the host (-1 while still waiting for Welcome).
int LocalSlot();

// Bitmask of currently connected player slots (bit p = slot p connected).
uint32_t ConnectedMask();

// True if `user` (0..3) should appear as a connected virtual controller to the
// game's XInputGetCapabilities (sub_824742E8) and PlayerAssignment::PresentMask.
bool HasVirtualController(uint32_t user);

// Called from the game's XInputGetState wrapper (sub_824742F0) for each socket
// 0..3. When `user == 0`, also drives the once-per-frame network poll and
// outgoing player snapshot broadcast.
//   gamepad_be12: 12-byte big-endian XINPUT_GAMEPAD inside guest memory.
//   xinput_result: in/out X_ERROR_* return code (0 = connected, 1167 = absent).
void FilterControllerState(uint32_t user, uint8_t* gamepad_be12, uint32_t& xinput_result,
                           uint8_t* base);

// Called right BEFORE __imp__sub_8217D0A0 (CPhysicsBehaviour::Update) in
// ground_physics.cpp. Caches `actor -> physics_behaviour` (since Titans do not
// store CPhysicsBehaviour at actor +308 like Crash does, findings/55) and moves
// a remote player's physics position/velocity toward the owner's interpolated
// snapshot so Radical's own ground sweep establishes real ground contact.
void NotePhysicsBehaviour(uint8_t* base, uint32_t physics_behaviour);
uint32_t LookupCachedPhysics(uint8_t* base, uint32_t actor);
void PrepareRemotePhysics(uint8_t* base, uint32_t physics_behaviour, float dt);

// Online menu immunity & transition gating: true when `player` (local or remote)
// currently has an in-game menu (Pause, Save Totem/List, Outfit Chest, Upgrade)
// open, or when ANY connected player in the session is in a menu.
bool IsPlayerInMenu(int player, uint8_t* base);
bool AnyPlayerInMenu(uint8_t* base);

// Called right AFTER __imp__sub_8217D0A0 (CPhysicsBehaviour::Update) in
// ground_physics.cpp. Clears the "floating, stuck" safety timer (+0xDC,
// findings/55) on remote puppets so they never locally auto-respawn, and
// applies the owner's interpolated facing yaw to the world matrix.
void FinishRemotePhysics(uint8_t* base, uint32_t physics_behaviour);

// Authoritative ground-contact sync (ground_physics.cpp: sub_8217D0A0 and
// sub_82182B18 IsOnGround). The owner records whether their active body (Crash
// or ridden titan) has ground contact; remote PCs return that authoritative
// answer to the fight tree so slopes never trigger false falling animations and
// jumps/jump-attacks enter airborne state immediately.
void NoteLocalGroundState(uint8_t* base, uint32_t physics_behaviour, bool on_ground);
int RemoteOnGroundOverride(uint8_t* base, uint32_t physics_behaviour);

// Spin replication (gameplay/game_mods.cpp): Spin is a stick-circle gesture or
// C++ simple-spin hook (HasControlDone360), not a plain XInput button. The owner
// notifies NoteLocalSpin() whenever their Crash starts a spin, and remote PCs
// consume TakeRemoteSpin() / RemoteSpinHolding() so spinning + dizziness sync.
void NoteLocalSpin(int player);
void SetLocalSpinHolding(int player, bool holding);
bool TakeRemoteSpin(int player);
bool RemoteSpinHolding(int player);

// Titan pocket/swap replication (session_sync.cpp, CheckInputEvent: map 6 event
// 37 riding, map 5 event 14 on foot): the owner's game decides (with its 1.25 s cooldown) and NoteLocalPocket
// sends each ACCEPTED swap; a remote puppet's pocket event answers yes only
// through TakeRemotePocket (one per accepted swap), never from its raw RB.
// `event` = 37 (riding: pocket/swap) or 14 (on foot: take it back out).
void NoteLocalPocket(int player, uint8_t event);

bool TakeRemotePocket(int player, uint8_t event);

// Once per game frame (session_sync::TickFrame): every remote player's body
// here is set to the health their own PC reported (kFlagHasHealth).
void ApplyRemoteHealth(PPCContext& ctx, uint8_t* base);

// ONLINE HITS (gameplay/game_mods.cpp sub_82132FF8, cheats.cpp sub_821349E0).
// One hit message being handled on the game thread: who is involved.
//   remote_hitter_on_enemy: host, another PC's player hit an enemy here;
//   remote_victim: another PC's player (0-3) was hit here, else -1;
//   forward: host, that hit came from an enemy -> its damage goes to the
//   victim's PC (kMsgPlayerHit) when the scope ends.
// --debug_online_trace (state_sync.cpp).
bool DebugTraceOn();

class OnlineHitScope {
 public:
  // `by` = the hitting player (0-3), -1 = an enemy / the world.
  OnlineHitScope(bool remote_hitter_on_enemy, int remote_victim, bool forward, int by);
  ~OnlineHitScope();
  OnlineHitScope(const OnlineHitScope&) = delete;
  OnlineHitScope& operator=(const OnlineHitScope&) = delete;
};
bool HitByRemotePlayerOnEnemy();          // inside a scope of that kind
bool HitByNonLocalOnThisPc();             // inside a scope whose hitter isn't our player
void NoteRemotePlayerDamage(float delta);  // a remote victim's damage (before the "for show" clamp)

// Mask aiming reticle sync (input/mouse_reticle.cpp): the owner records their
// 2D canvas reticle spot (reticle +0x14 / +0x28), and remote PCs apply it to
// that player's CReticleController so their mask looks and shoots where they
// aim while players/reticle_idle.cpp hides remote crosshairs from view.
void SetLocalReticle(int player, float x, float y);
bool GetRemoteReticle(int player, float& out_x, float& out_y);

// Mask entry gate (players/more_players.cpp: sub_82235CB8 and sub_82235978).
// Returns true when `player` is a remote online player whose own PC has NOT
// entered mask state (authoritative coop_state is neither 1=mask nor 3=entering
// mask). Prevents a remote player pressing B near a totem/NPC or off-screen
// from accidentally turning into a mask on another PC while leaving titan
// interactions and mounting completely untouched.
bool BlockRemoteMaskEntry(int player);

// One-line / multi-line status string for the debug console (`online` command)
// and ImGui diagnostics.
std::string DebugStatus(uint8_t* base);

}  // namespace net
