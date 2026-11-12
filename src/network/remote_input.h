// =============================================================================
// network/remote_input.h -- remote player states at the game's XInput boundary
// =============================================================================
//
// WHAT: lets a future LAN or Steam transport supply one controller state for
// each of the game's four XInput user slots. The game remains the consumer of
// input; no guest memory is shared with another process.
//
// HOW IT HOOKS IN: saves/save_library.cpp calls Apply from the game's native
// XInput wrapper (sub_824742F0), after the SDK's local devices were polled.
// The gamepad layout is the 16-byte XINPUT_STATE: packet number at +0,
// buttons at +4, triggers at +6/+7, and four big-endian axes at +8..+15.
// This module is only the thread-safe handoff. Transport and player assignment
// are deliberately separate; see the multiplayer implementation plan.
// =============================================================================
#pragma once

#include <cstdint>

namespace network::remote_input {

struct State {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t left_x = 0;
  int16_t left_y = 0;
  int16_t right_x = 0;
  int16_t right_y = 0;
};

// Reserves XInput user slot 0-3 for a remote player. Disabling a slot clears
// its buffered state so the next local poll is used unchanged.
void SetRemoteOwner(uint32_t user, bool remote);

// Whether a slot is currently reserved for a remote peer. Used by the
// existing player-presence accounting so remote players count as signed in.
bool OwnsSlot(uint32_t user);

// Replaces the latest state for a reserved slot. Call from the networking
// thread after validating the packet and assigning its sender to this slot.
void Publish(uint32_t user, const State& state);

// Applies a remote state to the guest's big-endian XINPUT_STATE. `result` is
// the game's XInputGetState return value (0 = success). Returns true only when
// the slot is reserved remotely. The caller should then skip local keyboard
// merging for that slot.
bool Apply(uint32_t user, uint8_t* guest_state, uint32_t& result);

}  // namespace network::remote_input
