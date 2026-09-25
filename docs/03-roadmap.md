# Roadmap: from "it runs" to a real PC port

Written 2026-09-25, right after the game first became playable (findings/06).
Goal: preserve *Crash: Mind over Mutant* as a proper PC game. Players bring
their own disc; we never ship game data, only our code and patches.

Where we are, in one line: the game's **CPU code is already native**
(recompiled to C++), the **OS layer is re-implemented** by ReXGlue, and the
**GPU is still emulated** (the game draws for Xbox 360 hardware and ReXGlue
translates that to Vulkan on the fly). See
[02-how-the-port-works.md](02-how-the-port-works.md) for the big picture.

The phases below are ordered by what unlocks what, not by what's most fun.
Phase 1 ("understand the game") feeds almost everything else, so it runs
alongside the others rather than strictly before them.

```
 0 Stabilize ──┬─> 2 PC input (remap menu, KB+M icons)
               ├─> 3 Frame rate (60, then uncapped)
 1 Understand ─┼─> 4 Native renderer (any res/aspect)
   the game    └─> 5 Mods (files, Lua, C++)
 6 Windows build + packaging: any time after 0
```

Difficulty legend: 🟢 days · 🟡 weeks · 🔴 a big project · 🔬 research first.

---

## Phase 0: stabilize (now)

Play the whole game and fix what breaks. Nothing else matters if the game
crashes in chapter 3.

* **Playtest** with `tools/play.sh` (one log per session) and write what you
  find in [playtest-notes.md](playtest-notes.md). The checklist is there.
* Fix bugs as they come (same method as findings/04–06).
* Report our SDK fixes upstream (XMA, fibers, empty resolves, FBO black
  frames), so we're not carrying patches forever.
* 🟢 Turn today's debug tools into an **automated smoke test**: scripted
  input + screenshots → "does it still reach the first level with a
  picture?" after every SDK update or big change.

## Phase 1: understand the game 🔬 (ongoing)

The recompiled C++ is machine-translated assembly: correct, but no names
and no structure. Most features need us to know *where* things happen:

| Find | Unlocks |
|---|---|
| ✅ The main loop and its **frame timing** (the 30 fps cap, delta time): [findings/07](findings/07-frame-rate.md) | Phase 3 |
| The **input path** (where XInput state becomes game actions) | Phase 2 |
| The **camera / projection** code (field of view, aspect ratio) | ultrawide (Phase 4) |
| The **UI system** (menus, button-prompt textures and font glyphs) | Phase 2 icons, a menu entry |
| The **Lua VM** and the `.blua` decryption | Phase 5 scripts, UI menus |
| 🟡 Radical's **PDDI** renderer interface: classes, vtables and a frame's recipe mapped, [findings/08](findings/08-renderer-map.md); individual methods still being named | Phase 4 |

Method: name functions as we identify them (a symbol list in `docs/`),
record each discovery in `docs/findings/`. Tools we have: `xexdis`, gdb
breakpoints on `__imp__sub_X`, the per-draw GPU log patch, scripted input.

## Phase 2: PC-quality input 🟡

Keyboard and mouse already work at a basic level (`--mnk_mode=true
--mnk_mouse=true`, `--keybind_*` flags, F4 settings), but it's controller
emulation with a config screen for developers. The goal:

1. 🟢–🟡 **An in-game remap menu of our own**: an overlay (ImGui, which
   the SDK already draws with) styled to fit the game, opened with a key.
   "Press a key to bind", mouse buttons and wheel, sensitivity, per-device
   profiles, saved to a config file. This needs **no** game modding: it
   feeds the input layer we already control.
2. 🟡 **Keyboard/mouse icons in the game's prompts.** The prompts are
   textures (or font glyphs) in the game's data. We find them once
   (Phase 1), then swap their pixels in memory at runtime: keyboard icons
   when the last input came from the keyboard, controller icons when it
   came from a pad. The GPU emulation re-reads textures the CPU changes, so
   no file edits and no Lua decryption are needed. Dynamic key names
   ("[E]" for whatever is bound) mean drawing those images ourselves.
3. 🟡 **Real mouse camera**: instead of pretending the mouse is a stick,
   hook the camera code (Phase 1) and feed it mouse deltas directly.
4. 🔬 Optional: an entry for our remap menu inside the game's own options
   menu. That's the part that needs the UI system understood, likely
   through the Lua scripts.

## Phase 3: frame rate 🟡 → 🔴

1. ✅ **60 fps**: `--fps_cap=60` ([findings/07](findings/07-frame-rate.md)).
   The cap was the renderer's vsync mode; game logic is time-based, so
   movement speed matches 30 fps. Title ~59, gameplay ~56 fps. Left: playtest
   it (cutscenes, physics, animations), and find why some gameplay frames
   take longer than 16.7 ms.
2. 🟡 **Uncapped / high refresh rate.** Good news from step 1: the game
   logic uses real elapsed time, so running faster shouldn't speed it up.
   What's left: the renderer still waits for the (emulated) 60 Hz vblank, so
   we need a faster guest refresh or its "no vsync" mode, then check
   anything else that counts vblanks, and watch for systems that break at
   very small time steps. Frame interpolation (Phase 4) stays the fallback
   if some system turns out to be frame-rate dependent.

## Phase 4: native renderer 🔴 (current focus, see [04-native-renderer.md](04-native-renderer.md))

Today every frame goes game → fake Xbox GPU commands → ReXGlue's
translator → Vulkan. A native renderer makes it game → **our Vulkan code**.
The game's engine has its own graphics layer (PDDI), which is a natural
place to cut in: hook those functions and implement them with Vulkan
directly. The game's GPU shaders get translated ahead of time.

What it unlocks: any resolution, any window shape (plus HUD fixes for
ultrawide or odd aspect ratios), interpolation for uncapped fps, graphics
options (AA, shadows, draw distance), and no more EDRAM emulation bugs like
today's. Precedent: *Unleashed Recompiled* (Sonic Unleashed, 360) took the
same route and got high resolutions, ultrawide, high frame rates and mods.

Cheap preview before this: the SDK can already render at 2×–3× internal
resolution (`draw_resolution_scale_x/y`, still to test with FSI).

## Phase 5: mods 🟡 → 🔴

1. 🟡 **File replacement**: a `mods/` folder that overrides files inside the
   `.rcf` archives (hook the file layer; we can already parse RCF). Covers
   textures, models, sounds, text. The `.p3d` model format is shared with
   *The Simpsons: Hit & Run*, whose modding community documented it.
2. 🔬 **Lua mods**: once `.blua` decryption is found, or by catching
   scripts just after the game decrypts them, mods can add or change
   gameplay scripts.
3. 🟢–🟡 **C++ plugins**: we can already replace or wrap any game function
   (`REX_HOOK`). A small plugin API makes that available to modders.
4. Later: a mod manager (enable/disable, load order).

## Phase 6: Windows build and packaging 🟢 → 🟡

* The SDK supports Windows (Vulkan and D3D12): mostly build work and
  testing (the Win32 half of patch 0002 is untested).
* A first-run setup that points at the player's own ISO and extracts it
  (`tools/xiso_extract.py` already does the hard part), so nobody has to
  run commands.
* Settings in a friendly config file, a proper icon, release builds.
