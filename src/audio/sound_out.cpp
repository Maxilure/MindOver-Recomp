// =============================================================================
// audio/sound_out.cpp -- the game's mixed sound, played by OUR code
// =============================================================================
//
// WHAT THE GAME DOES (findings/54 section 1)
// ------------------------------------------
// Radical's audio engine plays every sound through Microsoft's XAudio library,
// which is statically linked into default.xex (code around 0x82470000-
// 0x82490000). XAudio mixes everything into ONE block at a time: 256 samples
// per channel, 6 channels (5.1: front left, front right, centre, LFE, back
// left, back right), 48 kHz, big-endian 32-bit floats, one whole channel after
// the other ("planar"). One block = 5.33 ms of sound.
//
// XAudio's render-driver object (vtables 0x8201BCAC / 0x8201BC90) talks to the
// console through three kernel calls, each made from exactly one place:
//
//   XAudioRegisterRenderDriverClient   0x824817C4 in sub_82481768
//     r3 = {callback, callback argument}, r4 = where the handle goes.
//     The console then calls `callback` every time it wants another block.
//   XAudioSubmitRenderDriverFrame      0x824819F0 in sub_824819A8
//     r3 = the handle, r4 = the mixed block.
//   XAudioUnregisterRenderDriverClient 0x82481794 (sub_82481768, re-register)
//                                      0x8248190C (sub_82481890, destructor)
//     r3 = the handle.
//
// Until now the SDK's fake Xbox answered them (thirdparty/rexglue-sdk
// src/audio/: an "Audio Worker" thread that runs the callback whenever the
// SDL device used up a block). Midasm hooks at those four instructions (the
// manifest's "SOUND OUT" entries) now answer them with this file instead and
// jump over the kernel call. The SDK's audio device is then never opened.
//
// HOW WE PLAY IT
// --------------
//   * a guest thread of ours, "Sound Out", runs the game's callback (the
//     callback is game code, so it needs a guest thread: the SDK's XHostThread)
//     whenever the speakers have room for another block (a "credit");
//   * the game's submit copies the block into our queue (byte-swapped to
//     native floats right away, on the game's thread);
//   * SDL's device callback takes blocks from the queue, converts them for the
//     device (5.1 as is, or folded to stereo) and gives a credit back.
//   The number of credits = how many blocks may be on their way at once
//   (--sound_out_buffer_blocks, default 8 = 43 ms, the SDK's audio_maxqframes
//   default too). When the speakers want sound and no block is ready, they get
//   a block of silence: counted as an UNDERRUN (log line once a minute).
//
//   The first version reproduces the SDK's output stage EXACTLY (same fold
//   weights, same clamp) so the two can be compared sample for sample
//   (--sound_out_native=false = the SDK's path); changes to the sound itself
//   come later, one at a time.
//
// WHY: going native (notes/plans.md section 9, "Sound out"): our own output
// stage is where volume, device choice, stereo / 5.1 / 7.1 and the fixes for
// the game's own sound problems can live, without patching the SDK.
// =============================================================================

#include "audio/sound_out.h"
#include "guest_memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cmath>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

REXCVAR_DEFINE_BOOL(sound_out_native, true, "CrashMoM",
                    "Play the game's sound with the port's own output (off = the SDK's fake Xbox "
                    "audio device, as before)");
REXCVAR_DEFINE_INT32(sound_out_buffer_blocks, 8, "CrashMoM",
                     "Sound blocks (5.33 ms each) allowed on their way to the speakers at once: "
                     "fewer = less delay, too few = crackles (3-32)");
REXCVAR_DEFINE_INT32(sound_speakers, 0, "CrashMoM",
                     "Speakers: 0 Automatic (the device's own layout; the system fits 5.1 to it), "
                     "1 Stereo, 2 Headphones (stereo with crossfeed), 3 5.1 Surround, 4 7.1 Surround "
                     "(Options -> Audio)");
REXCVAR_DEFINE_STRING(sound_device, "", "CrashMoM",
                      "Sound output device by name; empty = the system's default output, followed "
                      "when it changes (Options -> Audio)");

namespace sound_out {
namespace {

constexpr uint32_t kChannels = 6;          // 5.1, XAudio's order: FL FR FC LFE BL BR
constexpr uint32_t kBlockSamples = 256;    // per channel
constexpr uint32_t kBlockFloats = kChannels * kBlockSamples;
constexpr int kRate = 48000;

// Our handle for the game: anything the SDK's own handles (0x4155xxxx) can't
// be, so a stray one would be caught by the SDK's asserts, not misread.
constexpr uint32_t kHandle = 0x4E410001;   // "NA" + client 1

using Block = std::array<float, kBlockFloats>;   // planar, NATIVE byte order

struct Output {
  // The game's callback and the guest word holding its argument (XAudio's
  // callback gets a POINTER to its argument: the SDK passes the same).
  uint32_t callback = 0;
  uint32_t arg_word = 0;

  rex::system::object_ref<rex::system::XHostThread> thread;
  SDL_AudioStream* stream = nullptr;
  int device_channels = 0;   // what we give SDL: 2 (our stereo mix), 6 or 8
  Speakers mode = Speakers::kAuto;   // as opened (sound_speakers)
  bool crossfeed = false;            // Headphones
  std::string device_name;           // the device playing now (for the log / Options)

  std::mutex mutex;
  std::condition_variable wake;      // credits came back / stop
  std::deque<Block> queue;           // mixed blocks waiting for the speakers
  int credits = 0;
  bool running = false;
  // Bumped at every register: a thread from an earlier register (the game
  // can re-register) sees the change and ends instead of running twice.
  uint32_t generation = 0;

  std::atomic<uint64_t> played{0};
  std::atomic<uint64_t> underruns{0};
};

Output g_out;
std::atomic<bool> g_muted{false};
// Opening / closing the device: Options (game thread), the watchdog (Sound
// Out thread) and the game's register / unregister can all do it.
std::recursive_mutex g_device_mutex;

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// --audio_mute is the SDK's flag (test runs pass it). It lives in the SDK's
// library, so it's read by name, not every block: once every 64 (~0.3 s).
void RefreshMute() {
  static uint32_t counter = 0;
  if (counter++ % 64 == 0) {
    g_muted.store(rex::cvar::GetFlagByName("audio_mute") == "true", std::memory_order_relaxed);
  }
}

// STEREO MIX (Stereo, Headphones, and Automatic on a device that says it has
// only 1-2 channels). The usual 5.1 -> stereo fold (ITU-R BS.775): front left
// / right at full level, centre and back at -3 dB (0.707) into both / their
// side, LFE left out (it only repeats the low end of the other channels).
// Kept at full level, so it is as loud as a system's own fold of the same
// block; the rare peak above full scale goes through a soft knee instead of
// being cut flat (a cut sounds like a crackle).
float SoftLimit(float x) {
  constexpr float kKnee = 0.8f;   // untouched below this
  const float a = std::fabs(x);
  if (a <= kKnee) return x;
  const float over = (a - kKnee) / (1.0f - kKnee);
  return std::copysign(kKnee + (1.0f - kKnee) * std::tanh(over), x);
}

// HEADPHONES = the stereo mix + CROSSFEED: on speakers each ear also hears
// the other speaker, a little later and duller; headphones don't, so sounds
// panned hard to one side sit inside one ear. A little of each side, low-
// passed at 700 Hz, 6 dB down and 0.3 ms late, goes to the other ear (the
// classic Bauer / bs2b recipe), then the level is brought back a bit so the
// added low end doesn't make it louder.
struct Crossfeed {
  static constexpr int kDelay = 14;   // 0.3 ms at 48 kHz
  float lp_l = 0, lp_r = 0;           // one-pole low-pass state
  std::array<float, kDelay> dl{}, dr{};
  int at = 0;
  void Reset() { *this = Crossfeed{}; }
  void Run(float& l, float& r) {
    // one-pole low-pass at 700 Hz: a = 1 - exp(-2 pi fc / fs)
    constexpr float kA = 0.0875f;
    lp_l += kA * (l - lp_l);
    lp_r += kA * (r - lp_r);
    const float from_l = dl[size_t(at)], from_r = dr[size_t(at)];
    dl[size_t(at)] = lp_l;
    dr[size_t(at)] = lp_r;
    at = (at + 1) % kDelay;
    constexpr float kFeed = 0.5f;     // -6 dB
    constexpr float kLevel = 0.8f;    // bring the level back
    const float nl = (l + kFeed * from_r) * kLevel;
    const float nr = (r + kFeed * from_l) * kLevel;
    l = nl;
    r = nr;
  }
};
Crossfeed g_crossfeed;   // only the SDL device thread touches it (reset on open)

// One planar 5.1 block -> what the device gets, interleaved.
void Convert(const Block& in, float* out, int channels, bool crossfeed) {
  const float* fl = &in[0 * kBlockSamples];
  const float* fr = &in[1 * kBlockSamples];
  const float* fc = &in[2 * kBlockSamples];
  const float* lf = &in[3 * kBlockSamples];
  const float* bl = &in[4 * kBlockSamples];
  const float* br = &in[5 * kBlockSamples];
  constexpr float k3dB = 0.70710678f;
  if (channels == 2) {
    for (uint32_t i = 0; i < kBlockSamples; ++i) {
      float l = fl[i] + k3dB * fc[i] + k3dB * bl[i];
      float r = fr[i] + k3dB * fc[i] + k3dB * br[i];
      if (crossfeed) g_crossfeed.Run(l, r);
      out[i * 2 + 0] = SoftLimit(l);
      out[i * 2 + 1] = SoftLimit(r);
    }
    return;
  }
  if (channels == 8) {
    // 7.1 (SDL's order: FL FR FC LFE BL BR SL SR): the game's surround pair
    // goes to the side AND back speakers at -3 dB each (same total power), so
    // the sound around you fills both instead of leaving a pair silent.
    for (uint32_t i = 0; i < kBlockSamples; ++i) {
      float* o = &out[i * 8];
      o[0] = std::clamp(fl[i], -1.0f, 1.0f);
      o[1] = std::clamp(fr[i], -1.0f, 1.0f);
      o[2] = std::clamp(fc[i], -1.0f, 1.0f);
      o[3] = std::clamp(lf[i], -1.0f, 1.0f);
      o[4] = o[6] = std::clamp(bl[i] * k3dB, -1.0f, 1.0f);
      o[5] = o[7] = std::clamp(br[i] * k3dB, -1.0f, 1.0f);
    }
    return;
  }
  // 6 channels: SDL's 5.1 order is the same as XAudio's (SDL / the system
  // fits it to whatever the device has).
  for (uint32_t i = 0; i < kBlockSamples; ++i) {
    out[i * 6 + 0] = std::clamp(fl[i], -1.0f, 1.0f);
    out[i * 6 + 1] = std::clamp(fr[i], -1.0f, 1.0f);
    out[i * 6 + 2] = std::clamp(fc[i], -1.0f, 1.0f);
    out[i * 6 + 3] = std::clamp(lf[i], -1.0f, 1.0f);
    out[i * 6 + 4] = std::clamp(bl[i], -1.0f, 1.0f);
    out[i * 6 + 5] = std::clamp(br[i], -1.0f, 1.0f);
  }
}

// SDL's device thread: "I need `additional` more bytes". Like the SDK we hand
// over whole blocks (or a block of silence when the game is behind).
void SDLCALL DeviceWants(void*, SDL_AudioStream* stream, int additional, int) {
  const int channels = g_out.device_channels;
  const bool crossfeed = g_out.crossfeed;
  const int bytes = int(kBlockSamples) * channels * int(sizeof(float));
  float data[kBlockSamples * 8];
  RefreshMute();
  while (additional > 0) {
    bool have = false;
    {
      std::lock_guard<std::mutex> lock(g_out.mutex);
      if (!g_out.queue.empty()) {
        if (g_muted.load(std::memory_order_relaxed)) {
          std::memset(data, 0, size_t(bytes));
        } else {
          Convert(g_out.queue.front(), data, channels, crossfeed);
        }
        g_out.queue.pop_front();
        ++g_out.credits;   // room for one more: the game may mix the next
        have = true;
      }
    }
    if (have) {
      g_out.wake.notify_one();
      g_out.played.fetch_add(1, std::memory_order_relaxed);
    } else {
      std::memset(data, 0, size_t(bytes));
      // Silence before the first block isn't the game being late.
      if (g_out.played.load(std::memory_order_relaxed) > 0) {
        g_out.underruns.fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (!SDL_PutAudioStreamData(stream, data, bytes)) {
      REXLOG_ERROR("Sound out: SDL_PutAudioStreamData failed: {}", SDL_GetError());
      break;
    }
    additional -= bytes;
  }
}

// The playback devices SDL sees now: id + name.
std::vector<std::pair<SDL_AudioDeviceID, std::string>> ListDevices() {
  std::vector<std::pair<SDL_AudioDeviceID, std::string>> out;
  int count = 0;
  SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&count);
  for (int i = 0; ids && i < count; ++i) {
    const char* name = SDL_GetAudioDeviceName(ids[i]);
    out.emplace_back(ids[i], name ? name : "");
  }
  SDL_free(ids);
  return out;
}

void CloseDevice() {
  std::lock_guard<std::recursive_mutex> lock(g_device_mutex);
  if (g_out.stream) {
    SDL_DestroyAudioStream(g_out.stream);   // waits for DeviceWants to finish
    g_out.stream = nullptr;
  }
}

// Opens the device (sound_device) in the layout sound_speakers asks for.
//   Automatic: like the SDK's driver did, so the default changes nothing you
//     hear: ask for 5.1, then look at what the opened device reports. Only one
//     that says 1-2 channels gets our stereo mix; otherwise the 5.1 block goes
//     as is and the system (SDL / PipeWire / Windows) fits it to the speakers.
//     (2026-10-31: a plain "Analog Stereo" PipeWire output reports 6 when
//     opened, so there the SYSTEM's fold is what's heard.)
//   Stereo / Headphones: 2 channels, our stereo mix (+ crossfeed).
//   5.1 / 7.1: 6 / 8 channels; a device with fewer gets SDL's own fold.
bool OpenDevice() {
  std::lock_guard<std::recursive_mutex> lock(g_device_mutex);
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    REXLOG_ERROR("Sound out: SDL audio didn't start: {}", SDL_GetError());
    return false;
  }
  CloseDevice();
  // Which device: by name, else the system default (SDL follows the
  // default when the system switches it, e.g. headphones plugged in).
  const std::string wanted = std::string(REXCVAR_GET(sound_device));
  SDL_AudioDeviceID target = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
  if (!wanted.empty()) {
    bool found = false;
    for (const auto& [id, name] : ListDevices()) {
      if (name == wanted) {
        target = id;
        found = true;
        break;
      }
    }
    if (!found) {
      REXLOG_WARN("Sound out: the chosen device '{}' isn't connected; using the system default",
                  wanted);
    }
  }
  const int mode = std::clamp(int(REXCVAR_GET(sound_speakers)), 0, 4);
  g_out.mode = Speakers(mode);
  g_out.crossfeed = g_out.mode == Speakers::kHeadphones;
  g_crossfeed.Reset();
  int channels = 6;
  if (g_out.mode == Speakers::kStereo || g_out.mode == Speakers::kHeadphones) channels = 2;
  if (g_out.mode == Speakers::k71) channels = 8;
  SDL_AudioSpec spec = {SDL_AUDIO_F32, channels, kRate};
  SDL_AudioSpec device = {};
  g_out.device_channels = channels;
  g_out.stream = SDL_OpenAudioDeviceStream(target, &spec, DeviceWants, nullptr);
  if (g_out.stream && g_out.mode == Speakers::kAuto &&
      SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(g_out.stream), &device, nullptr) &&
      device.channels <= 2) {
    SDL_DestroyAudioStream(g_out.stream);
    spec.channels = 2;
    g_out.device_channels = 2;
    g_out.stream = SDL_OpenAudioDeviceStream(target, &spec, DeviceWants, nullptr);
  }
  if (!g_out.stream) {
    REXLOG_ERROR("Sound out: no audio device: {}", SDL_GetError());
    g_out.device_name.clear();
    return false;
  }
  const SDL_AudioDeviceID id = SDL_GetAudioStreamDevice(g_out.stream);
  SDL_GetAudioDeviceFormat(id, &device, nullptr);
  const char* name = SDL_GetAudioDeviceName(id);
  g_out.device_name = name ? name : "";
  static const char* const kModes[] = {"automatic", "stereo", "headphones", "5.1", "7.1"};
  REXLOG_INFO("Sound out: playing on '{}' ({} channels at {} Hz; we send {}, speakers {}), {} "
              "blocks of 5.33 ms on their way at once",
              g_out.device_name.empty() ? "?" : g_out.device_name, device.channels, device.freq,
              g_out.device_channels, kModes[mode], REXCVAR_GET(sound_out_buffer_blocks));
  SDL_ResumeAudioDevice(id);
  return true;
}

// "Sound Out" guest thread: run the game's mixer callback once per credit.
int ThreadMain(uint32_t generation) {
  auto* kernel = REX_KERNEL_STATE();
  auto* dispatcher = kernel->function_dispatcher();
  auto last_report = std::chrono::steady_clock::now();
  uint64_t last_underruns = 0;
  uint64_t last_played = 0;
  uint64_t stalled_at_played = ~0ull;
  auto stalled_since = std::chrono::steady_clock::now();
  while (true) {
    {
      std::unique_lock<std::mutex> lock(g_out.mutex);
      g_out.wake.wait_for(lock, std::chrono::milliseconds(100),
                          [] { return g_out.credits > 0 || !g_out.running; });
      if (!g_out.running || g_out.generation != generation) {
        break;
      }
      if (g_out.credits <= 0) {
        // WATCHDOG: no block taken for 2 s while every credit is out = the
        // device stopped asking (a chosen device unplugged: SDL doesn't move
        // it to another). Open it again: by name if it's back, else the
        // system default. (The game's mixer keeps waiting meanwhile.)
        const uint64_t p = g_out.played.load(std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        if (p != stalled_at_played) {
          stalled_at_played = p;
          stalled_since = now;
        } else if (now - stalled_since > std::chrono::seconds(g_out.stream ? 2 : 10)) {
          // (No device at all: try again every 10 s, quietly after the first.)
          const bool had_stream = g_out.stream != nullptr;
          lock.unlock();
          if (had_stream) {
            REXLOG_WARN("Sound out: the device stopped taking sound; opening it again");
          }
          OpenDevice();
          stalled_since = now;
        }
        continue;
      }
      --g_out.credits;
    }
    uint64_t args[] = {g_out.arg_word};
    dispatcher->Execute(g_out.thread->thread_state(), g_out.callback, args, 1);

    // Once a minute: blocks played and GAPS (the speakers wanted a block the
    // game hadn't mixed yet = 5.33 ms of silence, heard as a click). A few at
    // start-up are normal; a steady stream = the game mixing slower than real
    // time.
    const auto now = std::chrono::steady_clock::now();
    if (now - last_report > std::chrono::seconds(60)) {
      const uint64_t p = g_out.played.load(std::memory_order_relaxed);
      const uint64_t u = g_out.underruns.load(std::memory_order_relaxed);
      REXLOG_INFO("Sound out: last minute {} blocks played, {} gaps of silence", p - last_played,
                  u - last_underruns);
      last_played = p;
      last_underruns = u;
      last_report = now;
    }
  }
  return 0;
}

bool Register(uint32_t callback, uint32_t arg) {
  auto* kernel = REX_KERNEL_STATE();
  auto* memory = kernel->memory();
  g_out.callback = callback;
  g_out.arg_word = memory->SystemHeapAlloc(4);
  uint8_t* word = GuestPtr(memory->virtual_membase(), g_out.arg_word);
  word[0] = uint8_t(arg >> 24);
  word[1] = uint8_t(arg >> 16);
  word[2] = uint8_t(arg >> 8);
  word[3] = uint8_t(arg);
  uint32_t generation;
  {
    std::lock_guard<std::mutex> lock(g_out.mutex);
    g_out.credits = std::clamp(REXCVAR_GET(sound_out_buffer_blocks), 3, 32);
    g_out.running = true;
    generation = ++g_out.generation;
  }
  if (!OpenDevice()) {
    // No device: keep the game's mixer turning anyway (it waits for us), at
    // real-time speed, into nothing. Handled by DeviceWants never running:
    // credits stay used up -> the game simply stops being asked. That's what
    // a console without speakers would do too.
  }
  g_out.thread = rex::system::object_ref<rex::system::XHostThread>(
      new rex::system::XHostThread(kernel, 128 * 1024, 0, [generation] { return ThreadMain(generation); }));
  g_out.thread->set_name("Sound Out");
  g_out.thread->Create();
  REXLOG_INFO("Sound out: the game's mixer (callback {:08X}) now plays through the port's own "
              "output",
              callback);
  return true;
}

void Unregister() {
  {
    std::lock_guard<std::mutex> lock(g_out.mutex);
    g_out.running = false;
  }
  g_out.wake.notify_all();
  CloseDevice();
  // The thread ends on its own at its next wake-up (<= 100 ms); it's never
  // waited for here because this runs on a game thread that may hold locks
  // the callback needs.
  std::lock_guard<std::mutex> lock(g_out.mutex);
  g_out.queue.clear();
  REXLOG_INFO("Sound out: the game closed its mixer");
}

void Submit(uint32_t frame) {
  auto* memory = REX_KERNEL_STATE()->memory();
  const uint8_t* src = GuestPtr(memory->virtual_membase(), frame);
  Block block;
  for (uint32_t i = 0; i < kBlockFloats; ++i) {
    const uint32_t bits = Be32(src + i * 4);
    std::memcpy(&block[i], &bits, sizeof(float));
  }
  std::lock_guard<std::mutex> lock(g_out.mutex);
  g_out.queue.push_back(block);
}

}  // namespace

std::vector<std::string> PlaybackDevices() {
  std::vector<std::string> names;
  if (!SDL_WasInit(SDL_INIT_AUDIO)) return names;
  for (const auto& d : ListDevices()) names.push_back(d.second);
  return names;
}

std::string CurrentDevice() {
  std::lock_guard<std::recursive_mutex> lock(g_device_mutex);
  return g_out.device_name;
}

void Reopen() {
  std::lock_guard<std::recursive_mutex> lock(g_device_mutex);
  if (!g_out.running) return;   // not playing yet: the settings apply at the start
  OpenDevice();
}

Stats GetStats() {
  Stats s;
  s.played = g_out.played.load(std::memory_order_relaxed);
  s.underruns = g_out.underruns.load(std::memory_order_relaxed);
  s.device_channels = g_out.device_channels;
  std::lock_guard<std::mutex> lock(g_out.mutex);
  s.queued = int(g_out.queue.size());
  return s;
}

}  // namespace sound_out

// -----------------------------------------------------------------------------
// The manifest's SOUND OUT midasm hooks. Each returns true = "handled, jump
// over the kernel call" (with r3 = 0 = success where the game checks it).
// With --sound_out_native=false they return false and the SDK answers.
// The flag is read once, at the game's register call: switching it later
// needs a restart (a handle can't change owners).
// -----------------------------------------------------------------------------
namespace {
bool g_ours = false;   // our handle was given out
}

// 0x824817C4 (sub_82481768): r3 = {callback, argument}, r4 = handle out.
bool CrashMomSoundRegister(PPCRegister& r3, PPCRegister& r4) {
  if (!REXCVAR_GET(sound_out_native)) {
    return false;
  }
  auto* memory = REX_KERNEL_STATE()->memory();
  uint8_t* membase = memory->virtual_membase();
  const uint8_t* pair = GuestPtr(membase, r3.u32);
  const uint32_t callback = sound_out::Be32(pair);
  const uint32_t arg = sound_out::Be32(pair + 4);
  if (!callback) {
    r3.u32 = 0x80070057;   // E_INVALIDARG, like the kernel
    return true;
  }
  sound_out::Register(callback, arg);
  g_ours = true;
  uint8_t* out = GuestPtr(membase, r4.u32);
  const uint32_t h = sound_out::kHandle;
  out[0] = uint8_t(h >> 24);
  out[1] = uint8_t(h >> 16);
  out[2] = uint8_t(h >> 8);
  out[3] = uint8_t(h);
  r3.u32 = 0;
  return true;
}

// 0x82481794 / 0x8248190C: r3 = the handle.
bool CrashMomSoundUnregister(PPCRegister& r3) {
  if (!g_ours || r3.u32 != sound_out::kHandle) {
    return false;
  }
  sound_out::Unregister();
  g_ours = false;
  r3.u32 = 0;
  return true;
}

// 0x824819F0 (sub_824819A8): r3 = the handle, r4 = the mixed block.
// Also feeds --debug_audio_dump (audio_trace.cpp), whichever path plays it.
void CrashMomAudioFrame(PPCRegister& r4);
bool CrashMomSoundSubmit(PPCRegister& r3, PPCRegister& r4) {
  CrashMomAudioFrame(r4);
  if (!g_ours || r3.u32 != sound_out::kHandle) {
    return false;
  }
  sound_out::Submit(r4.u32);
  return true;
}

// 0x8247F0C0 (sub_8247F088, before every block): r3 = handle, r4 = mask out.
// "No category volume changed": mask 0, success.
bool CrashMomSoundVolumeChanges(PPCRegister& r3, PPCRegister& r4) {
  if (!g_ours || r3.u32 != sound_out::kHandle) {
    return false;
  }
  uint8_t* out = GuestPtr(REX_KERNEL_STATE()->memory()->virtual_membase(), r4.u32);
  std::memset(out, 0, 4);
  r3.u32 = 0;
  return true;
}

// 0x8247F0F0: r3 = category, r4 = float out. Only reached for a set mask bit
// (never, given the answer above); 1.0 = full volume, as the SDK answered.
bool CrashMomSoundCategoryVolume(PPCRegister& r3, PPCRegister& r4) {
  if (!g_ours) {
    return false;
  }
  uint8_t* out = GuestPtr(REX_KERNEL_STATE()->memory()->virtual_membase(), r4.u32);
  out[0] = 0x3F; out[1] = 0x80; out[2] = 0; out[3] = 0;   // 1.0f, big-endian
  r3.u32 = 0;
  return true;
}
