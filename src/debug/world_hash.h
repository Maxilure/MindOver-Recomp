// =============================================================================
// world_hash.h -- debug console `worldhash`: a fingerprint of the game's world
// every game frame, to prove two runs stayed EXACTLY the same (or find the
// first frame and the first memory page where they didn't)
// =============================================================================
//
// WHY. `track` (debug_console.cpp) compares where the PLAYERS are. Two runs
// can agree on the players for minutes while something else already differs
// (an enemy's decision, a script timer, a random draw) and only reaches the
// players much later, far from the cause. This fingerprint looks wider:
//
//   * the game's RANDOM GENERATOR: 8 bytes at 0x825A14F4, a multiply-with-
//     carry pair (x = 36969 x (x & 0xFFFF) + (x >> 16), y = 18000 x ... ;
//     inlined at every caller, e.g. the camera shake sub_82104E20, findings/66).
//     Every random draw moves it, so a run that draws one number more or less
//     (an enemy deciding differently, an effect that ran once more) shows here
//     on the frame it happens.
//   * every player's body: physics position + velocity (the `track` numbers).
//   * the game's GLOBAL DATA: the writable part of the executable image,
//     0x824D0000-0x82650000 (after the code, 384 pages of 4 KB: the game's
//     globals, managers, tables, counters), hashed PER PAGE. The first page
//     that differs between two runs points at the globals to look at; the
//     optional dump (`worldhash <file> dump <frame>`) writes that frame's whole
//     data section so tools/world_hash_compare.py can name the differing words.
//
// Some pages change with REAL time on other threads (sound, streaming, the
// vblank counter 0x8258E460): they differ between any two runs. The compare
// tool reports "noisy" pages (differ from the very first frame / flicker) apart
// from the first real divergence.
//
// FILE FORMAT (little-endian, written at each frame end on the main thread):
//   header 32 bytes: "MORWHASH", u32 version 1, u32 page count, u32 first page
//                    address, u32 page size, u64 0
//   record per frame: i64 frame (fixed_step frame, else a count), i64 replay
//                    frame (-1 none), u32 rng x, u32 rng y, u32 players present
//                    mask, u32 0, u64 players hash, u64 data hash (all pages),
//                    u64 page hash x page count
//   ~3.1 KB per frame = ~185 KB per second at 60 fps.
//
// Console: worldhash <file> [frames] [dump [r]<frame>] | worldhash stop | worldhash
//   dump r<N> = at REPLAY frame N (waits for the replay to start)
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace world_hash {

// The console command (any thread). Returns the reply text; throws
// std::runtime_error on bad arguments.
std::string Command(const std::vector<std::string_view>& words);

// A small fingerprint of the world NOW (game main thread, between frames):
// the main random generator + every player's body (position, velocity, titan
// or not; never a heap address). The network session (net/net_session.h)
// compares it between PCs every frame: cheap enough for that, wide enough to
// catch a drift within moments (any random draw or movement that differs).
uint64_t QuickFingerprint();
// Its parts as one line of text (debug: --debug_net_fingerprint_log).
std::string QuickFingerprintParts();

}  // namespace world_hash
