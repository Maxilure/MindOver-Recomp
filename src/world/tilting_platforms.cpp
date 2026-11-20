// =============================================================================
// TILTING AND ROLLING PLATFORMS ABOVE 60 FPS (2026-11-20, docs/findings/69 s.10)
// =============================================================================
//
// WHAT A PLAYER SEES: the platforms that tip under Crash's weight (the Ice
// Kingdom's tilting icebergs, Wumpa Island's "tilty" prop, the Desert / Phantom
// co-op tilting props, Nina's tilty platform) and the ones that roll as he
// walks on them (the Ice Kingdom's "donut" and C-shaped wheel, Junk's spinning
// sphere and gears) react differently above 60 fps: at 180 fps an iceberg
// tips about HALF as fast and the rolling logs spin sluggishly, so the balance
// the designers tuned is gone.
//
// HOW THEY WORK: every such platform has a CBounceBehaviour (vtable
// 0x8202C2D4; settings from its template's SetBehaviourDataPlatform: mass,
// spring_coef_x/z, rotation_dampening_coef_x/z, min/max angles,
// rotation_type ERotType_ROLLING_LOG_Z ...). Its Update (slot 4,
// sub_8212ABB0) runs the tilt physics in sub_8212BDC0, which cuts the frame
// time into slices of at most 1/60 s (a slice is 1/60 while more than 0.0183
// is left, else the rest). Per slice and axis (x: sub_8212B8B8, z: sub_8212B988,
// rolling log: sub_8212BCD8, scripted turn: sub_8212BA08):
//
//   push  = (torque from riders) x dt + (1/mass) x -(damping x v + spring x angle) x dt
//                                     (this +0x130 / +0x138; riders' torques arrive by message,
//                                      sub_8212AF10 -> sub_8212C890, added up per frame)
//   then the step itself, sub_8212B7F8:
//   v     = v + push x dt          <- 0x8212B84C  fmadds f11,f0,f29,f12   (f0 push, f29 dt)
//   v     = clamp(v, -max speed, max speed)
//   angle = angle + v x dt, clamped to the template's limits (v = 0 there)
//
// THE BUG: dt counts TWICE in the velocity change (push already holds one).
// Over one second the velocity gains (sum over slices) a x dt^2 = a x dt: at
// 30 and 60 fps the slices are all 1/60 s (30 fps = two of them), at 180 fps
// they are 1/180 s -> a third of the push per second. The designers tuned at
// 30 fps, so 1/60-s slices ARE the intended behaviour (30 and 60 already agree).
//
// THE FIX: at the velocity step, when the slice is shorter than 1/60 s, the
// push is scaled by (1/60) / dt: the velocity then gains push x (1/60) per
// slice, the same per second as with 1/60-s slices, while the angle still
// moves smoothly every frame. Slices of 1/60 s or more (30 and 60 fps) are
// left alone, bit for bit.
//
// HOW IT WAS FOUND: the test course's lane "tiltgap" (src/world/test_course.cpp)
// holds the Ice Kingdom's tilting iceberg; Crash dropped 1 unit off its middle,
// the console's `watch` on the behaviour's angle (+0x120) per game frame:
// -22.3 degrees after 3 s at 60 fps, -21.9 at 30, -12.2 at 180. Code read with
// Ghidra (tools/ghidra_decompile.sh) and xexdis 0x8212A000-0x8212E000.
//
// --tilting_platform_fix=false = the original (A/B tests).
// =============================================================================

#include <rex/cvar.h>
#include <rex/ppc/context.h>

REXCVAR_DEFINE_BOOL(tilting_platform_fix, true, "CrashMoM",
                    "Tilting and rolling platforms (icebergs, rolling logs) react the same at every frame rate "
                    "above 60 fps (their physics counted the frame time twice); false = the original");

namespace {
// The slice the game's tilt physics is built around: the constant sub_8212BDC0
// steps by (0x8201FB80, 0.016666668f).
constexpr float kSlice = 1.0f / 60.0f;
}  // namespace

// Midasm hook at 0x8212B84C (sub_8212B7F8, the per-slice step), just before
// `fmadds f11,f0,f29,f12` = v + push x dt. f0 = push (loaded from the
// behaviour at 0x8212B820), f29 = this slice's dt (from f1 at 0x8212B818).
// f0 is written again at 0x8212B860 before any other read, so scaling it here
// touches nothing else.
void CrashMomTiltSliceStep(PPCRegister& f0, PPCRegister& f29) {
  if (!REXCVAR_GET(tilting_platform_fix)) return;
  const double dt = f29.f64;
  // Below 1/60 s only (a hair of tolerance: a 60 fps frame of real time can
  // be a bit shorter than the constant; at exactly 1/60 nothing changes).
  if (!(dt > 0.0) || dt >= double(kSlice) * 0.999) return;
  f0.f64 = double(float(f0.f64 * (double(kSlice) / dt)));
}
