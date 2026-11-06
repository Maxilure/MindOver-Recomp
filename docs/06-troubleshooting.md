# Troubleshooting

> ⚠️ The port is in **alpha**. Some of the problems below are known
> rough edges that will go away; others depend on your system. If yours
> isn't here, [open an issue](https://github.com/Maxilure/MindOver-Recomp/issues)
> and attach your log (see [Logs and bug reports](#logs-and-bug-reports)).

Every path below is inside the game's folder: on Windows the folder the
installer made (`Crash Mind over Mutant`, wherever you chose to install),
on Linux the folder you unpacked (the one with the launcher in it). `user/`
holds everything that is yours: saves, settings, controls, photos and logs.

* [Known issues](#known-issues)
* [Every system](#every-system)
* [Linux](#linux)
* [Windows](#windows)
* [Logs and bug reports](#logs-and-bug-reports)

---

## Known issues

Already known: no need to report these (adding details to the linked issue
is welcome).

* **Crash can break "heavy" objects** that only a titan should break
  ([#1](https://github.com/Maxilure/MindOver-Recomp/issues/1)).
* **The game hasn't been played through to the end** on the port yet:
  later levels may have problems nobody has seen.
* **Above 60 fps is experimental**: physics and animations can misbehave.
  60 or 30 is the safe choice.
* **Three or four players and the co-op camera are experimental.**
* **Only NVIDIA graphics cards have been tested.** On other cards the
  emulated picture may show black movies or menus (see
  [below](#starting-and-playing)).
* **The port's own renderer ("native") may miss effects** in areas not
  checked yet. The emulated picture is the reference: if something looks
  different, a report with a screenshot of both (**F1** → Switch renderer) helps.
* **Windows is less tested than Linux** (see [Windows](#windows)).

---

## Every system

### Setup

**"Not enough free space"**: the disc's files need about 6.5 GB, and the
whole folder grows to roughly 25 GB after the first build (your `.iso` can
be deleted afterwards). Free some space, or move the whole folder to a
bigger drive. On Linux, moving the folder is fine: the launcher finds
everything relative to itself. On Windows the Start menu and *Installed
apps* entries point at the old place: uninstall (keeping your saves) and
install again on the other drive, ticking **Copy saves and settings from an
older version** if you didn't keep them in place.

**"default.xex isn't Crash: Mind over Mutant"**: the `.iso` is another game
or another region. The port is built for the **USA** release (title ID
565507FA). The image must be a full dump of the disc (about 7.8 GB).

**"ninja: manifest 'build.ninja' still dirty after 100 tries"**: your
computer's clock is wrong, so files look like they come from the future.
Set the correct date, time **and time zone** (on Windows: Settings → Time &
language → Date & time → **Sync now**), then run the step again.

**The build fails or the computer freezes while building**: building the
game needs a lot of memory (each of the ~140 big generated files takes 1-2
GB to compile). The installer (Windows) or launcher (Linux) picks how many
files it builds at once from your free memory, so close other programs
(browsers, games) before the build starts. 16 GB of RAM is recommended. With less,
it still works, only slower.

**A step failed and you don't know why**: the full output of every step
is in `user/logs/setup-<date>_<time>.log`. The last red lines are usually
the reason. After fixing it, start **Mind over Recomp** again (Windows: from
the Start menu, it offers **Build it now**) or press the step's button again
(Linux): it continues from where it stopped.

### Starting and playing

**The movies, title screen or some menus are black**: the default
("emulated") picture needs a Vulkan feature called *fragment shader
interlock*. Most NVIDIA cards have it. Some other cards (likely AMD's)
don't, and then those screens stay black. The port has only been tested on
NVIDIA so far. Try the port's own renderer: in the game, **Options →
Graphics → Renderer** (or **F1** → Switch renderer). It is still in progress, so a few
effects may be missing in later levels.

**The game is slow or stutters**: the emulated picture is the heavy part.
**Native only** (the default; **Options → Graphics → Renderer** in the game)
draws the game with the port's own renderer and stops the emulated one, which usually runs much
faster: check it hasn't been switched to Emulated. Also check that
the frame rate cap isn't set higher than your computer can keep up with.

**Odd movement above 60 fps** (Crash falling for a split second, physics
glitches, animations too fast or too slow): frame rates above 60 are
**experimental**. Go back to 60 (or 30,
the original) and the game behaves as on the Xbox 360.

**My controller does nothing**:
* Press a button while the game's window is **focused**. Input is ignored
  while you're in another window.
* Open the Controls menu (**F1** → Controls, Players tab): every controller found is
  listed there with the player it plays as. One set to "off" is ignored.
* A second, third or fourth player joins from the pause menu (**Join
  Game**), like in the original's co-op.
* Any controller SDL recognizes as a gamepad works. Some unusual ones need
  Steam Input or a driver to look like an Xbox controller.

**A controller was unplugged during play**: the game asks whether that
player wants to drop out. Plug it back in and press a button to continue,
or answer **Yes** to drop them out.

**Keys don't do what I expect**: all keys are listed and can be changed
in the Controls menu (**F1** → Controls). "Reset keys to defaults" there undoes your
changes. The file is `user/controls.toml`.

**F-keys do nothing, or something else happens**: the port only uses **F1**
(the Tools menu: cheats, controls, renderer, photos, developer windows),
**F2** (hide / show the HUD) and **F11** (fullscreen). The other function
keys do nothing on purpose. If another program (an overlay such as
MangoHud, Steam or a recording tool) uses one of these keys, change that
program's keys.

**The HUD is gone**: F2 hides it (a card at the bottom says so); press F2
again.

**The game crashed**: the game closes and writes a crash report to
`user/logs/crash-<date>_<time>.txt`, next to the session log. Attach it to
your bug report (on Linux the launcher's **Report a problem** includes it). On Linux, versions before 0.1.1-alpha froze
instead of closing (the picture stopped and one CPU core stayed busy): close
the window in that case.

**I changed a setting and now the game won't start / looks wrong**: set it
back in **Options** (on Linux, the launcher's Settings tab also has a
**Default** button next to it). To reset everything, delete
`user/settings.toml`: the game starts with every setting at its default.

**The game hangs while starting (or freezes when a controller is plugged
in)**: a controller that has stopped answering holds the game up while it
asks every connected controller what it is. Seen with a DualSense over USB
whose connection had failed (on Linux, `journalctl -k` shows it
reconnecting over and over with errors `-110` or `-71`). Unplug the
controller and the game starts. To fix the controller: reset it (a
DualSense has a small reset button in a hole on the back: press it for
about 5 seconds), then use another cable or USB port, not a hub, or connect
it over Bluetooth.

**Achievements stopped showing**: opening the Cheats menu (F1 → Cheats)
switches achievements off, and the game says so ("Cheats Enabled"). Loading
a save turns them back on, as long as no cheat is still switched on (god
mode, no AI, game speed, freeze, hidden HUD, free camera) and the Cheats
menu is closed. A game SAVED while cheats were on is marked as made with
cheats: achievements stay off whenever that save is played ("Save Made With
Cheats" when it loads), and every later save of it keeps the mark. This is
on purpose.

### Saves

**Where are my saves?** In `user/saves/`, one file per save:
`CrashMOM GameSlot 1.sav`, `CrashMOM GameSlot 2.sav`, ... (the number is the
save's, its name is inside the file). Copy that folder to back them up (a
good habit during the alpha).

**I deleted a save by mistake**: deleted saves aren't destroyed. They're
moved into `user/saves/Deleted saves/`, named after the save and the time it
was deleted (`CrashMOM GameSlot 4 (2026-10-02 18.05.31).sav`). To restore
one, move it back into `user/saves/` and rename it to `CrashMOM GameSlot
<number>.sav` with a number no other save uses.

**I saved over the wrong save**: the game deletes a save before writing
over it; the port keeps that old version in `user/saves/Backups/` (the 10
newest per save, named with the time). Restore it the same way.

**My saves are in folders like `B13EBABEBABEBABE/565507FA/...`**: that's
where versions before 0.1.1 kept them (the emulated Xbox's layout). The
first start of a newer version copies them into `user/saves/` as `.sav`
files (listed in `copied-from-xbox-layout.txt`) and leaves the old folders
exactly as they were, as a backup. You can delete the old folders once
you're happy with the new ones.

### Updates

**Updating**: on Windows, open **Mind over Recomp** from the Start menu; it
shows a new version and updates the game (your `user/` folder stays). On
Linux, the launcher's Setup tab does the same.

**The update failed or the folder seems broken**: on Windows, download the
newest release and install it into a **new** folder, ticking **Copy saves
and settings from an older version** (pick the old folder). On Linux,
unpack it into a new folder, copy your `user/` folder (and `game/`, to skip
extracting the disc again) into it, then start the launcher there and run
Setup.

**Shaders or changed game data look wrong after an update**: delete the
`cache/` folder. It is rebuilt at the next start.

---

## Linux

**Double-clicking the launcher does nothing (or opens it as text)**: your
file manager may not run programs on a double-click. Right-click it →
Properties → allow it to run as a program, or start it from a terminal in
that folder:

```bash
./"Mind over Recomp"
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

On Windows the game is installed by **Mind over Recomp.exe**, the
installer: it installs the build tools, builds the game, adds **Crash Mind
over Mutant** to the Start menu and to *Installed apps*, updates it later
and uninstalls it. The game's settings are in the game itself (**Options**).

**"Windows protected your PC" (SmartScreen)**: the installer isn't signed
(signing costs money; this is a free fan project). Click **More info**,
then **Run anyway**. Windows only asks once.

**The installer won't install into a folder**: it refuses folders the build
can't work in:
* **Program Files** and the Windows folder: they need administrator
  rights to write there. The suggested place is `Games` in your user folder.
* **OneDrive folders** (Documents, Desktop and Pictures often are): it
  would try to upload many GB, and syncing can lock files during the build.
* **exFAT / FAT32 drives** (most USB sticks and SD cards): only NTFS (or
  ReFS) drives work.
* **Very long paths**: some of the build's files already have long names;
  pick a short place such as `C:\Games` or `D:\`.
* A folder that isn't empty, or a drive with too little free space (about
  15 GB: the finished install is about 9 GB). A folder that holds a game
  set up before is fine: see the next point.

**Installing again over a game folder you kept** (after uninstalling with
your saves kept, a lost Start menu entry, or a 0.1.0 folder): pick that
folder, or the folder it's in. The installer finds the game that was set
up there and uses it as it is: nothing is built again, and the disc image
is only needed if the disc's files (`game\`) are gone. It adds the Start
menu and *Installed apps* entries back. If that game is older, **Mind over
Recomp** offers the update afterwards.

**Installing the tools**: the installer lists each tool it needs and
**Install the missing tools** installs them with **winget**, Windows' own
package manager (Visual Studio's C++ build tools, LLVM, CMake, Ninja, Git,
Python, the Vulkan SDK and the Visual C++ runtime). Windows asks for
permission for some of them. It takes a while (Visual Studio's tools are
several GB).

**"winget isn't there"**: winget comes with Microsoft's **App Installer**.
Get it with the installer's **Download App Installer (from Microsoft)**
button, or from Microsoft's own link <https://aka.ms/getwinget> (it
downloads `Microsoft.DesktopAppInstaller_8wekyb3d8bbwe.msixbundle`; open it
to install). Don't search the Microsoft Store or the web for "App
Installer": some Windows versions hide the real one in the Store, and
searches turn up look-alike apps that aren't Microsoft's. Then press
**Check again**.

The first 0.2.0-alpha download (replaced on 2026-11-06) could say winget was
missing even when App Installer *was* installed (winget is a special kind of
link in Windows' `WindowsApps` folder that its check couldn't read). Download
the installer again from the release page; with the old one, the tools can
still be installed by hand (next entry).

**A tool keeps failing to install**: install it by hand (the list shows its
winget name), for example in *Terminal*:

```
winget install LLVM.LLVM
```

Visual Studio's C++ tools come from the **Visual Studio Installer**: install
"Build Tools for Visual Studio 2022" (`winget install
Microsoft.VisualStudio.2022.BuildTools`), open the Visual Studio Installer,
choose **Modify**, and tick the **Desktop development with C++** workload
(with a **Windows 11 SDK** under *Individual components*). Then press
**Check again**. If a tool still shows as missing, restart Windows.

**"Vulkan loader: not found"**: update your graphics card's driver from
NVIDIA, AMD or Intel. The game needs Vulkan.

**The install failed**: the installer shows what failed, opens the
[bug report form](https://github.com/Maxilure/MindOver-Recomp/issues/new/choose)
already filled in and the logs folder: attach the newest
`setup-<date>_<time>.log`. Starting **Mind over Recomp** again continues
where it stopped.

**Uninstalling**: Windows' *Settings → Apps → Installed apps* → **Mind over
Recomp (Crash Mind over Mutant)** → Uninstall, or **Uninstall** in Mind
over Recomp itself. **Keep my saves, settings and logs** (on by default)
leaves the `user` folder; everything else in the game's folder is removed.
Your `.iso` and the build tools stay (other programs may use the tools:
remove them from *Installed apps* if you want).

**The build is very slow**: Windows Defender scans every file the build
writes. That is normal, just slower. You may choose to add the game's
folder as an exclusion in Windows Security, but you don't have to.

**Windows is less tested than Linux**: the port has only run on a few
Windows PCs so far. Gameplay, keyboard and mouse, controllers, co-op, the
native renderer and 120 fps have been tried; the installer's update and
uninstall, renaming and deleting saves, cutscene voices and unplugging a
controller mid-game have not been confirmed yet. Reports are very welcome.

---

## Logs and bug reports

Every game session writes its own log: `user/logs/play-<date>_<time>.log`.
On Linux the launcher's **Game log** tab shows it live, with errors in red
and warnings in yellow. Building writes `user/logs/setup-<date>_<time>.log`.

What a log holds: a header (the port's version, your system, CPU and RAM,
how the game was started, the settings you changed), the graphics card and
driver, then the game's events: frame rate every 5 seconds, the sounds it
plays, co-op joins and players' states, which player a menu answers to,
and every menu screen change. On Linux the launcher adds a last line saying
how the session ended (closed normally, crashed, ended from outside). That's a few
MB per hour of play. Old logs are deleted, oldest first, once the folder
passes 300 MB (setting `logs_budget_mb`). To log only the basics, set
`event_logs = false` in `user/settings.toml` (or in the Linux launcher's
Settings tab).

### Reporting a bug

**Windows**: open the
[bug report form](https://github.com/Maxilure/MindOver-Recomp/issues/new/choose),
describe the problem and drag the session's log (`user\logs\play-<date>_<time>.log`,
plus a `crash-*.txt` if there is one) into its **Log** box. A save near the
spot where it happens helps too (`user\saves`).

**Linux**:

1. In the launcher, press **Report a problem...** (on the Play page, and
   highlighted after a session that crashed or logged errors).
2. Pick the session(s) where it happened (the newest is picked already),
   add a few words if you like, and press **Make the report**. It writes
   one `.zip` to `user/reports/` (**Change...** picks another folder; the
   launcher remembers it): those sessions' logs, your settings and
   controls, a summary, and optionally the F10 photos you took then and
   **a save** near the spot where it happens (it lets the problem be seen
   first-hand; a save holds only its name and the game's progress).
3. Press **Open the bug report form on GitHub**: the form opens with the
   version, system, graphics card and settings filled in. Describe the
   problem and drag the `.zip` into its **Log** box.

Check the [known issues](#known-issues) and the
[open issues](https://github.com/Maxilure/MindOver-Recomp/issues) first: if
your problem is already there, add your details to it instead. Never attach
game files (the disc image, extracted files or the built game).

Nothing is sent anywhere by the launcher: you attach the file yourself. The
report replaces your home folder's path (which holds your account name)
with `~` in every file and includes a save only when you pick one. Without the launcher,
attach `user/logs/play-<date>_<time>.log` by hand.
