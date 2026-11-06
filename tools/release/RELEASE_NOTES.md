**Mind over Recomp** is an unofficial, fan-made native PC port of the Xbox 360
game *Crash: Mind over Mutant*. It is not affiliated with or endorsed by
Activision or the game's other owners.

> ⚠️ **Alpha.** This is an early test version of an unofficial fan project.
> The game is playable, but expect bugs, crashes and rough edges. Keep a
> copy of your saves (the `user/saves` folder). Many features are
> experimental and have been tested on only a few computers.

**You need your own copy of the game:** an `.iso` dumped from your own
Xbox 360 disc of *Crash: Mind over Mutant* (USA). These downloads contain
no game files. The game is built on your computer, from your disc, by the
installer.

> 🐧 **Linux:** this release is **Windows only**. The Linux version comes
> later, as a separate download. Until then, Linux players stay on
> 0.1.0-alpha.

> ⚠️ **Coming from 0.1.0-alpha on Windows?** Use the new **installer**
> (*Mind over Recomp.exe* in the `.zip`), not the old launcher's **Update**
> button, and back up your old `user` folder first. Your saves, settings
> and achievements are copied over, but some things work differently now:
> see [Updating from 0.1.0-alpha](#updating-from-010-alpha) before you play.


### What's new in 0.2.1-alpha

A small fix-up release for the Windows installer. Already on 0.2.0? Open
**Mind over Recomp** from the Start menu and it offers the update.

**The install no longer stops with "the computer's clock looks wrong"
when the clock is right.** On release day, the 0.2.0 download's files
could look a few hours *newer than now* to computers in time zones behind
UTC (most of the Americas). The build then saw "files from the future" and
gave up after 100 tries, with a message blaming the clock. Now the release's
files are dated safely in the past for every time zone, and before every
build the installer also sets any file dated in the future back to the
present. So even a clock that was wrong earlier can't block the build any
more. Thanks to **[MatiGitHB](https://github.com/MatiGitHB)** for the clear
report in [#7](https://github.com/Maxilure/MindOver-Recomp/issues/7): their
"but my clock *is* synced" was what led to the real cause.

**"winget isn't there" when App Installer *is* installed: fixed.** The
installer installs its build tools with winget (part of Microsoft's App
Installer). Windows keeps winget as a special kind of shortcut in its
`WindowsApps` folder, which the first 0.2.0 download couldn't read, so it
asked for App Installer even when it was already there. It now finds winget
properly. (The 0.2.0 download was replaced with this fix on 2026-11-06;
0.2.1 includes it for everyone.)

**Clearer troubleshooting.** The
[Troubleshooting](https://github.com/Maxilure/MindOver-Recomp/blob/main/docs/06-troubleshooting.md)
page was rewritten: a "find your problem" list at the top and numbered steps
for each fix, including a step-by-step clock check for Windows.

### What's new in 0.2.0-alpha

**Updating from 0.1.0 on Windows: don't use this launcher's Update
button.** Download the new installer from the release page instead, back up
your `user` folder, and install into a new folder with **Copy saves and
settings from an older version** ticked (details under *Updating from
0.1.0-alpha* on the release page).

Everything below is new since 0.1.0-alpha (0.1.1 was never released).

**Windows: a real installer.** *Mind over Recomp.exe* asks for your `.iso`
and a folder, installs the build tools it needs (through winget, Windows'
own package manager), builds the game, and adds **Crash Mind over Mutant**
to the Start menu and to Windows' *Installed apps* list. The same program
updates the game later, and *Uninstall* in Windows' settings removes it,
keeping your saves, settings and logs if you tick the box. Pointed at a
folder with a game that was already set up, it uses that game as it is,
without building it again. The installer
speaks English and Brazilian Portuguese. The game's icon is its own portrait
of Crash as a mask, taken from your disc during the install. Installing the
build tools with winget is based on the Windows setup script contributed by
**[YWZsaXRv](https://github.com/YWZsaXRv)** in
[#4](https://github.com/Maxilure/MindOver-Recomp/pull/4).

**An Options screen** in the game's own look (Pause → Options, and in the
main menu), every change at once, with six tabs:
- **Display:** window or fullscreen, monitor, V-sync, frame-rate cap, FPS
  counter, brightness, HUD size.
- **Graphics:** render resolution, anti-aliasing, motion blur, depth of
  field, shadow softness, sharpening, texture sharpness.
- **Audio:** volumes, output device, speakers (stereo, headphones, 5.1, 7.1).
- **Controls:** change keys, button picture styles (Xbox 360, Xbox Series,
  PS5, PS4, Switch, keyboard light or dark), mouse aiming.
- **Gameplay:** friendly fire, partner hits keep combos, simple spinning,
  keep spinning.
- **Language:** text and voices, each picked from the disc's languages.

**Keyboard and mouse**
- Key pictures in every button prompt.
- The mouse works in every menu, in Crash's house screens, the minigame
  picker and the credits.
- Aiming with the mouse, with a Wii-style pointer.
- Names typed on the keyboard.
- Any button starts the title screen and skips movie cutscenes.

**Co-op**
- A **Players** screen: who plays with which controller, and each player's
  button pictures.
- Outfits for all four players, Crash in several colours, through a co-op
  outfit chest.
- Back drops a player out; an unplugged controller is handled properly,
  including another player taking over.
- Controllers keep working while the window isn't focused.
- A jack symbol over stunned titans, with the jack button only on the one
  you'd actually jack.
- Players 3 and 4 in victory scenes; their mojo flies to their own portrait.

**Everything else**
- Achievements kept by the port, with their own screen and pop-up.
- The main menu has **Extras** (Players, Credits) and **Quit Game**.
- The port's own texts in 11 languages; the whole game in Brazilian
  Portuguese.
- Sound output and audio decoding are the port's own (no emulated Xbox
  audio).
- Saves are plain files with automatic backups; no Xbox profiles.
- F1 opens the Tools menu, F2 hides the HUD, F11 switches fullscreen.
- Cheats can unlock outfits, concept art, enemy info and tutorials; a save
  made with cheats is marked.

**Fixes**
- Players 3 and 4 showing up on player 1's shoulders after a desert
  cutscene, even when nobody was playing them
  ([#2](https://github.com/Maxilure/MindOver-Recomp/issues/2)).
- Players 3 and 4 losing the camera when they're the only ones left after
  a level change.
- Fight barriers light up for players 3 and 4.
- Masks, respawns and spawn points for players 3 and 4.
- The co-op camera: a shorter reach, no jitter on bumps, follows the nearest
  player, no frozen partners far away, behaves after cutscenes and level
  changes.
- Things seen through clear ice no longer flicker.
- Motion trails in the port's own picture; some particle glows were too dim.
- Moving floors no longer stop above 30 fps (a dead end at high frame rates).
- The title screen after the intro movie loads in under a second (was 23 s).
- No more artificial Xbox waits or "do not turn off the Xbox" messages on
  the save screens.
- The speed cheat now blocks achievements like the other cheats
  ([#3](https://github.com/Maxilure/MindOver-Recomp/issues/3)).

**Thanks** to **[YWZsaXRv](https://github.com/YWZsaXRv)** for the Windows
setup script ([#4](https://github.com/Maxilure/MindOver-Recomp/pull/4)) the
installer builds on, and to **[hesyourgamer](https://github.com/hesyourgamer)**
for reporting [#2](https://github.com/Maxilure/MindOver-Recomp/issues/2) and
[#3](https://github.com/Maxilure/MindOver-Recomp/issues/3).

### Updating from 0.1.0-alpha

**First, back up.** Copy the old version's whole `user` folder (saves,
settings, achievements, logs) somewhere safe. This is an alpha: keep that
copy until you've checked your saves in 0.2.0.

**Windows: use the new installer, not the old launcher.** 0.1.0's launcher
will say a new version is out; **don't press its Update button**. The
launcher is replaced by the installer in 0.2.0, and a fresh install is the
tested way:
1. Download the new `.zip`, unpack it and start **Mind over Recomp.exe**.
2. Pick your `.iso` and a **new** folder (not the old version's).
3. Tick **Copy saves and settings from an older version** and pick the old
   version's folder (the one with the `user` folder in it).
4. Once your saves work in the new install, you can delete the old folder
   (and any shortcut you made to the old launcher). From now on, start the
   game from the Start menu, and update it with **Mind over Recomp** there.

**What happens to your old files**
- **Saves** are now plain files in `user/saves` (`CrashMOM GameSlot 1.sav`,
  ...). The first start copies your old saves into them once. The old
  Xbox-style folders next to them are left exactly as they were, as a
  backup. **0.1.0 can't see saves made in 0.2.0**: going back to 0.1.0 means
  going back to your progress from before the update.
- **Settings** you changed yourself are kept. Settings you never changed
  pick up 0.2.0's new defaults: **60 fps** (was 30), **1080p** render
  resolution, music, voices and effects at **3 of 5 stars**, and
  **Crosshair** mouse aiming. Change them back in **Options** if you like.
- **Volumes and Invert Axis** are no longer stored in each save. They're
  set once for the whole game in **Options**, so the values your old saves
  had are not used.
- **Achievements** you earned in 0.1.0 are copied once into
  `user/saves/achievements.toml`. The old record is kept but no longer
  updated.
- **Cheats:** a save written while cheats were on is now marked, and
  playing it blocks achievements.

### Downloads

| System | File |
|---|---|
| Windows 10 / 11 (64-bit) | `MindOverRecomp-<version>-windows-x86_64.zip` |

`release.toml` is for the update check. You don't need to download it.

### How to start

**Windows**
1. Unpack the `.zip` anywhere and start **Mind over Recomp.exe** (Windows
   may warn about an unknown program: **More info → Run anyway**).
2. Pick your `.iso` and where to install (**~15 GB free**, an NTFS drive,
   not a OneDrive folder), then press **Next: install**. It installs the build
   tools it needs and builds the game: the first time takes a while.
3. Start **Crash Mind over Mutant** from the Start menu. To update or
   uninstall later, open **Mind over Recomp** from the Start menu.

### Known issues and bug reports

What's already known (experimental features, untested graphics cards...) is
listed under
[Known issues](https://github.com/Maxilure/MindOver-Recomp/blob/main/docs/06-troubleshooting.md#known-issues);
common problems and their fixes are in
[Troubleshooting](https://github.com/Maxilure/MindOver-Recomp/blob/main/docs/06-troubleshooting.md).

Found a bug? Fill in the
**[bug report form](https://github.com/Maxilure/MindOver-Recomp/issues/new/choose)**
and attach your logs: the `user\logs` folder in the game's folder. If
the installer fails, it opens that form and the logs folder for you.
