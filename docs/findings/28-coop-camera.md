# 28. The co-op camera: framing every player

Status: experimental, **needs extensive testing** (on by default;
`--coop_camera=false` = the original). The group focus for three or four
players (section 2, item 4) is the newest part and has only been tested
with fake controllers.

With two players, the original camera follows one of them and only backs off
a little when the other walks away; the other player easily ends up off
screen. With players 3 and 4 ([findings/26](26-more-local-players.md)) it
didn't count them at all. The port's co-op camera keeps the game's own camera
(each area's angle, its smoothing, its collision) and changes two of its
inputs: **where it looks** (the centre of all players in game) and **how far
back it sits** (far enough for everyone to be in the picture).

![Original co-op camera vs the reworked one](../images/coop-camera-before-after.jpg)

*Same save, same scripted walk (Wumpa Island, two players running apart).
Left: the original camera stays on player 1; player 2 is already off screen.
Right: the reworked camera centres between them and backs off until both
fit.*

Code: `src/players/coop_camera.*`, one mid-function hook in the manifest
(`CoopCameraDistance`). `--coop_camera=false` gives back the original camera;
`--coop_camera_reach` (default 1.3) caps how far it backs off relative to
the area's own farthest camera distance, and `--coop_camera_max_distance`
(default 30, was 45) is the absolute cap (section 11);
`--coop_camera_group_focus` (default 2, 0 = everyone counts the same) sets
how much a group of players outweighs a player on their own;
`--coop_camera_pinned_rooms` (on) keeps the camera at the original's spot
in rooms where the original never moves it and widens the view instead
(section 10); `--coop_camera_walls` (on) stops a back-off at the scenery
behind the camera; `--debug_coop_camera_trace` logs the camera, the players
and the volume's numbers every 10th frame.

## 1. How the game's camera works

Gameplay views come from a **camera volume** behaviour (`CCameraVolumeBehaviour`,
vtable `0x8202998C`), run by a `CCameraBehaviourController` (modes VOLUME,
SCRIPTCAMERA, MAYA, NIS, STATIC: cutscenes and scripted shots are the other
modes).

* **Volumes.** Each level's `cameradata.p3d` places camera volumes (424-byte
  records in the camera manager, `*(*(0x8259B190)+44)+8`); each gives an angle,
  a field of view (+396) and a **zoom range** (+388 min, +392 max).
  `levels/<L>/cameraoverrides.lua` (plain Lua bytecode in `default.rcf`) sets
  them per volume with the manager's script methods `MinZoomValue`,
  `MaxZoomValue`, `SetCameraVolumeFOV`, `DisableLeadCamera`,
  `AddTransitionOverride` (registered at `0x8223AA9C`). Wumpa Island near
  Crash's house: 12 / 20.
* **Interests.** What the camera should show are *interests*:
  `CCameraInterestBehaviour` (vtable `0x82029864`) on an actor, with a priority
  (PRIMARY / SECONDARY), a weight and a size (CRASH, STRONG, BEAR, CAPTAIN,
  BOSS, PROJECTILE). Fields: +28 the actor, +32 a fixed position used instead
  of the actor's when flag `0x10` of +56 is set (scripts), +44 priority, +48
  weight. They reach the volume behaviour as messages through the camera
  manager (subtype 0 add, 1 priority change, 2 remove, 16 weight; handler
  `sub_82108548`). The volume behaviour keeps two lists of 36-byte records,
  PRIMARY at +196 and SECONDARY at +212 (+4 = the interest).
* **The focus.** A third list at +492, with room for **two**, holds the
  players' interests; its first entry is the **focus** (`sub_82109848`), and
  `sub_821099C0` writes its player number to the game object's +40, which the
  level scripts read as `EPlayer_CAMERA_FOCUSING_ON`.

Per frame:

| step | function | what |
|---|---|---|
| target | `sub_82107290` | +228 = the focus's position, pulled toward SECONDARY interests (enemies, a boss) by weight |
| volume | `sub_82107628` | +264 = the volume containing the target |
| spread | `sub_82107000` | +484 = number of PRIMARY interests, +488 = how far the farthest one is from the focus |
| zoom | `sub_821076B0` | min zoom, or with 2+ primaries min + (max - min) x f(spread); handed to a smoother at +296 |

Every interest position in these steps comes from one function,
`sub_82105538` (record -> pointer to its position: the interest's fixed
position, else its actor's world matrix position, blended in height while a
record is new; its callers are all in the camera code).

**Jumps.** A record also holds the camera's height through a jump
(`sub_82105758`, called per record per frame by `sub_82105628` with the
interest's flags): at take-off (flag `0x80` of +56, "in the air") the height
is held at +16 and the blend +8 set to 0; while held, `sub_82105538` answers
that height (x and z still follow); after landing the blend runs back to 1
and the height eases to the new ground (squared ease); landing LOWER than
the held height follows at once (dropping off a ledge). Flag `0x40` snaps
the blend to 1.

**Measured in one-player play** (every frame, Wumpa Island): walking, the
camera follows Crash almost rigidly (offset camera - Crash stays near
(-3.5, 5, 10.4), within a few tenths); jumping, its height doesn't move at
all (Crash 3.3 units up, camera y fixed); after a double jump onto ground 2
units higher it eased up in about 0.25 s; walking down a slope it followed
down at once.

**+296 is the camera's distance from its target**, measured: at rest the
camera sat 12.0 units from Crash, and it went to 20 as the players
separated. A two-player trace (same save) showed the result: the camera moved
only with player 1 (the focus), the zoom stopped at 20, and at 57 units apart
player 2 was far outside the picture. Both players were PRIMARY interests;
players 3-4's interests never made it into the two-entry focus list.

The Pure3D camera that ends up drawing (`VectorCamera`,
[findings/27](27-cheat-menu.md) section 5) has its **horizontal field of
view at +16** (1.113 rad = 64 degrees here) and its **aspect ratio at +28**
(16:9). Checked against a capture: Crash's feet project where they appear on
screen.

## 2. The reworked camera

With **two or more players in game** (state IN_GAME; masks don't count):

1. **Target = the centre of the players.** `sub_82105538` answers the centre
   (a 12-byte guest buffer of ours) for any interest whose actor is a
   player's Crash or jacked titan. The original still runs first for that
   record, so each player keeps the **jump hold** above: its answer is that
   player's *camera point*, and the centre is made of camera points (a
   player's jump doesn't move the camera; a landing higher eases it). Record
   creation (`sub_82105420`, call at `0x82105450`) gets the original answer.
   An interest with a fixed position (flag `0x10`) keeps it. As a result the
   volume is picked by the centre, the focus can't swap, the spread is 0 and
   the game's zoom stays at its minimum: the same view a single player gets,
   centred on the group. SECONDARY interests still pull the target as before.
   A player joining or leaving fades in or out of the centre over 0.6 s.
   **Smoothing:** the centre follows the camera points with a critically
   damped spring (no overshoot), smoothing time 0.22 s, down to 0.06 s as
   **urgency** rises. Urgency (0..1) comes from the picture just drawn: 0
   while every player's feet and head are inside 85% of the half-width /
   half-height, 1 at the edge or beyond. Two players running apart reached
   0.35 and nobody left the picture.
2. **Distance = enough to see everyone.** A mid-function hook at
   `0x82107864` (where both paths of the zoom function meet, `f1` = the
   distance the game wants, `r31` = the volume behaviour) raises `f1` to the
   smallest distance at which every player's feet and head (2 units up, 4.5
   for a titan) are inside the picture: 78% of the half-width either side,
   62% of the half-height up (the HUD sits at the top), 75% down (players
   3-4's HUDs sit in the bottom corners). The camera keeps its angle: the
   test moves it along the line from the centre to where it was last frame,
   using the camera's field of view and aspect ratio; a bisection finds the
   distance. Never closer than the game's own, never farther than
   `--coop_camera_max_distance` (45).
3. **Backing off.** The game's smoother is slow (12 to 45 units took about
   4 s, and two players running apart left one of them off screen), so when
   more room is needed the hook also moves the current distance (+296)
   toward the new one: 1.5x per second when calm, up to 10x per second
   (about 95% in 0.3 s) when urgent. (A first version always used 8x and
   skipped the jump hold: the camera bobbed and zoomed with every jump.) The
   fit uses the camera points, so jumps don't change the zoom either. Coming
   back in stays the game's own smoothing.

4. **Groups (three or four players).** The centre leans toward where MOST
   players are. Each player's *group size* is 1 plus, for every other player
   in game, how near they are: 1 within 7 units, fading to 0 at 15 (a soft
   edge, so no weight jumps when someone crosses a line). In the centre a
   player counts group size to the power `--coop_camera_group_focus` (2):
   two players together and one apart = 4 : 4 : 1, so the camera sits with
   the pair and the one apart gets a ninth of the say; two pairs = 4 each,
   balanced. With two players both always have the same group size, so
   two-player co-op is unchanged. The distance still fits EVERYONE: the
   player apart stays in the picture while the maximum distance allows it,
   and past it is the first to leave the screen (where the game's own
   catch-up applies: section 4).

With **one player in game** nothing changes: the trace before a join matched
the original camera to the hundredth of a unit. Cutscenes and scripted
cameras don't use the volume behaviour and are untouched.

## 3. Tested

On Wumpa Island with fake controllers (`--debug_fake_pads`):

* **Two players** running apart (scripted, 33-57 units): both stay in the
  picture up to the cap; the camera follows the centre through three volume
  changes; walking back together it closes in again.
* **Four players** (`--local_players=4`) running in four directions: all four
  in the picture (one far back, one in front, two at the sides), clear of
  the corner HUDs; all four interests count (the volume reported 4
  primaries). Turning back into masks one by one, the camera returned to the
  original single-player view on player 1.
* **Two players walking and jumping together:** the camera's height stays
  through the jumps; walking, it trails the pair by under 2 units and
  catches up when they stop.
* **Three players, two walking off together** (players 2-3 about 25 units
  from player 1): group sizes 2 / 2 / 1, centre 3 units from the pair
  instead of 8 (the plain middle); at the 45-unit cap player 1 is still in
  the picture, at its edge.
* No errors in the logs.

Played on two levels: experimental, working well so far (two players far
apart on Wumpa Island both stay in the picture). Not tested yet: fights (enemies as SECONDARY interests), titans (their size),
bosses, tight spaces (the wider view may show places the level designers
never meant to be seen), cutscenes started with players apart, and how it
feels in play.

## 4. The game's own catch-up: B while off screen

The original has a way back for a player who got lost: every frame the
co-op behaviour's update (`sub_82234FB0`) asks whether that player's Crash
is **off screen** (`sub_82235978`: the camera's visibility test, vtable slot
68 of `*(game + 76) + 444`, on the actor's position) and whether the player
just **released B** (`sub_82272338(controller, 5, 42, 0)`: input map PLAYER,
event `SPECIAL` = `AddInputUp(CIRCLE)` in `inputmap_methods.lua`). Both =
sub-state 5, "forced into a mask": the player becomes a mask on a partner.
On screen, B is the ordinary "turn into a mask", which instead requires the
partner to be ON screen (`sub_82235CB8`). So the same button either hops
on a nearby partner or pulls a far-away player back.

`sub_82235978` only answered "off screen" while players 1 AND 2 were both
in game (states 2 or 3, read from `0x8259B11C`): with player 1 and players
3-4 but no player 2, nobody counted as off screen and the catch-up never
fired. Both of its checks meet at `0x82235A20` with the answer in r11; a
mid-function hook there (`MorePlayersOffScreenGate`) now asks for any two
players in game. Tested with players 1 and 3 only (fake controller, maximum
distance 14, `--coop_mask_from_anywhere=false` so the ordinary B can't do
it): player 3 ran about 35 units away, the ordinary "turn into a mask" was
refused (off screen), and B then took the catch-up: player 3 became a mask
on player 1. The camera's fit (section 2) keeps everyone in view up to the
maximum distance, so the catch-up only matters past it (and in pinned rooms,
section 10).


## 5. Second round: cutscenes, shimmer, room around players

Three problems came up in play, all seen on Wumpa Island at the NV mail
(two players, the mail's in-engine cutscene and the cartoon movie after it,
both skipped).

**Zoomed far out after a cutscene.** Cutscenes and scripted shots rebuild
the same `VectorCamera` (`sub_82373D18`) that gameplay uses, so the co-op
code kept reading the CUTSCENE'S view while one played: both players were
"off screen" from its angle (urgency 1). At the first gameplay frame the fit,
still working from that view, asked for the 45-unit maximum and moved the
current distance (+296) most of the way at once; the game's own smoother
then took several seconds to come back in from about 35 units.

![Right after skipping the NV-mail cutscene: zoomed far out before the fix, the
normal distance after it](../images/coop-camera-after-cutscene.jpg)

Fix: a view counts as gameplay only when the volume behaviour's zoom
(`sub_821076B0`) ran since the previous rebuild. Traced: in gameplay that is
every frame (one rebuild per frame, never two), during cutscenes and movies
never. While no volume view arrives the co-op camera is off. When play
resumes it takes over again starting from the target the game's camera was
looking at (+228, the focus player), gliding to the centre of the players.
For the first 15 frames it doesn't push the distance out itself unless a
player is about to leave the picture. The first fit after the skip asked
for 14 units instead of 12 for a moment. After the fix: 14 at the first
frame, 12 within 0.6 s, with no visible zoom.

**A shimmer while standing still.** With both players idle the trace showed
the distance cycling 12.1 → 12.5 → 12.1 units about every 5 frames, with
the camera moving a tenth of a unit back and forth. The fit read where the
camera was on the previous frame to know its angle, and the camera moved
with each fit, so every frame chased the last one. Fix:

* the camera's offset from the centre is taken in the camera's own frame
  (right, up, forward) and averaged over about 0.4 s;
* the distance we ask for goes out at once when more room is needed, but
  only comes back in once the fit is at least 0.6 units closer, then eases in
  the whole way;
* urgency rises at once and falls back over 0.4 s, so the centre's smoothing
  speed doesn't flip with every step near the edge.

The distance's direction changes in the same test (with both players idle,
then after the cutscene): 63 in 2,131 frames before, 4 in 1,793 after.

**Room around each player.** Feet and head on screen wasn't enough. A player
at the edge of the picture couldn't see the ground around them (a ledge, a
drop). Besides feet and head, the fit now keeps points
`--coop_camera_room` units (default 4, about two Crash heights) to each
player's left and right and beyond them on screen. Not the point between a
player and the camera: the game's own view keeps Crash in the lower half of
the picture, so that point sits below him on screen and backed the camera
off to 18-22 units even with both players together (tried, removed). With
the room, two players standing together still get the game's own 12-unit
view (12.3 side by side). Walking apart on the cliff by the house, the player
on the cliff edge keeps more ground and water around them than without the
room (`--coop_camera_room=0` gives the old fit).


## 6. A partner frozen far away: the level's active zones

**The symptom.** In Ratcicle Kingdom's courtyard, player 2 walked up the
snowy path while player 1 stood still. At one spot player 2 stopped dead:
no input worked (back, sideways, jump), not even gravity. Player 1 walking
near brought them back. With 3-4 players the same applies to everyone but
the camera's focus player.

**Not the camera's fit, not the off-screen rule.** The stop point was the
same spot (319.1, 38.5, -199.5) every time: with the original camera
(`--coop_camera=false`), with the off-screen answer of section 4 forced to
"on screen", and with player 1 standing 13 units closer. A per-actor count
of the physics update (`CPhysicsBehaviour` slot 4, `sub_8217D0A0`) showed
the reason: player 2's update stopped being called at that moment, as did a
few other characters' nearby.

**How the level decides who runs.** A guest stack walk from that update
led to the world manager's frame (`sub_822E6EC0`): the level is split into
**zones** (its list at +20), and each zone's characters are updated
(`sub_822F0100`) only while `sub_822EFFD8` calls the zone **active**:

- the zone is loaded (+596 == 2), and
- either the **camera** is near it (`sub_822F4AA0`: the camera's position,
  `*(*(uber + 76) + 444) + 216`, against the zone's bounding sphere at
  +488 / radius² +500, then its portals at +376),
- or it is the zone of the player the camera **focuses** on
  (`sub_822E7A70`: game +40 = that player, their character's +40 = its zone).

A character belongs to the zone its position falls in (the same frame's
loop over moving actors at +144 re-files them: `sub_822E6AA0`, actor +40).
Only one player is the focus. With the original camera a partner that far
away is already off screen (and B brings them back as a mask, section 4).
With ours, which backs off to keep everyone in the picture, the frozen
partner stays in view.

**The fix.** A mid-function hook at `0x822F003C`, where both of
`sub_822EFFD8`'s answers meet in r11 (r31 = the zone): a loaded zone that
holds any player in game (their Crash, or the titan they ride) is active
too (`CoopCameraZoneActive` in `src/players/coop_camera.cpp`). The same walk
now goes on up the path (to 332, -206), back down, sideways and jumps.
Characters in the partner's zone (enemies too) now run as well, as they
would for the focus player.


## 7. Front to back: the camera stays with the nearest player

![Player 2 walks up a path away from the camera while player 1 stands still. Top: the camera aims at the middle of the players, follows player 2 up the path and loses player 1 off the bottom (then both). Bottom: the camera stays with player 1 and backs off; player 2 stays in view, smaller, near the top](../images/coop-camera-near-focus.jpg)

**The problem.** The centre of section 2 was the middle of the players in
all three directions. When one player walked deep into the picture (up a
path, away from the camera), the camera's aim followed them halfway, and
the player near the camera slipped off the bottom of the picture, although
the far player was in plain view all along.

**Why front to back is different.** The game's cameras look down only about
10 degrees. A far player on the ground appears near the middle of the
picture, just smaller, without the camera moving toward them. Left / right
and height are what push a player out of the picture.

**The change.** Along the camera's forward direction (kept level) the centre
now sits with the player **nearest** the camera; left / right and height
still use the middle (`--coop_camera_near_focus`, on by default; off = the middle as
before; the launcher's "Stay with the nearest player").
The distance fit is unchanged, so the camera still backs off when the far
player would leave the top or the sides. The nearest player's distance
along that direction is a minimum over the players, which changes
continuously, so two players swapping who is nearer doesn't make the
camera jump.

Same walk as in the picture (fake controllers, player 2 climbing the path
from the Ratcicle Kingdom courtyard): with the middle, the camera ended up
looking at a wall with nobody in view; with the nearest player, player 1
stayed on screen throughout and player 2 stayed in view up the path.

## 8. Back from a cutscene with the partners far away

**The symptom.** Wumpa Island, three players: players 2 and 3 stay at the
save totem, player 1 walks alone to N. Gin's lab and starts its cutscene.
When play resumed, the camera came back inside a rock face, flew out to the
45-unit maximum and then crept back in over about 3 seconds.

**Why.** Section 5's take-over still glided: it started from the game's own
target (player 1) and only measured the fit on the live view. After a cut
the game places its camera around the FOCUS player (the cut ends a frame
before ours turns on) and then spends about 0.6 s gliding between camera
volumes (89 → 91 here) toward the players' centre. Fits measured on that
moving view put the players at the edge of the picture: the 45-unit
maximum, written into the current distance at once (urgency 1), followed by
the game's slow smoother coming back in to 19.5.

**The change.** After a non-volume view (cutscene, scripted shot) the next
take-over SNAPS for 1 s: the centre jumps straight to the players, and the
distance is the fit itself (no settle, no gentle back-off, no deadband).
The fit is measured on the last STEADY view from before the cutscene
(saved every settled frame with its averaged offset), not on the gliding
one. That is used for the whole second when the camera is back in the same
volume, and otherwise only until the live view's direction stops changing.
A join still glides as before.

| After the NV lab cutscene (trace every 3 frames) | camera distance |
|---|---|
| before | 45 for 0.6 s, then 39 → 19.5 over ~3 s |
| live fit only, no glide | 19 → 24 → 17.4 over ~1 s |
| steady view (the change) | 20 → 17 during the glide, then 17 → 19.4 gently |

In the 250 ms captures the picture shows the final framing about 0.5 s
after the fade; the last couple of units are the normal gentle zoom out.


## 9. Moving between levels

**The symptom.** Two players walk out of Crash's house onto Wumpa Island,
standing together at the door: the camera starts far out (35 units, the
game's own distance is 12) and comes back in over about 4 seconds.

**Why.** The level's own loading screen keeps the co-op camera on (the
players are in game the whole time), and its smoothed centre was still in
the house, 360 units from where the players now stand. At the first frames
of the new level the centre glided across the map; seen from that moving
view the players were at the edge of the picture (urgency 1), so the fit
pushed the distance to 35 at once, and the game's smoother took seconds to
come back.

**The change.** The players' middle moving more than 20 units in one camera
frame (a level change, a teleport) is a cut: the centre jumps to the
players, and the distance stays the game's own while the game's camera
glides into the new level. When its view is near the players, in a known
volume and has stopped turning for 3 frames, the fit snaps in on that view
(the 1 s snap of section 8). Under the loading screen the game's camera
stands still back in the old level; a first try took that for "arrived"
and fitted 45 units.

| Out of Crash's house (trace every 3 frames) | camera distance |
|---|---|
| before | 15 → 24 → 36 at the door, then down to 12 over ~4 s |
| after | 14 → 13.2 → 12.8 (the game's own) → 12.0, steady |

## 10. Rooms: where the original camera never moves

![Two players at opposite walls of Crash's house. Left: the camera backed off out of the house, the floor's edge and the sky beside it. Right: the camera stays at the game's own spot and the view widens](../images/coop-camera-pinned-room.jpg)

**The symptom.** With two players at opposite walls of Crash's house, the
camera backs off until it is outside the house; the floor's edge and the
sky around the house show (from about 8 units, the game's own being 6).

**How the original handles it.** Each camera volume has a designed zoom
range (section 1). The house has a single volume with **min = max = 6**: the
original never moves the camera there at all. Wumpa Island's 161 volumes are
mostly 14-20 (90) or 12-20 (43); twelve are pinned too (12/12, 14/14,
20/20).

**Walls don't help here.** A first attempt cast the game's scenery ray
(`sub_8224C7F8`, the one behind the Wii cursor's mojo collector,
[findings/47](47-mouse-reticle-and-wii-cursor.md): r3 `*(uber + 48)`, r4 start,
r5 end, r6 a query with flag `0x80` = scenery only at +0x1C, r7 the result:
+0 the hit's distance from the start, +12 nonzero = hit) along the camera's
line. From the players it hit the house's table, which stopped every
back-off. From the game's own camera spot outward it hit nothing: the house
is built like a stage set, with no wall on the camera's side. The ray stays
(`--coop_camera_walls`, on): from the game's camera spot outward, a hit
stops the back-off 0.6 units before it, and pulls the camera in at once
when it is already past it. Outdoors on Wumpa Island it never hit.

**The change.** In a pinned volume (min = max) the camera stays at the
game's distance, and when that doesn't show everyone the **field of view**
widens instead (the VectorCamera's +16, which the game sets again every
frame; written right before the rebuild), up to 1.4x on the tangent (64 to
about 82 degrees at 16:9), eased in (4/s) and out (1.5/s), never during
cutscenes. A first version also let the camera back off 15% (6.9 units):
the sky still showed in one corner. `--coop_camera_pinned_rooms=false`
gives the old behaviour. Past 1.4x a player at the far wall can still be at
the edge or just outside the picture; the catch-up of section 4 brings
them back.

Tested in Crash's house with a fake second player in five poses (both
players at opposite walls, corners): no outside visible in any. On Wumpa
Island, two players running apart: unchanged (back-off to 45, no wall hit,
no field-of-view change).

## 11. Reach and bump jitter (round 4)

**Too far.** Backing off to 45 units made the players specks; the original
never goes past its volume's designed farthest distance (20 on most of Wumpa
Island). The farthest is now **the current volume's designed maximum x
`--coop_camera_reach`** (1.3: Wumpa Island 20 → 26), never past
`--coop_camera_max_distance` (now 30). The room each player needs around
them (`--coop_camera_room`) went from 4 to 2 units. A player beyond that
reach leaves the screen, where the game's own catch-up (section 4) applies.
The wall ray only looks as far as that reach too.

**Jitter when bumping into things.** A bump pushes a player's position back
and forth for a few frames, and three parts reacted at once:

| Part | Before | Now |
|---|---|---|
| player points the fit / urgency measure | raw position | eased over 0.08 s |
| urgency (spring 0.22 → 0.06 s, back-off 1.5 → 10/s) | rose at once | rises over 0.12 s; at most 0.25 while nobody-fits-even-at-the-farthest |
| wall test | pulled in at once, let out at once; ray stopped once the wall had pulled the camera in, so it went out and in again | closer wall at once, release 3 units/s; ray keeps running while a wall is held |

The debug trace prints the wall's held and raw values and "(at the
farthest)" when the reach cap is what stops the back-off.

## 12. The original camera, read in full (research for the second rework)

Sections 1-11 changed two inputs of the game's camera (where it looks, how
far back it sits) without knowing how a camera volume turns a target into a
view. Two problems in play showed that this matters: the camera leaving the
level ("out of bounds"), and scripted camera moves that stop working with
two players, the clearest being the final fight against Dr. Neo Cortex. This
section reads the whole volume camera (Ghidra pseudo-C of the functions
named below, the level scripts and `levels/L<n>/cameradata.p3d`).

### 12.1 A camera volume is three boxes, not an angle

`cameradata.p3d` (chunks `0x05432104` file → `0x05432106` **group** with a
name → `0x05432103` **volume** with a number → three `0x0543210x` shapes)
gives each volume three eight-cornered shapes, named after the volume's
number:

| shape | chunk | in the 424-byte record | meaning |
|---|---|---|---|
| `I_<n>` | `0x05432100` | +4 (8 corners) | where the **target** may be: the volume "contains" the target when it is inside all six faces (`sub_822E8710`) |
| `C_<n>` | `0x05432101` | +0x68 | where the **camera** goes |
| `L_<n>` | `0x05432102` | +0xD0 | where the camera **looks** |

Per frame (`sub_82108780`, called from the volume behaviour's update
`sub_82106C38` slot 4):

1. The target's position inside `I` becomes eight weights, one per corner
   (`sub_822E8A70`: mean-value coordinates over the shape's triangles, so it
   works for any eight-cornered shape and changes smoothly).
2. The same weights applied to the corners of `C` and `L` (`sub_822E9210`)
   give a **camera point** and a **look point**. Near the edge of a volume
   whose neighbour is listed in the record (+0x195 / +0x196), the camera point
   is blended with the neighbour's (`sub_82109228`, `sub_821094F0`).
3. The camera then sits on the line from the look point through the camera
   point, at the distance that puts it **zoom** units (+0x128 = 296) from the
   target, or farther if the target would otherwise be outside the field of
   view (`d / tan(fov)`, d = the target's distance from that line).

So the boxes decide the **direction** of the view and where it looks; the
zoom only slides the camera along that line. A target moving up through a
volume whose `C` box is taller than its `I` box makes the camera rise faster
than the target; moving sideways through a ring of volumes swings the camera
around. This is how the designers built camera moves without scripted
shots.

Other parts of the record: +0x184 minimum zoom (default 14), +0x188 maximum
zoom (default 20), +0x18C field of view (`MinZoomValue`, `MaxZoomValue`,
`SetCameraVolumeFOV` in `cameraoverrides.lua` address volumes by their
number, the record's +400 bits 18-27), +400 bits 28-31 the **group**, +0x194
flags (bit 7 combat data used, bit 6 lead camera off: `DisableLeadCamera`),
+0x198 a list of up to six neighbour volumes (searched first when the target
leaves the current one, `sub_82107490`), +0x13C per interest size two
"combat" values (`SetCombatCameraData`, smoothed in `sub_821079B8`).

**Lead camera.** When the target moves faster than 4.5 units/s, the look
point is taken from a point ahead of the target, mapped through the volume
the same way (`sub_82108780`, smoothed vector at +0x1A4), and half of that
lead is added.

**A second camera type.** A record whose word at +100 is not zero carries a
curve object: the camera then runs on a **rail** (`sub_82108E48`: the
target's nearest point on the curve, followed with a speed limit, the camera
and look points read from curves). The `cameradata.p3d` files hold only the
three shapes per volume; what sets +100 (and whether any level uses a rail)
is not known yet (name TBD).

### 12.2 Groups: the scripts switch the designed cameras

Volumes belong to named groups; a group can be switched on and off by the
level scripts (`DO_EnableCameraGroup` / `DO_DisableCameraGroup`,
`CActionEnableCameraGroup`; the containment test `sub_821075D8` skips volumes
of a disabled group). Seven of the ten levels do it (`levels/L1`, `L2`,
`L3`, `L4`, `L5`, `L8`, `L9` hold groups besides the default "cameras"):
the camera of a place changes with the story, most often at arenas.

### 12.3 What the target is in the original

`sub_82107290`: the focus player's interest position, pulled toward every
SECONDARY interest (enemies, a boss) closer than 20 units, by weight. The
volume is then picked by THAT point (`sub_82107628`): the current volume if
it still contains it, else its neighbours, else every volume in order.

**When no volume contains the target** (+264 = -1), `sub_82106C38` skips the
box mapping entirely: the camera and look points just move by however much
the target moved. Nothing then keeps the camera where the designers allowed
it, and the zoom no longer acts (it is only used inside the mapping). With
one player this almost never happens: the `I` shapes cover where a player
can stand.

### 12.4 The final fight (Cortex, `levels/L8` room 5)

The fight (described in player guides and read in `levels/L8/objectives.lua`):
Cortex is fought in a round arena in stages; the last stage has the player
jacking Cortex and using his special attack on a **spinner** in the middle
of the arena (`L8_R5_Elevator1`, connected to its own path:
`DO_ConnectBehaviours ... SPINNER ... PATH_POSITION`); spinning it raises the
platform (`WAIT_Spinner_Spun`), an iris door opens, the self-destruct button
appears (`CAM_l8_r5_selfdestruct_establish`) and pressing it ends the fight.
Every trigger in this room waits for `EPlayer_CAMERA_FOCUSING_ON`.

The room's camera is three groups, switched per stage:

| group | when | volumes | design |
|---|---|---|---|
| `R5_Cam1` | arrival, stage 1 | 20 | a **ring** of wedge-shaped `I` shapes around the arena's middle; each `C` sits outside its wedge, so the camera circles the arena behind the player, looking in |
| `R5_Cam2` | after stage 1 | 6 | **stacked layers**: `I` shapes covering the whole arena, each 10-14 units high, from the floor (y -4) to y 60; each layer's `C` and `L` sit higher, the upper layers' cameras above the target looking down |
| `R5_Cam3` | after stage 3 (the spinner) | 3 | floor layer, one tall layer (y 10-50) and top layer: the platform's ride |

In `R5_Cam3` the camera rises with whatever the target is: on the platform
it follows it up through the tall layer. Minimum zoom there is 16 (maximum:
the default 20); the ring has 12-16 or 12-18.

### 12.5 Why the co-op camera breaks it

**The spinner ride.** The port's target is the players' centre. With one
player riding the platform and another on the floor, the centre is halfway
up: the volume picked is a middle layer, so the camera stops at half height,
and the distance fit then backs it off along that layer's line (up and
behind). With the original, the target stays on the focus player: the camera
rides up with them and the player left on the floor goes off screen (where
B brings them back as a mask, section 4).

**The ring.** Two players on opposite sides of the arena put the centre at
the arena's middle, which no wedge's `I` shape contains (judged from their
corners, the wedges leave a hole about 4 units across there). The camera then has **no volume** (section
12.3): it drifts with the centre, the zoom fit stops having any effect, and
nothing holds it inside the room.

**Out of bounds in general**, from the code above:

1. **No volume:** whenever the players' centre falls outside every `I`
   shape (a gap between rooms, a drop between two ledges, over a wall, the
   middle of a ring), the camera floats free. On Wumpa Island this did not
   happen in a test with two players 85 units apart (its 161 volumes cover
   the ground everywhere); arenas and rooms built from wedges and layers are
   where it does.
2. **Past the designed zoom:** the fit raises the zoom up to the volume's
   maximum x 1.3 (section 11). The line from the look point through the
   camera point keeps going past the `C` shape, into or behind scenery the
   designers never meant to be seen. The wall ray (section 10) only catches
   scenery that has collision.
3. **Wider field of view** in pinned rooms (section 10): more of the room's
   edges, by design limited to 1.4x.
4. **Near focus** (section 7) moves the target off the middle of the
   players, toward a point that may be in a different volume than either
   player.

## 13. The second rework: the plan

The first rework treated the volume camera as "an angle and a distance".
It is a designed mapping, so the second one starts from the volumes:

1. **A leader for the camera only.** Story triggers no longer depend on it:
   any player in game starts them ([findings/26](26-more-local-players.md)
   section 69). The camera still needs one player to follow when the
   players can't share a view; which one (the game's focus, the player who
   last started something, a player inside an enabled volume) is decided
   when the rework is built. A camera group switched by a partner can
   leave the leader outside every enabled volume: then the leader moves to
   a player who is inside one.
2. **Partners framed only where the design allows it.** Each player's own
   volume is known (the game's own search, `sub_82107490`, called for their
   position). A partner counts for the framing when the point between the
   leader and them is still inside an enabled volume whose view looks the
   same way (direction within a set angle of the leader's) and the zoom that
   fits them stays within the designed range. The target then slides from
   the leader toward the framed partners (the centre of section 2, item 1,
   but never into a gap or another layer). A partner who doesn't fit fades
   out of the framing and is left to the original's rules: off screen, then
   B = become a mask on a partner.
3. **No volume = never.** The target is kept inside the volumes: a slide
   stops at the last point that is still contained.
4. **Zoom within the design.** The farthest zoom is the volume's own
   maximum (perhaps a little more, measured per area) and the camera point
   must stay inside the volume's `C` shape grown by a small margin (the same
   containment test on `C`): the designers' camera space is the limit, not a
   fixed number of units.
5. **Dropped:** near focus (section 7, iffy in play) and the 1.3x reach.
   Kept: the jump hold (each player's own camera point), the cutscene /
   level-change handling (sections 5, 8, 9), active zones (section 6),
   pinned rooms (section 10), the wall ray (section 10), bump easing
   (section 11).
6. **Tests:** volume index per frame with the debug trace (never -1 while
   players are in game), every group switch of `levels/L1` and `L8`, the
   ring and the layered rooms. The Cortex arena needs a save in `levels/L8`
   room 5.
