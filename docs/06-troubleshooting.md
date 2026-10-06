# Troubleshooting

> ⚠️ The port is in **alpha**. Some of the problems below are known
> rough edges that will go away; others depend on your system. If yours
> isn't here, [open an issue](https://github.com/Maxilure/MindOver-Recomp/issues)
> and attach your log (see [Logs and bug reports](#logs-and-bug-reports)).

Every path below is inside the folder you unpacked (the one with the
launcher in it). `user/` holds everything that is yours: saves, settings,
controls, photos and logs.

* [Every system](#every-system)
* [Linux](#linux)
* [Windows](#windows)
* [Logs and bug reports](#logs-and-bug-reports)

---

## Every system

### Setup

**"Not enough free space"**: the disc's files need about 6.5 GB, and the
whole folder grows to roughly 25 GB after the first build (your `.iso` can
be deleted afterwards). Free some space, or move the whole folder to a
bigger drive. Moving it is fine: the launcher finds everything relative to
itself.

**"default.xex isn't Crash: Mind over Mutant"**: the `.iso` is another game
or another region. The port is built for the **USA** release (title ID
565507FA). The image must be a full dump of the disc (about 7.8 GB).

**"ninja: manifest 'build.ninja' still dirty after 100 tries"**: your
computer's clock is wrong, so files look like they come from the future.
Set the correct date, time **and time zone** (on Windows: Settings → Time &
language → Date & time → **Sync now**), then run the step again.

**The build fails or the computer freezes while building**: building the
game needs a lot of memory (each of the ~140 big generated files takes 1-2
GB to compile). The launcher picks how many files it builds at once from
your free memory, so close other programs (browsers, games) before
pressing **Set up everything**. 16 GB of RAM is recommended. With less,
it still works, only slower.

**A step failed and you don't know why**: the full output of every step
is in `user/logs/setup-<date>_<time>.log`. The last red lines are usually
the reason. Press the step's button again after fixing it: Setup continues
from where it stopped.

### Starting and playing

**The movies, title screen or some menus are black**: the default
("emulated") picture needs a Vulkan feature called *fragment shader
interlock*. Most NVIDIA cards have it. Some other cards (likely AMD's)
don't, and then those screens stay black. The port has only been tested on
NVIDIA so far. Try the port's own renderer: launcher → Settings →
Picture → **Native renderer** (or press **F9** in the game). It is still in progress, so a few
effects may be missing in later levels.

**The game is slow or stutters**: the emulated picture is the heavy part.
**Native only (fastest)** (Settings → Picture) draws the game with the port's own renderer and
stops the emulated one, which usually runs much faster. Also check that
the frame rate cap isn't set higher than your computer can keep up with.

**Odd movement above 60 fps** (Crash falling for a split second, physics
glitches, animations too fast or too slow): frame rates above 60 are
**experimental**. The launcher marks them with a ⚠️. Go back to 60 (or 30,
the original) and the game behaves as on the Xbox 360.

**My controller does nothing**:
* Press a button while the game's window is **focused**. Input is ignored
  while you're in another window.
* Open the Controls menu (**F6** → Players tab): every controller found is
  listed there with the player it plays as. One set to "off" is ignored.
* A second, third or fourth player joins from the pause menu (**Join
  Game**), like in the original's co-op.
* Any controller SDL recognizes as a gamepad works. Some unusual ones need
  Steam Input or a driver to look like an Xbox controller.

**A controller was unplugged during play**: the game asks whether that
player wants to drop out. Plug it back in and press a button to continue,
or answer **Yes** to drop them out.

**Keys don't do what I expect**: all keys are listed and can be changed
in the Controls menu (**F6**). "Reset keys to defaults" there undoes your
changes. The file is `user/controls.toml`.

**F-keys do nothing, or something else happens**: the port uses F2 (rename
a save), F4 (all settings), F5 (cheats), F6 (controls), F8-F12 (renderer
tools). If another program (an overlay such as MangoHud, Steam or a
recording tool) uses the same keys, change that program's keys.

**I changed a setting and now the game won't start / looks wrong**: in the
launcher's Settings tab, press **Default** next to it. To reset everything,
delete `user/settings.toml` (the launcher writes a new one).

**Achievements stopped showing**: after a cheat that changes progress (F5),
achievements are switched off until you restart the game. This is on
purpose.

### Saves

**Where are my saves?** In `user/saves/`. Copy that folder to back them up
(a good habit during the alpha).

**I deleted a save by mistake**: deleted saves aren't destroyed. They're
moved into a `Deleted saves` folder, a few levels down inside
`user/saves/` (next to the `00000001` folder that holds the saves). Each
deleted save is a folder named after it and the time it was deleted; move
the folder inside it back into `00000001` to restore the save.

### Updates

**The update failed or the folder seems broken**: download the newest
release again, unpack it into a **new** folder, and copy your `user/`
folder (and `game/`, to skip extracting the disc again) into it. Then
start the launcher there and run Setup.

**Shaders or changed game data look wrong after an update**: delete the
`cache/` folder. It is rebuilt at the next start.

---

## Linux

**Double-clicking the launcher does nothing (or opens it as text)**: your
file manager may not run programs on a double-click. Right-click it →
Properties → allow it to run as a program, or start it from a terminal in
that folder:

```bash
./"Crash Mind over Mutant"
```

**"version `GLIBC_2.39' not found"**: your distribution is older than the
launcher supports (it needs glibc 2.39 or newer: Ubuntu 24.04, Fedora 40,
Linux Mint 22, current Arch or newer). The game also needs a recent clang
(20 or newer), which older systems don't ship.

**Setup says clang is too old**: some distributions name newer versions
`clang-20`, `clang-21`, and keep an older one as plain `clang`. The
launcher checks `clang --version`. Install a newer clang so that
`clang` itself is version 20 or newer (on Ubuntu, for example, from
[apt.llvm.org](https://apt.llvm.org/), then make it the default with
`update-alternatives`).

**A tool is missing but the launcher shows no package name**: your
distribution isn't one it knows (Arch, Debian/Ubuntu, Fedora, openSUSE and
their relatives). Install the tool with your package manager; the names are
usually close to the ones listed in [Building](01-building.md#0-prerequisites).

**The launcher isn't in my applications menu**: it adds itself the first
time it starts. If you removed it, or moved the folder, use the button at
the bottom right of the Play tab to add it again.

**The launcher says the game "was ended from outside"**: often
the system ran out of memory and the out-of-memory killer stopped the
game. Close other programs and try again. If it keeps happening, report it
with the log.

---

## Windows

**"Windows protected your PC" (SmartScreen)**: the launcher isn't signed
(signing costs money; this is a free fan project). Click **More info**,
then **Run anyway**. Windows only asks once.

**Where to put the folder**: a short path on an **NTFS** drive, for example
`C:\Games\Crash Mind over Mutant` or `D:\Crash Mind over Mutant`. Avoid:
* **Program Files**: needs administrator rights to write there.
* **OneDrive folders** (Documents, Desktop and Pictures often are): it
  would try to upload ~25 GB, and syncing can lock files during the build.
* **exFAT / FAT32 drives** (most USB sticks and SD cards): Setup links
  folders together, which only NTFS supports.
* **Very deep paths**: some of the build's files already have long names.

**Installing the tools**: the Setup tab names each missing tool. Most come
from **winget**, Windows' own package manager. Open *Terminal* (or
*PowerShell*) and type, for example:

```
winget install LLVM.LLVM
```

Visual Studio's C++ tools come from the **Visual Studio Installer**:
install "Build Tools for Visual Studio 2022" (`winget install
Microsoft.VisualStudio.2022.BuildTools`), open the Visual Studio
Installer, choose **Modify**, and tick the **Desktop development with
C++** workload. Make sure a **Windows 11 SDK** is ticked under
*Individual components*. Then press **Check again** in the launcher.

**A tool is installed but Setup still says "not found"**: close the
launcher and start it again (a program only sees tools installed before
it started). If it's still missing, restart Windows.

**"Python 3: not found (or only the Store shortcut)"**: Windows has a fake
`python` that only opens the Microsoft Store. Install the real one with
`winget install Python.Python.3.12`, then restart the launcher.

**"glslc: not found"**: glslc comes with the **Vulkan SDK**
(`winget install KhronosGroup.VulkanSDK`). Restart the launcher after
installing it.

**"Vulkan loader: not found"**: update your graphics card's driver from
NVIDIA, AMD or Intel. The game needs Vulkan.

**The build is very slow**: Windows Defender scans every file the build
writes. That is normal, just slower. You may choose to add the game's
folder as an exclusion in Windows Security, but you don't have to.

**The game crashed**: a crash report is written to
`user\logs\crash-<date>_<time>.txt`, next to the session log. Attach both
to your bug report.

**Windows is less tested than Linux**: the port has only run on a few
Windows PCs so far. Gameplay, keyboard and mouse, controllers, co-op, the
native renderer and 120 fps have been tried; renaming and deleting saves,
cutscene voices and unplugging a controller mid-game have not been
confirmed yet. Reports are very welcome.

---

## Logs and bug reports

Every game session writes its own log: `user/logs/play-<date>_<time>.log`.
The launcher's **Game log** tab shows it live, with errors in red and
warnings in yellow, and has buttons to open the file. Setup writes
`user/logs/setup-<date>_<time>.log`.

When reporting a bug on the
[issues page](https://github.com/Maxilure/MindOver-Recomp/issues), include:

1. what you were doing, and what happened instead;
2. the session's log (and on Windows a `crash-*.txt` if there is one);
3. your system (Linux distribution or Windows version, graphics card);
4. the port's version (shown in the launcher).

The logs contain no personal data beyond the folder paths on your
computer. Look through them before posting if that matters to you.
