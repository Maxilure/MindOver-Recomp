# Findings: the first screens drawn natively (native renderer, milestone 3)

2026-09-25. Roadmap phase 4, milestone 3 of
[04-native-renderer.md](../04-native-renderer.md): "first real screen,
natively". **Result:** everything from power-on to the main menu is now
drawn by our own Vulkan code when the native picture is on (F9, or
`--renderer=native`): the Sierra copyright card, the intro movies, the
"Loading" screen with its spinner, the title logo with "Press START", the
main menu with its slide-in, the Load Game screen with the save thumbnails,
and in-game the HUD. Measured against screenshots of the emulated picture
at the same moments, static screens agree to a mean difference of
**0.15 / 255** (Sierra card) and animated menus to ~1.6 / 255, which is mostly
the menu animations being caught a frame apart.

The game still runs on the emulated GPU underneath ("shadow mode",
[findings/08](08-renderer-map.md) section 6); what's new is that we
understand these screens well enough to redraw them from scratch.

## 1. How the game draws a 2D screen

Everything on these screens is **immediate-mode geometry**: small batches of
vertices the game writes by hand, one batch per sprite or text run
(10 batches for the title, ~100 for the main menu). Each batch is:

```
xnContext::BeginPrims(material, primitive type, vertex format, count)
    -> material's SetupDraw: D3D shaders, textures, constants, blend state
    -> xnContext::UpdateTransforms: matrices into vertex shader constants
    -> returns a stream object with write pointers into D3D's command buffer
game code writes `count` vertices through those pointers (no calls)
xnContext::EndPrims()
```

**Vertex format flags** (read from the stream's "begin" function
`0x824372E0`, which computes the stride and a pointer per component):
position is always 12 bytes first; `0x10` adds a normal (12), `0x20` a
colour (4, a D3DCOLOR `0xAARRGGBB`), the low 4 bits count texture-coordinate
sets (8 each), `0x800` / `0x1000` add two more 12-byte vectors (tangents?),
`0x4000` a variable number of 4-byte values. The menus use `0x2021`
(colour + 1 UV = 24 bytes) and `0x2020`. The stream keeps the pointers at
+4 (position), +8 (normal), +20 (colour), +24 (UV), 0 when absent, which is
how the recorder finds each component without decoding the flags itself.

**Primitive types** (PDDI's numbering → D3D's, from a table in the same
function): 0 triangle list, 1 triangle strip, 2 line list, 3 line strip,
4 points. Menus use lists (sprites as 6 vertices) and strips (4).

## 2. Where each piece of state lives

Every offset is named in [`src/native/guest.h`](../../src/native/guest.h).

| What | Where | Found in |
|---|---|---|
| clear colour / depth / stencil | `*(xnContext+136)` +4 / +8 / +12 | `xnContext::Clear` `0x824306A0` (passes 1 − depth: **reversed Z**) |
| depth test / compare / write, cull | `*(xnContext+140)` +8 / +12 / +16, +4 | the setters `0x82430990`–`0x82430A78` |
| fog on, colour, start, end | `*(xnContext+148)` | `SetFogEnable` / `SetFog` `0x82430C48` / `0x82430C98` |
| stencil states | `*(xnContext+152)` | `0x82430AA0`–`0x82430B38` |
| D3D device | `xnContext+400` | everywhere |
| immediate-mode stream | `xnContext+428` | BeginPrims / EndPrims |
| **every shader constant** | D3D device `+1920 + 16*reg` (VS c0–c255, then PS) | D3D's `SetVertexShaderConstantF` `0x82312998` |
| texture fetch constant per sampler | D3D device `+1152 + 24*sampler` | D3D's `SetTexture` `0x823066D0` |
| material's blend + alpha test (D3D values) | `*(xnShader+20)` +204..+228 | xnShader's common state `0x8242DA28` |
| material's blend mode (PDDI) | `xnShader+24` | same |
| simple material's texture | `xnSimpleShader+92` | its "TEX" setter `0x82442490`, SetupDraw `0x82441EA8` |
| movie planes Y / Cr / Cb | `xnBinkShader+92/+96/+100` | its SetupDraw `0x82446C58` |
| texture's D3D object | `xnTexture+20` | `xnTexture::Lock?` `0x8243ABD8` |
| palette (8-bit textures) | ring of 16 D3D textures at `xnTexture+24`, index `+88`; `+276 == 1` = palettized | `SetPalette` `0x8243AA68`, `0x8243AE08` |
| a D3D texture's fetch constant | D3D texture object `+28` (6 words) | `SetTexture` |

**Shadow-mode shortcuts.** Three of these are D3D's own copies (constants,
sampler fetch constants, the xn shaders' cache of applied states). They're
exact while the original backend still runs, but once our renderer
*replaces* it, D3D won't be called and they'll go stale. Then the same values
have to be computed the way the xn* code does it: matrices from
pddiBaseContext's matrix stacks (`UpdateTransforms` multiplies the stack top
at `*(this+32)`, the view at +52, the projection from `pddiBaseContext::v21`
and two more at +956 / +1020), blending from the material's PDDI blend mode
through the table below. Noted in the code where each one is read.

**The xnContext render-state setters** map PDDI values through small tables
(read from the image; values are Xenos/D3D enums):

| Table | Meaning |
|---|---|
| `0x824EDCA0` = {0, 6, 2, 0} | cull mode: none, CCW, CW |
| `0x824EDCE4` = {0, 7, 4, 6, 1, 3, 2, 5} | depth compare: never, always, then "less" → **greater**, "less-equal" → greater-equal... (reversed Z) |
| `0x824EE80C` = {0, 7, 1, 3, 4, 6, 2, 5} | alpha compare, not reversed |

**Material blend modes** (table `0x824EE758`, 16 bytes per mode: enable, op,
source, destination; factors through `0x824EE7D8`):

| Mode | Blending |
|---|---|
| 0 | off |
| 1 | alpha: src·α + dst·(1−α) |
| 2 | additive: src + dst |
| 3 | subtractive: dst − src |
| 4 | modulate: src·dst |
| 5 | modulate 2×: src·dst + dst·src |
| 6, 7 | variants with source alpha |

When a material is faded (its alpha × a global fade < 1), modes other than 2
and 3 are forced to mode 1. For additive/subtractive modes the pixel shader
scales the colour by the fade instead (alpha does nothing there).

## 3. The "simple" material's shaders

`xnSimpleShader` picks one of 2 vertex and 4 pixel shaders by name from
`game/shaders/`: lit or unlit (`+56`), palettized or not:
`simpleshader{lit,unlit}vp`, `simpleshader{lit,unlit}fp`,
`palsimpleshader{lit,unlit}fp`.

* **Vertex shader**: the unlit one ships with a debug file holding its
  source, so we know its constants by name: model-view-projection matrix in
  **c4–c7** (column-major), world matrix c12–c15, eye position c8, and a
  **tint colour c100** that multiplies the vertex colour.
* **Pixel shaders**: no source. Their microcode was disassembled with the
  SDK's `--dump_shaders=<dir>` (the dump's hashes are matched to the
  `.out` files by content). The unlit one comes down to:
  vertex colour × texture (only if **c20.x > 0**); a loop over up to 4 point
  lights (count in c29.x, positions and colours c30–c41); a second loop over
  up to 8 spheres (count c200.x, c201–c208) that darkens the colour; fog
  when bool **b69** is set (colour c70, start c71.x, end c72.x); finally
  alpha × **c2.x** (the fade), and RGB × c2.x too when bool b80 or b81 is
  set (= blend mode 2 or 3). On the 2D screens the light counts are 0 and
  fog is off, so only the first and last steps matter.
* **Palettized variant**: fetches the 4 index texels around the sample
  point with point sampling (texture size in **c112**), looks each up in
  the 256×1 palette (sampler 8), and blends the 4 colours bilinearly
  itself. Filtering indices directly would mix unrelated palette entries.

Our GLSL versions (`src/native/shaders/simple.*`) do those steps, with
alpha test (`discard`) added since Vulkan has no fixed-function one. They're
written from this description; no game shader code is copied.

## 4. Textures

A D3D texture object is a 28-byte resource header followed by the GPU's
6-word **fetch constant**: format, width/height, pitch, tiled or linear,
endian mode, and the address of the pixels (a *virtual* address here; D3D's
SetTexture converts it to a physical one for the GPU). Our texture cache
reads the pixels through the virtual mapping, undoes the endian swap,
untiles with the SDK's `texture_util::GetTiledOffset2D`, and uploads. The
fetch constant's component swizzle becomes the Vulkan view's swizzle.

Formats seen from boot to the main menu (debug log of every upload):
`k_8_8_8_8` linear (palettes, some UI), `k_8` linear (the palettized
sprites' indices and the movie planes), `k_8_8_8_8` tiled (2). **No
compressed (DXT) textures** on these screens: the UI is mostly
palettized. Palettes change often (the game animates them), so textures
are keyed by their layout words plus a hash of their pixels, checked once
per frame.

## 5. Two surprises found by comparing screenshots

**Gamma.** The first native picture was visibly brighter, most in the dark
colours (27 → 42, 128 → 140, 242 → 244 out of 255). The console's video
output passes every channel through a 256-entry **gamma ramp** the game sets
(D3D writes it to the display registers); the SDK's command processor keeps
that table and applies it when it presents the emulated picture. Our present
pass now applies the same table (read from the command processor; its
entries have the bit layout of Vulkan's A2B10G10R10 with red and blue
swapped, so they upload as a 256×1 texture unchanged). After that, colours
agree within 1/255. The game has a brightness screen ("Calibration" in the
main menu), which is presumably what the ramp is for.

**Half pixels.** D3D9 puts pixel centres at whole coordinates, Vulkan at
+0.5, so the first version shifted everything by half a pixel to
compensate. The comparison said otherwise: the native picture was about half
a pixel right and down of the emulated one, and without the shift they agree
within a quarter pixel. The 360's D3D has a half-pixel-offset state, and this
game draws for the +0.5 convention. No shift, then.

## 6. Movies (Bink)

`xnBinkShader` draws each decoded frame as one quad with three 8-bit planes:
Y (1280×720) and Cr, Cb (640×360), linear. Its vertex shader is `passthru`
(debug source: position and texcoord unchanged) and the positions are
**screen pixels** (0..1280, 0..720, logged): the Xbox GPU can skip its
viewport transform for such pretransformed vertices, so our vertex shader
does that step (pixels → clip space). The pixel shader `binkdecompress`
(microcode, 10 instructions) computes RGB = rows of a 3×4 matrix in c0–c2
applied to (Y, Cr, Cb), offsets in .w scaled by c3.x, alpha = c3.w; its
SetupDraw loads those constants from a fixed table at `0x824F0090`, the
standard BT.601 video conversion. We read them from the D3D device's copy
each frame.

## 7. What isn't drawn natively yet

The recorder logs each gap once ("not drawn natively yet: ..."). From boot
to the first hub level:

* **Static meshes** (`xnContext::DrawPrimBuffer` → `xnPrimBuffer`): the
  whole 3D world. Milestone 4.
* Materials `xnShadowShader` (one draw in the main menu, invisible in the
  comparison), `xnDOFShader` and `xnParticleShader` (hub).
* `xnContext::BeginIndexedPrims?` (hub).
* Drawing into textures (`SetRenderTarget` with a texture): skipped. The
  main menu does it 3 times per frame without a visible difference so far.
* The lit simple material's lighting (drawn unlit, reported if seen).
* Mipmaps (base level only), cube and volume textures.

## 8. Performance note

Shadow mode costs extra: the emulated GPU still does all its work, plus our
recording (a few hundred memory reads per draw) and our own rendering, on the
game's main thread. Not measured yet (no obvious slowdown in a two-minute
hub run). The payoff comes when the originals stop being called.

## 9. A memory leak that crashed the whole PC (SDK patch 0005)

After a day of testing the game died at launch with **SIGBUS**, other
programs on the machine started crashing, and a reboot was needed. Nothing
to do with the renderer: RAM was full (with swap in use and no game
running) because of two things kept in **RAM-backed tmpfs** filesystems
(`/tmp` and `/dev/shm`):

* **83 leftover `/dev/shm/xenia_memory_*` files, 6.1 GB.** On Linux the SDK
  keeps the 4 GB guest address space in a POSIX shared-memory object, and
  it only removed that object's name when it shut down cleanly. Every test
  run that timed out, crashed, was stopped from a debugger or exited
  without that cleanup (even some normal closes) left its object behind,
  holding every page of guest RAM the game had touched (~200 MB on the
  menus) until reboot. No process mapped any of them (`grep /proc/*/maps`).
  The SIGBUS is what a program gets when it touches a page of such a
  mapping and the kernel can't provide one.
* 2.2 GB of test screenshots in `/tmp` from the day's A/B runs.

Fix: `patches/rexglue-sdk/0005-shm-unlink-at-create.patch` removes the name
right after creating the object. It then lives exactly as long as the
game's own mapping of it, and the kernel frees it however the process ends.
Test captures now get deleted after each comparison.

## Open questions

* Which game code sets the gamma ramp (for the replacement phase), and
  whether the brightness calibration changes it live.
* `xnContext::v37` (stores to `*(this+144)+996`) and `v78` (table
  `0x824EDD04` = {0, 37, 1}).
* The sphere loop in the simple pixel shader (c200–c208): what the game uses
  it for. *Later:* the "Fx shadows", dark spots under characters and
  objects; drawn natively since [findings/20](20-texture-write-watch.md)
  section 8.
* The main menu's render-to-texture passes: what they draw.

## Reproduce

```bash
# native picture from the start, screenshots every 500 ms, into the main menu
out/build/linux-amd64-relwithdebinfo/crash_mom --game_data_root=$PWD/game --renderer=native \
    --debug_capture_dir=<tmp>/cap --debug_capture_interval_ms=500 \
    --debug_input_script="8000:start,9500:start,11000:start,12500:start,14000:start,17000:start"
# the same with --renderer=emulated, then compare the PPMs with PIL (numpy abs diff)
# texture formats in use: add --log_level=debug, grep "NativeRenderer: texture"
# the game's shader microcode: add --dump_shaders=<tmp>/shaders (game content: keep it local)
```
