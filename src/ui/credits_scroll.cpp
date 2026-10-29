// =============================================================================
// ui/credits_scroll.cpp -- see credits_scroll.h
// =============================================================================
#include "credits_scroll.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../guest_memory.h"
#include "../input/keyboard_mouse.h"
#include "../input/mouse_menus.h"
#include "scrooby.h"

extern "C" REX_FUNC(__imp__sub_820C6330);  // CCreditScreenAction::Update (r3 this, f1 dt, f2 clock)
extern "C" REX_FUNC(__imp__sub_820C6EB8);  // phase 1: roll the lines by f1
extern "C" REX_FUNC(__imp__sub_820C7260);  // phase 2: the end
extern "C" REX_FUNC(__imp__sub_820C7340);  // phase 3: the end
extern "C" REX_FUNC(__imp__sub_82373278);  // text element: set its string (UTF-16 pointer, copied)
extern "C" REX_FUNC(__imp__sub_82371C58);  // text element: set its string by text-bank key
extern "C" REX_FUNC(__imp__sub_820C71F0);  // the credits: are voice clips on? (the language)
extern "C" REX_FUNC(__imp__sub_82370FD0);  // element: scale by (f1, f2, f3) around its centre

REXCVAR_DEFINE_BOOL(credits_scroll, true, "CrashMoM",
                    "The credits scroll with the mouse wheel; Up slows them past a stop into rolling "
                    "backwards, Down rolls them on. false = the original (forward only, Up / Down = speed 0..1)");

namespace credits_scroll {
namespace {

// CCreditScreenAction (credits_scroll.h).
constexpr uint32_t kState = 16;        // s32, 3 = running
constexpr uint32_t kLinesBegin = 28;   // -> lines, 272 bytes each
constexpr uint32_t kLinesEnd = 32;
constexpr uint32_t kEndA = 44, kEndB = 48;  // the two end elements (shown by the last line)
// Every end element, +44..+72: +44 / +48 shown by the last line; phase 2
// (sub_820C7260) moves +72 and +52..+64 up and goes to phase 3 once +64 is
// high enough; phase 3 (sub_820C7340) places the last card. All their
// places go into a checkpoint (without them, rolling back out of the end
// then forward again jumped straight to phase 3: +64 was still up).
constexpr uint32_t kEndFirst = 44, kEndLast = 72;
constexpr int kEndCount = int((kEndLast - kEndFirst) / 4) + 1;
constexpr uint32_t kSlots = 84;        // 30 x {element, line index}
constexpr int kSlotCount = 30;
constexpr uint32_t kSpeed = 324;       // f32 0..1
constexpr uint32_t kNextY = 328;       // f32, where the next line goes (moves with the text)
constexpr uint32_t kScale = 332;       // f32, the divisor of every distance
constexpr uint32_t kLastLine = 336;    // s32, the last line handed out
constexpr uint32_t kHitLine = 340;     // s32, the voice line under the cursor (-1 none)
constexpr uint32_t kPhase = 344;       // s32 1 rolling, 2 / 3 the end
constexpr uint32_t kFlags = 348;       // u8, 0x80 = a voice clip plays (no rolling)
constexpr uint32_t kLineSize = 272;
constexpr uint32_t kLineText = 4, kLineHeading = 260, kLineVoice = 262, kLineByKey = 268;
constexpr uint32_t kElemScale = 16;  // f32, an element's x scale (its matrix [0])
// A voice line in the middle grows to this (the phase's own constant at
// 0x8201EE28 + 7720); the mouse's highlight uses the same.
constexpr float kVoiceScale = 1.9f;
constexpr uint32_t kElemX = 92, kElemY = 96, kElemFlags = 110;  // +110 bit 0x80 = shown
constexpr float kStepPerSecond = 500.0f;  // the constant at 0x82020C38 (x speed / +332)
constexpr float kLineSpacing = 30.0f;     // the phase's spacing (x 1 / +332), 0x8201EE28 + 7700
// How far a notch goes, how often we keep a checkpoint, and the biggest
// single step we hand the game's phase function while replaying (one third
// of a line: an element is recycled once per call at most).
constexpr float kNotchLines = 1.0f, kCheckpointLines = 2.0f, kChunkLines = 1.0f / 3.0f;
constexpr float kSpeedStep = 0.1f;  // the game's Up / Down step
// SMOOTH WHEEL (2026-10-30 playtest: a notch JUMPED 3 lines at once, "not
// smooth, too big"): a notch adds one line to the distance still to glide,
// and each frame moves this fraction-per-second of what is left (an ease
// out: ~95% of a notch in 0.2 s), plus a small floor so it doesn't crawl
// at the very end.
constexpr float kWheelGlidePerSecond = 15.0f;
constexpr float kWheelMinLinesPerSecond = 2.0f;
// THE END: phase 2 brings the last card ("Thanks For Playing!") up, phase 3
// holds it in the middle; the step keeps coming (the game rolls on, showing
// nothing new). We stop counting this far into phase 3 (lines), or one
// notch back from the end would still be the same card (tested: 420 notches
// down then 10 up = no change).
constexpr float kEndLines = 20.0f;

uint32_t R32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = GuestPtr(base, a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void W32(uint8_t* base, uint32_t a, uint32_t v) {
  uint8_t* p = GuestPtr(base, a);
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
float RF(const uint8_t* base, uint32_t a) {
  const uint32_t v = R32(base, a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void WF(uint8_t* base, uint32_t a, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  W32(base, a, v);
}

// The whole rolling state at one distance.
struct Checkpoint {
  float distance = 0;
  float next_y = 0;
  uint32_t last_line = 0, hit_line = 0, phase = 0;
  uint8_t flags = 0;
  struct Elem {
    float x = 0, y = 0;
    uint8_t shown = 0;
  };
  std::array<Elem, kEndCount> end{};
  std::array<Elem, kSlotCount> slot_elem{};
  std::array<uint32_t, kSlotCount> slot_line{};
};

struct State {
  uint32_t action = 0;      // the credits screen we follow (0 = none yet)
  float speed = 0;          // ours, -1..1 (the game's is 0..1)
  float distance = 0;       // rolled so far (in the game's units: x 1 / +332 done)
  float wheel_left = 0;     // wheel distance still to glide (game units, + = on)
  float end = -1;           // the distance where it stops (phase 3 + kEndLines; -1 = not reached yet)
  std::vector<Checkpoint> checkpoints;
} g;

Checkpoint::Elem ReadElem(const uint8_t* base, uint32_t e) {
  if (!e) return {};
  return {RF(base, e + kElemX), RF(base, e + kElemY), *GuestPtr(base, e + kElemFlags)};
}
void WriteElem(uint8_t* base, uint32_t e, const Checkpoint::Elem& v) {
  if (!e) return;
  WF(base, e + kElemX, v.x);
  WF(base, e + kElemY, v.y);
  *GuestPtr(base, e + kElemFlags) = v.shown;
}

Checkpoint Take(const uint8_t* base, uint32_t a) {
  Checkpoint c;
  c.distance = g.distance;
  c.next_y = RF(base, a + kNextY);
  c.last_line = R32(base, a + kLastLine);
  c.hit_line = R32(base, a + kHitLine);
  c.phase = R32(base, a + kPhase);
  c.flags = *GuestPtr(base, a + kFlags);
  for (int i = 0; i < kEndCount; ++i) c.end[size_t(i)] = ReadElem(base, R32(base, a + kEndFirst + 4 * uint32_t(i)));
  for (int i = 0; i < kSlotCount; ++i) {
    c.slot_elem[size_t(i)] = ReadElem(base, R32(base, a + kSlots + 8 * uint32_t(i)));
    c.slot_line[size_t(i)] = R32(base, a + kSlots + 8 * uint32_t(i) + 4);
  }
  return c;
}

// An element shows line `line` again: the game's own rule (0x820C6F8C).
void ShowLine(PPCContext& ctx, uint8_t* base, uint32_t a, uint32_t element, uint32_t line) {
  const uint32_t begin = R32(base, a + kLinesBegin), end = R32(base, a + kLinesEnd);
  if (!element || !begin || line >= (end - begin) / kLineSize) return;
  const uint32_t l = begin + line * kLineSize;
  if (!*GuestPtr(base, l + kLineHeading) && !*GuestPtr(base, l + kLineByKey)) {
    scrooby::Call(__imp__sub_82373278, ctx, base, element, l + kLineText);
  } else {
    scrooby::Call(__imp__sub_82371C58, ctx, base, element, R32(base, l));
  }
}

void Restore(PPCContext& ctx, uint8_t* base, uint32_t a, const Checkpoint& c) {
  g.distance = c.distance;
  WF(base, a + kNextY, c.next_y);
  W32(base, a + kLastLine, c.last_line);
  W32(base, a + kHitLine, c.hit_line);
  W32(base, a + kPhase, c.phase);
  *GuestPtr(base, a + kFlags) = c.flags;
  for (int i = 0; i < kEndCount; ++i) WriteElem(base, R32(base, a + kEndFirst + 4 * uint32_t(i)), c.end[size_t(i)]);
  for (int i = 0; i < kSlotCount; ++i) {
    const uint32_t slot = a + kSlots + 8 * uint32_t(i);
    const uint32_t element = R32(base, slot);
    WriteElem(base, element, c.slot_elem[size_t(i)]);
    if (R32(base, slot + 4) != c.slot_line[size_t(i)]) {
      W32(base, slot + 4, c.slot_line[size_t(i)]);
      ShowLine(ctx, base, a, element, c.slot_line[size_t(i)]);
    }
  }
}

// Rolls forward by `d` (>= 0) the way the game's Update does, in steps of
// at most a third of a line. Stops at the end (phase 3 runs on by itself).
void Advance(PPCContext& ctx, uint8_t* base, uint32_t a, float d) {
  const float unit = 1.0f / RF(base, a + kScale);
  const float chunk = kChunkLines * kLineSpacing * unit;
  while (d > 1e-6f) {
    if (g.end < 0 && R32(base, a + kPhase) == 3) g.end = g.distance + kEndLines * kLineSpacing * unit;
    if (g.end >= 0 && g.distance >= g.end) break;  // the end card is up: nothing more to roll
    const float step = std::min({d, chunk, g.end >= 0 ? g.end - g.distance : d});
    d -= step;
    // As the Update does before its phase (0x820C6530): the end elements
    // hidden (the last line shows them again), no voice line.
    for (uint32_t e : {R32(base, a + kEndA), R32(base, a + kEndB)}) {
      if (e) *GuestPtr(base, e + kElemFlags) &= 0x7F;
    }
    W32(base, a + kHitLine, 0xFFFFFFFF);
    switch (R32(base, a + kPhase)) {
      case 1: scrooby::CallF(__imp__sub_820C6EB8, ctx, base, a, step); break;
      case 2: scrooby::CallF(__imp__sub_820C7260, ctx, base, a, step); break;
      case 3: scrooby::CallF(__imp__sub_820C7340, ctx, base, a, step); break;
      default: break;
    }
    WF(base, a + kNextY, RF(base, a + kNextY) + step);
    g.distance += step;
    const float every = kCheckpointLines * kLineSpacing * unit;
    if (g.checkpoints.empty() || g.distance >= g.checkpoints.back().distance + every) {
      g.checkpoints.push_back(Take(base, a));
    }
  }
}

// Rolls back by `d` (>= 0): the last checkpoint at or before the target,
// then forward to it. Never before the start.
void Rewind(PPCContext& ctx, uint8_t* base, uint32_t a, float d) {
  if (g.checkpoints.empty() || d <= 0) return;
  const float target = std::max(0.0f, g.distance - d);
  auto it = std::upper_bound(g.checkpoints.begin(), g.checkpoints.end(), target,
                             [](float t, const Checkpoint& c) { return t < c.distance; });
  if (it != g.checkpoints.begin()) --it;
  REXLOG_DEBUG("Credits: back {:.3f} -> {:.3f} from checkpoint {:.3f} (phase {}; {} kept)", g.distance, target,
               it->distance, it->phase, g.checkpoints.size());
  Restore(ctx, base, a, *it);
  Advance(ctx, base, a, target - it->distance);
}

// THE VOICE LINES BY MOUSE (2026-10-29). Some lines (+262) carry a voice
// clip of that person: the game grows the one passing the middle of the
// screen (a window of the phase function, 0x820C712C) and A plays it (the
// Update, 0x820C63BC: line +340, plays while +348 bit 0x80 is set). The
// mouse can now pick ANY voice line on screen: the one under the pointer
// grows the same way and becomes +340 (the line A plays), and a left click
// on it is that A. Voice clips exist only where the game says so
// (sub_820C71F0: the language). Runs after the frame's phase calls, which
// reset every element's scale first (0x82370A58), so the highlight is
// redrawn each frame and goes with the pointer.
void PointAtVoiceLine(PPCContext& ctx, uint8_t* base, uint32_t a) {
  kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  if (!driver || !scrooby::Call(__imp__sub_820C71F0, ctx, base, a)) return;
  const uint32_t begin = R32(base, a + kLinesBegin), end = R32(base, a + kLinesEnd);
  const uint32_t lines = begin && end > begin ? (end - begin) / kLineSize : 0;
  for (int i = 0; i < kSlotCount; ++i) {
    const uint32_t slot = a + kSlots + 8 * uint32_t(i);
    const uint32_t element = R32(base, slot), line = R32(base, slot + 4);
    if (!element || line >= lines || !*GuestPtr(base, begin + line * kLineSize + kLineVoice)) continue;
    if (!mouse_menus::PointerOver(base, element)) continue;
    if (RF(base, element + kElemScale) < 1.5f) {  // (the middle one is grown already)
      scrooby::CallF(__imp__sub_82370FD0, ctx, base, element, kVoiceScale, kVoiceScale, kVoiceScale);
    }
    W32(base, a + kHitLine, line);  // the line A plays (the middle one's place)
    if (driver->PeekMenuClick() && mouse_menus::TakeClickOver(base, element)) {
      driver->Pulse(rex::input::X_INPUT_GAMEPAD_A);  // the game plays it next frame
      REXLOG_INFO("Credits: voice line {} clicked", line);
    }
    return;
  }
}

void Move(PPCContext& ctx, uint8_t* base, uint32_t a, float d) {
  if (d > 0) Advance(ctx, base, a, d);
  if (d < 0) Rewind(ctx, base, a, -d);
}

}  // namespace
}  // namespace credits_scroll

// The credits screen's Update (see credits_scroll.h, HOW).
extern "C" REX_FUNC(sub_820C6330) {
  using namespace credits_scroll;
  const uint32_t a = ctx.r3.u32;
  const double dt = ctx.f1.f64, clock = ctx.f2.f64;
  {  // the log: each change of the screen's state / phase
    static uint64_t last = 0;
    const uint64_t now = uint64_t(R32(base, a + kState)) << 32 | R32(base, a + kPhase);
    if (now != last) {
      last = now;
      REXLOG_INFO("Credits: screen {:08X} state {} phase {} (rolled {:.1f} lines)", a,
                  int32_t(now >> 32), int32_t(now), g.action == a && RF(base, a + kScale) > 0
                      ? g.distance * RF(base, a + kScale) / kLineSpacing : 0.0f);
    }
  }
  if (!REXCVAR_GET(credits_scroll) || int32_t(R32(base, a + kState)) != 3 ||
      R32(base, a + kPhase) < 1 || R32(base, a + kPhase) > 3) {
    __imp__sub_820C6330(ctx, base);
    return;
  }
  if (g.action != a || g.checkpoints.empty()) {  // a new visit (or the first running frame)
    g = {};
    g.action = a;
    g.speed = RF(base, a + kSpeed);
    g.checkpoints.push_back(Take(base, a));
    REXLOG_INFO("Credits: rolling (speed {:.1f}, {} lines); the wheel and Up / Down scroll them",
                g.speed, (R32(base, a + kLinesEnd) - R32(base, a + kLinesBegin)) / kLineSize);
  }
  const bool waiting_before = *GuestPtr(base, a + kFlags) & 0x80;  // a voice clip plays
  // The game's Update without its own step; its Up / Down read on 0.5.
  WF(base, a + kSpeed, 0.5f);
  ctx.f1.f64 = 0.0;
  ctx.f2.f64 = clock + 30.0 * dt;  // its clock +124 = f1 x 30 + f2: the same as before
  __imp__sub_820C6330(ctx, base);
  const uint64_t result = ctx.r3.u64;
  const float heard = RF(base, a + kSpeed);
  // DIRECTION (2026-10-30 playtest): the game reads Up as 0.6 (+0.1) and
  // Down as 0.4; on the Xbox Up = faster. Here the credits read like a list
  // you move through, as on PC: UP (W / arrow up / D-pad up) heads back
  // toward the start (rolling forward: slows, stops, then rolls back = the
  // names move down), DOWN heads on toward the end (names move up). Until
  // then Up sped the roll on and Down sent it back: the wrong way round.
  if (heard > 0.55f) g.speed = std::max(-1.0f, g.speed - kSpeedStep);
  if (heard < 0.45f) g.speed = std::min(1.0f, g.speed + kSpeedStep);
  if (heard > 0.55f || heard < 0.45f) {
    g.speed = std::round(g.speed * 10.0f) / 10.0f;  // no 0.30000001 drift
    REXLOG_INFO("Credits: speed {:.1f}{}", g.speed, g.speed < 0 ? " (backwards)" : "");
  }
  WF(base, a + kSpeed, std::clamp(g.speed, 0.0f, 1.0f));  // what the game expects there
  const bool running = int32_t(R32(base, a + kState)) == 3;
  const bool waiting = waiting_before || (*GuestPtr(base, a + kFlags) & 0x80);
  if (running && !waiting) {
    Move(ctx, base, a, float(g.speed * dt * kStepPerSecond / RF(base, a + kScale)));
    if (g.speed < 0 && g.distance <= 0) {  // back at the start (an empty screen): stop there
      g.speed = 0;
      REXLOG_DEBUG("Credits: back at the start, stopped");
    }
  }
  // The wheel: ours while the credits roll (input/mouse_menus.h). Like a
  // list (2026-10-30 playtest, the 10-29 direction felt inverted): a notch
  // DOWN = on toward the end (the names go up), UP = back. One line a notch,
  // glided over a few frames (SMOOTH WHEEL above), not jumped.
  if (running) {
    mouse_menus::ClaimWheel(mouse_menus::Wheel::kList);
    const float scale = RF(base, a + kScale);
    if (auto* driver = kbm::KeyboardMouseDriver::Get()) {
      if (const int notches = driver->TakeListWheel()) {
        g.wheel_left += notches * kNotchLines * kLineSpacing / scale;
      }
    }
    if (g.wheel_left != 0.0f) {
      const float left = std::abs(g.wheel_left);
      const float floor = float(kWheelMinLinesPerSecond * kLineSpacing / scale * dt);
      const float step = std::min(left, std::max(left * std::min(1.0f, float(kWheelGlidePerSecond * dt)), floor));
      const float signed_step = std::copysign(step, g.wheel_left);
      g.wheel_left -= signed_step;
      if (std::abs(g.wheel_left) < 1e-4f) g.wheel_left = 0;
      Move(ctx, base, a, signed_step);
      // At the start (nothing before it) there is nowhere left to glide.
      if (g.wheel_left < 0 && g.distance <= 0) g.wheel_left = 0;
    }
  }
  if (running && !waiting) PointAtVoiceLine(ctx, base, a);
  ctx.r3.u64 = result;
}
