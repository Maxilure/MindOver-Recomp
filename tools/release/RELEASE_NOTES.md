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
rebuilding the game before you play (it says so on the Play page).

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
