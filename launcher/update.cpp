// =============================================================================
// update.cpp -- see update.h
// =============================================================================
#include "update.h"

#include "sha256.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using task_runner::Command;

namespace update {
namespace {

// `key = "value"` lines (release.toml / RELEASE.toml / or the plain VERSION.txt).
std::string Value(const std::string& text, const std::string& key) {
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    const size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos || line.compare(start, key.size(), key) != 0) {
      continue;
    }
    const size_t eq = line.find('=', start + key.size());
    if (eq == std::string::npos || line.find_first_not_of(" \t", start + key.size()) != eq) {
      continue;  // a longer key that starts the same
    }
    const size_t open = line.find('"', eq);
    const size_t close = open == std::string::npos ? open : line.find('"', open + 1);
    if (close != std::string::npos) {
      return line.substr(open + 1, close - open - 1);
    }
  }
  return {};
}

std::string ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), {});
}

// "0.1.0-alpha" -> {0,1,0} and "alpha".
void Split(const std::string& version, int numbers[3], std::string* pre) {
  numbers[0] = numbers[1] = numbers[2] = 0;
  const size_t dash = version.find('-');
  *pre = dash == std::string::npos ? "" : version.substr(dash + 1);
  std::sscanf(version.substr(0, dash).c_str(), "%d.%d.%d", &numbers[0], &numbers[1], &numbers[2]);
}

// Rank of a pre-release name: alpha < beta < rc < (none = the release).
int PreRank(const std::string& pre) {
  if (pre.empty()) return 100;
  if (pre.starts_with("rc")) return 3;
  if (pre.starts_with("beta")) return 2;
  if (pre.starts_with("alpha")) return 1;
  return 0;
}

// Same size and bytes?
bool SameContent(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  if (fs::file_size(a, ec) != fs::file_size(b, ec) || ec) {
    return false;
  }
  return ReadFile(a) == ReadFile(b);
}

// Moves `from` to `to` if it exists (a folder of build state), replacing
// whatever `to` has.
bool MoveIfThere(const fs::path& from, const fs::path& to, std::string* message) {
  std::error_code ec;
  if (!fs::exists(from, ec)) {
    return true;
  }
  fs::remove_all(to, ec);
  fs::create_directories(to.parent_path(), ec);
  fs::rename(from, to, ec);
  if (ec) {
    *message += "couldn't move " + from.string() + ": " + ec.message() + "\n";
    return false;
  }
  return true;
}

}  // namespace

bool Newer(const std::string& a, const std::string& b) {
  int na[3], nb[3];
  std::string pa, pb;
  Split(a, na, &pa);
  Split(b, nb, &pb);
  for (int i = 0; i < 3; ++i) {
    if (na[i] != nb[i]) {
      return na[i] > nb[i];
    }
  }
  if (PreRank(pa) != PreRank(pb)) {
    return PreRank(pa) > PreRank(pb);
  }
  return pa > pb;  // "alpha.2" after "alpha.1"
}

std::string InstalledVersion(const folders::Folders& folders) {
  if (folders.source.empty()) {
    return {};
  }
  std::string version = Value(ReadFile(folders.source / "RELEASE.toml"), "version");
  if (version.empty()) {
    version = ReadFile(folders.source / "VERSION.txt");
    version.erase(std::remove_if(version.begin(), version.end(), ::isspace), version.end());
  }
  return version;
}

Updater::Updater(folders::Folders folders, std::string feed)
    : folders_(std::move(folders)),
      feed_(feed.empty() ? kDefaultFeed : std::move(feed)),
      installed_(InstalledVersion(folders_)) {}

Updater::~Updater() {
  if (thread_.joinable()) {
    thread_.join();
  }
}

void Updater::Check() {
  if (checking_.exchange(true)) {
    return;
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  thread_ = std::thread([this] {
    bool ok = false;
    const std::string text = task_runner::Capture(
        {"curl", "-fsSL", "--max-time", "20", feed_}, &ok, {}, 25000);
    Release release;
    // One archive per system: "<system>_archive" / "<system>_sha256"
    // (tools/make_release.sh); plain "archive" / "sha256" = Linux, the
    // first format.
#if defined(_WIN32)
    const std::string system = "windows";
#else
    const std::string system = "linux";
#endif
    release.version = Value(text, "version");
    release.archive = Value(text, system + "_archive");
    release.sha256 = Value(text, system + "_sha256");
#if !defined(_WIN32)
    if (release.archive.empty()) {
      release.archive = Value(text, "archive");
      release.sha256 = Value(text, "sha256");
    }
#endif
    release.date = Value(text, "date");
    if (release.archive.find("://") != std::string::npos) {
      release.archive_url = release.archive;
    } else {
      release.archive_url = feed_.substr(0, feed_.rfind('/') + 1) + release.archive;
    }
    std::lock_guard lock(mutex_);
    if (!ok || release.version.empty() || release.archive.empty()) {
      have_ = false;
      error_ = !ok ? "Couldn't reach the update server (offline?)."
            : release.version.empty() ? "The update information couldn't be read."
                                      : "No download for this system in the newest release yet.";
    } else {
      have_ = true;
      error_.clear();
      latest_ = release;
    }
    checking_ = false;
  });
}

bool Updater::Latest(Release* out, bool* available, std::string* error) {
  std::lock_guard lock(mutex_);
  *error = error_;
  if (!have_) {
    return false;
  }
  *out = latest_;
  *available = Newer(latest_.version, installed_);
  return true;
}

std::vector<Command> Updater::Job(const Release& release, const fs::path& launcher) {
  const fs::path root = folders_.root;
  const fs::path work = root / ".update";
  const fs::path archive = work / release.archive;
  std::vector<Command> commands;

  Command prepare;
  prepare.title = "Preparing";
  prepare.function = [work](std::string* message) {
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work / "new", ec);
    *message = ec ? "couldn't make " + work.string() + ": " + ec.message() : "";
    return !ec;
  };
  commands.push_back(std::move(prepare));

  commands.push_back({"Downloading " + release.version,
                      {"curl", "-fL", "--retry", "2", "-o", archive.string(), release.archive_url},
                      root});

  Command verify;
  verify.title = "Checking the download";
  verify.function = [archive, sha = release.sha256](std::string* message) {
    const std::string got = sha256::OfFile(archive);
    if (got.empty() || (!sha.empty() && got != sha)) {
      *message = "The download is damaged (SHA-256 " + got + ", expected " + sha + ").";
      return false;
    }
    *message = "SHA-256 matches.";
    return true;
  };
  commands.push_back(std::move(verify));

  // tar unpacks both formats: .tar.gz (Linux) and .zip (Windows' own tar.exe
  // is bsdtar, which reads zip files too).
  commands.push_back({"Unpacking", {"tar", "-xf", archive.string(), "-C", (work / "new").string()},
                      root});

  Command swap;
  swap.title = "Swapping in the new version";
  swap.function = [root, work, launcher](std::string* message) {
    // The unpacked release's top folder (whatever it's called).
    fs::path fresh;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(work / "new", ec)) {
      if (entry.is_directory(ec) && fs::exists(entry.path() / "source" / "RELEASE.toml", ec)) {
        fresh = entry.path();
      }
    }
    if (fresh.empty()) {
      *message = "The download isn't a release of this port (no source/RELEASE.toml).";
      return false;
    }
    const fs::path new_src = fresh / "source";
    const fs::path old_src = root / "source";

    // Dates: unchanged files keep the old date, changed / new ones get now.
    const auto now = fs::file_time_type::clock::now();
    int changed = 0, same = 0;
    for (auto it = fs::recursive_directory_iterator(new_src, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (!it->is_regular_file(ec)) {
        continue;
      }
      const fs::path relative = fs::relative(it->path(), new_src, ec);
      const fs::path old = old_src / relative;
      std::error_code old_ec;
      if (fs::is_regular_file(old, old_ec) && SameContent(it->path(), old)) {
        fs::last_write_time(it->path(), fs::last_write_time(old, old_ec), old_ec);
        ++same;
      } else {
        fs::last_write_time(it->path(), now, old_ec);
        ++changed;
      }
    }
    *message = std::to_string(changed) + " files changed or new, " + std::to_string(same) +
               " unchanged.\n";

    // The build state moves into the new source tree.
    bool ok = true;
    for (const char* part : {"out", "generated/default", "thirdparty/rexglue-sdk",
                             "thirdparty/rexglue-install"}) {
      ok = MoveIfThere(old_src / part, new_src / part, message) && ok;
    }
    if (!ok) {
      return false;  // nothing swapped yet; the old source still has the rest
    }
    // Old out, new in.
    const fs::path retired = work / "old-source";
    fs::rename(old_src, retired, ec);
    if (ec) {
      *message += "couldn't move the old source/ away: " + ec.message();
      return false;
    }
    fs::rename(new_src, old_src, ec);
    if (ec) {
      fs::rename(retired, old_src, ec);  // put the old one back
      *message += "couldn't put the new source/ in place";
      return false;
    }
    // The launcher (replacing the running file is fine: rename) and the readme.
    for (const auto& entry : fs::directory_iterator(fresh, ec)) {
      if (!entry.is_regular_file(ec)) {
        continue;
      }
      const std::string name = entry.path().filename().string();
      const bool is_launcher = name != "READ ME FIRST.txt";
      const fs::path target =
          is_launcher && !launcher.empty() && launcher.parent_path() == root ? launcher
                                                                             : root / name;
      std::error_code move_ec;
#if defined(_WIN32)
      // Windows can't replace a running program's file, but it can RENAME
      // it: the running launcher moves aside to <name>.old (deleted at the
      // next start, main.cpp), then the new one takes its name.
      if (is_launcher && fs::exists(target, move_ec)) {
        fs::path aside = target;
        aside += ".old";
        fs::remove(aside, move_ec);
        fs::rename(target, aside, move_ec);
      }
#endif
      fs::rename(entry.path(), target, move_ec);
      if (move_ec) {
        *message += "couldn't replace " + target.string() + ": " + move_ec.message() + "\n";
      }
    }
    fs::remove_all(work, ec);
    *message += "Done: the new version is in place.";
    return true;
  };
  commands.push_back(std::move(swap));
  return commands;
}

}  // namespace update
