// =============================================================================
// tools/xma_test/xma_test.cpp -- decode one of the game's .rsd sounds with the
// port's XMA decoder core (src/audio/xma_stream.*), outside the game
// =============================================================================
//
// Usage: xma_test <file.rsd> <out_prefix>
//   -> <out_prefix>_s<k>.raw per stream: native 16-bit samples, interleaved
//      (1 or 2 channels), from the very first sample (no lead-in dropped).
//
// Why: to check the decoder against an independent one (the system FFmpeg's
// full XMA2 decoder) on sounds the game rarely plays, e.g. the 5.1 cutscene
// tracks (findings/54 s.5). The game data stays on the player's disc:
// extract a file with an RCF tool into a temporary folder, never the repo.
//
// .rsd layout (findings/19; the header the game's XmaDecoder reads, findings/54):
//   0x000 "RSD6" "XMA " channels bits rate (LE) ... designer's .wav path
//   0x800 u32 header size, u32 seek table size, u32 data size (big-endian)
//   0x80C header H (H+1 streams, H+0x0C rate, H+0x18 block size,
//         H+0x28+4k channels of stream k), then the seek table, then data.
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "audio/xma_stream.h"

static uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: xma_test <file.rsd> <out_prefix>\n");
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), {});
  if (d.size() < 0x80C || std::string(d.begin(), d.begin() + 4) != "RSD6") {
    std::fprintf(stderr, "not an RSD6 file\n");
    return 1;
  }
  const uint32_t hsize = Be32(&d[0x800]), tsize = Be32(&d[0x804]), dsize = Be32(&d[0x808]);
  const uint8_t* h = &d[0x80C];
  const uint32_t streams = h[1], rate = Be32(h + 0x0C), block = Be32(h + 0x18);
  int channels[xma::Stream::kMaxStreams] = {};
  for (uint32_t k = 0; k < streams && k < xma::Stream::kMaxStreams; ++k) channels[k] = h[0x28 + 4 * k];
  size_t pos = 0x80C + hsize + tsize;
  const size_t end = std::min(d.size(), pos + dsize);
  std::printf("%u stream(s), %u Hz, blocks of %u, %u data bytes, lead-in %u, end %u\n", streams,
              rate, block, dsize, Be32(h + 4), Be32(h + 8));

  xma::Stream x;
  if (!x.Init(streams, channels, rate, block, [&](std::vector<uint8_t>& b) {
        if (pos >= end) return false;
        const size_t n = std::min<size_t>(block, end - pos);
        b.assign(d.begin() + pos, d.begin() + pos + n);
        pos += n;
        return true;
      })) {
    std::fprintf(stderr, "decoder setup failed\n");
    return 1;
  }
  x.Restart(0, 0);
  for (uint32_t k = 0; k < streams; ++k) {
    std::string name = std::string(argv[2]) + "_s" + std::to_string(k) + ".raw";
    std::FILE* f = std::fopen(name.c_str(), "wb");
    uint64_t total = 0;
    std::vector<int16_t> buf;
    while (true) {
      const uint32_t n = x.Ready(k, 4096);
      if (n == 0) break;
      buf.resize(size_t(n) * channels[k]);
      x.Take(k, buf.data(), n);
      std::fwrite(buf.data(), sizeof(int16_t), buf.size(), f);
      total += n;
    }
    std::fclose(f);
    std::printf("stream %u: %d ch, %llu frames\n", k, channels[k], (unsigned long long)total);
  }
  std::printf("frames the decoder refused: %u\n", x.errors());
  return 0;
}
