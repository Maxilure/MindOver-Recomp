# Findings: black movies and the vanishing title logo (one SDK render-target bug)

Session of 2026-09-25 (fourth session that day). Starting point: the game
reached the title screen (findings/05), but:

* the four boot movies played with sound and a **black picture**, and
* on the title screen the logo **dropped in, then the screen went black**.
  The game still reacted to START, and the logo reappeared for its fade-out.

**Result:** both are the same problem, and neither is in the game code.
ReXGlue's default way of emulating the Xbox 360's on-chip framebuffer
("FBO" render-target path) loses the picture in these frames. Switching to
the SDK's other path, **FSI**, fixes both at the same 30 fps. We now
default to it in `src/crash_mom_app.h`. The movies play with picture, and the
title idles with the logo and "Press START" on screen. The Sierra copyright
card before the first loading screen, black until now, shows up too.

Along the way we built two reusable debugging tools (screenshots and a
scripted controller) and an optional SDK logging patch.

---

## Background: EDRAM, resolves and tiling

The 360's GPU never draws straight into RAM. It draws into **EDRAM**, 10 MB
of fast memory on the GPU die, and then a **resolve** copies a rectangle of
EDRAM out to a texture in RAM. The TV shows a texture in RAM (the
*frontbuffer*) that the game passes to `VdSwap`.

10 MB isn't enough for a 1280×720 frame with 2× anti-aliasing plus depth, so
games use **predicated tiling**: the frame is drawn several times, each time
into a vertical strip, and each strip is resolved into its part of the
destination texture. The GPU's *window offset* register shifts screen
coordinates so each strip lands at EDRAM x = 0.

This game uses **3 strips**: screen x `[0,448)`, `[416,864)`, `[832,1280)`
(32 px overlap), window offsets 0, −416, −832, EDRAM surface pitch 480,
2× MSAA.

## Part 1: the 42,000 "Resolve region is empty" errors are harmless

The user's run log had 21,184 `Resolve region is empty` errors (each
followed by a `PM4_DRAW_INDX_2 ... Failed in backend` line), steadily 6–8 per
frame. Earlier runs only showed 1,806, all on the first loading screen;
the difference is simply that we now get further into the game.

A temporary log in the SDK's `GetResolveInfo()` (`src/graphics/util/draw.cpp`)
printed each resolve's rectangle before and after the scissor clamp. In
every strip pass, the game issues the resolve for **all three** strips'
rectangles, each aimed at that pass's destination:

```
pass for strip 0 (window offset 0,    scissor 0..448):
  rect [0,448)    -> copies [0,448)         correct
  rect [416,864)  -> clamped to [416,448)   the overlap, correct pixels, correct place
  rect [832,1280) -> clamped to nothing     "Resolve region is empty"
pass for strip 2 (window offset -832, scissor 832..1280):
  rect [0,448)    -> after offset [-832,-384) -> nothing   "empty" again
  ...
```

Worked through per strip, every resolve either writes correct pixels to
their correct place or does nothing. So the errors are noise. Since they
drowned everything else in the log (~40,000 lines per 2 minutes), SDK patch
[`0003-empty-resolve-is-noop`](../../patches/rexglue-sdk/0003-empty-resolve-is-noop.patch)
makes a clipped-away resolve a silent success. A 30 s run now logs 0 errors. (Xenia users
see the same message in many tiled games.) The SDK does implement the
per-strip predicate bits (`bin_mask`/`bin_select` in
`command_processor.cpp`); these resolves just aren't restricted to one
strip by the game's D3D code.

## Part 2: two new tools, so Claude can see the screen

* **`--debug_capture_dir=<dir>`** (`src/debug_frame_capture.*`): a thread
  calls the presenter's `CaptureGuestOutput()` (implemented by the SDK,
  never called) every `--debug_capture_interval_ms` and saves the frame as a
  `.ppm`. All-black frames are only logged, with their timestamp.
* **`--debug_input_script="8000:start,9500:start,..."`**
  (`src/debug_input_script.*`): a synthetic input device, merged into
  player 1 like the SDK's keyboard driver, that taps buttons on a timeline.
  Tapping START every 1.5 s from 8 s to 14 s skips the intros, so a run
  reaches the title in ~16 s instead of ~2 minutes. It works without window
  focus.

Also found here: **the build didn't copy SDK GPU-plugin changes** next to the
exe. The copy was a post-link step, and nothing relinks the exe when only
`librexgpu-xenosrd.so` changes (it's dlopen'ed). It's now the always-run
`crash_mom_stage_libs` target in `CMakeLists.txt`.

## Part 3: finding where the picture goes black

### What the screenshots showed (FBO, the SDK default)

| time (intro skipped) | on screen |
|---|---|
| 0–6 s | black (should be the Sierra copyright card) |
| 6–7 s, 13–15 s | "Loading" + paw prints: fine |
| 8–13 s | movies: black |
| 15.8–16.3 s | logo drops in: fine |
| 16.5 s onward | **exactly 0 in every pixel**, for as long as we waited |

With START pressed at 17 s instead, the logo fade-out and the whole main
menu (New Game / Load Game / Credits / Calibration) rendered correctly.
Turning off background shader compilation (`--async_shader_compilation=false`)
changed nothing, so it wasn't draws being skipped while pipelines compiled.

### What the frames look like on the GPU

Resolve logs per phase (addresses stable across runs):

* **Movies:** each frame is 3 strip resolves (with EDRAM clear) straight
  into the frontbuffer, `0x16BF8000` / `0x16860000` alternating. Nothing else.
* **Title and menu:** a 3-strip 3D pass, with depth resolved into textures
  (`0x15666000`, `0x159FE000`, …) and colour into `0x15D96000`; then
  full-screen 1280×720 (no MSAA) passes: composite, copy to `0x164C6000`,
  sprites, then the final resolve into the frontbuffer.

The menu works with the same structure, so tiling as such isn't the problem.

### Per-draw logs: visible frame vs black frame

To compare individual draws, the optional SDK patch
[`patches/rexglue-sdk/debug/0100-gpu-draw-resolve-diag.patch`](../../patches/rexglue-sdk/debug/0100-gpu-draw-resolve-diag.patch)
logs every draw and resolve of chosen frames: shader hashes, surface
pitch/MSAA, render-target and depth bases, blend/depth registers, bound
textures. Frame 638 (last visible) and 650 (first black), 0.4 s apart:

```
both:  ... resolve composite -> 0x164C6000
       draw full-screen quad, scene texture 0x15D96000     (background)
       ~17 sprite quads, palette pixel shader 28DB7E4D9D1CB9CF
         (each piece = an 8-bit k_8 texture + a 256x1 RGBA palette texture:
          the game's "palsimpleshader")
650 adds: the subtitle's sprite pieces, 4 triangle lists (72/12/36/18
          vertices: "Press START" and the electricity), same shader
both:  2 depth-only rectangle draws: surface pitch 640, 4x MSAA,
       depth base 720, depth write "always"   (clears)
638 only: one more full-screen quad (pixel shader E3E90848987DDDBD):
          the fade overlay; the drop-in is a fade from black
both:  resolve 1280x720 -> frontbuffer, swap
```

So the black frame draws everything the visible one does, and more. The
only extra step in the visible frame is a *colour* draw between those
depth-only clears and the final resolve. That pointed at the SDK's
render-target bookkeeping, not at the game:

| experiment (FBO unless noted) | title idle | movies |
|---|---|---|
| skip the 4 triangle-list draws | black | – |
| skip the two 640-wide 4× MSAA depth-only clears | **visible** (Sierra card too) | black |
| `--direct_host_resolve=false` | black | black |
| `--render_target_path_vulkan=fsi` | **visible** | **visible** |

**FBO** ("host render targets") maps EDRAM onto ordinary Vulkan render
targets and has to track which one currently "owns" each EDRAM tile. **FSI**
keeps a real EDRAM image in a storage buffer and emulates the 360's
blending and depth logic in pixel shaders under *fragment shader
interlock*. It's slower in general, but exact. On the GTX 1660 SUPER it
holds the game's 30.0 fps cap (same as FBO), measured over 5 s windows by
counting swaps.

### Why the logo "disappeared", in plain terms

During the drop-in the game draws a fade overlay as its very last draw. On
the idle title there's no fade, so the last GPU work before the final copy
is a depth-buffer clear on a differently-shaped surface. In FBO mode that
sequence leaves the copy reading an empty (black) colour buffer. So the logo
was drawn every frame and then lost on its way to the screen. Pressing
START starts the fade-out, and the overlay draw brings it back.

## The fix

`CrashMomApp::OnPreSetup` sets `render_target_path_vulkan=fsi` unless the
user chose a path. The SDK falls back to FBO by itself on GPUs without
fragment-shader interlock (then these bugs would come back).

Gotcha: `render_target_path_vulkan` is defined **inside the GPU plugin**,
which the SDK loads right *after* `OnPreSetup`. `SetFlagByName` on a
not-yet-registered flag fails silently: the first attempt changed nothing.
We now load the plugin ourselves in `OnPreSetup`; the SDK skips its own
load when `config.graphics` is set. Loading registers the flag and replays
command-line / `REX_*` env / config values, so we can tell whether the user
picked one. The log says `CrashMoM: render_target_path_vulkan = "fsi"`.

Verified afterwards with default settings: the copyright card, movies and
idle title all show. Scripted input then goes title → main menu → New Game,
which opens a save-name screen with an on-screen keyboard, all rendering
correctly under FSI.

## Open questions / leads

* **Report the FBO bug upstream** (rexglue-sdk, probably inherited from
  Xenia). Minimal description: colour drawn at 1280×720 1× into EDRAM base 0,
  then depth-only rectangle draws on a 640-pitch 4× MSAA surface (depth base
  720, no colour target bound), then a resolve of colour base 0 → black. The
  movies fail differently: 3-strip 2× MSAA colour, resolved with clear
  directly into the frontbuffer. Not narrowed down further.
* The 3D scene behind the title and menu is drawn in strips and composited.
  FSI handles it, but it's the kind of frame to recheck if something looks
  off in gameplay.
* Keep an eye on performance in real levels. FSI costs more GPU time than
  FBO, and so far we've only measured menus.
