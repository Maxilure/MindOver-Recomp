// =============================================================================
// ground_physics.cpp -- Crash "falling" for a split second above 30 fps
//                       (--ground_grace_ms=40 by default; --debug_ground_trace)
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

REXCVAR_DECLARE(int32_t, fps_cap);  // frame_rate.cpp

REXCVAR_DEFINE_INT32(ground_grace_ms, 40, "CrashMoM",
                     "Above 30 fps: a ground contact lost for less than this many ms isn't "
                     "reported to the game's move logic, so Crash doesn't 'fall' for a split "
                     "second stepping down; 0 = off (src/ground_physics.cpp)");
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
  std::memcpy(&v, base + address, 4);
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
  const uint32_t storage = VariableAddress(base, actor, base[self + index_field]);
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

bool GraceOn() { return REXCVAR_GET(ground_grace_ms) > 0 && REXCVAR_GET(fps_cap) != 30; }

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
                       "# G,ms,actor,gravity,max_fall_speed\n",
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
