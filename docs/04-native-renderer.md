# Native Vulkan renderer: kickoff notes (roadmap phase 4)

Written 2026-09-25, when work on the native renderer started; the
milestones at the end record the progress since. Read
[03-roadmap.md](03-roadmap.md) phase 4 and findings 06 and 07 first.

## Why now

* **Performance.** On Wumpa Island, the first area, the host GPU sits
  at 89–96% while the game's CPU threads have headroom (main 88%, mostly the
  game's own spin; GPU command thread 27%). The cost is ReXGlue emulating the
  360's EDRAM: we use the **FSI** path (findings/06), where every pixel of
  every draw does an interlocked read-modify-write of an EDRAM buffer, with
  blending and depth tests done in shader code, per MSAA sample. On top of
  that, the game draws each frame as **3 strips** (predicated tiling) and
  issues ~60 resolves per frame.
* **Correctness.** The faster **FBO** path loses whole frames (title idle,
  Bink movies, in-game screens). Fixing FBO is pointless if we replace the
  emulation anyway, so we skip it.
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

**Update 2026-09-25 (milestone 1 done, [findings/08](findings/08-renderer-map.md)):**
the class names survived in the exe, so PDDI's Xbox backend is fully mapped
(`xnContext`, `xnDisplay`, `xnDevice`, `xnTexture`, `xnPrimBuffer`,
`xnPrimBufferStream`, 18 `xn*Shader` classes; 298 functions of their own).
D3D has **no callers outside that backend**, so there's no "direct D3D use"
to fall back on. Recommendation: cut in at the `xn*` backend, with host
objects keyed by the guest `this`, in "shadow mode" first (details in
findings/08, sections 5 and 6).

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
* Test on a **copy** of the save data (`--user_data_root`), so a test run
  can never overwrite real saves.
* Comparing the two renderers (added along the way): **F9** switches the
  window's picture, **F10** saves both pictures of one frame as PNG
  (`src/native/ab_capture.*`), and **dual mode** (`--native_window`,
  `tools/play.sh --dual`, or **F8** while playing) shows them side by side
  live: the emulated picture in the main window, ours in a second window
  with its own presenter on the same Vulkan device
  (`src/native/native_window.*`). Closing the second window doesn't quit
  the game, and the controller works whichever window is focused.

## Milestones (proposal)

1. ✅ **Map it.** PDDI classes and vtables, the D3D functions the game calls
   per frame, which shaders and textures each draw uses (diag patch + gdb).
   Decide the layer. Output: findings/08 and a named-function list.
   Done 2026-09-25 with new tools instead of gdb: `tools/rtti_vtables.py`,
   `tools/callgraph.py` and the frame tracer `--debug_pddi_trace_dir`.
2. ✅ **Stand up our own Vulkan path next to ReXGlue's.** Either share the SDK's
   Vulkan device and presenter or run our own and hand over the final
   image. Show one native-drawn thing (a clear colour, then a test quad)
   in the game window.
   Plan from findings/08: share the SDK's `VulkanDevice` (from the
   presenter), draw into our own image, hand it over with
   `Presenter::RefreshGuestOutput` (any size), a hotkey to switch the window
   between the emulated and the native picture, and a small SDK patch so the
   emulated command processor stops presenting while the native picture is
   on. Hook point for "a frame is done": `xnDisplay::SwapBuffers`
   (`0x82433510`).
   **Done 2026-09-25**, exactly that way:
   * `src/pddi/intercept.*`: one strong wrapper per renderer function
     (same table as the tracer), with a per-function **handler** slot
     (`pddi::SetHandler`: a handler gets the original and may call it =
     shadow mode, or not = replacement) and **end-of-frame listeners**
     (after `SwapBuffers`, on the game's main thread). The tracer
     (`src/pddi/trace.cpp`) now sits on the same layer.
   * `src/native/native_renderer.*`: shares the SDK's `VulkanDevice` (from
     the presenter), draws its own 1280×720 RGBA8 frame (for now a test
     picture: a checkerboard quad rotating with the game's frame counter),
     then a "present" pass samples it into the presenter's guest-output
     image (that image can be drawn into but not copied into). Submits on
     graphics queue 0 through `AcquireQueue`, 3 command buffers in flight
     tracked by the SDK's `VulkanSubmissionTracker`.
   * **F9** (`bind_renderer`, rebindable in F4) switches the window between
     the emulated and the native picture; `--renderer=native` starts on the
     native one. SDK patch `0004-external-guest-output` makes the emulated
     GPU skip refreshing the window while ours is shown. The game keeps
     running on the emulated GPU underneath either way.
   * Shaders are GLSL in `src/native/shaders/`, compiled by `glslc` at
     build time into headers.
3. ✅ **First real screen, natively:** the loading screen or title (few
   draws): texture upload with untiling, the palette shader, vertex data
   byte-swapping, alpha blending. Compare side by side with the emulated
   path using the capture tool.
   **Done 2026-09-25, and further than planned** ([findings/09](findings/09-native-first-screens.md)):
   everything from power-on to the main menu is drawn natively (Sierra
   card, intro movies, loading screen, title, menus, Load Game screen, the
   in-game HUD), within ~1/255 of the emulated picture on static screens.
   * `src/native/recorder.*`: shadow handlers on BeginFrame, Clear,
     BeginPrims / EndPrims and SetRenderTarget turn each frame into a list
     of clears and draws (`src/native/frame.h`): vertices converted to host
     format, matrix / tint / fade from D3D's constant copy, textures by
     fetch constant, blend and alpha test from the material.
   * `src/native/texture_cache.*`: fetch constant → untiled, byte-swapped
     Vulkan image (SDK's tiling helpers), re-uploaded when the pixels'
     hash changes (palettes and movie frames change every frame).
   * Two materials rewritten in GLSL from reading the game's shaders:
     simple (texture × vertex colour, palettized variant with manual
     bilinear filtering, fade, alpha test) and Bink (Y/Cr/Cb → RGB).
   * The present pass applies the game's display **gamma ramp** (read from
     the SDK's command processor); without it the picture was too bright.
   * Missing pieces are logged once each ("not drawn natively yet: ...").
4. **The first area, Wumpa Island (in progress):** the 3D material shaders, depth, MSAA, then the
   post-processing chain (bloom, depth of field, colour matrix). Drop the
   3-strip tiling: render the whole frame in one pass.
   **Part 1 done 2026-09-25** ([findings/10](findings/10-native-3d-world.md)):
   the static world of Wumpa Island is drawn natively, within 3/255 of the
   emulated picture on average:
   * static meshes (`xnContext::DrawPrimBuffer` → `xnPrimBuffer::Draw`) from
     the game's own vertex/index buffers, uploaded once by
     `src/native/buffer_cache.*`;
   * depth (the game's reversed depth is a flipped viewport), culling
     (two-sided materials), colour masks, per-draw viewport, **stencil**
     (the blob-shadow volumes; without it the world was 25% too dark) and
     distance **fog**;
   * a render-target model, `src/native/render_targets.*`: one Vulkan image
     per D3D surface (tiled surfaces full size, no strips), resolves as
     image copies into images that stand for the resolved textures, the
     picture = what `EndFrame` resolves into the frontbuffer.
   **Part 2 done 2026-09-26** ([findings/11](findings/11-native-characters.md)):
   the characters (Crash, Crunch, Coco), within 2.3/255 of the emulated
   picture of the SAME frame for the whole of Wumpa Island (Crash's own pixels show no
   colour shift at all):
   * immediate geometry kept in the game's own vertex layout, with indices
     (`BeginIndexedPrims`: characters are skinned by the game's CPU);
   * the character material (`shaders/character.*`: normal map, 4
     directional lights with specular, rim light, Fx shadows) and the lit
     variant of the simple material (`shaders/lit.*`), their constants in
     a per-draw uniform buffer block (`LitParams`);
   * a same-frame A/B tool, `--debug_native_ab_ms` / `--debug_native_ab_trigger`
     (`src/native/ab_capture.*`).
   **Part 3 started 2026-09-26** ([findings/12](findings/12-native-shadows.md)):
   the characters' soft shadows (`xnShadowShader`), depth/stencil resolves,
   and two fixes found on the way (blend factors through the game's PDDI
   table; front faces from the cull mode, for two-sided stencil).
   Then the digging marker (the underground material) and the reflect material
   ([findings/13](findings/13-native-reflections.md)), and water and
   particles ([findings/14](findings/14-native-water-particles.md), within
   0.6 / 255 and 1.2 / 255 of the emulated picture on their own pixels):
   both read the scene's depth, and water the scene's colour, as the game
   resolves them mid-frame. Then the depth of field
   ([findings/15](findings/15-native-depth-of-field.md)): one full-screen
   pass blurring the distance; Wumpa Island and the waterfall area (the
   game's second area, name TBD) now log nothing as
   "not drawn natively".
   Then MSAA ([findings/16](findings/16-native-msaa.md)): the game's 2x
   surfaces are 2x natively, colour resolves average the samples, depth
   resolves take one sample through a small shader.
   Then mipmaps ([findings/17](findings/17-native-mipmaps.md)): every mip
   level uploaded, sampled trilinear with 16x anisotropic filtering like the
   game's; Wumpa Island within 0.51 / 255 of the emulated picture.
   Then dual mode (`src/native/native_window.*`): the native picture live in
   a second window next to the emulated one, to spot differences anywhere
   while playing; the first difference playtesting found with it, the double
   mojo flash (`xnFxGridShader`), is drawn now
   ([findings/18](findings/18-native-fx-grid-and-dual-mode.md)).
   Then cheaper texture checks, the bump material (TK blocks, see-through ice) and the
   proximity lights of the simple material
   ([findings/20](findings/20-texture-write-watch.md)), and the motion blur
   question: nothing to draw, the Xbox build has it switched off
   ([findings/21](findings/21-motion-blur-cut.md)). `--native_only` already
   turns the emulated GPU's drawing off while our picture is shown.
   Next: later areas' materials as playtesting reaches them.
5. **Full coverage and the payoff:** every effect and screen, Bink movies,
   then resolution scaling, other aspect ratios (camera + HUD), frame pacing.

Expect this to be the biggest part of the project (weeks to months). The
game stays playable on the emulated path the whole time.
