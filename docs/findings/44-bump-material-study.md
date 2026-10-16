# 44. The bump material, feature by feature

2026-10-16. `xnBumpMegaShader` ("bump mega") was written natively in
findings/20 and checked in small boxes around crystals and ice. This is the
full study: what the material is, which of its features the game actually
uses, and how each one compares with the emulated picture, measured only
where the material is drawn.

## 1. What it is

"Bump" = **normal mapping**: a second texture (the normal map) tilts the
surface direction pixel by pixel, so a flat polygon catches light as if it
had grooves and bumps. "Mega" = Radical's do-everything version: on top of
the bumps it can reflect a picture, show what's behind it (see-through,
"refraction"), glow at its silhouette (rim), take palettized textures,
receive the small blob shadows and fog. Each of those is switched by one
pixel shader bool (b10-b16, b69; frame.h `kBumpMega*`). It draws the
see-through ice of the Ratcicle Kingdom and the Ice Prison (the statue,
crystals, frozen shapes) and the glowing crystals of the TK blocks.

## 2. Which features the game uses

The photo draw lists now print every bump draw's switches and numbers
(`| bump flags 0x.. rim refract fog fxshadows N | refract .. | reflect .. |
rim ..`). Saves checked (the starting spot of each, frozen): Ratcicle
Kingdom courtyard, two Ice Prison spots, Wumpa Island, the mountain path,
the way to the Ratcicle Kingdom, the desert and a metal corridor.

| Where | Bump draws | Features |
|---|---|---|
| Ratcicle Kingdom courtyard | 19 | rim + see-through + fog |
| Ice Prison (two spots) | 1 / 17 | rim + see-through + fog |
| the other spots | 0 | - |

Every bump draw found uses the same three features. Reflection, the
reflection mask, palettized textures and "see-through x colour" never came
up.

## 3. Measuring only the bump material

- **Mask**: F11 paints the bump material magenta in the native picture.
  Freeze the game (F5), take a photo pair, press F11, take another: the
  pixels that turned magenta are exactly the material's.
- **Both-pictures switch**: `--debug_bump_off=10,11` (or the console's
  `flag debug_bump_off 11`, live) clears those bools right after the game's
  own bump setup, in D3D's copy of the constants, and marks them dirty
  (bit 56 of device +0x20, what SetPixelShaderConstantB 0x82312B58 does).
  The emulated GPU uploads them at the draw and our recorder reads them, so
  BOTH pictures lose the feature. Checked: with see-through off, the
  emulated picture changed by 53.4/255 inside the mask and ours by 53.5;
  with the rim off, 18.7 and 18.9. Our see-through and rim have the game's
  strength.

## 4. Results

Measured with Texture Sharpness at Original (`--texture_sharpness=0`). A
first round ran with Sharpest, which keeps OUR textures crisper on purpose
(findings/36) and doubled every textured and edge difference; those
numbers are not used here.

Ratcicle Kingdom courtyard (the statue + ice shapes, 4% of the screen):

| Part | Native vs emulated (/255) |
|---|---|
| bump interior, away from any edge | 0.44 |
| flat areas of everything else | 0.16 |
| bump outlines | 3.15 |
| outlines of everything else | 2.99 |

Ice Prison: interior 0.37-0.60, outlines 4.65 (other outlines 2.64).

Compared at the **same contrast** (outlines sorted by how different the
two sides are), the courtyard's bump outlines match the other outlines
within noise. In the Ice Prison only the very highest-contrast ones (dark
ice against white snow, under 700 pixels) are about 2x the others. With
see-through switched off in both pictures the bump outline excess goes
away. So:

- Every feature in use matches the game: texture x vertex colour, normal
  map, shine, rim, see-through, fog.
- What remains is the edge difference EVERY object has (section 6), a
  little stronger where the see-through effect shows a pushed copy of the
  picture right up to the ice's silhouette.

## 5. The edge difference of every object (open)

Outlines differ by about 0.06-0.1 x their contrast between the two
renderers (15/255 on the sharpest edges), for every object, not only the
bump material. Ruled out:

- A shift of the picture: a sub-pixel registration by gradients suggested
  ~0.2 px, but a blur-free (frequency domain) shift search finds the best
  match at no shift at all; the gradient method was fooled by the edges
  themselves.
- Depth of field and the native anti-aliasing level (1x / 2x / 4x: 2x
  matches best).
- The render target format: the scene surface is format 0 (8-bit RGBA, no
  gamma; render_targets.cpp now logs each colour format once).
- Our later passes: with the emulator's own scene copies fed into our
  post passes (`--debug_native_emulated_resolves`), the difference moves
  with the 3D drawing, not the passes after it.

Candidate not confirmed: SAMPLE POSITIONS. The emulator's fragment-shader-
interlock path draws the Xbox's 2x as "2x as 4x" (render_target_cache.cpp:
"parity with D3D12 ROV"): host 4x samples 0 and 3, at (-0.125, -0.375) and
(0.125, 0.375) from the pixel centre (nearly vertical, like the Xbox). Ours
use Vulkan's standard 2x pair at (+-0.25, +-0.25), one diagonal. That
predicts our worst edges along ONE diagonal and a big difference on
horizontal edges; binning the edge error by direction shows the opposite
(both diagonals alike, horizontal edges the best).

**Tried anyway** (a temporary switch, since removed): the game's 2x
surfaces become 4x images, every 3D pipeline drawing into
them gets the sample mask 0b1001 (samples 0 and 3 only), their colour
resolves average those two samples (shaders/colour_resolve_2of4.frag: the
GPU's own resolve would average all four), and depth resolves read sample 0
(the Xbox's sample 1 = Vulkan's sample 3). Same frozen spots, Texture
Sharpness at Original:

| Edges (courtyard) | standard 2x | 2x as 4x |
|---|---|---|
| contrast 20-50 | 1.96 | 1.84 |
| 50-100 | 5.33 | 4.95 |
| 100-160 | 11.72 | 11.04 |
| 160+ | 15.36 | 14.54 |
| 160+, bump outlines | 15.23 | 12.24 |

Ice Prison: the sharpest bump outlines 19.3 -> 15.4; whole frames 0.65 ->
0.63 and 0.43 -> 0.42. So the sample positions explain a small part (5-20%)
of the edge difference, not most of it. Cost, native only at 60 fps in the
courtyard: GPU 37% at 1441 MHz -> 39% at 1870 MHz (the card clocked up:
about a third more work). Not worth it: the experiment was taken out
again. Rebuilding it = the four pieces above (4x for the game's 2x
surfaces, mask 0b1001 in the scene pipelines, a two-sample colour resolve
pass, depth sample 0).

While building it: the renderer kept the last bound pipeline across a
resolve, and a resolve that runs its own pass (the depth copy, this one)
binds a different one; the next draw, if it used the same pipeline as the
one before the resolve, ran with the resolve's. Fixed (`bound` is forgotten
after every resolve, native_renderer.cpp); with standard 2x the numbers are
the same before and after.

## 6. Tools added

- Draw lists: the bump line above.
- `debug_bump_off`: the both-pictures switch (spotter.cpp).
- F11 got a fourth step, **motion trails** magenta (findings/42; the simple
  shader's `kFlagHighlight`), for masks of the attack trails.
