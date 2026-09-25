# Findings: the freeze after the intro movies (an SDK fiber bug), and black movies

Session of 2026-09-25 (third session that day). Starting point: loading and
audio worked (findings/04). The intro movies played with sound but a black
picture. When they ended, or the user skipped them, a second "Loading"
screen appeared and **froze**. The cause was a bug in ReXGlue's host fiber
code, not in the game. It's fixed by a local SDK patch:
[`patches/rexglue-sdk/0002-fiber-destroy-wrong-thread.patch`](../../patches/rexglue-sdk/0002-fiber-destroy-wrong-thread.patch).

**Result:** the game gets past that loading screen, compiles shaders for a
new scene (the title screen, presumably) and keeps rendering at 30 fps.
Left alone there, it starts streaming `movie2.rcf` again after ~45 s, which
looks like the attract-mode demo movie.

The black movies were narrowed down too (second half of this file): the
recompiled Bink decoder works. The problem is on the GPU side.

---

## Part 1: the freeze

### What the movies are

`movie1.rcf` and `movie2.rcf` are RCF archives (same format as
`default.rcf`: directory of `(hash, offset, size)` at `0x3C`, names at
`0x800`). Every entry is a **Bink 1** video (`BIKi` magic). `movie2.rcf`
holds the boot sequence: `sierra.bik` (6.2 MB), `radical.bik` (17.5 MB),
`dolby.bik` (9.1 MB), `crash_mom_attract.bik` (144.1 MB), plus story
cutscenes `Crash_11..19_51.bik`. `movie1.rcf` holds `Crash_01..10_51.bik`.
(`_51` = 5.1 audio.)

In a trace run the game read exactly **176,893,132 bytes** from `movie2.rcf`,
and the four boot movies add up to 176,865,756. The rest is headers. So the
"intros" are those four, played back to back in ~100 s.

### Step 1: the log goes silent

With `--log_level=trace --log_noisy=true`, two runs of 150 s and 240 s
both logged **nothing at all** after ~113 s: no file reads, no GPU texture
uploads. That's the moment the attract movie's last byte was read. The log
isn't size-capped (spdlog `basic_file_sink`). The last line was cut
mid-write only because gdb killed the process with data still in the stdio
buffer.

### Step 2: stuck, or spinning? (per-thread CPU)

`/proc/<pid>/task/<tid>/stat` gives each thread's CPU time. **Gotcha:** the
thread name (field 2) can contain spaces and parentheses, e.g.
`(Main XThread (F)`. Splitting on spaces shifts every field, and those
threads falsely read 0. Parse after the *last* `)` instead.

Measured over 5 s windows (500 ticks = one full core):

| thread | during movie (t=60 s) | after freeze (t=135 s) |
|---|---|---|
| Main XThread | 513 (100%) | 515 (100%) |
| Audio Worker (SDK XMA) | 408 | 410 |
| host main (UI/present) | 361 | 368 |
| GPU Commands | 92 | 94 |

So the game's main thread wasn't blocked. It burned a full core.

### Step 3: always the same instruction

Every gdb stack sample of the main thread was identical:

```
#0 swapcontext+4                      <- libc
#1 rex::kernel::crt::SwitchToFiber_entry
#3 sub_8235B218   resume task          (game)
#4 sub_82356138   yield to scheduler
#5 sub_823567D8   per-frame task pump
#6 sub_8227AEE0   main loop
#7 sub_8227A3C0 <- xstart
```

A thread at 100% CPU with the *exact same* PC in 12 of 12 samples doesn't
look like a loop. It looks like one instruction running over and over. In
one sample gdb stopped the thread inside the SDK's `ExceptionHandlerCallback`,
which was the giveaway: a **fault loop**. The runtime installs a SIGSEGV
handler (for guest memory tricks). When a fault isn't one it knows, it
returns, and the CPU re-runs the faulting instruction forever. Our gdb
scripts pass SIGSEGV silently, so this stays invisible.

To see it, let the game freeze, then flip gdb to `handle SIGSEGV stop print`:

```
=> swapcontext+4:  mov %rbx,0x80(%rdi)
   $rdi = 0x0      fault address = 0x80
```

`swapcontext(&from->context_, &to->context_)` with `from == nullptr`.

### Step 4: who nulled it?

The SDK's `rex::thread::Fiber` (src/core/fiber_posix.cpp) keeps the fiber
running on each host thread in a `thread_local Fiber* tls_current_`.
`SwitchTo(target)` reads `from = tls_current_`. It had become null.

The only code that writes null there is `Fiber::Destroy()` for a *thread
fiber* (the fiber a thread starts on; every XThread gets one in
`XThread::Execute`). A gdb Python breakpoint on `Fiber::Destroy` (log and
continue) caught it:

```
DESTROY thread='Main XThread' this=0x7ffff0054550 thread_fiber=true tls_current_=0x7ffff0000ee0 SAME=False
   Destroy <- ~XThread <- RemoveHandle <- ReleaseHandle <- NtClose_entry <- __imp__NtClose
DESTROY thread='Main XThread' ... thread_fiber=true tls_current_=0x0 SAME=False   (x3 more)
```

When the movies end, the game's main thread `NtClose`s the handles of
**4 helper threads** that have exited. Which subsystem they belong to
isn't pinned down yet (movie playback is the obvious suspect). The last
reference goes away, so `~XThread()` runs **on the main thread** and destroys
the dead thread's host fiber. `Destroy()` then did `tls_current_ = nullptr`
unconditionally, on the wrong thread. On the main thread's next
fiber switch (its task pump does one every frame) it faulted as above.

Skipping the movies only makes those threads finish sooner, which is
why it froze either way.

### The fix

`Fiber::Destroy()` now clears `tls_current_` only if this fiber *is* the
one running on the calling thread (`tls_current_ == this`). Destroying some
other thread's fiber is a plain `delete`. The Win32 backend had the same
flaw (`::ConvertFiberToThread()` also acts on the calling thread) and gets
the same guard, untested for now.

### The game's task scheduler (identified along the way)

The game runs a cooperative task system on XAPI fibers:

| Address | Role |
|---|---|
| `0x8256D17C` | the scheduler object (global) |
| `0x8235B160` | get the current thread's task context |
| `0x8235B1D8` | get the current task |
| `0x8235B218` | resume task: `ctx->[+44] = task; SwitchToFiber(task->[+20])` |
| `0x82356138` | yield: from a worker task, switch back to the main task (`sched+36`); from the main task, switch to the scheduler fiber if work is queued |
| `0x823567D8` | per-frame pump: queue at `+24..+36` non-empty (or `+12` set) → yield; then pop completed items from the ring at `+44` and call their vtable slots 3 and 1 |
| `0x8227AEE0` | the main loop that calls the pump |

---

## Part 2: why the movies are black

### The pictures are decoded correctly

Bink on the 360 decodes on the CPU into three 8-bit planes, which a pixel
shader converts from YUV to RGB. The GPU trace (`Loaded linear ...`) shows
them re-uploaded every frame, double-buffered, at the same physical
addresses in every run:

| plane | format | buffer A | buffer B |
|---|---|---|---|
| Y (brightness) | 1280×720 `k_8`, pitch 1280 | `0x1F5BF000` | `0x1F72B000` |
| Cr, Cb (colour) | 640×360 `k_8`, pitch 768 | `0x1F6A1000`, `0x1F6E6000` | `0x1F80D000`, `0x1F852000` |

A tiled 1280×720 `k_8_8_8_8` texture at `0x16860000` / `0x16BF8000` is also
re-uploaded every frame (likely a render target read back as a texture).

A guest *physical* address P lives at host `0x200000000 + P` (the
"physical base" printed at startup). gdb dumped the Y plane every 5 s
through the whole intro (`dump binary memory ... 0x21F5BF000 +0xE1000`),
into the scratchpad, never the repo. The images are **correct**: the
Dolby logo, the Radical logo, then recognizable attract-movie scenes, with
average brightness varying from 16 to 220 between samples.

So the CPU side works: file streaming, Bink video decode, Bink audio.
**The black screen comes from how the GPU draws those planes.**

Caution for single snapshots: two early dumps looked broken (all zeros once,
flat "black + grain" another time). Both just caught a gap between movies
or a dark fade. Sample over time before concluding anything.

### Leads for next time

* The texture cache only uploads a texture when a draw binds it, so a draw
  that samples the Y/Cr/Cb planes **does** happen every frame. Its output
  is lost somewhere between the shader and the screen. Next step: find that
  draw (the one binding `0x1F5BF000`/`0x1F72B000`), then check its pixel
  shader translation, blend/colour-write state, render target, and the
  resolve to the front buffer (possibly the tiled `k_8_8_8_8` above).
* Weak lead: in two runs a pipeline with a vertex shader and **no pixel
  shader** (`VS B6C9863F710683EC`) was created during the movies. But
  several such pipelines are also in the startup cache, and the run after
  the fix created none during the movies, so it may be unrelated.
* The Bink YUV→RGB shader is probably embedded in `default.xex` (the 15
  files in `game/shaders/` are Radical's own).

## Side notes

* gdb: also pass real-time signals (`handle SIG32 ... SIG40 nostop noprint
  pass`). The runtime delivered SIG35 to a game thread and halted a trace
  script. `tools/guest_stacks.sh` now does this.
* The "Resolve region is empty" errors come to exactly 1,806 per run, all
  during the first loading screen.
* One thread name showed up as garbage (`"B*��"`) once: guest thread names
  are read from guest memory that may since have been reused. Harmless.
