<!-- Thanks for contributing! The rules are in CONTRIBUTING.md (people) and AGENTS.md (AI agents). -->

## What this changes

<!-- What and why, in a few sentences. Link the issue if there is one. -->

## How it was tested

<!-- What you ran (Linux / Windows, renderer, number of players, which part of the game) and what you did NOT test. -->

## Checklist

- [ ] No game data added (disc files, `generated/default/`, game textures / audio / shaders / text)
- [ ] What I learned about the game is written down in `docs/findings/` (new file or section + README row), or this change needed no research
- [ ] New code is commented (file headers, why + how-found on every manifest entry)
- [ ] New manifest functions were checked with `xexdis`
- [ ] The original's default behaviour is unchanged, or the change is behind a setting
- [ ] Guest memory is read through `GuestPtr` / `guest::Load32`
- [ ] Player-facing text uses `tr()` and `tools/translations.py update` was run (or no new text)
- [ ] AI assistance used: yes / no (which tool:)
