# AGENTS.md: rules for AI coding agents working on Mind over Recomp

This file is for **AI coding agents** (Claude Code, Codex, Cursor, Copilot,
Gemini CLI, Aider, ...) that are helping someone contribute to
[Mind over Recomp](README.md), an unofficial native PC port of
*Crash: Mind over Mutant* (Xbox 360, Radical Entertainment, 2008).

Most agents read `AGENTS.md` from the repository root by themselves. If yours
doesn't, paste this file into its instructions (for Claude Code: a
`CLAUDE.md` line `@AGENTS.md`). Human contributors: the same rules, shorter,
are in [CONTRIBUTING.md](CONTRIBUTING.md).

The rules marked **(hard)** are checked in review: a pull request that breaks
one can't be merged as it is.

## 1. What the project is

- The game's `default.xex` (PowerPC) is **statically recompiled** into C++ by
  [ReXGlue](https://github.com/rexglue/rexglue-sdk), a Xenia-derived runtime
  (kernel, fibers, Vulkan GPU emulation, audio, input). We fix the
  recompiler's analysis, then **replace parts with native code** one at a
  time: the native Vulkan renderer, input, saves, sound, menus.
- Long-term goal: a functional C++ decompilation of the game's own functions,
  one function at a time, with the recompiled original kept as fallback and
  as the answer key for tests. Microsoft's libraries get replaced, not
  decompiled.
- Read first, as needed: [docs/02-how-the-port-works.md](docs/02-how-the-port-works.md)
  (big picture), [docs/01-building.md](docs/01-building.md) (build + SDK
  patches), [docs/findings/](docs/findings/) (everything learned so far: search
  it before investigating something, the answer is often there).

## 2. Legal: no game data in the repository (hard)

Everyone builds the port from their own copy of the game. The repository holds
only the project's own code, notes and art.

- Never commit or paste: `*.iso`, `*.xex`, anything from `game/`,
  `generated/default/` (that IS the game's code, translated), `out/`, `user/`,
  `cache/`, `logs/`, `photos/`, raw captures (`*.ppm`). They are gitignored:
  never force-add them, never weaken `.gitignore`.
- Never copy game content into tracked files: shader source, audio, textures,
  models, text strings from the disc's text banks, script bytecode,
  disassembly listings longer than a few illustrative lines. **Describe it,
  don't paste it** (an address, a size, a field layout, what it does).
- `assets/` is only for original art or free-licensed third-party art with its
  licence and source credited in [CREDITS.md](CREDITS.md). Never extracted game
  files.
- Screenshots in `docs/images/` are a curated exception: compressed (JPEG q90 /
  small PNG crops, ~100-150 KB), labelled, and used by a finding or the README.
- Before every commit: `git diff --cached --name-only` and check the list.

## 3. Write down what you find (findings)

Reverse engineering that isn't written down is lost. Every pull request that
*learns something about the game* records it.

- **New investigation** (a system, a bug's cause, a new feature that needed
  research): a new `docs/findings/NN-short-topic.md`, NN = the next free
  number, plus one row in the table of
  [docs/findings/README.md](docs/findings/README.md).
- **Small addition** to a known topic: a new numbered section in the existing
  finding (`grep -rl` the function address or topic first).
- **Trivial fixes** (typos, a build fix, a one-line obvious bug): no finding
  needed.

What a finding contains (look at any existing one for the style):

1. What was seen: the symptom or the question.
2. How it was found: tools, traces, measurements, and the **wrong turns**
   (they save the next person time).
3. What it is: functions (`sub_8XXXXXXX`), addresses, struct offsets, data
   formats, the game's own names when known.
4. What we changed and how it was verified (numbers, before/after).
5. What is still open or untested.

Voice for every tracked file (docs, README, code comments, commit messages):
**neutral and factual**. No personal details, no hardware/PC specs of a
specific person, no chat quotes, no dates of play sessions, no names of
people's save files. Unknown official names of places/enemies/objects: write
**(name TBD)** so they can be found and replaced later. Don't guess names.

## 4. Comments: heavy, and they explain why

This project is read by people learning how recompilation works. Comments are
a priority, not a nicety.

- Every new file starts with a header comment: what it is for, how it hooks
  into the game, and the formats/offsets it relies on.
- Comment every non-obvious line, every guest address and every offset
  (`// +0x24 = vertex stride (xnPrimBuffer), findings/10`).
- **Every `crash_mom_manifest.toml` entry gets a *why + how we found it*
  comment** (hard).
- Point to the finding that explains the code (`findings/NN s.M`).
- Plain words; explain jargon the first time it appears.

## 5. Working with the game's code

- **Verify before you fix the manifest (hard).** Disassemble with `xexdis`
  (`out/build/<preset>/xexdis game 0xSTART 0xEND`) before adding any
  `[entrypoint.functions]` entry. A wrong entry splits a real function and
  corrupts it silently. After a codegen, read the log for `Unresolved` lines.
- **Overriding a game function:** `extern "C" REX_FUNC(sub_X)` in `src/` (the
  generated one is a weak alias; the original stays callable as
  `__imp__sub_X`). **Changing registers mid-function:**
  `[[entrypoint.midasm_hook]]` in the manifest; check in the generated code
  that the register still holds what you expect *at that instruction*.
- **Never edit `generated/`** (it's rebuilt) **or `thirdparty/rexglue-sdk`
  directly.** SDK fixes go in `patches/rexglue-sdk/NNNN-name.patch` with a
  header (what broke, why, the fix, how it was found), listed in the patch
  table of [docs/01-building.md](docs/01-building.md). Check new patch files
  for stray `.orig`/`.rej` hunks.
- **Guest memory from C++ (hard):** never `base + addr` or `base[addr]`. On
  Windows guest `0xE0000000+` sits 4 KB further in host memory; Linux has no
  shift, so this bug only shows on Windows. Use `GuestPtr(base, addr)` from
  `src/guest_memory.h` or `guest::Load32(base, addr)` (`src/native/guest.h`). Guest data is
  **big-endian**.
- A pointer read out of a game object can be garbage: check it's readable
  before following it.
- **Windows builds too:** don't name functions or variables after Win32 APIs
  or `windows.h` macros (`near`, `far`, `FindText`, `PlaySound`,
  `ReplaceFile`, `min`, `max`, ...). No POSIX-only calls outside `#ifdef`'d
  platform code.

## 6. Design rules

- **Native over emulated.** Emulated-Xbox workarounds are temporary. Never
  drive a feature by **faking controller presses**: find the game's own
  decision (the function that asks "was the button pressed?" or "is X
  allowed?") and answer it.
- **Keep the original's defaults.** The game must look and play like the
  original unless the player changes a setting. New behaviour is an option
  (`REXCVAR` flag, usually also a row in the in-game Options screen), or it
  only affects things the original didn't have (players 3-4, new menus).
- **Player-facing UI uses the game's own look**: its menu library (Scrooby
  pages, `src/ui/scrooby.*`, `src/options/`) and its font. ImGui is only for
  developer tools (the F1 Tools menu, cheats, debug windows), and those stay
  **English only**.
- **Player-facing text is translatable**: wrap it in `tr(u"...")` /
  `trc(context, u"...")` / `trf(...)` (`src/language/texts.h`), then run
  `python3 tools/translations.py update`. The game's font has no `« »`, `Ё`,
  and several ASCII symbols; `tools/translations.py check` warns.
- **Players' saves are sacred.** Never move or delete save files or settings
  in a format change: copy, keep the old data as a backup. Deleting is only
  ever done by a player-triggered action.
- **Up to four players.** Code that handles "the player" usually needs to work
  for players 1-4 (`more_players` in `src/players/`): check every read, write
  and loop of a per-player table, not just the one you hit.

## 7. Testing

Say what you tested and what you didn't. "Untested on Windows" is fine;
claiming something works that wasn't run is not.

- Build: see [docs/01-building.md](docs/01-building.md). Generated files take
  1-2 GB of RAM each to compile: limit parallel jobs (`-j 4`-`6` on 16 GB).
- **Test on copies, never on real saves**: `--user_data_root=<a copy of
  user/saves>` and `--controls_file=<a copy>`. Pass `--audio_mute=true` to
  test runs, and `--fps_cap=` explicitly (settings files can change it).
- **Use the project's debug tools before gdb or screenshot guessing**
  ([findings/29](docs/findings/29-debug-console.md)):
  `--debug_console=<socket>` + `tools/mom.py` (game state, players, memory
  `read`/`eval`, `wait level`, `goto`, `who` = which code writes an address,
  `replay` of recorded input, `photo`, `profile`); `--load_save=<n|name|last>`
  jumps straight into a save; `--debug_fake_pads=1-3` adds controllers for
  players 2-4; crash reports land in `user/logs/crash-*.txt`.
- Renderer changes: compare both pictures of the **same frame** with the A/B
  tool (`--renderer=native --readback_resolve=full --debug_native_ab_ms=...`,
  [findings/11](docs/findings/11-native-characters.md)) and give the mean
  difference.
- Stop a test game by its PID with `kill -TERM` (or `-KILL`). **Never** send it
  `ABRT`/`SEGV`/`QUIT`/`BUS`/`TRAP`: the core dump of several GB of guest
  memory can exhaust RAM and take the whole desktop down. Never `pkill`/
  `killall` by name: you may close a game someone is playing.
- Don't build while the game is running: the build re-copies the shared
  libraries the running game has loaded.

## 8. Pull requests and commits

- One topic per pull request. Don't reformat files you didn't otherwise touch.
- Commit messages: a short summary line, then what changed and why, in the
  neutral voice of section 3.
- The pull request description says: what changed, why, how it was tested
  (and what wasn't), which finding documents it, and screenshots for visible
  changes. Fill in the checklist of the pull request template.
- AI assistance is welcome and normal here (most of the project is written
  with it): say so in the description. The human who opens the PR is
  responsible for it having been run on the real game.
- Licence: contributions are GPL-3.0 like the project; changes to
  `patches/rexglue-sdk/` stay BSD-3-Clause like the SDK. Add yourself to
  [CREDITS.md](CREDITS.md) with what you did.

## 9. Quick map

| Where | What |
|---|---|
| `crash_mom_manifest.toml` | recompiler config: function fixes, hooks, all commented |
| `src/native/` | the native Vulkan renderer (`shaders/` = GLSL) |
| `src/pddi/` | wrappers around the game's renderer (Radical's PDDI) |
| `src/input/` | keyboard + mouse, button prompts, mouse in menus |
| `src/players/` | up to four local players, co-op camera, outfits |
| `src/options/`, `src/ui/` | in-game Options screen, menu-library helpers |
| `src/saves/` | PC save files, save library, rename screen |
| `src/audio/`, `src/language/`, `src/achievements/` | sound out + XMA, languages + translations, achievements |
| `src/data/` | run-time patches of the game's data files (menus, fight trees) |
| `src/debug*`, `src/debug/` | debug console, write watch, crash reports, profiler |
| `launcher/` | the launcher and the Windows installer (no game code) |
| `tools/` | disc extractor, disassembler, Ghidra scripts, console client, release tools |
| `patches/rexglue-sdk/` | our SDK fixes |
| `docs/findings/` | the investigation notes |
