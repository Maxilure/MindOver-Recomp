# Findings: the 30 fps cap, and 60 fps with `--fps_cap=60`

Session of 2026-09-25 (fifth). Roadmap phase 3, step 1. **Result:**
`--fps_cap=60` runs the title at ~59 fps and gameplay at ~56 fps, and the
game moves at the same speed as at 30 (same inputs on the same save end in
the same place). Default stays 30 (original behavior, every hook a no-op).
Code: `src/frame_rate.cpp`, two `[[entrypoint.midasm_hook]]` entries in
`crash_mom_manifest.toml`.

The cap turned out to be three layers deep. Two wrong guesses first, since
they're instructive.

## Wrong guess 1: D3D's presentation interval

Microsoft's D3D library is linked statically into `default.xex`, so it got
recompiled with the game. It paces frames with the vertical blank (vblank,
60 per second; ReXGlue's "GPU VSync" thread fakes them at the video mode's
refresh rate, or 1000 Hz with `--vsync=false`):

| Address | Role |
|---|---|
| `0x82308798` | GPU interrupt handler the game registers (`SetInterruptCallback(82308798, 40005200)`). Source 0 = vblank |
| `0x82310628` | per-vblank: counts vblanks (`device+0x40A4`), flips queued frames whose target vblank arrived (store to GPU register `0x7FC86110`), calls the game's vblank callback (`device+0x40A0`) |
| `0x82310728` | per presented frame (through a pointer, from the swap's GPU interrupt): picks the target vblank. r3 packs frontbuffer address (bits 12–31), **presentation interval** (8–11) and "immediate threshold" (0–7) |
| `0x82310DA8` | D3D swap. The only caller of `VdSwap` (at `0x82310FF4`) |
| `0x82311388` | D3D SetVerticalBlankCallback: stores the callback at `device+0x40A0` |

Wrapping `sub_82310728` (a function override: generated `sub_X` is a weak
alias of `__imp__sub_X`) and logging showed the game asks for **interval 1**
(60 Hz), plus interval 0 ("immediate") on loading screens. Forcing the
threshold to 100% didn't help either. D3D wasn't the cap. The wrapper
stayed, as the fps counter (`--debug_log_fps`).

## Wrong guess 2 (half right): the main loop's 1/30 s check

The main loop `sub_8227AEE0` reads `sub_8235AAE0` (QueryPerformanceCounter
scaled to **microseconds**) and derives the frame's delta time with the
constant at `0x82046FCC` = 1e-6, i.e. real seconds. It has two paths:

* **Path A**: `0x8227B0DC cmplwi cr6,r29,33333` / `ble` → spin until 1/30 s
  has passed since the last frame, then run one (vtable slot 44).
* **Path B**: run a frame right away (vtable slot 40). No check.

A midasm hook on the compare (`CrashMomFrameLimiter`) did take effect, but
its own statistics printed only during loading and movies. Menus and
gameplay use path B, so the cap had to be *inside* the frame.

## The real cap: the renderer's vsync mode

Measurements that cornered it:

| run | title fps |
|---|---|
| default (60 Hz guest vblanks) | 30.0 |
| `--vsync=false` (1000 Hz guest vblanks), no main-loop limit | 59.2 (host monitor limit) |

Faster vblanks made the game faster, so something *counts vblanks*.
Searching the generated code for readers of D3D's vblank data led to the
callback the game registers through SetVerticalBlankCallback: `0x82433380`,
registered by `sub_82433B00` (Radical's renderer, PDDI, init). It only
copies the vblank count into a global, `0x8258E460`. The only reader of
that global is PDDI's "swap buffers" routine **`sub_82433510`**:

```
mode = this+1660
if mode == 1:
    while (vblanks - last < 1): Sleep(0)          ; sub_82472DC0
    if (vblanks - last == 1): last = vblanks + 1  ; on time -> next frame waits 2
    else:                     last = vblanks      ; late -> catch up
    interval = ONE
elif mode == 0: interval = ONE                    ; plain vsync, 60 fps
else:           interval = IMMEDIATE (0x80000000) ; no vsync (loading screens)
D3D SetRenderState(present interval) if changed (sub_8230C308), then swap (sub_82310DA8)
```

Mode 1, used by title, menus and gameplay, is a deliberate 30 fps pace:
each on-time frame is followed by a two-vblank wait.

## The fix

`CrashMomVsyncMode`: midasm hook at `0x82433534` (`cmpwi cr6,r10,1`, r10 =
mode loaded at `0x8243352C`). With `--fps_cap` ≠ 30 it turns mode 1 into 0,
so the game takes its own plain-vsync path. r10 is only read by the two mode
compares before being overwritten. `CrashMomFrameLimiter` covers path A
(loading, movies) with the same cap. Codegen emits
`extern void Name(PPCRegister& rN);` and calls it before the instruction;
our definitions have plain C++ linkage.

## Verification

Tested on a **copy** of the user's saves (`--user_data_root` in the
scratchpad), driving the game with the new live input FIFO
(`--debug_input_fifo`): title → Load Game → slot 2 → Continue.

| | 30 (default) | `--fps_cap=60` |
|---|---|---|
| title | 30.0 fps | 59.2 fps |
| gameplay (first hub) | 30.0 fps | ~56 fps |
| stick right 2 s, then forward 2 s | reference position, Mojo 724 → 725 | **same position**, same Mojo pickup |

So movement is time-based. Gameplay's ~56 fps means some frames take longer
than 16.7 ms and wait for the next vblank (per-thread CPU at the title:
main 94% (mostly the spin), GPU command thread 22%, host present 62%).

## Open questions

* Physics, animation blending, particles, cutscenes (in-engine and Bink) and
  audio sync at 60: playtesting (`tools/play.sh --fps_cap=60`).
* Why gameplay frames sometimes exceed 16.7 ms: CPU (recompiled code) or
  GPU (FSI render-target path)? Needs profiling. Frame pacing may feel
  uneven when frames alternate between 16.7 and 33.3 ms.
* Above 60 fps: mode 0 still vsyncs to the guest's 60 Hz vblanks. Options:
  a higher guest refresh rate (with `--vsync=false` the SDK ticks at 1000 Hz
  and the title ran at the host monitor's ~59), or the "immediate" mode.
  Anything else in the game that counts vblanks would then need checking.
* For netplay (phase 6): `sub_8235AAE0` / `0x824731D8` (QPC) and the vblank
  counter `0x8258E460` are time sources a virtual clock must control.
