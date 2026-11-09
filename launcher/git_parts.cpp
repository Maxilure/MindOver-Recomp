// =============================================================================
// git_parts.cpp -- see git_parts.h (why: `git submodule` needs Git's shell)
// =============================================================================
#include "git_parts.h"
#include "texts.h"

#include <map>
#include <sstream>

namespace fs = std::filesystem;
using task_runner::Command;

namespace git_parts {
namespace {

// Output lines without the CR that Git for Windows may add.
std::vector<std::string> Lines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) lines.push_back(line);
  }
  return lines;
}

// `git config -f .gitmodules --get-regexp '^submodule\..*\.<key>$'` ->
// name -> value. A line is "submodule.<name>.<key> <value>"; the name may
// hold dots (most do: "thirdparty/fmt" style names have none, but "a.b"
// would), so it is cut at the LAST ".<key> " before the value's space.
std::map<std::string, std::string> ModulesValues(const fs::path& repo, const std::string& key, bool* ok) {
  std::map<std::string, std::string> values;
  const std::string output = task_runner::Capture(
      {"git", "-C", repo.string(), "config", "-f", ".gitmodules", "--get-regexp",
       "^submodule\\..*\\." + key + "$"},
      ok, {}, 20000);
  const std::string marker = "." + key + " ";
  for (const std::string& line : Lines(output)) {
    if (!line.starts_with("submodule.")) continue;
    const size_t at = line.find(marker);
    if (at == std::string::npos) continue;
    values[line.substr(10, at - 10)] = line.substr(at + marker.size());
  }
  return values;
}

std::vector<std::string> Git(const std::vector<std::string>& options, std::vector<std::string> args) {
  std::vector<std::string> argv = {"git"};
  argv.insert(argv.end(), options.begin(), options.end());
  argv.insert(argv.end(), args.begin(), args.end());
  return argv;
}

// The commands for one part that isn't right yet: prepare (in-process, quick),
// fetch (the download: a child process), checkout, then its own parts.
void AddPartCommands(const Part& part, const std::vector<std::string>& options,
                     std::vector<Command>* out) {
  const std::string where = part.path.string();
  // 1. Its own repository + settings. In-process: a few quick git calls
  //    (each a plain program, no shell), not worth a line each in the log.
  Command prepare;
  prepare.title = trf(trs("Preparing {0}"), {part.name});
  prepare.function = [part, options, where](std::string* message) {
    std::error_code ec;
    fs::create_directories(part.path, ec);
    bool ok = false;
    // `git init` in a folder that already has a repository just "reinitializes"
    // it (keeps everything): fine for a part a stopped download left behind.
    const std::string init = task_runner::Capture(Git(options, {"init", "--quiet", where}), &ok, {}, 30000);
    if (!ok) {
      *message = "git init failed in " + where + ": " + init;
      return false;
    }
    // Stored in the part's own config, so every later git command there sees
    // them too: LF line endings kept (our SDK patches need them), long paths.
    for (const auto& [key, value] : {std::pair{"core.autocrlf", "false"}, std::pair{"core.longpaths", "true"}}) {
      task_runner::Capture(Git(options, {"-C", where, "config", key, value}), &ok, {}, 20000);
      if (!ok) {
        *message = std::string("git config ") + key + " failed in " + where;
        return false;
      }
    }
    // A remote named origin, as a normal submodule checkout has (only for the
    // people who look inside; the fetch below names the url itself).
    task_runner::Capture(Git(options, {"-C", where, "remote", "add", "origin", part.url}), &ok, {}, 20000);
    if (!ok) task_runner::Capture(Git(options, {"-C", where, "remote", "set-url", "origin", part.url}), &ok, {}, 20000);
    *message = part.name + ": " + part.url + " at " + part.commit.substr(0, 12);
    return true;
  };
  out->push_back(std::move(prepare));
  // 2. Only that one commit, no history (= the old --depth 1).
  out->push_back({trf(trs("Downloading {0}"), {part.name}),
                  Git(options, {"-C", where, "fetch", "--depth", "1", "--no-tags", "origin", part.commit}),
                  {}});
  // 3. Its files. --force: a part a stopped checkout left half-written, or one
  //    at another version, ends up exactly at this commit.
  Command checkout{trf(trs("Unpacking {0}"), {part.name}),
                   Git(options, {"-C", where, "-c", "advice.detachedHead=false", "checkout", "--force",
                                 "--quiet", part.commit}),
                   {}};
  // 4. Its own parts, listed now that its .gitmodules is there.
  const fs::path path = part.path;
  checkout.then = [path, options]() { return DownloadCommands(path, options); };
  out->push_back(std::move(checkout));
}

}  // namespace

std::vector<Part> PartsOf(const fs::path& repo, bool* ok) {
  *ok = true;
  std::vector<Part> parts;
  std::error_code ec;
  if (!fs::is_regular_file(repo / ".gitmodules", ec)) return parts;  // none
  // `git config --get-regexp` exits 1 when nothing matches: an EMPTY
  // .gitmodules (tomlplusplus has one) = no parts, not an error.
  bool paths_ok = false, urls_ok = false;
  const auto paths = ModulesValues(repo, "path", &paths_ok);
  if (paths.empty()) return parts;
  const auto urls = ModulesValues(repo, "url", &urls_ok);
  if (!urls_ok) {
    *ok = false;
    return parts;
  }
  // The recorded commits, all in one call: "160000 commit <id>\t<path>".
  std::vector<std::string> argv = {"git", "-C", repo.string(), "ls-tree", "HEAD", "--"};
  for (const auto& [name, path] : paths) argv.push_back(path);
  bool tree_ok = false;
  const std::string tree = task_runner::Capture(argv, &tree_ok, {}, 20000);
  if (!tree_ok) {
    *ok = false;
    return parts;
  }
  std::map<std::string, std::string> commits;  // path -> commit
  for (const std::string& line : Lines(tree)) {
    const size_t tab = line.find('\t');
    if (!line.starts_with("160000 commit ") || tab == std::string::npos) continue;
    commits[line.substr(tab + 1)] = line.substr(14, tab - 14);
  }
  for (const auto& [name, path] : paths) {
    const auto url = urls.find(name);
    const auto commit = commits.find(path);
    // A .gitmodules entry the tree doesn't record (removed but left in the
    // file) isn't a part: `git submodule` skips those too.
    if (url == urls.end() || commit == commits.end()) continue;
    parts.push_back({name, (repo / fs::path(path)).lexically_normal(), url->second, commit->second});
  }
  return parts;
}

PartState StateOf(const Part& part) {
  std::error_code ec;
  // Without its own .git (folder, or a submodule's .git file), `git -C` would
  // answer for the PARENT repository: check for it first.
  if (!fs::exists(part.path / ".git", ec)) return PartState::kMissing;
  bool ok = false;
  std::string head = task_runner::Capture({"git", "-C", part.path.string(), "rev-parse", "HEAD"}, &ok, {}, 20000);
  head = head.substr(0, head.find_first_of("\r\n"));
  if (!ok) return PartState::kMissing;  // a repository with nothing fetched yet
  return head == part.commit ? PartState::kOk : PartState::kOtherVersion;
}

std::vector<Part> AllParts(const fs::path& repo, bool* ok, bool* all_ok) {
  *ok = true;
  *all_ok = true;
  std::vector<Part> all;
  std::vector<fs::path> repos = {repo};
  for (size_t i = 0; i < repos.size(); ++i) {
    bool listed = false;
    for (Part& part : PartsOf(repos[i], &listed)) {
      if (StateOf(part) == PartState::kOk) {
        repos.push_back(part.path);  // its own parts are known only when it's there
      } else {
        *all_ok = false;
      }
      all.push_back(std::move(part));
    }
    if (!listed) {
      *ok = false;
      *all_ok = false;
    }
  }
  return all;
}

std::vector<Command> DownloadCommands(const fs::path& repo, const std::vector<std::string>& git_options,
                                      std::string* message) {
  std::vector<Command> commands;
  bool ok = false;
  const std::vector<Part> parts = PartsOf(repo, &ok);
  if (!ok) {
    // Couldn't read the list: one failing step that says so (instead of
    // quietly downloading nothing).
    Command fail;
    fail.title = trs("Listing the SDK's libraries");
    const std::string where = repo.string();
    fail.function = [where](std::string* m) {
      *m = "Couldn't read the list of libraries (.gitmodules) in " + where + ".";
      return false;
    };
    commands.push_back(std::move(fail));
    return commands;
  }
  size_t needed = 0;
  for (const Part& part : parts) {
    if (StateOf(part) == PartState::kOk) {
      // Right already: only its own parts may still be missing (a download
      // stopped inside them).
      for (Command& nested : DownloadCommands(part.path, git_options)) commands.push_back(std::move(nested));
      continue;
    }
    ++needed;
    AddPartCommands(part, git_options, &commands);
  }
  if (message) {
    *message = std::to_string(needed) + " of " + std::to_string(parts.size()) + " libraries to download in " +
               repo.string() + ".";
  }
  return commands;
}

}  // namespace git_parts
