# 62. How the game hears its players: input maps and events

**Why this was read (2026-11-14).** The keyboard and mouse reach the game
as a pretend controller: the port builds the 360 controller state the keys
stand for and merges it into what `XInputGetState` returns
([findings/38](38-keyboard-mouse-real-keybinds.md) s.2). That loses where a
press came from. A key pressed in a menu (Backspace = the menus' Y) that is
still held when play goes on becomes a Y button, and Y in play is a heavy
attack ([findings/26](26-more-local-players.md) s.67). The question:
can the keyboard answer the game's own questions instead ("did the player
jump?", "was Back pressed in this menu?"), the way the Spin key already does
([findings/38](38-keyboard-mouse-real-keybinds.md) s.4)? This section maps
every layer between the controller and those questions. Read with Ghidra's
pseudo-C (`tools/ghidra_decompile.sh`), `xexdis` and `tools/callgraph.py`;
nothing has been changed yet.

## 1. The layers

```
XInputGetState (sub_824742F0, the port's wrapper)
  -> Radical's input manager poll sub_8235D188, per socket, every frame
     -> BUTTON OBJECTS (controller +0x1C, 60 B each; count per controller
        type, table 0x824E8B18): sub_8235CAE8 reads one value out of the
        state (sub_8235C200: offset / shift / mask / kind from table
        0x824E8B1C: 0 on-off, 1 0..1, 2-3 centred axis), keeps it at +0x18,
        and on a change (more than +0x24) tells its listener (+0x34, arg +0x38)
        -> the INPUT CONTROLLER's VALUE CHANNELS: controller +0x2C + 12 x value
           (EControllerValue, PlayStation names: X, CIRCLE, SQUARE, TRIANGLE,
           R1, R2, L1, L2, DPAD*, START, SELECT, the four stick axes...);
           each channel = a short history of samples (16 B: value + time;
           sub_82273478 = the newest value)
  -> once per frame, CInputManager (vtable 0x8203B4C4) slot 10 sub_822745A0:
     for every input controller, sub_822710D8(dt, controller):
       * each INPUT MAP of the controller (list +0x198..+0x19C):
         sub_82275E20 -> per event sub_82275768 = the EVENT RULES (below)
       * sub_82270FE8 (a lock count at +0x204 bits 14-15)
       * sub_82271610 / sub_82271358: stick histories -> the circle
         detectors (+0x1E8 left, +0x1EC right: HasControlDone360 /
         HasLookDone360)
       * sub_82272B40 (a timer at +0x1FC / +0x200)
```

**Input maps** (`CInputMap`, vtable `0x8203BDEC`) are made by the script
`inputmap_methods.lua` ([findings/23](23-keyboard-and-mouse.md) s.1.1):
`SetInputMapType`, then per event `BeginEvent(E)`, `AddInputDown(v)` /
`AddInputUp(v)` / `AddInputHeldDown(v)` / `AddInputHeldUp(v)` (registered at
`0x822762F4..`: `sub_82275FB0` / `5FC0` / `5FD0` / `5FE0` -> `sub_822760D0`
with kind 0 / 1 / 2 / 3), `EndEvent()`. Map +0x18 = its type, +0x1C = its
EVENT TABLE: entries of 28 bytes sorted by event id (binary search
`sub_82276AF0`): +0 the event, +4 its inputs (12 B each: kind and value),
+0xD (high nibble) the input count, +0x14 bit 0x80 = **happened this frame**,
+0x18 not 0 = removed (the lookup treats the entry as missing). An input =
12 bytes: u32 +0 = the controller value (top 16 bits) and the kind (bits
12-15), +4 flags (0x80 = a held kind, 0x20 = met so far).

| Map type | Number |
|---|---|
| CAMERA | 0 |
| MENU | 1 |
| MOVIE | 2 |
| CHEAT | 3 |
| GENERAL | 4 |
| PLAYER | 5 |
| ENEMY (a jacked titan) | 6 |
| PROJECTILE | 7 |

Events are numbered in the order of their names in the exe (`0x8203B838..`):
0 GAME_PAUSE, 1 ACCEPT, 2 ACTION, 3-6 ATTACK_COMBO1-4, 7 ATTACK_HEAVY,
8 ATTACK_LIGHT, 9 ATTACK_LIGHT_CHARGED, 10 ATTACK_SPECIAL, 11 ATTACK_SHOVE,
12 BACK, 13 BLOCK, 14 CHEER, 15 DETACH, 16 DOWN, 17 DOWN_HELD, 18 FINISHMOVE,
19-21 FIRE_WEAPON_FROM_MASK_BUTTON1-3, 22 GROUND_SLIDE, 23 JACK, 24 JUMP,
25 JUMP_HOLD, 26 JUMP_ATTACK, 27 LEFT, 28 LEFT_HELD, 29-31 the light / heavy
combos, 32 MAP, 33 NEXT, 34-35 OBJECTIVE_TRIGGER_1-2, 36 PAUSE, 37 POCKET,
38 PREVIOUS, 39 PROJECTILE_FIRE, 40 RIGHT, 41 RIGHT_HELD, 42 SPECIAL, 43 SPIN,
44 STAND, 45 UP, 46 UP_HELD, 47 WII_SPECIAL_PROJECTILE, 48 PROJECTILE_HARPOON,
49 CO_OP_JOIN, 50 GOD_MODE. The numbering is confirmed by calls with known
meanings: CheckInputEvent(5, 8) = PLAYER ATTACK_LIGHT (Keep Spinning,
findings/40 s.4.3), the dismount (4, 2) = GENERAL ACTION and (6, 23) = ENEMY
JACK (findings/26 s.60), the main menu's IsButtonPressed 1 = ACCEPT and 12 =
BACK, the Options tabs' 27 / 40 = LEFT / RIGHT, the map screen's 32 = MAP.

**The event rules** (`sub_82275768`): each input of the event is tested
against its value channel by `sub_822752E0`: kind 0 "went down" and 1 "went
up" (`sub_82271B88` / `sub_82271C48`), 2 / 3 "held down / up"
(`sub_82271B50`). Inputs in order make a sequence with a time window
(several inputs = a combination, like ATTACK_SHOVE = block held + Y); when
all are met the event's happened bit is set for this frame.

## 2. The questions

Every question about a button or an event goes through ONE function,
`sub_82272338(controller, map type, event, ignore_lock)`:

1. walks up the controller's parents (+0x24) and stops with "no" when the
   root is LOCKED (+0x204 top byte > 0: cutscenes; `sub_82272D68`), unless
   the caller says to ignore the lock;
2. finds the map of that type (`sub_822725B0`, list +0x198);
3. answers its event's happened bit (`sub_82275D00`).

30 call sites in 13 functions (map, event):

| Caller | What | Asks |
|---|---|---|
| `sub_8221F1E8` CheckInputEvent | script / compiled fight trees: every move | any (r4, r5) |
| `sub_82265A28`, `sub_82265798`, `sub_82265978` | the front end's IsButtonPressed `sub_822652B0` (menus, pause, totems...) | MENU, the "button" = an event |
| `sub_820D0B88` | a menu screen action (front end, `0x820D` range) | MENU 46, 17, 28, 41 (UP_HELD, DOWN_HELD, LEFT_HELD, RIGHT_HELD) |
| `sub_82264988` | the co-op join pick (findings/26 s.10) | CHEAT 1, 27, 40, 45, 16, 46, 28, 41; MENU 36, 1, 49 (PAUSE, ACCEPT, CO_OP_JOIN) |
| `sub_8210DF28` | | PLAYER 8 (ATTACK_LIGHT) |
| `sub_82110B18` (x2) | | ENEMY 7 (ATTACK_HEAVY) |
| `sub_821AE850` | near the free jack `sub_821AE998` | GENERAL 24 (JUMP) |
| `sub_82234FB0` | CCoOp code | PLAYER 42 (SPECIAL) |
| `sub_82274BE8` (x2) | the movie skip | MOVIE |
| `sub_8216F9C8` (x2), `sub_822A1340` | | (to read) |

So the menus are no different from play: **"was A pressed in the menu" is
the MENU map's ACCEPT event**, "Back" is MENU BACK, the Options tabs MENU
PREVIOUS / NEXT. Each map's events come from the same pad buttons, but they
are separate questions.

**Raw values** are read in a few places only (not through events):

* `sub_82271D08` (newest value of a channel, i.e. the sticks): the circle
  detectors (`sub_82271610`, `sub_82271358`), the script's stick functions
  (`sub_822718E8`, `sub_82271D20`, `sub_82272208`, `sub_822723F8`:
  GetSquareOfStickExtent, GetControllerWorldSpaceDirectionVector and their
  kin, registered at `0x8221EA58` / `0x8221EC7C`), the front end
  (`sub_82265798`, `sub_820D0B88`) and the HUD (`sub_8226B2C8`): 9 functions.
* `sub_82271B50` ("held") outside the event rules: `sub_820D0B88` (a menu
  screen) and `sub_82274968`.

## 3. What a native keyboard would be

The keyboard stops being a controller for buttons. Each frame, after the
game has worked out a map's events from the controller (`sub_82275E20`,
wrapped), the keyboard player's events are added from the KEYS:

* a key is bound to game ACTIONS, and an action is an event of a map:
  Jump = PLAYER JUMP (and ENEMY's, GENERAL's: the game asks several maps),
  Heavy attack = PLAYER / ENEMY ATTACK_HEAVY, menu confirm = MENU ACCEPT,
  menu back = MENU BACK, menu Y = MENU CHEER (s.5: the menus' Y is the CHEER
  event);
* a key that is only bound to a MENU event can never set a PLAYER event:
  Backspace held out of a menu is nothing in play, by construction, without
  muting anything;
* "went down / held / went up" are worked out from the key's own state,
  with the map's own sequence rules (a combination like ATTACK_SHOVE = block
  held + heavy pressed) evaluated the same way as for the controller;
* a controller on the same player (player 1 = keyboard + a pad) keeps its
  events: the keyboard's are added to them, not written over them;
* the cutscene lock, the menus' owners and every per-player rule stay the
  game's: they are checked in `sub_82272338` and its callers, after the bit.

What stays a value: the left stick (WASD) and the right stick. The game's
movement, the circle detectors and the HUD read the stick values directly
(section 2), and a direction plus how far it leans IS what WASD means; the
keys keep writing those two channels, nothing else.

**How it would be checked** (like the native back end, findings/60): a
check mode that runs the keyboard evaluator on the CONTROLLER's values too
and compares its event bits with the game's own, frame by frame, for every
map; zero differences before the keyboard path goes on by default.

## 4. Steps

1. DONE (s.5): a console command `inputmaps <socket>` that dumps every map of a player's
   controller: type, each event, its inputs (kind + value), what happened
   this frame. That gives the real list of events per map (the script's,
   as the 360 build made them) instead of guesses, and the default key for
   each from the input's button.
2. PART DONE (s.6: the shadow route + its check). The event evaluator for keys (`input/key_events.*`): bindings by action
   -> (map, event), the sequence rules from `sub_82275768`, checked
   against the original on controller input (step above).
3. DONE (s.7). Turn it on for the keyboard player; stop merging keyboard BUTTONS into
   the controller state (sticks stay). The muting of findings/26 s.67 and
   the menu-context rule in the keyboard driver become unnecessary and go.
4. The port's own one-shot answers (Spin, Keep Spinning, the dismount)
   fold into the same evaluator.
5. Later, optional: the same for controllers (events from SDL buttons
   directly, no 360 state at all) = the last of "talking to the game as a
   controller".

Open: the three call sites marked "(to read)", what `sub_82274968` does with
a raw "held", and whether any compiled fight tree reads a value channel
directly (the call graph shows none for `sub_82271B50` / `sub_82271D08`
outside the functions above).

## 5. The real maps (step 1: `inputmaps`)

Console `inputmaps <socket 1-4 | 0xADDRESS> [all]` (`src/debug_console.cpp`)
prints every map of a socket's input controller with each event's inputs
("down", "up", "held-down", "held-up" + the value, in the order the
sequence must happen) and HAPPENED for this frame's bits. Dumped 2026-11-14
on a test copy: sockets 1-3 in a level (player 1 on foot, and riding a
Ratcicle), and in the pause menu (no change: a menu doesn't swap maps, it
only asks MENU).

Controller values (EControllerValue, strings at `0x8203B5D0..`): 0 MOVEX,
1 MOVEY, 2 CAMERAX, 3 CAMERAY, 4-7 DPADLEFT / RIGHT / UP / DOWN, 8 X (= A),
9 CIRCLE (B), 10 TRIANGLE (Y), 11 SQUARE (X), 12 R1 (RT), 13 L1 (LT),
14 R2 (RB), 15 L2 (LB), 16 START, 17 SELECT (Back). The menus' NEXT / PREVIOUS
on R2 / L2 settle the shoulder question of findings/23 s.1.3: R2 = RB,
L2 = LB.

Every socket has MENU, CHEAT and GENERAL; on foot PLAYER, on a titan ENEMY +
PROJECTILE instead (the character's script adds and removes maps,
`AddInputMap` / `SetCurrentAttackInputMap`). No CAMERA or MOVIE map was on
these controllers (MOVIE: asked by the movie skip, to look at during a
movie).

| Map | Events (360 buttons) |
|---|---|
| MENU | ACCEPT A, ACTION X, BACK B, **CHEER Y**, UP / DOWN / LEFT / RIGHT D-pad (+ _HELD: down then held), MAP Back, NEXT RB, PREVIOUS LB, PAUSE Start, CO_OP_JOIN B, SPECIAL RT held then RB twice, GOD_MODE Start held |
| CHEAT | seven-press D-pad sequences (down, up, down, up...) on UP, UP_HELD, DOWN, LEFT, LEFT_HELD, RIGHT, RIGHT_HELD; ACCEPT = RT held: the original's cheat codes |
| GENERAL | JUMP A, JUMP_HOLD A held, STAND A held up (happens every frame A isn't held), ACTION B held, BLOCK / GROUND_SLIDE RT held, DETACH Y, SPIN X held |
| PLAYER | ATTACK_LIGHT X, ATTACK_HEAVY Y held, JACK B, SPECIAL B released, CHEER RB, FINISHMOVE LB, FIRE_WEAPON_FROM_MASK_BUTTON1-3 RT / RB / X held, OBJECTIVE_TRIGGER_1 / 2 = RT + RB held then A / X |
| ENEMY | ATTACK_LIGHT X, ATTACK_LIGHT_CHARGED X held, ATTACK_HEAVY Y held, ATTACK_SPECIAL LT held, ATTACK_SHOVE RT held then Y, JACK B, POCKET RB, LIGHT_LIGHT_HEAVY / LIGHT_HEAVY_LIGHT / LIGHT_HEAVY_HEAVY (X / Y press-and-release chains), LB held + A / X / D-pad = ATTACK_COMBO1-4, LB + Y = CHEER, LB + LT + D-pad left = JUMP_ATTACK |
| PROJECTILE | PROJECTILE_FIRE, PROJECTILE_HARPOON: X held |

What this means for the keyboard (step 2):

* The menus' Y is the MENU map's **CHEER** event; Crash's heavy attack is the
  PLAYER map's ATTACK_HEAVY (and ENEMY's on a titan). Answered as events,
  a key bound to "menu Y" can only ever be CHEER in MENU.
* Most events are one press or one hold, so the key evaluator is mostly
  "went down / is held / went up" of one action. The chains (LIGHT_HEAVY_*,
  the CHEAT codes, OBJECTIVE_TRIGGER, SPECIAL in MENU, the LB debug
  combos) follow the game's sequence rules; the simplest faithful way is to
  run the game's own rules (`sub_82275768`) over a keyboard-only copy of the
  value channels, map by map: MENU's copy from the menu keys, the others
  from the play keys. Then nothing about the timing is guessed, and the
  check of s.3 is exact by construction.

## 6. Step 2: the game's rules on a value set of our own

The value channels (findings/62 s.1) in detail, read with Ghidra: a channel =
12 bytes {samples, ring size, newest}; a sample = 16 bytes: +0 the value
(f32), +4 its time in ms (the game clock `sub_8235ABC8`), +8 bit 0x80 = down
(value above 0.05). A new sample is added by `sub_82273328` (only when the
value moved). The tests read the ROOT controller (walking +0x24):
"went down" (`sub_82271B88` -> `sub_822734B0`) = walking back from the
newest sample, a down one newer than the controller's previous update time
(+0x194) right after an up one; "went up" (`sub_82271C48` -> `sub_82273558`)
the same the other way; "held" (`sub_82271B50` -> `sub_82273490`) = the
newest sample's bit. The rules (`sub_82275768`) touch nothing else of the
controller but +0x20 (`sub_82273E10`).

So the game's own rules can run on values of our own
(`src/input/key_events.*`): a **shadow controller** (those fields, rings of
its own) and, per input map, a **shadow map** (a copy of its event table
and its events' inputs in guest memory: the rules keep their progress
there, e.g. how long a button has been held). The override of
`sub_82275E20` runs the game's map update first, then the same code
(`__imp__sub_82275E20`) on the shadow map with the shadow controller. A shadow
is made from the real map just before its update (same starting state) and
remade when the real table changes (another level, a titan's maps).

**The game consumes events.** Besides the rules, two functions reset an
event from outside: `sub_82275DC8(map, event)` (one event; its only caller
`sub_82273E98` = IsButtonPressed's "consume" for every socket) and
`sub_82275C90(map)` (every event, from `sub_822729A8`). Found by the check
below: a titan's ATTACK_HEAVY (Y held) came out in the shadow for 3 frames
and not in the game. Both are overridden: the same reset runs on the shadow
map (`sub_82275DC8`'s lookup `sub_82276CE0` would ADD a missing event
through the game's allocator, so the shadow's reset only runs for an event it
has). A keyboard event (step 3) will be consumed exactly like the game's own.

**The check** (`--debug_key_events_check`): the shadow controller gets a
copy of the REAL channels each frame, and after both runs every event's
bytes (all but its inputs pointer) and its inputs' bytes are compared. Log:
"Key events check: N map updates, E events compared (H happened), D differ",
plus the list of events seen happening. Runs with a fake controller on a
test copy, 2026-11-14: player 1 on a Ratcicle (light, heavy, held heavy,
charged, shove = RT held then Y, the light-light-heavy chain, LB combos, LT,
RB, the pause menu's D-pad and held D-pad, Start held, B), player 2 joining,
Back (the map screen): about 170,000 events compared per run, ~3,000
happened, 29-32 different events, **0 differ** (before the consume
overrides: 3 differ, the case above). Without the switch no shadow is made
(the consume overrides only look the map up).

## 7. Step 3: the keyboard as events

On by default (`keyboard_events`; false = the old pretend controller).

* **The keys, split by map** (`KeyboardMouseDriver::LastEventImage`, made at
  every poll): `menu` = the menu actions, the D-pad keys, the mouse's
  pulses (a click's A, a right click's B, a notch's D-pad) and the play
  actions on Start / Back (Pause, Map: the MENU map's PAUSE and MAP events
  are how the game hears them in play); `play` = the play actions and the
  D-pad keys. A key keeps the job it had when it went down
  ([findings/38](38-keyboard-mouse-real-keybinds.md) s.3), so a key pressed
  in a menu is never in `play`.
* **Two keyboard shadow controllers** per keyboard socket (menu, play), with
  rings the size of the real ones, fed once a frame through the game's own
  sample writer (`sub_82273328`, at the game clock `sub_8235ABC8`): values
  4-17 (D-pad, face buttons, triggers, shoulders, Start, Back) = 0 or 1.
  The sticks (values 0-3) stay in the controller state, where the game's
  movement and circle detectors read them.
* **The keyboard's own shadow of each map** (its own sequence progress,
  reset clean when made), worked out by the game's rules on the menu
  controller for MENU and CHEAT, the play one for GENERAL, PLAYER, ENEMY,
  PROJECTILE, either for MOVIE. Its happened bits are ADDED to the real
  map's; a controller on the same player keeps its own (the real update ran
  first, on the controller's values). The consume resets of s.6 reach it too.
* **A bit the keys set lasts one frame.** First test: a held heavy attack and
  Start kept "happening" frame after frame. The game clears an event's bit
  through its rules, by the event's sequence state; a bit added from outside
  has no such state behind it in the real map. The keys' bits are taken back
  before the next update.
* **The game no longer gets the keyboard's buttons.** The XInputGetState
  wrapper (`saves/save_library.cpp`) still merges them (the port's own
  readers: menus, Options, the save list, the lost-controller box, the
  title's any button, the prompts' device check), then takes what the merge
  added back out at the end (not what a port filter added itself, like the
  title's START; not during an input replay). Since s.8 the sticks too:
  the keyboard leaves nothing in the game's controller state.

Tested with the debug console and `--debug_key_events_trace` ("Key events:
map M event E from the keys"), player 1 = keyboard (+ a fake pad), Wumpa
Island: Space = GENERAL JUMP, J = PLAYER ATTACK_LIGHT, K = PLAYER ATTACK_HEAVY
(+ GENERAL DETACH), Shift held = GENERAL BLOCK / GROUND_SLIDE + PLAYER
FIRE_WEAPON_FROM_MASK_BUTTON1; Escape in play = MENU PAUSE (the pause menu
opens), Down = MENU DOWN / DOWN_HELD, Escape there = MENU BACK (closes it),
Tab = MENU MAP (the map screen), Escape there = BACK. **The disconnect box**
(player 1's pad unplugged, Backspace held 1.5 s): MENU CHEER only, the box
closed, no heavy attack while it was still held; a fresh K afterwards =
ATTACK_HEAVY. Main menu: Escape skips the boot movies, Enter at the title,
arrows + Enter into Options, Q / E tabs, Escape back, a mouse click on
Extras. The interim fix of findings/26 s.67 for the keyboard (muting held
keys, menu keys dead in play) is removed; the controllers' part stays.

Not covered yet: the game's few RAW readers of the value channels
(`sub_82271B50` "held" in `sub_820D0B88`, a menu screen action asking RT
held, and `sub_82274968`) see no keyboard buttons now; nothing known depends
on them. A keyboard on player 2-4 takes the same route (by socket), untested.

## 8. The sticks: WASD into the movement values

The game has no "walk forward" event: movement is a direction and how far,
the value channels 0-3 (MOVEX, MOVEY, CAMERAX, CAMERAY). How a controller's
stick gets there (Radical's poll `sub_8235D188`, r3 = the socket's
controller object, socket at +0x10):

1. XInputGetState into a stack copy (r1+80; the gamepad at r1+84);
2. each stick's dead zone, in place: `sub_8235C9F0(f1 = 0.25 from
   0x820037BC, r3 = &x, r4 = &y)` for the left (r1+88/90) and right
   (r1+92/94) sticks;
3. every INPUT OBJECT of the controller (+0x1C, 60 bytes each, count from the
   table at 0x824E8B18) reads its own number from the gamepad
   (`sub_8235CAE8(r3 object, r4 gamepad)` -> `sub_8235C200`): its entry in
   the table at 0x824E8B1C (per controller type, 28 bytes: count, entries)
   = 20 bytes {name, kind, offset, size, mask}. For the gamepad type: 0-13
   the buttons (kind 0, the 16-bit buttons word, one mask each), 14-15 the
   triggers (kind 1, offsets 2 / 3, 8 bits), **16-19 LeftStickX / Y,
   RightStickX / Y** (kinds 2 / 3 = centred axes, offsets 4 / 6 / 8 / 10,
   16 bits); when its number moved, its listener adds the value sample.

The port (`input/key_events.cpp`, part of `keyboard_events`): the poll's
socket is noted around `sub_8235D188`; for the keyboard's socket, the four
stick objects (offset 4-10) read a copy of the gamepad holding the KEYS'
stick numbers, with the game's dead zone applied by its own function, while
the keys push that stick (they replace the controller's stick, as before).
The numbers are the ones the keyboard used to type into the state
(diagonals on the circle, Walk's lean), so movement is unchanged; and the
XInputGetState wrapper takes the keyboard's sticks back out of the state.

Measured on a test copy (Wumpa Island, debug console, `pos` before / after):

| Keys | New route | Old route (`keyboard_events=false`) |
|---|---|---|
| W 1.5 s | (364.000, -278.000) -> (366.850, -288.167) | -> (366.881, -288.161) |
| Control + S 1.5 s (Walk) | -> (365.547, -286.205) | -> (365.577, -286.189) |
| D + W 1 s (diagonal) | -> (361.894, -292.961) | -> (361.928, -292.939) |

(the few hundredths = real-time frame jitter between runs.) A log line
proves the route: "the keys' left stick goes into the movement values (0,
32767); the controller state holds (0, 0)".

