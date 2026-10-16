**Mind over Recomp** is an unofficial, fan-made native PC port of the Xbox 360
game *Crash: Mind over Mutant*. It is not affiliated with or endorsed by
Activision or the game's other owners.

> ⚠️ **Alpha.** This is an early test version of an unofficial fan project.
> The game is playable, but expect bugs, crashes and rough edges. Keep a
> copy of your saves (the `user/saves` folder). Many features are
> experimental and have been tested on only a few computers.

**You need your own copy of the game:** an `.iso` dumped from your own
Xbox 360 disc of *Crash: Mind over Mutant* (USA). These downloads contain
no game files. The launcher builds the game on your computer, from your disc.

### What's new in 0.1.1-alpha

**An Options screen** in the game's own look (Pause → Options, and new in
the main menu): tabs (LB / RB), every change takes effect at once. A card
beside the list explains the selected setting (a second glass panel, with
a picture where it helps), showing the keys and controller buttons it talks
about as pictures. Long tabs slide one row at a time (the mouse wheel too),
with the row's place shown under the list.

- **Display**: frame-rate cap, renderer (Emulated Only, Emulated, Native,
  Native Only; X on Emulated or Native opens Dual Mode), brightness (the
  original's calibration grey scale as the guide; it replaces Calibration in
  the main menu), V-sync (no tearing, the game held to your monitor's
  refresh rate), fullscreen, and a frame-rate counter in the game's font,
  on screen everywhere, in any corner.
- **Audio** and **Controls**: the game's own volumes and Invert Axis, the
  button prompt pictures (Light or Dark keys; for controllers Automatic,
  which shows PlayStation, Switch or Xbox buttons to match the controller in
  your hands, or a set of your choice), and every keyboard / mouse key you
  play with, shown as key pictures and rebound right there (two keys per
  move, Backspace removes one, Default Keys puts them all back). The volumes and Invert Axis
  are now ONE setting for
  the whole game, no longer stored in save files: on the Xbox 360 each save
  carried its own volumes, so loading a save or starting a New Game changed
  them.
- **Graphics**: **resolution** (720p like the original, 1080p, 1440p, 4K: the
  Native renderer draws the whole game with that many pixels, menus and HUD
  included), **anti-aliasing** (Off, 2x like the original, 4x, 8x),
  **texture sharpness** (crisper textures in the distance) and **motion blur** (new, the port's own: Off, Low, Medium, High) for the
  Native renderer: motion blur smears the scenery when the camera moves
  while the players stay sharp; the original game had none, so it starts
  off. Also Native only: **sharpening** (a contrast-adaptive filter over
  the picture: Off, Low, Medium, High) and **shadow softness** (how wide the
  characters' shadows fade at their edge: Sharper, Original, Softer,
  Softest). **Depth of field** (the game's gentle blur of the distance): Off,
  Original or Strong, in both renderers. Settings that need the Native
  renderer are greyed out while the emulated picture is shown.
- **Camera**: the co-op camera's settings.
- **Gameplay** (changes to the game's own rules; each starts as the original,
  except that a partner's hit no longer ends your combo):
  **Friendly Fire** off (co-op partners' attacks pass harmlessly),
  **Partner Hits** (Keep Combos by default: a partner's hit no longer ends
  your hit combo; End Combos = the original), **Simple Spinning** (one button spins: LB on a controller, the Spin key
  on the keyboard; the spin tutorials show it in a second row) and **Keep Spinning: Hold to Spin** (hold LB or Spin, or keep
  circling the stick, and Crash keeps spinning: the game's Old Skool spin,
  its "spin again" decision answered directly, so no button is pressed for you).
- **Native Only** (the port's own renderer alone, the lightest) is now the
  default picture; the emulated one is a choice away.
- **Attack trails** in the Native renderer: the pale streaks some titans'
  attacks sweep behind them (the Phantom's punches, the Roller's rolling
  charge) were missing; they're drawn now, like the original.
- **Glows** in the Native renderer: big attack glows (a Stinky's punch, the
  Phantom's time shift) were a little too dim towards their edge; they now
  match the original.

**Keyboard and mouse prompts**: while you play on the keyboard and mouse,
the game's button prompts show YOUR keys and mouse buttons ("Press E to
Jack", WASD for the stick, the Spin key for spins), in menus too (Enter /
Backspace on the menu bar); pick up a controller and its buttons come back.
Rebinding a key (F6) changes the prompts at once. Key pictures: Xelu's Free
Controller & Key Prompts (https://thoseawesomeguys.com/prompts/), public domain.

**Real keyboard controls**: menus have keys of their own: Enter / Space to
choose, Esc / Backspace to go back, Q / E for the Options tabs, X / Y for the
save list's rename / delete, while Esc still pauses in play. Spin is a real
key now (no more stick circles behind the scenes): tap it for one spin, hold
it to keep spinning, and steer while you spin. Your old key settings carry
over.

**The mouse in menus**: point at a menu item to select it and click to
choose it, right click to go back, scroll with the wheel. The save list, the
button prompts ("Back", "Rename"...), the Options tabs and the game's Yes / No
questions are clickable too, and on the Audio tab a click on a star sets that
volume.

**Press any button**: the title screen starts on any controller button, any
key or a mouse click, and says so ("Press Any Button").

**Type names on the keyboard**: on the name screen (New Game, renaming a
save) just type; Backspace erases, Enter is Done, Esc goes back. The mouse
works on the on-screen keyboard, and its old erase key is now a SPACE key.
Rename (X) and Delete (Backspace) in the save list work on the keyboard
again.

**Walk works**: holding Walk (Ctrl) stopped Crash instead of slowing him
(the stick leaned less than the game notices). In menus, Backspace now
removes / deletes (a key in Options, a save in the save list) instead of
going back; Escape goes back.

**Co-op** (the co-op camera is experimental and needs more testing)

- **No more frozen partners**: a player far from the camera used to stop
  dead, unable to move, until the other player came near. Every player's
  part of the level now keeps running.
- **Turn into a mask from anywhere**: a player far from their partner can
  press B to become a mask and fly back, with 2 to 4 players. Before, B
  did nothing while the partner was off screen.
- **Walking into the distance no longer pushes the others off screen**:
  front to back, the camera stays with the player nearest to it. Setting:
  *Stay with the nearest player*.
- The camera returns to its normal distance after cutscenes, holds still
  while everyone stands still, and keeps some ground around each player in
  view (setting: *Room around players*). After a cutscene with the other
  players far away, it goes straight to everyone instead of zooming out
  to the maximum and slowly back in.
- The purple walls that close a fight now light up when players 3 or 4
  come near them, not only players 1 and 2.
- When player 1 drops out and only players 3 or 4 are left, the camera
  now follows them and they can start story scenes. Before, the camera
  stood still and the story couldn't go on.

**Saves and profiles, natively** (less pretend Xbox; see the README's
*Going native* progress)

- **Saves are plain files** now: `user/saves/CrashMOM GameSlot 1.sav`, ...
  one file per save, no Xbox profile or storage folders. The first start
  **copies** your saves there from the old folders and leaves the old ones
  untouched, as a backup.
- **Saving over a save keeps the old one** in `user/saves/Backups/` (the 10
  newest per save), and a save is written in one go: a crash while saving
  leaves the previous save whole.
- **No more profiles or sign-in** behind the scenes: who plays is simply
  which controllers play (player 1 always, players 2-4 as soon as a
  controller plays as them).
- **Invert Axis stays as you set it**: it used to reset to off at every
  Press START, quit to the title and co-op join (the Xbox profile's
  setting).
- **No more waiting on save screens**: the Xbox held its storage messages
  for a fixed time, even though saving on a PC takes a few milliseconds.
  Opening Save Game or Load Game, saving and loading a save now go straight
  on (about 4 to 7 seconds saved each time), and the "Load successful." /
  "Save successful." screens no longer wait for A. A quick read or save
  shows no message at all (no dimmed flash); a slow one shows it after a
  second and a half. The messages also
  read "Reading save files...", "Saving..." and "Loading..." instead of
  asking you not to turn off the Xbox 360. Settings: *save_screen_waits*
  and *save_success_screens* bring the original timings and screens back.

**Fixes**

- **Above 30 fps, moving floors no longer stop.** In the space station, a
  round arena's floor sinks ring by ring while enemies attack; above about
  120 fps it moved a few centimetres and stopped, leaving no way forward. The game ignored movements smaller than
  a fixed size per frame, and at a high frame rate every step was smaller.
  That minimum is now scaled to the frame time, the same as at 30 fps.

**Launcher and logs**

- **Report a problem** in the launcher: one click packs the logs of the
  sessions you pick, your settings and a short summary (version, system,
  graphics card) into a `.zip`, optionally with a save near the problem
  spot, and opens the bug report form with those details filled in.
- **Better logs**: every session's log now starts with the version, system
  and changed settings, records what the game does (frame rate, sounds,
  co-op, menus) and ends with how the session ended. Old logs are cleaned
  up automatically. The *Event logs* setting turns the extra lines off.
- **What's new** in the launcher: from this version on, it shows a
  release's notes before you update and once after.
- Each session also records your controller presses next to its log (they
  go into problem reports, so a bug's moves can be replayed).
- After an update the game now always gets rebuilt, whichever tab is open
  (0.1.0's launcher could leave the old build in place).
- **Linux: a crash now closes the game with a crash report** (like on
  Windows) instead of freezing it. The report in `user/logs/crash-*.txt`
  names the game code it happened in and goes into *Report a problem*.

### Updating from 0.1.0-alpha

The launcher shows **Version 0.1.1-alpha is out**: open *Setup* and update.
Your saves and settings stay. After the restart, let Setup finish
rebuilding the game before you play (it says so on the Play page). The
first start copies your saves into the new layout (see above); keep your
backup until you've checked they're all there.

### Downloads

| System | File |
|---|---|
| Linux (x86-64, glibc 2.39+: Ubuntu 24.04, Fedora 40, Arch… or newer) | `MindOverRecomp-<version>-linux-x86_64.tar.gz` |
| Windows 10 / 11 (64-bit) | `MindOverRecomp-<version>-windows-x86_64.zip` |

`release.toml` is for the launcher's update check. You don't need to download it.

### How to start

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

Found a bug? Press **Report a problem...** in the launcher: it makes the file
to attach and opens the
**[bug report form](https://github.com/Maxilure/MindOver-Recomp/issues/new/choose)**.
