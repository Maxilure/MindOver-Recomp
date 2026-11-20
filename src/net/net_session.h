// =============================================================================
// net/net_session.h -- LAN CO-OP, FIRST STEP (online plan milestone M1): two
// copies of the game, one player each, kept in LOCKSTEP over UDP
// =============================================================================
//
// THE IDEA IN PLAIN WORDS. Each PC runs the WHOLE game. Only the players'
// controller states travel over the network. Every game frame, each PC
// waits until it has every player's controller state for that frame, then
// runs the frame with them. The game is deterministic once the fixed step is
// on (milestone M0, notes in fixed_step.h / system/helper_threads.h), so the
// same inputs on the same frames give the same world on both PCs, frame for
// frame: no positions, enemies or objects are ever sent.
//
//   PC 1 (host)  = player 1          PC 2 (guest) = player 2
//   reads its keyboard / pad         reads its keyboard / pad
//   for frame N + delay              for frame N + delay
//            \______ UDP: "my input for frames A..B" ______/
//   frame N runs when BOTH inputs for N are in (the FRAME GATE)
//
// INPUT DELAY: a press read on frame N is played on frame N + delay (3 =
// 50 ms at 60 fps by default). The other PC's packet has that long to
// arrive before anybody has to wait. On one PC (loopback) or a LAN that is
// plenty; the internet (milestone M6) will pick it from the ping.
//
// WHERE IT HOOKS IN (all on the game's main thread):
//   * fixed_step.cpp CrashMomPassDelta, every main-loop pass:
//       OnPassStart(k)  -> the local input for frame s + delay goes out; the
//                          world fingerprint of the frame that just ended is
//                          noted (s = session frame, below)
//       BetweenFrames(k) -> THE FRAME GATE: waits for the other PC's input
//                          for frame s (the helper threads' gate is open
//                          meanwhile: a wait here is "between frames", it
//                          changes nothing in the world)
//   * saves/save_library.cpp, the game's XInputGetState wrapper:
//       OverridePad()   -> notes this PC's own controller (socket 0, after
//                          a replay's override: tests drive it by replay),
//                          then answers sockets 0-1 with the session's
//                          inputs for this frame, sockets 2-3 "not connected"
//   * fixed_step::StartSession (the save is loaded, the level starts: quick
//     load, or the Load Game menu's "Load successful" exit while a session
//     is wanted) -> OnSessionStart: session frame 0 = the NEXT pass.
//
// THE SESSION'S FIRST FRAME. Each PC reaches the level with its own history
// (boot, menus, how long its loads took). fixed_step::StartSession wipes that
// history out of the world (clock on a whole hour, every known random
// generator from the session seed). The two PCs reach it at different real
// times: the first one plays `delay` frames (everyone's inputs for frames
// 0..delay-1 are "nothing pressed") and then waits at the gate for the other.
//
// WHAT ELSE A SESSION CHANGES (must be alike on both PCs, else they part):
//   * who has a controller (input/players.cpp PresentMask): players 1 and 2,
//     from the moment a session is set up, whatever devices this PC has;
//   * the keyboard goes through the controller state (keyboard_events off:
//     its native events would reach only THIS PC's game);
//   * the mouse aiming (mouse_reticle) is off: it writes the aim point into
//     the world directly, not through the controller state;
//   * player 1's "controller unplugged" watch is off (it reads this PC's
//     devices).
//   * the fixed step: 60, paced (turned on at launch by --net_host/--net_join,
//     or by the Network window's Host / Join).
//
// IS IT STILL THE SAME WORLD? Each PC computes a FINGERPRINT at every frame's
// end (world_hash::QuickFingerprint: the main random generator + every
// player's body) and sends its recent ones along with its inputs. The first
// frame where they differ = "DRIFTED at frame N" (log ERROR + the Network
// window). "Dump both sides" asks both PCs to write the whole world data at
// the same session frame (world_hash dumps; tools/world_hash_compare.py).
//
// SECURITY (online plan s.7.3, the LAN part): packets carry a magic word, a
// protocol number and the session's token (random, made by the host); sizes
// and ranges are checked before anything is read; after the handshake only
// the one peer address is listened to; nothing a packet says is ever used as
// a memory address or a file name. Only controller states, frame numbers and
// fingerprints are accepted.
//
// FLAGS:
//   --net_host               be the host (player 1); waits for one guest
//   --net_join=<address>     join a host (player 2), e.g. 127.0.0.1
//   --net_port=37373         UDP port (the host listens on it)
//   --net_delay=3            input delay in frames (the host's counts)
//   --net_drop_after_s=30    a player whose PC went quiet stays this long
// Plus the F1 Tools menu's "Network" window (net/network_window.h).
//
// NOT YET (later milestones): 3-4 players, save transfer (both PCs must load
// the SAME save themselves: a different save shows as a drift at frame 0, the
// players stand elsewhere), pause /
// menus rules, late join, the internet, the game-look "Play Together" menu.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, net_host);
REXCVAR_DECLARE(std::string, net_join);
REXCVAR_DECLARE(int32_t, net_port);
REXCVAR_DECLARE(int32_t, net_delay);

namespace net {

enum class Role { kNone, kHost, kGuest };

// Reads the flags; with --net_host / --net_join: turns the fixed step on (60)
// and starts the network (CrashMomApp OnPreSetup, before the game runs).
void Init();

// The Network window's buttons (UI thread). Host/Join return "" or why not.
std::string Host(int port);
std::string Join(const std::string& address, int port);
void Leave();

// A session is set up (host or guest, connected or not yet): the rules in
// the header apply (players 1-2 have a controller, keyboard as controller
// state, no mouse aiming). Any thread.
bool Configured();
// Player bit N = player N plays in the session (input/players.cpp). 0 = no
// session: this PC's own devices decide. Any thread.
uint32_t PlayerMask();
// The lockstep is running (session started, peer there). Any thread.
bool InSession();
// This PC's player (0-based) in the session, -1 = none. Any thread.
int LocalPlayer();
// Player `player`'s PC is gone (the session plays on alone; their character
// stays a while, then drops out): they can't press anything any more.
bool PlayerAway(int player);

// --- hooks (the game's main thread) ------------------------------------------
// fixed_step::StartSession, after the clock / generators were reset.
void OnSessionStart(uint64_t fixed_step_frame);
// The session seed StartSession should use (the host's; Configured only).
bool SessionSeed(uint32_t& seed);
// fixed_step's pass hook: before the helper threads' gate opens / after.
void OnPassStart(uint64_t fixed_step_frame);
// Returns how long it waited (ns): the paced clock moves its schedule by that
// much, so the PC that was ahead takes the other's rhythm instead of waiting
// a little on every frame from then on.
int64_t BetweenFrames(uint64_t fixed_step_frame);
// The XInputGetState wrapper (save_library.cpp), right after a replay's
// override: `gamepad` = the 12-byte big-endian XINPUT_GAMEPAD, `result` 0 =
// connected. Returns true when the session answered (the state was replaced).
bool OverridePad(uint32_t socket, uint8_t* gamepad, uint32_t& result);
// The session's controller state of player `player` (0-3) for the frame being
// played (the same on every PC): 12-byte big-endian XINPUT_GAMEPAD. False out
// of a session / no such player. Game main thread.
bool FrameInput(int player, uint8_t gamepad[12]);
// THE STICK'S FRAME (net/online_sticks.cpp): every input carries its PC's
// camera facing on the ground (65536 steps a turn). The facing player
// `player`'s PC sent for the frame being played (false = none known: the
// local camera then). Game main thread.
bool FrameFacing(int player, uint16_t& facing);
// The whole camera pose player `player`'s PC sent for this frame: the game
// camera's 10 words (position, direction, up = +216..+251, field of view +16)
// as the game stores them. False = none. Game main thread.
bool FrameCamera(int player, uint32_t words[10]);
// The answer player `player`'s PC's camera gave the hold logic's question
// (online_sticks.cpp OnlineHoldQuery) when that input was made. False = none.
bool FrameCameraCut(int player, uint8_t& cut);
// The "settled" bit of player `player`'s PC's camera when that input was made
// (online_sticks.cpp OnlineUnlockSettled). False = none.
bool FrameCameraSettled(int player, uint8_t& settled);
// Every PC of the session said, with its input for this frame, "a cutscene
// step has a state change held back here" / "the movie is done here"
// (online_sticks.cpp CUTSCENES AND MOVIES MOVE ON TOGETHER). True outside a
// session.
bool FrameCutsceneReadyEverywhere();
bool FrameMovieDoneEverywhere();

// A PLAYER DISCONNECTED (2026-11-17 rules): the other PC stopped answering
// (2 s of silence) or left. Nothing pauses: this PC plays on alone, the other
// player's input is "nothing pressed", their character stays until
// --net_drop_after_s (silence; at once after a goodbye) and then drops out by
// itself. Only the player who lost the session (a guest whose host is gone, or
// a PC that pressed Leave) sees a message (the game's message bar card).
// lost_controller.cpp, every frame in a level: the player to drop out now
// (-1 none), and "done".
int DropOutWanted();
void DroppedOut(int player);

// --- the Network window ------------------------------------------------------
struct Status {
  Role role = Role::kNone;
  bool connected = false;      // handshake done, peer heard from lately
  bool in_session = false;     // lockstep running
  std::string peer;            // "address:port" or ""
  std::string state;           // one line: what it's doing
  double ping_ms = -1;
  int delay = 0;
  int local_player = -1;       // 0-based
  int64_t session_frame = -1;  // frame being played (-1 before the session)
  int64_t remote_inputs = 0;   // frames of the peer's inputs received
  int64_t waits = 0;           // frames that had to wait at the gate
  double wait_ms_total = 0;
  double last_wait_ms = 0;
  bool waiting_now = false;
  uint64_t my_settings = 0, peer_settings = 0;  // session_settings fingerprints
  bool peer_settings_known = false;
  int64_t checked_through = -1;  // fingerprints compared and equal up to here
  int64_t drift_frame = -1;      // first frame they differed (-1 = never)
  std::string last_event;        // newest notable thing (left, refused, ...)
  bool alone = false;            // the other PC is gone: playing on alone
  bool dropped = false;          // ... and their player dropped out
};
Status GetStatus();
// "Dump both sides": both PCs dump the world at the same session frame (a
// second from now). Returns the reply text.
std::string RequestDump();

}  // namespace net
