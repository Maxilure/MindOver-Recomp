// =============================================================================
// players/coop_camera.cpp -- the co-op camera: frames EVERY player in game
// =============================================================================
// THE ORIGINAL CO-OP CAMERA (traced 2026-10-06, Wumpa Island, two players).
// Gameplay views come from a CCameraVolumeBehaviour (vtable 0x8202998C): the
// level's camera volumes (levels/<L>/cameradata.p3d, 424-byte records in the
// camera manager *(*(0x8259B190)+44)+8) each give an angle, a field of view and
// a zoom range (cameraoverrides.lua: MinZoomValue / MaxZoomValue, e.g. 12 / 20
// on Wumpa Island). Players are "camera interests" (CCameraInterestBehaviour
// on each Crash / titan): both players are PRIMARY interests in the volume
// behaviour's list (+196, 36-byte records), and ONE of them is the FOCUS
// (+492 list, the game object's +40 = "EPlayer_CAMERA_FOCUSING_ON").
//   - target (+228)  = the FOCUS player's position (sub_82107290), pulled a
//     little toward secondary interests (enemies, bosses) by weight;
//   - volume (+264)  = the one containing that target (sub_82107628);
//   - spread (+488)  = how far the other primaries are from the focus;
//   - zoom (+296)    = the camera's DISTANCE from the target: min zoom, or with
//     2+ primaries min + (max - min) x f(spread) (sub_821076B0), smoothed.
// So the camera stays glued to one player and only backs off from 12 to 20
// units: the other player walks off screen (measured: 57 units apart, player 2
// far outside the picture). The +492 "players" list has room for 2, so players
// 3-4 never counted at all (more_players.cpp now refills it when a player
// leaves it: findings/26 s.43).
//
// OUR CAMERA (the camera rework, step 1: two players; players 3-4 count too).
// Everything still happens INSIDE the game's camera, so each area keeps its
// designed angle, its volume changes, its collision and its smoothing:
//   1. TARGET = the centre of the players in game. Every position the volume
//      code reads of an interest goes through sub_82105538 (interest record ->
//      pointer to a position: the interest's own override at +32 if flagged
//      0x10, else its actor's matrix position, its height HELD through a
//      jump). For a PLAYER's interest we answer the centre instead (a guest
//      buffer of ours), made of each player's own answer from the original,
//      so jumps keep the original's hold. The volume is then picked by the
//      centre, and the spread is 0, so the game's own zoom stays at its
//      minimum (the single-player look). The centre moves with a spring,
//      faster when a player nears the edge of the picture (URGENCY). With 3-4
//      players the centre leans toward the biggest GROUP (kGroupNear).
//   2. DISTANCE = enough to see everyone: a mid-function hook right where the
//      zoom is handed to its smoother (0x82107864) raises it to the smallest
//      distance at which every player (feet and head) is inside the picture,
//      with margins (the HUD sits in the top corners), never closer than the
//      game's own and never farther than the area's designed farthest x
//      --coop_camera_reach (capped at --coop_camera_max_distance; REACH).
// With one player in game nothing changes: the original camera, untouched.
// Cutscenes and scripted cameras don't use the volume behaviour: untouched,
// and while one runs ours is OFF (a view counts as the volume's only when the
// volume's zoom ran since the previous view: section 5 of findings/28). Back
// in play, ours takes over again from where the game's camera was looking.
// STEADY: the fit reads where the camera was last frame, and the camera moves
// with the fit, so a raw fit chased itself (12.1 -> 12.5 -> 12.1 units every
// ~5 frames with both players standing still: a visible shimmer). The camera's
// offset from the centre is now averaged over ~0.4 s, and the distance holds
// still for small changes (kZoomOutHold / kZoomInHold).
// ROOM: each player needs --coop_camera_room units of their surroundings on
// screen (left / right and beyond them), not just their own body.
//
// --debug_coop_camera_trace logs the camera, the players and the volume's
// numbers every 10th frame.
#include "coop_camera.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "more_players.h"
#include "../net/net_session.h"
#include "../data/fight_tree.h"

REXCVAR_DEFINE_BOOL(coop_camera, true, "CrashMoM",
                    "Co-op camera: frame every player in game (centre between them, back off until all "
                    "fit); false = the original camera that follows one player");
// REACH (2026-10-25): 45 units was far too far (players became specks; the
// original never goes past ~20). Now the farthest = the area's OWN designed
// farthest distance x coop_camera_reach (Wumpa Island 20), and never
// past coop_camera_max_distance. A player beyond that leaves the screen, where
// the game's own catch-up takes over (off screen, B = mask on a partner).
REXCVAR_DEFINE_DOUBLE(coop_camera_max_distance, 30.0, "CrashMoM",
                      "Co-op camera: the farthest it ever backs off from the players' centre (game units; the "
                      "original's range is about 12-20)");
// SECOND REWORK (2026-11-14, findings/28 s.12-13): 1.3 -> 1.0. Each volume's
// farthest zoom is designed for its place; past it the camera's line runs on
// beyond the designed camera space (behind scenery, out of the level).
REXCVAR_DEFINE_DOUBLE(coop_camera_reach, 1.0, "CrashMoM",
                      "Co-op camera: how far it may back off compared to the area's own farthest camera distance "
                      "(1 = never farther than the original camera; 1.3 = 30% farther)");
REXCVAR_DEFINE_DOUBLE(coop_camera_group_focus, 2.0, "CrashMoM",
                      "Co-op camera (3-4 players): how much more a GROUP of players near each other counts "
                      "in the centre than a player on their own (each player counts by group size to this "
                      "power; 0 = everyone counts the same)");
REXCVAR_DEFINE_DOUBLE(coop_camera_room, 2.0, "CrashMoM",
                      "Co-op camera: how much of each player's surroundings must stay on screen (game units "
                      "left / right of them and beyond them; Crash is about 2 tall). 0 = just the players");
REXCVAR_DEFINE_BOOL(coop_camera_pinned_rooms, true, "CrashMoM",
                    "Co-op camera: in rooms whose camera never moves in the original (Crash's house), stay near "
                    "its spot and widen the view instead of backing off (backing off there shows the outside)");
REXCVAR_DEFINE_BOOL(coop_camera_walls, true, "CrashMoM",
                    "Co-op camera: never back off through a wall (the scenery behind the camera); false = back "
                    "off freely (indoors it can end up outside the room)");
// GAME CLOCK (2026-11-14): our springs measured time by the PC's clock, the
// game's camera moves by the game's own frame step; with uneven frames the two
// disagree a little every frame (suspected fine jitter, uncapped play). On =
// our springs use the step the game handed its camera this frame.
REXCVAR_DEFINE_BOOL(coop_camera_game_clock, true, "CrashMoM",
                    "Co-op camera: smooth by the game's own frame step (false = the PC clock)");
// ONLINE OWN CAMERA, EXPERIMENT (online plan M4 / R2, 2026-11-17): in a
// session each PC's camera follows ONLY its own player (the others count as
// not in game for the camera). The world must not care: the stick no longer
// asks the camera (net/online_sticks.cpp); what else does is what this switch
// is for finding (two copies + the world fingerprint). Needs coop_camera on.
REXCVAR_DEFINE_BOOL(net_own_camera, false, "CrashMoM",
                    "Online (experiment): each PC's camera follows only its own player");
// THE SAME CAMERA OFFLINE, FOR TEST RECORDINGS (2026-11-19): with this on
// (and coop_camera, and no session) the camera follows only player 1, the
// way the host's own camera does online. A recording made like that
// (tools/play.sh --record-test) replays online with the host's camera, and so
// player 1's stick directions, the same as when it was recorded: measured,
// an online replay of a recording made with the usual co-op camera (framing
// both players) walked ~1 unit off in 15 s; with the shared camera online, 0.03.
REXCVAR_DEFINE_BOOL(debug_camera_player1_only, false, "CrashMoM",
                    "Debug: offline, the co-op camera follows only player 1 (like the host's own camera online)");

REXCVAR_DEFINE_BOOL(debug_coop_camera_trace, false, "CrashMoM",
                    "Debug: log the game camera, the players and the camera volume's numbers (co-op camera)");
REXCVAR_DEFINE_INT32(debug_coop_camera_trace_every, 10, "CrashMoM",
                     "Debug: --debug_coop_camera_trace logs every Nth camera frame");

namespace coop_camera {
namespace {
// A CUTSCENE OR CAMERA SHOT IS ON (2026-11-20): the front end sits in one of
// its "Cinematics" states (node 517 and everything under it, 518..729 in
// Frontend.bfig: the per-level groups L1..L9 with every nis_* / cam_* state;
// all states in that number range are in it, checked with fig_tree.py). Why:
// online with each PC's own camera, the guest's volume camera still ran
// during N. Gin's lab shot (cam_l1_r5_ngin, save "N.Gin After Mail") and our
// fit widened the SHOT's field of view on the guest's PC only (camera +16:
// 1.31 against the host's 1.01, same position and direction). A shot's view
// is never ours: everything below stands aside while one is on.
bool InCinematicState() {
  constexpr int32_t kFirstCinematic = 517, kLastCinematic = 729;
  const int32_t state = fight_tree::CurrentFrontEnd().state;
  return state >= kFirstCinematic && state <= kLastCinematic;
}


constexpr int kMaxPlayers = 4;
constexpr uint32_t kPlayerStates = 0x8259B11C;  // u32 per player (4 entries since more_players)
constexpr uint32_t kInGame = 2;                 // ECoOpPlayerState IN_GAME

// VectorCamera (findings/27): position / direction / up, horizontal field of
// view (radians) and aspect ratio (found 2026-10-06: Crash's feet on screen
// match +16 = 1.113 rad = 64 degrees at 16:9).
constexpr uint32_t kCamPosition = 216, kCamDirection = 228, kCamUp = 240;
constexpr uint32_t kCamFovX = 16, kCamAspect = 28;

// CCameraInterestBehaviour: +28 its actor, +56 flags (0x10 = fixed position).
constexpr uint32_t kInterestActor = 28, kInterestFlags = 56;
// An interest record of the volume behaviour (36 bytes): +4 the interest,
// +12 a position the original uses while blending a new record in.
constexpr uint32_t kRecordInterest = 4, kRecordPosition = 12;

// How much of the picture the players may use (fractions of the half-width /
// half-height from the centre). Top and bottom are tighter: HUDs sit in the
// corners (players 3-4 have theirs at the bottom).
constexpr float kMarginX = 0.78f, kMarginTop = 0.62f, kMarginBottom = 0.85f;  // (bottom 0.75 until 2026-11-14)
// The points of a player that must be on screen: feet and head.
constexpr float kCrashHeight = 2.0f, kTitanHeight = 4.5f;
// A player joining / leaving moves the centre over this long (seconds).
constexpr float kBlendSeconds = 0.6f;
// SMOOTHING: the camera glides after the players, and hurries only when one
// of them is about to leave the picture. URGENCY 0..1 =
// how close the player nearest the edge of the picture is to leaving it: 0
// inside 85% of the half-width / half-height, 1 at the edge or beyond.
//   - the centre follows the players with a critically damped spring, its
//     smoothing time from kSmoothCalm (urgency 0) to kSmoothUrgent (1);
//   - the distance: ONE spring both ways (kZoomSmooth, faster when urgent;
//     see CoopCameraDistance).
constexpr float kSmoothCalm = 0.22f, kSmoothUrgent = 0.06f;
// (0.85 until 2026-11-14: urgency then rose while everyone was still in the
// picture, the back-off ran at up to 10x, overshot, and the zoom came back in:
// the "bump and readjust" in play. Now 0 anywhere inside the picture, 1 once a
// player is kUrgentRange past its edge.)
constexpr float kUrgentFrom = 1.0f, kUrgentRange = 0.15f;
// GROUPS (3-4 players; added 2026-10-06): the camera leans toward where MOST
// players are. Each player's group size = 1 + how "near" every other player in
// game is (1 within kGroupNear units, fading to 0 at kGroupFar: a soft edge so
// nobody's weight jumps when someone crosses a line). In the centre a player
// counts group ^ coop_camera_group_focus: two together + one alone (power 2) =
// 4 : 4 : 1, so the centre sits with the pair and the lone player gets 1/9 of
// the say. 2 vs 2 = 4 each = balanced; with two players both always count the
// same, so two-player co-op doesn't change. The FIT still takes everyone: the
// lone player stays in the picture while the maximum distance allows it, and
// past it they are the one who leaves the screen first (where the game's own
// catch-up applies: off screen, B = become a mask on a partner).
constexpr float kGroupNear = 7.0f, kGroupFar = 15.0f;
// CCameraVolumeBehaviour +296: the camera's current (smoothed) distance from
// its target; +228: the target it is looking at (the game's own choice when we
// take over: the focus player, pulled toward enemies).
constexpr uint32_t kVolumeDistance = 296, kVolumeTarget = 228;
// STEADINESS (see the header): the camera's offset from the centre (in its
// own right / up / forward frame) is averaged with this time constant; the
// distance holds still for small changes (kZoomOutHold / kZoomInHold).
// Urgency falls back over kUrgencyFall seconds (rises at once), so the
// spring's speed doesn't flip with every step near the edge.
constexpr float kOffsetSeconds = 0.4f;
constexpr float kZoomSmooth = 1.0f, kZoomSmoothUrgent = 0.15f, kZoomOutHold = 0.8f, kZoomInHold = 0.5f;  // (0.6: swung 13 <-> 18 every 1-2 s)
constexpr float kUrgencyFall = 0.4f;
// BUMP JITTER (2026-10-25): bumping into scenery shook the camera a little.
// A bump pushes a player's position back and forth for a few frames, and
// three things answered that at once: urgency jumped to 1 (the spring went
// from 0.22 s to 0.06 s smoothing and the back-off to 10x), the fit pushed the
// distance out, and the wall test (a ray that hits / misses scenery edges
// frame by frame) pulled the camera in at once and let it out again. Now:
//   - the points the fit and urgency measure are each player's position
//     eased over kPointSeconds (the centre has its own spring anyway);
//   - urgency RISES over kUrgencyRise too (still falls over kUrgencyFall),
//     and while nobody-fits-even-at-the-farthest it stays below
//     kUrgencyCapped: hurrying can't help then, it only shook the picture;
//   - a wall that comes closer counts at once (never show outside the room),
//     one that goes away lets the camera out at kWallRelease units / second.
constexpr float kPointSeconds = 0.08f;
constexpr float kJoinIgnore = 1.2f;  // seconds after a join in which the fit ignores the joiner
constexpr float kUrgencyRise = 0.12f, kUrgencyCapped = 0.25f;
constexpr float kWallRelease = 3.0f;
// Camera frames after a take-over during which we don't push the distance
// out ourselves (see CoopCameraDistance).
constexpr int kSettleFrames = 15;
// BACK FROM A CUTSCENE (added 2026-10-09, findings/28 s.8): the picture cut
// anyway, so ours takes over at once: the centre jumps to the players and, for
// kSnapSeconds, the distance IS the fit (no settle, no gentle back-off, no
// deadband), measured on the last STEADY view from before the cutscene while
// the game's camera glides (~0.6 s from its own one-player spot, where the cut
// puts it, to ours), then on the live view once its direction holds still.
// Fits measured on the gliding view asked for the maximum (45 units), then
// crept back to 19.5 over ~3 s: the "zoom out + readjust" after the NV lab
// cutscene with partners left behind.
constexpr float kSnapSeconds = 1.0f;
// A LEVEL CHANGE / TELEPORT (added 2026-10-23): the players' middle moving
// more than kJumpUnits in ONE camera frame. Found: walking out of Crash's
// house, our camera stayed on under the loading screen and its smoothed
// centre was still in the house, 360 units away; at the first frames of
// Wumpa Island it glided across the map, the players looked like they were at
// the edge of the picture (urgency 1), the fit pushed the distance to ~35 and
// the game's smoother took ~4 s to come back to 12 with both players standing
// together at the door. Now such a jump cuts: the centre jumps to the players,
// and the distance stays the GAME'S own until the game's camera has stopped
// gliding into the new level (kLiveStill frames with an unchanged direction),
// then snaps to the fit (kSnapSeconds) on that settled view.
constexpr float kJumpUnits = 20.0f;
// WALLS (added 2026-10-23): the camera never backs off through the scenery.
// The original never needs it: each volume's zoom range is designed for its
// room (Crash's house: min = max = 6, it never moves). Ours backs off to fit
// everyone and, with two players at opposite walls of the house, went to 12.9
// units: outside the house, the sky beside it on screen. Each camera frame the
// game's own scenery ray (sub_8224C7F8, query flag 0x80 = scenery only: the
// one the Wii cursor uses to place its mojo collector, findings/47) runs from
// where the GAME'S OWN distance puts the camera (inside the room by design;
// kWallRayLift up) outward along the camera's line; a hit caps the distance
// kWallMargin before it. Furniture between the players and the camera
// doesn't count (a ray from the players hit the house's table). The test is skipped while the fit needs nothing more than the game's
// distance.
constexpr float kWallRayLift = 1.0f, kWallMargin = 0.6f;
// LINE OF SIGHT (second rework, 2026-11-14, findings/28 s.14): the walls test
// above starts where the GAME'S distance puts the camera; when that spot is
// already inside the scenery (the aim between a player on a cliff and one on
// the beach below: the volume's line from the low aim runs into the cliff) it
// never hits, and the camera sat inside the ground. A second ray runs from
// the aim itself (kSightLift up: over low things like a table) toward the
// camera; a hit puts the camera kWallMargin before it, closer than our
// distance, eased in (kSightIn); never closer than the game's own distance:
// the aim's fourth rule (room behind it) keeps that much clear.
constexpr float kSightLift = 1.5f, kSightIn = 3.0f;  // (8: still a visible lurch, 20 -> 15 units in 0.1 s)
// PINNED ROOMS (added 2026-10-23): a camera volume whose designed zoom range
// is a single value (min == max: Crash's house 6 / 6) is one the designers
// never let move. Its room is built for that one spot: the house has no wall
// on the camera's side at all (a stage set), so no ray finds one, and from
// 8 units back (6 by design) the floor's edge and the sky show. There the
// camera stays at kPinnedReach x the designed distance (1 = exactly; 1.15 still showed the sky in a corner), and when
// that isn't enough for everyone the FIELD OF VIEW widens instead (up to
// kMaxFovScale on the tangent: 64 -> ~82 degrees at 16:9), from the camera's
// own spot, eased (kFovOut / kFovIn per second). Volumes with a range (most
// of the outdoors: 12-20, 14-20) back off as before.
constexpr float kPinnedReach = 1.0f, kMaxFovScale = 1.4f;
constexpr float kFovOut = 4.0f, kFovIn = 1.5f;

uint8_t* Guest(uint32_t a) { return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(a); }
uint32_t Read32(uint32_t a) {
  const uint8_t* p = Guest(a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
float ReadFloat(uint32_t a) {
  const uint32_t b = Read32(a);
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}
void WriteFloat(uint32_t a, float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  uint8_t* p = Guest(a);
  p[0] = uint8_t(b >> 24); p[1] = uint8_t(b >> 16); p[2] = uint8_t(b >> 8); p[3] = uint8_t(b);
}

struct Vec { float x = 0, y = 0, z = 0; };
Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec operator*(Vec a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec Cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Length(Vec a) { return std::sqrt(Dot(a, a)); }
Vec Normalized(Vec a) {
  const float l = Length(a);
  return l > 1e-6f ? a * (1.0f / l) : Vec{0, 0, 1};
}
Vec ReadVec(uint32_t a) { return {ReadFloat(a), ReadFloat(a + 4), ReadFloat(a + 8)}; }
bool Finite(Vec v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// Where player p's body is (the titan they ride, else their Crash), false if
// they have none / an unreadable matrix.
bool BodyOf(int p, Vec& at, bool& titan) {
  const uint32_t t = more_players::TitanOfPlayer(p);
  const uint32_t actor = t ? t : more_players::CharacterOfPlayer(p);
  titan = t != 0;
  if (!actor) return false;
  const uint32_t matrix = Read32(actor + 44);  // the actor's world matrix (position at +48)
  if (matrix < 0x40000000) return false;
  at = ReadVec(matrix + 48);
  return Finite(at);
}

// --- State (game main thread only) ---------------------------------------------
using Clock = std::chrono::steady_clock;
struct Player {
  float weight = 0.0f;  // 0..1: how much this player counts in the centre (ramps)
  float joining = 0.0f; // seconds left in which the fit and centre ignore them (kJoinIgnore)
  bool seen = false;    // in game last frame (false = the next sighting is a join)
  float group = 1.0f;   // 1..4: this player's group size (see kGroupNear), for the centre
  Vec last;             // last known camera point (kept while fading out)
  Vec eased;            // `last` eased over kPointSeconds: what the fit / urgency measure
  bool eased_valid = false;
  bool in_game = false, titan = false;
  // The game's own camera point for this player (sub_82105538's answer for
  // their interest: the height HELD through a jump, eased after landing) and
  // the frame it was read in.
  Vec point;
  uint32_t point_frame = 0;
};
Player g_players[kMaxPlayers];
int g_in_game = 0;          // players in game this frame
bool g_active = false;      // our camera is in charge this frame
Vec g_centre;               // the target we answer (smoothed)
Vec g_centre_velocity;      // its spring's velocity
float g_urgency = 0.0f;     // 0..1, see kUrgentFrom
uint32_t g_frame = 1;       // camera frames counted (for Player::point_frame)
uint32_t g_answer = 0;      // guest buffer (3 floats) our answers point to
Clock::time_point g_last_update{};
float g_frame_dt = 0.0f;    // seconds since the previous camera frame
float g_game_dt = -1.0f;    // the step the game handed its camera this frame (sub_821076B0's f1), -1 = none
// The last camera view (read at every rebuild): for the "does everyone fit".
struct View {
  bool valid = false;
  Vec position, direction, up;
  float tan_x = 0.62f, tan_y = 0.35f;  // the GAME'S field of view (tangents of the half angles)
  float fov_scale = 1.0f;               // ours on top (PINNED ROOMS): the drawn view = tan x this
} g_view;
uint32_t g_volume = 0;     // the last CCameraVolumeBehaviour seen
// The volume's zoom ran since the last camera rebuild = the view being built
// is the volume's (gameplay). Cutscenes / scripted shots / the NIS camera
// rebuild the same VectorCamera without it.
bool g_volume_ran = false;
// The camera's offset from the centre in its own frame (right, up, forward),
// averaged (kOffsetSeconds); valid once seen.
Vec g_offset_local;
bool g_offset_valid = false;
float g_fit = 0.0f;        // the distance we ask for (deadband / ease-in state; 0 = none yet)
float g_zoom = 0.0f, g_zoom_velocity = 0.0f;  // ONE ZOOM SPRING: the distance we set, its speed
bool g_zoom_valid = false;  // false = the next frame starts from the camera's current distance
int g_active_frames = 0;    // camera frames since ours took over
// A non-volume view (cutscene, scripted shot) was seen in play since ours was
// last on: the next take-over SNAPS (kSnapSeconds) instead of gliding.
bool g_after_cutscene = false;
float g_snap_left = 0.0f;   // seconds left in which the distance snaps to the fit
// The last view (and averaged offset) while ours was on and settled: what the
// fit reads during the snap. Forgotten outside play (a level change).
View g_steady_view;
bool g_steady_valid = false;
Vec g_steady_offset;
// Camera frames in a row in which the live view's direction didn't change
// (the game's glide after a cut has ended once it's a few).
int g_live_still_frames = 0;
constexpr int kLiveStill = 3;
int32_t g_steady_volume = -1;  // the camera volume (+264) the steady view was seen in
// The steady view is used for the whole snap when the camera is back in the
// SAME volume (same angle: exact; measured 17.4 before and after the NV lab
// cutscene, while live fits wandered 19 -> 24 -> 17.4: the first views after a
// cut are still built around the game's own one-player target). In another
// volume only until the live view stops gliding.
bool UsingSteadyView() {
  if (g_snap_left <= 0.0f || !g_steady_valid) return false;
  const bool same_volume = g_volume && int32_t(Read32(g_volume + 264)) == g_steady_volume;
  return same_volume || g_live_still_frames < kLiveStill;
}
// The view the fit, the urgency and the near focus measure on: the steady one
// while snapping and the game's camera is still gliding, else the live one.
const View& FitView() { return UsingSteadyView() ? g_steady_view : g_view; }
// LEVEL CHANGE / TELEPORT (kJumpUnits): last frame's raw centre, and "hold the
// game's distance until its camera stops gliding".
Vec g_prev_raw;
bool g_prev_raw_valid = false;
bool g_hold_for_still = false;
// PINNED ROOMS: the field-of-view scale wanted this frame and the eased one drawn.
float g_fov_want = 1.0f, g_fov_scale = 1.0f;
// WALLS: the farthest distance the scenery allows (1e30 = none): the ray's
// answer this frame, and the one used (closer at once, released gently:
// kWallRelease).
float g_wall_raw = 1e30f;
float g_wall = 1e30f;
// LINE OF SIGHT: the ray's answer this frame and the one used (same easing).
float g_sight_raw = 1e30f;
float g_sight = 1e30f;
// The last fit couldn't get everyone in even at the farthest (kUrgencyCapped).
bool g_capped = false;
float g_game_zoom = 0.0f;  // the game's own zoom this frame, and ours (debug trace)
float g_our_zoom = 0.0f;
int g_trace_count = 0;
// INSIDE THE VOLUMES (second rework, findings/28 s.12.3 + s.13 item 3). The
// original's camera only follows its designed boxes while its target is
// inside a camera volume's target shape; with the target in NO volume
// (+264 = -1) sub_82106C38 skips the box mapping and drags the camera along
// by however much the target moved: nothing keeps it in the level then, and
// the zoom stops mattering. The players' middle lands in such gaps (between
// two ledges, over a wall, the hole in the middle of the Cortex arena's ring
// of volumes). So the point we answer (the AIM) is the centre only when the
// game's own volume search (sub_82107490: the current volume, its listed
// neighbours, then every volume of an enabled group) finds a volume for it;
// otherwise it slides from the ANCHOR (the camera's player: the game's focus,
// +40, else the first player in game) toward the centre and stops at the
// last point that is still inside a volume (bisection, kAimSteps).
constexpr int kAimSteps = 10;
Vec g_aim;                  // the point answered this camera frame (smoothed)
Vec g_aim_goal;             // what the rules ask for this frame
Vec g_hold;                 // the held-back part, smoothed (aim = centre + this)
Vec g_hold_velocity;        // its spring
bool g_aim_valid = false;   // false = the next aim jumps to its goal
constexpr float kAimSmooth = 0.3f;
constexpr float kHoldSpeed = 6.0f;  // units / second (see UpdateAim)
uint32_t g_aim_frame = 0;   // g_frame it was made in
bool g_aim_clamped = false; // the centre was outside every volume (trace)
float g_aim_t = 1.0f;       // how far from the anchor toward the centre it got
uint32_t g_probe = 0;       // guest buffer (3 floats) for the volume search

// The experiment above is on: a session runs and this PC follows its own player.
bool OwnCameraOnline() {
  if (REXCVAR_GET(debug_camera_player1_only) && !net::Configured()) return true;  // offline: player 1's
  return REXCVAR_GET(net_own_camera) && net::InSession() && net::LocalPlayer() >= 0;
}
// Whose camera it is (OwnCameraOnline true): this PC's player, player 1 offline.
int OwnCameraPlayer() { return net::Configured() ? net::LocalPlayer() : 0; }

// Once per frame (from the camera rebuild): who is in game, their weights and
// the centre. Players in game ramp up to 1, the others down to 0.
void UpdatePlayers(bool volume_view) {
  const Clock::time_point now = Clock::now();
  float dt = g_last_update == Clock::time_point{}
                 ? 0.0f
                 : std::chrono::duration<float>(now - g_last_update).count();
  g_last_update = now;
  if (REXCVAR_GET(coop_camera_game_clock) && g_game_dt > 0.0f && g_game_dt < 0.25f) dt = g_game_dt;
  g_game_dt = -1.0f;
  dt = std::clamp(dt, 0.0f, 0.1f);
  g_frame_dt = dt;
  const float step = dt / kBlendSeconds;
  ++g_frame;
  g_in_game = 0;
  for (int p = 0; p < kMaxPlayers; ++p) {
    Player& pl = g_players[p];
    Vec at;
    bool titan = false;
    pl.in_game = Read32(kPlayerStates + 4 * p) == kInGame && BodyOf(p, at, titan);
    if (pl.in_game && OwnCameraOnline() && p != OwnCameraPlayer()) pl.in_game = false;
    if (pl.in_game) {
      // A first sighting starts fully counted when nobody else is (load, the
      // first player); later joins fade in.
      if (!pl.seen) {  // a first sighting (a join, or loading)
        pl.seen = true;
        if (pl.weight == 0.0f) pl.weight = g_in_game == 0 && !g_active ? 1.0f : 0.0f;
        pl.joining = pl.weight == 0.0f ? kJoinIgnore : 0.0f;  // (a first player on loading: no)
      }
      pl.joining = std::max(0.0f, pl.joining - dt);
      // The game's camera point when the volume code asked for it this frame
      // or the last (jump hold), else the body (feet).
      pl.last = pl.point_frame + 2 >= g_frame ? pl.point : at;
      // BUMP JITTER: the eased point (a jump of kJumpUnits = a teleport: no easing).
      if (!pl.eased_valid || Length(pl.last - pl.eased) > kJumpUnits) {
        pl.eased = pl.last;
        pl.eased_valid = true;
      } else {
        pl.eased = pl.eased + (pl.last - pl.eased) * (1.0f - std::exp(-dt / kPointSeconds));
      }
      pl.titan = titan;
      // (Not in the centre either until the join's jump is over: their jump
      // lifted the centre 2 units and the camera backed off to keep player 1's
      // feet in the picture.)
      if (pl.joining <= 0.0f) pl.weight = std::min(1.0f, pl.weight + step);
      ++g_in_game;
    } else {
      pl.seen = false;
      pl.joining = 0.0f;
      pl.weight = std::max(0.0f, pl.weight - step);
      pl.eased_valid = false;
    }
  }
  // Group sizes (kGroupNear): players fading in / out count by their weight.
  for (int p = 0; p < kMaxPlayers; ++p) {
    Player& pl = g_players[p];
    pl.group = 1.0f;
    for (int q = 0; q < kMaxPlayers; ++q) {
      const Player& other = g_players[q];
      if (q == p || other.weight <= 0.0f) continue;
      // ("closeness", not "near": Windows' headers define near / far as macros)
      const float closeness = std::clamp((kGroupFar - Length(other.last - pl.last)) / (kGroupFar - kGroupNear), 0.0f, 1.0f);
      pl.group += closeness * other.weight;
    }
  }
  const float focus = std::max(0.0f, float(REXCVAR_GET(coop_camera_group_focus)));
  Vec sum;
  float total = 0.0f;
  for (const Player& pl : g_players) {
    const float w = pl.weight * std::pow(pl.group, focus);
    sum = sum + pl.last * w;
    total += w;
  }
  const bool was_active = g_active;
  if (!volume_view && more_players::InPlay()) g_after_cutscene = true;
  if (!more_players::InPlay()) g_steady_valid = false;
  g_snap_left = std::max(0.0f, g_snap_left - dt);
  g_active = volume_view && g_answer && REXCVAR_GET(coop_camera) && more_players::InPlay() &&
             g_in_game >= (OwnCameraOnline() ? 1 : 2) &&
             total > 0.0f;
  if (!g_active) {  // the next take-over starts fresh
    g_aim_valid = false;
    g_zoom_valid = false;
    g_offset_valid = false;
    g_fit = 0.0f;
    g_active_frames = 0;
  } else {
    ++g_active_frames;
  }
  if (total <= 0.0f) return;
  Vec raw = sum * (1.0f / total);
  // (NEAR FOCUS, 2026-10-09 .. 2026-11-14: front to back the centre sat with
  // the player nearest the camera. Removed in the second rework, findings/28
  // s.12-13: it could put the aim in a different camera volume than either
  // player, and felt iffy in play.)
  // LEVEL CHANGE / TELEPORT (kJumpUnits): cut to the players, keep the game's
  // distance while its camera glides in (CoopCameraDistance), then snap.
  const bool jumped = g_prev_raw_valid && Length(raw - g_prev_raw) > kJumpUnits;
  g_prev_raw = raw;
  g_prev_raw_valid = true;
  if (g_active && jumped) {
    g_hold_for_still = true;
    g_after_cutscene = false;
    g_steady_valid = false;  // (the old level's view)
    g_offset_valid = false;
    g_fit = 0.0f;
    g_snap_left = 0.0f;
    g_centre = raw;
    g_centre_velocity = {};
    g_aim_valid = false;
    return;
  }
  if (g_hold_for_still) {
    if (!g_active) {
      g_hold_for_still = false;
    } else if (g_view.valid && g_live_still_frames >= kLiveStill && g_volume &&
               int32_t(Read32(g_volume + 264)) >= 0 &&
               Length(g_view.position - raw) < 2.0f * std::max(g_game_zoom, 6.0f) + 4.0f) {
      // (Near the players too: under the loading screen the game's camera
      // stands still back in the OLD level, which looked "arrived" and gave a
      // 45-unit fit on that far view.)
      g_hold_for_still = false;  // the game's camera has arrived: snap to the fit on this view
      g_snap_left = kSnapSeconds;
      g_offset_valid = false;
    }
  }
  if (!was_active && g_after_cutscene && g_active) {
    // Back from a cutscene / scripted shot: the picture cut anyway, so start
    // on the players' centre right away (kSnapSeconds). Gliding from the
    // game's own view (one player) showed as a zoom out + re-aim lasting
    // ~2 s after the NV lab cutscene with partners left far behind.
    g_after_cutscene = false;
    g_snap_left = kSnapSeconds;
    if (g_steady_valid) {
      g_offset_local = g_steady_offset;
      g_offset_valid = true;
    }
    g_centre = raw;
    g_centre_velocity = {};
    g_aim_valid = false;
    return;
  }
  if (g_active) g_after_cutscene = false;
  // Remember a settled view (not while snapping, nor in the first frames).
  if (g_active && g_snap_left <= 0.0f && g_offset_valid && g_view.valid && g_active_frames > kSettleFrames) {
    g_steady_view = g_view;
    g_steady_offset = g_offset_local;
    g_steady_volume = g_volume ? int32_t(Read32(g_volume + 264)) : -1;
    g_steady_valid = true;
  }
  if (!was_active || !Finite(g_centre)) {
    // Taking over (a second player joins): start where the game's camera was
    // looking and glide to the players' centre, so the picture doesn't jump.
    const Vec target = g_volume ? ReadVec(g_volume + kVolumeTarget) : raw;
    g_centre = Finite(target) && Length(target - raw) < 30.0f ? target : raw;
    g_centre_velocity = {};
    return;
  }
  // Critically damped spring toward the raw centre (the usual "smooth damp":
  // no overshoot, eases in and out), faster the more urgent.
  const float smooth = kSmoothCalm + (kSmoothUrgent - kSmoothCalm) * g_urgency;
  const float omega = 2.0f / smooth, x = omega * dt;
  const float decay = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
  const Vec change = g_centre - raw;
  const Vec temp = (g_centre_velocity + change * omega) * dt;
  g_centre_velocity = (g_centre_velocity - temp * omega) * decay;
  g_centre = raw + (change + temp) * decay;
}

// URGENCY from the view the game just drew: 0 while every player's feet and
// head are inside kUrgentFrom of the picture, 1 at its edge or beyond.
void UpdateUrgency() {
  const float before = g_urgency;
  g_urgency = 0.0f;
  const View& view = FitView();
  if (!view.valid || !g_active) return;
  const Vec d = Normalized(view.direction);
  const Vec r = Normalized(Cross(view.up, d));
  const Vec u = Cross(d, r);
  float edge = 0.0f;  // 1 = the edge of the picture
  for (const Player& pl : g_players) {
    // (Only players fully in the centre: one still fading in after a join
    // isn't aimed at yet, so they sit near the edge and raised the alarm.)
    if (!pl.in_game || pl.weight < 0.999f) continue;
    const float height = pl.titan ? kTitanHeight : kCrashHeight;
    for (float h : {0.0f, height}) {
      const Vec v = pl.eased + Vec{0, h, 0} - view.position;
      const float depth = Dot(v, d);
      if (depth <= 0.5f) {
        edge = 2.0f;
        continue;
      }
      edge = std::max({edge, std::fabs(Dot(v, r) / (depth * view.tan_x * view.fov_scale)),
                       std::fabs(Dot(v, u) / (depth * view.tan_y * view.fov_scale))});
    }
  }
  float now = std::clamp((edge - kUrgentFrom) / kUrgentRange, 0.0f, 1.0f);
  // Nobody-fits-even-at-the-farthest: hurrying can't help (BUMP JITTER).
  if (g_capped) now = std::min(now, kUrgencyCapped);
  // Rises over kUrgencyRise, falls back over kUrgencyFall (BUMP JITTER: it
  // used to rise at once, so one bump near the edge flipped the spring fast).
  g_urgency = now > before ? std::min(now, before + g_frame_dt / kUrgencyRise)
                           : std::max(now, before - g_frame_dt / kUrgencyFall);
}

// The camera's offset from the centre, averaged (see kOffsetSeconds): from
// the view the game just built around the centre we answered.
void UpdateOffset() {
  if (!g_view.valid || !g_active || UsingSteadyView()) return;  // (the steady offset holds)
  const Vec d = Normalized(g_view.direction);
  const Vec r = Normalized(Cross(g_view.up, d));
  const Vec u = Cross(d, r);
  const Vec off = Normalized(g_view.position - g_aim);  // (the point this view was built on)
  const Vec local{Dot(off, r), Dot(off, u), Dot(off, d)};
  // Snapping on the live view (its glide ended): no averaging, it IS the offset.
  if (!g_offset_valid || g_snap_left > 0.0f) {
    g_offset_local = local;
    g_offset_valid = true;
    return;
  }
  const float k = 1.0f - std::exp(-g_frame_dt / kOffsetSeconds);
  g_offset_local = g_offset_local + (local - g_offset_local) * k;
}

// Player p whose body this actor is (Crash or jacked titan), -1 if none.
int PlayerOfActor(uint32_t actor) {
  if (!actor) return -1;
  for (int p = 0; p < kMaxPlayers; ++p) {
    if (actor == more_players::CharacterOfPlayer(p) || actor == more_players::TitanOfPlayer(p)) return p;
  }
  return -1;
}

// Where the camera sits relative to the centre (unit vector): the averaged
// offset (not last frame's raw position: see STEADY in the header).
Vec OffsetDirection(const View& view) {
  const Vec d = Normalized(view.direction);
  const Vec r = Normalized(Cross(view.up, d));
  const Vec u = Cross(d, r);
  Vec offset = g_offset_valid ? r * g_offset_local.x + u * g_offset_local.y + d * g_offset_local.z
                              : view.position - g_aim;
  if (Length(offset) < 1e-3f) offset = d * -1.0f;
  return Normalized(offset);
}

bool AllFit(float dist, float fov_scale, const View& view, Vec d, Vec r, Vec u, Vec offset);
float FitDistanceUncapped(float game_zoom, float max_d, const View& view, Vec d, Vec r, Vec u, Vec offset);

// The current camera volume's designed zoom range (+388 min, +392 max of its
// 424-byte record in the camera manager *(*(0x8259B190)+44), records from +8
// to +12; the volume behaviour's +264 = its index). False if unknown.
bool VolumeZoomRange(float& zoom_min, float& zoom_max) {
  if (!g_volume) return false;
  const int32_t index = int32_t(Read32(g_volume + 264));
  const uint32_t uber = Read32(0x8259B190);
  const uint32_t manager = uber ? Read32(uber + 44) : 0;
  if (index < 0 || !manager) return false;
  const uint32_t begin = Read32(manager + 8), end = Read32(manager + 12);
  if (begin < 0x40000000 || end <= begin || uint32_t(index) >= (end - begin) / 424) return false;
  zoom_min = ReadFloat(begin + 424 * uint32_t(index) + 388);
  zoom_max = ReadFloat(begin + 424 * uint32_t(index) + 392);
  return std::isfinite(zoom_min) && std::isfinite(zoom_max) && zoom_max > 0.0f;
}

// REACH: the farthest ours may back off in this area (see coop_camera_reach):
// the volume's designed farthest x the reach, never past the absolute maximum,
// never closer than the game's own distance this frame.
float MaxDistance(float game_zoom) {
  float max_d = float(REXCVAR_GET(coop_camera_max_distance));
  float zoom_min = 0.0f, zoom_max = 0.0f;
  if (VolumeZoomRange(zoom_min, zoom_max)) {
    max_d = std::min(max_d, zoom_max * std::max(1.0f, float(REXCVAR_GET(coop_camera_reach))));
  }
  return std::max(game_zoom, max_d);
}


// The smallest camera distance (target -> camera) at which every player in
// game is inside the picture, the camera keeping its current angle: from the
// last view, the camera sits on the line target + offset x d (offset = where
// the camera was relative to the centre, normalised), looking along its
// direction. Bisection between the game's own distance and the maximum.
float FitDistance(float game_zoom) {
  float max_d = MaxDistance(game_zoom);
  g_fov_want = 1.0f;
  g_capped = false;
  const View& view = FitView();
  if (!view.valid) return game_zoom;
  const Vec d = Normalized(view.direction);
  const Vec r = Normalized(Cross(view.up, d));  // screen right
  const Vec u = Cross(d, r);                       // screen up
  const Vec offset = OffsetDirection(view);
  // PINNED ROOMS (kPinnedReach): the designers' "never moves" volume keeps
  // the camera near its spot; the rest of the room comes from a wider view.
  float zoom_min = 0.0f, zoom_max = 0.0f;
  const bool pinned = REXCVAR_GET(coop_camera_pinned_rooms) && VolumeZoomRange(zoom_min, zoom_max) &&
                      zoom_max - zoom_min < 0.01f;
  if (pinned) max_d = std::max(game_zoom, std::min(max_d, zoom_max * kPinnedReach));
  // WALLS: never past the scenery behind the camera (g_wall, from the ray).
  if (g_wall < max_d) max_d = std::max(game_zoom, g_wall);
  const float dist = FitDistanceUncapped(game_zoom, max_d, view, d, r, u, offset);
  g_capped = dist >= max_d - 1e-3f && !AllFit(max_d, 1.0f, view, d, r, u, offset);
  if (pinned && !AllFit(max_d, 1.0f, view, d, r, u, offset)) {
    // Not everyone fits at the pinned distance: the smallest field-of-view
    // scale (up to kMaxFovScale) that shows them all.
    if (!AllFit(max_d, kMaxFovScale, view, d, r, u, offset)) {
      g_fov_want = kMaxFovScale;
    } else {
      float lo = 1.0f, hi = kMaxFovScale;
      for (int i = 0; i < 14; ++i) {
        const float mid = 0.5f * (lo + hi);
        (AllFit(max_d, mid, view, d, r, u, offset) ? hi : lo) = mid;
      }
      g_fov_want = hi;
    }
  }
  return std::min(dist, max_d);
}

// Every player in game inside the picture with the camera `dist` from the
// centre and the field of view widened by fov_scale (on the tangent)?
bool AllFit(float dist, float fov_scale, const View& view, Vec d, Vec r, Vec u, Vec offset) {
  // ROOM: besides feet and head, points `room` units to the player's left /
  // right (screen right, kept level) and beyond them (the camera's forward,
  // kept level) must be on screen too, so a player at the edge still sees the
  // ground around them. Not the ground between them and the camera: the
  // game's own view keeps Crash in the lower half, so that point (below him
  // on screen) would back the camera off even with everyone together (tried:
  // 12 -> 18-22 units at rest).
  const float room = std::max(0.0f, float(REXCVAR_GET(coop_camera_room)));
  const Vec side = Normalized(Vec{r.x, 0, r.z}) * room;
  const Vec ahead = Normalized(Vec{d.x, 0, d.z}) * room;
  const Vec cam = g_aim + offset * dist;  // (the camera is built around the answered aim)
  const float tan_x = view.tan_x * fov_scale, tan_y = view.tan_y * fov_scale;
  auto inside = [&](Vec point) {
    const Vec v = point - cam;
    const float depth = Dot(v, d);
    if (depth <= 0.5f) return false;
    const float sx = Dot(v, r) / (depth * tan_x);
    const float sy = Dot(v, u) / (depth * tan_y);
    return std::fabs(sx) <= kMarginX && sy <= kMarginTop && sy >= -kMarginBottom;
  };
  for (const Player& pl : g_players) {
    // (A player who just joined (kJoinIgnore), or is still fading into the
    // centre after it, doesn't push the distance yet: the aim isn't on them
    // yet, so they sat near the edge and the camera rushed out. A
    // join pops them out of the mask in a jump ~4 units up lasting about a
    // second, and the fit backed off 12 -> 17 for their head in mid-air, then
    // came back in over ~2 s. 2026-11-14.)
    if (!pl.in_game || pl.joining > 0.0f || pl.weight < 0.999f) continue;
    const float height = pl.titan ? kTitanHeight : kCrashHeight;
    const Vec feet = pl.eased_valid ? pl.eased : pl.last;
    const Vec head = feet + Vec{0, height, 0};
    if (!inside(feet) || !inside(head)) return false;
    if (room > 0.0f && (!inside(feet + side) || !inside(feet - side) || !inside(feet + ahead))) {
      return false;
    }
  }
  return true;
}

// The fit itself: the smallest distance between the game's and max_d at
// which everyone is in the picture (max_d when nobody-fits-even-there).
float FitDistanceUncapped(float game_zoom, float max_d, const View& view, Vec d, Vec r, Vec u, Vec offset) {
  if (AllFit(game_zoom, 1.0f, view, d, r, u, offset)) return game_zoom;
  if (!AllFit(max_d, 1.0f, view, d, r, u, offset)) return max_d;
  float lo = game_zoom, hi = max_d;
  for (int i = 0; i < 18; ++i) {
    const float mid = 0.5f * (lo + hi);
    (AllFit(mid, 1.0f, view, d, r, u, offset) ? hi : lo) = mid;
  }
  return hi;
}

void Trace(uint32_t camera) {
  if (++g_trace_count % std::max(1, REXCVAR_GET(debug_coop_camera_trace_every))) return;
  std::string s = fmt::format("Co-op camera: {} cam ({:.3f} {:.3f} {:.3f}) dir ({:.4f} {:.4f} {:.4f}) centre "
                              "({:.2f} {:.2f} {:.2f}) urgency {:.2f} zoom game {:.2f} ours {:.3f} dt {:.4f}{}{}{}{}{}{}",
                              g_active ? "ON " : "off", ReadFloat(camera + 216), ReadFloat(camera + 220),
                              ReadFloat(camera + 224), ReadFloat(camera + 228), ReadFloat(camera + 232),
                              ReadFloat(camera + 236), g_centre.x, g_centre.y, g_centre.z, g_urgency, g_game_zoom, g_our_zoom, g_frame_dt,
                              g_wall < 1e29f ? fmt::format(" wall {:.1f} (ray {})", g_wall,
                                                             g_wall_raw < 1e29f ? fmt::format("{:.1f}", g_wall_raw) : "none")
                                             : std::string(),
                              g_hold_for_still ? " (holding: level change)" : "",
                              g_sight < 1e29f ? fmt::format(" sight {:.1f}", g_sight) : std::string(),
                              g_fov_scale > 1.0005f ? fmt::format(" fov x{:.2f}", g_fov_scale) : std::string(),
                              g_capped ? " (at the farthest)" : "",
                              g_active && g_aim_clamped ? fmt::format(" AIM HELD BACK ({:.1f} {:.1f} {:.1f}, {:.0f}% of the way)",
                                                          g_aim_goal.x, g_aim_goal.y, g_aim_goal.z, 100.0f * g_aim_t)
                                            : std::string());
  for (int p = 0; p < kMaxPlayers; ++p) {
    const Player& pl = g_players[p];
    if (pl.weight == 0.0f && !pl.in_game) continue;
    s += fmt::format(" | P{} st {} w {:.2f} group {:.2f} ({:.1f} {:.1f} {:.1f})", p + 1,
                     Read32(kPlayerStates + 4 * p), pl.weight, pl.group, pl.last.x, pl.last.y, pl.last.z);
  }
  if (const uint32_t v = g_volume) {
    s += fmt::format(" || volume {} primaries {} spread {:.1f} distance {:.1f} focus P{}", int32_t(Read32(v + 264)),
                     Read32(v + 484), ReadFloat(v + 488), ReadFloat(v + 296), int32_t(Read32(0x825B0008 + 40)) + 1);
  }
  REXLOG_INFO("{}", s);
}

}  // namespace

void OnGameCameraRebuilt(uint32_t camera) {
  // The view the game just set (the next frame's fit starts from it).
  const Vec previous_direction = g_view.direction;
  g_view.position = ReadVec(camera + kCamPosition);
  g_view.direction = ReadVec(camera + kCamDirection);
  g_live_still_frames = Dot(Normalized(previous_direction), Normalized(g_view.direction)) > 0.99999f
                            ? g_live_still_frames + 1
                            : 0;
  g_view.up = ReadVec(camera + kCamUp);
  const float fov_x = ReadFloat(camera + kCamFovX), aspect = ReadFloat(camera + kCamAspect);
  if (fov_x > 0.1f && fov_x < 3.0f && aspect > 0.5f && aspect < 4.0f) {
    g_view.tan_x = std::tan(0.5f * fov_x);
    g_view.tan_y = g_view.tan_x / aspect;
  }
  // PINNED ROOMS: our wider view goes in before the rebuild (the game sets
  // +16 again every frame, so it never piles up). Eased; off at once when
  // ours isn't in charge (a cutscene's own view is never touched).
  if (g_volume_ran && g_active && !InCinematicState() && Finite(g_view.position)) {
    const float rate = g_fov_want > g_fov_scale ? kFovOut : kFovIn;
    g_fov_scale += (g_fov_want - g_fov_scale) * std::min(1.0f, g_frame_dt * rate);
    if (std::fabs(g_fov_scale - g_fov_want) < 0.002f) g_fov_scale = g_fov_want;
  } else {
    g_fov_scale = 1.0f;
    g_fov_want = 1.0f;
  }
  if (g_fov_scale > 1.0005f && fov_x > 0.1f && fov_x < 3.0f) {
    WriteFloat(camera + kCamFovX, 2.0f * std::atan(std::tan(0.5f * fov_x) * g_fov_scale));
  }
  g_view.fov_scale = g_fov_scale;
  // A view only counts when the volume camera built it (see g_volume_ran).
  const bool volume_view = g_volume_ran && !InCinematicState();
  g_volume_ran = false;
  g_view.valid = volume_view && Finite(g_view.position) && Finite(g_view.direction) && Finite(g_view.up);
  // Our answer's home in guest memory (once): the volume code reads the
  // position through the pointer we return.
  if (!g_answer) g_answer = rex::system::kernel_memory()->SystemHeapAlloc(16);
  if (REXCVAR_GET(debug_coop_camera_trace) && more_players::InPlay()) Trace(camera);
  UpdateOffset();  // (around the centre this view was built on: before it moves)
  UpdatePlayers(volume_view);
  UpdateUrgency();
}

bool SharedShotFov(uint32_t lr, double& fov) {
  constexpr uint32_t kRenderViewSetUp = 0x82381B64;  // return address in sub_82381B38
  constexpr uint32_t kGameGlobal = 0x8259B190, kFocus = 40;
  if (lr != kRenderViewSetUp || !OwnCameraOnline() || !net::InSession() || !InCinematicState()) return false;
  const uint32_t game = Read32(kGameGlobal);
  if (!game) return false;
  int focus = int32_t(Read32(game + kFocus));
  if (focus < 0 || focus > 3) focus = 0;
  if (focus == net::LocalPlayer()) return false;  // this PC already follows the focus player
  uint32_t words[10];
  if (!net::FrameCamera(focus, words)) return false;
  float f = 0.0f;
  std::memcpy(&f, &words[9], 4);  // the sent field of view (+16, the game's 16:9 one)
  if (!(f > 0.05f && f < 3.0f)) return false;
  static bool told = false;
  if (!told) {
    told = true;
    REXLOG_INFO("Co-op camera: a cutscene shot online: the field of view player {}'s PC sent ({:.3f}) instead of "
                "this PC's own camera's ({:.3f})", focus + 1, f, fov);
  }
  fov = f;
  return true;
}

}  // namespace coop_camera

extern "C" REX_FUNC(sub_82107490);
extern "C" REX_FUNC(__imp__sub_8224C7F8);
namespace coop_camera {
namespace {
float SceneryRay(PPCContext& ctx, uint8_t* base, Vec from, Vec to);  // (below, with the wall test)
// The game's own volume search (r3 = the current volume's index, r4 = a
// position; returns r3 = the index of an enabled volume whose target shape
// holds it, -1 if none). Read-only: it only tests shapes and groups.
bool InsideAVolume(PPCContext& ctx, uint8_t* base, Vec at) {
  if (!g_probe) g_probe = rex::system::kernel_memory()->SystemHeapAlloc(16);
  if (!g_probe || !g_volume) return true;  // can't ask: assume yes (the original's behaviour)
  WriteFloat(g_probe, at.x);
  WriteFloat(g_probe + 4, at.y);
  WriteFloat(g_probe + 8, at.z);
  const PPCContext saved = ctx;
  ctx.r3.u64 = Read32(g_volume + 264);  // (sign-extension doesn't matter: compared as a word)
  ctx.r4.u64 = g_probe;
  sub_82107490(ctx, base);
  const int32_t found = int32_t(ctx.r3.u32);
  ctx = saved;
  return found != -1;
}

// The camera's player (INSIDE THE VOLUMES): who the camera stays with when
// not everyone can be framed. A player RIDING A TITAN first (the game's focus
// if it rides one, else the first rider), then the game's focus, then the
// first player in game. -1 if none. (Riders first since 2026-11-14: in the
// Cortex fight player 2 jacked Cortex and rode the spinner platform up while
// player 1 stayed on the floor; the camera stayed with player 1, the focus,
// and the rider left the picture. The rider is the one doing the important
// thing; a partner left on foot goes off screen as in the original.)
int AnchorPlayer() {
  const int32_t focus = int32_t(Read32(0x825B0008 + 40));
  const bool focus_ok = focus >= 0 && focus < kMaxPlayers && g_players[focus].in_game;
  if (focus_ok && g_players[focus].titan) return focus;
  for (int p = 0; p < kMaxPlayers; ++p) {
    if (g_players[p].in_game && g_players[p].titan && g_players[p].weight >= 0.999f) return p;
  }
  if (focus_ok) return focus;
  for (int p = 0; p < kMaxPlayers; ++p) {
    if (g_players[p].in_game) return p;
  }
  return -1;
}

// The AIM for this camera frame (see INSIDE THE VOLUMES). A good aim is
// inside a camera volume AND sees the camera's player (the scenery ray from
// it to them, both kSightLift up, is clear). The second rule found
// 2026-11-14 on Wumpa Island: one player on the cliff by the house, one on
// the beach below; their middle was INSIDE the cliff, still inside a camera
// volume (target shapes are 20-25 units tall), and the camera, built around a
// point in the rock, showed the inside of the ground.
void UpdateAimTarget(PPCContext& ctx, uint8_t* base);
// The AIM glides to its target (AIM SMOOTHING, same day: the held-back point
// jumped up to 7 units between frames when a rule flipped, and the camera
// lurched): a critically damped spring (kAimSmooth), the rules' answer as
// its goal. A cut (a teleport, a level change, a take-over) jumps.
// Only the HELD-BACK PART (goal - centre) is smoothed, not the aim itself:
// the centre has its own spring already, and two springs in a row made the
// camera lag the players by ~0.5 s while they moved; the fit then backed off
// extra to keep the moving player in the picture and came back in once the
// aim caught up (measured: 12 -> 14.2 -> 12 at a join, 19.7 -> 18.1 after a
// short walk; seen in play as the camera "adjusting itself"). With nothing
// held back the part is 0 and the aim is the centre, without extra lag.
void UpdateAim(PPCContext& ctx, uint8_t* base) {
  g_aim_frame = g_frame;
  UpdateAimTarget(ctx, base);
  const Vec goal = g_aim_goal - g_centre;
  if (!g_aim_valid || !Finite(g_hold) || Length(goal - g_hold) > kJumpUnits) {
    g_hold = goal;
    g_hold_velocity = {};
    g_aim_valid = true;
  } else {
    const float omega = 2.0f / kAimSmooth, x = omega * g_frame_dt;
    const float decay = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    const Vec change = g_hold - goal;
    const Vec temp = (g_hold_velocity + change * omega) * g_frame_dt;
    g_hold_velocity = (g_hold_velocity - temp * omega) * decay;
    const Vec next = goal + (change + temp) * decay;
    // At most kHoldSpeed units a second: a rule flipping (a ray just hitting
    // or missing) moved the aim's goal several units and the view turned
    // ~2 degrees in 0.2 s, felt as a bump.
    const Vec step = next - g_hold;
    const float most = kHoldSpeed * g_frame_dt;
    g_hold = Length(step) > most ? g_hold + step * (most / Length(step)) : next;
  }
  g_aim = g_centre + g_hold;
}

void UpdateAimTarget(PPCContext& ctx, uint8_t* base) {
  g_aim_goal = g_centre;
  g_aim_clamped = false;
  g_aim_t = 1.0f;
  const int a = AnchorPlayer();
  const Vec anchor = a >= 0 ? g_players[a].last : g_centre;
  const Vec lift{0, kSightLift, 0};
  const bool walls = REXCVAR_GET(coop_camera_walls);
  // Third rule (same day, same place: players 75 units apart): the camera's
  // player must stay in the picture with the camera at its farthest. The aim
  // went 95% of the way to the middle, the camera ended up IN FRONT of player
  // 1 and both players were off screen. When not everyone can fit, the
  // original keeps its player and the other walks off screen (B brings them
  // back, findings/28 s.4): so does ours now.
  const View& view = FitView();
  const bool can_frame = view.valid && a >= 0;
  const Vec vd = Normalized(view.direction);
  const Vec vr = Normalized(Cross(view.up, vd));
  const Vec vu = Cross(vd, vr);
  const Vec offset = can_frame ? OffsetDirection(view) : Vec{};
  const float farthest = MaxDistance(g_game_zoom);
  const float anchor_height = a >= 0 && g_players[a].titan ? kTitanHeight : kCrashHeight;
  auto anchor_in_picture = [&](Vec at) {
    if (!can_frame) return true;
    const Vec cam = at + offset * farthest;
    for (float h : {0.0f, anchor_height}) {
      const Vec v = anchor + Vec{0, h, 0} - cam;
      const float depth = Dot(v, vd);
      if (depth <= 0.5f) return false;
      const float sx = Dot(v, vr) / (depth * view.tan_x * view.fov_scale);
      const float sy = Dot(v, vu) / (depth * view.tan_y * view.fov_scale);
      if (std::fabs(sx) > kMarginX || sy > kMarginTop || sy < -kMarginBottom) return false;
    }
    return true;
  };
  // Fourth rule (same day, from the motion: the camera LURCHED): room behind
  // the aim. From a spot hemmed in by a slope the line of sight hit the
  // ground a few units back and pulled the camera from 20 to 3 units in one
  // frame. The aim must see out to the game's own camera distance along the
  // camera's line; then the camera is never pulled closer than that.
  float zoom_lo = 0.0f, zoom_hi = 0.0f;
  const bool pinned = REXCVAR_GET(coop_camera_pinned_rooms) && VolumeZoomRange(zoom_lo, zoom_hi) &&
                      zoom_hi - zoom_lo < 0.01f;
  const bool check_back = walls && can_frame && !pinned && g_game_zoom > 0.5f;
  auto room_behind = [&](Vec at) {
    return !check_back || SceneryRay(ctx, base, at + lift, at + lift + offset * g_game_zoom) >= 1e29f;
  };
  auto good = [&](Vec at) {
    if (!anchor_in_picture(at) || !InsideAVolume(ctx, base, at) || !room_behind(at)) return false;
    return !walls || a < 0 || Length(at - anchor) < 0.5f || SceneryRay(ctx, base, at + lift, anchor + lift) >= 1e29f;
  };
  if (!Finite(g_centre) || good(g_centre)) return;
  if (a < 0 || !Finite(anchor) || !InsideAVolume(ctx, base, anchor)) return;  // nothing better to offer
  float lo = 0.0f, hi = 1.0f;
  for (int i = 0; i < kAimSteps; ++i) {
    const float mid = 0.5f * (lo + hi);
    (good(anchor + (g_centre - anchor) * mid) ? lo : hi) = mid;
  }
  g_aim_goal = anchor + (g_centre - anchor) * lo;
  g_aim_t = lo;
  g_aim_clamped = true;
}
}  // namespace
}  // namespace coop_camera

// SAFETY NET for INSIDE THE VOLUMES: the volume pick (r3 = the volume
// behaviour; +0xE4 = the target, the aim pulled toward SECONDARY interests:
// enemies, a boss, which can still push it out of every volume; result in
// +264). If it found none while ours is on, the target goes back to the aim
// and the pick runs again.
extern "C" REX_FUNC(__imp__sub_82107628);
extern "C" REX_FUNC(sub_82107628) {
  using namespace coop_camera;
  const uint32_t behaviour = ctx.r3.u32;
  const PPCContext saved = ctx;
  __imp__sub_82107628(ctx, base);
  if (!g_active || g_aim_frame + 1 < g_frame || int32_t(Read32(behaviour + 264)) != -1) return;
  WriteFloat(behaviour + 0xE4, g_aim.x);
  WriteFloat(behaviour + 0xE8, g_aim.y);
  WriteFloat(behaviour + 0xEC, g_aim.z);
  ctx = saved;
  __imp__sub_82107628(ctx, base);
}

// The position of an interest record (r3 = record; returns r3 = a pointer to 3
// floats). A player's interest answers the players' centre while ours is on.
// The original still runs first for it: it keeps the record's JUMP HOLD going
// (sub_82105758 / this function: the height is held from take-off while the
// interest's "in the air" flag 0x80 is set, eased to the new ground after
// landing, followed at once when landing lower; measured 2026-10-06 in one-
// player play: the camera's height didn't move during a jump, and eased up
// in ~0.25 s after landing 2 units higher), and its answer becomes that
// player's camera point in the centre. Record creation (sub_82105420 copies
// the answer into the new record, call at 0x82105450) gets the original.
extern "C" REX_FUNC(__imp__sub_82105538);
extern "C" REX_FUNC(sub_82105538) {
  using namespace coop_camera;
  const uint32_t record = ctx.r3.u32;
  if (g_active && uint32_t(ctx.lr) != 0x82105454) {
    const uint32_t interest = Read32(record + kRecordInterest);
    // An interest with a fixed position (flag 0x10, set by scripts) keeps it.
    const int p = interest && !(*Guest(interest + kInterestFlags) & 0x10)
                      ? PlayerOfActor(Read32(interest + kInterestActor))
                      : -1;
    if (p >= 0) {
      __imp__sub_82105538(ctx, base);
      const Vec point = ReadVec(ctx.r3.u32);
      if (Finite(point)) {
        g_players[p].point = point;
        g_players[p].point_frame = g_frame;
      }
      if (g_aim_frame != g_frame) UpdateAim(ctx, base);  // once per camera frame
      WriteFloat(g_answer, g_aim.x);
      WriteFloat(g_answer + 4, g_aim.y);
      WriteFloat(g_answer + 8, g_aim.z);
      ctx.r3.u64 = g_answer;
      return;
    }
  }
  __imp__sub_82105538(ctx, base);
}

// The camera volume's zoom (r3 = CCameraVolumeBehaviour): remember the object
// for the debug trace.
// Before it, the WALL test for this frame (see kWallRayLift): the game's
// scenery ray sub_8224C7F8 (r3 the world = *(uber + 48), r4 start, r5 end, r6
// a query: +0x1C flags 0x80 = scenery only, the rest unused for that; r7 the
// result: +0 the hit's distance from the start, +12 nonzero = hit; read from
// its decompile and sub_8224C6C0, the screen-spot ray around it).
extern "C" REX_FUNC(__imp__sub_821076B0);
extern "C" REX_FUNC(__imp__sub_8224C7F8);
namespace coop_camera {
namespace {
// The game's scenery ray from `from` to `to`: the hit's distance from `from`,
// 1e30 if nothing is in the way (see the WALL comment above).
float SceneryRay(PPCContext& ctx, uint8_t* base, Vec from, Vec to) {
  static uint32_t room = 0;  // start, end, query, result (guest memory, made once)
  constexpr uint32_t kStart = 0, kEnd = 16, kQuery = 32, kResult = 32 + 0x24;
  if (!room) room = rex::system::kernel_memory()->SystemHeapAlloc(kResult + 0x20);
  const uint32_t uber = Read32(0x8259B190);
  const uint32_t world = uber ? Read32(uber + 48) : 0;
  if (!room || !world || !Finite(from) || !Finite(to)) return 1e30f;
  std::memset(Guest(room), 0, kResult + 0x20);
  for (int i = 0; i < 3; ++i) {
    WriteFloat(room + kStart + 4 * i, (&from.x)[i]);
    WriteFloat(room + kEnd + 4 * i, (&to.x)[i]);
  }
  *Guest(room + kQuery + 0x1C) = 0x80;  // scenery only
  const PPCContext saved = ctx;
  ctx.r3.u64 = world;
  ctx.r4.u64 = room + kStart;
  ctx.r5.u64 = room + kEnd;
  ctx.r6.u64 = room + kQuery;
  ctx.r7.u64 = room + kResult;
  __imp__sub_8224C7F8(ctx, base);
  ctx = saved;
  const float hit = std::fabs(ReadFloat(room + kResult));
  return Read32(room + kResult + 12) && std::isfinite(hit) ? hit : 1e30f;
}
}  // namespace
}  // namespace coop_camera

extern "C" REX_FUNC(sub_821076B0) {
  using namespace coop_camera;
  g_volume = ctx.r3.u32;
  g_volume_ran = true;
  g_game_dt = float(ctx.f1.f64);  // (GAME CLOCK: the camera update's step, passed on unchanged)
  g_wall_raw = 1e30f;
  g_sight_raw = 1e30f;
  const bool pinned_room = [] {
    float lo = 0.0f, hi = 0.0f;
    return REXCVAR_GET(coop_camera_pinned_rooms) && VolumeZoomRange(lo, hi) && hi - lo < 0.01f;
  }();
  // Only while ours is on and has backed off (or wants to), or a wall is
  // still held (else: the wall pulls the camera in, the ray stops, the camera
  // goes out, the ray starts again = a back-and-forth): the game's own
  // distance is designed for the room.
  if (g_active && !g_hold_for_still && g_view.valid && REXCVAR_GET(coop_camera_walls) &&
      (g_fit > g_game_zoom + 0.05f || g_urgency > 0.0f || g_wall < 1e29f)) {
    const float reach = MaxDistance(g_game_zoom) + kWallMargin;
    // From the game's OWN camera spot (its distance: inside the room by
    // design) outward. (A first try started at the players: the table
    // between them and the camera in Crash's house stopped every back-off.)
    const Vec dir = OffsetDirection(g_view);
    const float hit = SceneryRay(ctx, base, g_aim + Vec{0, kWallRayLift, 0} + dir * g_game_zoom,
                                 g_aim + Vec{0, kWallRayLift, 0} + dir * reach);
    if (hit < 1e29f) g_wall_raw = g_game_zoom + std::max(0.0f, hit - kWallMargin);
  }
  // LINE OF SIGHT (second rework, 2026-11-14): from the aim (kSightLift up)
  // out to the farthest the camera may go. Not in pinned rooms (their
  // camera never moves; Crash's house has a table between the players and
  // the camera).
  if (g_active && !g_hold_for_still && g_view.valid && REXCVAR_GET(coop_camera_walls) && !pinned_room) {
    const Vec dir = OffsetDirection(g_view);
    const Vec from = g_aim + Vec{0, kSightLift, 0};
    const float hit = SceneryRay(ctx, base, from, from + dir * (MaxDistance(g_game_zoom) + kWallMargin));
      if (hit < 1e29f) g_sight_raw = std::max(g_game_zoom, hit - kWallMargin);
  }
  // BUMP JITTER: a closer wall counts at once, a wall that went away (or a
  // ray that slipped past an edge for a frame) lets the camera out gently.
  if (!g_active || g_hold_for_still) {
    g_wall = 1e30f;
  } else if (g_wall_raw <= g_wall) {
    g_wall = g_wall_raw;
  } else if (g_wall < 1e29f) {
    g_wall = std::min(g_wall_raw, g_wall + kWallRelease * g_frame_dt);
    if (g_wall > MaxDistance(g_game_zoom) + kWallMargin) g_wall = 1e30f;  // out of reach: no wall
  }
  // The same for the line of sight.
  if (!g_active || g_hold_for_still) {
    g_sight = 1e30f;
  } else if (g_sight_raw <= g_sight) {
    // In quickly but not in one frame (kSightIn x the gap per second).
    g_sight = g_sight >= 1e29f ? std::max(g_sight_raw, g_our_zoom - (g_our_zoom - g_sight_raw) * std::min(1.0f, g_frame_dt * kSightIn))
                               : std::max(g_sight_raw, g_sight - (g_sight - g_sight_raw) * std::min(1.0f, g_frame_dt * kSightIn));
  } else if (g_sight < 1e29f) {
    g_sight = std::min(g_sight_raw, g_sight + kWallRelease * g_frame_dt);
    if (g_sight > MaxDistance(g_game_zoom) + kWallMargin) g_sight = 1e30f;
  }
  __imp__sub_821076B0(ctx, base);
}

// ... the zoom is about to be handed to its smoother (sub_821076B0 at
// 0x82107864: f1 = the distance the game wants, r31 = the volume behaviour;
// both of its paths meet here). Ours: at least far enough for every player in
// game to be in the picture. The game's smoother (+296 = the current distance)
// backs off slowly (12 -> 45 took ~4 s: two players running apart left it
// behind, one of them off screen), so when more room is needed we move the
// current distance ourselves (ONE ZOOM SPRING below).
// (Both ways since 2026-11-14.)
void CoopCameraDistance(PPCRegister& f1, PPCRegister& r31) {
  using namespace coop_camera;
  g_game_zoom = float(f1.f64);
  g_our_zoom = g_game_zoom;
  if (!g_active || !std::isfinite(g_game_zoom) || g_game_zoom <= 0.0f) return;
  // Just after a level change / teleport: the game's own distance until its
  // camera has glided in (kJumpUnits).
  if (g_hold_for_still) return;
  const uint32_t current = r31.u32 + kVolumeDistance;
  // WALLS: a wall closer than the current distance pulls the camera in at once
  // (easing in would leave it outside the room for a second).
  const float wall_cap = std::max(g_game_zoom, g_wall);
  if (g_wall < 1e29f) {
    const float now = ReadFloat(current);
    if (std::isfinite(now) && now > wall_cap) WriteFloat(current, wall_cap);
    if (g_fit > wall_cap) g_fit = wall_cap;
  }
  // Out at once (when more room is needed), back in only once the fit is
  // clearly closer, then easing (STEADY in the header).
  const float fit = FitDistance(g_game_zoom);
  if (g_snap_left > 0.0f) {
    // Back from a cutscene (kSnapSeconds): the distance is the fit, at once.
    g_fit = fit;
    g_our_zoom = std::min(std::max(g_game_zoom, g_fit), g_sight);
    g_zoom = g_our_zoom;
    g_zoom_velocity = 0.0f;
    g_zoom_valid = true;
    f1.f64 = g_our_zoom;
    WriteFloat(r31.u32 + kVolumeDistance, g_our_zoom);
    return;
  }
  // ONE ZOOM SPRING (2026-11-14, from a replay of a real session: the zoom
  // "hit bumps and readjusted"). It used to go out by our own easing and come
  // back in by the game's slower smoother after a 0.6-unit dead zone: hold,
  // then suddenly move. Now, while ours is on, the distance is ours in both
  // directions: a critically damped spring toward the fit (kZoomSmooth, faster
  // when urgent), still with small dead zones (kZoomOutHold / kZoomInHold) so standing still
  // doesn't shimmer. The game's smoother then has nothing left to do.
  const float now = ReadFloat(current);
  if (!g_zoom_valid || !std::isfinite(g_zoom)) {
    g_zoom = std::isfinite(now) && now > 0.0f ? now : g_game_zoom;
    g_zoom_velocity = 0.0f;
    g_zoom_valid = true;
  }
  g_fit = fit;
  float want = std::max(g_game_zoom, fit);
  // Aim held back (someone can't be framed): the original in that situation
  // sits at its farthest distance (spread at its maximum); ours too, instead
  // of chasing a fit that can't succeed (it rippled 18 <-> 20 about once a
  // second as the held-back aim shifted).
  if (g_aim_clamped) want = MaxDistance(g_game_zoom);
  want = std::min(want, g_sight);
  // Not in the first quarter second after taking over (back from a cutscene:
  // the first fit reads the game's own one-player view), unless urgent.
  const bool settling = g_active_frames < kSettleFrames && g_urgency <= 0.0f;
  if (settling) want = std::min(want, std::max(g_zoom, g_game_zoom));
  // Small needs are ignored while the zoom is at rest (BREATHING, same day,
  // from a replay of a real session: out 1.5 units when a partner stepped 9
  // units away, back in when they came within 4, felt as "hitting a bump and
  // readjusting"): out only for at least kZoomOutHold more (at once when
  // someone is past the edge of the picture), in only for kZoomInHold less.
  // Once moving, it follows the fit all the way.
  if (std::fabs(g_zoom_velocity) < 0.5f) {
    const bool out_small = want > g_zoom && want < g_zoom + kZoomOutHold && g_urgency <= 0.0f;
    const bool in_small = want < g_zoom && want > g_zoom - kZoomInHold;
    if (out_small || in_small) want = g_zoom;
  }
  const float smooth = kZoomSmooth + (kZoomSmoothUrgent - kZoomSmooth) * g_urgency;
  const float omega = 2.0f / smooth, x = omega * g_frame_dt;
  const float decay = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
  const float change = g_zoom - want;
  const float temp = (g_zoom_velocity + change * omega) * g_frame_dt;
  g_zoom_velocity = (g_zoom_velocity - temp * omega) * decay;
  g_zoom = want + (change + temp) * decay;
  // Scenery limits act at once on top (they already ease where measured).
  g_zoom = std::min(g_zoom, std::max(g_game_zoom, g_wall));
  if (g_sight < g_zoom) g_zoom = g_sight;
  g_our_zoom = g_zoom;
  f1.f64 = g_our_zoom;
  WriteFloat(current, g_our_zoom);
}


// ACTIVE ZONES: every player keeps their part of the level running.
// A level is split into ZONES (the world manager's list at +20; each frame
// sub_822E6EC0 runs sub_822F0100 per zone, which updates the zone's characters
// only if sub_822EFFD8 calls it ACTIVE; a character belongs to the zone its
// position falls in, actor +40). Active = the zone is loaded (+596 == 2) and
// either the CAMERA is near it (sub_822F4AA0: the camera position
// *(*(uber + 76) + 444) + 216 against the zone's sphere +488/+500, then its
// portals) or it's the zone of the player the camera FOCUSES on (sub_822E7A70:
// game +40 = that player, their character's +40). Only ONE player counts: the
// others' zones run only while the camera is close. With the original camera
// a partner who walked that far was already off screen (and B pulls them back
// as a mask, findings/28 s.4); with ours, which backs off to fit everyone,
// they're on screen and simply STOP: no gravity, no input, until the focus
// player walks near (found 2026-10-08, findings/28 s.6: player 2 on the path
// up from the Ratcicle Kingdom courtyard froze at one spot, wherever player 1
// stood; her physics update stopped being called; same with --coop_camera=
// false, so the rule is the game's).
// Fix: a mid-function hook where both of sub_822EFFD8's paths meet with the
// answer in r11 (0x822F003C, r31 = the zone): a loaded zone that holds any
// player in game (their Crash, or the titan they ride) is active too.
void CoopCameraZoneActive(PPCRegister& r11, PPCRegister& r31) {
  using namespace coop_camera;
  if (r11.u32 & 0xFF) return;  // active already
  const uint32_t zone = r31.u32;
  if (!zone || Read32(zone + 596) != 2) return;  // not loaded: can't run anyway
  for (int p = 0; p < kMaxPlayers; ++p) {
    if (Read32(kPlayerStates + 4 * p) != kInGame) continue;
    const uint32_t t = more_players::TitanOfPlayer(p);
    const uint32_t actor = t ? t : more_players::CharacterOfPlayer(p);
    if (actor && Read32(actor + 40) == zone) {
      r11.u64 = 1;
      return;
    }
  }
}
