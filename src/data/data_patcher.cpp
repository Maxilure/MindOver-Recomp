// =============================================================================
// data/data_patcher.cpp -- see data_patcher.h
// =============================================================================
#include "data_patcher.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>

extern "C" REX_FUNC(__imp__sub_822E3828);  // file path: (category, name, out[512])

namespace data_patcher {
namespace {

// Where the game finds the changed files (data_patcher.h): the relative
// path it opens as D:\crashmom\<file>, and the device mounted there.
constexpr char kGamePathPrefix[] = "crashmom/";
constexpr char kDeviceMount[] = "\\Device\\Harddisk0\\Partition1\\crashmom";
// The archive every patched file so far comes from.
constexpr char kArchive[] = "default.rcf";

uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
}

// Reads one file out of an RCF 2.1 archive ("ATG CORE CEMENT LIBRARY",
// big-endian; format in audio_trace.cpp): the directory (hash, offset, size;
// sorted by hash) gives each offset its size, the names table lists the
// files in data order.
bool ReadFromArchive(const std::filesystem::path& archive, std::string_view wanted, Bytes* out,
                     std::string* error) {
  std::ifstream in(archive, std::ios::binary);
  uint8_t header[0x3C];
  if (!in.read(reinterpret_cast<char*>(header), sizeof(header)) ||
      std::memcmp(header, "ATG CORE CEMENT LIBRARY", 23) != 0) {
    *error = archive.string() + " isn't a readable RCF archive";
    return false;
  }
  const uint32_t directory_offset = Be32(header + 0x24);
  const uint32_t names_offset = Be32(header + 0x2C);
  const uint32_t names_size = Be32(header + 0x30);
  const uint32_t count = Be32(header + 0x38);
  Bytes directory(size_t(count) * 12);
  in.seekg(directory_offset);
  if (!in.read(reinterpret_cast<char*>(directory.data()), directory.size())) {
    *error = "can't read the archive's directory";
    return false;
  }
  std::vector<std::pair<uint32_t, uint32_t>> files(count);  // (offset, size)
  for (uint32_t i = 0; i < count; ++i) {
    files[i] = {Be32(&directory[size_t(i) * 12 + 4]), Be32(&directory[size_t(i) * 12 + 8])};
  }
  std::sort(files.begin(), files.end());
  Bytes names(names_size);
  in.seekg(names_offset);
  if (!in.read(reinterpret_cast<char*>(names.data()), names.size())) {
    *error = "can't read the archive's names";
    return false;
  }
  size_t p = 8;
  for (uint32_t i = 0; i < count && p + 16 <= names.size(); ++i) {
    p += 12;
    const uint32_t length = Le32(&names[p]);
    p += 4;
    if (length == 0 || p + length > names.size()) {
      break;
    }
    const std::string_view name(reinterpret_cast<const char*>(&names[p]), length - 1);
    p += length + 3;
    if (name == wanted) {
      out->resize(files[i].second);
      in.seekg(files[i].first);
      if (!in.read(reinterpret_cast<char*>(out->data()), out->size())) {
        *error = "can't read " + std::string(wanted);
        return false;
      }
      return true;
    }
  }
  *error = std::string(wanted) + " isn't in " + archive.filename().string();
  return false;
}

struct Registered {
  std::string label;
  uint32_t category;
  std::string name;
  Patch patch;
};

std::vector<Registered> g_patches;  // registered before Install, read-only after
std::atomic<bool> g_installed{false};
std::filesystem::path g_folder;  // <user data>/cache/patched_data
std::mutex g_mutex;
// (category, name) -> the path to give the game ("crashmom/<file>"), or
// empty = its own file. Decided once per run.
std::map<std::pair<uint32_t, std::string>, std::string> g_redirects;
// "crashmom/<file>" -> the game's own path of that file.
std::map<std::string, std::string, std::less<>> g_originals;

bool AnyPatchFor(uint32_t category, const std::string& name) {
  return std::any_of(g_patches.begin(), g_patches.end(), [&](const Registered& r) {
    return r.category == category && (r.name.empty() || r.name == name);
  });
}

// The path the game should use for file `name` of `category`, whose own
// path is `game_path` ("package/7a8185b0.p3d"); empty = its own. The first
// time: reads the file, runs the patches, writes the result.
std::string RedirectFor(uint32_t category, const std::string& name, const std::string& game_path) {
  std::lock_guard lock(g_mutex);
  const auto key = std::make_pair(category, name);
  if (const auto it = g_redirects.find(key); it != g_redirects.end()) {
    return it->second;
  }
  std::string& redirect = g_redirects[key];
  // The archive lists names with backslashes.
  std::string archive_name = game_path;
  std::replace(archive_name.begin(), archive_name.end(), '/', '\\');
  Bytes data;
  std::string error;
  if (!ReadArchiveFile(archive_name, &data, &error)) {
    REXLOG_WARN("Data patch: {}: {}", game_path, error);
    return redirect;
  }
  const size_t original_size = data.size();
  std::string labels;
  for (const Registered& r : g_patches) {
    if (r.category != category || (!r.name.empty() && r.name != name)) continue;
    Bytes patched;
    if (r.patch(data, &patched)) {
      data = std::move(patched);
      labels += (labels.empty() ? "" : ", ") + r.label;
    }
  }
  if (labels.empty()) {
    return redirect;  // no patch wanted this one: the game's own file
  }
  const std::string file_name = std::filesystem::path(game_path).filename().string();
  const std::filesystem::path file = g_folder / file_name;
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
  out.close();
  if (!out) {
    REXLOG_WARN("Data patch: can't write {}", file.string());
    return redirect;
  }
  // The file system read the folder's file list when it was mounted, and
  // the game's open only looks names up in that list; a lookup of the full
  // path is what adds a new host file.
  if (!rex::Runtime::instance()->file_system()->ResolvePath(std::string(kDeviceMount) + "\\" +
                                                             file_name)) {
    REXLOG_WARN("Data patch: the game can't see {}", file.string());
    return redirect;
  }
  redirect = kGamePathPrefix + file_name;
  g_originals[redirect] = game_path;
  REXLOG_INFO("Data patch: {} ({} bytes) + {} = {} ({} bytes)", game_path, original_size, labels,
              redirect, data.size());
  return redirect;
}

}  // namespace

void Register(std::string label, uint32_t category, std::string name, Patch patch) {
  g_patches.push_back({std::move(label), category, std::move(name), std::move(patch)});
}

std::string OriginalPath(std::string_view path) {
  std::lock_guard lock(g_mutex);
  const auto it = g_originals.find(path);
  return it != g_originals.end() ? it->second : std::string();
}

bool ReadArchiveFile(std::string_view name, Bytes* out, std::string* error) {
  auto* runtime = rex::Runtime::instance();
  return ReadFromArchive(std::filesystem::path(runtime->game_data_root()) / kArchive, name, out,
                         error);
}

void Install() {
  auto* runtime = rex::Runtime::instance();
  auto* vfs = runtime ? runtime->file_system() : nullptr;
  if (!vfs || runtime->user_data_root().empty() || g_patches.empty()) {
    return;
  }
  // A folder of its own: the SDK keeps its shader cache in <user data>/cache,
  // which the game shouldn't see as D:\crashmom.
  const std::filesystem::path folder =
      std::filesystem::absolute(runtime->user_data_root()) / "cache" / "patched_data";
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  auto device = std::make_unique<rex::filesystem::HostPathDevice>(kDeviceMount, folder, true);
  if (!device->Initialize() || !vfs->RegisterDevice(std::move(device))) {
    REXLOG_WARN("Data patch: can't mount {}: the game gets its own files", folder.string());
    return;
  }
  g_folder = folder;
  g_installed = true;
  REXLOG_INFO("Data patch: {} patches, changed files in {}", g_patches.size(), folder.string());
}

bool Served(uint32_t category, std::string_view name) {
  std::lock_guard lock(g_mutex);
  const auto it = g_redirects.find(std::make_pair(category, std::string(name)));
  return it != g_redirects.end() && !it->second.empty();
}

}  // namespace data_patcher

// The file path (r3 = category, r4 = name, r5 = 512-byte buffer, built
// "<folder>/<name>.<extension>"): a file with a patch -> its changed copy.
extern "C" REX_FUNC(sub_822E3828) {
  using namespace data_patcher;
  const uint32_t category = ctx.r3.u32;
  const uint32_t name = ctx.r4.u32;
  const uint32_t out = ctx.r5.u32;
  __imp__sub_822E3828(ctx, base);
  if (!g_installed || !name || !out) {
    return;
  }
  const std::string name_text(reinterpret_cast<const char*>(base + name));
  if (!AnyPatchFor(category, name_text)) {
    return;
  }
  const std::string game_path(reinterpret_cast<const char*>(base + out));
  const std::string redirect = RedirectFor(category, name_text, game_path);
  if (!redirect.empty() && redirect.size() < 512) {
    std::memcpy(base + out, redirect.c_str(), redirect.size() + 1);
  }
}
