# 55. The physics frame, read end to end: what depends on the frame rate

2026-11-06. A read-through of the game's character physics (`CPhysicsBehaviour`,
vtable `0x82032FC4`, code `0x8217B000-0x82184000`) and the code it talks to,
to find every place where the original 30 fps shows through at higher frame
rates. Method: Ghidra pseudo-C of all 217 functions in the physics range and
242 in the collision range (`tools/ghidra_decompile.sh`), checked against
`xexdis` where Ghidra gave up (functions that use the save/restore helpers),
then the running game: the debug console's `who` (findings/29 s.4) on a
titan's and Crash's position and ground contact, and reads of the
physics' own fields while walking, jumping and standing.

Short version: most of the physics is written in **seconds**, not frames, and
behaves the same at any frame rate: gravity, drag (fixed 1/60 s steps), both
"stuck" timers, the measured speed. What does depend on the frame rate is a
handful of **per-frame minimums** (a step, a push or a speed that must be
bigger than a fixed number, found and fixed for moving floors in findings/22
s.9) and the way **gravity keeps a body on a downward slope**: each frame it
pulls the body down by gravity x frame time², about 36 times less at 180 fps
than at 30 (findings/22 s.8). The second is the likely root of the slope trips
and the "bumpy ground"; the first is a pattern to fix one site at a time.

## 1. Who moves a character

A character's position reaches the physics in one of five ways. Which one a
move uses decides which frame-rate rules apply to it.

| Way | Entry | Used by (seen) | Frame rate |
|---|---|---|---|
| **Velocity** | physics velocity +104, integrated every frame | walking on foot (Crash and titans: a titan walking showed velocity 7.6 / 3.7 / -6.0), falling | time-based (s.2) |
| **Set position** | message 0x27 → `sub_8217F528` (writes the world matrix and the physics position) | a rider following its titan, placed moves (a titan's jump: velocity 0, no ground contact while in the air) | none of its own: it's whatever the sender computed |
| **Move by** | command 1 of message 0x19 → `sub_8217E6B0` | path followers (moving floors) | per-frame minimum, fixed (findings/22 s.9) |
| **Move with collision** | command 3 → `sub_8217F3E8` (sweeps the collision ellipsoid with `sub_82254270`) | (not caught in a test yet) | to measure |
| **Velocity commands** | command 0 (take away speed along a direction, `sub_8217E5C8`), 5 (add velocity, `sub_8217EA58` → `sub_8217EAC0`), 9 (gravity on / off), 10 (gravity value), 14 (air drag) | the character's move code, knockbacks | add velocity has a minimum (s.3) |

The set-position chain, caught by `who` on a ridden titan's Crash: the
fight tree's update (`CCharacterFightTreeBehaviour` slot 4, `sub_8212EE90`)
→ Crash's controller (`Crash` slot 5, `sub_821ACAD8`) → the script controller
and the behaviour controller (slot 5 of each, `sub_8221D730` → `sub_82103C88`,
both just routers: every behaviour that listens gets the message) → the
physics' message handler (slot 5, `sub_8217DCF8`). The physics' whole interface
is that handler: message 1 (impulses), 3 (questions: "would this move hit?",
gravity value), 5, 9 (contact reports from the collision), 0x11, 0x19 (the
commands above, 17 of them), 0x27 (set position), 0x37, 0x3D.

## 2. One physics frame (slot 4, `sub_8217D0A0`)

In order, with the frame time `dt` (stored at +0xA4 for the message handlers):

1. **Integrate** (`sub_821805F8`): gravity `vy += g x dt`, clamped to a fall
   speed limit (`sub_82182A00`; Crash -60 units/s²); air drag in **fixed
   1/60 s steps** with a leftover accumulator (`sub_82180748`, the drag
   factors at +0x9C / +0xA0); then the move `v x dt` is ADDED to a pending
   move at +0x8C (not applied yet), and |v| goes into a variable.
2. **Steering** (`sub_8217BE48`, the physics' turning helper at +0x1C): turns
   toward a target direction over time, timers counted down by `dt`.
3. The pending move is sent to the character as a message; the collision
   sweeps the ellipsoid and reports each touch back (message 9 →
   `sub_821809C8`): the touch is sorted by its normal into ground / steep /
   wall, the contact set (`SetContact` `sub_82182400`), and the velocity into
   the surface taken away (the slide of findings/22 s.8).
4. **Reach checks** for the ground and steep contacts (`sub_8217D008`): the
   same triangle must still be within 0.25 x the ellipsoid's height (0.2
   units for Crash), else the contact is dropped.
5. **"Standing still"** (`sub_8217D938`): when the measured speed is at most
   0.1 units/s (squared speed ≤ 0.01) and nothing holds the body, a timer at
   +0xD8 grows by `dt`; "still" = more than 1 s.
6. **"Floating, stuck"** (`sub_8217D5A0`): gravity on and pointing down, NOT on
   the ground, and either still (step 5) or the measured speed below **0.06
   units/s**. While that holds, a timer at +0xDC grows by `dt`; past **5 s** the
   game moves the body back to safety (the "floating too long" safety net),
   except for a player's character in some states. Any frame where it doesn't
   hold sets the timer back to 0.

The **measured speed** used in 5 and 6 (two variables, a vector and its
length) is written by the transform handler (command 4, `sub_8217E850`):
the distance the body really moved since the last frame, divided by `dt`. A
titan standing on flat ground at 180 fps reads exactly 0; walking, about 10.

## 3. What is safe and what isn't

**Same at any frame rate** (written in seconds):

* gravity and the fall speed limit (an update per `dt`; the arc itself
  shifts by about half a frame's worth of rise, less than 0.1 units on a
  normal jump at 30 fps, smaller at higher rates);
* air drag (fixed 1/60 s steps; slightly uneven timing below 60 fps only);
* both timers (+0xD8 one second, +0xDC five seconds);
* the measured speed (distance / `dt`);
* the steering helper's timers;
* the ground / steep / wall sorting (by the contact normal only);
* elsewhere: a fight-tree function that sub-steps in 1/60 s pieces
  (`sub_8212BDC0`), animation frames turned into seconds with 1/30
  (`sub_8223DDC8`, `sub_822414E8`: time-based, safe).

**Depends on the frame rate:**

| What | Where | At 30 fps | At 180 fps | Effect |
|---|---|---|---|---|
| Gravity's pull back onto the ground after a slide zeroed `vy` | integrate + sweep | g x dt² = 0.067 units per frame | 0.0019 | the body drifts off a downward slope before gravity catches up: contact lost, fall branch (findings/22 s.8, measured) |
| Move-by minimum (step² > speed² x 1.6667e-05) | `sub_82159618`, `sub_8217E6B0` | passes | drops slow moves | **fixed** (findings/22 s.9, `--platform_move_fix`) |
| Transform minimum (moved² > 1e-05) | `sub_8217E850` | passes | moving things hop every other frame | **fixed** (same) |
| Add-velocity minimum (\|push\|² > 0.001 = 0.032 units/s) | `sub_8217EAC0` | — | — | harmless if the sender sends a whole push; drops pushes that a sender spreads over frames as `accel x dt` (sender unknown: to check) |
| "Stuck" speed limit 0.06 units/s on a body that keeps getting pushed out of a wall | `sub_8217D5A0` | 0.002 units of jitter per frame read as "still" | 0.0003 | the same per-frame push-out reads 6x faster at 180: a titan wedged on an edge may never count as stuck, so the 5 s safety net never starts (hypothesis, s.4) |
| The ground grace (port, findings/22 s.5) | `ground_physics.cpp` | — | 40 ms = 7 frames | every non-physics caller hears "on the ground" (findings/22 s.7, playtest list) |

Outside the physics, one camera function (`sub_8210BA90`) picks a smoothing
factor from the frame time (1.0 at 1/60 s or less, 0.5 at 1/30 s, down to
0.4 at 0.1 s), on the first update after a flag is set. Camera only; noted
for later.

## 4. The playtest list at 180 fps, against this map

| Seen at 180 fps | Best lead from the code | Status |
|---|---|---|
| Climbs and jumps sometimes "zip" across | placed moves (set position): the sender's numbers, not the physics | needs a save at a climb + a 30 vs 180 replay |
| A ground attack in mid-air after letting go of a climb | the ground grace answering the climb's exit (40 ms = 7 frames) | narrow the grace; needs the same save |
| Crash reacts to small visual bumps | the slope skip (gravity's pull per frame, s.3 row 1) | measured on a slope; a fix must keep slopes from becoming slides (the first stick-to-ground try slid on steep ground) |
| Titans stuck floating at an edge, no respawn | the stuck check's 0.06 units/s against per-frame jitter | hypothesis: read the titan's measured speed at such an edge |
| Clipping into balancing platforms (Ratcicle Kingdom) | per-frame minimums on moving things (fixed for path followers; a balancing platform may move another way) | needs a save there |

## 5. How to look further

* `notes/scratch-tools/physics/` (private): the pseudo-C dumps, `constscan.py`
  (every instruction that loads a chosen constant, by function: found the
  1/60 and 1/30 users above), `beh.py` (an actor's behaviours by class name,
  through the console: a titan's physics isn't at actor +308 like Crash's),
  `run.sh` (one muted test game on workspace 1 with a console).
* A titan's physics: walk the actor's behaviour vectors from +108 and pick
  the one with vtable `0x82032FC4`. Its variables: index bytes at +0x23
  (measured velocity), +0x24 (measured speed), +0x26 (held), +0x27 (ground
  contact); storage = `*(*(actor + 32) + 4 x index)`.
* For each item in s.4: a save right before the spot, played once (the input
  recording is on by default), then replayed at exactly 30 and 180 fps
  (findings/29 s.6) with `track`, and `who` on the fields named here.
