# Findings: first boot

The first runs of the recompiled executable (2026-09-25). How each crash
was fixed, and exactly where things stand now.

## Launch command

```bash
out/build/linux-amd64-relwithdebinfo/crash_mom --game_data_root=$PWD/game --log_file=logs/run.log
```

(`--game_data_root` is required. A plain positional path is *not* accepted
by this SDK version, whatever the wiki says. Add `--log_level=debug` to see
kernel calls.)

## Boot attempt log

| # | Symptom | Cause | Fix |
|---|---|---|---|
| 1 | `librexruntimerd.so: cannot open shared object file` | The SDK doesn't copy its `.so` files next to the exe on Linux | POST_BUILD copy in `CMakeLists.txt` |
| 2 | `--game_data_root was not provided` | Game path must be passed as a named option | Use `--game_data_root=...` |
| 3 | `VdInitializeRingBuffer: no GPU emulation loaded` (warning) | ReXGlue loads no GPU plugin by default | `CrashMomApp::OnPreSetup` defaults `gpu_plugin` to `"xenos"` |
| 4 | `[FATAL] Call to invalid or unregistered function at guest address 0x82480008` on the audio thread | A vtable-only function the analyzer missed (COM `AddRef`) | Wrote `tools/find_missing_functions.py`, which found 42; see [02-recompilation.md](02-recompilation.md) |
| 5 | Runs 60 s+ without crashing | — | — |

## What works right now

* All 28,833 recompiled functions are registered and the executable is stable.
* The runtime starts: guest memory, kernel, file system, SDL input (Wayland),
  and audio (6-channel, 48 kHz).
* **Vulkan** comes up on the GPU (tested on a GTX 1660 SUPER).
* The game's own code runs:
  * it checks for the optional `D:\sku.txt` (missing on the disc, which is normal)
  * `ConvertThreadToFiber` ×9 and `CreateFiber` ×1, so its task system is
    starting up on ReXGlue's native fibers
  * `XAudioRegisterRenderDriverClient`, then `XAudioSubmitRenderDriverFrame`:
    the audio engine is running
  * names its threads (`SetThreadName` ×5), creates XAM notification listeners
  * submits GPU work every frame (~30 fps)

## Where it's stuck: nothing on screen

Every frame, the GPU emulation logs:

```
[error] [gpu] PM4_DRAW_INDX_2(3, 8, 2): Failed in backend (... edram_mode=6)
[error] [gpu] Resolve region is empty
```

About 1,776 times per 60 s, so **exactly once per frame at 30 fps**.

What that means: the Xbox 360 draws into 10 MB of on-chip eDRAM, and a
**resolve** copies the finished image out into normal memory (a texture or
the front buffer). `edram_mode=6` is copy mode, and primitive type 8 with
3 vertices is the single rectangle a D3D9 `Resolve()` draws. The SDK
computes the copy rectangle from the draw's vertices, clamped to the
**scissor** and the **eDRAM surface pitch**
(`thirdparty/rexglue-sdk/src/graphics/util/draw.cpp`, around line 887).
The result is zero-sized, so the frame is never copied out and never
presented.

> **Correction (later the same day):** the two claims below were wrong.
> A picture *does* display (the user saw the loading screen), and `.rcf`
> files *are* opened. `--log_level=debug` simply doesn't log kernel calls:
> you need `--log_level=trace --log_noisy=true`. The real problem was a
> hang in the audio decoder, see [04-loading-hang-xma.md](04-loading-hang-xma.md).

~~Also, **no `.rcf` data file has been opened after 20 s**, so the game hasn't
started loading content. It's still in early startup, or it's waiting on
something.~~

## Next steps (for the next session)

*Superseded: steps 2–4 were answered by
[04-loading-hang-xma.md](04-loading-hang-xma.md). Step 1 is still open but
low priority, since a picture displays.*

1. **Log the resolve inputs.** Temporarily print x0/y0/x1/y1, the scissor
   and `RB_SURFACE_INFO.surface_pitch` at the "Resolve region is empty"
   site. That tells us which one is zero.
2. **Check the video mode handshake.** If the game asked the kernel for the
   display size (`VdQueryVideoMode` / `XGetVideoMode`) and got 0×0 or an
   unexpected mode, it would build zero-sized render targets. That would
   explain the empty resolve.
3. **Find out why no files load.** Break down what the main thread and the
   fibers are doing (debug log, or `REX_HOOK` a few game functions to trace
   them). Candidates: waiting on a XAM notification (sign-in/storage
   device), a fiber that never gets switched to, or a sync object that
   never gets signaled.
4. Compare against Xenia running the same disc: its log shows the expected
   sequence of kernel calls and file opens.
