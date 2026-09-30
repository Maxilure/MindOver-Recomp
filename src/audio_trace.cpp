// =============================================================================
// audio_trace.cpp -- which sounds the game asks for, by name (--debug_audio_trace)
// =============================================================================
//
// WHY (2026-09-27)
//   A playtest found a voice line missing from a cutscene (Coco's line in the
//   NV cutscene, docs/findings/19) and the log said nothing about audio at
//   all. Two very different causes look the same from the couch:
//     * the game never asked for the line (its own choice, or a timing
//       difference: e.g. it skips a line while another one still plays), or
//     * it asked, and the emulated audio (XMA decoder, voices) lost it.
//   This log tells them apart: it names every sound the game requests.
//
// THE GAME'S SOUND FILES (found 2026-09-27)
//   * Every sound is an .rsd file (Radical's container: "RSD6", codec
//     "XMA ", then channels / bits / sample rate as little-endian u32s,
//     "****" and the sound designer's original file name) inside a .rcf
//     archive: voices in english.rcf (8,320: Coco, Crunch, Cortex, Aku Aku,
//     the tutorial, and every enemy type: Bratgirls "bratgirl1..6",
//     Ratnicians "labrat1..6", Znus "znu", Slap-Es "slappe", "monkey1..6"
//     (name TBD)...), the other
//     sounds in default.rcf (1,162).
//   * Files are NAMED by a 64-bit sound ID in hex:
//     sound\rsd\english\8\8150b53fbe2b48d3.rsd, whose header says it is
//     in_game_art\sound\sounds\character\english\labrat1\battlecry_labrat1_09.wav.
//     We log that last name (without the "in_game_art\sound\sounds\" part).
//
// THE CODE WE WRAP
//   * sub_82336430 builds that path from the ID: r3 = the output string,
//     r4 = a prefix string, r5 = a POINTER to the 8-byte ID, r6 = a suffix
//     (or 0); it prints the ID as two "%08x" words (0x82330E80). Found by
//     searching the code for the ".rsd" string at 0x820093E8.
//   * Its callers (tools/callgraph.py), through two small wrappers:
//       0x82336598 <- CSoundResourceManager::ClipLoadThread (0x822DA8C0:
//                     loading sounds into memory) and 0x82348BC8
//       0x82336B20 <- audio::SeqEventClip vtable slot 1 (0x823385E8),
//                     audio::SeqEventStream slot 1 (0x82348AF8), 0x82338228
//     (class names from RTTI, tools/rtti_vtables.py). 0x82336B20 then opens
//     the file (0x823411C8).
//   We wrap sub_82336430 (it logs, after the original ran) and the two
//   SeqEvent methods (they only label the calling thread "clip" / "stream"
//   while they run), the way frame_rate.cpp wraps sub_82310728: the
//   generated sub_X is a weak alias of __imp__sub_X, so our strong sub_X
//   catches every call, virtual ones included.
//
// THE ARCHIVE INDEX (RCF 2.1, big-endian, "ATG CORE CEMENT LIBRARY")
//   0x24: u32 directory offset, directory size, names offset, names size,
//         (0), file count
//   directory: per file u32 name hash, data offset, size (sorted by HASH)
//   names: 8 bytes, then per file 12 bytes (a timestamp and two words), a
//          LITTLE-endian u32 length, the name (length bytes incl. its NUL)
//          and 3 bytes of padding. Names are listed in DATA order: sorting
//          the directory's offsets pairs them up. Checked on default.rcf:
//          all 1,162 .rsd names land on "RSD6" data, 1,102 .p3d names on
//          P3D models, 239 .lua/.blua names on Lua bytecode.
//   Only default.rcf and english.rcf are indexed (the game opens
//   english.rcf at boot; other languages repeat the same IDs).
//
// OUTPUT, one line per request:
//   Audio: <kind> <designer's file name> (<ID>) [dump <seconds>]
//   kind: "load" (through 0x82336598), "clip" (SeqEventClip), "stream"
//   (SeqEventStream), "other" (0x82338228). First log (2026-09-27): "load"
//   covers level loads AND sounds opened as they play (the enemies' lines
//   during a fight, a cutscene's dialogue track), so a line appears at the
//   moment a voice starts. "[dump s]" only with --debug_audio_dump: where
//   that moment is in the WAV file.
//
// THE SOUND ITSELF (--debug_audio_dump=<file.wav>)
//   Everything the game sends to the speakers, as a 6-channel 48 kHz 16-bit
//   WAV (front left/right, centre, LFE, rear left/right), recorded where
//   the XDK's audio thread hands each mixed frame to the kernel (manifest:
//   midasm hook CrashMomAudioFrame at 0x824819F0). One frame = 256 samples
//   per channel as big-endian floats, one whole channel after the other.
//   Compared with a sound decoded straight from the disc, it shows whether
//   a line reached the output. The file's sizes are rewritten about once a
//   second, so it stays readable if the game is closed or killed (closing
//   the window ends the process at once). ~0.6 MB per second.
// =============================================================================

#include "audio_trace.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(debug_audio_trace, false, "CrashMoM",
                    "Log every sound the game asks for, by the sound designer's file name");
REXCVAR_DEFINE_STRING(debug_audio_dump, "", "CrashMoM",
                      "Record the game's sound output to this WAV file (6 channels, 48 kHz, "
                      "16-bit)");

namespace audio_trace {
namespace {

uint32_t Be32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return __builtin_bswap32(v);
}
uint32_t Le32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

// Where one sound's .rsd file is.
struct RsdFile {
  uint16_t archive;  // index into Index::archives
  uint32_t offset;   // its first byte inside that archive
  std::string name;  // the designer's file name, read on first use ("" = not yet)
};

struct Index {
  std::mutex mutex;  // requests come from several game threads
  std::vector<std::filesystem::path> archives;
  std::unordered_map<uint64_t, RsdFile> files;  // key: the sound's 64-bit ID
};
Index g_index;
std::atomic<bool> g_on{false};

// Adds an archive's .rsd files to the index (see the header comment for the
// format). False if it isn't a readable RCF.
bool IndexArchive(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  uint8_t header[0x3C];
  if (!in.read(reinterpret_cast<char*>(header), sizeof(header)) ||
      std::memcmp(header, "ATG CORE CEMENT LIBRARY", 23) != 0) {
    return false;
  }
  const uint32_t directory_offset = Be32(header + 0x24);
  const uint32_t names_offset = Be32(header + 0x2C);
  const uint32_t names_size = Be32(header + 0x30);
  const uint32_t count = Be32(header + 0x38);

  // The data offsets, in data order (= the order of the names).
  std::vector<uint8_t> directory(size_t(count) * 12);
  in.seekg(directory_offset);
  if (!in.read(reinterpret_cast<char*>(directory.data()), directory.size())) {
    return false;
  }
  std::vector<uint32_t> offsets(count);
  for (uint32_t i = 0; i < count; ++i) {
    offsets[i] = Be32(&directory[size_t(i) * 12 + 4]);
  }
  std::sort(offsets.begin(), offsets.end());

  std::vector<uint8_t> names(names_size);
  in.seekg(names_offset);
  if (!in.read(reinterpret_cast<char*>(names.data()), names.size())) {
    return false;
  }
  const uint16_t archive = uint16_t(g_index.archives.size());
  g_index.archives.push_back(path);
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
    // "sound\rsd\english\8\8150b53fbe2b48d3.rsd": the last 16 characters
    // before ".rsd" are the ID in hex.
    if (name.size() < 21 || !name.ends_with(".rsd")) {
      continue;
    }
    const std::string_view hex = name.substr(name.size() - 20, 16);
    uint64_t id = 0;
    if (std::from_chars(hex.data(), hex.data() + hex.size(), id, 16).ptr != hex.end()) {
      continue;
    }
    g_index.files.try_emplace(id, RsdFile{archive, offsets[i], {}});
  }
  return true;
}

// The designer's file name of sound `id`, read from its RSD header the first
// time (the game reads the same file right after, so it's a cheap read).
std::string NameOf(uint64_t id) {
  std::lock_guard<std::mutex> lock(g_index.mutex);
  auto it = g_index.files.find(id);
  if (it == g_index.files.end()) {
    return "(not in default.rcf / english.rcf)";
  }
  RsdFile& file = it->second;
  if (file.name.empty()) {
    char header[0x200] = {};
    std::ifstream in(g_index.archives[file.archive], std::ios::binary);
    in.seekg(file.offset);
    in.read(header, sizeof(header) - 1);
    // "RSD6" "XMA " channels bits rate "****" then the name (NUL-terminated).
    std::string_view name(header + 0x18);
    constexpr std::string_view kPrefix = "in_game_art\\sound\\sounds\\";
    if (name.starts_with(kPrefix)) {
      name.remove_prefix(kPrefix.size());
    }
    file.name = name.empty() ? std::string("(no name in its header)") : std::string(name);
  }
  return file.name;
}

// --debug_audio_dump: the WAV file (written by the game's audio thread only)
// and how many frames it holds (read by the trace lines, any thread).
constexpr uint32_t kFrameSamples = 256;  // per channel
constexpr uint32_t kChannels = 6;
constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kWavHeaderBytes = 68;  // RIFF + WAVE_FORMAT_EXTENSIBLE fmt + data header
std::FILE* g_dump = nullptr;
std::atomic<uint64_t> g_dump_frames{0};

// The WAV header for `data_bytes` of samples: 16-bit PCM, 6 channels in the
// standard 5.1 order (mask 0x3F: FL FR FC LFE BL BR, like the Xbox's).
void WriteWavHeader(std::FILE* file, uint32_t data_bytes) {
  uint8_t h[kWavHeaderBytes] = {};
  auto put16 = [&](size_t at, uint16_t v) { std::memcpy(h + at, &v, 2); };
  auto put32 = [&](size_t at, uint32_t v) { std::memcpy(h + at, &v, 4); };
  std::memcpy(h, "RIFF", 4);
  put32(4, kWavHeaderBytes - 8 + data_bytes);
  std::memcpy(h + 8, "WAVEfmt ", 8);
  put32(16, 40);                             // fmt chunk size
  put16(20, 0xFFFE);                         // WAVE_FORMAT_EXTENSIBLE
  put16(22, kChannels);
  put32(24, kSampleRate);
  put32(28, kSampleRate * kChannels * 2);    // bytes per second
  put16(32, kChannels * 2);                  // bytes per sample frame
  put16(34, 16);                             // bits per sample
  put16(36, 22);                             // extension size
  put16(38, 16);                             // valid bits
  put32(40, 0x3F);                           // channel mask
  static const uint8_t kPcmGuid[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                       0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
  std::memcpy(h + 44, kPcmGuid, 16);
  std::memcpy(h + 60, "data", 4);
  put32(64, data_bytes);
  std::fseek(file, 0, SEEK_SET);
  std::fwrite(h, 1, sizeof(h), file);
  std::fseek(file, 0, SEEK_END);
}

// " [dump 12.34 s]" while recording, else "".
std::string DumpTime() {
  if (!g_dump) {
    return {};
  }
  const double seconds =
      double(g_dump_frames.load(std::memory_order_relaxed)) * kFrameSamples / kSampleRate;
  return fmt::format(" [dump {:.2f} s]", seconds);
}

// While a SeqEvent method runs on this thread: which one ("clip", "stream").
thread_local const char* t_kind = nullptr;

// Return address inside 0x82336598 (the "load" wrapper) after its call.
constexpr uint32_t kFromLoadWrapper = 0x823365FC;

}  // namespace

void Start(const std::filesystem::path& game_data_root) {
  if (const std::string& dump = REXCVAR_GET(debug_audio_dump); !dump.empty()) {
    g_dump = std::fopen(dump.c_str(), "wb");
    if (g_dump) {
      WriteWavHeader(g_dump, 0);
      REXLOG_INFO("Audio dump: recording the game's sound output into {}", dump);
    } else {
      REXLOG_WARN("Audio dump: can't write {}", dump);
    }
  }
  if (!REXCVAR_GET(debug_audio_trace)) {
    return;
  }
  for (const char* archive : {"default.rcf", "english.rcf"}) {
    if (!IndexArchive(game_data_root / archive)) {
      REXLOG_WARN("Audio trace: can't read the file list of {}", archive);
    }
  }
  REXLOG_INFO("Audio trace: {} sounds indexed; logging every sound the game asks for",
              g_index.files.size());
  g_on.store(true, std::memory_order_release);
}

}  // namespace audio_trace

// -----------------------------------------------------------------------------
// The wrapped game functions (strong definitions of the generated weak ones)
// -----------------------------------------------------------------------------

// The .rsd path from a sound ID (r5 points to it).
extern "C" REX_FUNC(__imp__sub_82336430);
extern "C" REX_FUNC(sub_82336430) {
  if (!audio_trace::g_on.load(std::memory_order_acquire)) {
    __imp__sub_82336430(ctx, base);
    return;
  }
  // Read everything before the call: it changes the registers.
  const uint32_t id_address = ctx.r5.u32;
  const uint32_t return_address = static_cast<uint32_t>(ctx.lr);
  const uint64_t id = (uint64_t(audio_trace::Be32(base + id_address)) << 32) |
                      audio_trace::Be32(base + id_address + 4);
  __imp__sub_82336430(ctx, base);
  const char* kind = return_address == audio_trace::kFromLoadWrapper ? "load"
                     : audio_trace::t_kind                           ? audio_trace::t_kind
                                                                      : "other";
  REXLOG_INFO("Audio: {} {} ({:016x}){}", kind, audio_trace::NameOf(id), id,
              audio_trace::DumpTime());
}

// --debug_audio_dump: one mixed frame on its way to the kernel (manifest
// midasm hook at 0x824819F0, r4 = the frame). Game's audio thread.
void CrashMomAudioFrame(PPCRegister& r4) {
  std::FILE* file = audio_trace::g_dump;
  if (!file) {
    return;
  }
  auto* memory = rex::system::kernel_memory();
  if (!memory) {
    return;
  }
  using audio_trace::kChannels;
  using audio_trace::kFrameSamples;
  const uint8_t* frame = memory->virtual_membase() + r4.u32;
  // Channel planes of big-endian floats -> interleaved 16-bit samples.
  int16_t out[kFrameSamples * kChannels];
  for (uint32_t c = 0; c < kChannels; ++c) {
    for (uint32_t i = 0; i < kFrameSamples; ++i) {
      uint32_t bits = audio_trace::Be32(frame + (size_t(c) * kFrameSamples + i) * 4);
      float v;
      std::memcpy(&v, &bits, sizeof(v));
      out[i * kChannels + c] = int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
    }
  }
  std::fwrite(out, sizeof(out), 1, file);
  const uint64_t frames = audio_trace::g_dump_frames.fetch_add(1, std::memory_order_relaxed) + 1;
  // Keep the header's sizes current (~every second: 188 frames of 5.3 ms).
  if (frames % 188 == 0) {
    audio_trace::WriteWavHeader(file, uint32_t(frames * sizeof(out)));
    std::fflush(file);
  }
}

// audio::SeqEventClip, vtable slot 1: only labels the thread while it runs.
extern "C" REX_FUNC(__imp__sub_823385E8);
extern "C" REX_FUNC(sub_823385E8) {
  const char* previous = audio_trace::t_kind;
  audio_trace::t_kind = "clip";
  __imp__sub_823385E8(ctx, base);
  audio_trace::t_kind = previous;
}

// audio::SeqEventStream, vtable slot 1: the same, "stream".
extern "C" REX_FUNC(__imp__sub_82348AF8);
extern "C" REX_FUNC(sub_82348AF8) {
  const char* previous = audio_trace::t_kind;
  audio_trace::t_kind = "stream";
  __imp__sub_82348AF8(ctx, base);
  audio_trace::t_kind = previous;
}
