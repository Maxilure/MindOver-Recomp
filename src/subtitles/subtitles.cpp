// =============================================================================
// subtitles/subtitles.cpp -- subtitles for the game's spoken lines (findings/63)
// =============================================================================
//
// See subtitles.h for what the player sees and where the timing comes from.
// This file holds:
//   1. the settings (REXCVARs, kept in user/settings.toml like the others);
//   2. the text file (user/subtitles/<code>.tsv, else the shipped
//      subtitles/<code>.tsv: next to the exe, or the repository's own
//      subtitles/ when the exe was built in the source tree; read at
//      start-up and AGAIN whenever one of the files changes on disk, so a
//      line saved in tools/subtitle_editor shows the next time it plays):
//      whole lines of a sound, and timed lines (cues) inside a Bink movie or
//      inside a sound that holds several lines (an in-engine cutscene track);
//   3. the lines on screen: a short list fed by voice_trace.cpp (audio
//      thread) and read by the drawing (UI thread), under one mutex;
//   4. the drawing: an ImGui "dialog" with no window, like fps_overlay.cpp:
//      textured quads of the game font's letters on ImGui's foreground draw
//      list, which the SDK paints over the guest picture in either renderer.
//
// THE PICTURE'S RECTANGLE: the game's 16:9 picture fitted into the window
// (the presenter letterboxes it the same way). The subtitle is centred in
// it, so a wider window (ultrawide, the widescreen option) keeps it in the
// middle, where the eye is, and never under the HUD's corners.
// =============================================================================

#include "subtitles/subtitles.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>

#include "game_folder.h"
#include "achievements/achievements.h"
#include "language/language.h"
#include "language/texts.h"
#include "options/options_menu.h"
#include "ui/game_font.h"

REXCVAR_DEFINE_INT32(subtitles, 0, "CrashMoM",
                     "Subtitles for spoken lines (cutscenes, characters, Aku Aku): 0 off, 1 on; "
                     "takes effect at once (findings/63)")
    .range(0, 1);
REXCVAR_DEFINE_INT32(subtitles_size, 1, "CrashMoM", "Subtitle size: 0 small, 1 medium, 2 large")
    .range(0, 2);
REXCVAR_DEFINE_BOOL(subtitles_background, true, "CrashMoM",
                    "A dark box behind the subtitles (off = none, whatever its opacity)");
REXCVAR_DEFINE_INT32(subtitles_background_opacity, 70, "CrashMoM",
                     "How opaque the box behind the subtitles is, 0-100 % (the text stays solid)")
    .range(0, 100);
REXCVAR_DEFINE_BOOL(subtitles_speakers, true, "CrashMoM",
                    "The speaker's name in front of a subtitle, when it is known");
// THE SPEAKERS' COLOURS (findings/63 s.26): each character's name in its own
// colour, from subtitles/speakers.tsv; off = every name in the menus' yellow
// (kSpeakerColour), as before. Read every frame: the Options row works at once.
REXCVAR_DEFINE_BOOL(subtitles_speaker_colours, true, "CrashMoM",
                    "Each speaker's name in its character's colour (subtitles/speakers.tsv); off = "
                    "every name in yellow");
REXCVAR_DEFINE_INT32(subtitles_max_lines, 2, "CrashMoM",
                     "How many subtitles may stand on screen at once (the newest at the bottom, older "
                     "ones pushed up and dimmed; one more makes the oldest fade out)")
    .range(1, 4);
REXCVAR_DEFINE_INT32(subtitles_linger_ms, 2000, "CrashMoM",
                     "How long a subtitle stays after its line has ended (its sound, or the end time "
                     "its text file gives it), in milliseconds, before it fades out")
    .range(0, 10000);
REXCVAR_DEFINE_BOOL(subtitles_placeholders, false, "CrashMoM",
                    "Developer: where a voice line has no text yet, show its speaker folder and "
                    "sound file name instead (for checking subtitle timing)");
REXCVAR_DEFINE_BOOL(debug_subtitle_clock, false, "CrashMoM",
                    "Developer: the playing movie's name and clock in the picture's top left corner "
                    "(for timing subtitle lines); never part of the subtitles");

namespace subtitles {
namespace {

using Clock = std::chrono::steady_clock;

// THE LINGER (subtitles_linger_ms, default 2 s): every line stays this long
// AFTER its time is up -- after its sound ends, and after the end time its
// text file gives it (a cutscene track's or a movie's timed line) -- so it
// can be read to the end; then it fades out (kFadeOutSeconds below). A newer
// line simply stacks under it meanwhile. A line the game cuts off (a skipped
// cutscene) still goes at once.
double LingerSeconds() { return std::clamp(int(REXCVAR_GET(subtitles_linger_ms)), 0, 10000) / 1000.0; }

// The voices and movies are FOLLOWED (lines queued, movie clock kept) when
// there is anything to show: a text file, or a developer option. Decided at
// start-up (audio_trace builds its sound index only then). Whether lines are
// DRAWN is the `subtitles` flag, read every frame: the Options row works at
// once, and turning it on mid-scene shows the line being spoken.
bool g_on = false;

// The pause menu's decision runs every frame the menu is up
// (achievements_screen.cpp calls NoteMenuFrame): the time of the last one.
std::atomic<int64_t> g_menu_seen_ms{0};
int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// 2. The text file
// ---------------------------------------------------------------------------
// A timed line: from `start` to `end` seconds of its movie / sound.
struct Cue {
  double start = 0, end = 0;
  std::u16string speaker;  // "" = none
  // WHO speaks, for the name's colour: the speaker's ENGLISH name (the
  // en.tsv cue at the same place), so a translated name ("Anciã
  // RhinoRoller") gets the same colour as its English one ("RhinoRoller
  // Elder") without a colour list per language. LinkSpeakers fills it.
  std::u16string speaker_key;
  std::u16string text;
};
// Everything the files say, read in one go. A reload builds a NEW set and
// swaps the pointer under g_mutex (below); readers take the pointer under
// it and keep their copy alive while they use it, so a set is never
// changed once published.
struct Texts {
  std::unordered_map<uint64_t, std::u16string> texts;          // sound ID -> its whole line
  std::unordered_map<uint64_t, std::vector<Cue>> sound_cues;   // sound ID -> its lines
  std::unordered_map<std::string, std::vector<Cue>> movie_cues;  // movie name (lower case) -> its lines
  // speakers.tsv: a speaker's English name -> the colour of its name.
  std::unordered_map<std::u16string, ImU32> colours;
};
std::shared_ptr<const Texts> g_data = std::make_shared<Texts>();  // under g_mutex

// THE SHIPPED FOLDER: subtitles/ next to the exe (the build copies the
// repository's folder there, an install has it in program/). An exe in the
// source tree's BUILD folder (<repo>/out/build/<preset>/, the repo = the
// folder with crash_mom_manifest.toml) reads the repository's subtitles/
// itself instead, so what the subtitle editor saves "to the repository" is
// what the game shows, without building again. Only out/build: an
// installed-style test copy elsewhere under out/ keeps its own files.
std::filesystem::path ShippedFolder() {
  static const std::filesystem::path folder = [] {
    const std::filesystem::path exe = rex::filesystem::GetExecutableFolder();
    const std::filesystem::path build = exe.parent_path();  // out/build
    const std::filesystem::path repo = build.parent_path().parent_path();
    std::error_code ec;
    if (build.filename() == "build" && build.parent_path().filename() == "out" &&
        std::filesystem::is_regular_file(repo / "crash_mom_manifest.toml", ec) &&
        std::filesystem::is_directory(repo / "subtitles", ec)) {
      return repo / "subtitles";
    }
    return exe / "subtitles";
  }();
  return folder;
}
// The player's own copy of a file if there is one, else the shipped one
// (`name` relative to a subtitles folder: "en.tsv", "timing/french.tsv").
std::filesystem::path FileFor(const std::filesystem::path& name) {
  const std::filesystem::path own = game_folder::UserFolder() / "subtitles" / name;
  std::error_code ec;
  return std::filesystem::is_regular_file(own, ec) ? own : ShippedFolder() / name;
}

// UTF-8 -> UTF-16 (the font's characters are all in the BMP).
std::u16string Utf16(std::string_view s) {
  std::u16string out;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp = c;
    size_t n = 1;
    if (c >= 0xF0 && i + 3 < s.size()) { cp = ((c & 7u) << 18) | ((s[i + 1] & 0x3Fu) << 12) | ((s[i + 2] & 0x3Fu) << 6) | (s[i + 3] & 0x3Fu); n = 4; }
    else if (c >= 0xE0 && i + 2 < s.size()) { cp = ((c & 0xFu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu); n = 3; }
    else if (c >= 0xC0 && i + 1 < s.size()) { cp = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3Fu); n = 2; }
    i += n;
    if (cp < 0x10000) out += char16_t(cp);
  }
  return out;
}

std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// A movie's key: its file name without folder and ".bik", lower case
// ("d:\movies\Crash_02_51.bik" -> "crash_02_51").
std::string MovieKey(std::string_view name) {
  if (const size_t slash = name.find_last_of("\\/"); slash != std::string_view::npos) {
    name.remove_prefix(slash + 1);
  }
  if (name.size() > 4 && Lower(name.substr(name.size() - 4)) == ".bik") name.remove_suffix(4);
  return Lower(name);
}

// 16 hex digits -> a sound ID.
bool ParseId(std::string_view s, uint64_t* id) {
  if (s.size() != 16) return false;
  uint64_t v = 0;
  for (char c : s) {
    const int d = c >= '0' && c <= '9' ? c - '0'
                  : c >= 'a' && c <= 'f' ? c - 'a' + 10
                  : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                         : -1;
    if (d < 0) return false;
    v = (v << 4) | uint64_t(d);
  }
  *id = v;
  return true;
}

// "mm:ss.cc" or "ss.cc" -> seconds (-1 if it isn't a time).
double ParseTime(std::string_view s) {
  double minutes = 0;
  if (const size_t colon = s.find(':'); colon != std::string_view::npos) {
    try { minutes = std::stod(std::string(s.substr(0, colon))); } catch (...) { return -1; }
    s.remove_prefix(colon + 1);
  }
  try { return minutes * 60.0 + std::stod(std::string(s)); } catch (...) { return -1; }
}

// "\n" written as two characters breaks the line.
std::u16string TextOf(std::string text) {
  for (size_t p = text.find("\\n"); p != std::string::npos; p = text.find("\\n", p + 1)) {
    text.replace(p, 2, "\n");
  }
  return Utf16(text);
}

// Reads `<user>/subtitles/<code>.tsv`, or, when the player has none, the
// shipped one (ShippedFolder: next to the exe, or the repository's in a
// source-tree build) into `out`; false if there is neither.
// A line is TAB-separated, one of:
//   <sound ID>  <text>                               a sound's whole line
//   <key>  <start>  <end>  <speaker>  <text>         a timed line (cue)
// where <key> = a sound ID (a sound holding several lines) or a movie's file
// name (Crash_02_51), the times are seconds from the start of that sound /
// movie ("mm:ss.cc" or "ss.cc"), <speaker> may be empty. # starts a comment.
bool ReadTexts(const std::string& code, Texts& out) {
  // The player's own file wins whole (not line by line): it is usually the
  // shipped one copied and edited.
  const std::filesystem::path path = FileFor(code + ".tsv");
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::string line;
  size_t whole = 0, cues = 0, skipped = 0;
  // A key's timed lines in THIS file replace those an earlier file gave it
  // (the language's file over English), so its first line here clears them.
  std::unordered_map<std::string, bool> movie_seen;
  std::unordered_map<uint64_t, bool> sound_seen;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();  // files written on Windows
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> f;
    for (size_t p = 0;;) {
      const size_t tab = line.find('\t', p);
      f.push_back(line.substr(p, tab == std::string::npos ? std::string::npos : tab - p));
      if (tab == std::string::npos) break;
      p = tab + 1;
    }
    uint64_t id = 0;
    const bool is_id = ParseId(f[0], &id);
    if (f.size() == 2 && is_id) {
      out.texts[id] = TextOf(f[1]);
      ++whole;
    } else if (f.size() == 5) {
      Cue cue;
      cue.start = ParseTime(f[1]);
      cue.end = ParseTime(f[2]);
      cue.speaker = Utf16(f[3]);
      cue.speaker_key = cue.speaker;  // the English name: LinkSpeakers fixes translated ones
      cue.text = TextOf(f[4]);
      if (cue.start < 0 || cue.end <= cue.start) { ++skipped; continue; }
      if (is_id) {
        if (!sound_seen[id]) { out.sound_cues[id].clear(); sound_seen[id] = true; }
        out.sound_cues[id].push_back(std::move(cue));
      } else {
        const std::string key = MovieKey(f[0]);
        if (!movie_seen[key]) { out.movie_cues[key].clear(); movie_seen[key] = true; }
        out.movie_cues[key].push_back(std::move(cue));
      }
      ++cues;
    } else {
      ++skipped;
    }
  }
  REXLOG_INFO("Subtitles: {} whole line(s), {} timed line(s) from {}{}", whole, cues,
              path.string(), skipped ? fmt::format(" ({} line(s) not understood)", skipped) : "");
  return true;
}

// THE VOICE LANGUAGE'S OWN TIMES (findings/63 s.24): the text files' times
// were made on the ENGLISH voices; a French line is not as long, nor are its
// pauses in the same places. `subtitles/timing/<voice>.tsv` (voice = the
// running voice language's key: french, german, norwegian...; the player's
// own <user>/subtitles/timing/<voice>.tsv wins whole over the shipped one)
// gives a sound's or a movie's pieces their times for that voice:
//   <key>  <piece number, from 1>  <start>  <end>
// It only changes WHEN pieces show, never which pieces there are: a key's
// times are used only if they cover exactly the pieces the text gave that
// key (same count), else that key keeps the text file's times (a text split
// later, a translation cut differently). Made with tools/subtitle_editor.
// The running voice language's key ("english", "french"...), "" if none.
std::string VoiceKey() {
  const auto& voices = language::VoiceLanguages();
  const int running = language::RunningVoice();
  return running >= 0 && running < int(voices.size()) ? voices[size_t(running)].key : std::string();
}

void ApplyVoiceTimes(Texts& out) {
  const std::string voice = VoiceKey();
  if (voice.empty()) return;
  const std::filesystem::path path = FileFor(std::filesystem::path("timing") / (voice + ".tsv"));
  std::ifstream in(path, std::ios::binary);
  if (!in) return;
  // key (a sound ID's 16 hex digits in lower case, or a movie's key) ->
  // piece number - 1 -> (start, end); (-1, -1) = not given.
  std::unordered_map<std::string, std::vector<std::pair<double, double>>> times;
  std::string line;
  size_t skipped = 0;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> f;
    for (size_t p = 0;;) {
      const size_t tab = line.find('\t', p);
      f.push_back(line.substr(p, tab == std::string::npos ? std::string::npos : tab - p));
      if (tab == std::string::npos) break;
      p = tab + 1;
    }
    const int number = f.size() == 4 ? std::atoi(f[1].c_str()) : 0;
    const double start = f.size() == 4 ? ParseTime(f[2]) : -1, end = f.size() == 4 ? ParseTime(f[3]) : -1;
    if (number < 1 || number > 500 || start < 0 || end <= start) {
      ++skipped;
      continue;
    }
    uint64_t id = 0;
    const std::string key = ParseId(f[0], &id) ? fmt::format("{:016x}", id) : MovieKey(f[0]);
    auto& list = times[key];
    if (list.size() < size_t(number)) list.resize(size_t(number), {-1.0, -1.0});
    list[size_t(number - 1)] = {start, end};
  }
  // Onto the cues, key by key, only where every piece has its time.
  size_t used = 0, not_matching = 0;
  const auto apply = [&](const std::string& key, std::vector<Cue>& cues) {
    const auto it = times.find(key);
    if (it == times.end()) return;
    const auto& list = it->second;
    const bool complete = list.size() == cues.size() &&
                          std::all_of(list.begin(), list.end(), [](const auto& t) { return t.first >= 0; });
    if (!complete) {
      ++not_matching;
      return;
    }
    for (size_t i = 0; i < cues.size(); ++i) {
      cues[i].start = list[i].first;
      cues[i].end = list[i].second;
    }
    ++used;
  };
  for (auto& [id, cues] : out.sound_cues) apply(fmt::format("{:016x}", id), cues);
  for (auto& [key, cues] : out.movie_cues) apply(key, cues);
  REXLOG_INFO("Subtitles: {} voices' own times for {} sound(s) / movie(s) from {}{}{}", voice, used,
              path.string(),
              not_matching ? fmt::format("; {} kept the text's times (pieces differ)", not_matching) : "",
              skipped ? fmt::format("; {} line(s) not understood", skipped) : "");
}

// ---------------------------------------------------------------------------
// Which sounds get a subtitle, and who speaks (findings/63 s.13)
// ---------------------------------------------------------------------------
// The designer's names look like character\<language>\<folder>\<file>.wav.
struct Kind {
  bool in_scope = false;
  std::string folder;  // the speaker folder ("akuaku", "coco", ...)
  std::string file;    // the file name without ".wav"
};

Kind KindOf(const std::string& name) {
  Kind k;
  std::vector<std::string_view> parts;
  std::string_view rest(name);
  for (size_t p; (p = rest.find('\\')) != std::string_view::npos; rest.remove_prefix(p + 1)) {
    parts.push_back(rest.substr(0, p));
  }
  parts.push_back(rest);
  if (parts.size() < 3 || parts[0] != "character") return k;  // not a voice archive sound
  k.folder = std::string(parts[2]);
  std::string_view file = parts.back();
  if (file.ends_with(".wav")) file.remove_suffix(4);
  k.file = std::string(file);
  const bool cutscene = k.folder == "nis";              // in-engine cutscene tracks
  const bool aku = k.folder == "akuaku";                // Aku Aku
  const bool tutorial = k.folder == "tutorial";         // the tutorial voice
  const bool story = file.starts_with("ingame");        // NPC / story lines
  const bool loudspeaker = file.starts_with("loudspeaker");  // Cortex's announcements
  k.in_scope = cutscene || aku || tutorial || story || loudspeaker;
  return k;
}

// The speaker's name, only where the folder names the character for sure
// (official names; the NPCs' are still (name TBD), findings/63 s.13.2, so
// they get none; nor do the tutorial voice and the cutscene tracks, whose
// lines can have several speakers).
std::u16string SpeakerOf(const Kind& k) {
  static const std::unordered_map<std::string, std::u16string> kNames = {
      {"akuaku", u"Aku Aku"}, {"coco", u"Coco"},   {"crunch", u"Crunch"},
      {"cortex", u"Cortex"},  {"ukauka", u"Uka Uka"}, {"nina", u"Nina"},
  };
  const auto it = kNames.find(k.folder);
  return it == kNames.end() ? std::u16string() : it->second;
}

// ---------------------------------------------------------------------------
// 3. The lines on screen
// ---------------------------------------------------------------------------
struct Line {
  uint64_t id = 0;
  // Which subtitle this is, for the drawing's fades and slides (section 4):
  // every queued line gets its own number (g_next_uid); a movie's timed line
  // is named by the movie and its place in the file (MovieUid). 0 = a sample.
  uint64_t uid = 0;
  std::u16string speaker;
  // The name's colour (speakers.tsv via ColourOf), 0 = the yellow. Drawn
  // only while subtitles_speaker_colours is on (read at drawing time).
  ImU32 speaker_colour = 0;
  std::u16string text;
  Clock::time_point start;
  Clock::time_point end;  // moved earlier by a cut
};
std::mutex g_mutex;
std::vector<Line> g_lines;  // oldest first; a cue's line may start in the future
uint64_t g_next_uid = 1;    // under g_mutex; the next queued line's uid

// A movie's n-th timed line's uid: the movie key's FNV-1a hash with the
// number mixed in, top bit set (never equal to a queued line's counter).
uint64_t MovieUid(const std::string& movie, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (char c : movie) h = (h ^ uint8_t(c)) * 1099511628211ull;
  h = (h ^ uint64_t(n)) * 1099511628211ull;
  return h | (1ull << 63);
}

// The Bink movie playing now (voice_trace.cpp's frame-end watcher, main
// thread): its key and its clock (seconds from its first frame).
std::string g_movie;  // "" = none
double g_movie_time = 0;

Clock::time_point After(Clock::time_point t, double seconds) {
  return t + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
}

}  // namespace

bool On() { return g_on; }

void NoteMenuFrame() { g_menu_seen_ms.store(NowMs(), std::memory_order_relaxed); }

namespace {

// The text language's code ("pt-br"): the language the game runs in
// (Options -> Language -> Text, language.h RunningText). It is fixed for
// the session (a change there restarts the game), so the subtitles follow
// it from the next start; no separate subtitle language exists.
std::string TextCode() {
  const auto& langs = language::TextLanguages();
  const int running = language::RunningText();
  return running >= 0 && running < int(langs.size()) ? langs[size_t(running)].key : "en-us";
}

// THE COLOURS FILE (findings/63 s.26): subtitles/speakers.tsv (the
// player's own <user>/subtitles/speakers.tsv wins whole, as for the texts),
// one line per character:
//   <speaker's English name, as in en.tsv>  TAB  #RRGGBB
// A name not in it keeps the yellow; so does every name with
// subtitles_speaker_colours off. One list for every language: a cue's
// colour is looked up by its English name (Cue::speaker_key).
void ReadColours(Texts& out) {
  const std::filesystem::path path = FileFor("speakers.tsv");
  std::ifstream in(path, std::ios::binary);
  if (!in) return;
  std::string line;
  size_t skipped = 0;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const size_t tab = line.find('\t');
    if (tab == std::string::npos) { ++skipped; continue; }
    std::string hex = line.substr(tab + 1);
    if (!hex.empty() && hex[0] == '#') hex.erase(0, 1);
    uint32_t rgb = 0;
    bool ok = hex.size() == 6;
    for (char c : hex) {
      const int d = c >= '0' && c <= '9' ? c - '0'
                    : c >= 'a' && c <= 'f' ? c - 'a' + 10
                    : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                           : -1;
      if (d < 0) { ok = false; break; }
      rgb = (rgb << 4) | uint32_t(d);
    }
    if (!ok) { ++skipped; continue; }
    out.colours[Utf16(line.substr(0, tab))] =
        IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 0xFF);
  }
  REXLOG_INFO("Subtitles: {} speaker colour(s) from {}{}", out.colours.size(), path.string(),
              skipped ? fmt::format(" ({} line(s) not understood)", skipped) : "");
}

// The English speakers of every key's cues, in order, taken right after
// en.tsv is read (the language's file then REPLACES a key's cues).
struct EnglishSpeakers {
  std::unordered_map<uint64_t, std::vector<std::u16string>> sounds;
  std::unordered_map<std::string, std::vector<std::u16string>> movies;
};
EnglishSpeakers SpeakersOf(const Texts& data) {
  EnglishSpeakers en;
  for (const auto& [id, cues] : data.sound_cues) {
    for (const Cue& c : cues) en.sounds[id].push_back(c.speaker);
  }
  for (const auto& [key, cues] : data.movie_cues) {
    for (const Cue& c : cues) en.movies[key].push_back(c.speaker);
  }
  return en;
}

// After the language's file: every translated speaker name -> its English
// name, learnt from the cues at the same place of the same key in both
// files (the n-th cue of a key in pt-br.tsv against the n-th in en.tsv:
// a translation keeps the pieces in order). The first pair seen wins; a
// name never paired keeps itself (it may be English already). Then every
// cue's speaker_key is set from that map, so the colour follows the
// character even where a translation cut a key into other pieces.
void LinkSpeakers(const EnglishSpeakers& en, Texts& data) {
  std::unordered_map<std::u16string, std::u16string> english_of;
  auto pair = [&](const std::vector<Cue>& cues, const std::vector<std::u16string>& english) {
    for (size_t n = 0; n < cues.size() && n < english.size(); ++n) {
      if (!cues[n].speaker.empty() && !english[n].empty()) english_of.emplace(cues[n].speaker, english[n]);
    }
  };
  for (const auto& [id, cues] : data.sound_cues) {
    if (const auto it = en.sounds.find(id); it != en.sounds.end()) pair(cues, it->second);
  }
  for (const auto& [key, cues] : data.movie_cues) {
    if (const auto it = en.movies.find(key); it != en.movies.end()) pair(cues, it->second);
  }
  auto link = [&](std::vector<Cue>& cues) {
    for (Cue& c : cues) {
      const auto it = english_of.find(c.speaker);
      c.speaker_key = it != english_of.end() ? it->second : c.speaker;
    }
  };
  for (auto& [id, cues] : data.sound_cues) link(cues);
  for (auto& [key, cues] : data.movie_cues) link(cues);
}

// A speaker's name colour from the colours file, 0 = none (the yellow).
// First by the speaker's English name; then, for a voice line, by its
// SPEAKER FOLDER: a "folder:<name>" line of speakers.tsv, the folder's
// trailing digits dropped, so labrat1 ... labrat6 (the six voices of one
// enemy kind) share "folder:labrat". The common enemies' lines (combat
// barks, findings/63 s.13) have no speaker names in the text files yet:
// keyed by folder, their colours hold whatever names they are given later
// (findings/63 s.26).
ImU32 ColourOf(const Texts& data, const std::u16string& speaker_key, std::string_view folder = {}) {
  if (!speaker_key.empty()) {
    if (const auto it = data.colours.find(speaker_key); it != data.colours.end()) return it->second;
  }
  while (!folder.empty() && folder.back() >= '0' && folder.back() <= '9') folder.remove_suffix(1);
  if (folder.empty()) return 0;
  const auto it = data.colours.find(Utf16("folder:" + std::string(folder)));
  return it == data.colours.end() ? 0 : it->second;
}

// Reads every file into a new set and publishes it. `english` / `own` say
// which text files were there.
void Load(bool* english, bool* own) {
  const std::string code = TextCode();
  auto data = std::make_shared<Texts>();
  // English first, then the language's own file over it (its lines replace
  // English ones key by key; what it doesn't have stays English).
  *english = ReadTexts("en", *data);
  const EnglishSpeakers en = SpeakersOf(*data);  // before the language's file replaces them
  *own = code != "en" && ReadTexts(code, *data);
  if (*own) LinkSpeakers(en, *data);
  ReadColours(*data);
  // Then the voice language's own times over theirs (timing/<voice>.tsv).
  ApplyVoiceTimes(*data);
  std::lock_guard<std::mutex> lock(g_mutex);
  g_data = std::move(data);
}

// THE WATCHER: once a second, the files the subtitles come from (both
// places of each: a player's own copy can appear or go too) are compared
// with what was read; any newer, new or gone file = everything read again
// (Load), so a line saved in the subtitle editor shows the next time it
// plays, without restarting. Lines already on screen keep their words.
// A stamp = the file's write time, or "none".
std::vector<std::filesystem::path> WatchedFiles() {
  std::vector<std::filesystem::path> names = {"en.tsv", "speakers.tsv"};
  if (const std::string code = TextCode(); code != "en") names.push_back(code + ".tsv");
  if (const std::string voice = VoiceKey(); !voice.empty()) {
    names.push_back(std::filesystem::path("timing") / (voice + ".tsv"));
  }
  std::vector<std::filesystem::path> files;
  for (const auto& name : names) {
    files.push_back(game_folder::UserFolder() / "subtitles" / name);
    files.push_back(ShippedFolder() / name);
  }
  return files;
}
std::vector<std::filesystem::file_time_type> Stamps(const std::vector<std::filesystem::path>& files) {
  std::vector<std::filesystem::file_time_type> out;
  for (const auto& f : files) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(f, ec);
    out.push_back(ec ? std::filesystem::file_time_type::min() : t);
  }
  return out;
}
void Watch() {
  const std::vector<std::filesystem::path> files = WatchedFiles();
  auto seen = Stamps(files);
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    auto now = Stamps(files);
    if (now == seen) continue;
    // A file being saved (the editor writes a temporary file and renames
    // it; another program may write in place): wait for it to settle.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    now = Stamps(files);
    seen = now;
    bool english = false, own = false;
    Load(&english, &own);
    REXLOG_INFO("Subtitles: reloaded (a subtitle file changed)");
  }
}

}  // namespace

void Start() {
  bool english = false, own = false;
  Load(&english, &own);
  const std::string code = TextCode();
  // A language without its own file shows English (the fallback); with
  // neither, nothing (never an empty box or a sound's name: the placeholders
  // are a developer option).
  if (!english && !own) {
    REXLOG_INFO("Subtitles: no text file in {} or {} ({}.tsv or en.tsv){}",
                (game_folder::UserFolder() / "subtitles").string(), ShippedFolder().string(), code,
                REXCVAR_GET(subtitles_placeholders) ? "; placeholders shown" : "");
  } else if (!own && code != "en" && code != "en-us") {
    REXLOG_INFO("Subtitles: no {}.tsv, English lines shown", code);
  }
  g_on = english || own || REXCVAR_GET(subtitles_placeholders) || REXCVAR_GET(debug_subtitle_clock);
  REXLOG_INFO("Subtitles: {} (shown: {})", g_on ? "following the voices and movies" : "nothing to show",
              REXCVAR_GET(subtitles) ? "on" : "off");
  // Watched only when the voices are followed (decided here, at start-up).
  if (g_on) std::thread(Watch).detach();
}

void OnVoiceStart(uint64_t id, const std::string& name, double seconds) {
  if (!g_on) return;
  const Kind k = KindOf(name);
  if (!k.in_scope) return;
  const Clock::time_point now = Clock::now();
  const bool speakers = REXCVAR_GET(subtitles_speakers);
  std::shared_ptr<const Texts> data;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    data = g_data;
  }
  std::vector<Line> add;
  if (const auto cues = data->sound_cues.find(id); cues != data->sound_cues.end()) {
    // A sound with several lines (an in-engine cutscene track): each at its
    // time inside the sound, never past the sound's end.
    for (const Cue& c : cues->second) {
      Line line;
      line.id = id;
      line.text = c.text;
      if (speakers) {
        line.speaker = c.speaker;
        line.speaker_colour = ColourOf(*data, c.speaker_key, k.folder);
      }
      line.start = After(now, c.start);
      line.end = After(now, std::min(c.end, seconds) + LingerSeconds());
      if (line.end > line.start) add.push_back(std::move(line));
    }
  } else {
    Line line;
    line.id = id;
    if (const auto it = data->texts.find(id); it != data->texts.end()) {
      line.text = it->second;
      if (speakers) {
        line.speaker = SpeakerOf(k);  // the official English names: their own colour keys
        line.speaker_colour = ColourOf(*data, line.speaker, k.folder);
      }
    } else if (REXCVAR_GET(subtitles_placeholders)) {
      // Test text: no words of the game, only where the line comes from.
      line.text = Utf16("[" + k.folder + "] " + k.file);
    } else {
      return;  // nothing to show (never an invented line)
    }
    line.start = now;
    line.end = After(now, seconds + LingerSeconds());
    add.push_back(std::move(line));
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  for (Line& l : add) {
    l.uid = g_next_uid++;
    g_lines.push_back(std::move(l));
  }
}

void DebugLine(double seconds, const std::string& text) {
  Line line;
  line.id = 0;  // no sound: never cut
  line.text = TextOf(text);
  line.start = Clock::now();
  line.end = After(line.start, seconds + LingerSeconds());
  std::lock_guard<std::mutex> lock(g_mutex);
  g_on = true;  // a test line shows even without a text file
  line.uid = g_next_uid++;
  g_lines.push_back(std::move(line));
}

void OnVoiceCut(uint64_t id) {
  if (!g_on) return;
  const Clock::time_point now = Clock::now();
  std::lock_guard<std::mutex> lock(g_mutex);
  // The line on screen ends now; its later cues never come.
  std::erase_if(g_lines, [&](const Line& l) { return l.id == id && l.start > now; });
  for (Line& l : g_lines) {
    if (l.id == id) l.end = std::min(l.end, now);
  }
}

void OnMovie(const std::string& name, double seconds) {
  if (!g_on) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_movie = MovieKey(name);
  g_movie_time = seconds;
}

void OnMovieEnd() {
  if (!g_on) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_movie.clear();  // skipped or played out: its lines go at once
}

// ---------------------------------------------------------------------------
// 4. The drawing
// ---------------------------------------------------------------------------
namespace {

constexpr float kCapHeights[3] = {15.0f, 19.0f, 24.0f};  // a capital's height at 720p
constexpr float kBottomShare = 0.11f;   // the lowest line's bottom: 11 % above the picture's bottom
constexpr float kWidthShare = 0.70f;    // lines wrap at 70 % of the picture's width
constexpr ImU32 kTextColour = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
constexpr ImU32 kSpeakerColour = IM_COL32(0xFF, 0xD9, 0x40, 0xFF);  // the menus' selected yellow
constexpr ImU32 kOutlineColour = IM_COL32(0, 0, 0, 220);
// THE MESSAGE BAR (achievements.h BarOnScreen): the game's pop-up at the
// bottom centre, drawn by the game INSIDE the picture. The subtitles are
// drawn after the whole picture (this layer), so they are never behind it;
// over it they would hide its words. So while the bar's page is up the
// subtitles' bottom moves up to just above it: its top edge at rest, in
// 720p pixels from the picture's top (options_page.cpp, canvas y up, at
// 720p y = 720 - 1.5 u): our card's glass top y 87 u -> 589.5; the bar's own
// wide glass top y 135 u -> 517.5. A gap of kBarGap over it. The move eases
// over kLiftSeconds so the text doesn't jump.
constexpr float kCardTop720 = 589.5f, kWideBarTop720 = 517.5f, kBarGap720 = 10.0f;
constexpr float kLiftSeconds = 0.2f;
// THE STACK (SubtitleDialog::StackLines): a line fades in over
// kFadeInSeconds and out over kFadeOutSeconds (slower, so the eye can
// finish it); blocks stand kStackGap720 (720p pixels) apart and slide to a
// new place at kSlideRate (an exponential follow: 1 - e^(-12 t), 95 % in 0.25 s).
constexpr float kFadeInSeconds = 0.15f, kFadeOutSeconds = 0.40f;
constexpr float kStackGap720 = 6.0f;
constexpr float kSlideRate = 12.0f;
// Older lines (every one above the newest) are drawn at kOlderShare of their
// opacity, box included, easing there over kDimSeconds.
constexpr float kOlderShare = 0.6f, kDimSeconds = 0.25f;

// One piece of a drawn row: a run of characters in one colour.
struct Run {
  std::u16string text;
  ImU32 colour;
};
using Row = std::vector<Run>;

class SubtitleDialog : public rex::ui::ImGuiDialog {
 public:
  SubtitleDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate)
      : ImGuiDialog(drawer), immediate_(immediate) {}

  // The window repaints with every game frame anyway.
  bool WantsContinuousRepaint() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!FontReady()) return;
    // The game's 16:9 picture in the window.
    const float window_w = io.DisplaySize.x, window_h = io.DisplaySize.y;
    if (window_w <= 0 || window_h <= 0) return;
    pic_w_ = window_w;
    pic_h_ = window_w * 9.0f / 16.0f;
    if (pic_h_ > window_h) {
      pic_h_ = window_h;
      pic_w_ = window_h * 16.0f / 9.0f;
    }
    pic_x_ = (window_w - pic_w_) * 0.5f;
    pic_y_ = (window_h - pic_h_) * 0.5f;
    unit_ = pic_h_ / 720.0f;  // one pixel of the game's 720p picture
    ImDrawList* list = ImGui::GetForegroundDrawList();

    // THE SAMPLE (the Options screen's Accessibility tab): one line in the
    // chosen size, box and speaker setting, in the usual place, shown even
    // with the subtitles off so their look can be judged before turning
    // them on. Nothing else is drawn meanwhile.
    if (options_menu::SubtitlePreview()) {
      Line sample;
      if (REXCVAR_GET(subtitles_speakers)) {
        sample.speaker = u"Aku Aku";
        std::lock_guard<std::mutex> lock(g_mutex);
        sample.speaker_colour = ColourOf(*g_data, sample.speaker);
      }
      sample.text = tr(u"This is how the subtitles look.");
      DrawLines({sample}, list, BaseBottom());
      return;
    }

    std::vector<Line> shown;
    std::string movie;
    double movie_time = 0;
    {
      const Clock::time_point now = Clock::now();
      std::lock_guard<std::mutex> lock(g_mutex);
      movie = g_movie;
      movie_time = g_movie_time;
      // A movie's lines at its own clock (it stops when the movie pauses).
      if (!g_movie.empty()) {
        const auto cues = g_data->movie_cues.find(g_movie);
        if (cues != g_data->movie_cues.end()) {
          for (size_t n = 0; n < cues->second.size(); ++n) {
            const Cue& c = cues->second[n];
            // Shown from its start until its end + the linger (the movie's
            // own clock: it stops when the movie pauses).
            if (g_movie_time < c.start || g_movie_time >= c.end + LingerSeconds()) continue;
            Line line;
            line.uid = MovieUid(g_movie, n);
            line.text = c.text;
            if (REXCVAR_GET(subtitles_speakers)) {
              line.speaker = c.speaker;
              line.speaker_colour = ColourOf(*g_data, c.speaker_key);
            }
            shown.push_back(std::move(line));
          }
        }
      }
      std::erase_if(g_lines, [&](const Line& l) { return l.end <= now; });
      for (const Line& l : g_lines) {
        if (l.start <= now) shown.push_back(l);
      }
      // (How many stand at once is the drawing's business: StackLines.)
    }

    // THE DEVELOPER CLOCK (debug_subtitle_clock): the movie's name and time,
    // top left, small, apart from the subtitles; gone the moment it's off.
    if (REXCVAR_GET(debug_subtitle_clock) && !movie.empty()) {
      const int tenths = int(movie_time * 10.0);
      Line clock;
      clock.text = Utf16(fmt::format("[{}] {:02d}:{:02d}.{}", movie, tenths / 600, (tenths / 10) % 60,
                                     tenths % 10));
      DrawClock(clock, list);
    }

    // Shown: the flag on, and not while a menu stands over the game (the
    // pause menu: its decision ran in the last 200 ms; the Options screen).
    // The lines keep their clocks meanwhile and reappear if still running.
    const bool menu = NowMs() - g_menu_seen_ms.load(std::memory_order_relaxed) < 200 || options_menu::Open();
    if (!g_on || !REXCVAR_GET(subtitles) || menu) {
      // Hidden at once (a menu opening is a cut, not a fade); lines still
      // running when it closes fade in again.
      lifted_ = 0;
      stack_.clear();
      return;
    }
    // A frame's time, never more than 0.1 s (a hitch would jump the fades).
    const float dt = std::clamp(io.DeltaTime, 0.0f, 0.1f);
    StackLines(shown, dt);
    if (stack_.empty()) {
      lifted_ = 0;
      return;
    }
    DrawStack(list, LiftedBottom(dt), dt);
  }

  // THE STACK (what the player sees): every subtitle is its own little
  // block with its own box. The newest stands at the bottom, appearing in
  // place with a fade; when another line starts, the ones above it are
  // PUSHED UP, sliding (not jumping) to their new place. A line fades in quickly (kFadeInSeconds) and fades out
  // a little more slowly (kFadeOutSeconds) once its sound is over; while it
  // fades out it keeps its place, and the lines above slide down only once
  // it is gone. At most `subtitles_max_lines` (default 2) are wanted at once:
  // a newer one makes the oldest start fading out early. Lines above the
  // newest are dimmed (kOlderShare).
  struct Entry {
    Line line;
    bool wanted = true;   // still to be shown (else fading out)
    bool placed = false;  // `rise` set yet (the first frame starts it low)
    float alpha = 0;      // 0..1, before easing (Eased)
    float dim = 1;        // 1 = the newest line; older ones ease down to kOlderShare
    float rise = 0;       // its bottom's height above the stack's bottom (screen px)
  };

  // Brings the stack up to date with the lines wanted this frame (oldest
  // first, as gathered in OnDraw): new ones join at the bottom, gone ones
  // are marked to fade out, the fades step on, faded-out ones leave.
  void StackLines(const std::vector<Line>& shown, float dt) {
    const size_t max_lines = size_t(std::clamp(int(REXCVAR_GET(subtitles_max_lines)), 1, 4));
    const size_t first = shown.size() > max_lines ? shown.size() - max_lines : 0;
    for (Entry& e : stack_) e.wanted = false;
    for (size_t i = first; i < shown.size(); ++i) {
      const Line& l = shown[i];
      const auto it = std::find_if(stack_.begin(), stack_.end(),
                                   [&](const Entry& e) { return e.line.uid == l.uid; });
      if (it != stack_.end()) {
        it->wanted = true;
        it->line = l;  // the same line (a reload may have changed its words)
      } else {
        Entry e;
        e.line = l;
        stack_.push_back(std::move(e));  // the newest: at the bottom
      }
    }
    // The newest line still wanted is shown whole; every older one is
    // DIMMED (kOlderShare), easing there over kDimSeconds, so the eye goes
    // to the line being spoken.
    const Entry* newest = nullptr;
    for (const Entry& e : stack_) {
      if (e.wanted) newest = &e;
    }
    for (Entry& e : stack_) {
      e.alpha = e.wanted ? std::min(1.0f, e.alpha + dt / kFadeInSeconds)
                         : std::max(0.0f, e.alpha - dt / kFadeOutSeconds);
      const float dim_target = &e == newest ? 1.0f : kOlderShare;
      const float step = (1.0f - kOlderShare) * dt / kDimSeconds;
      e.dim = e.dim < dim_target ? std::min(dim_target, e.dim + step) : std::max(dim_target, e.dim - step);
    }
    std::erase_if(stack_, [](const Entry& e) { return !e.wanted && e.alpha <= 0.0f; });
  }

  // A fade's 0..1 eased (smoothstep): starts and ends gently.
  static float Eased(float a) { return a * a * (3.0f - 2.0f * a); }

  // The stack drawn with its lowest block's bottom at `bottom`.
  void DrawStack(ImDrawList* list, float bottom, float dt) {
    SetScale();
    const float max_width = pic_w_ * kWidthShare;
    const float gap = kStackGap720 * unit_;
    // Each block's rows and height first, then the targets from the bottom
    // up (the newest = the last entry = lowest).
    std::vector<std::vector<Row>> rows(stack_.size());
    std::vector<float> heights(stack_.size());
    for (size_t i = 0; i < stack_.size(); ++i) {
      Wrap(stack_[i].line, max_width, rows[i]);
      heights[i] = BlockHeight(rows[i]);
    }
    // How fast a block slides to its place: 1 - e^(-rate t) of the way per
    // t seconds (about 95 % of a move within 0.25 s).
    const float follow = 1.0f - std::exp(-kSlideRate * dt);
    float above = 0;  // the room taken below the block being placed
    for (size_t k = stack_.size(); k-- > 0;) {
      Entry& e = stack_[k];
      const float target = above;
      if (!e.placed) {
        // A new line appears IN its place (it only fades in; the lines above
        // it are the ones that move up).
        e.rise = target;
        e.placed = true;
      }
      e.rise += (target - e.rise) * follow;
      if (std::fabs(target - e.rise) < 0.25f) e.rise = target;
      if (!rows[k].empty()) above += heights[k] + gap;
    }
    for (size_t k = 0; k < stack_.size(); ++k) {
      if (rows[k].empty()) continue;
      DrawBlock(rows[k], list, bottom - stack_[k].rise, Eased(stack_[k].alpha) * stack_[k].dim);
    }
  }

  // The lowest row's bottom where nothing is in the way: 11 % above the
  // picture's bottom.
  float BaseBottom() const { return pic_y_ + pic_h_ * (1.0f - kBottomShare); }

  // The bottom moved up above the message bar while its page is up, eased.
  float LiftedBottom(float dt) {
    const float base = BaseBottom();
    float target = 0;  // how far up, in screen pixels
    switch (achievements::BarOnScreen()) {
      case achievements::BarLook::kCard:
        target = std::max(0.0f, base - (pic_y_ + (kCardTop720 - kBarGap720) * unit_));
        break;
      case achievements::BarLook::kWideBar:
        target = std::max(0.0f, base - (pic_y_ + (kWideBarTop720 - kBarGap720) * unit_));
        break;
      case achievements::BarLook::kNone:
        break;
    }
    const float step = pic_h_ * 0.25f * std::max(dt, 0.0f) / kLiftSeconds;  // a 25 %-of-picture move takes kLiftSeconds
    if (lifted_ < target) lifted_ = std::min(target, lifted_ + step);
    else lifted_ = std::max(target, lifted_ - step);
    return base - lifted_;
  }

  // The text's scale for the chosen size (sets scale_).
  void SetScale() {
    const int size = std::clamp(int(REXCVAR_GET(subtitles_size)), 0, 2);
    scale_ = kCapHeights[size] * unit_ / font_.cap_height();
  }
  // Rows are this far apart (the font's cell has room above / below).
  float RowHeight() const { return font_.cell_height() * scale_ * 0.78f; }
  // A block of rows: RowHeight apart, the last one a whole glyph cell tall.
  float BlockHeight(const std::vector<Row>& rows) const {
    if (rows.empty()) return 0;
    return RowHeight() * float(rows.size() - 1) + font_.cell_height() * scale_;
  }

  // Lines (oldest first) as ONE block of wrapped rows, its bottom at
  // `bottom`, fully shown (the Options screen's sample).
  void DrawLines(const std::vector<Line>& shown, ImDrawList* list, float bottom) {
    SetScale();
    std::vector<Row> rows;
    for (const Line& l : shown) Wrap(l, pic_w_ * kWidthShare, rows);
    DrawBlock(rows, list, bottom, 1.0f);
  }

  // One block: the box, then the outline layer, then the letters, all at
  // `alpha` (0..1, a fade). Never above the picture's top (a very long line
  // at Large): then it starts at the top and runs down.
  void DrawBlock(const std::vector<Row>& rows, ImDrawList* list, float bottom, float alpha) {
    if (rows.empty() || alpha <= 0.0f) return;
    const float block_h = BlockHeight(rows);
    const float top = std::max(pic_y_, bottom - block_h);
    bottom = top + block_h;
    const int opacity = REXCVAR_GET(subtitles_background)
                            ? std::clamp(int(REXCVAR_GET(subtitles_background_opacity)), 0, 100)
                            : 0;
    if (opacity > 0) {
      float widest = 0;
      for (const Row& r : rows) widest = std::max(widest, RowWidth(r));
      const float pad_x = 14.0f * unit_;  // the cells' own blank space pads it above and below
      const float cx = pic_x_ + pic_w_ * 0.5f;
      // The box's own opacity (the setting) times the fade.
      const int a = int(std::lround(float(opacity) * 2.55f * alpha));
      list->AddRectFilled(ImVec2(cx - widest * 0.5f - pad_x, top), ImVec2(cx + widest * 0.5f + pad_x, bottom),
                          IM_COL32(0, 0, 0, a), 6.0f * unit_);
    }
    const float row_height = RowHeight();
    for (int layer = 0; layer < 2; ++layer) {
      float y = top;
      for (const Row& r : rows) {
        DrawRow(list, r, std::round(pic_x_ + (pic_w_ - RowWidth(r)) * 0.5f), y, layer, alpha);
        y += row_height;
      }
    }
  }

  // The developer clock: one small row in the picture's top left corner.
  void DrawClock(const Line& l, ImDrawList* list) {
    scale_ = kCapHeights[0] * unit_ / font_.cap_height();
    const Row row{{l.text, kTextColour}};
    for (int layer = 0; layer < 2; ++layer) {
      DrawRow(list, row, std::round(pic_x_ + 12.0f * unit_), pic_y_ + 8.0f * unit_, layer);
    }
  }

  // One row from `pen`, its top at `y`: layer 0 = the dark outline, 1 = the letters.
  // `alpha` (0..1) scales the colours' own alpha (a fade).
  void DrawRow(ImDrawList* list, const Row& r, float pen, float y, int layer, float alpha = 1.0f) {
    for (const Run& run : r) {
      for (char16_t c : run.text) {
        const ImU32 colour = Faded(layer == 0 ? kOutlineColour : run.colour, alpha);
        const Composite k = CompositeOf(c);
        const game_font::Glyph* g = font_.Find(k.base ? k.base : c);
        if (!g) continue;
        DrawGlyph(list, *g, pen, y, layer == 1, colour, 1.0f);
        if (k.base) {
          // The tilde: the top rows of õ / Õ, down to where o / O's ink begins,
          // centred over the base letter.
          const game_font::Glyph* accent = font_.Find(k.accent);
          const game_font::Glyph* plain = font_.Find(k.plain);
          const float ink = plain ? InkTop(*plain) : -1.0f;  // o's first inked row
          if (accent && accent->height > 0 && ink > 0) {
            const float share = std::min(1.0f, ink / accent->height);
            const float shift = (g->left + g->width * 0.5f) - (accent->left + accent->width * 0.5f);
            DrawGlyph(list, *accent, pen + shift * scale_, y, layer == 1, colour, share);
          }
        }
        pen += g->advance * scale_;
      }
    }
  }

 private:
  // One glyph at pen / y (the row's top); `share` < 1 draws only its top part.
  void DrawGlyph(ImDrawList* list, const game_font::Glyph& g, float pen, float y, bool fill,
                 ImU32 colour, float share) {
    if (g.width <= 0) return;
    rex::ui::ImmediateTexture* texture = Texture(g.page, fill);
    if (!texture) return;
    const ImVec2 a(std::round(pen + g.left * scale_), std::round(y + g.top * scale_));
    const ImVec2 b(a.x + g.width * scale_, a.y + g.height * scale_ * share);
    const float v1 = g.v0 + (g.v1 - g.v0) * share;  // v runs top to bottom here (game_font.h)
    list->AddImage(ImTextureID(reinterpret_cast<uintptr_t>(texture)), a, b, ImVec2(g.u0, g.v0),
                   ImVec2(g.u1, v1), colour);
  }

  // How many of a glyph's drawn rows (from its top) have no ink in the
  // letter layer: where o / O's ink begins, i.e. where õ / Õ's tilde ends.
  // Read from the font's page once per glyph (UI thread).
  float InkTop(const game_font::Glyph& g) {
    const auto key = std::make_pair(g.page, uint32_t(g.v0 * 65536.0f) ^ (uint32_t(g.u0 * 65536.0f) << 16));
    if (const auto it = ink_top_.find(key); it != ink_top_.end()) return it->second;
    const game_font::PageLayers& l = font_.Layers(g.page);
    float result = -1.0f;
    if (l.width && l.height && !l.fill.empty()) {
      const uint32_t x0 = uint32_t(g.u0 * float(l.width)), x1 = uint32_t(g.u1 * float(l.width));
      const uint32_t y0 = uint32_t(g.v0 * float(l.height)), y1 = uint32_t(g.v1 * float(l.height));
      for (uint32_t y = y0; y < y1 && y < l.height && result < 0; ++y) {
        for (uint32_t x = x0; x < x1 && x < l.width; ++x) {
          if (l.fill[(size_t(y) * l.width + x) * 4 + 3] > 64) {  // RGBA: alpha = the ink
            result = float(y - y0);
            break;
          }
        }
      }
    }
    ink_top_[key] = result;
    return result;
  }

  // A line's width: the font's advances, with Ã / ã as wide as their base letter.
  float Width(std::u16string_view s) const {
    float w = 0;
    for (char16_t c : s) {
      const Composite k = CompositeOf(c);
      const game_font::Glyph* g = font_.Find(k.base ? k.base : c);
      if (g) w += g->advance;
    }
    return w * scale_;
  }

  // A colour with its alpha scaled by `alpha` (0..1).
  static ImU32 Faded(ImU32 colour, float alpha) {
    if (alpha >= 1.0f) return colour;
    const uint32_t a = (colour >> IM_COL32_A_SHIFT) & 0xFF;
    const uint32_t faded = uint32_t(std::lround(float(a) * std::max(alpha, 0.0f)));
    return (colour & ~IM_COL32_A_MASK) | (faded << IM_COL32_A_SHIFT);
  }

  // Ã and ã are listed in the game's font but EMPTY on the disc (zero width,
  // no picture: findings/49 s.8.3). The port composes them for the game's own
  // menus (ui/font_letters.cpp, a data patch of the loaded font); this
  // overlay reads the font from the disc, so it composes them the same way
  // while drawing: the base letter (a / A), plus the TILDE of õ / Õ, i.e. the
  // rows of õ above the first inked row of a plain o.
  struct Composite {
    char16_t base = 0;   // the letter drawn whole
    char16_t accent = 0; // whose top rows give the tilde
    char16_t plain = 0;  // the same letter without the accent (where the tilde ends)
  };
  static Composite CompositeOf(char16_t c) {
    if (c == u'\u00E3') return {u'a', u'\u00F5', u'o'};  // ã = a + the tilde of õ
    if (c == u'\u00C3') return {u'A', u'\u00D5', u'O'};  // Ã = A + the tilde of Õ
    return {};
  }
  float RowWidth(const Row& r) const {
    float w = 0;
    for (const Run& run : r) w += Width(run.text);
    return w;
  }

  // Splits one line into rows no wider than `max_width`, at spaces (and at
  // "\n"); the speaker's name goes in front of the first row, in its
  // character's colour (speakers.tsv), or yellow when it has none or the
  // colours are off (subtitles_speaker_colours).
  void Wrap(const Line& l, float max_width, std::vector<Row>& rows) const {
    Row row;
    float row_w = 0;
    if (!l.speaker.empty()) {
      const std::u16string label = l.speaker + u": ";
      const bool own = REXCVAR_GET(subtitles_speaker_colours) && l.speaker_colour != 0;
      row.push_back({label, own ? l.speaker_colour : kSpeakerColour});
      row_w = Width(label);
    }
    std::u16string current;  // the text run of the row being filled
    auto flush = [&]() {
      if (!current.empty()) row.push_back({current, kTextColour});
      if (!row.empty()) rows.push_back(row);
      row.clear();
      current.clear();
      row_w = 0;
    };
    size_t i = 0;
    const std::u16string& t = l.text;
    while (i < t.size()) {
      if (t[i] == u'\n') { flush(); ++i; continue; }
      size_t j = i;
      while (j < t.size() && t[j] != u' ' && t[j] != u'\n') ++j;
      const std::u16string word = t.substr(i, j - i);
      const std::u16string piece = current.empty() ? word : u" " + word;
      const float w = Width(piece);
      if (row_w + w > max_width && (row_w > 0)) {
        flush();
        current = word;
        row_w = Width(word);
      } else {
        current += piece;
        row_w += w;
      }
      i = j;
      while (i < t.size() && t[i] == u' ') ++i;
    }
    flush();
  }

  // The font, read on first use (as fps_overlay.cpp). A failure is final.
  bool FontReady() {
    if (font_.loaded()) return true;
    if (font_failed_ || !rex::Runtime::instance()) return false;
    font_failed_ = !font_.Load("Titans_Small");
    if (!font_failed_) textures_.resize(font_.page_count());
    return !font_failed_;
  }

  // A page's layer as a texture, made on first use (UI thread).
  rex::ui::ImmediateTexture* Texture(uint32_t page, bool fill) {
    if (!immediate_ || page >= textures_.size()) return nullptr;
    std::unique_ptr<rex::ui::ImmediateTexture>& slot =
        fill ? textures_[page].fill : textures_[page].outline;
    if (!slot) {
      const game_font::PageLayers& l = font_.Layers(page);
      slot = immediate_->CreateTexture(l.width, l.height, rex::ui::ImmediateTextureFilter::kLinear,
                                       false, fill ? l.fill.data() : l.outline.data());
    }
    return slot.get();
  }

  struct PageTextures {
    std::unique_ptr<rex::ui::ImmediateTexture> fill, outline;
  };
  rex::ui::ImmediateDrawer* immediate_;
  game_font::Font font_;
  bool font_failed_ = false;
  std::vector<PageTextures> textures_;
  float scale_ = 1.0f;
  // The game's 16:9 picture in the window this frame, and 720p's pixel in it.
  float pic_x_ = 0, pic_y_ = 0, pic_w_ = 0, pic_h_ = 0, unit_ = 1.0f;
  float lifted_ = 0;  // how far up the subtitles stand now (screen pixels), LiftedBottom
  std::vector<Entry> stack_;  // the subtitles on screen, oldest (top) first: StackLines
  std::map<std::pair<uint32_t, uint32_t>, float> ink_top_;  // InkTop's cache
};

// The one dialog, kept for the whole session (as fps_overlay's).
SubtitleDialog* g_dialog = nullptr;

}  // namespace

void Create(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate) {
  if (drawer && !g_dialog) g_dialog = new SubtitleDialog(drawer, immediate);  // registers itself
}

}  // namespace subtitles
