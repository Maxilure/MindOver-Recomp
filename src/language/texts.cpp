// =============================================================================
// language/texts.cpp -- see texts.h
// =============================================================================
//
// The .po format (GNU gettext), the parts we read:
//
//     #. translator note (from a "// TR:" comment)    <- ignored here
//     #: src/options/options_settings.cpp:174         <- ignored here
//     #, fuzzy                                        <- machine draft
//     msgctxt "window mode"                           <- optional context
//     msgid "Fullscreen"                              <- the English text
//     msgstr "Plein écran"                            <- the translation
//
// Strings may continue over several "..." lines (joined), with C escapes
// (\n \" \\ \t). An entry's msgstr "" = not translated yet. Lines starting
// "#~" are obsolete entries (texts the code no longer has): skipped. The
// first entry (msgid "") is the file's header: skipped.
// =============================================================================
#include "texts.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include "../game_folder.h"
#include "language.h"

REXCVAR_DEFINE_BOOL(debug_pseudo_texts, false, "CrashMoM",
                    "Translators' test: every text the port adds is drawn accented and 30% longer in brackets, "
                    "so untranslated texts and boxes too small for longer languages stand out");

namespace {

// UTF-8 (the .po files) -> UTF-16 (the game's text).
std::u16string FromUtf8(std::string_view s) {
  std::u16string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp = c;
    int extra = 0;
    if (c >= 0xF0) { cp = c & 0x07; extra = 3; }
    else if (c >= 0xE0) { cp = c & 0x0F; extra = 2; }
    else if (c >= 0xC0) { cp = c & 0x1F; extra = 1; }
    ++i;
    for (int k = 0; k < extra && i < s.size(); ++k, ++i) cp = cp << 6 | (s[i] & 0x3F);
    if (cp >= 0x10000) {  // outside the first plane: a surrogate pair
      cp -= 0x10000;
      out += char16_t(0xD800 + (cp >> 10));
      out += char16_t(0xDC00 + (cp & 0x3FF));
    } else {
      out += char16_t(cp);
    }
  }
  return out;
}

// One entry's key: context + 0x04 + English (gettext's own convention).
std::u16string Key(std::string_view context, std::u16string_view english) {
  std::u16string key = FromUtf8(context);
  key += char16_t(4);
  key += english;
  return key;
}

// The text language -> its .po file name (standard language codes).
std::string CodeOf(const std::string& key) {
  if (key == "en-us" || key == "en-gb") return "";  // the code's own texts
  if (key == "fr-ca") return "fr_CA";
  if (key == "pt-br") return "pt_BR";
  if (key == "no") return "nb";  // Norwegian Bokmål
  return key;
}

struct Catalogue {
  std::string code;                                      // "fr"
  std::unordered_map<std::u16string, std::u16string> strings;  // key -> translation
  int total = 0, translated = 0, checked = 0;            // entries in the file
};

// Unescapes one quoted .po string ("..." with C escapes) and adds it to `out`.
void AddQuoted(std::string_view line, std::string* out) {
  const size_t open = line.find('"');
  const size_t close = line.rfind('"');
  if (open == std::string_view::npos || close <= open) return;
  for (size_t i = open + 1; i < close; ++i) {
    char c = line[i];
    if (c == '\\' && i + 1 < close) {
      const char e = line[++i];
      c = e == 'n' ? '\n' : e == 't' ? '\t' : e;  // \" and \\ = the character itself
    }
    *out += c;
  }
}

// Reads one .po file into `cat` (adds to what's there). False = no file.
bool ReadPo(const std::filesystem::path& path, Catalogue* cat) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::string ctx, id, str, *field = nullptr;
  bool fuzzy = false;    // "#, fuzzy" = a machine draft nobody has checked
  bool seen_str = false; // this entry's msgstr came: the next msgctxt/msgid starts another
  auto finish = [&] {
    if (seen_str && !id.empty()) {  // id "" = the file's header
      ++cat->total;
      if (!str.empty()) {
        ++cat->translated;
        if (!fuzzy) ++cat->checked;
        cat->strings[Key(ctx, FromUtf8(id))] = FromUtf8(str);
      }
    }
    ctx.clear(); id.clear(); str.clear();
    field = nullptr; fuzzy = false; seen_str = false;
  };
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) { finish(); continue; }       // entries are separated by blank lines
    if (line.rfind("#~", 0) == 0) continue;         // obsolete entry
    if (line.rfind("#,", 0) == 0) {                 // flags
      if (seen_str) finish();
      if (line.find("fuzzy") != std::string::npos) fuzzy = true;
      continue;
    }
    if (line[0] == '#') continue;                   // comments
    if (line.rfind("msgctxt", 0) == 0 || (line.rfind("msgid", 0) == 0 && line.rfind("msgid_plural", 0) != 0)) {
      if (seen_str) {                               // no blank line before this entry
        const bool f = fuzzy;
        finish();
        fuzzy = f;
      }
      field = line[3] == 'c' ? &ctx : &id;
    } else if (line.rfind("msgid_plural", 0) == 0) {
      field = nullptr;                              // plurals: not used by the port
      continue;
    } else if (line.rfind("msgstr", 0) == 0) {
      field = &str;
      seen_str = true;
    } else if (line[0] != '"') {
      continue;
    }
    if (field) AddQuoted(line, field);
  }
  finish();
  return true;
}

// The session's catalogue, loaded at the first tr().
Catalogue& Loaded() {
  static Catalogue cat;
  static std::once_flag once;
  std::call_once(once, [] {
    const auto& langs = language::TextLanguages();
    const int running = language::RunningText();
    if (running < 0 || running >= int(langs.size())) return;
    cat.code = CodeOf(langs[size_t(running)].key);
    if (cat.code.empty()) {
      REXLOG_INFO("Texts: English (the port's own texts as written)");
      return;
    }
    // The player's own copy first (translators' tests), else the shipped one.
    // fr_CA falls back to fr for whatever it doesn't have: read fr first, then
    // fr_CA over it.
    std::vector<std::string> codes;
    if (const size_t us = cat.code.find('_'); us != std::string::npos && cat.code != "pt_BR") {
      codes.push_back(cat.code.substr(0, us));
    }
    codes.push_back(cat.code);
    for (const std::string& code : codes) {
      const std::filesystem::path own = game_folder::UserFolder() / "languages" / (code + ".po");
      const std::filesystem::path shipped =
          rex::filesystem::GetExecutableFolder() / "languages" / (code + ".po");
      std::error_code ec;
      const bool use_own = std::filesystem::is_regular_file(own, ec);
      Catalogue part;
      if (ReadPo(use_own ? own : shipped, &part)) {
        for (auto& [k, v] : part.strings) cat.strings[k] = std::move(v);
        if (code == cat.code) {
          cat.total = part.total;
          cat.translated = part.translated;
          cat.checked = part.checked;
        }
        REXLOG_INFO("Texts: {} = {} of {} texts translated, {} checked by a person ({})", code,
                    part.translated, part.total, part.checked, (use_own ? own : shipped).string());
      } else if (code == cat.code) {
        REXLOG_WARN("Texts: no {} file ({}): the port's texts stay English", code, shipped.string());
      }
    }
  });
  return cat;
}

// Radical's pseudo-language look: accents, padding, brackets (texts.h).
std::u16string Pseudo(std::u16string_view text) {
  static const std::u16string_view plain = u"aeiouAEIOUcnyCN";
  static const std::u16string_view accented = u"àéîõüÀÉÎÕÜçñýÇÑ";
  std::u16string out = u"[";
  size_t letters = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    const char16_t c = text[i];
    // {0} placeholders stay as they are.
    if (c == u'{' && i + 2 < text.size() && text[i + 2] == u'}') {
      out.append(text.substr(i, 3));
      i += 2;
      continue;
    }
    const size_t at = plain.find(c);
    out += at == std::u16string_view::npos ? c : accented[at];
    if (c > u' ' && c < 0x7F) ++letters;
  }
  out.append((letters * 3 + 9) / 10, u'~');
  out += u']';
  return out;
}

std::mutex g_mutex;
// Texts handed out (translation, English fallback or pseudo form) by key:
// unordered_map nodes never move, so the references stay valid.
std::unordered_map<std::u16string, std::u16string> g_handed;
std::atomic<bool> g_missing_logged{false};

}  // namespace

const std::u16string& trc(std::string_view context, std::u16string_view english) {
  Catalogue& cat = Loaded();
  std::u16string key = Key(context, english);
  std::lock_guard lock(g_mutex);
  if (auto it = g_handed.find(key); it != g_handed.end()) return it->second;
  std::u16string text;
  if (auto it = cat.strings.find(key); it != cat.strings.end()) {
    text = it->second;
  } else {
    text = std::u16string(english);
    if (!cat.code.empty() && !g_missing_logged.exchange(true)) {
      REXLOG_INFO("Texts: a text without a {} translation is shown in English (first of them only)", cat.code);
    }
  }
  if (REXCVAR_GET(debug_pseudo_texts)) text = Pseudo(text);
  return g_handed.emplace(std::move(key), std::move(text)).first->second;
}

const std::u16string& tr(std::u16string_view english) { return trc({}, english); }

std::u16string trf(std::u16string_view text, std::initializer_list<std::u16string_view> args) {
  std::u16string out;
  out.reserve(text.size() + 16);
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == u'{' && i + 2 < text.size() && text[i + 2] == u'}' && text[i + 1] >= u'0' &&
        text[i + 1] <= u'9') {
      const size_t n = size_t(text[i + 1] - u'0');
      if (n < args.size()) out.append(*(args.begin() + n));
      i += 2;
      continue;
    }
    out += text[i];
  }
  return out;
}

// UTF-16 -> UTF-8 (the ImGui windows' text).
static std::string ToUtf8(std::u16string_view s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    uint32_t c = s[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size()) c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
    if (c < 0x80) {
      out += char(c);
    } else if (c < 0x800) {
      out += char(0xC0 | c >> 6);
      out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += char(0xE0 | c >> 12);
      out += char(0x80 | (c >> 6 & 0x3F));
      out += char(0x80 | (c & 0x3F));
    } else {
      out += char(0xF0 | c >> 18);
      out += char(0x80 | (c >> 12 & 0x3F));
      out += char(0x80 | (c >> 6 & 0x3F));
      out += char(0x80 | (c & 0x3F));
    }
  }
  return out;
}

const char* trs(const char* english) {
  // Each English text translated once; the map's strings never move.
  static std::mutex mutex;
  static std::unordered_map<std::string, std::string> done;
  std::lock_guard lock(mutex);
  auto it = done.find(english);
  if (it == done.end()) it = done.emplace(english, ToUtf8(tr(FromUtf8(english)))).first;
  return it->second.c_str();
}

namespace texts {

const std::unordered_map<GameTextKey, std::u16string, GameTextKeyHash>& GameTranslation() {
  static std::unordered_map<GameTextKey, std::u16string, GameTextKeyHash> out;
  static std::once_flag once;
  std::call_once(once, [] {
    const std::string& code = Loaded().code;
    if (code.empty()) return;  // English: the disc's own
    const std::filesystem::path own = game_folder::UserFolder() / "languages" / "game" / (code + ".po");
    const std::filesystem::path shipped =
        rex::filesystem::GetExecutableFolder() / "languages" / "game" / (code + ".po");
    std::error_code ec;
    const std::filesystem::path& path = std::filesystem::is_regular_file(own, ec) ? own : shipped;
    Catalogue cat;
    if (!ReadPo(path, &cat)) return;  // most languages: the disc has them
    for (const auto& [k, text] : cat.strings) {
      // Key = context ("frontend") + 0x04 + msgid ("213").
      const size_t sep = k.find(char16_t(4));
      if (sep == std::u16string::npos) continue;
      GameTextKey key;
      for (size_t i = 0; i < sep; ++i) key.bank += char(k[i]);
      for (size_t i = sep + 1; i < k.size(); ++i) {
        if (k[i] < u'0' || k[i] > u'9') { key.key = 0; break; }
        key.key = key.key * 10 + uint32_t(k[i] - u'0');
      }
      if (!key.bank.empty() && key.key) out.emplace(std::move(key), text);
    }
    REXLOG_INFO("Texts: the game's own text in {}: {} of {} strings translated ({})", code, out.size(),
                cat.total, path.string());
  });
  return out;
}

const std::string& FileCode() { return Loaded().code; }

int CheckedPercent() {
  const Catalogue& cat = Loaded();
  if (cat.code.empty()) return 100;
  return cat.total ? cat.checked * 100 / cat.total : 0;
}

int TranslatedPercent() {
  const Catalogue& cat = Loaded();
  if (cat.code.empty()) return 100;
  return cat.total ? cat.translated * 100 / cat.total : 0;
}

}  // namespace texts
