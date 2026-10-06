# Building the port

Step-by-step, from a disc image to a native executable. These steps were
verified on Linux with Clang 22 on 2026-09-25. Windows
should work with the `win-amd64-*` presets but hasn't been tested yet.

## 0. Prerequisites

| Tool | Version | Why |
|---|---|---|
| Clang | 20+ | ReXGlue requires it (C++23, and GCC is not supported) |
| CMake | 3.25+ | Build system |
| Ninja | any | Build backend used by the presets |
| Python | 3.8+ | Our tools in `tools/` |
| glslc (shaderc) | any | Compiles the native renderer's shaders at build time |
| GTK3 dev headers | 3.x | ReXGlue's Linux file dialogs |
| Vulkan driver | 1.3 | Rendering (any recent NVIDIA/AMD/Intel driver works) |
| Disk space | ~15 GB | ISO (7.3) + extracted game (6.0) + build (~2) |
| RAM | 16 GB recommended | Generated C++ files are huge; see step 4 |

Arch-based: `sudo pacman -S clang cmake ninja gtk3 vulkan-icd-loader shaderc`
Debian/Ubuntu: `sudo apt install clang-20 cmake ninja-build libgtk-3-dev libvulkan-dev glslc`
(Optional, for renderer work: the Vulkan validation layer, `vulkan-validation-layers`.)

You also need **your own dump** of the Xbox 360 disc as an `.iso` (a full
XGD2 dump is 7,835,492,352 bytes). Put it in the repo root.

## 1. Extract the disc

```bash
python3 tools/xiso_extract.py "Crash - Mind over Mutant (USA).iso" game/
```

This writes 83 files (6.0 GB) to `game/`. It takes seconds on an SSD.
Optional sanity check: `python3 tools/xex_info.py game/default.xex`
should report Title ID `565507FA`.

## 2. Build and install the ReXGlue SDK (once)

```bash
git submodule update --init --recursive      # SDK + its ~20 dependencies
cd thirdparty/rexglue-sdk
git apply ../../patches/rexglue-sdk/*.patch  # our SDK fixes (see below)
cmake --preset linux-amd64 -DCMAKE_INSTALL_PREFIX=$PWD/../rexglue-install
cmake --build out/build/linux-amd64 --config Release        --target install
cmake --build out/build/linux-amd64 --config RelWithDebInfo --target install
cd ../..
```

A few minutes per configuration.

* **Why both configs?** The `rexglue` code generator is *only* installed
  from the **Release** build (`CONFIGURATIONS Release` in the SDK's install
  rules). Without it, configuring the port fails with
  `IMPORTED_LOCATION not set for imported target "rex::rexglue"`.
  RelWithDebInfo gives us optimized runtime libraries *with* debug symbols,
  which is what you want while bringing a game up.
* The install registers itself in `~/.cmake/packages/rexglue/`, so the
  port's CMake finds it automatically.
* **Our SDK patches** live in `patches/rexglue-sdk/`, one fix per file,
  each with a header explaining what broke and why. They're applied to the
  submodule's working tree, not committed into it, so the submodule stays
  pinned to the upstream tag. After changing or re-applying a patch,
  re-run both `--target install` lines (incremental, seconds) and rebuild
  the port; the build copies the new `.so` files next to the exe. (That
  copy is the always-run `crash_mom_stage_libs` target. It used to be a
  post-link step, which silently skipped GPU-plugin-only changes because
  nothing relinks the exe when only a dlopen'ed plugin changes.)
  `git -C thirdparty/rexglue-sdk status` shows whether they're applied.

  | Patch | Fixes |
  |---|---|
  | `0001-xma-release-held-back-frame` | XMA decoder output ended one frame short, so the first loading screen hung with no sound ([findings/04](findings/04-loading-hang-xma.md)) |
  | `0002-fiber-destroy-wrong-thread` | Closing a finished thread's handle wiped the *closing* thread's current fiber, so the game froze on "Loading" after the intro movies ([findings/05](findings/05-post-intro-freeze-fibers.md)) |
  | `0003-empty-resolve-is-noop` | Tiled rendering's clipped-away resolves were logged as errors, ~240 lines per second, burying real errors ([findings/06](findings/06-black-screens-render-target-path.md)) |
  | `0004-external-guest-output` | Not a fix, a feature: `Presenter::SetGuestOutputExternal()` lets our native renderer own the window's picture; the emulated GPU then skips refreshing it ([04-native-renderer.md](04-native-renderer.md), milestone 2) |
  | `0005-shm-unlink-at-create` | Every run that crashed or was killed left its guest memory (hundreds of MB) in RAM-backed `/dev/shm` until reboot; after a day of testing the machine ran out of memory and the game died with SIGBUS at launch ([findings/09](findings/09-native-first-screens.md), section 9) |
  | `0006-idle-overlay-no-continuous-repaint` | The window repainted non-stop (600+ times a second, same picture each time) because the always-open achievement toast counted as an active overlay: wasted GPU time, and MangoHud showed the repaint rate instead of the game's frame rate. Now only an overlay that needs it (F3/F4/console, a visible toast) repaints continuously |
  | `0007-xma-multistream-buffer-carry` | 5.1 XMA sounds (three stereo streams sharing each buffer) went on at the next buffer's first packet instead of their own, so from the second 64 KB block on the centre and rear channels played the front channels' data: a cutscene's dialogue (centre channel) went silent after ~3.4 s ([findings/19](findings/19-missing-dialogue-5-1-xma.md)) |
  | `0008-physical-release-order` | "BaseHeap::Release failed because address is not a region start" (1-7 per session, when a new part of a level streamed in or on quitting a level): freeing physical memory released the physical pages before the window's own bookkeeping, and another thread could be handed those pages in between (e.g. D3D creating a texture while the XMA audio library freed its old buffer). The second step then erased the new owner's entry, so its own free failed later. Now the window's entry goes first, the physical pages second. Found with a temporary alloc/free history keyed by physical address |
  | `0009-skip-draws-flag` | Not a fix, a feature: GPU flag `skip_draws` makes the emulated GPU skip every draw and resolve (packets still parsed, so fences/interrupts/swaps carry on). Our `--native_only` sets it while the native picture is shown: the host GPU then only runs our renderer (15-20% busy at a low clock instead of ~97% with both renderers) |
  | `0010-guest-refresh-rate` | Not a fix, a feature: GPU flag `guest_refresh_hz` sets the guest's vblank rate (default: the video mode's 60 Hz). A game that vsyncs waits for the next vblank every frame, so 60 Hz meant 60 fps at most. First used to raise it to the `--fps_cap`; since the clock pacer (findings/07) frame_rate.cpp holds it at 60, so the SDK's `video_mode_refresh_rate` setting can't speed up the original 30 fps pacing |
  | `0011-local-players-share-profile` | Not a fix, a feature: users 1-3 can be signed in, sharing user 0's profile (same XUID, name, settings, saves), while the app says a device plays as them (`SetLocalPlayerPresentCallback`); `NotifyLocalPlayersChanged` broadcasts the sign-in change. Without it a second local player gets "You do not have an active gamer profile" (findings/23). No callback = user 0 only, as before |
  | `0012-vfs-longest-mount-wins` | Not a fix, a feature: the file system picks the device with the LONGEST matching mount path, so a host folder can be mounted inside the game drive's tree (changed copies of game data files at `D:\crashmom\`, `src/data/data_patcher.h`, findings/24 section 7.6). Without nested mounts nothing changes |
  | `0013-draw-every-nth-frame` | Not a fix, a feature (apply after 0009): GPU flag `draw_every_nth_frame` makes the emulated GPU draw and present only every Nth frame (skipped frames are parsed but neither drawn nor shown; read per draw, so it can change mid-frame). Our `--emulated_draw_every` (default 2) sets it while the native renderer draws too, e.g. dual mode: GPU 97% -> 60% busy at 60 fps (findings/20 section 5) |

  `patches/rexglue-sdk/debug/` holds **optional debugging patches** that
  the `*.patch` glob above deliberately skips. Apply one by hand when
  needed, and remove it with `git apply -R` when done:

  | Debug patch | Adds |
  |---|---|
  | `debug/0100-gpu-draw-resolve-diag` (apply after 0001–0003) | Logs every GPU draw and resolve of chosen frames (`CRASHMOM_DIAG_FRAMES=ms,ms`), plus env switches to skip suspect draws ([findings/06](findings/06-black-screens-render-target-path.md)) |

## 3. Configure the port

```bash
cmake --preset linux-amd64-relwithdebinfo
```

## 4. Recompile + build

```bash
# translate default.xex -> generated/default/*.cpp   (~11 s)
cmake --build --preset linux-amd64-relwithdebinfo --target crash_mom_codegen

# FIRST TIME ONLY: re-configure so CMake picks up the generated files
cmake --preset linux-amd64-relwithdebinfo

# compile everything
cmake --build --preset linux-amd64-relwithdebinfo -j 6
```

**Why re-configure?** `generated/rexglue.cmake` only adds the generated
sources if `generated/default/sources.cmake` exists *when CMake
configures*. On a fresh checkout it doesn't exist yet, so the build links
without any game code and fails with `undefined reference to
'PPCImageConfig'`. After the first codegen you never need to do this
again.

The first step isn't strictly required, since a normal build re-runs
codegen whenever `crash_mom_manifest.toml` changes. It's worth running on
its own, though, so you can **read its output**: every
`Unresolved ...` line is a crash waiting to happen (see
[findings/02-recompilation.md](findings/02-recompilation.md)).

**Why `-j 6`?** Codegen writes ~139 files totalling ~71 MB of C++, and
Clang can use 1–2 GB of RAM *per file*. With 16 GB of RAM, 6 parallel jobs
is safe. Raise it if you have more memory.

## 5. Run

```bash
out/build/linux-amd64-relwithdebinfo/crash_mom --game_data_root=$PWD/game --log_file=logs/run.log
```

`--game_data_root` is the extracted disc folder (it becomes `game:\` and
`D:\` inside the game). It's **required**, and a plain positional path
is rejected. Useful extras:

* `--log_level=trace --log_noisy=true` logs **every kernel call** (file
  opens and reads, thread creation, ...). Both flags are needed: the kernel
  call tracer uses "noisy trace" macros. `--log_level=debug` alone does
  *not* show them, which once misled us into thinking no files were opened.
  Expect ~15 MB of log per minute. Note that `--log_file` **appends**, so
  delete the old file first or you'll be reading two runs at once.
* `--gpu_plugin=<name>` (defaults to `xenos`, set in `src/crash_mom_app.h`).
* `--render_target_path_vulkan=fbo|fsi`: how the 360's EDRAM is emulated.
  We default to `fsi` (in `src/crash_mom_app.h`), because the SDK's own
  default (`fbo`) blacks out the movies and the title screen
  ([findings/06](findings/06-black-screens-render-target-path.md)). Pass
  `fbo` to compare.
* `--debug_capture_dir=<dir> --debug_capture_interval_ms=500`: saves what's
  on screen as `.ppm` screenshots (all-black frames are only logged). Keep
  the folder out of the repo, the pictures are game content.
* `--fps_cap=60`: lifts the game's 30 fps pacing
  ([findings/07](findings/07-frame-rate.md)). Default 30 = original.
  Values above 60 (e.g. `144`, or `0` for no cap) are **work in progress**:
  they run, but aren't confirmed stable yet. `--debug_log_fps` logs the
  frame rate every 5 s.
* `--renderer=native`: start on the native renderer's picture (F9 switches
  while playing; [04-native-renderer.md](04-native-renderer.md)).
  `--native_only`: the emulated GPU skips its drawing while the native
  picture is shown (much less GPU work). `--native_window`: both pictures,
  in two windows (F8).
* `--debug_input_script="8000:start,9500:start,30000:lsright:2000"`: a fake
  controller that presses inputs at those milliseconds after launch (held
  150 ms, or the optional third field). The first two taps skip intro movies.
  Inputs: `a b x y start back lb rb up down left right` (d-pad),
  `lsup lsdown lsleft lsright` / `rsup ...` (sticks), combine with `+`.
* `--debug_input_fifo=<path>`: the same fake controller, live. The game reads
  commands from that named pipe, e.g. `echo "down" > <path>` or
  `echo "lsright 2000" > <path>` (Linux).

In-game overlays: **F3** stats, **`** log console, **F4** settings. The
port's own keys: **F8** second window (native picture), **F9** switch the
picture, **F10** photo of the same frame from both renderers, **F11** /
**F12** renderer debugging (findings/20).

For the current state, see the Status table in the [README](../README.md).

### Your files: saves, settings, controls

Everything that belongs to the player stays **inside the game's folder**,
not in a system folder, so copying the folder moves the whole game, progress
included ([`src/game_folder.h`](../src/game_folder.h)):

```
user/saves/        saves (the emulated profile's folder layout inside, for now), achievements
user/settings.toml every changed setting (F4 saves here)
user/controls.toml keyboard + mouse keys, which device is which player (F6)
user/photos/       F10 photos          user/logs/   a log per run without --log_file
user/markers/      your own player 3/4 marker pictures (optional)
cache/             shader cache + changed game data: safe to delete, rebuilt
```

Which folder counts as the game's folder depends on where the exe is: a
build run from this source tree uses the **repository folder** (`user/` and
`cache/` are gitignored, and deleting `out/` never touches them); an
installed copy with the exe in a `program/` folder uses that folder's
parent; anything else uses the exe's own folder.

The **first start** copies saves from the old place
(`~/.local/share/crash_mom`), and `crash_mom.toml` / `controls.toml` from
next to the exe, keeping the save files' dates (the save list is ordered by
them). The originals are left where they were. If the game's folder can't be
written to, the game says so and quits instead of saving somewhere else.
`--user_data_root`, `--cache_root`, `--controls_file`, `--photo_dir` and
`--log_file` still choose other places (test runs use copies this way).

## Developer tools

| Tool | What it does |
|---|---|
| `tools/xiso_extract.py` | Unpacks an Xbox (360) disc image (`--list` to only list) |
| `tools/xex_info.py` | Dumps a XEX header: memory layout, SDK libraries, imports |
| `xexdis` | Disassembles any address range of the game code. Build with `--target xexdis`, run as `out/build/linux-amd64-relwithdebinfo/xexdis game 0x82300BA0 0x82300C00` |
| `src/debug_frame_capture.*`, `src/debug_input_script.*` | The `--debug_capture_dir` and `--debug_input_script` options above |
| `tools/play.sh [game flags]` | Playtest launcher: a fresh `logs/play-<date_time>.log` per session, reports crashes and error counts at exit, and how many F10 photos were taken (they go in `photos/`). `--dual`: two windows, the emulated picture in the main one and the native renderer's in a second one (F8 opens/closes it while playing) |
| `tools/guest_stacks.sh [secs]` | Runs the game under gdb, pauses it after *secs*, and prints which game function (`sub_XXXXXXXX`) every thread is in and what kernel call it waits on. First stop for any hang |

## ReXGlue gotchas we hit (SDK nightly 0.10.0.9, Sep 2026)

The SDK moves fast and its wiki lags behind. What's different from the wiki:

* `rexglue init` takes `--project-name`, `--xex-path`, `--game-root` and
  `--project-root` (the wiki still documents `--app_name` / `--app_root`).
* The config file is `<name>_manifest.toml`, with per-exe settings under
  `[entrypoint]`. Function overrides go in `[entrypoint.functions]`, CRT
  mappings in `[entrypoint.rexcrt]`, and so on.
* `rexglue init` **lowercased** the absolute path it wrote into the manifest
  (`/home/<name>/documents/...`). That breaks on case-sensitive Linux file
  systems, so we use relative paths instead.
* The generated `CMakePresets.json` hardcodes `clang-20` / `clang++-20`
  (Debian-style names). We changed it to plain `clang` / `clang++`.
  Debian/Ubuntu users with only `clang-20` installed can set
  `CC`/`CXX` or edit the preset.
