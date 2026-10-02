# Findings: the 30 fps cap, and 60 fps with `--fps_cap=60`

2026-09-25. Roadmap phase 3, step 1. **Result:**
`--fps_cap=60` runs the title at ~59 fps and gameplay at ~56 fps, and the
game moves at the same speed as at 30 (same inputs on the same save end in
the same place). Default stays 30 (original behavior, every hook a no-op).
Code: `src/frame_rate.cpp`, two `[[entrypoint.midasm_hook]]` entries in
`crash_mom_manifest.toml`. (Later the same week: above 60, and every cap
paced by the clock: the last two sections.)

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

Tested on a **copy** of the save data (`--user_data_root`), driving the
game with the new live input FIFO (`--debug_input_fifo`): title → Load
Game → a save on Wumpa Island → Continue.

| | 30 (default) | `--fps_cap=60` |
|---|---|---|
| title | 30.0 fps | 59.2 fps |
| gameplay (Wumpa Island) | 30.0 fps | ~56 fps |
| stick right 2 s, then forward 2 s | reference position, Mojo 724 → 725 | **same position**, same Mojo pickup |

So movement is time-based. Gameplay's ~56 fps means some frames take longer
than 16.7 ms and wait for the next vblank (per-thread CPU at the title:
main 94% (mostly the spin), GPU command thread 22%, host present 62%).

## Measuring it: the game's frames, not the window's (2026-09-26)

A playtest with `--fps_cap=180` and MangoHud showed **600+ fps** in places.
The game itself never went above 60 in gameplay: the native renderer's
periodic log line carries the game's frame number with a timestamp, and it
said **59.4 frames per second** throughout (about 96 once, during a loading
screen, which runs without vsync). Two separate things:

* **The cap above 60 has nothing to cap in gameplay.** Mode 0 waits for the
  guest's 60 Hz vblank; `--fps_cap` values above 60 only raise the main-loop
  limiter, which paces loading screens and movies. (Above 60: see the open
  questions.)
* **MangoHud counted the window's repaints**, and the window was repainting
  non-stop. The SDK's ImGui layer asks for another repaint after every paint
  while any overlay dialog is open, and the achievement toast is a dialog
  that's open all session (created at launch, drawing nothing until an
  achievement pops). With the Vulkan presenter's immediate (no vsync) present
  mode, that loop ran as fast as the GPU allowed: hundreds of presents a
  second of the same picture, fewer in heavy scenes. SDK patch **0006** lets
  a dialog say when it needs continuous repaints (the toast: only while one
  is showing); the window now repaints once per game frame. After it,
  MangoHud's average over a loading screen and gameplay was 52.8 fps, in line
  with the game's own count, and the window thread at the title went from
  **62 % of a core** (the "host present" figure in the verification above) to
  **9 %**. That loop was also taking GPU time from the emulator.

**The game's own numbers** (`src/frame_rate.cpp`, section 4):
`--debug_log_fps` now logs, every 5 s of frames, the average frame rate, the
1% and 0.1% lows (1000 / the 99th and 99.9th percentile frame time: the
frame rate 99% / 99.9% of frames reach) and the worst frame, for those 5 s
and for the whole session so far, plus a whole-session line at exit.
`--debug_fps_csv=<file>` writes every frame's time. Each sample is the time
between two frame ends (right after `xnDisplay::SwapBuffers`, which includes
the vblank wait), whichever picture is shown. Example, the waterfall area (name TBD) at `--fps_cap=60`: `59.4 fps average, 1% low 56.8, 0.1% low 52.5, worst
frame 19.0 ms`. `tools/play.sh` passes `--debug_log_fps` and prints the
session line when the game closes. MangoHud is fine too now, as long as the
F3/F4/console overlays are closed: those still repaint continuously on
purpose.

**Who is slow** (2026-09-27, findings/19 section 6): each 5 s line is now
followed by the average frame's main thread cut in three at
`xnDisplay::SwapBuffers`: the game's own work (plus our recorder and any
wait for the emulated GPU), the end-of-frame work (our native renderer),
and the time inside SwapBuffers (mostly waiting for the screen: the
slack). The native renderer logs its own breakdown (GPU wait, textures,
mesh buffers). The CSV has the same three columns.

## Above 60 fps: two more walls (2026-09-30)

`--fps_cap=144` first changed nothing: gameplay stayed at exactly 59 fps,
16.7 ms per frame, all of it in the "game" part of the frame (none in
SwapBuffers). Two things we tried did NOT help on their own:

* **A faster virtual screen.** The SDK fires the guest's vblanks at the
  video mode's refresh rate (60 Hz). A new GPU flag `guest_refresh_hz`
  (SDK patch 0010) raises it; `--fps_cap` above 60 now sets it to the cap.
  The game's vblank counter confirmed ~143 ticks a second, and still 59 fps.
* **The game's own no-vsync mode.** Forcing PDDI's mode 2 (D3D present
  interval 0) for a test: still 59 fps, even on the plain title screen.

**What gdb showed.** Sixty stack samples of the main thread during gameplay
(a small gdb Python sampler: SIGINT every ~70 ms, print the "Main XThread"
stack): only ~1 sample in 8 was inside the frame function `sub_8227C5D8`.
The rest were the main loop itself (`sub_8227AEE0`) going round and round:
reading the clock (`sub_824731D8` = QueryPerformanceCounter), updating the
engine's subsystems (`sub_8235DBB0` walks a list of them). The frame
function explains why:

```
0x8227C604  lfs    f30,28(r27)      ; f30 = 0.016666668, the float at 0x8201FB80 (1/60 s)
0x8227C658  lfs    f0,160(r31)      ; time accumulated since the last frame
0x8227C65C  fadds  f0,f31,f0        ;   += this pass's delta
0x8227C670  fcmpu  cr6,f0,f30       ; under 1/60 s so far?
0x8227C674  blt    cr6,0x8227c7e4   ;   -> return without a frame
```

The game **accumulates time and refuses to run a frame before 1/60 s has
built up** (and clamps a step to 0.1 s, the float at 0x8201FB64). That's a
60 fps ceiling written into the game itself, invisible from the renderer's
side. A midasm hook at 0x8227C670 (`CrashMomFrameStep`, frame_rate.cpp
section 1b) lowers f30 to 1/fps_cap above 60, or 0 for `--fps_cap=0`.

**Both are needed.** With the step lifted but `--guest_refresh_hz=60` given
by hand, gameplay is back to 60.0 fps: the vsync mode 0 path still waits
for one (60 Hz) vblank per frame.

**Measured** (Wumpa Island jungle, native renderer with the emulated GPU off
(`--native_only`), on our test machine):

| `--fps_cap` | gameplay fps | main thread per frame |
|---|---|---|
| 60 | 59.0 | game 16.7 ms (mostly the spin above) |
| 144 | 141.7 (1% low 134) | game 6.7 ms |
| 0 (no cap) | 150-160 | game 4.0-4.6 ms, 1% low ~130 |

Menus run faster too. **Status: work in progress.** Playtesting at 144
fps and uncapped hasn't found anything broken so far (movement, fights,
cutscenes), and input feels more responsive than at 60, but it isn't
confirmed stable yet: anything in the game that counts frames or vblanks
instead of time could still misbehave somewhere not yet played.

(Superseded the same day by the clock pacer, next section: the "virtual
screen at the cap" part had a flaw.)

## Pacing by the clock (2026-09-30)

**The flaw.** A playtest at `--fps_cap=180` (virtual screen at 180 Hz) sat
at **exactly 90 fps** in one part of Wumpa Island, 150-160 elsewhere; the
same spot uncapped ran at 120-200. 90 = 180 / 2. A frame there needed about
5.6 ms of main-thread work (game 4.5 + our renderer's frame end 1.1), a
hair over one 180 Hz tick (5.56 ms). A finished frame is shown only at the
next vblank, so every frame waited for the tick after the one it just
missed: two ticks per frame, half the cap. Any rate paced by ticks drops to
half the moment frames run slightly long, and 60 had the same problem in a
milder form (a 17 ms frame lasted 33 ms: the old ~56-59 fps in gameplay).

**Even 1000 Hz ticks cost time.** With only the native renderer drawing,
uncapped, the title ran at 597 fps and spent 1.2 ms of every frame inside
SwapBuffers waiting for its tick, four times the game's own work (0.3 ms).

**The fix: two parts** (`src/frame_rate.cpp` sections 1 and 1b).

1. **Present right away.** PDDI's swap (`sub_82433510`) has a third mode:
   2 = D3D presentation interval IMMEDIATE, used by the loading screens. Its
   30 fps mode 1 already takes that path for a frame that finished late
   (the late branch at `0x82433594` skips `li r29,1`, leaving
   `r29 = 0x80000000` = IMMEDIATE). With `--fps_cap` other than 30 the vsync
   hook now turns mode 1 into 2 instead of 0. Uncapped title, native only:
   597 → **1736 fps**, SwapBuffers 1.2 → 0.1 ms. The virtual screen's rate
   stops mattering (1705 fps with it at 60 Hz), so it stays at the standard
   60 Hz for every cap, pinned with patch 0010's flag (the SDK's display
   setting `video_mode_refresh_rate` also sets the vblank rate; 180 there
   would make the original 30 fps pacing run at 90).
2. **A clock pacer at the frame step.** With nothing waiting for ticks, the
   frame function's gate (`0x8227C670`, previous section) decides when
   frames start. How the game keeps time there: each main-loop pass adds its
   real delta to an accumulator at `160(r31)`; a frame runs with the
   accumulator (clamped to 0.1 s) as its time step, then it's reset to 0.0
   (`0x8227C7DC`, the float at `0x82046720`). So a frame's step is always
   the real time since the previous frame started, and the hook can choose
   *when* frames run without changing the game's speed: `f30 = 0` runs one,
   `f30 = 1e30` skips the pass. The pacer schedules frame *k* at
   `start + k / cap` (on the clock, so the average is exact; a slightly late
   frame gives the next one a little less wait; one over a whole period late
   restarts the schedule instead of bursting). A frame too slow for the cap
   just runs at its own speed: no halving.

**Sleeping instead of spinning.** Between frames the main thread sleeps
until 1 ms before the next start, spins that last millisecond in a tight
clock loop, then lets the next main-loop pass read the game's clock right
at the start time. Linux sleeps here wake ~6 µs late (99% within 40 µs,
rare outliers up to 0.8 ms), so the spin margin keeps starts exact. The
original game doesn't spin between frames either: at 30 fps it waits inside
SwapBuffers, one main-loop pass per frame. A first version let the game's
own loop spin the last millisecond; frame starts then wandered by a pass
(each pass also updates the engine's subsystems): at 144, 98% of frames
took 6.41-7.49 ms; waiting inside the hook tightened that to 6.83-7.07 ms.
Waiting *before* the frame (rather than at SwapBuffers, like the original)
also keeps input fresh: the frame reads the controller right after the
wait, not before it. (A Windows port needs a high-resolution waitable
timer here: plain `Sleep()` is coarse.)

**Measured** (title screen, 20-30 s after boot; main-thread CPU from
`/proc`):

| `--fps_cap` | picture | fps | 1% low | main thread CPU |
|---|---|---|---|---|
| 30 (original) | emulated | 30.0 | 29.1 | 5% |
| 60 | emulated | 60.0 | 59.8 | 11% (was ~100%: the spin) |
| 144 | emulated | 144.0 | 143.6 | 26% |
| 144 | native only | 144.0 | 141.2 | 22% |
| 165 | native only | 165.0 | 162.0 | 27% |
| 180 | emulated | 180.0 | 178.9 | 32% |
| 0 (no cap) | emulated | ~300 (the emulated GPU's limit) | 221 | 98% |
| 0 (no cap) | native only | 1642 | 681 | 90% |

The log's second statistics line now reads *game / waiting for its start
time / frame-end work / SwapBuffers*; the wait is taken out of "game" (it
happens in the main loop, where the old split counted it) and the CSV has
it as `pace_wait_ms`. The intro movies (the game's own mode 0, on the
loading/movie path) are unchanged: at 30 and at 144 alike they run at
30 fps, then 38.0 during the attract movie, and the title appears at the
same moment (~1:55 after boot). Gameplay under load (the spot that was
stuck at 90) is for the next playtest.

## Open questions

* Physics, animation blending, particles, cutscenes (in-engine and Bink) and
  audio sync at 60: playtesting (`tools/play.sh --fps_cap=60`). Found so
  far: Crash "falling" for a split second stepping down (the move logic
  saw brief ground losses that 30 fps frames hid), fixed in
  [findings/22](22-ground-contact-high-fps.md).
* Why gameplay frames sometimes exceed 16.7 ms: CPU (recompiled code) or
  GPU (FSI render-target path)? Needs profiling. (With the clock pacer a
  long frame no longer turns into 33.3 ms; it just takes as long as it
  takes.)
* Above 60 fps: done (section above). Still open: does anything in the game
  misbehave at 120+ (anything that counts frames or vblanks instead of time)?
* Time sources worth knowing for any later timing work (replays, a
  controlled clock): `sub_8235AAE0` / `0x824731D8` (QPC) and the vblank
  counter `0x8258E460`.
