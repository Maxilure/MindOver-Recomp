# Translations

The game itself already speaks many languages: its menus, tutorials, subtitles and voices come
from the disc in English, French, German, Italian, Spanish, Dutch, Danish, Finnish, Norwegian,
Swedish and Russian (Options → Language). The texts in this folder are the ones **this port adds**
on top: its Options rows and descriptions, the Players screen, the outfit chest, question boxes,
button prompts, the launcher, the tool windows (F1) and so on, about 970 short texts. (The
achievements come from the disc in every language the game has: nothing to translate here.)

The first version of every language here is a **machine translation**, written using the
game's own official words for each language (its name for "Options", the titans, the moves...).
Nobody who speaks these languages has checked them yet. **That's where help is needed.**

## Progress

"Checked" = a person who speaks the language has read the text and agrees with it.

<!-- status -->
<!-- /status -->

## How to help

You don't need to build anything or know how to program. Pick whichever is easiest for you:

1. **Point out a mistake.** Open a [translation issue](https://github.com/Maxilure/MindOver-Recomp/issues/new?template=translation.yml),
   write what's wrong and what it should say. A screenshot helps.
2. **Edit the file on GitHub.** Open your language's `.po` file above, press the pencil icon
   ("Edit this file"), change the lines, and propose the change. GitHub makes the pull request
   for you.
3. **Use a translation app.** [Poedit](https://poedit.net/) (free, Windows / macOS / Linux)
   opens `.po` files and shows each English text next to its translation. Save, then send the
   file in an issue or a pull request.

### What a `.po` file looks like

```
#. {0} = the key that shows the HUD again ("F2").
#: src/ui/hud_hide.cpp:73
#, fuzzy
msgid "Press {0} to show it again"
msgstr "Appuie sur {0} pour le réafficher"
```

- `msgid` is the English text. **Never change it**: it's how the game finds the translation.
- `msgstr` is your language. That's the line to edit.
- `#.` lines are notes about where the text shows and what the `{0}` stands for.
- `#, fuzzy` marks a machine draft. **When you've checked a text, delete its `#, fuzzy` line**
  (Poedit: untick "Needs work"). That's what moves the "Checked" column above.
- `msgctxt` (on a few entries) tells apart two texts that are the same word in English but
  different things, like the **Delete key** on a keyboard and the **Delete** button of the save
  list.

### Rules of the game's text

- **Keep every `{0}`, `{1}`...** They're filled in by the game: a button picture, a key, a
  player's number, a name. You may move them anywhere in the sentence, but each one must stay.
- **Keep the line breaks** (`\n`) where the English has them: those texts go in the game's
  question boxes, one line each.
- **Use the game's own words.** If the game calls the jack move "Patschen" in German or Ratcicle
  "Frigirat" in French, the port's texts say the same.
- **Talk to the player the way the game does**: the informal "you" (tu, du, tú...) in every
  language that has one.
- **Only characters the game's font has.** It has the Latin letters with their accents, and
  Cyrillic (but no Ё/ё: write Е/е, like the game). It has **no** curly quotes (“ ” « »), long
  dashes (– —), € or •: use straight quotes `"` and a plain `-`. The characters ¥ to ¾ are
  the font's button pictures: never use them as letters.
- **Short where the English is short.** Row names and values sit in small boxes. A longer
  description is fine: the cards wrap.

## Trying your changes in the game

Put your edited file in your game folder as `user/languages/<code>.po` (for example
`user/languages/fr.po`). The game reads it instead of its own copy, so you see your version
without building anything. Choose the language in Options → Language → Text and restart.

## For developers

- Texts are written in English in the C++ code as `tr(u"...")` (see
  [`src/language/texts.h`](../src/language/texts.h) for `trc`, `trf`, `trn`).
- After adding or changing texts: `python3 tools/translations.py update` (refreshes
  `template.pot` and every `.po`, keeping what's translated), then `check` and `status`.
- `tools/translations.py check` lists missing texts, mismatched `{0}` slots, wrong line
  counts and characters the font can't draw.
- The flag `debug_pseudo_texts = true` shows every port text accented, 30 % longer and in
  brackets: English that slipped past `tr()` and boxes too small for long languages stand out.
- `tools/text_banks.py game/default.rcf <folder>` writes the game's own strings for each
  language next to the English (a glossary). Those are the disc's texts: keep them on your
  machine, never in the repository.


## The game's own text in Brazilian Portuguese

The disc has no Portuguese, so for Brazilian Portuguese the port also
translates the game's own menus, tutorials and missions:
[`game/pt_BR.po`](game/pt_BR.po). That file has no English in it (the game's
text isn't ours to publish): its lines are keyed by number. To work on it,
make a copy with the English beside each line from your own game files:

    python3 tools/game_texts.py edit pt_BR     (writes user/languages/game/pt_BR.edit.po)
    python3 tools/game_texts.py pack pt_BR     (puts your changes back into game/pt_BR.po)
    python3 tools/game_texts.py check pt_BR

Keep the button symbols (± ² ³ ...) and the "†" (a space that keeps two
words on one line) exactly where the English has them.
