// =============================================================================
// language/game_text_banks.h -- the GAME's own text banks, read by the port
// =============================================================================
//
// WHY: some texts the port shows are the game's own words in the player's
// language: the achievements' names and descriptions (achievements.cpp), and
// the save messages it rewords (saves/save_screen_waits.cpp). Reading them
// from the player's disc keeps every language right without shipping any of
// the game's text (the repo's rule 1).
//
// THE FORMAT (findings/30, findings/49; tools/text_banks.py reads the same):
// a package (.p3d in default.rcf) is Pure3D chunks. Chunk 0x1800D = a text
// bank ("ingame", "frontend"...): its name, then one 0x1800E child per
// language. A child = its name, the language LETTER (findings/49 s.1: U
// English US, E UK, F French...), u32 count, u32 modulus, u32 size, `count`
// keys (u32), `count` offsets (u32, bytes into the string area), then the
// strings: UTF-16 LITTLE-endian, each ended by a 0 character. A string's key
// is the same in every language of a bank.
//
// WHICH PACKAGE: package\c1e387c7.p3d holds the "ingame" bank in EVERY
// language (each level group's in-game package holds one language's copy),
// including the achievement list the other consoles show in their pause
// menus (findings/51 s.8): read once, at the first question.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../data/data_patcher.h"

namespace game_text_banks {

// One language of one bank inside a package's bytes.
struct BankLanguage {
  std::string bank;        // "ingame"
  char letter = 0;         // 'U', 'G'...
  uint32_t count = 0;      // strings
  size_t keys_at = 0;      // byte offset of the key table
  size_t offsets_at = 0;   // byte offset of the offset table
  size_t area_at = 0;      // byte offset of the string area
  size_t area_end = 0;     // end of the string area (the chunk's end)

  uint32_t KeyAt(const data_patcher::Bytes& d, uint32_t i) const;
  // String `i`: its first byte and the byte of its ending 0 character.
  void SpanOf(const data_patcher::Bytes& d, uint32_t i, size_t* begin, size_t* end) const;
  std::u16string StringAt(const data_patcher::Bytes& d, uint32_t i) const;
};

// Every bank language in a package file's bytes (empty = none, or not Pure3D).
std::vector<BankLanguage> Find(const data_patcher::Bytes& file);

// The running text language's letter (language::TextLanguages()).
char RunningLetter();

// The game's text in a language it doesn't have (texts::GameTranslation():
// Brazilian Portuguese): a data patch on every menu package rebuilds the
// running language's copy of each bank with the translated strings (keys
// kept, offsets and the string area made again, chunk sizes follow).
// Untranslated strings stay as they are. From CrashMomApp at start-up.
void Install();

// The "ingame" bank's string `key` in language `letter` (0 = the running
// one); empty if the disc has none. Reads package\c1e387c7.p3d at the first
// call (thread-safe).
std::u16string InGame(uint32_t key, char letter = 0);

}  // namespace game_text_banks
