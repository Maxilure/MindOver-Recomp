// =============================================================================
// net/transport.h -- pluggable transport interface for online multiplayer
// =============================================================================
//
// WHAT: a minimal packet transport abstraction so the state-synchronization
// layer (net/state_sync.h) never depends on a specific socket or SDK backend.
//
// BACKENDS (planned / implemented):
//   1. UdpTransport (net/udp_transport.cpp) -- direct LAN / IP over UDP with a
//      lightweight reliable-channel ACK layer. Zero external dependencies.
//   2. Future: Valve's open-source GameNetworkingSockets (encryption + relay).
//   3. Future: Steamworks / Spacewar (friend invites + Valve SDR relay).
//
// THREADING: callers drive Poll() from the game's main loop (non-blocking),
// so no background threads touch guest memory or race with the game.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace net {

// Opaque handle for a connected remote endpoint. PeerId 0 is invalid/none.
using PeerId = uint32_t;
constexpr PeerId kInvalidPeer = 0;

struct PeerEvent {
  enum class Type {
    kConnected,
    kDisconnected,
    kPacket,
  };
  Type type = Type::kPacket;
  PeerId peer = kInvalidPeer;
  const uint8_t* data = nullptr;
  size_t size = 0;
  bool reliable = false;
  // kDisconnected only: true = the peer said goodbye (its Close() sent the
  // disconnect notice: it quit on purpose); false = it went SILENT for the
  // transport's timeout (crash, network gone, PC switched off). The session
  // tells players which one happened ("left" vs "lost connection",
  // notes/online-coop.md s.16.6).
  bool closed_by_peer = false;
};

using EventCallback = std::function<void(const PeerEvent&)>;

class ITransport {
 public:
  virtual ~ITransport() = default;

  // Listen for incoming clients on `port` (UDP).
  virtual bool Host(uint16_t port) = 0;

  // Connect to a host at `address:port` (IPv4 dotted quad or hostname).
  virtual bool Connect(std::string_view address, uint16_t port) = 0;

  // Close the socket and forget all peers.
  virtual void Close() = 0;

  // True while hosting or connecting/connected.
  virtual bool IsOpen() const = 0;
  virtual bool IsHost() const = 0;

  // Non-blocking poll: drains incoming packets, resends unacked reliable
  // packets, and times out silent peers. Invokes `cb` for every event.
  virtual void Poll(const EventCallback& cb) = 0;

  // Send a payload (up to ~1200 bytes) to one peer or all connected peers.
  virtual bool Send(PeerId peer, const uint8_t* data, size_t size, bool reliable) = 0;
  virtual void Broadcast(const uint8_t* data, size_t size, bool reliable,
                         PeerId exclude = kInvalidPeer) = 0;

  // Last error message if Host() or Connect() returned false.
  virtual std::string LastError() const = 0;
};

// Creates the direct LAN / IPv4 UDP transport (net/udp_transport.cpp).
std::unique_ptr<ITransport> CreateUdpTransport();

}  // namespace net
