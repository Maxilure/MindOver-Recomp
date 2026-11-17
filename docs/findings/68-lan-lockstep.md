# 68. LAN co-op, first step: two copies of the game in lockstep

Status: **experimental test tool** (online plan milestone M1). Two copies of the
game, one player each, on one PC or a LAN. Source: `src/net/`.

## 1. The idea

Each PC runs the whole game. Only the players' controller states travel over
the network: every game frame, each PC waits until it holds every player's
controller state for that frame, then runs the frame with them. Nothing about
the world (positions, enemies, objects) is ever sent.

This only works because the game is **deterministic** once the fixed step is on
(every frame exactly 1/60 s, helper threads and sound on game frames, the
random generators and the clock reset at the session's start; earlier work in
`fixed_step.h` and `system/helper_threads.h`): the same inputs on the same
frames give the same world, frame for frame, on every PC.

```
PC 1 (host)  = player 1            PC 2 (guest) = player 2
reads its keyboard / pad           reads its keyboard / pad
for frame N + delay                for frame N + delay
         \______ UDP: "my input for frames A..B" ______/
frame N runs when both inputs for N are in (the FRAME GATE)
```

**Input delay**: a press read on frame N plays on frame N + 3 (50 ms at 60 fps,
`--net_delay`). The other PC's packet has that long to arrive before anybody
waits.

## 2. Where it hooks in

| Place | What happens there |
|---|---|
| Main loop's pass hook (`fixed_step.cpp`, the frame's time delta) | Before the helper threads may run: this PC's input goes out for frame s + delay, and the frame that just ended is fingerprinted. After they may run: **the frame gate** (a wait here is "between frames": the helpers finish their work, their clock stands still, so a wait changes nothing in the world). |
| The game's controller read (`sub_824742F0`, wrapped in `save_library.cpp`) | Socket 0 = this PC's own player (keyboard + pads), noted; then every socket is answered from the session's table for this frame: sockets 0-1 = players 1-2, sockets 2-3 "not connected". |
| The game's controller capabilities call (`sub_824742E8`) | Sockets 0-1 answer "standard pad" on both PCs. Without it the other PC's player never joined: the game creates a controller object only for a socket whose capabilities answer, so player 2's START was never read on the host. |
| Who has a controller (`players.cpp` PresentMask) | Players 1 and 2, on both PCs, whatever devices each PC has. |
| Session start (`fixed_step::StartSession`) | The front end's "Load successful" exit (state 282 -> 283) or quick load: clock + random generators reset from the host's seed; session frame 0 = the next main-loop pass. At most once per frame (two callers in the same frame would otherwise restart the clock twice on one PC only). |

Also changed for a session, because they read this PC's devices or write into
the world directly:

* the keyboard reaches the game as controller state (its native input events
  would reach only one PC's game);
* mouse aiming is off (it writes the aim point into the world, not through the
  controller state);
* player 1's "controller unplugged" watch is off.

## 3. The frame gate's margin

First version: wait for the other PC's input for frame s. The PC that started
first then ran exactly `delay` frames ahead of the other, with no room left on
its side: it waited a few milliseconds on nearly every frame (6,300 waits in a
2-minute run), and any late packet would have stalled it.

Now: wait for the other PC's input for frame s + (delay - 1) / 2, i.e. one frame
of room both ways at delay 3. And after a wait, the paced clock moves its
schedule by the time waited, so the PC that was ahead takes the other's
rhythm instead of keeping its own phase. Result: 9 waits after the start in the
same kind of run.

## 4. Is it still the same world?

Each PC computes a small **fingerprint** at every frame's end
(`world_hash::QuickFingerprint`: the main random generator + every player's
position, velocity and titan flag; never a heap address, which may differ
between PCs) and sends its newest 16 with every packet. The first frame where
they differ is reported on both PCs ("DRIFTED at frame N": log error, console
`net`, Network window). **Dump both sides** asks both PCs to record the world's
data at the same session frame; `tools/world_hash_compare.py` then names the
differing words.

The **world settings fingerprint** (`system/session_settings.h`) is compared
when the PCs connect: a different value is a warning on both (those settings
change what the game decides).

## 5. Tests (one PC, two windows)

Both copies load the same save; a recorded script drives each copy's player 1
socket (player 1's part on the host, player 2's part relabelled on the guest).

| Test | Result |
|---|---|
| 70 s, player 2 joins, both move and fight | Session starts on the same frame on both; player 2 joins on both; players' tracks identical on every frame (16,812 rows); fingerprints equal on every frame |
| One copy with a different world setting | Warning on both at connect |
| Random generator poked on the guest (console `write`) | "DRIFTED at frame 972" on both PCs |
| Dump both sides | Both dumps at the same frame; the compare tool names exactly the poked word |

## 6. Security (the LAN part)

Packets carry a magic word, a protocol number and the session's random token;
every size and range is checked before use; after the handshake only the one
peer address is listened to. Only controller states, frame numbers and
fingerprints are accepted: nothing a packet says is ever used as a memory
address or a file name.

## 6b. A player disconnects

Nothing pauses. When the other PC goes quiet (2 s without a packet) or leaves,
the PC that's left **plays on alone**: the other player's input is "nothing
pressed" from then on, so the frame gate never waits again. Their character
stays in the level for `--net_drop_after_s` (30 s; at once when they left on
purpose) and then **drops out by itself** through the game's own Drop Out (the
"controller disconnected" question isn't asked). Only the player who lost the
session sees a message (the message bar card "Disconnected"): a guest whose
host went quiet, or a PC that pressed Leave. The host's view is always "the
guest left", with no message.

| Test (drop after 5 s) | Result |
|---|---|
| Guest's PC stops answering | Host plays on, no pause; player 2 stays 5 s, then drops out; no message on the host |
| Host's PC stops answering | Guest plays on as player 2; player 1 drops out after 5 s; the guest sees "Disconnected" |
| Guest presses Leave | Player 2 drops out at once on both PCs; the guest plays on alone and sees "Disconnected" |
| 30 minutes, 2 players (108,000 frames) | In sync on every frame, players' tracks identical, 24 frames waited at the gate |

## 7. Not yet

* Coming back after a disconnect (late join / reconnect: the plan's milestone M5).
* Pause: the pause menu still stops the world on both PCs (next: an online pause
  that only opens the menu). Saves: the host-only rule isn't in yet.
* Each PC's own camera following its own player (the plan's milestone M4).
* A session starts at a save load: Host / Join first, then load the same save
  on both. The save itself isn't sent or checked (a different save shows as a
  drift at frame 0).
* Two players, one per PC; menus, pause and cutscene-skip rules; late join;
  the internet; a menu in the game's own look. Windows: the socket code is
  written but not built there yet.

## Controls

* `--net_host` (player 1) / `--net_join=<address>` (player 2), `--net_port`
  (default 37373), `--net_delay` (default 3), `--net_drop_after_s` (default 30). Both turn the fixed step on (60).
* F1 Tools -> **Network (LAN co-op test)**: Host / Join / Leave and the status.
* Debug console: `net` (status line), `net dump`, `net leave`.
