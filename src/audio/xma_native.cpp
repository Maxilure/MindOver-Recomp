// =============================================================================
// audio/xma_native.cpp -- the game's compressed sounds unpacked by OUR code
// =============================================================================
//
// WHAT THIS REPLACES (docs/findings/54 section 5)
// -----------------------------------------------
// Every sound on the disc is XMA (the Xbox 360's compressed format, a WMA Pro
// variant). The console had a decoder CHIP for it; the SDK emulates that chip
// (thirdparty/rexglue-sdk src/audio/xma_*). In this game the ONLY user of the
// chip is one Radical class, audio::hal::hw::XmaDecoder (vtable 0x8200A9E8,
// a "Stream" whose Read hands out 16-bit PCM). It reads the compressed sound
// from an inner stream (RsdFileStream, the .rsd file) block by block, feeds
// the chip, and copies what the chip produced into its own rings:
//
//   constructor sub_8234BE88    reads the header, allocates, CREATES THE CHIP
//                               CONTEXT (sub_8247D4F0 at 0x8234C090)
//   destructor  sub_8234BC58    frees (incl. sub_8247D6F0 = release context)
//   Read        sub_8234C8A8    seek if pending, then "fill N frames"
//   fill        sub_8234C580    loop: pull, feed, copy rings -> caller's
//                               buffer, else busy-wait 50 us and retry
//   feed        sub_8234C220    read the next block into a chip input buffer
//   pull        sub_8234C398    copy the chip's decoded samples into the rings
//   seek        sub_8234C740    seek table -> block, reset chip, skip samples
//
// Here feed / pull / seek are rewritten: the block goes to a SOFTWARE decoder
// (FFmpeg's single-XMA-frame decoder, the same one the SDK's chip emulation
// uses), which writes straight into the object's rings. The chip context is
// never created (manifest midasm CrashMomXmaNoContext), so no lock / kick /
// clear registers, no emulated chip, no XMACreateContext/XMAReleaseContext.
// The fill loop and Read stay the game's own code.
//
// THE OBJECT (offsets found by decompiling the methods above, Ghidra)
//   +0x04 inner stream (RsdFileStream*): vtable slot 5 (+0x14) Seek(offset),
//         slot 6 (+0x18) Read(buffer, 1, bytes)
//   +0x08 seek table: u32 per block = sample count at that block's END
//   +0x14 H = the sound's XMA header:
//         H+1 (u8) stream count     H+0x0C sample rate
//         H+0x04 lead-in samples    H+0x18 block size (bytes)
//         H+0x08 end sample         H+0x24 seek table entries
//         H+0x28+4k (u8) channels of stream k (1 or 2)
//   +0x1C compressed data size      +0x24 data offset in the file
//   +0x28 input buffers (2 x block size, physical memory)
//   +0x30 next input buffer         +0x34 "first block after a seek"
//   +0x38 bytes fed so far          +0x3C rings: 0x18 bytes per stream:
//         +0 ring (8192 frames of that stream's channels, BIG-ENDIAN int16),
//         +4 frames written in total, +8 write position (frames, & 0x1FFF),
//         +0xC frames read in total,  +0x10 read position, +0x14 (fill's count)
//   +0x40 position (frames handed out) +0x44 total channels
//   +0x48 (u8) seek pending
//
// The decoding itself (the XMA2 format, frames, the 192-sample realignment)
// is the game-independent core in xma_stream.{h,cpp}; this file is the glue
// to the game's object.
//
// THE ORIGINAL'S RING BUG (findings/54 s.6). The game's pull computes the
// room in a ring as "8192" whenever the ring is EMPTY, wherever its write
// position is: with the write position at 7000 and 2000 decoded frames
// ready, it copies 7000..9000, i.e. ~800 frames PAST THE END of the ring's
// heap block, and the reader then finds stale samples at 0..800. It needs
// an empty ring and more ready frames than are left before the end; in our
// A/B runs (the SDK's chip) it never happened (outputs identical), but it's
// a latent memory overwrite in the audio heap. Here the room is the free
// space up to the ring's end, always.
//
// Off with --xma_native=false (read when each sound object is created): the
// game's own code and the SDK's chip, as before.
// =============================================================================

#include "audio/xma_stream.h"
#include "guest_memory.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_BOOL(xma_native, true, "CrashMoM",
                    "Unpack the game's XMA sounds with the port's own decoder (off = the SDK's "
                    "emulated Xbox sound chip, as before)");

extern "C" REX_FUNC(__imp__sub_8234C220);
extern "C" REX_FUNC(__imp__sub_8234C398);
extern "C" REX_FUNC(__imp__sub_8234C740);
extern "C" REX_FUNC(__imp__sub_8234BC58);
extern "C" REX_FUNC(__imp__sub_8247D6F0);

namespace xma_native {
namespace {

constexpr uint32_t kRingFrames = 0x2000;   // the game's ring: 8192 frames

uint8_t* g_base = nullptr;
uint8_t* Ptr(uint32_t a) { return GuestPtr(g_base, a); }
uint32_t R32(uint32_t a) {
  const uint8_t* p = Ptr(a);
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint8_t R8(uint32_t a) { return *Ptr(a); }
void W32(uint32_t a, uint32_t v) {
  uint8_t* p = Ptr(a);
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

// Call a game function at `address` (a vtable slot) with r3-r6, keeping the
// caller's registers (the pattern of ui/scrooby.cpp Call).
uint32_t CallGuest(PPCContext& ctx, uint8_t* base, uint32_t address, uint32_t r3, uint32_t r4,
                   uint32_t r5 = 0, uint32_t r6 = 0) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  ctx.r6.u64 = r6;
  rex::runtime::ResolveIndirectFunction(address)(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

struct Sound {
  uint32_t object = 0;
  xma::Stream stream;
  // The game thread's registers while one of our overrides runs: the core
  // asks for blocks only from inside Ready(), i.e. during our pull.
  PPCContext* ctx = nullptr;
  uint8_t* base = nullptr;
};

std::mutex g_mutex;
std::unordered_map<uint32_t, std::unique_ptr<Sound>> g_sounds;
std::atomic<uint64_t> g_created{0};
std::atomic<uint32_t> g_errors_logged{0};

Sound* Find(uint32_t object) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_sounds.find(object);
  return it == g_sounds.end() ? nullptr : it->second.get();
}

// The next block, read like the original feed: through the inner stream
// (RsdFileStream vtable slot 6) into the object's own input buffer, copied out.
bool ReadBlock(Sound& s, std::vector<uint8_t>& out) {
  const uint32_t o = s.object;
  const uint32_t data_size = R32(o + 0x1C);
  const uint32_t fed = R32(o + 0x38);
  if (fed >= data_size || !s.ctx) return false;
  const uint32_t block_size = R32(R32(o + 0x14) + 0x18);
  const uint32_t n = std::min(data_size - fed, block_size);
  const uint32_t current = R32(o + 0x30);
  const uint32_t buffer = R32(o + 0x28) + block_size * current;
  const uint32_t inner = R32(o + 0x04);
  CallGuest(*s.ctx, s.base, R32(R32(inner) + 0x18), inner, buffer, 1, n);
  out.assign(Ptr(buffer), Ptr(buffer) + n);
  W32(o + 0x38, fed + n);
  W32(o + 0x30, (current + 1) % std::max<uint32_t>(1, R32(o + 0x2C)));
  W32(o + 0x34, 0);
  return true;
}

}  // namespace
}  // namespace xma_native

using namespace xma_native;

// ---------------------------------------------------------------------------
// Constructor: midasm hook at 0x8234C090 (bl sub_8247D4F0 = "create the chip
// context") in sub_8234BE88, r31 = the object, header already read. Returns
// true = jump over the call: the object's context (+0x20) stays 0 and our
// decoder takes it.
// ---------------------------------------------------------------------------
bool CrashMomXmaNoContext(PPCRegister& r31) {
  if (!REXCVAR_GET(xma_native)) return false;
  g_base = rex::system::kernel_memory()->virtual_membase();
  const uint32_t o = r31.u32;
  const uint32_t h = R32(o + 0x14);
  const uint32_t streams = R8(h + 1);
  int channels[xma::Stream::kMaxStreams] = {};
  for (uint32_t k = 0; k < std::min(streams, xma::Stream::kMaxStreams); ++k) {
    channels[k] = R8(h + 0x28 + 4 * k);
  }
  auto s = std::make_unique<Sound>();
  s->object = o;
  Sound* raw = s.get();
  if (!s->stream.Init(streams, channels, R32(h + 0x0C), R32(h + 0x18),
                      [raw](std::vector<uint8_t>& b) { return ReadBlock(*raw, b); })) {
    REXLOG_ERROR("XMA (native): couldn't set up a sound ({} streams, {} Hz, blocks of {}); "
                 "the emulated chip takes it",
                 streams, R32(h + 0x0C), R32(h + 0x18));
    return false;
  }
  if (g_created.fetch_add(1) == 0) {
    REXLOG_INFO("XMA (native): sounds now unpacked by the port's own decoder (first: {} "
                "stream(s), {} Hz, blocks of {} bytes)",
                streams, R32(h + 0x0C), R32(h + 0x18));
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_sounds[o] = std::move(s);
  return true;
}

// Release of a context that was never created: nothing to do.
extern "C" REX_FUNC(sub_8247D6F0) {
  if (ctx.r3.u32 == 0) return;
  __imp__sub_8247D6F0(ctx, base);
}

// Destructor: forget our decoder, then the game frees its buffers.
extern "C" REX_FUNC(sub_8234BC58) {
  std::unique_ptr<Sound> gone;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_sounds.find(ctx.r3.u32);
    if (it != g_sounds.end()) {
      gone = std::move(it->second);
      g_sounds.erase(it);
    }
  }
  if (gone && gone->stream.errors() && g_errors_logged.fetch_add(1) < 5) {
    REXLOG_WARN("XMA (native): a sound had {} frame(s) the decoder refused",
                gone->stream.errors());
  }
  gone.reset();
  __imp__sub_8234BC58(ctx, base);
}

// Feed: our pull reads blocks itself, exactly when a stream needs one.
// Returns 1 = "nothing fed", like the original when its buffers are busy.
extern "C" REX_FUNC(sub_8234C220) {
  if (!Find(ctx.r3.u32)) {
    __imp__sub_8234C220(ctx, base);
    return;
  }
  ctx.r3.u64 = 1;
}

// Pull: fill each stream's ring with decoded frames, up to its free room.
// Returns 0 if anything was added, 1 if not (as the original).
extern "C" REX_FUNC(sub_8234C398) {
  const uint32_t o = ctx.r3.u32;
  Sound* s = Find(o);
  if (!s) {
    __imp__sub_8234C398(ctx, base);
    return;
  }
  g_base = base;
  s->ctx = &ctx;
  s->base = base;
  bool added = false;
  const uint32_t rings = R32(o + 0x3C);
  std::vector<int16_t> frames;
  for (uint32_t k = 0; k < s->stream.streams(); ++k) {
    const uint32_t ch = uint32_t(s->stream.channels(k));
    const uint32_t r = rings + 0x18 * k;
    while (true) {
      const uint32_t written = R32(r + 4), read = R32(r + 0xC);
      const uint32_t used = written - read;
      if (used >= kRingFrames) break;
      const uint32_t wpos = R32(r + 8) & (kRingFrames - 1);
      // THE FIX (findings/54 s.6): room = the free space, only up to the
      // ring's end. The original says 8192 for any EMPTY ring, wherever its
      // write position is, and could write past the ring's end.
      const uint32_t room = std::min(kRingFrames - used, kRingFrames - wpos);
      const uint32_t n = std::min(room, s->stream.Ready(k, room));
      if (n == 0) break;
      frames.resize(size_t(n) * ch);
      s->stream.Take(k, frames.data(), n);
      uint8_t* dst = Ptr(R32(r) + wpos * ch * 2);
      for (size_t i = 0; i < frames.size(); ++i) {
        const uint16_t v = uint16_t(frames[i]);
        dst[i * 2] = uint8_t(v >> 8);   // big-endian, as the chip wrote
        dst[i * 2 + 1] = uint8_t(v);
      }
      W32(r + 4, written + n);
      W32(r + 8, (wpos + n) & (kRingFrames - 1));
      added = true;
    }
  }
  s->ctx = nullptr;
  ctx.r3.u64 = added ? 0 : 1;
}

// Seek (when +0x48 is set; the first Read always seeks): the original's seek
// with our decoder in place of the chip. target = lead-in + position; the
// seek table (+0x08: sample count at each block's END) gives the block and
// the frames to drop inside it (sub_8234BDC8).
extern "C" REX_FUNC(sub_8234C740) {
  const uint32_t o = ctx.r3.u32;
  Sound* s = Find(o);
  if (!s) {
    __imp__sub_8234C740(ctx, base);
    return;
  }
  g_base = base;
  if (R8(o + 0x48) == 0) return;
  *Ptr(o + 0x48) = 0;
  const uint32_t h = R32(o + 0x14);
  const uint32_t target = R32(h + 4) + R32(o + 0x40);
  // The original leaves the block index unset (an old stack value) when the
  // target is past every entry; here it's the last block.
  const uint32_t entries = R32(h + 0x24);
  const uint32_t table = R32(o + 0x08);
  uint32_t block = entries ? entries - 1 : 0;
  for (uint32_t i = 0; i < entries; ++i) {
    if (target < R32(table + 4 * i)) {
      block = i;
      break;
    }
  }
  const uint32_t skip = block ? target - R32(table + 4 * (block - 1)) : target;
  const uint32_t block_size = R32(h + 0x18);

  s->stream.Restart(block, skip);
  W32(o + 0x34, 1);
  W32(o + 0x38, block_size * block);
  const uint32_t inner = R32(o + 0x04);
  CallGuest(ctx, base, R32(R32(inner) + 0x14), inner, R32(o + 0x24) + block_size * block);
  // The rings start empty at the target (the original counts the dropped
  // frames through them; the totals end up the same).
  const uint32_t rings = R32(o + 0x3C);
  for (uint32_t k = 0; k < s->stream.streams(); ++k) {
    const uint32_t r = rings + 0x18 * k;
    W32(r + 4, target);
    W32(r + 0xC, target);
    W32(r + 0x10, R32(r + 8));
  }
}
