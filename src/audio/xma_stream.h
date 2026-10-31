// =============================================================================
// audio/xma_stream.h -- an XMA2 sound, decoded in software (no game, no chip)
// =============================================================================
//
// The decoding core of the port's XMA decoder: compressed blocks go in (from
// whatever reads them: the game's sound object in xma_native.cpp, or a file
// in tools/xma_test), 16-bit PCM per stream comes out. It knows nothing
// about guest memory, so it can be tested on its own against another
// decoder (tools/xma_test, docs/findings/54 s.5).
//
// The format, the frame rules and the 192-sample realignment are explained
// at the top of xma_stream.cpp.
// =============================================================================

#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

struct AVCodec;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;

namespace xma {

class Stream {
 public:
  static constexpr uint32_t kMaxStreams = 8;

  // `channels[k]` = 1 or 2 for each of `streams` streams; `rate` in Hz (the
  // file's own); `block_size`
  // = bytes per XMA2 block (each block starts every stream afresh).
  // `read_block` is asked for the next block when a stream needs one; it
  // fills the vector (the last block may be short) or returns false = the end.
  bool Init(uint32_t streams, const int* channels, uint32_t rate, uint32_t block_size,
            std::function<bool(std::vector<uint8_t>&)> read_block);
  ~Stream();

  // Start again at block `block` (the caller has moved its reader there) and
  // throw away the first `drop` frames of every stream (the seek's offset
  // inside the block).
  void Restart(uint64_t block, uint32_t drop);

  uint32_t streams() const { return streams_; }
  int channels(uint32_t k) const { return sub_[k].channels; }

  // Frames of stream k ready to take, decoding more until there are at least
  // `want` (or the sound ended).
  uint32_t Ready(uint32_t k, uint32_t want);
  // Copy `frames` ready frames of stream k (interleaved native int16) out.
  void Take(uint32_t k, int16_t* out, uint32_t frames);
  // Stream k is at the end (nothing more will come after what's ready).
  bool Ended(uint32_t k) const { return sub_[k].flushed; }

  uint32_t errors() const { return errors_; }

 private:
  static constexpr uint32_t kFrameSamples = 512;

  struct Sub {
    int channels = 2;
    AVCodecContext* codec = nullptr;
    uint64_t block = 0;              // absolute block index
    bool in_block = false;
    std::vector<uint8_t> payload;    // this stream's packet payloads of `block`
    std::vector<uint32_t> first;     // per payload packet: first frame's bit offset
    uint64_t bit = 0;                // next frame's start, bits into `payload`
    uint64_t payload_bits = 0;
    std::deque<int16_t> out;         // decoded, realigned, interleaved
    std::array<int16_t, kFrameSamples * 2> carry{};
    bool carry_valid = false;
    bool flushed = false;
    uint32_t drop = 0;
  };

  bool EnterBlock(uint32_t k);
  bool ReadBlock();
  bool DecodeNext(uint32_t k);
  void Emit(Sub& st, const int16_t* decoded);
  bool OpenCodec(Sub& st);
  void ForgetPassedBlocks();

  uint32_t streams_ = 0;
  uint32_t rate_ = 48000;
  uint32_t block_size_ = 0;
  std::function<bool(std::vector<uint8_t>&)> read_block_;
  std::deque<std::vector<uint8_t>> blocks_;
  uint64_t first_block_ = 0;   // absolute index of blocks_.front()
  uint64_t blocks_read_ = 0;   // absolute index of the next block to read
  bool input_ended_ = false;
  std::array<Sub, kMaxStreams> sub_;
  AVPacket* packet_ = nullptr;
  AVFrame* frame_ = nullptr;
  std::vector<uint8_t> frame_bytes_;
  uint32_t errors_ = 0;
};

}  // namespace xma
