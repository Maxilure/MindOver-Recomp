// =============================================================================
// subtitles/voice_trace.cpp -- when each voice really plays (--debug_subtitle_trace)
// =============================================================================
//
// WHY (findings/63 s.6, s.12)
//   A subtitle must follow its voice: appear when the first samples of the
//   line go out, stay while it plays (and while the game is paused), leave
//   when it ends or is cut off. The port already sees every sound's samples
//   (it decodes them, audio/xma_native.cpp), but one link is unproven: which
//   sound ID a decoder object belongs to. The ID is known in sub_82336430
//   (ID -> .rsd path, audio_trace.cpp), the object in its constructor, and
//   nothing in the object holds the ID (findings/63 s.13.3). The first run
//   (findings/63 s.15) showed the simplest guess wrong: the IDs are turned
//   into paths on the main thread and the loader thread, the decoders are
//   built on the audio thread. So the link is made by LENGTH: the decoder's
//   XMA header (+0x14 -> H) and the .rsd file's header at 0x80C are the same
//   bytes, so H+8 (end sample) equals the archive's value for its ID, and the
//   decoder's +0x1C (compressed bytes, from the 12-byte size header it reads)
//   equals the .rsd file's word at 0x808. Lengths alone repeat (1,840
//   different ones among 8,320 English clips; with the size, 3,925), so both
//   are compared. Every ID asked for (any thread) goes into a short list with
//   both (read with its name, on the asking thread: no disk read on the audio
//   thread); a new decoder takes the NEWEST ID of the last 15 s with the same
//   pair. The log says how many IDs had that pair (1 = sure).
//
// WHAT IS WRAPPED HERE (strong definitions of the generated weak sub_X, as
// audio_trace.cpp does; the originals stay callable as __imp__sub_X)
//   sub_8234BE88  audio::hal::hw::XmaDecoder constructor (r3 = object,
//                 r4 = inner RsdFileStream). Its own midasm hook
//                 CrashMomXmaNoContext (xma_native.cpp) still runs inside.
//   sub_8234C8A8  XmaDecoder::Read (vtable slot 6): hands out PCM to the
//                 mixer; object +0x40 = frames handed out so far.
// And called from the existing wrappers: the destructor sub_8234BC58
// (xma_native.cpp), sub_82336430 (audio_trace.cpp), PlayerBink's open (slot 6,
// language.cpp's handler on the renderer interceptor). The movie's frames and
// its close are watched at the end of every frame (pddi frame-end listener):
// the close sets the player's Bink handle (+0x1C) to 0 (findings/54 s.8).
// That needs no handler on slots 8 / 11, which other code owns in newer
// versions (options/global_options.cpp: the movie volume).
//
// THE OBJECT (findings/54 s.5, xma_native.cpp)
//   +0x14 H: H+1 (u8) streams, H+4 lead-in samples, H+8 end sample,
//         H+0x0C sample rate (all big-endian)
//   +0x40 position: frames handed out (after the lead-in)
//
// LOG LINES ("Voice:" / "Movie trace:"), all with the session clock in
// seconds and, with --debug_audio_dump, the place in the recording:
//   Voice: new <object> = <name> (<id>), <s> s, asked <ms> ms before, <n> ID(s) of that length and size
//   Voice: start <object> <name> after <ms> ms
//   Voice: refill <object> <name> at <s> s, <ms> ms after the last read
//   Voice: decoded <object> <name> at <s> s of <s> s
//   Voice: gone <object> <name> at <s> s of <s> s, <reason>          (destroyed)
// NOTE (findings/63 s.16): +0x40 counts what the mixer was HANDED, which runs
// up to 4.52 s ahead of what is heard (the first Read takes 4.52 s, then
// 2.26 s each time half of it has played). So "decoded" and "gone" are not
// the end of the line: the line ends at start + its length, unless the
// object is destroyed before it was all decoded (then it was cut off).
// Sounds whose guessed name is not a voice (music, effects) are counted, and
// logged only with their first line, so the log stays readable: names under
// character\ and nis\ are logged in full.
// =============================================================================

#include "subtitles/voice_trace.h"

#include "audio_trace.h"
#include "guest_memory.h"
#include "pddi/intercept.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(debug_subtitle_trace, false, "CrashMoM",
                    "Log when each voice line really plays (decoder start / end, pauses, "
                    "movies): the first step of subtitles, findings/63");

namespace voice_trace {
namespace {

std::atomic<bool> g_on{false};

using Clock = std::chrono::steady_clock;
const Clock::time_point g_t0 = Clock::now();
double Now() { return std::chrono::duration<double>(Clock::now() - g_t0).count(); }

uint32_t R32(uint8_t* base, uint32_t a) {
  const uint8_t* p = GuestPtr(base, a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// " [dump 12.34 s]" while --debug_audio_dump records, else "".
std::string Dump() {
  const double s = audio_trace::DumpSeconds();
  return s < 0 ? std::string() : fmt::format(" [dump {:.2f} s]", s);
}

// The sound IDs asked for lately (any thread), newest last, with their end
// sample (H+8 of the .rsd file; 0 = unknown).
struct Asked {
  uint64_t id = 0;
  uint32_t end = 0;
  uint32_t data = 0;  // compressed bytes (.rsd 0x808)
  double time = -1;
  uint32_t from = 0;
};
constexpr size_t kAsked = 256;          // ring size: a level load asks for a few hundred
constexpr double kAskedWindow = 15.0;   // seconds an ID stays a candidate
std::array<Asked, kAsked> g_asked;
size_t g_asked_next = 0;
std::mutex g_asked_mutex;

// One decoder object while it lives.
struct Voice {
  uint64_t id = 0;
  std::string name;   // the guessed sound's designer name ("" = no guess)
  bool voice = false; // a character\ or nis\ name: logged in full
  uint32_t rate = 0;
  uint32_t lead_in = 0;
  uint32_t end = 0;   // H+8
  double created = 0;
  double last_read = -1;
  uint32_t last_pos = 0;
  bool started = false;
  bool ended = false;
};
std::mutex g_mutex;
std::unordered_map<uint32_t, Voice> g_voices;  // key: the object's guest address
uint64_t g_other_count = 0;                    // non-voice sounds seen (not logged)

// Frames after the lead-in: what +0x40 counts up to.
uint32_t Playable(const Voice& v) { return v.end > v.lead_in ? v.end - v.lead_in : v.end; }
double Seconds(const Voice& v, uint32_t frames) { return v.rate ? double(frames) / v.rate : 0; }

// A gap between two Reads longer than this is logged as a refill: a voice is
// read in large pieces (4.52 s, then 2.26 s every 2.26 s of playing).
constexpr double kStallSeconds = 0.4;

// Bink: the player with a movie open, and when its handle words were last logged.
uint32_t g_movie = 0;
double g_movie_logged = -1;
std::string g_movie_name;

void OnFrameEnd(void*);
void LogMovieWords(uint8_t* base, uint32_t handle, double now);

}  // namespace

bool On() { return g_on.load(std::memory_order_acquire); }

void Start() {
  if (!REXCVAR_GET(debug_subtitle_trace)) return;
  g_on.store(true, std::memory_order_release);
  pddi::AddFrameEndListener(&OnFrameEnd, nullptr);
  REXLOG_INFO("Voice trace: on (findings/63 milestone 1){}",
              audio_trace::Indexed() ? ""
                                     : "; WARNING: --debug_audio_trace is off, so sound names "
                                       "and lengths are unknown");
}

void NoteSoundId(uint64_t id, uint32_t from) {
  // The header is read here, on the asking thread (cached by audio_trace).
  const audio_trace::ClipHeader clip = audio_trace::ClipHeaderOf(id);
  std::lock_guard<std::mutex> lock(g_asked_mutex);
  g_asked[g_asked_next % kAsked] =
      Asked{id, clip.ok ? clip.end : 0, clip.ok ? clip.data : 0, Now(), from};
  ++g_asked_next;
}

void OnDecoderDestroyed(uint32_t object, uint8_t* base) {
  if (!On()) return;
  Voice v;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_voices.find(object);
    if (it == g_voices.end()) return;
    v = std::move(it->second);
    g_voices.erase(it);
  }
  if (!v.voice) return;
  const uint32_t pos = R32(base, object + 0x40);
  const char* reason = v.ended          ? "all decoded (still playing from the mixer's buffer)"
                       : !v.started     ? "never started (loaded, not played?)"
                                        : "cut off before the end (skip / interruption)";
  REXLOG_INFO("Voice: gone {:08x} {} at {:.2f} s of {:.2f} s, {}, lived {:.2f} s{}", object, v.name,
              Seconds(v, pos), Seconds(v, Playable(v)), reason, Now() - v.created, Dump());
}

void OnMovieOpen(uint32_t player, const std::string& name, uint8_t* base) {
  if (!On()) return;
  g_movie = player;
  g_movie_name = name;
  g_movie_logged = -1;
  const uint32_t handle = R32(base, player + 0x1C);
  REXLOG_INFO("Movie trace: open {} (player {:08x}, Bink handle {:08x}) at {:.2f} s{}", name,
              player, handle, Now(), Dump());
  if (handle) LogMovieWords(base, handle, Now());
}

// Once a second while a movie plays: the first 8 words of the Bink handle.
// Bink 1 keeps its width, height, frame count and current frame number near
// the start of its struct (to be confirmed here: the word counting up at the
// movie's frame rate is the current frame).
namespace {
void LogMovieWords(uint8_t* base, uint32_t handle, double now) {
  std::string words;
  for (uint32_t i = 0; i < 8; ++i) words += fmt::format(" {}", R32(base, handle + 4 * i));
  REXLOG_INFO("Movie trace: {} at {:.2f} s, handle words:{}", g_movie_name, now, words);
}

// Main thread, right after each SwapBuffers (pddi/intercept.h).
void OnFrameEnd(void*) {
  if (!g_movie) return;
  uint8_t* base = rex::system::kernel_memory()->virtual_membase();
  const double now = Now();
  const uint32_t handle = R32(base, g_movie + 0x1C);
  if (!handle) {  // BinkClose ran (PlayerBink slot 8): the movie ended or was skipped
    REXLOG_INFO("Movie trace: close {} at {:.2f} s{}", g_movie_name, now, Dump());
    g_movie = 0;
    return;
  }
  if (g_movie_logged >= 0 && now - g_movie_logged < 1.0) return;
  g_movie_logged = now;
  LogMovieWords(base, handle, now);
}
}  // namespace

}  // namespace voice_trace

// -----------------------------------------------------------------------------
// The wrapped game functions
// -----------------------------------------------------------------------------

using namespace voice_trace;

// XmaDecoder constructor: r3 = the object, r4 = its inner stream.
extern "C" REX_FUNC(__imp__sub_8234BE88);
extern "C" REX_FUNC(sub_8234BE88) {
  const uint32_t object = ctx.r3.u32;
  __imp__sub_8234BE88(ctx, base);
  if (!On()) return;
  const double now = Now();
  const uint32_t h = R32(base, object + 0x14);  // the XMA header H
  Voice v;
  v.created = now;
  v.lead_in = R32(base, h + 4);
  v.end = R32(base, h + 8);
  v.rate = R32(base, h + 0x0C);
  const uint32_t data = R32(base, object + 0x1C);  // compressed bytes
  // The newest ID asked for in the last kAskedWindow seconds with the same
  // end sample, and how many different IDs had that length.
  Asked match;
  int same_length = 0;
  {
    std::lock_guard<std::mutex> lock(g_asked_mutex);
    uint64_t seen[8] = {};
    for (size_t i = 0; i < std::min(g_asked_next, kAsked); ++i) {
      const Asked& a = g_asked[(g_asked_next - 1 - i) % kAsked];  // newest first
      if (a.time < 0 || now - a.time > kAskedWindow) break;
      if (a.end == 0 || a.end != v.end || a.data != data) continue;
      bool counted = false;
      for (int k = 0; k < same_length && k < 8; ++k) counted |= seen[k] == a.id;
      if (counted) continue;
      if (same_length < 8) seen[same_length] = a.id;
      if (same_length++ == 0) match = a;
    }
  }
  const double age_ms = same_length ? (now - match.time) * 1000.0 : -1;
  std::string check = "no ID of that length and size asked for lately";
  if (same_length) {
    v.id = match.id;
    v.name = audio_trace::SoundName(v.id);
    check = fmt::format("{} ID(s) of that length and size", same_length);
  }
  v.voice = v.name.starts_with("character\\") || v.name.starts_with("nis\\");
  bool log_it = v.voice;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!v.voice && g_other_count++ < 20) log_it = true;  // a sample of the others
    g_voices[object] = v;
  }
  if (log_it) {
    REXLOG_INFO("Voice: new {:08x} = {} ({:016x}), {:.2f} s, {} stream(s), asked {:.0f} ms "
                "before (from 0x{:08X}), {}{}",
                object, v.name.empty() ? "?" : v.name, v.id, Seconds(v, Playable(v)),
                uint32_t(*GuestPtr(base, h + 1)), age_ms, match.from, check, Dump());
  }
}

// XmaDecoder::Read: r3 = the object. After it, +0x40 = frames handed out.
extern "C" REX_FUNC(__imp__sub_8234C8A8);
extern "C" REX_FUNC(sub_8234C8A8) {
  const uint32_t object = ctx.r3.u32;
  __imp__sub_8234C8A8(ctx, base);
  if (!On()) return;
  const double now = Now();
  const uint32_t pos = R32(base, object + 0x40);
  std::string line;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_voices.find(object);
    if (it == g_voices.end()) return;
    Voice& v = it->second;
    if (v.voice) {
      if (!v.started && pos > 0) {
        v.started = true;
        line = fmt::format("Voice: start {:08x} {} after {:.0f} ms", object, v.name,
                           (now - v.created) * 1000.0);
      } else if (v.started && !v.ended && v.last_read >= 0 && now - v.last_read > kStallSeconds) {
        line = fmt::format("Voice: refill {:08x} {} at {:.2f} s, {:.0f} ms after the last read", object,
                           v.name, Seconds(v, v.last_pos), (now - v.last_read) * 1000.0);
      }
      if (!v.ended && pos >= Playable(v) && Playable(v) > 0) {
        v.ended = true;
        if (!line.empty()) line += "; ";
        line += fmt::format("Voice: decoded {:08x} {} at {:.2f} s of {:.2f} s", object, v.name,
                            Seconds(v, pos), Seconds(v, Playable(v)));
      }
    }
    v.last_read = now;
    v.last_pos = pos;
  }
  if (!line.empty()) REXLOG_INFO("{}{}", line, Dump());
}
