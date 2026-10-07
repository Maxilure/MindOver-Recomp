// =============================================================================
// update.h -- new versions: finding them, downloading, swapping them in
// =============================================================================
//
// Only for the RELEASE layout (folders.h): a developer clone updates with
// `git pull` (the tab says so).
//
// THE FEED: every GitHub release of the project carries a small
// release.toml (tools/make_release.sh) next to its archive:
//   version = "0.1.1-alpha"
//   archive = "MindOverRecomp-0.1.1-alpha-linux-x86_64.tar.gz"
//   sha256 = "..."
// The launcher reads the NEWEST one through GitHub's fixed link
//   https://github.com/Maxilure/MindOver-Recomp/releases/latest/download/release.toml
// (once at start, quietly; "Check for updates" again on request) and the
// archive from the same folder. `--update_feed=<url>` points elsewhere,
// e.g. file:///.../out/release/0.1.1-alpha/release.toml for tests. All
// downloads go through `curl` (on every desktop Linux).
//
// VERSIONS: "MAJOR.MINOR.PATCH[-pre]"; a version with "-pre" comes before
// the same one without it, and alpha < beta < rc (Newer()).
//
// APPLYING AN UPDATE (a job on the Setup tab's runner, task_runner.h):
//   1. download the archive into <game folder>/.update/, check its SHA-256
//   2. unpack it there
//   3. SWAP (in-process):
//      - every file of the new source/ that's byte-identical to the old one
//        gets the old one's date, every changed or new one NOW: the build
//        (Ninja compares dates) then redoes exactly what changed, no more;
//      - the build state moves from the old source/ into the new one: out/
//        (builds), generated/default/ (the recompiled code),
//        thirdparty/rexglue-sdk/ (the SDK download; Setup re-downloads it if
//        the new version names another SDK) and thirdparty/rexglue-install/;
//      - old source/ out, new source/ in (two renames);
//      - the launcher and READ ME FIRST.txt replaced (the running launcher
//        keeps running: a rename doesn't touch the open file);
//      - <game folder>/.update/ removed.
//   user/, game/ and program/ are never touched (program/ is replaced by
//   Setup's next game build).
//   4. "Restart the launcher": starts the new launcher (execv) with
//   --after_update, which opens Setup and runs "Set up everything": the SDK
//   and the game rebuild what the update changed.
// =============================================================================
#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "folders.h"
#include "task_runner.h"

namespace update {

constexpr const char* kDefaultFeed =
    "https://github.com/Maxilure/MindOver-Recomp/releases/latest/download/release.toml";
// A release's notes at its tag (+ "v<version>/tools/release/RELEASE_NOTES.md")
// and its page (+ "v<version>"): update.h's WHAT'S NEW.
constexpr const char* kNotesAtTag = "https://raw.githubusercontent.com/Maxilure/MindOver-Recomp/";
constexpr const char* kReleasePages = "https://github.com/Maxilure/MindOver-Recomp/releases/tag/";

struct Release {
  std::string version, archive, sha256, date;
  std::string archive_url;  // archive resolved against the feed's folder
  std::string notes;        // "What's new" of that version, plain text ("" = couldn't get it)
};

// WHAT'S NEW: every release's notes are tools/release/RELEASE_NOTES.md at its
// tag; its "### What's new in <version>" section is what the launcher shows:
// before updating (fetched from GitHub at that version's tag, or from the
// feed's `notes_url = "..."` when given: tests with a file:// feed) and once
// after an update's restart (the installed source's own copy).
// The section as plain text (**bold**, `code` and [links](...) unwrapped);
// "" if the notes have no section for `version`.
std::string WhatsNew(const std::string& markdown, const std::string& version);
// The installed version's notes (source/tools/release/RELEASE_NOTES.md).
std::string InstalledWhatsNew(const folders::Folders& folders);
// The release's page on GitHub.
std::string ReleasePage(const std::string& version);

// a newer than b? (see the header)
bool Newer(const std::string& a, const std::string& b);

// This copy's version: source/RELEASE.toml (release) or VERSION.txt (developer).
std::string InstalledVersion(const folders::Folders& folders);

class Updater {
 public:
  Updater(folders::Folders folders, std::string feed);
  ~Updater();

  // Reads the feed on a thread of its own (quiet when offline).
  void Check();
  bool checking() const { return checking_; }

  // The newest release, once a check succeeded; `available` = newer than ours.
  bool Latest(Release* out, bool* available, std::string* error);

  // The commands that download and swap in `release` (see the header);
  // `launcher` = the running launcher's file, replaced by the new one.
  std::vector<task_runner::Command> Job(const Release& release,
                                        const std::filesystem::path& launcher);

  const std::string& installed() const { return installed_; }
  const std::string& feed() const { return feed_; }

 private:
  folders::Folders folders_;
  std::string feed_, installed_;
  std::thread thread_;
  std::atomic<bool> checking_{false};
  std::mutex mutex_;
  bool have_ = false;
  Release latest_;
  std::string error_;
};

}  // namespace update
