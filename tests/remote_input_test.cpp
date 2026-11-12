// =============================================================================
// tests/remote_input_test.cpp -- remote controller handoff tests
// =============================================================================
//
// WHAT: verifies slot ownership, state publication, player-presence notices
// and the guest's big-endian XINPUT_STATE bytes without booting the game.
// =============================================================================
#include "../src/network/remote_input.h"

#include <array>
#undef NDEBUG  // These checks must remain active in RelWithDebInfo builds.
#include <cassert>
#include <cstdint>

namespace {
int g_player_changes = 0;
}

namespace who_plays {
void PlayersChanged() { ++g_player_changes; }
}  // namespace who_plays

int main() {
  using network::remote_input::Apply;
  using network::remote_input::OwnsSlot;
  using network::remote_input::Publish;
  using network::remote_input::SetRemoteOwner;

  std::array<uint8_t, 16> guest_state{};
  guest_state.fill(0xAA);
  uint32_t result = 0xDEADBEEF;
  assert(!Apply(1, guest_state.data(), result));
  assert(result == 0xDEADBEEF && guest_state[0] == 0xAA);

  SetRemoteOwner(1, true);
  assert(OwnsSlot(1) && g_player_changes == 1);
  network::remote_input::State state{};
  state.buttons = 0x1234;
  state.left_trigger = 0x56;
  state.right_trigger = 0x78;
  state.left_x = -2;
  state.left_y = 0x1234;
  state.right_x = -32768;
  state.right_y = 32767;
  Publish(1, state);

  assert(Apply(1, guest_state.data(), result));
  assert(result == 0);
  assert(guest_state[0] == 0 && guest_state[1] == 0 && guest_state[2] == 0 && guest_state[3] == 2);
  assert(guest_state[4] == 0x12 && guest_state[5] == 0x34);
  assert(guest_state[6] == 0x56 && guest_state[7] == 0x78);
  assert(guest_state[8] == 0xFF && guest_state[9] == 0xFE);
  assert(guest_state[10] == 0x12 && guest_state[11] == 0x34);
  assert(guest_state[12] == 0x80 && guest_state[13] == 0x00);
  assert(guest_state[14] == 0x7F && guest_state[15] == 0xFF);

  SetRemoteOwner(1, false);
  assert(!OwnsSlot(1) && g_player_changes == 2);
  guest_state.fill(0xCC);
  result = 55;
  assert(!Apply(1, guest_state.data(), result));
  assert(result == 55 && guest_state[0] == 0xCC);
  SetRemoteOwner(4, true);  // out of range is ignored
  Publish(4, state);
  assert(!OwnsSlot(4) && g_player_changes == 2);
}
