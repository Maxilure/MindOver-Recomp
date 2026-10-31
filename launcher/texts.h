// =============================================================================
// launcher/texts.h -- the launcher's texts in the player's language
// =============================================================================
//
// The launcher shows its texts in the language the GAME's texts use: the
// setting text_language in user/settings.toml (the game's Options ->
// Language, or the launcher's Settings tab). It reads the SAME files as the game
// (languages/<code>.po, src/language/texts.h; tools/translations.py extracts
// the launcher's texts too), but on its own: the launcher has no game code.
//
//     trs("Play")                        the English text = the key, UTF-8
//     trsn("Play")                       marks a text in a static table
//     trsc("difficulty", "Normal")       with a context (one English word, two meanings)
//     trf(trs("{0} saves"), {"3"})       fills {0} {1}... after translating
//
// Where the .po file is, first found wins:
//   <game folder>/user/languages/<code>.po   a translator's own copy
//   the game's folder (program/languages/, next to the built game)
//   <source>/languages/                      before the game is built
// English (US/UK) = the code's texts. Read once, at the first trs() call
// after Init(); a language change shows after the launcher restarts (the
// game works the same way).
//
// Not translated (like the game's): the logs, and the "All settings" list's
// descriptions (the game's raw setting documentation, many from the SDK).
// =============================================================================
#pragma once

#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>

namespace texts {

// Before the first trs(): the folders to look in (folders.h) and the
// player's settings file. Without it, trs() returns the English.
void Init(const std::filesystem::path& user_folder, const std::filesystem::path& game_folder,
          const std::filesystem::path& source_folder);

// The text_language in use ("de"; empty = English).
const std::string& Code();

}  // namespace texts

// The English text in the launcher's language; valid for the whole run.
const char* trs(const char* english);
// The same with a CONTEXT, for a word with several meanings ("Normal": an
// outfit in the game, a difficulty here).
const char* trsc(const char* context, const char* english);
constexpr const char* trsn(const char* english) { return english; }

// {0}, {1}, ... in `text` replaced by the arguments.
std::string trf(std::string_view text, std::initializer_list<std::string_view> args);
