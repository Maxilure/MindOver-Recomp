# 23. Keyboard and mouse: what every 360 button does, and a keyboard that spins the stick

2026-10-02. Roadmap phase 2, step 1: play with keyboard and mouse.

Short version: the game's own data says what each controller input does. An
input map script binds controller values to game events (jump, light attack,
block, jack, pocket...), and the font draws the private characters of the
button prompts ("Jump ±") as 360 button pictures. One move is not a button:
**Spin is "Rotate the Left Analog Stick"**, counted by a stick-circle
detector. A new keyboard / mouse driver (`src/input/`) turns keys into a
virtual 360 controller, with a Spin key that draws circles with the virtual
stick, any key combination (Shift to block while moving), mouse buttons and
the wheel as keys, and a Controls menu (**F6**) to rebind everything, saved
in `controls.toml` next to the exe. The same menu decides which device plays
as which player, for the game's two-player co-op (keyboard on player 1, a
controller on player 2).

## 1. Which button does what (from the game's data)

### 1.1 The input map script

`script/mixins/inputmap_methods.lua` in `default.rcf` (bytecode with the Lua
5.1 signature `\x1bLuaQ` but Radical's 5.0-style layout and opcodes, the
same as the fight trees of [findings/22](22-ground-contact-high-fps.md))
defines `InputMapMethods.SetupController`. It builds one `CInputMap` per
input map type and fills it with events:

```
BeginEvent(EInputMapEvent_JUMP)  AddInputDown(EControllerValue_X)  EndEvent()
BeginEvent(EInputMapEvent_BLOCK) AddInputDown(R1) AddInputHeldDown(R1) EndEvent()
...
```

The controller values use **PlayStation names** (`X`, `CIRCLE`, `SQUARE`,
`TRIANGLE`, `R1`, `R2`, `L1`, `L2`, `DPAD*`, `START`, `SELECT`), with
`RAD_WII` branches for the Wii. The map types are `GENERAL` (always),
`PLAYER` (Crash), `ENEMY` (a jacked titan: the player controls the enemy),
and `PROJECTILE`.

### 1.2 The button prompts and their font

The text bank (a Scrooby text bible, chunk `0x1800D`, in a package of
`default.rcf`; UTF-16 big-endian, every platform's strings in one bank)
writes button prompts with private characters: "Jump ±", "Map µ",
"Stun an enemy and Press ² to Jack", "Hold ·". The game's two fonts
(`Titans_Large` and `Titans_Small`, Pure3D texture fonts, chunk `0x22000`)
have a glyph table (chunk `0x22001`: a count, then 40 bytes per glyph:
texture index, 8 floats of texture rectangle and metrics, character code).
For the characters U+00A5 to U+00BE the texture index has bit 31 set and
points to a **picture of its own** (font textures 21-35: 64x64 or 128x128,
8-bit palettized TGA) instead of a cut-out of the letter sheet:

| Character | Picture | Character | Picture |
|---|---|---|---|
| ± U+00B1 | A | · U+00B7 | RT |
| ² U+00B2 | B | ¸ U+00B8 | RB |
| ³ U+00B3 | X | ½ U+00BD | LT |
| ´ U+00B4 | Y | ¾ U+00BE | LB |
| µ U+00B5 | Back | ¹ U+00B9 | left stick |
| ¶ U+00B6 | Start | ¥ U+00A5 | right stick |
| º, » | red arrows (left, right) | ¼ U+00BC | a circular "rotate" arrow |

### 1.3 Together

PlayStation face-button positions map to the 360's (X = A, CIRCLE = B,
SQUARE = X, TRIANGLE = Y). The prompts pin the shoulders: the block event is
on `R1` and its prompt is "Hold ·" (RT); pocketing is on `R2` and its prompt
is "¸" (RB). By the same symmetry `L1` = LT, `L2` = LB. (The exe's own
table from controller values to 360 buttons, used by Radical's input
manager `sub_8235D188`, hasn't been read; the `L1`/LT pair rests on that
symmetry and on the "Hold ½" prompts.)

| 360 | What it does (event names from the script) |
|---|---|
| A | Jump (`JUMP`; held: `JUMP_HOLD`); confirm in menus |
| B | Jack / unjack a titan (`JACK`, `ACTION`); back in menus |
| X | Light attack (`ATTACK_LIGHT`; as a titan, held: `ATTACK_LIGHT_CHARGED`) |
| Y | Heavy attack (`ATTACK_HEAVY`, `DETACH`); as a titan RT + Y = `ATTACK_SHOVE` |
| RT | Block, held (`BLOCK`, `GROUND_SLIDE`) |
| LT | A titan's special attack (`ATTACK_SPECIAL`) |
| RB | Pocket / unpocket a titan (`POCKET`) |
| Back, Start | Map, pause |
| Left stick | Move; **rotating it = spin** (section 2) |
| Right stick | Co-op: "Rotate the Right Analog Stick for a Power Shot" (the camera is fixed) |
| D-pad | Menus |
| LB | Only debug events (`GOD_MODE` outside release builds, combo tests) |
| L3, R3 | Nothing found |

The general map also has an event named `SPIN` on the X button (pressed or
held). Crash's fight tree checks it in one place only (a state that also
watches for landing on bouncy objects), so it isn't the spin attack itself.

## 2. Spin: a stick gesture

The prompts write spins as "¹¼" (left stick + the rotate arrow), and the
moves' descriptions spell them out: "Rotate Left Analog Stick and Press Heavy
Attack" (Gyro Jackhammer), "Rotate the Left Analog Stick and Press Unjack"
(Spin Dismount), "Rotate Left Analog Stick and Repeatedly Press Light
Attack". The basic spin attack is the rotation alone.

The code has a **stick-circle detector** on the controller object:

* `sub_82273030` walks to the controller's detector (object at `+0x1EC`)
  and returns its "circle done" byte (`+40`, read by `sub_82150608`), set by
  the detector's message handler `sub_82150258` (message type 3, subtype 7
  or 8). The angle tracking itself sits behind that message system and
  wasn't traced further.
* Script function `HasLookDone360` (`sub_8221E1C8`; props you crank by
  turning the stick) asks the same detector.
* `GetNumberof360RotationsDone` (`CCoOpBehaviour`, registered at
  `0x822360C0`, returns byte `+52`) counts circles: `sub_822350C8` adds one
  per completed circle and restarts a **1.0 s** timer (constant
  `0x8201EE28`); if the timer runs out first, the count drops to 0. So a
  circle every second or faster keeps a count going.

The fight tree also reads the stick's push: `GetSquareOfStickExtent < 0.81`
(stick under 90 %) picks a different state than a full push in two places,
which is what a "walk" key can use.

## 3. The driver (`src/input/`)

The SDK has a keyboard driver (`--mnk_mode`), a generic table from keys to
buttons. It can't do what this game needs: it matches Shift / Ctrl / Alt
exactly (holding Shift silences W/A/S/D, so "block while moving" is
impossible), it has no gestures, no mouse side buttons or wheel, and it
listens to the main window only. Our driver replaces it (it stays off).

**Actions, not buttons** (`bindings.h`). A table of what a player does
(Move, Walk, Spin, Jump, Light attack, Heavy attack, Block, Jack, Titan
special, Pocket, Map, Pause, Menu: confirm, Menu: back, D-pad, the rest of
the controller), each pointing at its 360 control. Two actions can press the
same button (Jump and Menu: confirm are both A). Each action has two key
slots. Default layout:

| Action | 360 | Keys |
|---|---|---|
| Move | Left stick | W A S D |
| Walk (hold) | Left stick, half way | Ctrl |
| Spin (hold) | Left stick circles | Q, middle mouse |
| Jump | A | Space |
| Light attack | X | Left mouse, J |
| Heavy attack | Y | Right mouse, K |
| Block (hold) | RT | Shift |
| Jack / unjack | B | E |
| Titan special attack | LT | F |
| Pocket / unpocket | RB | R |
| Map | Back | Tab, M |
| Pause | Start | Esc |
| Menu: confirm / back | A / B | Enter / Backspace |
| D-pad | D-pad | arrow keys |
| Right stick | Right stick | numpad 8 4 2 6 |
| LB, L3, R3 | | none |

F3, F4, F6-F12 and the backtick key belong to the tools (SDK overlays,
Controls menu, renderer tools) and can't be bound.

**The pad from the keys** (`keyboard_mouse.h`), built at every poll (the
game polls once per frame):

* Opposite keys held together: the one pressed last wins (a quick change
  of direction never stops Crash dead).
* Diagonals are scaled onto the stick's circle; Walk shortens the push
  (0.5 by default).
* **Spin**, while held, turns the stick clockwise at 3 circles per second
  (adjustable), starting from the direction of the last movement, so Crash
  doesn't jerk. On release it finishes the circle in progress plus 0.15 of
  a turn, like a thumb overshooting: a quick tap is one whole circle.
* The mouse wheel has no "held": each notch is a 60 ms press and a 40 ms
  gap, queued (up to 4).
* Keyboard and controller work together: by default both are player 1 and
  the input system merges them (buttons OR'ed, the bigger stick push wins);
  section 5 puts them on different players.
* The game gets a neutral pad while no game window has the focus, while an
  ImGui overlay uses the keyboard (F4, console), and while the Controls menu
  is open. While ImGui only wants the mouse (the pointer over an overlay or a
  pop-up), just the mouse buttons and wheel stop counting: clicking an
  overlay never attacks, and an achievement pop-up under the pointer doesn't
  freeze the keys. A window losing the focus forgets the held keys (their
  key-up would never arrive). Both windows of dual mode (F8) take keys.

**The Controls menu** (`controls_menu.h`, F6), Keys tab: the action table by section
(Moving, Fighting, Titans, Menus and map, Rest of the controller), with the
360 control in its button colour (that's what the game's prompts show).
Click a key slot, then press the new key or mouse button (Esc cancels);
right-click empties it. A key already in use moves over (one key = one
action) and the menu says from where. Sliders for spin speed and walk lean,
a reset button. (The Players tab: section 5.) The driver sees every key before ImGui (listener z-order
128 vs 64), which is how it hands "the next key" to the menu and keeps it
from both the game and ImGui.

**`controls.toml`** next to the exe (`--controls_file` to move it): plain
`action = "Key, Key"` lines, the whole file written on every change (the
SDK's F4 save writes every changed flag, command-line ones included, so the
controls don't go through it). Hand edits: unknown keys or names are logged
and skipped, tool keys refused, a key on two actions goes to the one the file
names (between two lines, the later one), missing actions keep their
defaults. The log lists what differs from the defaults at every start.

## 4. Tests

* `--debug_input_fifo` takes `key <Name> [ms]` (a key as if typed, through
  the bindings); `--debug_kbm_trace` logs every change of the pad state.
  Boot to the main menu with keys only: Esc (Start) skips the intro movies,
  Enter (A) leaves "Press START", the arrow keys (D-pad) move the selection
  (two presses: Load Game, then Credits), Backspace (B) goes back to the
  title, Enter opens the menu again.
* A 0.3 s tap of Spin at 30 fps: the stick goes up, right, down, left, up
  and a little further (12 positions over 0.4 s), then lets go.
* `controls.toml` written and read back gives the same keys (a key alone
  in the second slot comes back in the first); a hand-written file with
  lowercase names, aliases ("Enter", "Esc"), a third key, a tool key, an
  unknown key, an unknown setting, a key on two actions and an out-of-range
  spin speed gave the expected warnings and result.

* Players: with `player.keyboard = 2` and one controller connected, the log
  said "controller = player 1 (automatic)", "Keyboard and mouse = player 2",
  "players with a device: 1 2". The game polls player 2 (the keyboard's
  states were read), and the title screen takes any player's Start. Before
  patch 0011 that gave the profile message (section 5.1); with it, Enter on
  the player-2 keyboard went straight to the main menu, and its Load Game
  list showed the profile's saves.

Playtested afterwards: keyboard and mouse through gameplay (the Spin key
spins Crash), and two players together (keyboard on player 1, a controller
on player 2, Join Game from the pause menu).

Both the shared profile (patch 0011) and the stick circles are stand-ins
that lean on the emulated Xbox and its controller. The goal is native
code: the game's own sign-in and save calls answered by the port itself,
and the spin triggered directly instead of through the virtual stick.
(Sign-in and saves: done 2026-10-10, [findings/32](32-native-saves-and-profiles.md).)

## 5. Players: which device is player 1, which is player 2

The game has drop-in co-op for two players (its pause menu offers "Join
Game" and "Drop Out"; the text bank has co-op puzzles and "LOSE CO-OP LIVES
ONLY WHEN BOTH PLAYERS ARE KNOCKED OUT OF THE GAME"). The 360 game asks for
each player's controller by number: Radical's input manager
(`sub_8235D358`) polls all four, `XInputGetState` for user 0 to 3.

Which device answers for which number is the input system's "device
assignment". The SDK's default (`SlotAssignment`) is fixed: every synthetic
device (the keyboard) on player 1, real pads by connection order (the first
pad on player 1 too, the second on player 2). With one controller, keyboard
and controller were always the same player.

`src/input/players.*` replaces it (installed before the game's first poll):

* Keyboard and mouse: the chosen player, default 1, or off.
* Each controller: the chosen player, off, or Automatic (the SDK's rule:
  the Nth controller connected = player N), the default.
* The SDK's "None" stand-in (a neutral pad that keeps player 1 connected)
  and the debug input script: always player 1.
* Devices on the same player are merged as before.

The Controls menu got a Players tab (the keys moved to a Keys tab): every
connected device with a "Plays as" choice, applied at the next poll, and
remembered in `controls.toml` as `player.keyboard = 1` / `player.pad:<SDL
GUID> = 2` (the GUID names the controller model, so a second identical pad
is `:2`; the controller's name is written as a comment). The log names who
plays as whom whenever a device comes or goes ("Players: ...").

### 5.1 Player 2 needs a profile: sharing player 1's

First try: with a controller on player 2, pressing Start on it at "Press
START" gave "You do not have an active gamer profile. You will be unable to
save your game progress without an active gamer profile." The game asks
`XamUserGetSigninState` for users 0-3 to build a mask of signed-in players
(`sub_8227B1D8`, run on the system's "sign-in changed" notification, and
`sub_8227CEA8`), and refuses a player whose bit is clear: what a real 360
does when nobody is signed in on controller 2. The SDK has a single profile
and answers only for user 0.

A PC has one player profile, so SDK patch 0011 lets users 1-3 share it:
while the app says a device plays as them (`players.cpp`: a keyboard or
controller assigned to that player), they report player 1's profile (signed
in, same XUID, name and settings). Saves were already stored by the
profile's XUID whatever the player number, so they stay in one place.
Whenever the set of players with a device changes, the game is told with a
"sign-in changed" notification (`XN_SYS_SIGNINCHANGED`), the one it rebuilds
its mask on.

> **Update 2026-10-10:** patch 0011 is removed, and so are profiles: the
> game's sign-in code is replaced by the port's, where "who plays" is simply
> which controllers play (the same device assignment), without the SDK ([findings/32](32-native-saves-and-profiles.md)
> section 4).

## 6. Next: keyboard pictures in the prompts

The prompts' button pictures are separate textures (section 1.2), so they
can be swapped without touching the game's files: when the last input came
from the keyboard, the native renderer can draw a key picture instead of the
A / X / RT picture (recognized by its contents, like the texture cache
already checks textures), showing the key bound to that button.
