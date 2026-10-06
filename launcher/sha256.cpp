// =============================================================================
// sha256.cpp -- see sha256.h (FIPS 180-4, straightforward)
// =============================================================================
#include "sha256.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

namespace sha256 {
namespace {

constexpr std::array<uint32_t, 64> kK = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

constexpr uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

// One 64-byte block into the running state.
void Block(std::array<uint32_t, 8>& h, const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 |
           p[4 * i + 3];
  }
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t t1 = k + s1 + ch + kK[i] + w[i];
    const uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = s0 + maj;
    k = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

}  // namespace

std::string OfFile(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return {};
  }
  std::array<uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                               0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::vector<uint8_t> buffer(1 << 20);
  uint64_t length = 0;
  std::vector<uint8_t> tail;  // the bytes after the last whole block
  while (in) {
    in.read(reinterpret_cast<char*>(buffer.data()), std::streamsize(buffer.size()));
    const size_t got = size_t(in.gcount());
    if (got == 0) break;
    length += got;
    size_t i = 0;
    for (; i + 64 <= got; i += 64) Block(h, buffer.data() + i);
    tail.assign(buffer.begin() + std::ptrdiff_t(i), buffer.begin() + std::ptrdiff_t(got));
    if (got < buffer.size()) break;
    // A full buffer of 1 MiB is a whole number of blocks: tail is empty here.
  }
  // Padding: 0x80, zeros, then the length in bits (big-endian), to a block end.
  tail.push_back(0x80);
  while (tail.size() % 64 != 56) tail.push_back(0);
  const uint64_t bits = length * 8;
  for (int i = 7; i >= 0; --i) tail.push_back(uint8_t(bits >> (8 * i)));
  for (size_t i = 0; i < tail.size(); i += 64) Block(h, tail.data() + i);
  char hex[65];
  for (int i = 0; i < 8; ++i) std::snprintf(hex + 8 * i, 9, "%08x", h[i]);
  return std::string(hex, 64);
}

}  // namespace sha256
