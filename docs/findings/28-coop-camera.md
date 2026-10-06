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
`--coop_camera_max_distance` (default 45) caps how far it backs off;
`--coop_camera_group_focus` (default 2, 0 = everyone counts the same) sets
how much a group of players outweighs a player on their own;
`--debug_coop_camera_trace` logs the camera, the players and the volume's
numbers every 10th frame.

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

`sub_82235978` only answers "off screen" while players 1 AND 2 are both in
game (states 2 or 3, read from `0x8259B11C`): with player 1 and players 3-4
but no player 2, nobody counts as off screen and the catch-up never fires.
The camera's fit (section 2) keeps everyone in view up to the maximum
distance, so the catch-up only matters past it.

