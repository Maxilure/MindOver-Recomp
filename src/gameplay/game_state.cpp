// Stores the latest activity published by the front-end dispatcher. One
// lock keeps phase, level and pause state from the same update together.
#include "game_state.h"

#include <mutex>

#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../guest_memory.h"
#include "../players/more_players.h"

extern "C" REX_FUNC(__imp__sub_822E10C8);  // Completion calculation used by the save screen.

namespace game_state {
namespace {

std::mutex g_mutex;
Snapshot g_snapshot;

uint32_t Read32(const uint8_t* base, uint32_t address) {
  const auto* p = GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

}  // namespace

Snapshot Current() {
  std::lock_guard lock(g_mutex);
  return g_snapshot;
}

void Publish(Snapshot snapshot) {
  std::lock_guard lock(g_mutex);
  g_snapshot = snapshot;
}

// Capture guest fields on the game thread. Native consumers only see the
// resulting values, so this is the only place that depends on the layout.
void Capture(PPCContext& ctx, uint8_t* base, int32_t node) {
  constexpr uint32_t kGameGlobal = 0x8259B190;  // Game object pointer (findings/24 s.6).
  constexpr uint32_t kMode = 8;                // Game mode (more_players::InPlay).
  constexpr uint32_t kLevelIndex = 18;         // Current level byte (findings/24 s.6).
  constexpr uint32_t kPauseFlags = 168;        // Game pause flags (input/mouse_reticle.cpp).
  constexpr uint32_t kInGameMode = 5;           // Includes in-game menus.
  constexpr uint8_t kPaused = 0x20;             // Game update's pause bit.
  constexpr int32_t kTitleLoadFirst = 51, kTitleLoadLast = 53;  // Title load (findings/29 s.7).
  constexpr int32_t kEnterLevel = 486;  // Before ExitLoadingComplete (findings/29).

  constexpr uint32_t kArea = 0x178;           // GAMESTATE area used by the original presence.
  constexpr uint32_t kProgress = 96;
  constexpr uint32_t kCollectibles = 68;      // Also read by the completion calculator.
  constexpr uint32_t kDifficulty = 0x825B0008 + 240;

  Snapshot snapshot;
  if ((node >= kTitleLoadFirst && node <= kTitleLoadLast) ||
      node == kEnterLevel || more_players::Loading()) {
    snapshot.phase = Phase::kLoading;
  } else {
    const uint32_t game = Read32(base, kGameGlobal);
    snapshot.phase = Phase::kFrontEnd;
    const auto* kernel = rex::system::kernel_state();
    auto* memory = kernel ? kernel->memory() : nullptr;
    auto* heap = game && memory ? memory->LookupHeap(game) : nullptr;
    // Check the complete span before following the object pointer. The
    // range query also rejects wraparound and ranges that leave this heap.
    if (heap && heap->QueryRangeAccess(game, game + kArea) != rex::memory::PageAccess::kNoAccess) {
      const auto mode = Read32(base, game + kMode);
      if (mode == kInGameMode) {
        snapshot.phase = Phase::kInGame;
        snapshot.level_id = *GuestPtr(base, game + kLevelIndex);
        snapshot.paused = (*GuestPtr(base, game + kPauseFlags) & kPaused) != 0;
        snapshot.area_id = *GuestPtr(base, game + kArea);
        const uint32_t difficulty = Read32(base, kDifficulty);
        if (difficulty < 3) snapshot.difficulty = static_cast<uint8_t>(difficulty);

        const uint32_t progress = Read32(base, game + kProgress);
        if (progress && Read32(base, game + kCollectibles)) {
          PPCContext registers = ctx;
          registers.r3.u64 = progress;
          __imp__sub_822E10C8(registers, base);
          if (registers.r3.u32 <= 100) {
            snapshot.completion = static_cast<uint8_t>(registers.r3.u32);
          }
        }
      } else if (mode == 1) {
        snapshot.phase = Phase::kMovie;
      }
    }
  }
  Publish(snapshot);
}

}  // namespace game_state
