# Contributing to Mind over Recomp

Thanks for helping. Code, reverse engineering, testing, bug reports,
corrections, translations and art are all welcome, and every contributor is
credited in [CREDITS.md](CREDITS.md).

- **Bugs and ideas:** open an issue with the forms on the
  [New issue](https://github.com/Maxilure/MindOver-Recomp/issues/new/choose)
  page. Check [Troubleshooting](docs/06-troubleshooting.md) first.
- **Translations:** see [languages/](languages/README.md). No programming
  needed.
- **Code:** read on.

## Using an AI coding agent?

Most of this project is written with AI assistance, so you're in good
company. Your agent should follow **[AGENTS.md](AGENTS.md)**: most agents read
it from the repository root by themselves; otherwise paste it into the
agent's instructions. It has the full rules. The short version is below.

## The rules, short

1. **No game data in the repository.** No disc images, `.xex`, files from
   `game/`, recompiled code from `generated/default/`, game textures, audio,
   shaders or text. Describe game content, don't paste it. Everyone builds
   from their own copy of the game.
2. **Write down what you learn.** If your change needed you to figure out how
   part of the game works, add a finding in
   [docs/findings/](docs/findings/) (a new numbered file, or a section in an
   existing one) and a row in its README table. Wrong turns belong in it too.
3. **Comment heavily**, and explain *why*. Every new file gets a header
   comment; every `crash_mom_manifest.toml` entry gets a "why + how we found
   it" comment.
4. **Check the disassembly** (`xexdis`) before adding a function to the
   manifest.
5. **Keep the original as it was by default.** New behaviour goes behind a
   setting, or only affects things the original didn't have.
6. **Player-facing menus use the game's own look** and translatable text;
   ImGui is for developer tools only (English only).
7. **Never risk players' saves.** Copy, don't move or delete.
8. **Guest memory goes through `GuestPtr`** (`src/guest_memory.h`), never
   `base + address`: it breaks on Windows otherwise.
9. **Neutral voice** in docs, comments and commits: facts, no personal
   details. Unknown official names: write **(name TBD)**.

## Pull requests

- One topic per pull request.
- Say what changed, why, **how you tested it and what you didn't test**
  (Linux? Windows? one player or four?), and link the finding.
- Screenshots for visible changes: compressed, labelled, small (see
  [AGENTS.md](AGENTS.md) section 2).
- If an AI agent helped, say so. You're responsible for having run the change
  on the real game.
- Contributions are licensed GPL-3.0 like the project; SDK patches in
  `patches/rexglue-sdk/` stay BSD-3-Clause. Add yourself to
  [CREDITS.md](CREDITS.md).

Building: [docs/01-building.md](docs/01-building.md). How it all fits
together: [docs/02-how-the-port-works.md](docs/02-how-the-port-works.md).
