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
| Back | Escape | B |
| Previous / next tab | Q / E | LB / RB |
| X action (rename a save, Dual Mode, the name screen's backspace) | X | X |
| Y action (delete a save, remove a key in Options -> Controls) | Backspace | Y |

Old controls files (no `controls_version` line) get Space on Menu: confirm
and Escape on Menu: back where the player hasn't used those keys for another
menu action; the file now starts with `controls_version = 2`, and each
action's comment says `[menus]` or `[play]`.

**2026-10-15 (`controls_version = 3`):** Backspace moved from Back to the Y
action: a keyboard player expects Backspace to remove things (a key in
Options -> Controls, a save in the save list), and Escape alone goes back.
Version 2 files lose Backspace on Back when another key stays there, and
Menu: Y goes from Y to Backspace when it was Y (or empty). Walk's lean went
from 0.5 to 0.75 (allowed 0.7-0.9): the game ignores a left stick leaning
less than about 0.65, so the old Walk stopped Crash (measured with the debug
console, Crash on foot, 1.5 s of Up: full lean 14.9 units, 0.5 and 0.6 none,
0.7 2.8, 0.8 5.1). Files holding the old 0.5 get 0.75.

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

## 5. Typing a name (2026-10-15)

On the game's name screen (New Game's name; the save list's rename, in the
menus and in game: findings/24 s.5 and s.7) the keyboard TYPES
(`input/name_typing.*`):

* **Text mode.** While the screen is in its typing state (+216 = 3), the
  keyboard driver switches the window's text input on (SDL text input:
  characters in the keyboard's own layout, Shift for capitals, accents
  through the system's input method) and keeps every typed character. Its
  keys make only these buttons then: the arrows (D-pad: the on-screen
  cursor), Enter (Start = Done) and Escape (B = Back). Every other key types;
  W types a W instead of moving the cursor, Space types a space.
* **Each character goes in through the screen's own typing**: "type the
  selected key" (`sub_820D6618`) asks "which character is under the cursor"
  (`sub_820D6450`, a lookup in a table at `0x82505558`: pages of 4 x 11
  UTF-16 cells, page = character set + caps, 0 = no key) and is answered the
  typed character for that one call. The 16-letter limit, the name field,
  the "letter_select" sound all stay the game's. A character the game's font
  (Titans_Large) can't draw is refused with the game's error sound. The
  on-screen cursor jumps to the typed character's key when the page in use
  has it.
* **Backspace** calls the screen's erase (`sub_820D66E0`: the last letter
  off, the erase sound), at once and then repeating while held (450 ms, then
  every 60 ms).
* **The prompts** say Backspace / Done / Select with the keys' own pictures
  (Backspace, Enter, the left mouse button: key slots, findings/37 s.4)
  while the keyboard is in use, the controller's X / Start / A otherwise.

**The mouse on the on-screen keyboard.** Pointing at a key moves the cursor
there (with the screen's "letter_scroll" sound); a click chooses it (an A
press: the screen types it, or does its special key's job). A key's cell is
29 x 31 canvas units around its letter (the letters are thin), the four
special keys are their 64-pixel buttons (`Key_CharSet_BG`, `Key_Caps_BG`,
`Key_Backspace_BG`, `Key_Enter_BG`). Moving the cursor from code redoes the
screen's own highlight around a move (read from its typing update
`0x820D5D00`-`0x820D5F54`): the old key back to scale 1.0 (the caps label 0.5)
and green (`0x82506BB4`), the new one 1.4 (caps 0.65) and yellow
(`0x82506BBC`); the two right-hand buttons (pictures at the screen's +48 /
+52) show frame 1 while selected. (Writing only the cursor left the old key
highlighted.)

**A space key.** The on-screen keyboard had no space. Its special keys are
handled by POSITION in the typing update: row 1, column 10 (the left arrow)
calls the erase, row 2, column 10 (the tick) is Done, column 0 rows 1-2 are
the character set and caps. Erasing already has Backspace, the controller's
X and its prompt, so that arrow key types a SPACE now (its only caller is
the typing update, `0x820D5FC8`; the port's own erase calls the original),
and its picture is the same arrow MIRRORED, pointing right ("move on"): a
data patch reverses each row of the sprites `FE_NameEntry_Backspace.tga`
and `_Highlight.tga` (an 8-bit palettized 32 x 32 TGA inside chunk 0x19002)
in the main menus' package and, where the rename route copied them, the
in-game menus' packages. Only the game's own pixels: a first version drew a
"␣" symbol in code.

**Also fixed:** the save list's own X / Y / A / B check (rename, delete,
the delete question) asked only the SDK's input system, which hasn't had
the keyboard since section 2's change: X and Backspace did nothing there on
the keyboard. It reads the keyboard's last pad too now.

## 6. Tests

On a copy of the saves, keys only (debug console key taps; the test copy's
keyboard switched on in a controls file of its own): title -> Enter -> main
menu -> Down, Enter -> Load Game (prompt bar: Delete Y, Rename X, Back Esc,
Select Enter) -> Esc -> main menu; in a level: Escape pauses, Escape resumes;
pause -> Options with the arrows and Enter, E / Q through the tabs; Spin on
foot and on a titan. The old controls file was migrated (log lines "also on
Space / Escape").

Not covered yet: the mouse in menus, typing a name with the keyboard on the
name screen, the texts that name the controller ("Rotate the Left Stick").
