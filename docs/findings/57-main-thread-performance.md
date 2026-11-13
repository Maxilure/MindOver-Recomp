# 57. The game's main thread: where its time goes, and the first cuts

## 1. The report

A log from a PC with a 2-core / 4-thread low-power CPU, playing at a 60 fps
cap with streaming software running, showed what the frame-rate lines call
**CPU-bound**:

| stretch | fps | main thread per frame |
|---|---|---|
| calm areas | 59-60 | game 2.7-11 ms |
| a busy fight (~420 draws per frame), over a minute | 37-46 | game 18-22 ms, our frame end 4.3 ms |

"Waiting for the GPU" stayed at ~0 ms and the sound had no gaps: the game's
main thread simply needed more than 16.7 ms per frame. The same scene on a
desktop CPU needs 3-5 ms, so the PC is roughly 4x slower per frame, before
the streaming encoder takes its share of the same four threads.

Two things in that log were ours to fix, not the PC's:

- **162,000 of its 163,671 lines** were one SDK warning, "Too few processor
  cores - scheduling will be wonky": ~300 lines a second (section 5).
- **Our renderer's texture checks** took 2.6-2.8 ms per frame in the fight.

## 2. A profiler for gameplay

The slow-load sampler (findings/29 s.7) already sampled the main thread's
host call stack with a signal; it only ran in the title's loading states.
The debug console now has

```
profile <seconds> [file]      wait profile [ms]
```

which samples the main thread every 1 ms in any state and writes a report:

- **SELF**: the innermost game function of each sample (where the time is
  spent), and **INCLUSIVE**: every game function on the stack once (the
  systems that contain the time).
- **HOST**: the innermost host frame, the innermost frame outside system
  libraries (libc, libm, the GPU driver), our and the SDK's functions
  inclusively, and a **CALLED FROM** table (each big entry's callers).
- The game frames' average timings over the run.

Measure with `--fps_cap=0 --present_vsync=false`: with a cap (or V-sync,
which caps at the monitor's refresh) the frame pacer's sleep shows up as
time, and a saving only moves time into the pacer (seen: two runs at 180 Hz
with very different profiles and the same frame rate).

## 3. The first profile (Wumpa Island, uncapped, 174.6 fps)

Per frame ~5.7 ms. The top entries:

| share | what |
|---|---|
| 23.2% | `sub_823ED688` alone: the characters' CPU SKINNING loop |
| 17.9% | `sub_82433510` (SwapBuffers): mostly our renderer's frame end, called from inside it |
| ~16% | our recorder inside the game's draw calls |
| 3.0% | XXH3 hashing (texture and mesh-buffer change checks) |
| 2.4% | creating Vulkan images (`CreateDedicatedAllocationImage`) |
| 1.1-1.6% | the SDK's `perf::IncrementCounter` |

### 3.1 Why the skinning loop is slow when recompiled

`sub_823ED688` moves every vertex of an animated mesh piece by its bones.
The recompiled version keeps every PowerPC register in memory, and each of
its ~34 fused multiply-adds per vertex (`fmadds`) is `std::fma` on doubles.
The game is compiled for plain x86-64 (`-msse4.1`, no FMA) with
`-mcmodel=large`, so `std::fma` can't become an instruction: it is an
INDIRECT CALL into libm's `fma` for every multiply-add. That is true of
every `fmadds` in the whole game; this loop just has the most of them.

## 4. The skinning loop in C++ (src/decomp/skinning.cpp)

The function, read from the disassembly (the file's header has the record
layout): r3 = the piece's skin record (vertex count +12, flags +8, streams
in/out: positions +24/+32, normals +28/+36 (flag 0x10), binormals +40/+44
(0x800), tangents +48/+52 (0x1000), 4 bone slots per vertex at +80, 3
weights per vertex at +84 or none), r4 / r5 = the bone matrices and the
palette (slot -> matrix number), r6 = the bounding box out. Positions blend
up to 4 bones (w3 = 1 - w0 - w1 - w2), skipped after bone 0 when w0 == 1.0;
directions use bone 0's 3x3 only; the box starts at +-100,000 (constants
0x8200F850 / 0x8200F860).

**Exact, not approximate.** The rewrite follows the original instruction by
instruction: products and sums as float operations (the recompiled code
computes them in double and rounds to float, which for two floats is the
same result), every `fmadds` as `float(fma(double, double, double))` like the
recompiled code, the same fusing order, inputs re-read after each store (in
case an input and an output ever share memory), the FPU's flush mode set as
the original leaves it. The CPU's FMA instruction is used when the CPU has
one (a clone compiled with `target("fma")`, chosen by CPUID + XGETBV); it
gives the same correctly rounded result as the library call. Older CPUs take
the same code with `std::fma`.

**Checked byte for byte** with `--debug_native_skin_check`: every call runs
the original first, keeps its outputs, runs ours and compares every byte
(on a mismatch the original's bytes are kept and the first few are logged).

| run | calls | vertices | differences |
|---|---|---|---|
| Wumpa Island, FMA | 13,959 | 14,015,682 | 0 |
| Wumpa Island, no FMA | 15,576 | 17,681,725 | 0 |
| a titan save, FMA | 18,061 | 22,106,125 | 0 |
| a titan save, no FMA | 18,067 | 22,112,173 | 0 |

Speed (same spot, uncapped, V-sync off): **184 -> 218 fps**, 0.86 ms less per
frame; the function's share 22.9% -> 10.2% (about 2.5x faster).
`--native_skinning=false` brings the original back for comparisons.

## 5. The SDK's core-count warning (patch 0020)

`XThread::SetActiveCpu` pins a host thread to the guest's core only on PCs
with 6+ logical processors, and otherwise logged the warning, at EVERY
`KeSetAffinityThread`. The game's D3D code moves its threads between cores
constantly: ~300 formatted log writes a second on the threads the game
waits for, on exactly the PCs with the least CPU to spare. Now said once,
with the core count. Behaviour unchanged (affinities ignored below 6).

## 6. Our renderer's main-thread costs

- **Recycled texture images.** Every texture upload created a new image
  with its own memory allocation (a slow driver call) and retired the old
  one. Even a still scene uploads ~0.9 textures a frame (palette copies,
  animated textures). Retired images the GPU is done with now wait in a
  pool (at most 48) and an upload of the same format, size, mip count and
  channel order reuses one: image creation 2.7% -> 0.7% of the main thread;
  ~75-90% of uploads go into a recycled image (log: "uploaded (N into
  recycled images, M new)"). Same-frame A/B against the emulated picture
  afterwards: 0.56 and 0.45/255 (the usual).
- **Mesh buffers behind the write watch.** Every mesh buffer a frame used was
  hashed in full every frame. They now use the texture cache's write watch
  (findings/20): pages protected after a hash, the hash skipped while nothing
  wrote there, a cool-down for buffers caught changing, every watched buffer
  re-hashed once every 32 frames as a safety net. Wumpa Island: ~356 buffers
  skipped, ~12 hashed per frame, 0 missed; hashing 3.7% -> 1.8%.
- **No zero-fill of the frame's vertex copy.** The recorder grows its
  per-frame vertex and index copies with `resize()`, which zero-filled ~700
  KB per frame right before overwriting it (~1%). `Frame::vertex_data` /
  `indices` are now `RawVector`s (an allocator whose default construction
  leaves plain words uninitialised).

## 7. The SDK's perf counters in the game's code

The SDK defines `REXGLUE_ENABLE_PERF_COUNTERS` for every build that isn't
"Release", and the game is built RelWithDebInfo (also by the installer).
Then every INDIRECT call in the recompiled code (each virtual function call:
`REX_CALL_INDIRECT_FUNC`) does an atomic add through a call into the SDK
library, for a counter nothing reads: ~1.5% of the main thread. CMakeLists
now undefines it for the game's two targets (`crash_mom` and the generated
code's `crash_mom_recomp`). The only SDK header whose class LAYOUT depends on
the switch is `ui/overlay/debug_overlay.h`, which the port never includes.

**But the SDK's `share/rexglue/rex_app.cpp` does** (fixed 2026-11-12): the SDK
ships ReXApp's source and `rexglue_setup_target` compiles it into the game's
exe, where it creates the `DebugOverlayDialog`. Undefined there, the object
was allocated in the small layout, while the SDK library's `OnDraw` (built
with the counters) wrote the frame-time history past its end: **Tools →
Debug overlay** crashed at once (a write in `debug_overlay.cpp:48`, from a
player's crash report on Windows). That one file gets the switch back
(`set_source_files_properties(... COMPILE_OPTIONS -D...)`, same "not Release"
rule; source options come after the target's `-U`). Checked in
`compile_commands.json` (`-D -U -D`) and by toggling the overlay in a run.

## 8. Results so far

Wumpa Island, uncapped, V-sync off, same spot:

| step | fps | per frame |
|---|---|---|
| before | 184 | 5.42 ms |
| skinning in C++ | 218 | 4.56 ms |
| + recycled images | 223 | 4.45 ms |
| + mesh-buffer watch, no zero-fill | 235 | 4.25 ms |
| + no perf counters in the game code | 236 | 4.21 ms |

**184 -> 236 fps (+28%), 1.2 ms less per frame** on this desktop CPU. The
per-frame work scales with the CPU, so on a PC about 4x slower that is
roughly 4-5 ms less per frame in the same kind of scene.

## 9. Next candidates (from the same profiles)

- Every `fmadds` in the game is a library call (section 3.1). Building the
  generated code with FMA would need a second build for CPUs without it (or
  a CPU floor); per function, a native rewrite like section 4 does it.
- The skinning loop is still the biggest single function (~10%): it could
  use SIMD (four vertices at once) and stay exact.
- `sub_8245C1A0` (~3%, called from everywhere) is the game's memory copy:
  the SDK has native CRT hooks for it (`[entrypoint.rexcrt]`).
- Recording our Vulkan commands (~6% including the driver) could move to
  another thread: the frame is already captured before it's drawn.

## 10. Recording our Vulkan commands on a thread of its own

After the native back-end rewrites (findings/60), the largest piece of the
main thread uncapped on Wumpa Island was our own renderer's frame end,
`NativeRenderer::OnFrameEnd`: ~20%, almost all of it recording the frame's
Vulkan commands (~7% inside the GPU driver).

That recording has two halves. The first resolves what each draw needs:
textures and mesh buffers from the caches (they hash and upload GUEST
memory, which the game changes during its next frame), uploads, pipelines.
The second records the render passes and draws, the present pass and the
submit, and reads only what the first half left: our copy of the frame's
command list, the prepared draws, the render targets. So the first half
stays on the game thread (`PrepareFrame`), and the second runs on a thread
of its own ("Native Record") inside the presenter's refresh, while the
game goes on with its next frame. The next frame end waits for it first.

- One Vulkan command buffer is recorded by both threads, one after the
  other (the hand-over is a mutex + condition variable).
- The frame is double-buffered: `pass_frame_` is the one the record thread
  draws, while the recorder refills the other one's storage.
- The per-draw descriptor sets (`vkAllocateDescriptorSets` +
  `vkUpdateDescriptorSets`) moved to the second half too: with only the
  passes moved, the record thread had 0.2 ms a frame and most of the
  driver's time was still on the game thread.
- When the presenter skips its callback (no active output, a failed
  refresh), the first half's commands are still submitted, so the caches'
  uploads are real.
- Native gaps the second half meets are collected and reported on the game
  thread; an F10 photo or an A/B capture runs that frame on the game thread
  (`ab_capture::NeedsSameFrame`), as before.
- Switch: `native_record_thread` (on by default; live).

Checked: frozen-game screen captures with the switch on, off and on again
differ only where the game itself moves (water, idle animation, a HUD card
leaving); F9 back and forth, the dual window opened and closed, photos and
quitting, all with the thread on, no errors.

Measured (uncapped, Native Only, the switch flipped live, two pairs):
`OnFrameEnd` 20.57 / 20.16% of the main thread -> 13.51 / 13.01%; 221 /
216 fps -> 247 / 236 fps. What stays on the game thread is mostly the
texture and buffer caches (they must: guest memory).

## 11. Skinning four vertices at once

The skinning loop (section 4) was still ~13% of the main thread. The SIMD
version (`SkinSimd`, switch `native_skinning_simd`, live) does the same
operations in the same order on four vertices, each in a 64-bit lane of a
256-bit register holding a float value as a double:

- fmadds = `_mm256_fmadd_pd` (per lane exactly std::fma), then the round to
  float (`cvtpd_ps` + `cvtps_pd`), as the recompiled code does;
- fmuls / fadds / fsubs in double, then the round (for two float values the
  same as the float operation: double has more than 2 x 24 + 2 bits);
- bone matrices loaded a row at a time, byte-swapped by `pshufb`,
  transposed into lanes; each group's 48 bytes of positions (and weights)
  loaded at once and split into x y z by blends and shuffles;
- blended vertices (w0 != 1, NaN included) take bones 1-3 per lane, the
  other lanes' results thrown away;
- the bounding box still grows vertex by vertex (with +0 / -0 the order
  decides which one stays), and a call whose outputs overlap any input runs
  the scalar loop (the original re-reads inputs after each store).

Checked with `--debug_native_skin_check`: ~45 million vertices on Wumpa
Island and the titan save (enemies spawned), 0 differences. Positive
control: a change in the blended-vertex branch only differed on the first
call (removed after).

Measured (two runs of two pairs, switch flipped live): one vertex at a time
12.64 / 13.19 / 13.47 / 13.94% -> four at once 9.69 / 10.64 / 11.61 /
10.95%: about a fifth faster. Run-to-run noise is large here (270-307 fps
for the same setting). What limits it is the round to float after every
single operation, which staying bit-identical requires.
