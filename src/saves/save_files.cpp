// =============================================================================
// saves/save_files.cpp -- see save_files.h
// =============================================================================
#include "save_files.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <system_error>
#include <vector>

#include <fmt/format.h>

#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>

namespace save_files {
namespace {

// The game's own names (its save manager formats "%s GameSlot %d" with the
// prefix "CrashMOM", findings/24 section 2.2).
constexpr std::string_view kNamePrefix = "CrashMOM GameSlot ";
constexpr std::string_view kSavedGameType = "00000001";  // XContentType::kSavedGame
constexpr std::string_view kDeletedFolder = "Deleted saves";
constexpr std::string_view kPlayedFile = "save_library_played.txt";

// The save file's layout (save_files.h).
constexpr size_t kFileSize = 0x4EED;
constexpr size_t kNameOffset = 4 + 20;  // the size word, then block +20
constexpr size_t kNameChars = 32;       // the field, terminator included
// The header (XCONTENT_AGGREGATE_DATA): the display name after the device id
// and content type.
constexpr size_t kHeaderSize = 0x148;
constexpr size_t kDisplayNameOffset = 8;
constexpr size_t kDisplayNameChars = 128;

// <user data>/<xuid>/<title id>: the profile's content for this game.
std::filesystem::path TitleFolder() {
  rex::Runtime* runtime = rex::Runtime::instance();
  rex::system::KernelState* kernel = rex::system::kernel_state();
  if (!runtime || runtime->user_data_root().empty() || !kernel || !kernel->user_profile()) {
    return {};
  }
  // The runtime makes the root absolute the same way (KernelState's
  // ContentManager), so relative --user_data_root values agree.
  const std::filesystem::path root = std::filesystem::absolute(runtime->user_data_root());
  return root / fmt::format("{:016X}", kernel->user_profile()->xuid()) /
         fmt::format("{:08X}", kernel->title_id());
}

std::filesystem::path HeaderPath(int number) {
  const std::filesystem::path title = TitleFolder();
  if (title.empty()) {
    return {};
  }
  return title / "Headers" / kSavedGameType / (ContentName(number) + ".header");
}

// Reads a whole file; false if it can't be opened.
bool ReadAll(const std::filesystem::path& path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

// Writes `data` next to `path` and renames it over: a crash mid-write
// leaves the old file whole.
bool ReplaceFile(const std::filesystem::path& path, const std::vector<uint8_t>& data,
                 std::string* error) {
  std::filesystem::path temp = path;
  temp += ".renaming";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      *error = "can't write " + temp.string();
      return false;
    }
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    if (!out) {
      *error = "can't write " + temp.string();
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    std::filesystem::remove(temp, ec);
    *error = "can't replace " + path.string();
    return false;
  }
  return true;
}

// UTF-16 <-> big-endian bytes (the guest's byte order, in the files too).
void PutUtf16Be(uint8_t* out, size_t chars, std::u16string_view text) {
  for (size_t i = 0; i < chars; ++i) {
    const char16_t c = i < text.size() ? text[i] : u'\0';
    out[2 * i] = uint8_t(c >> 8);
    out[2 * i + 1] = uint8_t(c & 0xFF);
  }
}
std::u16string GetUtf16Be(const uint8_t* in, size_t chars) {
  std::u16string text;
  for (size_t i = 0; i < chars; ++i) {
    const char16_t c = char16_t(in[2 * i] << 8 | in[2 * i + 1]);
    if (c == 0) {
      break;
    }
    text.push_back(c);
  }
  return text;
}

// The header's display name with the name part replaced: the game's
// "Name: <old> Slot: ..." keeps everything after " Slot:". Anything else
// (a header written by something else) is left alone.
bool RenameInDisplayName(std::u16string& display, std::u16string_view name) {
  constexpr std::u16string_view kStart = u"Name: ";
  constexpr std::u16string_view kSlot = u" Slot:";
  if (display.rfind(kStart, 0) != 0) {
    return false;
  }
  const size_t slot = display.find(kSlot, kStart.size());
  if (slot == std::u16string::npos) {
    return false;
  }
  display = std::u16string(kStart) + std::u16string(name) + display.substr(slot);
  return true;
}

// "CrashMOM GameSlot <digits>" -> the number, 0 if the name isn't one (a
// hand-made "CrashMOM GameSlot 4 copy" isn't a save the game can open).
int NumberOf(const std::string& name) {
  if (name.size() <= kNamePrefix.size() || name.compare(0, kNamePrefix.size(), kNamePrefix)) {
    return 0;
  }
  int number = 0;
  const char* first = name.data() + kNamePrefix.size();
  const char* last = name.data() + name.size();
  const auto [end, err] = std::from_chars(first, last, number);
  return err == std::errc() && end == last && number > 0 ? number : 0;
}

// The "played" list: number -> file-clock ticks of the last load.
std::map<int, int64_t> ReadPlayed() {
  std::map<int, int64_t> played;
  const std::filesystem::path title = TitleFolder();
  if (title.empty()) {
    return played;
  }
  std::ifstream in(title / kPlayedFile);
  int number = 0;
  long long ticks = 0;
  while (in >> number >> ticks) {
    played[number] = ticks;
  }
  return played;
}
void WritePlayed(const std::map<int, int64_t>& played) {
  const std::filesystem::path title = TitleFolder();
  if (title.empty()) {
    return;
  }
  std::string text;
  for (const auto& [number, ticks] : played) {
    text += fmt::format("{} {}\n", number, ticks);
  }
  std::string ignored;
  ReplaceFile(title / kPlayedFile, std::vector<uint8_t>(text.begin(), text.end()), &ignored);
}

}  // namespace

std::string ContentName(int number) {
  return fmt::format("{}{}", kNamePrefix, number);
}

std::filesystem::path SavesFolder() {
  const std::filesystem::path title = TitleFolder();
  return title.empty() ? title : title / kSavedGameType;
}

bool Exists(int number) {
  const std::filesystem::path folder = SavesFolder();
  std::error_code ec;
  return !folder.empty() && std::filesystem::is_directory(folder / ContentName(number), ec);
}

int HighestNumber() {
  const std::filesystem::path folder = SavesFolder();
  std::error_code ec;
  if (folder.empty() || !std::filesystem::is_directory(folder, ec)) {
    return 0;
  }
  int highest = 0;
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
    if (entry.is_directory(ec)) {
      highest = std::max(highest, NumberOf(entry.path().filename().string()));
    }
  }
  return highest;
}

std::vector<SaveInfo> ListSaves() {
  std::vector<SaveInfo> saves;
  const std::filesystem::path folder = SavesFolder();
  std::error_code ec;
  if (folder.empty() || !std::filesystem::is_directory(folder, ec)) {
    return saves;
  }
  const std::map<int, int64_t> loaded = ReadPlayed();
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
    const int number = entry.is_directory(ec) ? NumberOf(entry.path().filename().string()) : 0;
    if (!number) {
      continue;
    }
    const std::filesystem::path file = entry.path() / ContentName(number);
    const auto written = std::filesystem::last_write_time(file, ec);
    if (ec) {
      ec.clear();
      continue;  // a folder without its save file
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

int FreeNumber() {
  int number = 1;
  while (Exists(number)) {
    ++number;
  }
  return number;
}

void MarkPlayed(int number) {
  std::map<int, int64_t> played = ReadPlayed();
  played[number] = int64_t(std::filesystem::file_time_type::clock::now().time_since_epoch().count());
  WritePlayed(played);
}

void ForgetPlayed(int number) {
  std::map<int, int64_t> played = ReadPlayed();
  if (played.erase(number)) {
    WritePlayed(played);
  }
}

bool ReadBlockStart(int number, uint8_t* out, size_t size) {
  const std::filesystem::path folder = SavesFolder();
  if (folder.empty() || number < 1) {
    return false;
  }
  const std::string content = ContentName(number);
  std::ifstream in(folder / content / content, std::ios::binary);
  if (!in) {
    return false;
  }
  in.seekg(4);  // past the size word
  in.read(reinterpret_cast<char*>(out), std::streamsize(size));
  return size_t(in.gcount()) == size;
}

bool Rename(int number, std::u16string_view name, std::string* error) {
  if (name.size() > kMaxNameLength) {
    *error = "name too long";
    return false;
  }
  const std::filesystem::path folder = SavesFolder();
  if (folder.empty()) {
    *error = "no profile";
    return false;
  }
  const std::string content = ContentName(number);
  const std::filesystem::path save = folder / content / content;
  std::vector<uint8_t> data;
  if (!ReadAll(save, data)) {
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
  const auto saved_at = std::filesystem::last_write_time(save, time_error);
  if (!ReplaceFile(save, data, error)) {
    return false;
  }
  if (!time_error) {
    std::filesystem::last_write_time(save, saved_at, time_error);
  }

  // The header's display name too (cosmetic: the game never reads it), if
  // it looks like the game's own.
  const std::filesystem::path header = HeaderPath(number);
  std::vector<uint8_t> head;
  if (ReadAll(header, head) && head.size() >= kHeaderSize) {
    std::u16string display = GetUtf16Be(head.data() + kDisplayNameOffset, kDisplayNameChars);
    if (RenameInDisplayName(display, name)) {
      display.resize(std::min(display.size(), kDisplayNameChars - 1));
      PutUtf16Be(head.data() + kDisplayNameOffset, kDisplayNameChars, display);
      std::string ignored;
      ReplaceFile(header, head, &ignored);  // the save itself is renamed already
    }
  }
  return true;
}

bool MoveToDeleted(int number, std::filesystem::path* moved_to, std::string* error) {
  const std::filesystem::path folder = SavesFolder();
  const std::string content = ContentName(number);
  std::error_code ec;
  if (folder.empty() || !std::filesystem::is_directory(folder / content, ec)) {
    *error = content + " doesn't exist";
    return false;
  }
  // "CrashMOM GameSlot 4 (2026-10-02 18.05.31)": unique enough, readable
  // (dots: Windows folder names can't hold colons).
  const std::time_t now = std::time(nullptr);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H.%M.%S", &local);
  const std::filesystem::path target =
      folder.parent_path() / kDeletedFolder / fmt::format("{} ({})", content, stamp);
  std::filesystem::create_directories(target, ec);
  if (ec) {
    *error = "can't create " + target.string();
    return false;
  }
  std::filesystem::rename(folder / content, target / content, ec);
  if (ec) {
    *error = fmt::format("can't move {}: {}", content, ec.message());
    return false;
  }
  // The header goes along (without it the runtime would still list the
  // content by its folder name, but there's no folder any more).
  const std::filesystem::path header = HeaderPath(number);
  if (std::filesystem::exists(header, ec)) {
    std::filesystem::rename(header, target / header.filename(), ec);
  }
  ForgetPlayed(number);
  *moved_to = target;
  return true;
}

}  // namespace save_files
