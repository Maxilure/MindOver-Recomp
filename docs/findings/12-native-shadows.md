# Findings: the characters' soft shadows drawn natively (milestone 4, part 3)

2026-09-26. After the characters
([findings/11](11-native-characters.md)), the most visible gap on Crash was
the soft dark patch under his feet. **Result:** it's drawn natively now, the
same shape, darkness and soft edges as the emulated picture of the same
frame. Getting there turned up two bugs that had been hiding since
milestone 3 and 4: blend factors read in the wrong numbering, and the front
and back faces swapped for two-sided stencil.

## 1. How the game draws the shadow

From a trace of a hub frame (the loop at `0x82394D88`, then `0x8238E3C0`):

1. **Shadow volumes.** For each shadow-casting character the CPU builds a
   volume: the character's silhouette stretched along the light direction,
   two triangle lists drawn with the simple material. Colour writes off, depth
   writes off, culling off, **two-sided stencil**: front faces increment,
   back faces decrement (wrapping). Where the volume's front is in front of
   the ground but its back behind it, the stencil ends up non-zero: that's
   the shadow's shape on the ground.
2. **The shadow quad** (`xnShadowShader`, vtable `0x82018C4C`, draw setup
   `0x82448E90`). The setup first **resolves the depth/stencil surface** into
   a texture (D3D `Resolve` with flags `0x14`: `0x4` = the depth/stencil
   surface; three strips while tiling), then draws one full-screen quad.
   Its pixel shader (microcode only; its constant table names "SampleOffsets",
   "SampleWeights", "StencilBuffer") looks at 9 points around the pixel in
   that texture (offsets c2-c10, about ±2 pixels), and for each one inside the
   shadow adds weight × (1 − vertex colour) of darkness; the result,
   1 − darkness, is **multiplied** onto the scene (blend: destination colour ×
   source, zero). Near the shadow's edge only some of the 9 points are
   inside, so the edge fades: a soft shadow from a hard stencil mask.

The vertex shader (`shadowshadervp`, debug file available) just passes the
quad's 0..1 position through as texture coordinates.

## 2. Which part of the texture is the stencil

The resolve writes the depth/stencil surface's 32-bit words (24-bit float
depth + 8-bit stencil) into a texture declared as `k_24_8_FLOAT`. But the
shadow material samples a **second texture object aliasing the same
memory**, declared `k_8_8_8_8` with a blue/red swap in its fetch swizzle.
After the Xbox's 32-bit byte swap, the stencil is the word's lowest byte,
the first colour channel; the swizzle hands it to the shader as `.z`, the
component the shader tests. So the shader really reads the stencil, as its
name for the texture says.

(Worth knowing: the resolve's destination holds a **virtual** address, D3D's
copy of the sampler state a **physical** one: `0xF4B9D000` virtual is
`0x14B9E000` physical, the `0xE0000000` window being 4 KB off. The render
targets now key resolved images by physical address, `guest::PhysicalAddress`.)

On our side a depth/stencil resolve fills two images per destination: the
depth as R32 float and the stencil as R8 (stencil / 255, sampled bilinearly
like the game's sampler; "> 0" = inside). Vulkan can't copy a depth/stencil
image into a colour image, so each aspect goes through a staging buffer.
The depth copy is ready for the soft particles and the water, which read
depth from the same kind of resolve.

## 3. Bug 1: blend factors were in the wrong numbering

The shadow's blend state didn't make sense: the material's state cache said
source factor 4 (source colour), but D3D was called with 8 (destination
colour). The xn shaders' common state function looks the cached value up in
a **table at `0x824EE7D8`** before calling D3D: the cache holds PDDI's
numbering. Table: `0→0, 1→1, 2→4, 3→5, 4→8, 5→9, 6→6, 7→7, 8→10, 9→11,
10→16`. The four values the menus and the world use (zero, one, source
alpha, one minus source alpha) map to themselves, which is why this went
unnoticed since milestone 3; every "multiply" blend was wrong. The recorder
now translates through the table (`shader::kBlendFactorTable`).

## 4. Bug 2: front and back faces were swapped

With all of that in place, our shadow had a **hard outer edge** where the
emulated one faded out softly on both sides. The shadow quad keeps the
stencil test from the volumes' setup ("only where stencil > 0") so our quad
never drew outside the shadow, while the emulator's clearly did.

Experiments first: skipping the depth of field in both renderers
(it softens everything a little) left the difference; turning the stencil
test off for our quad made it match. Then the facts, from the SDK's
per-draw GPU log (`patches/rexglue-sdk/debug/0100`) for the shadow quad:
`RB_DEPTHCONTROL = 0x1C7249B1`: stencil **on**, depth test off, and
**two-sided stencil still on** from the volumes, with the back-face test
"always" (the volumes never set a back-face compare).

So the quad is a **back face** on the Xbox. Why: the Xbox's D3D cull values
are the GPU's `PA_SU_SC_MODE_CNTL` bits: bit 1 "cull back faces", bit 2
"clockwise is the front". Cull mode 0 (off) and 2 mean **counter-clockwise
is the front**; only 6 makes clockwise the front. We had always taken
clockwise as the front. Culling still came out right (2 and 6 remove the
same triangles either way), but two-sided stencil used the wrong half. The
volumes didn't show it (+1 or −1, both mark the shadow); the quad did:
it's clockwise, so with culling off it's a back face, its stencil test is
"always", and the blur softens the edge outwards too. The renderer now
derives Vulkan's front face from the cull mode (`ToFacing`), like the GPU.

Our recorder's copy of the two-sided state was right all along: in the hub
it matches the register exactly.

## 5. Results (same-frame A/B, section 6 of findings/11)

| Frame | Mean difference |
|---|---|
| Intro movie | 0.00 / 255 |
| Title, logo fully shown | 1.4 / 255 (see below) |
| Main menu | 0.47 / 255 |
| Hub, whole frame | 2.3 / 255 |
| Hub, around Crash | 1.7-1.9 / 255 |

The shadow patches now agree in shape, darkness and softness. What remains
around Crash is the depth-of-field blur (not drawn yet), anti-aliasing
(MSAA) and texture detail (mipmaps).

**A correction to findings/11:** "title identical" there came from a capture
during the title's fade-in, when the screen is almost black. With the logo
fully shown the title differs by about 1.3-1.4 / 255, only along the logo's
edges (the emulated edges are a little softer; no sub-pixel shift fits
better). The previous commit shows the same, so it isn't new. Likely MSAA
or texture filtering; open.

## 6. Not drawn natively yet

`xnReflectShader` (done since, findings/13), water (`xnWaterSpecularShader`), `xnParticleShader`,
`xnDOFShader` (depth of field), render target 1 (the lit material's motion
blur velocity), MSAA, mipmaps, the character material's refraction variant,
the unlit simple material's point lights and Fx shadows.

## Reproduce

Same as findings/11 (same-frame A/B at 55 s in the hub). The shadow quad's
GPU state: apply `patches/rexglue-sdk/debug/0100-*`, rebuild the SDK, run
with `CRASHMOM_DIAG_FRAMES=58000`, look for `PS=15A32D06C1982433`; remove
the patch afterwards.
