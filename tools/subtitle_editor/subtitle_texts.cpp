// =============================================================================
// tools/subtitle_editor/subtitle_texts.cpp -- see subtitle_texts.h
// =============================================================================
#include "subtitle_texts.h"

#include "subtitles/button_tags.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <set>

namespace subtitle_editor {
namespace {

bool ParseId(const std::string& s, uint64_t* id) {
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

// "mm:ss.cc" or "ss.cc" -> seconds; -1 if it isn't a time (as the game).
double ParseTime(std::string s) {
  double minutes = 0;
  if (const size_t colon = s.find(':'); colon != std::string::npos) {
    try { minutes = std::stod(s.substr(0, colon)); } catch (...) { return -1; }
    s.erase(0, colon + 1);
  }
  try { return minutes * 60.0 + std::stod(s); } catch (...) { return -1; }
}

std::string TextOf(std::string text) {
  for (size_t p = text.find("\\n"); p != std::string::npos; p = text.find("\\n", p + 1)) {
    text.replace(p, 2, "\n");
  }
  return text;
}

bool Read(const std::filesystem::path& path, TextFile* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out->path = path;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    out->raw.push_back(line);
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
    Piece piece;
    if (f.size() == 2 && is_id) {
      piece.end = -1;
      piece.text = TextOf(f[1]);
      out->sounds[id].push_back(std::move(piece));
    } else if (f.size() == 5) {
      piece.start = ParseTime(f[1]);
      piece.end = ParseTime(f[2]);
      piece.speaker = f[3];
      piece.text = TextOf(f[4]);
      if (piece.start < 0 || piece.end <= piece.start) {
        ++out->not_understood;
        continue;
      }
      if (is_id) out->sounds[id].push_back(std::move(piece));
      else out->movies[f[0]].push_back(std::move(piece));
    } else {
      ++out->not_understood;
    }
  }
  return true;
}

}  // namespace

std::vector<TextFile> ReadTextFiles(const std::filesystem::path& shipped_dir,
                                    const std::filesystem::path& user_dir) {
  std::set<std::string> codes;
  for (const auto& dir : {shipped_dir, user_dir}) {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
      if (e.path().extension() == ".tsv") codes.insert(e.path().stem().string());
    }
  }
  std::vector<TextFile> out;
  for (const std::string& code : codes) {
    TextFile t;
    t.code = code;
    std::error_code ec;
    const auto own = user_dir / (code + ".tsv");
    t.players_own = std::filesystem::is_regular_file(own, ec);
    if (Read(t.players_own ? own : shipped_dir / (code + ".tsv"), &t)) out.push_back(std::move(t));
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const TextFile& a, const TextFile& b) { return a.code == "en" && b.code != "en"; });
  return out;
}

namespace {

// A piece's text as one file field: line breaks as "\\n", no TABs / CRs.
std::string Field(const std::string& text) {
  std::string out;
  for (char c : text) {
    if (c == '\n') out += "\\n";
    else if (c == '\t') out += ' ';
    else if (c != '\r') out += c;
  }
  return out;
}

std::string Stamp() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H%M%S", &tm);
  return buf;
}

}  // namespace

bool SaveTextFile(TextFile& file, const std::filesystem::path& target,
                  const std::filesystem::path& backup_dir, std::string* error) {
  namespace fs = std::filesystem;
  // The lines of one sound as the game reads them: a lone whole line keeps
  // the short form, anything else is written as timed pieces.
  const auto lines_of = [&](uint64_t id, std::vector<std::string>& out) {
    const auto it = file.sounds.find(id);
    if (it == file.sounds.end()) return;
    char key[20];
    std::snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(id));
    const std::vector<Piece>& pieces = it->second;
    if (pieces.size() == 1 && pieces[0].whole()) {
      out.push_back(std::string(key) + "\t" + Field(pieces[0].text));
      return;
    }
    for (const Piece& p : pieces) {
      if (p.whole()) continue;  // can't sit beside timed pieces; the editor times them first
      out.push_back(std::string(key) + "\t" + FormatTime(p.start) + "\t" + FormatTime(p.end) + "\t" +
                    Field(p.speaker) + "\t" + Field(p.text));
    }
  };
  // A movie's pieces (always timed: a movie has no "whole line").
  const auto movie_lines_of = [&](const std::string& key, std::vector<std::string>& out) {
    const auto it = file.movies.find(key);
    if (it == file.movies.end()) return;
    for (const Piece& p : it->second) {
      if (p.whole()) continue;
      out.push_back(key + "\t" + FormatTime(p.start) + "\t" + FormatTime(p.end) + "\t" + Field(p.speaker) +
                    "\t" + Field(p.text));
    }
  };
  std::set<std::string> movies_written;
  std::vector<std::string> out;
  std::set<uint64_t> written;
  for (const std::string& line : file.raw) {
    uint64_t id = 0;
    const std::string key = line.substr(0, line.find('\t'));
    if (!line.empty() && line[0] != '#' && !ParseId(key, &id) && file.edited_movies.count(key)) {
      if (movies_written.insert(key).second) movie_lines_of(key, out);  // an edited movie's lines
      continue;
    }
    if (line.empty() || line[0] == '#' || !ParseId(key, &id) || !file.edited.count(id)) {
      out.push_back(line);
      continue;
    }
    if (written.insert(id).second) lines_of(id, out);  // the new lines at the old place
  }
  for (const uint64_t id : file.edited) {
    if (!written.count(id)) lines_of(id, out);  // sounds without a line before
  }
  for (const std::string& key : file.edited_movies) {
    if (!movies_written.count(key)) movie_lines_of(key, out);
  }

  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);
  if (fs::is_regular_file(target, ec)) {
    fs::create_directories(backup_dir, ec);
    fs::copy_file(target, backup_dir / (file.code + "-" + Stamp() + ".tsv"),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
      *error = "couldn't make a backup in " + backup_dir.string() + ": " + ec.message();
      return false;
    }
  }
  fs::path temp = target;
  temp += ".saving";
  {
    std::ofstream o(temp, std::ios::binary | std::ios::trunc);
    for (const std::string& line : out) o << line << '\n';
    if (!o) {
      *error = "couldn't write " + temp.string();
      return false;
    }
  }
  fs::rename(temp, target, ec);
  if (ec) {
    *error = "couldn't replace " + target.string() + ": " + ec.message();
    return false;
  }
  file.raw = std::move(out);
  file.edited.clear();
  file.edited_movies.clear();
  file.path = target;
  return true;
}

std::string KeyOf(uint64_t id) {
  char key[20];
  std::snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(id));
  return key;
}

VoiceTiming ReadVoiceTiming(const std::string& voice, const std::filesystem::path& shipped_dir,
                            const std::filesystem::path& user_dir) {
  VoiceTiming t;
  t.voice = voice;
  std::error_code ec;
  const auto own = user_dir / (voice + ".tsv");
  t.players_own = std::filesystem::is_regular_file(own, ec);
  const auto path = t.players_own ? own : shipped_dir / (voice + ".tsv");
  std::ifstream in(path, std::ios::binary);
  if (!in) return t;
  t.path = path;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    t.raw.push_back(line);
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> f;
    for (size_t p = 0;;) {
      const size_t tab = line.find('\t', p);
      f.push_back(line.substr(p, tab == std::string::npos ? std::string::npos : tab - p));
      if (tab == std::string::npos) break;
      p = tab + 1;
    }
    if (f.size() != 4) continue;
    const int number = std::atoi(f[1].c_str());
    const double start = ParseTime(f[2]), end = ParseTime(f[3]);
    if (number < 1 || number > 500 || start < 0 || end <= start) continue;
    uint64_t id = 0;
    const std::string key = ParseId(f[0], &id) ? KeyOf(id) : f[0];
    auto& list = t.times[key];
    if (list.size() < size_t(number)) list.resize(size_t(number), {-1.0, -1.0});
    list[size_t(number - 1)] = {start, end};
  }
  return t;
}

bool SaveVoiceTiming(VoiceTiming& t, const std::filesystem::path& target,
                     const std::filesystem::path& backup_dir, std::string* error) {
  namespace fs = std::filesystem;
  const auto lines_of = [&](const std::string& key, std::vector<std::string>& out) {
    const auto it = t.times.find(key);
    if (it == t.times.end()) return;
    for (size_t i = 0; i < it->second.size(); ++i) {
      const auto& [a, b] = it->second[i];
      if (a < 0) continue;
      out.push_back(key + "\t" + std::to_string(i + 1) + "\t" + FormatTime(a) + "\t" + FormatTime(b));
    }
  };
  std::vector<std::string> out = t.raw;
  if (out.empty()) {
    out = {"# Subtitle times for the " + t.voice + " voices (src/subtitles/subtitles.cpp ApplyVoiceTimes).",
           "# The pieces and their words are in the text files (subtitles/<language>.tsv); this file only",
           "# says when each piece shows while these voices play. Made with tools/subtitle_editor.",
           "# <sound ID or movie> TAB <piece number> TAB <start mm:ss.cc> TAB <end mm:ss.cc>", ""};
  }
  std::vector<std::string> kept;
  std::set<std::string> written;
  for (const std::string& line : out) {
    const std::string first = line.substr(0, line.find('\t'));
    uint64_t id = 0;
    const std::string key = ParseId(first, &id) ? KeyOf(id) : first;
    if (line.empty() || line[0] == '#' || !t.edited.count(key)) {
      kept.push_back(line);
      continue;
    }
    if (written.insert(key).second) lines_of(key, kept);
  }
  for (const std::string& key : t.edited) {
    if (!written.count(key)) lines_of(key, kept);
  }
  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);
  if (fs::is_regular_file(target, ec)) {
    fs::create_directories(backup_dir, ec);
    fs::copy_file(target, backup_dir / ("timing-" + t.voice + "-" + Stamp() + ".tsv"),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
      *error = "couldn't make a backup in " + backup_dir.string() + ": " + ec.message();
      return false;
    }
  }
  fs::path temp = target;
  temp += ".saving";
  {
    std::ofstream o(temp, std::ios::binary | std::ios::trunc);
    for (const std::string& line : kept) o << line << '\n';
    if (!o) {
      *error = "couldn't write " + temp.string();
      return false;
    }
  }
  fs::rename(temp, target, ec);
  if (ec) {
    *error = "couldn't replace " + target.string() + ": " + ec.message();
    return false;
  }
  t.raw = std::move(kept);
  t.edited.clear();
  t.path = target;
  return true;
}

CheckFile ReadCheckFile(const std::string& code, const std::filesystem::path& shipped_dir,
                        const std::filesystem::path& user_dir) {
  CheckFile c;
  c.code = code;
  std::error_code ec;
  const auto own = user_dir / (code + ".tsv");
  const auto path = std::filesystem::is_regular_file(own, ec) ? own : shipped_dir / (code + ".tsv");
  std::ifstream in(path, std::ios::binary);
  if (!in) return c;
  c.path = path;
  std::string line;
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
    if (f.size() != 5) continue;
    uint64_t id = 0;
    const std::string key = ParseId(f[0], &id) ? KeyOf(id) : f[0];
    c.checks[{key, f[1]}] = Check{f[2], f[3], f[4]};
  }
  return c;
}

bool SaveCheckFile(CheckFile& c, const std::filesystem::path& target, const std::filesystem::path& backup_dir,
                   std::string* error) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);
  if (fs::is_regular_file(target, ec)) {
    fs::create_directories(backup_dir, ec);
    fs::copy_file(target, backup_dir / ("checked-" + c.code + "-" + Stamp() + ".tsv"),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
      *error = "couldn't make a backup in " + backup_dir.string() + ": " + ec.message();
      return false;
    }
  }
  fs::path temp = target;
  temp += ".saving";
  {
    std::ofstream o(temp, std::ios::binary | std::ios::trunc);
    o << "# Lines checked by hand in the " << c.code << " subtitles (tools/subtitle_editor). Not read by the game.\n"
      << "# <sound ID or movie> TAB <voice listened to> TAB <who> TAB <date, UTC> TAB <fingerprint of the line>\n"
      << "# A fingerprint that no longer matches the line = changed since it was checked.\n\n";
    for (const auto& [key, check] : c.checks) {  // sorted by key and voice: small, stable diffs
      o << key.first << '\t' << key.second << '\t' << Field(check.name) << '\t' << check.date << '\t'
        << check.fingerprint << '\n';
    }
    if (!o) {
      *error = "couldn't write " + temp.string();
      return false;
    }
  }
  fs::rename(temp, target, ec);
  if (ec) {
    *error = "couldn't replace " + target.string() + ": " + ec.message();
    return false;
  }
  c.path = target;
  c.edited = false;
  return true;
}

UnusedFile ReadUnusedFile(const std::filesystem::path& shipped_dir, const std::filesystem::path& user_dir) {
  UnusedFile u;
  std::error_code ec;
  const auto own = user_dir / "unused.tsv";
  const auto path = std::filesystem::is_regular_file(own, ec) ? own : shipped_dir / "unused.tsv";
  std::ifstream in(path, std::ios::binary);
  if (!in) return u;
  u.path = path;
  std::string line;
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
    if (f.size() != 3) continue;
    uint64_t id = 0;
    const std::string key = ParseId(f[0], &id) ? KeyOf(id) : f[0];
    u.marks[key] = Unused{f[1], f[2]};
  }
  return u;
}

bool SaveUnusedFile(UnusedFile& u, const std::filesystem::path& target, const std::filesystem::path& backup_dir,
                    std::string* error) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);
  if (fs::is_regular_file(target, ec)) {
    fs::create_directories(backup_dir, ec);
    fs::copy_file(target, backup_dir / ("unused-" + Stamp() + ".tsv"), fs::copy_options::overwrite_existing, ec);
    if (ec) {
      *error = "couldn't make a backup in " + backup_dir.string() + ": " + ec.message();
      return false;
    }
  }
  fs::path temp = target;
  temp += ".saving";
  {
    std::ofstream o(temp, std::ios::binary | std::ios::trunc);
    o << "# Sounds and movies the game never plays, marked by hand (tools/subtitle_editor). Not read by the game.\n"
      << "# The same for every language. <sound ID or movie> TAB <who marked it> TAB <date, UTC>\n\n";
    for (const auto& [key, mark] : u.marks) {  // sorted by key: small, stable diffs
      o << key << '\t' << Field(mark.name) << '\t' << mark.date << '\n';
    }
    if (!o) {
      *error = "couldn't write " + temp.string();
      return false;
    }
  }
  fs::rename(temp, target, ec);
  if (ec) {
    *error = "couldn't replace " + target.string() + ": " + ec.message();
    return false;
  }
  u.path = target;
  u.edited = false;
  return true;
}

// REVIEW NOTES (subtitle_texts.h): same reading / saving as the unused marks,
// two more fields (the flag, the note).
ReviewFile ReadReviewFile(const std::filesystem::path& shipped_dir, const std::filesystem::path& user_dir) {
  ReviewFile r;
  std::error_code ec;
  const auto own = user_dir / "review.tsv";
  const auto path = std::filesystem::is_regular_file(own, ec) ? own : shipped_dir / "review.tsv";
  std::ifstream in(path, std::ios::binary);
  if (!in) return r;
  r.path = path;
  std::string line;
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
    if (f.size() != 5) continue;
    uint64_t id = 0;
    const std::string key = ParseId(f[0], &id) ? KeyOf(id) : f[0];
    Review review{f[1] == "yes", f[2], f[3], TextOf(f[4])};
    if (!review.empty()) r.reviews[key] = std::move(review);
  }
  return r;
}

bool SaveReviewFile(ReviewFile& r, const std::filesystem::path& target, const std::filesystem::path& backup_dir,
                    std::string* error) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(target.parent_path(), ec);
  if (fs::is_regular_file(target, ec)) {
    fs::create_directories(backup_dir, ec);
    fs::copy_file(target, backup_dir / ("review-" + Stamp() + ".tsv"), fs::copy_options::overwrite_existing, ec);
    if (ec) {
      *error = "couldn't make a backup in " + backup_dir.string() + ": " + ec.message();
      return false;
    }
  }
  fs::path temp = target;
  temp += ".saving";
  {
    std::ofstream o(temp, std::ios::binary | std::ios::trunc);
    o << "# Lines flagged for reviewing and notes left on them (tools/subtitle_editor). Not read by the game.\n"
      << "# The same for every language. <sound ID or movie> TAB <needs reviewing: yes|no> TAB <who wrote last>"
         " TAB <date, UTC> TAB <note, line breaks as \\n>\n\n";
    for (const auto& [key, review] : r.reviews) {  // sorted by key: small, stable diffs
      if (review.empty()) continue;
      o << key << '\t' << (review.needs_review ? "yes" : "no") << '\t' << Field(review.name) << '\t'
        << review.date << '\t' << Field(review.note) << '\n';
    }
    if (!o) {
      *error = "couldn't write " + temp.string();
      return false;
    }
  }
  fs::rename(temp, target, ec);
  if (ec) {
    *error = "couldn't replace " + target.string() + ": " + ec.message();
    return false;
  }
  r.path = target;
  r.edited = false;
  return true;
}

std::string Fingerprint(const std::vector<Piece>& pieces) {
  uint64_t h = 1469598103934665603ull;
  const auto add = [&](const std::string& s) {
    for (unsigned char ch : s) {
      h ^= ch;
      h *= 1099511628211ull;
    }
    h ^= 0xFF;  // a separator byte between fields
    h *= 1099511628211ull;
  };
  for (const Piece& p : pieces) {
    add(p.whole() ? std::string("whole") : FormatTime(p.start) + "-" + FormatTime(p.end));
    add(p.speaker);
    add(p.text);
  }
  return KeyOf(h);
}

std::string TodayUtc() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buf[16];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
  return buf;
}

std::string MissingFontCharacters(const std::string& text) {
  // A "{...}" pair is a BUTTON TAG (src/subtitles/button_tags.h): the game
  // draws a picture there, not the braces. Known or not, its braces are left
  // out here (an unknown one: UnknownButtonTags says so).
  std::string rest = text;
  std::vector<std::pair<size_t, size_t>> pairs;
  button_tags::ForEach(std::string_view(text), [&](size_t at, size_t length, const button_tags::Tag*) {
    pairs.push_back({at, length});
  });
  for (size_t k = pairs.size(); k-- > 0;) rest.erase(pairs[k].first, pairs[k].second);
  std::string missing;
  for (char c : std::string_view("\"*<>^_`{|}~\t")) {
    if (rest.find(c) != std::string::npos) missing += c == '\t' ? std::string("TAB") : std::string(1, c);
  }
  return missing;
}

std::string UnknownButtonTags(const std::string& text) {
  std::string out;
  button_tags::ForEach(std::string_view(text), [&](size_t at, size_t length, const button_tags::Tag* tag) {
    if (!tag) out += (out.empty() ? "" : " ") + text.substr(at, length);
  });
  return out;
}

std::string FormatTime(double seconds) {
  if (seconds < 0) seconds = 0;
  const int cs = int(seconds * 100.0 + 0.5);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02d:%02d.%02d", cs / 6000, (cs / 100) % 60, cs % 100);
  return buf;
}

}  // namespace subtitle_editor
