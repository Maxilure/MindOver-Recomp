# Findings: the loading-screen hang (an XMA audio decoder bug)

Session of 2026-09-25 (second session that day). The game showed its first
loading screen with no sound, and the screen never moved on. The cause was a
bug in ReXGlue's **XMA audio decoder**, not in our recompiled code. It's fixed
by a local SDK patch:
[`patches/rexglue-sdk/0001-xma-release-held-back-frame.patch`](../../patches/rexglue-sdk/0001-xma-release-held-back-frame.patch).

**Result:** loading finishes in about 10 s, the game then streams
`movie2.rcf` (an intro movie), and the audio output carries real sound in
5.1 (measured, see the end).

---

## Background: what XMA is

The Xbox 360 has a hardware **XMA decoder**: a chip that turns compressed
audio (XMA, a WMA Pro variant) into 16-bit PCM. A game talks to it through:

* **Contexts**: 64-byte structs in memory, one per sound being decoded. They
  hold two *input* buffer pointers (compressed data, in 2 KB packets), one
  *output* ring buffer (PCM, in 256-byte blocks), and read/write offsets.
* **Registers** at `0x7FEA0000+` (memory-mapped I/O): *Kick* (start decoding
  a context), *Lock* (pause it so the CPU can edit it safely), *Clear*.

The SDK emulates the chip in `thirdparty/rexglue-sdk/src/audio/xma_*.cpp`,
using FFmpeg to do the actual decoding.

| Register | Address | Meaning |
|---|---|---|
| ContextArrayAddress | `0x7FEA1800` | physical address of the context array |
| (unknown, reset) | `0x7FEA1804` | written 0 then `0x03000000` to reset the engine |
| CurrentContextIndex | `0x7FEA1818` | which context the chip is working on |
| Kick *n* | `0x7FEA1940 + 4n` | bit *i* = start context 32n+i |
| Lock *n* | `0x7FEA1A40 + 4n` | bit *i* = pause context 32n+i |
| Clear *n* | `0x7FEA1A80 + 4n` | bit *i* = reset context 32n+i |

---

## Step 1: last session's conclusion was wrong (log level)

`03-first-boot.md` said "no `.rcf` file is opened". That was a logging
artifact. Kernel call tracing uses *noisy trace* macros, which need
**both** flags:

```bash
--log_level=trace --log_noisy=true
```

With those, the picture changed completely:

* 44 file opens: the 15 `D:\shaders\*.out`, then `default.rcf`,
  `movie1.rcf`, `movie2.rcf`, `english.rcf`.
* One thread read `default.rcf` steadily for ~7 s, then **stopped for good**.
  No error, no further kernel calls from it.

Same story for audio: the debug log showed only 10 audio frames and then
silence, which looked like the audio thread dying. In fact the SDK
**caps those diagnostic lines at 10** (`diag_pump_count < 10` in
`audio_system.cpp`). Audio kept running the whole time, it was just mixing
silence because no sound had been loaded yet.

## Step 2: where is each thread? (gdb)

Every game function is now a C++ function called `sub_<address>`, so a
normal debugger backtrace *is* the game's call stack. We ran the game under
gdb, paused it after 30 s, and listed every thread (this is now
`tools/guest_stacks.sh`). Almost all game threads were idle, waiting on
events. One wasn't:

```
Thread "audio::IOLoadTaskQueue":
    sub_824731D8    <- QueryPerformanceCounter (just `mftb`, reads the CPU clock)
    sub_8235AAE0    <- clock -> microseconds (x 1,000,000 / frequency)
    sub_8234C580    <- the caller: a loop
    sub_8234C8A8
    ...
```

At `0x8234C710–0x8234C730` in `sub_8234C580` there's a busy-wait:

```
while (samples_copied < samples_needed) {
    poll the decoder (sub_8234C398), feed it (sub_8234C220), copy what's ready
    spin for 50 microseconds  (two calls to the clock via stub 0x8232B5B8)
}
```

## Step 3: what that loop does

The game's audio loader decodes a short XMA sound effect **entirely into
memory** during loading (its name, `IOLoadTaskQueue`, fits). It asks for an
exact number of samples, taken from the sound's header, and waits until it
has them all.

Game functions we identified (all in the Radical audio engine plus the XDK's
XMA helper library):

| Address | Role |
|---|---|
| `0x8234C8A8` | "read N bytes of PCM from this XMA sound"; clamps N to `(header.end − header.start) − already_read` |
| `0x8234C740` | start/seek: skips a lead-in (here 384 samples) before real reading starts |
| `0x8234C580` | fill N samples; the 50 µs busy-wait loop |
| `0x8234C398` | pull decoded output from the context into the game's own 8192-sample rings |
| `0x8234C220` | feed compressed input when an input buffer is free |
| `0x8247D4D8` | read the ContextArrayAddress register, cache it at `0x8259787C` |
| `0x8247D828` | set an input buffer (pointer, packet count, "valid" bit) in the **software copy** |
| `0x8247D980` | "is an input buffer free?" |
| `0x8247DA48` | "how many decoded samples are ready?" |
| `0x8247DAD8` | get a pointer to decoded output |
| `0x8247E0B0` | create contexts; writes the **Clear** register |
| `0x8247E178` | **Lock** register write |
| `0x8247E210` | wait until the lock succeeds (125 ms timeout, then resets the engine) |
| `0x8247E2D8` | try-lock: checks CurrentContextIndex, copies hardware context → software copy, sets flag `0x20000` |
| `0x8247E428` | unlock: copies software copy → hardware context, writes the **Kick** register |

Notice the game never edits the hardware context directly. It locks, copies
it to a software copy, edits that, and copies it back while kicking.

## Step 4: detour, do the register writes even reach the SDK?

This took a while and turned out fine, but it's worth knowing. In a static
recompilation there's no memory trap for MMIO: the **recompiler** decides,
per instruction, whether a load/store might hit a hardware register, and
emits `REX_MM_LOAD/STORE` (which checks the address at runtime) instead of a
plain memory access. Its heuristic (`src/codegen/builders/`): a register built
with `lis rX,0x7FEA` (or `0x7FC8–0x7FCF` for the GPU) inside the same
function, plus **every byte-reversed store (`stwbrx`)**.

The game's XMA library hides the address: it computes
`(0x1FFA8650 + n) << 2` = `0x7FEA1940 + 4n` (Kick), so the constant
`0x7FEA` never appears. It still works because the write itself is a
`stwbrx`, which is always checked. We confirmed that the kick arrives (the
SDK logged `XmaContext 0: Codec reinit: 44100 Hz, 1 channel` on the game's
own thread, i.e. `Work()` ran inline from the Kick handler). If a future
hardware write gets lost, this heuristic is the first suspect: grep the
generated code for `REX_MM_` to see what was caught.

## Step 5: watching the decoder live

A gdb script put breakpoints on the game functions above. At a function's
entry, `$rdi` points to the `PPCContext`, so `((PPCContext*)$rdi)->r3.u32`
is guest register r3. Guest memory for address A sits at host `0x100000000 + A`.
We decoded the 64-byte hardware context at each step:

* `0x8247D828` (set input) ran **once**: 6,144 bytes (3 packets) at
  `0xB4120000`. That's the whole sound, and it fits in one buffer.
* After the next unlock/kick, the hardware context had `in0_valid=1`,
  3 packets, and the input read position advanced (`0x20 → 0x9DF → 0x114A`
  bits) while output blocks filled and the game drained them.

So feeding and decoding worked. At the hang, the context was fully drained:
no input left, output ring empty (write == read), nothing left to decode.
**The game had everything the decoder was ever going to give it, and wanted
more.**

## Step 6: counting samples (the actual bug)

At the hang we read the loop's registers (r22 = wanted, r25 = got) and the
sound's header:

| | samples |
|---|---|
| Stream length: 60 XMA frames × 512 | 30,720 |
| Game wants (header start 384 → end 30,592) | **30,208** |
| Decoder delivered (after the game's 384-sample skip) | **29,824** |

Short by exactly **one frame** (29,824 + 384 = 30,208 = 59 × 512).

The cause is in `XmaContext::Decode()`. Since SDK commit `f24299f`
("XMA loop wraps silenced the loop's last 192 samples", 2026-07-30), the
decoder outputs every frame **one decode late**. FFmpeg's decoded samples
run 192 samples ahead of the bitstream's numbering, so to line them up the
SDK holds each decoded frame back (`carry_frame_`) and completes it with
the first 192 samples of the *next* decoded frame. When the input runs
out, there is no next frame, so **the last frame of every stream is never
output**. The real chip has no such delay. A game that plays sounds in a
loop hardly notices a missing frame; a game that waits for an exact count
hangs forever.

## Step 7: the fix, and the bug hiding behind it

**Fix 1** (`FlushCarry()`): when no input buffer is valid and nothing is
pending, output the held-back frame: its 320 decoded samples plus 192
samples of silence standing in for the frame that doesn't exist.

The first test made things *worse*: 29,312 samples, one frame fewer. A
gdb breakpoint on `FlushCarry` showed it fired correctly, once, after the
60th decode, and that its 4 blocks filled the 8-block output ring **exactly**.
Then write == read. The SDK's main decode path marks that case as "full" by
clearing `output_buffer_valid`, but the "consume-only" path (used once the
input is gone) never did. So the game read write == read as **empty** and
lost the whole ring: 29,824 + 512 − 1,024 = 29,312. This was an older latent
bug that fix 1 exposed.

**Fix 2**: the consume-only path clears `output_buffer_valid` when it fills
the ring, and waits for free space before writing (`RingBuffer::Write`
doesn't check, so it could overwrite unread samples).

**Trade-off:** if a game is only *late* with its next input buffer (an
underrun) rather than finished, the 192 padding samples at that seam become
silence. The total sample count still matches the hardware, and that's
what games synchronize on.

## Result

With the patched SDK:

* The audio load task finishes. `default.rcf` loads completely, and the game
  creates 84 XMA contexts (instead of 1 and a hang), all within 10 s of
  starting.
* The game then streams `movie2.rcf` continuously (~110 reads/s), which looks
  like the intro movie playing.
* The mixed audio is real sound. Peak level of the 5.1 frames the game hands
  to the audio driver, sampled from a gdb breakpoint on
  `SDLAudioDriver::SubmitFrame`:

  | time | peak | note |
  |---|---|---|
  | 0–7 s | 0.000 | loading, silence (expected) |
  | 8–9 s | ~0.008 | quiet start |
  | 10–43 s | 0.05–0.73, varying | sound on front, surround and LFE channels |

`Resolve region is empty` turns out to belong to the **loading screen only**.
In the user's first run after the fix, all 1,806 of them came in the first
~8 s. The renderer then compiled shaders for a new scene and logged none for
the next 60 s. A picture displays throughout, so it's likely a harmless
zero-size copy the loading screen issues, but it's worth a look later.

## Follow-ups

* **Report upstream** (rexglue-sdk): the one-frame shortfall affects any
  game that decodes XMA to an exact length. Include the two numbers and the
  consume-only full-ring issue.
* Ask the user what the screen shows now (movie? menu?) and whether they
  hear sound.
* Keep an eye out for 192-sample dropouts in streamed music (the trade-off
  above). If that shows up, flush only after the input stays empty across
  several kicks.
