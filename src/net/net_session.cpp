// =============================================================================
// net/net_session.cpp -- the LAN lockstep session (net/net_session.h)
// =============================================================================
//
// Three parts, top to bottom:
//   1. PACKETS: the byte layout of every message, written / read with explicit
//      little-endian helpers and size checks (nothing is trusted).
//   2. THE NETWORK THREAD: one UDP socket; receives, answers pings, runs the
//      handshake, files the peer's inputs and fingerprints.
//   3. THE GAME'S SIDE: the hooks (pass start, frame gate, controller reads,
//      session start) on the game's main thread.
//
// PACKET LAYOUT (UDP payload, little-endian):
//   header 12 bytes: "MORN", u8 protocol (kProtocol), u8 type, u16 0,
//                    u32 session token (0 in HELLO)
//   HELLO   (guest -> host)  u64 world settings fingerprint, char build[32]
//   WELCOME (host -> guest)  u32 token, u8 your player (0-based), u8 delay,
//                            u16 0, u32 session seed, u64 settings fingerprint,
//                            char build[32]
//   REFUSE  (host -> guest)  char reason[64]
//   INPUTS  (both ways)      u8 player, u8 count (1-64), u16 0, i32 first frame,
//                            i32 ack (the newest frame of YOUR inputs I hold
//                            without a gap, -1 none), count x 13 bytes (u8
//                            connected, 12-byte XINPUT_GAMEPAD as the game reads
//                            it, big-endian), u8 fingerprint count (0-16),
//                            that many x (i32 frame, u64 fingerprint)
//   PING / PONG              u64 sender's clock (ns); PONG echoes it
//   BYE                      (nothing): I'm leaving
//   DUMP                     i32 session frame: dump the world there
// =============================================================================

#include "net_session.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <random>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <fmt/format.h>
#include <rex/logging.h>

#include "../data/fight_tree.h"
#include "../debug/world_hash.h"
#include "../fixed_step.h"
#include "../game_folder.h"
#include "../achievements/achievements.h"
#include "../input/players.h"
#include "../language/texts.h"
#include "../system/session_settings.h"
#include "online_rules.h"

REXCVAR_DEFINE_BOOL(net_host, false, "CrashMoM",
                    "LAN co-op (test, milestone M1): host a lockstep session as player 1 and wait for "
                    "one guest. Turns the fixed step on (60). Both PCs load the same save");
REXCVAR_DEFINE_STRING(net_join, "", "CrashMoM",
                      "LAN co-op (test, milestone M1): join the host at this address (e.g. "
                      "127.0.0.1) as player 2. Turns the fixed step on (60)");
REXCVAR_DEFINE_INT32(net_port, 37373, "CrashMoM", "LAN co-op: the UDP port the host listens on");
REXCVAR_DEFINE_INT32(net_drop_after_s, 30, "CrashMoM",
                     "LAN co-op: a player whose PC stops answering stays in the level (idle) this many "
                     "seconds, then drops out by itself. Nobody is paused meanwhile");
REXCVAR_DEFINE_INT32(net_delay, 3, "CrashMoM",
                     "LAN co-op: input delay in game frames (the host's value counts): a press "
                     "plays this many frames later, so the other PC's packet has time to arrive");

namespace net {
namespace {

using Clock = std::chrono::steady_clock;

// -----------------------------------------------------------------------------
// 1. PACKETS
// -----------------------------------------------------------------------------
constexpr uint8_t kProtocol = 1;  // bump on any layout change: old copies refuse
constexpr char kMagic[4] = {'M', 'O', 'R', 'N'};
enum Type : uint8_t {
  kHello = 1,
  kWelcome = 2,
  kRefuse = 3,
  kInputs = 4,
  kPing = 5,
  kPong = 6,
  kBye = 7,
  kDump = 8,
};
constexpr size_t kHeader = 12;
constexpr int kMaxInputsPerPacket = 64;
constexpr int kMaxFingerprintsPerPacket = 16;
constexpr size_t kBuildLength = 32;
constexpr int kPlayers = 2;  // M1: two PCs, one player each

// Writer: appends little-endian numbers to a buffer.
struct Writer {
  std::vector<uint8_t> b;
  void U8(uint8_t v) { b.push_back(v); }
  void U16(uint16_t v) {
    for (int i = 0; i < 2; ++i) b.push_back(uint8_t(v >> (8 * i)));
  }
  void U32(uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
  }
  void U64(uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(uint8_t(v >> (8 * i)));
  }
  void Bytes(const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
  // A fixed-size text field, cut and zero-padded.
  void Text(std::string_view s, size_t n) {
    for (size_t i = 0; i < n; ++i) b.push_back(i < s.size() ? uint8_t(s[i]) : 0);
  }
};

// Reader: every read checks there's room; `ok` goes false for good once a
// read ran past the end (the caller drops the packet).
struct Reader {
  const uint8_t* p;
  size_t n, at = 0;
  bool ok = true;
  bool Need(size_t k) {
    if (!ok || n - at < k) ok = false;
    return ok;
  }
  uint8_t U8() { return Need(1) ? p[at++] : 0; }
  uint16_t U16() {
    if (!Need(2)) return 0;
    uint16_t v = uint16_t(p[at] | p[at + 1] << 8);
    at += 2;
    return v;
  }
  uint32_t U32() {
    if (!Need(4)) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= uint32_t(p[at + i]) << (8 * i);
    at += 4;
    return v;
  }
  uint64_t U64() {
    if (!Need(8)) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t(p[at + i]) << (8 * i);
    at += 8;
    return v;
  }
  void Bytes(void* out, size_t k) {
    if (Need(k)) {
      std::memcpy(out, p + at, k);
      at += k;
    }
  }
  // A fixed-size text field: printable ASCII only (anything else = '?').
  std::string Text(size_t k) {
    std::string s;
    if (!Need(k)) return s;
    for (size_t i = 0; i < k && p[at + i]; ++i) {
      const char c = char(p[at + i]);
      s.push_back(c >= 32 && c < 127 ? c : '?');
    }
    at += k;
    return s;
  }
};

std::string Build() {
#ifdef CRASHMOM_VERSION
  return CRASHMOM_VERSION;
#else
  return "unknown";
#endif
}

// One player's controller on one frame, as the game reads it.
struct PadIn {
  uint8_t connected = 1;   // 1 = connected (the game's result 0)
  uint8_t bytes[12] = {};  // XINPUT_GAMEPAD, big-endian, after the device + keyboard merge
  bool operator==(const PadIn& o) const {
    return connected == o.connected && std::memcmp(bytes, o.bytes, 12) == 0;
  }
};

// -----------------------------------------------------------------------------
// Shared state. g_m guards everything below it that both the network thread
// and the game touch; the atomics are read without it.
// -----------------------------------------------------------------------------
std::mutex g_m;
std::condition_variable g_cv;  // "an input arrived" (the gate waits on it)

std::atomic<Role> g_role{Role::kNone};
std::atomic<bool> g_in_session{false};
std::atomic<bool> g_presence_dirty{false};  // PlayerMask changed: tell the game (main thread)

#if defined(_WIN32)
using Socket = SOCKET;
const Socket kNoSocket = INVALID_SOCKET;
#else
using Socket = int;
const Socket kNoSocket = -1;
#endif
Socket g_sock = kNoSocket;
std::thread g_thread;
std::atomic<bool> g_stop{false};

sockaddr_in g_peer{};       // the one peer (host: learned from HELLO; guest: the host)
bool g_have_peer = false;   // host: a guest said HELLO
bool g_welcomed = false;    // handshake done
uint32_t g_token = 0;       // the session's token (host makes it)
int g_local = 0, g_remote = 1;
int g_delay = 3;
uint32_t g_seed = 0;
bool g_seed_known = false;
uint64_t g_peer_settings = 0;
bool g_peer_settings_known = false;
std::string g_peer_build;
Clock::time_point g_last_heard{};
double g_ping_ms = -1;
std::string g_state = "off";
std::string g_last_event;

// The session.
int64_t g_base = 0;  // fixed-step frame of session frame 0
std::vector<PadIn> g_inputs[kPlayers];  // [player][session frame]
int64_t g_peer_has_mine = -1;           // newest of MY frames the peer holds without a gap
std::map<int64_t, uint64_t> g_fp_local, g_fp_remote;
int64_t g_checked = -1, g_drift = -1;
int64_t g_waits = 0;
double g_wait_total = 0, g_wait_last = 0;
std::atomic<bool> g_waiting{false};
int64_t g_dump_at = -1;  // session frame both PCs dump the world at
bool g_dump_started = false;

// A PLAYER DISCONNECTED (user's rules, 2026-11-17): nothing pauses. The PC
// that's left plays on ALONE: the other player's input is "nothing pressed"
// from then on (the frame gate never waits again), their character stays in
// the level until net_drop_after_s (a PC that stopped answering) or at once
// (a PC that said goodbye), then drops out by itself (lost_controller.cpp
// asks DropOutWanted every frame). Only the player who lost the session sees
// a message: a guest that lost its host. The host's view: the guest left.
std::atomic<bool> g_alone{false};
bool g_alone_keep = false;      // keep their character for the timeout (silence) or not (goodbye)
int64_t g_alone_since = -1;     // session frame it started (-1 = not noted yet)
std::atomic<int> g_drop_wanted{-1};
std::atomic<bool> g_dropped{false};
std::u16string g_notice_heading, g_notice_line;  // shown by the game's thread

// Main thread only.
PadIn g_latest_local;       // this PC's own controller, last read
int64_t g_last_sent = -1;   // newest local frame filed

void Log(const std::string& event) {
  REXLOG_INFO("Network: {}", event);
  g_last_event = event;
}

std::string AddressText(const sockaddr_in& a) {
  char ip[INET_ADDRSTRLEN] = {};
  inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
  return fmt::format("{}:{}", ip, ntohs(a.sin_port));
}
bool SameAddress(const sockaddr_in& a, const sockaddr_in& b) {
  return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

void Header(Writer& w, Type type) {
  w.Bytes(kMagic, 4);
  w.U8(kProtocol);
  w.U8(type);
  w.U16(0);
  w.U32(type == kHello ? 0 : g_token);
}

void SendTo(const Writer& w, const sockaddr_in& to) {
  if (g_sock == kNoSocket) return;
  sendto(g_sock, (const char*)w.b.data(), int(w.b.size()), 0, (const sockaddr*)&to, sizeof(to));
}

// g_m held.
void SendInputsLocked() {
  if (!g_welcomed || !g_in_session.load() || g_alone.load()) return;
  const auto& mine = g_inputs[g_local];
  const int64_t first = std::max<int64_t>(0, g_peer_has_mine + 1);
  const int64_t last = int64_t(mine.size()) - 1;
  Writer w;
  Header(w, kInputs);
  const int count = int(std::clamp<int64_t>(last - first + 1, 0, kMaxInputsPerPacket));
  w.U8(uint8_t(g_local));
  w.U8(uint8_t(count));
  w.U16(0);
  w.U32(uint32_t(int32_t(first)));
  w.U32(uint32_t(int32_t(int64_t(g_inputs[g_remote].size()) - 1)));  // ack
  for (int i = 0; i < count; ++i) {
    const PadIn& in = mine[size_t(first + i)];
    w.U8(in.connected);
    w.Bytes(in.bytes, 12);
  }
  // The newest fingerprints (16 frames: one packet a frame, so each is sent
  // 16 times; how far the PEER compared is its business, not ours).
  std::vector<std::pair<int64_t, uint64_t>> fps;
  for (auto it = g_fp_local.rbegin(); it != g_fp_local.rend() && fps.size() < kMaxFingerprintsPerPacket;
       ++it) {
    fps.push_back(*it);
  }
  w.U8(uint8_t(fps.size()));
  for (auto& [frame, fp] : fps) {
    w.U32(uint32_t(int32_t(frame)));
    w.U64(fp);
  }
  SendTo(w, g_peer);
}

// Compares fingerprints both PCs have (g_m held).
void CompareLocked() {
  while (true) {
    const int64_t next = g_checked + 1;
    auto a = g_fp_local.find(next), b = g_fp_remote.find(next);
    if (a == g_fp_local.end() || b == g_fp_remote.end()) break;
    if (a->second != b->second && g_drift < 0) {
      g_drift = next;
      REXLOG_ERROR("Network: *** DRIFTED at session frame {} *** (fingerprint here {:016X}, other PC "
                   "{:016X}): the two games no longer show the same world. \"Dump both sides\" in "
                   "the Network window records the world data for tools/world_hash_compare.py",
                   next, a->second, b->second);
      g_last_event = fmt::format("DRIFTED at frame {}", next);
    }
    g_checked = next;
    g_fp_remote.erase(g_fp_remote.begin(), std::next(b));
  }
  // Ours: kept a while after we compared them (the peer may not have yet:
  // the newest 16 go out with every packet).
  while (g_fp_local.size() > 128 && g_fp_local.begin()->first <= g_checked) {
    g_fp_local.erase(g_fp_local.begin());
  }
}

void EndSessionLocked(const std::string& why) {
  if (g_role.load() == Role::kNone && !g_in_session.load()) return;
  Log(why);
  g_in_session = false;
  g_welcomed = false;
  g_have_peer = false;
  g_role = Role::kNone;
  g_state = "off (" + why + ")";
  g_presence_dirty = true;  // back to this PC's own devices
  g_cv.notify_all();
}

// The other PC is gone during a session (g_m held). `keep`: their character
// stays for net_drop_after_s first. `me_lost_it`: this PC is the one that lost
// the session (a guest whose host went quiet, or this PC left): it says so.
void GoAloneLocked(const std::string& why, bool keep, bool me_lost_it) {
  if (g_alone.load()) return;
  Log(why + (keep ? fmt::format(": their character stays {} s, then drops out", REXCVAR_GET(net_drop_after_s))
                  : ": their character drops out"));
  g_alone_keep = keep;
  g_alone_since = -1;
  g_alone = true;
  g_welcomed = false;
  g_state = "playing on alone (" + why + ")";
  if (me_lost_it) {
    g_notice_heading = tr(u"Disconnected");
    g_notice_line = g_role.load() == Role::kGuest ? tr(u"The connection to the host was lost")
                                                  : tr(u"You left the online session");
  }
  g_cv.notify_all();  // (a frame waiting at the gate goes on)
}

// -----------------------------------------------------------------------------
// 2. THE NETWORK THREAD
// -----------------------------------------------------------------------------
void SendWelcome() {
  Writer w;
  Header(w, kWelcome);
  w.U32(g_token);
  w.U8(uint8_t(g_remote));
  w.U8(uint8_t(g_delay));
  w.U16(0);
  w.U32(g_seed);
  w.U64(session_settings::Fingerprint());
  w.Text(Build(), kBuildLength);
  SendTo(w, g_peer);
}

void Refuse(const sockaddr_in& to, const std::string& reason) {
  Writer w;
  Header(w, kRefuse);
  w.Text(reason, 64);
  SendTo(w, to);
}

void SettingsCheckLocked() {
  const uint64_t mine = session_settings::Fingerprint();
  if (g_peer_settings != mine) {
    REXLOG_WARN("Network: the other PC's WORLD SETTINGS differ (fingerprint {:016X} here, {:016X} "
                "there): the games WILL drift apart. Console `settings` lists them; make them alike",
                mine, g_peer_settings);
  } else {
    REXLOG_INFO("Network: world settings alike on both PCs ({:016X})", mine);
  }
}

void HandlePacket(const uint8_t* data, size_t size, const sockaddr_in& from) {
  Reader r{data, size};
  char magic[4];
  r.Bytes(magic, 4);
  const uint8_t protocol = r.U8(), type = r.U8();
  r.U16();
  const uint32_t token = r.U32();
  if (!r.ok || std::memcmp(magic, kMagic, 4) != 0) return;  // not ours
  std::lock_guard lock(g_m);
  const Role role = g_role.load();
  if (role == Role::kNone) return;
  if (protocol != kProtocol) {
    if (role == Role::kHost && type == kHello) Refuse(from, "different protocol (update the game)");
    return;
  }
  // HOST: a HELLO opens the session (one guest).
  if (role == Role::kHost && type == kHello) {
    const uint64_t settings = r.U64();
    const std::string build = r.Text(kBuildLength);
    if (!r.ok) return;
    if (g_have_peer && !SameAddress(from, g_peer)) {
      Refuse(from, "the session is full");
      return;
    }
    if (!g_have_peer) {
      g_peer = from;
      g_have_peer = true;
      g_welcomed = true;
      g_peer_settings = settings;
      g_peer_settings_known = true;
      g_peer_build = build;
      g_last_heard = Clock::now();
      Log(fmt::format("player 2 joined from {} (version {})", AddressText(from), build));
      if (build != Build()) {
        REXLOG_WARN("Network: the other PC runs version {}, this one {}", build, Build());
      }
      SettingsCheckLocked();
      g_state = g_in_session ? "playing" : "connected: load the save on both PCs";
    }
    SendWelcome();  // (again, if the first one got lost)
    return;
  }
  // Everything else: only from the peer, with the session's token.
  if (!g_have_peer || !SameAddress(from, g_peer)) return;
  if (type == kRefuse) {
    const std::string reason = r.Text(64);
    if (role == Role::kGuest && !g_welcomed) EndSessionLocked("the host refused: " + reason);
    return;
  }
  if (role == Role::kGuest && type == kWelcome) {
    const uint32_t t = r.U32();
    const int you = r.U8(), delay = r.U8();
    r.U16();
    const uint32_t seed = r.U32();
    const uint64_t settings = r.U64();
    const std::string build = r.Text(kBuildLength);
    if (!r.ok || you != 1 || delay < 1 || delay > 30) return;
    if (g_welcomed) return;  // (a repeat)
    g_token = t;
    g_local = you;
    g_remote = 0;
    g_delay = delay;
    g_seed = seed;
    g_seed_known = true;
    g_peer_settings = settings;
    g_peer_settings_known = true;
    g_peer_build = build;
    g_welcomed = true;
    g_last_heard = Clock::now();
    Log(fmt::format("joined the host at {} as player {} (delay {} frames, version {})",
                    AddressText(from), you + 1, delay, build));
    SettingsCheckLocked();
    g_state = "connected: load the save on both PCs";
    return;
  }
  if (!g_welcomed || token != g_token) return;
  g_last_heard = Clock::now();
  switch (type) {
    case kPing: {
      const uint64_t t = r.U64();
      if (!r.ok) return;
      Writer w;
      Header(w, kPong);
      w.U64(t);
      SendTo(w, g_peer);
      return;
    }
    case kPong: {
      const uint64_t t = r.U64();
      if (!r.ok) return;
      const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              Clock::now().time_since_epoch()).count();
      const double ms = double(now - int64_t(t)) / 1e6;
      if (ms >= 0 && ms < 10000) g_ping_ms = g_ping_ms < 0 ? ms : g_ping_ms * 0.8 + ms * 0.2;
      return;
    }
    case kBye:
      if (g_in_session) {
        GoAloneLocked(role == Role::kHost ? "player 2 left the session" : "the host left the session",
                      false, role == Role::kGuest);
      } else {
        EndSessionLocked("the other PC left the session");
      }
      return;
    case kDump: {
      const int32_t frame = int32_t(r.U32());
      if (!r.ok || frame < 0) return;
      g_dump_at = frame;
      g_dump_started = false;
      Log(fmt::format("the other PC asked for a world dump at session frame {}", frame));
      return;
    }
    case kInputs: {
      const int player = r.U8(), count = r.U8();
      r.U16();
      const int64_t first = int32_t(r.U32());
      const int64_t ack = int32_t(r.U32());
      if (!r.ok || player != g_remote || count > kMaxInputsPerPacket || first < 0) return;
      if (!g_in_session) return;  // (not started here yet: they come again)
      std::vector<PadIn> got(static_cast<size_t>(count));
      for (PadIn& in : got) {
        in.connected = r.U8() ? 1 : 0;
        r.Bytes(in.bytes, 12);
      }
      const int fp_count = r.U8();
      if (!r.ok || fp_count > kMaxFingerprintsPerPacket) return;
      std::vector<std::pair<int64_t, uint64_t>> fps(static_cast<size_t>(fp_count));
      for (auto& f : fps) {
        f.first = int32_t(r.U32());
        f.second = r.U64();
      }
      if (!r.ok) return;
      // Their inputs: only the part that continues ours without a gap.
      auto& theirs = g_inputs[g_remote];
      bool added = false;
      for (int i = 0; i < count; ++i) {
        const int64_t frame = first + i;
        if (frame < int64_t(theirs.size())) continue;  // have it (a resend)
        if (frame > int64_t(theirs.size())) break;     // gap: wait for the resend
        theirs.push_back(got[size_t(i)]);
        added = true;
      }
      // What they hold of mine.
      if (ack > g_peer_has_mine && ack < int64_t(g_inputs[g_local].size())) g_peer_has_mine = ack;
      for (auto& [frame, fp] : fps) {
        if (frame > g_checked) g_fp_remote[frame] = fp;
      }
      CompareLocked();
      if (added) g_cv.notify_all();
      return;
    }
    default:
      return;
  }
}

void SendHello() {
  Writer w;
  Header(w, kHello);
  w.U64(session_settings::Fingerprint());
  w.Text(Build(), kBuildLength);
  SendTo(w, g_peer);
}

void NetworkThread() {
  auto next_ping = Clock::now();
  auto next_hello = Clock::now();
  std::vector<uint8_t> buffer(2048);
  while (!g_stop.load()) {
#if defined(_WIN32)
    fd_set set;
    FD_ZERO(&set);
    FD_SET(g_sock, &set);
    timeval tv{0, 50000};
    const int ready = select(0, &set, nullptr, nullptr, &tv);
#else
    pollfd pfd{g_sock, POLLIN, 0};
    const int ready = poll(&pfd, 1, 50);
#endif
    if (ready > 0) {
      sockaddr_in from{};
      socklen_t from_size = sizeof(from);
      const int got = int(recvfrom(g_sock, (char*)buffer.data(), int(buffer.size()), 0,
                                   (sockaddr*)&from, &from_size));
      if (got > 0 && from.sin_family == AF_INET) HandlePacket(buffer.data(), size_t(got), from);
    }
    const auto now = Clock::now();
    std::lock_guard lock(g_m);
    const Role role = g_role.load();
    if (role == Role::kNone) continue;
    if (role == Role::kGuest && !g_welcomed && now >= next_hello) {
      next_hello = now + std::chrono::milliseconds(250);
      SendHello();
    }
    if (g_welcomed && now >= next_ping) {
      next_ping = now + std::chrono::milliseconds(500);
      Writer w;
      Header(w, kPing);
      w.U64(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count()));
      SendTo(w, g_peer);
      // While waiting at the gate the game thread resends; otherwise keep the
      // peer supplied here too (a lost packet must not stall it).
      SendInputsLocked();
    }
    // Silence: in a session after 2 s (nobody may stand still longer than
    // that), before one after 8 s.
    if (g_welcomed && g_in_session && !g_alone && now - g_last_heard > std::chrono::seconds(2)) {
      GoAloneLocked(role == Role::kHost ? "player 2's PC stopped answering" : "the host stopped answering", true,
                    role == Role::kGuest);
    } else if (g_welcomed && !g_in_session && now - g_last_heard > std::chrono::seconds(8)) {
      EndSessionLocked("the other PC stopped answering (8 s)");
    }
  }
}

void CloseSocket() {
  g_stop = true;
  if (g_thread.joinable()) g_thread.join();
  if (g_sock != kNoSocket) {
#if defined(_WIN32)
    closesocket(g_sock);
#else
    close(g_sock);
#endif
    g_sock = kNoSocket;
  }
  g_stop = false;
}

// Opens the socket (host: on `port`; guest: any port) and the thread.
std::string OpenSocket(int port) {
#if defined(_WIN32)
  static bool wsa = [] {
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0;
  }();
  if (!wsa) return "Windows sockets didn't start";
#endif
  CloseSocket();
  g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (g_sock == kNoSocket) return "no UDP socket";
  sockaddr_in me{};
  me.sin_family = AF_INET;
  me.sin_addr.s_addr = htonl(INADDR_ANY);
  me.sin_port = htons(uint16_t(port));
  if (bind(g_sock, (const sockaddr*)&me, sizeof(me)) != 0) {
    CloseSocket();
    return fmt::format("port {} is taken (another copy hosting?)", port);
  }
  g_thread = std::thread(NetworkThread);
  return "";
}

void ResetSessionLocked() {
  for (auto& v : g_inputs) v.clear();
  g_peer_has_mine = -1;
  g_fp_local.clear();
  g_fp_remote.clear();
  g_checked = g_drift = -1;
  g_waits = 0;
  g_wait_total = g_wait_last = 0;
  g_dump_at = -1;
  g_in_session = false;
  g_ping_ms = -1;
  g_peer_settings_known = false;
  g_alone = false;
  g_alone_since = -1;
  g_drop_wanted = -1;
  g_dropped = false;
  g_notice_heading.clear();
}

// Turns the fixed step on (60, paced) if it isn't: lockstep needs it. At
// launch (Init) the flag is set instead, so it starts like --fixed_step=60.
void NeedFixedStep() {
  if (REXCVAR_GET(fixed_step) == 60 && !fixed_step::Active()) return;  // (launch: comes on at the 1st pass)
  if (!fixed_step::Active() || fixed_step::Fps() != 60) {
    REXLOG_INFO("Network: turning the fixed step on (60 frames a second, paced)");
    fixed_step::Set(60, false);
  }
}

}  // namespace

// =============================================================================
// Public: setting up
// =============================================================================
std::string Host(int port) {
  if (g_role.load() != Role::kNone) return "already in a session (Leave first)";
  CloseSocket();  // a socket left from an ended session (its thread takes g_m: not under it)
  std::lock_guard lock(g_m);
  if (g_role.load() != Role::kNone) return "already in a session (Leave first)";
  ResetSessionLocked();
  if (std::string why = OpenSocket(port); !why.empty()) return why;
  g_token = std::random_device{}() | 1u;
  g_local = 0;
  g_remote = 1;
  g_delay = std::clamp(REXCVAR_GET(net_delay), 1, 30);
  g_seed = fixed_step::SessionSeed();  // (M1: the default; a host's own pick later)
  g_seed_known = true;
  g_have_peer = false;
  g_welcomed = false;
  g_role = Role::kHost;
  g_presence_dirty = true;
  g_state = fmt::format("hosting on UDP port {}: waiting for player 2", port);
  Log(g_state);
  NeedFixedStep();
  return "";
}

std::string Join(const std::string& address, int port) {
  if (g_role.load() != Role::kNone) return "already in a session (Leave first)";
  CloseSocket();  // (see Host)
  std::lock_guard lock(g_m);
  if (g_role.load() != Role::kNone) return "already in a session (Leave first)";
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(uint16_t(port));
  if (inet_pton(AF_INET, address.c_str(), &to.sin_addr) != 1) {
    // A name ("my-pc.local"): look it up once.
    addrinfo hints{}, *found = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(address.c_str(), nullptr, &hints, &found) != 0 || !found) {
      return "can't find address " + address;
    }
    to.sin_addr = reinterpret_cast<sockaddr_in*>(found->ai_addr)->sin_addr;
    freeaddrinfo(found);
  }
  ResetSessionLocked();
  if (std::string why = OpenSocket(0); !why.empty()) return why;
  g_peer = to;
  g_have_peer = true;
  g_welcomed = false;
  g_seed_known = false;
  g_local = 1;
  g_remote = 0;
  g_token = 0;
  g_role = Role::kGuest;
  g_presence_dirty = true;
  g_state = "joining " + AddressText(to) + " ...";
  Log(g_state);
  NeedFixedStep();
  return "";
}

void Leave() {
  {
    std::lock_guard lock(g_m);
    if (g_role.load() == Role::kNone || g_alone) return;
    if (g_welcomed) {
      Writer w;
      Header(w, kBye);
      for (int i = 0; i < 3; ++i) SendTo(w, g_peer);  // (UDP: say it a few times)
    }
    // In a level: this PC plays on alone (the other player drops out here).
    // Before one: simply back to normal.
    if (g_in_session) {
      GoAloneLocked("left the session", false, true);
    } else {
      EndSessionLocked("left the session");
    }
  }
  CloseSocket();
}

void Init() {
  online_rules::Init();
  // Whatever we were, the dump / drift tools also want the session start
  // from the Load Game menu (not only quick load): "Load successful" 282 ->
  // its exit 283 into the level.
  fight_tree::AddFrontEndExitListener([](PPCContext&, uint8_t*, int32_t from, int32_t exit) {
    if (from == 282 && exit == 283 && Configured()) fixed_step::StartSession();
  });
  const int port = REXCVAR_GET(net_port);
  if (!REXCVAR_GET(net_host) && REXCVAR_GET(net_join).empty()) return;
  if (REXCVAR_GET(fixed_step) != 60) {
    REXLOG_INFO("Network: --fixed_step=60 from launch (lockstep needs it)");
    rex::cvar::SetFlagByName("fixed_step", "60");
  }
  if (REXCVAR_GET(fixed_step_fast)) {
    REXLOG_WARN("Network: --fixed_step_fast is on: this PC runs ahead and waits at every frame");
  }
  std::string why;
  if (REXCVAR_GET(net_host)) {
    why = Host(port);
  } else if (!REXCVAR_GET(net_join).empty()) {
    why = Join(REXCVAR_GET(net_join), port);
  } else {
    return;
  }
  if (!why.empty()) REXLOG_ERROR("Network: couldn't start: {}", why);
}

bool Configured() { return g_role.load() != Role::kNone; }
uint32_t PlayerMask() {
  if (!Configured()) return 0u;
  // Alone after the other player dropped out: only this PC's player has a
  // controller (a guest stays player 2).
  return g_dropped.load() ? 1u << g_local : 0x3u;
}

int DropOutWanted() { return g_dropped.load() ? -1 : g_drop_wanted.load(); }

void DroppedOut(int player) {
  if (g_drop_wanted.load() != player || g_dropped.exchange(true)) return;
  REXLOG_INFO("Network: player {} dropped out (left the online session)", player + 1);
  g_presence_dirty = true;
}
bool InSession() { return g_in_session.load(); }
int LocalPlayer() { return Configured() ? g_local : -1; }

// =============================================================================
// 3. THE GAME'S SIDE (main thread)
// =============================================================================
bool SessionSeed(uint32_t& seed) {
  std::lock_guard lock(g_m);
  if (!Configured() || !g_seed_known) return false;
  seed = g_seed;
  return true;
}

void OnSessionStart(uint64_t fixed_step_frame) {
  std::lock_guard lock(g_m);
  if (!Configured()) return;
  if (g_in_session) {
    // Both PCs got here on the same session frame (lockstep: same inputs,
    // same menus): the world was reset alike on both. The session goes on.
    Log(fmt::format("a save was loaded again inside the session (frame {})",
                    int64_t(fixed_step_frame + 1) - g_base));
    return;
  }
  if (!g_seed_known) {
    REXLOG_ERROR("Network: the level started before the host answered: NO SESSION. Load the save "
                 "again once the Network window says \"connected\"");
    g_last_event = "level started before connecting: load again";
    return;
  }
  for (auto& v : g_inputs) v.clear();
  // Frames 0 .. delay-1: nobody can have pressed anything yet (every PC
  // agrees without asking).
  for (auto& v : g_inputs) v.assign(size_t(g_delay), PadIn{});
  g_peer_has_mine = -1;
  g_fp_local.clear();
  g_fp_remote.clear();
  g_checked = g_drift = -1;
  g_base = int64_t(fixed_step_frame) + 1;  // the NEXT pass is session frame 0
  g_last_sent = g_delay - 1;
  g_latest_local = PadIn{};
  g_in_session = true;
  g_state = "playing";
  Log(fmt::format("SESSION START (fixed-step frame {} = session frame 0, seed {:08X}, delay {})",
                  g_base, g_seed, g_delay));
}

void OnPassStart(uint64_t fixed_step_frame) {
  if (g_presence_dirty.exchange(false)) {
    if (auto* a = kbm::PlayerAssignment::Get()) a->RefreshPresence();
  }
  if (!g_in_session.load()) return;
  const int64_t s = int64_t(fixed_step_frame) - g_base;
  if (s < 0) return;
  std::lock_guard lock(g_m);
  if (!g_in_session) return;
  if (g_alone) {
    if (g_alone_since < 0) g_alone_since = s;
    const int64_t timeout = int64_t(std::max(0, REXCVAR_GET(net_drop_after_s))) * 60;
    if (!g_dropped && (!g_alone_keep || s - g_alone_since >= timeout)) g_drop_wanted = g_remote;
    if (!g_notice_heading.empty()) {
      achievements::ShowNotice(g_notice_heading, g_notice_line);
      g_notice_heading.clear();
    }
  }
  // The world as the previous frame left it.
  if (s >= 1) g_fp_local[s - 1] = world_hash::QuickFingerprint();
  // This PC's controller (last read) plays on frame s + delay.
  auto& mine = g_inputs[g_local];
  while (int64_t(mine.size()) <= s + g_delay) mine.push_back(g_latest_local);
  g_last_sent = int64_t(mine.size()) - 1;
  CompareLocked();
  SendInputsLocked();
  // Every 10 s of session: one line for the log (tests read it).
  if (s > 0 && s % 600 == 0) {
    REXLOG_INFO("Network: session frame {}: {}, {} frames waited ({:.0f} ms), ping {:.1f} ms", s,
                g_drift >= 0 ? fmt::format("DRIFTED at frame {}", g_drift)
                             : fmt::format("in sync through frame {}", g_checked),
                g_waits, g_wait_total, g_ping_ms);
  }
  // "Dump both sides": start recording a little before the frame.
  if (g_dump_at >= 0 && !g_dump_started && s >= g_dump_at - 30) {
    g_dump_started = true;
    const std::string file =
        (game_folder::UserFolder() / "logs" /
         fmt::format("net-dump-player{}-frame{}.whash", g_local + 1, g_dump_at))
            .string();
    const std::string frame = std::to_string(g_base + g_dump_at);
    try {
      const std::string reply = world_hash::Command({"worldhash", file, "40", "dump", frame});
      Log("world dump: " + reply);
    } catch (const std::exception& e) {
      Log(std::string("world dump failed: ") + e.what());
    }
  }
}

int64_t BetweenFrames(uint64_t fixed_step_frame) {
  if (!g_in_session.load()) return 0;
  const int64_t s = int64_t(fixed_step_frame) - g_base;
  if (s < 0) return 0;
  std::unique_lock lock(g_m);
  // THE FRAME GATE. Needed: the other PC's input for this frame. Asked for:
  // a little more, its input for frame s + margin, i.e. it has started frame
  // s + margin - delay. With delay 3 and margin 1 neither PC runs more than 2
  // frames ahead of the other, and there's a frame of room both ways for a
  // late packet before anyone stands still (the plain "frame s" rule let the
  // first PC run a full `delay` ahead: no room left on its side, a small wait
  // on nearly every frame).
  const int64_t margin = (g_delay - 1) / 2;
  auto& theirs = g_inputs[g_remote];
  if (g_alone) {
    // Nobody to wait for: their input is "nothing pressed" from here on
    // ("not connected" once they dropped out: OverridePad).
    while (int64_t(theirs.size()) <= s + margin) theirs.push_back(PadIn{});
    return 0;
  }
  if (int64_t(theirs.size()) > s + margin || !g_in_session) return 0;
  const auto start = Clock::now();
  g_waiting = true;
  auto next_resend = start;
  while (g_in_session && !g_alone && int64_t(theirs.size()) <= s + margin) {
    const auto now = Clock::now();
    if (now >= next_resend) {
      next_resend = now + std::chrono::milliseconds(100);
      SendInputsLocked();
    }
    g_cv.wait_for(lock, std::chrono::milliseconds(2));
  }
  g_waiting = false;
  while (g_alone && int64_t(theirs.size()) <= s + margin) theirs.push_back(PadIn{});  // (gone while waiting)
  const auto waited = Clock::now() - start;
  const double ms = std::chrono::duration<double, std::milli>(waited).count();
  ++g_waits;
  g_wait_total += ms;
  g_wait_last = ms;
  if (ms > 500) {
    REXLOG_INFO("Network: frame {} waited {:.0f} ms for the other PC", s, ms);
  }
  return std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count();
}

bool FrameInput(int player, uint8_t gamepad[12]) {
  if (!g_in_session.load() || player < 0 || player >= kPlayers) return false;
  const int64_t s = int64_t(fixed_step::Frame()) - g_base;
  std::lock_guard lock(g_m);
  if (s < 0 || s >= int64_t(g_inputs[player].size())) return false;
  const PadIn& in = g_inputs[player][size_t(s)];
  if (!in.connected) return false;
  std::memcpy(gamepad, in.bytes, 12);
  return true;
}

bool OverridePad(uint32_t socket, uint8_t* gamepad, uint32_t& result) {
  if (!Configured()) return false;
  // This PC's own controller = whatever answers socket 0 here (the keyboard
  // and this PC's pads; a test replay's override included).
  if (socket == 0) {
    g_latest_local.connected = result == 0 ? 1 : 0;
    std::memcpy(g_latest_local.bytes, gamepad, 12);
    if (!g_latest_local.connected) std::memset(g_latest_local.bytes, 0, 12);
  }
  if (!g_in_session.load()) return false;
  const int64_t s = int64_t(fixed_step::Frame()) - g_base;
  if (s < 0) return false;
  constexpr uint32_t kNotConnected = 0x48F;  // ERROR_DEVICE_NOT_CONNECTED
  std::lock_guard lock(g_m);
  if (!g_in_session) return false;
  if (socket >= uint32_t(kPlayers)) {
    std::memset(gamepad, 0, 12);
    result = kNotConnected;
    return true;
  }
  const auto& v = g_inputs[socket];
  if (s >= int64_t(v.size())) {
    // Can't happen after the gate; if it does, nothing pressed (logged once).
    static bool told = false;
    if (!told) {
      told = true;
      REXLOG_ERROR("Network: player {}'s input for frame {} was missing at a controller read",
                   socket + 1, s);
    }
    std::memset(gamepad, 0, 12);
    result = 0;
    return true;
  }
  if (int(socket) == g_remote && g_dropped) {
    std::memset(gamepad, 0, 12);
    result = kNotConnected;
    return true;
  }
  const PadIn& in = v[size_t(s)];
  std::memcpy(gamepad, in.bytes, 12);
  result = in.connected ? 0 : kNotConnected;
  return true;
}

// =============================================================================
// The Network window
// =============================================================================
Status GetStatus() {
  std::lock_guard lock(g_m);
  Status st;
  st.role = g_role.load();
  st.connected = g_welcomed;
  st.in_session = g_in_session;
  st.peer = g_have_peer ? AddressText(g_peer) : "";
  st.state = g_state;
  st.ping_ms = g_ping_ms;
  st.delay = g_delay;
  st.local_player = st.role == Role::kNone ? -1 : g_local;
  if (g_in_session) st.session_frame = int64_t(fixed_step::Frame()) - g_base;
  st.remote_inputs = int64_t(g_inputs[g_remote].size());
  st.waits = g_waits;
  st.wait_ms_total = g_wait_total;
  st.last_wait_ms = g_wait_last;
  st.waiting_now = g_waiting.load();
  st.my_settings = session_settings::Fingerprint();
  st.peer_settings = g_peer_settings;
  st.peer_settings_known = g_peer_settings_known;
  st.checked_through = g_checked;
  st.drift_frame = g_drift;
  st.last_event = g_last_event;
  st.alone = g_alone;
  st.dropped = g_dropped;
  return st;
}

std::string RequestDump() {
  std::lock_guard lock(g_m);
  if (!g_in_session) return "no session running";
  const int64_t at = int64_t(fixed_step::Frame()) - g_base + 120;  // 2 s from now
  g_dump_at = at;
  g_dump_started = false;
  Writer w;
  Header(w, kDump);
  w.U32(uint32_t(int32_t(at)));
  for (int i = 0; i < 3; ++i) SendTo(w, g_peer);
  Log(fmt::format("world dump of both PCs at session frame {} (files net-dump-*.whash in user/logs)", at));
  return fmt::format("both PCs dump at session frame {}", at);
}

}  // namespace net
