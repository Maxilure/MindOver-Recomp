# Native Vulkan renderer: kickoff notes (roadmap phase 4)

Written 2026-09-25 at the end of the session that made 60 fps work, as the
starting point for a new session dedicated to this. Read
[03-roadmap.md](03-roadmap.md) phase 4 and findings 06 and 07 first.

## Why now

* **Performance.** In the first hub level the host GPU (GTX 1660 SUPER) sits
  at 89–96% while the game's CPU threads have headroom (main 88%, mostly the
  game's own spin; GPU command thread 27%). The cost is ReXGlue emulating the
  360's EDRAM: we use the **FSI** path (findings/06), where every pixel of
  every draw does an interlocked read-modify-write of an EDRAM buffer, with
  blending and depth tests done in shader code, per MSAA sample. On top of
  that, the game draws each frame as **3 strips** (predicated tiling) and
  issues ~60 resolves per frame.
* **Correctness.** The faster **FBO** path loses whole frames (title idle,
  Bink movies, in-game screens). Fixing FBO is pointless if we replace the
  emulation anyway, so the user decided to skip it.
* **Features.** Any resolution or window shape, uncapped fps with frame
  pacing we control, graphics options: all much easier when we own the
  renderer (roadmap phases 3 and 4).

A native renderer makes both FBO and FSI unnecessary: no EDRAM emulation, no
strips, no resolves. The game's draws go straight to Vulkan render targets
with hardware blending, depth and MSAA.

## What "native" means here

Today: game code → Microsoft's D3D library (recompiled, statically linked) →
Xenos GPU command packets (PM4) → ReXGlue's command processor → Vulkan.

Goal: game code → **our renderer** → Vulkan. We intercept the game's
graphics calls at some layer and implement them ourselves, so the fake Xbox
GPU never sees those frames. The emulated path stays available behind a flag
(e.g. `--renderer=emulated|native`) until the native one covers everything,
so the game stays playable the whole time.

## Where to cut in: the first decision

| Layer | What it is | For | Against |
|---|---|---|---|
| **PDDI** (Radical's renderer interface) | Pure3D's C++ device layer. The shader build paths say `pure3d\pddi\src\xenon\...`: we'd replace the **xenon backend** | Highest level, fewest entry points, engine-shaped (textures, shaders, prim streams, render contexts). The *Simpsons: Hit & Run* PC version (same engine family) has a D3D PDDI backend whose structure the modding community has studied | Needs reverse engineering of the classes and vtables |
| **XDK D3D** (Microsoft's library) | D3D9-like API: SetRenderState, SetTexture, Draw..., Swap | Well-known API shape. The *Unleashed Recompiled* approach (hook the game's D3D calls, translate its shaders ahead of time) | On 360 much of D3D is inlined into callers as direct command-buffer writes; coverage has to be checked function by function |
| Improve ReXGlue's emulator | Keep PM4, fix FBO / speed up FSI | Least new code | Still emulating EDRAM, tiling, resolves: the problem we're leaving |

Suggested first step: **research both D3D and PDDI for a few sessions,
then decide**. Likely outcome: PDDI, falling back to D3D-level hooks for any
direct D3D use by the game.

## What we already know

Addresses (all in `default.xex`, image `0x82000000`):

| Address | What |
|---|---|
| `0x82433B00` | PDDI init: registers the vblank callback via D3D `SetVerticalBlankCallback` (`0x82311388`) |
| `0x82433510` | PDDI "swap buffers": vsync mode at `this+1660` (findings/07); then D3D SetRenderState(present interval) `0x8230C308`, `0x82310DA0`, D3D Swap `0x82310DA8` with the frontbuffer texture `this[1564 + 4*this[1652]]` |
| `0x82431470` | PDDI code on the main thread's per-frame stack at the title: `sub_8227AEE0` (main loop) → `8227C5D8` → `8227A958` → `8238AA78` → `82431470` → `823193D0` (D3D) → … |
| `0x82300000`–`0x8232xxxx` | D3D library range (swap `0x82310DA8`, vblank ISR `0x82310628`, flip target `0x82310728`, command-buffer allocation `0x82308B60`, …) |
| `0x16860000` / `0x16BF8000` | the two frontbuffers (1280×720 `k_8_8_8_8`, tiled) |

Frame structure (findings/06, from the per-draw log): the 3D scene is drawn in
3 strips (x 0–448, 416–864, 832–1280, 2× MSAA, EDRAM pitch 480) with depth
resolved into textures (used by effects) and colour into `0x15D96000`, then
full-screen 1280×720 passes: composite → `0x164C6000` → sprites → frontbuffer.
UI sprites use **palettized textures**: an 8-bit `k_8` texture plus a 256×1
RGBA palette, drawn by pixel shader `28DB7E4D9D1CB9CF` (the game's
`palsimpleshader`). Bink movies are three 8-bit planes (Y, Cr, Cb) turned
into RGB by `binkdecompress`.

Shaders:

* `game/shaders/` has **48 compiled shaders** (`.out`, Xenos microcode).
  **19** ship with a `.updb` debug file holding the original **HLSL
  source**; 29 don't (bloom, DOF, colour matrix, fur, heat shimmer, motion
  blur, Bink, ...). `default.rcf` holds 35 more `.out` entries (mostly the
  same names).
* **Rule:** game shader source must never be copied into the repo. So
  shaders have to be either (a) translated on the player's machine from
  their own disc files (ReXGlue already has a Xenos microcode → SPIR-V
  translator, `src/graphics/pipeline/shader/`, worth evaluating for reuse
  outside the EDRAM emulation), or (b) written from scratch by us, from our
  understanding of what each effect does. Descriptions are fine to commit;
  pasted HLSL is not.

Other things the renderer must handle that the emulator does today: Xbox
texture formats and **tiling** (untile + convert on load; the SDK's texture
code can be reused), **big-endian** vertex/index data, the 360's half-pixel
offset and depth conventions, gamma, and the resolve-to-texture uses (depth
textures for effects, render-to-texture for post-processing).

## Tools already available

* `patches/rexglue-sdk/debug/0100-gpu-draw-resolve-diag.patch`: logs every
  draw of chosen frames (shader hashes, render targets, blend/depth state,
  bound textures with addresses and formats) and every resolve. Ideal for
  building the "which PDDI/D3D call produced which draw" map.
* `--debug_capture_dir` screenshots, `--debug_input_script` /
  `--debug_input_fifo` scripted and live input (sticks too),
  `--debug_log_fps`, `--fps_cap`.
* `xexdis`, gdb breakpoints on `*__imp__sub_X`, function overrides
  (`extern "C" REX_FUNC(sub_X)` in `src/`) and mid-function hooks
  (`[[entrypoint.midasm_hook]]`), both shown in `src/frame_rate.cpp`.
* Test on a **copy** of the user's saves (`--user_data_root`), never while
  they're playing (`pgrep -x crash_mom`).

## Milestones (proposal)

1. **Map it.** PDDI classes and vtables, the D3D functions the game calls
   per frame, which shaders and textures each draw uses (diag patch + gdb).
   Decide the layer. Output: findings/08 and a named-function list.
2. **Stand up our own Vulkan path next to ReXGlue's.** Either share the SDK's
   Vulkan device and presenter or run our own and hand over the final
   image. Show one native-drawn thing (a clear colour, then a test quad)
   in the game window.
3. **First real screen, natively:** the loading screen or title (few
   draws): texture upload with untiling, the palette shader, vertex data
   byte-swapping, alpha blending. Compare side by side with the emulated
   path using the capture tool.
4. **The first hub level:** the 3D material shaders, depth, MSAA, then the
   post-processing chain (bloom, depth of field, colour matrix). Drop the
   3-strip tiling: render the whole frame in one pass.
5. **Full coverage and the payoff:** every effect and screen, Bink movies,
   then resolution scaling, other aspect ratios (camera + HUD), frame pacing.

Expect this to be the biggest part of the project (weeks to months). The
game stays playable on the emulated path the whole time.
