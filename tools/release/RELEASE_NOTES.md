**Mind over Recomp** is an unofficial, fan-made native PC port of the Xbox 360
game *Crash: Mind over Mutant*. It is not affiliated with or endorsed by
Activision or the game's other owners.

> ⚠️ **Alpha.** This is an early test version of an unofficial fan project.
> The game is playable, but expect bugs, crashes and rough edges. Keep a
> copy of your saves (the `user/saves` folder). Many features are
> experimental and have been tested on only a few computers.

**You need your own copy of the game:** an `.iso` dumped from your own
Xbox 360 disc of *Crash: Mind over Mutant* (USA). These downloads contain
no game files. The game is built on your computer, from your disc (on
Windows by the installer, on Linux by the launcher).


### What's new in 0.2.0-alpha

Everything below is new since 0.1.0-alpha (0.1.1 was never released).

**Windows: a real installer.** *Mind over Recomp.exe* asks for your `.iso`
and a folder, installs the build tools it needs (through winget, Windows'
own package manager), builds the game, and adds **Crash Mind over Mutant**
to the Start menu and to Windows' *Installed apps* list. The same program
updates the game later, and *Uninstall* in Windows' settings removes it,
keeping your saves, settings and logs if you tick the box. The installer
speaks English and Brazilian Portuguese. The game's icon is its own portrait
of Crash as a mask, taken from your disc during the install.

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
- Players 3 and 4 standing on player 1's shoulders after a desert cutscene
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

### Updating from 0.1.0-alpha

**Windows:** the simplest way is a fresh install: download the new `.zip`,
start **Mind over Recomp.exe** and pick a new folder; tick **Copy saves and
settings from an older version** and pick the old version's folder. (Pressing
Update in 0.1.0's launcher works too: it swaps in the installer, which then
rebuilds the game.)

**Linux:** the launcher shows **Version 0.2.0-alpha is out**: open *Setup*
and update. After the restart, let it finish rebuilding before you play.

Your saves and settings stay either way. Old saves may not carry over
perfectly (alpha): keep a backup of the old `user/saves` folder until
you've checked them.

### Downloads

| System | File |
|---|---|
| Windows 10 / 11 (64-bit) | `MindOverRecomp-<version>-windows-x86_64.zip` |
| Linux (x86-64, glibc 2.39+: Ubuntu 24.04, Fedora 40, Arch… or newer) | `MindOverRecomp-<version>-linux-x86_64.tar.gz` |

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

**Linux**
1. Unpack the archive somewhere with **~25 GB free**.
2. Open **READ ME FIRST.txt** in that folder, then start **Mind over Recomp**.
3. The launcher opens on **Setup**. Install any tools it lists, pick your
   `.iso`, then press **Set up everything**. The first build takes a while.
4. Press **Play**.

### Known issues and bug reports

What's already known (experimental features, untested graphics cards...) is
listed under
[Known issues](https://github.com/Maxilure/MindOver-Recomp/blob/main/docs/06-troubleshooting.md#known-issues);
common problems and their fixes are in
[Troubleshooting](https://github.com/Maxilure/MindOver-Recomp/blob/main/docs/06-troubleshooting.md).

Found a bug? Fill in the
**[bug report form](https://github.com/Maxilure/MindOver-Recomp/issues/new/choose)**
and attach your logs: the `user\logs` folder in the game's folder. If
the Windows installer fails, it opens that form and the logs folder for you.
On Linux, **Report a problem...** in the launcher makes the file to attach.
