// =============================================================================
// setup.cpp -- see setup.h
// =============================================================================
#include "setup.h"
#include "git_parts.h"
#include "texts.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <regex>
#include <sstream>


#include <SDL3/SDL.h>

namespace fs = std::filesystem;
using task_runner::Command;
using task_runner::Progress;

namespace setup {
namespace {

// -----------------------------------------------------------------------------
// Distros and packages (step 1)
// -----------------------------------------------------------------------------

#if !defined(_WIN32)
enum Family { kArch, kDebian, kFedora, kSuse, kUnknown };

struct Distro {
  Family family = kUnknown;
  std::string name;
};

// /etc/os-release: ID and ID_LIKE pick the family, NAME / PRETTY_NAME the name.
Distro DetectDistro() {
  Distro distro;
  std::ifstream in("/etc/os-release");
  std::string line, id, like;
  auto value = [](const std::string& text) {
    std::string v = text.substr(text.find('=') + 1);
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'')) {
      v = v.substr(1, v.size() - 2);
    }
    return v;
  };
  while (std::getline(in, line)) {
    if (line.starts_with("ID=")) id = value(line);
    if (line.starts_with("ID_LIKE=")) like = value(line);
    if (line.starts_with("NAME=") && distro.name.empty()) distro.name = value(line);
  }
  const std::string both = " " + id + " " + like + " ";
  auto has = [&](const char* word) { return both.find(std::string(" ") + word + " ") != std::string::npos; };
  if (has("arch")) distro.family = kArch;
  else if (has("debian") || has("ubuntu")) distro.family = kDebian;
  else if (has("fedora") || has("rhel")) distro.family = kFedora;
  else if (has("suse") || has("opensuse") || id.starts_with("opensuse")) distro.family = kSuse;
  if (distro.name.empty()) distro.name = id.empty() ? "this Linux" : id;
  return distro;
}
#endif  // !_WIN32


// The first "N.N" in a tool's --version output, as its major number.
int MajorVersion(const std::string& text, std::string* full) {
  std::smatch match;
  static const std::regex number(R"((\d+)\.(\d+)(\.\d+)?)");
  if (!std::regex_search(text, match, number)) {
    return -1;
  }
  *full = match.str(0);
  return std::stoi(match.str(1));
}
int MinorVersion(const std::string& full) {
  const size_t dot = full.find('.');
  return dot == std::string::npos ? 0 : std::atoi(full.c_str() + dot + 1);
}

// -----------------------------------------------------------------------------
// Small helpers
// -----------------------------------------------------------------------------

fs::file_time_type TimeOf(const fs::path& path) {
  std::error_code ec;
  const auto time = fs::last_write_time(path, ec);
  return ec ? fs::file_time_type::min() : time;
}

// The newest file time under `folder` (recursively).
fs::file_time_type NewestUnder(const fs::path& folder) {
  fs::file_time_type newest = fs::file_time_type::min();
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(folder, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it->is_regular_file(ec)) {
      newest = std::max(newest, it->last_write_time(ec));
    }
  }
  return newest;
}

bool Exists(const fs::path& path) {
  std::error_code ec;
  return fs::exists(path, ec);
}

// The installer's "use fewer resources" choice (setup.h SetLimitedResources).
std::atomic<bool> g_limited{false};

// Parallel compile jobs from the RAM that's FREE now (not the total: a
// desktop with apps open may have a third of it; going over makes Linux's
// out-of-memory killer end the whole desktop session, seen 2026-10-06; on
// Windows it means swapping, which made some builds take hours). The game's
// generated files need 1-2 GB each (docs/01-building.md, step 4): 2.5 GB per
// job; the SDK's ~1 GB.
//   full     one core left free for the desktop (never the whole PC);
//   limited  half the cores, and the RAM counted with room to spare
//            (the build also runs at low priority: UseBuildEnvironment).
int JobsFor(double gb_per_job) {
  const bool limited = g_limited;
  const int cores = platform::Cores();
  const int max_jobs = limited ? std::max(1, cores / 2) : std::max(1, cores - 1);
  const int by_ram = int(platform::MemAvailableGb() / (limited ? gb_per_job * 1.5 : gb_per_job));
  return std::clamp(by_ram, 1, max_jobs);
}
int GameJobs() { return JobsFor(2.5); }
int SdkJobs() { return JobsFor(1.0); }

// What differs per system in the build (CMakePresets.json, the SDK's own
// presets, file names).
#if defined(_WIN32)
constexpr const char* kSdkPreset = "win-amd64";
constexpr const char* kGamePreset = "win-amd64-relwithdebinfo";
constexpr const char* kLibFolder = "bin";      // where the SDK installs its libraries
constexpr const char* kLibSuffix = ".dll";
// The SDK defaults to Direct3D 12 on Windows; our native renderer is Vulkan,
// and the emulated one should match Linux: Vulkan on, D3D12 off.
// Tracy OFF (2026-11-09, GitHub issue #10): the SDK builds the Tracy profiler
// as its own DLL (TracyClient.dll / TracyClientrd.dll). Players never use it
// (our profiling is the game's own console `profile`), and an antivirus
// quarantined the freshly built DLL on a player's PC: the SDK's install step
// then failed with "file INSTALL cannot find ... TracyClient.dll". Off = no
// such DLL is built at all; the game's CMake only stages rex::TracyClient
// when the SDK has it. Checked with a full SDK + game build on Windows.
const std::vector<std::string> kSdkOptions = {"-DREXGLUE_USE_VULKAN=ON", "-DREXGLUE_USE_D3D12=OFF",
                                              "-DREXGLUE_ENABLE_TRACY=OFF"};
#else
constexpr const char* kSdkPreset = "linux-amd64";
constexpr const char* kGamePreset = "linux-amd64-relwithdebinfo";
constexpr const char* kLibFolder = "lib";
constexpr const char* kLibSuffix = ".so";
const std::vector<std::string> kSdkOptions = {};
#endif

// =============================================================================
// THE BUILD KIT (2026-11-10, GitHub issues #10 #11 #14 #16 #17 #18 #20)
// =============================================================================
// Every one of those reports failed while building the SDK, each for a
// different reason that came from the PLAYER'S PC, not from our code: an
// antivirus deleting a DLL mid-build (#10), a CMake/Ninja found by us but
// not by the build (#11), Git's shell unable to start (#14), the Microsoft
// Store's python3 shortcut (#16), devkitPro's MSYS CMake first on PATH
// ("Could not use disabled preset": it calls the system "MSYS", #17), and a
// configure that inherited a broken cache from an earlier try (every SDL
// feature check "failed": no SSE, no XInput, #20). Fixing them one by one
// only waited for the next PC with something odd installed.
//
// Installers in general don't build from source with whatever the user has.
// They ship what they need, pinned, and run it in a known environment. So a
// Windows release now ships (inside source/, see tools/make_release.sh and
// the release workflow's windows-sdk job):
//   thirdparty/rexglue-install/   the ReXGlue SDK, ALREADY BUILT and patched
//                                 by GitHub Actions (BSD-3 code, no game
//                                 content: fine to ship), + the 3 header
//                                 folders the game reads from SDK sources
//   buildtools/cmake/  ninja/  glslc/  python/
//                                 pinned CMake, Ninja, glslc (shaders) and
//                                 Python's embeddable package (disc tools)
// What's left for the player's PC: LLVM's clang and Visual Studio's C++
// tools + Windows SDK (the compiler and Windows' own headers/libraries:
// Microsoft's licence doesn't allow shipping those). And every build step
// runs with a CLEAN PATH (BuildEnvironment): our tools, LLVM, Visual Studio,
// Windows' own folders, nothing else, so nothing else on the PC can be
// picked instead. The game's configure is always --fresh (no state of an
// earlier failed try is reused).
//
// A developer clone (no buildtools/kit.txt) works as before: SDK from the
// submodule, tools from PATH.
struct Kit {
  bool present = false;           // buildtools/kit.txt is there
  fs::path cmake, ninja, glslc, python;
  fs::path sdk_marker;            // the shipped SDK's own note (prebuilt.txt)
};

Kit KitIn(const fs::path& source) {
  Kit kit;
  const fs::path tools = source / "buildtools";
  kit.present = Exists(tools / "kit.txt");
  kit.cmake = tools / "cmake" / "bin" / platform::ExeName("cmake");
  kit.ninja = tools / "ninja" / platform::ExeName("ninja");
  kit.glslc = tools / "glslc" / platform::ExeName("glslc");
  kit.python = tools / "python" / platform::ExeName("python");
  kit.sdk_marker = source / "thirdparty" / "rexglue-install" / "share" / "mindoverrecomp" / "prebuilt.txt";
  return kit;
}

// Every file of the kit, for the check (a missing one = the release folder
// was damaged: an antivirus, a half-copied folder).
std::vector<fs::path> KitFiles(const Kit& kit) {
  return {kit.cmake, kit.ninja, kit.glslc, kit.python, kit.sdk_marker};
}

// Git options for the SDK download: no line-ending conversion (Git for
// Windows would turn LF into CR LF, and our patches no longer fit), long
// paths allowed (some of the SDK's libraries nest deep). Harmless on Linux.
const std::vector<std::string> kGitOptions = {"-c", "core.autocrlf=false", "-c",
                                              "core.longpaths=true"};

// Git for Windows checks SYMBOLIC LINKS out as small text files holding the
// target's path (core.symlinks is off: real links need admin rights or
// developer mode). One of the SDK's libraries has such links to C sources
// (libmspack: cabextract/mspack/lzxd.c = "../../libmspack/mspack/lzxd.c"), and
// the compiler then reads that path as code. This step replaces every file
// link, in the SDK and all its libraries, with a copy of its target (found
// on the first Windows build, 2026-10-06). Linux: nothing to do.
void AddLinkFix(const fs::path& sdk, std::vector<task_runner::Command>* commands) {
#if defined(_WIN32)
  task_runner::Command fix;
  fix.title = trs("Replacing link files with copies (Git for Windows)");
  fix.function = [sdk](std::string* message) {
    bool ok = false;
    // The SDK + every library checked out in it (git_parts: not `git
    // submodule foreach`, a shell script that fails on some PCs, issue #14).
    std::vector<fs::path> repos = {sdk};
    bool all_ok = false;
    for (const git_parts::Part& part : git_parts::AllParts(sdk, &ok, &all_ok)) {
      if (git_parts::StateOf(part) == git_parts::PartState::kOk) repos.push_back(part.path);
    }
    std::string line;
    int fixed = 0;
    for (const fs::path& repo : repos) {
      const std::string files =
          platform::Capture({"git", "-C", repo.string(), "ls-files", "-s"}, &ok, {}, 60000);
      std::istringstream file_lines(files);
      while (std::getline(file_lines, line)) {
        if (!line.starts_with("120000 ")) continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string relative = line.substr(tab + 1);
        if (!relative.empty() && relative.back() == '\r') relative.pop_back();
        const fs::path file = repo / fs::path(relative);
        std::ifstream in(file);
        std::string target;
        std::getline(in, target);
        in.close();
        std::error_code ec;
        const fs::path source = (file.parent_path() / fs::path(target)).lexically_normal();
        if (!target.empty() && fs::is_regular_file(source, ec)) {
          fs::copy_file(source, file, fs::copy_options::overwrite_existing, ec);
          if (!ec) ++fixed;
        }
      }
    }
    *message = std::to_string(fixed) + " link files replaced in " + std::to_string(repos.size()) +
               " repositories.";
    return true;
  };
  commands->push_back(std::move(fix));
#else
  (void)sdk;
  (void)commands;
#endif
}

// The SDK's runtime library (its date decides "out of date" checks).
fs::path RuntimeLibrary(const fs::path& install) {
#if defined(_WIN32)
  return install / "bin" / "rexruntimerd.dll";
#else
  return install / "lib" / "librexruntimerd.so";
#endif
}

// A release's source/RELEASE.toml (written by tools/make_release.sh): its
// version and the exact ReXGlue SDK it builds with (no git history to ask).
//   version = "0.1.0-alpha"
//   sdk_url = "https://github.com/rexglue/rexglue-sdk.git"
//   sdk_tag = "nightly-20260921-923c1a59"
//   sdk_commit = "923c1a59..."
struct Pin {
  std::string version, url, tag, commit;
};
Pin ReadPin(const fs::path& source) {
  Pin pin;
  std::ifstream in(source / "RELEASE.toml");
  std::string line;
  auto value = [](const std::string& text) {
    const size_t open = text.find('"');
    const size_t close = open == std::string::npos ? open : text.find('"', open + 1);
    return close == std::string::npos ? std::string() : text.substr(open + 1, close - open - 1);
  };
  while (std::getline(in, line)) {
    if (line.starts_with("version")) pin.version = value(line);
    if (line.starts_with("sdk_url")) pin.url = value(line);
    if (line.starts_with("sdk_tag")) pin.tag = value(line);
    if (line.starts_with("sdk_commit")) pin.commit = value(line);
  }
  return pin;
}

std::vector<fs::path> Patches(const fs::path& root) {
  std::vector<fs::path> list;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(root / "patches" / "rexglue-sdk", ec)) {
    if (entry.is_regular_file(ec) && entry.path().extension() == ".patch") {
      list.push_back(entry.path());
    }
  }
  std::sort(list.begin(), list.end());
  return list;
}

// The files the patches change ("+++ b/<path>" lines), and which of them the
// patches create (their "--- /dev/null"): those aren't in the clean SDK.
void TouchedFiles(const std::vector<fs::path>& patches, std::vector<std::string>* changed,
                  std::vector<std::string>* created) {
  for (const fs::path& patch : patches) {
    std::ifstream in(patch);
    std::string line, previous;
    while (std::getline(in, line)) {
      if (line.starts_with("+++ b/")) {
        const std::string path = line.substr(6);
        auto& list = previous == "--- /dev/null" ? *created : *changed;
        if (std::find(changed->begin(), changed->end(), path) == changed->end() &&
            std::find(created->begin(), created->end(), path) == created->end()) {
          list.push_back(path);
        }
      }
      previous = line;
    }
  }
}

// Where the SDK stands against the patch STACK (see RunChecks): rebuilds
// "clean HEAD + every patch" for the files the patches touch in `scratch`
// (a real `git apply`: a stack applies fine in one go, while
// `git apply --check` of the set tests each patch against the ORIGINAL files
// and fails as soon as two patches touch the same lines, e.g. 0013 on 0009),
// then compares the SDK's files with it and with HEAD.
enum class PatchState { kNone, kAll, kMixed };
PatchState CheckPatchStack(const fs::path& sdk, const std::vector<fs::path>& patches,
                           const fs::path& scratch) {
  std::vector<std::string> changed, created;
  TouchedFiles(patches, &changed, &created);
  std::error_code ec;
  fs::remove_all(scratch, ec);
  fs::create_directories(scratch / "head", ec);
  fs::create_directories(scratch / "patched", ec);
  bool ok = false;
  // No line-ending conversion (kGitOptions): Git for Windows' default would
  // export CR LF files, which never match the SDK's own LF ones.
  std::vector<std::string> archive = {"git", "-c", "core.autocrlf=false", "archive", "-o",
                                      (scratch / "head.tar").string(), "HEAD", "--"};
  archive.insert(archive.end(), changed.begin(), changed.end());
  task_runner::Capture(archive, &ok, sdk);
  bool extracted = false;
  if (ok) {
    task_runner::Capture({"tar", "-xf", "../head.tar"}, &extracted, scratch / "head");
    task_runner::Capture({"tar", "-xf", "../head.tar"}, &ok, scratch / "patched");
    extracted = extracted && ok;
  }
  bool patched = false;
  if (extracted) {
    std::vector<std::string> apply = {"git", "-c", "core.autocrlf=false", "apply"};
    for (const fs::path& patch : patches) {
      apply.push_back(patch.string());
    }
    task_runner::Capture(apply, &patched, scratch / "patched");
  }
  auto read = [](const fs::path& file, bool* exists) {
    std::ifstream in(file, std::ios::binary);
    *exists = bool(in);
    return std::string((std::istreambuf_iterator<char>(in)), {});
  };
  bool equals_patched = patched, equals_head = extracted;
  if (extracted) {
    std::vector<std::string> all = changed;
    all.insert(all.end(), created.begin(), created.end());
    for (size_t i = 0; i < all.size(); ++i) {
      const bool is_new = i >= changed.size();  // made by a patch: not in HEAD
      bool have_exists = false, want_exists = false, head_exists = false;
      const std::string have = read(sdk / all[i], &have_exists);
      const std::string want = read(scratch / "patched" / all[i], &want_exists);
      const std::string head = is_new ? std::string() : read(scratch / "head" / all[i], &head_exists);
      if (!have_exists || !want_exists || have != want) {
        equals_patched = false;
      }
      if (is_new ? have_exists : (!head_exists || have != head)) {
        equals_head = false;
      }
    }
  }
  fs::remove_all(scratch, ec);
  if (equals_patched) return PatchState::kAll;
  if (equals_head && patched) return PatchState::kNone;
  return PatchState::kMixed;
}

// AN UPDATE'S NEW PATCH SET, touching only what changes (2026-11-08). The
// old way, `git checkout -- .` + `git clean -fd` + `git apply`, rewrote EVERY
// patched file, so Ninja saw all of them (and every file including the
// patched headers) as changed: one new patch = most of the SDK and the game
// compiled again. Now:
//   1. what "clean HEAD + the new patches" looks like is built in `scratch`
//      for every file the new patches touch AND every tracked file the old
//      ones had changed (`git diff --name-only`, submodules left out; link files that Git for
//      Windows checked out as text, mode 120000, are left alone: AddLinkFix
//      replaced those on purpose);
//   2. only files whose content differs are written (they get a new date);
//   3. files an old patch had ADDED that the new set doesn't make are removed
//      (`git ls-files --others --exclude-standard`: untracked, not ignored,
//      so out/ and the other build output stay).
// False + message on failure (the SDK is then partly up to date: the next
// try does the rest the same way).
bool RefreshPatchedFiles(const fs::path& sdk, const std::vector<fs::path>& patches,
                         const fs::path& scratch, std::string* message) {
  std::vector<std::string> changed, created;
  TouchedFiles(patches, &changed, &created);
  auto lines_of = [](const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (!line.empty()) out.push_back(line);
    }
    return out;
  };
  auto has = [](const std::vector<std::string>& list, const std::string& item) {
    return std::find(list.begin(), list.end(), item) != list.end();
  };
  bool ok = false;
  // Tracked files the old patches changed (minus link files, see above).
  std::vector<std::string> modified =
      lines_of(task_runner::Capture({"git", "-c", "core.autocrlf=false", "diff", "--name-only",
                                    // the SDK's own libraries (submodules: AddLinkFix changes
                                    // files in them on Windows) aren't ours to restore
                                    "--ignore-submodules=all"},
                                   &ok, sdk, 60000));
  if (!ok) {
    *message = "git diff failed in " + sdk.string();
    return false;
  }
  if (!modified.empty()) {
    std::vector<std::string> argv = {"git", "ls-files", "-s", "--"};
    argv.insert(argv.end(), modified.begin(), modified.end());
    for (const std::string& line : lines_of(task_runner::Capture(argv, &ok, sdk, 60000))) {
      const size_t tab = line.find('\t');
      if (line.starts_with("120000 ") && tab != std::string::npos) {
        modified.erase(std::remove(modified.begin(), modified.end(), line.substr(tab + 1)), modified.end());
      }
    }
  }
  std::vector<std::string> from_head = changed;
  for (const std::string& file : modified) {
    if (!has(from_head, file) && !has(created, file)) from_head.push_back(file);
  }
  // 1. The wanted versions.
  std::error_code ec;
  fs::remove_all(scratch, ec);
  fs::create_directories(scratch / "patched", ec);
  if (!from_head.empty()) {
    std::vector<std::string> archive = {"git", "-c", "core.autocrlf=false", "archive", "-o",
                                        (scratch / "head.tar").string(), "HEAD", "--"};
    archive.insert(archive.end(), from_head.begin(), from_head.end());
    task_runner::Capture(archive, &ok, sdk, 60000);
    if (ok) task_runner::Capture({"tar", "-xf", "../head.tar"}, &ok, scratch / "patched", 60000);
    if (!ok) {
      *message = "couldn't take the SDK's original files (git archive)";
      fs::remove_all(scratch, ec);
      return false;
    }
  }
  if (!patches.empty()) {
    std::vector<std::string> apply = {"git", "-c", "core.autocrlf=false", "apply"};
    for (const fs::path& patch : patches) apply.push_back(patch.string());
    const std::string out = task_runner::Capture(apply, &ok, scratch / "patched", 60000);
    if (!ok) {
      *message = "the patches don't apply to this SDK: " + out;
      fs::remove_all(scratch, ec);
      return false;
    }
  }
  // 2. Write only what differs.
  auto read = [](const fs::path& file, bool* exists) {
    std::ifstream in(file, std::ios::binary);
    *exists = bool(in);
    return std::string((std::istreambuf_iterator<char>(in)), {});
  };
  int written = 0, kept = 0, removed = 0;
  std::vector<std::string> wanted = from_head;
  wanted.insert(wanted.end(), created.begin(), created.end());
  for (const std::string& file : wanted) {
    bool have_exists = false, want_exists = false;
    const std::string want = read(scratch / "patched" / file, &want_exists);
    const std::string have = read(sdk / file, &have_exists);
    if (!want_exists) continue;  // (a patch deleting a file: none in our set)
    if (have_exists && have == want) {
      ++kept;
      continue;
    }
    fs::create_directories((sdk / file).parent_path(), ec);
    std::ofstream out(sdk / file, std::ios::binary | std::ios::trunc);
    out << want;
    if (!out) {
      *message = "couldn't write " + (sdk / file).string();
      fs::remove_all(scratch, ec);
      return false;
    }
    ++written;
  }
  // 3. Files an old patch added that no new one makes.
  for (const std::string& file :
       lines_of(task_runner::Capture({"git", "ls-files", "--others", "--exclude-standard"}, &ok, sdk, 60000))) {
    if (ok && !has(created, file)) {
      if (fs::remove(sdk / file, ec)) ++removed;
    }
  }
  fs::remove_all(scratch, ec);
  *message = std::to_string(written) + " files updated, " + std::to_string(kept) + " already right, " +
             std::to_string(removed) + " removed.";
  return true;
}

// A fingerprint of the patch set (FNV-1a 64 over names + contents), written
// next to the SDK install by the launcher's SDK job: a different one later =
// a patch changed since that build. File dates can't tell: a patch file is
// usually saved AFTER the SDK build it came from.
std::string PatchFingerprint(const std::vector<fs::path>& patches) {
  uint64_t hash = 1469598103934665603ull;
  auto add = [&](const std::string& bytes) {
    for (unsigned char c : bytes) {
      hash = (hash ^ c) * 1099511628211ull;
    }
  };
  for (const fs::path& patch : patches) {
    add(patch.filename().string());
    std::ifstream in(patch, std::ios::binary);
    add(std::string((std::istreambuf_iterator<char>(in)), {}));
  }
  char text[24];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
  return text;
}
constexpr const char* kFingerprintFile = "crash_mom_patches.txt";

std::string Stamp() {
  const std::time_t now = std::time(nullptr);
  const std::tm local = platform::LocalTime(now);
  char text[32];
  std::strftime(text, sizeof(text), "%Y-%m-%d_%H%M%S", &local);
  return text;
}

const char* StateText(StepState state) {
  switch (state) {
    case StepState::kChecking: return trs("checking...");
    case StepState::kDone: return trs("done");
    case StepState::kOutdated: return trs("out of date");
    case StepState::kMissing: return trs("to do");
    case StepState::kBlocked: return trs("needs attention");
  }
  return "";
}


#if defined(_WIN32)

// Visual Studio's x64 build environment: what vcvars64.bat sets (compiler,
// Windows SDK and library paths), read once by running it and printing the
// environment. LLVM's and the Vulkan SDK's folders are added to PATH when
// their installers didn't (LLVM's doesn't by default).
//
// CLEAN (the build kit, see Kit): `clean_dirs` given = vcvars64.bat runs with
// PATH = those folders + Windows' own, and without the variables that steer
// compilers and build tools from outside (CC, CMAKE_*, INCLUDE, MSYSTEM,
// PYTHONHOME...). What comes out is Visual Studio's environment on top of
// exactly that: a devkitPro / MSYS2 / Cygwin / Strawberry Perl / Store
// shortcut folder on the player's PATH can no longer be found by any step.
std::shared_ptr<const platform::Environment> BuildEnvironment(
    const fs::path& vcvars, const fs::path& extra_path, const std::vector<fs::path>* clean_dirs = nullptr) {
  std::error_code ec;
  const fs::path script = fs::temp_directory_path(ec) / "crash_mom_vcvars.cmd";
  {
    std::ofstream out(script, std::ios::trunc);
    out << "@call \"" << vcvars.string() << "\" >nul\r\n@set\r\n";
  }
  platform::Environment start;
  if (clean_dirs) {
    static const char* const kOutside[] = {
        "PATH", "CC", "CXX", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "INCLUDE", "LIB", "LIBPATH",
        "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH", "PKG_CONFIG_PATH",
        "CMAKE_GENERATOR", "CMAKE_GENERATOR_PLATFORM", "CMAKE_GENERATOR_TOOLSET", "CMAKE_PREFIX_PATH",
        "CMAKE_TOOLCHAIN_FILE", "CMAKE_MAKE_PROGRAM", "CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER",
        "CMAKE_BUILD_PARALLEL_LEVEL", "CMAKE_EXPORT_COMPILE_COMMANDS", "VCPKG_ROOT", "MSYSTEM", "MSYS",
        "MSYS2_PATH_TYPE", "CHERE_INVOKING", "PYTHONHOME", "PYTHONPATH", "PYTHONSTARTUP", "VULKAN_SDK",
        "NINJA_STATUS", "DEVKITPRO", "DEVKITARM", "DEVKITPPC"};
    for (const auto& [key, value] : platform::CurrentEnvironment()) {
      bool outside = false;
      for (const char* name : kOutside) outside = outside || _stricmp(key.c_str(), name) == 0;
      if (!outside) start[key] = value;
    }
    const std::string windows = platform::GetEnv("SystemRoot").empty() ? "C:\\Windows"
                                                                       : platform::GetEnv("SystemRoot");
    std::string path;
    for (const fs::path& dir : *clean_dirs) {
      if (!dir.empty()) path += dir.string() + ";";
    }
    path += windows + "\\System32;" + windows + ";" + windows + "\\System32\\Wbem;" + windows +
            "\\System32\\WindowsPowerShell\\v1.0";
    start["Path"] = path;
  }
  bool ok = false;
  const std::string text =
      platform::Capture({script.string()}, &ok, {}, 60000, clean_dirs ? &start : nullptr);
  fs::remove(script, ec);
  if (!ok) {
    return nullptr;
  }
  auto env = std::make_shared<platform::Environment>();
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t eq = line.find('=');
    if (eq != std::string::npos && eq > 0) {
      (*env)[line.substr(0, eq)] = line.substr(eq + 1);
    }
  }
  if (!extra_path.empty()) {
    for (auto& [key, value] : *env) {
      if (_stricmp(key.c_str(), "PATH") == 0) {
        value = extra_path.string() + ";" + value;
      }
    }
  }
  return env;
}

// Step 1 on Windows: each tool, how it was found, and how the installer gets
// it when it's missing (Tool::winget / install_argv; Setup::ToolsJob).
//
// Fixes of 2026-11-05 (players reported the setup "never works" on Windows;
// found by reading this code, then tested on a Windows 11 PC):
//   - PATH is re-read from the registry first (platform::RefreshEnvironment):
//     tools installed while the installer was open used to stay "not found";
//   - Python: Windows' "python" is often only the Microsoft Store shortcut,
//     and a python.org install doesn't always put itself on PATH: the "py"
//     launcher and Python's default folders are tried too;
//   - CMake and Ninja: Visual Studio's C++ tools carry both; when they aren't
//     on PATH, Visual Studio's copies count (its build environment, which
//     every build step runs in, has them on PATH);
//   - the Visual C++ runtime (msvcp140.dll ...), which the BUILT GAME needs
//     to start, is checked too.
// Package ids are winget's (the same ones a community script used, PR #4 on
// GitHub: Visual Studio's override and winget's "already installed" code).

// THE TOOLS THE CHECKS FOUND, by full path (GitHub issue #16, 2026-11-09).
// The installer runs the disc tools with this Python, and every configure
// names these files to CMake (FoundToolOptions) instead of letting each
// library search PATH by itself. Why: the build's own search can land on a
// DIFFERENT file than the one the checks looked at. Issue #16: glslang
// searches "python3" before "python" in all of PATH; a python.org install
// has only python.exe, so CMake took the Microsoft Store's python3.exe
// SHORTCUT (an App Execution Alias in WindowsApps). It even answered "3.14"
// while configuring, then said "Python was not found" when the build ran it.
// The same family as issues #10/#11 (a CMake/Ninja found by us but not by
// the build). Empty = not found: the option is left out.
struct FoundTools {
  std::string python = "python";  // full path once the checks found one
  fs::path ninja, git, glslc;
};
std::mutex g_found_mutex;
FoundTools g_found;

std::string PythonProgram() {
  std::lock_guard lock(g_found_mutex);
  return g_found.python;
}

// "-D" options naming the checked tools, for the SDK's and the game's
// configure. Forward slashes: CMake's own spelling, so a value the cache
// already holds compares equal (nothing reruns).
//   PYTHON_EXECUTABLE   glslang (old FindPythonInterp)
//   Python3_EXECUTABLE  SPIRV-Tools, the Vulkan loader (FindPython3)
//   CMAKE_MAKE_PROGRAM  the Ninja every build runs
//   GIT_EXECUTABLE      the SDK's version text (rex_version.cmake)
//   GLSLC_EXECUTABLE    the game's shaders (CMakeLists.txt)
// Compilers are NOT named: a changed CMAKE_C_COMPILER makes CMake throw
// its whole cache away (= a full rebuild on every update), and clang is
// found by the same PATH search as in the checks.
std::vector<std::string> FoundToolOptions() {
  std::lock_guard lock(g_found_mutex);
  std::vector<std::string> options;
  auto add = [&](const char* name, const fs::path& file) {
    if (file.is_absolute()) options.push_back(std::string("-D") + name + "=" + file.generic_string());
  };
  add("PYTHON_EXECUTABLE", fs::path(g_found.python));
  add("Python3_EXECUTABLE", fs::path(g_found.python));
  add("CMAKE_MAKE_PROGRAM", g_found.ninja);
  add("GIT_EXECUTABLE", g_found.git);
  add("GLSLC_EXECUTABLE", g_found.glslc);
  return options;
}

// A real Python 3 (see above): its full path, or "" if none.
fs::path FindPython(std::string* version) {
  bool ok = false;
  auto try_exe = [&](const fs::path& exe) {
    if (exe.empty()) return false;
    const std::string out = platform::Capture({exe.string(), "--version"}, &ok);
    if (ok && out.find("Python 3") != std::string::npos) {
      *version = out.substr(0, out.find_first_of("\r\n"));
      return true;
    }
    return false;
  };
  // 1. "python" on PATH (fails quietly when it's the Store shortcut: that
  //    one prints nothing to us and exits 9009).
  if (const fs::path exe = platform::FindProgram("python"); try_exe(exe)) return exe;
  // 2. The "py" launcher python.org installs into Windows' own folder.
  if (!platform::FindProgram("py").empty()) {
    std::string where = platform::Capture({"py", "-3", "-c", "import sys; print(sys.executable)"}, &ok);
    where = where.substr(0, where.find_first_of("\r\n"));
    if (ok && try_exe(fs::path(where))) return fs::path(where);
  }
  // 3. Python's default folders (newest first by name: Python313 > Python312).
  std::vector<fs::path> roots;
  if (const std::string local = platform::GetEnv("LOCALAPPDATA"); !local.empty()) {
    roots.push_back(fs::path(local) / "Programs" / "Python");
  }
  if (const std::string pf = platform::GetEnv("ProgramFiles"); !pf.empty()) roots.push_back(pf);
  std::vector<fs::path> found;
  std::error_code ec;
  for (const fs::path& root : roots) {
    for (const auto& entry : fs::directory_iterator(root, ec)) {
      const std::string name = entry.path().filename().string();
      if (name.starts_with("Python3") && Exists(entry.path() / "python.exe")) {
        found.push_back(entry.path() / "python.exe");
      }
    }
  }
  std::sort(found.rbegin(), found.rend());
  for (const fs::path& exe : found) {
    if (try_exe(exe)) return exe;
  }
  return {};
}

void CheckTools(Status* s, const Kit& kit) {
  s->distro = "Windows";
  // What Windows has NOW (see above), before any lookup.
  platform::RefreshEnvironment();
  const std::string pf_env = platform::GetEnv("ProgramFiles");
  const std::string pf86_env = platform::GetEnv("ProgramFiles(x86)");
  const std::string program_files = pf_env.empty() ? "C:\\Program Files" : pf_env;
  const std::string program_files_x86 = pf86_env.empty() ? "C:\\Program Files (x86)" : pf86_env;
  std::string extra_path;  // folders of tools found off PATH
  auto add = [&](const char* label, const char* package, bool found, std::string detail,
                 const char* winget = "", const char* override_args = "") {
    Tool tool;
    tool.label = label;
    tool.found = found;
    tool.detail = std::move(detail);
    tool.package = package;
    tool.winget = winget;
    tool.winget_override = override_args;
    s->tools.push_back(std::move(tool));
  };
  bool ok = false;
  std::string version;

  // Visual Studio first: its C++ tools also carry CMake and Ninja (below).
  // vswhere = Microsoft's own finder, installed with every Visual Studio.
  const fs::path vs_installer = fs::path(program_files_x86) / "Microsoft Visual Studio" / "Installer";
  const fs::path vswhere = vs_installer / "vswhere.exe";
  fs::path vs_cpp, vs_any;  // with the C++ tools / any Visual Studio at all
  if (Exists(vswhere)) {
    auto where = [&](std::vector<std::string> extra) {
      std::vector<std::string> argv = {vswhere.string(), "-latest", "-products", "*"};
      argv.insert(argv.end(), extra.begin(), extra.end());
      argv.insert(argv.end(), {"-property", "installationPath"});
      std::string text = platform::Capture(argv, &ok);
      text = text.substr(0, text.find_first_of("\r\n"));
      return ok ? fs::path(text) : fs::path();
    };
    vs_cpp = where({"-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64"});
    vs_any = where({});
  }
  const fs::path vcvars = vs_cpp.empty() ? fs::path()
                                         : vs_cpp / "VC" / "Auxiliary" / "Build" / "vcvars64.bat";
  const fs::path vs_cmake_dir = vs_cpp.empty() ? fs::path()
      : vs_cpp / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake";

  // clang: on PATH, or LLVM's default folder (its installer doesn't add it).
  // With the kit, LLVM's own folder FIRST (the clean PATH puts that folder
  // in): another clang on PATH (MSYS2's, devkitPro's) targets another system.
  const fs::path llvm_clang = fs::path(program_files) / "LLVM" / "bin" / "clang.exe";
  fs::path clang = kit.present && Exists(llvm_clang) ? llvm_clang : platform::FindProgram("clang");
  if (clang.empty() && Exists(llvm_clang)) {
    clang = llvm_clang;
    extra_path += llvm_clang.parent_path().string() + ";";
  }
  if (clang.empty()) {
    add("clang 20 or newer", "LLVM", false, trs("not found"), "LLVM.LLVM");
  } else {
    const int major = MajorVersion(platform::Capture({clang.string(), "--version"}, &ok), &version);
    add("clang 20 or newer", "LLVM", major >= 20,
        major >= 20 ? version : trf(trs("too old: {0} ({1} or newer needed)"), {version, "20"}),
        "LLVM.LLVM");
  }
  if (kit.present) {
    // This release's own CMake, Ninja, glslc and Python (Kit): nothing to
    // look for on the PC, nothing to install. One line for all of them.
    std::string missing;
    for (const fs::path& file : KitFiles(kit)) {
      if (!Exists(file)) missing += (missing.empty() ? "" : ", ") + file.filename().string();
    }
    add("CMake, Ninja, glslc, Python (included)", "Mind over Recomp", missing.empty(),
        missing.empty() ? std::string(trs("included in this release"))
                        : trf(trs("files missing from the release folder ({0}): run the installer's Update "
                                  "(same version = repair), and check your antivirus"),
                              {missing}));
    std::lock_guard lock(g_found_mutex);
    g_found.python = kit.python.string();
    g_found.ninja = kit.ninja;
    g_found.glslc = kit.glslc;
    g_found.git.clear();
  } else {
    // CMake 3.25+: on PATH, else Visual Studio's own copy.
    fs::path cmake = platform::FindProgram("cmake");
    if (cmake.empty() && Exists(vs_cmake_dir / "CMake" / "bin" / "cmake.exe")) {
      cmake = vs_cmake_dir / "CMake" / "bin" / "cmake.exe";
      extra_path += cmake.parent_path().string() + ";";
    }
    if (cmake.empty()) {
      add("CMake 3.25 or newer", "CMake", false, trs("not found"), "Kitware.CMake");
    } else {
      const int major = MajorVersion(platform::Capture({cmake.string(), "--version"}, &ok), &version);
      const bool good = major > 3 || (major == 3 && MinorVersion(version) >= 25);
      add("CMake 3.25 or newer", "CMake", good,
          good ? version : trf(trs("too old: {0} ({1} or newer needed)"), {version, "3.25"}),
          "Kitware.CMake");
    }
    // Ninja: on PATH (winget puts it in its "Links" folder), else Visual Studio's.
    fs::path ninja = platform::FindProgram("ninja");
    if (ninja.empty() && Exists(vs_cmake_dir / "Ninja" / "ninja.exe")) {
      ninja = vs_cmake_dir / "Ninja" / "ninja.exe";
      extra_path += ninja.parent_path().string() + ";";
    }
    add("Ninja", "Ninja", !ninja.empty(), ninja.empty() ? trs("not found") : ninja.string(),
        "Ninja-build.Ninja");
    // Python 3 (see above). Installed with "add to PATH" on, so the build's own
    // tools find it too.
    const fs::path python = FindPython(&version);
    if (!python.empty()) {
      std::lock_guard lock(g_found_mutex);
      g_found.python = python.string();
      extra_path += python.parent_path().string() + ";";
    }
    add("Python 3", "Python 3.12", !python.empty(),
        python.empty() ? trs("not found (or only the Store shortcut)") : version + "  " + python.string(),
        "Python.Python.3.12", "/quiet InstallAllUsers=0 PrependPath=1 Include_launcher=1");
    const fs::path git = platform::FindProgram("git");
    add("Git", "Git", !git.empty(), git.empty() ? trs("not found") : git.string(), "Git.Git");
    // glslc: on PATH or in the Vulkan SDK (VULKAN_SDK, re-read from the registry).
    fs::path glslc = platform::FindProgram("glslc");
    if (const std::string sdk = platform::GetEnv("VULKAN_SDK"); glslc.empty() && !sdk.empty()) {
      const fs::path in_sdk = fs::path(sdk) / "Bin" / "glslc.exe";
      if (Exists(in_sdk)) {
        glslc = in_sdk;
        extra_path += in_sdk.parent_path().string() + ";";
      }
    }
    add("glslc (shader compiler)", "Vulkan SDK", !glslc.empty(),
        glslc.empty() ? trs("not found") : glslc.string(), "KhronosGroup.VulkanSDK");
    {
      // For the configures (FoundToolOptions). Python is stored above.
      std::lock_guard lock(g_found_mutex);
      g_found.ninja = ninja;
      g_found.git = git;
      g_found.glslc = glslc;
    }
  }
  // Visual Studio's C++ tools + the Windows SDK (Windows' own headers).
  std::string windows_sdk;
  std::error_code ec;
  for (const auto& entry :
       fs::directory_iterator(fs::path(program_files_x86) / "Windows Kits" / "10" / "Include", ec)) {
    if (Exists(entry.path() / "um" / "windows.h")) {
      windows_sdk = entry.path().filename().string();
    }
  }
  {
    Tool tool;
    tool.label = "Visual Studio C++ tools + Windows SDK";
    tool.package = "Visual Studio Build Tools 2022";
    tool.found = Exists(vcvars) && !windows_sdk.empty();
    tool.detail = tool.found ? vs_cpp.string() + "  (Windows SDK " + windows_sdk + ")"
                  : !Exists(vcvars) ? std::string(trs("not found"))
                                    : std::string(trs("the Windows SDK is missing"));
    if (!tool.found && !vs_any.empty() && Exists(vs_installer / "setup.exe")) {
      // A Visual Studio is there, just without these parts: its own installer
      // adds them (it needs administrator rights: asked through Windows' usual
      // prompt, which CreateProcess can't show, hence PowerShell's RunAs).
      const std::string args = "modify --installPath \"" + vs_any.string() +
                               "\" --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
                               " --add Microsoft.VisualStudio.Component.Windows11SDK.26100"
                               " --passive --norestart";
      tool.install_argv = {"powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command",
                           "$p = Start-Process -FilePath '" + (vs_installer / "setup.exe").string() +
                               "' -ArgumentList '" + args +
                               "' -Verb RunAs -Wait -PassThru; exit $p.ExitCode"};
    } else {
      // Microsoft's free Build Tools with the C++ workload; "recommended" brings
      // the Windows SDK. --passive: Visual Studio's own progress window shows.
      tool.winget = "Microsoft.VisualStudio.2022.BuildTools";
      tool.winget_override =
          "--passive --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools "
          "--includeRecommended";
    }
    s->tools.push_back(std::move(tool));
  }
  // The Visual C++ runtime: what the BUILT GAME loads at start (the launcher
  // has it built in; the game, built with Visual Studio's libraries, doesn't).
  {
    const fs::path system32 = fs::path(platform::GetEnv("SystemRoot")) / "System32";
    const bool runtime = Exists(system32 / "msvcp140.dll") && Exists(system32 / "vcruntime140_1.dll");
    add("Visual C++ runtime (x64)", "Visual C++ Redistributable", runtime,
        runtime ? "msvcp140.dll" : trs("not found"), "Microsoft.VCRedist.2015+.x64");
  }
  const bool vulkan = platform::LibraryAvailable("", "vulkan-1.dll");
  add("Vulkan loader", "your graphics card's driver", vulkan,
      vulkan ? "vulkan-1.dll" : trs("not found: update your graphics card's driver"));

  if (Exists(vcvars)) {
    if (kit.present) {
      const std::vector<fs::path> dirs = {kit.cmake.parent_path(), kit.ninja.parent_path(),
                                          kit.glslc.parent_path(), kit.python.parent_path(),
                                          clang.empty() ? fs::path() : clang.parent_path()};
      s->build_env = BuildEnvironment(vcvars, {}, &dirs);
    } else {
      s->build_env = BuildEnvironment(vcvars, extra_path);
    }
  }
}

#else  // Linux

// The tools of step 1, and the package that has each one, per family
// (kArch, kDebian, kFedora, kSuse). Keep in sync with docs/01-building.md.
struct ToolSpec {
  const char* label;
  const char* packages[4];
};
constexpr ToolSpec kTools[] = {
    {"clang 20 or newer", {"clang", "clang", "clang", "clang"}},
    {"CMake 3.25 or newer", {"cmake", "cmake", "cmake", "cmake"}},
    {"Ninja", {"ninja", "ninja-build", "ninja-build", "ninja"}},
    {"Python 3", {"python", "python3", "python3", "python3"}},
    {"Git", {"git", "git", "git", "git"}},
    {"glslc (shader compiler)", {"shaderc", "glslc", "glslc", "shaderc"}},
    {"pkg-config", {"pkgconf", "pkg-config", "pkgconf-pkg-config", "pkg-config"}},
    {"GTK 3 headers", {"gtk3", "libgtk-3-dev", "gtk3-devel", "gtk3-devel"}},
    {"Vulkan loader", {"vulkan-icd-loader", "libvulkan-dev", "vulkan-loader-devel", "vulkan-devel"}},
};

std::string PythonProgram() { return "python3"; }

// Step 1 on Linux: each tool with its package on this distribution (no
// kit: Linux releases build with the distribution's packages).
void CheckTools(Status* s, const Kit&) {
  const Distro distro = DetectDistro();
  s->distro = distro.name;
  for (size_t i = 0; i < std::size(kTools); ++i) {
    Tool tool;
    tool.label = kTools[i].label;
    tool.package = distro.family == kUnknown ? "" : kTools[i].packages[distro.family];
    bool ok = false;
    std::string version;
    switch (i) {
      case 0: {  // clang + clang++, 20+
        if (platform::FindProgram("clang").empty() || platform::FindProgram("clang++").empty()) {
          tool.detail = trs("not found (clang and clang++)");
          break;
        }
        const int major = MajorVersion(platform::Capture({"clang", "--version"}, &ok), &version);
        tool.found = major >= 20;
        tool.detail = tool.found ? version : trf(trs("too old: {0} ({1} or newer needed)"), {version, "20"});
        break;
      }
      case 1: {  // cmake 3.25+
        if (platform::FindProgram("cmake").empty()) {
          tool.detail = trs("not found");
          break;
        }
        const int major = MajorVersion(platform::Capture({"cmake", "--version"}, &ok), &version);
        tool.found = major > 3 || (major == 3 && MinorVersion(version) >= 25);
        tool.detail = tool.found ? version : trf(trs("too old: {0} ({1} or newer needed)"), {version, "3.25"});
        break;
      }
      case 7: {  // GTK 3 headers: pkg-config knows gtk+-3.0
        const std::string text =
            platform::Capture({"pkg-config", "--modversion", "gtk+-3.0"}, &ok);
        tool.found = ok;
        tool.detail = ok ? text.substr(0, text.find('\n')) : trs("not found (needs pkg-config too)");
        break;
      }
      case 8:  // the Vulkan loader library
        tool.found = platform::LibraryAvailable("libvulkan.so.1", "");
        tool.detail = tool.found ? "libvulkan.so.1" : trs("not found");
        break;
      default: {
        static const char* kPrograms[] = {"", "", "ninja", "python3", "git", "glslc", "pkg-config"};
        const fs::path program = platform::FindProgram(kPrograms[i]);
        tool.found = !program.empty();
        tool.detail = tool.found ? program.string() : trs("not found");
        break;
      }
    }
    s->tools.push_back(std::move(tool));
  }
}

#endif

// NO FILE FROM THE FUTURE (2026-11-06, GitHub issue #7). Ninja rebuilds
// whatever is older than its inputs, and re-writes its build plan
// (build.ninja) whenever one of CMake's inputs is newer than it. A file
// dated later than "now" is newer forever: Ninja re-writes the plan 100
// times and gives up ("manifest 'build.ninja' still dirty after 100 tries").
// Two ways to get such a file:
//   - a wrong clock / time zone when the files were written;
//   - the release .zip: zip dates carry NO time zone, ours were written in
//     UTC on GitHub's machines and Windows reads them as LOCAL time, so in
//     time zones behind UTC (the Americas) the 0.2.0 files were up to ~10
//     hours "ahead" on release day (found from #7: clock synced, still failing).
// So before each build step, every file under `folder` dated later than now
// is set to now. Only dates change, never contents; a file that's merely
// "now" is still rebuilt as usual. Links / junctions aren't followed.
Command NoFutureDates(const fs::path& folder) {
  Command command;
  command.title = trs("Checking file dates");
  command.function = [folder](std::string* message) {
    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    int fixed = 0, failed = 0;
    for (auto it = fs::recursive_directory_iterator(
             folder, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
      std::error_code file_ec;
      if (!it->is_regular_file(file_ec) || it->is_symlink(file_ec)) {
        continue;
      }
      if (it->last_write_time(file_ec) > now && !file_ec) {
        fs::last_write_time(it->path(), now, file_ec);
        ++(file_ec ? failed : fixed);
      }
    }
    // Never fails the job: a date it can't fix shows up as Ninja's own error.
    *message = fixed || failed
                   ? std::to_string(fixed) + " files were dated in the future: set to now" +
                         (failed ? " (" + std::to_string(failed) + " couldn't be changed)" : "") + ".\n"
                   : "No file dated in the future.\n";
    return true;
  };
  return command;
}

}  // namespace

// -----------------------------------------------------------------------------
// Checks
// -----------------------------------------------------------------------------

Setup::Setup(folders::Folders folders, std::string feed, fs::path launcher, bool after_update)
    : folders_(std::move(folders)), launcher_(std::move(launcher)) {
  continue_setup_ = after_update;
  if (folders_.release) {
    updater_ = std::make_unique<update::Updater>(folders_, std::move(feed));
    updater_->Check();  // quietly, once at start
  }
  Check();
}

bool Setup::UpdateAvailable(std::string* version) {
  if (!updater_ || update_applied_) {
    return false;
  }
  update::Release latest;
  bool available = false;
  std::string error;
  if (updater_->Latest(&latest, &available, &error) && available) {
    *version = latest.version;
    return true;
  }
  return false;
}

// The Updates line at the top of the tab (update.h).
void Setup::DrawUpdates(bool busy) {
  const std::string installed = update::InstalledVersion(folders_);
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(look_->muted, trs("Version %s"), installed.empty() ? "?" : installed.c_str());
  ImGui::SameLine();
  if (!updater_) {
    ImGui::TextColored(look_->muted, trs(" -  a developer copy: update it with git pull"));
    ImGui::Separator();
    return;
  }
  if (update_applied_) {
    ImGui::TextColored(look_->good, trs(" -  updated to %s: restart the launcher to finish"),
                       pending_.version.c_str());
    ImGui::SameLine();
    if (ImGui::Button(trs("Restart the launcher"))) {
      // The new launcher, in this process's place; it carries on with the
      // rebuild (--after_update). Returns only if that failed.
      platform::Restart(launcher_, {"--after_update"});
      message_ = trs("Couldn't restart: start the launcher again by hand.");
    }
    ImGui::Separator();
    return;
  }
  update::Release latest;
  bool available = false;
  std::string error;
  const bool known = updater_->Latest(&latest, &available, &error);
  if (updater_->checking()) {
    ImGui::TextColored(look_->muted, trs(" -  checking for updates..."));
  } else if (known && available) {
    ImGui::TextColored(look_->accent, trs(" -  %s is available%s%s"), latest.version.c_str(),
                       latest.date.empty() ? "" : " (", latest.date.empty() ? "" : (latest.date + ")").c_str());
    ImGui::SameLine();
    // (Not while Setup's checks run: they read the SDK folder the update moves.)
    ImGui::BeginDisabled(busy || checking_);
    if (ImGui::Button(trf(trs("Update to {0}"), {latest.version}).c_str())) {
      pending_ = latest;
      std::vector<Command> commands = updater_->Job(latest, launcher_);
      message_.clear();
      follow_ = true;
      runner_.Start(trf(trs("Update to {0}"), {latest.version}), std::move(commands),
                    folders_.user / "logs" / ("update-" + Stamp() + ".log"), [this](bool success) {
                      if (success) {
                        update_applied_ = true;
                      }
                      Check();
                    });
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
      ImGui::SetTooltip(trs("Downloads the new version and swaps it in: your saves, settings and the "
                        "game's files stay. Then the launcher restarts and rebuilds what changed."));
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(trs("Release page"))) {
      SDL_OpenURL(update::ReleasePage(latest.version).c_str());
    }

  } else if (known) {
    ImGui::TextColored(look_->good, trs(" -  the newest version"));
  } else if (!error.empty()) {
    ImGui::TextColored(look_->muted, " -  %s", error.c_str());
  }
  if (!updater_->checking() && !update_applied_) {
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::SmallButton(trs("Check for updates"))) {
      updater_->Check();
    }
    ImGui::EndDisabled();
  }
  if (known && available) {
    // What the update brings (update.h: WHAT'S NEW).
    if (!latest.notes.empty()) {
      ImGui::PushFont(look_->bold, 0.0f);
      ImGui::Text(trs("What's new in %s"), latest.version.c_str());
      ImGui::PopFont();
      ImGui::Indent();
      ImGui::TextWrapped("%s", latest.notes.c_str());
      ImGui::Unindent();
    }
  }
  ImGui::Separator();
}

Setup::~Setup() {
  runner_.Cancel();
  if (checker_.joinable()) {
    checker_.join();
  }
}

void Setup::Check() {
  if (checking_.exchange(true)) {
    return;
  }
  if (checker_.joinable()) {
    checker_.join();
  }
  checker_ = std::thread([this] {
    Status status = RunChecks();
    std::lock_guard lock(mutex_);
    status_ = std::move(status);
    checking_ = false;
  });
}

Status Setup::RunChecks() {
  Status s;
  const fs::path root = folders_.source;  // the source tree (release: <game folder>/source)
  const fs::path sdk = root / "thirdparty" / "rexglue-sdk";
  const fs::path install = root / "thirdparty" / "rexglue-install";
  const Pin pin = ReadPin(root);

  // 1. Tools (and, on Windows, Visual Studio's build environment).
  const Kit kit = folders_.release ? KitIn(root) : Kit{};
  CheckTools(&s, kit);
  const bool tools_ok = std::all_of(s.tools.begin(), s.tools.end(), [](const Tool& t) { return t.found; });
  s.tools_state = tools_ok ? StepState::kDone : StepState::kMissing;

  // 2. Source parts. With the build kit (Kit) there are none to download:
  // the SDK comes ready-built in the release (steps 2 and 4 = "included").
  bool source_ok = kit.present || (Exists(sdk / "CMakeLists.txt") &&
                                   Exists(sdk / "thirdparty" / "fmt" / "CMakeLists.txt") &&
                                   Exists(sdk / "thirdparty" / "sdl3" / "CMakeLists.txt"));
  if (source_ok && !kit.present) {
    // Every one of the SDK's own libraries checked out, at the commit the SDK
    // records? A download stopped halfway leaves some empty. (git_parts, not
    // `git submodule status`: that one is a shell script, and Git's shell
    // can't start programs on some Windows PCs, issue #14.)
    bool ok = false, all_ok = false;
    git_parts::AllParts(sdk, &ok, &all_ok);
    source_ok = ok && all_ok;
  }
  s.source_state = source_ok ? StepState::kDone : StepState::kMissing;
  s.source_detail = kit.present ? trs("included in this release")
                    : source_ok ? trs("the ReXGlue SDK and its libraries are there")
                              : folders_.release ? trs("the ReXGlue SDK isn't downloaded yet")
                                                 : trs("the ReXGlue SDK submodule isn't downloaded yet");
  // A release names the exact SDK version (source/RELEASE.toml): another
  // one in the folder (an update moved to a newer SDK) = download again.
  if (source_ok && folders_.release && !kit.present && !pin.commit.empty()) {
    bool ok = false;
    std::string head = task_runner::Capture({"git", "rev-parse", "HEAD"}, &ok, sdk);
    head = head.substr(0, head.find('\n'));
    if (!ok || head != pin.commit) {
      source_ok = false;
      s.source_state = StepState::kOutdated;
      s.source_detail = trf(trs("this version needs another ReXGlue SDK ({0}): download it again"),
                            {pin.tag.empty() ? pin.commit.substr(0, 12) : pin.tag});
    }
  }

  // 3. Disc.
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(folders_.root, ec)) {
    if (entry.is_regular_file(ec) && entry.path().extension() == ".iso") {
      s.isos.push_back(entry.path());
    }
  }
  if (folders_.DiscExists()) {
    s.disc_state = StepState::kDone;
    s.disc_detail = trf(trs("extracted in {0}"), {folders::Pretty(folders_.disc)});
  } else {
    s.disc_state = StepState::kMissing;
    s.disc_detail = s.isos.empty() ? std::string(trs("no disc image found yet: pick your .iso"))
                                   : trf(trs("ready to extract {0}"), {s.isos.front().filename().string()});
  }

  // 4. SDK: patches, then the install.
  const std::vector<fs::path> patches = Patches(root);
  s.patches_total = int(patches.size());
  // The patches are a STACK (0013 builds on 0009): `git apply --check` of the
  // set fails both ways (forward on a clean tree, reverse on a patched one),
  // so CheckPatchStack compares the files themselves.
  bool all_applied = false, none_applied = false;
  if (source_ok && !kit.present && !patches.empty()) {
    // The scratch folder must be OUTSIDE any git repository: `git apply`
    // inside one takes the paths as relative to that repository's root and
    // silently skips them (the game folder is one when built from source).
    std::error_code tmp_ec;
    const fs::path scratch = fs::temp_directory_path(tmp_ec) /
                             ("crash_mom_patch_check_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const PatchState state = CheckPatchStack(sdk, patches, scratch);
    all_applied = state == PatchState::kAll;
    none_applied = state == PatchState::kNone;
  }
  s.patches_applied = all_applied ? s.patches_total : 0;
  const fs::path runtime = RuntimeLibrary(install);
  // The recompiler: RelWithDebInfo's "rexgluerd" (SDK patch 0019: one SDK
  // build) or an older install's Release "rexglue".
  const bool installed = (Exists(install / "bin" / platform::ExeName("rexgluerd")) ||
                          Exists(install / "bin" / platform::ExeName("rexglue"))) &&
                         Exists(runtime);
  std::string built_with;  // the fingerprint the launcher's last SDK build left
  {
    std::ifstream in(install / kFingerprintFile);
    std::getline(in, built_with);
  }
  if (kit.present) {
    // Built by GitHub Actions with our patches (the release workflow); its
    // files can only be missing if something deleted them (an antivirus).
    s.patches_applied = s.patches_total;
    s.sdk_state = installed && Exists(kit.sdk_marker) ? StepState::kDone : StepState::kBlocked;
    s.sdk_detail = s.sdk_state == StepState::kDone
                       ? trf(trs("included in this release (built with our {0} patches)"),
                             {std::to_string(s.patches_total)})
                       : trs("files of the included SDK are missing: run the installer's Update (same "
                             "version = repair), and check your antivirus");
  } else if (!source_ok) {
    s.sdk_state = StepState::kMissing;
    s.sdk_detail = trs("needs the source parts first");
  } else if (!patches.empty() && !all_applied && !none_applied && folders_.release) {
    // A release's SDK folder is the launcher's own download: patched by an
    // older patch set (an update brought new ones). Back to clean, then the
    // new set (SdkJob).
    s.sdk_state = StepState::kOutdated;
    s.sdk_needs_reset = true;
    s.sdk_detail = trs("the patches changed (an update): it will be reset and patched again, then "
                       "rebuilt");
  } else if (!patches.empty() && !all_applied && !none_applied) {
    s.sdk_state = StepState::kBlocked;
    s.sdk_detail = trf(trs("our {0} SDK patches are neither all applied nor all missing: the SDK folder was "
                           "changed by hand (or a patch was updated). Not touched; see "
                           "docs/01-building.md, step 2 (git -C thirdparty/rexglue-sdk status)."),
                       {std::to_string(s.patches_total)});
  } else if (!installed) {
    s.sdk_state = StepState::kMissing;
    s.sdk_detail = s.patches_applied ? trs("patched, not built yet") : trs("not patched or built yet");
  } else if (s.patches_applied < s.patches_total ||
             (!built_with.empty() && built_with != PatchFingerprint(patches))) {
    s.sdk_state = StepState::kOutdated;
    s.sdk_detail = s.patches_applied < s.patches_total
                       ? trs("built without our patches: rebuild it")
                       : trs("the patches changed since the SDK was built: rebuild it");
  } else {
    s.sdk_state = StepState::kDone;
    s.sdk_detail = trf(trs("built and installed, {0} patches"), {std::to_string(s.patches_total)});
  }

  // 5. The game (a release then plays a copy in program/).
  const fs::path built = folders_.BuiltExe();
  if (!Exists(built)) {
    s.game_state = StepState::kMissing;
    s.game_detail = trs("not built yet");
  } else if (folders_.release && TimeOf(folders_.exe) < TimeOf(built)) {
    s.game_state = StepState::kOutdated;
    s.game_detail = trs("built, not copied into program/ yet");
  } else {
    const auto exe_time = TimeOf(built);
    // Newer sources, or ANY newer SDK library (an SDK patch may change only
    // the GPU plugin, librexgpu-xenos*.so): the build stages them all next to
    // the game, and a release copies them into program/.
    fs::file_time_type newest_lib = fs::file_time_type::min();
    for (const auto& entry : fs::directory_iterator(install / kLibFolder, ec)) {
      if (entry.path().string().find(kLibSuffix) != std::string::npos) {
        newest_lib = std::max(newest_lib, TimeOf(entry.path()));
      }
    }
    ec.clear();
    const auto newest = std::max({NewestUnder(root / "src"), TimeOf(root / "crash_mom_manifest.toml"),
                                  TimeOf(root / "CMakeLists.txt"), newest_lib});
    if (newest > exe_time) {
      s.game_state = StepState::kOutdated;
      s.game_detail = trs("the sources changed since the last build: rebuild it");
    } else {
      s.game_state = StepState::kDone;
      s.game_detail = trf(trs("built: {0}"), {folders::Pretty(folders_.exe)});
    }
  }
  s.done = true;
  return s;
}

bool Setup::NeedsAttention() {
  return !folders_.ExeExists() || !folders_.DiscExists();
}

// -----------------------------------------------------------------------------
// Jobs
// -----------------------------------------------------------------------------

std::vector<Command> Setup::SourceJob() {
  const fs::path sdk = folders_.source / "thirdparty" / "rexglue-sdk";
  if (!folders_.release) {
    // A developer clone: the submodule, as git records it.
    std::vector<Command> commands;
    std::vector<std::string> argv = {"git"};
    argv.insert(argv.end(), kGitOptions.begin(), kGitOptions.end());
    argv.insert(argv.end(), {"submodule", "update", "--init", "--recursive", "--progress"});
    commands.push_back({trs("Downloading the source parts (git submodules)"), argv, folders_.source});
    AddLinkFix(sdk, &commands);
    return commands;
  }
  // A release has no git history: the exact SDK version it names
  // (source/RELEASE.toml), downloaded straight from its repository, its own
  // libraries included (shallow: no history, ~550 MB of sources).
  const Pin pin = ReadPin(folders_.source);
  std::vector<Command> commands;
  // Already the right SDK version, only its libraries unfinished (a download
  // that was stopped): finish those instead of downloading everything again.
  bool head_ok = false;
  std::string head = task_runner::Capture({"git", "rev-parse", "HEAD"}, &head_ok, sdk);
  head = head.substr(0, head.find('\n'));
  // The libraries come through git_parts (plain init / fetch / checkout), not
  // `git submodule update`: that is a shell script, and Git for Windows' shell
  // can't start its helper programs on some PCs (0xC0000142, issue #14).
  if (head_ok && head == pin.commit) {
    Command finish;
    finish.title = trs("Finishing the SDK's libraries (a download was stopped)");
    finish.function = [sdk](std::string* message) {
      git_parts::DownloadCommands(sdk, kGitOptions, message);  // only for the count
      return true;
    };
    finish.then = [sdk]() { return git_parts::DownloadCommands(sdk, kGitOptions); };
    commands.push_back(std::move(finish));
    AddLinkFix(sdk, &commands);
    return commands;
  }
  Command clear;
  clear.title = trs("Removing the old SDK download");
  clear.function = [sdk](std::string* message) {
    std::error_code ec;
    fs::remove_all(sdk, ec);
    *message = ec ? "couldn't remove " + sdk.string() + ": " + ec.message() : "";
    return !ec;
  };
  commands.push_back(std::move(clear));
  {
    std::vector<std::string> argv = {"git"};
    argv.insert(argv.end(), kGitOptions.begin(), kGitOptions.end());
    // The SDK alone (no --recurse-submodules: that runs `git submodule`, see
    // above); its libraries follow, one by one, once their list is here.
    argv.insert(argv.end(), {"clone", "--depth", "1", "--branch", pin.tag, pin.url, sdk.string()});
    Command clone{trf(trs("Downloading the ReXGlue SDK {0} (and its libraries)"), {pin.tag}), argv,
                  folders_.source};
    clone.then = [sdk]() { return git_parts::DownloadCommands(sdk, kGitOptions); };
    commands.push_back(std::move(clone));
  }
  AddLinkFix(sdk, &commands);
  Command verify;
  verify.title = trs("Checking the SDK's version");
  verify.function = [sdk, pin](std::string* message) {
    bool ok = false;
    std::string head = task_runner::Capture({"git", "rev-parse", "HEAD"}, &ok, sdk);
    head = head.substr(0, head.find('\n'));
    if (head != pin.commit) {
      *message = "The SDK download is " + head + ", this version needs " + pin.commit + ".";
      return false;
    }
    *message = "SDK " + pin.tag + " = " + head.substr(0, 12) + ", as this version expects.";
    return true;
  };
  commands.push_back(std::move(verify));
  return commands;
}

bool Setup::DiscJob(const fs::path& iso, std::vector<Command>* out, std::string* error) {
  if (iso.empty()) {
    *error = trs("Pick your disc image (.iso) first.");
    return false;
  }
  const double free_gb = platform::FreeDiskGb(folders_.root);
  if (free_gb >= 0 && free_gb < 7) {
    char text[160];
    std::snprintf(text, sizeof(text),
                  trs("Not enough free space: the game's files need about 6.5 GB, %.1f GB are free."),
                  free_gb);
    *error = text;
    return false;
  }
  // The extractor's --list: is it a 360 disc with the game, and how many files.
  bool ok = false;
  const std::string listing = task_runner::Capture(
      {PythonProgram(), (folders_.source / "tools" / "xiso_extract.py").string(), "--list", iso.string()},
      &ok, folders_.root, 60000);
  std::smatch match;
  static const std::regex count(R"(\[xiso\] (\d+) files)");
  if (!ok || !std::regex_search(listing, match, count)) {
    *error = trs("That file isn't an Xbox 360 disc image the extractor can read.");
    // What the extractor said (or that it didn't start), for the log.
    *error += "\n(" + PythonProgram() + ": " + (listing.empty() ? std::string("no output") : listing.substr(0, 400)) + ")";
    return false;
  }
  if (listing.find(" default.xex") == std::string::npos) {
    *error = trs("That disc image has no default.xex: not a game disc?");
    return false;
  }
  Command extract{trs("Extracting the disc into game/"),
                  {PythonProgram(), "-u", (folders_.source / "tools" / "xiso_extract.py").string(),
                   iso.string(), folders_.disc.string()},
                  folders_.root,
                  Progress::kCountLines};
  extract.count_prefix = "  ";
  extract.count_total = std::stoi(match.str(1));
  out->push_back(std::move(extract));
  // Is it THIS game? The executable's title id (tools/xex_info.py).
  const fs::path xex = folders_.disc / "default.xex";
  const fs::path root = folders_.source;
  Command verify;
  verify.title = trs("Checking the game's executable");
  verify.function = [xex, root](std::string* message) {
    bool ok = false;
    const std::string info = task_runner::Capture(
        {PythonProgram(), (root / "tools" / "xex_info.py").string(), xex.string()}, &ok, root);
    if (info.find("565507FA") == std::string::npos) {
      *message = trs("default.xex isn't Crash: Mind over Mutant (title id 565507FA not found). "
                     "The files are in game/; this port only runs that game.");
      return false;
    }
    *message = trs("Title id 565507FA: Crash: Mind over Mutant.");
    return true;
  };
  out->push_back(std::move(verify));
  return true;
}

bool Setup::SdkJob(std::vector<Command>* out, std::string* error) {
  Status status;
  {
    std::lock_guard lock(mutex_);
    status = status_;
  }
  if (status.sdk_state == StepState::kBlocked) {
    *error = status.sdk_detail;
    return false;
  }
  const fs::path sdk = folders_.source / "thirdparty" / "rexglue-sdk";
  out->push_back(NoFutureDates(sdk));  // issue #7: see NoFutureDates
  if (status.sdk_needs_reset) {
    // A release's own SDK download, patched by an older set (RunChecks): the
    // new set, rewriting only the files whose content changes, so the SDK
    // rebuild (and the game's) redoes only what the new patches touch
    // (RefreshPatchedFiles).
    const std::vector<fs::path> patches = Patches(folders_.source);
    Command refresh;
    refresh.title = trs("Updating our fixes in the SDK (only the files that change)");
    refresh.function = [sdk, patches](std::string* message) {
      std::error_code ec;
      const fs::path scratch = fs::temp_directory_path(ec) /
                               ("crash_mom_patch_refresh_" +
                                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
      return RefreshPatchedFiles(sdk, patches, scratch, message);
    };
    out->push_back(std::move(refresh));
  } else if (status.patches_applied == 0 && status.patches_total > 0) {
    Command apply{trs("Applying our fixes to the SDK (patches/rexglue-sdk)"), {"git", "apply"}, sdk};
    for (const fs::path& patch : Patches(folders_.source)) {
      apply.argv.push_back(patch.string());
    }
    out->push_back(std::move(apply));
  }
  // Note: the SDK's install step registers it in the per-user CMake package
  // list (~/.cmake/packages/rexglue) whatever the options, where ANY folder's
  // game build could pick it up (seen: a release test built with another test
  // folder's SDK). So each game build names its own SDK instead (GameJob).
  {
    std::vector<std::string> argv = {"cmake", "--preset", kSdkPreset,
                                     "-DCMAKE_INSTALL_PREFIX=" +
                                         (folders_.source / "thirdparty" / "rexglue-install").string()};
    argv.insert(argv.end(), kSdkOptions.begin(), kSdkOptions.end());
#ifdef _WIN32
    // The checked tools by full path (FoundToolOptions, GitHub issue #16):
    // a path wins over any search, and over a wrong one cached by an
    // earlier configure (the retry after a failure).
    const std::vector<std::string> tools = FoundToolOptions();
    argv.insert(argv.end(), tools.begin(), tools.end());
#endif
    out->push_back({trs("Configuring the SDK"), argv, sdk});
  }
  // ONE build (2026-11-08): RelWithDebInfo has everything the game needs,
  // the recompiler included since SDK patch 0019. It used to be built a
  // second time in Release just for the recompiler: the whole SDK twice.
  out->push_back({trs("Building the SDK"),
                  {"cmake", "--build", std::string("out/build/") + kSdkPreset, "-j", std::to_string(SdkJobs()), "--config", "RelWithDebInfo",
                   "--target", "install"},
                  sdk, Progress::kNinja});
  // The patch set it was built with (see PatchFingerprint).
  const fs::path stamp = folders_.source / "thirdparty" / "rexglue-install" / kFingerprintFile;
  const std::string fingerprint = PatchFingerprint(Patches(folders_.source));
  Command note;
  note.title = trs("Noting which patches the SDK was built with");
  note.function = [stamp, fingerprint](std::string* message) {
    std::ofstream out(stamp, std::ios::trunc);
    out << fingerprint << "\n";
    *message = "patch set " + fingerprint;
    return bool(out);
  };
  out->push_back(std::move(note));
  return true;
}

std::vector<Command> Setup::GameJob() {
  const fs::path root = folders_.source;
  std::vector<Command> commands;
  if (folders_.release) {
    // The manifest reads the disc at <source>/game/default.xex; a release
    // keeps it at the top (<game folder>/game): a link bridges the two.
    const fs::path link = root / "game";
    const fs::path target = folders_.disc;
    Command bridge;
#if defined(_WIN32)
    // WINDOWS: a COPY of default.xex in a real source\game folder, no link.
    // The build only reads that one file (the recompiler's input; 6 MB). A
    // directory junction worked at first, but Windows' "redirection guard"
    // refuses to follow junctions a normal user made for some programs:
    // ninja failed with "The path cannot be traversed because it contains
    // an untrusted mount point" (seen 2026-11-05 on Windows 11 26200).
    bridge.title = trs("Copying default.xex for the build");
    bridge.function = [link, target](std::string* message) {
      std::error_code ec;
      const fs::path from = target / "default.xex";
      const fs::path to = link / "default.xex";
      // An old junction (an earlier version made one): remove the LINK, never
      // what it points at (MSVC's std::filesystem knows junctions as their own
      // file type; removing one removes only the link).
      const fs::file_type type = fs::symlink_status(link, ec).type();
      if (type == fs::file_type::symlink || type == fs::file_type::junction) {
        fs::remove(link, ec);
      } else if (fs::exists(to, ec) && fs::file_size(to, ec) != fs::file_size(from, ec)) {
        fs::remove(to, ec);  // another disc's xex: copy again
      }
      ec.clear();
      fs::create_directories(link, ec);
      if (!fs::exists(to, ec)) {
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
      }
      if (ec || !fs::exists(to)) {
        *message = "couldn't copy " + from.string() + " to " + to.string() + ": " + ec.message();
        return false;
      }
      fs::last_write_time(to, fs::last_write_time(from, ec), ec);  // same date: no needless recompile
      return true;
    };
#else
    bridge.title = trs("Linking source/game to the disc's files");
    bridge.function = [link, target](std::string* message) {
      std::error_code ec;
      if (fs::exists(link / "default.xex", ec)) {
        return true;  // already there
      }
      fs::remove(link, ec);
      fs::create_directory_symlink(target, link, ec);
      if (ec) {
        *message = "couldn't link " + link.string() + ": " + ec.message();
        return false;
      }
      return true;
    };
#endif
    commands.push_back(std::move(bridge));
  }
  // After the copy above (it keeps default.xex's date): nothing in the
  // source tree may be dated in the future (issue #7, NoFutureDates).
  commands.push_back(NoFutureDates(root));
  // The build kit (Kit): the release's own CMake by full path, and a FRESH
  // configure every time: nothing an earlier try cached (a compiler test
  // that failed, another CMake's choices, a tool found elsewhere) is reused.
  // Only build.ninja is written again; ninja then rebuilds only files whose
  // command changed (it compares each command with the last one it ran).
  const Kit kit = folders_.release ? KitIn(root) : Kit{};
  const std::string cmake = kit.present ? kit.cmake.string() : std::string("cmake");
  // Always configured (seconds), naming THIS folder's SDK explicitly and
  // ignoring the per-user CMake package list: a build must never use another
  // folder's SDK (see SdkJob); a cached choice is corrected too.
  Command configure{
      trs("Configuring the game's build"),
      {cmake, "--preset", kGamePreset,
       "-Drexglue_DIR=" +
           (root / "thirdparty" / "rexglue-install" / "lib" / "cmake" / "rexglue").string(),
       "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF"},
      root};
#ifdef _WIN32
  // The checked tools by full path, like the SDK's configure (FoundToolOptions,
  // GitHub issue #16): here glslc and Ninja matter.
  const std::vector<std::string> tools = FoundToolOptions();
  configure.argv.insert(configure.argv.end(), tools.begin(), tools.end());
#endif
  if (kit.present) configure.argv.push_back("--fresh");
  commands.push_back(configure);
  // First time: recompile on its own, then configure again so the build picks
  // the generated files up (docs/01-building.md, step 4).
  if (!Exists(root / "generated" / "default" / "sources.cmake")) {
    commands.push_back({trs("Recompiling the game's code (default.xex -> C++)"),
                        {cmake, "--build", "--preset", kGamePreset, "--target",
                         "crash_mom_codegen"},
                        root, Progress::kNinja});
    commands.push_back(configure);
  }
  commands.push_back({trf(trs("Compiling the game ({0} at a time; the first build takes a while)"),
                          {std::to_string(GameJobs())}),
                      {cmake, "--build", "--preset", kGamePreset, "-j",
                       std::to_string(GameJobs())},
                      root, Progress::kNinja});
  if (folders_.release) {
    // A release plays from program/: the exe, the SDK libraries next to it
    // (it finds them in its own folder) and the port's assets. Copied into a
    // fresh folder, then swapped in, so a failed copy never leaves half a game.
    const fs::path from = folders_.BuiltExe().parent_path();
    const fs::path to = folders_.root / "program";
    const std::string installed_name = folders::InstalledExeName();
    Command install;
    install.title = trs("Copying the game into program/");
    install.function = [from, to, installed_name](std::string* message) {
      std::error_code ec;
      const fs::path fresh = to.string() + ".new";
      fs::remove_all(fresh, ec);
      fs::create_directories(fresh, ec);
      int files = 0;
      for (const auto& entry : fs::directory_iterator(from, ec)) {
        const std::string name = entry.path().filename().string();
        // The exe, the SDK's libraries (.so / .dll) and, on Windows, the debug
        // symbols (.pdb: crash reports name the functions, src/crash_report.h).
        const std::string ext = entry.path().extension().string();
        // The exe goes in under its installed name (folders::InstalledExeName:
        // "Crash Mind over Mutant.exe" on Windows); its .pdb keeps the build's
        // name (the exe records that name for crash reports' function names).
        const bool is_exe = name == platform::ExeName("crash_mom");
        if (is_exe || name.find(".so") != std::string::npos || ext == ".dll" || ext == ".pdb") {
          fs::copy_file(entry.path(), fresh / (is_exe ? installed_name : name),
                        fs::copy_options::overwrite_existing, ec);
          ++files;
        }
        if (ec) break;
      }
      if (!ec) {
        fs::copy(from / "assets", fresh / "assets", fs::copy_options::recursive, ec);
      }
      // The translations of the port's texts (languages/*.po, src/language/texts.h).
      if (!ec && fs::is_directory(from / "languages")) {
        fs::copy(from / "languages", fresh / "languages", fs::copy_options::recursive, ec);
      }
      if (ec) {
        *message = "Couldn't copy the game: " + ec.message();
        return false;
      }
      fs::remove_all(to, ec);
      fs::rename(fresh, to, ec);
      if (ec) {
        *message = "Couldn't put program/ in place: " + ec.message();
        return false;
      }
      *message = std::to_string(files) + " files + assets/ in " + to.string();
      return true;
    };
    commands.push_back(std::move(install));

    // The icon, made from this player's disc (IconJob).
    auto icon = IconJob(folders_);
    commands.insert(commands.end(), icon.begin(), icon.end());
  }
  return commands;
}

// THE ICON (2026-11-05): the game's own HUD portrait of Crash as a mask,
// taken from THIS player's disc (tools/make_icon.py: the port ships no game
// art) -> program/crash_mom.ico + .png. A missing picture never fails the
// setup (exit 1 = "not on this disc": the game just has no icon). After the
// game's copy into program/ (GameJob), and by the installer for a game
// folder that was built before the icon existed.
std::vector<Command> Setup::IconJob(const folders::Folders& folders) {
  std::vector<Command> commands;
  const fs::path to = folders.root / "program";
  Command icon;
  icon.title = trs("Making the game's icon from your disc");
  icon.argv = {PythonProgram(), (folders.source / "tools" / "make_icon.py").string(),
               (folders.disc / "default.rcf").string(), to.string()};
  icon.work_folder = folders.source;
  icon.also_ok = {1};
  commands.push_back(std::move(icon));
  // Windows: the icon INTO the exe (Explorer, Start menu, Apps list, and the
  // game window's taskbar button: SDL uses the exe's first icon group).
  if (platform::ExeName("x") != "x") {
    Command embed;
    embed.title = trs("Putting the icon on the game");
    const fs::path exe = to / folders::InstalledExeName();
    embed.function = [exe, to](std::string* message) {
      std::error_code ec;
      if (!fs::is_regular_file(to / "crash_mom.ico", ec)) {
        *message = "no icon made (the game has the plain one)";
        return true;
      }
      std::string error;
      *message = platform::EmbedIcon(exe, to / "crash_mom.ico", &error) ? "icon added to " + exe.filename().string()
                                                                       : error + " (the game has the plain icon)";
      return true;  // never fails the setup
    };
    commands.push_back(std::move(embed));
  }
  return commands;
}

void Setup::StartJob(const std::string& title, std::vector<Command> commands) {
  UseBuildEnvironment(commands);
  message_.clear();
  follow_ = true;
  runner_.Start(title, std::move(commands), folders_.user / "logs" / ("setup-" + Stamp() + ".log"),
                [this](bool success) {
                  if (!success) {
                    continue_setup_ = false;  // a failed step: stop, show it
                  }
                  Check();
                });
}

// "Set up everything": whatever is left of steps 2-5, as one job. The SDK's
// patches can only be checked once its sources are there, so a run that
// downloads them ends there and the next one (started by itself:
// continue_setup_, see Draw) does the SDK and the game.
void Setup::SetUpEverything(const Status& s, const fs::path& iso) {
  std::vector<Command> commands;
  std::string error;
  bool ok = true;
  const bool source_needed = s.source_state != StepState::kDone;
  if (source_needed) {
    auto part = SourceJob();
    commands.insert(commands.end(), part.begin(), part.end());
  }
  if (s.disc_state != StepState::kDone) {
    ok = DiscJob(iso, &commands, &error);
  }
  if (ok && !source_needed && s.sdk_state != StepState::kDone) {
    ok = SdkJob(&commands, &error);
  }
  if (ok && !source_needed && s.game_state != StepState::kDone) {
    auto part = GameJob();
    commands.insert(commands.end(), part.begin(), part.end());
  }
  if (!ok) {
    message_ = error;
    continue_setup_ = false;
  } else if (commands.empty()) {
    continue_setup_ = false;
  } else {
    // Check again afterwards and go on if anything is left: the SDK after
    // its download, the game after an SDK rebuild made it out of date. A
    // round with nothing to do ends it (commands empty above); a failed
    // step too (StartJob's on_done).
    continue_setup_ = true;
    StartJob(trs("Setting up"), std::move(commands));
  }
}

int Setup::RunHeadless(const std::string& step, const fs::path& iso) {
  // Wait for the checks (SdkJob reads them).
  while (checking_) {
    SDL_Delay(50);
  }
  std::vector<Command> commands;
  std::string error;
  if (step == "check") {
    std::lock_guard lock(mutex_);
    std::printf("tools %s, source %s, disc %s, sdk %s (%s), game %s (%s)\n",
                StateText(status_.tools_state), StateText(status_.source_state),
                StateText(status_.disc_state), StateText(status_.sdk_state),
                status_.sdk_detail.c_str(), StateText(status_.game_state),
                status_.game_detail.c_str());
    for (const Tool& tool : status_.tools) {
      std::printf("  %-26s %s  [%s]\n", tool.label.c_str(), tool.detail.c_str(),
                  tool.package.c_str());
    }
    return 0;
  }
  if (step == "update") {
    if (!updater_) {
      std::printf("update: only a release can update itself\n");
      return 1;
    }
    while (updater_->checking()) {
      SDL_Delay(50);
    }
    update::Release latest;
    bool available = false;
    if (!updater_->Latest(&latest, &available, &error)) {
      std::printf("update: %s\n", error.c_str());
      return 1;
    }
    std::printf("installed %s, newest %s (%s)\n", updater_->installed().c_str(),
                latest.version.c_str(), available ? "newer: updating" : "nothing to do");
    if (!available) {
      return 0;
    }
    commands = updater_->Job(latest, launcher_);
  } else if (step == "all") {
    // Set up everything, again until nothing is left (like the button).
    for (int round = 0; round < 3; ++round) {
      Check();
      while (checking_) SDL_Delay(50);
      Status status;
      {
        std::lock_guard lock(mutex_);
        status = status_;
      }
      std::vector<Command> part;
      std::string why;
      const bool source_needed = status.source_state != StepState::kDone;
      if (source_needed) part = SourceJob();
      if (status.disc_state != StepState::kDone && !DiscJob(iso, &part, &why)) {
        std::printf("refused: %s\n", why.c_str());
        return 1;
      }
      if (!source_needed && status.sdk_state != StepState::kDone && !SdkJob(&part, &why)) {
        std::printf("refused: %s\n", why.c_str());
        return 1;
      }
      if (!source_needed && status.game_state != StepState::kDone) {
        auto game = GameJob();
        part.insert(part.end(), game.begin(), game.end());
      }
      if (part.empty()) {
        std::printf("== everything is set up\n");
        return 0;
      }
      if (RunHeadlessJob("all", std::move(part)) != 0) {
        return 1;
      }
    }
    return 0;
  } else if (step == "source") commands = SourceJob();
  else if (step == "game") commands = GameJob();
  else if (step == "sdk" && !SdkJob(&commands, &error)) {
    std::printf("refused: %s\n", error.c_str());
    return 1;
  } else if (step == "disc" && !DiscJob(iso, &commands, &error)) {
    std::printf("refused: %s\n", error.c_str());
    return 1;
  }
  if (commands.empty()) {
    std::printf("unknown step %s\n", step.c_str());
    return 1;
  }
  return RunHeadlessJob(step, std::move(commands));
}

// Windows: every step runs in Visual Studio's x64 environment (the checks
// read it, Status::build_env); Linux: the launcher's own (null).
void Setup::UseBuildEnvironment(std::vector<Command>& commands) {
  std::shared_ptr<const platform::Environment> env;
  {
    std::lock_guard lock(mutex_);
    env = status_.build_env;
  }
  for (Command& command : commands) {
    if (!command.function && !command.environment) {
      command.environment = env;
    }
    if (!command.function) command.low_priority = g_limited;  // "use fewer resources"
  }
}

void SetLimitedResources(bool limited) { g_limited = limited; }
bool LimitedResources() { return g_limited; }

// THE UPDATE'S DISC CHECK (2026-11-08): the installed game's extracted disc
// files (game\) against the player's .iso: only missing or wrong-sized files
// are copied again (tools/xiso_extract.py --repair); matching files keep
// their dates, so nothing is rebuilt because of this. Run from the NEW
// source (after the swap: the repair mode is new in 0.2.2). The extractor
// refuses a different disc (another default.xex) and changes nothing then.
Command DiscRepairCommand(const fs::path& iso, const fs::path& source, const fs::path& disc) {
  Command repair{trs("Checking the game's files against your disc"),
                 {PythonProgram(), "-u", (source / "tools" / "xiso_extract.py").string(), "--repair",
                  iso.string(), disc.string()},
                 source.parent_path()};
  return repair;
}

int Setup::RunHeadlessJob(const std::string& step, std::vector<Command> commands) {
  UseBuildEnvironment(commands);
  runner_.Start(step, std::move(commands), folders_.user / "logs" / ("setup-" + Stamp() + ".log"));
  task_runner::Snapshot view;
  size_t printed = 0;
  std::string last_detail;
  while (true) {
    if (runner_.Copy(view)) {
      for (; printed < view.lines.size(); ++printed) {
        std::printf("%s\n", view.lines[printed].c_str());
      }
      if (view.detail != last_detail && !view.detail.empty()) {
        std::printf("   progress %.0f%% (%s)\n", view.fraction * 100, view.detail.c_str());
        last_detail = view.detail;
      }
      std::fflush(stdout);
    }
    if (view.state != task_runner::State::kRunning && view.state != task_runner::State::kIdle) {
      break;
    }
    SDL_Delay(100);
  }
  return view.state == task_runner::State::kDone ? 0 : 1;
}

// -----------------------------------------------------------------------------
// The tab
// -----------------------------------------------------------------------------

namespace {

// A coloured dot for a step's state.
void StateDot(StepState state, const Look& look) {
  const ImVec4 colour = state == StepState::kDone       ? look.good
                        : state == StepState::kBlocked  ? look.bad
                        : state == StepState::kChecking ? look.muted
                                                        : look.warn;
  const float size = ImGui::GetFontSize() * 0.7f;
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float y = at.y + ImGui::GetTextLineHeight() * 0.5f;
  ImGui::GetWindowDrawList()->AddCircleFilled({at.x + size * 0.5f, y}, size * 0.5f,
                                              ImGui::GetColorU32(colour));
  ImGui::Dummy({size, ImGui::GetTextLineHeight()});
  ImGui::SameLine();
}

}  // namespace

void Setup::DrawStep(int number, const char* title, StepState state, const std::string& detail) {
  ImGui::Spacing();
  StateDot(state, *look_);
  ImGui::PushFont(look_->bold, 0.0f);
  ImGui::Text("%d. %s", number, title);
  ImGui::PopFont();
  ImGui::SameLine();
  const ImVec4 colour = state == StepState::kDone ? look_->good
                        : state == StepState::kBlocked ? look_->bad
                                                       : look_->warn;
  ImGui::TextColored(state == StepState::kChecking ? look_->muted : colour, "%s",
                     StateText(state));
  if (!detail.empty()) {
    ImGui::Indent(ImGui::GetFontSize() * 1.4f);
    ImGui::PushStyleColor(ImGuiCol_Text, look_->muted);
    ImGui::TextWrapped("%s", detail.c_str());
    ImGui::PopStyleColor();
    ImGui::Unindent(ImGui::GetFontSize() * 1.4f);
  }
}

void Setup::DrawJob() {
  runner_.Copy(job_);
  const bool running = job_.state == task_runner::State::kRunning;
  ImGui::BeginChild("job", {0, 0}, ImGuiChildFlags_Borders);
  ImGui::PushFont(look_->bold, 0.0f);
  if (running) {
    ImGui::TextColored(look_->accent, "%s", job_.title.c_str());
  } else if (job_.state == task_runner::State::kDone) {
    ImGui::TextColored(look_->good, trs("%s: done"), job_.title.c_str());
  } else if (job_.state == task_runner::State::kCancelled) {
    ImGui::TextColored(look_->warn, trs("%s: cancelled"), job_.title.c_str());
  } else {
    ImGui::TextColored(look_->bad, trs("%s: failed"), job_.title.c_str());
  }
  ImGui::PopFont();
  if (!running && !job_.error.empty() && job_.state == task_runner::State::kFailed) {
    ImGui::PushStyleColor(ImGuiCol_Text, look_->bad);
    ImGui::TextWrapped("%s", job_.error.c_str());
    ImGui::PopStyleColor();
  }
  char overlay[160];
  std::snprintf(overlay, sizeof(overlay), "%s%s%s", job_.step.c_str(),
                job_.detail.empty() ? "" : "  -  ", job_.detail.c_str());
  ImGui::ProgressBar(float(job_.fraction), {-FLT_MIN, 0}, overlay);
  if (running && job_.cancellable) {
    if (ImGui::Button(trs("Cancel"))) {
      runner_.Cancel();
    }
  } else if (running) {
    // (An in-process step, e.g. an update's swap: stopping it halfway would
    // leave a mix of two versions.)
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(look_->muted, trs("This step can't be stopped: a few seconds."));
  } else {
    if (ImGui::Button(trs("OK"))) {
      runner_.Forget();
    }
  }
  ImGui::SameLine();
  ImGui::TextColored(look_->muted, trs("Log: %s"), folders::Pretty(job_.log).c_str());
  ImGui::SameLine();
  ImGui::Checkbox(trs("Follow"), &follow_);

  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.07f, 0.07f, 0.09f, 1.0f});
  ImGui::BeginChild("output", {0, 0}, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
  ImGui::PushFont(look_->mono, ImGui::GetStyle().FontSizeBase * 0.85f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {ImGui::GetStyle().ItemSpacing.x, 2.0f});
  ImGuiListClipper clipper;
  clipper.Begin(int(job_.lines.size()));
  while (clipper.Step()) {
    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
      const std::string& line = job_.lines[i];
      const bool header = line.starts_with("== ") || line.starts_with("$ ");
      const bool bad = line.find("error:") != std::string::npos ||
                       line.find("FAILED") != std::string::npos ||
                       line.find("fatal:") != std::string::npos;
      ImGui::PushStyleColor(ImGuiCol_Text, bad      ? look_->bad
                                           : header ? look_->accent
                                                    : ImGui::GetStyleColorVec4(ImGuiCol_Text));
      ImGui::TextUnformatted(line.data(), line.data() + line.size());
      ImGui::PopStyleColor();
    }
  }
  if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0) {
    follow_ = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1 && ImGui::GetIO().MouseWheel < 0;
  }
  if (follow_) {
    ImGui::SetScrollHereY(1.0f);
  }
  ImGui::PopStyleVar();
  ImGui::PopFont();
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::EndChild();
}

void Setup::Tick(bool game_running) {
  if (!folders_.from_source || !continue_setup_ || game_running) {
    return;
  }
  Status s;
  {
    std::lock_guard lock(mutex_);
    s = status_;
  }
  runner_.Copy(job_);
  const bool busy = job_.state == task_runner::State::kRunning;
  const bool checking = checking_ || !s.done;
  // "Set up everything" goes on by itself after the SDK download, and after
  // an update's restart (--after_update), once the checks have run again.
  if (!busy && !checking && job_.state != task_runner::State::kFailed) {
    fs::path pick;
    {
      std::lock_guard lock(mutex_);
      pick = !iso_.empty() ? iso_ : (s.isos.empty() ? fs::path() : s.isos.front());
    }
    runner_.Forget();
    SetUpEverything(s, pick);
  }
}

// --- the installer's API (setup.h) -------------------------------------------

Status Setup::CurrentStatus() {
  std::lock_guard lock(mutex_);
  return status_;
}

bool Setup::StartSetUp(const fs::path& iso, std::string* error) {
  if (runner_.Running() || checking_) {
    *error = trs("Still busy: wait a moment.");
    return false;
  }
  Status s = CurrentStatus();
  if (!s.done) {
    *error = trs("Still checking: wait a moment.");
    return false;
  }
  {
    std::lock_guard lock(mutex_);
    iso_ = iso;
  }
  runner_.Forget();
  message_.clear();
  SetUpEverything(s, iso);
  if (!message_.empty() && !runner_.Running()) {
    *error = message_;
    return false;
  }
  return true;
}

// winget's answers that mean "fine" (winget's own error list,
// AppInstallerErrors.h): no newer version to install / already installed /
// installed, a restart finishes it. 3010 = an installer's "done, restart
// to finish" (Visual Studio's, through PowerShell).
static const std::vector<int> kWingetFine = {int(0x8A15002Bu), int(0x8A150061u), int(0x8A150109u),
                                             3010};

std::vector<Command> Setup::ToolsJob(const Status& status) {
  std::vector<Command> commands;
  for (const Tool& tool : status.tools) {
    if (tool.found) {
      continue;
    }
    if (!tool.install_argv.empty()) {
      Command command{trf(trs("Adding {0} (Windows asks for permission)"), {tool.label}),
                      tool.install_argv};
      command.also_ok = kWingetFine;
      commands.push_back(std::move(command));
      continue;
    }
    if (tool.winget.empty()) {
      continue;  // the graphics driver's Vulkan: nothing we can install
    }
    Command command;
    command.title = trf(trs("Installing {0} (winget)"), {tool.package});
    command.argv = {"winget", "install", "--id", tool.winget, "--exact", "--source", "winget",
                    "--accept-package-agreements", "--accept-source-agreements",
                    "--disable-interactivity"};
    if (!tool.winget_override.empty()) {
      command.argv.insert(command.argv.end(), {"--override", tool.winget_override});
    }
    command.also_ok = kWingetFine;
    commands.push_back(std::move(command));
  }
  return commands;
}

Status Setup::CheckToolsOnly(const fs::path& release_source) {
  Status s;
  // The installer checks before anything is installed: the release's own
  // source/ (next to it) says whether the build kit comes with it.
  CheckTools(&s, release_source.empty() ? Kit{} : KitIn(release_source));
  const bool ok = std::all_of(s.tools.begin(), s.tools.end(), [](const Tool& t) { return t.found; });
  s.tools_state = ok ? StepState::kDone : StepState::kMissing;
  s.done = true;
  return s;
}

bool Setup::StartToolsInstall(std::string* error) {
  if (runner_.Running() || checking_) {
    *error = trs("Still busy: wait a moment.");
    return false;
  }
#if defined(_WIN32)
  if (platform::FindProgram("winget").empty()) {
    *error = trf(trs("winget, Windows' own app installer, isn't there. Get \"App Installer\" "
                     "from Microsoft ({0}; searching the Store for it can find fake apps), "
                     "install it, then try again."),
                 {kAppInstallerUrl});
    return false;
  }
#endif
  std::vector<Command> commands = ToolsJob(CurrentStatus());
  if (commands.empty()) {
    *error = trs("Nothing here can be installed automatically.");
    return false;
  }
  runner_.Forget();
  message_.clear();
  follow_ = true;
  // The tools' own environment (not Visual Studio's: it may not exist yet);
  // the checks afterwards read PATH again (platform::RefreshEnvironment).
  runner_.Start(trs("Installing the build tools"), std::move(commands),
                folders_.user / "logs" / ("tools-" + Stamp() + ".log"),
                [this](bool) { Check(); });
  return true;
}

const task_runner::Snapshot& Setup::JobSnapshot() {
  runner_.Copy(job_);
  return job_;
}

void Setup::CancelJob() {
  continue_setup_ = false;
  runner_.Cancel();
}

bool Setup::StartUpdate(const update::Release& release, std::string* error) {
  if (!updater_ || runner_.Running() || checking_) {
    *error = trs("Still busy: wait a moment.");
    return false;
  }
  pending_ = release;
  runner_.Forget();
  message_.clear();
  runner_.Start(trf(trs("Update to {0}"), {release.version}), updater_->Job(release, launcher_),
                folders_.user / "logs" / ("update-" + Stamp() + ".log"), [this](bool success) {
                  if (success) {
                    update_applied_ = true;
                  }
                  Check();
                });
  return true;
}

bool Setup::GameNeedsBuild() {
  std::lock_guard lock(mutex_);
  return folders_.from_source && status_.done && folders_.ExeExists() &&
         status_.game_state != StepState::kDone;
}

bool Setup::Building() {
  return runner_.Running();
}

void Setup::Draw(const Look& look, bool game_running) {
  look_ = &look;
  if (!folders_.from_source) {
    ImGui::PushStyleColor(ImGuiCol_Text, look.muted);
    ImGui::TextWrapped(trs("Setup builds the game from its source code, so it works from the "
                       "project's folder (a git clone). This copy is an installed one."));
    ImGui::PopStyleColor();
    return;
  }
  Status s;
  {
    std::lock_guard lock(mutex_);
    s = status_;
  }
  runner_.Copy(job_);
  const bool busy = job_.state == task_runner::State::kRunning;
  const bool show_job = job_.state != task_runner::State::kIdle;
  const bool checking = checking_ || !s.done;

  DrawUpdates(busy || game_running);

  // Top line: where things stand + the big button.
  const bool all_done = s.done && s.tools_state == StepState::kDone &&
                        s.source_state == StepState::kDone && s.disc_state == StepState::kDone &&
                        s.sdk_state == StepState::kDone && s.game_state == StepState::kDone;
  if (checking) {
    ImGui::TextColored(look.muted, trs("Checking what's there..."));
  } else if (all_done) {
    ImGui::TextColored(look.good, trs("Everything is set up: the game is ready to play."));
  } else {
    ImGui::TextColored(look.warn, trs("Some steps are left."));
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(busy || checking);
  if (ImGui::SmallButton(trs("Check again"))) {
    Check();
  }
  ImGui::EndDisabled();

  fs::path iso;
  {
    std::lock_guard lock(mutex_);  // the file dialog's callback writes iso_
    iso = !iso_.empty() ? iso_ : (s.isos.empty() ? fs::path() : s.isos.front());
  }
  if (!all_done && !checking) {
    ImGui::SameLine();
    const bool can = s.tools_state == StepState::kDone && !busy && !game_running &&
                     (s.disc_state == StepState::kDone || !iso.empty()) &&
                     s.sdk_state != StepState::kBlocked;
    ImGui::BeginDisabled(!can);
    if (ImGui::Button(trs("Set up everything"))) {
      SetUpEverything(s, iso);
    }
    ImGui::EndDisabled();
    if (s.tools_state != StepState::kDone && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip(trs("Install the missing build tools first (step 1)."));
    }
  }
  if (game_running) {
    ImGui::TextColored(look.warn, trs("The game is running: building waits until it's closed."));
  }
  if (!message_.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, look.warn);
    ImGui::TextWrapped("%s", message_.c_str());
    ImGui::PopStyleColor();
  }

  // The steps (left), the job's output (right half when there is one).
  const float steps_width = show_job ? ImGui::GetContentRegionAvail().x * 0.45f : 0.0f;
  ImGui::BeginChild("steps", {steps_width, 0}, ImGuiChildFlags_None);

  // 1. Tools: listed only (setup.h).
  DrawStep(1, trs("Build tools"), checking ? StepState::kChecking : s.tools_state,
           s.tools_state == StepState::kDone ? trs("everything needed to build is installed") : "");
  if (!checking) {
    ImGui::Indent(ImGui::GetFontSize() * 1.4f);
    if (ImGui::BeginTable("tools", 3, ImGuiTableFlags_SizingFixedFit)) {
      for (const Tool& tool : s.tools) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(tool.found ? look.good : look.bad, "%s", tool.found ? trs("found") : trs("missing"));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(tool.label.c_str());
        ImGui::TableNextColumn();
        ImGui::TextColored(look.muted, "%s%s%s", tool.detail.c_str(),
                           !tool.found && !tool.package.empty() ? trs("  -  package: ") : "",
                           !tool.found ? tool.package.c_str() : "");
      }
      ImGui::EndTable();
    }
    if (s.tools_state != StepState::kDone) {
      ImGui::PushStyleColor(ImGuiCol_Text, look.muted);
      ImGui::TextWrapped(trs("Install the missing ones with your package manager (the package names "
                         "above are %s's), then press Check again."),
                         s.distro.c_str());
      ImGui::PopStyleColor();
    }
    ImGui::Unindent(ImGui::GetFontSize() * 1.4f);
  }

  // 2. Source parts.
  DrawStep(2, trs("Source parts"), checking ? StepState::kChecking : s.source_state, s.source_detail);
  if (!checking && s.source_state != StepState::kDone) {
    ImGui::Indent(ImGui::GetFontSize() * 1.4f);
    ImGui::BeginDisabled(busy || s.tools_state != StepState::kDone);
    if (ImGui::Button(trs("Download them"))) {
      StartJob(trs("Source parts"), SourceJob());
    }
    ImGui::EndDisabled();
    ImGui::Unindent(ImGui::GetFontSize() * 1.4f);
  }

  // 3. Disc.
  DrawStep(3, trs("Your disc"), checking ? StepState::kChecking : s.disc_state, s.disc_detail);
  if (!checking && s.disc_state != StepState::kDone) {
    ImGui::Indent(ImGui::GetFontSize() * 1.4f);
    if (!iso.empty()) {
      ImGui::TextColored(look.muted, trs("Disc image: %s"), folders::Pretty(iso).c_str());
    }
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(trs("Pick the disc image..."))) {
      static const SDL_DialogFileFilter kFilters[] = {{trs("Xbox 360 disc image"), "iso"}};
      static std::string root_text;  // SDL keeps the pointer until the dialog opens
      root_text = folders_.root.string();
      SDL_ShowOpenFileDialog(
          [](void* self, const char* const* files, int) {
            if (files && files[0]) {
              auto* setup = static_cast<Setup*>(self);
              std::lock_guard lock(setup->mutex_);
              setup->iso_ = files[0];
            }
          },
          this, nullptr, kFilters, 1, root_text.c_str(), false);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(iso.empty() || s.tools_state != StepState::kDone);
    if (ImGui::Button(trs("Extract it"))) {
      std::vector<Command> commands;
      std::string error;
      if (DiscJob(iso, &commands, &error)) {
        StartJob(trs("Disc"), std::move(commands));
      } else {
        message_ = error;
      }
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::TextColored(look.muted, trs("Your own dump of the disc (a full one is 7.8 GB); the game's "
                                   "files (6 GB) go into game/."));
    ImGui::Unindent(ImGui::GetFontSize() * 1.4f);
  }

  // 4. SDK.
  DrawStep(4, trs("ReXGlue SDK"), checking ? StepState::kChecking : s.sdk_state, s.sdk_detail);
  if (!checking && s.sdk_state != StepState::kDone && s.sdk_state != StepState::kBlocked) {
    ImGui::Indent(ImGui::GetFontSize() * 1.4f);
    ImGui::BeginDisabled(busy || game_running || s.tools_state != StepState::kDone ||
                         s.source_state != StepState::kDone);
    if (ImGui::Button(s.sdk_state == StepState::kOutdated ? trs("Rebuild the SDK") : trs("Build the SDK"))) {
      std::vector<Command> commands;
      std::string error;
      if (SdkJob(&commands, &error)) {
        StartJob(trs("ReXGlue SDK"), std::move(commands));
      } else {
        message_ = error;
      }
    }
    ImGui::EndDisabled();
    ImGui::TextColored(look.muted, trs("A few minutes."));
    ImGui::Unindent(ImGui::GetFontSize() * 1.4f);
  }

  // 5. The game.
  DrawStep(5, trs("The game"), checking ? StepState::kChecking : s.game_state, s.game_detail);
  if (!checking) {
    ImGui::Indent(ImGui::GetFontSize() * 1.4f);
    ImGui::BeginDisabled(busy || game_running || s.sdk_state != StepState::kDone ||
                         s.disc_state != StepState::kDone);
    const char* label = s.game_state == StepState::kMissing ? trs("Build the game")
                        : s.game_state == StepState::kOutdated ? trs("Rebuild (update)")
                                                               : trs("Rebuild");
    if (ImGui::Button(label)) {
      StartJob(trs("The game"), GameJob());
    }
    ImGui::EndDisabled();
    ImGui::TextColored(look.muted, trs("%d compile jobs at a time (from the RAM free now: close big "
                                   "programs first for a faster build). The first build takes a "
                                   "while; later ones only redo what changed."),
                       GameJobs());
    ImGui::Unindent(ImGui::GetFontSize() * 1.4f);
  }
  ImGui::EndChild();

  if (show_job) {
    ImGui::SameLine();
    DrawJob();
  }
}

}  // namespace setup
