# Troubleshooting

> ⚠️ The port is in **alpha**: expect rough edges. If your problem isn't
> here, [open an issue](https://github.com/Maxilure/MindOver-Recomp/issues)
> and attach your log ([how](#reporting-a-bug)).

**How to use this page:** find what you see in the list below, then follow
the steps under it.

**Where things are:** every path on this page is inside the game's folder.
On Windows that's the `Crash Mind over Mutant` folder the installer made; on
Linux, the folder you unpacked (the one with the launcher in it). The
`user` folder inside it holds everything that's yours: saves, settings,
controls, photos and logs.

## Find your problem

**Installing and building**
* [The clock looks wrong / "build.ninja still dirty after 100 tries"](#the-clock-looks-wrong)
* [Not enough free space](#not-enough-free-space)
* ["default.xex isn't Crash: Mind over Mutant"](#defaultxex-isnt-crash-mind-over-mutant)
* [The build fails or the computer freezes while building](#the-build-fails-or-the-computer-freezes)
* [A step failed and I don't know why](#a-step-failed-and-i-dont-know-why)

**Windows only**
* ["Windows protected your PC"](#windows-protected-your-pc)
* [The installer won't use my folder](#the-installer-wont-use-my-folder)
* [Installing over a game folder I kept](#installing-over-a-game-folder-i-kept)
* ["winget isn't there"](#winget-isnt-there)
* ["Couldn't find cmake" although CMake shows OK](#couldnt-find-cmake)
* ["file INSTALL cannot find ... TracyClient.dll"](#file-install-cannot-find-a-dll)
* ["fork: Resource temporarily unavailable" / exit code 254](#fork-resource-temporarily-unavailable--exit-code-254)
* ["Python was not found; run without arguments to install from the Microsoft Store"](#python-was-not-found-run-without-arguments-to-install-from-the-microsoft-store)
* [A tool keeps failing to install](#a-tool-keeps-failing-to-install)
* ["Vulkan loader: not found"](#vulkan-loader-not-found)
* [The install failed](#the-install-failed) · [Uninstalling](#uninstalling) · [The build is very slow](#the-build-is-very-slow)

**Linux only**
* [Double-clicking the launcher does nothing](#double-clicking-the-launcher-does-nothing)
* ["GLIBC_2.39 not found"](#glibc_239-not-found) · [clang is too old](#setup-says-clang-is-too-old) · [No package name shown](#a-tool-is-missing-but-no-package-name-is-shown)
* [The launcher isn't in my applications menu](#the-launcher-isnt-in-my-applications-menu)
* [The game "was ended from outside"](#the-game-was-ended-from-outside)

**Playing**
* [Black movies, title screen or menus](#black-movies-title-screen-or-menus)
* [Slow or stuttering](#slow-or-stuttering)
* [Odd movement above 60 fps](#odd-movement-above-60-fps)
* [My controller does nothing](#my-controller-does-nothing) · [A controller was unplugged](#a-controller-was-unplugged-during-play)
* [The game hangs while starting](#the-game-hangs-while-starting)
* [Keys or F-keys don't do what I expect](#keys-or-f-keys-dont-do-what-i-expect) · [The HUD is gone](#the-hud-is-gone)
* [The game crashed](#the-game-crashed)
* [A setting broke something](#a-setting-broke-something)
* [Achievements stopped showing](#achievements-stopped-showing)
* [Screen sharing shows a frozen picture](#screen-sharing-shows-a-frozen-picture)

**Saves and updates**
* [Where are my saves?](#where-are-my-saves) · [I deleted a save](#i-deleted-a-save-by-mistake) · [I saved over the wrong save](#i-saved-over-the-wrong-save)
* [Old save folders like `B13EBABEBABEBABE`](#my-saves-are-in-folders-like-b13ebabebabebabe)
* [Updating](#updating) · [The update failed](#the-update-failed) · [Wrong-looking shaders after an update](#shaders-or-game-data-look-wrong-after-an-update)

**[Known issues](#known-issues)** · **[Logs and bug reports](#logs-and-bug-reports)**

---

## Installing and building

### The clock looks wrong

**What you see:** the install stops with *"The computer's clock looks wrong
(files seem to come from the future)"*, or a log line *"ninja: manifest
'build.ninja' still dirty after 100 tries"*. It usually happens at
*Recompiling the game's code*.

**Why:** the build tool decides what to rebuild by comparing file dates.
A file dated later than "now" always looks newer, so it keeps trying to
catch up with it and gives up after 100 tries. Two things cause it: the
computer's clock or time zone is wrong, or the downloaded files themselves
carry a date a few hours ahead (0.2.0-alpha, see step 2).

**Since 0.2.1-alpha** the installer fixes such dates by itself before every
build, so this should no longer happen. If you still see it, follow the
steps below.

**Step 1: check the clock (Windows)**

1. Right-click the clock at the bottom right of the screen and pick
   **Adjust date and time**. (Or: **Settings → Time & language → Date &
   time**.)
2. Turn on **Set time automatically**.
3. Turn on **Set time zone automatically**. If it's greyed out or picks
   the wrong place, turn it off and choose your zone in **Time zone**
   yourself. Check the zone carefully: a clock that shows the right time
   with the **wrong time zone** is the usual cause.
4. Under *Additional settings*, press **Sync now**.
5. Open **Mind over Recomp** from the Start menu again. It continues from
   where it stopped (**Build it now**).

On Linux: `timedatectl` shows the time, the time zone and whether it's
synchronised; `timedatectl set-ntp true` turns syncing on. Then press the
step's button again.

**Step 2: the clock is right, but it still fails**

With **0.2.0-alpha**, the files you downloaded can be dated a few hours
ahead: on its release day, mostly in the Americas (time zones behind UTC).
The easiest fix is to download **0.2.1-alpha or newer** from the
[releases page](https://github.com/Maxilure/MindOver-Recomp/releases/latest)
and install again (into a new, empty folder; delete the failed one). To
fix the 0.2.0 files by hand instead, on Windows:

1. Close Mind over Recomp.
2. Open the Start menu, type **PowerShell** and open **Windows PowerShell**.
3. Paste this line, with your game's folder in place of the example path,
   and press Enter. It changes the date of every file dated in the future to
   *now*. Nothing else changes.

   ```powershell
   Get-ChildItem -Recurse -File "C:\Users\you\Games\Crash Mind over Mutant" | Where-Object { $_.LastWriteTime -gt (Get-Date) } | ForEach-Object { $_.LastWriteTime = Get-Date }
   ```

4. Open **Mind over Recomp** from the Start menu again and continue.

### Not enough free space

The disc's files need about 6.5 GB, and the whole folder grows to roughly
25 GB after the first build (on Windows the installer asks for about 15 GB
free; a finished install is about 9 GB). Your `.iso` can be deleted
afterwards.

* **Linux:** moving the whole folder to a bigger drive is fine. The
  launcher finds everything relative to itself.
* **Windows:** the Start menu and *Installed apps* entries point at the old
  place. Uninstall (keeping your saves), then install again on the other
  drive. Tick **Copy saves and settings from an older version** if your
  saves weren't kept in place.

### "default.xex isn't Crash: Mind over Mutant"

The `.iso` is another game or another region. The port is built for the
**USA** release (title ID 565507FA). The image must be a full dump of the
disc (about 7.8 GB).

### The build fails or the computer freezes

Building needs a lot of memory: each of the ~140 big generated files takes
1-2 GB to compile. The installer (Windows) or launcher (Linux) builds as many
at once as your free memory allows.

1. Close other programs (browsers, games) before the build starts.
2. Start the build again.

16 GB of RAM is recommended. With less it still works, only slower.

### A step failed and I don't know why

1. Open `user/logs/setup-<date>_<time>.log` (the newest one). The full
   output of every step is there; the last red lines are usually the reason.
2. Fix what it says.
3. Start again: on Windows, open **Mind over Recomp** from the Start menu
   (it offers **Build it now**); on Linux, press the step's button again. It
   continues from where it stopped.

Still stuck? [Report it](#reporting-a-bug) with that log.

---

## Windows

The game is installed by **Mind over Recomp.exe**, the installer. It
installs the build tools, builds the game, adds **Crash Mind over Mutant**
to the Start menu and *Installed apps*, and later uninstalls it. Each new
release comes with a new installer, which also updates an installed game.
The game's settings are in the game itself (**Options**).

Since 0.3.0 the release brings the ReXGlue SDK ready-built and its own
CMake, Ninja, glslc and Python (`source\buildtools\`), and every build step
runs with only those, LLVM and Visual Studio on its PATH. Most entries below
about CMake, Git, Python or the SDK step are for older installers: the
simplest fix there is the newest release's installer (**Update** keeps your
game, saves and settings).

### "Windows protected your PC"

That's SmartScreen. The installer isn't signed (signing costs money; this
is a free fan project). Click **More info**, then **Run anyway**. Windows
only asks once.

### The installer won't use my folder

It refuses folders the build can't work in. Pick another place, such as
`Games` in your user folder (the suggested one), `C:\Games` or `D:\`.

| Refused | Why |
|---|---|
| **Program Files**, the Windows folder | Writing there needs administrator rights |
| **OneDrive folders** (Documents, Desktop and Pictures often are) | It would upload many GB, and syncing can lock files mid-build |
| **exFAT / FAT32 drives** (most USB sticks, SD cards) | Only NTFS (or ReFS) drives work |
| **Very long paths** | Some build files already have long names |
| A folder that isn't empty, or too little free space (~15 GB) | A folder with a game set up before is fine: see below |

### Installing over a game folder I kept

After uninstalling with your saves kept, losing the Start menu entry, or
coming from 0.1.0:

1. Pick that folder (or the folder it's in) in the installer.
2. The installer finds the game set up there and uses it as it is: nothing
   is built again. The disc image is only needed if the `game` folder is gone.
3. It adds the Start menu and *Installed apps* entries back. If that game is
   older, choose **Update an installed game** instead (see [Updating](#updating)).

### "winget isn't there"

The installer installs its tools with **winget**, Windows' own package
manager. It comes with Microsoft's **App Installer**.

1. Press the installer's **Download App Installer (from Microsoft)**
   button, or open Microsoft's link <https://aka.ms/getwinget>. It downloads
   `Microsoft.DesktopAppInstaller_8wekyb3d8bbwe.msixbundle`.
2. Open that file to install it.
3. Press **Check again** in the installer.

Don't search the Microsoft Store or the web for "App Installer": some
Windows versions hide the real one in the Store, and searches turn up
look-alike apps that aren't Microsoft's.

**The first 0.2.0-alpha download** (replaced on 2026-11-06) could say winget
was missing even when App Installer *was* installed. Download the installer
again from the release page. With the old one, the tools can still be
installed by hand (next entry).

### "Couldn't find cmake"

The install stops with **Couldn't find cmake (is it installed?)** right
after "Configuring the SDK", although the tools page shows CMake as OK.

This was a bug in 0.2.0 and 0.2.1: the tools check accepted the copy of
CMake that comes with Visual Studio's C++ tools, but the build step didn't
look there. **Fixed in 0.2.2-alpha.**

1. Download **0.2.2-alpha or newer** from the
   [releases page](https://github.com/Maxilure/MindOver-Recomp/releases)
   and run its installer again.
2. Or, staying on 0.2.1: install CMake by itself, either with
   `winget install Kitware.CMake` in a terminal or from
   [cmake.org](https://cmake.org/download/) (tick **Add CMake to the PATH**).
   Then close the installer, open it again and continue.

### "file INSTALL cannot find" a DLL

The SDK step ("Building the SDK") stops at the very end with
**file INSTALL cannot find ".../TracyClient.dll": File exists.** (or another
`.dll`). The SDK compiled fine: the file was made minutes earlier and then
disappeared before the install step could copy it. That's almost always the
**antivirus** quarantining a freshly built file by mistake. (The "File
exists" part is a leftover error code; ignore it.)

1. Open **Windows Security > Virus & threat protection > Protection
   history**. If the DLL is listed, restore it / allow it.
2. Add the game's folder as an exclusion (**Manage settings > Exclusions**)
   so the next build isn't touched.
3. Run the installer again and continue.

TracyClient.dll is a profiler library nobody needs to play: installers newer
than 0.2.2-alpha no longer build it at all.

### "fork: Resource temporarily unavailable" / exit code 254

The step **"Finishing the SDK's libraries"** (or another Git step) stops
with lines like **dofork: child -1 - forked process ... died unexpectedly,
exit code 0xC0000142** and **fork: Resource temporarily unavailable**, then
**exit code 254**. Git for Windows' own shell couldn't start its helper
programs: Windows stopped each one while its libraries were loading
(`0xC0000142`), before Git ran any code. The download itself isn't the
problem.

Try these in order, and run the step again after each:

1. **Restart the PC.** Windows running short of resources is a common cause,
   and a restart clears it.
2. **Pause the antivirus** and close overlay or "game booster" programs
   while installing. Some of them load themselves into every program, and
   Git's shell is sensitive to that.
3. **Exploit protection:** open **Windows Security > App & browser control >
   Exploit protection settings**. If **Force randomization for images
   (Mandatory ASLR)** is **On by default**, set it to **Off by default**
   (Git's shell can't run with it forced on), restart, and try again.

Installers from 0.3.0-alpha on no longer use Git's shell to download the
SDK's libraries (each one is fetched with plain Git commands), so this error
should not come back there. Older installers: the steps above.

### "Python was not found; run without arguments to install from the Microsoft Store"

The step **"Building the SDK"** stops a few minutes in, at a line about
**glslang/build_info.h**, with this message just below it. Python itself is
fine (the installer used it to copy the disc). One of the SDK's libraries
looked for a program named `python3` on its own and found Windows'
**Microsoft Store shortcut** for Python instead (a stand-in file in
`AppData\Local\Microsoft\WindowsApps`). A python.org install has no
`python3.exe`, so the shortcut was the only match.

Installers from 0.2.2-alpha.3 on give the build the exact Python, Ninja, Git
and glslc the tool check found, so this can't happen there. With an older
installer:

1. Open **Settings > Apps > Advanced app settings > App execution aliases**
   and turn **off** the two entries for **python.exe** and **python3.exe**
   (the ones called "App Installer" or "Python").
2. Delete the file
   `source\thirdparty\rexglue-sdk\out\build\win-amd64\CMakeCache.txt` in the
   game's folder. It still remembers the shortcut, and a retry would use it
   again.
3. Run the installer again.

### A tool keeps failing to install

Install it by hand. The installer's list shows each tool's winget name.

1. Open **Terminal** (Start menu → type *Terminal*).
2. Type `winget install` and the name, for example:

   ```
   winget install LLVM.LLVM
   ```

3. Press **Check again** in the installer. If a tool still shows as
   missing, restart Windows.

**Visual Studio's C++ tools** need one more step:

1. `winget install Microsoft.VisualStudio.2022.BuildTools`
2. Open the **Visual Studio Installer**, choose **Modify**.
3. Tick the **Desktop development with C++** workload, and a **Windows 11
   SDK** under *Individual components*.
4. Install, then press **Check again**.

### "Vulkan loader: not found"

Update your graphics card's driver from NVIDIA, AMD or Intel. The game needs
Vulkan.

### The install failed

The installer shows what failed, opens the
[bug report form](https://github.com/Maxilure/MindOver-Recomp/issues/new/choose)
already filled in, and opens the logs folder. Attach the newest
`setup-<date>_<time>.log`. Starting **Mind over Recomp** again continues
where it stopped.

### Uninstalling

**Settings → Apps → Installed apps → Mind over Recomp (Crash Mind over
Mutant) → Uninstall**, or **Uninstall** in Mind over Recomp itself.

* **Keep my saves, settings and logs** (on by default) keeps the `user`
  folder. Everything else in the game's folder is removed.
* Your `.iso` and the build tools stay. Other programs may use the tools;
  remove them from *Installed apps* if you want.

### The build is very slow

Windows Defender scans every file the build writes. That's normal, just
slower. You can add the game's folder as an exclusion in Windows Security,
but you don't have to.

---

## Linux

### Double-clicking the launcher does nothing

Your file manager may not run programs on a double-click (or opens it as
text). Either right-click it → **Properties** → allow it to run as a
program, or start it from a terminal in that folder:

```bash
./"Mind over Recomp"
```

### "GLIBC_2.39 not found"

Your distribution is older than the launcher supports. It needs glibc 2.39
or newer: Ubuntu 24.04, Fedora 40, Linux Mint 22, current Arch, or newer.
The game also needs clang 20 or newer, which older systems don't ship.

### Setup says clang is too old

Some distributions name newer versions `clang-20`, `clang-21` and keep an
older one as plain `clang`. The launcher checks `clang --version`.

1. Install a newer clang (on Ubuntu, for example, from
   [apt.llvm.org](https://apt.llvm.org/)).
2. Make it the default, so that `clang --version` says 20 or newer (on
   Ubuntu: `update-alternatives`).

### A tool is missing but no package name is shown

Your distribution isn't one the launcher knows (it knows Arch,
Debian/Ubuntu, Fedora, openSUSE and their relatives). Install the tool with
your package manager; the names are usually close to the ones in
[Building](01-building.md#0-prerequisites).

### The launcher isn't in my applications menu

It adds itself the first time it starts. If you removed it or moved the
folder, press the button at the bottom right of the **Play** tab to add it
again.

### The game "was ended from outside"

Usually the system ran out of memory and stopped the game. Close other
programs and try again. If it keeps happening, [report it](#reporting-a-bug)
with the log.

---

## Playing

### Black movies, title screen or menus

The default **emulated** picture needs a Vulkan feature called *fragment
shader interlock*. Most NVIDIA cards have it; some others (likely AMD's)
don't, and then those screens stay black. Only NVIDIA has been tested so far.

**Fix:** use the port's own renderer: in the game, **Options → Graphics →
Renderer** (or **F1** → Switch renderer). It's still in progress, so a few
effects may be missing in later levels.

### Slow or stuttering

1. Check **Options → Graphics → Renderer** is set to **Native only** (the
   default). It draws the game with the port's own renderer and stops the
   heavy emulated one.
2. Check the frame rate cap isn't higher than your computer can keep up with.

### Odd movement above 60 fps

Crash falling for a split second, physics glitches, animations too fast or
slow: frame rates above 60 are **experimental**. Set the cap back to 60 (or
30, the original) and the game behaves as on the Xbox 360.

### My controller does nothing

1. Click the game's window so it's **focused**, then press a button. Input
   is ignored while another window is active.
2. Open **F1** → Controls → **Players** tab. Every controller found is listed
   with the player it plays as. One set to "off" is ignored.
3. Players 2-4 join from the pause menu (**Join Game**), like in the
   original's co-op.

Any controller SDL recognizes as a gamepad works. Some unusual ones need
Steam Input or a driver to look like an Xbox controller.

### A controller was unplugged during play

The game asks whether that player wants to drop out. Plug it back in and
press a button to continue, or answer **Yes** to drop them out.

### The game hangs while starting

(Or it freezes when a controller is plugged in.) A controller that stopped
answering holds the game up while it asks every connected controller what it
is. Seen with a DualSense over USB whose connection had failed (on Linux,
`journalctl -k` shows it reconnecting over and over with errors `-110` or
`-71`).

1. Unplug the controller: the game starts.
2. Reset the controller (a DualSense has a small reset button in a hole on
   the back: hold it for about 5 seconds).
3. Use another cable or USB port (not a hub), or connect it over Bluetooth.

### Keys or F-keys don't do what I expect

* **Keys:** all of them are listed and can be changed in **F1** → Controls.
  **Reset keys to defaults** there undoes your changes. The file is
  `user/controls.toml`.
* **F-keys:** the port only uses **F1** (Tools menu: cheats, controls,
  renderer, photos, developer windows), **F2** (hide / show the HUD) and
  **F11** (fullscreen). The others do nothing on purpose. If an overlay
  (MangoHud, Steam, a recording tool) uses one of these keys, change that
  program's keys.

### The HUD is gone

**F2** hides it (a card at the bottom says so). Press **F2** again.

### Screen sharing shows a frozen picture

On Windows, sharing or recording the game in **Fullscreen** (Discord, for
example) can show one frame that barely updates, while **Windowed** shares
fine. In **Options → Display → Window Mode** pick **Borderless Window**: a
window without a border over the whole monitor. If it still freezes:

* NVIDIA: **NVIDIA Control Panel → Manage 3D settings → Vulkan/OpenGL present
  method → Prefer layered on DXGI swapchain**.
* Share the **window** instead of the whole screen, or play in **Windowed**.

Please say on GitHub whether Borderless Window helped (which GPU, and
what the sharing app captured): it's new and not checked on many systems.

### The game crashed

The game closes and writes a crash report to
`user/logs/crash-<date>_<time>.txt`, next to the session log. Attach both to
your [bug report](#reporting-a-bug) (on Linux, the launcher's **Report a
problem** includes them).

On Linux, versions before 0.1.1-alpha froze instead (picture stopped, one CPU
core busy): close the window in that case.

### A setting broke something

(The game won't start, or looks wrong after a change.)

* Set it back in **Options** (on Linux, the launcher's **Settings** tab also
  has a **Default** button next to each setting).
* To reset everything, delete `user/settings.toml`. The game starts with
  every setting at its default.

### Achievements stopped showing

This is on purpose: **opening the Cheats menu** (F1 → Cheats) switches
achievements off, and the game says so (*"Cheats Enabled"*).

* **To get them back:** close the Cheats menu, switch every cheat off (god
  mode, no AI, game speed, freeze, hidden HUD, free camera), then load a save.
* **A save made while cheats were on** stays marked: achievements are off
  whenever it's played (*"Save Made With Cheats"* when it loads), and every
  later save of it keeps the mark.

---

## Saves and updates

### Where are my saves?

In `user/saves/`, one file per save: `CrashMOM GameSlot 1.sav`,
`CrashMOM GameSlot 2.sav`, ... The save's name is inside the file. Copy that
folder to back them up (a good habit during the alpha).

### I deleted a save by mistake

Deleted saves are moved, not destroyed: they're in
`user/saves/Deleted saves/`, named after the save and the time
(`CrashMOM GameSlot 4 (2026-10-02 18.05.31).sav`).

1. Move the file back into `user/saves/`.
2. Rename it to `CrashMOM GameSlot <number>.sav`, with a number no other
   save uses.

### I saved over the wrong save

The port keeps the old version in `user/saves/Backups/` (the 10 newest per
save, named with the time). Restore it the same way as a deleted save.

### My saves are in folders like `B13EBABEBABEBABE`

That's where versions before 0.1.1 kept them (the emulated Xbox's layout).
A newer version's first start copies them into `user/saves/` as `.sav` files
(listed in `copied-from-xbox-layout.txt`) and leaves the old folders as they
were, as a backup. Delete the old folders once you're happy with the new ones.

### Updating

* **Windows:** download the new release's `.zip`, unpack it, start its
  **Mind over Recomp.exe** and choose **Update an installed game**. Pick the
  game's folder (the installer finds one listed in *Installed apps* by
  itself) and your disc image; it shows the installed version and the new
  one. The game's extracted disc files (`game` folder) are checked against
  the disc image: only missing or damaged ones are copied again (all of them
  if the folder is gone). Your `user` folder and
  the earlier build stay, so only what changed is built again: usually
  minutes, not the first install's hour. (The old **Update** button in the
  installed copy's **Mind over Recomp** was removed: it didn't work reliably.)
* **Linux:** the launcher's **Setup** tab does the same.

### The update failed

* **Windows:** download the newest release and install it into a **new**
  folder, ticking **Copy saves and settings from an older version** (pick
  the old folder).
* **Linux:** unpack it into a new folder, copy your `user/` folder (and
  `game/`, to skip extracting the disc again) into it, then start the
  launcher there and run **Setup**.

### The install is very slow (hours)

A first install is usually 20-60 minutes. When it takes much longer:

* **Something heavy runs at the same time** (a game, video editing, many
  browser tabs, a big download): the build competes with it. Close what you
  can until it's done.
* **Too little free memory.** Each compile needs 1-2 GB; the installer runs
  as many at once as the free memory allows, so with little free it runs one
  at a time. Close programs before starting.
* **Sleep.** The installer keeps the PC awake while it works, but a laptop
  closed or out of power still stops it. Keep it plugged in.
* **"Use fewer resources while building"** is on: the build gives way to
  everything else and takes longer. Leave it off for the fastest install.
* Minimizing the installer is fine: it keeps working at full speed.
  Closing it stops the build; the next start carries on where it stopped.

### Shaders or game data look wrong after an update

Delete the `cache/` folder. It's rebuilt at the next start.

---

## Known issues

Already known: no need to report these (details added to the linked issue
are welcome).

* **Crash can break "heavy" objects** that only a titan should break
  ([#1](https://github.com/Maxilure/MindOver-Recomp/issues/1)).
* **Nobody has played to the end** on the port yet: later levels may have
  problems nobody has seen.
* **Above 60 fps is experimental**: physics and animations can misbehave.
  60 or 30 is the safe choice.
* **Three or four players and the co-op camera are experimental.**
* **Only NVIDIA graphics cards have been tested.** Other cards may show black
  movies or menus in the emulated picture ([fix](#black-movies-title-screen-or-menus)).
* **The native renderer may miss effects** in areas not checked yet. The
  emulated picture is the reference: a report with a screenshot of both
  (**F1** → Switch renderer) helps.
* **Windows is less tested than Linux.** Gameplay, keyboard and mouse,
  controllers, co-op, the native renderer and 120 fps have been tried. Not
  confirmed yet: the installer's update and uninstall, renaming and deleting
  saves, cutscene voices, unplugging a controller mid-game. Reports are very
  welcome.

---

## Logs and bug reports

Every game session writes its own log: `user/logs/play-<date>_<time>.log`.
Building writes `user/logs/setup-<date>_<time>.log`. On Linux the launcher's
**Game log** tab shows the game's log live (errors in red, warnings in yellow).

<details>
<summary>What's in a log</summary>

A header (the port's version, your system, CPU and RAM, how the game was
started, the settings you changed), the graphics card and driver, then the
game's events: frame rate every 5 seconds, the sounds it plays, co-op joins
and players' states, which player a menu answers to, and every menu screen
change. On Linux the launcher adds a last line saying how the session ended.

That's a few MB per hour of play. Old logs are deleted, oldest first, once
the folder passes 300 MB (setting `logs_budget_mb`). To log only the basics,
set `event_logs = false` in `user/settings.toml` (or in the Linux launcher's
Settings tab).

</details>

### Reporting a bug

First check the [known issues](#known-issues) and the
[open issues](https://github.com/Maxilure/MindOver-Recomp/issues): if your
problem is already there, add your details to it instead.

**Windows**

1. Open the
   [bug report form](https://github.com/Maxilure/MindOver-Recomp/issues/new/choose)
   and describe the problem.
2. Drag the session's log (`user\logs\play-<date>_<time>.log`, plus a
   `crash-*.txt` if there is one) into its **Log** box. For an install
   problem, the newest `setup-<date>_<time>.log` instead.
3. A save near the spot where it happens helps too (`user\saves`).

**Linux**

1. In the launcher, press **Report a problem...** (on the Play page;
   highlighted after a session that crashed or logged errors).
2. Pick the session(s) where it happened (the newest is picked already), add
   a few words if you like, and press **Make the report**. It writes one
   `.zip` to `user/reports/` (**Change...** picks another folder). It holds
   those sessions' logs, your settings and controls, a summary, and
   optionally the F10 photos you took and **a save** near the spot (a save
   holds only its name and the game's progress).
3. Press **Open the bug report form on GitHub**: the version, system,
   graphics card and settings are filled in. Describe the problem and drag
   the `.zip` into its **Log** box.

Without the launcher, attach `user/logs/play-<date>_<time>.log` by hand.

**Privacy:** nothing is sent anywhere by itself: you attach the file. The
Linux report replaces your home folder's path (which holds your account
name) with `~`, and includes a save only when you pick one. Never attach game
files (the disc image, extracted files or the built game).
