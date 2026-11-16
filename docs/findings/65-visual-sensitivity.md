# 65. Visual sensitivity: reduce screen flashes (native renderer)

Options -> Accessibility -> **Visual Sensitivity** -> **Reduce Screen
Flashes** (`reduce_screen_flashes`, off by default): for photosensitive
players, the game's screen-wide flashes are weakened in the native renderer.

## 1. What flashes, and where

The one screen-wide flash this project has traced is the game's **fx grid**
(findings/18): it copies the picture so far into a texture and covers the
screen with about 40 batches through `xnFxGridShader`; each vertex has a
colour and alpha. In "add" mode (pixel shader bool b10) the result is
`picture + colour x alpha`: the even, clamped-at-white glow of the double
mojo power-up. In the other mode it is a tint, `picture x colour`. Both are
drawn natively by `shaders/fxgrid.frag`.

## 2. The change

With the setting on, the native renderer sets `kFlagCalmFx` on every fx grid
draw (`native_renderer.cpp`, the draw's flags; `simple_params.glsl`), and
`fxgrid.frag` keeps a quarter of the effect: a glow adds a quarter of its
colour, a tint moves a quarter of the way from white (no change) to its
colour. The moment is still visible (the power-up still reads) but it is
never a full-screen flash. Nothing changes with the setting off; the
emulated picture is not touched.

Read every draw: the Options row works at once. Kept in
`user/settings.toml` like every REXCVAR (a missing entry = off).

## 3. Not done / to check

* Other bright full-screen moments the game may draw another way (an
  explosion's sprite, a white fade, a hit effect on the characters, whose
  yellow flash is combat feedback and is kept) are not covered: a
  playthrough with the setting on should list what still flashes.
* A general limiter of sudden brightness changes between frames (the
  picture's average brightness, eased over a few frames) would catch any
  of them, but needs a small GPU reduction and a value kept from frame to
  frame in the present pass: left for a next step.
* The emulated picture keeps its flashes (this work is for the native
  renderer).
