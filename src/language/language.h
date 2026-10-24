// =============================================================================
// language/language.h -- which of the disc's languages the game uses: TEXT
// and VOICES chosen separately (findings/49)
// =============================================================================
//
// The disc keeps a language in two parts:
//   * TEXT: every text bank (menus, HUD, tutorials) holds 14 languages, each
//     marked by one LETTER (E = English UK, U = English US, F = French, ...).
//     The game asks sub_82262980 for the letter once at boot; the letter picks
//     which strings the bank loader keeps (sub_823B4A20) and which in-game
//     package loads (Fe_InGame + letter, sub_82266648).
//   * VOICES: one archive per language (english.rcf, french.rcf, ...), mounted
//     at boot by sub_82279E30 -> sub_82279D48, and a folder name for the sound
//     system ("english", sub_82262AD8 at 0x82262C04) that the voice paths use
//     (sound\rsd\<folder>\...).
// On the 360 one console setting decides both. Here two settings do:
// text_language and voice_language (user/settings.toml, Options -> Language),
// so any text can go with any voices ("English voices, German text").
//
// Both are read ONCE, the first time the game asks, and stay for the session:
// the game loads only the chosen strings, so a change needs a restart (the
// Options rows say so and offer "Restart Now": app_restart.h).
// =============================================================================
#pragma once

#include <string>
#include <vector>

namespace language {

// A text language: the setting's value, the game's letter, its name as its
// own speakers write it (the Options row shows that).
struct Text {
  const char* key;        // text_language value ("de")
  char letter;            // the game's language letter ('G')
  const char16_t* name;   // "Deutsch"
  const char* voice;      // the voices that go with it ("german")
};

// A voice language: the setting's value = the sound folder name ("german"),
// its archive ("german.rcf" is NOT always the folder: norway.rcf), its name.
struct Voice {
  const char* key;        // voice_language value = the sound folder ("norwegian")
  const char* archive;    // the disc archive ("norway.rcf")
  const char16_t* name;   // "Norsk"
  char letter;            // its text letter: the movies pick their dialogue track by it
};

// Every text language of the disc's banks (the 14 letters minus X, Radical's
// test pseudo-language), in the Options row's order.
const std::vector<Text>& TextLanguages();
// Every voice language the game knows; Available() = its archive is in the
// game folder (a disc may lack some; russian.rcf is on none we've seen).
const std::vector<Voice>& VoiceLanguages();
bool Available(const Voice& voice);

// What this session runs with (fixed at the first question; before that,
// what the settings say now). Indexes into the lists above.
int RunningText();
int RunningVoice();
// What the settings say now.
int ChosenText();   // index into TextLanguages() (English (US) by default)
int ChosenVoice();  // index into VoiceLanguages(), -1 = "Same as Text"
// The voices "Same as Text" stands for: the text's own, if on the disc, else English.
int AutomaticVoice(int text_index);
// The settings name a language other than the running one = a restart
// would change something.
bool RestartNeeded();

// The movie hook (pddi handler), from CrashMomApp at start-up.
void Install();

}  // namespace language
