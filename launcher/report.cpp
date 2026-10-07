// =============================================================================
// report.cpp -- see report.h
// =============================================================================
#include "report.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <set>

#include "platform.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX  // windows.h's min/max macros would break std::min / std::max
#endif
#include <windows.h>
#else
#include <sys/utsname.h>
#endif

namespace fs = std::filesystem;

namespace report {
namespace {

// Where the bug form lives (.github/ISSUE_TEMPLATE/bug_report.yml).
constexpr const char* kBugForm =
    "https://github.com/Maxilure/MindOver-Recomp/issues/new?template=bug_report.yml";

// A log bigger than this keeps only its head and tail (report.h).
constexpr uint64_t kMaxLog = 8ull << 20;
constexpr uint64_t kKeepHead = 2ull << 20;
constexpr uint64_t kKeepTail = 6ull << 20;
// GitHub's limit for an attachment.
constexpr uint64_t kGitHubLimit = 25ull << 20;

const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

std::string ReadAll(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The first line of a file (a log's first line holds its start time).
std::string FirstLine(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::string line;
  std::getline(in, line);
  return line;
}

// "[2026-10-07 16:20:41.191] ..." -> that local time; 0 if it isn't one.
std::time_t LineTime(const std::string& line) {
  std::tm tm{};
  if (std::sscanf(line.c_str(), "[%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                  &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) {
    return 0;
  }
  tm.tm_year -= 1900;
  tm.tm_mon -= 1;
  tm.tm_isdst = -1;
  return std::mktime(&tm);
}

// A file's last change as a time_t.
std::time_t ChangedAt(const fs::path& path) {
  std::error_code ec;
  const auto file_time = fs::last_write_time(path, ec);
  if (ec) return 0;
  return std::chrono::system_clock::to_time_t(
      std::chrono::clock_cast<std::chrono::system_clock>(file_time));
}

std::string When(std::time_t t) {
  const std::tm tm = platform::LocalTime(t);
  char text[48];
  std::snprintf(text, sizeof(text), "%s %d, %02d:%02d", kMonths[tm.tm_mon], tm.tm_mday,
                tm.tm_hour, tm.tm_min);
  return text;
}

std::string Length(long seconds) {
  char text[48];
  if (seconds >= 3600) {
    std::snprintf(text, sizeof(text), "%ld h %02ld min", seconds / 3600, seconds / 60 % 60);
  } else if (seconds >= 60) {
    std::snprintf(text, sizeof(text), "%ld min", seconds / 60);
  } else {
    std::snprintf(text, sizeof(text), "%ld s", std::max(seconds, 0L));
  }
  return text;
}

std::string Megabytes(uint64_t bytes) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f MB", bytes / 1048576.0);
  return text;
}

// The message part of a log line (after "[time] [level] [category] [thread] ").
std::string MessageOf(const std::string& line) {
  size_t at = 0;
  for (int i = 0; i < 4 && at != std::string::npos; ++i) {
    at = line.find("] ", at);
    if (at != std::string::npos) at += 2;
  }
  return at == std::string::npos ? line : line.substr(at);
}

// Files in `folder` whose last change falls in [from, to].
std::vector<fs::path> ChangedBetween(const fs::path& folder, std::time_t from, std::time_t to,
                                     const std::string& prefix = {}) {
  std::vector<fs::path> found;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(folder, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    if (!prefix.empty() && !entry.path().filename().string().starts_with(prefix)) continue;
    const std::time_t t = ChangedAt(entry.path());
    if (t >= from && t <= to) found.push_back(entry.path());
  }
  std::sort(found.begin(), found.end());
  return found;
}

// A session's time window: its first log line .. the log's last change
// (+5 s: a photo or crash file written as the game went down).
std::pair<std::time_t, std::time_t> Window(const fs::path& log) {
  std::time_t from = LineTime(FirstLine(log));
  const std::time_t to = ChangedAt(log);
  if (from == 0) from = to;
  return {from - 5, to + 5};
}

// The system, asked by the launcher itself: for a report without any game
// session (e.g. a Setup problem). The game's log says the same and more
// (src/session_log.cpp), so this is only the fallback.
std::string LauncherSystem() {
#if defined(_WIN32)
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  OSVERSIONINFOW info{};
  info.dwOSVersionInfoSize = sizeof(info);
  if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
    if (auto get = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))) {
      get(&info);
    }
  }
  if (info.dwMajorVersion == 0) return "Windows";
  const unsigned shown = info.dwMajorVersion == 10 && info.dwBuildNumber >= 22000 ? 11 : info.dwMajorVersion;
  return "Windows " + std::to_string(shown) + " build " + std::to_string(info.dwBuildNumber);
#else
  utsname name{};
  std::string out = uname(&name) == 0 ? std::string(name.sysname) + " " + name.release : "Linux";
  std::ifstream in("/etc/os-release");
  std::string line;
  while (std::getline(in, line)) {
    if (line.starts_with("PRETTY_NAME=")) {
      std::string distro = line.substr(12);
      std::erase(distro, '"');
      out += " (" + distro + ")";
    }
  }
  const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
  const char* session = std::getenv("XDG_SESSION_TYPE");
  if (desktop || session) {
    out += std::string(", desktop ") + (desktop ? desktop : "?") + " (" + (session ? session : "?") + ")";
  }
  return out;
#endif
}

// --- privacy -------------------------------------------------------------------

// Replaces the home folder's path with "~" (both slash styles on Windows).
std::string Scrub(std::string text) {
  std::vector<std::string> homes;
  const fs::path home = platform::HomeDir();
  if (!home.empty()) {
    homes.push_back(home.string());
    homes.push_back(home.generic_string());
  }
  for (const std::string& needle : homes) {
    if (needle.size() < 3) continue;  // "/" or "C:" would wreck everything
    size_t at = 0;
    while ((at = text.find(needle, at)) != std::string::npos) {
      text.replace(at, needle.size(), "~");
      at += 1;
    }
  }
  return text;
}

// --- the zip writer ------------------------------------------------------------------

uint32_t Crc32(const std::string& data) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  uint32_t crc = 0xFFFFFFFFu;
  for (unsigned char byte : data) crc = table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

// A .zip of "stored" (uncompressed) entries, little-endian records:
// [local header + name + data] per file, then the central directory (one
// record per file) and its end record. Times are MS-DOS local time.
class Zip {
 public:
  explicit Zip(const fs::path& path) : out_(path, std::ios::binary | std::ios::trunc) {}
  bool ok() const { return bool(out_); }

  void Add(const std::string& name, const std::string& data, std::time_t when) {
    Entry entry{name, Crc32(data), uint32_t(data.size()), uint32_t(out_.tellp()), DosTime(when)};
    U32(0x04034B50);              // local file header
    U16(10); U16(0x0800);         // version needed 1.0; flags: UTF-8 names
    U16(0);                       // method: stored
    U32(entry.dos_time);
    U32(entry.crc); U32(entry.size); U32(entry.size);
    U16(uint16_t(name.size())); U16(0);
    out_.write(name.data(), std::streamsize(name.size()));
    out_.write(data.data(), std::streamsize(data.size()));
    entries_.push_back(std::move(entry));
  }

  bool Finish() {
    const uint32_t directory_at = uint32_t(out_.tellp());
    for (const Entry& e : entries_) {
      U32(0x02014B50);            // central directory record
      U16(20); U16(10); U16(0x0800); U16(0);
      U32(e.dos_time);
      U32(e.crc); U32(e.size); U32(e.size);
      U16(uint16_t(e.name.size())); U16(0); U16(0);  // name, extra, comment lengths
      U16(0); U16(0); U32(0);     // disk, internal attributes, external attributes
      U32(e.offset);
      out_.write(e.name.data(), std::streamsize(e.name.size()));
    }
    const uint32_t directory_size = uint32_t(out_.tellp()) - directory_at;
    U32(0x06054B50);              // end of central directory
    U16(0); U16(0);
    U16(uint16_t(entries_.size())); U16(uint16_t(entries_.size()));
    U32(directory_size); U32(directory_at);
    U16(0);
    out_.close();
    return !out_.fail();
  }

 private:
  struct Entry {
    std::string name;
    uint32_t crc, size, offset, dos_time;
  };
  static uint32_t DosTime(std::time_t t) {
    const std::tm tm = platform::LocalTime(t);
    const int year = std::max(tm.tm_year + 1900, 1980);
    return uint32_t((year - 1980) << 25 | (tm.tm_mon + 1) << 21 | tm.tm_mday << 16 |
                    tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
  }
  void U16(uint16_t v) { const char b[2] = {char(v), char(v >> 8)}; out_.write(b, 2); }
  void U32(uint32_t v) { U16(uint16_t(v)); U16(uint16_t(v >> 16)); }

  std::ofstream out_;
  std::vector<Entry> entries_;
};

// --- what the logs say -----------------------------------------------------------------

struct LogFacts {
  std::map<std::string, std::string> session;  // "system" -> "Linux ..." (the Session: header)
  std::string gpu, driver, ended, fps;
  int errors = 0, warnings = 0;
  std::set<std::string> error_kinds, warning_kinds;
};

LogFacts ReadFacts(const std::string& text) {
  LogFacts facts;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(start, end - start);
    start = end + 1;
    const std::string message = MessageOf(line);
    if (line.find("] [error] ") != std::string::npos) {
      ++facts.errors;
      facts.error_kinds.insert(message);
    } else if (line.find("] [warning] ") != std::string::npos) {
      ++facts.warnings;
      facts.warning_kinds.insert(message);
    }
    if (message.starts_with("Session: ")) {
      // "Session: system X" / "Session: CPU X" / "Session: settings changed from the defaults: X"
      static const char* const kKeys[] = {"Mind over Recomp ", "system ", "CPU ", "started with ",
                                          "settings changed from the defaults: "};
      for (const char* key : kKeys) {
        if (message.compare(9, std::strlen(key), key) == 0) {
          facts.session[key] = message.substr(9 + std::strlen(key));
        }
      }
    } else if (message.starts_with("Vulkan device '")) {
      // "Vulkan device 'NVIDIA GeForce GTX 1660 SUPER': API 1.4.351, ..."
      const size_t close = message.find('\'', 15);
      if (close != std::string::npos) facts.gpu = message.substr(15, close - 15);
    } else if (message.starts_with("* driverInfo: ")) {
      facts.driver = message.substr(14);
    } else if (message.starts_with("Launcher: the game")) {
      facts.ended = message.substr(10);
    } else if (const size_t at = message.find("frame_rate: at exit, "); at != std::string::npos) {
      facts.fps = message.substr(at + 21);
    }
  }
  return facts;
}

// A log too big for a report: its head + tail, cut at line ends.
std::string Shorten(const std::string& text) {
  if (text.size() <= kMaxLog) return text;
  size_t head = text.rfind('\n', kKeepHead);
  head = head == std::string::npos ? kKeepHead : head + 1;
  size_t tail = text.find('\n', text.size() - kKeepTail);
  tail = tail == std::string::npos ? text.size() - kKeepTail : tail + 1;
  return text.substr(0, head) + "\n... (report: " + Megabytes(tail - head) +
         " cut out of the middle of this log) ...\n\n" + text.substr(tail);
}

std::string UrlEncode(const std::string& text) {
  std::string out;
  for (unsigned char c : text) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += char(c);
    } else {
      char hex[4];
      std::snprintf(hex, sizeof(hex), "%%%02X", c);
      out += hex;
    }
  }
  return out;
}

}  // namespace

std::vector<Session> RecentSessions(const folders::Folders& folders, int count) {
  std::vector<std::pair<std::time_t, fs::path>> logs;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(folders.user / "logs", ec)) {
    const std::string name = entry.path().filename().string();
    if (entry.is_regular_file(ec) && name.starts_with("play-") && name.ends_with(".log")) {
      logs.emplace_back(ChangedAt(entry.path()), entry.path());
    }
  }
  std::sort(logs.begin(), logs.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  if (int(logs.size()) > count) logs.resize(size_t(count));

  std::vector<Session> sessions;
  for (const auto& [changed, path] : logs) {
    Session session;
    session.log = path;
    session.bytes = fs::file_size(path, ec);
    const std::time_t start = LineTime(FirstLine(path));
    session.when = When(start ? start : changed);
    session.length = start ? Length(long(changed - start)) : "?";
    const auto [from, to] = Window(path);
    for (const fs::path& photo : ChangedBetween(folders.user / "photos", from, to)) {
      ++session.photos;
      session.photo_bytes += fs::file_size(photo, ec);
    }
    // How it ended: the launcher's closing line (game_process.cpp), at the
    // end of the file: read the last few KB only.
    std::ifstream in(path, std::ios::binary);
    const uint64_t tail = std::min<uint64_t>(session.bytes, 4096);
    in.seekg(std::streamoff(session.bytes - tail));
    std::string end(tail, '\0');
    in.read(end.data(), std::streamsize(tail));
    if (const size_t at = end.rfind("Launcher: the game"); at != std::string::npos) {
      session.ended = end.substr(at + 10, end.find('\n', at) - at - 10);
    }
    sessions.push_back(std::move(session));
  }
  return sessions;
}

fs::path NewestSetupLog(const folders::Folders& folders) {
  fs::path newest;
  std::time_t newest_time = 0;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(folders.user / "logs", ec)) {
    const std::string name = entry.path().filename().string();
    if (name.starts_with("setup-") && name.ends_with(".log")) {
      const std::time_t t = ChangedAt(entry.path());
      if (t > newest_time) {
        newest_time = t;
        newest = entry.path();
      }
    }
  }
  return newest;
}

namespace {

// user/launcher.toml's report_folder line (report.h), or empty.
fs::path ReadRememberedFolder(const folders::Folders& folders) {
  std::ifstream in(folders.user / "launcher.toml");
  std::string line, value;
  while (std::getline(in, line)) {
    if (!line.starts_with("report_folder")) continue;
    // report_folder = "<path>"  (backslashes and quotes escaped)
    const size_t open = line.find('"');
    if (open == std::string::npos) continue;
    value.clear();
    for (size_t i = open + 1; i < line.size() && line[i] != '"'; ++i) {
      if (line[i] == '\\' && i + 1 < line.size()) ++i;
      value += line[i];
    }
  }
  return value;
}

}  // namespace

fs::path ReportFolder(const folders::Folders& folders) {
  const fs::path chosen = ReadRememberedFolder(folders);
  std::error_code ec;
  if (!chosen.empty() && fs::is_directory(chosen, ec)) return chosen;
  return folders.user / "reports";
}

void RememberReportFolder(const folders::Folders& folders, const fs::path& folder) {
  // Rewrites launcher.toml with every other line kept (desktop_entry.cpp's
  // menu_entry_added lives there too).
  const fs::path file = folders.user / "launcher.toml";
  std::vector<std::string> lines;
  {
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
      if (!line.starts_with("report_folder")) lines.push_back(line);
    }
  }
  if (!folder.empty() && folder != folders.user / "reports") {
    std::string quoted;
    for (char c : folder.string()) {
      if (c == '\\' || c == '"') quoted += '\\';
      quoted += c;
    }
    lines.push_back("report_folder = \"" + quoted + "\"  # where \"Report a problem\" saves");
  }
  std::error_code ec;
  fs::create_directories(folders.user, ec);
  const fs::path temp = file.string() + ".new";
  {
    std::ofstream out(temp, std::ios::trunc);
    for (const std::string& line : lines) out << line << "\n";
  }
  fs::rename(temp, file, ec);
}

namespace {

// A save file and its header, with their places under user/saves (report.h):
// user/saves/<profile>/565507FA/00000001/CrashMOM GameSlot N/CrashMOM GameSlot N
// user/saves/<profile>/565507FA/Headers/00000001/CrashMOM GameSlot N.header
std::vector<std::pair<std::string, fs::path>> SaveFiles(const folders::Folders& folders,
                                                        const fs::path& file) {
  std::vector<std::pair<std::string, fs::path>> files;
  std::error_code ec;
  const fs::path saves = folders.user / "saves";
  const fs::path slot = file.parent_path();      // CrashMOM GameSlot N/
  const fs::path content = slot.parent_path();   // 00000001/
  const fs::path title = content.parent_path();  // 565507FA/
  const fs::path header = title / "Headers" / content.filename() / (slot.filename().string() + ".header");
  for (const fs::path& path : {file, header}) {
    if (!fs::exists(path, ec)) continue;
    const fs::path relative = fs::relative(path, saves, ec);
    if (ec || relative.empty()) continue;
    files.emplace_back("saves/" + relative.generic_string(), path);
  }
  return files;
}

}  // namespace

bool Build(const folders::Folders& folders, const Request& request, Built* built,
           std::string* error) {
  const fs::path reports = request.folder.empty() ? folders.user / "reports" : request.folder;
  std::error_code ec;
  fs::create_directories(reports, ec);
  const std::time_t now = std::time(nullptr);
  const std::tm local = platform::LocalTime(now);
  char name[64];
  std::strftime(name, sizeof(name), "report-%Y-%m-%d_%H%M%S.zip", &local);
  Built result;
  result.file = reports / name;
  Zip zip(result.file);
  if (!zip.ok()) {
    *error = "Couldn't write " + result.file.string();
    return false;
  }

  // The chosen sessions, newest first (the order they were picked in).
  std::vector<fs::path> logs = request.logs;
  std::sort(logs.begin(), logs.end(),
            [](const fs::path& a, const fs::path& b) { return ChangedAt(a) > ChangedAt(b); });
  const fs::path logs_folder = folders.user / "logs";

  std::string sessions_text;
  LogFacts newest;            // the newest log's facts fill the summary's top part
  std::set<fs::path> added;   // crash files etc. could belong to two windows
  auto add_file = [&](const std::string& zip_name, const fs::path& path, bool text) {
    if (!added.insert(path).second) return;
    std::string data = ReadAll(path);
    if (text) data = Scrub(Shorten(data));
    zip.Add(zip_name, data, ChangedAt(path));
    result.contents.push_back(zip_name);
  };

  // The newest log on disk: last-launch-output.txt belongs to it.
  std::time_t newest_on_disk = 0;
  for (const auto& entry : fs::directory_iterator(logs_folder, ec)) {
    const std::string n = entry.path().filename().string();
    if (n.starts_with("play-") && n.ends_with(".log")) {
      newest_on_disk = std::max(newest_on_disk, ChangedAt(entry.path()));
    }
  }

  for (size_t i = 0; i < logs.size(); ++i) {
    const fs::path& log = logs[i];
    const std::string text = ReadAll(log);
    LogFacts facts = ReadFacts(text);
    const std::time_t start = LineTime(FirstLine(log));
    const std::time_t changed = ChangedAt(log);
    sessions_text += "  " + log.filename().string() + ": " + When(start ? start : changed) +
                     ", " + (start ? Length(long(changed - start)) : "?") + ", " +
                     std::to_string(facts.errors) + " errors (" +
                     std::to_string(facts.error_kinds.size()) + " different), " +
                     std::to_string(facts.warnings) + " warnings (" +
                     std::to_string(facts.warning_kinds.size()) + " different)\n";
    if (!facts.ended.empty()) sessions_text += "    " + facts.ended + "\n";
    if (!facts.fps.empty()) sessions_text += "    frame rate: " + facts.fps + "\n";
    if (text.size() > kMaxLog) {
      sessions_text += "    (" + Megabytes(text.size()) + " log: only its start and end are in the report)\n";
    }
    if (i == 0) newest = std::move(facts);

    zip.Add("logs/" + log.filename().string(), Scrub(Shorten(text)), changed);
    result.contents.push_back("logs/" + log.filename().string());
    added.insert(log);

    const auto [from, to] = Window(log);
    for (const fs::path& crash : ChangedBetween(logs_folder, from, to, "crash-")) {
      add_file("logs/" + crash.filename().string(), crash, true);
    }
    if (changed == newest_on_disk) {
      const fs::path output = logs_folder / "last-launch-output.txt";
      if (fs::exists(output, ec)) add_file("logs/last-launch-output.txt", output, true);
    }
    if (request.photos) {
      for (const fs::path& photo : ChangedBetween(folders.user / "photos", from, to)) {
        add_file("photos/" + photo.filename().string(), photo, photo.extension() == ".txt");
      }
    }
  }
  if (request.setup_log) {
    const fs::path setup = NewestSetupLog(folders);
    if (!setup.empty()) add_file("logs/" + setup.filename().string(), setup, true);
  }
  if (request.settings) {
    for (const char* file : {"settings.toml", "controls.toml"}) {
      if (fs::exists(folders.user / file, ec)) add_file(std::string("settings/") + file, folders.user / file, true);
    }
  }

  std::string save_text;
  if (!request.save_file.empty()) {
    const auto files = SaveFiles(folders, request.save_file);
    for (const auto& [zip_name, path] : files) {
      add_file(zip_name, path, false);  // binary: as it is
    }
    save_text = files.empty() ? "(couldn't be read)" : request.save_label;
  }

  // No session chosen (a Setup problem, say): the summary's facts still come
  // from the newest session log on disk, if there is one, else the launcher
  // says what it knows (the system).
  std::string facts_from;
  if (logs.empty()) {
    std::time_t newest_time = 0;
    fs::path newest_log;
    for (const auto& entry : fs::directory_iterator(logs_folder, ec)) {
      const std::string n = entry.path().filename().string();
      if (n.starts_with("play-") && n.ends_with(".log") && ChangedAt(entry.path()) > newest_time) {
        newest_time = ChangedAt(entry.path());
        newest_log = entry.path();
      }
    }
    if (!newest_log.empty()) {
      newest = ReadFacts(ReadAll(newest_log));
      facts_from = "  (no session picked: the facts above are from the newest one, " +
                   newest_log.filename().string() + ", not included)\n";
    }
    if (!newest.session.count("system ")) {
      newest.session["system "] = LauncherSystem();
    }
  }

  // summary.txt
  auto fact = [&](const char* key, const char* missing) {
    const auto it = newest.session.find(key);
    return it == newest.session.end() ? std::string(missing) : it->second;
  };
  const std::string version =
      newest.session.count("Mind over Recomp ")
          ? fact("Mind over Recomp ", "")
          : (request.version.empty() ? std::string("?") : request.version);
  // The log says "0.1.0-alpha (built <date>)": the form wants the version alone.
  const std::string version_only = version.substr(0, version.find(" (built"));
  const std::string system = Scrub(fact("system ", "(not in the log)"));
  std::string gpu = newest.gpu.empty() ? std::string("(not in the log)") : newest.gpu;
  if (!newest.driver.empty()) gpu += ", driver " + newest.driver;
  const std::string settings = Scrub(fact("settings changed from the defaults: ", "(not in the log)"));

  char created[64];
  std::strftime(created, sizeof(created), "%Y-%m-%d %H:%M", &local);
  std::string summary;
  summary += "Mind over Recomp problem report, made " + std::string(created) + "\n\n";
  summary += "Version:        " + version + "\n";
  summary += "System:         " + system + "\n";
  summary += "CPU / RAM:      " + fact("CPU ", "(not in the log)") + "\n";
  summary += "Graphics card:  " + gpu + "\n";
  summary += "Settings changed from the defaults: " + settings + "\n";
  summary += "Started with:   " + Scrub(fact("started with ", "(not in the log)")) + "\n";
  summary += facts_from + "\n";
  summary += "Sessions in this report (newest first):\n";
  summary += sessions_text.empty() ? "  (none)\n" : sessions_text;
  if (!save_text.empty()) {
    summary += "\nSave included:  " + save_text +
               " (in saves/: copy it into a test copy's user/saves and load it with "
               "--load_save=<its number>)\n";
  }
  summary += "\nWhat happened:\n";
  summary += request.what_happened.empty() ? "  (not described)\n" : Scrub(request.what_happened) + "\n";
  zip.Add("summary.txt", summary, now);
  result.contents.insert(result.contents.begin(), "summary.txt");
  if (!zip.Finish()) {
    *error = "Couldn't finish " + result.file.string() + " (disk full?)";
    return false;
  }
  result.bytes = fs::file_size(result.file, ec);
  result.summary = summary;

  // The bug form with the fields we know filled in (ids from bug_report.yml).
  result.issue_url = std::string(kBugForm) + "&version=" + UrlEncode(version_only) +
                     "&system=" + UrlEncode(system) + "&gpu=" + UrlEncode(gpu) +
                     "&settings=" + UrlEncode(settings);
  if (result.bytes > kGitHubLimit) {
    result.contents.push_back("(bigger than GitHub's 25 MB limit: leave the photos out, "
                              "or share it another way)");
  }
  *built = std::move(result);
  return true;
}

}  // namespace report
