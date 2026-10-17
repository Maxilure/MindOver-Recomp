// =============================================================================
// png_writer.cpp -- see png_writer.h for the format in brief
// =============================================================================

#include "png_writer.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

namespace {

// CRC-32 (the PNG/zip polynomial, reflected 0xEDB88320), table-driven.
uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      }
      t[n] = c;
    }
    return t;
  }();
  crc = ~crc;
  for (size_t i = 0; i < size; ++i) {
    crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return ~crc;
}

void PutBigEndian32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v >> 24));
  out.push_back(uint8_t(v >> 16));
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v));
}

// One chunk: length, type, data, CRC-32 of type + data.
void AppendChunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data) {
  const size_t start = out.size();
  PutBigEndian32(out, uint32_t(data.size()));
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  PutBigEndian32(out, Crc32(out.data() + start + 4, data.size() + 4));
}

}  // namespace

std::vector<uint8_t> EncodePng(const uint8_t* pixels, uint32_t width, uint32_t height, size_t stride,
                               bool alpha) {
  // The raw image data: each row = filter byte 0 ("none") + R, G, B (, A) per pixel.
  const size_t channels = alpha ? 4 : 3;
  const size_t row_bytes = 1 + size_t(width) * channels;
  std::vector<uint8_t> raw(row_bytes * height);
  for (uint32_t y = 0; y < height; ++y) {
    uint8_t* dst = raw.data() + y * row_bytes;
    const uint8_t* src = pixels + y * stride;
    dst[0] = 0;
    for (uint32_t x = 0; x < width; ++x) {
      for (size_t c = 0; c < channels; ++c) dst[1 + x * channels + c] = src[x * 4 + c];
    }
  }
  // The zlib stream: header (0x78 0x01: deflate, 32 KB window, no preset
  // dictionary), stored blocks of up to 65535 bytes (1 byte "final block?" +
  // type 00, LEN and its complement NLEN, little-endian, then the bytes),
  // and the Adler-32 checksum of the raw data, big-endian.
  std::vector<uint8_t> idat{0x78, 0x01};
  idat.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
  uint32_t adler_a = 1, adler_b = 0;
  for (size_t pos = 0; pos < raw.size() || raw.empty();) {
    const size_t len = std::min<size_t>(raw.size() - pos, 65535);
    const bool last = pos + len == raw.size();
    idat.push_back(last ? 1 : 0);
    idat.push_back(uint8_t(len));
    idat.push_back(uint8_t(len >> 8));
    idat.push_back(uint8_t(~len));
    idat.push_back(uint8_t(~len >> 8));
    idat.insert(idat.end(), raw.begin() + pos, raw.begin() + pos + len);
    for (size_t i = pos; i < pos + len; ++i) {
      adler_a = (adler_a + raw[i]) % 65521;
      adler_b = (adler_b + adler_a) % 65521;
    }
    pos += len;
    if (last) break;
  }
  PutBigEndian32(idat, (adler_b << 16) | adler_a);

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  PutBigEndian32(ihdr, width);
  PutBigEndian32(ihdr, height);
  // 8 bits, colour type 6 (RGBA) or 2 (RGB), deflate, filter 0, no interlace
  ihdr.insert(ihdr.end(), {8, uint8_t(alpha ? 6 : 2), 0, 0, 0});
  AppendChunk(png, "IHDR", ihdr);
  AppendChunk(png, "IDAT", idat);
  AppendChunk(png, "IEND", {});
  return png;
}

bool WritePng(const std::filesystem::path& path, const uint8_t* pixels, uint32_t width,
              uint32_t height, size_t stride) {
  const std::vector<uint8_t> png = EncodePng(pixels, width, height, stride, false);
  std::FILE* f = std::fopen(path.string().c_str(), "wb");
  if (!f) {
    return false;
  }
  std::fwrite(png.data(), 1, png.size(), f);
  const bool ok = std::ferror(f) == 0;
  std::fclose(f);
  return ok;
}
