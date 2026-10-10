// =============================================================================
// saves.cpp -- see saves.h
// =============================================================================
#include "saves.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <fstream>
#include <map>
#include <string_view>
#include <system_error>

namespace fs = std::filesystem;

namespace saves {
namespace {

constexpr std::string_view kTitle = "565507FA";        // the game's title id
constexpr std::string_view kSavedGame = "00000001";    // content type: saved game
constexpr std::string_view kPrefix = "CrashMOM GameSlot ";
constexpr std::string_view kPlayedFile = "save_library_played.txt";
constexpr std::string_view kExtension = ".sav";                          // flat layout: <name>.sav
constexpr std::string_view kCopiedNote = "copied-from-xbox-layout.txt";  // the game's one-time copy is done
constexpr size_t kBlock = 4;                           // the save block's start in the file

// "CrashMOM GameSlot <digits>" -> the number, 0 if it isn't one.
int NumberOf(const std::string& name) {
  if (name.size() <= kPrefix.size() || !name.starts_with(kPrefix)) {
    return 0;
  }
  int number = 0;
  const char* first = name.data() + kPrefix.size();
  const char* last = name.data() + name.size();
  const auto [end, err] = std::from_chars(first, last, number);
  return err == std::errc() && end == last && number > 0 ? number : 0;
}

uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// UTF-16 (big-endian, zero-terminated, `count` characters at most) -> UTF-8.
std::string Utf16BeToUtf8(const uint8_t* p, size_t count) {
  std::string out;
  for (size_t i = 0; i < count; ++i) {
    uint32_t c = uint32_t(p[2 * i]) << 8 | p[2 * i + 1];
    if (c == 0) {
      break;
    }
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < count) {  // a surrogate pair
      const uint32_t low = uint32_t(p[2 * i + 2]) << 8 | p[2 * i + 3];
      if (low >= 0xDC00 && low < 0xE000) {
        c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
        ++i;
      }
    }
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

// The summary at the start of one save file (saves.h); false if unreadable.
bool ReadSummary(const fs::path& file, Save& save) {
  std::ifstream in(file, std::ios::binary);
  uint8_t head[kBlock + 104] = {};
  if (!in.read(reinterpret_cast<char*>(head), sizeof(head))) {
    return false;
  }
  const uint8_t* block = head + kBlock;
  save.year = block[12] << 8 | block[13];
  save.month = block[14];
  save.day = block[15];
  save.hour = block[16];
  save.minute = block[17];
  save.name = Utf16BeToUtf8(block + 20, 32);
  const uint32_t seconds_bits = Be32(block + 88);
  static_assert(sizeof(float) == sizeof(uint32_t));
  std::memcpy(&save.play_seconds, &seconds_bits, sizeof(float));
  if (!(save.play_seconds >= 0 && save.play_seconds < 1e8f)) {
    save.play_seconds = 0;  // NaN or nonsense
  }
  save.percent = int(Be32(block + 92));
  save.difficulty = int(Be32(block + 96));
  return true;
}

// <title folder>/save_library_played.txt: number -> ticks of the last load.
std::map<int, int64_t> ReadPlayed(const fs::path& title_folder) {
  std::map<int, int64_t> played;
  std::ifstream in(title_folder / kPlayedFile);
  int number = 0;
  long long ticks = 0;
  while (in >> number >> ticks) {
    played[number] = ticks;
  }
  return played;
}


// One save file -> its list entry (false if it can't be read).
bool ReadEntry(const fs::path& file, int number, const std::map<int, int64_t>& loaded, Save& save) {
  std::error_code ec;
  const auto written = fs::last_write_time(file, ec);
  if (ec || !ReadSummary(file, save)) {
    return false;
  }
  save.number = number;
  save.file = file;
  save.played = int64_t(written.time_since_epoch().count());
  if (const auto it = loaded.find(number); it != loaded.end()) {
    save.played = std::max(save.played, it->second);
  }
  return true;
}

// The flat layout (since 2026-10-10): user/saves/CrashMOM GameSlot N.sav.
std::vector<Save> ListFlat(const fs::path& saves_folder) {
  std::vector<Save> list;
  std::error_code ec;
  const std::map<int, int64_t> loaded = ReadPlayed(saves_folder);
  for (const auto& entry : fs::directory_iterator(saves_folder, ec)) {
    if (!entry.is_regular_file(ec) || entry.path().extension() != kExtension) {
      continue;
    }
    const int number = NumberOf(entry.path().stem().string());
    Save save;
    if (number && ReadEntry(entry.path(), number, loaded, save)) {
      list.push_back(std::move(save));
    }
  }
  return list;
}

// The old Xbox-style layout (user/saves/<profile id>/565507FA/00000001/
// CrashMOM GameSlot N/CrashMOM GameSlot N): shown until the game has run once
// after an update and copied it into the flat layout.
std::vector<Save> ListOldLayout(const fs::path& saves_folder) {
  std::vector<Save> list;
  std::error_code ec;
  // Normally one profile; "achievements" and anything else without the title
  // folder is skipped by the checks below.
  for (const auto& profile : fs::directory_iterator(saves_folder, ec)) {
    const fs::path title = profile.path() / kTitle;
    const fs::path folder = title / kSavedGame;
    if (!fs::is_directory(folder, ec)) {
      ec.clear();
      continue;
    }
    const std::map<int, int64_t> loaded = ReadPlayed(title);
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
      const std::string name = entry.path().filename().string();
      const int number = entry.is_directory(ec) ? NumberOf(name) : 0;
      Save save;
      if (number && ReadEntry(entry.path() / name, number, loaded, save)) {
        list.push_back(std::move(save));
      }
    }
  }
  return list;
}

}  // namespace

std::vector<Save> List(const fs::path& user_folder) {
  const fs::path saves_folder = user_folder / "saves";
  std::error_code ec;
  std::vector<Save> list = ListFlat(saves_folder);
  // Nothing flat yet, and the game hasn't done its one-time copy: the old
  // folders still hold the saves (same numbers the copy will give them).
  if (list.empty() && !fs::exists(saves_folder / kCopiedNote, ec)) {
    list = ListOldLayout(saves_folder);
  }
  std::sort(list.begin(), list.end(), [](const Save& a, const Save& b) {
    return a.played != b.played ? a.played > b.played : a.number < b.number;
  });
  return list;
}

const char* DifficultyName(int difficulty) {
  switch (difficulty) {
    case 0: return "Easy";
    case 1: return "Normal";
    case 2: return "Hard";
    default: return "?";
  }
}

}  // namespace saves
