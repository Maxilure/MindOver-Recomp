# 26. More than two local players (research + a first experiment)

2026-10-04. The original has drop-in co-op for two players. The aim of this
port is up to four players on one PC ([enhancements](../05-enhancements.md#up-to-four-players)).
This page is the first milestone on the way: how the game handles its two
players, every place that is built for exactly two, and an experiment that
put a **third player character** into a level.

![A third player character next to player 1](../images/third-player-spawn-experiment.jpg)

Short version:

* Player 2's Crash is spawned with player 1's when a level loads, and waits
  "not joined". START on a second controller joins it **as the mask**
  (internally the "backpack"), floating next to player 1.
* What each player is doing lives in small global tables with **two
  entries** each (state, starting state, starting sub-state, which
  controller, a 1,028-byte block). They can't simply grow: other variables
  sit right after them.
* The game can hold a third player character: running player 2's spawn
  event a second time, as player 3, puts a third Crash in the level (in
  player 2's look), drawn, standing on the ground, with the game stable.
  He doesn't respond to a controller yet: his lookups read past the end of
  the two-entry tables.
* New test tool: `--debug_fake_pads=N`, up to three more fake controllers
  (players 2-4) driven by script or FIFO commands.

## 1. The original's co-op, as seen in a test run

A test copy of a Wumpa Island save, player 1 on a fake controller, player 2
on a second input device:

* "Join Game (B)" is shown at the top right while player 2 is free.
* Player 2's START joins at once (drop-in); a second START opens a "P2
  Paused" menu (Resume Game, Missions, Tutorials, **Drop Out**, Options,
  Save Game, Quit Game).
* Player 2 joins as the **mask**: the HUD gains player 2's portrait and
  health at the top right, and player 2's stick moves the mask.

The enum names in the executable spell out the states (`ECoOpPlayerState`,
in this order, so numbered 0-4): `NOT_JOINED_GAME`, `MASK`, `IN_GAME`,
`ENTERING_MASK`, `INVALID`. Sub-states (`ECoOpSubState`): `JOINING_GAME`,
`DEAD`, `DISPLAYING_NO_EXIT_ERROR`, `LEAVING_MASK`, `BEING_FORCED_INTO_MASK`,
`DYING`. The mask's "attach" behaviours (`CMaskAttachableBehaviour`,
`CMaskAttacherBehaviour`) and the action `CActionForceEnterBackpack` show
the internal name of the mask: the backpack.

## 2. The per-player tables

| Address | Entries | What | Users |
|---|---|---|---|
| `0x8259B11C` | 2 × u32 | current state (`ECoOpPlayerState`); player 1 = 2 after the level loads, player 2 = 0, then 1 (mask) when it joins | 37 code sites |
| `0x824F3C30` | 2 × u32 | starting state: `[2, 0]`; 0 = "CoOpNotJoinedGame", 3 = "EnterLevelInMask" (names from `sub_82235C30`) | 25 functions (with the next row) |
| `0x824F3C38` | 2 × u32 | starting sub-state: `[9, 9]` | (see above) |
| front end `+8524` | 2 × s32 | **controller of each player**, `(2131 + player) * 4` into the front end manager; -1 = none. Read by `sub_82266130` (36 callers), written by `sub_82266150`; START on a free controller takes the first free entry of the two | |
| `0x825A6280`, `0x825A6A88` | 2 × 1,028 bytes | per-player blocks read while spawning | 5 functions (spawn + front end) |

Facts from the code:

* "How many players" (`GetCurrentNumPlayers`, `sub_82270590`) counts the
  entries of `0x8259B11C` that equal 2, entry 0 and entry 1, written out:
  a player in the mask doesn't count.
* `HaveBothPlayersJoinedGame` (`sub_82265FB8`): in game state 5, both state
  entries non-zero; otherwise both players have a controller.
* What follows `0x8259B11C`: a second
  per-player table, one BYTE per player at `0x8259B124` (digging, the
  turret, co-op), two single bytes (`0x8259B126`; `0x8259B127`, set when
  the game object of section 9 is first created), and two pointers
  (`0x8259B128`, `0x8259B12C`, used by the co-op behaviours' script
  helpers). `0x8259B110/114/118` before it belong to the rope, tube route
  and stream-interest behaviours. So the state table can't grow without
  moving what follows it.

## 3. Who reads the state table

Every `lis`/`addi` style reference to `0x8259B11C` in the code, matched to
the class that owns the function (through the RTTI vtables of
`tools/rtti_vtables.py`, or the nearest caller that is a virtual method,
`tools/callgraph.py`):

| System | Classes (vtable slot) |
|---|---|
| Co-op core | `CCoOpBehaviour` (vtable `0x82037D54`: [4] update `sub_82234FB0`, which writes the state through `sub_82234DC8`; [5], [10], [14], [17]), `CCoOpCountdownBehaviour`, `CMaskAttachableBehaviour`, `CActionForceEnterBackpack` |
| Spawning and level logic | `CActionSpawnPlayer` (section 4), `CActionSpawnLauncher`, `CActionResetPlayer`, `CActionTeleportActor`, `CRequirementNumPlayers`, `CRequirementPlayerInGame`, `CRequirementTriggerVolume`, cinematics (`CMasterCinematicAction`), ambient actors |
| HUD and menus | `CHealthDisplay` (4 sites), `CPlayerIdentifierArrow` (the P1 / P2 arrows), `CMapScreenAction`, `CTutorialScreenAction`, `CFrontendManager` (pause menus, joining) |
| Gameplay | `Crash` (slots 5 and 10), `CCharacterFightTreeBehaviour`, `CFloorEffectAction` (+ projectile / radial), `CProximityVibrationEmitterBehaviour`, `CReticleAimingBehaviour`, `TurretProp`, `CControllerDigBehaviour` |
| Owner not found yet | `sub_8221E510`, `sub_822FF8A8`, `sub_821B9D48`, and co-op helpers `sub_82235160`-`sub_82235CB8` |

The camera is not in the list: it finds the players another way (the level
scripts know a player called `EPlayer_CAMERA_FOCUSING_ON`). The two-player
camera is planned to be reworked rather than patched for four.

How the write was found: a gdb hardware watchpoint on `0x8259B110-0x8259B12F`
(guest address + `0x100000000` on the host), set once the game's main loop
`sub_8227AEE0` runs, logging each write with the game functions on the
stack. Level load: `sub_8229C6F8` (the spawn, section 4) writes player 1's
2. Player 2's START: `sub_82234DC8` writes player 2's 1, called from the
game's script engine.

## 4. Spawning the players

Every level has one `CActionSpawnPlayer` event per player (vtable
`0x8203C2E4`, a `CActionSpawnActor` / `CObjectiveEvent`), executed by slot 2
= `sub_8229C6F8` when the level loads: player 1's first, then player 2's.
So player 2's Crash exists from the start, hidden, "not joined".

| Event offset | What |
|---|---|
| `+0` | vtable |
| `+4` | next event (player 1 -> player 2 -> 0) |
| `+16` | the actor it spawned; the base spawn (`sub_8229AC08`) only creates one while this is 0 |
| `+20`, `+24`, `+28` | position (floats) |
| `+44`, `+48` | 64-bit name hash of the new actor (differs per player) |
| `+88` | player index (0 = player 1, 1 = player 2) |

After the base spawn, `sub_8229C6F8` copies the starting state into the
current state (`0x8259B11C[i] = 0x824F3C30[i]`) and reads the per-player
blocks at `0x825A6280` / `0x825A6A88`. Player 2's different look
(`Crash_Player2.bmp` instead of player 1's texture) is a texture swap made
for the player's index.

## 5. Co-op in the game's scripts

The scripts in `default.rcf` (plain Lua bytecode, findings/19 section 1)
use co-op in about 15 files: `script/classes/crash` (the co-op behaviours
on Crash), `script/util` (`KillPlayer1` / `KillPlayer2`),
`script/mixins/npc_methods` (`GetCurrentNumPlayers`, kills in co-op),
`script/objectives/objectives` (players named `EPlayer_ONE`, `EPlayer_TWO`,
`EPlayer_EITHER`, `EPlayer_CAMERA_FOCUSING_ON`; `DO_ForcePlayerIntoMask`;
"one / two players out of pack" deaths; co-op tutorials and counters), the
levels' `objectives` files, the fight trees of Crash and several titans,
and `createtemplateactor` (the tilting co-op platforms: two-player
puzzles). `SetPlayerCoOpState` / `GetPlayerCoOpState` are never called by
a script: state changes happen in compiled code. Script rules that name
player one and two will need rules for three and four (who gets forced into
the mask, what a two-person platform does with three on it), which the
data patcher (findings/24 section 7) can deliver as changed script files.

## 6. The experiment: a third player character

A temporary hook on `sub_8229C6F8` (switched on by an environment variable,
not part of the port; replaced by section 8's spawn): right after player 2's event ran, it runs a copy of
it with player index 2, `+16` cleared (else the copy "already spawned"
player 2's Crash and does nothing: the first try), the name hash changed by
one bit (an actor name of its own) and the position moved 2 units. For the
call, the starting-state word read for index 2 (one past the table: really
player 1's starting sub-state) says "in game", and both words past the
tables are put back afterwards.

Result (picture at the top): a third Crash appears in player 2's look,
drawn, standing on the ground, idle; the game runs on, and player 2 still
joins as the mask. A controller playing as player 3 doesn't move him,
also with "controller 2" written into a third entry of the front end's
controller table: his state and controller lookups still read whatever
follows the two-entry tables.

## 7. Test tool: more fake controllers

`--debug_fake_pads=N` (1-3, with the debug input FIFO or script; see
`src/debug_input_script.h`) adds fake controllers 2 to N+1, which play as
players 2 to N+1 unless the Players tab says otherwise. A `p<N>.` prefix
sends a command to one of them:

```
--debug_fake_pads=3 --debug_input_fifo=<fifo>
echo "p2.start" > <fifo>          # player 2 joins (as the mask)
echo "p2.lsright 2000" > <fifo>   # ... and moves it
echo "p3.lsup+a 1000" > <fifo>    # player 3: stick up + jump
```

The keyboard driver must be on (it installs the player assignment); with
no keyboard player wanted, `player.keyboard = off` in the controls file.
Checked: fake controller 2's START joins player 2, its stick moves the
mask.

## 8. Four entries: the tables moved (built)

`src/players/more_players.*` gives the co-op tables four entries, always
on (a table can't change address during a run; nothing changes for players
1 and 2):

| Table | How it gets four entries |
|---|---|
| state `0x8259B11C` (37 sites) | grows in place to `0x8259B12B`; the flag bytes, the two single bytes and the pointer `0x8259B128` move to a block of ours (20 references) |
| starting state `0x824F3C30` (5 sites) | moves to our block |
| starting sub-state `0x824F3C38` (24 sites) | grows in place to `0x824F3C47`; `0x824F3C40` has no references, the float at `0x824F3C44` (read and written by `sub_821AD970`, read by `sub_821AE1A0`) moves |
| controllers (front end `+8524`) | getter `sub_82266130` / setter `sub_82266150` / reset `sub_82266178`: players 3-4 kept on our side |
| characters `0x8259B1C0` | getter / setter: players 3-4 play player 2's character |
| per-player lists `0x825A6280`, `0x825A6A88` | 2 hooks in the spawn: players 3-4 read empty lists of ours |
| reset `sub_82234B58` | rewritten for four players |

Tiny getters and setters are rewritten in C++. Every other reference gets a
**midasm hook** (`crash_mom_manifest.toml`), right before the instruction
that uses the old address, that points the register holding it at the new
place: after an `addi` that computed a table's address, the result register;
before a `lwz`/`stb` with a `lis` base, the base register (checked dead
afterwards, or used only for that variable). `GetCurrentNumPlayers` counts
four entries. `--local_players=3|4` adds players 3-4's spawn events to
every level: the game's own creator `sub_82283670` allocates and constructs
player 2's, then ours are built the same way **as player 2** (character,
spawn point, name) and renumbered, with a name hash of their own.

A lesson from the first version: the address scan stopped at a `lis`
base's first use, so it missed the second variable reached through the
same base. The float at `0x824F3C44` looked read-only and was served as a
constant; in a test Crash lay on the ground, respawning, and player 2 could
not join. The scan now follows a base register until it is overwritten;
with the float moved properly, two-player co-op behaves as before (player
2 joins as the mask, player 1 walks). With `--local_players=3` the third
spawn event is created; player 3's START still joins **as player 2**: the
join logic, below.

## 9. The join and the game object

The front end's per-frame input (`sub_82264988`) loops over the four
controllers. For one pressing START (or two other inputs) during play
(game state 5, not paused): nothing if `HaveBothPlayersJoinedGame`, or if
the controller already belongs to player 1 or 2; else the player to join
is player 2 when player 2's state is "not joined" (`r31` at `0x822650E0`),
the controller is given to that player (`sub_82266150`) and join messages
(ids 36, 1, 42) go to that player's actor.

The actor comes from the GAME OBJECT, a singleton at `0x825B0008` returned
by `sub_82270608` (311 call sites), which holds more per-player data:

| Offset | What | Code |
|---|---|---|
| `+16`, `+20` | the players' actors | 27 direct reads; ~29 indexed reads `(p + 4) * 4`, each behind a "player < 2" check |
| `+24`, `+28` | a second per-player list (cleared with the actors) | 10 direct reads; ~14 indexed reads `(p + 6) * 4` |
| `+36`, `+37` | per-player bytes | 4 + 4 reads |
| `+40` | an int, -1 at start | |

The "player < 2" checks made players 3-4 harmless at first: they got "no
actor". Next, in that order (section 10): the actor list with four entries (the `+24`
list moves; the checks let players 3-4 through), the join choosing the
first player not joined, then the mask, the off-screen rules and the camera
for more than two, and the HUD (portraits, health, arrows) with a look for
players 3 and 4.

## 10. Players 3-4 join; the one mask slot

Built on section 9 (all in `src/players/more_players.*`, hooks generated
by a scratch script that checks every site's instruction pattern):

* **The character list**: the 30 indexed readers and the writer
  (`sub_821AD9E0`, found with a gdb watch on the object's `+16..+31` during
  a level load) get two hooks each: the "player < 2" guard lets players 3-4
  through, and the read takes theirs from our block. Most guards let a
  player 3-4 through only once joined: with every guard open, the
  original's "which player is the other one" questions could find a hidden
  player 3. The join's own two lookups and the writer pass always.
* **Which player is this character?** (`sub_8226FD20`, the first step of
  `sub_8226FCD8`, 26 callers): rewritten to search four players. Before
  that, player 3's join never got past "not joined": the co-op behaviour
  asks it for its own number.
* **Wiring a character to a controller** (`sub_822747F8`): for a player
  without a controller the original takes "the one the other player
  doesn't use", a two-player rule. Players 3-4 take their own number's
  controller if free, else the first free one. Without any wiring the
  level never finished loading (tested).
* **The join** picks the first player not joined (hook at `0x822650F8`),
  `HaveBothPlayersJoinedGame` means "every local player", and the
  characters of players 3-4 are released when the next level's players are
  created (the store `sub_820B1D10` is reference-counted).

Result (`--debug_coop_trace` logs every state change, join decision and
controller assignment):

```
Co-op: controller 1 pressed START: joins as player 2
Co-op: player 2 controller = 1
Co-op: states 2 1 0 0
Co-op: controller 2 pressed START: joins as player 3
Co-op: player 3 controller = 2
Co-op: states 2 1 1 0          (player 1 in game, players 2 and 3 masks)
```

**The one mask slot.** With a third player, player 2's mask stays where it
appears and doesn't follow the stick. Bisected: without player 3's spawn it
works; moving player 3's spawn point 20 units away or not wiring player 3
does not matter (without wiring the level doesn't load). Counting every
co-op / mask function call (gdb) for 4 s after player 2's START: with three
players the mask's per-frame movement functions (`0x82237D60`,
`0x82238020`, `0x822380E0`, `0x822382C8`, `0x82238380`, 76 calls each with
two players) never run. The mask's update (`CMaskAttachableBehaviour`,
`sub_82237778`) only does its work while its mask is attached (`+144` bit
`0x80`); with two players player 2's mask is attached from the spawn (to
player 1, hidden), with three only player 3's ever is. So player 1 carries
ONE mask and the player-2-style Crash spawned last takes it: how masks
work with more than two players is the next design question (one mask per
Crash on foot, or players 3-4 joining on foot).

## 11. Three players on foot

A joined player can leave the mask with **B** and play as a Crash on foot
(state 2, "in game"). Test: players 2 and 3 join (START), press B, then
walk in opposite directions:

```
Co-op: states 2 1 1 0
Co-op: states 2 2 1 0          (player 2: B)
Co-op: states 2 2 2 0          (player 3: B)
```

![Three players on foot](../images/three-players-on-foot.jpg)

Each Crash answers to its own controller, and the camera zooms out to keep
everyone in view. Players 3-4 have no HUD of their own and no marker over
their head yet, and player 2's mask stayed put while a player 3 existed
(section 10, fixed in section 12).

**Characters.** Each player has a character number (`0x8259B1C0`, getter
`sub_8229F8B8`): 0 = Crash, which with a player number other than 1 gets
player 2's texture (Carbon Crash, the white variant); 9 = Coco, who takes
player 2's place later in the story (an Ice Prison save). The first
version gave players 3-4 player 2's number: two Cocos. Now players 3-4
keep their own (Carbon Crash, 0, until something sets it), and their
spawn event, built as player 2's, asks for their character instead of
player 2's while it is constructed. Outfits are chosen per player in
Crash's house (players 1-2); players 3-4 can't choose yet.

The plan for masks with more players: a player who turns into a mask
rides the **nearest** Crash on foot, and one Crash can carry up to three
masks. That replaces the original's single mask slot on player 1 (built:
section 12).

## 12. Masks for more players: three riders, the nearest host

![Three masks riding player 1](../images/four-players-three-masks.jpg)

**How the original carries a mask.** Every Crash has a holder
(`CMaskAttacherBehaviour`, vtable `0x8203801C`, at Crash `+260`) with ONE
rider: the rider's Crash at holder `+52` (a reference-counted pointer; the
mask is that Crash's own mask form). Crash's message handler
(`sub_821ACAD8`) acts on mask messages (`+16` = 4 attach / 5 detach, `+20`
= the host, `+24` = the rider): attach calls the holder's `sub_822392A0`,
which FIRST lets go of whatever rider it has; detach calls `sub_82239418`,
which lets go of the rider without asking which. Every frame the holder's
message handler (`sub_82238F18`, message types 39 and 25) sends its rider
the attach point's matrix (a joint of the host's skeleton times the host's
world matrix), which the mask's own handler (`sub_82237960`, message 56,
subtypes 2/3) copies. At every level start each player-2-style Crash
attaches itself to player 1 (hidden until it joins). Found by logging both
holder functions and both message branches: with three players, player 3's
spawn pushed player 2 off player 1, and player 2's B (leave the mask)
detached player 3.

**Three riders per Crash** (`src/players/more_players.cpp`):

* attach: with a rider already on, the newcomer becomes an EXTRA rider (two
  more at most): the original attaches it with the main slot briefly empty,
  then the main rider goes back (the original's own "let go of the current
  rider" call inside the attach is passed through untouched; a first version
  promoted an extra rider there and scrambled the riders);
* detach: a hook on the detach branch (`0x821AD890`) remembers the leaving
  rider (message `+24`); an extra one leaves through the main slot for the
  call, the main one as before, and then the first extra one moves up;
* every frame: after the main rider got its matrix, each extra rider gets
  the same message; the mask's handler then moves an extra mask 0.45 units
  to one side or the other along the matrix's first row.

**The nearest host** (the design for more players: a player turning into a
mask rides the nearest Crash on foot): "the other player"
(`sub_82235B10`, 5 callers: turning into a mask, leaving it, the co-op
update) answers, with more than two players: a mask that rides someone ->
its host (the holder bookkeeping keeps a rider -> holder map; a first
version answered "nearest" here too and player 3's leave message went to
player 2, who wasn't carrying it); anyone else -> the nearest Crash in
state 2 (on foot) by world position (Crash `+44` points to its world
matrix, translation at `+48`; checked against the spawn positions).

Tested (fake controllers, `--debug_coop_trace`):

```
holder (player 1) attach player 2 as main / player 3 as extra / player 4 as extra
states 2 1 1 1                       (four players, three masks on player 1)
detach player 2: main -> player 3    (player 2's B: only player 2 leaves)
other player of player 2: player 3, the nearest Crash on foot (1.3 away)
holder (player 3) attach player 2 as main
```

**Crash's own player number** comes from its constructor (`sub_821AC378`):
the FIRST player number without a character yet (a loop over the game
object's list behind a "player < 2" guard). That guard must let every
local player through: with the joined-only one, player 4's Crash took
player 3's number, player 4's slot stayed empty and player 4's START sent
the join message to a null character (a fault loop at guest `0x34`, found
with a gdb breakpoint on the SDK's access-violation callback).

## 13. Leaving a level with players 3-4 (a crash, fixed)

With more than two players, a level change, or Quit Game and loading
another save, ended in "Call to invalid or unregistered function at guest
address 0x00000040" (or another small address). Reproduced with Quit Game
from player 1's pause menu (fake controllers: START, up = Quit Game, A,
left = Yes, A).

* When a Crash leaves the level, `sub_821AC768` clears its own slot in the
  game object's character list (`stwx r28(=0),r10,r3` at `0x821AC814`, r10
  = (p + 4) * 4) and releases the character. Found with a gdb watch on the
  list during Quit Game, then a scan for indexed WRITES into the list (the
  earlier scan only looked for reads). For players 3-4 the read before it
  was redirected to our block but the write was not: it zeroed the object's
  `+24` list and released player 3's character while our block still held
  it; the next level's spawn then released the freed character again
  (caught with a gdb breakpoint on the SDK's invalid-function report: the
  call came from our own release in `sub_82283670`). Now a hook sets the
  store's BASE so it clears our slot (a first version set the full slot
  address, the store added the index on top and missed by 24 bytes), and
  the function's two guards let every local player through.
* A holder that is disabled (its Crash leaves the level; the behaviour's
  slot 16, `sub_82239250`) lets go of ALL its riders; a first version moved
  an extra rider up into the main slot of a holder that was going away.

Quit Game + loading a save tested with three and four players: players
3-4's characters are cleared on the way out, created again in the next
level, and their masks attach again; no errors.

## 14. The loading screen's footprints (research)

The loading screen (`GameLoad_Loading.pag`, persistent package) draws
walking footprint trails from one white picture, `FE_Footprint.tga`,
tinted per trail (red for player 1, blue for player 2). Its update
(around `0x8226B2C8`) loops over the players behind a "player < 2" check
and asks each one's controller (`sub_82266130`): one trail per player with
a controller, two at most, with each trail's colour and walking state kept
in the loading screen object. Players 3-4 would need two more trails there
(state, colours) and the loop opened to four.

## 15. Level changes, masks on more Crashes, one model for every player

Tested with four players (fake controllers), each case with a real level
change: in the first level after Crash's house (name TBD), walking left
leads into the next level within a few seconds.

**A level change with player 3 or 4 on foot faulted** (an endless "read of
guest 0x11"). A gdb backtrace at the SDK's access-violation callback ended
in the Lua interpreter, called by the spawn (`sub_8229C6F8`). For a player
who starts a level on foot the spawn asks the game object for that
player's **carried-actor name** (`sub_822705F0`: object `+108 + 64 * p`,
written before a level change by `sub_8229CB78` through `sub_822705D0`); a
non-empty name is spawned by script next to the player (a titan brought
along). The object has two names: for player 3 the address is `+236`,
which holds other fields (`+240`, and `+244` / `+248` = the level and entry
point of the last level change), so the "name" was garbage. Both tiny
functions are rewritten; players 3-4's names live in our block (empty).

**The game object, field by field** (constructor `sub_8226FA18`):

| Offset | Per player | What |
|---|---|---|
| `+16`, `+20` | yes | characters (section 10) |
| `+24`, `+28` | yes | the **titan** each player rides (`CJackingBehaviour`), 0 = on foot |
| `+32` | no | one more actor (set when player 1's Crash spawns) |
| `+36`, `+37` | yes | a byte each (`+38`, `+39` are padding before `+40`) |
| `+40` | no | an int, -1 at start |
| `+108`, `+172` | yes | carried-actor names, 64 bytes each |
| `+236` .. | no | other fields: `+244` / `+248` level and entry of a level change |

The titan list had the same "player < 2" guard + `(p + 6) * 4` read as the
character list, at 13 sites plus three writers; one function
(`sub_822420D8`) had its guard opened by the character-list hooks and then
read player 4's titan from `+36` = the bytes `01010101`. Players 3-4's titans
now live in our block (guards open for every local player, reads and
writes redirected; manifest), and the helpers `sub_8226FC88` (a player's
titan, else character) and `sub_8226FD60` (whose titan is this) cover four
players. Players 3-4 carry their titan into the next level since
section 22.

**"May I turn into a mask?"** (`sub_82235CB8`): the original says yes only
when nobody is entering a mask and **players 1 and 2 are both on foot**. With
player 2 a mask, players 3-4 could never turn into one. Rewritten for every
local player: nobody entering a mask, I am on foot, my host (the nearest
Crash on foot) is on foot, on screen (the original's check) and carries
fewer than three masks.

**Riders follow their host.**

![Player 3 turns into a mask and the three masks move to player 1](../images/masks-follow-their-host.jpg)

When a Crash that carries masks turns into a
mask itself, its holder is disabled and lets go of every rider; they froze
in place. They now wait and attach to the same Crash their old host
attaches to:

```
may player 3 turn into a mask (host F9D72150)? yes
holder F9F9E4A0 (owner F9F9B690) detach 00000000          (player 3's holder, disabled)
holder F9CB1F80 (owner F9D72150) attach F9F9B690 as main  (player 3 rides player 1)
rider F9F8E960 follows its host F9F9B690 to holder F9CB1F80   (player 2)
rider F9FA72A0 follows its host F9F9B690 to holder F9CB1F80   (player 4)
states 2 1 1 1
```

**A mask's host by player number.** At every level start each
player-2-style Crash attaches to player 1. Who rides whom is now kept by
player number (set at each attach of a joined player, cleared when the
player leaves the mask) and survives a level change: a mask goes back to the
same player if that player is on foot. A detach message to a Crash that
doesn't carry the leaving rider is ignored (before, the original let go of
that Crash's own rider: player 2's mask fell off player 1 when player 4,
riding nobody, pressed B).

Left as it is: `sub_82235D70` (how many players have a character, used by
two script helpers) counts players 1-2. Counting four stopped the hidden
masks' setup at the level start in a test.

**The aiming reticle** (research). `CReticleAimingBehaviour` registers its
reticle in a global table `0x8259AFAC`: one entry per player for players
1-2 and one shared entry for anyone else; the per-frame pass draws the
shared entry with player 1's number. The reticle's own code reads no
controller: what moves it comes from the mask's input. Players 3-4 not
moving it is not reproduced yet.

## 16. An audit: every "for each player" loop

To stop finding two-player code one bug at a time, every "compare with 2 +
branch" in a function that touches players (calls the game object getter,
a player-number function or a controller lookup, or references one of the
per-player tables) was listed: 67 functions, 107 sites (scratch scripts
`audit.py`, `loops.py`). Most are already hooked or unrelated (message
types, sub-states). One new class stood out: **20 loops over the players**
that walk the game object's characters and titans with a counter
(`rO = 16 + 4p` or `24 + 4p`), a "player < 2" guard inside, and
`cmpwi rO,24|32 ; blt top` at the end.

17 of them now run for every local player: the guards open, each list
read gives players 3-4 the entry in our block, and a hook on the end
compare goes back to the top while players remain. They belong to:
cutscenes (`CMasterCinematicAction`, `CAmbientActorAction`,
`CMovieScreenAction`: a message to every player and titan),
`CRequirementTriggerVolume` ("any / every player inside" a trigger
volume), `CRequirementJack`, rumble (`CProximityVibrationEmitterBehaviour`),
`CUpgradeableBehaviour`, `CCinematicKillActorAction`,
`CActionKillJackedActor`, `CActionSetMIPUnStoreActive`,
`CActionSpawnLauncher` (the nearest player), `CActionUnlockMove` (3 loops),
the front end, the aiming reticle's ray (players it ignores) and one
function whose owner is TBD (`sub_822F3978`).

Three stayed two-player at first: the reticle registry (`sub_82161B40`,
writes a three-entry table), the save before a level change
(`sub_8229CB78`, extended in section 22) and the character count
(`sub_82235D70`, section 15).

Tested: four players, three level changes, masks moving between hosts:
no errors; two-player co-op behaves as in the original.

## 17. Mojos collected by players 3-4 (two crashes)

**A freeze**: a mojo collected by player 3 or 4 looped on "write of guest
0x00000110". The HUD controller (constructor `sub_8226A690`) keeps one
display per player at `+12` / `+16`, and `+20` is a 0/1 setting. Three of its
methods take a player number and use the display at `+12 + 4p`
(`sub_8226AB18` / `AB78` / `ABD8`, each setting a "flash" timer at display
`+268` / `+272` / `+276`). For player 3 that is `+20`, read as display 0 or 1,
and `0 + 272` is the address in the error. Found with the new
`tools/play.sh --catch` (below).

**50 or 100 million mojos and endless "Level Up!" screens**: Crash's
collect code (`sub_821ACAD8`) multiplies each mojo by the collector's
**mojo multiplier**, front end `+8572 + 4p` (two entries; `+8580` onwards
are other front-end fields). For player 3 the multiplier was whatever word
followed. Players 3-4's multipliers now live in our block (starting at 1),
and the four users are redirected: raise (`sub_82263690`), reset to 1
(`sub_822636B8`), the collect and the HUD display (hooks).

Mojos themselves are one shared total (the count beside each portrait is
that total), so players 3-4 add to the same count as players 1-2.

`tools/play.sh --catch` runs the game under gdb with
`tools/gdb/catch_fault.py`: at the first read or write through a null
pointer (guest address below `0x10000`; the SDK's access-violation callback
also serves page-protection watches, which are skipped) it writes the game
functions on the stack to `logs/fault-<date_time>.txt` and ends the game.

## 18. A HUD for players 3 and 4

![Players 3 and 4's HUD in the bottom corners](../images/four-player-hud.jpg)

Player 3's HUD sits in the bottom-left corner and player 4's in the
bottom-right, with player 1 / 2's set: portrait, health and special bars,
mojo count, combo multiplier, and "Join Game" while the player isn't in.
With two players nothing changes.

How the original builds it: the HUD controller makes one `CHealthDisplay`
(vtable `0x8203AFC4`, 284 bytes, constructor `sub_822678F8(this, player)`,
player at `+144`) per player and loops over the two in every method. A
display's set-up (`sub_82267B18`) finds its parts by name from two-entry
tables indexed by its player: the "Join Game" page `InGame_CoOp_PlayerN`
(texts Message and Button) and the texts `MojoMultiplierPlayerN`,
`MojoMultiplierFXPlayerN` and `MojoCountPlayerN` of the page `InGame`. Then
it sets a **base position**: x at `+252` (player 1 left, player 2 right,
from the screen width), y at `+256` (456: y counts up from the bottom of a
640 x 480 screen) and a mirror factor at `+260` (+1 player 1, -1 player 2).
Every bar, picture and text it draws is placed from these three numbers.

What the port adds (`src/players/more_players_hud.cpp`):

* a data patch for every in-game menus package (each holds `InGame.prj`):
  copies of player 1's texts named `...Player3` and of player 2's named
  `...Player4`, the pages `InGame_CoOp_Player3` / `4` (copies of 1 / 2 with
  the texts moved down), both pages added to the screen `InGame.scr`. A copy
  changes one digit of a name, so every size stays the same;
* displays for players 3-4, run by every controller method;
* their names (four midasm hooks in the set-up) and their base position:
  player 3 takes player 1's x and mirror, player 4 player 2's, and y =
  `--hud_bottom_y` (130). Since the HUD sits at the bottom, two parts swap
  sides vertically: the bars are drawn `--hud_bottom_bar_shift` (40) lower,
  and the mojo count and multiplier texts move `--hud_bottom_text_shift`
  (100) up, above the portrait;
* the display's two character lookups now take every local player, joined
  or not, like player 2's hidden display (with the joined-only guard a
  display of a player not yet joined bound to nothing and read null).

Portraits: player 2's look (Carbon Crash) has no portrait of its own in the
game's files; players 3-4 show the same portraits as player 2 would.

## 19. The front end's per-player objects

The front end keeps five kinds of per-player HUD objects in arrays of two:

| Offset | Size | Class | What |
|---|---|---|---|
| `+1720` | 32 | `CPlayerIdentifierArrow` | the "1" / "2" marker over a player |
| `+1852` | 3,152 | `CComboCounter` | the combo meter (its player at `+3148`) |
| `+8156` | 92 | `CPlayerLockOnArrow` | the lock-on arrow |
| `+8340` | 40 | `CCounterOpportunityDisplay` | the "counter now" prompt (player at `+8`) |
| `+8420` | 52 | `CReticleController` | the aiming reticle (player at `+44`) |

Four of them now exist for players 3-4 too (`src/players/more_players_frontend.cpp`):
built with the front end, run by its loops (set-up, update, draw, the
reticles' update, the destructor), and found by every "object of player p"
lookup: the combo meter's script helpers and Crash's collect, the counter
prompt, the lock-on arrow, and the reticle, by controller (`sub_822661B8`,
rewritten for four) and by character (`sub_8213FF18` / `sub_8213FFA8` /
`sub_82140038`, which compared with players 1-2 only). Without a reticle
controller, masked players 3-4 could shoot but not move their reticle. The
counter prompt also places itself like the HUD (x `+24`, y `+28`, mirror
`+32`) and finds its text `CounterButtonPlayerN` by name; the front end
sets these objects up at boot, before any in-game package is loaded, so
players 3-4's prompts are set up with the in-game HUD instead.

The markers over the players' heads: players 3-4 get their own too, set
up, updated and drawn like the others (the original's group draw, which
sorts players 1-2's two by distance, stays as it is; ours are drawn after).
A marker takes its picture by name from a two-entry table (`0x825A5B20`,
`HUD_coop_player_one` / `two`) and its tint from a two-entry colour table
(`0x825B0120`: for player 3 the next word, an init flag, a black see-through
marker). Until players 3-4 have pictures of their own they show player 1 /
2's banner in a colour of their own (`--marker_colour_player3` green,
`--marker_colour_player4` purple). The marker's character lookup, like the
HUD's, takes every local player, joined or not.

The reticles' draw list: `CReticleAimingBehaviour` registers each masked
player's reticle in a global table `0x8259AFAC` of three (player 1's,
player 2's, one shared, drawn in player 1's colours), so with players 3 and
4 both masks only one of their reticles was drawn. The original's update
still registers; right after it the reticle moves to a table of ours (one
entry per player plus the shared one) and the draw (`sub_82162540`) is
rewritten over ours. The reticle's colours come from two-entry tables by
player (`0x825A72FC`, and `0x82507760` for its flashing variant: player 1
red, player 2 green): players 3-4 use their marker colours.

## 20. A NaN position (safety net)

At the save totem in the first level after Crash's house, player 3's
hidden Crash, riding as a mask, was given a NaN position: leaving the mask
it stayed invisible, picked up mission items and mojos it never touched
(the optional missions ticked, the shared mojo count jumped) and held the
co-op camera, so player 1 couldn't walk away. Found by adding positions to
`--debug_coop_trace`, then a gdb watch on that Crash's position: the NaN
came from the physics behaviour's "set position" (`sub_8217F528`, which
snaps the position from the mask's message to the world with
`sub_822E6AA0`). The exact cause is still unknown: the mask's matrices were
valid, and it only happens at that spot. A "set position" that ends at a
NaN now keeps the previous position (logged), which removes every symptom.

## 21. Who controls what: devices, players and menus

Three problems showed up once three and four players could play together:
a device moved to another player in the Controls menu (F6) often did nothing
or drove someone else, players 3-4 couldn't use menus at all, and a menu
opened by one player (the save totem, an upgrade, the name screen) was
worked by another.

**Sockets and players.** The 360 game asks for four controller *sockets*
(XInput users 0-3) and decides by itself which socket belongs to which of
its players: the title screen gives player 1 the socket that pressed START,
and a START on a free socket in play joins as "the first free player". The
Controls menu's choice picked a *socket*, so "Player 2" there and the game's
player 2 could be different people, and moving one device could leave a
player with an empty socket. Now one meaning everywhere: **socket N = the
game's player N**.

| Where | What |
|---|---|
| the join (`sub_82264988`, hook `MorePlayersJoinPick`) | the controller in socket N joins as player N (if N < `--local_players` and player N isn't playing), never as "the first free player" |
| the controller setter (`sub_82266150`) | player p is always given socket p (the title tried to give player 1 the keyboard's own socket when the keyboard was set to player 4) |
| `input/players.cpp` | outside play (game state != 5: title, main menus, loading) every device also answers on socket 1, so any device can start the game and work the main menu (which listens to player 1 only) |
| the Controls menu | moving a device onto a player **swaps**: the devices that were there take the moved device's old player; the list offers players 1 to `--local_players` |

**How the menus read buttons.** Every front-end screen asks "was button B
pressed?" through one script method, `IsButtonPressed` (`sub_822652B0`;
`r4` button, `r6` a player or -1). The front end's script methods were
listed from their registration function `sub_822636D8` (names next to
addresses: `StoreWhoPausedGame` 0x82265968, `ResetPlayerWhoPausedGame`
0x82266120, `GetPlayersControllerNum` 0x82266130, `SetInMainMenuFE`
0x8225F870, `ShowSavePrompt` 0x8225F718, `HasUpgradeToPresent` 0x822635E8
...). With `r6` = -1 the answer comes from:

* the menu owner, front end `+8536` ("PlayerWhoPausedGame"), while the game
  global's byte 168 bit 0x10 is set (on in the pause menu and the level-up
  screen); in that mode a question naming another player is refused too;
* player 1 only in the main menus (`+8594` bit 0x20, `SetInMainMenuFE`);
* otherwise a loop over the players (**0..1**) or over every controller
  (0..3), by button.

The owner is `+8540` (who last pressed START in play, recorded by
`sub_82265978` inside the 0..1 player loop) copied by `StoreWhoPausedGame`
when a pause menu opens. The level-up screen (`CUpgradeScreenAction`,
vtable `0x82025AA4`: slot 4 Enter, 5 Update, 6 Exit) goes through the same
step, so it belonged to whoever last pressed START, usually player 1: only
that player could close it. The totem's menus run with no owner. Front
end flags found on the way: `+8592` 0x20 map, 0x08 tutorial, 0x04 save
prompt (set the moment Crash uses the totem), 0x02 mini-game menu, 0x01
game complete; `+8594` bit 0x80 = a menu is on screen in play (the totem's
question, the pause menu, the whole save-list flow); the confirm box at
`+172` (its byte `+4`: 0x80 enabled, 0x40 showing; update `sub_8225DE68`
asks for left 27, right 40, A 1).

Traced with fake controllers at the save totem: player 2's A answered the
totem's question, player 3's never did; player 3's START never paused.

**The rule now** (`src/players/menu_input.cpp`), while a level is played:

* the three player loops of `IsButtonPressed` cover every local player
  (manifest hooks `MenuInputPlayerLoop`), so players 3-4 can pause and use
  menus;
* a question for "anyone" goes to the menu's owner only, when there is one:
  1. a pause menu: who paused (`+8536`);
  2. the player who last used an object (`CInteractorBehaviour`
     `sub_8214A9A8` sends the object nearby message 2 with its own
     character at `+28`): the totem's question if it appears within 8 s,
     then the whole flow behind it (save list, overwrite question, name
     screen) while `+8594` bit 0x80 stays set; also the save prompt and
     mini-game menu flags.
* the level-up screen answers to every player: upgrades are shared, like
  mojos. Between its Enter and Exit each playing player is asked in turn,
  each one as the stored owner for the length of its own question (the
  game's owner check refuses anyone else), and the owner is put back;
* anything else (tutorials, the map) and every question naming a player
  stay as they were.

Tested with fake controllers: player 3 uses the totem, players 1 and 2 press
A, Down and Right throughout: only player 3 answers the question, moves in
the save list, opens the overwrite question and types in the name screen.
Player 3 pauses: player 1's Down and START are ignored, player 3 scrolls and
unpauses (the title then reads "Paused": the game only has "Player 1/2
paused" texts). Two players: after the totem, player 2 pauses and owns the
pause menu. Keyboard set to player 4: Enter leaves the title, the arrows
move the main menu, and in a level its START joins as player 4. The
level-up rule was tested by applying it to a pause menu (a temporary
switch): player 2 paused, players 1 and 3 moved the cursor and player 3
closed it. A real level-up couldn't be set off by a script: calling the
upgrade check directly (`sub_82177D38` with "present" = 1, as the
behaviour's message handler `sub_82177AA8` does) opens the screen but its
reveal (`sub_820E57A0` -> `sub_820E5E08`) reads a null pointer, for player 1
as much as player 2: something a real mojo pickup prepares is missing.

`--debug_menu_input_trace` logs every press a menu hears (button, the player
asked, who it was answered for and why, the caller), START presses, owner
changes, interactions, game state and flag changes.

## 22. Titans carried into the next level by players 3 and 4

A player 3 or 4 riding a titan into a level change arrived on foot, the
titan gone. Carrying a titan takes three steps, and two of them stopped
at player 2:

1. **Before the level change**, `sub_8229CB78` walks the players
   (`r19 = p`, `r22 = 24 + 4p`, `r28 = 1028p`): the character's state goes
   into list A (`0x825A6280 + 1028p`), the ridden titan's template name
   into the game object's carried-actor name (`sub_822705D0`) and its state
   into list B (`0x825A6A88 + 1028p`). The loop had a "player < 2" guard
   and ended at `r22 = 32`. It now runs for every local player with the
   section 16 hooks (guard, character and titan reads, loop end) plus the
   list A/B base hooks of section 10. Players 3-4's lists and names live in
   our block.
2. **While the next level loads**, the level loader `sub_822E5E00` (mode
   3, `0x822E606C`-`0x822E60B8`) hashes each player's carried name and
   references its inventory section (`sub_822E44B8`), which loads the
   titan's package. Its counter stopped at 2. With step 1 fixed but not
   this one, player 3's titan was created without its assets: the physics
   set-up (`sub_8217F808`) looked up the titan's `EllipsoidShape`, got
   null, and `sub_822531E8` read `null + 0x1C` (fault loop, caught with
   `tools/play.sh --catch`'s gdb script). A new loop-end hook
   (`MorePlayersLoopEndPlayer`) runs it for every local player.
3. **At the spawn**, `sub_8229C6F8` reads the name and lists back (4-player
   since sections 10 and 15) and puts the player in the titan.

Tested with three players: player 3 takes a pocketed Roller out (RB),
player 1 walks into the next level, and player 3 starts that level on the
Roller with its level, mojo and health unchanged, without errors.


## 23. Deaths with three or four players

**The bug.** With `--local_players` 3 or 4 and three or more players in
game, a player other than player 1 who died (a fight, a pit, the cheat
menu's Kill) lay on the ground for good: health 0, co-op state still 2, no
drop-out, no respawn. Player 2 too. With two players, player 2's death drops
it out (state 0, back to the mask, "Please Wait", then "Join Game").

**Where deaths are handled.** Not in compiled code: in the scripts.
`script/objectives/objectives.lua` (Radical's Lua 5.0 layout,
`notes/scratch-tools/dis50.py` reads it) has four objectives, each
"`WAIT_NumPlayers(n)` AND `WAIT_PlayerIsDead(p)`, then `DO_ResetPlayer(p,
drop)`":

| Objective | n | p | Reset |
|---|---|---|---|
| `OnePlayerOutOfPack_P1_Dies` | 1 | player 1 | fade out, back at the checkpoint |
| `OnePlayerOutOfPack_P2_Dies` | 1 | player 2 | fade out, back at the checkpoint |
| `TwoPlayersOutOfPack_P1_Dies` | 2 | player 1 | drops out, the other plays on |
| `TwoPlayersOutOfPack_P2_Dies` | 2 | player 2 | drops out, the other plays on |

`n` is the number of players **in game** (state 2; `CRequirementNumPlayers`,
check `sub_822A3178` = `GetCurrentNumPlayers` `sub_82270590` == n). With three
or four in game nothing matched, and players 3-4 have no objectives at all.
`WAIT_PlayerIsDead` = `CRequirementActorIsDead` (vtable `0x82040B30`, check
`sub_822A01E8`, player at `+20`): the game object's alive byte `+36 + player`
is 0. Crash's code writes that byte by his own player number (`+320`; e.g.
`0x821AC96C`), so players 3-4's land in `+38` / `+39`, unused otherwise.
`DO_ResetPlayer` = `CActionResetPlayer` (vtable `0x8203FCEC`, player at `+8`,
"drop" in bit 0x80 of `+12`); its work, `sub_82297928`, reaches the dying
Crash's co-op behaviour (message kind 48, sub-kind 8 -> `sub_82235760`: state
0, sub-state 6). Its per-player reads were already opened to four players
(manifest hooks at `0x82297954` / `0x822979C8`). Found with a write watch on
the state table (two players: the drop-out came from `sub_82234DC8` <-
`sub_82235350` <- ... <- `sub_82297928`; four players: never called).

**The fix** (`more_players.cpp`): the scripts' own rules, widened.

1. `WAIT_NumPlayers(2)` = two **or more** in game (only these objectives and
   one more n = 1 rule in the same file use `WAIT_NumPlayers`).
2. `WAIT_PlayerIsDead(player 2)` = one of players 2-4 in game is dead (the
   first found is remembered).
3. `DO_ResetPlayer(player 2)` resets the one remembered.

Tested with four players on foot, with and without god mode: players 2, 3
and 4 killed one after another each drop out (states 2 0 2 2 -> 2 0 0 2 ->
2 0 0 0); player 2 gets its "Please Wait" countdown, players 3-4 "Join Game"
in their corners right away (cause and fix: section 24);
players 3-4 rejoin with START and leave the mask with B, with full health.
Player 1 dying while the others play drops out too, as with two players.
Two players: unchanged.

## 24. Players 3-4 on their own: countdown, drop out, "the other player"

**The countdown.** A player who drops out after a death sees "Please Wait
5.. 1" in its HUD corner, then "Join Game"; START is refused until then.
It is per player, kept by the dying Crash itself:

* `CCoOpCountdownBehaviour` (vtable `0x82037E6C`, field
  `m_CoOpCountdownBehaviour` of every Crash): seconds left at `+28`, counted
  down by its update (slot 4); script methods `StartCountDown` (`sub_82236EF0`,
  f1 = seconds), `StopCountDown`, `IsACountDownActive`; messages 48/14
  (running?) and 48/15 (whole seconds left + 1) answer the HUD.
* Started from data, not code: `fighttrees/Crash.bfig`'s death node runs
  `SetPlayerCoOpSubState(DEAD)`, `HideAndAttachToOtherPlayer`,
  `m_CoOpCountdownBehaviour:StartCountDown(5)`, then
  `SetPlayerCoOpState(NOT_JOINED_GAME)`. Caught with a temporary hook on
  `sub_82236EF0` (guest stack: the Lua VM `0x8240xxxx` called from Crash's
  message handler `0x821AD8BC`): it ran for players 2, 3 and 4 alike.
* Crash's compiled fight tree asks `IsACountDownActive` before a join, so a
  START during the countdown is refused for every player.
* The HUD display's corner (`sub_82269480`): while the player is out and its
  sub-state is DEAD (1), it asks **its player's Crash** (game object list,
  `0x822695EC`) whether a countdown runs and shows "Please Wait N", else
  "Join Game".

Players 3-4 showed "Join Game" at once because that lookup went through the
port's "joined players only" guard (section 10): a dropped-out player 3 is
"not joined", so its corner found no Crash and never asked. Player 2's
hidden Crash always answers. The site now lets every local player through,
like the display's other lookups (section 18). Tested with four players:
players 3, 4 and 2 killed (2 and 4 in the same frame): each corner counts
down from 5 on its own clock, then "Join Game"; player 3's START during its
countdown is refused, after it player 3 joins; two players unchanged.

**"The other player" by number.** `CCoOpBehaviour`'s helper `sub_82235B00`
answers "player 2 for player 1, player 1 for everyone else". Its users, by
the script names registered at `0x82235E70`..:

| Function | What "the other" was used for |
|---|---|
| `IsOtherPlayerDigging` `sub_82234AE0` | the other's digging flag |
| `DeatchFromOtherPlayerAndUnhide` `sub_82235258` | leaving a mask at the host's height while it digs |
| `IsOkayToLeaveMaskState` `sub_82235A80` | no while the other is DYING |
| Drop Out (`sub_822357B0`, front end message 48/7) | the rider forced out of its mask |
| `ForceOtherPlayerToBecomeMask` `sub_82235B70` | the one forced into a mask (Crash.bfig, before `InteractWithInteractable`) |

With more than two players "the other" is now the partner the mask code
already uses (`sub_82235B10`: the Crash carrying me if I am a mask, else the
nearest Crash on foot). Sub-state numbers (registration at `0x8223619C`):
0 joining, 1 dead, 2 no-exit message, 3 leaving the mask, 5 forced into a
mask, 7 dying; unnamed 4 (forced out of the mask), 6 (dropped out), 8 (a
mask dropping out), 9 (nothing going on).

* **Drop Out** chose its path by "players 1 and 2 both in game": player 3
  dropping out while player 2 was a mask set player 1's sub-state to 4. Now
  (two midasm hooks on its state reads + a wrapper): the "on foot, someone
  else plays" path when nobody rides the player, else the "free my riders"
  path, which frees every rider. Tested: player 3 drops out with players 2
  and 4 as masks on player 1; player 1 untouched.
* **ForceOtherPlayerToBecomeMask** worked only with players 1 and 2 both on
  foot and forced one of them; now every other player on foot is forced
  (three masks fit on one Crash). Not tested in play yet (it needs the
  interaction that calls it).

