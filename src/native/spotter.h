// =============================================================================
// native/spotter.h -- loud log banners when a material we're hunting shows up
// =============================================================================
//
// WHY (2026-09-29): some materials are written for the native renderer but
// nobody has seen them drawn yet, because they only appear in later levels
// (xnBumpMegaShader, docs/findings/20). The console makes it OBVIOUS when
// the game uses one, so a playtester can look at both pictures (dual mode)
// and press F10 right there.
//
// WHAT: a pddi handler on each hunted material's setup function (vtable
// slot 14, called by the game every time a draw uses that material; it runs
// in every mode, whether our picture is drawn or not) counts calls per
// frame. At the frame's end:
//   * the material is drawn (first time, or again after 10 s without it):
//       *** SPOTTED: the bump material (xnBumpMegaShader) is being drawn nearby: ...
//     "Nearby", not "on screen": the game also draws objects hidden behind
//     others or just past the screen's edge (the banner can fire with no TK
//     block in view); F11 shows where it is.
//     and the first time in a session an F10 photo is taken automatically
//     (plus a frame trace with tools/play.sh --trace);
//   * it has been gone for 10 s: one line saying how long it was on
//     screen and its most setups in one frame.
// tools/play.sh shows these lines in colour in its live console.
//
// F11 (bind_highlight): HIGHLIGHT. Cycles off -> bump material -> particles
// -> off, painting that material magenta in OUR picture (the emulated one is
// untouched), to see which objects on screen use it. Log: "Spotter:
// highlight (F11): ...". Particles become flat see-through magenta patches
// whatever their texture, fade or soft-particle factor say, so a particle
// our renderer draws INVISIBLY still shows up (added 2026-09-29 for the
// Ratcicle titan's missing cyan glow). Flags: kBumpMegaHighlight,
// kParticleHighlight.
//
// To hunt another material: add a row to kHunted in spotter.cpp (its setup
// address must be in src/pddi/functions.inc).
// =============================================================================

#pragma once

#include <cstdint>

namespace native::spotter {

// Installs the handlers + the frame-end listener. `native_drawn` says whether
// our picture is drawn this frame (for the automatic photo: a pair when it
// is, else the emulated screen alone).
void Install(bool (*native_drawn)(void* user), void* user);
void Uninstall();

// Which material F11 paints magenta right now. Any thread (the recorder asks
// per draw).
enum class Highlight { kOff = 0, kBumpMega = 1, kParticles = 2 };
Highlight Highlighting();

// F12 (bind_particle_experiment): PARTICLE EXPERIMENTS, for a particle that
// looks different natively. Cycles: normal -> no soft edge -> no fog -> no
// texture (white) -> no vertex colour (white) -> base mip level only ->
// smallest mip level only -> normal, each skipping (or forcing) that
// one step of OUR particle shader (particle.frag), so a photo pair shows
// which step makes the difference. Log: "Spotter: particle experiment (F12):
// ...", and every draw list (ab_capture.h) names the mode. The kParticle*
// flags to OR into a particle draw's block (0 = normal); its name.
uint32_t ParticleExperimentFlags();
const char* ParticleExperimentName();
// The last F12 modes change the GAME's own vertex data, so they show in
// BOTH pictures: a group of particle draws gets its vertex colours zeroed
// right before the game hands them to the GPU (additive ones then add
// nothing, see-through ones get alpha 0): whichever group takes an effect
// away in the EMULATED picture is the one drawing it. Only while our
// picture is recorded (native shown, or dual mode).
enum class ParticleErase { kNone, kBig, kAdditive, kAlpha };
ParticleErase ParticleEraseMode();

}  // namespace native::spotter
