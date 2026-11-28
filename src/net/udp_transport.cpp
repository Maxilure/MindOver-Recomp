// =============================================================================
// net/udp_transport.cpp -- non-blocking IPv4 UDP transport with reliable ACKs
// =============================================================================
//
// WHAT: implements net::ITransport over plain IPv4 UDP sockets (POSIX on Linux,
// Winsock2 on Windows) for LAN and localhost testing with zero dependencies.
//
// WIRE FRAMING (little-endian, prepended to every datagram):
//   +0  u32  magic       0x31534F4D ("MOS1" = Mind Over Sync v1)
//   +4  u8   flags       bit 0 = reliable (requires ACK)
//                        bit 1 = ack-only control packet
//                        bit 2 = disconnect notice
//   +5  u8   reserved    0
//   +6  u16  seq         reliable sequence number (1..65535; 0 for unreliable)
//   +8  u16  ack         last reliable sequence received from the sender
//   +10 u16  payload_len bytes following this 12-byte header
//
// RELIABILITY: reliable packets are queued per peer and resent every 120 ms
// until the peer's `ack` advances past their `seq` (duplicate reliable packets
// are silently ACKed and dropped). Unreliable packets (30 Hz state snapshots)
// are sent fire-and-forget with zero resend overhead.
// =============================================================================
#include "transport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
// Winsock's functions (socket, bind, sendto, recvfrom, getaddrinfo...) live in
// ws2_32.lib; nothing else in the build links it, so the linker is told here,
// like the other Windows libraries (session_log.cpp, crash_report.cpp).
#pragma comment(lib, "ws2_32.lib")
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

#include <rex/logging.h>

namespace net {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kWireMagic = 0x31534F4Du;  // "MOS1"
constexpr size_t kHeaderSize = 12;
constexpr size_t kMaxDatagram = 1400;
constexpr uint8_t kFlagReliable = 1u << 0;
constexpr uint8_t kFlagAckOnly = 1u << 1;
constexpr uint8_t kFlagDisconnect = 1u << 2;

constexpr auto kResendInterval = std::chrono::milliseconds(120);
constexpr auto kPeerTimeout = std::chrono::seconds(8);

void CloseSocket(SocketHandle s) {
  if (s == kInvalidSocket) return;
#ifdef _WIN32
  closesocket(s);
#else
  close(s);
#endif
}

bool SetNonBlocking(SocketHandle s) {
#ifdef _WIN32
  u_long mode = 1;
  return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
  const int flags = fcntl(s, F_GETFL, 0);
  if (flags < 0) return false;
  return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void WriteLE16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v & 0xFFu);
  p[1] = uint8_t((v >> 8) & 0xFFu);
}
void WriteLE32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v & 0xFFu);
  p[1] = uint8_t((v >> 8) & 0xFFu);
  p[2] = uint8_t((v >> 16) & 0xFFu);
  p[3] = uint8_t((v >> 24) & 0xFFu);
}
uint16_t ReadLE16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }
uint32_t ReadLE32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// Wrap-safe 16-bit sequence comparison: is `a` strictly newer than `b`?
bool SeqNewer(uint16_t a, uint16_t b) { return a != b && uint16_t(a - b) < 32768u; }

struct PendingReliable {
  uint16_t seq = 0;
  std::vector<uint8_t> datagram;
  Clock::time_point last_sent;
};

struct Peer {
  PeerId id = kInvalidPeer;
  sockaddr_in addr{};
  Clock::time_point last_heard;
  uint16_t next_reliable_seq = 1;
  uint16_t last_recv_reliable_seq = 0;
  std::deque<PendingReliable> pending;
};

bool SameAddr(const sockaddr_in& a, const sockaddr_in& b) {
  return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

class UdpTransport final : public ITransport {
 public:
  ~UdpTransport() override { Close(); }

  bool Host(uint16_t port) override {
    Close();
    if (!EnsureSocketsInit()) return false;
    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == kInvalidSocket) {
      last_error_ = "socket() failed";
      return false;
    }
    int reuse = 1;
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));
    if (!SetNonBlocking(sock_)) {
      last_error_ = "failed to set non-blocking socket";
      Close();
      return false;
    }
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(port);
    if (bind(sock_, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
      last_error_ = "bind() failed on UDP port " + std::to_string(port);
      Close();
      return false;
    }
    is_host_ = true;
    REXLOG_INFO("Online UDP transport hosting on port {}", port);
    return true;
  }

  bool Connect(std::string_view address, uint16_t port) override {
    Close();
    if (!EnsureSocketsInit()) return false;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    const std::string addr_str(address);
    const std::string port_str = std::to_string(port);
    if (getaddrinfo(addr_str.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
      last_error_ = "could not resolve address: " + addr_str;
      return false;
    }
    sockaddr_in host_addr{};
    std::memcpy(&host_addr, res->ai_addr, sizeof(host_addr));
    freeaddrinfo(res);

    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == kInvalidSocket) {
      last_error_ = "socket() failed";
      return false;
    }
    if (!SetNonBlocking(sock_)) {
      last_error_ = "failed to set non-blocking socket";
      Close();
      return false;
    }
    // Bind ephemeral local port so recvfrom works cleanly on all platforms.
    sockaddr_in local_addr{};
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = 0;
    if (bind(sock_, reinterpret_cast<const sockaddr*>(&local_addr), sizeof(local_addr)) != 0) {
      last_error_ = "bind() failed for client socket";
      Close();
      return false;
    }

    is_host_ = false;
    Peer host_peer;
    host_peer.id = next_peer_id_++;
    host_peer.addr = host_addr;
    host_peer.last_heard = Clock::now();
    peers_.push_back(std::move(host_peer));
    REXLOG_INFO("Online UDP transport targeting {}:{}", addr_str, port);
    return true;
  }

  void Close() override {
    if (sock_ != kInvalidSocket) {
      // Best-effort disconnect notice to connected peers.
      for (const Peer& peer : peers_) {
        SendControl(peer, kFlagDisconnect);
      }
      CloseSocket(sock_);
      sock_ = kInvalidSocket;
    }
    peers_.clear();
    is_host_ = false;
  }

  bool IsOpen() const override { return sock_ != kInvalidSocket; }
  bool IsHost() const override { return is_host_; }
  std::string LastError() const override { return last_error_; }

  bool Send(PeerId peer_id, const uint8_t* data, size_t size, bool reliable) override {
    if (sock_ == kInvalidSocket || size > kMaxDatagram - kHeaderSize) return false;
    Peer* peer = FindPeer(peer_id);
    if (!peer) return false;
    return SendToPeer(*peer, data, size, reliable);
  }

  void Broadcast(const uint8_t* data, size_t size, bool reliable, PeerId exclude) override {
    if (sock_ == kInvalidSocket || size > kMaxDatagram - kHeaderSize) return;
    for (Peer& peer : peers_) {
      if (peer.id == exclude) continue;
      SendToPeer(peer, data, size, reliable);
    }
  }

  void Poll(const EventCallback& cb) override {
    if (sock_ == kInvalidSocket) return;
    const auto now = Clock::now();

    std::array<uint8_t, kMaxDatagram> buf{};
    for (int limit = 0; limit < 256; ++limit) {
      sockaddr_in from{};
      socklen_t from_len = sizeof(from);
      const auto n = recvfrom(sock_, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0,
                              reinterpret_cast<sockaddr*>(&from), &from_len);
      if (n <= 0) break;
      if (size_t(n) < kHeaderSize) continue;
      if (ReadLE32(buf.data()) != kWireMagic) continue;

      const uint8_t flags = buf[4];
      const uint16_t seq = ReadLE16(buf.data() + 6);
      const uint16_t ack = ReadLE16(buf.data() + 8);
      const uint16_t payload_len = ReadLE16(buf.data() + 10);
      if (kHeaderSize + payload_len > size_t(n)) continue;

      Peer* peer = FindPeerByAddr(from);
      if (!peer) {
        if (!is_host_ || (flags & kFlagDisconnect)) continue;
        Peer new_peer;
        new_peer.id = next_peer_id_++;
        new_peer.addr = from;
        new_peer.last_heard = now;
        peers_.push_back(std::move(new_peer));
        peer = &peers_.back();
        if (cb) {
          PeerEvent ev;
          ev.type = PeerEvent::Type::kConnected;
          ev.peer = peer->id;
          cb(ev);
        }
      }

      peer->last_heard = now;

      // Clear any pending reliable packets acknowledged by `ack`.
      if (ack != 0) {
        while (!peer->pending.empty() && !SeqNewer(peer->pending.front().seq, ack)) {
          peer->pending.pop_front();
        }
      }

      if (flags & kFlagDisconnect) {
        const PeerId gone = peer->id;
        RemovePeer(gone);
        if (cb) {
          PeerEvent ev;
          ev.type = PeerEvent::Type::kDisconnected;
          ev.peer = gone;
          cb(ev);
        }
        continue;
      }

      if (flags & kFlagAckOnly) continue;

      const bool is_reliable = (flags & kFlagReliable) != 0;
      if (is_reliable) {
        const bool is_new =
            (peer->last_recv_reliable_seq == 0) || SeqNewer(seq, peer->last_recv_reliable_seq);
        if (is_new) {
          peer->last_recv_reliable_seq = seq;
        }
        // Always ACK reliable packets promptly (even duplicates so sender stops resending).
        SendControl(*peer, kFlagAckOnly);
        if (!is_new) continue;
      }

      if (cb && payload_len > 0) {
        PeerEvent ev;
        ev.type = PeerEvent::Type::kPacket;
        ev.peer = peer->id;
        ev.data = buf.data() + kHeaderSize;
        ev.size = payload_len;
        ev.reliable = is_reliable;
        cb(ev);
      }
    }

    // Resend unacknowledged reliable packets and time out silent peers.
    std::vector<PeerId> timed_out;
    for (Peer& peer : peers_) {
      if (now - peer.last_heard > kPeerTimeout) {
        timed_out.push_back(peer.id);
        continue;
      }
      for (PendingReliable& item : peer.pending) {
        if (now - item.last_sent >= kResendInterval) {
          // Refresh the piggybacked ACK field before resending.
          WriteLE16(item.datagram.data() + 8, peer.last_recv_reliable_seq);
          RawSend(peer.addr, item.datagram.data(), item.datagram.size());
          item.last_sent = now;
        }
      }
    }
    for (PeerId id : timed_out) {
      RemovePeer(id);
      if (cb) {
        PeerEvent ev;
        ev.type = PeerEvent::Type::kDisconnected;
        ev.peer = id;
        cb(ev);
      }
    }
  }

 private:
  bool EnsureSocketsInit() {
#ifdef _WIN32
    static bool initialized = false;
    if (!initialized) {
      WSADATA wsa{};
      if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        last_error_ = "WSAStartup failed";
        return false;
      }
      initialized = true;
    }
#endif
    return true;
  }

  Peer* FindPeer(PeerId id) {
    for (Peer& p : peers_) {
      if (p.id == id) return &p;
    }
    return nullptr;
  }

  Peer* FindPeerByAddr(const sockaddr_in& addr) {
    for (Peer& p : peers_) {
      if (SameAddr(p.addr, addr)) return &p;
    }
    return nullptr;
  }

  void RemovePeer(PeerId id) {
    peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
                                [id](const Peer& p) { return p.id == id; }),
                 peers_.end());
  }

  bool RawSend(const sockaddr_in& to, const uint8_t* data, size_t size) {
    const auto sent = sendto(sock_, reinterpret_cast<const char*>(data), int(size), 0,
                             reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    return sent == static_cast<decltype(sent)>(size);
  }

  void SendControl(const Peer& peer, uint8_t flags) {
    std::array<uint8_t, kHeaderSize> hdr{};
    WriteLE32(hdr.data(), kWireMagic);
    hdr[4] = flags;
    hdr[5] = 0;
    WriteLE16(hdr.data() + 6, 0);
    WriteLE16(hdr.data() + 8, peer.last_recv_reliable_seq);
    WriteLE16(hdr.data() + 10, 0);
    RawSend(peer.addr, hdr.data(), hdr.size());
  }

  bool SendToPeer(Peer& peer, const uint8_t* data, size_t size, bool reliable) {
    std::vector<uint8_t> dgram(kHeaderSize + size);
    uint16_t seq = 0;
    if (reliable) {
      seq = peer.next_reliable_seq++;
      if (peer.next_reliable_seq == 0) peer.next_reliable_seq = 1;
    }
    WriteLE32(dgram.data(), kWireMagic);
    dgram[4] = reliable ? kFlagReliable : 0;
    dgram[5] = 0;
    WriteLE16(dgram.data() + 6, seq);
    WriteLE16(dgram.data() + 8, peer.last_recv_reliable_seq);
    WriteLE16(dgram.data() + 10, uint16_t(size));
    if (size > 0) std::memcpy(dgram.data() + kHeaderSize, data, size);

    const bool ok = RawSend(peer.addr, dgram.data(), dgram.size());
    if (reliable) {
      PendingReliable item;
      item.seq = seq;
      item.datagram = std::move(dgram);
      item.last_sent = Clock::now();
      peer.pending.push_back(std::move(item));
    }
    return ok;
  }

  SocketHandle sock_ = kInvalidSocket;
  bool is_host_ = false;
  PeerId next_peer_id_ = 1;
  std::vector<Peer> peers_;
  std::string last_error_;
};

}  // namespace

std::unique_ptr<ITransport> CreateUdpTransport() { return std::make_unique<UdpTransport>(); }

}  // namespace net
