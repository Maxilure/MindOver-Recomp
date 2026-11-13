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

## 9. Small leaves don't pay on their own

The byte-for-byte check moved to src/decomp/check.h (shared by every
rewrite: the original on a copy of the registers, ours on the same input,
written bytes and r3 compared, the original's results kept; log lines
"Native check:").

D3D's SetPixelShaderConstantB (sub_82312B58, 24 instructions, ~200,000
calls a second uncapped, 0.26% of the main thread) was rewritten the same way
and checked (856,000 calls, 0 differences), then measured: ~19 ns a call
either way, its share 0.26% -> 0.41%. For a leaf that small the call itself
(the PDDI intercept wrapper, the handler, the switch reads) is the whole
cost, so it was taken out again. Such leaves only pay inside a bigger native
caller, where their few instructions are inlined: they belong with the
material set-ups that call them, not alone.

What is left of the back end's own code after section 8 (Wumpa Island,
uncapped): ~6% of the main thread in Radical's renderer code and ~3% in D3D,
spread over some 30 functions of 0.1-0.8% each. The largest: the tint and
light writer sub_8242FF88 (0.8%, 1,656 bytes, eight virtual calls into the
lighting objects), DrawPrimBuffer's own code (0.4%), the PS-bool setter
(0.4%), the common material state sub_8242DA28 (0.3% own, 0.7% inclusive:
blend, alpha, depth bias, shadow spheres), the simple set-up's own code
(0.25%), SetTexture (0.2%).

Next (option 1 continued): the material set-ups as WHOLE native functions
with their D3D leaves inlined (slot 14, the simple material first: 1.8%
inclusive with its common state and leaves), then the tint and light writer.

## 10. The simple material's set-up (src/decomp/simple_setup.cpp)

xnSimpleShader::SetupDraw (sub_82441EA8, 377 instructions, slot 14 of the
simple material: most of the world, the HUD, the menus) now runs as C++,
written from xexdis next to Ghidra's pseudo-C. It is reached through the one
PDDI handler that function already had (players/outfits.cpp, the hair and
floating-button texture swaps), which now calls it instead of the original.

What it writes, in the original's order: the state cache reset, the sampler 0
texture, the vertex and pixel shaders (table 0x82597010, by the +56 byte and
"palettized" = texture +276 == 1), PS bools b80 / b81 (+24 == 2 / 3), PS
c66-c69 (the four ARGB colour words / 255), c70 (+80..+92 raw), c4 (x =
context +364 x +88), c22 (x = +-1 by the texture), c52 (the camera
position), sampler 0's filter and address mode, the common material state
(sub_8242DA28), then for palettized textures the palette in sampler 8 and
c124 = (width, height, 0, 0), else sampler 8 cleared. Inlined: the
palettized test, the texture pick of sub_8243AD80 / sub_8243AE20, D3D's
SetPixelShaderConstantB, the camera copy sub_824339A0, the filter setter
sub_8242D760 and the three virtual getters (context slot 99 = the float at
+364, texture slots 5 / 6 = +288 / +292; any other function there = the
original runs). Still the game's code, called: D3D's SetTexture,
SetVertexShader, SetPixelShader, the address-mode setter sub_8242D810 and
the common state.

### 10.1 Stack leftovers, all the way down

Being byte-for-byte exact meant reproducing more than the visible results:

* the original reads words of its own frame it never writes (r1+100/104/108
  into PS c4.yzw / c22.y, r1+172 into c52.w), like UpdateTransforms' c8.w:
  read from the same guest addresses;
* the common state copies eight rows from ITS stack frame into PS
  c201-c208: past the active shadow spheres those rows are leftovers, and
  they include the registers the earlier helpers SAVED there (r27-r31 of the
  set-up and the return address). So each helper is called with the
  registers the original had at that point (r27 = 1, r28 = 0, r29 =
  palettized, r30 = the caller's, then 0xFFFFFFFF82597010 (lis + addi,
  sign-extended), then (+68 >> 8) & 0xFF, r31 = the shader, lr = the
  address after the original's `bl`), with r1 = the original's frame;
* the original's own frame writes (its prologue's saved registers and back
  chain, its integer-to-float temporaries) are written as it leaves them, so
  later code at that depth finds the same leftovers.

None of these words is read by a shader that the picture depends on, but the
check found each of them.

Registers: r1, r27-r31 and lr are put back as the epilogue does; scratch
registers aren't reproduced (all 36 slot-14 virtual call sites in
0x823F0000-0x82460000 were scanned: none reads one after the call before
writing it).

### 10.2 Checking a function that calls D3D

The shared checker (decomp/check.h) runs the original, puts the compared
memory back, runs ours and compares. With D3D calls inside, three things had
to change:

* the stack below the caller's frame is put back too, without being compared
  (`Range::compare = false`): the original's run changes the leftovers ours
  reads;
* D3D's device has a window, +10896..+11600, that other threads and the GPU
  side change while the function runs (+10904 = the lock of D3D's allocator,
  sub_82308798; +10988; +11540 a counter; +11556 a pointer moving megabytes
  ahead). Putting a snapshot of it back froze the game: the main thread then
  waited in D3D's swap (sub_82310DA8 -> sub_8230A158) for a fence that never
  came. The window is left out; the set-up only reads there;
* a setter that replaces an object the GPU still uses writes a fence into
  D3D's command ring and may kick it (sub_82307D08, allocations outside the
  device). Running that twice is unsafe, so such calls aren't checked (the
  same "+10908 == 0 and +10912 & object" test the setters make, done up
  front); none occurred in the runs below.

Up to six ranges per check now.

Results: `--debug_native_simple_check`, Wumpa Island, 30 fps and uncapped,
walking, attacking, the pause menu: 0 differences in 504,074 calls (uncapped
run) and 44,525 (capped run). Positive control: one flipped bit of PS c4.x
made every call report a difference.

### 10.3 Measured

Uncapped, Native Only, three pairs of 8 s profiles with the switch flipped
live: the set-up's inclusive share of the main thread 2.11 / 2.01 / 2.27% ->
1.80 / 1.71 / 1.69%; game time per frame within noise (4.37-4.60 ms either
way). A first version that copied the whole register context for each helper
call (vector registers included) cost as much as it saved; helpers are now
called on the live context. The real gain needs the remaining helpers
inlined (SetTexture / SetVertexShader / SetPixelShader without their ring
paths, sub_8242D810, the common state): the next step.

## 11. The set-up's helpers inlined

The rest of what the simple set-up calls is now C++ in the same file: D3D's
SetTexture, SetVertexShader and SetPixelShader (their constant blocks:
copies through memcpy and and / or patches into the device's fetch
constants), the address-mode setter sub_8242D810 with D3D's two sampler
leaves (sub_8230C338 / sub_8230C4E0: same code, other bit fields), and the
common material state sub_8242DA28: blend (sub_8242D8C8 and D3D's
sub_8230AAC8 / AB58 / ABE8 / AC78, which fold the blend word into four
render-target words), alpha test (sub_8242D980, sub_8230AA90 / AF50 / AEF0),
the +84 word (sub_8230AA30 / sub_82430970), the eight shadow-sphere records
(sub_8237F378) and SetPixelShaderConstantF (sub_82312A78, an unaligned
vector copy of 8 rows). About 20 functions, ~1,000 instructions,
transliterated with rlwinm / rlwimi helpers (wrapped masks included) and the
recompiled code's `fctidz` (NaN -> 0x8000000000000000, overflow clamped).

Only one path still runs the game's code: a setter that replaces an object
the GPU still uses (device +10908 == 0 and +10912 & the object's first word)
writes a fence into D3D's command ring and may kick it; such a call goes to
the game's function, decided up front with the setter's own test.

Each inlined helper also writes what it leaves on the guest stack, in the
original's order: savegpr's registers (r20-r31 for SetTexture, r28-r31 for
most, r29-r31 for the leaves) and return address, the back chain, memcpy's
`std r3,-8(r1)`, SetAlphaRef's `std r11,-16(r1)`, the common state's own
temporaries and sphere copies, SetPixelShaderConstantF's `std r31,-8(r1)`.
That is what PS c202-c210 are copied from.

Checked with `--debug_native_simple_check`: 557,324 calls uncapped, 0
differences. New `--debug_native_simple_check_stack` compares the 4 KB of
guest stack below the caller's frame too: 31,435 calls, 0 differences, so
every saved register, return address and temporary matches. Positive
control: one changed back-chain word made every call differ.

Measured (uncapped, Native Only, three pairs of 8 s profiles, the switch
flipped live): the set-up's inclusive share of the main thread 2.57 / 2.21 /
2.40% (original) -> 1.14 / 1.07 / 1.08% (C++). Frame rate 187-197 fps with
the original, 194-197 with ours: within the run-to-run noise at this size.

## 12. The tint and light writer (src/decomp/light_constants.cpp)

Before every draw (BeginPrims sub_82430780, BeginIndexedPrims sub_824307F8,
DrawPrimBuffer sub_82430878), xnContext calls sub_8242FF88 (414
instructions): the largest piece of the back end's own code left after
section 11. It is now C++ as well, written from xexdis. Its five virtual
calls all land in one-to-three-instruction leaves (read from the vtables of
xnContext 0x82016744 and xnExtUnLitColourTint 0x8201672C), so they are
inlined, each behind a check that the slot still holds that function; so is
SetPixelShaderConstantB (sub_82312B58, no frame of its own).

What it writes:

| Constant | From |
|---|---|
| VS c100 | the unlit tint: xnExtUnLitColourTint (context slot 91 with 263 = *(ctx +1208)), when enabled (byte +12) its ARGB word +8 x 1/128, else (1, 1, 1); w = a never-written word of its own frame (r1+364) |
| PS c8 | ambient: *( *(ctx +144) +996) / 255 |
| PS b0-b7, c9-c24 | eight directional light slots: records *(ctx +144) + 124 i, one "changed" byte each at ctx +944 + i |
| PS c30-c41 | up to four point lights (record type 3): position, colour / 255, four parameters |
| PS c29 | (point light count, three never-written frame words r1+132..140) |

The lights part runs only when the "lights changed" byte ctx +952 is set
(about a third of the calls on Wumpa Island). Two details of the original,
kept as they are: only CHANGED directional slots are visited, and they are
numbered in the order visited (a slot that didn't change doesn't move the
numbering on); the point lights are gathered from all eight records,
changed or not.

Exactness follows section 10: byte / 255 and / 128 as one float multiply,
words moved with lfs / stfs through double, the prologue's saves (r17-r31,
f27-f31, the return address, the back chain) and every temporary written to
the frame as the original leaves them, r3 as it leaves it (six different
endings, by path).

Checked with `--debug_native_light_check` (original + ours, compared: the
device's first 10,136 bytes, the context's nine "changed" bytes, the whole
544-byte frame): Wumpa Island, the ice courtyard on a titan, a jacked
Ratcicle: 900,000+ calls, 0 differences. No scene reached in testing had a
point light on, so a temporary test switch turned 0-7 records into point
lights with random contents (and random "changed" bytes) before each checked
call: 131,072 calls, 400,265 point lights written, 0 differences (switch
removed). Positive control: 1.0 added to PS c8.x made every lights call
differ. The check also logs what it covered ("covered N calls, N with lights
changed, N directional, N point").

Measured (uncapped, Native Only, Wumpa Island, three pairs of 8 s profiles,
the switch `native_light_constants` flipped live): its inclusive share of
the main thread 1.14 / 0.95 / 0.79% (original) -> 0.38 / 0.33 / 0.36% (C++).
Frame rate 226-247 fps either way: within the noise at this size.

## 13. The vertex stream writer (src/decomp/vertex_streams.cpp)

A function-by-function profile after section 12 put the largest piece of
the renderer's own code left at sub_82439A48: ~3% of the main thread
uncapped on Wumpa Island, all of it its own loop. It is one of a family of
xnPrimBufferStream writers (0x824398E8, 0x82439A48, 0x82439B88,
0x82439CC0: one per vertex format, picked by sub_823E1780 from which
arrays exist) and the one used in the scenes profiled: position, normal,
binormal and tangent (3 floats each) and a uv (2 floats) per vertex, about
2,300 vertices a call, ~900 calls a second uncapped. The stream object:
+0 stride, +4 / +8 / +12 / +16 / +24 the next position / normal / binormal
/ tangent / uv, +28 the count written. The original re-reads each column
pointer from the stream before every one of the 14 stores of a vertex and
moves all five on by the stride.

The C++ copies the same words in the same order, with two decisions up
front that send a call back to the original: a destination column that
overlaps a source array or the stream object (then the original's re-reads
would see its own stores), or a range that wraps or crosses 0xE0000000
(the Windows host window shift). Destination columns overlapping each
other (one interleaved vertex) are fine: stores keep the original's order.
In all test runs no call needed the original.

Each float goes through lfs / stfs, which in the recompiled code is float
-> double -> float: every value survives except a signalling NaN, which
comes out quiet. Copied as raw words with that bit set for any NaN (a
quiet one already has it), branch-free over the vertex's 14 words.

**Registers.** Its one caller tail-returns into a function reached through
a vtable, so a scan can't show that no caller reads a scratch register
afterwards (the argument used for the set-ups). So all of them are left as
the original leaves them: the loop's walkers r5-r11, r12 and lr from
restgpr, the last vertex's 14 floats in f0-f13 (as lfs loaded them), cr0,
cr6 and the carry. decomp/check.h gained `Ranges::registers`: the check
then compares every guest-visible register (r0-r31, f0-f31, cr0-cr7, xer,
lr, ctr), not only r3.

Checked with `--debug_native_vertex_check` (memory: the stream object, the
saved registers below r1 and the span the five columns cover; every
register): Wumpa Island, the titan save, another level: ~12,400 calls, ~25
million vertices, 0 differences, every call handled by ours ("covered" log
line: handled / left to the original / vertices). Positive control: one
changed word below r1 made every call differ.

Measured (uncapped, Native Only, three pairs of 8 s profiles, the switch
`native_vertex_streams` flipped live): 2.80 / 3.04 / 2.79% (original) ->
1.78 / 1.60 / 1.86% (C++). A first version that checked each float for a
signalling NaN with a branch saved only a third as much (2.2 / 1.8 / 2.8%).
