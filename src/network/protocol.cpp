// =============================================================================
// network/protocol.cpp -- bounded multiplayer packet codec
// =============================================================================
//
// WHAT: serializes and validates the common packet format before a transport
// hands data to a session. It deliberately has no platform socket or Steam API.
//
// FORMAT: MOMN magic, protocol version, message type, reserved flags, session
// id, packet sequence, payload size, then a type-specific payload. Integers
// are big-endian on the wire; input axes retain their signed 16-bit bit pattern.
// =============================================================================
#include "protocol.h"

namespace network::protocol {
namespace {

constexpr std::array<uint8_t, 4> kMagic{'M', 'O', 'M', 'N'};
constexpr uint16_t kKnownButtons = 0xF7FF;  // all X_INPUT_GAMEPAD_BUTTON bits

uint16_t Load16(const uint8_t* p) {
  return uint16_t(uint16_t(p[0]) << 8 | uint16_t(p[1]));
}

int16_t LoadSigned16(const uint8_t* p) {
  const uint16_t bits = Load16(p);
  // Keep the conversion in range instead of relying on an out-of-range
  // unsigned-to-signed cast for values whose top bit is set.
  return bits & 0x8000 ? int16_t(int32_t(bits) - 0x10000) : int16_t(bits);
}

uint32_t Load32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

uint64_t Load64(const uint8_t* p) {
  return uint64_t(Load32(p)) << 32 | Load32(p + 4);
}

void Store16(uint8_t* p, uint16_t value) {
  p[0] = uint8_t(value >> 8);
  p[1] = uint8_t(value);
}

void Store32(uint8_t* p, uint32_t value) {
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

void Store64(uint8_t* p, uint64_t value) {
  Store32(p, uint32_t(value >> 32));
  Store32(p + 4, uint32_t(value));
}

bool IsKnownType(uint8_t type) {
  return type >= uint8_t(MessageType::kHello) && type <= uint8_t(MessageType::kStart);
}

bool ValidPayloadSize(MessageType type, uint8_t size) {
  switch (type) {
    case MessageType::kHello: return size == 8;       // build fingerprint
    case MessageType::kWelcome: return size == 4;     // first slot, count, reserved
    case MessageType::kReject: return size == 2;      // reason code
    case MessageType::kInput: return size == kInputPayloadSize;
    case MessageType::kKeepAlive: return size == 4;   // latest game frame
    case MessageType::kStart: return size == 4;       // start sequence/frame
  }
  return false;
}

bool ValidSessionId(MessageType type, uint64_t session_id) {
  // A peer must identify a session after hello. A zero id is allowed for an
  // initial hello and for a rejection before a session was created.
  return session_id != 0 || type == MessageType::kHello || type == MessageType::kReject;
}

bool ValidPayload(MessageType type, std::span<const uint8_t> payload) {
  if (!ValidPayloadSize(type, uint8_t(payload.size()))) return false;
  if (type == MessageType::kInput) {
    const uint16_t buttons = Load16(payload.data() + 4);
    return (buttons & uint16_t(~kKnownButtons)) == 0;
  }
  if (type == MessageType::kWelcome) {
    // XInput user 0 is reserved for the host; remote peers occupy 1-3.
    return payload[0] >= 1 && payload[0] <= 3 && payload[1] >= 1 &&
           payload[1] <= 4 && uint16_t(payload[0]) + payload[1] <= 4 &&
           payload[2] == 0 && payload[3] == 0;
  }
  return true;
}

}  // namespace

size_t Encode(const Packet& packet, std::span<uint8_t> output) {
  if (packet.payload_size > kMaxPayloadSize ||
      !IsKnownType(uint8_t(packet.type)) ||
      !ValidPayloadSize(packet.type, packet.payload_size) ||
      !ValidSessionId(packet.type, packet.session_id) ||
      output.size() < kHeaderSize + packet.payload_size) {
    return 0;
  }
  const std::span<const uint8_t> payload(packet.payload.data(), packet.payload_size);
  if (!ValidPayload(packet.type, payload)) return 0;

  for (size_t i = 0; i < kMagic.size(); ++i) output[i] = kMagic[i];
  Store16(output.data() + 4, kVersion);
  output[6] = uint8_t(packet.type);
  output[7] = 0;  // reserved flags must remain zero in protocol v1
  Store64(output.data() + 8, packet.session_id);
  Store32(output.data() + 16, packet.sequence);
  Store16(output.data() + 20, packet.payload_size);
  for (size_t i = 0; i < packet.payload_size; ++i) output[kHeaderSize + i] = packet.payload[i];
  return kHeaderSize + packet.payload_size;
}

bool Decode(std::span<const uint8_t> input, Packet& packet) {
  if (input.size() < kHeaderSize || input.size() > kMaxPacketSize) return false;
  for (size_t i = 0; i < kMagic.size(); ++i) {
    if (input[i] != kMagic[i]) return false;
  }
  if (Load16(input.data() + 4) != kVersion || input[7] != 0 || !IsKnownType(input[6])) {
    return false;
  }

  const auto type = MessageType(input[6]);
  const uint16_t payload_size = Load16(input.data() + 20);
  if (payload_size > kMaxPayloadSize || input.size() != kHeaderSize + payload_size ||
      !ValidPayloadSize(type, uint8_t(payload_size))) {
    return false;
  }
  const uint64_t session_id = Load64(input.data() + 8);
  if (!ValidSessionId(type, session_id) ||
      !ValidPayload(type, input.subspan(kHeaderSize, payload_size))) {
    return false;
  }

  Packet decoded{};
  decoded.type = type;
  decoded.session_id = session_id;
  decoded.sequence = Load32(input.data() + 16);
  decoded.payload_size = uint8_t(payload_size);
  for (size_t i = 0; i < payload_size; ++i) decoded.payload[i] = input[kHeaderSize + i];
  packet = decoded;
  return true;
}

bool EncodeInputPayload(uint32_t game_frame, const remote_input::State& state,
                        std::span<uint8_t> output) {
  if (output.size() < kInputPayloadSize || (state.buttons & uint16_t(~kKnownButtons)) != 0) {
    return false;
  }
  Store32(output.data(), game_frame);
  Store16(output.data() + 4, state.buttons);
  output[6] = state.left_trigger;
  output[7] = state.right_trigger;
  Store16(output.data() + 8, uint16_t(state.left_x));
  Store16(output.data() + 10, uint16_t(state.left_y));
  Store16(output.data() + 12, uint16_t(state.right_x));
  Store16(output.data() + 14, uint16_t(state.right_y));
  return true;
}

bool DecodeInputPayload(std::span<const uint8_t> input, uint32_t& game_frame,
                        remote_input::State& state) {
  if (input.size() != kInputPayloadSize ||
      (Load16(input.data() + 4) & uint16_t(~kKnownButtons)) != 0) {
    return false;
  }
  remote_input::State decoded{};
  decoded.buttons = Load16(input.data() + 4);
  decoded.left_trigger = input[6];
  decoded.right_trigger = input[7];
  decoded.left_x = LoadSigned16(input.data() + 8);
  decoded.left_y = LoadSigned16(input.data() + 10);
  decoded.right_x = LoadSigned16(input.data() + 12);
  decoded.right_y = LoadSigned16(input.data() + 14);
  state = decoded;
  game_frame = Load32(input.data());
  return true;
}

}  // namespace network::protocol
