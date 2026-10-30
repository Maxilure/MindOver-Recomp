// =============================================================================
// language/language.cpp -- text and voice language from the settings
// (findings/49; the overview is in language.h)
// =============================================================================
//
// The hooks (1-3 at boot, 4-5 per movie):
//   1. sub_82262980 REPLACED: the game's "which language letter?" (console
//      language + country on the 360) answers the text_language setting.
//      Its callers: the boot mount (sub_82279E30, picks the voice archive by
//      the letter) and the front end's start (sub_82260048 -> sub_82262AD8,
//      which stores the letter, tells the engine's language listeners, and
//      names the sound folder).
//   2. sub_82279D48 WRAPPED (mount one more .rcf): a voice archive's name is
//      swapped for the voice_language one. The boot mount is its only caller
//      with a voice archive (the others mount default / movie archives).
//   3. Mid-function hook LanguageVoiceFolder at 0x82262C04 (manifest): where
//      sub_82262AD8's switch on the letter meets with r4 = the folder name
//      string ("english", table at 0x8203A1D8), r4 = the voice_language one.
//   4./5. Movies: their dialogue track follows the voice language too
//      (hook MovieDialogueLanguage, 0x822C433C) + a log line per movie.
// Both choices are made once, at the first question, and kept: the game
// keeps only the chosen strings in memory and loads its in-game package by
// the letter, so a change while running would need a reload of everything.
// =============================================================================
#include "language.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>

#include "../guest_memory.h"
#include "../pddi/intercept.h"

REXCVAR_DEFINE_STRING(text_language, "en-us", "CrashMoM",
                      "Language of the game's text: en-us (default), en-gb, fr, fr-ca, de, it, es, nl, da, "
                      "fi, no, sv, ru, pt-br (the port's own texts only; the game's stay English). Takes effect at the next start");
REXCVAR_DEFINE_STRING(voice_language, "auto", "CrashMoM",
                      "Language of the voices: auto (the text language's own, else English), "
                      "english, french, german, italian, spanish, dutch, danish, finnish, "
                      "norwegian, swedish, russian (needs russian.rcf). Takes effect at the next start");

extern "C" REX_FUNC(__imp__sub_82279D48);  // mount one more .rcf archive (r3 its name)

namespace language {
namespace {

// The 13 real text languages of the banks (+ pt-br, last) (letters: sub_82262980 and the
// bank sub-chunk names, findings/49 s.1). Order = the Options row's.
const std::vector<Text> kTexts = {
    {"en-us", 'U', u"English (US)", "english"},
    {"en-gb", 'E', u"English (UK)", "english"},
    {"fr", 'F', u"Français", "french"},
    {"fr-ca", 'Z', u"Français (Canada)", "french"},
    {"de", 'G', u"Deutsch", "german"},
    {"it", 'I', u"Italiano", "italian"},
    {"es", 'S', u"Español", "spanish"},
    {"nl", 'D', u"Nederlands", "dutch"},
    {"da", 'A', u"Dansk", "danish"},
    {"fi", 'H', u"Suomi", "finnish"},
    {"no", 'N', u"Norsk", "norwegian"},
    {"sv", 'W', u"Svenska", "swedish"},
    {"ru", 'R', u"Русский", "russian"},
    // Not a disc language: the game's own text stays English (US) (letter U),
    // only the port's texts are Brazilian Portuguese (texts.h, languages/pt_BR.po).
    {"pt-br", 'U', u"Português (Brasil)", "english"},
};

// The voices: folder name (= setting value), archive. The game's own pairs
// (its tables at 0x8203A1D8 and 0x8203AC78 / 0x8203BED4): only Norwegian's
// archive is named differently from its folder.
const std::vector<Voice> kVoices = {
    {"english", "english.rcf", u"English", 'E'},
    {"french", "french.rcf", u"Français", 'F'},
    {"german", "german.rcf", u"Deutsch", 'G'},
    {"italian", "italian.rcf", u"Italiano", 'I'},
    {"spanish", "spanish.rcf", u"Español", 'S'},
    {"dutch", "dutch.rcf", u"Nederlands", 'D'},
    {"danish", "danish.rcf", u"Dansk", 'A'},
    {"finnish", "finnish.rcf", u"Suomi", 'H'},
    {"norwegian", "norway.rcf", u"Norsk", 'N'},
    {"swedish", "swedish.rcf", u"Svenska", 'W'},
    {"russian", "russian.rcf", u"Русский", 'R'},
};
constexpr int kEnglishVoice = 0;

// Where the game keeps each voice's two strings (same order as kVoices):
// the folder name (sub_82262AD8's table) and the archive name (the boot
// mount's). Checked against guest memory before use (Freeze).
constexpr uint32_t kFolderAddress[] = {0x8203A1D8, 0x8203A1E8, 0x8203A1F0, 0x8203A1F8,
                                       0x8203A200, 0x8203A1E0, 0x8203A208, 0x8203A210,
                                       0x8203A218, 0x8203A224, 0x8203A22C};
constexpr uint32_t kArchiveAddress[] = {0x8203AC78, 0x8203BEE0, 0x8203BEEC, 0x8203BEF8,
                                        0x8203BF04, 0x8203BED4, 0x8203AC84, 0x8203AC90,
                                        0x8203AC9C, 0x8203ACA8, 0x8203ACB4};
static_assert(std::size(kFolderAddress) == 11 && std::size(kArchiveAddress) == 11);

// The session's choice, made at the first question (Freeze).
std::once_flag g_frozen;
std::atomic<int> g_text{-1}, g_voice{-1};
// The guest strings check out (else the voice hooks leave the game alone).
std::atomic<bool> g_strings_ok{false};

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

int TextIndex(const char* key) {
  for (size_t i = 0; i < kTexts.size(); ++i) {
    if (std::strcmp(kTexts[i].key, key) == 0) return int(i);
  }
  return -1;
}

int VoiceIndex(const std::string& key) {
  for (size_t i = 0; i < kVoices.size(); ++i) {
    if (key == kVoices[i].key) return int(i);
  }
  return -1;
}

// Resolved choices: what a start right now would use.
int ResolvedText() { return ChosenText(); }
int ResolvedVoice(int text_index) {
  const int chosen = ChosenVoice();
  if (chosen >= 0 && Available(kVoices[size_t(chosen)])) return chosen;
  return AutomaticVoice(text_index);
}

std::string GuestString(uint8_t* base, uint32_t address) {
  std::string s;
  for (const uint8_t* p = GuestPtr(base, address); *p && s.size() < 96; ++p) s += char(*p);
  return s;
}

// The first question: the session's languages, fixed from here on.
void Freeze(uint8_t* base) {
  std::call_once(g_frozen, [base] {
    const int text = ResolvedText();
    int voice = ResolvedVoice(text);
    if (ChosenVoice() >= 0 && voice != ChosenVoice()) {
      REXLOG_WARN("Language: the {} voices aren't in the game folder ({} missing): {} voices instead",
                  kVoices[size_t(ChosenVoice())].key, kVoices[size_t(ChosenVoice())].archive,
                  kVoices[size_t(voice)].key);
    }
    // The game's own strings where we expect them? (A different build of
    // the game would have them elsewhere: then its own choice stands.)
    bool ok = true;
    for (size_t i = 0; i < kVoices.size(); ++i) {
      ok &= GuestString(base, kFolderAddress[i]) == kVoices[i].key;
      ok &= GuestString(base, kArchiveAddress[i]) == kVoices[i].archive;
    }
    g_strings_ok = ok;
    if (!ok) {
      REXLOG_WARN("Language: the game's voice names aren't where expected: voices follow the text");
      voice = AutomaticVoice(text);
    }
    g_text = text;
    g_voice = voice;
    REXLOG_INFO("Language: text {} ('{}', setting {}), voices {} ({}, setting {})",
                kTexts[size_t(text)].key, kTexts[size_t(text)].letter,
                std::string(REXCVAR_GET(text_language)), kVoices[size_t(voice)].key,
                kVoices[size_t(voice)].archive, std::string(REXCVAR_GET(voice_language)));
  });
}

}  // namespace

const std::vector<Text>& TextLanguages() { return kTexts; }
const std::vector<Voice>& VoiceLanguages() { return kVoices; }

bool Available(const Voice& voice) {
  auto* runtime = rex::Runtime::instance();
  if (!runtime) return std::strcmp(voice.key, "english") == 0;
  std::error_code ec;
  return std::filesystem::is_regular_file(
      std::filesystem::path(runtime->game_data_root()) / voice.archive, ec);
}

int ChosenText() {
  // English (US) by default, and for a value the list doesn't know.
  const int chosen = TextIndex(Lower(std::string(REXCVAR_GET(text_language))).c_str());
  return chosen >= 0 ? chosen : TextIndex("en-us");
}
int ChosenVoice() { return VoiceIndex(Lower(std::string(REXCVAR_GET(voice_language)))); }

int AutomaticVoice(int text_index) {
  if (text_index < 0 || text_index >= int(kTexts.size())) return kEnglishVoice;
  const int own = VoiceIndex(kTexts[size_t(text_index)].voice);
  return own >= 0 && Available(kVoices[size_t(own)]) ? own : kEnglishVoice;
}

int RunningText() {
  const int t = g_text.load();
  return t >= 0 ? t : ResolvedText();
}
int RunningVoice() {
  const int v = g_voice.load();
  return v >= 0 ? v : ResolvedVoice(RunningText());
}

bool RestartNeeded() {
  if (g_text.load() < 0) return false;  // nothing loaded yet
  const int text = ResolvedText();
  return text != g_text.load() || ResolvedVoice(text) != g_voice.load();
}

}  // namespace language

// ---------------------------------------------------------------------------
// The hooks (see the top of this file)
// ---------------------------------------------------------------------------

// 1. "Which language letter?": the setting's, fixed for the session.
extern "C" REX_FUNC(sub_82262980) {
  language::Freeze(base);
  ctx.r3.u64 = uint64_t(uint8_t(language::TextLanguages()[size_t(language::RunningText())].letter));
}

// 2. Mount one more archive: a voice archive becomes the chosen one.
extern "C" REX_FUNC(sub_82279D48) {
  if (language::g_strings_ok.load() && ctx.r3.u32) {
    const std::string name = language::Lower(language::GuestString(base, ctx.r3.u32));
    const auto& voices = language::VoiceLanguages();
    for (size_t i = 0; i < voices.size(); ++i) {
      if (name != voices[i].archive) continue;
      const int running = language::RunningVoice();
      if (int(i) != running) {
        REXLOG_INFO("Language: mounting {} instead of {}", voices[size_t(running)].archive, name);
        ctx.r3.u64 = language::kArchiveAddress[size_t(running)];
      }
      break;
    }
  }
  __imp__sub_82279D48(ctx, base);
}

// 3. The sound folder name (sub_82262AD8, r4 = "english" / "french" / ...).
void LanguageVoiceFolder(PPCRegister& r4) {
  if (!language::g_strings_ok.load() || language::g_voice.load() < 0) return;
  r4.u64 = language::kFolderAddress[size_t(language::g_voice.load())];
}

// 5. Movies: dialogue in the VOICE language. Every Bink movie has 14 audio
// tracks: 0 / 2 / 3 surround (front, LFE, back), 1 English dialogue, 4-13
// the other dialogue languages (French 4, Danish 5, Dutch 6, German 7,
// Italian 8, Finnish 9, Norwegian 10, Spanish 11, Swedish 12, Russian 13;
// a stereo-output list too). The movie start sub_822C42A0 picks them with a
// switch on the front end's letter (+0x3C, loaded into r7 at 0x822C4338 and
// kept in r7 through both switches): the hook at 0x822C433C (manifest
// MovieDialogueLanguage) gives it the voice language's letter instead.
// Found 2026-10-24 by logging the track list at PlayerBink's open
// (sub_82428E58 -> BinkSetSoundTrack count +0xD8, ids +0xDC): English
// 0 1 2 3, French 0 4 2 3, German 0 7 2 3.
void MovieDialogueLanguage(PPCRegister& r7) {
  const int voice = language::g_voice.load();
  if (!language::g_strings_ok.load() || voice < 0) return;
  r7.u64 = uint64_t(uint8_t(language::VoiceLanguages()[size_t(voice)].letter));
}

// 4. Movie log: one line per movie with the tracks Bink was given (PlayerBink
// open, vtable slot 6 sub_82428E58; the renderer interceptor already wraps
// it, pddi/functions.inc "PlayerBink::v6", so this is a handler there).
// How the dialogue track was found (5. above); stays as a check in reports.
namespace language {
namespace {
void OnMovieOpen(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t player = ctx.r3.u32;
  const std::string name = ctx.r4.u32 ? GuestString(base, ctx.r4.u32) : std::string("?");
  const uint32_t lr = uint32_t(ctx.lr);
  original(ctx, base);
  // What Bink was told (sub_824BC9A8 = BinkSetSoundTrack(count +0xD8, ids +0xDC)).
  auto word = [&](uint32_t a) {
    const uint8_t* p = GuestPtr(base, a);
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  };
  const uint32_t count = word(player + 0xD8);
  std::string ids;
  for (uint32_t i = 0; i < count && i < 16; ++i) ids += " " + std::to_string(word(player + 0xDC + 4 * i));
  REXLOG_INFO("Language: movie {} opened, {} track(s):{} (from 0x{:08X})", name, count, ids, lr);
}
}  // namespace
void Install() { pddi::SetHandler(pddi::kFn_82428E58, &OnMovieOpen); }
}  // namespace language
