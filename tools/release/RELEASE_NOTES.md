<!--
  Release notes for the GitHub release page (release.yml) AND the installer's
  "What's new" panel (launcher/update.cpp). The panel shows only the
  "### What's new in <version>" section, as plain text, and stops at the next
  line starting with "#". So keep that heading as it is, use **bold** labels
  (not #### headings) inside it, and write each bullet on a single line.
  tools/release/notes/ keeps the text of each published release page.
-->

> [!WARNING]
> **Alpha.** Playable, but expect bugs and crashes. Keep a copy of your `user/saves` folder.

> [!IMPORTANT]
> **You need your own game.** An `.iso` dumped from your own Xbox 360 disc of *Crash: Mind over Mutant* (USA). These downloads contain no game files: the installer builds the game on your computer, from your disc.

> [!NOTE]
> **Windows only.** Linux comes later as a separate download; until then, Linux players stay on [0.1.0-alpha](https://github.com/Maxilure/MindOver-Recomp/releases/tag/v0.1.0-alpha).

### What's new in 0.2.1-alpha

A small bug-fix release for the Windows installer.

**Fixed**
- Fixed the install stopping with "the computer's clock looks wrong" when the clock was right, in time zones behind UTC ([#7](https://github.com/Maxilure/MindOver-Recomp/issues/7)).
- Fixed the installer asking for App Installer when winget was already installed.

**Changed**
- Rewrote the [Troubleshooting](https://github.com/Maxilure/MindOver-Recomp/blob/main/docs/06-troubleshooting.md) page: a "find your problem" list and numbered steps for each fix.

**Thanks** to [MatiGitHB](https://github.com/MatiGitHB) for reporting [#7](https://github.com/Maxilure/MindOver-Recomp/issues/7).

New since 0.1.0? See everything that changed in [0.2.0-alpha](https://github.com/Maxilure/MindOver-Recomp/releases/tag/v0.2.0-alpha).

**Full Changelog:** [v0.2.0-alpha...v0.2.1-alpha](https://github.com/Maxilure/MindOver-Recomp/compare/v0.2.0-alpha...v0.2.1-alpha)

### How to install

1. Download the `.zip` below, unpack it and start **Mind over Recomp.exe** (if Windows warns: **More info → Run anyway**).
2. Pick your `.iso` and an install folder (**~15 GB free**, NTFS drive, not OneDrive), then **Next: install**. The first build takes a while.
3. Start **Crash Mind over Mutant** from the Start menu.

### How to update

- **From 0.2.0:** open **Mind over Recomp** from the Start menu and accept the update.
- **From 0.1.0:** don't use the old launcher's **Update** button. Back up your `user` folder, then do a fresh install with **Copy saves and settings from an older version** ticked. [Full steps](https://github.com/Maxilure/MindOver-Recomp/releases/tag/v0.2.0-alpha#updating-from-010-alpha).

### Help and bug reports

- [Known issues](https://github.com/Maxilure/MindOver-Recomp/blob/main/docs/06-troubleshooting.md#known-issues) · [Troubleshooting](https://github.com/Maxilure/MindOver-Recomp/blob/main/docs/06-troubleshooting.md)
- Found a bug? Use the [bug report form](https://github.com/Maxilure/MindOver-Recomp/issues/new/choose) and attach the `user\logs` folder.

<sub>`release.toml` is for the update check; you don't need it. Mind over Recomp is an unofficial fan project, not affiliated with or endorsed by Activision.</sub>
