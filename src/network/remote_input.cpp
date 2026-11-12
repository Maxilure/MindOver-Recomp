// =============================================================================
// network/remote_input.cpp -- synchronized handoff of remote pad states
// =============================================================================
//
// WHAT: stores the newest validated state for each remote player. It connects
// a future transport thread to the game's existing XInput wrapper without
// passing guest addresses to the transport or polling networking in game code
// (findings/59 s.2).
//
// FORMAT: Apply writes the game's big-endian XINPUT_STATE (16 bytes): packet
// number +0, buttons +4, triggers +6/+7, and signed axes +8/+10/+12/+14.
// The game's four-player slots are documented in findings/26; the handoff is
// described in findings/59 s.2.
// =============================================================================
#include "remote_input.h"

#include <array>
#include <mutex>

#include "../players/who_plays.h"

namespace network::remote_input {
namespace {

struct Slot {
  State state{};
  uint32_t packet_number = 0;
  bool owned = false;
};

std::array<Slot, 4> g_slots{};
std::mutex g_mutex;

void Store16(uint8_t* out, uint16_t value) {
  out[0] = uint8_t(value >> 8);
  out[1] = uint8_t(value);
}

void Store32(uint8_t* out, uint32_t value) {
  out[0] = uint8_t(value >> 24);
  out[1] = uint8_t(value >> 16);
  out[2] = uint8_t(value >> 8);
  out[3] = uint8_t(value);
}

}  // namespace

void SetRemoteOwner(uint32_t user, bool remote) {
  if (user >= g_slots.size()) return;
  bool changed;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    Slot& slot = g_slots[user];
    changed = slot.owned != remote;
    slot.owned = remote;
    slot.state = {};
    ++slot.packet_number;
  }
  // The game refreshes its player mask on this notification. Do it outside
  // the lock because that refresh can query this module again.
  if (changed) who_plays::PlayersChanged();
}

bool OwnsSlot(uint32_t user) {
  if (user >= g_slots.size()) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_slots[user].owned;
}

void Publish(uint32_t user, const State& state) {
  if (user >= g_slots.size()) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  Slot& slot = g_slots[user];
  if (!slot.owned) return;
  slot.state = state;
  ++slot.packet_number;
}

bool Apply(uint32_t user, uint8_t* guest_state, uint32_t& result) {
  if (user >= g_slots.size() || !guest_state) return false;

  State state;
  uint32_t packet_number;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const Slot& slot = g_slots[user];
    if (!slot.owned) return false;
    state = slot.state;
    packet_number = slot.packet_number;
  }

  // Guest memory is big-endian; write each field through bytes rather than
  // treating the guest address as a host XINPUT_STATE object.
  Store32(guest_state, packet_number);
  Store16(guest_state + 4, state.buttons);
  guest_state[6] = state.left_trigger;
  guest_state[7] = state.right_trigger;
  Store16(guest_state + 8, uint16_t(state.left_x));
  Store16(guest_state + 10, uint16_t(state.left_y));
  Store16(guest_state + 12, uint16_t(state.right_x));
  Store16(guest_state + 14, uint16_t(state.right_y));
  result = 0;  // The remote slot is connected, even if its current pad is neutral.
  return true;
}

}  // namespace network::remote_input
