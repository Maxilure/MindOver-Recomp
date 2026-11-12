# 60. Radical's PDDI Xbox back end in Native Only: what it really costs; the native back end started (matrix helpers, UpdateTransforms, memcpy as bit-identical C++)

Findings/59 s.5 left one piece of the Xbox graphics stack running in Native
Only: Radical's PDDI Xbox back end (the `xn*` classes), which computes every
draw's matrices, material constants and sampler states and stores them in
Direct3D's copy of the GPU state (the "device copy", findings/09), where the
native renderer's recorder reads them. The planned next step was to have the
back end hand that state to the native renderer directly, material by
material, so that no Xbox Direct3D code runs at all. This page measures what
that step is worth before building it.

## 1. A profile appendix for exact sums

Console `profile` (src/debug/slow_load_sampler.*, findings/57) only printed
the top 60 / 80 functions. Its report now ends with an appendix:

```
== ALL GAME FUNCTIONS (address, own code, self, inclusive; sample counts) ==
sub_82424358 251 263 304
```

per game function seen: samples with the CPU in that function's own
instructions ("own code"), samples where it was the innermost game function
(host code it called included: "self"), and samples with it anywhere on the
stack ("inclusive"). Any address range or function list can then be summed
afterwards. The classification below used the call graph
(tools/callgraph.py): renderer = every PDDI method in src/pddi/functions.inc
and every `xn*Shader` draw set-up (vtable slot 14), plus everything they
reach inside the renderer block (0x82420000-0x82460000) and Direct3D.

## 2. Measured

Wumpa Island, Native Only, 720p, uncapped (vsync off), 15 s, 14,177 samples,
219 fps, game time 3.70 ms per frame:

| | share of the main thread |
|---|---|
| renderer block + Direct3D code reached from the renderer, own code | 13.3% + 3.1% |
| of which the game's `memcpy` (sub_8245C1A0, also used everywhere else) | 3.6% |
| of which two matrix functions (sub_82424358 4x4 multiply, sub_824241D0 affine multiply) | 3.3% |
| renderer block code NOT reached from the renderer (the same math library, called by game logic) | 7.3% |
| `xnContext::UpdateTransforms` (sub_82431730), inclusive | 4.1% |
| all material draw set-ups (slot 14), inclusive | 2.7% (simple material 1.75%) |

The BY AREA line of findings/59 ("PDDI Xbox back end, 20% own code")
overstated the renderer: the 0x82420000-0x82460000 block also holds
Radical's math library (matrices, vectors), and `SwapBuffers` (sub_82433510,
19% self) is the native renderer's own frame end running under it.

So the whole back end, material set-ups, transforms and Direct3D state
writes together, is roughly 10% of the main thread uncapped (about 0.35 ms of
a 4.5 ms frame). At a 60 fps cap the main thread already waits most of the
frame: no visible gain there.

## 3. Who writes what the recorder reads (console `who`)

Each value ReadMaterial (src/native/recorder.cpp) takes from the device copy
(device 0x40005200: VS constant N at +1920 + 16N, PS constant N at +5984 +
16N), with the console's write watch, Wumpa Island:

| value | writer |
|---|---|
| VS c0-c3, c4-c7 (MVP), c12-c15 (world), c16-c19, c8-c9, PS c128-c129 | `xnContext::UpdateTransforms` sub_82431730, through D3D's SetVertexShaderConstantF sub_82312998 (only when the context's "transforms changed" byte at +0x104C4 is set) |
| VS c100 (tint) | sub_8242FF88 (called from BeginPrims / BeginIndexedPrims / DrawPrimBuffer), direct stores |
| PS c2 (fade) | each material's set-up (simple sub_82441EA8 +0x440, particle, character, reflect, water, DOF), direct stores |
| PS c20 (textured), c50, c64-c69 | the simple set-up, direct stores |
| shadow PS constants | the shadow set-up through sub_82312A78 |

The set-ups and UpdateTransforms store straight into the device copy and set
its dirty bits; the only D3D functions they call are small `Set...` helpers.
Nothing Microsoft-specific happens there beyond memory writes.

`UpdateTransforms` itself: copies the world matrix from the context's stack
(+0x20: base +0xC, depth +8, 64 bytes each), multiplies world x view (+0x34)
x projection (+0x3BC / +0x3FC), inverts with sub_82424618, normalises the 3x3
with sub_82423C78, and stores the camera position / direction rows. VS c8.w
is copied from a stack slot the function never writes (whatever an earlier
call left there): nothing reads it, but a byte-for-byte check has to skip it.

## 4. Where the Xbox video-driver calls come from

The Xbox calls meter (console `xbox`) in Native Only, Wumpa Island: per frame
only VdSwap, VdGetSystemCommandBuffer and VdRetrainEDRAM, all from
Direct3D's swap (sub_82310DA8, called by PDDI's SwapBuffers). The material
set-ups call none. Ending the video-driver calls is therefore a separate
step: Direct3D's swap and command-buffer kicks, which also drive the fences
D3D's vertex allocator (sub_82308B60) waits on before reusing memory.

## 5. Options

1. **Native back end, bit-identical**: C++ versions of UpdateTransforms, the
   math library functions it uses and each material's set-up, writing exactly
   what the originals write (the device copy stays the recorder's source, so
   F9 and dual mode keep working), each checked against the original with a
   byte-for-byte compare like the skinning (findings/57). Gain: up to ~10% of
   the main thread uncapped; the math library also speeds up game logic.
2. **Direct feed**: the same C++ hands its values to the recorder and the
   device copy is no longer written in Native Only. Same gain as 1, more
   risk (every reader in ReadMaterial needs a new source, and the emulated
   picture needs the copy rebuilt on F9).
3. **No Xbox video driver**: Direct3D's swap skipped in Native Only with the
   fences answered by the port; independent of 1 and 2.

Option 1 was chosen. Sections 6-8 are its first steps.

## 6. The matrix helpers and UpdateTransforms (src/decomp/matrices.*)

Replaced, each as C++ that writes exactly what the original writes:

| function | what it does |
|---|---|
| sub_82424358 | out = a x b, 4x4 (through a temporary: out may be a or b) |
| sub_824241D0 | out = a x b for affine matrices (column 3 = 0 0 0 1) |
| sub_82424618 | in place: general 3x4 inverse, or the rigid one when the 3x3's determinant is within 1e-5 of 1 (a NaN takes the general way) |
| sub_82423BB8 | in place: rigid inverse (transpose the 3x3, translation = -(t x R^T)) |
| sub_82423C78 | in place: orthonormalise rows 0-2 (normalise row 0, row 2 = row 0 x row 1 normalised, row 1 = row 2 x row 0) |
| sub_82424880 | v / \|v\| (no zero check) |
| sub_82431730 | xnContext::UpdateTransforms (section 3), a PDDI intercept handler since the intercept layer already wraps it |

How the bodies were written: a small translator turned each straight-line
stretch of the disassembly into C++ statement by statement (each float
register a local of the same name, loads and stores in the original's order,
`fmadds a,c,b` = `float(fma(double a, double c, double b))` exactly as the
recompiled code computes it, `fneg` = sign bit flip, the |x| through the
stack = sign bit cleared); branches and loops written by hand around them.
The same template bodies run on guest memory (the overrides) and on a host
array laid out like the original's stack frame (inside the native
UpdateTransforms). Two builds of the maths, with the CPU's FMA instruction
and without (decomp/cpu_features.h, shared with the skinning), picked once;
the bodies are always-inline so each build holds all of its maths (clang's
`flatten` doesn't inline two levels down: the first build still called the
library's fma from inside UpdateTransforms).

Registers: r3 is left as the original leaves it (UpdateTransforms: the last
dirty-mask value, Orthonormalise: its stack vector's address). The
floating-point and integer scratch registers are not reproduced: over all
278 call sites of the six helpers, no caller reads a scratch register (f0-f13,
r0, r4-r12) after the call before writing it (a scan of each caller's
instructions up to its next branch).

VS c8.w (section 3) is copied from the same guest stack word the original
reads.

Checked with `--debug_native_matrix_check` (each call: the original on a
copy of the registers, its written bytes kept, ours on the same input,
compared byte for byte with r3; the original's results stay): Wumpa Island,
walking and fighting, 0 differences in about 2 million calls per run
(165,000 multiplies, 420,000 affine multiplies, 91,000 UpdateTransforms...),
with the FMA build and without (`--debug_native_matrix_no_fma`). Positive
control: one flipped bit in our multiply made every multiply and half of the
UpdateTransforms calls (the ones whose matrices changed) report a
difference.

## 7. The game's memcpy (src/decomp/memory_copy.cpp)

sub_8245C1A0 is the runtime's memcpy: a forward copy in bytes, 8-byte words
and 32 / 128-byte blocks, cache hints only (no `dcbz`, so nothing outside
the range is touched), returning the destination. For buffers that don't
overlap any forward copy leaves the same bytes, so those calls are one host
memcpy. Overlapping calls (their result depends on the block order), ranges
crossing 0xE0000000 (the Windows +0x1000 shift) and ranges wrapping past 4
GB run the original. `--debug_native_memcpy_check`: 4.2 million calls (537
MB) through boot, a level and the pause menu, 0 differences; 111 calls
overlapped and ran the original.

## 8. Measured

One run, Wumpa Island, Native Only, uncapped, the switches flipped live
(console `flag native_matrices` / `flag native_memcpy`), three pairs of 8 s
profiles:

| | game time per frame | frame rate |
|---|---|---|
| originals | 3.88 / 3.78 / 3.70 ms | 209 / 214 / 218 fps |
| C++ | 3.35 / 3.49 / 3.40 ms | 236 / 228 / 233 fps |

About 10% of the main thread. Inclusive shares: UpdateTransforms 4.5% ->
0.9%, the two multiplies 5.5% -> 1.0%, memcpy 4.0% -> 1.1%. Boot, intro
movies, title, main menu, Load Game and play checked with the C++ path on:
0 errors.

Next (option 1 continued): the material set-ups (slot 14, the simple
material first: 1.8% inclusive), the tint writer sub_8242FF88 (0.9%), then
the D3D `Set...` helpers they call.
