// =============================================================================
// saves/pc_save_drive.cpp -- see pc_save_drive.h
// =============================================================================
// Each override below says what the ORIGINAL method did (findings/32) and how
// ours does the same with a plain file. Argument registers are the original's.
#include "pc_save_drive.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../guest_memory.h"
#include "save_files.h"

namespace {

namespace fs = std::filesystem;

// --- Guest memory (big-endian) ----------------------------------------------

uint32_t Load32(uint8_t* base, uint32_t address) {
  const uint8_t* p = GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  uint8_t* p = GuestPtr(base, address);
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}
// A C string the game passed (at most `max` bytes).
std::string GuestString(uint8_t* base, uint32_t address, size_t max = 256) {
  std::string s;
  for (size_t i = 0; i < max; ++i) {
    const char c = char(*GuestPtr(base, address + uint32_t(i)));
    if (!c) break;
    s += c;
  }
  return s;
}

// --- The drive object's fields that Radical's other code reads ---------------
// (XenonSaveDrive, findings/32; offsets from the drive's start)
constexpr uint32_t kMediaError = 124;     // media info: error (Radical number)
constexpr uint32_t kMediaFreeBytes = 128; // free space, bytes (u32, clamped)
constexpr uint32_t kMediaFreeBlocks = 132;
constexpr uint32_t kMediaBlockSize = 136; // 2048 on the Xbox 360
constexpr uint32_t kMediaName = 140;      // volume name, 65 bytes
constexpr uint32_t kLastError = 208;      // the last method's error (Radical number)
constexpr uint32_t kBlockSize = 2048;

// Radical's error numbers (the Win32 -> Radical table at 0x82368958).
enum RadError : uint32_t {
  kNone = 0,
  kNotFound = 1,
  kAlreadyExists = 6,
  kNoSpace = 7,
  kFailure = 8,
  kCorrupt = 12,
};

// Open's mode (r5): how the original maps it onto CreateFile / content
// creation (0x823691E4): 0 = open an existing save, 1 = open or create,
// 3 = create (replace), 2 = create new (never seen; treated like the Win32
// CREATE_NEW it most likely was).
enum OpenMode : uint32_t { kOpenExisting = 0, kOpenAlways = 1, kCreateNew = 2, kCreateAlways = 3 };

// --- Our state: open files and finds (the original kept 8 + 4 in the drive) --

struct OpenFile {
  bool used = false;
  std::string name;            // the game's name ("CrashMOM GameSlot 4")
  fs::path path;               // user/saves/<name>.sav
  std::vector<uint8_t> data;   // the whole file, size header included
  uint32_t total = 0;          // the size the drive recorded at Open (header value at Commit)
  bool created = false;        // made by this Open: Commit writes the size header
  bool writable = false;
  bool dirty = false;          // written since the last flush to disk
};
struct Find {
  bool used = false;
  std::string pattern;
  size_t next = 0;
};

std::array<OpenFile, 8> g_files;  // the original's table had 8 entries too
std::array<Find, 4> g_finds;      // ... and 4 finds

void SetError(uint8_t* base, uint32_t drive, uint32_t error) {
  Store32(base, drive + kLastError, error);
}

// The original strips one leading backslash from names (0x823688B0 builds
// "s0:\" + name).
std::string CleanName(std::string name) {
  if (!name.empty() && name[0] == '\\') name.erase(0, 1);
  return name;
}

// Only names the game itself makes ("CrashMOM GameSlot N") or anything
// without path characters: never a path out of the saves folder.
bool SafeName(const std::string& name) {
  return !name.empty() && name.size() < 128 && name.find_first_of("/\\:") == std::string::npos &&
         name != "." && name != "..";
}

// Writes the file to disk (temp + rename). False on failure.
bool Flush(OpenFile& f) {
  if (!f.dirty) return true;
  std::string error;
  if (!save_files::ReplaceFile(f.path, f.data, &error)) {
    REXLOG_ERROR("PC save drive: COULD NOT WRITE {}: {}", f.path.string(), error);
    return false;
  }
  f.dirty = false;
  REXLOG_INFO("PC save drive: wrote {} ({} bytes)", f.path.filename().string(), f.data.size());
  return true;
}

// Case-insensitive * and ? match (the original used Radical's matcher,
// sub_82357EC8, on the content file names).
bool Matches(std::string_view name, std::string_view pattern) {
  if (pattern.empty()) return name.empty();
  if (pattern[0] == '*') {
    for (size_t i = 0; i <= name.size(); ++i) {
      if (Matches(name.substr(i), pattern.substr(1))) return true;
    }
    return false;
  }
  if (name.empty()) return false;
  if (pattern[0] != '?' && std::tolower(uint8_t(pattern[0])) != std::tolower(uint8_t(name[0]))) {
    return false;
  }
  return Matches(name.substr(1), pattern.substr(1));
}

}  // namespace

// -----------------------------------------------------------------------------
// slot 9: CheckMedia. ORIGINAL: re-checked the storage device (notifications,
// device data, content enumeration) and filled the media info (+124 error,
// +128 free bytes, +132 free blocks, +136 block size, +140 the device's name).
// OURS: the saves folder is always there; free space = the PC drive's.
// -----------------------------------------------------------------------------
extern "C" REX_FUNC(sub_82369040) {
  const uint32_t drive = ctx.r3.u32;
  std::lock_guard guard(save_files::Lock());
  std::error_code ec;
  const fs::path folder = save_files::SavesFolder();
  fs::create_directories(folder, ec);
  const fs::space_info space = fs::space(folder, ec);
  const uint64_t free_bytes = ec ? 0 : uint64_t(space.available);
  Store32(base, drive + kMediaError, kNone);
  Store32(base, drive + kMediaFreeBytes, uint32_t(std::min<uint64_t>(free_bytes, 0xFFFFFFFFu)));
  Store32(base, drive + kMediaFreeBlocks,
          uint32_t(std::min<uint64_t>(free_bytes / kBlockSize, 0xFFFFFFFFu)));
  Store32(base, drive + kMediaBlockSize, kBlockSize);
  constexpr char kName[] = "Hard Drive";  // what the Xbox called its built-in drive
  uint8_t* name = GuestPtr(base, drive + kMediaName);
  std::memset(name, 0, 65);
  std::memcpy(name, kName, sizeof(kName));
  SetError(base, drive, kNone);
  ctx.r3.u64 = 1;
}

// -----------------------------------------------------------------------------
// slot 11: Open(r4 name, r5 mode, r6 write access, r7 metadata (display name +
// thumbnail: Xbox dashboard only), r8 size for a new file, r9 -> slot out,
// r10 -> size out). ORIGINAL (0x823690D8): looked the name up in the device's
// content list, created/opened the content package ("s<slot>:"), then the file
// inside; an existing file's u32 header must equal the file's size (else
// error 12, "corrupt"); a new file is size+4 bytes long with header 0 until
// Commit. Out: slot, file size - 4. OURS: the same on user/saves/<name>.sav.
// -----------------------------------------------------------------------------
extern "C" REX_FUNC(sub_823690D8) {
  const uint32_t drive = ctx.r3.u32;
  const std::string name = CleanName(GuestString(base, ctx.r4.u32));
  const uint32_t mode = ctx.r5.u32;
  const bool write = (ctx.r6.u32 & 0xFF) != 0;
  const uint32_t new_size = ctx.r8.u32;
  const uint32_t slot_out = ctx.r9.u32, size_out = ctx.r10.u32;
  ctx.r3.u64 = 0;
  std::lock_guard guard(save_files::Lock());

  if (!SafeName(name)) {
    REXLOG_ERROR("PC save drive: refused to open \"{}\" (not a save name)", name);
    SetError(base, drive, kFailure);
    return;
  }
  auto slot = std::find_if(g_files.begin(), g_files.end(), [](const OpenFile& f) { return !f.used; });
  if (slot == g_files.end()) {
    REXLOG_ERROR("PC save drive: no free file slot for {}", name);
    SetError(base, drive, kFailure);
    return;
  }
  const fs::path path = save_files::FileOf(name);
  std::error_code ec;
  const bool exists = fs::is_regular_file(path, ec);

  OpenFile f;
  f.name = name;
  f.path = path;
  f.writable = write;
  if (exists && (mode == kOpenExisting || mode == kOpenAlways)) {
    // An existing save: the whole file, its header checked like the original.
    std::ifstream in(path, std::ios::binary);
    f.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    const uint32_t header = f.data.size() >= 4 ? uint32_t(f.data[0]) << 24 | uint32_t(f.data[1]) << 16 |
                                                     uint32_t(f.data[2]) << 8 | f.data[3]
                                               : 0;
    if (!in.eof() && in.fail()) {
      REXLOG_ERROR("PC save drive: can't read {}", path.string());
      SetError(base, drive, kFailure);
      return;
    }
    if (f.data.size() < 4 || header != f.data.size()) {
      // The game shows it as corrupt. The file itself is never touched.
      REXLOG_WARN("PC save drive: {} has size header {} but is {} bytes: reported as corrupt",
                  path.filename().string(), header, f.data.size());
      SetError(base, drive, kCorrupt);
      return;
    }
    f.total = uint32_t(f.data.size());
  } else if (!exists && mode == kOpenExisting) {
    SetError(base, drive, kNotFound);  // the scan of an empty slot: normal
    return;
  } else if (exists && mode == kCreateNew) {
    SetError(base, drive, kAlreadyExists);
    return;
  } else {
    // A new save (or a replaced one: the old file goes to Backups first).
    if (exists) {
      fs::path moved;
      std::string error;
      if (!save_files::MoveToBackups(path, &moved, &error)) {
        REXLOG_ERROR("PC save drive: can't set {} aside before replacing it: {}", path.string(), error);
        SetError(base, drive, kFailure);
        return;
      }
      REXLOG_INFO("PC save drive: {} replaced; the old one is {}", path.filename().string(),
                  moved.string());
    }
    f.data.assign(size_t(new_size) + 4, 0);  // header 0 until Commit, like the original
    f.total = new_size + 4;
    f.created = true;
    f.dirty = true;
  }
  f.used = true;
  *slot = std::move(f);
  const uint32_t index = uint32_t(slot - g_files.begin());
  Store32(base, slot_out, index);
  Store32(base, size_out, slot->total - 4);
  SetError(base, drive, kNone);
  ctx.r3.u64 = 1;
  REXLOG_DEBUG("PC save drive: open {} mode {} {} -> slot {}, {} bytes", name, mode,
               write ? "write" : "read", index, slot->total - 4);
}

// slot 12: Close(r4 slot). ORIGINAL: closed the file and the content package.
// OURS: what was written reaches the disk (the original's writes went
// straight to the file, so a Close without Commit still kept them).
extern "C" REX_FUNC(sub_82368B98) {
  const uint32_t index = ctx.r4.u32;
  std::lock_guard guard(save_files::Lock());
  if (index < g_files.size() && g_files[index].used) {
    Flush(g_files[index]);
    g_files[index] = OpenFile{};
  }
  ctx.r3.u64 = 1;  // the original always reported success
}

// slot 13: Commit(r4 slot). ORIGINAL (0x82369558): a file made by this Open
// gets its u32 size header (the size recorded at Open), then the content
// package is flushed. OURS: the same header, then the file goes to disk.
extern "C" REX_FUNC(sub_82369558) {
  const uint32_t drive = ctx.r3.u32;
  const uint32_t index = ctx.r4.u32;
  std::lock_guard guard(save_files::Lock());
  ctx.r3.u64 = 0;
  if (index >= g_files.size() || !g_files[index].used) {
    SetError(base, drive, kFailure);
    return;
  }
  OpenFile& f = g_files[index];
  if (f.created) {
    const uint32_t t = f.total;
    f.data[0] = uint8_t(t >> 24);
    f.data[1] = uint8_t(t >> 16);
    f.data[2] = uint8_t(t >> 8);
    f.data[3] = uint8_t(t);
    f.dirty = true;
  }
  if (!Flush(f)) {
    SetError(base, drive, kNoSpace);  // the likeliest reason; the game says "couldn't save"
    return;
  }
  ctx.r3.u64 = 1;
}

// slot 14: Read(r4 slot, r7 position, r8 buffer, r9 count, r10 -> bytes read).
// ORIGINAL (0x82369678): positions count after the 4-byte header; at or past
// the end = success with nothing read (and r10 left alone).
extern "C" REX_FUNC(sub_82369678) {
  const uint32_t drive = ctx.r3.u32;
  const uint32_t index = ctx.r4.u32;
  const uint32_t position = ctx.r7.u32, buffer = ctx.r8.u32, count = ctx.r9.u32;
  const uint32_t read_out = ctx.r10.u32;
  std::lock_guard guard(save_files::Lock());
  ctx.r3.u64 = 0;
  if (index >= g_files.size() || !g_files[index].used) {
    SetError(base, drive, kFailure);
    return;
  }
  const OpenFile& f = g_files[index];
  const uint64_t from = uint64_t(position) + 4;
  SetError(base, drive, kNone);
  ctx.r3.u64 = 1;
  if (from >= f.data.size()) {
    return;
  }
  const uint32_t n = uint32_t(std::min<uint64_t>(count, f.data.size() - from));
  std::memcpy(GuestPtr(base, buffer), f.data.data() + from, n);
  Store32(base, read_out, n);
}

// slot 15: Write(r4 slot, r7 position, r8 buffer, r9 count, r10 -> bytes
// written, 9th argument (caller's stack +84) -> the file's size). ORIGINAL
// (0x82369790): writes after the header; reports the size recorded at Open
// minus 4 (not updated by the write).
extern "C" REX_FUNC(sub_82369790) {
  const uint32_t drive = ctx.r3.u32;
  const uint32_t index = ctx.r4.u32;
  const uint32_t position = ctx.r7.u32, buffer = ctx.r8.u32, count = ctx.r9.u32;
  const uint32_t written_out = ctx.r10.u32;
  const uint32_t size_out = Load32(base, ctx.r1.u32 + 84);
  std::lock_guard guard(save_files::Lock());
  ctx.r3.u64 = 0;
  if (index >= g_files.size() || !g_files[index].used || !g_files[index].writable) {
    SetError(base, drive, kFailure);
    return;
  }
  OpenFile& f = g_files[index];
  const size_t from = size_t(position) + 4;
  if (from + count > f.data.size()) f.data.resize(from + count, 0);
  std::memcpy(f.data.data() + from, GuestPtr(base, buffer), count);
  f.dirty = true;
  Store32(base, written_out, count);
  Store32(base, size_out, f.total - 4);
  SetError(base, drive, kNone);
  ctx.r3.u64 = 1;
}

// slot 19: Delete(r4 name). ORIGINAL (0x823699D0): XamContentDelete of the
// content with that file name (not found = error 1). The save manager deletes
// a slot's save before writing a new one into it. OURS: the file moves to
// user/saves/Backups/ (save_files::MoveToBackups): nothing is really lost.
extern "C" REX_FUNC(sub_823699D0) {
  const uint32_t drive = ctx.r3.u32;
  const std::string name = CleanName(GuestString(base, ctx.r4.u32));
  std::lock_guard guard(save_files::Lock());
  ctx.r3.u64 = 0;
  const fs::path path = save_files::FileOf(name);
  std::error_code ec;
  if (!SafeName(name) || !fs::is_regular_file(path, ec)) {
    SetError(base, drive, kNotFound);
    return;
  }
  for (OpenFile& f : g_files) {  // never seen; the original would fail too
    if (f.used && f.path == path) {
      SetError(base, drive, kFailure);
      return;
    }
  }
  fs::path moved;
  std::string error;
  if (!save_files::MoveToBackups(path, &moved, &error)) {
    REXLOG_ERROR("PC save drive: can't set {} aside: {}", path.string(), error);
    SetError(base, drive, kFailure);
    return;
  }
  REXLOG_INFO("PC save drive: the game deleted {}; kept as {}", name, moved.string());
  SetError(base, drive, kNone);
  ctx.r3.u64 = 1;
}

// slot 21: FindNext(r4 -> find number, r5 entry). ORIGINAL (0x82369890):
// entry +0 = 2 (nothing more) or 3 (a file), +4 its name (257 bytes); walks
// the device's content list with a wildcard match.
extern "C" REX_FUNC(sub_82369890) {
  const uint32_t drive = ctx.r3.u32;
  const uint32_t index = Load32(base, ctx.r4.u32);
  const uint32_t entry = ctx.r5.u32;
  std::lock_guard guard(save_files::Lock());
  Store32(base, entry, 2);
  *GuestPtr(base, entry + 4) = 0;
  SetError(base, drive, kNone);
  ctx.r3.u64 = 1;
  if (index >= g_finds.size() || !g_finds[index].used) {
    return;
  }
  Find& find = g_finds[index];
  const std::vector<std::string> names = save_files::GameNames();
  while (find.next < names.size()) {
    const std::string& name = names[find.next++];
    if (Matches(name, find.pattern)) {
      uint8_t* out = GuestPtr(base, entry + 4);
      std::memset(out, 0, 257);
      std::memcpy(out, name.data(), std::min<size_t>(name.size(), 256));
      Store32(base, entry, 3);
      return;
    }
  }
}

// slot 20: FindFirst(r4 pattern, r5 entry, r6 -> find number out). ORIGINAL
// (0x82368BE8): takes one of 4 find slots, stores the pattern, then FindNext.
extern "C" REX_FUNC(sub_82368BE8) {
  const uint32_t drive = ctx.r3.u32;
  const std::string pattern = CleanName(GuestString(base, ctx.r4.u32));
  const uint32_t entry = ctx.r5.u32, find_out = ctx.r6.u32;
  {
    std::lock_guard guard(save_files::Lock());
    auto find = std::find_if(g_finds.begin(), g_finds.end(), [](const Find& f) { return !f.used; });
    if (find == g_finds.end()) {
      SetError(base, drive, kFailure);
      Store32(base, entry, 2);
      ctx.r3.u64 = 0;
      return;
    }
    *find = Find{true, pattern, 0};
    Store32(base, find_out, uint32_t(find - g_finds.begin()));
  }
  ctx.r4.u64 = find_out;
  ctx.r5.u64 = entry;
  sub_82369890(ctx, base);
}

// slot 22: FindClose(r4 -> find number). ORIGINAL (0x82368890): frees the slot.
extern "C" REX_FUNC(sub_82368890) {
  const uint32_t index = Load32(base, ctx.r4.u32);
  std::lock_guard guard(save_files::Lock());
  if (index < g_finds.size()) g_finds[index] = Find{};
  ctx.r3.u64 = 1;
}

// -----------------------------------------------------------------------------
// The storage device selector. ORIGINAL: the save handler (sub_82258E60, once
// at boot) calls XShowDeviceSelectorUI(user r3, content type r4, flags r5,
// bytes needed r6, r7 -> device id, r8 overlapped) through this wrapper; the
// Xbox shows a pop-up when there's a choice, and answers through the
// overlapped (polled by the handler, sub_82300300: InternalLow 0 = done,
// success). A return of 997 (ERROR_IO_PENDING) = request accepted (anything
// else = the handler retries once with other flags). OURS: device 1 (the only
// one: the saves folder), done at once.
// -----------------------------------------------------------------------------
extern "C" REX_FUNC(sub_823002D8) {
  const uint32_t device_out = ctx.r7.u32, overlapped = ctx.r8.u32;
  Store32(base, device_out, 1);
  if (overlapped) {
    Store32(base, overlapped + 0, 0);  // InternalLow: status = success (not 997, pending)
    Store32(base, overlapped + 4, 0);  // InternalHigh: result
  }
  ctx.r3.u64 = 997;
}

// The save drive's constructor (0x823686D0) made a system notification
// listener (storage devices added / removed), polled by its device check.
// PC: no storage devices come and go; nothing listens (handle 0). The game's
// other listener (sub_8227CE70) is gone too (players/who_plays.h): this
// wrapper (XamNotifyCreateListener) has no other caller.
extern "C" REX_FUNC(sub_823007D0) {
  ctx.r3.u64 = 0;
}
