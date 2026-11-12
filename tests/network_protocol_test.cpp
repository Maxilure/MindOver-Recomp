// =============================================================================
// tests/network_protocol_test.cpp -- tests for the multiplayer wire codec
// =============================================================================
//
// WHAT: checks protocol v1 encoding, decoding, input field order and rejection
// of malformed packets. It does not initialize ReXGlue or read game data.
// =============================================================================
#include "../src/network/protocol.h"

#include <array>
#undef NDEBUG  // These checks must remain active in RelWithDebInfo builds.
#include <cassert>
#include <cstdint>

using network::protocol::Decode;
using network::protocol::Encode;
using network::protocol::MessageType;
using network::protocol::Packet;

namespace {

void TestRoundTripAndCanonicalHeader() {
  Packet packet{};
  packet.type = MessageType::kHello;
  packet.sequence = 0x12345678;
  packet.payload_size = 8;
  packet.payload[0] = 0x01;
  packet.payload[7] = 0xFE;
  std::array<uint8_t, network::protocol::kMaxPacketSize> bytes{};
  const size_t size = Encode(packet, bytes);
  assert(size == network::protocol::kHeaderSize + 8);
  assert(bytes[0] == 'M' && bytes[1] == 'O' && bytes[2] == 'M' && bytes[3] == 'N');
  assert(bytes[4] == 0 && bytes[5] == network::protocol::kVersion);
  assert(bytes[6] == uint8_t(MessageType::kHello) && bytes[7] == 0);
  assert(bytes[16] == 0x12 && bytes[17] == 0x34 && bytes[18] == 0x56 && bytes[19] == 0x78);

  Packet decoded{};
  assert(Decode(std::span<const uint8_t>(bytes.data(), size), decoded));
  assert(decoded.type == packet.type && decoded.session_id == 0);
  assert(decoded.sequence == packet.sequence && decoded.payload_size == packet.payload_size);
  assert(decoded.payload[0] == 0x01 && decoded.payload[7] == 0xFE);
}

void TestInputPayloadRoundTrip() {
  network::remote_input::State sent{};
  sent.buttons = 0xF7FF;
  sent.left_trigger = 0x12;
  sent.right_trigger = 0xFE;
  sent.left_x = INT16_MIN;
  sent.left_y = -12345;
  sent.right_x = 23456;
  sent.right_y = INT16_MAX;
  std::array<uint8_t, network::protocol::kInputPayloadSize> bytes{};
  assert(network::protocol::EncodeInputPayload(0xFEDCBA98, sent, bytes));
  assert(bytes[0] == 0xFE && bytes[1] == 0xDC && bytes[2] == 0xBA && bytes[3] == 0x98);
  assert(bytes[4] == 0xF7 && bytes[5] == 0xFF);

  uint32_t frame = 0;
  network::remote_input::State received{};
  assert(network::protocol::DecodeInputPayload(bytes, frame, received));
  assert(frame == 0xFEDCBA98 && received.buttons == sent.buttons);
  assert(received.left_trigger == sent.left_trigger && received.right_trigger == sent.right_trigger);
  assert(received.left_x == sent.left_x && received.left_y == sent.left_y);
  assert(received.right_x == sent.right_x && received.right_y == sent.right_y);
}

void TestSessionPacketRoundTrip() {
  Packet packet{};
  packet.type = MessageType::kInput;
  packet.session_id = 0x0123456789ABCDEF;
  packet.sequence = 17;
  packet.payload_size = network::protocol::kInputPayloadSize;
  network::remote_input::State state{};
  state.buttons = 0x1000;
  state.left_x = -1;
  assert(network::protocol::EncodeInputPayload(91, state, packet.payload));
  std::array<uint8_t, network::protocol::kMaxPacketSize> bytes{};
  const size_t size = Encode(packet, bytes);
  assert(size == network::protocol::kHeaderSize + network::protocol::kInputPayloadSize);
  Packet decoded{};
  assert(Decode(std::span<const uint8_t>(bytes.data(), size), decoded));
  assert(decoded.session_id == packet.session_id && decoded.sequence == packet.sequence);
  uint32_t frame = 0;
  network::remote_input::State decoded_state{};
  assert(network::protocol::DecodeInputPayload(
      std::span<const uint8_t>(decoded.payload.data(), decoded.payload_size), frame, decoded_state));
  assert(frame == 91 && decoded_state.buttons == state.buttons && decoded_state.left_x == -1);
}

void TestEveryMessageTypeHasAValidShape() {
  struct Example {
    MessageType type;
    uint8_t payload_size;
    uint64_t session_id;
  };
  const std::array examples{
      Example{MessageType::kHello, 8, 0},
      Example{MessageType::kWelcome, 4, 77},
      Example{MessageType::kReject, 2, 0},
      Example{MessageType::kInput, 16, 77},
      Example{MessageType::kKeepAlive, 4, 77},
      Example{MessageType::kStart, 4, 77},
  };
  for (const Example& example : examples) {
    Packet packet{};
    packet.type = example.type;
    packet.session_id = example.session_id;
    packet.sequence = 4;
    packet.payload_size = example.payload_size;
    if (example.type == MessageType::kWelcome) {
      packet.payload[0] = 1;  // first remote XInput slot
      packet.payload[1] = 1;  // one player assigned
    }
    std::array<uint8_t, network::protocol::kMaxPacketSize> bytes{};
    const size_t size = Encode(packet, bytes);
    assert(size == network::protocol::kHeaderSize + example.payload_size);
    Packet decoded{};
    assert(Decode(std::span<const uint8_t>(bytes.data(), size), decoded));
    assert(decoded.type == example.type && decoded.session_id == example.session_id);
    assert(decoded.payload_size == example.payload_size);
  }
}

void TestRejectsMalformedPacketsWithoutChangingOutput() {
  Packet packet{};
  packet.type = MessageType::kKeepAlive;
  packet.session_id = 123;
  packet.payload_size = 4;
  packet.payload[3] = 7;
  std::array<uint8_t, network::protocol::kMaxPacketSize> bytes{};
  const size_t size = Encode(packet, bytes);
  assert(size == network::protocol::kHeaderSize + 4);
  Packet unchanged{};
  unchanged.sequence = 999;
  auto reject = [&](const auto& broken, size_t broken_size) {
    assert(!Decode(std::span<const uint8_t>(broken.data(), broken_size), unchanged));
    assert(unchanged.sequence == 999);
  };
  auto broken = bytes;
  broken[0] ^= 0xFF;
  reject(broken, size);
  broken = bytes;
  broken[5]++;
  reject(broken, size);
  broken = bytes;
  broken[7] = 1;
  reject(broken, size);
  broken = bytes;
  broken[6] = 99;
  reject(broken, size);
  reject(bytes, size - 1);  // truncated payload
  reject(bytes, size + 1);  // trailing bytes
  std::array<uint8_t, network::protocol::kMaxPacketSize + 1> oversized{};
  assert(!Decode(oversized, unchanged));
  broken = bytes;
  broken[20] = 0;
  broken[21] = 0xFF;  // declared payload exceeds the bounded packet
  reject(broken, size);
  std::array<uint8_t, network::protocol::kHeaderSize + 3> too_small{};
  assert(Encode(packet, too_small) == 0);
}

void TestRejectsInvalidSessionAndPayloadValues() {
  Packet packet{};
  packet.type = MessageType::kInput;
  packet.payload_size = network::protocol::kInputPayloadSize;
  std::array<uint8_t, network::protocol::kMaxPacketSize> bytes{};
  assert(Encode(packet, bytes) == 0);  // input needs an assigned session
  packet.session_id = 99;
  packet.payload_size = network::protocol::kInputPayloadSize - 1;
  assert(Encode(packet, bytes) == 0);  // fixed-size message cannot be shortened
  packet.payload_size = network::protocol::kInputPayloadSize;
  packet.payload[4] = 0x08;  // undefined XInput button bit
  assert(Encode(packet, bytes) == 0);

  network::remote_input::State invalid{};
  invalid.buttons = 0x0800;
  std::array<uint8_t, network::protocol::kInputPayloadSize> input{};
  assert(!network::protocol::EncodeInputPayload(0, invalid, input));

  Packet welcome{};
  welcome.type = MessageType::kWelcome;
  welcome.session_id = 99;
  welcome.payload_size = 4;
  welcome.payload[0] = 0;  // host slot cannot be assigned to a remote peer
  welcome.payload[1] = 1;
  assert(Encode(welcome, bytes) == 0);
  welcome.payload[0] = 1;
  welcome.payload[1] = 1;
  welcome.payload[2] = 1;  // reserved bytes must be zero
  assert(Encode(welcome, bytes) == 0);
}

}  // namespace

int main() {
  TestRoundTripAndCanonicalHeader();
  TestInputPayloadRoundTrip();
  TestSessionPacketRoundTrip();
  TestEveryMessageTypeHasAValidShape();
  TestRejectsMalformedPacketsWithoutChangingOutput();
  TestRejectsInvalidSessionAndPayloadValues();
}
