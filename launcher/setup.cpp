// =============================================================================
// setup.cpp -- see setup.h
// =============================================================================
#include "setup.h"

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

// Parallel compile jobs from the RAM that's FREE now (not the total: a
// desktop with apps open may have a third of it; going over makes Linux's
// out-of-memory killer end the whole desktop session, seen 2026-10-06), at
// most one per core. The game's generated files need 1-2 GB each
// (docs/01-building.md, step 4): 2.5 GB per job; the SDK's ~1 GB.
int JobsFor(double gb_per_job) {
  const int by_ram = int(platform::MemAvailableGb() / gb_per_job);
  return std::clamp(by_ram, 1, platform::Cores());
}
int GameJobs() { return JobsFor(2.5); }
int SdkJobs() { return JobsFor(1.0); }

// What differs per system in the build (CMakePresets.json, the SDK's own
// presets, file names).
#if defined(_WIN32)
constexpr const char* kSdkPreset = "win-amd64";
constexpr const char* kGamePreset = "win-amd64-relwithdebinfo";
constexpr const char* kPython = "python";
constexpr const char* kLibFolder = "bin";      // where the SDK installs its libraries
constexpr const char* kLibSuffix = ".dll";
// The SDK defaults to Direct3D 12 on Windows; our native renderer is Vulkan,
// and the emulated one should match Linux: Vulkan on, D3D12 off.
const std::vector<std::string> kSdkOptions = {"-DREXGLUE_USE_VULKAN=ON", "-DREXGLUE_USE_D3D12=OFF"};
#else
constexpr const char* kSdkPreset = "linux-amd64";
constexpr const char* kGamePreset = "linux-amd64-relwithdebinfo";
constexpr const char* kPython = "python3";
constexpr const char* kLibFolder = "lib";
constexpr const char* kLibSuffix = ".so";
const std::vector<std::string> kSdkOptions = {};
#endif

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
  fix.title = "Replacing link files with copies (Git for Windows)";
  fix.function = [sdk](std::string* message) {
    bool ok = false;
    std::vector<fs::path> repos = {sdk};
    const std::string subs = platform::Capture(
        {"git", "-C", sdk.string(), "submodule", "foreach", "--recursive", "--quiet",
         "echo $displaypath"},
        &ok, {}, 60000);
    std::istringstream sub_lines(subs);
    std::string line;
    while (std::getline(sub_lines, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (!line.empty()) repos.push_back(sdk / fs::path(line));
    }
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
    case StepState::kChecking: return "checking...";
    case StepState::kDone: return "done";
    case StepState::kOutdated: return "out of date";
    case StepState::kMissing: return "to do";
    case StepState::kBlocked: return "needs attention";
  }
  return "";
}


#if defined(_WIN32)

// Visual Studio's x64 build environment: what vcvars64.bat sets (compiler,
// Windows SDK and library paths), read once by running it and printing the
// environment. LLVM's and the Vulkan SDK's folders are added to PATH when
// their installers didn't (LLVM's doesn't by default).
std::shared_ptr<const platform::Environment> BuildEnvironment(const fs::path& vcvars,
                                                              const fs::path& extra_path) {
  std::error_code ec;
  const fs::path script = fs::temp_directory_path(ec) / "crash_mom_vcvars.cmd";
  {
    std::ofstream out(script, std::ios::trunc);
    out << "@call \"" << vcvars.string() << "\" >nul\r\n@set\r\n";
  }
  bool ok = false;
  const std::string text = platform::Capture({script.string()}, &ok, {}, 60000);
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

// Step 1 on Windows: each tool with its winget package name (`winget install
// <name>`; Visual Studio's parts through the Visual Studio Installer).
void CheckTools(Status* s) {
  s->distro = "Windows";
  const std::string program_files = std::getenv("ProgramFiles") ? std::getenv("ProgramFiles") : "C:\\Program Files";
  const std::string program_files_x86 =
      std::getenv("ProgramFiles(x86)") ? std::getenv("ProgramFiles(x86)") : "C:\\Program Files (x86)";
  std::string extra_path;  // folders of tools found off PATH
  auto add = [&](const char* label, const char* package, bool found, std::string detail) {
    s->tools.push_back({label, found, std::move(detail), package});
  };
  bool ok = false;
  std::string version;

  // clang: on PATH, or LLVM's default folder (its installer doesn't add it).
  fs::path clang = platform::FindProgram("clang");
  const fs::path llvm_clang = fs::path(program_files) / "LLVM" / "bin" / "clang.exe";
  if (clang.empty() && Exists(llvm_clang)) {
    clang = llvm_clang;
    extra_path += llvm_clang.parent_path().string() + ";";
  }
  if (clang.empty()) {
    add("clang 20 or newer", "LLVM.LLVM", false, "not found");
  } else {
    const int major = MajorVersion(platform::Capture({clang.string(), "--version"}, &ok), &version);
    add("clang 20 or newer", "LLVM.LLVM", major >= 20,
        major >= 20 ? version : "too old: " + version + " (20 or newer needed)");
  }
  // CMake 3.25+.
  if (platform::FindProgram("cmake").empty()) {
    add("CMake 3.25 or newer", "Kitware.CMake", false, "not found");
  } else {
    const int major = MajorVersion(platform::Capture({"cmake", "--version"}, &ok), &version);
    const bool good = major > 3 || (major == 3 && MinorVersion(version) >= 25);
    add("CMake 3.25 or newer", "Kitware.CMake", good,
        good ? version : "too old: " + version + " (3.25 or newer needed)");
  }
  const fs::path ninja = platform::FindProgram("ninja");
  add("Ninja", "Ninja-build.Ninja", !ninja.empty(), ninja.empty() ? "not found" : ninja.string());
  // Python: a real one (Windows has a "python" that only opens the Store).
  const std::string py = platform::Capture({"python", "--version"}, &ok);
  add("Python 3", "Python.Python.3.12", ok && py.find("Python 3") != std::string::npos,
      ok ? py.substr(0, py.find_first_of("\r\n")) : "not found (or only the Store shortcut)");
  const fs::path git = platform::FindProgram("git");
  add("Git", "Git.Git", !git.empty(), git.empty() ? "not found" : git.string());
  // glslc: on PATH or in the Vulkan SDK.
  fs::path glslc = platform::FindProgram("glslc");
  if (glslc.empty() && std::getenv("VULKAN_SDK")) {
    const fs::path in_sdk = fs::path(std::getenv("VULKAN_SDK")) / "Bin" / "glslc.exe";
    if (Exists(in_sdk)) {
      glslc = in_sdk;
      extra_path += in_sdk.parent_path().string() + ";";
    }
  }
  add("glslc (shader compiler)", "KhronosGroup.VulkanSDK", !glslc.empty(),
      glslc.empty() ? "not found" : glslc.string());
  // Visual Studio's C++ tools (vswhere: Microsoft's own finder).
  const fs::path vswhere =
      fs::path(program_files_x86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
  fs::path vcvars;
  if (Exists(vswhere)) {
    std::string where = platform::Capture(
        {vswhere.string(), "-latest", "-products", "*", "-requires",
         "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"},
        &ok);
    where = where.substr(0, where.find_first_of("\r\n"));
    if (!where.empty()) {
      vcvars = fs::path(where) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat";
    }
  }
  add("Visual Studio C++ tools (x64)",
      "Microsoft.VisualStudio.2022.BuildTools + the \"Desktop development with C++\" workload",
      Exists(vcvars),
      // vcvars64.bat sits in <install>\VC\Auxiliary\Build: show <install>.
      Exists(vcvars) ? vcvars.parent_path().parent_path().parent_path().parent_path().string()
                     : "not found");
  // The Windows SDK (Windows' own headers and libraries).
  std::string windows_sdk;
  std::error_code ec;
  for (const auto& entry :
       fs::directory_iterator(fs::path(program_files_x86) / "Windows Kits" / "10" / "Include", ec)) {
    if (Exists(entry.path() / "um" / "windows.h")) {
      windows_sdk = entry.path().filename().string();
    }
  }
  add("Windows 10/11 SDK", "Windows 11 SDK (Visual Studio Installer, Individual components)",
      !windows_sdk.empty(), windows_sdk.empty() ? "not found" : windows_sdk);
  const bool vulkan = platform::LibraryAvailable("", "vulkan-1.dll");
  add("Vulkan loader", "your graphics card's driver", vulkan, vulkan ? "vulkan-1.dll" : "not found");

  if (Exists(vcvars)) {
    s->build_env = BuildEnvironment(vcvars, extra_path);
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

// Step 1 on Linux: each tool with its package on this distribution.
void CheckTools(Status* s) {
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
          tool.detail = "not found (clang and clang++)";
          break;
        }
        const int major = MajorVersion(platform::Capture({"clang", "--version"}, &ok), &version);
        tool.found = major >= 20;
        tool.detail = tool.found ? version : "too old: " + version + " (20 or newer needed)";
        break;
      }
      case 1: {  // cmake 3.25+
        if (platform::FindProgram("cmake").empty()) {
          tool.detail = "not found";
          break;
        }
        const int major = MajorVersion(platform::Capture({"cmake", "--version"}, &ok), &version);
        tool.found = major > 3 || (major == 3 && MinorVersion(version) >= 25);
        tool.detail = tool.found ? version : "too old: " + version + " (3.25 or newer needed)";
        break;
      }
      case 7: {  // GTK 3 headers: pkg-config knows gtk+-3.0
        const std::string text =
            platform::Capture({"pkg-config", "--modversion", "gtk+-3.0"}, &ok);
        tool.found = ok;
        tool.detail = ok ? text.substr(0, text.find('\n')) : "not found (needs pkg-config too)";
        break;
      }
      case 8:  // the Vulkan loader library
        tool.found = platform::LibraryAvailable("libvulkan.so.1", "");
        tool.detail = tool.found ? "libvulkan.so.1" : "not found";
        break;
      default: {
        static const char* kPrograms[] = {"", "", "ninja", "python3", "git", "glslc", "pkg-config"};
        const fs::path program = platform::FindProgram(kPrograms[i]);
        tool.found = !program.empty();
        tool.detail = tool.found ? program.string() : "not found";
        break;
      }
    }
    s->tools.push_back(std::move(tool));
  }
}

#endif

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
  ImGui::TextColored(look_->muted, "Version %s", installed.empty() ? "?" : installed.c_str());
  ImGui::SameLine();
  if (!updater_) {
    ImGui::TextColored(look_->muted, " -  a developer copy: update it with git pull");
    ImGui::Separator();
    return;
  }
  if (update_applied_) {
    ImGui::TextColored(look_->good, " -  updated to %s: restart the launcher to finish",
                       pending_.version.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Restart the launcher")) {
      // The new launcher, in this process's place; it carries on with the
      // rebuild (--after_update). Returns only if that failed.
      platform::Restart(launcher_, {"--after_update"});
      message_ = "Couldn't restart: start the launcher again by hand.";
    }
    ImGui::Separator();
    return;
  }
  update::Release latest;
  bool available = false;
  std::string error;
  const bool known = updater_->Latest(&latest, &available, &error);
  if (updater_->checking()) {
    ImGui::TextColored(look_->muted, " -  checking for updates...");
  } else if (known && available) {
    ImGui::TextColored(look_->accent, " -  %s is available%s%s", latest.version.c_str(),
                       latest.date.empty() ? "" : " (", latest.date.empty() ? "" : (latest.date + ")").c_str());
    ImGui::SameLine();
    // (Not while Setup's checks run: they read the SDK folder the update moves.)
    ImGui::BeginDisabled(busy || checking_);
    if (ImGui::Button(("Update to " + latest.version).c_str())) {
      pending_ = latest;
      std::vector<Command> commands = updater_->Job(latest, launcher_);
      message_.clear();
      follow_ = true;
      runner_.Start("Update to " + latest.version, std::move(commands),
                    folders_.user / "logs" / ("update-" + Stamp() + ".log"), [this](bool success) {
                      if (success) {
                        update_applied_ = true;
                      }
                      Check();
                    });
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
      ImGui::SetTooltip("Downloads the new version and swaps it in: your saves, settings and the "
                        "game's files stay. Then the launcher restarts and rebuilds what changed.");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Release page")) {
      SDL_OpenURL(update::ReleasePage(latest.version).c_str());
    }

  } else if (known) {
    ImGui::TextColored(look_->good, " -  the newest version");
  } else if (!error.empty()) {
    ImGui::TextColored(look_->muted, " -  %s", error.c_str());
  }
  if (!updater_->checking() && !update_applied_) {
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::SmallButton("Check for updates")) {
      updater_->Check();
    }
    ImGui::EndDisabled();
  }
  if (known && available) {
    // What the update brings (update.h: WHAT'S NEW).
    if (!latest.notes.empty()) {
      ImGui::PushFont(look_->bold, 0.0f);
      ImGui::Text("What's new in %s", latest.version.c_str());
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
  CheckTools(&s);
  const bool tools_ok = std::all_of(s.tools.begin(), s.tools.end(), [](const Tool& t) { return t.found; });
  s.tools_state = tools_ok ? StepState::kDone : StepState::kMissing;

  // 2. Source parts.
  bool source_ok = Exists(sdk / "CMakeLists.txt") &&
                   Exists(sdk / "thirdparty" / "fmt" / "CMakeLists.txt") &&
                   Exists(sdk / "thirdparty" / "sdl3" / "CMakeLists.txt");
  if (source_ok) {
    // Every one of the SDK's own libraries checked out? A download stopped
    // halfway leaves some empty: `git submodule status` marks those with
    // '-' (not checked out) or '+' (another version than recorded).
    bool ok = false;
    const std::string list =
        task_runner::Capture({"git", "submodule", "status", "--recursive"}, &ok, sdk, 20000);
    std::istringstream lines(list);
    std::string line;
    while (std::getline(lines, line)) {
      if (!line.empty() && (line[0] == '-' || line[0] == '+' || line[0] == 'U')) {
        ok = false;
      }
    }
    source_ok = ok;
  }
  s.source_state = source_ok ? StepState::kDone : StepState::kMissing;
  s.source_detail = source_ok ? "the ReXGlue SDK and its libraries are there"
                              : folders_.release ? "the ReXGlue SDK isn't downloaded yet"
                                                 : "the ReXGlue SDK submodule isn't downloaded yet";
  // A release names the exact SDK version (source/RELEASE.toml): another
  // one in the folder (an update moved to a newer SDK) = download again.
  if (source_ok && folders_.release && !pin.commit.empty()) {
    bool ok = false;
    std::string head = task_runner::Capture({"git", "rev-parse", "HEAD"}, &ok, sdk);
    head = head.substr(0, head.find('\n'));
    if (!ok || head != pin.commit) {
      source_ok = false;
      s.source_state = StepState::kOutdated;
      s.source_detail = "this version needs another ReXGlue SDK (" +
                        (pin.tag.empty() ? pin.commit.substr(0, 12) : pin.tag) +
                        "): download it again";
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
    s.disc_detail = "extracted in " + folders::Pretty(folders_.disc);
  } else {
    s.disc_state = StepState::kMissing;
    s.disc_detail = s.isos.empty() ? "no disc image found yet: pick your .iso"
                                   : "ready to extract " + s.isos.front().filename().string();
  }

  // 4. SDK: patches, then the install.
  const std::vector<fs::path> patches = Patches(root);
  s.patches_total = int(patches.size());
  // The patches are a STACK (0013 builds on 0009): `git apply --check` of the
  // set fails both ways (forward on a clean tree, reverse on a patched one),
  // so CheckPatchStack compares the files themselves.
  bool all_applied = false, none_applied = false;
  if (source_ok && !patches.empty()) {
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
  const bool installed = Exists(install / "bin" / platform::ExeName("rexglue")) && Exists(runtime);
  std::string built_with;  // the fingerprint the launcher's last SDK build left
  {
    std::ifstream in(install / kFingerprintFile);
    std::getline(in, built_with);
  }
  if (!source_ok) {
    s.sdk_state = StepState::kMissing;
    s.sdk_detail = "needs the source parts first";
  } else if (!patches.empty() && !all_applied && !none_applied && folders_.release) {
    // A release's SDK folder is the launcher's own download: patched by an
    // older patch set (an update brought new ones). Back to clean, then the
    // new set (SdkJob).
    s.sdk_state = StepState::kOutdated;
    s.sdk_needs_reset = true;
    s.sdk_detail = "the patches changed (an update): it will be reset and patched again, then "
                   "rebuilt";
  } else if (!patches.empty() && !all_applied && !none_applied) {
    s.sdk_state = StepState::kBlocked;
    s.sdk_detail = "our " + std::to_string(s.patches_total) +
                   " SDK patches are neither all applied nor all missing: the SDK folder was "
                   "changed by hand (or a patch was updated). Not touched; see "
                   "docs/01-building.md, step 2 (git -C thirdparty/rexglue-sdk status).";
  } else if (!installed) {
    s.sdk_state = StepState::kMissing;
    s.sdk_detail = s.patches_applied ? "patched, not built yet" : "not patched or built yet";
  } else if (s.patches_applied < s.patches_total ||
             (!built_with.empty() && built_with != PatchFingerprint(patches))) {
    s.sdk_state = StepState::kOutdated;
    s.sdk_detail = s.patches_applied < s.patches_total
                       ? "built without our patches: rebuild it"
                       : "the patches changed since the SDK was built: rebuild it";
  } else {
    s.sdk_state = StepState::kDone;
    s.sdk_detail = "built and installed, " + std::to_string(s.patches_total) + " patches";
  }

  // 5. The game (a release then plays a copy in program/).
  const fs::path built = folders_.BuiltExe();
  if (!Exists(built)) {
    s.game_state = StepState::kMissing;
    s.game_detail = "not built yet";
  } else if (folders_.release && TimeOf(folders_.exe) < TimeOf(built)) {
    s.game_state = StepState::kOutdated;
    s.game_detail = "built, not copied into program/ yet";
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
      s.game_detail = "the sources changed since the last build: rebuild it";
    } else {
      s.game_state = StepState::kDone;
      s.game_detail = "built: " + folders::Pretty(folders_.exe);
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
    commands.push_back({"Downloading the source parts (git submodules)", argv, folders_.source});
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
  if (head_ok && head == pin.commit) {
    std::vector<std::string> argv = {"git"};
    argv.insert(argv.end(), kGitOptions.begin(), kGitOptions.end());
    argv.insert(argv.end(), {"submodule", "update", "--init", "--recursive", "--depth", "1"});
    commands.push_back({"Finishing the SDK's libraries (a download was stopped)", argv, sdk});
    AddLinkFix(sdk, &commands);
    return commands;
  }
  Command clear;
  clear.title = "Removing the old SDK download";
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
    argv.insert(argv.end(), {"clone", "--depth", "1", "--branch", pin.tag, "--recurse-submodules",
                             "--shallow-submodules", pin.url, sdk.string()});
    commands.push_back({"Downloading the ReXGlue SDK " + pin.tag + " (and its libraries)", argv,
                        folders_.source});
  }
  AddLinkFix(sdk, &commands);
  Command verify;
  verify.title = "Checking the SDK's version";
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
    *error = "Pick your disc image (.iso) first.";
    return false;
  }
  const double free_gb = platform::FreeDiskGb(folders_.root);
  if (free_gb >= 0 && free_gb < 7) {
    char text[160];
    std::snprintf(text, sizeof(text),
                  "Not enough free space: the game's files need about 6.5 GB, %.1f GB are free.",
                  free_gb);
    *error = text;
    return false;
  }
  // The extractor's --list: is it a 360 disc with the game, and how many files.
  bool ok = false;
  const std::string listing = task_runner::Capture(
      {kPython, (folders_.source / "tools" / "xiso_extract.py").string(), "--list", iso.string()},
      &ok, folders_.root, 60000);
  std::smatch match;
  static const std::regex count(R"(\[xiso\] (\d+) files)");
  if (!ok || !std::regex_search(listing, match, count)) {
    *error = "That file isn't an Xbox 360 disc image the extractor can read.";
    return false;
  }
  if (listing.find(" default.xex") == std::string::npos) {
    *error = "That disc image has no default.xex: not a game disc?";
    return false;
  }
  Command extract{"Extracting the disc into game/",
                  {kPython, "-u", (folders_.source / "tools" / "xiso_extract.py").string(),
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
  verify.title = "Checking the game's executable";
  verify.function = [xex, root](std::string* message) {
    bool ok = false;
    const std::string info = task_runner::Capture(
        {kPython, (root / "tools" / "xex_info.py").string(), xex.string()}, &ok, root);
    if (info.find("565507FA") == std::string::npos) {
      *message = "default.xex isn't Crash: Mind over Mutant (title id 565507FA not found). "
                 "The files are in game/; this port only runs that game.";
      return false;
    }
    *message = "Title id 565507FA: Crash: Mind over Mutant.";
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
  if (status.sdk_needs_reset) {
    // A release's own SDK download, patched by an older set (RunChecks):
    // every tracked file back to the downloaded version. Untracked build
    // output (out/, ignored) stays, so the rebuild is incremental.
    out->push_back({"Undoing the old patches", {"git", "checkout", "--", "."}, sdk});
    out->push_back({"Removing files the old patches added", {"git", "clean", "-fd"}, sdk});
  }
  if ((status.patches_applied == 0 || status.sdk_needs_reset) && status.patches_total > 0) {
    Command apply{"Applying our fixes to the SDK (patches/rexglue-sdk)", {"git", "apply"}, sdk};
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
    out->push_back({"Configuring the SDK", argv, sdk});
  }
  out->push_back({"Building the SDK (Release: the recompiler)",
                  {"cmake", "--build", std::string("out/build/") + kSdkPreset, "-j", std::to_string(SdkJobs()), "--config", "Release", "--target",
                   "install"},
                  sdk, Progress::kNinja});
  out->push_back({"Building the SDK (RelWithDebInfo: the runtime the game uses)",
                  {"cmake", "--build", std::string("out/build/") + kSdkPreset, "-j", std::to_string(SdkJobs()), "--config", "RelWithDebInfo",
                   "--target", "install"},
                  sdk, Progress::kNinja});
  // The patch set it was built with (see PatchFingerprint).
  const fs::path stamp = folders_.source / "thirdparty" / "rexglue-install" / kFingerprintFile;
  const std::string fingerprint = PatchFingerprint(Patches(folders_.source));
  Command note;
  note.title = "Noting which patches the SDK was built with";
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
    bridge.title = "Linking source/game to the disc's files";
    bridge.function = [link, target](std::string* message) {
      std::error_code ec;
      if (fs::exists(link / "default.xex", ec)) {
        return true;  // already there
      }
      fs::remove(link, ec);
#if defined(_WIN32)
      // A directory JUNCTION (needs no admin rights, unlike a symbolic link).
      bool ok = false;
      const std::string out = platform::Capture(
          {"cmd", "/d", "/c", "mklink", "/J", link.string(), target.string()}, &ok);
      if (!ok) {
        *message = "couldn't link " + link.string() + ": " + out;
        return false;
      }
#else
      fs::create_directory_symlink(target, link, ec);
      if (ec) {
        *message = "couldn't link " + link.string() + ": " + ec.message();
        return false;
      }
#endif
      return true;
    };
    commands.push_back(std::move(bridge));
  }
  // Always configured (seconds), naming THIS folder's SDK explicitly and
  // ignoring the per-user CMake package list: a build must never use another
  // folder's SDK (see SdkJob); a cached choice is corrected too.
  const Command configure{
      "Configuring the game's build",
      {"cmake", "--preset", kGamePreset,
       "-Drexglue_DIR=" +
           (root / "thirdparty" / "rexglue-install" / "lib" / "cmake" / "rexglue").string(),
       "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF"},
      root};
  commands.push_back(configure);
  // First time: recompile on its own, then configure again so the build picks
  // the generated files up (docs/01-building.md, step 4).
  if (!Exists(root / "generated" / "default" / "sources.cmake")) {
    commands.push_back({"Recompiling the game's code (default.xex -> C++)",
                        {"cmake", "--build", "--preset", kGamePreset, "--target",
                         "crash_mom_codegen"},
                        root, Progress::kNinja});
    commands.push_back(configure);
  }
  commands.push_back({"Compiling the game (" + std::to_string(GameJobs()) +
                          " at a time; the first build takes a while)",
                      {"cmake", "--build", "--preset", kGamePreset, "-j",
                       std::to_string(GameJobs())},
                      root, Progress::kNinja});
  if (folders_.release) {
    // A release plays from program/: the exe, the SDK libraries next to it
    // (it finds them in its own folder) and the port's assets. Copied into a
    // fresh folder, then swapped in, so a failed copy never leaves half a game.
    const fs::path from = folders_.BuiltExe().parent_path();
    const fs::path to = folders_.root / "program";
    Command install;
    install.title = "Copying the game into program/";
    install.function = [from, to](std::string* message) {
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
        if (name == platform::ExeName("crash_mom") || name.find(".so") != std::string::npos ||
            ext == ".dll" || ext == ".pdb") {
          fs::copy_file(entry.path(), fresh / name, fs::copy_options::overwrite_existing, ec);
          ++files;
        }
        if (ec) break;
      }
      if (!ec) {
        fs::copy(from / "assets", fresh / "assets", fs::copy_options::recursive, ec);
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
    StartJob("Setting up", std::move(commands));
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
  }
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
    ImGui::TextColored(look_->good, "%s: done", job_.title.c_str());
  } else if (job_.state == task_runner::State::kCancelled) {
    ImGui::TextColored(look_->warn, "%s: cancelled", job_.title.c_str());
  } else {
    ImGui::TextColored(look_->bad, "%s: failed", job_.title.c_str());
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
    if (ImGui::Button("Cancel")) {
      runner_.Cancel();
    }
  } else if (running) {
    // (An in-process step, e.g. an update's swap: stopping it halfway would
    // leave a mix of two versions.)
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(look_->muted, "This step can't be stopped: a few seconds.");
  } else {
    if (ImGui::Button("OK")) {
      runner_.Forget();
    }
  }
  ImGui::SameLine();
  ImGui::TextColored(look_->muted, "Log: %s", folders::Pretty(job_.log).c_str());
  ImGui::SameLine();
  ImGui::Checkbox("Follow", &follow_);

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
    ImGui::TextWrapped("Setup builds the game from its source code, so it works from the "
                       "project's folder (a git clone). This copy is an installed one.");
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
    ImGui::TextColored(look.muted, "Checking what's there...");
  } else if (all_done) {
    ImGui::TextColored(look.good, "Everything is set up: the game is ready to play.");
  } else {
    ImGui::TextColored(look.warn, "Some steps are left.");
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(busy || checking);
  if (ImGui::SmallButton("Check again")) {
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
    if (ImGui::Button("Set up everything")) {
      SetUpEverything(s, iso);
    }
    ImGui::EndDisabled();
    if (s.tools_state != StepState::kDone && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip("Install the missing build tools first (step 1).");
    }
  }
  if (game_running) {
    ImGui::TextColored(look.warn, "The game is running: building waits until it's closed.");
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
  DrawStep(1, "Build tools", checking ? StepState::kChecking : s.tools_state,
           s.tools_state == StepState::kDone ? "everything needed to build is installed" : "");
  if (!checking) {
    ImGui::Indent(ImGui::GetFontSize() * 1.4f);
    if (ImGui::BeginTable("tools", 3, ImGuiTableFlags_SizingFixedFit)) {
      for (const Tool& tool : s.tools) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(tool.found ? look.good : look.bad, "%s", tool.found ? "found" : "missing");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(tool.label.c_str());
        ImGui::TableNextColumn();
        ImGui::TextColored(look.muted, "%s%s%s", tool.detail.c_str(),
                           !tool.found && !tool.package.empty() ? "  -  package: " : "",
                           !tool.found ? tool.package.c_str() : "");
      }
      ImGui::EndTable();
    }
    if (s.tools_state != StepState::kDone) {
      ImGui::PushStyleColor(ImGuiCol_Text, look.muted);
      ImGui::TextWrapped("Install the missing ones with your package manager (the package names "
                         "above are %s's), then press Check again.",
                         s.distro.c_str());
      ImGui::PopStyleColor();
    }
    ImGui::Unindent(ImGui::GetFontSize() * 1.4f);
  }

  // 2. Source parts.
  DrawStep(2, "Source parts", checking ? StepState::kChecking : s.source_state, s.source_detail);
  if (!checking && s.source_state != StepState::kDone) {
    ImGui::Indent(ImGui::GetFontSize() * 1.4f);
    ImGui::BeginDisabled(busy || s.tools_state != StepState::kDone);
    if (ImGui::Button("Download them")) {
      StartJob("Source parts", SourceJob());
    }
    ImGui::EndDisabled();
    ImGui::Unindent(ImGui::GetFontSize() * 1.4f);
  }

  // 3. Disc.
  DrawStep(3, "Your disc", checking ? StepState::kChecking : s.disc_state, s.disc_detail);
  if (!checking && s.disc_state != StepState::kDone) {
    ImGui::Indent(ImGui::GetFontSize() * 1.4f);
    if (!iso.empty()) {
      ImGui::TextColored(look.muted, "Disc image: %s", folders::Pretty(iso).c_str());
    }
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Pick the disc image...")) {
      static const SDL_DialogFileFilter kFilters[] = {{"Xbox 360 disc image", "iso"}};
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
    if (ImGui::Button("Extract it")) {
      std::vector<Command> commands;
      std::string error;
      if (DiscJob(iso, &commands, &error)) {
        StartJob("Disc", std::move(commands));
      } else {
        message_ = error;
      }
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::TextColored(look.muted, "Your own dump of the disc (a full one is 7.8 GB); the game's "
                                   "files (6 GB) go into game/.");
    ImGui::Unindent(ImGui::GetFontSize() * 1.4f);
  }

  // 4. SDK.
  DrawStep(4, "ReXGlue SDK", checking ? StepState::kChecking : s.sdk_state, s.sdk_detail);
  if (!checking && s.sdk_state != StepState::kDone && s.sdk_state != StepState::kBlocked) {
    ImGui::Indent(ImGui::GetFontSize() * 1.4f);
    ImGui::BeginDisabled(busy || game_running || s.tools_state != StepState::kDone ||
                         s.source_state != StepState::kDone);
    if (ImGui::Button(s.sdk_state == StepState::kOutdated ? "Rebuild the SDK" : "Build the SDK")) {
      std::vector<Command> commands;
      std::string error;
      if (SdkJob(&commands, &error)) {
        StartJob("ReXGlue SDK", std::move(commands));
      } else {
        message_ = error;
      }
    }
    ImGui::EndDisabled();
    ImGui::TextColored(look.muted, "A few minutes.");
    ImGui::Unindent(ImGui::GetFontSize() * 1.4f);
  }

  // 5. The game.
  DrawStep(5, "The game", checking ? StepState::kChecking : s.game_state, s.game_detail);
  if (!checking) {
    ImGui::Indent(ImGui::GetFontSize() * 1.4f);
    ImGui::BeginDisabled(busy || game_running || s.sdk_state != StepState::kDone ||
                         s.disc_state != StepState::kDone);
    const char* label = s.game_state == StepState::kMissing ? "Build the game"
                        : s.game_state == StepState::kOutdated ? "Rebuild (update)"
                                                               : "Rebuild";
    if (ImGui::Button(label)) {
      StartJob("The game", GameJob());
    }
    ImGui::EndDisabled();
    ImGui::TextColored(look.muted, "%d compile jobs at a time (from the RAM free now: close big "
                                   "programs first for a faster build). The first build takes a "
                                   "while; later ones only redo what changed.",
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
