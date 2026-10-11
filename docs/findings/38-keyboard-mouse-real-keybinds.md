# 38. Real keyboard binds: menus with keys of their own, Spin as a key

2026-10-11. Roadmap phase 2: the keyboard stops pretending to be a
controller.

Short version: until now the keyboard was a virtual 360 controller inside
the SDK's input system ([findings/23](23-keyboard-and-mouse.md)): every key
pressed a pad button, so a key had the same job everywhere (Backspace was
"B" in menus because Escape was "Start"), and Spin drew circles with the
virtual stick. Now:

* the keys go into the game's controller state through the port's own
  XInputGetState wrapper, built with knowledge of what's on screen;
* every action belongs to **play**, the **menus**, or both, and a key does
  the job of the context it was pressed in: Escape pauses in play and goes
  back in menus, Enter / Space confirm, Q / E switch the Options tabs;
* **Spin** answers the game's own "did the left stick just make a full
  circle?" question, so Crash spins on a key press and can still be steered.

## 1. How Radical's input manager reads a controller

`sub_8235D188` polls one controller object per socket each frame:
`XInputGetCapabilities` (`sub_824742E8`), then `XInputGetState`
(`sub_824742F0`), the sticks through a dead-zone helper (`sub_8235C9F0`),
then every **button object** of the controller (+28: 60-byte records, the
count per controller type from the table at `0x824E8B0C`) updates itself
from that state (`sub_8235CAE8`: +24 value, +36 change threshold, +52 a
listener told of each change). The game's input maps and menus sit behind
those listeners. So the place to feed keys "directly" is the state that
poll receives: `sub_824742F0`, which the port already wraps (input recording,
the lost-controller filter, the Options tabs).

## 2. The keyboard's way in

* The SDK still lists a "Keyboard and mouse" device, **connected and
  silent** (its state is always neutral): the player assignment
  ([findings/23](23-keyboard-and-mouse.md) s.5) keeps knowing that a
  keyboard plays and as whom, so "who plays", the lost-controller check and
  the Players tab work as before.
* In the wrapper, for each socket the keyboard answers on (its player; outside
  play also socket 1, the same rule the SDK side uses), the driver builds the
  keys' state at that moment and merges it into the game's: buttons together,
  the bigger trigger, a stick the keys push replaces the pad's.
* No longer behind the SDK's own "is the window active" gate; the port's
  focus check (no game window focused, an ImGui overlay using the keys)
  applies as before.

## 3. Play and menus

"Is a menu on screen?" (`menu_input::MenuOnScreen`): outside a level (the
front end: title, main menus, loading), or in a level the front end's flags
(+8594: 0x80 = pause / totem / save menus, 0x20 = main menus), a question box
(+176 bit 0x40), or the level-up screen.

Each action has a context: **play** (jump, attacks, block, jack, titan
special, pocket, map, pause, walk, spin, right stick, LB/L3/R3), **menus**
(confirm, back, previous / next tab, the X and Y actions), or **both**
(movement keys: menus answer to the stick too; the D-pad). One key may hold
one play action and one menu action, never two of an overlapping context
(the Controls menu's rule follows it).

A key's context is taken **when it goes down** and kept until it's released:
Escape pressed in play is "Start" and opens the pause menu; still held, it
doesn't become "B" and close it again. Measured: Escape in play -> buttons
0x0010 (Start), the pause menu opens; Escape again -> 0x2000 (B), it closes.

| Menus | Keys (default) | 360 |
|---|---|---|
| Confirm | Enter, Space | A |
| Back | Escape, Backspace | B |
| Previous / next tab | Q / E | LB / RB |
| X action (rename a save, Dual Mode, the name screen's backspace) | X | X |
| Y action (delete a save) | Y | Y |

Old controls files (no `controls_version` line) get Space on Menu: confirm
and Escape on Menu: back where the player hasn't used those keys for another
menu action; the file now starts with `controls_version = 2`, and each
action's comment says `[menus]` or `[play]`.

The prompts follow ([findings/37](37-keyboard-mouse-prompts.md) s.3): the
font patch now adds six menu glyphs (A, B, X, Y, LB, RB), the prompt bar's
button texts use them, and so do the port's own menus (the Options screen's
tab hints and its Select / Dual Mode prompts).

## 4. Spin

Crash's moves test the left stick's circle through the script function
`HasControlDone360` (registered at `0x8221EAA0`: `sub_8221E200`). It asks
the player's controller object (the character's behaviour controller,
vtable slot 92) for its left-stick circle detector at +488 (`sub_82272FB8`
-> detector byte +40, `sub_82150608`); +492 is the right stick's
(`HasLookDone360`, `sub_8221E1C8`, the co-op power shot). Several compiled
fight-tree states ask it (`sub_8219B1D0`, `sub_821C1498`, `sub_821C7940`,
`sub_821DE7D8`, `sub_821FB998`, and `sub_8221E238`, "either stick").

Measured with a logging wrapper and fake stick circles: the answer is true
for **one** question after a finished circle, false at the next. So a circle
is a one-shot event, and the Spin key can produce the same event:

* a press = one circle waiting; held, one more every 1 / *spins per second*
  (default 3; the Controls menu's slider), each kept 0.4 s and then dropped
  (a stale one would start a spin long after the key, e.g. on landing);
* the wrapper of `sub_8221E200` answers "yes" when the question is about the
  keyboard player's character and a circle is waiting. The question's `r3` is
  the **behaviour controller** (class `Crash`, a `CScriptBehaviourController`)
  that every `CActor` holds at +0x40: P1's Crash at `...A30` was asked about
  as `...A70`. The same holds for a titan being ridden.

Measured: a tap = one spin; held 1.5 s = a new spin each time the last one
ended (the fight tree only asks while a spin can start); riding the
Ratcicle, the same. The stick stays with the movement keys, so the spin can
be steered.

## 5. Tests

On a copy of the saves, keys only (debug console key taps; the test copy's
keyboard switched on in a controls file of its own): title -> Enter -> main
menu -> Down, Enter -> Load Game (prompt bar: Delete Y, Rename X, Back Esc,
Select Enter) -> Esc -> main menu; in a level: Escape pauses, Escape resumes;
pause -> Options with the arrows and Enter, E / Q through the tabs; Spin on
foot and on a titan. The old controls file was migrated (log lines "also on
Space / Escape").

Not covered yet: the mouse in menus, typing a name with the keyboard on the
name screen, the texts that name the controller ("Rotate the Left Stick").
