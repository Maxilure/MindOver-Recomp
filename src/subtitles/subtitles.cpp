// =============================================================================
// subtitles/subtitles.cpp -- subtitles for the game's spoken lines (findings/63)
// =============================================================================
//
// See subtitles.h for what the player sees and where the timing comes from.
// This file holds:
//   1. the settings (REXCVARs, kept in user/settings.toml like the others);
//   2. the text file (user/subtitles/<code>.tsv, read once at start-up):
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
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>

#include "game_folder.h"
#include "language/language.h"
#include "ui/game_font.h"

REXCVAR_DEFINE_INT32(subtitles, 0, "CrashMoM",
                     "Subtitles for spoken lines (cutscenes, characters, Aku Aku): 0 off, 1 on "
                     "(findings/63)")
    .range(0, 1);
REXCVAR_DEFINE_INT32(subtitles_size, 1, "CrashMoM", "Subtitle size: 0 small, 1 medium, 2 large")
    .range(0, 2);
REXCVAR_DEFINE_BOOL(subtitles_background, true, "CrashMoM",
                    "A dark translucent box behind the subtitles");
REXCVAR_DEFINE_BOOL(subtitles_speakers, true, "CrashMoM",
                    "The speaker's name in front of a subtitle, when it is known");
REXCVAR_DEFINE_BOOL(subtitles_placeholders, false, "CrashMoM",
                    "Where a line has no text yet, show its speaker folder and sound file name "
                    "(for checking subtitle timing)");

namespace subtitles {
namespace {

using Clock = std::chrono::steady_clock;

// A line stays this long after its sound ends, so a short line can be read
// (cut short if another line starts: at most kMaxLines are shown).
constexpr double kHoldSeconds = 0.3;
constexpr size_t kMaxLines = 2;

bool g_on = false;

// ---------------------------------------------------------------------------
// 2. The text file
// ---------------------------------------------------------------------------
// A timed line: from `start` to `end` seconds of its movie / sound.
struct Cue {
  double start = 0, end = 0;
  std::u16string speaker;  // "" = none
  std::u16string text;
};
// Read-only after Start.
std::unordered_map<uint64_t, std::u16string> g_texts;     // sound ID -> its whole line
std::unordered_map<uint64_t, std::vector<Cue>> g_sound_cues;  // sound ID -> its lines
std::unordered_map<std::string, std::vector<Cue>> g_movie_cues;  // movie name (lower case) -> its lines

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

// Reads `<user>/subtitles/<code>.tsv`; false if there is no such file.
// A line is TAB-separated, one of:
//   <sound ID>  <text>                               a sound's whole line
//   <key>  <start>  <end>  <speaker>  <text>         a timed line (cue)
// where <key> = a sound ID (a sound holding several lines) or a movie's file
// name (Crash_02_51), the times are seconds from the start of that sound /
// movie ("mm:ss.cc" or "ss.cc"), <speaker> may be empty. # starts a comment.
bool ReadTexts(const std::string& code) {
  const std::filesystem::path path = game_folder::UserFolder() / "subtitles" / (code + ".tsv");
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
      g_texts[id] = TextOf(f[1]);
      ++whole;
    } else if (f.size() == 5) {
      Cue cue;
      cue.start = ParseTime(f[1]);
      cue.end = ParseTime(f[2]);
      cue.speaker = Utf16(f[3]);
      cue.text = TextOf(f[4]);
      if (cue.start < 0 || cue.end <= cue.start) { ++skipped; continue; }
      if (is_id) {
        if (!sound_seen[id]) { g_sound_cues[id].clear(); sound_seen[id] = true; }
        g_sound_cues[id].push_back(std::move(cue));
      } else {
        const std::string key = MovieKey(f[0]);
        if (!movie_seen[key]) { g_movie_cues[key].clear(); movie_seen[key] = true; }
        g_movie_cues[key].push_back(std::move(cue));
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
  std::u16string speaker;
  std::u16string text;
  Clock::time_point start;
  Clock::time_point end;  // moved earlier by a cut
};
std::mutex g_mutex;
std::vector<Line> g_lines;  // oldest first; a cue's line may start in the future

// The Bink movie playing now (voice_trace.cpp's frame-end watcher, main
// thread): its key and its clock (seconds from its first frame).
std::string g_movie;  // "" = none
double g_movie_time = 0;

Clock::time_point After(Clock::time_point t, double seconds) {
  return t + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
}

}  // namespace

bool On() { return g_on; }

void Start() {
  g_on = REXCVAR_GET(subtitles) != 0;
  if (!g_on) return;
  // The text language's code ("pt-br").
  const auto& langs = language::TextLanguages();
  const int running = language::RunningText();
  const std::string code =
      running >= 0 && running < int(langs.size()) ? langs[size_t(running)].key : "en-us";
  // English first, then the language's own file over it (its lines replace
  // English ones key by key; what it doesn't have stays English).
  const bool english = ReadTexts("en");
  const bool own = code != "en" && ReadTexts(code);
  if (!english && !own) {
    REXLOG_INFO("Subtitles: on, no text file in {} ({}.tsv or en.tsv){}",
                (game_folder::UserFolder() / "subtitles").string(), code,
                REXCVAR_GET(subtitles_placeholders) ? "; placeholders shown" : "");
  }
}

void OnVoiceStart(uint64_t id, const std::string& name, double seconds) {
  if (!g_on) return;
  const Kind k = KindOf(name);
  if (!k.in_scope) return;
  const Clock::time_point now = Clock::now();
  const bool speakers = REXCVAR_GET(subtitles_speakers);
  std::vector<Line> add;
  if (const auto cues = g_sound_cues.find(id); cues != g_sound_cues.end()) {
    // A sound with several lines (an in-engine cutscene track): each at its
    // time inside the sound, never past the sound's end.
    for (const Cue& c : cues->second) {
      Line line;
      line.id = id;
      line.text = c.text;
      if (speakers) line.speaker = c.speaker;
      line.start = After(now, c.start);
      line.end = After(now, std::min(c.end, seconds + kHoldSeconds));
      if (line.end > line.start) add.push_back(std::move(line));
    }
  } else {
    Line line;
    line.id = id;
    if (const auto it = g_texts.find(id); it != g_texts.end()) {
      line.text = it->second;
      if (speakers) line.speaker = SpeakerOf(k);
    } else if (REXCVAR_GET(subtitles_placeholders)) {
      // Test text: no words of the game, only where the line comes from.
      line.text = Utf16("[" + k.folder + "] " + k.file);
    } else {
      return;  // nothing to show (never an invented line)
    }
    line.start = now;
    line.end = After(now, seconds + kHoldSeconds);
    add.push_back(std::move(line));
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  for (Line& l : add) g_lines.push_back(std::move(l));
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
constexpr ImU32 kBoxColour = IM_COL32(0, 0, 0, 140);

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
    if (!g_on || !FontReady()) return;
    // The lines to show now (the newest kMaxLines), expired ones dropped.
    std::vector<Line> shown;
    {
      const Clock::time_point now = Clock::now();
      std::lock_guard<std::mutex> lock(g_mutex);
      // A movie's lines at its own clock (it stops when the movie pauses).
      if (!g_movie.empty()) {
        const auto cues = g_movie_cues.find(g_movie);
        if (cues != g_movie_cues.end()) {
          for (const Cue& c : cues->second) {
            if (g_movie_time < c.start || g_movie_time >= c.end) continue;
            Line line;
            line.text = c.text;
            if (REXCVAR_GET(subtitles_speakers)) line.speaker = c.speaker;
            shown.push_back(std::move(line));
          }
        }
        if (shown.empty() && REXCVAR_GET(subtitles_placeholders)) {
          // Test text: the movie's name and clock, for checking (and for
          // noting down the times of its lines).
          const int tenths = int(g_movie_time * 10.0);
          shown.push_back({0, u"", Utf16(fmt::format("[{}] {:02d}:{:02d}.{}", g_movie, tenths / 600,
                                                     (tenths / 10) % 60, tenths % 10)),
                           now, now});
        }
      }
      std::erase_if(g_lines, [&](const Line& l) { return l.end <= now; });
      for (const Line& l : g_lines) {
        if (l.start <= now) shown.push_back(l);
      }
      if (shown.size() > kMaxLines) shown.erase(shown.begin(), shown.end() - std::ptrdiff_t(kMaxLines));
    }
    if (shown.empty()) return;

    // The game's 16:9 picture in the window.
    const float window_w = io.DisplaySize.x, window_h = io.DisplaySize.y;
    if (window_w <= 0 || window_h <= 0) return;
    float pic_w = window_w, pic_h = window_w * 9.0f / 16.0f;
    if (pic_h > window_h) {
      pic_h = window_h;
      pic_w = window_h * 16.0f / 9.0f;
    }
    const float pic_x = (window_w - pic_w) * 0.5f, pic_y = (window_h - pic_h) * 0.5f;
    const float unit = pic_h / 720.0f;  // one pixel of the game's 720p picture
    const int size = std::clamp(int(REXCVAR_GET(subtitles_size)), 0, 2);
    scale_ = kCapHeights[size] * unit / font_.cap_height();
    const float max_width = pic_w * kWidthShare;
    const float row_height = font_.cell_height() * scale_ * 0.78f;  // the cell has room above / below

    // Rows of every shown line, wrapped by words.
    std::vector<Row> rows;
    for (const Line& l : shown) Wrap(l, max_width, rows);
    if (rows.empty()) return;

    // The block, bottom-up from 11 % above the picture's bottom: rows are
    // row_height apart, the last one a whole glyph cell tall.
    const float cell_h = font_.cell_height() * scale_;
    const float block_h = row_height * float(rows.size() - 1) + cell_h;
    const float bottom = pic_y + pic_h * (1.0f - kBottomShare);
    const float top = bottom - block_h;
    ImDrawList* list = ImGui::GetForegroundDrawList();
    if (REXCVAR_GET(subtitles_background)) {
      float widest = 0;
      for (const Row& r : rows) widest = std::max(widest, RowWidth(r));
      const float pad_x = 14.0f * unit, pad_y = 0.0f;  // the cells' own blank space pads it
      const float cx = pic_x + pic_w * 0.5f;
      list->AddRectFilled(ImVec2(cx - widest * 0.5f - pad_x, top - pad_y),
                          ImVec2(cx + widest * 0.5f + pad_x, bottom + pad_y), kBoxColour,
                          6.0f * unit);
    }
    // Outline layer first (dark), then the letters, like the game's look.
    for (int layer = 0; layer < 2; ++layer) {
      float y = top;
      for (const Row& r : rows) {
        float pen = std::round(pic_x + (pic_w - RowWidth(r)) * 0.5f);
        for (const Run& run : r) {
          for (char16_t c : run.text) {
            const ImU32 colour = layer == 0 ? kOutlineColour : run.colour;
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
        y += row_height;
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
  // "\n"); the speaker's name goes in front of the first row, in yellow.
  void Wrap(const Line& l, float max_width, std::vector<Row>& rows) const {
    Row row;
    float row_w = 0;
    if (!l.speaker.empty()) {
      const std::u16string label = l.speaker + u": ";
      row.push_back({label, kSpeakerColour});
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
  std::map<std::pair<uint32_t, uint32_t>, float> ink_top_;  // InkTop's cache
};

// The one dialog, kept for the whole session (as fps_overlay's).
SubtitleDialog* g_dialog = nullptr;

}  // namespace

void Create(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate) {
  if (drawer && !g_dialog) g_dialog = new SubtitleDialog(drawer, immediate);  // registers itself
}

}  // namespace subtitles
