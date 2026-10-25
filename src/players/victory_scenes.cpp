// =============================================================================
// players/victory_scenes.cpp -- see victory_scenes.h (docs/findings/26 s.39)
// =============================================================================
//
// HOW (game addresses verified with xexdis / Ghidra, 2026-10-17):
//
// 1. THE TELEPORT (sub_8229E3C8, r3 = the action, r4 / r5 = the two spots:
//    locators, position at +0x1C, rotation at +0xC). Per spot it builds a
//    4 x 4 matrix (sub_822FE2A8(spot, m)), lets the body adjust the position
//    row m+48 (sub_8229E250(body, m+48): asks the body message 0x11), builds
//    a teleport message from the matrix (sub_820B7DF0(msg, m, 0, 0)), sets
//    the message's flag byte +0x54 = (action +0x18 >> 1 & 0x10) | 0x20 |
//    (old & 0xEF), sends it (sub_820B0B70(body, msg)) and frees it
//    (sub_820B7F50). Bodies: sub_8226FC88(game, player) (4 players since
//    more_players.cpp). We run the same steps for players 3-4 after the
//    original, with the position BETWEEN the two spots (player 2's own spot
//    when player 2 isn't playing): the same action moves players at area
//    doors, where a spot beyond them can be over a drop (findings/26 s.48).
//
// 2. THE "HOLD STILL" LOOP. The action's update (sub_8229DE30), on its first
//    call when its flag +0x18 bit 0x40 is set (the variant that waits for a
//    fade), sends players 1-2's bodies message 8 with (0, 1, 0)
//    (sub_820B6340(msg, 8, &v), sub_820B0B70, sub_820B6508): a "for p < 2"
//    loop. Players 3-4 get it from our wrapper.
//
// 3. THE SHOT. WAIT_PlayCinematic's requirement (CRequirementPlayCinematic,
//    vtable 0x8204103C, update sub_822A36D0) gives the cinematic manager
//    (game +0x6C) the camera's name (+0x28) and starts it; the manager's +0x2C
//    has bit 0x10, 0x40 or 0x80 set while it plays. A teleport that moved
//    player 3 or 4 arms the next shot (within 15 s); while that shot plays,
//    every rebuild of the camera (VectorCamera: position +216, direction
//    +228, up +240, horizontal field of view +16, aspect +28) is preceded by
//    our pull-back: the position goes back along the direction by the least
//    distance k with every player's feet and head inside the picture's
//    margins (for a point at camera-space (x, y, z): |x| <= m tan(fov/2)
//    (z + k), per point k >= |x| / (m tan) - z; the same for y). The shot's
//    direction, framing of its centre and movement stay the game's.
// =============================================================================

#include "victory_scenes.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "more_players.h"

extern "C" REX_FUNC(__imp__sub_8229E3C8);  // DO_TeleportPlayer: move the bodies to the two spots
extern "C" REX_FUNC(__imp__sub_8229DE30);  // DO_TeleportPlayer: the action's update
extern "C" REX_FUNC(__imp__sub_822FE2A8);  // locator -> 4 x 4 matrix (r3 locator, r4 matrix)
extern "C" REX_FUNC(__imp__sub_8229E250);  // the body adjusts a teleport position (r3 body, r4 position)
extern "C" REX_FUNC(__imp__sub_820B7DF0);  // teleport message from a matrix (r3 msg, r4 matrix, 0, 0)
extern "C" REX_FUNC(__imp__sub_820B0B70);  // send a message to an actor (r3 actor, r4 msg)
extern "C" REX_FUNC(__imp__sub_820B7F50);  // teleport message: free
extern "C" REX_FUNC(__imp__sub_820B6340);  // message (r3 msg, r4 type, r5 vector)
extern "C" REX_FUNC(__imp__sub_820B6508);  // message: free
// Player r4's body (r3 game object). NOT __imp__: that is the original, which
// knows players 1-2 only (the first version called it: players 3-4 were
// skipped); sub_8226FC88 is more_players.cpp's 4-player version.
extern "C" REX_FUNC(sub_8226FC88);

namespace victory_scenes {
namespace {

constexpr int kMaxPlayers = 4;
constexpr uint32_t kGameObject = 0x825B0008;     // sub_82270608's answer
constexpr uint32_t kGameGlobal = 0x8259B190;     // the game (front end, cinematics, ...)
constexpr uint32_t kCinematicsOf = 0x6C;         // game +0x6C: the cinematic manager
constexpr uint32_t kCinematicFlags = 0x2C;       // manager: 0x10 / 0x40 / 0x80 = playing
constexpr uint32_t kPlayerStates = 0x8259B11C;   // u32 per player: 2 = in game
constexpr uint32_t kSpotPosition = 0x1C;         // locator: position (3 floats)
constexpr uint32_t kCamPosition = 216, kCamDirection = 228, kCamUp = 240;
constexpr uint32_t kCamFovX = 16, kCamAspect = 28;
// How much of the picture the players may use (fraction of the half-width /
// half-height from the centre), and the heights checked above the feet.
constexpr float kMarginX = 0.85f, kMarginTop = 0.80f, kMarginBottom = 0.88f;
constexpr float kCrashHeight = 2.0f, kTitanHeight = 4.5f;
constexpr int64_t kArmedMs = 15000;  // a shot must start this soon after the teleport

uint8_t* Guest(uint32_t a) { return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(a); }
uint32_t Read32(uint32_t a) {
  const uint8_t* p = Guest(a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Write32(uint32_t a, uint32_t v) {
  uint8_t* p = Guest(a);
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
float ReadFloat(uint32_t a) { const uint32_t b = Read32(a); float f; std::memcpy(&f, &b, 4); return f; }
void WriteFloat(uint32_t a, float f) { uint32_t b; std::memcpy(&b, &f, 4); Write32(a, b); }

struct Vec {
  float x = 0, y = 0, z = 0;
};
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
void WriteVec(uint32_t a, Vec v) { WriteFloat(a, v.x); WriteFloat(a + 4, v.y); WriteFloat(a + 8, v.z); }
bool Finite(Vec v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

uint32_t CallGame(void (*f)(PPCContext&, uint8_t*), PPCContext& ctx, uint8_t* base, uint64_t r3,
                  uint64_t r4 = 0, uint64_t r5 = 0, uint64_t r6 = 0) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = r3; ctx.r4.u64 = r4; ctx.r5.u64 = r5; ctx.r6.u64 = r6;
  f(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

bool InGame(int p) { return Read32(kPlayerStates + 4 * p) == 2; }
uint32_t BodyOf(PPCContext& ctx, uint8_t* base, int p) {
  return CallGame(sub_8226FC88, ctx, base, kGameObject, uint32_t(p));
}

// Guest scratch memory for the calls (game main thread only): a matrix (64
// bytes) and a message (up to 0xA0 bytes in the game's own frames).
uint32_t g_scratch = 0;
constexpr uint32_t kMatrix = 0, kMessage = 128, kScratchBytes = 512;
bool Scratch() {
  if (!g_scratch) g_scratch = rex::system::kernel_memory()->SystemHeapAlloc(kScratchBytes);
  return g_scratch != 0;
}

// --- the shot ---
int64_t g_armed_ms = -1;  // when a teleport moved player 3 or 4 (-1 = not armed)
bool g_in_shot = false;   // that shot is playing
bool CinematicPlaying() {
  const uint32_t game = Read32(kGameGlobal);
  const uint32_t manager = game ? Read32(game + kCinematicsOf) : 0;
  return manager && (*Guest(manager + kCinematicFlags) & 0xD0) != 0;
}

// Player 3 or 4's body moved to `spot`'s pose at `at` (section 1).
void Teleport(PPCContext& ctx, uint8_t* base, uint32_t action, uint32_t spot, uint32_t body, Vec at) {
  const uint32_t m = g_scratch + kMatrix, msg = g_scratch + kMessage;
  std::memset(Guest(g_scratch), 0, kScratchBytes);
  CallGame(__imp__sub_822FE2A8, ctx, base, spot, m);
  WriteVec(m + 48, at);
  CallGame(__imp__sub_8229E250, ctx, base, body, m + 48);
  CallGame(__imp__sub_820B7DF0, ctx, base, msg, m, 0, 0);
  uint8_t& flags = *Guest(msg + 0x54);
  flags = uint8_t(((*Guest(action + 0x18) >> 1) & 0x10) | 0x20 | (flags & 0xEF));
  CallGame(__imp__sub_820B0B70, ctx, base, body, msg);
  CallGame(__imp__sub_820B7F50, ctx, base, msg);
}

}  // namespace

void BeforeCameraRebuild(uint32_t camera) {
  if (g_armed_ms < 0) return;
  const bool playing = CinematicPlaying();
  if (!g_in_shot) {
    if (playing) {
      g_in_shot = true;
      REXLOG_INFO("Victory scenes: the shot is framed for every player");
    } else if (NowMs() - g_armed_ms > kArmedMs) {
      g_armed_ms = -1;  // no shot came
      return;
    } else {
      return;
    }
  } else if (!playing) {
    g_in_shot = false;
    g_armed_ms = -1;
    return;
  }
  const Vec pos = ReadVec(camera + kCamPosition);
  const Vec d = Normalized(ReadVec(camera + kCamDirection));
  const Vec r = Normalized(Cross(ReadVec(camera + kCamUp), d));
  const Vec u = Cross(d, r);
  const float fov_x = ReadFloat(camera + kCamFovX), aspect = ReadFloat(camera + kCamAspect);
  if (!Finite(pos) || !(fov_x > 0.1f && fov_x < 3.0f && aspect > 0.5f && aspect < 4.0f)) return;
  const float tan_x = std::tan(0.5f * fov_x), tan_y = tan_x / aspect;
  float back = 0.0f;
  for (int p = 0; p < kMaxPlayers; ++p) {
    if (!InGame(p)) continue;
    const uint32_t t = more_players::TitanOfPlayer(p);
    const uint32_t actor = t ? t : more_players::CharacterOfPlayer(p);
    const uint32_t matrix = actor ? Read32(actor + 44) : 0;  // world matrix, position at +48
    if (matrix < 0x40000000) continue;
    const Vec feet = ReadVec(matrix + 48);
    if (!Finite(feet)) continue;
    for (const Vec point : {feet, feet + Vec{0, t ? kTitanHeight : kCrashHeight, 0}}) {
      const Vec v = point - pos;
      const float x = Dot(v, r), y = Dot(v, u), z = Dot(v, d);
      back = std::max(back, std::fabs(x) / (kMarginX * tan_x) - z);
      back = std::max(back, (y > 0 ? y / (kMarginTop * tan_y) : -y / (kMarginBottom * tan_y)) - z);
    }
  }
  if (back > 0.0f && back < 60.0f) WriteVec(camera + kCamPosition, pos - d * back);
}

}  // namespace victory_scenes

using namespace victory_scenes;

extern "C" REX_FUNC(sub_8229E3C8) {
  const uint32_t action = ctx.r3.u32, first = ctx.r4.u32, second = ctx.r5.u32;
  __imp__sub_8229E3C8(ctx, base);
  REXLOG_INFO("Victory scenes: the players are moved to two spots ({:08X}, {:08X}); {} local players", first,
              second, more_players::LocalPlayerCount());
  if (!first || !second || more_players::LocalPlayerCount() <= 2 || !Scratch()) return;
  // Players 3-4 BETWEEN the two spots (2026-10-25, findings/26 s.48). The
  // first version put them 2.5-5 units BEYOND the spots along the line through
  // them; this same action moves the players at area doors / exits too, and
  // there the spots sit on a path or a ledge: player 4 was placed over a drop
  // ((-363.7, 17.0, -244.9), dead 33 units lower a second later). The spots
  // themselves are designer-placed on solid ground, so the stretch between
  // them is the safe place: player 3 at a third, player 4 at two thirds; when
  // player 2 isn't playing, their spot is free and player 4 takes it. Heights
  // follow the spots, 0.25 higher (they drop onto the ground).
  const Vec a = ReadVec(first + kSpotPosition), b = ReadVec(second + kSpotPosition);
  const bool second_free = !InGame(1);
  int moved = 0;
  for (int p = 2; p < kMaxPlayers; ++p) {
    if (!InGame(p)) continue;
    const uint32_t body = BodyOf(ctx, base, p);
    if (!body) {
      REXLOG_WARN("Victory scenes: player {} is in game but has no body: left where they are", p + 1);
      continue;
    }
    const bool beside_first = p == 2;
    const float along = p == 3 && second_free ? 1.0f : (p == 2 ? 1.0f / 3.0f : 2.0f / 3.0f);
    const Vec at = a + (b - a) * along + Vec{0, 0.25f, 0};
    Teleport(ctx, base, action, beside_first ? first : second, body, at);
    ++moved;
    REXLOG_INFO("Victory scenes: player {} placed at ({:.1f} {:.1f} {:.1f})", p + 1, at.x, at.y, at.z);
  }
  if (moved) {
    g_armed_ms = NowMs();
    g_in_shot = false;
  }
}

extern "C" REX_FUNC(sub_8229DE30) {
  const uint32_t action = ctx.r3.u32;
  const bool first_call = action && Read32(action + 0x14) >> 16 == 0;  // u16 state at +0x14
  const bool holds = action && (*Guest(action + 0x18) & 0x40) != 0;
  __imp__sub_8229DE30(ctx, base);
  if (!first_call || !holds || more_players::LocalPlayerCount() <= 2 || !Scratch()) return;
  // Section 2: players 3-4 get the "hold still" message too.
  const PPCContext saved = ctx;
  for (int p = 2; p < kMaxPlayers; ++p) {
    const uint32_t body = BodyOf(ctx, base, p);
    if (!body) continue;
    const uint32_t msg = g_scratch + kMessage, v = g_scratch + kMatrix;
    std::memset(Guest(g_scratch), 0, kScratchBytes);
    WriteVec(v, Vec{0, 1, 0});
    CallGame(__imp__sub_820B6340, ctx, base, msg, 8, v);
    CallGame(__imp__sub_820B0B70, ctx, base, body, msg);
    CallGame(__imp__sub_820B6508, ctx, base, msg);
  }
  ctx = saved;
}
