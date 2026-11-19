// =============================================================================
// ground_physics.cpp -- Crash "falling" for a split second above 30 fps
//                       (--ground_grace_ms=40 by default; --ground_triangle_handover, at the
//                       end of this file: bumpy slopes, findings/22 s.11; --debug_ground_trace)
// =============================================================================
//
// THE BUG (2026-10-01, docs/findings/22): at 60 fps Crash briefly plays his
// FALLING animation at spots of ordinary walkable ground, every time he steps
// down a stair-like descent (first seen near the entrance of the Ratcicle
// Kingdom); at the original 30 fps the same spots are fine.
//
// WHO DECIDES "FALLING": Crash's FIGHT TREE, the logic that picks his moves
// and animations. Its source ships as Lua (fighttrees/Crashlua in
// default.rcf: Lua 5.0 instructions in Radical's own file layout, decoded
// from the game's loader, LoadFunction 0x8241AEA8), but on the Xbox 360 the
// same tree is COMPILED INTO THE GAME: the Lua binding of IsOnGround is
// never called while Crash walks (the Lua copy, with its Wii motion
// checks, is for other platforms). The walking/running state is
// sub_821A0DB0 (the same branch numbers as the Lua: 317, 321, ..., 558, -1);
// in Lua, script line ~2409:
//     if not IsOnGround(physics) and not IsTeleporting(physics) then
//       if IsWalkingOffCliff(ledge) and GetSquareOfStickExtent(self) < 0.81 then
//         return 522      -- walking slowly off an edge
//       end
//       return 523        -- FALL
//     end
// (compiled: IsOnGround called at 0x821A121C, IsTeleporting = this+242 bit
// 2 inline, IsWalkingOffCliff 0x82152270, stick extent 0x8221E540.) No
// counting, no timer: ONE "not on ground" answer starts the fall.
//
// THE GAME'S GROUND PHYSICS (static reading with xexdis + this file's trace)
//   Every moving character has a CPhysicsBehaviour (RTTI; vtable 0x82032FC4).
//   Its state lives in the character's VARIABLES ("blackboard": actor = this
//   +88, variable N's storage = *(actor+32)[N], or a static table for one
//   special actor; lookup sub_821830E8). The behaviour keeps the variable
//   INDICES in bytes: +39 = the GROUND contact (variable ID 10, a pointer to
//   the collision object the character stands on, 0 = none), +40 = a
//   steep-ground contact (ID 11). IsOnGround = sub_82182A88 ("ground variable
//   != 0"), reached through the stub sub_82182B18 from ~150 call sites:
//   the physics itself (inside CPhysicsBehaviour's code, 0x8217B000-
//   0x82184000), the compiled fight trees of every character (0x8219E000
//   onwards), and other game code; Lua scripts could too (registered at
//   0x82181A4C, binding 0x821845B0), but none do here.
//
//   Per frame, CPhysicsBehaviour::Update = vtable slot 4 = sub_8217D0A0
//   (f1 = the frame's time step):
//     1. sub_821805F8 integrates: gravity every frame (sub_82182A00:
//        vy += this+92 (-60 units/s^2 for Crash, -20 for the others seen) x
//        dt, clamped at this+96), air drag (sub_82180748, in fixed 1/60 s
//        steps) only off the ground or on steep ground; the physics position
//        (this+140/144/148) moves by velocity (this+104/108/112) x dt.
//     2. The move goes on as a message: the collision code sweeps the
//        character through the world and reports what it touched through
//        slot 5 (sub_8217DCF8 -> sub_8217E2A8 -> sub_821809C8), which sets
//        the ground (or steep) contact: SetContact = sub_82182400(this,
//        k = 0 ground / 1 steep, object) (calls at 0x82180DBC / 0x82180E64).
//     3. sub_8217D008(this, k) checks each contact every frame: the SAME
//        collision triangle must still be within reach of the character's
//        collision ellipsoid (sub_8217D668 -> sub_8224E380; reach = a fixed
//        distance, 0.25 x an ellipsoid size); if not, the contact is cleared
//        (call at 0x8217D090, return address 0x8217D094).
//   A jump drops the contact itself (the fight tree's ForceOffGround: SetContact
//   called from 0x82182CFC, return address 0x82182D00).
//
// WHY ONLY ABOVE 30 FPS: one frame is, in this order, the fight tree asking
// IsOnGround, then the physics update (whose reach check may drop the
// contact), then the collision sweep (which may find ground again). So the
// tree only hears about a lost contact that is STILL lost after the sweep of
// the frame that lost it. Stepping down a small ledge, Crash loses the ground
// and lands on the next step a moment later, about as often at 30 fps as at
// 60 (the same descent in both). At 30 fps a 33 ms frame holds most of those
// drops: lost and found again within one frame, the tree never knows. At 60
// fps (17 ms frames) the same drop spans two frames, the tree asks in
// between, hears "no" and starts the fall. Measured (trace with every
// IsOnGround call, 60 fps, ms from the loss):
//     -0.4  tree: on ground     +0.0  reach check drops the contact
//    +15.3  tree: NOT on ground -> fall      +16.5  sweep: ground again
//
// WHY IT STAYS ON (2026-11-19, findings/69 s.5): slopes are fixed at the source
// by ground_triangle_handover (end of this file), but REAL STEPS are not: on
// the test course's stairs, with the grace off, walking down 0.25-unit steps at
// 180 fps loses the ground 21 times for one frame each, at 30 fps never. The
// grace hides those until a step-down fix at the source exists.
//
// THE FIX (--ground_grace_ms, default 40, 0 = off; nothing changes at
// --fps_cap=30): for up to that long (see GraceTime) after the reach
// check drops a character's ground contact, every caller of IsOnGround
// OUTSIDE the physics' own code still hears "yes". A drop that lands within
// that time never reaches the fight trees (or anything else), as at 30 fps; a
// real fall (an edge, a cliff) starts once it has lasted a little longer than
// a frame of the original game. The physics itself (gravity, air drag, sliding, the reach
// check) keeps the real answer, and jumps (ForceOffGround) get no grace. A first attempt (2026-10-01) changed
// gravity instead (its 1/30 s step on the ground, to press characters onto
// uneven ground as hard as at 30 fps): it made Crash slide on steep ground
// (the steep-ground slide accelerates with that gravity) and didn't stop the
// falls, because the drops themselves were never the difference. Removed.
//
// THE TRACE (--debug_ground_trace=<file.csv>, tools/play.sh --ground-trace).
// Our strong definitions of the generated weak sub_X (like audio_trace.cpp)
// wrap three functions:
//   sub_8217D0A0  CPhysicsBehaviour::Update  -> one "U" line per update of a
//                 character that moved or whose contact changed
//   sub_82182400  SetContact                 -> one "C" line per change
//   sub_82182B18  IsOnGround (stub)          -> one "S" line per call from
//                 outside the physics answered "not on ground" by it
// CSV, flushed about once a second (closing the window ends the process at
// once):
//   U,ms,actor,dt,ground_before,ground_after,steep_after,x,y,z,
//     vy_before,vx,vy,vz
//   C,ms,actor,k,new_object,old_object,caller
//   S,ms,actor,answer_given,ms_since_contact_lost,lost_by,caller
//   G,ms,actor,gravity,max_fall_speed      (once per actor, first seen)
//   R,ms,actor,caller,record,nx,ny,nz,e,vx,vy,vz (before),vx,vy,vz (after)
//                                          (one collision response, s.10)
//   H,ms,actor,object,old_triangle,kept_triangle (-1 none),tried,skipped,nx,ny,nz
//                                          (one triangle handover, s.11)
// MOVING PLATFORMS ABOVE 30 FPS (2026-10-16, docs/findings/22 s.9): a
// separate bug, a soft-lock. The space station level's round arena has a
// floor of rings that SINK at 0.5 units/s while enemies attack; at 180 fps
// they moved a few centimetres and stopped (player stuck on the top ring).
// The rings follow a path: CPathFollowingBehaviour (vtable 0x8202FA1C,
// Update = slot 4 sub_82158830, f1 = frame time). Each frame its step
// sub_82159428 counts the path's remaining distance down by speed x dt and
// hands the step to sub_82159618, which SENDS it to the object only if
//     |step|^2 > (game speed)^2 x 1.6667e-05    (constant 0x820472F0,
//                                               compare at 0x82159670)
// = 0.0041 units. At 30 fps the floor steps 0.5 / 30 = 0.0167: sent. At 180
// it steps 0.0028: DROPPED, though the distance left was already counted
// down, so the floor loses that movement for good and never reaches the
// end of its path. It only moved on frames slower than ~8 ms, rarer and
// rarer as the fight emptied out and the frame rate steadied.
// The step then reaches the object's CPhysicsBehaviour as a "move by"
// message, and its handler sub_8217E6B0 checks THE SAME minimum again
// (|step|^2 > (game speed)^2 x 1.6667e-05, compare at 0x8217E72C) before
// adding it to the position: dropped again at 180 fps. These two are the
// only reads of the constant.
// The same kind of minimum sits one step later: the physics' transform
// handler sub_8217E850 moves the world matrix (and tells the riders) only
// when |matrix - new position|^2 > 1e-05 (0.0032 units, constant
// 0x82020020, compare at 0x8217E984). There the step isn't lost (the gap
// grows until it passes), but the object moves in jerky every-other-frame
// hops above ~160 fps.
// HOW FOUND: replays of a playtest's recording (input_record.h) with Native
// Only at 30 vs 180 (the emulated GPU can't reach 180, which hid it); the
// console's snap + diff (diffs can now go to a file) found the rings (x -190,
// z 157); temporary probes on the physics position writers gave the call
// stack into the path follower; Ghidra pseudo-C + xexdis showed the minima.
// THE FIX: both minima scaled by (frame time x 30)^2 when a frame is shorter
// than 1/30 s, so each is the same SPEED the original had at 30 fps:
//   CrashMomPathMoveMinimum (0x82159670, f8): frame time from the path
//     follower's Update (wrapped below, kept while it runs);
//   CrashMomMoveByMinimum (0x8217E72C, f7): the same frame time when the
//     step comes from a path follower, else the physics' +0xA4;
//   CrashMomPlatformMoveMinimum (0x8217E984, f0): frame time = the physics'
//     +0xA4 (stored by this frame's Update).
// --platform_move_fix=false turns all three off (A/B tests).
//
// ms = since the trace started; actor = the CPhysicsBehaviour's guest
// address (Crash = the one with gravity -60 that moves when the player
// walks); x,y,z = the physics position (y up); objects are guest addresses
// (0 = no contact; one big object holds a whole area's ground, so walking
// from triangle to triangle doesn't show); caller / lost_by = return address
// of the SetContact call (0x82180DC0 sweep: ground, 0x82180E68 sweep: steep,
// 0x8217D094 the reach check, 0x82182D00 ForceOffGround); answer_given 1 =
// the grace said "on ground" (the fix at work), 0 = the caller heard "no";
// S caller = IsOnGround's return address (0x821A1220 = Crash's walking
// state).
// =============================================================================

#include <array>
#include <cmath>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include "fixed_step.h"  // logic_rate
#include "guest_memory.h"

REXCVAR_DECLARE(int32_t, fps_cap);  // frame_rate.cpp

REXCVAR_DEFINE_INT32(ground_grace_ms, 40, "CrashMoM",
                     "Above 30 fps: a ground contact lost for less than this many ms isn't "
                     "reported to the game's move logic, so Crash doesn't 'fall' for a split "
                     "second stepping down; 0 = off (src/ground_physics.cpp)");
REXCVAR_DEFINE_BOOL(platform_move_fix, true, "CrashMoM",
                    "Above 30 fps: moving floors and platforms (e.g. the space station's sinking "
                    "arena) keep moving; the game drops steps under a fixed size, which every "
                    "step is at high fps (src/ground_physics.cpp)");
REXCVAR_DEFINE_BOOL(ground_triangle_handover, true, "CrashMoM",
                    "Above 30 fps: walking over uneven ground, the ground contact moves on to the next "
                    "triangle instead of being dropped, so Crash doesn't 'fall' on bumpy slopes "
                    "(findings/22 s.11; src/ground_physics.cpp)");
REXCVAR_DEFINE_BOOL(ground_touch_keeps_contact, true, "CrashMoM",
                    "Above 30 fps: a ground contact the collision sweep has just touched is kept even when "
                    "the reach check calls it out of reach, so Crash doesn't 'fall' riding over a step's edge "
                    "on stairs (findings/22 s.12; src/ground_physics.cpp)");
REXCVAR_DEFINE_STRING(debug_ground_trace, "", "CrashMoM",
                      "Debug: write ground contact changes, physics updates of moving "
                      "characters and the 'not on ground' answers the game logic gets to this CSV file "
                      "(src/ground_physics.cpp)");

namespace ground_physics {
namespace {

using Clock = std::chrono::steady_clock;

// CPhysicsBehaviour fields (see the header comment).
constexpr uint32_t kActor = 88;         // u32: the character (owner of the variables)
constexpr uint32_t kGroundIndex = 39;   // u8: variable index of the ground contact
constexpr uint32_t kSteepIndex = 40;    // u8: variable index of the steep contact
constexpr uint32_t kGravity = 92;       // float, units/s^2 (negative = down)
constexpr uint32_t kMaxFallSpeed = 96;  // float, units/s (negative)
constexpr uint32_t kVelocity = 104;     // 3 floats, units/s (x, y = up, z)
constexpr uint32_t kPosition = 140;     // 3 floats: the physics position
// Actor fields.
constexpr uint32_t kActorVariables = 32;  // u32: array of variable storage pointers
// sub_821830E8: one actor uses this static variable table instead.
constexpr uint32_t kSpecialActorGlobal = 0x8259ACE0;
constexpr uint32_t kSpecialVariables = 0x82597B30;

// The physics' own code (CPhysicsBehaviour's methods and helpers): its
// IsOnGround calls keep the real answer.
constexpr uint32_t kPhysicsCodeStart = 0x8217B000, kPhysicsCodeEnd = 0x82184000;
// SetContact's return address when the reach check clears a contact
// (sub_8217D008).
constexpr uint32_t kFromReachCheck = 0x8217D094;

// How long a lost ground contact stays hidden from the game logic
// (--ground_grace_ms). At 60 fps the tree asks ~16, ~33 and ~50 ms after a
// loss. Playtest traces on the stair-like descent (60 fps, 2026-10-01):
//   grace  0: every loss of 10+ ms heard (48 of 63)
//   grace 30: losses up to 25 ms hidden; the 33-34 ms ones (two 60 fps
//             frames) still heard, at 32-34 ms: the grace ran out 2-4 ms
//             before the contact came back -> falls still seen sometimes
//   grace 50: everything under 42 ms hidden, but 50 sits right on the
//             third check (~50 ms): heard at 50 or not, by frame jitter
// Default 40 = halfway between the second and third check at 60 fps: drops
// of up to two 60 fps frames are hidden, real drops (an edge, ~50 ms and
// more) still start the fall within 3 frames. At 144 fps (checks every
// 7 ms) the same 40 ms applies. Real time, not the physics' step sum: the
// tree asks BEFORE the frame's physics update, so a step sum would lag one
// frame behind.
Clock::duration GraceTime() { return std::chrono::milliseconds(REXCVAR_GET(ground_grace_ms)); }

uint32_t Be32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, GuestPtr(base, address), 4);
  return __builtin_bswap32(v);
}
float BeFloat(const uint8_t* base, uint32_t address) {
  return std::bit_cast<float>(Be32(base, address));
}

// Variable `index`'s storage address for the actor, as sub_821830E8 finds it.
uint32_t VariableAddress(const uint8_t* base, uint32_t actor, uint32_t index) {
  const uint32_t table = actor == Be32(base, kSpecialActorGlobal)
                             ? kSpecialVariables
                             : Be32(base, actor + kActorVariables);
  return table ? Be32(base, table + 4 * index) : 0;
}

// The contact object in variable `index_field` (kGroundIndex / kSteepIndex).
uint32_t Contact(const uint8_t* base, uint32_t self, uint32_t index_field) {
  const uint32_t actor = Be32(base, self + kActor);
  if (!actor) {
    return 0;
  }
  const uint32_t storage = VariableAddress(base, actor, (*GuestPtr(base, self + index_field)));
  return storage ? Be32(base, storage) : 0;
}

// --- the grace ---------------------------------------------------------------

// Per CPhysicsBehaviour: when and how its ground contact was last lost
// (kept until the contact is back).
struct Loss {
  Clock::time_point when;
  uint32_t by = 0;  // SetContact's return address (kFromReachCheck = the physics)
};
std::mutex g_loss_mutex;
std::unordered_map<uint32_t, Loss> g_losses;

// Off at the original's 30 fps: the frame-rate cap at 30, or the game logic
// at the console's fixed 30 (logic_rate 30: frames of exactly 1/30 s, the
// original's physics, whatever the cap says).
bool GraceOn() {
  return REXCVAR_GET(ground_grace_ms) > 0 && REXCVAR_GET(fps_cap) != 30 && REXCVAR_GET(logic_rate) != 30;
}

// --- the trace ---------------------------------------------------------------

struct Trace {
  std::mutex mutex;
  std::FILE* file = nullptr;
  Clock::time_point start, last_flush;
  // Per actor: the physics position at its last update ("G" line written
  // when first seen).
  std::unordered_map<uint32_t, std::array<float, 3>> last_position;
};
Trace g_trace;

// 0 = not decided yet, 1 = off, 2 = on (decided on the first call: the
// flag is parsed long before the game's code runs).
std::atomic<int> g_trace_mode{0};

bool TraceOn() {
  int mode = g_trace_mode.load(std::memory_order_acquire);
  if (mode == 0) {
    std::lock_guard lock(g_trace.mutex);
    mode = g_trace_mode.load(std::memory_order_relaxed);
    if (mode == 0) {
      const std::string& path = REXCVAR_GET(debug_ground_trace);
      if (!path.empty()) {
        g_trace.file = std::fopen(path.c_str(), "w");
        if (g_trace.file) {
          std::fprintf(g_trace.file,
                       "# ground trace (src/ground_physics.cpp), --ground_grace_ms=%d, "
                       "--fps_cap=%d\n"
                       "# U,ms,actor,dt,ground_before,ground_after,steep_after,x,y,z,"
                       "vy_before,vx,vy,vz\n"
                       "# C,ms,actor,k,new_object,old_object,caller\n"
                       "# S,ms,actor,answer_given,ms_since_contact_lost,lost_by,caller\n"
                       "# G,ms,actor,gravity,max_fall_speed\n"
                       "# R,ms,actor,caller,record,nx,ny,nz,e,vx,vy,vz,vx_after,vy_after,vz_after\n"
                       "# H,ms,actor,object,old_triangle,kept_triangle,tried,skipped,nx,ny,nz\n"
                       "# K,ms,actor,object,triangle (kept: the sweep touched ground within 1/30 s)\n",
                       REXCVAR_GET(ground_grace_ms), REXCVAR_GET(fps_cap));
          g_trace.start = g_trace.last_flush = Clock::now();
          REXLOG_INFO("ground_physics: writing the ground trace to {}", path);
        } else {
          REXLOG_WARN("ground_physics: can't write {}", path);
        }
      }
      mode = g_trace.file ? 2 : 1;
      g_trace_mode.store(mode, std::memory_order_release);
    }
  }
  return mode == 2;
}

double TraceMs(Clock::time_point t = Clock::now()) {
  return std::chrono::duration<double, std::milli>(t - g_trace.start).count();
}

// Caller holds g_trace.mutex.
void FlushNowAndThen() {
  const auto now = Clock::now();
  if (now - g_trace.last_flush > std::chrono::seconds(1)) {
    std::fflush(g_trace.file);
    g_trace.last_flush = now;
  }
}

}  // namespace
}  // namespace ground_physics

// -----------------------------------------------------------------------------
// The wrapped game functions (strong definitions of the generated weak ones)
// -----------------------------------------------------------------------------

// IsOnGround's stub (`b sub_82182A88`): r3 = this (CPhysicsBehaviour),
// returns r3 = 1 on the ground. Callers outside the physics' own code get
// the grace (and a trace line when the physics says "no").
extern "C" REX_FUNC(__imp__sub_82182B18);
extern "C" REX_FUNC(sub_82182B18) {
  using namespace ground_physics;
  const uint32_t self = ctx.r3.u32;
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  const bool from_physics = caller >= kPhysicsCodeStart && caller < kPhysicsCodeEnd;
  __imp__sub_82182B18(ctx, base);
  if (from_physics || ctx.r3.u32 != 0) {
    return;
  }
  const bool grace_on = GraceOn();
  const bool tracing = TraceOn();
  if (!grace_on && !tracing) {
    return;
  }
  Loss loss;
  bool known = false;
  {
    std::lock_guard lock(g_loss_mutex);
    const auto it = g_losses.find(self);
    if (it != g_losses.end()) {
      loss = it->second;
      known = true;
    }
  }
  const auto now = Clock::now();
  if (grace_on && known && loss.by == kFromReachCheck && now - loss.when < GraceTime()) {
    ctx.r3.u64 = 1;  // still "on the ground" for the fight tree (and the rest)
  }
  if (tracing) {
    std::lock_guard lock(g_trace.mutex);
    std::fprintf(g_trace.file, "S,%.3f,%08X,%u,%.1f,%08X,%08X\n", TraceMs(now), self,
                 ctx.r3.u32,
                 known ? std::chrono::duration<double, std::milli>(now - loss.when).count() : -1.0,
                 known ? loss.by : 0, caller);
    FlushNowAndThen();
  }
}

// CPhysicsBehaviour::Update (vtable slot 4): r3 = this, f1 = time step.
// Wrapped only for the trace.
extern "C" REX_FUNC(__imp__sub_8217D0A0);
extern "C" REX_FUNC(sub_8217D0A0) {
  using namespace ground_physics;
  if (!TraceOn()) {
    __imp__sub_8217D0A0(ctx, base);
    return;
  }
  // Read everything before the call: it changes the registers.
  const uint32_t self = ctx.r3.u32;
  const double dt = ctx.f1.f64;
  const uint32_t ground_before = Contact(base, self, kGroundIndex);
  const float vy_before = BeFloat(base, self + kVelocity + 4);
  __imp__sub_8217D0A0(ctx, base);

  const uint32_t ground_after = Contact(base, self, kGroundIndex);
  std::array<float, 3> position, velocity;
  for (int i = 0; i < 3; ++i) {
    position[i] = BeFloat(base, self + kPosition + 4 * i);
    velocity[i] = BeFloat(base, self + kVelocity + 4 * i);
  }
  std::lock_guard lock(g_trace.mutex);
  const auto [last, first_seen] = g_trace.last_position.try_emplace(self, position);
  const bool moved = last->second != position;
  last->second = position;
  if (first_seen) {
    std::fprintf(g_trace.file, "G,%.3f,%08X,%.4f,%.4f\n", TraceMs(), self,
                 BeFloat(base, self + kGravity), BeFloat(base, self + kMaxFallSpeed));
  }
  // Standing still with nothing changing: not interesting (most characters).
  if (!moved && !first_seen && ground_before == ground_after) {
    return;
  }
  std::fprintf(g_trace.file,
               "U,%.3f,%08X,%.6f,%08X,%08X,%08X,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n", TraceMs(),
               self, dt, ground_before, ground_after, Contact(base, self, kSteepIndex),
               position[0], position[1], position[2], vy_before, velocity[0], velocity[1],
               velocity[2]);
  FlushNowAndThen();
}

// The COLLISION RESPONSE (sub_8217F068, findings/22 s.8): r3 = this
// (CPhysicsBehaviour), r5 = the contact record (+68 surface normal, +84
// restitution e). It slides the velocity along the surface (v - (1+e)(n.v)n,
// sub_822EB850) and sets it (sub_821829E0). Wrapped only for the trace: one
// "R" line per response with the normal and the vertical speed before and
// after = which surfaces snap a body's falling speed to 0 on a slope.
// Line: R,ms,this,caller,record,nx,ny,nz,e,vx,vy,vz before,vx,vy,vz after.
// Found with it (findings/22 s.10): the caller sub_821809C8 (+0x858) hands
// in the game's own STRAIGHT-UP normal (0,1,0) for slow bodies on walkable
// ground: the falling speed on a slope becomes exactly 0.
extern "C" REX_FUNC(__imp__sub_8217F068);
extern "C" REX_FUNC(sub_8217F068) {
  using namespace ground_physics;
  if (!TraceOn()) {
    __imp__sub_8217F068(ctx, base);
    return;
  }
  const uint32_t self = ctx.r3.u32;
  const uint32_t record = ctx.r5.u32;
  std::array<float, 3> normal, v_before;
  for (int i = 0; i < 3; ++i) {
    normal[i] = BeFloat(base, record + 68 + 4 * i);
    v_before[i] = BeFloat(base, self + kVelocity + 4 * i);
  }
  const float e = BeFloat(base, record + 84);
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  __imp__sub_8217F068(ctx, base);
  std::lock_guard lock(g_trace.mutex);
  std::fprintf(g_trace.file, "R,%.3f,%08X,%08X,%08X,%.5f,%.5f,%.5f,%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
               TraceMs(), self, caller, record, normal[0], normal[1], normal[2], e, v_before[0], v_before[1],
               v_before[2], BeFloat(base, self + kVelocity), BeFloat(base, self + kVelocity + 4),
               BeFloat(base, self + kVelocity + 8));
  FlushNowAndThen();
}

// SetContact: r3 = this (CPhysicsBehaviour), r4 = 0 ground / 1 steep,
// r5 = the collision object (0 = clear). Remembers when and how a ground
// contact was lost (for the grace) and writes the trace's "C" lines.
extern "C" REX_FUNC(__imp__sub_82182400);
extern "C" REX_FUNC(sub_82182400) {
  using namespace ground_physics;
  const bool grace_on = GraceOn();
  const bool tracing = TraceOn();
  if (!grace_on && !tracing) {
    __imp__sub_82182400(ctx, base);
    return;
  }
  const uint32_t self = ctx.r3.u32;
  const uint32_t k = ctx.r4.u32;
  const uint32_t object = ctx.r5.u32;
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  const uint32_t old_object =
      k == 0 ? Contact(base, self, kGroundIndex) : k == 1 ? Contact(base, self, kSteepIndex) : 0;
  __imp__sub_82182400(ctx, base);
  if (object == old_object) {
    return;  // the same contact reported again (every frame on the ground)
  }
  const auto now = Clock::now();
  if (k == 0) {
    std::lock_guard lock(g_loss_mutex);
    if (object == 0) {
      g_losses[self] = Loss{now, caller};
    } else {
      g_losses.erase(self);
    }
  }
  if (tracing) {
    std::lock_guard lock(g_trace.mutex);
    std::fprintf(g_trace.file, "C,%.3f,%08X,%u,%08X,%08X,%08X\n", TraceMs(now), self, k, object,
                 old_object, caller);
    FlushNowAndThen();
  }
}

// MOVING PLATFORMS ABOVE 30 FPS (see the header): at 0x8217E984 the handler
// compares the squared step (f13) with its minimum (f0 = 1e-05); r30 = the
// CPhysicsBehaviour, its frame time at +0xA4. f0 isn't used again before
// it's reloaded (0x8217E9BC), so scaling it here only changes this compare.

// The path follower's Update (CPathFollowingBehaviour slot 4): r3 = this,
// f1 = frame time; remembered while it runs for CrashMomPathMoveMinimum
// (its step goes out from inside this call). Game main thread only.
namespace ground_physics {
namespace {
double g_path_frame_time = 0.0;  // 0 = not inside a path follower's Update

// A minimum step (a squared distance per frame, tuned for 30 fps) scaled to
// a frame of `dt` seconds: x (dt x 30)^2 when the frame is shorter than
// 1/30 s, i.e. the same minimum SPEED. Nothing changes at 30 fps or slower.
void ScaleMinimumStep(PPCRegister& minimum, double dt) {
  if (!REXCVAR_GET(platform_move_fix)) return;
  constexpr double kOriginalFrame = 1.0 / 30.0;
  if (!(dt > 0.0) || dt >= kOriginalFrame) return;
  const double scale = dt / kOriginalFrame;
  minimum.f64 *= scale * scale;
}

// A CPhysicsBehaviour's frame time, stored by its last Update (+0xA4).
double PhysicsFrameTime(uint32_t physics) {
  auto* memory = rex::system::kernel_memory();
  if (!memory || !physics) return 0.0;
  uint32_t bits;
  std::memcpy(&bits, GuestPtr(memory->virtual_membase(), physics + 0xA4), 4);
  bits = std::byteswap(bits);
  float dt;
  std::memcpy(&dt, &bits, 4);
  return dt;
}
}  // namespace
}  // namespace ground_physics

extern "C" REX_FUNC(__imp__sub_82158830);
extern "C" REX_FUNC(sub_82158830) {
  const double saved = ground_physics::g_path_frame_time;
  ground_physics::g_path_frame_time = ctx.f1.f64;
  __imp__sub_82158830(ctx, base);
  ground_physics::g_path_frame_time = saved;
}

// The path follower's minimum step (see the header): f8 = (game speed)^2 x
// 1.6667e-05, compared with the squared step (f31) at 0x82159670; f8 isn't
// used after the compare.
void CrashMomPathMoveMinimum(PPCRegister& f8) {
  ground_physics::ScaleMinimumStep(f8, ground_physics::g_path_frame_time);
}

// The physics' "move by" handler's minimum (see the header): f7 = (game
// speed)^2 x 1.6667e-05, compared with the squared step (f6) at 0x8217E72C;
// r31 = the CPhysicsBehaviour. f7 isn't used after the compare.
void CrashMomMoveByMinimum(PPCRegister& f7, PPCRegister& r31) {
  double dt = ground_physics::g_path_frame_time;
  if (!(dt > 0.0)) dt = ground_physics::PhysicsFrameTime(r31.u32);
  ground_physics::ScaleMinimumStep(f7, dt);
}

// The physics' transform handler's minimum (see the header): at 0x8217E984
// f0 = 1e-05 is compared with the squared move (f13); r30 = the
// CPhysicsBehaviour. f0 is reloaded (0x8217E9BC) before any other use.
void CrashMomPlatformMoveMinimum(PPCRegister& f0, PPCRegister& r30) {
  ground_physics::ScaleMinimumStep(f0, ground_physics::PhysicsFrameTime(r30.u32));
}

// =============================================================================
// THE TRIANGLE HANDOVER (findings/22 s.11, 2026-11-18): bumpy slopes above 30 fps
// =============================================================================
//
// THE BUG (what a player sees): running down uneven ground above 30 fps,
// Crash keeps dropping into his fall animation for a split second, as if he
// tripped over the bumps. Rare at 30 fps. Exact replays at 30 and 180 of the
// same walk (Ratcicle Kingdom courtyard): 0 ground losses at 30, 5 at 180.
//
// WHY: the ground contact remembers ONE collision triangle: contact slot k
// of the CPhysicsBehaviour = this +0x34 + 12k (+0 object, +8 triangle
// number). Every frame the reach check sub_8217D668(r3 = this, r4 = k) asks
// sub_8224E380 whether the body is still within reach (0.25 x its shape
// size) of THAT triangle; if not, the contact is dropped (and the fight tree
// hears "not on ground" = the fall). The collision sweep moves the contact
// to the triangle underneath only when the body actually TOUCHES the
// surface. At 30 fps one frame of gravity (2 units/s) puts the body back
// into the slope every frame, so the contact always holds the triangle
// under Crash. At 180 fps a frame of gravity is 6x smaller: after the
// collision response has stopped his downward speed (s.10), he hovers a
// hair above the slope for a few frames without touching it. Walking on
// meanwhile, he leaves the remembered triangle for the next one (where the
// slope gets steeper), the check fails, and the contact is dropped while
// he's still right on the ground. Every loss measured was at such a
// triangle change.
//
// THE FIX (ground_triangle_handover, on by default; only for frames shorter
// than the original 1/30 s, so 30 fps stays the original): when the GROUND
// check (k = 0) fails, try the triangles that share a corner with the old
// one, i.e. the ones Crash can have walked onto, if they're ground by the
// game's own threshold (normal . up > the float at 0x825A54C4, the one the
// collision report sorts contacts with). Each is tried by the
// game's OWN reach check (its triangle number written into the contact
// first); the first within reach keeps the contact, now on that triangle.
// None within reach = the original's answer (the contact is dropped: a real
// ledge or a jump still drops it).
//
// THE MESH (read 2026-11-18 from a live game, Ice Kingdom courtyard): the
// object's collision part = *(object +0x10), its mesh = *(part +0x24):
// +0x08 corners (3 floats each, 12 bytes), +0x10 triangles, +0x18 u16
// corner count, +0x1A u16 triangle count, +0x1C byte: triangle entries 16
// bytes instead of 8. A triangle entry = u16 corner numbers at +0/+2/+4
// (sub_8224E380 reads them the same way), +6 flags. The corners are read
// untransformed: the normal test assumes the object isn't tilted (true for
// level scenery; the game's own reach test still does the real geometry).
// =============================================================================

namespace ground_physics {
namespace {

constexpr uint32_t kContactObject = 0x34;    // contact slot 0: object
constexpr uint32_t kContactTriangle = 0x3C;  // contact slot 0: triangle number
// Ground vs steep vs wall: the collision report sub_821809C8 calls a contact
// ground when normal . up > the float at 0x825A54C4 (steep above 0x825A54C0).
// The neighbour must pass the same test as ground (read live: it's data).
constexpr uint32_t kGroundThresholdGlobal = 0x825A54C4;

uint16_t Be16(const uint8_t* base, uint32_t address) {
  uint16_t v;
  std::memcpy(&v, GuestPtr(const_cast<uint8_t*>(base), address), 2);
  return __builtin_bswap16(v);
}

void StoreBe32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(GuestPtr(base, address), &value, 4);
}

// A collision mesh as the reach check reads it (header above).
struct CollisionMesh {
  uint32_t corners = 0, triangles = 0, corner_count = 0, triangle_count = 0, stride = 8;
};

bool ReadMesh(const uint8_t* base, uint32_t object, CollisionMesh& m) {
  if (!object || !guest_memory::Readable(object + 0x10, 4)) return false;
  const uint32_t part = Be32(base, object + 0x10);
  if (!part || !guest_memory::Readable(part + 0x24, 4)) return false;
  const uint32_t mesh = Be32(base, part + 0x24);
  if (!mesh || !guest_memory::Readable(mesh, 0x20)) return false;
  m.corners = Be32(base, mesh + 0x08);
  m.triangles = Be32(base, mesh + 0x10);
  m.corner_count = Be16(base, mesh + 0x18);
  m.triangle_count = Be16(base, mesh + 0x1A);
  m.stride = GuestPtr(const_cast<uint8_t*>(base), mesh + 0x1C)[0] ? 16 : 8;
  return m.corners && m.triangles && m.corner_count && m.triangle_count &&
         guest_memory::Readable(m.corners, m.corner_count * 12) &&
         guest_memory::Readable(m.triangles, m.triangle_count * m.stride);
}

std::array<uint16_t, 3> Corners(const uint8_t* base, const CollisionMesh& m, uint32_t t) {
  const uint32_t at = m.triangles + t * m.stride;
  return {Be16(base, at), Be16(base, at + 2), Be16(base, at + 4)};
}

// The triangle's unit normal (untransformed corners); false if degenerate
// or a corner number is out of range.
bool Normal(const uint8_t* base, const CollisionMesh& m, const std::array<uint16_t, 3>& c,
            std::array<float, 3>& n) {
  std::array<std::array<float, 3>, 3> p;
  for (int i = 0; i < 3; ++i) {
    if (c[size_t(i)] >= m.corner_count) return false;
    for (int j = 0; j < 3; ++j) p[size_t(i)][size_t(j)] = BeFloat(base, m.corners + c[size_t(i)] * 12 + 4 * j);
  }
  const float ux = p[1][0] - p[0][0], uy = p[1][1] - p[0][1], uz = p[1][2] - p[0][2];
  const float vx = p[2][0] - p[0][0], vy = p[2][1] - p[0][1], vz = p[2][2] - p[0][2];
  n = {uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx};
  const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  if (!(len > 1e-8f)) return false;
  for (float& x : n) x /= len;
  return true;
}

std::atomic<uint64_t> g_handovers{0};  // contacts kept by the handover (log line)

}  // namespace
}  // namespace ground_physics

// =============================================================================
// STEP EDGES: the contact the sweep has JUST touched (findings/22 s.12)
// =============================================================================
//
// THE BUG: walking down real stairs above 30 fps, Crash's rounded body rides
// over each step's edge. Every frame the collision sweep touches the edge
// (a ground-type contact, normal about (0.75, 0.65, 0) on the corner) and the
// next physics update copies it into contact slot 0, then the reach check
// (sub_8217D668, the same as above) calls the upper step's triangle out of
// reach: the body's middle is past its edge. The contact flips off and on
// 7-8 times per 0.25-unit step at 180 fps, and Crash's walk logic hears "not
// on ground" each time. At 30 fps one frame carries him over the edge and the
// same frame's sweep lands him on the lower step: never heard (0 losses on the
// test course's 0.25 stairs walked at 30 fps, 21 at 180; findings/69 s.5).
//
// WHERE IT IS: inside CPhysicsBehaviour::Update (sub_8217D0A0), at +0xE0,
// sub_8217CF78(this, k) refreshes contact slot k from the sweep's result table
// (*(this +168) + 56k): only when the entry's flag byte +52 has bit 0x80 (the
// sweep found a contact of that kind this frame) it writes the object
// (sub_82182FE8 -> this +0x34 + 12k), the part (+0x38) and the triangle
// (entry +48 >> 3 -> +0x3C), and returns 1. Later in the same update, at
// +0x3D8, sub_8217D008 runs the reach check and clears a slot out of reach
// (sub_8217C888). Found with the console's `who` on the contact words.
//
// THE FIX (ground_touch_keeps_contact, on by default; frames shorter than
// 1/30 s only, like the handover): when the reach check (after the handover
// found no neighbour) calls the ground contact out of reach, keep it if a
// refresh copied a GROUND contact from the sweep (k = 0 returned 1) within
// the last 1/30 s of this body's physics time, i.e. no longer ago than one
// of the original's frames. At 30 fps the sweep refreshes the contact every
// frame; at 180 fps, riding over an edge, it touches the corner only every
// other update (the body moves down a hair between touches): a first version
// that only looked at THIS update's refresh kept 17 contacts in a whole run
// and halved nothing. The contact holds the triangle the sweep last touched.
// Walking off a real ledge still drops it once the sweep has touched nothing
// for 1/30 s (the original decides once per 1/30 s frame too).

namespace ground_physics {
namespace {
// Per CPhysicsBehaviour: its physics time (sum of its update steps) and when
// a refresh last copied a ground contact from the sweep (game thread only:
// physics runs on the main thread).
struct Touch {
  double time = 0.0, touched = -1.0;
};
std::unordered_map<uint32_t, Touch> g_touch;
constexpr double kOriginalFrame = 1.0 / 30.0;
std::atomic<uint64_t> g_touch_keeps{0};
}  // namespace
}  // namespace ground_physics

extern "C" REX_FUNC(__imp__sub_8217CF78);
extern "C" REX_FUNC(sub_8217CF78) {
  using namespace ground_physics;
  const uint32_t self = ctx.r3.u32;
  const uint32_t k = ctx.r4.u32;
  __imp__sub_8217CF78(ctx, base);
  if (k != 0) return;  // once per update (k = 0 comes first): the body's clock and its last touch
  Touch& t = g_touch[self];
  const double dt = PhysicsFrameTime(self);
  if (dt > 0.0 && dt < 1.0) t.time += dt;
  if ((ctx.r3.u32 & 0xFF) != 0) t.touched = t.time;
}

namespace ground_physics {
namespace {
// The reach check said "out of reach" for ground slot 0: keep the contact if
// the sweep touched ground within one original frame (STEP EDGES above). Trace
// line "K,ms,this,object,triangle".
bool KeepIfTouched(PPCContext& ctx, uint8_t* base, uint32_t self) {
  if (!REXCVAR_GET(ground_touch_keeps_contact)) return false;
  const auto it = g_touch.find(self);
  if (it == g_touch.end() || it->second.touched < 0.0 ||
      it->second.time - it->second.touched > kOriginalFrame + 1e-6) {
    return false;
  }
  ctx.r3.u64 = 1;
  if (TraceOn()) {
    std::lock_guard lock(g_trace.mutex);
    std::fprintf(g_trace.file, "K,%.3f,%08X,%08X,%u\n", TraceMs(), self, Be32(base, self + kContactObject),
                 Be32(base, self + kContactTriangle));
    FlushNowAndThen();
  }
  const uint64_t n_kept = ++g_touch_keeps;
  if (n_kept == 1 || n_kept % 1000 == 0) {
    REXLOG_INFO("ground_physics: a just-touched ground contact kept over a step's edge ({} so far)", n_kept);
  }
  return true;
}
}  // namespace
}  // namespace ground_physics

// The reach check of contact slot k: r3 = this (CPhysicsBehaviour), r4 = k
// (0 ground, 1 steep). Returns r3 = 1 when the contact is still within reach.
extern "C" REX_FUNC(__imp__sub_8217D668);
extern "C" REX_FUNC(sub_8217D668) {
  using namespace ground_physics;
  const PPCContext entry = ctx;  // to ask again with other triangles
  __imp__sub_8217D668(ctx, base);
  const uint32_t self = entry.r3.u32;
  if ((ctx.r3.u32 & 0xFF) != 0 || entry.r4.u32 != 0) return;
  // Only frames shorter than the original's: 30 fps keeps the original rule.
  const double dt = PhysicsFrameTime(self);
  if (!(dt > 0.0) || dt >= 0.9 / 30.0) return;
  // First the handover (a neighbour triangle in reach), then the step edges
  // (the sweep touched this contact just now): KeepIfTouched.
  if (!REXCVAR_GET(ground_triangle_handover)) {
    KeepIfTouched(ctx, base, self);
    return;
  }
  const uint32_t object = Be32(base, self + kContactObject);
  const uint32_t old_triangle = Be32(base, self + kContactTriangle);
  CollisionMesh m;
  std::array<uint16_t, 3> old_corners;
  std::array<float, 3> old_normal;
  if (!ReadMesh(base, object, m) || old_triangle >= m.triangle_count ||
      !Normal(base, m, old_corners = Corners(base, m, old_triangle), old_normal)) {
    KeepIfTouched(ctx, base, self);
    return;
  }
  const float ground_threshold = BeFloat(base, kGroundThresholdGlobal);
  // Trace ("H" lines): what the handover tried. tried = neighbours asked,
  // skipped = neighbours that aren't walkable ground.
  uint32_t tried = 0, skipped = 0;
  auto trace = [&](int32_t kept) {
    if (!TraceOn()) return;
    std::lock_guard lock(g_trace.mutex);
    std::fprintf(g_trace.file, "H,%.3f,%08X,%08X,%u,%d,%u,%u,%.3f,%.3f,%.3f\n", TraceMs(), self, object,
                 old_triangle, kept, tried, skipped, old_normal[0], old_normal[1], old_normal[2]);
    FlushNowAndThen();
  };
  for (uint32_t t = 0; t < m.triangle_count; ++t) {
    if (t == old_triangle) continue;
    const std::array<uint16_t, 3> c = Corners(base, m, t);
    bool shares = false;
    for (uint16_t a : c)
      for (uint16_t b : old_corners) shares |= a == b;
    if (!shares) continue;
    std::array<float, 3> n;
    if (!Normal(base, m, c, n)) continue;
    // Ground like the old one by the game's own test (the winding of the
    // corners isn't known: the normal's side is taken from the old one's).
    const float up = old_normal[1] < 0.0f ? -n[1] : n[1];
    if (!(up > ground_threshold)) {
      ++skipped;
      continue;
    }
    ++tried;
    StoreBe32(base, self + kContactTriangle, t);
    PPCContext again = entry;
    __imp__sub_8217D668(again, base);
    if ((again.r3.u32 & 0xFF) != 0) {
      ctx = again;  // still on the ground, on triangle t
      trace(int32_t(t));
      const uint64_t n_kept = ++g_handovers;
      if (n_kept == 1 || n_kept % 1000 == 0) {
        REXLOG_INFO("ground_physics: triangle handover kept a ground contact ({} so far)", n_kept);
      }
      return;
    }
  }
  StoreBe32(base, self + kContactTriangle, old_triangle);  // no neighbour in reach
  trace(-1);
  KeepIfTouched(ctx, base, self);  // a step's edge the sweep touched just now; else the original's answer
}
