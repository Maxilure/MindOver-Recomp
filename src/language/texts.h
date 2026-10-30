// =============================================================================
// language/texts.h -- the PORT'S OWN texts in the player's language
// =============================================================================
//
// The game's own texts (menus, HUD, subtitles) come from its text banks in
// every language on the disc (language.h, findings/49). The texts the PORT
// adds (Options rows, the Players screen, question boxes, prompts...) are
// written in English in the code and translated through these files:
//
//   languages/<code>.po   one per language, the standard "gettext" format
//                         (editable on GitHub, in Poedit, or on Weblate)
//   languages/template.pot  every English text, made by
//                         tools/translations.py extract (never by hand)
//
// In the code a text is written as
//     tr(u"Fullscreen")                     the English text = the key
//     trc("window mode", u"Off")            same, with a CONTEXT: one English
//                                           word that may need different
//                                           translations ("Off" a mode vs a
//                                           level: French désactivé/-ée)
//     trf(tr(u"Player {0} joined"), {n})    fills {0} {1}... after translating
//     trn(u"Ruby")                          marks a text in a static table
//                                           for the extract tool; returns it
//                                           untouched: tr() it where shown
//     trcn("key name", u"Delete")           the same with a context: shown
//                                           with trc("key name", text)
//
// A comment line "// TR: ..." right above a tr() call is copied into the .po
// file for translators (where it shows, how much room it has).
//
// Which file: the running text language (language::RunningText(); fixed for
// the session like the game's own, so a change needs a restart). English
// (US and UK) uses the code's texts as they are. A missing translation falls
// back to English. Loaded once, at the first tr() call.
//
// Players' own files: <game folder>/user/languages/<code>.po is read INSTEAD
// of the shipped one when present = translators try a change without
// building anything (restart the game).
//
// Machine drafts are marked "#, fuzzy" in the .po files (Weblate shows them
// as "needs checking"); the game still uses them, and the Language tab says
// how much of the language a person has checked (CheckedPercent()).
//
// The flag debug_pseudo_texts = Radical's own trick (their "X" language,
// findings/49 s.1): every port text drawn as "[Ŧëxŧ ~~~]", accented and 30 %
// longer, so an English text that skipped tr() and a box too small for longer
// languages both stand out.
// =============================================================================
#pragma once

#include <initializer_list>
#include <string>
#include <string_view>

// The English text in the running language. The reference stays valid for
// the whole session.
const std::u16string& tr(std::u16string_view english);
const std::u16string& trc(std::string_view context, std::u16string_view english);

// {0}, {1}, ... in `text` replaced by the arguments (in order of the
// numbers, not of the text: a language may put them the other way round).
std::u16string trf(std::u16string_view text, std::initializer_list<std::u16string_view> args);

// A text marked for translation but used later (static tables); trcn with
// a context, looked up later with trc(context, ...).
constexpr const char16_t* trn(const char16_t* english) { return english; }
constexpr const char16_t* trcn(const char* /*context*/, const char16_t* english) { return english; }

namespace texts {

// The .po file the session uses ("fr", "pt_BR"; empty = English).
const std::string& FileCode();
// Share of the language's texts a person has checked (not "fuzzy"), 0-100;
// 100 for English.
int CheckedPercent();
// Share of the texts translated at all, 0-100.
int TranslatedPercent();

}  // namespace texts
