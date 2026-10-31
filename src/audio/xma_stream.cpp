// =============================================================================
// audio/xma_stream.cpp -- XMA2 decoding in software (see xma_stream.h)
// =============================================================================
//
// HOW AN XMA2 SOUND IS LAID OUT (what the Xbox's decoder chip walked through;
// written down plainly from the SDK's chip emulation, src/audio/xma_context.cpp
// in thirdparty/rexglue-sdk, and checked against the game's sounds)
//
//   * The data is BLOCKS (64 KB in this game) of 2048-byte PACKETS. Every
//     block starts each stream afresh, so a seek can land on any block.
//   * A packet = 4-byte header + 2044 bytes of payload. Header bits:
//       6 frame count | 15 bit offset of the FIRST frame that starts in this
//       packet (from the end of the header; 0x7FFF = none) | 3 metadata |
//       8 SKIP count.
//   * A sound with several streams (5.1 = three stereo streams: front,
//     centre + LFE, back) interleaves their packets: every block starts with
//     stream k's first packet at packet k, and a packet's skip count says how
//     many packets of OTHER streams come before this stream's next one
//     (0xFF = none in this block).
//   * A stream's payloads, back to back, are a bit stream of FRAMES: a 15-bit
//     length (in bits, counting itself), the frame, and the frame's LAST bit
//     = "another frame starts in this same packet". When it's 0, the next
//     frame starts at the next packet's first-frame offset (a frame can run
//     on into the next packet; the rest of a packet can be padding).
//   * One frame = 512 samples per channel. FFmpeg's frame decoder (the
//     "xmaframes" decoder of the SDK's FFmpeg) gets one frame at a time,
//     bit-aligned as it sat, behind one byte that says how many padding bits
//     come before and after it (the SDK's PreparePacket).
//   * The decoder's output runs 192 samples AHEAD of the bitstream's sample
//     numbering, so frame n's samples are decoded block n [192..512) plus
//     decoded block n+1 [0..192). At the end of the sound the last tail goes
//     out with 192 samples of silence (the codec has no flush for it; the SDK
//     does the same, findings/04): the sample count then matches the chip's.
//   * Samples: float x 32767, rounded, saturated to int16 (the SDK's
//     ConvertFrame), so both decoders give the same numbers.
// =============================================================================

#include "audio/xma_stream.h"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include "libavcodec/avcodec.h"
}

namespace xma {
namespace {

constexpr uint32_t kPacketBytes = 2048;
constexpr uint32_t kPacketHeaderBytes = 4;
constexpr uint32_t kPayloadBits = (kPacketBytes - kPacketHeaderBytes) * 8;
constexpr uint32_t kStartPadding = 192;
constexpr uint32_t kNoFrame = 0x7FFF;

uint32_t PeekBits(const std::vector<uint8_t>& d, uint64_t bit, uint32_t count) {
  uint32_t v = 0;
  for (uint32_t i = 0; i < count; ++i, ++bit) {
    v = (v << 1) | ((d[bit >> 3] >> (7 - (bit & 7))) & 1);
  }
  return v;
}

const AVCodec* Codec() {
  static const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
  return codec;
}

}  // namespace

bool Stream::Init(uint32_t streams, const int* channels, uint32_t rate, uint32_t block_size,
                  std::function<bool(std::vector<uint8_t>&)> read_block) {
  if (!Codec() || streams == 0 || streams > kMaxStreams || block_size < kPacketBytes) {
    return false;
  }
  streams_ = streams;
  // The file's own rate (e.g. 47994 for a 48 kHz cutscene track): the
  // decoder's frame layout depends on it, and with it the output matches the
  // SDK's chip emulation sample for sample (rounding it to the chip's four
  // rates changed 12 of 97 sounds, 2026-10-31).
  rate_ = rate;
  block_size_ = block_size;
  read_block_ = std::move(read_block);
  for (uint32_t k = 0; k < streams; ++k) sub_[k].channels = std::clamp(channels[k], 1, 2);
  packet_ = av_packet_alloc();
  frame_ = av_frame_alloc();
  frame_bytes_.assign(2 + kPacketBytes * 4, 0);
  return packet_ && frame_;
}

Stream::~Stream() {
  for (Sub& st : sub_) {
    if (st.codec) avcodec_free_context(&st.codec);
  }
  av_packet_free(&packet_);
  av_frame_free(&frame_);
}

bool Stream::OpenCodec(Sub& st) {
  st.codec = avcodec_alloc_context3(Codec());
  if (!st.codec) return false;
  st.codec->sample_rate = int(rate_);
  st.codec->channels = st.channels;
  st.codec->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;   // as the SDK opens it
  if (avcodec_open2(st.codec, Codec(), nullptr) < 0) {
    avcodec_free_context(&st.codec);
    return false;
  }
  return true;
}

void Stream::Restart(uint64_t block, uint32_t drop) {
  blocks_.clear();
  first_block_ = block;
  blocks_read_ = block;
  input_ended_ = false;
  for (uint32_t k = 0; k < streams_; ++k) {
    Sub& st = sub_[k];
    st.block = block;
    st.in_block = false;
    st.payload.clear();
    st.first.clear();
    st.out.clear();
    st.carry_valid = false;
    st.flushed = false;
    st.drop = drop;
    if (st.codec) avcodec_flush_buffers(st.codec);
  }
}

bool Stream::ReadBlock() {
  if (input_ended_) return false;
  std::vector<uint8_t> b;
  if (!read_block_ || !read_block_(b) || b.empty()) {
    input_ended_ = true;
    return false;
  }
  blocks_.push_back(std::move(b));
  ++blocks_read_;
  return true;
}

// Gather stream k's packet payloads of its current block.
bool Stream::EnterBlock(uint32_t k) {
  Sub& st = sub_[k];
  if (st.block < first_block_ || st.block - first_block_ >= blocks_.size()) return false;
  const std::vector<uint8_t>& b = blocks_[st.block - first_block_];
  const uint32_t packets = uint32_t(b.size() / kPacketBytes);
  st.payload.clear();
  st.first.clear();
  for (uint32_t p = k; p < packets;) {
    const uint8_t* h = &b[size_t(p) * kPacketBytes];
    st.first.push_back((uint32_t(h[0] & 3) << 13) | (uint32_t(h[1]) << 5) | (h[2] >> 3));
    st.payload.insert(st.payload.end(), h + kPacketHeaderBytes, h + kPacketBytes);
    const uint8_t skip = h[3];
    if (skip == 0xFF) break;
    p += uint32_t(skip) + 1;
  }
  st.payload_bits = uint64_t(st.payload.size()) * 8;
  st.bit = st.payload_bits;
  for (size_t i = 0; i < st.first.size(); ++i) {
    if (st.first[i] < kPayloadBits) {
      st.bit = uint64_t(i) * kPayloadBits + st.first[i];
      break;
    }
  }
  st.in_block = true;
  return true;
}

void Stream::Emit(Sub& st, const int16_t* decoded) {
  const uint32_t ch = uint32_t(st.channels);
  if (st.carry_valid) {
    st.out.insert(st.out.end(), st.carry.begin(),
                  st.carry.begin() + (kFrameSamples - kStartPadding) * ch);
    st.out.insert(st.out.end(), decoded, decoded + kStartPadding * ch);
  }
  std::memcpy(st.carry.data(), decoded + kStartPadding * ch,
              (kFrameSamples - kStartPadding) * ch * sizeof(int16_t));
  st.carry_valid = true;
}

bool Stream::DecodeNext(uint32_t k) {
  Sub& st = sub_[k];
  while (true) {
    if (!st.in_block) {
      if (!EnterBlock(k)) {
        if (ReadBlock()) continue;
        // The end: the last tail + 192 frames of silence, once.
        if (!st.flushed && st.carry_valid) {
          const uint32_t ch = uint32_t(st.channels);
          st.out.insert(st.out.end(), st.carry.begin(),
                        st.carry.begin() + (kFrameSamples - kStartPadding) * ch);
          st.out.insert(st.out.end(), kStartPadding * ch, int16_t(0));
          st.carry_valid = false;
        }
        st.flushed = true;
        return false;
      }
    }
    if (st.bit + 15 > st.payload_bits) {   // this block is done for stream k
      ++st.block;
      st.in_block = false;
      ForgetPassedBlocks();
      continue;
    }
    const uint32_t length = PeekBits(st.payload, st.bit, 15);
    if (length < 16 || length == kNoFrame || st.bit + length > st.payload_bits ||
        length > (frame_bytes_.size() - 2) * 8) {
      ++st.block;
      st.in_block = false;
      ForgetPassedBlocks();
      continue;
    }
    // The frame as it sits, behind the padding byte.
    const uint32_t pad_start = uint32_t(st.bit & 7);
    const uint64_t first_byte = st.bit >> 3;
    const uint32_t bytes = (pad_start + length + 7) / 8;
    std::fill(frame_bytes_.begin(), frame_bytes_.begin() + bytes + 2, 0);
    std::memcpy(frame_bytes_.data() + 1, &st.payload[first_byte],
                std::min<size_t>(bytes, st.payload.size() - first_byte));
    const uint32_t pad_end = bytes * 8 - (pad_start + length);
    frame_bytes_[0] = uint8_t(((pad_start & 7) << 5) | ((pad_end & 7) << 2));

    const bool more_in_packet = PeekBits(st.payload, st.bit + length - 1, 1) != 0;
    const uint64_t start_packet = st.bit / kPayloadBits;
    const uint64_t end = st.bit + length;
    st.bit = end;
    if (!more_in_packet) {
      // Next frame: the first one starting in a later packet, at or after
      // where this one ended.
      st.bit = st.payload_bits;
      for (uint64_t q = start_packet + 1; q < st.first.size(); ++q) {
        if (st.first[q] < kPayloadBits && q * kPayloadBits + st.first[q] >= end) {
          st.bit = q * kPayloadBits + st.first[q];
          break;
        }
      }
    }

    if (!st.codec && !OpenCodec(st)) return false;
    packet_->data = frame_bytes_.data();
    packet_->size = int(1 + bytes);
    if (avcodec_send_packet(st.codec, packet_) >= 0 &&
        avcodec_receive_frame(st.codec, frame_) >= 0) {
      const uint32_t ch = uint32_t(st.channels);
      int16_t decoded[kFrameSamples * 2];
      for (uint32_t c = 0; c < ch; ++c) {
        const float* in = reinterpret_cast<const float*>(frame_->data[c]);
        for (uint32_t i = 0; i < kFrameSamples; ++i) {
          const float v = std::nearbyint(in[i] * 32767.0f);
          decoded[i * ch + c] = int16_t(std::clamp(v, -32768.0f, 32767.0f));
        }
      }
      Emit(st, decoded);
      return true;
    }
    // A frame the decoder refused: the carry can't be spliced across it.
    st.carry_valid = false;
    ++errors_;
  }
}

void Stream::ForgetPassedBlocks() {
  uint64_t oldest = blocks_read_;
  for (uint32_t k = 0; k < streams_; ++k) oldest = std::min(oldest, sub_[k].block);
  while (first_block_ < oldest && !blocks_.empty()) {
    blocks_.pop_front();
    ++first_block_;
  }
}

uint32_t Stream::Ready(uint32_t k, uint32_t want) {
  Sub& st = sub_[k];
  const uint32_t ch = uint32_t(st.channels);
  while (true) {
    if (st.drop) {
      const uint32_t n = std::min<uint32_t>(st.drop, uint32_t(st.out.size() / ch));
      st.out.erase(st.out.begin(), st.out.begin() + size_t(n) * ch);
      st.drop -= n;
    }
    if ((st.drop == 0 && st.out.size() / ch >= want) || st.flushed) break;
    // false + not at the end = the decoder couldn't open: give up for now.
    if (!DecodeNext(k) && !st.flushed) break;
  }
  return st.drop ? 0 : uint32_t(st.out.size() / ch);
}

void Stream::Take(uint32_t k, int16_t* out, uint32_t frames) {
  Sub& st = sub_[k];
  const size_t n = size_t(frames) * uint32_t(st.channels);
  std::copy(st.out.begin(), st.out.begin() + n, out);
  st.out.erase(st.out.begin(), st.out.begin() + n);
}

}  // namespace xma
