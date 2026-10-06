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
//      faster when a player nears the edge of the picture (URGENCY).
//   2. DISTANCE = enough to see everyone: a mid-function hook right where the
//      zoom is handed to its smoother (0x82107864) raises it to the smallest
//      distance at which every player (feet and head) is inside the picture,
//      with margins (the HUD sits in the top corners), never closer than the
//      game's own and never farther than --coop_camera_max_distance.
// With one player in game nothing changes: the original camera, untouched.
// Cutscenes and scripted cameras don't use the volume behaviour: untouched.
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
// CCameraVolumeBehaviour +296: the camera's current (smoothed) distance from its target.
constexpr uint32_t kVolumeDistance = 296;

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
uint32_t g_volume = 0;     // the last CCameraVolumeBehaviour seen (debug trace)
float g_game_zoom = 0.0f;  // the game's own zoom this frame, and ours (debug trace)
float g_our_zoom = 0.0f;
int g_trace_count = 0;

// Once per frame (from the camera rebuild): who is in game, their weights and
// the centre. Players in game ramp up to 1, the others down to 0.
void UpdatePlayers() {
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
  Vec sum;
  float total = 0.0f;
  for (const Player& pl : g_players) {
    sum = sum + pl.last * pl.weight;
    total += pl.weight;
  }
  const bool was_active = g_active;
  g_active = g_answer && REXCVAR_GET(coop_camera) && more_players::InPlay() && g_in_game >= 2 && total > 0.0f;
  if (total <= 0.0f) return;
  const Vec raw = sum * (1.0f / total);
  if (!was_active || !Finite(g_centre)) {  // taking over: start where the game looked
    g_centre = raw;
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
  g_urgency = 0.0f;
  if (!g_view.valid) return;
  const Vec d = Normalized(g_view.direction);
  const Vec r = Normalized(Cross(g_view.up, d));
  const Vec u = Cross(d, r);
  float edge = 0.0f;  // 1 = the edge of the picture
  for (const Player& pl : g_players) {
    if (!pl.in_game) continue;
    const float height = pl.titan ? kTitanHeight : kCrashHeight;
    for (float h : {0.0f, height}) {
      const Vec v = pl.last + Vec{0, h, 0} - g_view.position;
      const float depth = Dot(v, d);
      if (depth <= 0.5f) {
        edge = 2.0f;
        continue;
      }
      edge = std::max({edge, std::fabs(Dot(v, r) / (depth * g_view.tan_x)),
                       std::fabs(Dot(v, u) / (depth * g_view.tan_y))});
    }
  }
  g_urgency = std::clamp((edge - kUrgentFrom) / (1.0f - kUrgentFrom), 0.0f, 1.0f);
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
  if (!g_view.valid) return game_zoom;
  const Vec d = Normalized(g_view.direction);
  const Vec r = Normalized(Cross(g_view.up, d));  // screen right
  const Vec u = Cross(d, r);                       // screen up
  Vec offset = g_view.position - g_centre;
  if (Length(offset) < 1e-3f) offset = d * -1.0f;
  offset = Normalized(offset);
  auto fits = [&](float dist) {
    const Vec cam = g_centre + offset * dist;
    for (const Player& pl : g_players) {
      if (!pl.in_game) continue;
      const float height = pl.titan ? kTitanHeight : kCrashHeight;
      for (float h : {0.0f, height}) {
        const Vec v = pl.last + Vec{0, h, 0} - cam;
        const float depth = Dot(v, d);
        if (depth <= 0.5f) return false;
        const float sx = Dot(v, r) / (depth * g_view.tan_x);
        const float sy = Dot(v, u) / (depth * g_view.tan_y);
        if (std::fabs(sx) > kMarginX || sy > kMarginTop || sy < -kMarginBottom) return false;
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
    s += fmt::format(" | P{} st {} w {:.2f} ({:.1f} {:.1f} {:.1f})", p + 1, Read32(kPlayerStates + 4 * p),
                     pl.weight, pl.last.x, pl.last.y, pl.last.z);
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
  g_view.position = ReadVec(camera + kCamPosition);
  g_view.direction = ReadVec(camera + kCamDirection);
  g_view.up = ReadVec(camera + kCamUp);
  const float fov_x = ReadFloat(camera + kCamFovX), aspect = ReadFloat(camera + kCamAspect);
  if (fov_x > 0.1f && fov_x < 3.0f && aspect > 0.5f && aspect < 4.0f) {
    g_view.tan_x = std::tan(0.5f * fov_x);
    g_view.tan_y = g_view.tan_x / aspect;
  }
  g_view.valid = Finite(g_view.position) && Finite(g_view.direction) && Finite(g_view.up);
  // Our answer's home in guest memory (once): the volume code reads the
  // position through the pointer we return.
  if (!g_answer) g_answer = rex::system::kernel_memory()->SystemHeapAlloc(16);
  if (REXCVAR_GET(debug_coop_camera_trace) && more_players::InPlay()) Trace(camera);
  UpdatePlayers();
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
  g_our_zoom = FitDistance(g_game_zoom);
  f1.f64 = g_our_zoom;
  const uint32_t current = r31.u32 + kVolumeDistance;
  const float now = ReadFloat(current);
  if (std::isfinite(now) && g_our_zoom > now) {
    const float rate = kZoomOutCalm + (kZoomOutUrgent - kZoomOutCalm) * g_urgency;
    WriteFloat(current, now + (g_our_zoom - now) * std::min(1.0f, g_frame_dt * rate));
  }
}
