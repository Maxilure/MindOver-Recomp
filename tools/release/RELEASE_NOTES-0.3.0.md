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

  THIS IS THE DRAFT OF THE 0.3.0-alpha PAGE (tools/release/RELEASE_NOTES-0.3.0.md).
  tools/release/RELEASE_NOTES.md holds 0.2.2-alpha until that release is out;
  then this text replaces it (rename it, bump VERSION.txt) and this
  paragraph goes. No Changelog section: 0.3.0 is the first of its line.
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
- **Achievements no longer cut off the game's messages.** An achievement
  earned while a message was on screen made that message vanish at once
  and the achievement pop up without its slide. Now it waits until the
  message has slid away, then slides in on its own.

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
