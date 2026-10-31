// =============================================================================
// launcher/texts.cpp -- see texts.h
// =============================================================================
//
// The .po reading is the game's (src/language/texts.cpp) in short: entries
// separated by blank lines, msgctxt / msgid / msgstr with "..." continuation
// lines and C escapes, "#~" obsolete entries skipped, msgstr "" = not
// translated. A context is kept as context + 0x04 + English (trsc).
// =============================================================================
#include "texts.h"

#include <fstream>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace texts {
namespace {

std::filesystem::path g_user, g_game, g_source;
bool g_init = false;
std::string g_code;  // "de", "fr_CA", "pt_BR"; empty = English

// The setting's value -> its .po file code (the game's CodeOf).
std::string CodeOf(const std::string& key) {
  if (key.empty() || key == "en-us" || key == "en-gb") return "";
  if (key == "fr-ca") return "fr_CA";
  if (key == "pt-br") return "pt_BR";
  if (key == "no") return "nb";
  return key;
}

// text_language from user/settings.toml (a plain "name = value" line).
std::string ReadTextLanguage(const std::filesystem::path& file) {
  std::ifstream in(file);
  std::string line;
  while (std::getline(in, line)) {
    const size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos || line.compare(start, 13, "text_language") != 0) continue;
    const size_t eq = line.find('=', start);
    if (eq == std::string::npos) continue;
    const size_t open = line.find('"', eq);
    const size_t close = open == std::string::npos ? open : line.find('"', open + 1);
    if (close != std::string::npos) return line.substr(open + 1, close - open - 1);
  }
  return "";
}

void AddQuoted(const std::string& line, std::string* out) {
  const size_t open = line.find('"');
  const size_t close = line.rfind('"');
  if (open == std::string::npos || close <= open) return;
  for (size_t i = open + 1; i < close; ++i) {
    char c = line[i];
    if (c == '\\' && i + 1 < close) {
      const char e = line[++i];
      c = e == 'n' ? '\n' : e == 't' ? '\t' : e;
    }
    *out += c;
  }
}

// Adds a .po file's translations (context-less entries) to `out`.
bool ReadPo(const std::filesystem::path& path, std::unordered_map<std::string, std::string>* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::string ctx, id, str, *field = nullptr;
  bool seen_str = false;
  auto finish = [&] {
    // Key: context + 0x04 + English (gettext's convention, like the game's).
    if (seen_str && !id.empty() && !str.empty()) (*out)[ctx.empty() ? id : ctx + '\x04' + id] = str;
    ctx.clear(); id.clear(); str.clear();
    field = nullptr;
    seen_str = false;
  };
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) { finish(); continue; }
    if (line[0] == '#') { if (seen_str) finish(); continue; }
    if (line.rfind("msgctxt", 0) == 0 || (line.rfind("msgid", 0) == 0 && line.rfind("msgid_plural", 0) != 0)) {
      if (seen_str) finish();
      field = line[3] == 'c' ? &ctx : &id;
    } else if (line.rfind("msgstr", 0) == 0) {
      field = &str;
      seen_str = true;
    } else if (line[0] != '"') {
      field = nullptr;  // msgid_plural and anything else: not ours
      continue;
    }
    if (field) AddQuoted(line, field);
  }
  finish();
  return true;
}

const std::unordered_map<std::string, std::string>& Strings() {
  static std::unordered_map<std::string, std::string> strings;
  static std::once_flag once;
  std::call_once(once, [] {
    if (!g_init || g_code.empty()) return;
    // A regional file reads its base language's first ("fr_CA" over "fr").
    std::vector<std::string> codes;
    if (const size_t us = g_code.find('_'); us != std::string::npos && g_code != "pt_BR") {
      codes.push_back(g_code.substr(0, us));
    }
    codes.push_back(g_code);
    for (const std::string& code : codes) {
      for (const auto& folder : {g_user / "languages", g_game / "languages", g_source / "languages"}) {
        if (!folder.empty() && ReadPo(folder / (code + ".po"), &strings)) break;
      }
    }
  });
  return strings;
}

}  // namespace

void Init(const std::filesystem::path& user_folder, const std::filesystem::path& game_folder,
          const std::filesystem::path& source_folder) {
  g_user = user_folder;
  g_game = game_folder;
  g_source = source_folder;
  g_code = CodeOf(ReadTextLanguage(user_folder / "settings.toml"));
  g_init = true;
}

const std::string& Code() { return g_code; }

}  // namespace texts

const char* trs(const char* english) {
  const auto& strings = texts::Strings();
  const auto it = strings.find(english);
  return it == strings.end() ? english : it->second.c_str();
}

const char* trsc(const char* context, const char* english) {
  const auto& strings = texts::Strings();
  const auto it = strings.find(std::string(context) + '\x04' + english);
  return it == strings.end() ? english : it->second.c_str();
}

std::string trf(std::string_view text, std::initializer_list<std::string_view> args) {
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '{' && i + 2 < text.size() && text[i + 2] == '}' && text[i + 1] >= '0' && text[i + 1] <= '9') {
      const size_t n = size_t(text[i + 1] - '0');
      if (n < args.size()) out.append(*(args.begin() + n));
      i += 2;
      continue;
    }
    out += text[i];
  }
  return out;
}
