// =============================================================================
// gameplay/character_rate.cpp -- CHARACTERS AT 30 HZ, THE WORLD AT FULL RATE
// =============================================================================
//
// EXPERIMENT (2026-11-18): players, titans and enemies run
// their logic 30 times a second, exactly like the original console, while
// everything else (props, platforms, particles, the camera, menus) keeps
// running every frame at the screen's rate. Goal: the characters' physics
// (the slope trip, titans at edges: findings/22, 55) behaves as at 30, with
// the rest of the world as smooth as the frame rate allows.
//
// HOW THE GAME UPDATES AN ACTOR (read 2026-11-18 from the code + a profile):
//   CActorManager slot 10 sub_820B30D8(f1 = frame time; skips while paused)
//   -> zone manager sub_822E6EC0(*(uber+100), dt) -> each active zone
//      (sub_822F0100, active check sub_822EFFD8) -> its actor lists
//   -> each actor's BEHAVIOUR CONTROLLER (CBehaviourController, vtable
//      0x82029634; embedded in the actor at +0x40), which is itself a behaviour: its Update
//      (slot 4) = sub_82103680(this, actor, f1 dt, r6 phase) runs every
//      behaviour of that phase (sub_821037C8: 5 lists at +28, 16 bytes each,
//      begin +0 / end +4; each behaviour's slot 4 Update with dt).
//
// THE RULE (--character_rate=30; 0 = off): once per game frame (the actor
// manager's update: a frame counter) each CHARACTER (an actor whose root
// controller holds a CCharacterFightTreeBehaviour, vtable 0x8202C53C, also
// one controller deep) runs only in TICK frames (1/30 s of frame time piled
// up, one clock for all), all phases, with exactly 1/30
// s. Otherwise its update is skipped for that frame. Non-characters are
// untouched. (Since the press fix: one shared clock, the TICK, below.)
//
// OPEN QUESTIONS the test answers: does Crash visibly step against the
// smooth camera; are presses lost (input events last one frame); platforms
// moving every frame under a 30 Hz character; hits between 30 Hz and
// full-rate actors. Not for online as is (lockstep needs the whole world
// at a fixed rate).
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../guest_memory.h"

REXCVAR_DEFINE_INT32(character_rate, 0, "CrashMoM",
                     "EXPERIMENT: players, titans and enemies run their logic this many times a "
                     "second (30 = the original console), everything else every frame; 0 = off")
    .range(0, 240);
REXCVAR_DEFINE_BOOL(character_rate_smooth, true, "CrashMoM",
                    "With character_rate: show characters moving smoothly between their steps "
                    "(their drawn position follows the step before, ~33 ms late; the logic is untouched)");
REXCVAR_DEFINE_BOOL(debug_character_rate_trace, false, "CrashMoM",
                    "Debug (character_rate): log every held input event handed to a tick");
REXCVAR_DEFINE_BOOL(debug_character_rate_hold_presses, true, "CrashMoM",
                    "Debug (character_rate): hold input events over to the characters' next tick");

namespace {

constexpr uint32_t kFightTreeVtable = 0x8202C53C;     // CCharacterFightTreeBehaviour
constexpr uint32_t kControllerListsOffset = 28;      // 5 lists x 16 bytes (begin, end, ...)
constexpr uint32_t kControllerRunPhase = 0x821037C8;  // CBehaviourController slot 22
// The actor's root behaviour controller is EMBEDDED in the actor at +0x40
// (not a pointer: the word there is the controller's vtable; seen 2026-11-18,
// r4 = the actor, CActor vtable 0x820211A0, r3 = r4 + 0x40).
constexpr uint32_t kActorController = 0x40;

uint32_t Read32(uint8_t* base, uint32_t address) {
  const uint8_t* p = GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// Main thread only (the actor updates run there).
uint64_t g_frame = 0;  // game frames seen by the actor manager's update
// ONE CLOCK FOR ALL CHARACTERS (2026-11-18, with the press fix): the frame
// time piles up here; a frame where 1/rate s has piled up is a TICK, and
// every character runs in it (with exactly 1/rate s). Characters stay in
// step with each other (hits, jacking), and held presses (below) are handed
// out once per tick.
double g_account = 0;
bool g_tick = false;

// HELD PRESSES (2026-11-18: 1 jump out of 5 short taps). The input manager
// (sub_822745A0) updates every controller's input maps EVERY frame
// (sub_822710D8 -> sub_82275E20 per map), and an event ("jump pressed")
// is marked as happened (event +0x14, bit 0x80) for one frame only; the
// characters listen once a tick. So: after each PLAY map update the
// happened events are remembered; right before a tick they are set again
// (only where the bit is clear), and right after it the bits we set are
// taken back (the game's own rules never clear a bit they didn't set; the
// keyboard code met the same: key_events.cpp).
constexpr uint32_t kMapType = 0x18, kMapEvents = 0x1C, kEventBytes = 28, kEventState = 0x14;
std::unordered_map<uint32_t, std::vector<uint32_t>> g_held;  // map -> event addresses that happened since the last tick
std::vector<uint32_t> g_set_by_us;                           // event addresses whose bit we set for this tick
struct Character {
  uint64_t frame = 0;   // the frame its decision was made for
  bool run = false;     // ...run in that frame?
  bool character = false;
  bool valid = false;
  uint64_t checked = 0;  // frame its "is a character" answer was taken (re-checked now and then)
};
std::unordered_map<uint32_t, Character> g_actors;  // by actor address
// Counts for the log line (every ~5 s of frames).
uint64_t g_roots = 0, g_characters = 0, g_ran = 0, g_skipped = 0;

// Does this controller hold a character fight tree (itself or one
// controller below)?
bool HoldsFightTree(uint8_t* base, uint32_t controller, int depth) {
  if (!controller || !guest_memory::Readable(controller + kControllerListsOffset, 5 * 16)) return false;
  for (uint32_t list = 0; list < 5; ++list) {
    const uint32_t at = controller + kControllerListsOffset + list * 16;
    uint32_t begin = Read32(base, at), end = Read32(base, at + 4);
    if (end < begin || end - begin > 4 * 256 || !guest_memory::Readable(begin, end - begin)) continue;
    for (uint32_t p = begin; p < end; p += 4) {
      const uint32_t behaviour = Read32(base, p);
      if (!behaviour || !guest_memory::Readable(behaviour, 4)) continue;
      const uint32_t vtable = Read32(base, behaviour);
      if (vtable == kFightTreeVtable) return true;
      if (depth > 0 && guest_memory::Readable(vtable + 22 * 4, 4) &&
          Read32(base, vtable + 22 * 4) == kControllerRunPhase && HoldsFightTree(base, behaviour, depth - 1)) {
        return true;
      }
    }
  }
  return false;
}

// SMOOTH DISPLAY (2026-11-18, last piece of the day). A character's world
// matrix (actor +44 -> 16 floats, row-major, position at +48) is what the
// game draws it with and what the camera follows. Between two ticks it is
// set to a point between the tick before (A) and the latest tick (B): the
// picture runs one step (~33 ms) behind the logic, as in any interpolation.
// THE TRUTH COMES BACK FIRST: right before a tick, B is written back (only
// if the matrix still holds what we wrote: a script or cutscene that moved
// the character meanwhile wins). A step longer than kSnapUnits (teleport,
// respawn) snaps.
constexpr uint32_t kActorMatrix = 44;
constexpr float kSnapUnits = 5.0f;
struct Smooth {
  uint32_t matrix = 0;
  float a[16], b[16], wrote[16];
  bool have_a = false, have_b = false, wrote_valid = false;
};
std::unordered_map<uint32_t, Smooth> g_smooth;  // by actor (characters only)

float ReadF(uint8_t* base, uint32_t address) {
  const uint32_t bits = Read32(base, address);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
void WriteF(uint8_t* base, uint32_t address, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  uint8_t* p = GuestPtr(base, address);
  p[0] = uint8_t(bits >> 24); p[1] = uint8_t(bits >> 16); p[2] = uint8_t(bits >> 8); p[3] = uint8_t(bits);
}
void ReadMatrix(uint8_t* base, uint32_t m, float* out) {
  for (int i = 0; i < 16; ++i) out[i] = ReadF(base, m + 4 * uint32_t(i));
}
void WriteMatrix(uint8_t* base, uint32_t m, const float* v) {
  for (int i = 0; i < 16; ++i) WriteF(base, m + 4 * uint32_t(i), v[i]);
}
bool SameMatrix(const float* x, const float* y) { return std::memcmp(x, y, 16 * sizeof(float)) == 0; }

// The matrix at t between a and b: position on a line; each of the three
// axis rows mixed, then made square again (Gram-Schmidt) at its mixed length.
void MixMatrix(const float* a, const float* b, float t, float* out) {
  for (int i = 0; i < 16; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
  float rows[3][3], lengths[3];
  for (int r = 0; r < 3; ++r) {
    for (int k = 0; k < 3; ++k) rows[r][k] = out[r * 4 + k];
    lengths[r] = std::sqrt(rows[r][0] * rows[r][0] + rows[r][1] * rows[r][1] + rows[r][2] * rows[r][2]);
  }
  if (!(lengths[0] > 1e-6f && lengths[1] > 1e-6f && lengths[2] > 1e-6f)) return;
  auto unit = [](float* v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f) for (int k = 0; k < 3; ++k) v[k] /= l;
  };
  float u0[3] = {rows[0][0], rows[0][1], rows[0][2]};
  unit(u0);
  float u1[3] = {rows[1][0], rows[1][1], rows[1][2]};
  const float d01 = u1[0] * u0[0] + u1[1] * u0[1] + u1[2] * u0[2];
  for (int k = 0; k < 3; ++k) u1[k] -= d01 * u0[k];
  unit(u1);
  float u2[3] = {rows[2][0], rows[2][1], rows[2][2]};
  const float d02 = u2[0] * u0[0] + u2[1] * u0[1] + u2[2] * u0[2];
  const float d12 = u2[0] * u1[0] + u2[1] * u1[1] + u2[2] * u1[2];
  for (int k = 0; k < 3; ++k) u2[k] -= d02 * u0[k] + d12 * u1[k];
  unit(u2);
  const float* units[3] = {u0, u1, u2};
  for (int r = 0; r < 3; ++r) {
    for (int k = 0; k < 3; ++k) out[r * 4 + k] = units[r][k] * lengths[r];
  }
}

bool SmoothOn() { return REXCVAR_GET(character_rate_smooth); }

// Before a tick: the truth back (where nobody else moved the character).
void RestoreTruth(uint8_t* base) {
  for (auto& [actor, sm] : g_smooth) {
    if (!sm.have_b || !sm.wrote_valid || !guest_memory::Readable(sm.matrix, 64)) continue;
    float now[16];
    ReadMatrix(base, sm.matrix, now);
    if (SameMatrix(now, sm.wrote)) WriteMatrix(base, sm.matrix, sm.b);
    sm.wrote_valid = false;
  }
}

// After a tick: the new truth becomes B (the old B becomes A).
void TakeNewTruth(uint8_t* base) {
  for (auto& [actor, c] : g_actors) {
    if (!c.character || c.frame != g_frame || !c.run) continue;  // (ran in this tick)
    if (!guest_memory::Readable(actor + kActorMatrix, 4)) continue;
    const uint32_t m = Read32(base, actor + kActorMatrix);
    if (!m || !guest_memory::Readable(m, 64)) continue;
    Smooth& sm = g_smooth[actor];
    if (sm.matrix != m) sm = Smooth{}, sm.matrix = m;
    if (sm.have_b) std::memcpy(sm.a, sm.b, sizeof(sm.a));
    sm.have_a = sm.have_b;
    ReadMatrix(base, m, sm.b);
    sm.have_b = true;
    const float dx = sm.b[12] - sm.a[12], dy = sm.b[13] - sm.a[13], dz = sm.b[14] - sm.a[14];
    if (sm.have_a && dx * dx + dy * dy + dz * dz > kSnapUnits * kSnapUnits) {
      std::memcpy(sm.a, sm.b, sizeof(sm.a));  // a jump: snap
    }
  }
}

// Between ticks (and right after one, t = 0): show the point at t.
void ShowAt(uint8_t* base, float t) {
  for (auto& [actor, sm] : g_smooth) {
    if (!sm.have_a || !sm.have_b || !guest_memory::Readable(sm.matrix, 64)) continue;
    float now[16];
    ReadMatrix(base, sm.matrix, now);
    // Someone else moved it since we last wrote (or since the tick): theirs.
    const float* expected = sm.wrote_valid ? sm.wrote : sm.b;
    if (!SameMatrix(now, expected)) {
      std::memcpy(sm.a, now, sizeof(sm.a));
      std::memcpy(sm.b, now, sizeof(sm.b));
      sm.wrote_valid = false;
      continue;
    }
    float mixed[16];
    MixMatrix(sm.a, sm.b, t, mixed);
    WriteMatrix(base, sm.matrix, mixed);
    std::memcpy(sm.wrote, mixed, sizeof(sm.wrote));
    sm.wrote_valid = true;
  }
}

}  // namespace

namespace character_rate {
// After an input map's update (key_events.cpp's wrapper of sub_82275E20,
// keyboard events included): remember its happened PLAY events (HELD PRESSES).
void AfterMapUpdate(uint8_t* base, uint32_t map) {
  // (No guest_memory::Readable here: the game has just updated this map, so
  // its memory is live; the check on every map every frame cost ~1/3 of the
  // frame rate, 180 -> 125 fps, in the first build.)
  if (REXCVAR_GET(character_rate) <= 0 || !map || !REXCVAR_GET(debug_character_rate_hold_presses)) return;
  const int32_t type = int32_t(Read32(base, map + kMapType));
  if (type == 1 || type == 2 || type == 3) return;  // menus, movies, cheats: every frame as before
  const uint32_t vec = Read32(base, map + kMapEvents);
  if (!vec) return;
  const uint32_t begin = Read32(base, vec), end = Read32(base, vec + 4);
  if (end < begin || (end - begin) % kEventBytes || end - begin > kEventBytes * 256) return;
  for (uint32_t e = begin; e < end; e += kEventBytes) {
    if (*GuestPtr(base, e + kEventState) & 0x80) {
      std::vector<uint32_t>& held = g_held[map];
      if (std::find(held.begin(), held.end(), e) == held.end()) held.push_back(e);
    }
  }
}
}  // namespace character_rate

// The actor manager's per-frame update (slot 10): one game frame.
extern "C" REX_FUNC(__imp__sub_820B30D8);
extern "C" REX_FUNC(sub_820B30D8) {
  ++g_frame;
  if (g_actors.size() > 4096) g_actors.clear();  // (dead actors: start over now and then)
  const int32_t rate = REXCVAR_GET(character_rate);
  if (rate <= 0) {
    g_held.clear();
    if (!g_smooth.empty()) {
      RestoreTruth(base);
      g_smooth.clear();
    }
    return __imp__sub_820B30D8(ctx, base);
  }
  if (g_frame % 900 == 0) {
    uint32_t characters = 0;
    for (const auto& [actor, c] : g_actors) characters += c.character;
    REXLOG_INFO("Character rate: {} actors known, {} characters; last 900 frames: {} root updates, {} of "
                "characters: {} ran, {} skipped",
                g_actors.size(), characters, g_roots, g_characters, g_ran, g_skipped);
    g_roots = g_characters = g_ran = g_skipped = 0;
  }
  // The tick (ONE CLOCK FOR ALL CHARACTERS).
  const double step = 1.0 / double(rate);
  if (ctx.f1.f64 > 0) g_account += ctx.f1.f64;
  if (g_account > 4 * step) g_account = 4 * step;  // (a long hitch: no burst of catch-up)
  g_tick = g_account >= step - 1e-6;
  if (g_tick) g_account -= step;
  // SMOOTH DISPLAY: the truth back before a tick; between ticks, the
  // characters shown part of the way from the step before to the latest
  // (before the update: the camera, updated in it, follows that point).
  if (SmoothOn()) {
    if (g_tick) {
      RestoreTruth(base);
    } else {
      ShowAt(base, float(std::clamp(g_account / step, 0.0, 1.0)));
    }
  } else if (!g_smooth.empty()) {
    RestoreTruth(base);
    g_smooth.clear();
  }
  // HELD PRESSES: set again before the tick...
  g_set_by_us.clear();
  if (g_tick) {
    for (const auto& [map, events] : g_held) {
      for (uint32_t e : events) {
        if (!guest_memory::Readable(e + kEventState, 1)) continue;
        uint8_t* state = GuestPtr(base, e + kEventState);
        if (!(*state & 0x80)) {
          *state |= 0x80;
          g_set_by_us.push_back(e);
          if (REXCVAR_GET(debug_character_rate_trace)) {
            const uint32_t vec = Read32(base, map + kMapEvents);
            REXLOG_INFO("Character rate: frame {} tick: held event map {:08X} type {} index {}", g_frame, map,
                        int32_t(Read32(base, map + kMapType)), (e - Read32(base, vec)) / kEventBytes);
          }
        }
      }
    }
    g_held.clear();
  }
  __imp__sub_820B30D8(ctx, base);
  // ...and taken back after it.
  for (uint32_t e : g_set_by_us) {
    if (guest_memory::Readable(e + kEventState, 1)) *GuestPtr(base, e + kEventState) &= 0x7F;
  }
  g_set_by_us.clear();
  // (SMOOTH DISPLAY's new truth is taken after the COLLISION update, below:
  // that's where a character's step reaches its matrix.)
}

// CBehaviourController::Update(this, actor, dt, phase).
extern "C" REX_FUNC(__imp__sub_82103680);
extern "C" REX_FUNC(sub_82103680) {
  const int32_t rate = REXCVAR_GET(character_rate);
  if (rate <= 0) return __imp__sub_82103680(ctx, base);
  const uint32_t controller = ctx.r3.u32, actor = ctx.r4.u32;
  if (!actor) return __imp__sub_82103680(ctx, base);
  // (Hot path: this runs for every controller of every actor, several
  // phases a frame. guest_memory::Readable is slow: only for an actor's
  // first time; the first build checked it every call = the game at 37 fps.)
  Character& c = g_actors[actor];
  if (c.checked == 0 || g_frame - c.checked > 300) {  // (re-checked every ~300 frames: addresses get reused)
    c.valid = guest_memory::Readable(actor + kActorController, 4);
    const bool was = c.character;
    c.character = c.valid && HoldsFightTree(base, actor + kActorController, 1);
    if (c.character && !was) REXLOG_INFO("Character rate: actor {:08X} runs at the character rate", actor);
    c.checked = g_frame;
  }
  const bool root = controller == actor + kActorController;
  g_roots += root;
  if (!c.character || !root) {
    return __imp__sub_82103680(ctx, base);  // not a character, or not its root controller
  }
  ++g_characters;
  const double step = 1.0 / double(rate);
  if (c.frame != g_frame) {  // (one decision a frame: the shared tick)
    c.frame = g_frame;
    c.run = g_tick;
  }
  if (!c.run) {
    ++g_skipped;
    return;  // not this frame
  }
  ++g_ran;
  ctx.f1.f64 = double(float(step));
  __imp__sub_82103680(ctx, base);
}

// The collision manager's per-frame update (CCollisionManager slot 10). It
// runs AFTER the actor manager in a frame (both called from the frame
// function sub_8227C5D8), and a character's step reaches its world matrix
// here: the sweep's transform command (sub_8217E850) writes it. So SMOOTH
// DISPLAY takes the tick's new truth after this, and shows the step before
// (t = 0). (First try read it after the actor update: the old position;
// the collision step then overwrote the picture, read as "moved by someone
// else", and the smoothing gave up.)
extern "C" REX_FUNC(__imp__sub_82249948);
extern "C" REX_FUNC(sub_82249948) {
  __imp__sub_82249948(ctx, base);
  if (REXCVAR_GET(character_rate) > 0 && SmoothOn() && g_tick) {
    TakeNewTruth(base);
    // (Not t = 0: the frame time left over past the tick counts already;
    // t = 0 gave a short step on every tick frame and a long one after = a
    // hitch 30 times a second.)
    const int32_t rate = REXCVAR_GET(character_rate);
    ShowAt(base, float(std::clamp(g_account * double(rate), 0.0, 1.0)));
  }
}

// A CHARACTER'S COLLISION AT ITS OWN RATE (2026-11-18: with the rule on,
// Crash's forward speed in a jump fell from 7.7 to 2.5 and he came to a
// halt). The collision system (CCollisionManager slot 10 sub_82249948, per
// frame) sweeps every collision object with sub_822543E0(object); a
// contact goes to the actor's physics (message 9 -> sub_821809C8 -> the
// collision response sub_8217F068, which SETS the velocity, trimming what
// points into the surface). Caught with `who` on Crash's velocity: set 81
// times a second by that path while his own update ran 30 times: each 30 Hz
// step was swept and trimmed six times at 180 fps. Now a character's object
// (object +8 = its actor) is swept only while the characters' latest
// decision was a TICK: right after the tick's physics if collision runs
// after the actors in a frame, else in the frame after: one sweep per step
// either way. (The caller ignores the return value.)
extern "C" REX_FUNC(__imp__sub_822543E0);
extern "C" REX_FUNC(sub_822543E0) {
  if (REXCVAR_GET(character_rate) > 0 && !g_tick) {
    const uint32_t actor = Read32(base, ctx.r3.u32 + 8);
    const auto it = g_actors.find(actor);
    if (it != g_actors.end() && it->second.character) return;  // not this frame
  }
  __imp__sub_822543E0(ctx, base);
}
