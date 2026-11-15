// =============================================================================
// tools/subtitle_editor/exchange.cpp -- see exchange.h
// =============================================================================
#include "exchange.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace subtitle_editor {
namespace {

std::string SrtTime(double seconds) {
  if (seconds < 0) seconds = 0;
  const long long ms = (long long)(seconds * 1000.0 + 0.5);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld,%03lld", ms / 3600000, (ms / 60000) % 60, (ms / 1000) % 60,
                ms % 1000);
  return buf;
}

// "HH:MM:SS,mmm" (also "." or one / two digits per field) -> seconds, -1 if not.
double ParseSrtTime(std::string s) {
  s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == ' ' || c == '\t'; }), s.end());
  std::replace(s.begin(), s.end(), ',', '.');
  double parts[3] = {0, 0, 0};
  int n = 0;
  size_t p = 0;
  while (n < 3) {
    const size_t colon = s.find(':', p);
    const std::string field = s.substr(p, colon == std::string::npos ? std::string::npos : colon - p);
    if (field.empty()) return -1;
    try {
      parts[n++] = std::stod(field);
    } catch (...) {
      return -1;
    }
    if (colon == std::string::npos) break;
    p = colon + 1;
  }
  // 3 fields = h:m:s, 2 = m:s, 1 = s.
  if (n == 3) return parts[0] * 3600 + parts[1] * 60 + parts[2];
  if (n == 2) return parts[0] * 60 + parts[1];
  return parts[0];
}

// Formatting the game can't show: HTML-like tags (<i>, </font>) and ASS
// override blocks ({\an8}).
std::string StripTags(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '<') {
      const size_t close = s.find('>', i);
      if (close != std::string::npos && close - i < 40) {
        i = close;
        continue;
      }
    }
    if (s[i] == '{' && i + 1 < s.size() && s[i + 1] == '\\') {
      const size_t close = s.find('}', i);
      if (close != std::string::npos) {
        i = close;
        continue;
      }
    }
    out += s[i];
  }
  return out;
}

std::string Trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

void Put16(std::ofstream& o, uint16_t v) { o.put(char(v & 0xFF)).put(char(v >> 8)); }
void Put32(std::ofstream& o, uint32_t v) {
  for (int i = 0; i < 4; ++i) o.put(char((v >> (8 * i)) & 0xFF));
}

}  // namespace

std::string WriteSrt(const std::vector<Piece>& pieces, double length) {
  std::string out;
  int number = 0;
  for (const Piece& p : pieces) {
    const double a = p.whole() ? 0 : p.start, b = p.whole() ? length : p.end;
    std::string text = p.speaker.empty() ? p.text : p.speaker + ": " + p.text;
    std::string rows;
    for (char c : text) rows += c == '\r' ? std::string() : std::string(1, c);
    out += std::to_string(++number) + "\n" + SrtTime(a) + " --> " + SrtTime(b) + "\n" + rows + "\n\n";
  }
  return out;
}

bool ReadSrt(const std::filesystem::path& file, std::vector<SrtEntry>* out, std::string* error) {
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    *error = "can't open " + file.string();
    return false;
  }
  std::stringstream ss;
  ss << in.rdbuf();
  std::string all = ss.str();
  if (all.starts_with("\xEF\xBB\xBF")) all.erase(0, 3);  // UTF-8 BOM
  out->clear();
  std::istringstream lines(all);
  std::string line;
  SrtEntry entry;
  enum { kNumber, kTime, kText } state = kNumber;
  const auto finish = [&]() {
    if (state == kText) {
      entry.text = Trim(entry.text);
      if (entry.end > entry.start) out->push_back(entry);
    }
    entry = {};
    state = kNumber;
  };
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (Trim(line).empty()) {
      finish();
      continue;
    }
    const size_t arrow = line.find("-->");
    if (state != kText && arrow != std::string::npos) {
      // The time line (the number line before it is optional here).
      entry.start = ParseSrtTime(line.substr(0, arrow));
      std::string rest = line.substr(arrow + 3);
      // Some files add positions after the end time ("X1:..."): cut them off.
      rest = Trim(rest);
      if (const size_t space = rest.find(' '); space != std::string::npos) rest = rest.substr(0, space);
      entry.end = ParseSrtTime(rest);
      if (entry.start < 0 || entry.end < 0) {
        *error = "a time line isn't understood: " + line;
        return false;
      }
      state = kText;
      continue;
    }
    if (state == kText) {
      entry.text += (entry.text.empty() ? "" : "\n") + StripTags(line);
    }
    // state kNumber: the entry's number, nothing to keep.
  }
  finish();
  std::sort(out->begin(), out->end(), [](const SrtEntry& a, const SrtEntry& b) { return a.start < b.start; });
  if (out->empty()) {
    *error = "no subtitles found in " + file.filename().string();
    return false;
  }
  return true;
}

bool WriteWav(const std::filesystem::path& file, const Pcm& pcm, std::string* error) {
  std::ofstream o(file, std::ios::binary | std::ios::trunc);
  if (!o) {
    *error = "can't write " + file.string();
    return false;
  }
  const uint32_t data = uint32_t(pcm.stereo.size() * 2);
  o.write("RIFF", 4);
  Put32(o, 36 + data);
  o.write("WAVEfmt ", 8);
  Put32(o, 16);
  Put16(o, 1);  // PCM
  Put16(o, 2);  // stereo
  Put32(o, pcm.rate);
  Put32(o, pcm.rate * 4);
  Put16(o, 4);
  Put16(o, 16);
  o.write("data", 4);
  Put32(o, data);
  for (float v : pcm.stereo) {
    const int s = int(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
    Put16(o, uint16_t(int16_t(s)));
  }
  if (!o) {
    *error = "couldn't write all of " + file.string();
    return false;
  }
  return true;
}

void SplitSpeaker(const std::string& line, const std::vector<std::string>& speakers, std::string* speaker,
                  std::string* text) {
  speaker->clear();
  *text = line;
  const size_t colon = line.find(": ");
  if (colon == std::string::npos || colon == 0 || colon > 40) return;
  const std::string name = line.substr(0, colon);
  for (const std::string& known : speakers) {
    std::string a = known, b = name;
    for (char& c : a) c = char(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : b) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (a == b) {
      *speaker = known;
      *text = line.substr(colon + 2);
      return;
    }
  }
}

}  // namespace subtitle_editor
