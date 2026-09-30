# Findings: characters drawn natively (native renderer, milestone 4, part 2)

2026-09-26. Roadmap phase 4, milestone 4 of
[04-native-renderer.md](../04-native-renderer.md), after the static world of
[findings/10](10-native-3d-world.md).
**Result:** Crash and the other characters of the first hub (Crunch, Coco)
are drawn natively, with their lighting: normal maps, 4 directional lights
with specular highlights, the rim light, and the lit variant of the
"simple" material that some character parts use (Crash's hair). Compared
with the emulated picture **of the same frame** (a new tool, section 6),
Crash's pixels differ by nothing measurable apart from his outline (no
anti-aliasing yet) and fine texture detail (no mipmaps yet). The whole hub
frame is within 2.3 / 255 on average and the main menu within 0.5 / 255.
(This first said the title screen was pixel-identical: that capture caught
the title's fade-in; with the logo shown it's 1.4 / 255, see findings/12.)

## 1. How the game draws a character

Characters aren't static meshes. The game poses (skins) them on its own
CPU every frame and streams the finished vertices through the same
immediate-mode path the menus use, plus an index list:
`xnContext::BeginIndexedPrims` (`0x824307F8`) is `BeginPrims` with one more
argument, r8 = the index count. The drawing loop is `0x823F3130`: per
material pass it calls `BeginIndexedPrims` (or `BeginPrims` when there are
no indices), then the object's own fill function (vtable slot 19), which
writes vertices and indices through the stream's pointers, then `EndPrims`.

The stream's "begin" (`0x824372E0`) asks D3D for room in its command buffer:
`D3D::BeginIndexedVertices` (`0x82477248`, arguments: primitive type, base
vertex, vertex count, index count, index format, stride, and two pointers
it fills in). The index format is 1; that function treats a set 0x4 bit as
32-bit indices, so these are **16-bit**. The index pointer lands at
**stream+32**, the vertex pointer at stream+4 as before.

Per vertex component the stream keeps a pointer to vertex 0's copy:
+8 normal, +12 binormal, +16 tangent, +20 colour, +24 texture coordinates.
The formats seen in the hub:

| Format flags | Components | Who |
|---|---|---|
| `0x3811` | position, normal, UV, binormal, tangent (56 bytes) | Crash, Crunch, Coco (character material) |
| `0x2011` | position, normal, UV | Crash's hair and other parts (lit simple material) |
| `0x2021` | position, colour, UV | props (unlit simple material) |
| `0x2031` | position, normal, colour, UV | water (not drawn yet) |

**What changed in the recorder.** Immediate geometry used to be converted
into a fixed 24-byte vertex (position, colour, UV). Now it's kept exactly
as the game wrote it: every component is made of 32-bit words (floats or a
D3DCOLOR), so swapping the bytes of each word turns the whole vertex into
host order without knowing its layout. That's what the buffer cache already
did for meshes, so both kinds of geometry now share one `VertexLayout`
(position, normal, colour, UV, binormal, tangent offsets). Indices are
copied at `EndPrims`; one pointing past the draw's vertices is made
degenerate rather than read out of bounds.

## 2. The character material (`xnCharacterRimShader`)

Vtable `0x82016294`, draw setup in slot 14 (`0x8242CD10`). It binds:

| Sampler | From | What |
|---|---|---|
| 0 | material +92 | diffuse texture |
| 1 | material +96 | normal map (**not rebound when missing**: the sampler keeps the previous draw's texture; we use a flat normal instead) |
| 3 | material +104 | a second texture the diffuse one can be blended towards |
| 2 | the context, only if material +136 is set | a copy of the screen, for a see-through "refraction" variant |
| 7, 8 | the textures' palettes | when the diffuse / normal map is 8-bit palettized |

Then it converts its colours (bytes to floats) and writes them **straight
into D3D's copy of the pixel shader constants**, bypassing D3D's setter
functions (so they never show up in a call trace), and sets three pixel
shader booleans with D3D's `SetPixelShaderConstantB` (`0x82312B58`).

**Where D3D keeps boolean constants:** pixel shader bool b*n* is bit *n* % 32
of the word at device + 10128 + 4 × (*n* / 32). (The GPU numbers pixel shader
bools from 128, so the shader's `b197` is D3D's b69, the fog switch.)

**The vertex shader** (`characterrimvp`, debug file available): position ×
MVP (c4-c7), world position (c12-c15), and normal, tangent and binormal
rotated into world space by c0-c2. No bones: the CPU already posed the
vertices.

**The pixel shader** (`characterrimfp`, microcode only). Its constant table
names every input, which made the decoding straightforward. In order:

1. **Normal mapping.** The normal map's RGB (×2 − 1) is a direction in the
   surface's own frame, turned into world space with the interpolated
   tangent, binormal and normal.
2. **Surface colour**: the diffuse texture, blended towards the second
   texture by a weight (c99.x; the game calls that texture the "evil map"),
   and optionally flashed yellow: red and green + c98.x / 4.
3. **Directional lights**, up to 4 (bools b0-b3; direction and colour in
   c9/c10, c11/c12, c13/c14, c15/c16): diffuse = surface diffuse (c64) ×
   light colour × how much the surface faces the light; specular = the
   light's reflection off the normal-mapped normal, seen from the camera
   (c128), to the power c68.x, × c65.x × light colour. A quirk kept as is:
   the diffuse part uses the plain interpolated vertex normal, not the
   normal-mapped one. colour = (ambient c8 × surface ambient c67 + diffuse)
   × surface colour + specular.
4. **Fx shadows**: up to 8 points (count c200.x, positions c201-c208) darken
   what's near them by up to 30%, fading out between 2 and 20 units.
5. **Rim light** (c102.x > 0): (1 − |N·V|)³ × c102.x × rim colour (c100) ×
   mix(rim colour, colour, c101.x). What outlines Crash against the scene.
6. Distance fog (the same as the simple material's), alpha × fade (c2.x).

A seventh section, the **refraction** variant (bool b11), mixes the result
with the screen copy, offset by the normal map. It isn't used in the hub and
isn't done yet (reported in the log if a draw wants it).

**Reading the microcode, two things worth knowing:**

* **Which input register is which.** A pixel shader's inputs are numbered in
  its own order, not the vertex shader's output order. Here they come
  texture coordinates first (TEXCOORD0, 1, 2), then colours (COLOR1, 2, 3):
  r0 = UV, r1 = tangent, r2 = binormal, r3 = normal, r4 = world position,
  r5 = clip position. Every use in the code agrees (r0.xy feeds every
  texture fetch, r5.w is divided by for the screen position, r4 is
  subtracted from the camera and the shadow points). The lit simple shader
  follows the same rule.
* **Literal constants** (c251-c255 here: 0.5, −1, 1, 0.3, 2, 400, 1/18...)
  aren't in D3D's constant copy. They sit in the `.out` file right **before**
  the microcode, which ends the file (findings/10 said "at the end": the
  microcode comes after them). Finding them was simple: known values like
  0.5 and −1 must land where the code clearly uses them, which fixes the
  block's position; every other use then checks out (the shadow-point loop
  compares the count with 1, 2, 3... 7 in exactly the right registers).

## 3. The lit simple material

`xnSimpleShader` with its "lit" byte (+56) set. Vertex shader
`simpleshaderlitvp` (debug file available): no vertex colour; the normal
rotated by c0-c2, the world position, the direction from the camera (c8) to
the vertex, and a screen-space **velocity** for the motion blur (from the
previous frame's MVP, c16-c19), written to a second render target.

Pixel shader `simpleshaderlitfp` (microcode + constant table):
diffuse and specular from the same 4 directional lights, then
colour = saturate(ambient × surface ambient + lights + emissive c66),
× the texture if there is one (then the Fx shadows), × (1 + the
"proximity" point lights: count c29.x, positions c30-c33, colours c34-c37,
parameters c38-c41, each brightness × 0.2 × intensity × falloff²), fog, fade.

Its specular part only lights surfaces facing **away** from the camera (the
view direction points from the camera to the surface, and the code requires
N·V > 0), so it practically never shows. Reproduced faithfully rather than
"fixed".

## 4. The lit draws' parameters: a uniform buffer

These materials read far more constants than the 128 bytes of push
constants the unlit ones use: ~860 bytes (three matrices, 4 lights, 4 point
lights, 8 shadow points...). Each lit draw now gets a `LitParams` block
(`src/native/frame.h`, mirrored by `shaders/lit_params.glsl`), laid out
with only 16-byte vectors so C++ and GLSL std140 agree without padding
rules, and padded to 1024 bytes (a multiple of 256, the largest offset
alignment Vulkan allows for uniform buffers). The recorder fills them, the
renderer uploads a frame's blocks in one piece and binds each draw's
through its descriptor set (binding 5). The descriptor set grew to 5
textures (character: diffuse, normal map, blend texture, and the two
palettes).

## 5. Checking it: first by eye, then exactly

The first native run showed Crash in front of his house with the right
colours, shoes, eyes and outline, next to a screenshot of the emulated
picture at the same moment. But the two runs caught different frames of
his idle animation, so the two pictures could only be compared by eye.
Since characters, water and particles all move, a better tool was worth
building first.

## 6. A new tool: the same frame from both renderers

`--debug_native_ab_ms=<times>` (or `--debug_native_ab_trigger=<file>`,
then `touch` it) saves `ab_<ms>_native.ppm` and `ab_<ms>_emulated.ppm` of
**one** game frame (`src/native/ab_capture.*`). The game runs both
renderers at once anyway (shadow mode), so every frame exists twice:

* ours is captured from the presenter right after we hand it over;
* the emulated one comes from the Xbox's RAM: with `--readback_resolve=full`
  the emulator copies each resolve back into guest memory, including
  EndFrame's resolve of the finished picture into the frontbuffer.

The emulated GPU runs behind the game's main thread, so the tool writes a
**marker** (a 32-bit pattern in 8 pixels of the first row and 8 of the
last) into the frontbuffer just before the game asks for the resolve, and
waits until the emulator has overwritten it. The two frontbuffers
alternate, and a lagging GPU could still be writing the *previous* frame
into the other one, so the frame before is marked and awaited first (the
GPU works in order). The marker sits on real pixels because a tiled
1280 × 720 texture is stored in whole 32 × 32 tiles: the memory's last
bytes are padding rows nothing writes (the first version waited there in
vain). The display gamma ramp is applied to the emulated picture on the
CPU, as the emulator does when it presents.

Results with it:

| Frame | Mean difference |
|---|---|
| Title screen, fading in (almost black) | 0.00 / 255 (fully shown: 1.4 / 255, findings/12) |
| Main menu | 0.47 / 255 |
| Hub, Crash in front of his house | 2.3 / 255 |
| Crash's own pixels (edges excluded) | 3.6 / 255 mean, signed mean per channel below 0.3: no colour shift at all; what's left is fine texture detail (mipmaps) |

| Intro movies (4 frames) | 0.01-0.04 / 255 (after the fix in section 7) |

A temporary magenta colour on the lit simple draws showed which parts use
that material (Crash's hair, bits of Crunch and Coco). Walking Crash over
to Crunch and Coco with the live input FIFO and triggering a capture there
showed them matching too; the visible difference is the depth-of-field blur
the emulated picture applies to the background.

## 7. Found on the way: the native movies had turned black

The first same-frame captures of the intro movies came out black on our
side, and so did the window (`--renderer=native`). Building the previous
commit showed the same: the movies had been black since the render-target
model of findings/10 (the checks after that change covered the menus and the hub, not the
movies).

A trace of a movie frame explained it. The movie is drawn into the tiled
main surface, and EndFrame's `EndTiling` resolves the three strips into the
frontbuffer with the flags `0x300`: **clear the surface right after the
copy**. We showed "the surface EndFrame resolves from", copied at the very
end of our frame, so after that clear: black. The hub's EndFrame resolves
from its post-processing surface without clearing it, which is why only the
movies were hit.

The fix follows the Xbox: the picture is the **frontbuffer texture's**
contents. The render-target model already keeps an image for every resolve
destination and fills it at the resolve's place in the frame (strip by
strip here), so the frame now shows that image. Movies are back, within
0.04 / 255 of the emulated picture of the same frame.

## 8. Not drawn natively yet

For characters:

* **The soft shadow under them**: `xnShadowShader`, a separate pass (with a
  depth resolve) that draws a dark patch on the ground. The biggest visible
  gap on Crash now.
* The **refraction** variant of the character material (not in the hub).
* The lit material's **velocity** output (render target 1), for the game's
  motion blur.
* **MSAA** (outlines are aliased) and **mipmaps** (fine detail shimmers).

Elsewhere (unchanged from findings/10): `xnReflectShader`, water
(`xnWaterSpecularShader`), `xnParticleShader`, `xnDOFShader`, depth
resolves, render target 1, the unlit simple material's point lights and Fx
shadows (0 in the hub so far).

## Reproduce

```bash
# into Wumpa Island on a COPY of the save data; same-frame A/B at 55 s
# (the Load Game presses depend on your own save list)
cp -a ~/.local/share/crash_mom <tmp>/userdata
out/build/linux-amd64-relwithdebinfo/crash_mom --game_data_root=$PWD/game \
    --user_data_root=<tmp>/userdata --renderer=native --readback_resolve=full \
    --debug_capture_dir=<tmp>/ab --debug_native_ab_ms=55000 \
    --debug_input_script="8000:start,9500:start,11000:start,12500:start,14000:start,17000:start,23000:down,25000:a,30000:down,32000:a,35000:a,38000:a"
# live: add --debug_input_fifo=<tmp>/input.fifo --debug_native_ab_trigger=<tmp>/ab.now,
# walk with `echo "lsright 2500" > input.fifo`, capture with `touch ab.now`
```
