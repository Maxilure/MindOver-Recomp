// =============================================================================
// saves/save_files.cpp -- see save_files.h
// =============================================================================
#include "save_files.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <system_error>
#include <vector>

#include <fmt/format.h>

#include "../game_folder.h"

namespace save_files {
namespace {

namespace fs = std::filesystem;

// The game's own names (its save manager formats "%s GameSlot %d" with the
// prefix "CrashMOM", findings/24 section 2.2).
constexpr std::string_view kNamePrefix = "CrashMOM GameSlot ";
constexpr std::string_view kExtension = ".sav";
constexpr std::string_view kDeletedFolder = "Deleted saves";
constexpr std::string_view kBackupsFolder = "Backups";
constexpr std::string_view kPlayedFile = "save_library_played.txt";
constexpr std::string_view kCopiedNote = "copied-from-xbox-layout.txt";

// The old layout's pieces (the emulated Xbox's content folders).
constexpr std::string_view kTitleId = "565507FA";
constexpr std::string_view kSavedGameType = "00000001";  // XContentType::kSavedGame

// The save file's layout (save_files.h).
constexpr size_t kFileSize = 0x4EED;
constexpr size_t kNameOffset = 4 + 20;  // the size word, then block +20
constexpr size_t kNameChars = 32;       // the field, terminator included

// Reads a whole file; false if it can't be opened.
bool ReadAll(const fs::path& path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

// UTF-16 -> big-endian bytes (the guest's byte order, in the files too).
void PutUtf16Be(uint8_t* out, size_t chars, std::u16string_view text) {
  for (size_t i = 0; i < chars; ++i) {
    const char16_t c = i < text.size() ? text[i] : u'\0';
    out[2 * i] = uint8_t(c >> 8);
    out[2 * i + 1] = uint8_t(c & 0xFF);
  }
}

// "CrashMOM GameSlot <digits>" -> the number, 0 if the name isn't one (a
// hand-made "CrashMOM GameSlot 4 copy" isn't a save the game can open).
int NumberOf(std::string_view name) {
  if (name.size() <= kNamePrefix.size() || name.substr(0, kNamePrefix.size()) != kNamePrefix) {
    return 0;
  }
  int number = 0;
  const char* first = name.data() + kNamePrefix.size();
  const char* last = name.data() + name.size();
  const auto [end, err] = std::from_chars(first, last, number);
  return err == std::errc() && end == last && number > 0 ? number : 0;
}

// "CrashMOM GameSlot 4.sav" -> 4, 0 for anything else.
int NumberOfFile(const fs::path& file) {
  if (file.extension() != kExtension) {
    return 0;
  }
  return NumberOf(file.stem().string());
}

// "2026-10-02 18.05.31" (dots: Windows file names can't hold colons).
std::string Stamp() {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H.%M.%S", &local);
  return stamp;
}

// A free name "<stem> (<stamp>)[ n].sav" inside `folder`.
fs::path StampedName(const fs::path& folder, const std::string& stem) {
  const std::string stamp = Stamp();
  fs::path target = folder / fmt::format("{} ({}){}", stem, stamp, kExtension);
  std::error_code ec;
  for (int n = 2; fs::exists(target, ec); ++n) {  // two moves within one second
    target = folder / fmt::format("{} ({}) {}{}", stem, stamp, n, kExtension);
  }
  return target;
}

// The "played" list: number -> file-clock ticks of the last load.
std::map<int, int64_t> ReadPlayedFile(const fs::path& path) {
  std::map<int, int64_t> played;
  std::ifstream in(path);
  int number = 0;
  long long ticks = 0;
  while (in >> number >> ticks) {
    played[number] = ticks;
  }
  return played;
}
std::map<int, int64_t> ReadPlayed() {
  const fs::path folder = SavesFolder();
  return folder.empty() ? std::map<int, int64_t>() : ReadPlayedFile(folder / kPlayedFile);
}
void WritePlayed(const std::map<int, int64_t>& played) {
  const fs::path folder = SavesFolder();
  if (folder.empty()) {
    return;
  }
  std::string text;
  for (const auto& [number, ticks] : played) {
    text += fmt::format("{} {}\n", number, ticks);
  }
  std::string ignored;
  ReplaceFile(folder / kPlayedFile, std::vector<uint8_t>(text.begin(), text.end()), &ignored);
}

// Copies one file (temp + rename), keeping its modification date: the save
// list orders saves by it.
bool CopyKeepingDate(const fs::path& from, const fs::path& to, std::string* error) {
  std::vector<uint8_t> data;
  if (!ReadAll(from, data)) {
    *error = "can't read " + from.string();
    return false;
  }
  if (!ReplaceFile(to, data, error)) {
    return false;
  }
  std::error_code ec;
  const auto when = fs::last_write_time(from, ec);
  if (!ec) {
    fs::last_write_time(to, when, ec);
  }
  return true;
}

bool IsProfileFolderName(const std::string& name) {
  return name.size() == 16 &&
         std::all_of(name.begin(), name.end(), [](char c) { return std::isxdigit(uint8_t(c)); });
}

}  // namespace

std::string ContentName(int number) {
  return fmt::format("{}{}", kNamePrefix, number);
}

fs::path SavesFolder() {
  return game_folder::SavesFolder();
}

fs::path FileOf(std::string_view game_name) {
  const fs::path folder = SavesFolder();
  return folder.empty() ? folder : folder / (std::string(game_name) + std::string(kExtension));
}

fs::path FileOf(int number) {
  return FileOf(ContentName(number));
}

std::recursive_mutex& Lock() {
  static std::recursive_mutex lock;
  return lock;
}

bool Exists(int number) {
  std::error_code ec;
  const fs::path file = FileOf(number);
  return !file.empty() && fs::is_regular_file(file, ec);
}

int HighestNumber() {
  int highest = 0;
  for (const SaveInfo& save : ListSaves()) {
    highest = std::max(highest, save.number);
  }
  return highest;
}

std::vector<SaveInfo> ListSaves() {
  std::lock_guard guard(Lock());
  std::vector<SaveInfo> saves;
  const fs::path folder = SavesFolder();
  std::error_code ec;
  if (folder.empty() || !fs::is_directory(folder, ec)) {
    return saves;
  }
  const std::map<int, int64_t> loaded = ReadPlayed();
  for (const auto& entry : fs::directory_iterator(folder, ec)) {
    const int number = entry.is_regular_file(ec) ? NumberOfFile(entry.path()) : 0;
    if (!number) {
      continue;
    }
    const auto written = fs::last_write_time(entry.path(), ec);
    if (ec) {
      ec.clear();
      continue;
    }
    SaveInfo info{number, int64_t(written.time_since_epoch().count())};
    if (const auto it = loaded.find(number); it != loaded.end()) {
      info.played = std::max(info.played, it->second);
    }
    saves.push_back(info);
  }
  std::sort(saves.begin(), saves.end(), [](const SaveInfo& a, const SaveInfo& b) {
    return a.played != b.played ? a.played > b.played : a.number < b.number;
  });
  return saves;
}

std::vector<std::string> GameNames() {
  std::vector<std::string> names;
  for (const SaveInfo& save : ListSaves()) {
    names.push_back(ContentName(save.number));
  }
  std::sort(names.begin(), names.end());
  return names;
}

int FreeNumber() {
  std::lock_guard guard(Lock());
  int number = 1;
  while (Exists(number)) {
    ++number;
  }
  return number;
}

void MarkPlayed(int number) {
  std::lock_guard guard(Lock());
  std::map<int, int64_t> played = ReadPlayed();
  played[number] = int64_t(fs::file_time_type::clock::now().time_since_epoch().count());
  WritePlayed(played);
}

void ForgetPlayed(int number) {
  std::lock_guard guard(Lock());
  std::map<int, int64_t> played = ReadPlayed();
  if (played.erase(number)) {
    WritePlayed(played);
  }
}

bool ReadBlockStart(int number, uint8_t* out, size_t size) {
  std::lock_guard guard(Lock());
  if (number < 1) {
    return false;
  }
  std::ifstream in(FileOf(number), std::ios::binary);
  if (!in) {
    return false;
  }
  in.seekg(4);  // past the size word
  in.read(reinterpret_cast<char*>(out), std::streamsize(size));
  return size_t(in.gcount()) == size;
}

bool ReplaceFile(const fs::path& path, const std::vector<uint8_t>& data, std::string* error) {
  fs::path temp = path;
  temp += ".writing";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      *error = "can't write " + temp.string();
      return false;
    }
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    out.flush();
    if (!out) {
      *error = "can't write " + temp.string();
      return false;
    }
  }
  std::error_code ec;
  fs::rename(temp, path, ec);
  if (ec) {
    fs::remove(temp, ec);
    *error = "can't replace " + path.string();
    return false;
  }
  return true;
}

bool Rename(int number, std::u16string_view name, std::string* error) {
  std::lock_guard guard(Lock());
  if (name.size() > kMaxNameLength) {
    *error = "name too long";
    return false;
  }
  const fs::path save = FileOf(number);
  std::vector<uint8_t> data;
  if (save.empty() || !ReadAll(save, data)) {
    *error = "can't read " + save.string();
    return false;
  }
  // Only a file shaped like the game's saves: the size word and the size.
  const uint32_t size_word = uint32_t(data.size() >= 4 ? data[0] << 24 | data[1] << 16 |
                                                             data[2] << 8 | data[3]
                                                       : 0);
  if (data.size() != kFileSize || size_word != kFileSize) {
    *error = fmt::format("{} isn't a save of the expected size ({} bytes)", save.string(),
                         data.size());
    return false;
  }
  PutUtf16Be(data.data() + kNameOffset, kNameChars, name);
  // The file time says when the game last saved it (the list's order): a
  // rename keeps it.
  std::error_code time_error;
  const auto saved_at = fs::last_write_time(save, time_error);
  if (!ReplaceFile(save, data, error)) {
    return false;
  }
  if (!time_error) {
    fs::last_write_time(save, saved_at, time_error);
  }
  return true;
}

bool MoveToDeleted(int number, fs::path* moved_to, std::string* error) {
  std::lock_guard guard(Lock());
  const fs::path file = FileOf(number);
  std::error_code ec;
  if (file.empty() || !fs::is_regular_file(file, ec)) {
    *error = ContentName(number) + " doesn't exist";
    return false;
  }
  const fs::path folder = SavesFolder() / kDeletedFolder;
  fs::create_directories(folder, ec);
  if (ec) {
    *error = "can't create " + folder.string();
    return false;
  }
  const fs::path target = StampedName(folder, ContentName(number));
  fs::rename(file, target, ec);
  if (ec) {
    *error = fmt::format("can't move {}: {}", file.string(), ec.message());
    return false;
  }
  ForgetPlayed(number);
  *moved_to = target;
  return true;
}

bool MoveToBackups(const fs::path& file, fs::path* moved_to, std::string* error) {
  std::lock_guard guard(Lock());
  const fs::path folder = SavesFolder() / kBackupsFolder;
  std::error_code ec;
  fs::create_directories(folder, ec);
  if (ec) {
    *error = "can't create " + folder.string();
    return false;
  }
  const std::string stem = file.stem().string();
  const fs::path target = StampedName(folder, stem);
  fs::rename(file, target, ec);
  if (ec) {
    *error = fmt::format("can't move {}: {}", file.string(), ec.message());
    return false;
  }
  *moved_to = target;

  // Keep the newest kBackupsPerSave of this save ("<stem> (<stamp>)...":
  // the stamp sorts by time).
  std::vector<fs::path> mine;
  const std::string prefix = stem + " (";
  for (const auto& entry : fs::directory_iterator(folder, ec)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind(prefix, 0) == 0 && entry.path().extension() == kExtension) {
      mine.push_back(entry.path());
    }
  }
  std::sort(mine.begin(), mine.end());
  for (size_t i = 0; i + kBackupsPerSave < mine.size(); ++i) {
    fs::remove(mine[i], ec);
  }
  return true;
}

std::vector<std::string> CopyFromXboxLayout() {
  std::lock_guard guard(Lock());
  std::vector<std::string> log;
  const fs::path folder = SavesFolder();
  std::error_code ec;
  if (folder.empty() || !fs::is_directory(folder, ec)) {
    return log;
  }
  const fs::path note = folder / kCopiedNote;
  if (fs::exists(note, ec)) {
    return log;  // done before: never again (a save deleted since stays deleted)
  }

  // Every profile folder (16 hex digits) with saves of this game, in name
  // order (one profile on every port install so far).
  std::vector<fs::path> profiles;
  for (const auto& entry : fs::directory_iterator(folder, ec)) {
    if (entry.is_directory(ec) && IsProfileFolderName(entry.path().filename().string()) &&
        fs::is_directory(entry.path() / kTitleId / kSavedGameType, ec)) {
      profiles.push_back(entry.path());
    }
  }
  std::sort(profiles.begin(), profiles.end());
  if (profiles.empty()) {
    return log;  // nothing in the old layout (a new install): no note either
  }

  std::string note_text =
      "Mind over Recomp copied these saves from the old Xbox-style folders into this folder\n"
      "(the old folders were left exactly as they were: they are a backup).\n"
      "While this file exists, the copy doesn't run again.\n\n";
  int copied = 0, failed = 0;
  std::map<int, int64_t> played = ReadPlayed();
  for (const fs::path& profile : profiles) {
    const fs::path content = profile / kTitleId / kSavedGameType;
    const std::map<int, int64_t> old_played =
        ReadPlayedFile(profile / kTitleId / kPlayedFile);
    std::vector<fs::path> saves;
    for (const auto& entry : fs::directory_iterator(content, ec)) {
      const std::string name = entry.path().filename().string();
      if (entry.is_directory(ec) && NumberOf(name) && fs::is_regular_file(entry.path() / name, ec)) {
        saves.push_back(entry.path() / name);
      }
    }
    std::sort(saves.begin(), saves.end());
    for (const fs::path& old_file : saves) {
      const int old_number = NumberOf(old_file.filename().string());
      // Already here with the same bytes (copied by hand, by an earlier try
      // that stopped half way, or a 2nd profile holding the same file): skip.
      std::vector<uint8_t> old_data;
      ReadAll(old_file, old_data);
      bool already = false;
      for (const SaveInfo& save : ListSaves()) {
        std::vector<uint8_t> have;
        if (ReadAll(FileOf(save.number), have) && have == old_data) {
          already = true;
          break;
        }
      }
      if (already) {
        note_text += fmt::format("{}: already here\n", old_file.string());
        continue;
      }
      // Its own number if that's free, else the next free one.
      const int number = Exists(old_number) ? FreeNumber() : old_number;
      std::string error;
      if (!CopyKeepingDate(old_file, FileOf(number), &error)) {
        ++failed;
        log.push_back(fmt::format("COULD NOT copy {}: {}", old_file.string(), error));
        note_text += fmt::format("{}: NOT COPIED ({})\n", old_file.string(), error);
        continue;
      }
      ++copied;
      if (const auto it = old_played.find(old_number); it != old_played.end()) {
        // (File-clock ticks can be negative: libstdc++'s clock counts from
        // 2174. Never compare against a default 0.)
        const auto have = played.find(number);
        played[number] = have == played.end() ? it->second : std::max(have->second, it->second);
      }
      const bool usual = old_data.size() == kFileSize;
      log.push_back(fmt::format("copied {} -> {}{}", old_file.string(),
                                FileOf(number).filename().string(),
                                usual ? "" : fmt::format(" (unusual size: {} bytes)", old_data.size())));
      note_text += fmt::format("{} -> {}\n", old_file.string(), FileOf(number).filename().string());
    }
  }
  WritePlayed(played);
  // A failed copy leaves no note: the next start tries again (copies that
  // worked are skipped then: same bytes).
  if (failed == 0) {
    std::string error;
    if (!ReplaceFile(note, std::vector<uint8_t>(note_text.begin(), note_text.end()), &error)) {
      log.push_back("COULD NOT write " + note.string() + ": " + error);
    }
  }
  log.push_back(fmt::format("old Xbox-style saves: {} copied, {} failed (the old folders are kept "
                            "as they were)",
                            copied, failed));
  return log;
}

}  // namespace save_files
