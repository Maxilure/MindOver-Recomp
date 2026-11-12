# 59. Native Only: what the Xbox graphics stack still costs, the light command stream, and skipping the game's Direct3D draws

The native renderer draws the whole picture, and in Native Only (the default)
the emulated GPU draws nothing (SDK patch 0009, `skip_draws`). But the game
still runs its whole Xbox graphics stack underneath: Radical's PDDI Xbox back
end (the `xn*` classes) and Microsoft's Direct3D for the 360 run as
recompiled code and build the GPU's command buffers every frame; the SDK's
emulated GPU still receives and reads them; the native renderer watches the
game's draw calls on their way (shadow mode, findings/08). This page measures
that stack and makes its emulated-GPU half cheap.

## 1. Measuring it

Tools added for this (both in `src/debug/slow_load_sampler.*`, findings/57):

* console `profile <seconds> <file> <thread name>`: the sampler can now
  sample ANY thread by name (`GPU Commands`, `Audio Worker`), not only the
  game's main one;
* a **BY AREA** section in every report: the main thread's samples split by
  the address of the innermost game function: Xbox Direct3D
  (0x82305000-0x82323000 + draws 0x82476000-0x82478000), PDDI Xbox back end
  (0x82420000-0x82460000), everything else; per area "own code" (the CPU was
  in that game code) and "host below" (in our renderer's handlers or the SDK
  it called);
* an **UNNAMED LIBRARY FRAMES** section: frames in shared libraries that
  export no names (the SDK's GPU plugin) as `file offset`, named afterwards
  with `llvm-symbolizer --obj=<file> --functions=linkage -C <offsets>`.

Per-thread CPU: `/proc/<pid>/task/<tid>/stat` (utime + stime) over 15 s.

Wumpa Island, Native Only, 720p:

| | uncapped (~211 fps) | 60 fps |
|---|---|---|
| main thread: Xbox Direct3D, own code | 7.1% | 2.1% |
| main thread: PDDI Xbox back end, own code | 19.6% | 6.4% |
| main thread: our renderer's handlers under the back end | 23.9% | 7.5% |
| thread "GPU Commands" (the emulated GPU), CPU of one core | 81-92% | 29-31% |

The emulated GPU's thread, sampled at 60 fps: ~71% asleep, ~7% in an idle
spin (`MaybeYield`), ~22% at work, and that work was the stream's register
writes: `CommandProcessor::WriteRegister`, `VulkanCommandProcessor::
WriteRegister`, `RegisterFile::GetRegisterInfo` (a debug check on every
write), `RingBuffer::ReadAndSwap`. All of it bookkeeping for DRAWING
(shader constant dirty marks, texture cache and vertex buffer notes) that
nothing uses while `skip_draws` is on.

## 2. The light stream (SDK patch 0021)

While `skip_draws` is on (and the new GPU flag `skip_draws_light_stream`,
default true), decided once per primary buffer:

* register writes are only STORED in the register file: type-0 runs in one
  copy straight from the ring, `SET_CONSTANT` / `LOAD_*_CONSTANT` ranges
  likewise. Registers with an effect outside drawing keep the full path:
  the scratch registers (D3D's fences, written back to memory), the
  coherency flag `COHER_STATUS_HOST`, the gamma ramp registers `DC_LUT_*`
  (the native renderer reads the ramp from the command processor);
* the idle worker sleeps on its event at once instead of 500 yields;
* the first draw issued afterwards (F9 to the emulated picture, the flag
  turned off) rebuilds what was skipped: every constant buffer out of date,
  every texture fetch constant "written", every vertex buffer out of sync
  (`InvalidateStateAfterLightWrites`, Vulkan).

Everything else in the stream runs as before: fences, interrupts, swaps,
waits, so the game never waits longer. Not for frames skipped by
`draw_every_nth_frame` (dual mode, patch 0013): the next drawn frame needs
their state.

## 3. Results

Same run, the flag switched live (`flag skip_draws_light_stream false`):

| | light stream | full stream (before) |
|---|---|---|
| "GPU Commands" at 60 fps | 8.3% of a core | 31.1% |
| "GPU Commands" uncapped | 28.9% | 92.1% |
| uncapped frame rate, average / 1% low | ~226 / 176-178 fps | ~220 / 163-170 fps |

The main thread is unchanged (the stream is built there either way); the
gain is a core given back, which matters most on CPUs with few cores.

Checked: gameplay, pause menu, boot with the intro movies, title and main
menu in Native Only; switching to the emulated picture and back (photos: the
emulated picture rebuilt correctly). 0 errors.

A frame-by-frame capture (10 ms) around each switch to the emulated picture
shows ONE frame with the HUD over a black world, and one stale frame from
the previous emulated period, the same with the light stream off: the
emulated GPU's first frame after drawing nothing for a while (its render
targets and resolves are empty), not this patch. A possible fix: keep the
native picture up until the emulated GPU has drawn a frame of its own.

## 4. Skipping the game's Direct3D draws (`src/native/d3d_draw_skip.*`)

One traced gameplay frame (`--debug_pddi_trace_dir`, 17,465 PDDI/D3D calls)
has ~4,000 calls into the Xbox Direct3D. Only a few kinds turn state into
GPU commands: `DrawIndexedVertices` 0x82477B58 (289 meshes, from
`xnPrimBuffer::Draw`), `BeginVertices` 0x82476DA0 / `BeginIndexedVertices`
0x82477248 + `EndVertices` 0x82477238 (61 immediate draws: characters, 2D),
`Clear` (11), `Resolve` (28). The rest are `Set...` calls that store state in
the device's copy.

Read with Ghidra, a D3D draw: (1) FLUSHES the device's pending state into the
command buffer (dirty masks at device +0x00..+0x20; sub_8230E730 shader
constants, sub_8230E398 register blocks, sub_8230F4C0 fetch constants,
sub_8230E5D8 / E458), clearing the masks; (2) for immediate draws takes the
vertex (and index) memory from D3D's allocator sub_82308B60(device, words,
16); (3) writes the draw packet at the command buffer's write pointer
(device +0x30, limit +0x38, sub_823098D0 = room / kick), `EndVertices` then
moving +0x30 to the packet's end. `BeginIndexedVertices` returns the index
pointer through r10 and the vertex pointer through its 9th argument
(`lwz r9,308(r1)` after `stwu r1,-224(r1)` = entry r1 + 0x54). The mesh draws'
results are unused by their caller (`li r3,0` after the call).

The console's `who` on the device's copies (fetch constants device +1152,
VS / PS constants +1920 / +5984, PS bools +10128) found only `Set...`
functions (SetStreamSource, SetTexture, SetVertexShaderConstantF, ...) and
Radical's back end as writers, never a draw. The recorder reads those copies
before the draw runs, one level up. So in Native Only (the emulated GPU
idle, `NativeRenderer::UpdateEmulatedDrawing`), inside a recorded PDDI call:

* mesh draws (`DrawIndexedVertices`, `DrawVertices` 0x82477768): skipped;
* `BeginVertices` / `BeginIndexedVertices`: only D3D's own allocations, the
  pointers handed back the original way (the game writes its vertices there,
  the recorder reads them); `EndVertices` skipped after a skipped Begin
  (paired by a flag);
* the dirty masks stay set: the first real draw afterwards (F9 to the
  emulated picture, dual mode) flushes the whole up-to-date copy;
* clears, resolves and draws outside the recorded roads still go to D3D.

`--d3d_draw_skip=false` (console `flag d3d_draw_skip false`) sends every draw
to D3D again.

Measured in one run, switched live (Wumpa Island, Native Only):

| | draws skipped | draws to D3D |
|---|---|---|
| uncapped frame rate | 226.6 fps | 204.1 fps |
| game time per frame | 3.58 ms | 4.03 ms |
| main thread in Xbox Direct3D, own code | 3.4% | 6.5% |
| "GPU Commands" at 60 fps | 2.7% of a core | 8.8% |

Checked: gameplay, pause menu, F9 to the emulated picture and back (the
emulated picture rebuilt correctly, pause menu included), dual mode (both
pictures alike), boot with the intro movies, main menu, Load Game into a
level through the loading screen, and ~85 s uncapped (21,480 frames, 200-230
fps, worst frame 13 ms): D3D's allocator keeps reusing its memory with almost
no commands sent. 0 errors.

## 5. What is left

* Radical's PDDI Xbox back end (~20% of the main thread at ~220 fps, own
  code) still runs, and with it the D3D `Set...` calls our renderer reads:
  the next step is the back end handing its state to the native renderer
  directly (its material set-ups, slot 14 of each xn shader, findings/09-17),
  so that no Xbox D3D runs at all in Native Only.
* Switching to the emulated picture shows one black-world frame and one
  stale frame (older; see section 3).
* "Audio Worker" uses ~49% of a core at any frame rate (seen in the same
  measurements; not looked into yet).
