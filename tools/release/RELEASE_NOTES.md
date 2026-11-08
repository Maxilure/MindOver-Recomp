<!--
  RELEASE NOTES: this file is the text of the GitHub release page (the
  release workflow passes it to `gh release create --notes-file`).

  Layout, one release at a time:
  - "What's new in <version>": ONLY this version's changes. Older versions'
    changes are not repeated here: they stay on their own release pages,
    linked from "Changelog" at the bottom (add the previous version there).
    The Changelog lists only releases of the SAME line (0.2.x lists 0.2.0,
    0.2.1, ...): older lines aren't mentioned.
  - The heading must stay exactly "### What's new in <version>" with no
    other heading inside it: the launcher/installer shows that section as
    "What's new" and stops at the next line starting with "#"
    (launcher/update.cpp WhatsNew).
  - HTML comments like this one are hidden on GitHub and outside that section.
  - tools/release/notes/v<version>.md = the text of each PUBLISHED release
    page, as it is on GitHub (copy it there after publishing or editing one;
    `gh release edit v<version> --notes-file tools/release/notes/v<version>.md`).

  - Inside "What's new" the changes are grouped under BOLD LABELS
    (**New**, **Fixes**, **Changes**), not "####" headings: a heading line
    would end the section the installer shows.

  0.3.0-alpha takes in what was planned as 0.2.2-alpha (never released).
  No Changelog section: 0.3.0 is the first of its line. Bump VERSION.txt
  to 0.3.0-alpha when releasing.
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

### What's new in 0.3.0-alpha

**New**

- **Widescreen at any shape.** With **Renderer: Native Only**, the picture
  can be wider than 16:9, up to 32:9: wider screens show more of the world
  at the sides, and nothing is stretched. **Options → Display → Aspect
  Ratio**: Match Window, 16:9 (original), 21:9 or 32:9. **Widescreen HUD**
  keeps the HUD where the game has it, or moves its corners to the screen's
  edges.
- **Resolutions in pixels.** **Options → Graphics → Resolution** lists real
  sizes at the shape in use (for example 2560 x 1080 at 21:9), plus Match
  Window.
- **Voodoo dolls get their own card.** The game's messages at the bottom of
  the screen (voodoo dolls, missions, concept art unlocked) now use the
  same card as the achievements, and a voodoo doll you pick up turns on the
  card in 3D, standing out over its top edge. Each of the seven doll kinds
  shows its own doll. The counter ("1/13") sits on the card's right, and
  long messages scroll. **Options → Display → HUD → Messages: Original**
  brings back the game's wide bar.
- **Cursor Fades.** **Options → Controls → Cursor Fades**: the glowing
  mouse cursor fades out after 1, 3 (default) or 5 seconds without a mouse
  move, or never.
- **Installer: "Use fewer resources while building".** It uses half the
  processor at low priority so the PC stays usable, but the build is
  slower. Unchecked, the build uses all cores but one, and no more at once
  than the free memory allows.
- **Installer: time shown.** The window shows the installer's version, how
  long the install has been running and, at the end, how long it took. If
  it runs longer than usual, a note suggests closing heavy programs
  meanwhile.

**Fixes**

- **The keyboard and mouse always belong to player 1.** A setting from an
  older version could put the keyboard on player 2 (or 3, 4), leaving
  player 1 with only a controller. That setting is now ignored, and
  every keyboard and mouse plugged in controls player 1.
- **Volumes are right on the title screen.** Your Music, Dialogue and
  Sound Effects volumes now apply before the title shows. Before, the
  title played at full volume until the first button press.
- **The mouse cursor no longer sits in every menu for controller
  players.** It shows only once the mouse moves and hides as soon as a
  controller is used.
- **Dig spots with 3-4 players.** When one Crash dug in or out, players 3
  and 4 could come up stuck in the digging pose; now everyone else turns
  into a mask, as player 2 always did. Three or more Crashes underground at
  once no longer crash the game.
- **Achievements no longer cut off the game's messages.** An achievement
  earned while a message was on screen made that message vanish at once
  and the achievement pop up without its slide. Now it waits until the
  message has slid away, then slides in on its own.
- **Text colours fixed in busy menus.** Some menu texts could show another
  text's colour, e.g. player 1's "P1" in the outfit chest turning blue with
  four players.
- **Installer: minimizing the window no longer pauses the install or makes
  it stop early.** Windows no longer slows the build down while the window
  is in the background, and the PC doesn't go to sleep during it.
- **Installer: steps no longer run twice** (for example, extracting the
  disc again) right after a step finished.

**Changes**

- **A faster, simpler installer.** A fresh install now takes about 20
  minutes on a mid-range PC, and an update a few minutes.
  - **Two choices at the start: Install the game or Update an installed
    game.** To update, download the new release, start its
    **Mind over Recomp.exe**, pick the installed game's folder and your
    `.iso`. It shows the installed version and the new one. Your saves,
    settings and the earlier build are kept, and only what changed is built
    again. Picking the same version repairs the install.
  - **The update checks the game's disc files against your `.iso`** and
    copies again only the missing or damaged ones.
  - **The SDK is built once instead of twice.**
  - **The Update button in the installed Mind over Recomp is gone:** it
    didn't work reliably. Each release's installer does the update instead.
  - **The installer downloads nothing itself.** It uses the files from its
    own `.zip`: unpack the whole `.zip` before starting it, or it says so.
- **Button styles named after their controllers.** In **Options → Controls
  → Controller Icons** and on the **Players** screen: **DualSense**
  (was PlayStation 5), **DualShock** (was PlayStation 4) and **Joy-Con**
  (was Switch). Xbox 360 and Xbox Series keep their names. Your choice is
  kept.

### Install or update

- **New install:** unpack the `.zip` anywhere and start
  **Mind over Recomp.exe** (if Windows warns about an unknown program:
  **More info → Run anyway**). Choose **Install the game**, pick your
  `.iso` and a folder (**~15 GB free**, an NTFS drive, not OneDrive), then
  **Next: install**. The first build takes a while. Then start
  **Crash Mind over Mutant** from the Start menu.
- **Already installed (0.1.0 or later):** unpack the new `.zip`, start its
  **Mind over Recomp.exe** and choose **Update an installed game**. Pick
  the game's folder and your `.iso`. Close the game first. Don't use the
  **Update** button of an older version's launcher.

**Download:** `MindOverRecomp-0.3.0-alpha-windows-x86_64.zip` (Windows 10 / 11,
64-bit). `release.toml` only tells older versions that an update exists;
you don't need it.

### Help and bug reports

- Already known: [Known issues](https://github.com/Maxilure/MindOver-Recomp/blob/main/docs/06-troubleshooting.md#known-issues).
- Common problems and fixes: [Troubleshooting](https://github.com/Maxilure/MindOver-Recomp/blob/main/docs/06-troubleshooting.md).
- Found a bug? Fill in the
  **[bug report form](https://github.com/Maxilure/MindOver-Recomp/issues/new/choose)**
  and attach the `user\logs` folder from the game's folder. If the
  installer fails, it opens the form and that folder for you.
