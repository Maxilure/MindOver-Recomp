# 59. Initial network co-op foundation

The port already supports up to four local players, but that does not connect
separate game processes. A LAN or Steam transport needs a safe place to hand
remote controller state to the game's existing input path and a transport
independent message format. This change adds those two foundations; it does
not create a network session.

## 1. What was established

The game's XInput wrapper, `sub_824742F0`, is already the native boundary where
the port observes controller state before the game consumes it. Its arguments
identify an XInput user slot and a guest address for the 16-byte
`XINPUT_STATE`. The port's player assignment also has a four-player presence
mask. Remote input can therefore be handed off at this boundary without
letting a transport thread access guest memory or inventing a second gameplay
input path.

The current player tables and co-op camera describe local co-op. They do not
replicate world state between processes, so this input handoff alone cannot
make independently running games stay synchronized.

## 2. What changed

`src/network/remote_input.*` provides four thread-safe remote slots. A future
session layer can reserve a slot and publish its latest controller state. The
game thread consumes that state from the XInput wrapper after the SDK polls
local devices. The state is written explicitly in the guest's big-endian
layout: packet number at `+0`, buttons at `+4`, triggers at `+6` and `+7`, and
four signed axes from `+8` through `+15`. Remote-owned slots count as present
in the game's player-presence mask; local keyboard merging is skipped for
those slots.

`src/network/protocol.*` defines protocol v1 without Steam or socket types. A
packet has a 22-byte big-endian header and at most 64 payload bytes. It carries
message type, protocol version, session ID, packet sequence and payload size.
The codec checks known message types, exact per-message lengths, reserved
bits, session-ID rules, player-slot ranges and XInput button bits. The input
payload carries a game frame and explicit button, trigger and axis values;
it contains no guest pointer or game-memory image. Slot ownership must come
from the authenticated connection in the future session layer, never from the
packet itself.

The standalone tests cover packet round trips, malformed packets and the
remote-state handoff into big-endian guest bytes. They run without loading the
game files.

## 3. Verification

The Windows AMD64 RelWithDebInfo build completed on a clean clone of the
upstream repository. Both `network_protocol_tests` and
`network_remote_input_tests` passed. The game was launched with copied test
user data, an isolated cache, muted audio and an explicit frame cap; it reached
a playable scene and continued rendering. This confirms the integration does
not prevent normal startup. It does not exercise the network code in-game:
there is not yet a transport or a caller that reserves remote slots.

## 4. Still open

- No LAN discovery, socket transport, Steam Networking Sockets adapter or
  transport-neutral session state exists yet.
- No production caller assigns peer connections to slots or calls
  `SetRemoteOwner` / `Publish`.
- Packet sequence values are encoded but replay and stale-packet policy are
  not implemented. Input freshness timeouts and disconnect recovery remain
  open.
- Build compatibility, authentication, rate limits and session startup need
  explicit policy before accepting peers.
- Each process still needs a usable, synchronized view of the game world.
  Determine whether the intended design is host-only simulation, state
  replication or another approach before presenting this as a complete
  online mode.
- Player-facing session UI remains to be implemented with the game's menus
  and translated text.
