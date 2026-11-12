<!--
Purpose: track the staged implementation of LAN and Steam co-op.
Game integration starts at the native XInput wrapper and four-player tables;
network messages must contain explicit protocol data, never guest addresses.
See findings/26 for the existing local player slots and findings/59 for the
remote input handoff and packet format.
-->
# Multiplayer implementation

This directory tracks the implementation plan and protocol decisions for
multiplayer over LAN and Steam Networking. The existing game code supports up
to four players in one process (`--local_players`); it does not synchronize
two game processes or their world state.

## Current progress

- **Step 1: remote input handoff.** Implemented a thread-safe buffer for four
  remote XInput states and connected it to `sub_824742F0`. Remote ownership is
  reflected in the game's player-presence mask. Its handoff unit test checks
  slot ownership and the big-endian guest state. There is no transport yet.
- **Step 2: session and wire protocol.** The bounded protocol v1 codec is
  implemented in `src/network/protocol.*`; it validates message types, exact
  payload lengths, reserved bits, session IDs and XInput button bits. The
  standalone tests cover all message types and malformed input. Session state,
  peer-to-slot assignment and message handling are still not started.
- **Step 3: LAN transport.** Not started. Use the same protocol as Steam.
- **Step 4: Steam transport.** Not started. Keep Steam identity and session
  setup inside the Steam adapter.
- **Step 5: simulation and recovery.** Not started. Establish whether the host
  alone can run the session or whether clients need replicated world state;
  specify disconnect, level transition and resynchronization behavior.
- **Step 6: player-facing session UI.** Not started. Use the game's Scrooby
  menus and translated text.

## Initial design constraints

1. One transport-neutral session layer owns role, peer identity, player-slot
   assignment and connection state.
2. LAN and Steam adapters send the same versioned messages. A message includes
   a type, protocol version, session identifier, sender, sequence number and
   payload length. Parsers reject unknown versions, invalid player slots,
   truncated payloads and out-of-range controller fields.
3. Controller state is sent as explicit button, trigger and axis values. No
   packet carries guest pointers, guest memory dumps or compiler structures.
4. The game thread consumes the newest validated controller state through
   `network/remote_input.*`; network callbacks do not write game memory.
5. Input freshness needs a timeout that neutralizes a silent/disconnected
   peer. Reliable session events and frequent input updates need separate
   delivery policies.
6. Do not treat matching input streams as proof that independently running
   game worlds remain synchronized. Measure that before choosing lockstep or
   state replication.

## Protocol v1 wire layout

Every integer is big-endian. A datagram is one complete message; framing is
not inferred from stream boundaries.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | Magic `MOMN` |
| 4 | 2 | Protocol version (`1`) |
| 6 | 1 | Message type (`Hello`, `Welcome`, `Reject`, `Input`, `KeepAlive`, `Start`) |
| 7 | 1 | Reserved flags (must be zero) |
| 8 | 8 | Session identifier |
| 16 | 4 | Packet sequence |
| 20 | 2 | Payload size (maximum 64) |
| 22 | variable | Type-specific payload; exact size required |

`Hello` and pre-session `Reject` may use session id zero. Other message types
require a nonzero session id. `Input` carries a game frame followed by the
12-byte XInput gamepad state. It has no player slot: the session layer must
derive the assigned slot from the sending connection. The codec validates
structure, not peer identity, replay windows, rate limits or game-version
compatibility; those belong to session/transport policy.

| Message | Payload | Size |
|---|---|---:|
| `Hello` | build fingerprint (`u64`) | 8 |
| `Welcome` | first assigned XInput user (`u8`), assigned seat count (`u8`), two zero reserved bytes | 4 |
| `Reject` | reason code (`u16`) | 2 |
| `Input` | game frame (`u32`), XInput gamepad state (`12` bytes) | 16 |
| `KeepAlive` | latest game frame (`u32`) | 4 |
| `Start` | initial game frame (`u32`) | 4 |

The packet sequence is monotonic per sending connection; the session layer
must reject duplicate or stale values. No wraparound policy is implemented
yet. `Hello` currently carries a build fingerprint but does not negotiate a
version range: peers must speak protocol v1 and the host must compare build
fingerprints before accepting them.

## Next implementation step

Define the transport-neutral session API and per-peer sequencing/timeout
policy, then implement a LAN adapter first and a Steam Networking Sockets adapter
second. The initial gameplay target should be a host running the existing
four-player simulation, with each connected peer assigned one player slot;
the prototype must determine how clients receive a usable view of that game
state before this is treated as a complete online mode.
