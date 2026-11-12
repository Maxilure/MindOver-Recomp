// =============================================================================
// network/protocol.h -- bounded, transport-neutral multiplayer wire format
// =============================================================================
//
// WHAT: defines versioned messages shared by the future LAN and Steam
// transports. It contains no Steam types, socket handles or guest addresses.
//
// FORMAT: a 22-byte big-endian header followed by at most 64 payload bytes.
// The transport associates each authenticated connection with its assigned
// player slot; the packet never chooses its own slot. See docs/multiplayer.
// =============================================================================
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "remote_input.h"

namespace network::protocol {

constexpr uint16_t kVersion = 1;
constexpr size_t kHeaderSize = 22;
constexpr size_t kMaxPayloadSize = 64;
constexpr size_t kMaxPacketSize = kHeaderSize + kMaxPayloadSize;

enum class MessageType : uint8_t {
  kHello = 1,
  kWelcome = 2,
  kReject = 3,
  kInput = 4,
  kKeepAlive = 5,
  kStart = 6,
};

struct Packet {
  MessageType type = MessageType::kHello;
  uint64_t session_id = 0;
  uint32_t sequence = 0;
  uint8_t payload_size = 0;
  std::array<uint8_t, kMaxPayloadSize> payload{};
};

// Encodes to the canonical wire format. Returns 0 when any header or
// type-specific payload rule is invalid; output is never partially valid.
size_t Encode(const Packet& packet, std::span<uint8_t> output);

// Decodes only complete packets with a supported version, known message type,
// zero reserved flags, valid session rules and an exact message payload size.
// On failure `packet` is left unchanged.
bool Decode(std::span<const uint8_t> input, Packet& packet);

// PlayerInput payload layout: game frame u32, buttons u16, two trigger bytes,
// and four signed axes u16. Buttons must be defined XInput gamepad buttons.
constexpr size_t kInputPayloadSize = 16;
bool EncodeInputPayload(uint32_t game_frame, const remote_input::State& state,
                        std::span<uint8_t> output);
bool DecodeInputPayload(std::span<const uint8_t> input, uint32_t& game_frame,
                        remote_input::State& state);

}  // namespace network::protocol
