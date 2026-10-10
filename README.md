# Mind over Recomp

An unofficial native PC port of **Crash: Mind over Mutant** (Xbox 360, 2008,
Radical Entertainment), for **Linux and Windows**. It is made by
**statically recompiling** the original Xbox 360 program into C++ with
[ReXGlue](https://github.com/rexglue/rexglue-sdk), then improving it piece
by piece with native code.

> [!WARNING]
> **The whole project is in alpha.** The game is playable, but expect bugs,
> crashes and rough edges. Back up your saves, and expect features marked
> *experimental* to change or break. It has been tested on only a few
> computers so far.

> [!IMPORTANT]
> **No game files are included, and none ever will be.** You need your own
> Xbox 360 disc of *Crash: Mind over Mutant* (USA), dumped to an `.iso`.
> The launcher builds the game on your computer from that disc.

Developed with extensive AI assistance: see
[Development approach](#development-approach).

![Wumpa Island: the port's own renderer (left) and the emulated Xbox 360 GPU (right), on the same frame](docs/images/wumpa-island-native-vs-emulated.jpg)

## Download and play

**[⬇ Download the latest release](https://github.com/Maxilure/MindOver-Recomp/releases/latest)**

| System | Download | Needs |
|---|---|---|
| **Windows** 10 / 11, 64-bit | `...-windows-x86_64.zip` | an NTFS drive, not a OneDrive folder |
| **Linux** x86-64 | `...-linux-x86_64.tar.gz` | glibc 2.39+ (Ubuntu 24.04, Fedora 40, Mint 22, Arch, or newer) |

Both need a **Vulkan** graphics card (only NVIDIA has been tested so far),
**16 GB RAM** recommended, **~25 GB of free disk space**, and an internet
connection for the first setup.

1. **Unpack** the download into a folder with enough space (on Windows: a
   short path such as `C:\Games\`).
2. **Start the launcher**, *Mind over Recomp*, in that folder.
   Windows may warn about an unknown program: **More info → Run anyway**.
3. **Setup tab:** install the build tools it lists (it shows each
   package's name for your system), pick your `.iso`, then press
   **Set up everything**. The first time takes a while: it downloads
   ~550 MB and builds the game on your computer.
4. Press **Play**. Next time, **Continue** jumps straight into your last save.

The launcher also has your saves, the settings, a live game log, and
updates: when a new version is out, it tells you, and updating keeps your
saves and settings.

Something not working? See **[Troubleshooting](docs/06-troubleshooting.md)**
(Linux and Windows) and the [known issues](docs/06-troubleshooting.md#known-issues).
Found a bug? Press **Report a problem...** in the launcher: it packs the
session's log and your settings into one file and opens the
**[bug report form](https://github.com/Maxilure/MindOver-Recomp/issues/new/choose)**
with your version, system and graphics card filled in.

## What works

* **The whole game boots and plays**: intro movies, menus, cutscenes with
  5.1 sound, gameplay, saving and loading. It hasn't been played through to
  the end yet.
* **60 fps**, with the same game speed as the original's 30. Higher frame
  rates (120, 144, uncapped) work but are **experimental**.
* **Keyboard and mouse**, with every key rebindable (**F6**), and any
  controller SDL recognizes. Both at once, or one per player.
* **The port's own Vulkan renderer** (in progress): it draws every area
  tested so far with the same look as the original, and on its own
  ("Native only", the default) runs much lighter than the emulated picture.
  The emulated picture is one choice away (Options → Display → Renderer, or
  **F9**).
* **Windows** builds and runs. It is less tested than Linux.

## Going native

The game's own code already runs as native PC code. What is still emulated
is the **Xbox 360 around it**: the system services the game asks for
(graphics driver, profiles, saves, controllers, sound...), answered today by
the ReXGlue runtime. The goal is to answer every one of them with the port's
own code, so the game no longer needs a pretend Xbox at all.

<!-- native-progress:begin -->
<!-- Written by tools/native_progress.py --readme: edit the tool, not this block. -->
**Xbox services replaced: 17 of 66 (26%)** `▰▰▰▱▱▱▱▱▱▱`

| Xbox piece | Replaced | Becomes |
|---|---|---|
| Graphics driver | `▱▱▱▱▱▱▱▱▱▱` 0 / 20 | the port's own Vulkan renderer, then the emulated GPU off |
| Profiles and sign-in | `▰▰▰▰▰▰▰▰▰▰` 4 / 4 | no profiles at all: who plays = which controllers play |
| Saves and storage | `▰▰▰▰▰▰▰▰▰▰` 11 / 11 | plain save files in one folder: no storage devices, no content packages |
| Controllers | `▱▱▱▱▱▱▱▱▱▱` 0 / 3 | the input manager fed directly (keyboard and mouse, any SDL controller, rumble) |
| Sound output | `▱▱▱▱▱▱▱▱▱▱` 0 / 5 | each mixed frame straight to the PC's audio |
| XMA sound decoder | `▱▱▱▱▱▱▱▱▱▱` 0 / 2 | a software decoder instead of the emulated sound chip |
| System pop-ups and notifications | `▰▰▰▰▱▱▱▱▱▱` 2 / 5 | PC behaviour (the port's own messages); gone once profiles and saves are native |
| System messages (achievements, ...) | `▱▱▱▱▱▱▱▱▱▱` 0 / 3 | the port's own handlers |
| Console settings | `▱▱▱▱▱▱▱▱▱▱` 0 / 8 | the port's settings (language, region, video mode) |
| Quit and launch | `▱▱▱▱▱▱▱▱▱▱` 0 / 4 | quit the program cleanly |
| Network | `▱▱▱▱▱▱▱▱▱▱` 0 / 1 | not needed (no online features) |

Not counted: 89 kernel basics (threads, locks, memory, files), already thin translations to the PC's own.
<!-- native-progress:end -->

## Enhancements

Things the Xbox 360 game never had:

* **Unlimited, named saves** (in testing): one list in the game's own save
  screen, with rename and delete.
* **Up to four players** (in progress): co-op for four instead of two,
  with HUDs and markers for players 3 and 4, and a camera that keeps
  everyone in view (experimental).

Details and pictures: **[Enhancements](docs/05-enhancements.md)**.

## Controls

| Action | Keyboard and mouse | Controller |
|---|---|---|
| Move / walk | W A S D / hold Ctrl | left stick |
| Jump (twice: double jump) | Space | A |
| Light / heavy attack | left / right mouse button | X / Y |
| Spin | Q or middle mouse button | rotate the left stick |
| Block | Shift | RT |
| Jack / unjack a titan | E | B |
| Titan special / pocket a titan | F / R | LT / RB |
| Map / pause | Tab / Esc | Back / Start |

| Key | What it does |
|---|---|
| **F5** | Cheats menu, for testing: level ups, god mode, free camera, spawning |
| **F6** | Controls: change keys, pick which device plays as which player |
| **F9** | Switch between the emulated picture and the port's renderer |
| **F10** | Save a photo of the game into `user/photos/` |

Friends join from the pause menu (**Join Game**), as in the original's co-op.

## Screenshots

The port's own renderer next to the emulated Xbox 360 GPU, on the **same
game frame**:

![The game's second area (name TBD), with waterfalls and particles: native left, emulated right](docs/images/waterfall-area-native-vs-emulated.jpg)

![The save list: five saves, and "Create New Save" with its preview](docs/images/save-library-list.jpg)

More, each with the write-up of how that effect was rebuilt:
[reflections](docs/findings/13-native-reflections.md),
[water and particles](docs/findings/14-native-water-particles.md),
[depth of field](docs/findings/15-native-depth-of-field.md),
[anti-aliasing](docs/findings/16-native-msaa.md),
[texture filtering](docs/findings/17-native-mipmaps.md). Captured from the
maintainer's own copy of the game; the game's imagery belongs to its owners
(see [Disclaimer](#disclaimer)).

## Building from source

For developers. Players don't need this: the launcher does it all.

Linux: clang 20+, CMake 3.25+, Ninja, Python 3, Git, `glslc`. The full
walkthrough, including Windows, is in **[Building](docs/01-building.md)**.

```bash
git clone --recursive https://github.com/Maxilure/MindOver-Recomp.git
cd MindOver-Recomp

# the launcher (Setup tab = everything below, with progress bars)
cmake -S launcher -B out/build/launcher -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build out/build/launcher && out/build/launcher/crash_mom_launcher

# or by hand: unpack the disc, build the SDK with our patches, then the game
python3 tools/xiso_extract.py "Crash - Mind over Mutant (USA).iso" game/
(cd thirdparty/rexglue-sdk && git apply ../../patches/rexglue-sdk/*.patch \
  && cmake --preset linux-amd64 -DCMAKE_INSTALL_PREFIX=$PWD/../rexglue-install \
  && cmake --build out/build/linux-amd64 --config Release --target install \
  && cmake --build out/build/linux-amd64 --config RelWithDebInfo --target install)
cmake --preset linux-amd64-relwithdebinfo
cmake --build --preset linux-amd64-relwithdebinfo --target crash_mom_codegen
cmake --preset linux-amd64-relwithdebinfo   # first time only: picks up the generated code
cmake --build --preset linux-amd64-relwithdebinfo -j 6
tools/play.sh                               # one log per session in logs/
```

<details>
<summary>Repository layout</summary>

```
crash_mom_manifest.toml    recompiler config: every analysis fix and hook, commented
CMakeLists.txt             builds the port (generated code + ReXGlue + src/)
src/                       the port's own native code
  input/                   keyboard + mouse, the Controls menu (F6)
  players/                 up to four players, the co-op camera
  saves/                   the save list: any number of saves, rename / delete
  cheats/                  the Cheats menu (F5)
  data/                    changed copies of game data, built at run time from your files
  pddi/, native/           the game's renderer calls, and the port's Vulkan renderer
launcher/                  the launcher: setup, play, settings, log, updates (no game code)
assets/                    the port's own pictures (players 3-4's markers)
tools/                     disc extraction, disassembler, play/debug and release scripts
patches/rexglue-sdk/       our fixes to the ReXGlue SDK (applied to the submodule)
docs/                      guides, and findings about the game's internals
thirdparty/rexglue-sdk     the recompiler + runtime (git submodule, pinned)

# made on your machine, never committed:
*.iso, game/               your disc and its extracted files
generated/default/         C++ produced by the recompiler
out/, logs/, cache/        builds, logs, caches
user/                      YOUR files: saves, settings, controls, photos, logs
```

</details>

## Documentation

1. [How the port works](docs/02-how-the-port-works.md): static recompilation explained
2. [Building](docs/01-building.md): every build step, the launcher, releases
3. [Troubleshooting](docs/06-troubleshooting.md): common problems on Linux and Windows
4. [Enhancements](docs/05-enhancements.md): what only this port adds
5. [Roadmap](docs/03-roadmap.md): where the project is going
6. [Native renderer](docs/04-native-renderer.md): the port's own Vulkan renderer
7. [Findings](docs/findings/): everything learned about the game's internals

## Development approach

This project is developed with extensive AI assistance (Claude, via
Claude Code). The code, the reverse-engineering notes and the documentation
were largely written by the AI. The maintainer sets direction, tests every
change on the real game and reviews the results, but isn't a specialist in
recompilation or reverse engineering.

* Findings are verified against the running game (logs, screenshots,
  measurements), but have **not been reviewed by domain experts**. Treat
  them as working notes, and please report errors.
* Reviews from people experienced with Xbox 360 internals, Radical's
  engine, ReXGlue/Xenia or Vulkan are especially valuable.

## Contributing

Contributions of any kind (code, reverse engineering, testing, bug reports,
corrections) are welcome and will be credited by name in [Credits](#credits) and
[CREDITS.md](CREDITS.md).
Hand-written, expert work is especially valued. Bug reports go in the
[issues](https://github.com/Maxilure/MindOver-Recomp/issues): the launcher's
**Report a problem...** makes the file to attach (see
[Troubleshooting](docs/06-troubleshooting.md#reporting-a-bug)).

## Credits

Every contribution in detail: **[CREDITS.md](CREDITS.md)**.

* **Maxilure**: project lead, direction, ideas, playtesting, art.
* **[ReXGlue](https://github.com/rexglue/rexglue-sdk)**: the static
  recompiler and runtime that make this possible.
* **[Xenia](https://github.com/xenia-project/xenia)**: the Xbox 360 emulator
  that ReXGlue's runtime and GPU emulation build on.
* **Claude** (Anthropic): AI assistant that wrote most of the code and
  documentation (see [Development approach](#development-approach)).
* **You?** Every human contribution gets listed here and in
  [CREDITS.md](CREDITS.md) by name.

## Why this project exists

*Crash: Mind over Mutant* divided fans, but it's part of Crash's history.
This project aims to preserve the game and its legacy as a native PC game,
and to give it the upgrades and love it deserves.

## License

* The code in this repository is licensed under the **GNU General Public
  License v3.0** ([LICENSE](LICENSE)).
* The patches in `patches/rexglue-sdk/` modify ReXGlue's source code and stay
  under ReXGlue's **BSD 3-Clause** license, like the SDK itself
  (`thirdparty/rexglue-sdk`, which also builds on the
  [Xenia](https://github.com/xenia-project/xenia) emulator's code).
* The screenshots in `docs/images/` show the game's own imagery and are not
  covered by these licenses.
* The marker pictures in `assets/markers/` were drawn for this port, after
  the save-slot numbers of *Crash of the Titans* (Radical Entertainment,
  2007), to match the game's own "1" / "2" markers.

## Disclaimer

This is an unofficial fan project for game preservation. It is not
affiliated with, endorsed by, or sponsored by Activision, Sierra
Entertainment, Radical Entertainment, or Microsoft. *Crash Bandicoot* and
*Crash: Mind over Mutant* are trademarks of their respective owners. This
repository contains no game code, assets or data (only a few screenshots in
`docs/images/` that document the port's progress, and two marker pictures
drawn for the port in `assets/markers/`): you need your own legally obtained
copy of the game.
