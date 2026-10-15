# 39. The mouse in the game's menus

2026-10-15. Roadmap phase 2: point and click in every menu.

Short version: the game's menus are Scrooby menus whose items each have a
"look" (the text drawn for the item) with a box in the 640 x 480 canvas
([findings/30](30-menus-and-hud.md)). The port maps the mouse pointer from
the window into that canvas and tests it against the boxes: the item under a
moving pointer is selected (with the game's move sound), a left click on it
chooses it (an A press), a right click goes back (B) and a wheel notch is a
step up or down (the D-pad). The prompt bar, the Options tabs, the save list
and the game's question boxes answer to the mouse too. The presses go into
the game the same way the keyboard's do ([findings/38](38-keyboard-mouse-real-keybinds.md)),
so the game's own menu code does the rest.

## 1. From the window to the canvas

* The SDK reports mouse positions in physical pixels. Its presenter fits the
  16:9 picture into the window, centred (bars left / right or top / bottom;
  `rex/ui/presenter.cpp`, safe area 100 %), for both renderers. The pointer
  becomes 0..1 across that picture.
* The game lays its menus out on a 640 x 480 canvas (y up) drawn 4:3 in the
  middle of the 16:9 picture: picture x = (160 + 1.5 x) / 1280, picture
  y = (720 - 1.5 y) / 720. Inverted: x = (1280 u - 160) / 1.5,
  y = 480 (1 - v).
* An element's box: +92 / +96 = x / y of its bottom-left corner as floats in
  1/640-of-the-width units (divide by the constant at `0x8204701C`), +88 /
  +90 = width / height in canvas units (u16); +110 bit 0x80 = shown. The
  Options screen's 0.85x layout and moved glass are already in those fields
  (hovering its rows lands on the right row).

**What counts as an item: its WORDS, not its box.** Menu boxes are often far
wider than the text in them (the main menu's are 400 units wide, about two
thirds of the screen): hovering the empty sides selected items (first
playtest). A Text element (vtable `0x8200BE2C`) also has: +108 its
horizontal justification (byte: 0 left, 1 right, 4 centred), +144 its font
(the `pure3d::TextureFont` object the font loader built: the port knows which
is `Titans_Large` / `Titans_Small`, findings/37), +156 its string, +16 its
4 x 4 transform (x scale at +16: a selected item is drawn 1.2x). The drawn
width = the string's advances in the font x 0.475 canvas units per font pixel
x that scale, measured: "Load Game" in Titans_Large = 346 font px -> 247
picture px (main menu), "Options" in Titans_Small = 170 -> 121 (pause menu),
both 0.475. The text's area is that width placed by its justification in
its box, plus 4 units around. A row with a value (the Options screen: the
name at item +140 on the left, the value at item +144 on the right) counts
from the name's left end to the value's right end, so pointing at the value
works too.

**Pages and layers move and scale as a whole.** The Options screen is drawn
at 0.85x around the screen's centre: in game by scaling its page, in the main
menu by scaling its "OptionsMenu" layer; both also slide in. That isn't in the
elements' own fields: on the in-game Audio tab "Dialogue" was computed at
x -14..86 but drawn at 40..123, so hovering missed or picked the wrong row,
which felt like lag and stutter. A page or layer's 4 x 4 transform at +16
(row vectors: x scale [0], y scale [5], translation [12] / [13] in element
position units = canvas units x the constant at `0x8204701C`) is applied to
every area, layer first, page second, read every frame (it moves during the
slide). Measured on the in-game Options page: scale 0.85, translation y
0.05625 = 36 units = 240 x (1 - 0.85). A CMenuAction knows its page (+16);
the Options screen passes its layer as well.

Timing (to rule out a real slowdown): selecting an item 0.02 ms, its sound
0.01 ms, the Options screen's refresh after a selection 0.06-0.1 ms, a value
change 0.05 ms.

## 2. Menus

`CMenuAction` (the generic menu driver of the front-end states: main menu,
pause menu, Options, difficulty...) has its Scrooby menu at +20; its Update
`sub_820D2A00` runs every frame the menu's screen is up (wrapped already for
the Options screen). For each item (menu +112 list; item +137 bit 0x80 =
selectable) the look at item +140 is hit-tested.

* **Hover** follows the pointer only when it MOVED since the last frame, so a
  pointer resting over an item never fights the arrow keys. The selection
  goes through the game's `SetSelection` (`sub_8237A4C0`) plus its move
  sound.
* **Left click**: made while a menu is on screen, it waits up to 0.2 s for a
  menu that has an item where the click was MADE (the pointer position at
  the button press, not a frame later); that item is selected and the menu
  gets an A press. A click over nothing does nothing.
* **After a click** (three playtests: a click on Options with the pointer
  moving on over Load Game opened Load Game; a 1 s guard fixed that but
  swallowed quick clicks on the Options screen; a ~150 ms guard broke the
  first case again). What happens after an A press differs per screen: the
  main menu reads its selection ~50 ms later (its decision runs when the
  accept flag goes up), the pause menu only when its slide ends (~1 s), a
  value on the Options screen changes at once and the screen stays. Three
  rules together cover all of them:
  1. **The click counts where it was made** (above).
  2. **No mouse during a slide**: the front end's +8592 bit 0x40 ("menus take
     input") is clear while a screen slides in or out (it drops within a
     frame of the A press). Hover and clicks do nothing then, as a
     controller's buttons, and the clicked item stays highlighted. A question
     box takes its own input while it shows (the flag stays clear under it),
     so its Yes / No are exempt.
  3. **The click is remembered** (menu + item) until the next screen takes
     input after a slide, or 60 ms after the press if the screen stays; while
     it is, the game's "which item is selected?" (`GetMenuIndex`
     `sub_821239C8`, also asked by the port's own main-menu decision, now
     through the wrapper) answers the clicked item, wherever the pointer went.
  The Options screen is the port's own: a click on a row changes its value
  directly (no pretend A press), so every click counts, 14 of 14 at 10-12 a
  second. The pretend presses for the game's menus are 60 ms long with 40 ms
  gaps (at least a frame each at 30 fps).
* **Right click** = B and **wheel** = D-pad up / down, in any menu, as short
  presses (~80 ms each, 50 ms apart, queued).
* The mouse buttons in play stay attacks: a key's job is decided when it goes
  down ([findings/38](38-keyboard-mouse-real-keybinds.md) s.3), and clicks
  only queue for menus while a menu is on screen.

**Other screens**

* **Save list** (`CGameSlotScreenAction`, not a CMenuAction): its update
  passes each panel's glass picture (`SlotN_BG`, 388 x 111) as the item's
  target; hover selects the save, a click loads / saves it, the wheel slides
  the list.
* **Prompt bar** (page `FE_Buttons`, every front-end update): a click on a
  prompt's button picture or label presses the button its glyph stands for
  (the play glyphs and the port's menu glyphs: A, B, X, Y, Back, Start, LB,
  RB). "Back" goes back, "Rename" renames, and so on.
* **Options tabs**: a click on the previous / next tab's name or its key hint
  (Q / E) = LB / RB.
* **Volume stars** (Options -> Audio): the pointer over a star selects its
  row; a click on star k sets the volume to k (1-5); a click on the first star
  of a row already at 1 mutes it.
* **Question boxes** ("Are you sure you want to quit?", the totem's save
  question): the front end's box at +172 (+176 bit 0x40 = showing, +188 = its
  Yes / No menu). While it shows, the screen under it (the pause menu) keeps
  running its CMenuAction every frame. In the first test the click on the
  box went to that pause menu instead (the pointer was over its "Options"
  row) and pressed A on whatever the box had selected; now, while a box
  shows, only its own Yes / No answer to the mouse.
  **The box reads its answer when it has FADED OUT, not at the press.** A or
  B sets its state (u32 at box +28) to 2 = closing (`0x8225E0E4`); the
  fade runs, and only at its end does the update read the Yes / No menu's
  selection (+152: 1 = Yes, `0x8225DF78`). The box first took the mouse as
  long as it showed, so a click on No and a flick of the pointer over Yes
  during the fade answered Yes (the "Are you sure you want to quit?" box
  quit the game): the same trap as the screens' slides above. Now a box
  takes the mouse only while its state isn't 2 (it reads 0 the first time
  the quit question opens, 1 after that), and the click is remembered
  through the fade like a screen's. Checked with the debug console: No
  clicked, pointer moved onto Yes 30-100 ms later, state 2 with the
  selection still 0 (No), back in the pause menu; the same after Enter on
  No; a click on Yes quits.

**The boot movies**: the front end counts as a menu ([findings/38](38-keyboard-mouse-real-keybinds.md)
s.3), so Escape had become "back" there, which the boot movies ignore: Escape
no longer skipped them. The boot states (3 BootUp, 41 LicenseScreen, 44
LoadPersistentPostBootUp, 51 Movie, 53 LoadingTitleScreen) now count as play:
Escape is Start again and skips them.

**The title screen: any button** (`input/title_any_button.*`). The Xbox
title asked for START ("Press START") and took A as well. Now any
controller button or trigger, any key (bound or not) and any mouse button
start the game:

* The title's check is in the front end's input update (`sub_82264988`,
  `0x822651DC`): while front end +8594 bit 0x10 is set (the title waits for
  START: 0x18 at the title, 0x28 in the main menu, read with the debug
  console), a controller's START makes it player 1 (findings/25 s.3). A
  filter on every XInputGetState the game makes turns a NEW press of
  anything on a socket into a 150 ms START while the bit is up (a key: the
  keyboard driver's "last key down" time moved, on the socket the keyboard
  plays on), and hides every other button then. After the title, buttons
  still held from it stay hidden until let go: an Enter held through the
  title is not an A in the main menu (checked: Enter held 6 s, the main
  menu stays; the next Enter opens New Game).
* The text is page `GameStart_Title.pag` (package cdd70a8c), text
  `PressStart` (400 x 41, centred, Titans_Small). Its English string "Press
  START" is 11 characters, too short to write "Press Any Button" over it in
  the text bank (the save messages' way, findings/33), so the front-end
  update sets the element's string while the front end is in state 55 (the
  title), only when it reads "Press START": other languages keep theirs.

## 3. Tests

Debug console: `mouse <x> <y>` (0..1 over the picture) moves the pointer,
`key LMB` / `key RMB` / `key WheelDown` click and scroll. On a copy of the
saves: main menu hover + click into Load Game, a save panel selected by
hover, the Back prompt clicked back to the main menu; main menu Options by
click, its tabs by clicking "Graphics" and the Q hint, rows highlighted under
the pointer; pause menu hover + click into Options, a right click back, the
wheel one step per notch; Quit Game's question: hover selects Yes / No, a
click on No closes it with the pause menu's selection untouched. After the
first playtest: the pointer beside the main menu's words selects nothing, on
them it does; a click on Options then an immediate move onto Load Game opens
Options; an Options row's value selects its row; Escape skips the boot movies
(title ~14 s after start). Second playtest: in-game Options -> Audio, the
names under the pointer select their rows; a star selects its row, Music's
4th star sets Music to 4, SFX's first star twice = 1 then muted.

Third playtest round (moving pointer after the click, as a hand does): main
menu Options click + 30 pointer moves over Load Game = Options; pause menu
Options click + moves over Tutorials = Options (after the 1 s slide); 6 clicks
at 100 ms + 8 at 80 ms on an Options row = 14 changes; quit question hover +
No; save list hover + the Back word.

The name screen's on-screen keyboard (its cursor is the screen's own, not a
Scrooby menu selection) and typing a name on the keyboard: findings/38 s.5.
