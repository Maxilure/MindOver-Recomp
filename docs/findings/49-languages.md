# 49. Languages: what the disc holds and how the game picks one

How the original chooses its language, what each language consists of, and the
port's Options -> Language tab (s.6): text and voices chosen apart, applied at
the next start, with a Restart Now button.

## 1. What is on the disc

| Piece | Where | Languages |
|---|---|---|
| Voices (8,320 clips per language) | `<language>.rcf`, one archive each, ~250 MB, ONLY `sound\rsd\<language>\...` | danish, dutch, english, finnish, french, german, italian, norway, spanish, swedish (10; `russian.rcf` is named in the code but not on this disc) |
| Menu / HUD text | text banks (Pure3D chunk 0x1800D, one 0x1800E sub-chunk per language) in packages of `default.rcf`: `frontend` (cdd70a8c), `persistent` (b4c85fe7), `bootup`, `ingame` (one package per language, English = 7a8185b0 = Fe_InGameE) | 14 letters: **A D E F G H I N R S U W X Z** |
| Movies | `movie1.rcf` / `movie2.rcf`, Bink 1, one file per movie | 14 audio tracks per movie: surround 0 / 2 / 3 + dialogue 1 English, 4 French, 5 Danish, 6 Dutch, 7 German, 8 Italian, 9 Finnish, 10 Norwegian, 11 Spanish, 12 Swedish, 13 Russian (s.5) |

Language letters (code below + sample strings decoded from each bank):

| Letter | Language | Voices |
|---|---|---|
| A | Danish | danish.rcf |
| D | Dutch | dutch.rcf |
| E | English (UK) | english.rcf |
| U | English (US) | english.rcf |
| F | French | french.rcf |
| Z | French (Canada; text identical to F in the samples) | french.rcf |
| G | German | german.rcf |
| H | Finnish | finnish.rcf |
| I | Italian | italian.rcf |
| N | Norwegian | norway.rcf |
| R | Russian | (russian.rcf, not on this disc) |
| S | Spanish | spanish.rcf |
| W | Swedish | swedish.rcf |
| X | Radical's PSEUDO-LANGUAGE: English with accents and padding, in brackets ("[Lóàdíñggg]"), a QA tool to check that longer translations fit | none |

The text banks are shared with the Wii and PS2 versions (strings mention the
Wii manual and PlayStation buttons where the console doesn't matter).

## 2. How the game picks one

- `sub_82262980` = the language LETTER: console country (`sub_82300398`) picks
  Danish / Finnish / Dutch / Norwegian / Swedish; otherwise the console
  language (`sub_8235B860`); English becomes U in the US, French becomes Z in
  Canada.
- At boot `sub_82279E30` mounts `default.rcf` plus the matching voice archive
  (`sub_82279D48` = mount one more .rcf; mounts only ADD, nothing unmounts).
- `sub_82262AD8(frontend, letter)` stores the letter at front end +0x3C, calls
  `sub_8235D488(letter)` and gives the sound system the voice folder name
  ("english", "french", ... table at 0x8203A1D8, via `sub_8232AB98` /
  `sub_8232DA28`).
- `sub_8235D488` stores the letter (global 0x824E8E1C) and calls every
  registered **language-change listener** (list 0x82571268, count 0x825712AC,
  three passes by listener category). The engine has a class for them:
  `core::ILanguageChangeCallback` (vtable 0x8200C67C). So the engine was
  built to switch languages while running.
- The original USES that: a `CFrontendManager` script method `SetLanguage`
  (`sub_82264888`, registered in `sub_822636D8` next to `IsNordic` /
  `IsRussian`) behind a `GameStart_LanguageSelect` screen, shown on Nordic
  (and Russian) consoles at boot: it mounts the chosen voice archive and calls
  `sub_82262AD8`. The Nordic disc ships all languages; the player picks one.

## 3. Before the port's setting

The SDK answers the game's question with two settings: `user_language`
(Xbox language number: 1 English, 3 German, 4 French, 5 Spanish, 6 Italian,
12 Russian) and `user_country`. Tested: `--user_language=4` boots the whole
game in French (menus, prompts, voices from french.rcf), no errors. The
Nordic languages and Dutch need the matching `user_country`. Since s.6 the
port answers the game's question itself, so these two no longer matter.

## 4. Switching while running

Not done: the port applies a change at the next start. Why:

- The text-bank loader (`sub_823B4A20`) keeps only the CURRENT letter's
  strings when a bank loads (a flag at 0x8257E6C0 skips the others), so the
  other languages aren't in memory to switch to.
- The in-game text comes in a per-language package, `Fe_InGame` + letter
  (`sub_82266648` hashes the letter into the section name), loaded with the
  level.
- Voice clips already loaded for a level stay in their language.
The Nordic select screen switches only at boot, before any of that loads.

## 5. Movies

`sub_822C42A0` (the movie start) builds each movie's Bink track list from the
front end's letter (+0x3C, register r7 from 0x822C4338): 4 tracks = front
stereo 0, the dialogue track of the language, LFE 2, back 3 (a stereo list
too, when the output isn't 5.1). It reaches Bink through PlayerBink's open
(`sub_82428E58`, vtable slot 6: count at player +0xD8, ids at +0xDC, then
`sub_824BC9A8` = BinkSetSoundTrack). Measured by logging that list: English
`0 1 2 3`, French `0 4 2 3`, German `0 7 2 3`. The movies have Russian
dialogue (track 13) although the disc has no Russian in-game voices.

## 6. The port: Options -> Language

Two settings, `text_language` (en-us = the default, en-gb, fr, fr-ca, de, it,
es, nl, da, fi, no, sv, ru) and `voice_language` (auto = "Same as Text",
english, french, german, italian, spanish, dutch, danish, finnish, norwegian,
swedish, russian), in `user/settings.toml` and on an Options tab whose rows
read **Text** and **Voices** (longer labels and values ran into each other).
English (US) is the default text, not the system's language. "Same as Text"
voices = the text language's own, else English (Russian: no voices on the
disc). Only voices whose archive is in the game folder are offered. X is left
out (a test language).

`src/language/language.cpp`:
1. `sub_82262980` replaced: the letter = `text_language`, decided at the
   first call and kept for the session.
2. `sub_82279D48` (mount one .rcf) wrapped: a voice archive's name becomes
   the `voice_language` one.
3. Mid-function hook at 0x82262C04 (`sub_82262AD8`, where every branch of
   its switch meets with r4 = the voice folder name): the `voice_language`
   folder.
4. Mid-function hook at 0x822C433C: the movie start sees the voice
   language's letter, so movie dialogue follows the voices.
Before any of it, the game's own strings at the expected addresses are
compared with the expected names; a mismatch leaves the voices to the game.

A changed row shows "Takes effect when the game starts again" and an X
prompt, **Restart Now** (`src/app_restart.cpp`): settings saved, then on
Linux the process replaces itself (`execv` of /proc/self/exe, same command
line without `--load_save`, every file but stdin/out/err closed: same
process id, so the launcher keeps following it and its log); on Windows a
new process starts and this one exits. In game the game's own Yes / No box
asks first (progress since the last save is lost); in the main menu it
restarts at once.

Tested (test copy, console): German text + English voices (pause menu in
German, english.rcf mounted instead of german.rcf); English text + German
voices (Aku Aku and tutorial clips from german.rcf); movies `0 1 2 3` with
German text + English voices, `0 4 2 3` with English text + French voices;
Restart Now in game (question, Yes = new session, same process id) and in
the main menu (saved `voice_language`, new session Italian text + English
voices). The Italian
title logo is localized too (Il Dominio Sui Mutanti). 0 errors. Not tested:
Windows.

## 7. The port's own text

The texts the port adds (Options rows and descriptions, the Players screen,
the outfit chest, prompts, question boxes, "Press Any Button") are written in
English in the code as `tr(u"...")` and translated through gettext `.po`
files in `languages/` (src/language/texts.h, languages/README.md):

- **Which file**: the running text language (`language::RunningText()`, fixed
  for the session like the game's own letter). English (US/UK) = the code's
  texts; French (Canada) reads `fr.po` under `fr_CA.po`; Norwegian = `nb.po`.
  A missing entry falls back to English. `<game folder>/user/languages/<code>.po`
  is read instead of the shipped file (translators' tests).
- **Brazilian Portuguese** (`pt-br`) is a text choice of its own: the game
  runs with letter U (English US), only the port's texts are Portuguese. The
  disc has no Portuguese bank.
- **Glossary**: `tools/text_banks.py` pairs every string of the game's four
  banks (bootup, frontend, ingame, persistent; 1,352 strings) with its English
  by KEY (the same in every language of a bank), so translations use the
  game's own terms (titan and outfit names, "jack", "drop out", the informal
  "you" of its tutorials). The disc's own lines are used where the port says
  the same thing ("Are you sure you want to quit the game?").
- **The font**: Titans Small/Large draw ASCII, Latin-1 except × ÷, a few
  Central European letters, Cyrillic А-я (no Ё/ё) and ’ † …. U+00A5-00BE are
  its BUTTON PICTURES (± = A, µ = Back, « » = arrows and buttons, not quotes),
  except © ® °. `tools/translations.py check` refuses anything else.
- Machine drafts carry gettext's `fuzzy` flag until a speaker checks them; the
  Language tab says so for the running language.
- Not translated: the developer tools (F1 Tools, cheats, the F6 controls
  window), the logs (kept English for bug reports), the FPS overlay's labels,
  the launcher (later).
- The title's "Press START" line is replaced in every language (by its element,
  not its words). The save-wait messages (findings/33: "do not turn off the
  Xbox 360 console" -> "Saving...") are still matched and reworded in the
  ENGLISH bank only: other languages show the original console wording.
