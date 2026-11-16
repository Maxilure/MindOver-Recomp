// =============================================================================
// game_time.h -- one clock for the port's own code that CHANGES THE GAME WORLD
// =============================================================================
//
// WHY (2026-11-17, toward online / LAN co-op): with input lockstep every PC
// runs the whole game and must compute the same world from the same inputs,
// frame for frame. The game's own clocks are already answered per frame while
// the fixed step is on (fixed_step.h), but much of OUR code kept time with
// std::chrono::steady_clock: the idle reticle's 2 s, the ground grace, spin
// circles, the dig-spot trigger, drop-out windows... Real time differs on
// every PC (and between two runs on one PC), so each of those can make two
// runs part. The first 4-player exact runs showed it: the idle reticle and the
// co-op camera made identical inputs end in different places.
//
// game_time::Clock is a drop-in std::chrono clock:
//   * fixed step OFF (normal play): exactly steady_clock (same numbers, same
//     behaviour as before: nothing changes for a player);
//   * fixed step ON (exact runs, and later an online session): the moment the
//     fixed step came on + frame number / N s, i.e. GAME time. Two runs, or two
//     PCs, then see the same intervals on the same frames.
//
// Use it (instead of steady_clock) in code whose timing decides anything in
// the game world. Pure view code (renderer, overlays, logs, menus' looks) and
// input-side code (before an input is stamped with its frame) may keep the
// real clock: notes in the online plan, section 8b "R5 AUDIT".
//
//   using Clock = game_time::Clock;
//   Clock::time_point t = Clock::now();  ...  Clock::now() - t > 2s
// =============================================================================
#pragma once

#include <chrono>
#include <cstdint>

#include "fixed_step.h"

namespace game_time {

struct Clock {
  using duration = std::chrono::nanoseconds;
  using rep = duration::rep;
  using period = duration::period;
  using time_point = std::chrono::time_point<Clock>;
  static constexpr bool is_steady = true;

  // Any thread.
  static time_point now() noexcept { return time_point(duration(fixed_step::GameClockNs())); }
};

}  // namespace game_time
