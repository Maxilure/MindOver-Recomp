# Findings: the 3D world drawn natively (native renderer, milestone 4, part 1)

2026-09-25. Roadmap phase 4, milestone 4 of
[04-native-renderer.md](../04-native-renderer.md): "the first hub level".
**Result:** in the first hub level the native picture now shows the whole
static world (ground, houses, trees, plants, sky, distant scenery) with its
textures, depth, culling, blob-shadow stencil and distance fog, plus the HUD
on top. Against a screenshot of the emulated picture at the same moment the
mean difference over the whole frame is **3 / 255**; walls, tree bark, far
bushes and sky agree exactly. What's still missing is listed in section 8:
Crash and the other characters, water, particles, a reflective material and
the depth-of-field blur.

It took three pieces: **static meshes** (the game's vertex/index buffers),
a **render-target model** (the game draws into several surfaces and copies
between them), and **stencil** (without it, the world came out 25% too dark:
section 5 tells how that was tracked down).

## 1. Static meshes

A hub frame draws 223 meshes through `xnContext::DrawPrimBuffer`
(`0x82430878`, r4 = material, r5 = the `xnPrimBuffer`). It asks the material
how many passes it has and, per pass, applies the pass (shaders, constants,
textures) and calls the mesh's `Draw` (vtable slot 15, `0x82443BD0`). So the
recorder remembers the material in `DrawPrimBuffer` and records one draw per
`xnPrimBuffer::Draw` call, when the pass is fully set up. The material
read-out (matrix, tint, fade, texture, blending) is the same function the 2D
batches use.

**`xnPrimBuffer`** (constructor `0x82443D98`, setup `0x82443E20`, draw
`0x82443BD0`): +16 the Xbox GPU primitive type (4 triangle list, 6 strip...),
+20 the PDDI vertex format flags, +24 stride, +28 vertex count, +36 index
count (0 = not indexed), +40 a count override, +44 the D3D index buffer, +48
the D3D vertex buffer. Names and details: `src/native/guest.h`, namespace
`prim_buffer`.

**Vertex layout.** The setup turns the format flags into a D3D vertex
declaration (`0x8243CF28`), which tells exactly where each component lives:
position (3 floats) always first, then optional second position (`0x100`),
blend indices (`0x80`), normal (`0x10`), colour (`0x20`, and `0x4000` /
`0x40` for a second colour), the texture coordinate sets (low 4 bits), two
single floats (`0x200`, `0x400`), binormal (`0x800`) and tangent (`0x1000`).
The hub's world meshes are all `0x2021`: position, a **vertex colour** and one
UV set, 24 bytes. The world is pre-lit: its lighting is baked into the vertex
colours.

**D3D buffer objects.** A vertex buffer holds the GPU's 2-word vertex fetch
constant at +24 (virtual address | 3, then size in words << 2 | endian mode),
as D3D's `SetStreamSource` (`0x8230CE58`) shows. An index buffer holds the
address at +24 and the byte size at +28 (D3D's creation `0x823071B8` and
lock `0x82307268`); `xnPrimBuffer` always uses 16-bit indices.

**The buffer cache** (`src/native/buffer_cache.*`) uploads each buffer once:
it byte-swaps the data by the buffer's endian mode (8in32 for vertices, which
suits floats and colours alike; 16-bit swaps for indices) into a Vulkan
buffer from the SDK's Vulkan Memory Allocator. Like the texture cache it
hashes every buffer used in a frame and re-uploads on change. The whole hub
is **358 buffers, 1.1 MB**, so hashing costs little. After the byte swap a
D3DCOLOR's bytes read B, G, R, A, so both mesh and immediate-mode colours are
fed to Vulkan as `B8G8R8A8_UNORM` (the immediate vertices now keep the
game's value as is).

## 2. The 3D render states

| State | Where the game keeps it | Notes |
|---|---|---|
| depth test / compare / write | `*(xnContext+140)` +8 / +12 / +16 | compare through the table `0x824EDCE4` |
| cull mode | same block +4, table `0x824EDCA0` = {none, 6, 2, none} | **none for a two-sided material** (material +84), the rule of xnShader's common-state function `0x8242DA28` |
| colour write mask | same block +24..+27 (a byte per R, G, B, A) | `pddiBaseContext::v58` `0x8243ECF0` |
| viewport | `xnContext` +404..+424 (x, y, w, h, MinZ, MaxZ) | computed by `xnContext::v122` `0x824319A8` |
| stencil | `*(xnContext+152)`, section 5 | |
| fog | `*(xnContext+148)`, section 6 | |

**Reversed depth comes from the viewport.** The game's projection matrices
produce ordinary D3D depth (0 near, 1 far); the `xnContext` constructor
(`0x82432158`) sets the viewport's depth range to **MinZ = 1, MaxZ = 0**,
which flips it. That's why `Clear` hands D3D `1 - depth` and the compare
table turns "less" into "greater". Vulkan accepts a flipped depth range too,
so we pass the viewport and the game's compare functions through unchanged.
(Xbox compare functions and stencil ops happen to be numbered exactly like
Vulkan's `VkCompareOp` / `VkStencilOp`.)

**Culling direction.** D3D value 2 culls triangles that are clockwise on
screen, 6 counter-clockwise ones. Our vertex shaders flip Y, so Vulkan's
framebuffer has the Xbox's orientation, and with front faces = clockwise,
2 → cull front, 6 → cull back. Confirmed by the picture: nothing vanished.

## 3. What a hub frame does (the render-target structure)

From the frame tracer, outlined with a small script (top-level calls,
collapsed runs, the render-target calls underneath):

| Step | Surface | What |
|---|---|---|
| 1 | main (tiled) | `BeginFrame`: binds the main colour + depth surfaces, starts 3-strip tiling |
| 2 | main | the **water reflection**: ~50 draws with a mirrored camera, resolved into a texture, then the surface is cleared |
| 3 | main | the **scene**: ~150 simple-material meshes, ~57 `xnReflectShader` meshes, characters, particles; mid-way resolves of depth and colour for particles, water and a shadow effect; blob-shadow stencil (section 5) |
| 4 | main | `EndTiling`: resolves each strip of the scene into a scene texture, clearing each strip after its copy |
| 5 | post | a second, full-size surface (bound by `0x82431600`, which calls D3D directly): the scene texture drawn back full-screen, the **depth of field** (`FramebufferFxExt::v30` → `xnDOFShader`), then the **HUD** |
| 6 | post | `EndFrame` resolves it into the frontbuffer the TV shows |

**Surfaces.** A D3D surface object keeps GPU registers: +24
`RB_SURFACE_INFO` (pitch, MSAA), +28 `RB_COLOR_INFO` / `RB_DEPTH_INFO`
(format), +36 the size as `(width-1) << 18 | (height-1) << 3`, +40 the
D3DFORMAT, +44 its EDRAM bytes. Read off D3D's `SetRenderTarget`
(`0x8230D708`) and checked against the hub's four surfaces:

| Surface | Size | Format | MSAA |
|---|---|---|---|
| main colour `0x4000B270` | **480** x 720 | A8R8G8B8 | 2x |
| main depth `0x4000B5F0` | 480 x 720 | D24FS8 (24-bit float depth) | 2x |
| post colour `0xF9B5E750` | 1280 x 720 | A8R8G8B8 | 1x |
| post depth `0xF9B5E780` | 1280 x 720 | D24FS8 | 1x |

The main surface is only 480 wide: it's the piece of EDRAM the game renders
its 1280-wide picture through, one strip at a time (findings/06). On our side
it has to be one full-size image, so the recorder watches D3D's
`BeginTiling` (`0x823193D0`) and sizes the surfaces bound during tiling by
the union of the strip rectangles.

**Resolves.** D3D's `Resolve` (`0x8231EE00`) takes the Xbox 360 API's
arguments: flags (bits 0-1 which render target, `0x4` depth instead,
`0x100` / `0x200` clear colour / depth afterwards), source rectangle,
destination texture, destination point, clear values. We never get the
emulated GPU's resolved pixels (it keeps them in its own buffers), so we
make our own: **`src/native/render_targets.*`** keeps one Vulkan image per
surface and one per resolve destination (keyed by the texture's address), a
resolve becomes an image copy, and any draw sampling a resolved texture
gets our image. The picture we show is the surface `EndFrame` resolved into
the frontbuffer (corrected in findings/11: the frontbuffer's own image, as
the movies' EndFrame clears the surface right after). Render passes load and store their contents (EDRAM keeps
its contents too); every image's layout is tracked and each transition is a
barrier.

**Black bars at the strip overlaps.** The first try had two 32-pixel black
columns at x = 416..448 and 832..864, exactly where the strips overlap. At the
end of tiling the game resolves strip 0 *and clears it*, then resolves strip 1,
and so on. On the Xbox each strip is a separate render, so strip 0's clear
can't touch strip 1's pixels; on our side it's one image, and the clear wiped
the overlap strip 1 still had to copy. Fix: a resolve's clear is held back
until the run of resolves ends.

The game's own clears and render-target switches also have to be caught at
D3D level (`Clear` `0x82322808`, `SetRenderTarget` `0x8230DD40`,
`SetDepthStencilSurface` `0x8230DA68`), because the post-processing setup
calls D3D directly. D3D's clear flags: `0x1..0x8` render targets 0-3,
`0x10` depth, `0x20` stencil (PDDI's colour clear becomes `0xF`, then a
separate grey clear of render target 1 alone: the game uses a second render
target, which we don't have yet).

## 4. The simple material in 3D

The world uses the same material as the menus, `xnSimpleShader`, unlit
(`simpleshaderunlitvp` / `simpleshaderunlitfp`). Its vertex shader's debug
file confirms what matters here: position times the MVP matrix (c4-c7),
vertex colour times the tint c100, the UVs, a world position (c12-c15) and,
for fog, the clip-space depth. The tint is written straight into D3D's
constant copy by `0x8242FF88` from the `xnExtUnLitColourTint` extension
object (colour bytes / 256), so reading the copy at draw time is right.

The pixel shader (microcode, read with `--dump_shaders` and matched to
`game/shaders/*.out` by content) in order: colour x texture; up to 4 point
lights (count c29.x, **0 in the hub**); up to 8 "spheres" (count c200.x,
centres c201-c208) that darken the colour by distance, which look like
character blob shadows (0 in this scene); distance fog (section 6); the
fade. Its literal constants (the light and sphere loop limits, 1.0...) are
**not** in D3D's constant copy: the compiler stores them in the shader
binary itself, at the end of the `.out` file.

## 5. The world was 25% too dark: finding the stencil shadow quad

The first complete native frame had the right content but was clearly
darker than the emulated one. The steps, because the wrong turns taught
something too:

1. **Not an offset.** A best-shift search between the two screenshots:
   no shift fits better than none. It's colour.
2. **Not the display gamma.** The HUD matched within 1/255 while the world
   didn't. The game switches from an identity gamma ramp (menus) to a real
   curve in-game; inverting that curve for both pictures showed the world
   uniformly **1.33-1.45x brighter** in the emulated picture, sky included.
3. **Not the post-processing.** Skipping `FramebufferFxExt::v30` (the
   depth of field) in the emulated run changed nothing. Neither did skipping
   all `xnReflectShader` meshes.
4. **Not the shader math.** Vertex fetch, tint, texture format (plain
   RGBA8, no exponent adjust or gamma flag), pixel shader and its literals
   all matched our shader.
5. **The decisive experiment:** with `--readback_resolve=full` the emulator
   copies every resolve back into guest memory. Letting our renderer sample
   *those* (the emulator's scene) instead of its own made our picture match
   the emulated one exactly. So everything after the scene was right, and the
   difference was inside our 3D pass. (Kept as the debug flag
   `--debug_native_emulated_resolves`.)
6. Listing the immediate-mode draws on the scene surface found it at once:
   a **full-screen, untextured black quad at 25% alpha**, alpha-blended over
   the scene. It's the blob-shadow fill: shadow volumes first mark the
   stencil buffer (drawn with colour writes off), then this quad darkens only
   the marked pixels. We had no stencil, so it darkened everything:
   1 / 0.75 = 1.33, the measured gain.

**Stencil state** (`*(xnContext+152)`, setters `0x82430AA0`-`0x82430B38`):
+4 enable, +8 compare (table `0x824EDCC4`, not reversed), +12 reference, +16
read mask, +20 write mask, +24 / +28 / +32 the ops on stencil fail / depth
fail / pass (table `0x824EDCAC` = {keep, zero, replace, incr-wrap, decr-wrap,
invert}). The shadow volumes use **two-sided stencil**: `xnContext::v95`
(back-face ops, and two-sided on), `v96` (back-face compare) and `v97`
(two-sided on/off) only reach D3D, so the recorder keeps their values
itself. Compare and ops are baked into pipelines; reference and masks are
dynamic state.

## 6. Fog

`*(xnContext+148)`: +4 on (also D3D's pixel shader bool b69), +8 colour, +12
start, +16 end; `SetFog` (`0x82430C98`) also writes them to the pixel shader
constants c70 (colour), c71.x (start), c72.x (end). The shader blends
towards the fog colour by `f = (end - d) / (end - start)` (1 nearer than
start, 0 beyond end), `d` = the vertex's clip-space depth; additive and
subtractive materials fade out instead. The hub fogs from 5 to 800 units
towards a pale pink. With fog the distant bushes and water edge match.

## 7. Performance

Not measured in detail yet. The native path runs in shadow mode (the
emulated GPU still does everything), and the log line every 10 s shows 244
draws per hub frame with no new errors in 2-minute runs. The buffer cache
hashes ~1 MB per frame.

## 8. Not drawn natively yet (the hub)

From the log ("not drawn natively yet") and the difference image:

* **Characters**: Crash, the mutants, the save totem (name TBD: the totem that
  shows a disc when Crash comes near, where the game is saved). They're drawn with
  `xnContext::BeginIndexedPrims?` (`0x824307F8`, immediate-mode geometry
  with indices) and the `xnCharacterRimShader` material. Done next: [findings/11](11-native-characters.md).
* Materials `xnReflectShader` (57 meshes, distant decoration and the
  reflection pass), `xnWaterSpecularShader` (water), `xnParticleShader`,
  `xnShadowShader` (a full-screen soft-shadow pass), `xnDOFShader` (depth of
  field).
* Depth resolves (soft particles and water read depth), render target 1.
* MSAA (the game uses 2x; our edges are aliased) and mipmaps (base level
  only: distant textures shimmer).
* The lit simple material, point lights and "spheres" (not used in this
  frame).

## Reproduce

```bash
# into Wumpa Island on a COPY of the save data: ~45 s (the Load Game presses
# at 23-38 s depend on your own save list)
cp -a ~/.local/share/crash_mom <tmp>/userdata
out/build/linux-amd64-relwithdebinfo/crash_mom --game_data_root=$PWD/game \
    --user_data_root=<tmp>/userdata --renderer=native \
    --debug_capture_dir=<tmp>/cap --debug_capture_interval_ms=5000 \
    --debug_input_script="8000:start,9500:start,11000:start,12500:start,14000:start,17000:start,23000:down,25000:a,30000:down,32000:a,35000:a,38000:a"
# the same with --renderer=emulated, then compare the captures near 55 s
# the emulator's resolves in our picture (the section 5 experiment):
#   add --readback_resolve=full --debug_native_emulated_resolves=true
```
