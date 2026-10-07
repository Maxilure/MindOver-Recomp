// =============================================================================
// report.h -- "Report a problem": one .zip a tester attaches to a bug report
// =============================================================================
//
// WHY: a bug report is only as good as what comes with it. Asking a tester
// for "the log of that session, your settings, your system, your graphics
// card" in a GitHub form is four chances to get it wrong. The launcher knows
// where all of it is, so it packs it: one click, one file.
//
// WHAT GOES IN (report-<date>_<time>.zip in user/reports/):
//
//   summary.txt         version, system, CPU / RAM, graphics card + driver,
//                       settings changed from the defaults (all read from the
//                       newest chosen log's "Session:" header, src/session_log.h),
//                       each chosen session (when, how long, how it ended,
//                       error / warning counts) and the tester's own words
//   logs/play-*.log     the chosen sessions' logs (newest first in the list;
//                       the newest is picked by default). A log over 8 MB (a
//                       stuck fault repeating one line) keeps its first 2 MB
//                       and last 6 MB, with a note where the middle was cut
//   logs/crash-*.txt    Windows crash reports written during those sessions
//   logs/last-launch-output.txt   the game's terminal output of the LAST launch
//                       (only when the newest session is chosen)
//   logs/setup-*.log    the newest Setup log (optional: build problems)
//   settings/           settings.toml + controls.toml (optional)
//   photos/             F10 photos taken during the chosen sessions (optional,
//                       off by default: ~3 MB each)
//   saves/...           ONE save the tester picks (optional, off by default):
//                       its file + its header, in the same folders as under
//                       user/saves/, so it can be dropped into a copy and
//                       loaded with --load_save to see the problem spot
//
// WHERE: user/reports/ unless the tester picked another folder in the
// window; that choice is remembered in user/launcher.toml
// (report_folder = "..."; ReportFolder / RememberReportFolder).
//
// PRIVACY: nothing is sent anywhere; the tester attaches the file themselves.
// Every text file has the home folder's path replaced by "~" (it holds the
// account name: /home/<name>, C:\Users\<name>). A save goes in only when
// picked (it holds the save's name and the game's progress, nothing else).
//
// THE BUG FORM: IssueUrl() opens GitHub's "Bug report" form with the version,
// system, graphics card and changed settings already filled in (an issue
// form takes its fields' values from the URL: ?template=bug_report.yml&<field
// id>=<value>, ids from .github/ISSUE_TEMPLATE/bug_report.yml). The zip is
// then dragged into its "Log" box.
//
// The .zip is written by a tiny writer of our own: "stored" entries (no
// compression), CRC-32, one central directory. Every unzip program and both
// desktops' file managers open it; GitHub accepts it as an attachment (up to
// 25 MB: the report says so when it's bigger, usually because of photos).
// =============================================================================
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "folders.h"

namespace report {

// One play session, from its log (user/logs/play-<date>_<time>.log).
struct Session {
  std::filesystem::path log;
  std::string when;       // "Oct 7, 16:20" (the log's first line)
  std::string length;     // "12 min" (first line -> the file's last change)
  uint64_t bytes = 0;
  int photos = 0;         // F10 photos taken during it
  uint64_t photo_bytes = 0;
  std::string ended;      // the launcher's "Launcher: the game ..." line, if any
};

// The newest `count` sessions, newest first.
std::vector<Session> RecentSessions(const folders::Folders& folders, int count = 10);

struct Request {
  std::string what_happened;                 // the tester's words (may be empty)
  std::vector<std::filesystem::path> logs;   // the chosen sessions' logs
  bool settings = true;                      // settings.toml + controls.toml
  bool setup_log = false;                    // the newest setup-*.log
  bool photos = false;                       // F10 photos of the chosen sessions
  std::filesystem::path save_file;           // one save to include (empty = none; saves.h)
  std::string save_label;                    // its name, for the summary ("12 Desert (21%)")
  std::filesystem::path folder;              // where the .zip goes (empty = user/reports)
  std::string version;                       // the launcher's (update::InstalledVersion)
};

struct Built {
  std::filesystem::path file;                // the .zip
  uint64_t bytes = 0;
  std::string summary;                       // = summary.txt (also for the clipboard)
  std::string issue_url;                     // GitHub's bug form, fields filled in
  std::vector<std::string> contents;         // the files inside, for the window
};

// Writes the .zip. False + `error` if nothing could be written.
bool Build(const folders::Folders& folders, const Request& request, Built* built,
           std::string* error);

// The folder reports go to: the remembered choice if it still exists, else
// user/reports.
std::filesystem::path ReportFolder(const folders::Folders& folders);
// Remembers `folder` for the next reports (empty = back to user/reports).
void RememberReportFolder(const folders::Folders& folders, const std::filesystem::path& folder);

// The newest setup-*.log (empty if none).
std::filesystem::path NewestSetupLog(const folders::Folders& folders);

}  // namespace report
