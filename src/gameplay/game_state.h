// Native game activity for consumers such as a presence integration. The
// front-end dispatcher publishes values from the game thread (findings/29 s.8).
// The interface uses host values and carries no guest pointers.
#pragma once

#include <cstdint>
#include <optional>

struct PPCContext;

namespace game_state {

enum class Phase { kStarting, kFrontEnd, kLoading, kInGame, kMovie };

struct Snapshot {
  Phase phase = Phase::kStarting;
  std::optional<uint8_t> level_id;  // Internal level index, available in game.
  bool paused = false;
  std::optional<uint8_t> area_id;       // Game area, distinct from the level index.
  std::optional<uint8_t> difficulty;   // 0 = Mild, 1 = Tricky, 2 = Bonkers.
  std::optional<uint8_t> completion;   // Same percentage as the save screen.
};

// Returns the most recent front-end update by value. Safe on any thread,
// including before the game starts. No game memory is accessed here.
Snapshot Current();

// Called by the game's main thread when it observes a new snapshot.
void Publish(Snapshot snapshot);

// Captures the game-thread state at the front-end dispatcher.
void Capture(PPCContext& ctx, uint8_t* base, int32_t node);

}  // namespace game_state
