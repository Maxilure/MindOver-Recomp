# Roadmap: from "it runs" to a real PC port

Written 2026-09-25, right after the game first became playable (findings/06);
brought up to date 2026-11-12. Goal: preserve *Crash: Mind over Mutant* as a
proper PC game. Players bring their own disc; we never ship game data, only
our code and patches.

Where we are, in one line: the game's **CPU code is native** (recompiled to
C++), the **OS layer** is ReXGlue's re-implementation with more and more of
it replaced by the port's own code (the README's
[Going native](../README.md#-going-native) bars), and the game is **drawn by
the port's own Vulkan renderer** by default, while the emulated GPU still
runs underneath with its drawing switched off. See
[02-how-the-port-works.md](02-how-the-port-works.md) for the big picture.

The phases below are ordered by what unlocks what, not by what's most fun.
Phase 1 ("understand the game") feeds almost everything else, so it runs
alongside the others rather than strictly before them.

```
 0 Stabilize ──┬─> 2 PC input (remap, KB+M pictures)      ✅
               ├─> 3 Frame rate (60, then uncapped)       ✅ 60 · 🧪 above
 1 Understand ─┼─> 4 Native renderer (any res/aspect)     ✅ default · 🔧 GPU off
   the game    └─> 5 Mods (files, Lua, C++)               📋
 6 Windows build + packaging                              ✅
```

Status: ✅ done · 🧪 in testing · 🔧 in progress · 📋 planned. Difficulty
where it's still open: 🟢 days · 🟡 weeks · 🔴 a big project · 🔬 research first.

---

## Phase 0: stabilize (ongoing)

Play the whole game and fix what breaks. Nothing else matters if the game
crashes in chapter 3. The game hasn't been played through to the end yet.

* **Playtest** with `tools/play.sh` (one log per session, F1 → photo) and
  note what breaks, with the session name and roughly when. Players' reports
  come in through the bug form (the installer opens it filled in when a step
  fails; the Linux launcher packs the logs into a report `.zip`).
* Fix bugs as they come (same method as findings/04–06). Crashes write a
  report with the game's own call stack ([findings/29](findings/29-debug-console.md)
  section 5).
* 📋 Report our SDK fixes upstream (XMA, fibers, empty resolves, FBO black
  frames), so we're not carrying patches forever.
* 🔧 An **automated smoke test**: the pieces exist (the debug console, exact
  input replays at a fixed time step, [findings/29](findings/29-debug-console.md));
  a "does it still reach the first level with a picture?" run after every
  SDK update or big change is still to be put together.

## Phase 1: understand the game 🔬 (ongoing)

The recompiled C++ is machine-translated assembly: correct, but no names
and no structure. Most features need us to know *where* things happen:

| Find | Unlocks |
|---|---|
| ✅ The main loop and its **frame timing** (the 30 fps cap, delta time): [findings/07](findings/07-frame-rate.md) | Phase 3 |
| ✅ The **input path**: Radical's input manager polls the pad once per frame, an input map script turns buttons into game events, a stick-circle detector counts spins ([findings/23](findings/23-keyboard-and-mouse.md), [38](findings/38-keyboard-mouse-real-keybinds.md)) | Phase 2 |
| ✅ The **projection** code (field of view, aspect ratio): [findings/56](findings/56-widescreen-and-resolution.md) | widescreen (Phase 4) |
| ✅ The **UI system**: Radical's Scrooby menu pages, the front end's state tree, the prompt font's button pictures ([findings/30](findings/30-menus-and-hud.md), [31](findings/31-options-screen.md), [37](findings/37-keyboard-mouse-prompts.md)) | Phase 2 pictures, the port's own menus |
| ✅ The **save system**: the save file, the save manager's three-slot table, the one function that turns a slot into a file ([findings/24](findings/24-save-system.md), [32](findings/32-native-saves-and-profiles.md)) | more saves, names, deleting ([enhancements](05-enhancements.md#more-saves-with-names)) |
| 🔧 The **Lua scripts**: the 239 in `default.rcf` are plain Lua bytecode, not encrypted ([findings/19](findings/19-missing-dialogue-5-1-xma.md) section 1; only the loose `script/ahoy.blua` is); the fight trees run as compiled copies of their scripts ([findings/22](findings/22-ground-contact-high-fps.md)). Next: decompile them | Phase 5 scripts |
| ✅ Radical's **PDDI** renderer interface: classes, vtables and a frame's recipe ([findings/08](findings/08-renderer-map.md)), every material met so far redrawn natively ([findings/09](findings/09-native-first-screens.md) onward); later areas' materials as they come up | Phase 4 |

Method: name functions as we identify them, record each discovery in
`docs/findings/`. Tools: `xexdis`, Ghidra pseudo-C, the call graph and RTTI
scripts, gdb, the debug console (memory, write watches, input replays).

## Phase 2: PC-quality input ✅

No mouse look: the game's camera is fixed. It follows Crash on its own and
the player can't turn it (not with a stick either), so the mouse is only
useful for its buttons, the wheel, the menus and aiming.

* ✅ **Keyboard and mouse as a real input**, not a pretend controller: the
  keys go into the game's own controller state, menus and play give keys
  their own jobs (Escape pauses in play, goes back in menus), Spin answers
  the game's own "stick circle done" question
  ([findings/23](findings/23-keyboard-and-mouse.md), [38](findings/38-keyboard-mouse-real-keybinds.md)).
* ✅ **Rebinding in the game's own look**: Options → Controls lists every
  key, "press the new key", defaults on a question box
  ([findings/31](findings/31-options-screen.md) section 9).
* ✅ **Key and controller pictures in the game's prompts**: the font's
  button pictures redrawn with the bound keys, or with the buttons of the
  controller in use (Xbox, PlayStation, Switch), per player
  ([findings/37](findings/37-keyboard-mouse-prompts.md)).
* ✅ **The mouse in every menu**, typing names, mouse aiming and a Wii-style
  cursor ([findings/39](findings/39-mouse-in-menus.md), [47](findings/47-mouse-reticle-and-wii-cursor.md)).
* ✅ **Controllers read by the port itself** through SDL, with rumble and
  its strength in Options ([findings/38](findings/38-keyboard-mouse-real-keybinds.md)
  sections 8-9).
* 📋 Still open: remapping a controller's buttons.

## Phase 3: frame rate

1. ✅ **60 fps**, the default ([findings/07](findings/07-frame-rate.md)).
   The cap was the renderer's vsync mode; game logic is time-based, so
   movement speed matches 30 fps. 30 (the original's rate) is one Options
   pick away.
2. 🧪 **Above 60 / uncapped** (experimental): any cap is paced by the
   clock, frames shown right away, the main thread asleep in between.
   Frame-rate quirks found so far are fixed one by one: brief ground losses
   when stepping down ([findings/22](findings/22-ground-contact-high-fps.md)),
   moving platforms' minimum steps (section 9 there). Exact input replays at
   a fixed time step compare two frame rates frame by frame
   ([findings/29](findings/29-debug-console.md) section 6). Anything that
   counts frames instead of time could still misbehave.

## Phase 4: native renderer (see [04-native-renderer.md](04-native-renderer.md))

Before: every frame went game → fake Xbox GPU commands → ReXGlue's
translator → Vulkan. Now it goes game → **the port's Vulkan code**: the
game's graphics layer (PDDI) is hooked and its materials are drawn by GLSL
shaders of our own.

* ✅ **The default picture**: every screen and every area playtested so far
  is drawn natively (findings 09 to 21, 42 to 44), and the emulated GPU
  skips its drawing while our picture is shown (`--native_only`, on by
  default). A gap reporter photographs and logs anything not drawn
  natively the first time it shows up.
* ✅ **Graphics options**: render resolution in pixels, anti-aliasing up to
  8x, motion blur, depth of field, shadow softness, sharpening, texture
  sharpness ([findings/34](findings/34-motion-blur-and-effect-settings.md)
  to [36](findings/36-texture-sharpness.md), [41](findings/41-shadow-softness-sharpening.md),
  [45](findings/45-render-resolution.md)).
* ✅ **Widescreen** wider than 16:9, up to 32:9, with the HUD in place or at
  the screen's edges ([findings/56](findings/56-widescreen-and-resolution.md)).
* 🔧 **The emulated GPU off entirely** (the README's "Graphics driver" bar):
  once nothing is left that only it draws or answers.
* 📋 Frame interpolation, if some system turns out to need a fixed rate.

Precedent: *Unleashed Recompiled* (Sonic Unleashed, 360) took the same
route and got high resolutions, ultrawide, high frame rates and mods.

## Phase 5: mods 📋

1. 🟡 **File replacement**: a `mods/` folder that overrides files inside the
   `.rcf` archives. The groundwork exists: the port already changes game
   files at run time (its own menus, fonts and pictures are built from the
   player's disc this way, `src/data/`). The `.p3d` model format is shared
   with *The Simpsons: Hit & Run*, whose modding community documented it.
2. 🟡 **Lua mods**: the game's scripts are plain Lua bytecode (not
   encrypted, findings/19), so once they're decompiled, mods can add or
   change gameplay scripts.
3. 🟢–🟡 **C++ plugins**: we can already replace or wrap any game function
   (`REX_HOOK`). A small plugin API makes that available to modders.
4. Later: a mod manager (enable/disable, load order).

## Phase 6: Windows build and packaging ✅

* ✅ **Windows**: the same code builds and runs on Windows (Vulkan).
* ✅ **An installer** that takes the player's own `.iso`: install, update
  and uninstall, with the game ready-built (only the disc supplies the
  game's files), updates from inside the installed copy, the previous
  version kept and put back if an update fails.
* ✅ **Releases** on GitHub, with a troubleshooting page for players' known
  problems ([06-troubleshooting.md](06-troubleshooting.md)).
* 📋 A ready-built **Linux** release (today: build from source,
  [01-building.md](01-building.md)).

## Beyond: the game's own code, readable

The recompiled code works, but it is machine-translated. Over time, game
functions are rewritten by hand in plain C++, one at a time, each checked
against the recompiled original it replaces (which stays as the fallback).
The first one: the characters' skinning, bit-identical and faster
([findings/57](findings/57-main-thread-performance.md), `src/decomp/`).
