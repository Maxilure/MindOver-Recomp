// =============================================================================
// tools/subtitle_editor/subtitle_texts.h -- the subtitle files the game reads
// =============================================================================
//
// The game (src/subtitles/subtitles.cpp) reads, per text language code
// ("en", "pt-br"), `<user folder>/subtitles/<code>.tsv` when the player has
// one, else the shipped `subtitles/<code>.tsv` next to the exe (in the source
// tree: <repo>/subtitles/). TAB-separated lines:
//   <sound ID>  <text>                                 a sound's whole line
//   <key>  <start>  <end>  <speaker>  <text>           a timed piece
// <key> = a sound ID (16 hex digits) or a movie's file name (Crash_02_51);
// times "mm:ss.cc" or seconds from the start of the sound / movie; "\n" in a
// text breaks the row; # starts a comment.
//
// SAVING keeps the file as it was written (its comments, its order, every
// line the editor didn't touch, the movies' lines byte for byte) and only
// replaces the lines of the sounds that were edited, at the place of their
// first line; sounds that had no line yet go at the end. A whole line that
// is edited becomes timed pieces.
// =============================================================================
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace subtitle_editor {

struct Piece {
  double start = 0, end = 0;  // seconds; a whole line (no times) = 0 .. -1
  std::string speaker;        // UTF-8, "" = none
  std::string text;           // UTF-8, "\n" already turned into a line break
  bool whole() const { return end < 0; }
  bool operator==(const Piece&) const = default;
};

struct TextFile {
  std::string code;                          // "en", "pt-br"
  std::filesystem::path path;                // the file read
  bool players_own = false;                  // from the user folder (else shipped)
  std::map<uint64_t, std::vector<Piece>> sounds;      // by sound ID
  std::map<std::string, std::vector<Piece>> movies;   // by movie name (as written)
  int not_understood = 0;
  std::vector<std::string> raw;              // the file's lines as read (for saving)
  std::set<uint64_t> edited;                 // sounds changed since reading / saving
  std::set<std::string> edited_movies;       // movies changed (keys as in `movies`)
  size_t Edits() const { return edited.size() + edited_movies.size(); }
};

// The text languages available: every <code>.tsv in the shipped folder and
// the user folder, English first. Each read like the game reads it (the
// player's own file wins over the shipped one).
std::vector<TextFile> ReadTextFiles(const std::filesystem::path& shipped_dir,
                                    const std::filesystem::path& user_dir);

// Writes `file` (whole) to `target`: a copy of what is there goes to
// `backup_dir` first (<code>-<date_time>.tsv), then a temporary file is
// renamed over it. On success `file` remembers the new contents (edited
// cleared, raw = what was written). False + `error` otherwise.
bool SaveTextFile(TextFile& file, const std::filesystem::path& target,
                  const std::filesystem::path& backup_dir, std::string* error);

// A VOICE LANGUAGE'S OWN TIMES (the game: src/subtitles/subtitles.cpp
// ApplyVoiceTimes). The text files' times are made on the English voices;
// `subtitles/timing/<voice>.tsv` gives other voices theirs, per piece:
//   <key>  <piece number, from 1>  <start>  <end>
// The game uses a key's times only when they cover exactly the pieces the
// text has for it (same count). Read and saved like a text file (the
// player's own file wins whole; untouched lines kept on saving).
struct VoiceTiming {
  std::string voice;                 // "french" (the voice language's key)
  std::filesystem::path path;        // the file read ("" = none yet)
  bool players_own = false;
  // key (16 lower-case hex digits, or a movie name) -> per piece (start, end);
  // (-1, -1) = a piece the file doesn't give.
  std::map<std::string, std::vector<std::pair<double, double>>> times;
  std::vector<std::string> raw;
  std::set<std::string> edited;
};
VoiceTiming ReadVoiceTiming(const std::string& voice, const std::filesystem::path& shipped_dir,
                            const std::filesystem::path& user_dir);
bool SaveVoiceTiming(VoiceTiming& timing, const std::filesystem::path& target,
                     const std::filesystem::path& backup_dir, std::string* error);
// A sound ID as the files write it.
std::string KeyOf(uint64_t id);

// CHECKS: who looked at a line by hand and found it right. One file per text
// language, `subtitles/checked/<code>.tsv` (the game never reads it; the
// player's own copy wins whole, as for the others):
//   <key>  <voice>  <name>  <date YYYY-MM-DD, UTC>  <fingerprint>
// <key> = a sound ID or a movie name; <voice> = the voice language listened
// to (its times were checked); <fingerprint> = 16 hex digits of a hash of
// the line's pieces (times for that voice, speakers, words) when checked:
// any later change to the line shows the check as out of date.
struct Check {
  std::string name, date, fingerprint;
};
struct CheckFile {
  std::string code;
  std::filesystem::path path;
  std::map<std::pair<std::string, std::string>, Check> checks;  // (key, voice) -> check
  bool edited = false;
};
CheckFile ReadCheckFile(const std::string& code, const std::filesystem::path& shipped_dir,
                        const std::filesystem::path& user_dir);
bool SaveCheckFile(CheckFile& file, const std::filesystem::path& target, const std::filesystem::path& backup_dir,
                   std::string* error);
// UNUSED: sounds and movies the game never plays (left on the disc), marked
// by hand so nobody has to time or check them. ONE file for every language
// (a sound is unused whatever its words), `subtitles/unused.tsv` (the game
// never reads it; the player's own copy wins whole, as for the others):
//   <key>  <name of who marked it>  <date YYYY-MM-DD, UTC>
struct Unused {
  std::string name, date;
};
struct UnusedFile {
  std::filesystem::path path;
  std::map<std::string, Unused> marks;  // key -> who / when
  bool edited = false;
};
UnusedFile ReadUnusedFile(const std::filesystem::path& shipped_dir, const std::filesystem::path& user_dir);
bool SaveUnusedFile(UnusedFile& file, const std::filesystem::path& target, const std::filesystem::path& backup_dir,
                    std::string* error);

// REVIEW NOTES: a line flagged "needs reviewing" and / or a note left on it
// (what is wrong, what to ask, who should listen). ONE file for every
// language, like the unused marks (a note can name a language itself),
// `subtitles/review.tsv` (the game never reads it; the player's own copy
// wins whole, as for the others):
//   <key>  <needs reviewing: yes | no>  <who wrote last>  <date YYYY-MM-DD, UTC>  <note>
// <note> = UTF-8, line breaks written "\n"; a line with neither the flag nor
// a note is dropped.
struct Review {
  bool needs_review = false;
  std::string name, date, note;
  bool empty() const { return !needs_review && note.empty(); }
};
struct ReviewFile {
  std::filesystem::path path;
  std::map<std::string, Review> reviews;  // key -> flag / note
  bool edited = false;
};
ReviewFile ReadReviewFile(const std::filesystem::path& shipped_dir, const std::filesystem::path& user_dir);
bool SaveReviewFile(ReviewFile& file, const std::filesystem::path& target, const std::filesystem::path& backup_dir,
                    std::string* error);

// The fingerprint of a line's pieces as shown (FNV-1a 64 of times in
// hundredths, speakers and words).
std::string Fingerprint(const std::vector<Piece>& pieces);
// Today in UTC, "2026-11-15".
std::string TodayUtc();

// "12.34" -> "00:12.34".
std::string FormatTime(double seconds);

// Characters the game's subtitle font has no picture for (they'd show as
// nothing): " * < > ^ _ ` { | } ~ (findings/49 s.8), plus TAB.
std::string MissingFontCharacters(const std::string& text);
// The "{...}" of a line that aren't button tags (a typo: {Yy}), space-separated;
// "" = none. The game leaves them as written.
std::string UnknownButtonTags(const std::string& text);

}  // namespace subtitle_editor
