<!--
  RELEASE NOTES: this file is the text of the GitHub release page (the
  release workflow passes it to `gh release create --notes-file`).

  Layout, one release at a time:
  - "What's new in <version>": ONLY this version's changes. Older versions'
    changes are not repeated here: they stay on their own release pages,
    linked from "Changelog" at the bottom (add the previous version there).
    The Changelog lists only releases of the SAME line (0.2.x lists 0.2.0,
    0.2.1, ...); a new line (0.3.0) starts with the previous line's
    first release (0.2.0) only.
  - The heading must stay exactly "### What's new in <version>" with no
    other heading inside it: the launcher/installer shows that section as
    "What's new" and stops at the next line starting with "#"
    (launcher/update.cpp WhatsNew).
  - HTML comments like this one are hidden on GitHub and outside that section.
-->

**Mind over Recomp** is an unofficial, fan-made native PC port of the Xbox 360
game *Crash: Mind over Mutant*. It is not affiliated with or endorsed by
Activision or the game's other owners.

> ⚠️ **Alpha:** playable, but expect bugs, crashes and rough edges. Keep a
> copy of your `user\saves` folder.
>
> **You need your own disc.** The installer builds the game on your PC from
> an `.iso` of your own Xbox 360 disc of *Crash: Mind over Mutant* (USA).
> No game files are included.
>
> **Windows only** for now. Linux comes later; until then, Linux players
> stay on [0.1.0-alpha](https://github.com/Maxilure/MindOver-Recomp/releases/tag/v0.1.0-alpha).

### What's new in 0.2.2-alpha.4

One installer fix on top of 0.2.2-alpha.3.

- **The SDK's libraries download without Git's shell.** On some PCs the
  step **"Finishing the SDK's libraries"** (or the SDK download before it)
  stopped with **"fork: Resource temporarily unavailable"** and **exit
  code 254**, even with no antivirus installed. The tool Git uses for
  submodules is a script that runs in Git for Windows' own shell, and on
  those PCs Windows stopped every program that shell tried to start. The
  installer now fetches each of the SDK's libraries with plain Git
  commands that don't need that shell. A download that was stopped goes on
  where it stopped. Thanks to **[lionessbzzz](https://github.com/lionessbzzz)**
  for the logs in [#14](https://github.com/Maxilure/MindOver-Recomp/issues/14).
- **If another Git step ever meets that error,** the installer's message
  says what usually causes it and what to try.

### Install or update

- **New install:** unpack the `.zip` anywhere and start
  **Mind over Recomp.exe** (if Windows warns about an unknown program:
  **More info → Run anyway**). Pick your `.iso` and a folder (**~15 GB
  free**, an NTFS drive, not OneDrive), then **Next: install**. The first
  build takes a while. Then start **Crash Mind over Mutant** from the Start
  menu.
- **Already on 0.2.0, 0.2.1 or 0.2.2:** open **Mind over Recomp** from the Start
  menu. It offers the update.
- **Your install stopped at "Couldn't find cmake", "file INSTALL cannot
  find", "Python was not found" or "fork: Resource temporarily
  unavailable"?** Start this version's
  **Mind over Recomp.exe** and install into the same folder: it carries on
  where it stopped.
- **Coming from 0.1.0:** don't use the old launcher's **Update** button.
  Follow [Updating from 0.1.0-alpha](https://github.com/Maxilure/MindOver-Recomp/releases/tag/v0.2.0-alpha#updating-from-010-alpha)
  on the 0.2.0 page.

**Download:** `MindOverRecomp-0.2.2-alpha.4-windows-x86_64.zip` (Windows 10 / 11,
64-bit). `release.toml` is only for the update check; you don't need it.

### Help and bug reports

- Already known: [Known issues](https://github.com/Maxilure/MindOver-Recomp/blob/main/docs/06-troubleshooting.md#known-issues).
- Common problems and fixes: [Troubleshooting](https://github.com/Maxilure/MindOver-Recomp/blob/main/docs/06-troubleshooting.md).
- Found a bug? Fill in the
  **[bug report form](https://github.com/Maxilure/MindOver-Recomp/issues/new/choose)**
  and attach the `user\logs` folder from the game's folder. If the
  installer fails, it opens the form and that folder for you.

### Changelog

- [0.2.2-alpha.3](https://github.com/Maxilure/MindOver-Recomp/releases/tag/v0.2.2-alpha.3):
  the SDK step no longer picks Windows' Microsoft Store shortcut for Python.
- [0.2.2-alpha.2](https://github.com/Maxilure/MindOver-Recomp/releases/tag/v0.2.2-alpha.2):
  the SDK step without the Tracy DLL an antivirus removed, controller aiming
  with Mouse Aiming on, the installer shows its version.
- [0.2.2-alpha](https://github.com/Maxilure/MindOver-Recomp/releases/tag/v0.2.2-alpha):
  installer fixes (Visual Studio's CMake, minimized window), dig spots with
  3-4 players, mouse cursor, faster game on the CPU.
- [0.2.1-alpha](https://github.com/Maxilure/MindOver-Recomp/releases/tag/v0.2.1-alpha):
  installer fixes (clock check, winget detection), clearer Troubleshooting page.
- [0.2.0-alpha](https://github.com/Maxilure/MindOver-Recomp/releases/tag/v0.2.0-alpha):
  Windows installer, Options screen, keyboard and mouse everywhere, Players
  screen and outfits for co-op, achievements, many co-op fixes.

