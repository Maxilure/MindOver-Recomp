# 63. Subtitles (research): what is spoken, what triggers it, and how a subtitle could follow it

**Status: research only, nothing changed in the game.** Sections 1-12
were written from the repository's own code and earlier findings; s.13 adds
a census of a player's own disc (the English voice archive) and of session
logs made with the audio trace on, using the new `tools/subtitles.py`. No
game code was changed or run for this finding. Every claim that still needs
a run is marked **(to check)**. The plan (s.9-s.12) waits for the
maintainer's approval before any game code is written.

**The question.** Can the port show subtitles for every spoken line: the
cutscenes (both kinds), the characters Crash talks to, Aku Aku's hints,
warnings and tutorial lines, and the rest of the voiced dialogue, in English
and Brazilian Portuguese, without touching the original voices and following
the real timing of each line rather than fixed timers?

## 1. The short answer

* **The disc has no dialogue text.** The Xbox 360 version has no subtitles at
  all; the only "subtitle" in the data is the title logo's picture
  `Crash_Subtitle_ENG.tga` ([findings/49](49-languages.md) s.8). The four text
  banks (1,352 strings) are menus, HUD, missions, hints and tutorials.
  **Every spoken line therefore needs a transcription.** Nothing can be
  "extracted" as text.
* **The voices are well indexed.** Each clip has a 64-bit sound ID and, in
  its `.rsd` header, the sound designer's original file name, whose folders
  name the character and, for cutscenes, the level / scene
  ([findings/19](19-missing-dialogue-5-1-xma.md) s.1). The port already maps
  ID -> name at run time (`src/audio_trace.cpp`).
* **The port already sits on the exact moment a voice plays.** It owns the
  XMA decoding of every sound object (`src/audio/xma_native.cpp`,
  [findings/54](54-native-sound-output.md) s.5) and wraps the function that
  turns a sound ID into its file (`sub_82336430`). Start, playback position
  and end of a voice can be read from these without timers (s.6).
* **Movies are a separate path**: Bink files with one dialogue track per
  language, opened and closed through `PlayerBink` (s.3.1). Their subtitles
  follow the movie's own clock.
* **Drawing**: a Scrooby text (the game's menu library) added to the in-game
  project by the data patcher, as the port already does for players 3-4's HUD
  texts, uses the game's font (Ã ã included, [findings/49](49-languages.md)
  s.8.3) and works in both renderers (s.7).

## 2. How this was investigated

Repository only: `AGENTS.md`, findings 19, 25, 26, 27, 30, 37, 39, 49, 51,
53, 54, 56 and the sources they point to (`audio_trace.cpp`,
`audio/xma_native.cpp`, `options/global_options.cpp`, `cheats/spawn.cpp`,
`players/victory_scenes.cpp`, `input/movie_skip.*`, `ui/scrooby.*`,
`language/*`). No disc, no generated code (`generated/default/` is not in
the repository) and no running game were available, so:

* counts below come from earlier findings, not from a new census;
* no Lua script (239 in `default.rcf`, plain Lua 5.1 bytecode with their
  source file names, [findings/19](19-missing-dialogue-5-1-xma.md) s.1) and
  no fight tree was read for this finding;
* no function was disassembled for it.

A public search found nothing that says another version (PS2, Wii, PSP)
shipped dialogue subtitles **(to check on those discs, if anyone has them:
a text source would save transcription work)**.

## 3. What is spoken: four families

| Family | Where the audio is | How many | Timing source |
|---|---|---|---|
| A. Story cartoons (2D movies) | `movie1.rcf` / `movie2.rcf`, Bink 1, one file per movie; dialogue on its own track per language (English = track 1) | `Crash_01..19_51.bik` (s.13.4) | the movie's clock |
| B. In-engine cutscenes (NIS) | `english.rcf`, `character\english\nis\...`; e.g. `nis\6ch\l1_r1_sc4_nvmail_ch*.wav`, ONE file per scene holding all of its lines | 6 scenes (s.13.2); most other scenes play their voices as clips (C / D) | the sound's playback position |
| C. Character voices in play | `english.rcf`, one clip per line, mono; folders per character (`labrat1`, `bratgirl1`, `znu`, `slappe`, `monkey1` (name TBD) ...), ~200 lines per enemy variant | 238 NPC / story + 36 loudspeaker in scope; ~7,700 combat lines out (s.13) | clip start / end |
| D. Aku Aku and the tutorial voice | `english.rcf`, `akuaku` and `tutorial` folders | 82 + 176 (s.13) | clip start / end |

8,320 voice clips per language in total; the other 1,162 sounds in
`default.rcf` are effects and music, not dialogue ([findings/19](19-missing-dialogue-5-1-xma.md)
s.1). Mono 9,256, stereo 191, 5.1 35 (6 of them dialogue) across both
archives.

### 3.1 A: the cartoons

* Started by `CMovieScreenAction` / `CMoviePlayerBehaviour` (vtable
  0x820334E4) in a level (the scene after talking to Coco on Wumpa Island is
  one, [findings/39](39-mouse-in-menus.md) s.3), from the movie list in
  Crash's house, and the boot sequencer (`sub_82188448`,
  [findings/25](25-quick-load.md) s.2; the boot movies are logos plus the
  attract movie).
* `sub_822C42A0` (the movie start) picks the dialogue track from the
  language letter; `PlayerBink` (vtable 0x8201609C) slot 6 `sub_82428E58`
  opens, slot 8 `sub_82428FA8` closes (`BinkClose`, handle at +0x1C becomes
  0), slot 11 `sub_82428908` is called every frame while a movie plays
  ([findings/54](54-native-sound-output.md) s.8). The renderer's interceptor
  already wraps every `PlayerBink` method.
* So: the movie's NAME is known at open (the path built from the table /
  the open's argument, **to check** which register), its end at close, and
  the current frame from the Bink handle (Bink 1 keeps the frame count and the
  current frame number near the start of its struct, **to check** the
  offsets on this build). Skipping closes the player: the subtitle clears
  with it.
* Pausing during a movie: **to check** whether the movie pauses (frame number
  stops) or plays on under the pause menu.

### 3.2 B: in-engine cutscenes

* Driven by the level scripts: `CMasterCinematicAction`,
  `CRequirementPlayCinematic` (vtable 0x8204103C, update `sub_822A36D0`,
  hands the cinematic manager at game +0x6C a camera name; the manager's
  +0x2C has bit 0x10 / 0x40 / 0x80 set while it plays,
  [findings/26](26-more-local-players.md)) and
  `CCinematicSoundTrackAction` (slots 4 / 6 `sub_82240EB0` / `sub_82241150`,
  [findings/32](32-native-saves-and-profiles.md)).
* The dialogue is one sound per scene: the nvmail track is 11.04 s, speech
  0-6.3 s, a pause, speech 7.5-9.7 s ([findings/19](19-missing-dialogue-5-1-xma.md)
  s.3). Several lines and possibly several speakers live in one file, so a
  subtitle needs **cues inside the sound**: (start, end) in samples from the
  sound's start, one per line, each with its speaker.
* Because the port decodes that sound itself, the position heard is known
  to the sample (s.6). A cutscene that is skipped or interrupted destroys the
  sound object: the subtitle clears with it.

### 3.3 C and D: voices in play (NPCs, enemies, Aku Aku)

* `CSoundDialogueBehaviour` (vtable 0x820370A4) is "the voice lines" of a
  character: set-up `sub_8222CBB8` links up to four named objects, update
  `sub_8222CFC8` reads a sound object at +136 ([findings/27](27-cheat-menu.md),
  the boss titans). Whether NPCs (Coco, the mission givers with a "!")
  and Aku Aku use the same behaviour is **to check**.
* `audio::SeqEventClip` (slot 1 `sub_823385E8`) and `audio::SeqEventStream`
  (slot 1 `sub_82348AF8`) are the sequencer events that play clips and
  streams; the trace already labels them "clip" / "stream". The FE_SFX bank
  shows cues = named lists of clips with random variants
  ([findings/51](51-achievements.md)): a voice cue picks one of several
  takes, so **the ID of the take actually played** is what a subtitle must
  key on, never the cue name.
* Interactables (`CInteractableBehaviour`, vtable 0x8202EA6C,
  [findings/37](37-keyboard-mouse-prompts.md)) say when Crash can talk to
  something; trigger volumes (`CRequirementTriggerVolume`) start
  proximity lines; scripts start mission lines. **The trigger kind is not
  needed to show a subtitle** (the voice playing is what matters), but it is
  wanted for the inventory and for the "barks" setting (s.8). It can be
  recovered from the call stack at the play moment (the debug console's
  `who` and guest stacks, [findings/29](29-debug-console.md)).
* Aku Aku vs the HUD: hints in play are also TEXT (`InGame.pag`
  `HintMessage`), the tutorials are text pages (`InGame_Tutorial.pag`,
  `CTutorialScreenAction`) and the message bar shows objectives. **To check**
  for each Aku Aku line whether the same words are already on screen; if
  they are, a subtitle would repeat them, and the setting may skip those
  lines (s.8). They must never be mixed up: a subtitle comes only from a
  voice playing, never from a HUD text.

## 4. What links a voice to its line

| Piece | Stable? | Use |
|---|---|---|
| 64-bit sound ID (archive file name) | yes, and the SAME in every language's archive ([findings/19](19-missing-dialogue-5-1-xma.md), audio_trace header) | **the key** of a subtitle entry |
| Designer file name (`.rsd` header) | yes | speaker (folder), scene (NIS names carry level / room / scene: `l1_r1_sc4_nvmail`), human-readable id |
| Sample count / rate (XMA header H: end frame +8, rate +0x0C) | yes | the clip's length: a cue's end can never be later |
| Movie file name | yes | the key for family A |

An entry's id: `<sound ID hex>` for a whole clip, `<sound ID hex>#<n>` for
the n-th line inside a cutscene track, `movie:<name>#<n>` for a movie. The
designer name goes beside it as a comment-like field so people can read the
file, not as the key (it doesn't need to be unique).

Voice languages: the IDs repeat across archives, but a line's TIMING
differs per language (another actor's take). Cues inside a NIS track or a
movie are therefore per voice language; a whole-clip line needs no cue. With
English voices only the English timing is needed first.

## 5. Text: where it comes from, and where it may live

There is nothing to extract, so every line needs a transcription (English)
and a translation (Brazilian Portuguese, other languages later).

**Repository rule ([AGENTS.md](../../AGENTS.md) s.2, hard):** no text from
the game in tracked files. An English transcription of the game's script is
the game's text. The precedent for translations of the game's text is
`languages/game/pt_BR.po`: keys only, the translated text, no English
([findings/49](49-languages.md) s.8.2). Options for the English side, for the
maintainer to decide (s.11, question 1):

1. **Player-made English, never in the repository.** A tool decodes the
   player's own disc locally (the port's XMA core, `tools/xma_test`), runs a
   local speech recogniser and writes drafts to
   `<game folder>/user/languages/subtitles/en.po`. Drafts are machine output,
   flagged `fuzzy`, and need checking by ear. The repository ships only
   the keys, the cue timings (numbers) and translations.
2. **English in the repository** like other fan subtitle projects do. Breaks
   the hard rule as written; needs the maintainer's explicit decision.
3. **Translations only** (the `pt_BR.po` precedent), with English shown only
   where a player has a local file.

Whatever is chosen: no invented lines. An entry without checked text is
`needs-transcription` and shows nothing in game.

## 6. Timing: following the voice, not a timer

What the port can see for each sound object ([findings/54](54-native-sound-output.md)
s.5, `xma_native.cpp`):

| Moment | Where |
|---|---|
| the sound's ID is turned into a file | `sub_82336430` wrapper (r5 -> ID), already in `audio_trace.cpp` |
| a decoder object is created for it | `sub_8234BE88` (XmaDecoder constructor) |
| playback position | object +0x40 = frames handed out; the rings' frames read (+0x3C + 0x18 k, +0xC) |
| its end | +0x40 reaching the end frame (H+8), or the destructor `sub_8234BC58` |
| a seek | `sub_8234C740` (rewritten by the port) |

Rules that follow:

* **Start** = the first frames of that object are handed out (not the file
  open: clips are also opened by level loads through
  `CSoundResourceManager::ClipLoadThread`, the trace's "load" kind, which
  does not mean "playing", [findings/19](19-missing-dialogue-5-1-xma.md) s.2).
* **End** = the end frame reached or the object destroyed, whichever comes
  first; plus a short hold (about 0.3 s) so a short line stays readable, cut
  off at once if the next line of the same family starts.
* **Pause**: the game stops reading a paused sound; the position stops and
  the subtitle simply stays, then goes on. **To check**: whether the pause
  menu stops voices or lets them finish. If it hides the HUD, the subtitle
  follows the HUD's own hiding (s.7).
* **Game speed / frame rate**: positions are in samples, so nothing depends
  on the frame rate.
* **Lag between "handed out" and "heard"**: the queue to the speakers is
  up to 8 blocks = 43 ms ([findings/54](54-native-sound-output.md) s.2) plus
  XAudio's own buffer **(to measure, with `--debug_audio_dump` and the
  trace's dump times)**. A fixed offset of that size is fine for reading.
* **A voice that fails to play** never hands out frames: no subtitle.
* **Several voices at once**: one line per active voice, newest at the
  bottom, at most two lines on screen; cutscene and Aku Aku lines go before
  enemy barks (s.8).

**The open link (to check, the first thing to verify):** connecting a
decoder object to its sound ID. The ID is known in `sub_82336430`, the
object in its constructor, and between them the file is opened
(`0x823411C8`) and an `RsdFileStream` built (the decoder's inner stream at
+0x04). Two candidate ways: remember the last ID per guest thread and claim
it in the constructor on the same thread; or remember the file stream the
open returns and match the decoder's +0x04. Which one holds, and whether
short clips go through an XmaDecoder at play time at all (rather than being
decoded once at load into memory), has to be seen in a run.

Movies (family A) use the Bink frame number / frame rate instead: a cue is a
time from the movie's start.

## 7. Drawing the subtitle

Options ([findings/30](30-menus-and-hud.md) s.7), in the order of the
project's rules ([AGENTS.md](../../AGENTS.md) s.6: player-facing UI in the
game's own look, ImGui only for tools):

1. **A Scrooby page of the port's own in `InGame.prj`** (every in-game
   package, the way `data_patcher` adds players 3-4's HUD texts), with one or
   two `Text` elements (Titans_Small, centred) and a `Polygon` behind them
   (the game's dimmer shape) for the background. Strings set every frame
   through the copying setter `sub_82373360` (UTF-16). Works in the emulated
   and the native picture, shows in photos, and the game's font has the
   Brazilian Portuguese letters. The same page is needed in
   `GameStart.prj` for movies watched from Crash's house **(to check which
   project is on screen then)**.
2. Native 2D drawing in the renderer: freer, but only in the native
   picture.
3. ImGui: not allowed for player-facing text.

Details to settle with route 1:

* **Size**: one more matrix on PDDI's 2D stack, as the HUD Size option does
  ([findings/53](53-hud-size.md)), gives Small / Medium / Large without new
  fonts.
* **Wrapping**: whether a Scrooby text wraps inside its box is **to check**;
  if not, the manager breaks lines itself by the font's advances (the font
  records are known, [findings/37](37-keyboard-mouse-prompts.md)).
* **Position**: the 640 x 480 canvas is centred on the screen; the bottom
  band (above the message bar's panel, which sits at the bottom) for play, a
  little lower inside the letterbox bars (`InGame_Letterbox`) in cutscenes.
  Widescreen and ultrawide: the canvas stays centred
  ([findings/56](56-widescreen-and-resolution.md)), so a centred subtitle
  needs nothing more.
* **Not over the HUD**: players 3-4's corners are at the bottom; the
  message bar and the "Skip" prompt (bottom right) are the elements to keep
  clear of.
* **Characters**: `tools/translations.py check` already refuses what the font
  can't draw; the same check runs on subtitle files.

## 8. Settings

`REXCVAR`s kept in `user/settings.toml`, rows on a new **Subtitles** tab or
heading of the Options screen (`src/options/options_settings.cpp`,
[findings/31](31-options-screen.md)). Default OFF, as the original has none
([AGENTS.md](../../AGENTS.md) s.6).

| Row | Setting | Values |
|---|---|---|
| Subtitles | `subtitles` | Off / Cutscenes / Cutscenes and Dialogue |
| Language | `subtitle_language` | Same as Text / English / Brazilian Portuguese / (more) |
| Speaker Names | `subtitle_speakers` | On / Off |
| Background | `subtitle_background` | On / Off |
| Size | `subtitle_size` | Small / Medium / Large |
| Position | `subtitle_position` | Default / Higher / Lower |

Combat lines (about 7,700 clips, s.13) are out of scope (s.11): in a fight
they would fill the screen, and most games leave them out. "Same as Text"
uses `language::RunningText()`; a missing line in the chosen language falls
back to English only if the player has an English file, else nothing.

Speaker names: the folder of the clip for families C and D, a name per cue
for A and B, written with the game's own names as the text banks spell them
(`tools/text_banks.py` glossary, [findings/49](49-languages.md) s.7); a
speaker that can't be told for sure gets no name.

## 9. Proposed architecture

```
src/subtitles/
  voice_events.{h,cpp}   hooks: ID -> decoder object -> start / position / end
                         (wraps sub_82336430 with audio_trace, sub_8234BE88,
                         sub_8234BC58; reads +0x40 each frame); PlayerBink
                         open / close / frame for movies
  subtitle_db.{h,cpp}    loads languages/subtitles/<code>.po (+ user/ override):
                         msgctxt = entry id, msgstr = text; cue timings and
                         speakers from a separate cue file (numbers only)
  subtitle_manager.{h,cpp} active voices -> lines on screen (priorities,
                         hold time, two lines max, settings)
  subtitle_view.{h,cpp}  the Scrooby page (data patch) and per-frame strings
tools/subtitles.py       inventory / export / check / status (like game_texts.py)
```

* Everything off unless `subtitles` is on; with it off the hooks only pass
  through (the original's behaviour).
* The database follows the existing translation pipeline (gettext `.po`,
  `fuzzy` for drafts, `tools/translations.py`-style checks): missing
  translations, unknown ids, cues longer than their clip, characters the
  font lacks.
* No game file is changed on disk; the page is a run-time data patch
  ([findings/24](24-save-system.md), `src/data/`).

## 10. Inventory tool (Phase 2)

`tools/subtitles.py inventory <game folder>` reads the player's own
`english.rcf` (and other voice archives), using the RCF / RSD formats
already documented ([findings/19](19-missing-dialogue-5-1-xma.md) s.1,
`audio_trace.cpp`), and writes **to `user/`, never to the repository**:
sound ID, designer name, speaker folder, family guess (nis / character /
Aku / tutorial), channels, length in seconds. Optional `--scripts`: the
string constants of the 239 Lua scripts (`luac`-style constant tables),
to find which scripts name which cues / clips, i.e. the triggers. The
repository's finding keeps only counts and structure.

A second source for the inventory is the game itself: with
`--debug_subtitle_trace` (Phase 3) every voice that really plays is logged
with its ID, start / end and the game call stack, so a playthrough lists
what is actually used and when.

## 11. Decisions so far (contributor's choices; the maintainer's review pending)

1. **Where the English text lives** (s.5): option 2 was chosen, English
   transcriptions in the repository. This goes against AGENTS.md s.2 as
   written ("no text strings from the game in tracked files"), so it needs
   the **maintainer's explicit approval** before any transcription is
   committed. Until then nothing with the game's words is committed; the
   tooling (keys, timings, the inventory tool) can go in either way.
2. **Scope**: cutscenes (both kinds), NPC / story lines and Aku Aku (with
   the tutorial voice and Cortex's loudspeaker lines, s.13). Combat lines
   (enemies' and the bosses' / partners' fight barks) are left out; the
   setting keeps room for them later (s.8: the "All" step is dropped for
   now). The voiced credits lines are optional (s.13).
3. **Documentation**: this finding, extended section by section, instead
   of separate `docs/subtitles/*` files; the roadmap entry is in
   [03-roadmap.md](../03-roadmap.md) (the repository has no
   `PROJECT_STATUS.md` / `NEXT_TASK.md`).

## 12. First milestone (proposed, small, no visible change)

**`--debug_subtitle_trace`**: a log line for every voice that actually plays,
with its sound ID, designer name, decoder object, first-frame time, end time,
end reason (finished / destroyed / seek) and, for movies, open / close and
frame count. Verified on three cases, each with `--debug_audio_dump` to see
the line in the recording:

1. the nvmail cutscene (B): one start, end at 11.04 s, the two speech
   stretches visible in the dump at the logged times;
2. an Aku Aku tutorial line (D);
3. a cartoon movie (A), including a skip halfway.

This answers the open link of s.6 and the pause / skip questions before any
subtitle is drawn. Next: one test line from a local `user/` file on the
Scrooby page, then the cutscenes.

## 13. The census: what the English voice archive and the logs show

Made with `python3 -I tools/subtitles.py inventory english.rcf <out> --logs
<play-*.log>` on a player's own disc (USA / Europe 360 disc) and fifteen of
that player's session logs (audio trace on, the default). The per-clip list
stays in `<out>` (user data); only counts and structure are written here.

### 13.1 Totals

8,320 clips, 322 minutes. All mono except the 8 cutscene tracks. Every name
is `character\english\<folder>\<file>.wav`.

| Group | Clips | Minutes | Folders | Played in the logs | In scope (s.11) |
|---|---|---|---|---|---|
| Combat lines: `hit*`, `*jacked`, `*wins`, enemy barks (bratgirl / labrat / monkey / slappe / znu 1-6, ~200-340 each) | 7,694 | 246.1 | 45 | 912 | no |
| NPC / story lines (`ingameNN_<who>_00k`) | 238 | 25.9 | 15 | 17 | yes |
| Tutorial voice (`tutorial08_NNN_<all / 360>`) | 176 | 14.0 | 1 | 60 | yes |
| Aku Aku (`akuaku\ingameNN_aku_00k`) | 82 | 5.2 | 1 | 39 | yes |
| Cortex's loudspeaker lines (`loudspeakerNN`) | 36 | 8.6 | 1 | 21 | yes |
| In-engine cutscene dialogue tracks (`nis\6ch`, `nis\pl2`) | 8 | 3.1 | 1 | 6 | yes |
| Credits (`credits\`: one clip per credited person, plus `credits0NN`) | 86 | 19.3 | 1 | 0 | optional |

**In scope: 540 clips, about 57 minutes** of English speech, plus the story
cartoons (s.13.4).

### 13.2 What the names say

* **`ingameNN` is one numbering across the whole script**, shared by every
  speaker: Coco 01-13, Uka Uka 14-15, the NPCs 19-73 (one contiguous range
  each), Aku Aku 01-141, and 99 for a scene with several speakers. Most NPC
  numbers have **4 takes** (`_001` .. `_004`), so an NPC line is a group of
  variants picked at play time: each take is its own entry (its own ID,
  possibly other words). Aku Aku has mostly one take per number.
* **Speakers** come straight from the folder (aristocrat, assistant, blob,
  cub, elder, melvin, old, scrappy, shady, nina, coco, crunch, cortex,
  ukauka, akuaku: folder names, NOT the characters' official names, which
  are still (name TBD) for most NPCs). The tutorial folder's speaker is
  **(to check by ear)**.
* **Tutorial lines come per platform**: 142 `_all` and 34 `_360` (the
  Xbox wording of a line that names buttons). No `_ps2` / `_wii` takes are in
  this archive. Only the clip the game plays matters, so the split needs
  nothing special.
* **In-engine cutscenes with dialogue: only 6 scenes** (11-34 s each; two
  of them also as a stereo `pl2` copy, never seen played: Pro Logic II
  output, (to check) when the game picks it). All the other in-engine
  scenes seen in the logs (about 30: monster intros, "establish" shots,
  fights) play an effects / music track from `default.rcf` and their voices
  as ordinary clips: e.g. the Aku Aku tutorial scene's track opens, then
  tutorial-voice clips follow at their own times; after the Crunch landing
  scene's track an Aku Aku line plays 5.7 s later. So most cutscene dialogue
  is **family C / D clips**, one line each, not cues inside a track.
* `default.rcf` holds a few voice-like `nis\*.wav` sounds with character
  names (e.g. one named after N. Brio) that are not in the language
  archives, so not localized: **(to check by ear)** whether they are
  speech. `default.rcf` (~990 MB) was not read for this finding.

### 13.3 When the trace sees them

* Every in-scope line in the logs is of kind **"load"** (through
  `0x82336598`), never "clip" or "stream", and it appears **at the moment
  the line plays**: Aku Aku and tutorial lines are not loaded with the
  level. So for these lines "load" is a usable start event, but not proof of
  playing (a level load also logs "load" for combat banks).
* `sub_82336B20` (the "clip" / "stream" path's wrapper) was read in the
  generated code: it builds the path, opens the file and reads the RSD
  header (1,060 bytes) into a sound record whose ID is at record +0 / +4,
  then stores two results at record +12 / +16. It reads the sound's INFO;
  it is not the moment of playing.
* The XmaDecoder constructor (`sub_8234BE88`) reads the 12-byte size header
  through its inner stream's Read (vtable +24) at the stream's current
  position; nothing in the object holds the ID. The link object -> ID stays
  the first thing milestone 1 has to establish (s.6, s.12).

### 13.4 Story cartoons

The logs show the movies by name when they open (the language hook's log
line, `language.cpp`): the four boot movies and **`Crash_NN_51.bik`**, NN =
01 to 19 (17 seen; 09 and 12 not in these logs). Each opened with tracks
`0 1 2 3` (English dialogue = track 1). Their length and the lines in them
are not known yet: `movie1.rcf` / `movie2.rcf` (~1.4 GB each) were not
read. The skip prompt's log lines (`Movie skip: ...`) show the skip moment,
so a subtitle's clear-on-skip can be checked from a log.

### 13.5 What this changes in the plan

* Most in-scope lines are **one clip = one line** (cues inside a sound only
  for the 6 cutscene tracks and the cartoons): the database is mostly
  "sound ID -> text", timing from the clip itself.
* A scene's speakers are known per clip (folder), so speaker names are
  reliable for everything but the 6 tracks and the cartoons (per cue there).
* Transcription workload in scope: ~57 minutes of clips + the cartoons.

## 14. Milestone 1 written: `--debug_subtitle_trace`

`src/subtitles/voice_trace.{h,cpp}`, off by default, log lines only:

* **Wrapped here:** the XmaDecoder constructor `sub_8234BE88` (after it:
  the object's H and its sound ID, found as in s.15.2) and its Read `sub_8234C8A8` (after it: +0x40;
  first frames out = "start", a gap over 0.4 s between Reads = "stalled ...
  resumed", +0x40 reaching end - lead-in = "end").
* **Called from existing wrappers:** `sub_82336430` (audio_trace.cpp, every
  ID, also with the audio trace off), the destructor `sub_8234BC58`
  (xma_native.cpp: "gone", with the position reached and why: played out /
  never started / cut off), PlayerBink's open (language.cpp).
* **Movies:** after the open, a frame-end listener (pddi) logs the first 8
  words of the Bink handle once a second (to find the current-frame word)
  and the close when the player's handle (+0x1C) becomes 0. No handler on
  PlayerBink slots 8 / 11, which `options/global_options.cpp` owns in newer
  versions: the trace works the same with or without the movie volume code.
* `audio_trace.h` gains `Indexed()`, `SoundName()`, `ClipHeaderOf()`,
  `DumpSeconds()`.

Checked before the first run: syntax-only compiles (`-fsyntax-only`, g++
13 and clang 18, the SDK's headers) of every changed file; then built on
Windows (clang-cl, the launcher's build) and run (s.15).

## 15. The first run: movies settled, the decoder link corrected

One Windows session with the first version of s.14 (new game to just after
the first boss fight: the nvmail cutscene, Aku Aku and tutorial lines,
twelve Bink movies, most skipped, one played out).

### 15.1 Movies: the Bink handle read

The words logged once a second (the player's +0x1C = the Bink handle) are,
in order: **width, height, frame count, current frame (1-based), the
previous frame, frame rate, frame-rate divisor**: e.g. 1280, 720, 2002,
400, 399, 2997, 100 = frame 400 of 2002 at 29.97 frames per second. So:

* a movie's position = (handle +0x0C - 1) x (+0x18) / (+0x14) seconds;
* its length = (+0x08) x (+0x18) / (+0x14) (story cartoons 1-8: 56 to 181 s);
* the close: the handle at player +0x1C becomes 0 within a frame of the
  skip or the end (10 skips and one played-out movie, Crash_02, frame 1826
  of 1841 at the close).

Family A's timing is settled: cues in movie seconds, no audio hook.

### 15.2 Voices: the same-thread guess was wrong

All 20 decoders logged had no ID on their thread. The IDs are turned into
paths on the main thread and on the loader thread (two thread ids in the
`Audio:` lines); every decoder is built on one other thread (the audio
thread). The trace then took every decoder for a non-voice and logged only
its sample of 20, so this run says nothing yet about voice start / end /
pause.

The link is now made by the sound's own numbers, compared with the IDs
asked for in the last 15 s on any thread (s.14, `voice_trace.cpp`):

| Decoder | `.rsd` file |
|---|---|
| H+8 (end sample) | 0x814 (H+8 at 0x80C) |
| +0x1C (compressed bytes; the third word of the 12-byte size header the constructor reads into +0x18 / +0x0C / +0x1C) | 0x808 |

Measured on the English archive: 1,840 different end samples among 8,320
clips, 3,925 different pairs (end, bytes); 102 of the 540 in-scope clips
share their pair with some other clip of the archive. Replaying this
session's `Audio:` log against the archive: **0 of the 41 in-scope lines
asked for** had another ID with the same pair asked for in the 15 s before
it. Both numbers are read on the asking thread with the name (cached), so
the audio thread reads no file. Sounds of `default.rcf` (515 asked for in
this session) are matched the same way.

Next run: the same checks with the corrected link (start, stall, end, gone
for voice lines; a pause during a line).

## 16. The second run: voices tied to their sounds, and what "playing" looks like

A new game from the start to the third story cartoon: the cartoons, the
tutorial voice, Aku Aku, Coco and Crunch talking in turn, the nvmail
cutscene, a fight with Ratnicians, two pauses during tutorial lines. 83
decoders were tied to a voice name (s.15.2).

### 16.1 The link holds

All 83 found exactly **one** ID with their (end sample, compressed bytes),
asked for **0 ms** before the decoder was built (the trace's own clock), so
the asking and the building happen back to back. The decoders were built
on one thread (the audio thread) and the `Audio:` lines came from the main
thread. The nvmail track: one decoder, 3 streams, 11.03 s, as findings/19.

### 16.2 The position read is the decode position, not what is heard

The decoder's +0x40 does not follow the sound out of the speakers:

* the **first Read takes 4.52 s** of sound (every clip: 12 of 12 longer
  clips logged their next read at exactly 4.52 s), and then **2.26 s every
  2.02-2.39 s** of real time: the mixer's buffer refilling as it plays;
* so a clip of 4.52 s or less is **all decoded in its first Read** (17-30 ms
  after it is asked for), and its decoder is destroyed right after (a 1.6 s
  Aku Aku line: built, read and destroyed within 22 ms);
* a longer clip's decoder is destroyed when its data runs out, still 2.26 s
  or so before the end of the sound (the nvmail track: destroyed 6.94 s
  after its start, an 11.03 s sound).

What this gives a subtitle:

| Moment | From |
|---|---|
| start | the decoder's first Read (+0x40 > 0), 10-40 ms after the ID is asked for |
| end | start + the clip's own length (H+8 / rate) |
| playing position | time since the start (the refills come every ~2.26 s of real time, as a check) |
| cut off | only for clips longer than 4.52 s: the decoder destroyed before all of it was decoded (seen once: a cutscene track destroyed at 4.52 s of 12.70 s, 1.98 s after its start, a cutscene skipped) |

A short line (4.52 s or less) that is stopped early can't be seen here: its
decoder is already gone. For cutscenes, the cutscene's own end (the
cinematic manager, game +0x6C, findings/26) will clear the subtitles; a
voice stopped by the game in the middle of play stays **(to check)** (the
playing object is Radical's sound player / an XAudio voice, not the
decoder).

### 16.3 The pause menu does not stop the voices

Both pauses (8 s and 2 s) fell inside tutorial lines (8.22 s and 6.12 s).
The refills went on at the same 2.26 s pace through the pause and the
lines ended on time, so the mixer kept playing them: the game lets a voice
line finish under its pause menu (**to confirm by ear**). A subtitle then
keeps its own clock through a pause too; nothing to freeze.

### 16.4 Other observations

* Coco and Crunch alternate in one conversation (`ingame04_coco_001`,
  `ingame04_crunch_002`, `ingame05_coco_001`, `ingame05_crunch_002`): the
  `ingameNN` number is the conversation, the last number the line within it
  (s.13.2 read `_00k` as takes; for these two it is the order of the
  lines). **(To check)** for the NPC folders with `_001`-`_004`: takes or
  lines.
* `default.rcf`'s `nis\*.wav` tracks (`*_establish`, `cocopart_1`, `mail`)
  play at the start of their scenes like the dialogue tracks; whether any
  holds speech is still to be heard.
* Two movies played to the end (Crash_02: 61.4 s, Crash_04: 65.6 s), their
  close at their length; the others skipped. Same as s.15.1.

### 16.5 Milestone 1: done

The questions of s.6 / s.12 are answered: the decoder -> sound ID link
(by length and size), the start, the end (start + length), cut-offs for
long clips and cutscene skips, pauses (nothing to do), movies (the Bink
frame). Next: milestone 2, the first subtitle on screen: a line from a
local file shown on a Scrooby page added to the in-game project, for
the voice families above.
