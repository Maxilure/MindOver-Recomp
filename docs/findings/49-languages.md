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
  except © ® °. `tools/translations.py check` refuses anything else. Some
  characters are listed but EMPTY on the disc (zero width, no picture):
  `" * < > ^ _ \` { | } ~` and Ã ã; the checker refuses the first group (the
  game's own Russian writes quotes as two apostrophes, ''like this''), Ã ã are
  drawn by the port (s.8.3).
- Machine drafts carry gettext's `fuzzy` flag until a speaker checks them; the
  Language tab says so for the running language.
- **The developer tools and the launcher** use the same files (2026-10-31): the
  ImGui windows (F1 Tools, F5 Cheats, F6 Controls, the renderer banners) through
  `trs("...")` = UTF-8 in and out, the frame-rate counter's words through `trc`
  (game font, context "fps counter"). The launcher has no game code: its own
  reader (launcher/texts.h: `trs`, `trsc` with a context, `trf`) loads
  `<code>.po` from user/languages, program/languages or source/languages, by the
  text_language in user/settings.toml. `tools/translations.py` extracts `src/`
  AND `launcher/`; its check also compares printf placeholders (%s %d %.1f, in
  ORDER: C can't reorder them) and the spaces at both ends (some labels are
  indented with spaces). The SDK's ImGui font is a 10 px pixel font with Latin-1
  only: `ui/imgui_fonts.cpp` merges a system font (Noto Sans / DejaVu Sans /
  Segoe UI) for Latin Extended-A, Greek and Cyrillic through ReXApp's
  OnConfigureFonts. Long translations: the launcher's Settings label column
  takes the widest label it has drawn (settings_page.cpp), the Renderer column
  the widest choice.
- Not translated: the logs (kept English for bug reports, like a report's
  summary.txt), the "All settings" list's descriptions in the launcher (the
  raw setting documentation, many from the SDK), the SDK's own windows (debug
  overlay, log console, F4 flag list), key names (Left Shift, LMB), and a
  setup job's step-by-step output (it is the job's log).
- The title's "Press START" line is replaced in every language (by its element,
  not its words).

### 7.1 The save messages, every language

The save-wait messages (findings/33: "do not turn off the Xbox 360 console")
are matched by their KEYS in the text banks, the same in every language:
33495 (reading the save list), 12194 (loading), 30106 and 35614 (saving;
35614 = "Saving content" or "Saving Crash: Mind Over Mutant game", by package).
Only the running language's copy (its letter: U for English US and Brazilian
Portuguese) is written over, with the port's own text in that language
(`tr("Reading save files...")`, "Loading...", "Saving..."), in place, followed
by 0 characters to the old end. The strings are UTF-16 LITTLE-endian (the first
English-only version searched big-endian and matched only because plain ASCII
lines up one byte off). Reader: `language/game_text_banks.h` (Find,
BankLanguage::KeyAt / SpanOf / StringAt).

### 7.2 Achievements in the player's language

The title data (XDBF in default.xex) has the 48 achievements' texts in English,
German, French, Spanish and Italian only. The SDK read them in English (its
user_language flag, which the port never sets). `achievements.cpp` now takes
the running text letter: G / F (and Z, French Canada) / S / I = the title
data's own words (the 360 wording); U / E = English. For the disc's other text
languages (Dutch, Danish, Finnish, Norwegian, Swedish, Russian) the game's
IN-GAME TEXT BANK holds the same achievements, as the list the other consoles
show in their pause menus: a numbered title ("08. Can U Dig It?", keys
8796-9181), an earned line ("You learned how to dig", 5590-5975) and a to-do
line ("Dig to get past the thorns in Wumpa Island", 64575-64960), in every disc
language. package\c1e387c7.p3d holds that bank in all of them (read once,
`game_text_banks::InGame(key, letter)`). The pairing id -> keys is a table in
achievements.cpp (BankKeysById), made by matching titles by name and the lines
by meaning; the "NN. " number is cut off. Notes:

- "Happy To See Me" (32) has no title of that name there: it is "Mutant
  Recycle" (key 8868), the same "stash a mutant" goal in the same place of the
  list.
- The five arena minigames have no earned line: their to-do line stands for both.
- That list's wording is a little plainer than the 360's ("Crash defeated 5
  Ratcicles" vs "5 Ratcicles put on ice"); Can U Dig It's to-do line names the
  Wumpa Island thorns.
- No achievement text ships with the port for the disc's languages: every word
  comes from the player's disc. Brazilian Portuguese takes the same list from
  the port's translation of the game's text (s.8).
- Log line: "Achievements: texts in language 'D' for 48 of 48 (the game's
  in-game list)".


## 8. Brazilian Portuguese: the game's own text translated

The disc has no Portuguese. For Brazilian Portuguese the port carries a
translation of the GAME's text too (menus, HUD, tutorials, missions, hints,
achievements, the platform messages), not only its own. Voices stay English
(none on the disc). The Xbox version has no cutscene subtitles: the only
"subtitle" in the data is the title logo's picture `Crash_Subtitle_ENG.tga`.

### 8.1 What there is

Four text banks, letter U (English US): bootup 26 strings (package 7efdcd91),
persistent 78 (b4c85fe7), frontend 504 (cdd70a8c, also inside the in-game
packages), ingame 744 (7a88cba0, c1e387c7; 97597d1b holds a Wii / PS2 copy:
"Nunchuk", all capitals, 355 keys worded differently). 1,352 strings; 194 stay
as they are (the name screen's keyboard letters, digits, "*", the language
names, "(null)"), 1,158 translated (about 5,700 English words). Every bank
copy of every package the game loads goes through the menus category (6) of
the data patcher.

### 8.2 How it is swapped in

- `languages/game/pt_BR.po`: msgctxt = the bank, msgid = the string's key,
  msgstr = the text; no English game text in the repo. `tools/game_texts.py`
  makes an editable copy with the English beside each line from the player's
  own disc (`edit`, into user/languages/game/), packs it back (`pack`), checks
  it (`check`: the button pictures U+00A5-00BE, the `†` non-breaking space and
  the line count must match the English) and counts it (`status`).
- `texts::GameTranslation()` reads it (or `<game folder>/user/languages/game/
  <code>.po` first) for the running text language; any language could have
  one (a fan's fixes to a disc language too).
- `game_text_banks::Install` = a data patch on every menu package: each bank's
  copy in the running letter (U for Brazilian Portuguese) is rebuilt with the
  translated strings: same keys (sorted; the 65599 "modulus" is the key
  hash's), new offsets, a new string area (UTF-16 LE, each ended by 0,
  padded to 4 bytes), the header's size = the area's bytes; the chunk sizes
  follow (pure3d Node). It runs after the save-wording patch (s.7.1).
- Achievements: with a game translation loaded, the in-game list (s.7.2) is
  read from it instead of the title data's English.

### 8.3 The font's Ã ã

Titans_Large / Titans_Small list Ã and ã but with EMPTY records (zero width,
no picture): "não" drew "n o". `ui/font_letters.cpp` (a data patch of
b4c85fe7, after the menu button glyphs) composes each from the font's own
pictures: the "a" / "A" cell plus the tilde rows of "õ" / "Õ" (the rows above
a plain "o"'s first inked row), centred, the cell widened to the wider of the
two (left bearing moved by half). It goes into free space of a text page
(Small: the empty bottom strip of page 6; Large: ã fits on page 8), else into
ONE new page, a cleared copy of a text page (Large's Ã: no 57 x 89 spot left;
62 -> 63 of the 64 pages a font may have). Colours are mapped to the
destination page's palette (the pages' palettes differ). prompt_icons checks a
loaded font's page count as disc + its own pages + `font_letters::
AddedPages()`. Page format: 0x19000 > 0x19001 > 0x19002 = u32 size + TGA
(colour-mapped, 256 x BGRA palette, 8 bit, top-left origin); glyph v runs
bottom-up (pixel row = (1 - v) x 256). The frame-rate counter draws with the
disc's font (ui/game_font.cpp) and so avoids Ã (its Portuguese "TROCA").

### 8.4 Checked

Main menu, Load Game list (difficulty names Leve / Complicado / Maluco!),
pause menu ("J1 em pausa"), Missions (story + optional pages), tutorial pages
with button pictures, the Achievements list: all Portuguese, Ã ã drawn. The
translation is a machine draft (fuzzy); names of titans and enemies stay as the
English (no official Brazilian release), places are translated (Ilha Wumpa,
Reino Ratcicle, Monte Grimly, Terra Devastada, Ferro-Velho, Escola do Mal).