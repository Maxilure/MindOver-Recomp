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
// 3-4 never count at all.
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
//      game's own and never farther than --coop_camera_max_distance.
// With one player in game nothing changes: the original camera, untouched.
// Cutscenes and scripted cameras don't use the volume behaviour: untouched,
// and while one runs ours is OFF (a view counts as the volume's only when the
// volume's zoom ran since the previous view: section 5 of findings/28). Back
// in play, ours takes over again from where the game's camera was looking.
// STEADY: the fit reads where the camera was last frame, and the camera moves
// with the fit, so a raw fit chased itself (12.1 -> 12.5 -> 12.1 units every
// ~5 frames with both players standing still: a visible shimmer). The camera's
// offset from the centre is now averaged over ~0.4 s, and the distance only
// comes back in once the fit is clearly (kDeadband) closer.
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

REXCVAR_DEFINE_BOOL(coop_camera, true, "CrashMoM",
                    "Co-op camera: frame every player in game (centre between them, back off until all "
                    "fit); false = the original camera that follows one player");
REXCVAR_DEFINE_DOUBLE(coop_camera_max_distance, 45.0, "CrashMoM",
                      "Co-op camera: the farthest it backs off from the players' centre (game units; the "
                      "original's range is about 12-20)");
REXCVAR_DEFINE_DOUBLE(coop_camera_group_focus, 2.0, "CrashMoM",
                      "Co-op camera (3-4 players): how much more a GROUP of players near each other counts "
                      "in the centre than a player on their own (each player counts by group size to this "
                      "power; 0 = everyone counts the same)");
REXCVAR_DEFINE_DOUBLE(coop_camera_room, 4.0, "CrashMoM",
                      "Co-op camera: how much of each player's surroundings must stay on screen (game units "
                      "left / right of them and beyond them; Crash is about 2 tall). 0 = just the players");
REXCVAR_DEFINE_BOOL(coop_camera_near_focus, true, "CrashMoM",
                    "Co-op camera: front to back, the camera stays with the player NEAREST to it instead of "
                    "the middle of the players (false); a player further into the picture just looks smaller. "
                    "Left / right and height still use the middle");
REXCVAR_DEFINE_BOOL(debug_coop_camera_trace, false, "CrashMoM",
                    "Debug: log the game camera, the players and the camera volume's numbers (co-op camera)");
REXCVAR_DEFINE_INT32(debug_coop_camera_trace_every, 10, "CrashMoM",
                     "Debug: --debug_coop_camera_trace logs every Nth camera frame");

namespace coop_camera {
namespace {

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
constexpr float kMarginX = 0.78f, kMarginTop = 0.62f, kMarginBottom = 0.75f;
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
//   - backing off: the distance covers kZoomOut* x what's missing per second
//     (calm 1.5 = gentle, urgent 10 = ~95% in 0.3 s). Coming back in is the
//     game's own smoother.
constexpr float kSmoothCalm = 0.22f, kSmoothUrgent = 0.06f;
constexpr float kZoomOutCalm = 1.5f, kZoomOutUrgent = 10.0f;
constexpr float kUrgentFrom = 0.85f;
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
// own right / up / forward frame) is averaged with this time constant, and
// the distance we ask for only comes back in once the fit is kDeadband units
// closer (then it eases in at kZoomInRate x the difference per second).
// Urgency falls back over kUrgencyFall seconds (rises at once), so the
// spring's speed doesn't flip with every step near the edge.
constexpr float kOffsetSeconds = 0.4f;
constexpr float kDeadband = 0.6f, kZoomInRate = 1.0f;
constexpr float kUrgencyFall = 0.4f;
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
  float group = 1.0f;   // 1..4: this player's group size (see kGroupNear), for the centre
  Vec last;             // last known camera point (kept while fading out)
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
// The last camera view (read at every rebuild): for the "does everyone fit".
struct View {
  bool valid = false;
  Vec position, direction, up;
  float tan_x = 0.62f, tan_y = 0.35f;
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
bool g_closing_in = false;  // g_fit is easing in toward a closer fit
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
float g_game_zoom = 0.0f;  // the game's own zoom this frame, and ours (debug trace)
float g_our_zoom = 0.0f;
int g_trace_count = 0;

// Once per frame (from the camera rebuild): who is in game, their weights and
// the centre. Players in game ramp up to 1, the others down to 0.
void UpdatePlayers(bool volume_view) {
  const Clock::time_point now = Clock::now();
  float dt = g_last_update == Clock::time_point{}
                 ? 0.0f
                 : std::chrono::duration<float>(now - g_last_update).count();
  g_last_update = now;
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
    if (pl.in_game) {
      // A first sighting starts fully counted when nobody else is (load, the
      // first player); later joins fade in.
      if (pl.weight == 0.0f) pl.weight = g_in_game == 0 && !g_active ? 1.0f : 0.0f;
      // The game's camera point when the volume code asked for it this frame
      // or the last (jump hold), else the body (feet).
      pl.last = pl.point_frame + 2 >= g_frame ? pl.point : at;
      pl.titan = titan;
      pl.weight = std::min(1.0f, pl.weight + step);
      ++g_in_game;
    } else {
      pl.weight = std::max(0.0f, pl.weight - step);
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
  g_active = volume_view && g_answer && REXCVAR_GET(coop_camera) && more_players::InPlay() && g_in_game >= 2 &&
             total > 0.0f;
  if (!g_active) {  // the next take-over starts fresh
    g_offset_valid = false;
    g_fit = 0.0f;
    g_active_frames = 0;
  } else {
    ++g_active_frames;
  }
  if (total <= 0.0f) return;
  Vec raw = sum * (1.0f / total);
  // NEAR FOCUS (added 2026-10-09): front to back (the camera's forward,
  // kept level) the centre sits with the player NEAREST the camera, not in
  // the middle. With the middle, a player walking deep into the picture (up
  // a path away from the camera) dragged the camera's aim halfway after them
  // and the player near the camera fell off the bottom of the picture, even
  // though the far one was in plain view (just small, near the horizon).
  // The game looks down only ~10 degrees, so a far player on the ground
  // stays near the middle of the picture by themselves; only left / right
  // and height need the middle. The fit (FitDistance) still backs off when
  // the far player would leave the top or the sides. min() over players is
  // continuous, so two players swapping who's nearer doesn't jump.
  if (REXCVAR_GET(coop_camera_near_focus) && FitView().valid) {
    const Vec forward = Vec{FitView().direction.x, 0.0f, FitView().direction.z};
    if (Length(forward) > 0.1f) {
      const Vec f = Normalized(forward);
      float nearest = 1e30f;
      for (const Player& pl : g_players) {
        if (pl.in_game) nearest = std::min(nearest, Dot(pl.last, f));
      }
      if (nearest < 1e29f) raw = raw + f * (nearest - Dot(raw, f));
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
    if (!pl.in_game) continue;
    const float height = pl.titan ? kTitanHeight : kCrashHeight;
    for (float h : {0.0f, height}) {
      const Vec v = pl.last + Vec{0, h, 0} - view.position;
      const float depth = Dot(v, d);
      if (depth <= 0.5f) {
        edge = 2.0f;
        continue;
      }
      edge = std::max({edge, std::fabs(Dot(v, r) / (depth * view.tan_x)),
                       std::fabs(Dot(v, u) / (depth * view.tan_y))});
    }
  }
  const float now = std::clamp((edge - kUrgentFrom) / (1.0f - kUrgentFrom), 0.0f, 1.0f);
  // Rises at once, falls back gently (kUrgencyFall).
  g_urgency = std::max(now, before - g_frame_dt / kUrgencyFall);
}

// The camera's offset from the centre, averaged (see kOffsetSeconds): from
// the view the game just built around the centre we answered.
void UpdateOffset() {
  if (!g_view.valid || !g_active || UsingSteadyView()) return;  // (the steady offset holds)
  const Vec d = Normalized(g_view.direction);
  const Vec r = Normalized(Cross(g_view.up, d));
  const Vec u = Cross(d, r);
  const Vec off = Normalized(g_view.position - g_centre);
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

// The smallest camera distance (target -> camera) at which every player in
// game is inside the picture, the camera keeping its current angle: from the
// last view, the camera sits on the line target + offset x d (offset = where
// the camera was relative to the centre, normalised), looking along its
// direction. Bisection between the game's own distance and the maximum.
float FitDistance(float game_zoom) {
  const float max_d = std::max(game_zoom, float(REXCVAR_GET(coop_camera_max_distance)));
  const View& view = FitView();
  if (!view.valid) return game_zoom;
  const Vec d = Normalized(view.direction);
  const Vec r = Normalized(Cross(view.up, d));  // screen right
  const Vec u = Cross(d, r);                       // screen up
  // Where the camera sits relative to the centre: the averaged offset (not
  // last frame's raw position: see STEADY in the header).
  Vec offset = g_offset_valid ? r * g_offset_local.x + u * g_offset_local.y + d * g_offset_local.z
                              : view.position - g_centre;
  if (Length(offset) < 1e-3f) offset = d * -1.0f;
  offset = Normalized(offset);
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
  auto fits = [&](float dist) {
    const Vec cam = g_centre + offset * dist;
    auto inside = [&](Vec point) {
      const Vec v = point - cam;
      const float depth = Dot(v, d);
      if (depth <= 0.5f) return false;
      const float sx = Dot(v, r) / (depth * view.tan_x);
      const float sy = Dot(v, u) / (depth * view.tan_y);
      return std::fabs(sx) <= kMarginX && sy <= kMarginTop && sy >= -kMarginBottom;
    };
    for (const Player& pl : g_players) {
      if (!pl.in_game) continue;
      const float height = pl.titan ? kTitanHeight : kCrashHeight;
      const Vec head = pl.last + Vec{0, height, 0};
      if (!inside(pl.last) || !inside(head)) return false;
      if (room > 0.0f && (!inside(pl.last + side) || !inside(pl.last - side) || !inside(pl.last + ahead))) {
        return false;
      }
    }
    return true;
  };
  if (fits(game_zoom)) return game_zoom;
  if (!fits(max_d)) return max_d;
  float lo = game_zoom, hi = max_d;
  for (int i = 0; i < 18; ++i) {
    const float mid = 0.5f * (lo + hi);
    (fits(mid) ? hi : lo) = mid;
  }
  return hi;
}

void Trace(uint32_t camera) {
  if (++g_trace_count % std::max(1, REXCVAR_GET(debug_coop_camera_trace_every))) return;
  std::string s = fmt::format("Co-op camera: {} cam ({:.1f} {:.1f} {:.1f}) dir ({:.2f} {:.2f} {:.2f}) centre "
                              "({:.1f} {:.1f} {:.1f}) urgency {:.2f} zoom game {:.1f} ours {:.1f}",
                              g_active ? "ON " : "off", ReadFloat(camera + 216), ReadFloat(camera + 220),
                              ReadFloat(camera + 224), ReadFloat(camera + 228), ReadFloat(camera + 232),
                              ReadFloat(camera + 236), g_centre.x, g_centre.y, g_centre.z, g_urgency, g_game_zoom, g_our_zoom);
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
  // A view only counts when the volume camera built it (see g_volume_ran).
  const bool volume_view = g_volume_ran;
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

}  // namespace coop_camera

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
      WriteFloat(g_answer, g_centre.x);
      WriteFloat(g_answer + 4, g_centre.y);
      WriteFloat(g_answer + 8, g_centre.z);
      ctx.r3.u64 = g_answer;
      return;
    }
  }
  __imp__sub_82105538(ctx, base);
}

// The camera volume's zoom (r3 = CCameraVolumeBehaviour): remember the object
// for the debug trace.
extern "C" REX_FUNC(__imp__sub_821076B0);
extern "C" REX_FUNC(sub_821076B0) {
  coop_camera::g_volume = ctx.r3.u32;
  coop_camera::g_volume_ran = true;
  __imp__sub_821076B0(ctx, base);
}

// ... the zoom is about to be handed to its smoother (sub_821076B0 at
// 0x82107864: f1 = the distance the game wants, r31 = the volume behaviour;
// both of its paths meet here). Ours: at least far enough for every player in
// game to be in the picture. The game's smoother (+296 = the current distance)
// backs off slowly (12 -> 45 took ~4 s: two players running apart left it
// behind, one of them off screen), so when more room is needed we move the
// current distance out ourselves: gently, quickly when urgent (kZoomOut*).
// Coming back in stays the game's.
void CoopCameraDistance(PPCRegister& f1, PPCRegister& r31) {
  using namespace coop_camera;
  g_game_zoom = float(f1.f64);
  g_our_zoom = g_game_zoom;
  if (!g_active || !std::isfinite(g_game_zoom) || g_game_zoom <= 0.0f) return;
  // Out at once (when more room is needed), back in only once the fit is
  // clearly closer, then easing (STEADY in the header).
  const float fit = FitDistance(g_game_zoom);
  if (g_snap_left > 0.0f) {
    // Back from a cutscene (kSnapSeconds): the distance is the fit, at once.
    g_fit = fit;
    g_closing_in = false;
    g_our_zoom = std::max(g_game_zoom, g_fit);
    f1.f64 = g_our_zoom;
    WriteFloat(r31.u32 + kVolumeDistance, g_our_zoom);
    return;
  }
  if (g_fit <= 0.0f || fit >= g_fit) {
    g_fit = fit;
    g_closing_in = false;
  } else {
    // Closing in starts past the deadband and then goes all the way.
    if (fit < g_fit - kDeadband) g_closing_in = true;
    if (g_closing_in) g_fit = std::max(fit, g_fit - std::max(0.05f, (g_fit - fit) * std::min(1.0f, g_frame_dt * kZoomInRate)));
  }
  g_our_zoom = std::max(g_game_zoom, g_fit);
  f1.f64 = g_our_zoom;
  const uint32_t current = r31.u32 + kVolumeDistance;
  const float now = ReadFloat(current);
  // Not in the first quarter second after taking over (back from a
  // cutscene: the first fit reads the game's own view, built around one
  // player, and asked 14 instead of 12 units for a moment), unless someone is
  // about to leave the picture.
  const bool settling = g_active_frames < kSettleFrames && g_urgency <= 0.0f;
  if (std::isfinite(now) && g_our_zoom > now && !settling) {
    const float rate = kZoomOutCalm + (kZoomOutUrgent - kZoomOutCalm) * g_urgency;
    WriteFloat(current, now + (g_our_zoom - now) * std::min(1.0f, g_frame_dt * rate));
  }
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
