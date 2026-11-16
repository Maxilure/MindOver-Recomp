# 65. Visual sensitivity: reduce screen flashes (native renderer)

Options -> Accessibility -> **Visual Sensitivity** -> **Reduce Screen
Flashes** (`reduce_screen_flashes`, off by default): for photosensitive
players, the game's flashes and light effects are weakened in the native
renderer.

## 1. What flashes, and where

The one screen-wide flash this project has traced is the game's **fx grid**
(findings/18): it copies the picture so far into a texture and covers the
screen with about 40 batches through `xnFxGridShader`; each vertex has a
colour and alpha. In "add" mode (pixel shader bool b10) the result is
`picture + colour x alpha`: the even, clamped-at-white glow of the double
mojo power-up. In the other mode it is a tint, `picture x colour`. Both are
drawn natively by `shaders/fxgrid.frag`.

## 2. The change

With the setting on, in the native renderer:

| What | Where | Kept |
|---|---|---|
| The fx grid's screen-wide glow / tint (the double mojo flash) | `kFlagCalmFx` on fx grid draws, `fxgrid.frag`: a glow adds 15 % of its colour, a tint moves 15 % of the way | 15 % |
| Glowing sprites: the particles with additive / subtractive blending (sparks, glows, the bursts of broken crates and objects) | `kParticleCalm` (`recorder.cpp`, `frame.h`), `particle.frag`: their colour x 0.3 | 30 % |
| The attack ribbons (motion trails: Crash's spin and the like, findings/42) | `kFlagCalmFx` on motion trail draws, `simple.frag`: colour and alpha x 0.3 | 30 % |
| The characters' yellow glow (the pulse, PS c98.x: red and green + a quarter of it; e.g. Crash's spinning jump) | `recorder.cpp`, the character material's `character[0]` x 0.25 | 25 % |
| The glowing outline (rim light, PS c102.x) of characters and titans | `recorder.cpp`, the character material's `character[3]` x 0.4 | 40 % |
| Glowing meshes (additive / subtractive blending) of the simple and lit simple materials, e.g. a titan ability's energy | `kFlagCalmFx` (simple, `simple.frag`), `kLitCalm` (lit, `lit.frag`): colour x 0.3 | 30 % |
| The "proximity" point lights an effect throws on what is near it (up to 4 per draw, e.g. the Ratcicle's cyan flash, findings/20) | `CalmProximityLights` (`recorder.cpp`): each light's intensity (PS c38+n .z) x 0.35 | 35 % |

Normal (alpha-blended) sprites such as smoke and debris and the scene's
own lighting are not touched. The effects still show, so the moment still reads, but none
lights the screen up. Nothing changes with the setting off; the emulated
picture is not touched.

A first version only weakened the fx grid (to 25 %); a playtest found no
difference in normal play: the bright moments players notice most (broken
crates, Crash's spin) are particles, trails and their lights, not the fx
grid. A second playtest (Windows, native renderer, one player) with the
current version, comparing on / off: breaking crates, Crash's spin and
spinning jump, the titans and their abilities, storing a titan; every one
was clearly calmer with the setting on and still readable.

Read every draw: the Options row works at once. Kept in
`user/settings.toml` like every REXCVAR (a missing entry = off).

**Menus and the HUD too.** The simple material draws the 2D menus and the
HUD as well, so their glowing (additive) pictures are dimmed the same way
(30 %): for a photosensitive player that is wanted, a flash is a flash.

**Light, not opacity, for the glowing meshes.** The first version scaled a
glowing simple mesh's colour AND alpha by 0.3; an additive blend weighs
the colour by its alpha, so 0.3 x 0.3 = 9 % of the light was kept, not
30 %. Now only the colour is scaled for those (as in `particle.frag` and
`lit.frag`); the attack ribbons (alpha-blended) keep both scaled.

## 3. Not done / to check

* Other bright moments drawn another way (a white fade, a bright
  alpha-blended sprite) are not covered: a playthrough with the setting on
  should list what still flashes.
* A general limiter of sudden brightness changes between frames (the
  picture's average brightness, eased over a few frames) would catch any
  of them, but needs a small GPU reduction and a value kept from frame to
  frame in the present pass: left for a next step.
* The emulated picture keeps its flashes (this work is for the native
  renderer).
