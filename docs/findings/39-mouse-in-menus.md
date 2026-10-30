# 39. The mouse in the game's menus

2026-10-15. Roadmap phase 2: point and click in every menu.

Short version: the game's menus are Scrooby menus whose items each have a
"look" (the text drawn for the item) with a box in the 640 x 480 canvas
([findings/30](30-menus-and-hud.md)). The port maps the mouse pointer from
the window into that canvas and tests it against the boxes: the item under a
moving pointer is selected (with the game's move sound), a left click on it
chooses it (an A press), a right click goes back (B) and the wheel scrolls
where there is something to scroll (section 5; it used to be a D-pad step in
every menu). The prompt bar, the Options tabs, the save list, the game's
question boxes, Crash's house's picture screens and the credits answer to the
mouse too. The presses go into
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
  Except the Options screen's long tabs: there the wheel scrolls the list
  itself, one row per notch, without moving the selection, and stops at the
  ends ([findings/31](31-options-screen.md) s.3: the sliding list).
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

## 3. Movie cutscenes: any button brings up "Skip"

`input/movie_skip.*`. The cartoon cutscenes played inside a level (for
example the one after talking to Coco on Wumpa Island) show a "Skip" prompt with
the A button in the lower right corner (page `InGame_MovieSkip.pag` in
`InGame.prj`). On the Xbox only START brings it up, and on the keyboard
nothing did: during the movie the front end reports a menu on screen
(+8594 bit 0x80), so the keyboard is in its menu context, where Escape is
Back (B) and Enter / Space are A. Neither is START, and A does nothing while
the prompt is hidden. Escape still skipped the boot movies because outside
play the game takes A or START directly.

How the game does it: the movie runs in `CMoviePlayerBehaviour` (vtable
0x820334E4), whose Update (slot 4, `sub_82187EF0`) asks the input events
"just pressed" (`sub_82274BE8(game +56, 2, event, 1)`: event 36 = START,
12 = A). Its `CMoviePlayer` (behaviour +36, vtable 0x82041C4C) keeps the
prompt's state in byte +296 bit 0x20 (read by `sub_822C49F0`, set by
`sub_822C49D8`); the player's update shows the skip page while it is set.

| game state | prompt | START | A |
|---|---|---|---|
| 5 (a level) | hidden | shows it, 2 s timer (behaviour +52 = 2.0 from 0x8201FB74) | nothing |
| 5 | shown | hides it | stops the movie (player slot 6) + the front end's sound |
| other (boot) | none | stops the movie | stops the movie |

The port wraps the Update only to watch it: after each call it notes whether
a movie plays in a level with the prompt hidden. While it does, the title
screen's any-button filter (section 2) does the same job here: a new press of
any controller button, trigger, key or mouse button becomes a 150 ms START,
and the other buttons are hidden. A button still held when the prompt appears
stays hidden until it is let go, so the press that brought the prompt up
never skips by itself. With the prompt shown, the input is untouched: the
game's own A skips, and START hides the prompt again.

One side effect had to be handled: the button prompts pick keyboard or
controller pictures by which device was used last, and the START made from
a key press looked like a controller press. The filter now reports the
buttons it adds for a key, and the prompt pictures ignore them. After a key
the prompt shows the keyboard's menu key, and after a controller button it
shows A.

Tests (debug console + input FIFO, Wumpa Island save, the Coco cutscene,
keyboard on player 1): Escape -> prompt with the key picture, hidden again
after 2 s; a controller A while hidden -> prompt with the A picture, the movie
keeps playing; Enter -> prompt (no skip), Enter again -> skipped (front end
492 -> 494). Before the change, Escape and Enter showed nothing and a
controller X did nothing, while START showed it. Title screen: any key still
goes to the main menu.

## 4. Tests

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

## 5. Crash's house's picture screens, the wheel, the credits

2026-10-29. Three gaps: the screens Crash's house opens took no mouse at
all, the wheel stepped through every menu (in a five-item menu it only moved
the cursor by surprise), and the credits could not be scrolled.

### 5.1 What the wheel does

`input/mouse_menus.h` (WHAT THE WHEEL DOES). A screen CLAIMS the wheel
every update while it is up (`ClaimWheel`, a claim lasts 300 ms, so it ends
by itself when the screen closes), and the keyboard driver sends each notch
where the claim says:

| claim | screens | a notch |
|---|---|---|
| list | Options (its own `WheelScrollsList`), the save list, the credits | the screen slides its list; the selection stays |
| left / right | the TV, the comic, the tiki face, the chest | D-pad left (up) / right (down) |
| none | every other menu (main menu, pause menu, difficulty, question boxes...) | nothing |

The save list now slides like the Options screen's long tabs: one save a
notch, the cursor kept on its save while that save is still in one of the
three panels, else on the nearest panel; nothing at the ends.

### 5.2 Crash's house's screens

`input/mouse_screens.*`. They are front-end states with a page each in
GameStart.prj, not Scrooby menus: one picture between a left and a right
arrow (`LeftArrow` / `RightArrow` elements):

| object in the house | front-end state | page | picture |
|---|---|---|---|
| TV | 988 MovieSelectionScreen | GameStart_MovieSelection | MovieImageSprite (17 movies) |
| comic (front table) | 865 ConceptArtScreen | GameStart_ConceptArt | MenuSprite (12 art packs) |
| tiki face | 860 EnemyInfoScreen | GameStart_EnemyInfo | portraits in the top row |
| chest | 847 SkinSelectionScreen | (the port's outfit picker) | (its own mouse) |

Their input states read IsButtonPressed (`sub_822652B0`) buttons 27 / 40
(left / right), 1 (A) and 12 (B) every frame (CMovieScreenAction: state
+76 == 3, `sub_820D4CC0`; CEnemyInfoScreenAction: +152 == 3,
`sub_820C91F8`, selected enemy at +156). So the mouse goes in as presses:
a click on an arrow = D-pad left / right, a click on the TV's or the comic's
picture = A (the TV plays that movie, the comic shows the art full screen).
The arrows the game hides at the ends never match.

The tiki face's portraits: its 20-entry table at 0x8259FCA8 (84 bytes per
enemy, the portrait picture at +76) puts the ones from s - 1 to s + 2 in the
top row (`sub_820C9440`), the selected one at alpha 255 (the top byte of
element +80), the others at 128. A click on a portrait steps toward it ONE
press at a time: after each step the screen loads that enemy and drops
presses meanwhile (two presses at once moved one place).

The tiki face's other A: each enemy has a sound (its battle cry), played by
A on this screen (`0x820C92D4`). The enemy itself is a 3D model, not a page
element, so a click inside a fixed canvas box right of the description panel
(x 345-575, y 40-295, below the portrait row) is that A. Tested: a click on
the Ratnician = `battlecry_labrat1_01.wav` in the audio trace.

Debug console: `interactables` lists every usable thing seen so far with its
position (CInteractableBehaviour, object +44 = world matrix). In a new game
in the house: front door (11.52, 0, -0.21), comic table (3.75, 0, -6.0), tiki
face (3.33, 0, 1.8), chest (7.36, 0, 2.01), TV (10.87, 0, -3.87), and one at
(9, 5.5, -1) (name TBD). The comic opens when Crash walks up to its table
(a teleport onto the spot didn't).

### 5.3 The credits

`ui/credits_scroll.*`. The wheel scrolls them both ways like a list: a notch
down = on toward the end (the names go up), up = back; one line a notch,
glided over about 0.2 s (each frame moves 15 x frame time of what is left,
at least 2 lines a second). The first version jumped 3 lines a notch the
other way round (a notch down = names down, like dragging a page); in play
that felt jerky, too big and inverted (changed 2026-10-30);
Up / Down (D-pad, stick, arrow keys, W / S) set the speed in 0.1 steps, the
way one moves through a list: Down rolls the names on toward the end
(faster), Up slows them, stops (0) and goes on into rolling backwards (to
-1), which stops by itself at the start. The Xbox had it the other way
round (Up = faster, Down = slower, never backwards); the first version kept
the Xbox's direction, which made Up head toward the end and Down back, the
reverse of the wheel's list feel (fixed 2026-10-30; measured with key taps:
W 0.2 -> 0.1 -> 0.0 -> -0.1, S back up to 0.2).

How the game rolls them (CCreditScreenAction, vtable 0x8202248C, Update
`sub_820C6330`): 30 text elements (this +84, 8 bytes each: element, line
index) roll up by step = speed (+324) x dt x 500 / +332 per frame through the
phase function (+344: 1 `sub_820C6EB8` rolling, 2 `sub_820C7260` the end
elements come up, 3 `sub_820C7340` the last card, "Thanks For Playing!",
held in the middle). An element past the top is RECYCLED at the bottom with
the next line (+336 the last line handed out, +328 where the next goes; +328
moves with the text). The lines (from +28 to +32, 272 bytes each): +0 a
text-bank key, +4 the text, +260 heading, +261 big heading, +262 a line with
a voice clip (A plays it; +348 bit 0x80 while it plays, the roll waits),
+268 text by key. Button 45 (up) and 16 (down) change the speed.

A recycled line is gone, so going back works by CHECKPOINTS: every 2 line
spacings of distance the port keeps the rolling state (the 30 elements'
places, lines and visibility; +328 to +348; the end elements +44..+72), and
back by d = restore the last checkpoint before the target, put each
element's text back by the game's own rule (0x820C6F8C: the text at +4, or
by key for headings / +268), then roll forward to the target with the
game's own phase functions in steps of a third of a line. The game's Update
runs with dt = 0 (its own step 0; f2 + 30 dt keeps its clock +124 the same)
and the speed set to 0.5, so its Up / Down show as 0.6 / 0.4 afterwards; the
port then moves the text by its own signed speed. The roll stops counting 20
spacings into phase 3 (it goes on stepping while showing nothing new, and a
notch back would still be the same card).

VOICE LINES BY MOUSE. Some people's lines carry a voice clip (+262): the
game grows the one passing the middle of the screen (1.9x, a window in the
phase function at `0x820C712C`), shows a "Play Sound" prompt, and A plays it
(+340 = that line; `sub_820C71F0` = are clips on, by language). The port lets
the pointer pick ANY voice line on screen: after the frame's phase calls
(which reset every element's scale, `0x82370A58`) the voice line under the
pointer is grown the same way (`sub_82370FD0`, 1.9) and written to +340, and a
left click on it is an A. Tested: pointer parked at 78% height, the names
under it grow while the middle one grows too; clicks played
`tschultz_ngin.wav` and `bchurchill_crunch_tiny.wav` (audio trace). A click
while a clip plays is ignored by the game (+348), as its A is.

Tests (debug console, main menu -> Credits, 623 lines, speed 0.2 at the
start): stopped, 4 notches forward and 4 back = the same frame (mean
difference 4.5 / 255: only the Back prompt's picture changed, pad -> key);
-0.2 rolls back (earlier names come down); 30 notches up = the empty start;
400 notches down = the end card, then 40 up = back through phase 2 to 1
and forward again to phase 2 / 3 at the same distance (837 / 879 spacings,
first pass 837 / 880). Before the end elements were in the checkpoints, the
second pass jumped to phase 3 at once.

### 5.4 Tests

Debug console on a copy of the saves, New Game into Crash's house (keyboard
on player 1): TV: right arrow 1 -> 2, wheel down -> 3, two notches up -> 1,
a click on the picture plays the movie (its skip prompt answers); comic:
right arrow 1 -> 2, wheel -> 3, a click on the picture = the art full
screen; tiki face: the third portrait clicked = two steps (Ratnician ->
Ratcicle); chest: the wheel steps the colour. Pause menu: two notches move
nothing. Load Game list (55 saves): the cursor on save 2, one notch = window
at save 2 with the cursor still on it, two more = it scrolled away and the
cursor is on the nearest panel, one back = cursor on the same save one panel
down. 0 errors in the logs.

## 6. The minigame level picker

A minigame spot in play opens a panel: the minigame's name and goal on the
left, "Level 1" .. "Level 10" on the right (front-end state 970
`MiniGameScreen`). The keys worked there from the start, the mouse didn't:
the shared mouse code hooks `CMenuAction`, and this screen is its own class,
`CMiniGameScreenAction` (vtable 0x82023CE4), which owns a Scrooby menu
without being a `CMenuAction`.

From its slots (xexdis):

| Slot | Function | What it does |
|---|---|---|
| 4 Enter | sub_820D3660 | page found by name -> +20; the level menu found on it (sub_82371988) -> +24; each of the 10 items made selectable only up to the levels won (count from the save's stats) |
| 5 Update | sub_820D3A98 | steps the screen by its state at +56 (2 opening, 3 open, 4/5 closing) |
| 6 Exit | sub_820D3B38 | |

`src/input/mouse_minigame.cpp` wraps Update and hands menu +24 and page +20
to `mouse_menus::HoverAndClick` before the original runs: hover selects,
a left click selects and presses A, locked levels (not selectable) are
skipped. Right click and the prompt bar's Back / Select were already
covered by the shared front-end code. The wheel does nothing here (ten
levels fit, nothing scrolls). Log line once per visit: "Mouse in menus: the
minigame level list".

## 7. The window loses the focus: controllers play on, or the game pauses

Status: built (`input/background_input.*`), setting `background_input`,
Options -> Controls "Window Not Focused" (Keep Playing / Pause).

**Before.** With no game window focused, the SDK's input check
(`ReXApp::ConstructRuntime`'s "active" callback) gave the game a neutral
controller. The game kept running with nobody at the controls.

**Keep Playing (the default).** Two things blocked a controller in the
background:

1. the SDK's active check. The port's own check (already used for the second
   window) is now installed at start-up for every driver. Unfocused, it
   answers with the setting.
2. SDL itself drops joystick and gamepad events while none of its windows
   has the keyboard focus (`SDL_PrivateJoystickShouldIgnoreEvent`), unless
   `SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS` is "1". SDL watches that hint
   with a callback, so changing the row takes effect at once.

The keyboard and mouse still stop while unfocused (their presses go to the
other window).

**Pause.** The front end's in-game decision (`sub_8211E850`, run every frame
in the GameRunning state, 489) is wrapped. Its exits, read from the code:

| exit | taken when |
|---|---|
| 502 | IsButtonPressed button 32 (Back) at `0x8211E9D0`, or the map flag (+8592 bit 0x20): the MAP screen (state 927) |
| 516 | button 36 (START) at `0x8211EAF4`, only if the game isn't paused (game +168 bit 0x20) and `sub_82265A60` says "no" (every player's controller locked, as in cutscenes): the PAUSE MENU (state 868) |
| -1 | nothing happened |

When the focus is lost and the original answers -1, the wrapper makes the
same two checks START's branch makes and returns 516. No press is faked. A
first try returned 502 and opened the map. The pause also waits for the
level's fade-in (front end +144 back to 0): the first version paused 0.2 s
into a level, over a black picture. It pauses once per focus loss. A focus
lost in menus or while loading pauses at the level's first visible frame,
unless the focus is back by then. The pause menu's owner is whoever last
pressed START (`StoreWhoPausedGame`), or nobody (anyone answers) before the
first START.

Tested with the debug console on a test copy, window unfocused: setting off =
pause menu 1.1 s after the level started (exit 516, state 868); B = back in
play, no second pause; a scripted START also takes exit 516. Setting on = no
pause. A real controller in the background is not tested yet.
