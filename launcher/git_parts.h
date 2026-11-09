// =============================================================================
// git_parts.h -- a repository's submodules ("parts"), WITHOUT `git submodule`
// =============================================================================
//
// WHY (GitHub issue #14, 2026-11-09): `git submodule` is a SHELL SCRIPT
// (libexec/git-core/git-submodule), and so is what `git clone
// --recurse-submodules` runs for the submodules. On Windows, Git for Windows
// runs it with its own bundled sh.exe (MSYS2), which starts every helper
// program through a Cygwin-style "fork". On some PCs Windows kills each of
// those children while they start up:
//   sh 335 dofork: child -1 - forked process 1924 died unexpectedly, retry 0,
//   exit code 0xC0000142, errno 11
//   git-submodule: fork: retry: Resource temporarily unavailable
// (0xC0000142 = a DLL failed to start; the usual triggers are Exploit
// protection's Mandatory ASLR, programs that inject themselves into every
// process, low memory). The rest of Git (clone, init, fetch, checkout,
// config, ls-tree, rev-parse) is ordinary programs and works on such PCs.
//
// HOW: this file does what `git submodule update --init --recursive --depth 1`
// did, with those ordinary commands only:
//   - the list: `git config -f .gitmodules --get-regexp` (name -> path, url)
//     + `git ls-tree HEAD -- <paths>` (mode 160000 entries = the commit the
//     repository records for each part);
//   - one part: `git init <path>` (its own .git folder inside the part, not
//     under the parent's .git/modules: nothing here needs that), its config
//     (no line-ending conversion, long paths), `git fetch --depth 1 --no-tags
//     <url> <commit>` (GitHub serves a single commit by its id), `git
//     checkout --force <commit>`; then the part's OWN parts, the same way
//     (simde has one: test/munit).
//   - checks: a part is there when <path>/.git exists and `git rev-parse
//     HEAD` inside it = the recorded commit. That works for these parts and
//     for ordinary submodule checkouts (a developer's clone) alike.
// The download is a list of task_runner commands: the slow fetch of each part
// is a child process like any other step (its output in the log, Cancel stops
// it). Parts that are already right are skipped, so a stopped download goes
// on where it stopped.
// =============================================================================
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "task_runner.h"

namespace git_parts {

struct Part {
  std::string name;              // the .gitmodules name ("thirdparty/fmt")
  std::filesystem::path path;    // absolute: <repo>/<path in .gitmodules>
  std::string url;               // where to fetch it from
  std::string commit;            // the commit the repository records for it
};

enum class PartState { kOk, kMissing, kOtherVersion };

// The parts `repo` records itself (not nested ones). A repository without a
// .gitmodules file has none (ok). *ok = false when git couldn't list them.
std::vector<Part> PartsOf(const std::filesystem::path& repo, bool* ok);

// Is `part` checked out at its recorded commit?
PartState StateOf(const Part& part);

// Every part of `repo`, nested ones included (looked for inside each part
// that is checked out). *all_ok = every one of them is kOk.
std::vector<Part> AllParts(const std::filesystem::path& repo, bool* ok, bool* all_ok);

// The commands that bring every part of `repo` (nested ones too: they are
// listed as each part arrives, through Command::then) to its recorded commit.
// `git_options` go before every git command ("-c", "core.longpaths=true"...).
// *message (optional) = "N of M parts to download".
std::vector<task_runner::Command> DownloadCommands(const std::filesystem::path& repo,
                                                   const std::vector<std::string>& git_options,
                                                   std::string* message = nullptr);

}  // namespace git_parts
