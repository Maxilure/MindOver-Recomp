# 14. Water and particles drawn natively

2026-09-26. The native renderer still left out two
things seen everywhere in the game: **water** (the waterfalls of the
waterfall area, name TBD; Wumpa Island's river and sea) and **particles** (glows, splashes,
sparks). The log said `not drawn natively yet: material class
xnWaterSpecularShader` and `... xnParticleShader`. Both are drawn now. On
the same frame from both renderers, the water's pixels differ from the
emulated picture by 0.55 / 255 on average and the particles' by 1.2 / 255,
with no colour shift above 0.3 / 255.

Both materials have something in common that nothing before them had:
they read pictures the game makes **in the middle of the frame**, the
scene's depth (both) and the scene's colour so far (water). The render
targets already stood in for those resolves (findings/10 and 12), so the
new work was mostly reading the shaders.

## 1. Particles: `xnParticleShader`

**How they're drawn.** Plain immediate triangles (`BeginPrims`, format
`0x2021`: position, colour, one UV), a few hundred vertices per batch. The
draw setup (vtable `0x82018B54`, slot 14, `0x82448158`):

* its texture (`+92`) on sampler 0; for an 8-bit palettized one the palette
  on sampler 8 and the texture size in PS c112, like the simple material;
* on sampler 1, the **scene's depth**: the context's `0x824310A0` resolves
  the depth/stencil surface into a texture the first time anyone asks in a
  frame (a resolve with flags `0x14`, strip by strip while tiling) and
  hands out the same texture afterwards (a "done this frame" byte at
  context `+1196`). It's sampled with point filtering;
* shaders from its own table at `0x825971F8`: `particleshadervp`,
  `particleshaderfp`, `palparticleshaderfp`;
* constants: PS c2.x the fade (the context's global fade × the material's
  `+88`), PS c4 "Falloff" (four floats from the game's data at
  `0x824F049C`, `.x` = 1.5), PS c20.x = ±1 textured (the pixel shader
  doesn't read it), PS bools b80 / b81 "Additive" / "Subtractive" from the
  blend mode at `+24`.

**The vertex shader** (`particleshadervp`, with a debug file): position ×
the model-view-projection matrix, colour and UV passed through, and the
clip-space position a second time for the pixel shader.

**The pixel shader** (microcode only, disassembled with `--dump_shaders`;
its constant table names every input). Its inputs are UV, clip position,
colour (texture coordinates first, then colours, as always):

1. **Soft particles.** A flat sprite that cuts into the ground shows a hard
   straight line where it meets it. The shader turns the pixel's clip
   position into a screen position (`0.5 + 0.5·x/w`, `0.5 − 0.5·y/w`),
   reads the scene's depth there, and turns it into a distance with
   **c73 "NearFar"**: `scene = NearFar.y / ((1 − NearFar.x) − depth)`
   (depth being the game's reversed one). The sprite fades out over the last
   "Falloff" units before the scene: `soft = saturate((scene − clip z) / Falloff)`.
2. colour = texture × vertex colour; alpha = texture alpha × vertex alpha ×
   **soft² × fade** (the square is really there: `soft` is multiplied in
   twice).
3. Additive and subtractive sprites (b80 / b81, where alpha does nothing)
   scale their colour by the same `soft² × fade`.
4. Fog (b69, c70-c72). Normal sprites blend towards the fog colour.
   Additive ones are multiplied by `mix(fog colour, 1, f) × f`, so they
   fade into the fog instead of adding its colour.

The literal constants (0.5, 1) sit right before the microcode in the
`.out` file, as for the other shaders (findings/11). The palettized
variant is the same after the usual four-texel palette lookup.

## 2. Water: `xnWaterSpecularShader`

**How it's drawn.** Indexed immediate geometry (`BeginIndexedPrims`,
format `0x2031`: position, normal, colour, UV), streamed by the same loop
as the characters (`0x823F3130`). The draw setup (vtable `0x82018E64`,
slot 14, `0x8244A1D0`, about 650 instructions) binds **nine textures**, one
per sampler; the pixel shader's constant table names them:

| Sampler | Name | Where it comes from |
|---|---|---|
| 0 | DiffuseTex | material `+92` |
| 1 | DistortMap | material `+96` |
| 2 | BackGroundMap | the scene drawn so far: the context's `0x82430F28` resolves colour target 0 **right inside this setup** (strip by strip with predication) |
| 3 | DepthMap | the scene's depth (`0x824310A0`, as for the particles) |
| 4 | NormalMap | material `+100` |
| 5, 6 | NormalTex0 / 1 | two consecutive frames of a 15-frame animated normal map (a table at `0x8258E420`, getter `0x82431440`), picked from the game's clock (`0x8235ABC8`, milliseconds × 0.001 × 15 frames per second) |
| 7 | ReflectMap | a D3D texture the context keeps at `+1084`: the planar reflection picture the frame renders first |
| 8 | ReflectMask | material `+104` |

Sampler 0 gets the material's address and filter modes, the others fixed
ones (the depth one point-sampled). Shaders from the table at
`0x82597378`: `waterspecularvp`, `waterspecularfp`. Constants, with the
names from the constant table: PS c2.x Fade, c23.x PixelHeight (1/720;
c22.x = 1/1280 is set too), c25.x Blend (between the two animation
frames), c100.x ReflectBumpiness and c101.x WaterBumpiness (floats at
`+112` / `+116`), c102 ReflectColor (the `0xAARRGGBB` at `+108`, as RGBA),
bool b10 IsDisableReflection (`+132`); plus, set by the context, c128
CameraPos, c129 ViewDirection, c73 NearFar, the fog (b69, c70-c72) and the
Fx shadows (c200-c208). The surface colours `+64..+76` go to c64-c67 like
every material, but this pixel shader doesn't use them.

**The vertex shader** (`waterspecularvp`, with a debug file): position ×
the model-view-projection matrix; colour and UV passed through; the world
position (× the world matrix c12-c15); the clip position once more. It
declares a time constant (VS c128) and never uses it.

**The pixel shader.** Since the Xbox can't look "through" a surface while
drawing it, the water paints a wobbly view of the copies made just before
it. In order:

1. The screen position, and two wobble vectors from the DistortMap: one
   read at the water's UV (its blue and alpha channels, moving with the
   water), one read at the screen position (red and green).
2. **Reflection** (unless b10): the ReflectMap at the screen position
   pushed by the first wobble × ReflectBumpiness/2, times ReflectColor; its
   strength = its alpha × ReflectColor's alpha × the ReflectMask (read at
   the same spot).
3. **Refraction**: the BackGroundMap, pushed vertically by the first
   wobble's y × ReflectBumpiness/4. The depth copy is read three times,
   under the pixel, at the pushed spot and one pixel above it, and turned
   into distances behind the water's surface (with NearFar, as the
   particles do). If either pushed spot is nearer than minus half the
   water's local depth (something standing in front of the water), there's
   no push: that's how the wobble avoids smearing foreground objects into
   the water.
4. The water's own colour: DiffuseTex at the UV pushed by the second wobble
   × WaterBumpiness/2, times the vertex colour, darkened by the Fx shadows.
   This material has its own numbers for those: up to **65 %** darker
   (the lit materials: 30 %), full within 2 units, fading over 18, none
   beyond 20.
5. A **glint**: the normal is the NormalMap (at the pushed UV) plus the
   animated normal map (the two frames mixed by Blend) minus 1,
   normalized. A light direction made from the camera's horizontal view
   direction and a slight downward part, `normalize(−view.x, −view.z,
   −0.5)`, is mirrored about that normal and compared with the direction
   to the camera: glint = saturate(that) × 0.2. The maps' blue channel is
   "up", so the comparison with world directions swaps y and z.
6. colour = diffuse × (1 − shadow) + glint, mixed towards the reflection by
   its strength, then laid **over the scene copy** by the diffuse alpha.
   The water does its own blending this way; its output alpha is just the
   fade.
7. Fog, as the other materials.

Its literal constants (the Fx shadow counts 1-7, 0.5, −1, 0.25, 0.2, 0.65,
400, 1/18) sit before the microcode as usual. Reading the microcode, the
components come shuffled (the colour copy is fetched as `.yx_z`, normals
as `.yzx_`), so each channel was followed through by hand; our texture
cache applies each texture's component swizzle the way the GPU does, so
`.x` in the microcode is red on our side.

## 3. Plumbing

* **Textures from D3D's sampler copy.** Water's samplers 5-7 aren't
  reachable from the material, so the recorder reads all nine from D3D's
  copy of the GPU fetch constants, which is also exactly what the GPU
  samples (a sampler the setup doesn't touch keeps its previous texture,
  on the Xbox and here). That copy holds **physical** addresses, and our
  texture cache reads guest memory by virtual address. Physical memory is
  visible through three windows (`0xA0000000`, `0xC0000000`,
  `0xE0000000` − 4 KB), but only the one the game allocated through has
  its pages mapped for us, so `guest::ReadableAddressOfPhysical` tries each.
  Resolved textures are found by physical address anyway.
* **Nine texture bindings.** A draw's descriptor set had 5 textures and its
  parameter block at binding 5. Water needs 9, so every set now has
  textures 0-8 and the block at **binding 9** (all shaders updated). For
  the particle and water materials the binding number is the game's own
  sampler number. Each has its own 1024-byte block, `ParticleParams` and
  `WaterParams` (`frame.h`, `shaders/*_params.glsl`).
* **A new log line**, once per material, binding and format: `a <material>
  draw's texture N isn't available (format F, address A)`, for a texture
  the renderer couldn't get (unsupported format, unreadable memory). Such
  a draw is skipped, or for water's optional textures drawn with white.
* The periodic log line now counts draws "with a material block" (lit,
  shadow, particle, water) instead of "lit/shadow".

## 4. Checking it

![The waterfall area (name TBD): native left, emulated right](../images/waterfall-area-native-vs-emulated.jpg)

*The waterfall area (name TBD), the game's second area, on the same frame
from both renderers: the waterfalls are the water material, the glow above
the save totem (name TBD) is particles
(captured 2026-09-27, with depth of field, MSAA and
mipmaps native too).*

**Which pixels are which.** For one run the water's pixel shader returned
magenta and the particles' green (then reverted). In the waterfall area:
the two **waterfalls** are the water material (80,000 pixels), the glow
above the save totem and the splash at the top of the waterfall are
particles (13,000 pixels). The small blue sparkles (name TBD) floating
around that area and Wumpa Island are **not** particles: they're another material, and were
already drawn natively. Those two masks then measure the difference in the
same-frame A/B (findings/11, section 6) at that spot:

| Region | Mean difference | Signed (R, G, B) |
|---|---|---|
| Whole frame | 0.65 / 255 | |
| Water (the waterfalls) | 0.55 / 255 | +0.00, +0.04, +0.18 |
| Particles | 1.15 / 255 | −0.18, −0.25, −0.27 |

A few steps further, 0.63 / 255 for the whole frame.

**Wumpa Island.** Its water uses two materials, one with the
reflection on and one with it off (a temporary log line printed their
values: ReflectBumpiness 0.015, WaterBumpiness about 0.01, ReflectColor
white). At the river the emulated picture is blurred by depth of field,
which isn't drawn natively yet; after blurring both pictures by 3 pixels
the river differs by 0.68 / 255. The sea in the background shows the same
wobbly reflection of the trees in both.

**Nothing else moved.** The draw set's layout changed for every material,
so the title and menu were compared again, against a build of the previous
commit: menu 1.41 (before) / 1.42 (after), title 0.06 / 0.15 (it animates).
The 0.47 in older notes was measured on a different menu frame, so it isn't
comparable.

## 5. Not drawn natively yet

* **Depth of field** (`xnDOFShader`, inside `FramebufferFxExt::v30`): now
  the biggest visible difference in the hub (the emulated background is
  soft, ours sharp). It's also what makes comparisons in the distance
  unfair. (Done next: findings/15.)
* `xnFxGridShader`, `xnBumpMegaShader` (later levels), MSAA, mipmaps,
  render target 1 (the lit material's motion-blur velocity), the character
  refraction variant (b11), the unlit simple shader's point lights and Fx
  shadows (its literals differ: c255.w = 0.65, like the water's shadows).

## Reproduce

On a **copy** of the save data, loading a save in the waterfall area. The
Load Game list sometimes loads straight away and sometimes waits, so look
at the newest capture before each press:

```bash
S=<tmp>; cp -a user/saves $S/userdata
crash_mom --game_data_root=$PWD/game --user_data_root=$S/userdata --renderer=native \
  --readback_resolve=full --debug_capture_dir=$S/cap --debug_capture_interval_ms=5000 \
  --debug_native_ab_trigger=$S/ab.now --debug_input_fifo=$S/input.fifo \
  --debug_input_script="8000:start,9500:start,11000:start,12500:start,14000:start,17000:start" &
# ~24 s: main menu. Then, a few seconds apart:
echo down > $S/input.fifo; echo a > $S/input.fifo      # Load Game
# ~40 s: the save list; `echo down` to the save (yellow = selected), then:
echo a > $S/input.fifo
touch $S/ab.now                                        # ~20 s later: ab_<ms>_{native,emulated}.ppm
```

The two pixel shaders: add `--dump_shaders=$S/shaders` and match the dumps
to `game/shaders/particleshaderfp.out` / `waterspecularfp.out` (each dump's
32-bit words, byte-swapped, appear in the file). Game content: keep it
local.
