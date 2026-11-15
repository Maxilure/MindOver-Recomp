// =============================================================================
// tools/subtitle_editor/main.cpp -- the subtitle editor's window (step 1)
// =============================================================================
//
// WHAT IT IS FOR (notes: plans.md "SUBTITLE EDITOR"; findings/63)
//   The subtitles' times have to fit each line's real sound, and some sounds
//   hold more than one sentence ("I smell evil and lunacy on the wind." then
//   "N. Gin must be up ahead somewhere!"), which then need one subtitle each.
//   This program lists every voice line of the player's own disc, in every
//   voice language, plays it, and shows its waveform with the subtitles the
//   game would show over it.
//
// STEP 1: list + filters + waveform + play. STEP 2: editing a line's
// subtitles (split, drag, type, merge, delete, undo) and saving the file;
// how, at the top of the Editor class below.
//
// HOW IT FINDS THINGS
//   The game folder: --game_folder=<path>, else the first folder at or above
//   the editor's own folder (or the current folder) that has game/*.rcf.
//   The voices: <game folder>/game/<language>.rcf (voice_archive.h).
//   The texts: the shipped subtitles/ (next to the editor in an install,
//   <repo>/subtitles in the source tree, <game folder>/program/subtitles) and
//   the player's own <game folder>/user/subtitles/, read like the game does.
//
// KEYS: Up / Down = previous / next line, Space = play / stop, S split,
//   N new piece, Delete, Enter play the piece, Ctrl+Z undo, Ctrl+Y or
//   Ctrl+Shift+Z redo (both anywhere, text boxes too), Ctrl+S save.
//
// TEST OPTIONS: --mute (no sound device: the play head runs on the clock),
//   --select=<sound ID>, --play (play the selected line at start),
//   --voices=<archive stem>, --screenshot=<file.png> [--screenshot_after=s].
// =============================================================================

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>  // Windows: SDL provides WinMain and calls main below
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <imgui_internal.h>  // ImGui::ClearActiveID: Ctrl+Z leaves a text box first
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <atomic>
#include <optional>
#include <cctype>
#include <iterator>
#include <map>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "exchange.h"
#include "icons.h"
#include "movie_archive.h"
#include "subtitle_texts.h"
#include "voice_archive.h"

namespace fs = std::filesystem;
using namespace subtitle_editor;
using Clock = std::chrono::steady_clock;

namespace {

// -----------------------------------------------------------------------------
// Finding the game folder and the system font
// -----------------------------------------------------------------------------

bool HasVoices(const fs::path& dir) { return !VoiceArchives(dir / "game").empty(); }

fs::path FindGameFolder(const fs::path& start) {
  std::error_code ec;
  for (fs::path p = fs::absolute(start, ec); !p.empty(); p = p.parent_path()) {
    if (HasVoices(p)) return p;
    if (p == p.parent_path()) break;
  }
  return {};
}

fs::path SystemFont() {
#ifdef _WIN32
  const fs::path segoe = "C:\\Windows\\Fonts\\segoeui.ttf";
  std::error_code ec;
  return fs::is_regular_file(segoe, ec) ? segoe : fs::path();
#else
  std::string path;
  if (FILE* pipe = popen("fc-match -f '%{file}' 'sans:style=Regular' 2>/dev/null", "r")) {
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe)) path += buffer;
    pclose(pipe);
  }
  std::error_code ec;
  return !path.empty() && fs::is_regular_file(path, ec) ? fs::path(path) : fs::path();
#endif
}

std::string Hex(uint64_t id) {
  char buf[20];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(id));
  return buf;
}

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// -----------------------------------------------------------------------------
// Sound out: one SDL audio stream per played line, at the line's own rate
// -----------------------------------------------------------------------------

class Player {
 public:
  explicit Player(bool mute) : mute_(mute) {}
  ~Player() { Stop(); }

  // Plays `pcm` from `from` seconds, up to `to` (< 0 = to the end).
  void Play(const Pcm& pcm, double from, double to = -1) {
    Stop();
    rate_ = pcm.rate;
    const size_t first = std::min(pcm.frames(), size_t(std::max(0.0, from) * pcm.rate));
    const size_t last = to < 0 ? pcm.frames() : std::clamp(size_t(to * pcm.rate), first, pcm.frames());
    start_ = double(first) / pcm.rate;
    total_frames_ = last - first;
    started_ = Clock::now();
    playing_ = true;
    if (mute_ || total_frames_ == 0) return;
    const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, int(pcm.rate)};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream_) {
      std::fprintf(stderr, "subtitle editor: no sound device: %s\n", SDL_GetError());
      return;  // the play head still runs on the clock
    }
    SDL_PutAudioStreamData(stream_, pcm.stereo.data() + first * 2, int(total_frames_ * 2 * sizeof(float)));
    SDL_FlushAudioStream(stream_);
    SDL_SetAudioStreamGain(stream_, volume_);
    SDL_ResumeAudioStreamDevice(stream_);
  }

  // The playback volume, 0 (silent) .. 2 (twice as loud); live on a playing line.
  float volume() const { return volume_; }
  void SetVolume(float v) {
    volume_ = std::clamp(v, 0.0f, 2.0f);
    if (stream_) SDL_SetAudioStreamGain(stream_, volume_);
  }

  void Stop() {
    if (stream_) {
      SDL_DestroyAudioStream(stream_);
      stream_ = nullptr;
    }
    playing_ = false;
  }

  bool playing() {
    if (playing_ && Elapsed() >= double(total_frames_) / std::max(1u, rate_)) playing_ = false;
    return playing_;
  }

  // Where the play head is, in seconds of the line.
  double Position() {
    return start_ + std::min(Elapsed(), double(total_frames_) / std::max(1u, rate_));
  }

 private:
  // Seconds played since Play: from what is still queued (the device's
  // own pace) when there is a device, else the clock.
  double Elapsed() const {
    if (stream_) {
      const int queued = SDL_GetAudioStreamQueued(stream_);
      const size_t left = queued > 0 ? size_t(queued) / (2 * sizeof(float)) : 0;
      return double(total_frames_ - std::min(left, total_frames_)) / std::max(1u, rate_);
    }
    return std::chrono::duration<double>(Clock::now() - started_).count();
  }

  bool mute_;
  float volume_ = 0.8f;  // a little below full: the 5.1 fold-down can be loud
  SDL_AudioStream* stream_ = nullptr;
  uint32_t rate_ = 0;
  double start_ = 0;
  size_t total_frames_ = 0;
  Clock::time_point started_;
  bool playing_ = false;
};

// -----------------------------------------------------------------------------
// The editor
// -----------------------------------------------------------------------------
//
// EDITING (step 2). A line's subtitles are PIECES: a time span of the sound
// with a speaker and a text. On the waveform:
//   * click            = put the cursor there (yellow); no sound starts (Space
//                        plays from it); while playing, playing jumps there;
//   * click a piece = select it (white frame, grips on its edges);
//   * drag the SELECTED piece's edge (pointer <->) = move that edge; drag
//     inside it (pointer +) = move the whole piece, its length kept;
//   * right-click      = Split here / New piece here;
// keys (outside the text boxes): S split at the play head (or the cursor),
// N new piece at the cursor, Delete the selected piece, Enter play the
// selected piece, Space play / stop, Ctrl+Z undo, Ctrl+Y redo, Ctrl+S save, Up / Down the
// list. Below the waveform each piece's start, end, speaker and text can be
// typed. A "whole line" (no times) becomes a timed piece over the whole
// sound the first time it is edited.
//
// SAVING writes the WHOLE text file (subtitle_texts.h: untouched lines kept
// as they were), to the source tree's subtitles/ (to share: commit it) or to
// the player's own user/subtitles/ (which the game then reads INSTEAD of the
// shipped file). A copy of the file it replaces goes to
// user/subtitles/backups/ first.
//
// TIMES AND VOICE LANGUAGES (step 3): the text files' times are made on the
// English voices. With another voice language picked, the pieces show that
// voice's OWN times when subtitles/timing/<voice>.tsv has them (else the
// English ones, marked so), and dragging / typing a time writes there (the
// whole line's pieces get their times from then on). The pieces themselves
// (split, new, merge, delete) are shared by every voice: changed with the
// English voices only. Words and speakers are the text file's, whatever
// voice plays.

// Colours of the pieces on the waveform, one after the other.
const ImU32 kPieceColours[] = {IM_COL32(245, 140, 33, 90), IM_COL32(80, 170, 255, 90),
                               IM_COL32(120, 210, 120, 90), IM_COL32(220, 110, 220, 90)};
const ImVec4 kWarnColour(1.0f, 0.80f, 0.35f, 1.0f);
const ImVec4 kUnusedColour(0.62f, 0.62f, 0.78f, 1.0f);  // lines marked unused
const ImVec4 kBadColour(1.0f, 0.42f, 0.38f, 1.0f);
constexpr double kMinPiece = 0.10;  // seconds: a piece is never shorter

// A divider the pointer drags to share space between two panels: `vertical`
// = a column divider (left / right, changes a width), else a row divider
// (above / below, changes a height). `size` = the first panel's size,
// kept within [min, max].
void Splitter(const char* id, bool vertical, float* size, float min, float max) {
  const float thick = ImGui::GetFontSize() * 0.45f;
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const ImVec2 box = vertical ? ImVec2(thick, avail.y) : ImVec2(avail.x, thick);
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton(id, box);
  const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
  if (hot) ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
  if (ImGui::IsItemActive()) {
    *size += vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
  }
  *size = std::clamp(*size, min, std::max(min, max));
  // A short grip in the middle of the bar, brighter while hovered / dragged.
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 colour = hot ? IM_COL32(245, 140, 33, 255) : IM_COL32(90, 90, 100, 255);
  const ImVec2 c(p0.x + box.x * 0.5f, p0.y + box.y * 0.5f);
  const float half = ImGui::GetFontSize() * 1.2f;
  if (vertical) dl->AddLine(ImVec2(c.x, c.y - half), ImVec2(c.x, c.y + half), colour, 3.0f);
  else dl->AddLine(ImVec2(c.x - half, c.y), ImVec2(c.x + half, c.y), colour, 3.0f);
}

class Editor {
 public:
  Editor(fs::path root, bool mute, SDL_Renderer* renderer)
      : root_(std::move(root)), renderer_(renderer), player_(mute) {
    archives_ = VoiceArchives(root_ / "game");
    user_dir_ = root_ / "user" / "subtitles";
    backup_dir_ = user_dir_ / "backups";
    std::error_code ec;
    // The source tree (its subtitles/ is what ships): saving there = to share.
    if (fs::is_regular_file(root_ / "crash_mom_manifest.toml", ec) && fs::is_directory(root_ / "subtitles", ec)) {
      source_dir_ = root_ / "subtitles";
    }
    save_to_source_ = !source_dir_.empty();
    movies_.Open(root_ / "game");
    // Who checks lines on this PC (kept in the player's own folder, never shared
    // by itself: it only goes out inside the checks they save).
    {
      std::ifstream in(user_dir_ / "checker-name.txt");
      std::getline(in, checker_);
      std::snprintf(name_buf_, sizeof(name_buf_), "%s", checker_.c_str());
    }
    ReloadTexts();
  }
  ~Editor() {
    if (loader_.joinable()) loader_.join();
    if (video_texture_) SDL_DestroyTexture(video_texture_);
  }

  void ReloadTexts() {
    // The shipped files: next to the editor (an install), the source tree's,
    // or an installed game's program/subtitles.
    fs::path shipped;
    std::error_code ec;
    for (const fs::path& candidate :
         {fs::path(SDL_GetBasePath() ? SDL_GetBasePath() : ".") / "subtitles", root_ / "subtitles",
          root_ / "program" / "subtitles"}) {
      if (fs::is_directory(candidate, ec)) {
        shipped = candidate;
        break;
      }
    }
    shipped_dir_ = shipped;
    texts_ = ReadTextFiles(shipped, user_dir_);
    timings_.clear();
    checks_.clear();
    for (const TextFile& t : texts_) {
      checks_[t.code] = ReadCheckFile(t.code, shipped_dir_ / "checked", user_dir_ / "checked");
    }
    unused_ = ReadUnusedFile(shipped_dir_, user_dir_);
    text_index_ = std::clamp(text_index_, 0, std::max(0, int(texts_.size()) - 1));
    undo_.clear();
    redo_.clear();
    box_undo_.reset();
    sel_piece_ = -1;
    CollectSpeakers();
  }

  void LoadVoices(int index, uint64_t keep_id) {
    if (index < 0 || index >= int(archives_.size())) return;
    if (loader_.joinable()) loader_.join();
    voice_index_ = index;
    loaded_ = false;
    progress_ = 0;
    total_ = 0;
    keep_id_ = keep_id;
    player_.Stop();
    pcm_ = {};
    loader_ = std::thread([this, path = archives_[size_t(index)]] {
      auto archive = std::make_unique<VoiceArchive>();
      archive->Open(path, &progress_, &total_);
      pending_ = std::move(archive);
      loaded_ = true;
    });
  }

  void Select(uint64_t id) { select_id_ = id; }
  void SelectMovie(std::string movie) { select_movie_ = std::move(movie); }
  void PlayAtStart() { play_at_start_ = true; }
  // --music: a cartoon's music and effects on from the start.
  void SetMusic(bool on) {
    for (const char* part : {"Front (music + effects)", "Back (music + effects)"}) {
      mixer_[part].on = on;
      mixer_[part].chosen = true;
    }
  }
  // --do=: actions run once the selected line is loaded (tests without a
  // mouse): cursor:<s> split new delete:<n> merge:<n> text:<n>:<words>
  // speaker:<n>:<name> times:<n>:<s>:<e> zoom:<from>:<len> undo save
  // (pieces numbered from 1, as on screen).
  void QueueActions(std::vector<std::string> actions) { actions_ = std::move(actions); }

  // Unsaved changes in any text language.
  int Unsaved() const {
    int n = 0;
    for (const TextFile& t : texts_) n += int(t.Edits());
    for (const auto& [voice, timing] : timings_) n += int(timing.edited.size());
    for (const auto& [code, file] : checks_) n += file.edited ? 1 : 0;
    n += unused_.edited ? 1 : 0;
    return n;
  }
  // The window's close button: quit, or ask first when something is unsaved.
  void RequestQuit() {
    if (Unsaved() == 0) quit_ = true;
    else ask_quit_ = true;
  }
  bool ShouldQuit() const { return quit_; }

  void Draw() {
    // A finished load takes over (on this thread: the UI reads it).
    if (loaded_ && pending_) {
      if (loader_.joinable()) loader_.join();
      archive_ = std::move(pending_);
      if (!select_movie_.empty()) {  // --select=<movie>: the Cartoons tab, that movie
        cartoons_ = true;
        force_tab_ = true;
        Refilter();
        for (size_t i = 0; i < shown_.size(); ++i) {
          if (Lower(movies_.clips()[size_t(shown_[i])].movie) == Lower(select_movie_)) SelectRow(int(i), true);
        }
        select_movie_.clear();
        if (play_at_start_ && clip()) {
          play_at_start_ = false;
          player_.Play(pcm_, 0);
        }
      } else if (cartoons_) {
        // The cartoon stays; its dialogue track is now the new voice's.
        DecodeSelected();
      } else {
        LoadedVoices();
      }
      RunActions();
    }
    DrawWindow();
  }

  // A voice archive just loaded: the list again, the same line selected.
  void LoadedVoices() {
    {
      Refilter();
      const uint64_t want = select_id_ ? select_id_ : keep_id_;
      select_id_ = 0;
      selected_ = -1;
      for (size_t i = 0; i < shown_.size(); ++i) {
        if (archive_->clips()[size_t(shown_[i])].id == want) SelectRow(int(i), true);
      }
      if (selected_ < 0 && want) {
        // Hidden by the filters: show everything so it can be selected.
        in_scope_only_ = false;
        without_text_only_ = false;
        search_[0] = 0;
        Refilter();
        for (size_t i = 0; i < shown_.size(); ++i) {
          if (archive_->clips()[size_t(shown_[i])].id == want) SelectRow(int(i), true);
        }
      }
      if (play_at_start_ && clip()) {
        play_at_start_ = false;
        player_.Play(pcm_, 0);
      }
    }
  }

  void DrawWindow() {
    TakePendingImport();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("Subtitle editor", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    HandleUndoKeys();  // before the widgets: a text box mustn't take Ctrl+Z first
    DrawTopBar();
    if (!archive_) {
      ImGui::TextDisabled("Reading %s ... %d of %d", archives_.empty() ? "?" :
                          archives_[size_t(voice_index_)].filename().string().c_str(),
                          progress_.load(), total_.load());
    } else {
      // The list | the line, with a divider to drag between them.
      const float avail = ImGui::GetContentRegionAvail().x;
      if (list_w_ <= 0) list_w_ = avail * 0.34f;
      const float bottom = -(ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y);
      ImGui::BeginChild("list", ImVec2(list_w_, bottom), ImGuiChildFlags_Borders);
      DrawList();
      ImGui::EndChild();
      ImGui::SameLine(0, 0);
      {
        // The divider only as tall as the panels.
        ImGui::BeginChild("split_col", ImVec2(ImGui::GetFontSize() * 0.45f, bottom), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
        Splitter("##list_split", true, &list_w_, ImGui::GetFontSize() * 10, avail - ImGui::GetFontSize() * 20);
        ImGui::EndChild();
      }
      ImGui::SameLine(0, 0);
      ImGui::BeginChild("line", ImVec2(0, bottom), ImGuiChildFlags_Borders);
      DrawLine();
      ImGui::EndChild();
    }
    DrawStatusBar();
    HandleKeys();
    DrawQuitQuestion();
    ImGui::End();
  }

  bool busy() { return !loaded_ || player_.playing() || drag_piece_ >= 0; }
  bool ready() const { return archive_ != nullptr; }

 private:
  const Clip* clip() const {
    if (selected_ < 0 || selected_ >= int(shown_.size()) || !List()) return nullptr;
    return &(*List())[size_t(shown_[size_t(selected_)])];
  }
  TextFile* text() {
    return text_index_ >= 0 && text_index_ < int(texts_.size()) ? &texts_[size_t(text_index_)] : nullptr;
  }
  // The list shown: the voice lines of the picked voice, or the cartoons.
  const std::vector<Clip>* List() const {
    if (cartoons_) return &movies_.clips();
    return archive_ ? &archive_->clips() : nullptr;
  }

  // A line's key in the files: a sound ID's 16 hex digits or a movie's name.
  static std::string KeyFor(const Clip& c) { return c.movie.empty() ? KeyOf(c.id) : c.movie; }
  // A movie's key as the text file writes it (its case), else the movie's own.
  static std::string MovieKeyIn(const TextFile& t, const std::string& movie) {
    if (t.movies.count(movie)) return movie;
    for (const auto& [key, pieces] : t.movies) {
      if (Lower(key) == Lower(movie)) return key;
    }
    return movie;
  }
  const std::vector<Piece>* PiecesOf(const Clip& c) {
    TextFile* t = text();
    if (!t) return nullptr;
    if (!c.movie.empty()) {
      const auto it = t->movies.find(MovieKeyIn(*t, c.movie));
      return it == t->movies.end() ? nullptr : &it->second;
    }
    const auto it = t->sounds.find(c.id);
    return it == t->sounds.end() ? nullptr : &it->second;
  }
  static bool IsEdited(const TextFile& t, const Clip& c) {
    return c.movie.empty() ? t.edited.count(c.id) != 0 : t.edited_movies.count(MovieKeyIn(t, c.movie)) != 0;
  }
  double Length() const { return pcm_.rate ? double(pcm_.frames()) / pcm_.rate : 0; }

  // The voice language picked, as the game names it (voice_language key):
  // the archive's name, but norway.rcf = "norwegian".
  std::string VoiceKey() const {
    if (archives_.empty()) return "english";
    const std::string stem = Lower(archives_[size_t(voice_index_)].stem().string());
    return stem == "norway" ? "norwegian" : stem;
  }
  bool OwnTimes() const { return VoiceKey() != "english"; }
  // The picked voice's timing file (read once, kept with its edits).
  VoiceTiming& Timing() {
    const std::string voice = VoiceKey();
    auto it = timings_.find(voice);
    if (it == timings_.end()) {
      it = timings_.emplace(voice, ReadVoiceTiming(voice, shipped_dir_ / "timing", user_dir_ / "timing")).first;
    }
    return it->second;
  }

  // What the pieces' times are for the picked voice.
  enum class Times { kEnglish, kOwn, kNoneYet, kNotMatching };
  // The selected line's pieces as the game would time them with the picked
  // voice: the text's pieces, with the voice's own times when its timing
  // file covers exactly these pieces (as the game decides).
  std::vector<Piece> View(const Clip& c, Times* state = nullptr) {
    std::vector<Piece> view;
    if (const auto* pieces = PiecesOf(c)) view = *pieces;
    Times st = Times::kEnglish;
    if (OwnTimes()) {
      st = Times::kNoneYet;
      const auto& times = Timing().times;
      if (const auto it = times.find(KeyFor(c)); it != times.end()) {
        const auto& list = it->second;
        const bool complete = list.size() == view.size() &&
                              std::all_of(list.begin(), list.end(), [](const auto& t) { return t.first >= 0; });
        st = complete ? Times::kOwn : Times::kNotMatching;
        if (complete) {
          for (size_t i = 0; i < view.size(); ++i) {
            if (view[i].whole()) continue;
            view[i].start = list[i].first;
            view[i].end = list[i].second;
          }
        }
      }
    }
    if (state) *state = st;
    return view;
  }

  // Piece i's times changed (drag / typed): with the English voices into the
  // text file, with another voice into its timing file (the line's whole
  // list, starting from the times shown, so the game sees every piece).
  // Moves piece `i` to start at `start`, its length kept, never over its
  // neighbours or past the sound's ends. False if it can't move (a whole
  // line, no such piece).
  bool MovePiece(int i, double start) {
    const Clip* c = clip();
    if (!c) return false;
    const std::vector<Piece> ps = View(*c);
    if (i < 0 || i >= int(ps.size()) || ps[size_t(i)].whole()) return false;
    const double length = Length();
    const Piece& p = ps[size_t(i)];
    const double len = p.end - p.start;
    const double lo = i > 0 && !ps[size_t(i - 1)].whole() ? ps[size_t(i - 1)].end : 0.0;
    const double hi = i + 1 < int(ps.size()) && !ps[size_t(i + 1)].whole() ? ps[size_t(i + 1)].start : length;
    const double a = std::clamp(start, lo, std::max(lo, hi - len));
    SetTimes(i, a, a + len);
    return true;
  }

  void SetTimes(int i, double start, double end) {
    const Clip* c = clip();
    if (!c) return;
    if (OwnTimes()) {
      const std::vector<Piece> view = View(*c);
      if (i < 0 || i >= int(view.size()) || view[size_t(i)].whole()) return;
      VoiceTiming& timing = Timing();
      auto& list = timing.times[KeyFor(*c)];
      if (list.size() != view.size()) {
        list.assign(view.size(), {-1.0, -1.0});
        for (size_t k = 0; k < view.size(); ++k) list[k] = {view[k].start, view[k].end};
      }
      for (size_t k = 0; k < view.size(); ++k) {
        if (list[k].first < 0) list[k] = {view[k].start, view[k].end};
      }
      list[size_t(i)] = {start, end};
      timing.edited.insert(KeyFor(*c));
      return;
    }
    std::vector<Piece>* ps = Change(false);
    if (ps && i >= 0 && i < int(ps->size())) {
      (*ps)[size_t(i)].start = start;
      (*ps)[size_t(i)].end = end;
    }
  }

  // Pieces are shared by every voice: changed with the English voices only.
  bool StructureLocked() {
    if (!OwnTimes()) return false;
    Status("Split, add, merge and delete pieces with the English voices: the pieces are shared by every "
           "voice language (here only their times and words change).", true);
    return true;
  }

  // ---- changes ---------------------------------------------------------------

  // The selected line's pieces, ready to change: the state before goes on the
  // undo list (unless `remember` is false: a text box already remembered it),
  // a whole line becomes a piece over the whole sound, and the line counts
  // as edited (saved by Save).
  std::vector<Piece>* Change(bool remember = true) {
    TextFile* t = text();
    const Clip* c = clip();
    if (!t || !c) return nullptr;
    if (remember) Remember();
    std::vector<Piece>& pieces =
        c->movie.empty() ? t->sounds[c->id] : t->movies[MovieKeyIn(*t, c->movie)];
    for (Piece& p : pieces) {
      if (p.whole()) {
        p.start = 0;
        p.end = Length();
      }
    }
    if (c->movie.empty()) t->edited.insert(c->id);
    else t->edited_movies.insert(MovieKeyIn(*t, c->movie));
    return &pieces;
  }

  struct Undo {
    int text_index = 0;
    uint64_t id = 0;
    std::string movie;      // a cartoon's key in the text file (id = 0)
    bool existed = false;   // the sound had an entry
    bool was_edited = false;
    std::vector<Piece> pieces;
    // The picked voice's own times of the line (another voice than English).
    std::string voice;
    bool timing_existed = false, timing_was_edited = false;
    std::vector<std::pair<double, double>> timing;
  };
  // UNDO AND REDO: an entry = one line's pieces (and the picked voice's own
  // times of it) as they were. Undo puts the entry back and keeps the state
  // it replaced on the redo list; redo does the reverse. A new change empties
  // the redo list (the usual rule: a new step forgets the undone ones).

  // The selected line as it is now.
  std::optional<Undo> Snapshot() {
    TextFile* t = text();
    const Clip* c = clip();
    if (!t || !c) return std::nullopt;
    Undo u;
    u.text_index = text_index_;
    u.id = c->id;
    if (!c->movie.empty()) u.movie = MovieKeyIn(*t, c->movie);
    if (OwnTimes()) u.voice = Timing().voice;
    return Capture(u);
  }
  // The line `where` names (text file, sound / movie, voice) as it is now.
  Undo Capture(const Undo& where) {
    Undo u;
    u.text_index = where.text_index;
    u.id = where.id;
    u.movie = where.movie;
    u.voice = where.voice;
    if (u.text_index >= int(texts_.size())) return u;
    TextFile& t = texts_[size_t(u.text_index)];
    if (!u.movie.empty()) {
      const auto it = t.movies.find(u.movie);
      u.existed = it != t.movies.end();
      if (u.existed) u.pieces = it->second;
      u.was_edited = t.edited_movies.count(u.movie) != 0;
    } else {
      const auto it = t.sounds.find(u.id);
      u.existed = it != t.sounds.end();
      if (u.existed) u.pieces = it->second;
      u.was_edited = t.edited.count(u.id) != 0;
    }
    if (!u.voice.empty()) {
      if (const auto it = timings_.find(u.voice); it != timings_.end()) {
        const std::string tkey = u.movie.empty() ? KeyOf(u.id) : u.movie;
        const auto tt = it->second.times.find(tkey);
        u.timing_existed = tt != it->second.times.end();
        if (u.timing_existed) u.timing = tt->second;
        u.timing_was_edited = it->second.edited.count(tkey) != 0;
      }
    }
    return u;
  }
  void Push(Undo u) {
    undo_.push_back(std::move(u));
    if (undo_.size() > 300) undo_.erase(undo_.begin());
    redo_before_ = std::move(redo_);  // kept for Forget()
    redo_.clear();
  }
  // Before a change: the selected line goes on the undo list.
  void Remember() {
    if (auto u = Snapshot()) Push(std::move(*u));
  }
  // A change that didn't happen after all (a split too near an edge): its
  // undo entry goes, the redo list comes back.
  void Forget() {
    if (!undo_.empty()) undo_.pop_back();
    redo_ = std::move(redo_before_);
    redo_before_.clear();
  }
  // Puts an entry's state back into the text file (and the voice's times).
  void Restore(const Undo& u) {
    if (u.text_index >= int(texts_.size())) return;
    TextFile& t = texts_[size_t(u.text_index)];
    if (!u.movie.empty()) {
      if (u.existed) t.movies[u.movie] = u.pieces;
      else t.movies.erase(u.movie);
      if (u.was_edited) t.edited_movies.insert(u.movie);
      else t.edited_movies.erase(u.movie);
    } else {
      if (u.existed) t.sounds[u.id] = u.pieces;
      else t.sounds.erase(u.id);
      if (u.was_edited) t.edited.insert(u.id);
      else t.edited.erase(u.id);
    }
    const std::string tkey = u.movie.empty() ? KeyOf(u.id) : u.movie;
    if (!u.voice.empty()) {
      if (const auto it = timings_.find(u.voice); it != timings_.end()) {
        VoiceTiming& timing = it->second;
        if (u.timing_existed) timing.times[tkey] = u.timing;
        else timing.times.erase(tkey);
        if (u.timing_was_edited) timing.edited.insert(tkey);
        else timing.edited.erase(tkey);
      }
    }
    sel_piece_ = -1;
  }
  void UndoLast() {
    LeaveTextBox();
    if (undo_.empty()) {
      Status("Nothing to undo.");
      return;
    }
    Undo u = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back(Capture(u));
    Restore(u);
    Status("Undone (Ctrl+Y redoes it).");
  }
  void RedoLast() {
    LeaveTextBox();
    if (redo_.empty()) {
      Status("Nothing to redo.");
      return;
    }
    Undo u = std::move(redo_.back());
    redo_.pop_back();
    undo_.push_back(Capture(u));
    Restore(u);
    Status("Redone.");
  }
  // A text box being typed in lets go (its words are already in the piece),
  // so Ctrl+Z / Ctrl+Y act on the whole editor and the box then shows the
  // restored words. ImGui's own per-box undo is not used.
  void LeaveTextBox() {
    if (ImGui::GetCurrentContext() && ImGui::GetActiveID() != 0) ImGui::ClearActiveID();
    box_undo_.reset();
  }

  // The piece under `t` seconds, or -1.
  int PieceAt(double t) {
    if (!clip()) return -1;
    const std::vector<Piece> view = View(*clip());
    const auto* pieces = &view;
    for (int i = 0; i < int(pieces->size()); ++i) {
      const Piece& p = (*pieces)[size_t(i)];
      const double a = p.whole() ? 0 : p.start, b = p.whole() ? Length() : p.end;
      if (t >= a && t < b) return i;
    }
    return -1;
  }

  // Splits the piece under `t` there: the first part keeps the start and
  // ALL the text, the second (from `t` to the end, same speaker) starts
  // EMPTY and selected, its text box ready for its own words.
  void SplitAt(double t) {
    if (StructureLocked()) return;
    const int i = PieceAt(t);
    if (i < 0) {
      Status("No piece under the cursor to split.");
      return;
    }
    std::vector<Piece>* pieces = Change();
    Piece& a = (*pieces)[size_t(i)];
    if (t - a.start < kMinPiece || a.end - t < kMinPiece) {
      Forget();
      Status("Too close to the piece's edge to split.");
      return;
    }
    Piece b = a;
    a.end = t;
    b.text.clear();
    b.start = t;
    pieces->insert(pieces->begin() + i + 1, b);
    sel_piece_ = i + 1;
    focus_text_ = i + 1;
    Status("Split at " + FormatTime(t) + ": type the new piece's text.");
  }

  // A new empty piece at `t`, up to 2 s long (never over the next piece or
  // the sound's end), with the nearest piece's speaker.
  void AddAt(double t) {
    if (StructureLocked()) return;
    const double length = Length();
    if (length <= 0 || PieceAt(t) >= 0) {
      Status("There is a piece there already: split it instead.");
      return;
    }
    std::vector<Piece>* pieces = Change();
    Piece p;
    p.start = std::clamp(t, 0.0, std::max(0.0, length - kMinPiece));
    p.end = std::min(p.start + 2.0, length);
    for (const Piece& q : *pieces) {
      if (q.start > p.start) p.end = std::min(p.end, q.start);
    }
    if (p.end - p.start < kMinPiece) {
      Forget();
      Status("No room for a piece there.");
      return;
    }
    // The speaker of the nearest piece, else (one known speaker) the folder's.
    double best = 1e9;
    for (const Piece& q : *pieces) {
      const double d = std::min(std::abs(q.end - p.start), std::abs(q.start - p.end));
      if (d < best) {
        best = d;
        p.speaker = q.speaker;
      }
    }
    pieces->push_back(p);
    std::sort(pieces->begin(), pieces->end(), [](const Piece& x, const Piece& y) { return x.start < y.start; });
    for (int i = 0; i < int(pieces->size()); ++i) {
      if ((*pieces)[size_t(i)].start == p.start) sel_piece_ = i;
    }
    Status("New piece at " + FormatTime(p.start) + ": type its text below.");
  }

  void DeletePiece(int i) {
    if (StructureLocked()) return;
    const auto* now = clip() ? PiecesOf(*clip()) : nullptr;
    if (!now || i < 0 || i >= int(now->size())) return;
    std::vector<Piece>* pieces = Change();
    pieces->erase(pieces->begin() + i);
    sel_piece_ = std::min(i, int(pieces->size()) - 1);
    Status("Piece deleted (Ctrl+Z brings it back).");
  }

  void MergeWithNext(int i) {
    if (StructureLocked()) return;
    const auto* now = clip() ? PiecesOf(*clip()) : nullptr;
    if (!now || i < 0 || i + 1 >= int(now->size())) return;
    std::vector<Piece>* pieces = Change();
    Piece& a = (*pieces)[size_t(i)];
    const Piece& b = (*pieces)[size_t(i + 1)];
    a.end = b.end;
    if (!b.text.empty()) a.text = a.text.empty() ? b.text : a.text + " " + b.text;
    pieces->erase(pieces->begin() + i + 1);
    sel_piece_ = i;
    Status("Merged.");
  }

  // Saves the picked text language's file and every voice's times that
  // changed, into the chosen place (the repository's subtitles/ or the
  // player's user/subtitles/; the times under timing/).
  void Save() {
    const fs::path dir = save_to_source_ && !source_dir_.empty() ? source_dir_ : user_dir_;
    std::string done, error;
    TextFile* t = text();
    if (t && t->Edits()) {
      const int n = int(t->Edits());
      const fs::path target = dir / (t->code + ".tsv");
      if (!SaveTextFile(*t, target, backup_dir_, &error)) {
        Status("NOT saved: " + error, true);
        return;
      }
      t->players_own = dir == user_dir_;
      done += std::to_string(n) + " line(s) to " + target.string();
    }
    for (auto& [voice, timing] : timings_) {
      if (timing.edited.empty()) continue;
      const int n = int(timing.edited.size());
      const fs::path target = dir / "timing" / (voice + ".tsv");
      if (!SaveVoiceTiming(timing, target, backup_dir_, &error)) {
        Status("NOT saved: " + error, true);
        return;
      }
      timing.players_own = dir == user_dir_;
      done += (done.empty() ? "" : ", ") + std::to_string(n) + " " + voice + " time(s) to " + target.string();
    }
    for (auto& [code, file] : checks_) {
      if (!file.edited) continue;
      const fs::path target = dir / "checked" / (code + ".tsv");
      if (!SaveCheckFile(file, target, backup_dir_, &error)) {
        Status("NOT saved: " + error, true);
        return;
      }
      done += (done.empty() ? "" : ", ") + std::string("the ") + code + " checks to " + target.string();
    }
    if (unused_.edited) {
      const fs::path target = dir / "unused.tsv";
      if (!SaveUnusedFile(unused_, target, backup_dir_, &error)) {
        Status("NOT saved: " + error, true);
        return;
      }
      done += (done.empty() ? "" : ", ") + std::string("the unused marks to ") + target.string();
    }
    Status(done.empty() ? std::string("Nothing to save.") : "Saved " + done + ".");
  }

  void Status(std::string text, bool bad = false) {
    status_ = std::move(text);
    status_bad_ = bad;
    status_time_ = Clock::now();
  }

  void CollectSpeakers() {
    speakers_.clear();
    for (const TextFile& t : texts_) {
      for (const auto& [id, pieces] : t.sounds) {
        for (const Piece& p : pieces) {
          if (!p.speaker.empty() &&
              std::find(speakers_.begin(), speakers_.end(), p.speaker) == speakers_.end()) {
            speakers_.push_back(p.speaker);
          }
        }
      }
    }
    std::sort(speakers_.begin(), speakers_.end());
  }

  // ---- drawing ---------------------------------------------------------------

  // Right-aligns the next `width` pixels of the row (or just goes on when
  // there's no room).
  static void AlignRight(float width) {
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    const float x = right - width;
    ImGui::SameLine();
    if (x > ImGui::GetCursorScreenPos().x) ImGui::SetCursorScreenPos(ImVec2(x, ImGui::GetCursorScreenPos().y));
  }

  // THE TOP BAR, two rows: what is shown (voices, text language) and the
  // file actions (save target, save, undo, reload) / searching and
  // filtering the list, and who checks.
  void DrawTopBar() {
    const float font = ImGui::GetFontSize();
    const std::string voice_name =
        archives_.empty() ? "none" : archives_[size_t(voice_index_)].stem().string();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Voices");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(font * 8);
    if (ImGui::BeginCombo("##voices", voice_name.c_str())) {
      for (int i = 0; i < int(archives_.size()); ++i) {
        if (ImGui::Selectable(archives_[size_t(i)].stem().string().c_str(), i == voice_index_) &&
            i != voice_index_) {
          LoadVoices(i, clip() ? clip()->id : 0);
        }
      }
      ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The voice language you listen to (its own times)");
    ImGui::SameLine(0, font * 1.2f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Text");
    ImGui::SameLine();
    TextFile* t = text();
    ImGui::SetNextItemWidth(font * 6);
    if (ImGui::BeginCombo("##text", t ? t->code.c_str() : "none")) {
      for (int i = 0; i < int(texts_.size()); ++i) {
        const std::string label = texts_[size_t(i)].code + (texts_[size_t(i)].Edits() ? " *" : "");
        if (ImGui::Selectable(label.c_str(), i == text_index_)) {
          text_index_ = i;
          sel_piece_ = -1;
          Refilter();
        }
      }
      ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("The subtitles' language (the file %s)", t ? t->path.string().c_str() : "-");
    }

    // Right: where to save, Save, Undo, Reload.
    int unsaved = t ? int(t->Edits()) : 0;
    for (const auto& [code, file] : checks_) unsaved += file.edited ? 1 : 0;
    unsaved += unused_.edited ? 1 : 0;
    for (const auto& [voice, timing] : timings_) unsaved += int(timing.edited.size());
    const std::string save_label = unsaved ? "Save (" + std::to_string(unsaved) + ")" : std::string("Saved");
    const float h = ImGui::GetFrameHeight();
    const float save_w = h + ImGui::CalcTextSize(save_label.c_str()).x + ImGui::GetStyle().FramePadding.x;
    const float target_w = source_dir_.empty() ? 0 : font * 10 + ImGui::GetStyle().ItemSpacing.x;
    AlignRight(target_w + save_w + 2 * h + 3 * ImGui::GetStyle().ItemSpacing.x);
    if (!source_dir_.empty()) {
      ImGui::SetNextItemWidth(font * 10);
      const char* targets[] = {"To the repository", "To my own file"};
      int target = save_to_source_ ? 0 : 1;
      if (ImGui::Combo("##target", &target, targets, 2)) save_to_source_ = target == 0;
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Where Save writes.\nThe repository: %s\n  (commit it to share it)\n"
                          "My own file: %s\n  (only this PC; the game reads it instead of the shipped one)",
                          source_dir_.string().c_str(), user_dir_.string().c_str());
      }
      ImGui::SameLine();
    }
    ImGui::BeginDisabled(unsaved == 0);
    if (IconButton("save", Icon::kSave, "Save every change (Ctrl+S)", save_label.c_str(), unsaved > 0)) Save();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(undo_.empty());
    if (IconButton("undo", Icon::kUndo, "Undo (Ctrl+Z)")) UndoLast();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(redo_.empty());
    if (IconButton("redo", Icon::kRedo, "Redo (Ctrl+Y or Ctrl+Shift+Z)")) RedoLast();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (IconButton("reload", Icon::kReload, "Read the subtitle files again")) {
      if (Unsaved()) ImGui::OpenPopup("Reload?");
      else {
        ReloadTexts();
        Refilter();
      }
    }
    if (ImGui::BeginPopupModal("Reload?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::Text("Throw away %d unsaved change(s) and read the files again?", Unsaved());
      if (ImGui::Button("Throw away and reload")) {
        ReloadTexts();
        Refilter();
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }

    // Row 2: search, filter, help; right: who checks.
    IconLabel(Icon::kSearch);
    ImGui::SetNextItemWidth(font * 14);
    if (ImGui::InputTextWithHint("##search", "file, speaker, ID or words", search_, sizeof(search_))) Refilter();
    ImGui::SameLine();
    const int filters = int(in_scope_only_) + int(without_text_only_) + int(unchecked_only_) + int(hide_unused_);
    const std::string filter_label = filters ? "Filter (" + std::to_string(filters) + ")" : std::string("Filter");
    if (IconButton("filter", Icon::kFilter, "Which lines the list shows", filter_label.c_str(), filters > 0)) {
      ImGui::OpenPopup("filters");
    }
    if (ImGui::BeginPopup("filters")) {
      bool changed = false;
      changed |= ImGui::Checkbox("Only lines the game subtitles", &in_scope_only_);
      changed |= ImGui::Checkbox("Only lines without text", &without_text_only_);
      changed |= ImGui::Checkbox("Only lines not checked", &unchecked_only_);
      changed |= ImGui::Checkbox("Hide lines marked unused", &hide_unused_);
      if (changed) Refilter();
      ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (IconButton("help", Icon::kHelp, "Mouse and keys")) ImGui::OpenPopup("help");
    DrawHelp();
    AlignRight(h * 0.85f + font * 9 + ImGui::GetStyle().ItemInnerSpacing.x);
    IconLabel(Icon::kCheck);
    ImGui::SetNextItemWidth(font * 9);
    if (ImGui::InputTextWithHint("##checker", "your name", name_buf_, sizeof(name_buf_))) {
      checker_ = name_buf_;
      std::error_code ec;
      fs::create_directories(user_dir_, ec);
      std::ofstream(user_dir_ / "checker-name.txt", std::ios::trunc) << checker_;
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("Who marks lines as checked on this PC.\nSaved with every check (public if the checks "
                        "go into the repository): a nickname is fine.");
    }
  }

  void DrawHelp() {
    if (!ImGui::BeginPopup("help")) return;
    const auto row = [](const char* what, const char* how) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(what);
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%s", how);
    };
    if (ImGui::BeginTable("help_table", 2, ImGuiTableFlags_SizingFixedFit)) {
      row("Play from the cursor / stop", "Space");
      row("Play the whole line from the start", "Shift+Space");
      row("Play the selected piece", "Enter");
      row("Put the cursor there", "click the waveform (Space plays)");
      row("Select a piece", "click it on the waveform");
      row("Resize the selected piece", "drag its edge");
      row("Move the selected piece", "drag inside it");
      row("Split / new piece", "S / N, or right-click the waveform");
      row("Delete the selected piece", "Delete");
      row("Zoom / scroll / height", "Ctrl+wheel / wheel / Shift+wheel");
      row("Previous / next line", "Up / Down");
      row("Undo / redo", "Ctrl+Z / Ctrl+Y or Ctrl+Shift+Z");
      row("Save", "Ctrl+S");
      ImGui::EndTable();
    }
    ImGui::EndPopup();
  }

  // The bottom line: the last thing done (8 s), and the game folder.
  void DrawStatusBar() {
    ImGui::Separator();
    if (!status_.empty() && Clock::now() - status_time_ < std::chrono::seconds(8)) {
      ImGui::TextColored(status_bad_ ? kBadColour : ImVec4(0.45f, 0.82f, 0.45f, 1.0f), "%s", status_.c_str());
    } else {
      ImGui::TextDisabled("Game folder: %s", root_.string().c_str());
    }
  }

  void Refilter() {
    if (!List()) return;
    const Clip* was = clip();
    const std::string keep = was ? KeyFor(*was) : std::string();
    shown_.clear();
    const std::string needle = Lower(search_);
    const auto& clips = *List();
    for (size_t i = 0; i < clips.size(); ++i) {
      const Clip& c = clips[i];
      if (in_scope_only_ && !InScope(c.family)) continue;
      const auto* pieces = PiecesOf(c);
      if (without_text_only_ && pieces && !pieces->empty()) continue;
      if (unchecked_only_ && StateOf(c) == CheckState::kOk) continue;
      if (hide_unused_ && UnusedMark(c)) continue;
      if (!needle.empty()) {
        bool hit = Lower(c.name).find(needle) != std::string::npos || Hex(c.id).find(needle) == 0;
        if (pieces) {
          for (const Piece& p : *pieces) hit |= Lower(p.text).find(needle) != std::string::npos;
        }
        if (!hit) continue;
      }
      shown_.push_back(int(i));
    }
    selected_ = -1;
    for (size_t i = 0; i < shown_.size(); ++i) {
      if (!keep.empty() && KeyFor(clips[size_t(shown_[i])]) == keep) selected_ = int(i);
    }
  }

  void SelectRow(int row, bool scroll) {
    if (row < 0 || row >= int(shown_.size()) || row == selected_) return;
    selected_ = row;
    scroll_to_ = scroll;
    sel_piece_ = -1;
    cursor_ = -1;
    player_.Stop();
    pcm_ = {};
    decode_error_.clear();
    DecodeSelected();
  }

  // The selected line's sound: a voice clip, or a cartoon's dialogue track
  // in the picked voice language (+ its music and effects if asked).
  void DecodeSelected() {
    player_.Stop();
    pcm_ = {};
    decode_error_.clear();
    view_from_ = 0;
    view_len_ = 0;
    peaks_key_ = {};
    const Clip* c = clip();
    if (!c) return;
    video_.Close();
    if (!c->movie.empty()) {
      std::string video_error;
      if (!video_.Open(movies_.ArchiveOf(*c), *c, &video_error)) {
        std::printf("video: %s\n", video_error.c_str());
      }
      const auto t0 = Clock::now();
      movies_.Decode(*c, VoiceKey(), &stems_, &decode_error_);
      Remix();
      std::printf("decoded %s (%s) in %.2f s, %.1f s of sound\n", c->movie.c_str(), VoiceKey().c_str(),
                  std::chrono::duration<double>(Clock::now() - t0).count(),
                  pcm_.rate ? double(pcm_.frames()) / pcm_.rate : 0.0);
      std::fflush(stdout);
    } else if (archive_) {
      archive_->Decode(*c, &stems_, &decode_error_);
      Remix();
    }
  }

  void DrawList() {
    // Voice lines or cartoons.
    if (ImGui::BeginTabBar("kind")) {
      const bool was = cartoons_;
      // (force_tab_: the code picked the tab, e.g. --select=<movie>.)
      // ImGui moves to a tab asked for this way a frame later: until then
      // the tabs' answers are not the player's choice and are ignored.
      const bool forced = force_tab_;
      const ImGuiTabItemFlags voices_flags = forced && !cartoons_ ? ImGuiTabItemFlags_SetSelected : 0;
      const ImGuiTabItemFlags cartoons_flags = forced && cartoons_ ? ImGuiTabItemFlags_SetSelected : 0;
      force_tab_ = false;
      bool picked_cartoons = cartoons_;
      if (ImGui::BeginTabItem("Voice lines", nullptr, voices_flags)) {
        picked_cartoons = false;
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Cartoons", nullptr, cartoons_flags)) {
        picked_cartoons = true;
        ImGui::EndTabItem();
      }
      if (tab_settle_ > 0) --tab_settle_;
      else if (!forced) cartoons_ = picked_cartoons;
      if (forced) tab_settle_ = 2;
      ImGui::EndTabBar();
      if (was != cartoons_) {
        selected_ = -1;
        Refilter();
        SelectRow(0, true);
      }
    }
    ImGui::TextDisabled("%zu of %zu %s", shown_.size(), List()->size(), cartoons_ ? "cartoons" : "lines");
    DrawCheckCounter();
    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("lines", 6, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Speaker", 0, 1.0f);
    ImGui::TableSetupColumn("File", 0, 2.6f);
    ImGui::TableSetupColumn("Group", 0, 1.0f);
    ImGui::TableSetupColumn("Length", 0, 0.8f);
    ImGui::TableSetupColumn("Subs", 0, 0.6f);
    ImGui::TableSetupColumn("Checked", 0, 0.8f);
    ImGui::TableHeadersRow();
    TextFile* t = text();
    ImGuiListClipper clipper;
    clipper.Begin(int(shown_.size()));
    if (scroll_to_ && selected_ >= 0) clipper.IncludeItemByIndex(selected_);
    while (clipper.Step()) {
      for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
        const Clip& c = (*List())[size_t(shown_[size_t(row)])];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::PushID(row);
        if (ImGui::Selectable(c.speaker.c_str(), row == selected_,
                              ImGuiSelectableFlags_SpanAllColumns)) {
          SelectRow(row, false);
        }
        if (scroll_to_ && row == selected_) {
          ImGui::SetScrollHereY(0.4f);
          scroll_to_ = false;
        }
        ImGui::PopID();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(c.file.c_str());
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", FamilyName(c.family));
        ImGui::TableNextColumn();
        ImGui::Text("%.2f s", c.seconds());
        ImGui::TableNextColumn();
        const auto* pieces = PiecesOf(c);
        const bool edited = t && IsEdited(*t, c);
        if (pieces && !pieces->empty()) ImGui::Text("%zu%s", pieces->size(), edited ? " *" : "");
        else if (InScope(c.family)) ImGui::TextColored(kBadColour, "none%s", edited ? " *" : "");
        ImGui::TableNextColumn();
        const CheckState check = (pieces && !pieces->empty()) ? StateOf(c) : CheckState::kNone;
        if (UnusedMark(c)) {  // unused: grey, with its check state after it
          ImGui::TextColored(kUnusedColour, "unused%s",
                             check == CheckState::kOk ? " + yes" : check == CheckState::kChanged ? " + changed" : "");
        } else if (check == CheckState::kOk) ImGui::TextColored(ImVec4(0.45f, 0.82f, 0.45f, 1.0f), "yes");
        else if (check == CheckState::kChanged) ImGui::TextColored(kWarnColour, "changed");
      }
    }
    ImGui::EndTable();
  }

  // The visible part of the waveform as min / max per pixel column, made
  // again only when the view or the width changes.
  void BuildPeaks(double from, double len, int width) {
    const PeaksKey key{from, len, width};
    if (key == peaks_key_) return;
    peaks_key_ = key;
    peaks_.assign(size_t(std::max(width, 1)), {0.0f, 0.0f});
    if (pcm_.frames() == 0 || len <= 0 || width <= 0) return;
    for (int x = 0; x < width; ++x) {
      const size_t a = size_t(std::max(0.0, (from + len * x / width) * pcm_.rate));
      const size_t b = std::max(a + 1, size_t(std::max(0.0, (from + len * (x + 1) / width) * pcm_.rate)));
      float lo = 0, hi = 0;
      for (size_t f = a; f < b && f < pcm_.frames(); ++f) {
        const float v = 0.5f * (pcm_.stereo[f * 2] + pcm_.stereo[f * 2 + 1]);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
      }
      peaks_[size_t(x)] = {lo, hi};
    }
  }

  void DrawLine() {
    const Clip* c = clip();
    if (!c) {
      ImGui::TextDisabled("Pick a line on the left.");
      return;
    }
    const double length = Length();
    // The title: the line's file / the cartoon, its facts in grey after it.
    ImGui::TextUnformatted(c->movie.empty() ? c->file.c_str() : c->movie.c_str());
    ImGui::SameLine();
    if (!c->movie.empty()) {
      ImGui::TextDisabled("cartoon  %.1f s  %s dialogue", c->seconds(), VoiceKey().c_str());
    } else {
      ImGui::TextDisabled("%s  %.2f s  %s", FamilyName(c->family), c->seconds(), c->speaker.c_str());
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s\n%s%u Hz, %u stream(s)", c->name.c_str(),
                        c->movie.empty() ? ("sound ID " + Hex(c->id) + "\n").c_str() : "", c->rate, c->streams);
    }
    AlignRight(ImGui::GetFrameHeight());
    if (IconButton("line_menu", Icon::kMenu, "More for this line")) ImGui::OpenPopup("line_menu");
    DrawLineMenu(*c);
    if (!decode_error_.empty()) ImGui::TextColored(kWarnColour, "%s", decode_error_.c_str());
    DrawCheckRow(*c);
    if (OwnTimes()) {
      Times state;
      const std::vector<Piece> view = View(*c, &state);
      const std::string voice = VoiceKey();
      bool all_whole = !view.empty();
      for (const Piece& p : view) all_whole &= p.whole();
      if (view.empty()) {
      } else if (all_whole) {
        ImGui::TextDisabled("A whole line: it lasts as long as the %s sound, nothing to time.", voice.c_str());
      } else if (state == Times::kOwn) {
        ImGui::TextColored(ImVec4(0.45f, 0.82f, 0.45f, 1.0f), "Own %s times.", voice.c_str());
      } else if (state == Times::kNotMatching) {
        ImGui::TextColored(kWarnColour, "The %s times no longer fit the pieces: showing the English times.",
                           voice.c_str());
      } else {
        ImGui::TextColored(kWarnColour, "No %s times yet: showing the English times (drag an edge to change).",
                           voice.c_str());
      }
    }
    ImGui::Spacing();

    // THE TOOLBAR: playing | volume | split, new | zoom, height | preview.
    const bool playing = player_.playing();
    if (IconButton("from_start", Icon::kPlayFromStart, "Play the whole line from the start (Shift+Space)")) {
      player_.Play(pcm_, 0);
    }
    ImGui::SameLine();
    if (IconButton("play", Icon::kPlay, "Play from the cursor (Space)", nullptr, playing)) {
      player_.Play(pcm_, cursor_ > 0 ? cursor_ : 0);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!playing);
    if (IconButton("stop", Icon::kStop, "Stop (Space)")) player_.Stop();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s / %s", FormatTime(playing ? player_.Position() : std::max(0.0, cursor_)).c_str(),
                FormatTime(length).c_str());
    const float h = ImGui::GetFrameHeight(), em = ImGui::GetFontSize();
    const float sp = ImGui::GetStyle().ItemSpacing.x;
    const auto labelled = [&](const char* text) { return h + ImGui::CalcTextSize(text).x + ImGui::GetStyle().FramePadding.x; };
    ToolbarGap(h * 0.85f + em * 6);
    IconLabel(Icon::kVolume);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    int percent = int(std::lround(player_.volume() * 100.0f));
    if (ImGui::SliderInt("##volume", &percent, 0, 200, "%d %%")) player_.SetVolume(percent / 100.0f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Volume");
    ToolbarGap(labelled("Split") + sp + labelled("New"));
    if (IconButton("split", Icon::kSplit, "Split the piece at the play head / cursor (S)", "Split")) {
      SplitAt(SplitPoint());
    }
    ImGui::SameLine();
    if (IconButton("new", Icon::kAdd, "A new piece at the cursor (N)", "New")) AddAt(std::max(0.0, cursor_));
    ToolbarGap(h * 3 + em * 6 + sp * 3);
    DrawZoomRow(length);


    // The preview (the picture with the subtitles as they'd show at the play
    // head / cursor), then the waveform, then the pieces; dividers between.
    const float font = ImGui::GetFontSize();
    if (show_preview_) {
      if (preview_h_ <= 0) preview_h_ = ImGui::GetContentRegionAvail().y * 0.36f;
      ImGui::BeginChild("preview", ImVec2(0, preview_h_), ImGuiChildFlags_None,
                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
      DrawPreview(*c, length);
      ImGui::EndChild();
      Splitter("##preview_split", false, &preview_h_, font * 4, ImGui::GetContentRegionAvail().y + preview_h_ - font * 12);
    }
    if (wave_h_ <= 0) wave_h_ = font * 9;
    DrawWaveform(*c, length);
    Splitter("##wave_split", false, &wave_h_, font * 3, wave_h_ + ImGui::GetContentRegionAvail().y - font * 5);
    DrawPieces(*c, length);
  }

  // A part's name in the mixer: the dialogue is one part whatever language.
  static std::string MixKey(const std::string& name) {
    return name.starts_with("Dialogue") ? std::string("Dialogue") : name;
  }

  // pcm_ (what plays and what the waveform shows) = the parts that are on,
  // each at its game balance x its volume. A playing line goes on from the
  // same moment with the new mix.
  void Remix() {
    const bool playing = player_.playing();
    const double at = playing ? player_.Position() : 0;
    pcm_.rate = stems_.rate;
    size_t samples = 0;
    for (const Stem& part : stems_.parts) samples = std::max(samples, part.stereo.size());
    pcm_.stereo.assign(samples, 0.0f);
    for (const Stem& part : stems_.parts) {
      const auto it = mixer_.find(MixKey(part.name));
      const bool chosen = it != mixer_.end() && it->second.chosen;
      const bool on = chosen ? it->second.on : part.on;
      if (!on) continue;
      const float gain = part.balance * (it != mixer_.end() ? it->second.percent : 100) / 100.0f;
      for (size_t i = 0; i < part.stereo.size(); ++i) pcm_.stereo[i] += part.stereo[i] * gain;
    }
    for (float& v : pcm_.stereo) v = std::clamp(v, -1.0f, 1.0f);
    peaks_key_ = {};
    if (playing) player_.Play(pcm_, at);
  }

  // The mixer row (sounds with more than one part): each part on / off and
  // its volume (0-200 %), applied at once.
  void DrawMixer() {
    if (stems_.parts.size() < 2) return;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Parts:");
    bool changed = false;
    for (const Stem& part : stems_.parts) {
      MixPart& m = mixer_[MixKey(part.name)];
      if (!m.chosen) m.on = part.on;
      // Beside the previous part when it fits, else on the next row.
      const ImGuiStyle& style = ImGui::GetStyle();
      const float needed = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x +
                           ImGui::CalcTextSize(part.name.c_str()).x + style.ItemSpacing.x +
                           ImGui::GetFontSize() * 5.5f;
      const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
      if (ImGui::GetItemRectMax().x + ImGui::GetFontSize() + needed <= right) {
        ImGui::SameLine(0, ImGui::GetFontSize());
      }
      ImGui::PushID(part.name.c_str());
      if (ImGui::Checkbox(part.name.c_str(), &m.on)) {
        m.chosen = true;
        changed = true;
      }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.5f);
      ImGui::BeginDisabled(!m.on);
      if (ImGui::SliderInt("##level", &m.percent, 0, 200, "%d%%")) {
        m.chosen = true;
        changed = true;
      }
      ImGui::EndDisabled();
      ImGui::PopID();
    }
    if (changed) Remix();
  }

  // ZOOM: how many seconds the waveform shows (time stretch: - / + / Fit,
  // or the slider) and how tall its peaks are drawn (Height). The wheel
  // over the waveform does the same (Ctrl+wheel: zoom, Shift+wheel: height;
  // the wheel alone scrolls).
  void DrawZoomRow(double length) {
    if (length <= 0) return;
    const double shown = view_len_ > 0 ? std::min(view_len_, length) : length;
    const auto zoom_to = [&](double len) {
      len = std::clamp(len, std::min(length, 0.5), length);
      const double centre = (cursor_ >= view_from_ && cursor_ <= view_from_ + shown) ? cursor_
                                                                                     : view_from_ + shown * 0.5;
      view_from_ = centre - len * 0.5;
      view_len_ = len;
    };
    if (IconButton("zoom_out", Icon::kZoomOut, "Zoom out (Ctrl+wheel)")) zoom_to(shown * 1.5);
    ImGui::SameLine();
    // The slider = seconds shown, on a log scale (0.5 s .. the whole sound).
    float seconds = float(shown);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
    if (ImGui::SliderFloat("##zoom", &seconds, float(std::min(length, 0.5)), float(length), "%.1f s",
                           ImGuiSliderFlags_Logarithmic)) {
      zoom_to(seconds);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Seconds the waveform shows");
    ImGui::SameLine();
    if (IconButton("zoom_in", Icon::kZoomIn, "Zoom in (Ctrl+wheel)")) zoom_to(shown / 1.5);
    ImGui::SameLine();
    if (IconButton("fit", Icon::kFit, "Show the whole line")) {
      view_from_ = 0;
      view_len_ = length;
    }
    const float h = ImGui::GetFrameHeight();
    ToolbarGap(h * 0.85f + ImGui::GetFontSize() * 4.5f);
    IconLabel(Icon::kHeight);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.5f);
    ImGui::SliderFloat("##height", &amp_, 0.5f, 16.0f, "%.1fx", ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Waveform height (Shift+wheel): for quiet speech");
    ToolbarGap(h);
    if (IconButton("preview", Icon::kPreview, "Show / hide the preview (picture + subtitles)", nullptr,
                   show_preview_)) {
      show_preview_ = !show_preview_;
    }
  }

  // THE PREVIEW: the picture (a cartoon's frame at that moment, else black)
  // in 16:9 with the subtitles the game would show at the play head (or the
  // cursor when stopped): the pieces under that moment, the newest at the
  // bottom, at most two, wrapped at 70 % of the picture's width, white with
  // a dark outline and the speaker in yellow over a dark box, like the game
  // (src/subtitles/subtitles.cpp; drawn here in the desktop's font, not the
  // game's).
  void DrawPreview(const Clip& c, double length) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    float w = avail.x, h = avail.x * 9.0f / 16.0f;
    if (h > avail.y) {
      h = avail.y;
      w = h * 16.0f / 9.0f;
    }
    const ImVec2 p0(ImGui::GetCursorScreenPos().x + (avail.x - w) * 0.5f, ImGui::GetCursorScreenPos().y);
    const ImVec2 p1(p0.x + w, p0.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 255));
    const double t = player_.playing() ? player_.Position() : std::max(0.0, cursor_);
    // The cartoon's picture at t.
    if (video_.open()) {
      const uint32_t n = uint32_t(std::max(0.0, t * video_.fps()));
      if (const uint8_t* rgba = video_.Frame(n)) {
        if (!video_texture_ || video_tex_w_ != video_.width() || video_tex_h_ != video_.height()) {
          if (video_texture_) SDL_DestroyTexture(video_texture_);
          video_texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                             video_.width(), video_.height());
          video_tex_w_ = video_.width();
          video_tex_h_ = video_.height();
          shown_frame_ = UINT32_MAX;
        }
        if (video_texture_ && shown_frame_ != video_.current()) {
          SDL_UpdateTexture(video_texture_, nullptr, rgba, video_.width() * 4);
          shown_frame_ = video_.current();
        }
        if (video_texture_) dl->AddImage(ImTextureID(reinterpret_cast<intptr_t>(video_texture_)), p0, p1);
      }
    } else {
      dl->AddText(ImVec2(p0.x + 8, p0.y + 6), IM_COL32(90, 90, 100, 255), "(a voice line: no picture)");
    }
    dl->AddText(ImVec2(p0.x + 8, p1.y - ImGui::GetFontSize() - 6), IM_COL32(200, 200, 200, 160),
                FormatTime(t).c_str());
    ImGui::Dummy(avail);

    // The subtitles at t.
    std::vector<const Piece*> now;
    const std::vector<Piece> view = View(c);
    for (const Piece& p : view) {
      const double a = p.whole() ? 0 : p.start, b = p.whole() ? length : p.end;
      if (t >= a && t < b && !p.text.empty()) now.push_back(&p);
    }
    if (now.size() > 2) now.erase(now.begin(), now.end() - 2);
    if (now.empty()) return;
    ImFont* font = ImGui::GetFont();
    const float size = h * 0.042f;  // ~ the game's Medium size on a 720p picture
    const float max_w = w * 0.70f;
    const ImU32 white = IM_COL32(255, 255, 255, 255), yellow = IM_COL32(255, 217, 64, 255);
    // Rows: (speaker part, text part) per row, wrapped by words.
    struct Row {
      std::string speaker, text;
    };
    std::vector<Row> rows;
    for (const Piece* p : now) {
      std::string label = p->speaker.empty() ? "" : p->speaker + ": ";
      std::string row_text;
      float row_w = font->CalcTextSizeA(size, FLT_MAX, 0, label.c_str()).x;
      const auto flush = [&]() {
        rows.push_back({label, row_text});
        label.clear();
        row_text.clear();
        row_w = 0;
      };
      size_t i = 0;
      const std::string& text = p->text;
      while (i < text.size()) {
        if (text[i] == '\n') {
          flush();
          ++i;
          continue;
        }
        size_t j = i;
        while (j < text.size() && text[j] != ' ' && text[j] != '\n') ++j;
        const std::string word = text.substr(i, j - i);
        const std::string piece = row_text.empty() ? word : " " + word;
        const float pw = font->CalcTextSizeA(size, FLT_MAX, 0, piece.c_str()).x;
        if (row_w + pw > max_w && row_w > 0) {
          flush();
          row_text = word;
          row_w = font->CalcTextSizeA(size, FLT_MAX, 0, word.c_str()).x;
        } else {
          row_text += piece;
          row_w += pw;
        }
        i = j;
        while (i < text.size() && text[i] == ' ') ++i;
      }
      flush();
    }
    const float line_h = size * 1.15f;
    const float bottom = p0.y + h * 0.89f;
    float top = bottom - line_h * float(rows.size());
    float widest = 0;
    for (const Row& r : rows) {
      widest = std::max(widest, font->CalcTextSizeA(size, FLT_MAX, 0, (r.speaker + r.text).c_str()).x);
    }
    const float cx = (p0.x + p1.x) * 0.5f;
    dl->AddRectFilled(ImVec2(cx - widest * 0.5f - size * 0.5f, top - size * 0.15f),
                      ImVec2(cx + widest * 0.5f + size * 0.5f, bottom + size * 0.15f), IM_COL32(0, 0, 0, 150),
                      size * 0.25f);
    for (const Row& r : rows) {
      const float rw = font->CalcTextSizeA(size, FLT_MAX, 0, (r.speaker + r.text).c_str()).x;
      float x = cx - rw * 0.5f;
      const auto outlined = [&](const std::string& s, ImU32 colour) {
        if (s.empty()) return;
        const float o = std::max(1.0f, size * 0.06f);
        for (int dx = -1; dx <= 1; ++dx) {
          for (int dy = -1; dy <= 1; ++dy) {
            if (dx || dy) dl->AddText(font, size, ImVec2(x + dx * o, top + dy * o), IM_COL32(0, 0, 0, 220), s.c_str());
          }
        }
        dl->AddText(font, size, ImVec2(x, top), colour, s.c_str());
        x += font->CalcTextSizeA(size, FLT_MAX, 0, s.c_str()).x;
      };
      outlined(r.speaker, yellow);
      outlined(r.text, white);
      top += line_h;
    }
  }

  // ---- checks: who looked at a line by hand (subtitle_texts.h) ------------

  enum class CheckState { kNone, kOk, kChanged };
  CheckFile* Checks() {
    TextFile* t = text();
    return t ? &checks_[t->code] : nullptr;
  }
  // The line's check for the picked voice, and whether the line is still as
  // it was when checked (the fingerprint of the pieces as shown).
  CheckState StateOf(const Clip& c, const Check** found = nullptr) {
    CheckFile* file = Checks();
    if (!file) return CheckState::kNone;
    const auto it = file->checks.find({KeyFor(c), VoiceKey()});
    if (it == file->checks.end()) return CheckState::kNone;
    if (found) *found = &it->second;
    return it->second.fingerprint == Fingerprint(View(c)) ? CheckState::kOk : CheckState::kChanged;
  }
  // THE CHECK COUNTERS (above the list), for the picked voice in this tab:
  // green = the lines the game shows (in scope: not combat / credits; not
  // marked unused), purple = the lines marked unused (checked on their own).
  // Each: how many checked and still as checked, how many left (lines
  // checked but changed since count as left).
  void DrawCheckCounter() {
    if (!List()) return;
    // [0] = lines the game uses, [1] = lines marked unused: checked / changed / all.
    size_t total[2] = {}, done[2] = {}, changed[2] = {};
    for (const Clip& c : *List()) {
      const int u = UnusedMark(c) ? 1 : 0;
      if (!u && !InScope(c.family)) continue;  // unused marks count whatever their group
      ++total[u];
      const CheckState state = StateOf(c);
      if (state == CheckState::kOk) ++done[u];
      else if (state == CheckState::kChanged) ++changed[u];
    }
    const char* what = cartoons_ ? "cartoons" : "lines";
    for (int u = 0; u < 2; ++u) {
      if (total[u] == 0) continue;
      char over[96];
      std::snprintf(over, sizeof(over), "%s%zu of %zu  (%zu left)", u ? "Unused: checked " : "Checked ", done[u],
                    total[u], total[u] - done[u]);
      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, u ? ImVec4(0.42f, 0.42f, 0.62f, 1.0f)
                                                      : ImVec4(0.30f, 0.62f, 0.32f, 1.0f));
      ImGui::ProgressBar(float(done[u]) / float(total[u]), ImVec2(-1, 0), over);
      ImGui::PopStyleColor();
      if (ImGui::IsItemHovered()) {
        if (u) {
          ImGui::SetTooltip("%s marked unused (the game never plays them), with the %s voices:\n"
                            "%zu checked, %zu changed since their check, %zu never checked.",
                            what, VoiceKey().c_str(), done[u], changed[u], total[u] - done[u] - changed[u]);
        } else {
          ImGui::SetTooltip("%s the game shows (not combat or credits, not marked unused), with the %s voices:\n"
                            "%zu checked, %zu changed since their check, %zu never checked.",
                            what, VoiceKey().c_str(), done[u], changed[u], total[u] - done[u] - changed[u]);
        }
      }
    }
  }

  // UNUSED MARKS (subtitle_texts.h): one list for every language.
  const Unused* UnusedMark(const Clip& c) const {
    const auto it = unused_.marks.find(KeyFor(c));
    return it == unused_.marks.end() ? nullptr : &it->second;
  }
  void MarkUnused(bool on) {
    const Clip* c = clip();
    if (!c) return;
    if (!on) {
      unused_.marks.erase(KeyFor(*c));
      unused_.edited = true;
      Status("No longer marked unused.");
      return;
    }
    if (checker_.empty()) {
      Status("Type your name (top right) first: a mark says who made it.", true);
      return;
    }
    unused_.marks[KeyFor(*c)] = Unused{checker_, TodayUtc()};
    unused_.edited = true;
    Status("Marked unused by " + checker_ + " (all languages). Save (Ctrl+S) to keep it.");
  }

  void MarkChecked(bool on) {
    const Clip* c = clip();
    CheckFile* file = Checks();
    if (!c || !file) return;
    const std::pair<std::string, std::string> key{KeyFor(*c), VoiceKey()};
    if (!on) {
      file->checks.erase(key);
      file->edited = true;
      Status("Check removed.");
      return;
    }
    if (checker_.empty()) {
      Status("Type your name (top right) first: a check says who checked.", true);
      return;
    }
    file->checks[key] = Check{checker_, TodayUtc(), Fingerprint(View(*c))};
    file->edited = true;
    Status("Checked by " + checker_ + ". Save (Ctrl+S) to keep it.");
  }

  // "..." for the line: less used actions.
  void DrawLineMenu(const Clip& c) {
    if (!ImGui::BeginPopup("line_menu")) return;
    if (c.movie.empty() && ImGui::MenuItem("Copy the sound ID", Hex(c.id).c_str())) {
      ImGui::SetClipboardText(Hex(c.id).c_str());
    }
    ImGui::SeparatorText("Other subtitle programs");
    if (ImGui::MenuItem("Export .srt + .wav")) ExportSrt();
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("This line's subtitles (.srt) and its sound as you hear it (.wav), into\n%s",
                        ExchangeDir().string().c_str());
    }
    if (ImGui::MenuItem("Import .srt...")) AskImport();
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("With the English voices: pieces, times, speakers, words.\nWith another voice: only "
                        "times (same number of pieces). Ctrl+Z undoes it.");
    }
    if (ImGui::MenuItem("Open the exchange folder")) {
      std::error_code ec;
      fs::create_directories(ExchangeDir(), ec);
      SDL_OpenURL(("file://" + ExchangeDir().string()).c_str());
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Reset the panel sizes")) list_w_ = preview_h_ = wave_h_ = 0;
    ImGui::EndPopup();
  }

  // The check row under the line's title.
  void DrawCheckRow(const Clip& c) {
    // Unused (a label of its own, for every language): its own row; the line
    // can still be checked like any other below it.
    const Unused* mark = UnusedMark(c);
    if (mark) {
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(kUnusedColour, "Unused: the game never plays this (marked by %s on %s).",
                         mark->name.c_str(), mark->date.c_str());
      ImGui::SameLine();
      if (IconButton("remove_unused", Icon::kDelete, "It is used after all: remove the mark")) MarkUnused(false);
    }
    const Check* check = nullptr;
    const CheckState state = StateOf(c, &check);
    const std::string voice = VoiceKey();
    if (state == CheckState::kOk) {
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(ImVec4(0.45f, 0.82f, 0.45f, 1.0f), "Checked by %s on %s (%s voices).", check->name.c_str(),
                         check->date.c_str(), voice.c_str());
    } else if (state == CheckState::kChanged) {
      ImGui::AlignTextToFramePadding();
      ImGui::TextColored(kWarnColour, "Checked by %s on %s (%s voices), but changed since: check it again.",
                         check->name.c_str(), check->date.c_str(), voice.c_str());
    } else {
      ImGui::AlignTextToFramePadding();
      ImGui::TextDisabled("Not checked by hand yet (%s voices).", voice.c_str());
    }
    ImGui::SameLine();
    if (state != CheckState::kOk) {
      if (IconButton("mark_checked", Icon::kCheck, nullptr, "Mark as checked")) MarkChecked(true);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("You listened to this line with the %s voices and its pieces, times, speakers and words "
                          "are right.\nSaved with your name and today's date; any later change shows it as "
                          "changed.",
                          voice.c_str());
      }
    }
    if (state != CheckState::kNone) {
      ImGui::SameLine();
      if (IconButton("remove_check", Icon::kDelete, "Remove the check")) MarkChecked(false);
    }
    if (!mark) {
      ImGui::SameLine();
      if (IconButton("mark_unused", Icon::kHide, nullptr, "Unused")) MarkUnused(true);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The game never plays this sound: mark it unused, for every language and everyone\n"
                          "(saved in unused.tsv with your name). It can still be checked; it is counted\n"
                          "on its own (the second bar above the list).");
      }
    }
  }

  // ---- .srt to and from other programs (exchange.h) ----------------------

  // user/subtitles/exchange/<voice>/ (made on first use).
  fs::path ExchangeDir() const { return user_dir_ / "exchange" / VoiceKey(); }
  // The files' name for the selected line: the cartoon's, or file + ID.
  // Only letters, digits, "_", "-" and "." (the game's own names can hold
  // "*", which no Windows file name may).
  std::string ExchangeName(const Clip& c) const {
    std::string name = c.movie.empty() ? c.file + "_" + KeyOf(c.id) : c.movie;
    for (char& ch : name) {
      if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_' && ch != '-' && ch != '.') ch = '_';
    }
    return name;
  }

  void ExportSrt() {
    const Clip* c = clip();
    if (!c) return;
    const fs::path dir = ExchangeDir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path srt = dir / (ExchangeName(*c) + ".srt");
    const fs::path wav = dir / (ExchangeName(*c) + ".wav");
    {
      std::ofstream o(srt, std::ios::binary | std::ios::trunc);
      o << WriteSrt(View(*c), Length());
      if (!o) {
        Status("NOT exported: can't write " + srt.string(), true);
        return;
      }
    }
    std::string error;
    if (!WriteWav(wav, pcm_, &error)) {
      Status("NOT exported: " + error, true);
      return;
    }
    Status("Exported " + srt.filename().string() + " + " + wav.filename().string() + " to " + dir.string() + ".");
  }

  // The system's file picker, opened in the exchange folder on this line's
  // .srt. Its answer arrives later (SDL calls back, maybe on another thread).
  void AskImport() {
    const Clip* c = clip();
    if (!c) return;
    std::error_code ec;
    fs::create_directories(ExchangeDir(), ec);
    import_default_ = (ExchangeDir() / (ExchangeName(*c) + ".srt")).string();
    static const SDL_DialogFileFilter kFilters[] = {{"SubRip subtitles", "srt"}, {"All files", "*"}};
    SDL_ShowOpenFileDialog(
        [](void* self, const char* const* files, int) {
          if (!files || !files[0]) return;  // cancelled or failed
          Editor* editor = static_cast<Editor*>(self);
          std::lock_guard<std::mutex> lock(editor->import_mutex_);
          editor->import_pending_ = files[0];
        },
        this, nullptr, kFilters, 2, import_default_.c_str(), false);
  }

  // A picked file, taken on the UI thread (from Draw).
  void TakePendingImport() {
    std::string file;
    {
      std::lock_guard<std::mutex> lock(import_mutex_);
      file.swap(import_pending_);
    }
    if (!file.empty()) ImportSrt(file);
  }

  void ImportSrt(const fs::path& file) {
    const Clip* c = clip();
    if (!c) return;
    std::vector<SrtEntry> entries;
    std::string error;
    if (!ReadSrt(file, &entries, &error)) {
      Status("NOT imported: " + error, true);
      return;
    }
    const double length = Length();
    if (OwnTimes()) {
      // Another voice: the times only, piece by piece.
      const std::vector<Piece> view = View(*c);
      if (view.size() != entries.size()) {
        Status("NOT imported: the file has " + std::to_string(entries.size()) + " subtitle(s), the line " +
                   std::to_string(view.size()) + " piece(s). With the " + VoiceKey() +
                   " voices only times can change: split or merge with the English voices first.",
               true);
        return;
      }
      Remember();
      for (size_t i = 0; i < entries.size(); ++i) {
        if (view[i].whole()) continue;
        const double a = std::clamp(entries[i].start, 0.0, length);
        SetTimes(int(i), a, std::clamp(entries[i].end, a + kMinPiece, std::max(length, a + kMinPiece)));
      }
      Status("Imported " + std::to_string(entries.size()) + " " + VoiceKey() + " time(s) from " +
             file.filename().string() + " (words unchanged).");
      return;
    }
    // English voices: the file's pieces replace the line's.
    std::vector<std::string> known = speakers_;
    for (const Piece& p : View(*c)) {
      if (!p.speaker.empty()) known.push_back(p.speaker);
    }
    std::vector<Piece>* pieces = Change();
    if (!pieces) return;
    pieces->clear();
    for (const SrtEntry& e : entries) {
      Piece p;
      p.start = std::clamp(e.start, 0.0, length);
      p.end = std::clamp(e.end, p.start + kMinPiece, std::max(length, p.start + kMinPiece));
      SplitSpeaker(e.text, known, &p.speaker, &p.text);
      pieces->push_back(std::move(p));
    }
    sel_piece_ = -1;
    Status("Imported " + std::to_string(entries.size()) + " subtitle(s) from " + file.filename().string() +
           " (Ctrl+Z undoes it).");
  }

  // Where S splits: the play head while playing, else the cursor.
  double SplitPoint() { return player_.playing() ? player_.Position() : cursor_; }

  void DrawWaveform(const Clip& c, double length) {
    const ImVec2 size(ImGui::GetContentRegionAvail().x, wave_h_);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("wave", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);
    dl->AddRectFilled(p0, p1, IM_COL32(18, 18, 22, 255));
    // The part shown: all of it, or a zoomed window (wheel = zoom around the
    // pointer, Shift+wheel = scroll; a playing head off screen is followed).
    const float mouse_x = ImGui::GetIO().MousePos.x;
    if (view_len_ <= 0 || view_len_ > length) {
      view_len_ = length;
      view_from_ = 0;
    }
    // The wheel over the waveform, like most sound editors: Ctrl+wheel =
    // zoom around the pointer, wheel (or a sideways wheel / touchpad swipe) =
    // scroll along the sound, Shift+wheel = the peaks' height.
    const ImGuiIO& io = ImGui::GetIO();
    if (hovered && length > 0 && (io.MouseWheel != 0 || io.MouseWheelH != 0)) {
      const float wheel = io.MouseWheel != 0 ? io.MouseWheel : io.MouseWheelH;
      if (io.KeyCtrl) {
        const double at = view_from_ + double(mouse_x - p0.x) / size.x * view_len_;
        const double len = std::clamp(view_len_ * std::pow(0.8, wheel), std::min(length, 0.5), length);
        view_from_ = at - (at - view_from_) * len / view_len_;
        view_len_ = len;
      } else if (io.KeyShift && io.MouseWheel != 0) {
        amp_ = std::clamp(amp_ * std::pow(1.25f, wheel), 0.5f, 16.0f);  // height, like the slider
      } else {
        view_from_ -= wheel * view_len_ * 0.15;
      }
    }
    if (player_.playing() && drag_piece_ < 0) {
      const double at = player_.Position();
      if (at > view_from_ + view_len_ || at < view_from_) view_from_ = at - view_len_ * 0.1;
    }
    view_from_ = std::clamp(view_from_, 0.0, std::max(0.0, length - view_len_));
    const double v0 = view_from_, vl = std::max(view_len_, 1e-6);
    const auto x_of = [&](double t) { return p0.x + float(length > 0 ? (t - v0) / vl : 0) * size.x; };
    const auto t_of = [&](float x) { return std::clamp(v0 + double(x - p0.x) / size.x * vl, 0.0, length); };
    dl->PushClipRect(p0, p1, true);
    const std::vector<Piece> view = View(c);
    const std::vector<Piece>* pieces = view.empty() ? nullptr : &view;

    // The edge under the pointer (6 px), for dragging: only the SELECTED
    // piece's edges (click a piece first, then resize it), so a click near a
    // border selects instead of resizing by accident.
    int edge_piece = -1, edge_side = 0;
    if (hovered && pieces && drag_piece_ < 0 && sel_piece_ >= 0 && sel_piece_ < int(pieces->size())) {
      float best = 6.0f;
      for (int i = sel_piece_; i == sel_piece_; ++i) {
        const Piece& p = (*pieces)[size_t(i)];
        if (p.whole() && OwnTimes()) continue;  // a whole line follows any voice's length
        const double a = p.whole() ? 0 : p.start, b = p.whole() ? length : p.end;
        for (int side = 0; side < 2; ++side) {
          const float d = std::abs(x_of(side ? b : a) - mouse_x);
          if (d < best) {
            best = d;
            edge_piece = i;
            edge_side = side;
          }
        }
      }
    }
    // Inside the selected piece (not on an edge): a drag MOVES it.
    const bool on_selected = hovered && pieces && edge_piece < 0 && drag_piece_ < 0 && length > 0 &&
                             sel_piece_ >= 0 && PieceAt(t_of(mouse_x)) == sel_piece_ &&
                             !(*pieces)[size_t(sel_piece_)].whole();
    if (edge_piece >= 0 || (drag_piece_ >= 0 && drag_side_ < 2)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    else if (on_selected || (drag_piece_ >= 0 && drag_side_ == 2)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    if (on_selected && ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      // Not moving yet: a press without a drag stays a click (moves the cursor).
      drag_piece_ = sel_piece_;
      drag_side_ = 2;
      move_grab_ = t_of(mouse_x);
      move_start_ = (*pieces)[size_t(sel_piece_)].start;
      moving_ = false;
    }
    if (edge_piece >= 0 && ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (OwnTimes()) Remember();  // remembered once for the whole drag
      else Change();               // (English: a whole line becomes a timed piece first)
      drag_piece_ = edge_piece;
      drag_side_ = edge_side;
      sel_piece_ = edge_piece;
    }
    if (drag_piece_ >= 0 && drag_side_ == 2) {
      // Moving the whole piece: starts after 4 px of drag (one undo step).
      if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (!moving_ && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
          if (OwnTimes()) Remember();
          else Change();
          moving_ = true;
        }
        if (moving_) MovePiece(drag_piece_, move_start_ + (t_of(mouse_x) - move_grab_));
      } else {
        if (!moving_) {  // just a click inside the selected piece
          cursor_ = move_grab_;
          if (player_.playing()) player_.Play(pcm_, cursor_);  // a click only seeks while playing
        } else {
          Status("Piece " + std::to_string(drag_piece_ + 1) + " moved.");
        }
        drag_piece_ = -1;
        moving_ = false;
      }
    } else if (drag_piece_ >= 0) {
      if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        std::vector<Piece> ps = View(c);
        if (drag_piece_ < int(ps.size())) {
          Piece p = ps[size_t(drag_piece_)];
          if (p.whole()) {
            p.start = 0;
            p.end = length;
          }
          const double prev_end = drag_piece_ > 0 && !ps[size_t(drag_piece_ - 1)].whole()
                                      ? ps[size_t(drag_piece_ - 1)].end : 0.0;
          const double next_start = drag_piece_ + 1 < int(ps.size()) && !ps[size_t(drag_piece_ + 1)].whole()
                                        ? ps[size_t(drag_piece_ + 1)].start : length;
          const double t = t_of(mouse_x);
          if (drag_side_ == 0) p.start = std::clamp(t, prev_end, p.end - kMinPiece);
          else p.end = std::clamp(t, p.start + kMinPiece, next_start);
          SetTimes(drag_piece_, p.start, p.end);
        }
      } else {
        drag_piece_ = -1;
      }
    } else if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && edge_piece < 0 && !on_selected && length > 0) {
      cursor_ = t_of(mouse_x);
      const int under = PieceAt(cursor_);
      if (under >= 0) sel_piece_ = under;
      if (player_.playing()) player_.Play(pcm_, cursor_);  // a click only seeks while playing
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && length > 0) {
      cursor_ = t_of(mouse_x);
      ImGui::OpenPopup("wave menu");
    }
    if (ImGui::BeginPopup("wave menu")) {
      ImGui::TextDisabled("At %s", FormatTime(cursor_).c_str());
      if (ImGui::MenuItem("Split here", "S", false, PieceAt(cursor_) >= 0)) SplitAt(cursor_);
      if (ImGui::MenuItem("New piece here", "N", false, PieceAt(cursor_) < 0)) AddAt(cursor_);
      ImGui::EndPopup();
    }

    // Pieces, the waveform over them, the cursor, the play head, the seconds.
    if (pieces) {
      for (size_t i = 0; i < pieces->size(); ++i) {
        const Piece& piece = (*pieces)[i];
        const double a = piece.whole() ? 0 : piece.start;
        const double b = piece.whole() ? length : std::min(piece.end, length);
        ImU32 colour = kPieceColours[i % std::size(kPieceColours)];
        if (int(i) == sel_piece_) colour = (colour & 0x00FFFFFF) | 0xA0000000;
        dl->AddRectFilled(ImVec2(x_of(a), p0.y), ImVec2(x_of(b), p1.y), colour);
        if (int(i) == sel_piece_) {
          dl->AddRect(ImVec2(x_of(a), p0.y), ImVec2(x_of(b), p1.y), IM_COL32(255, 255, 255, 200), 0, 0, 2);
          // Grips: the edges that can be dragged now.
          if (!(piece.whole() && OwnTimes())) {
            const float gh = size.y * 0.18f, gy = (p0.y + p1.y) * 0.5f;
            for (const double e : {a, b}) {
              const float gx = x_of(e);
              dl->AddRectFilled(ImVec2(gx - 3, gy - gh), ImVec2(gx + 3, gy + gh), IM_COL32(255, 255, 255, 230), 2);
            }
          }
        }
        dl->AddText(ImVec2(x_of(a) + 4, p0.y + 2), IM_COL32(255, 255, 255, 220), std::to_string(i + 1).c_str());
      }
    }
    const float mid = (p0.y + p1.y) * 0.5f, half = size.y * 0.45f * amp_;
    BuildPeaks(v0, vl, int(size.x));
    for (size_t x = 0; x < peaks_.size(); ++x) {
      const float px = p0.x + float(x);
      dl->AddLine(ImVec2(px, mid - peaks_[x].second * half), ImVec2(px, mid - peaks_[x].first * half + 1),
                  IM_COL32(170, 200, 230, 255));
    }
    if (cursor_ >= 0) dl->AddLine(ImVec2(x_of(cursor_), p0.y), ImVec2(x_of(cursor_), p1.y), IM_COL32(255, 220, 60, 255), 1.5f);
    if (player_.playing()) {
      const float x = x_of(player_.Position());
      dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), IM_COL32(255, 255, 255, 255), 2.0f);
    }
    // A tick per second (longer and labelled every 5 / 10 s when there's room).
    const int label_every = vl > 60 ? 10 : 5;
    for (int s = int(v0); s <= int(v0 + vl) + 1; ++s) {
      if (s <= 0 || s >= length) continue;
      const bool big = s % label_every == 0;
      dl->AddLine(ImVec2(x_of(s), p1.y - (big ? 12 : 6)), ImVec2(x_of(s), p1.y), IM_COL32(120, 120, 130, 255));
      if (big) {
        dl->AddText(ImVec2(x_of(s) + 3, p1.y - ImGui::GetFontSize() - 2), IM_COL32(140, 140, 150, 255),
                    FormatTime(s).substr(0, 5).c_str());
      }
    }
    dl->PopClipRect();
    if (hovered || drag_piece_ >= 0) ImGui::SetTooltip("%s", FormatTime(t_of(mouse_x)).c_str());
    // Zoomed: a bar to scroll the view along the sound.
    if (vl < length - 1e-3) {
      ImGui::SetNextItemWidth(size.x);
      float from = float(view_from_);
      if (ImGui::SliderFloat("##scroll", &from, 0.0f, float(length - vl), "")) view_from_ = from;
    }
  }

  void DrawPieces(const Clip& c, double length) {
    TextFile* t = text();
    const std::vector<Piece> view = View(c);
    const std::vector<Piece>* pieces = &view;
    if (pieces->empty()) {
      if (InScope(c.family)) {
        ImGui::TextDisabled("No subtitle for this line in %s yet: click in the waveform where it starts, then N.",
                            t ? t->code.c_str() : "?");
      } else {
        ImGui::TextDisabled("The game doesn't subtitle %s lines (findings/63 s.11).", FamilyName(c.family));
      }
      return;
    }
    if (!ImGui::BeginTable("pieces", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
                                            ImGuiTableFlags_ScrollY)) {
      return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#", 0, 0.25f);
    ImGui::TableSetupColumn("Start", 0, 0.9f);
    ImGui::TableSetupColumn("End", 0, 0.9f);
    ImGui::TableSetupColumn("Speaker", 0, 1.3f);
    ImGui::TableSetupColumn("Text", 0, 5.0f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight() * 3 + ImGui::GetStyle().ItemSpacing.x * 3);
    ImGui::TableHeadersRow();
    int act_delete = -1, act_merge = -1, act_play = -1;
    for (int i = 0; i < int(pieces->size()); ++i) {
      // Copies for the widgets: a change goes in through Change() (undo).
      Piece p = (*pieces)[size_t(i)];
      ImGui::PushID(i);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      if (ImGui::Selectable(std::to_string(i + 1).c_str(), i == sel_piece_)) sel_piece_ = i;
      // Start / end: drag (0.01 s steps) or Ctrl+click to type.
      bool changed = false, times_changed = false;
      double start = p.whole() ? 0 : p.start, end = p.whole() ? length : p.end;
      // A whole line keeps no times of its own for another voice (it lasts the sound).
      const bool fixed = p.whole() && OwnTimes();
      const double lo = 0, hi = std::max(length, end);
      ImGui::TableNextColumn();
      ImGui::BeginDisabled(fixed);
      ImGui::SetNextItemWidth(-1);
      times_changed |= ImGui::DragScalar("##start", ImGuiDataType_Double, &start, 0.01f, &lo, &hi, "%.2f s");
      RememberWhenActivated(i);
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-1);
      times_changed |= ImGui::DragScalar("##end", ImGuiDataType_Double, &end, 0.01f, &lo, &hi, "%.2f s");
      RememberWhenActivated(i);
      ImGui::EndDisabled();
      // Speaker: typed, or picked from the names already used.
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-ImGui::GetFrameHeight());
      std::string speaker = p.speaker;
      changed |= ImGui::InputText("##speaker", &speaker);
      RememberWhenActivated(i);
      ImGui::SameLine(0, 0);
      if (ImGui::BeginCombo("##speakers", nullptr, ImGuiComboFlags_NoPreview | ImGuiComboFlags_PopupAlignLeft)) {
        if (ImGui::Selectable("(none)")) {
          speaker.clear();
          changed = true;
          Remember();
        }
        for (const std::string& name : speakers_) {
          if (ImGui::Selectable(name.c_str(), name == speaker)) {
            speaker = name;
            changed = true;
            Remember();
          }
        }
        ImGui::EndCombo();
      }
      // Text: wraps; Enter = a line break in the subtitle.
      ImGui::TableNextColumn();
      std::string words = p.text;
      if (focus_text_ == i) {  // a fresh split: straight into its text
        ImGui::SetKeyboardFocusHere();
        focus_text_ = -1;
      }
      changed |= ImGui::InputTextMultiline("##text", &words,
                                           ImVec2(-1, ImGui::GetTextLineHeight() * 2.4f + ImGui::GetStyle().FramePadding.y * 2));
      RememberWhenActivated(i);
      // Problems the game would show.
      std::string problems;
      if (const std::string missing = MissingFontCharacters(words); !missing.empty()) {
        problems += "The game's font can't show: " + missing + "  ";
      }
      if (!p.whole() && p.end > length + 0.05) problems += "Past the sound's end.  ";
      if (i > 0 && !p.whole() && !(*pieces)[size_t(i - 1)].whole() && p.start < (*pieces)[size_t(i - 1)].end - 0.005) {
        problems += "Overlaps the piece before.  ";
      }
      if (words.empty()) problems += "No text yet.";
      if (!problems.empty()) ImGui::TextColored(kWarnColour, "%s", problems.c_str());
      // Buttons.
      ImGui::TableNextColumn();
      if (IconButton("play_piece", Icon::kPlay, "Play this piece (Enter)")) act_play = i;
      ImGui::SameLine();
      ImGui::BeginDisabled(i + 1 >= int(pieces->size()));
      if (IconButton("merge", Icon::kMerge, "Join with the next piece")) act_merge = i;
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (IconButton("delete", Icon::kDelete, "Delete this piece (Delete)")) act_delete = i;
      // The undo entry was made at the box's first change (RememberWhenActivated).
      if (changed) {  // words / speaker: the text file's, whatever voice plays
        std::vector<Piece>* ps = Change(false);
        if (ps && i < int(ps->size())) {
          (*ps)[size_t(i)].speaker = speaker;
          (*ps)[size_t(i)].text = words;
          sel_piece_ = i;
        }
      }
      if (times_changed) {
        const double a = std::min(start, end - kMinPiece);
        SetTimes(i, a, std::max(end, a + kMinPiece));
        sel_piece_ = i;
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
    if (act_play >= 0) PlayPiece(act_play);
    if (act_merge >= 0) MergeWithNext(act_merge);
    if (act_delete >= 0) DeletePiece(act_delete);
  }

  void RunActions() {
    for (const std::string& a : actions_) {
      const size_t colon = a.find(':');
      const std::string verb = a.substr(0, colon);
      const std::string arg = colon == std::string::npos ? "" : a.substr(colon + 1);
      const int n = std::atoi(arg.c_str()) - 1;
      const auto set = [&](bool speaker) {
        const size_t c2 = arg.find(':');
        std::vector<Piece>* ps = Change();
        if (ps && c2 != std::string::npos && n >= 0 && n < int(ps->size())) {
          (speaker ? (*ps)[size_t(n)].speaker : (*ps)[size_t(n)].text) = arg.substr(c2 + 1);
        }
      };
      if (verb == "cursor") cursor_ = std::atof(arg.c_str());
      else if (verb == "split") SplitAt(cursor_);
      else if (verb == "new") AddAt(std::max(0.0, cursor_));
      else if (verb == "delete") DeletePiece(n);
      else if (verb == "merge") MergeWithNext(n);
      else if (verb == "text") set(false);
      else if (verb == "speaker") set(true);
      else if (verb == "times") {  // times:<n>:<start>:<end>
        const size_t c2 = arg.find(':'), c3 = arg.find(':', c2 + 1);
        Remember();
        SetTimes(n, std::atof(arg.c_str() + c2 + 1), std::atof(arg.c_str() + c3 + 1));
      }
      else if (verb == "zoom") {  // zoom:<from>:<length> (seconds)
        view_from_ = std::atof(arg.c_str());
        view_len_ = std::atof(arg.c_str() + arg.find(':') + 1);
      }
      else if (verb == "unused") MarkUnused(arg != "off");  // unused / unused:off (name = checker_)
      else if (verb == "check") {  // check:<name> = mark the line checked by <name>
        checker_ = arg;
        MarkChecked(true);
      }
      else if (verb == "export") ExportSrt();
      else if (verb == "import") ImportSrt(arg.empty() ? ExchangeDir() / (ExchangeName(*clip()) + ".srt") : fs::path(arg));
      else if (verb == "undo") UndoLast();
      else if (verb == "redo") RedoLast();
      else if (verb == "move") {  // move:<n>:<start> = what dragging inside piece n does
        Remember();
        if (!MovePiece(n, std::atof(arg.c_str() + arg.find(':') + 1))) Forget();
        else Status("Piece " + std::to_string(n + 1) + " moved.");
      }
      else if (verb == "save") Save();
      std::printf("do %s -> %s\n", a.c_str(), status_.c_str());
    }
    std::fflush(stdout);
    actions_.clear();
  }

  // A box was clicked into: the line as it was is kept aside, and goes on
  // the undo list at the box's FIRST change (a click without typing leaves
  // no empty undo step). One undo step per visit to a box.
  void RememberWhenActivated(int piece) {
    if (ImGui::IsItemActivated()) {
      box_undo_ = Snapshot();
      sel_piece_ = piece;
    }
    if (ImGui::IsItemEdited() && box_undo_) {
      Push(std::move(*box_undo_));
      box_undo_.reset();
    }
  }

  void PlayPiece(int i) {
    if (!clip()) return;
    const std::vector<Piece> view = View(*clip());
    const auto* pieces = &view;
    if (i < 0 || i >= int(pieces->size())) return;
    const Piece& p = (*pieces)[size_t(i)];
    sel_piece_ = i;
    player_.Play(pcm_, p.whole() ? 0 : p.start, p.whole() ? -1 : p.end);
  }

  // Ctrl+Z undo, Ctrl+Y / Ctrl+Shift+Z redo: anywhere, text boxes too.
  void HandleUndoKeys() {
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.KeyCtrl || !archive_) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Z)) {
      if (io.KeyShift) RedoLast();
      else UndoLast();
    } else if (ImGui::IsKeyPressed(ImGuiKey_Y)) {
      RedoLast();
    }
  }

  void HandleKeys() {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) Save();
    if (io.WantTextInput || !archive_) return;
    if (io.KeyCtrl) return;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) SelectRow(std::min(selected_ + 1, int(shown_.size()) - 1), true);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) SelectRow(std::max(selected_ - 1, 0), true);
    if (!clip()) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
      if (io.KeyShift) player_.Play(pcm_, 0);  // the whole line from the start
      else if (player_.playing()) player_.Stop();
      else player_.Play(pcm_, cursor_ > 0 ? cursor_ : 0);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_S, false)) SplitAt(SplitPoint());
    if (ImGui::IsKeyPressed(ImGuiKey_N, false)) AddAt(std::max(0.0, cursor_));
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && sel_piece_ >= 0) DeletePiece(sel_piece_);
    if ((ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) &&
        sel_piece_ >= 0) {
      PlayPiece(sel_piece_);
    }
  }

  void DrawQuitQuestion() {
    if (ask_quit_) {
      ImGui::OpenPopup("Unsaved changes");
      ask_quit_ = false;
    }
    if (!ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::Text("%d line(s) aren't saved.", Unsaved());
    if (ImGui::Button("Save and quit")) {
      for (int i = 0; i < int(texts_.size()); ++i) {
        if (!texts_[size_t(i)].Edits()) continue;
        text_index_ = i;
        Save();
      }
      if (Unsaved() == 0) quit_ = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Quit without saving")) quit_ = true;
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  fs::path root_, user_dir_, backup_dir_, source_dir_, shipped_dir_;
  std::map<std::string, VoiceTiming> timings_;  // by voice key, read on first use
  std::map<std::string, CheckFile> checks_;     // by text language code
  std::string checker_;                         // who checks on this PC
  char name_buf_[64] = {};
  bool unchecked_only_ = false;
  bool hide_unused_ = false;
  UnusedFile unused_;  // one list for every language
  bool save_to_source_ = false;
  std::vector<fs::path> archives_;
  int voice_index_ = 0;
  std::vector<TextFile> texts_;
  int text_index_ = 0;
  std::vector<std::string> speakers_;
  std::vector<Undo> undo_, redo_;
  std::vector<Undo> redo_before_;  // the redo list a Forget() brings back
  std::optional<Undo> box_undo_;   // a text box's line before its first change
  int focus_text_ = -1;            // piece whose text box gets the keyboard next

  std::thread loader_;
  std::atomic<bool> loaded_{false};
  std::atomic<int> progress_{0}, total_{0};
  std::unique_ptr<VoiceArchive> pending_, archive_;
  uint64_t keep_id_ = 0, select_id_ = 0;
  bool play_at_start_ = false;

  char search_[128] = {};
  bool in_scope_only_ = true;
  bool without_text_only_ = false;
  std::vector<int> shown_;  // indexes into archive_->clips()
  int selected_ = -1;
  bool scroll_to_ = false;

  Pcm pcm_;
  std::string decode_error_;
  std::vector<std::pair<float, float>> peaks_;
  MovieArchive movies_;
  SDL_Renderer* renderer_ = nullptr;
  MovieVideo video_;                       // the selected cartoon's picture
  SDL_Texture* video_texture_ = nullptr;   // its frame on screen
  int video_tex_w_ = 0, video_tex_h_ = 0;
  uint32_t shown_frame_ = UINT32_MAX;
  bool show_preview_ = true;
  float amp_ = 1.0f;  // waveform height (1 = full scale fills it)
  // Panel sizes (pixels; 0 = not laid out yet): the list's width, the
  // preview's and the waveform's heights. Dragged with the dividers.
  float list_w_ = 0, preview_h_ = 0, wave_h_ = 0;
  bool cartoons_ = false;  // the list shows the cartoons
  bool force_tab_ = false;
  int tab_settle_ = 0;
  std::string select_movie_;
  // THE MIXER: the selected sound's parts (stems_) and, by part name, the
  // player's choice (on / off, volume in %), kept from line to line.
  Stems stems_;
  struct MixPart {
    bool on = true;
    int percent = 100;
    bool chosen = false;  // false = the part's own default (Stem::on) still applies
  };
  std::map<std::string, MixPart> mixer_;
  // A file picked in the import dialog (written by SDL's callback).
  std::mutex import_mutex_;
  std::string import_pending_, import_default_;
  // The waveform's visible part (seconds; len 0 = all) and what peaks_ holds.
  double view_from_ = 0, view_len_ = 0;
  struct PeaksKey {
    double from = -1, len = -1;
    int width = 0;
    bool operator==(const PeaksKey&) const = default;
  } peaks_key_;
  Player player_;

  int sel_piece_ = -1;
  double cursor_ = -1;  // seconds; -1 = none
  int drag_piece_ = -1, drag_side_ = 0;  // side 0 start, 1 end, 2 = the whole piece
  double move_grab_ = 0, move_start_ = 0;  // a whole-piece drag: where it was grabbed, its start then
  bool moving_ = false;                    // that drag has really moved (past 4 px)
  std::string status_;
  bool status_bad_ = false;
  Clock::time_point status_time_;
  bool ask_quit_ = false, quit_ = false;
  std::vector<std::string> actions_;
};

}  // namespace

int main(int argc, char** argv) {
  fs::path root;
  std::string screenshot, voices;
  double screenshot_after = 0;
  uint64_t select = 0;
  bool mute = false, play = false, music = false;
  std::string select_movie;
  std::vector<std::string> actions;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.starts_with("--game_folder=")) root = arg.substr(14);
    else if (arg.starts_with("--screenshot_after=")) screenshot_after = std::atof(arg.c_str() + 19);
    else if (arg.starts_with("--screenshot=")) screenshot = arg.substr(13);
    else if (arg.starts_with("--select=")) {
      // A sound ID (16 hex digits) or a cartoon's name (Crash_02_51).
      const std::string what = arg.substr(9);
      if (what.size() == 16 && what.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
        select = std::strtoull(what.c_str(), nullptr, 16);
      } else {
        select_movie = what;
      }
    }
    else if (arg == "--music") music = true;
    else if (arg.starts_with("--voices=")) voices = arg.substr(9);
    else if (arg == "--mute") mute = true;
    else if (arg.starts_with("--do=")) {
      // Comma-separated (a text may not hold a comma here: tests only).
      std::string list = arg.substr(5);
      for (size_t p = 0; p <= list.size();) {
        const size_t comma = std::min(list.find(',', p), list.size());
        if (comma > p) actions.push_back(list.substr(p, comma - p));
        p = comma + 1;
      }
    }
    else if (arg == "--play") play = true;
    else if (arg == "-h" || arg == "--help") {
      std::printf("subtitle_editor [--game_folder=<path>] [--voices=english] [--select=<sound ID>]\n"
                  "Lists, plays and shows the subtitles of the game's voice lines.\n");
      return 0;
    }
  }
  if (root.empty()) {
    if (const char* base = SDL_GetBasePath()) root = FindGameFolder(base);
    if (root.empty()) root = FindGameFolder(fs::current_path());
  }

  if (!SDL_Init(SDL_INIT_VIDEO | (mute ? 0 : SDL_INIT_AUDIO))) {
    std::fprintf(stderr, "subtitle editor: SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  if (root.empty() || !HasVoices(root)) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Subtitle editor",
                             "No game folder found (a folder with game/english.rcf).\n"
                             "Start the editor from inside the game folder, or pass "
                             "--game_folder=<path>.",
                             nullptr);
    return 1;
  }
  const float scale = std::max(1.0f, SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()));
  SDL_Window* window = SDL_CreateWindow("Subtitle editor - Mind over Recomp", int(1400 * scale),
                                        int(820 * scale), SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
  if (!renderer) {
    std::fprintf(stderr, "subtitle editor: no window: %s\n", SDL_GetError());
    return 1;
  }
  SDL_SetRenderVSync(renderer, 1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.FontSizeBase = 17.0f;
  style.FontScaleDpi = scale;
  style.FrameRounding = 5;
  style.FramePadding = {9, 6};
  style.ItemSpacing = {9, 8};
  style.ItemInnerSpacing = {6, 5};
  style.WindowPadding = {14, 12};
  style.CellPadding = {8, 5};
  style.ChildRounding = 6;
  style.PopupRounding = 6;
  style.GrabRounding = 4;
  style.GrabMinSize = 12;
  style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.96f, 0.55f, 0.13f, 0.85f);
  style.Colors[ImGuiCol_Header] = ImVec4(0.96f, 0.55f, 0.13f, 0.45f);
  style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.96f, 0.55f, 0.13f, 0.30f);
  // Buttons grey; Crash's orange marks what is ACTIVE (playing, unsaved
  // changes, a toggle that is on, the selected row).
  style.Colors[ImGuiCol_Button] = ImVec4(0.22f, 0.23f, 0.27f, 1.0f);
  style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.33f, 0.34f, 0.40f, 1.0f);
  style.Colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.17f, 0.20f, 1.0f);
  style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.25f, 0.29f, 1.0f);
  style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.96f, 0.55f, 0.13f, 0.9f);
  style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(1.00f, 0.72f, 0.35f, 1.0f);
  style.Colors[ImGuiCol_CheckMark] = ImVec4(0.96f, 0.55f, 0.13f, 1.0f);
  style.Colors[ImGuiCol_Tab] = ImVec4(0.18f, 0.19f, 0.22f, 1.0f);
  style.Colors[ImGuiCol_TabSelected] = ImVec4(0.96f, 0.55f, 0.13f, 0.55f);
  style.Colors[ImGuiCol_TabHovered] = ImVec4(0.96f, 0.55f, 0.13f, 0.40f);
  if (const fs::path font = SystemFont(); !font.empty()) {
    io.Fonts->AddFontFromFileTTF(font.string().c_str(), 17.0f);
  }
  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  Editor editor(root, mute, renderer);
  int voice = 0;
  {
    const auto archives = VoiceArchives(root / "game");
    for (int i = 0; i < int(archives.size()); ++i) {
      if (!voices.empty() && Lower(archives[size_t(i)].stem().string()) == Lower(voices)) voice = i;
    }
  }
  if (select) editor.Select(select);
  if (!select_movie.empty()) editor.SelectMovie(select_movie);
  if (music) editor.SetMusic(true);
  if (play) editor.PlayAtStart();
  editor.QueueActions(actions);
  editor.LoadVoices(voice, 0);

  const Clock::time_point launched = Clock::now();
  bool running = true;
  int frames = 0;
  while (running) {
    SDL_Event event;
    // Idle: wait for input; while loading or playing, redraw at the screen's pace.
    if (!editor.busy() && screenshot.empty() && SDL_WaitEventTimeout(&event, 500)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT) editor.RequestQuit();
    }
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if (event.type == SDL_EVENT_QUIT) editor.RequestQuit();
    }
    if (editor.ShouldQuit()) running = false;
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
      SDL_Delay(50);
      continue;
    }
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    editor.Draw();
    ImGui::Render();
    SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
    SDL_SetRenderDrawColor(renderer, 23, 23, 28, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    if (!screenshot.empty() && ++frames >= 4 && editor.ready() &&
        Clock::now() - launched >= std::chrono::duration<double>(screenshot_after)) {
      if (SDL_Surface* shot = SDL_RenderReadPixels(renderer, nullptr)) {
        if (!SDL_SavePNG(shot, screenshot.c_str())) {
          std::fprintf(stderr, "subtitle editor: screenshot failed: %s\n", SDL_GetError());
        }
        SDL_DestroySurface(shot);
      }
      running = false;
    }
    SDL_RenderPresent(renderer);
  }
  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
