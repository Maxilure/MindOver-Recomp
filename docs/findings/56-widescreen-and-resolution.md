# 56. Widescreen (any aspect ratio) and resolutions in pixels

The Xbox 360 game draws a 16:9 picture. With the native renderer drawing
alone (Renderer: Native Only), the port now draws a picture of any shape from
16:9 up to just over 32:9, "Hor+": a wider picture shows **more of the world
at the sides**, the top and bottom stay exactly the game's, and nothing is
stretched. Options -> Display -> **Aspect Ratio**: Match Window (the
window's shape), 16:9 (original), 21:9, 32:9. A window of another shape gets
black bars. Options -> Graphics -> **Resolution** is now a list of real sizes
at the shape in use ("2560 x 1080" at 21:9), plus Match Window (the window's
own size).

![16:9 and 32:9](../images/widescreen-16-9-vs-32-9.jpg)

*The same moment at 16:9 and 32:9: the middle is the same picture, the sides
are new.*

Code: `src/native/widescreen.*` (the shape, the display, the camera, the
projection), `src/native/render_targets.*` + `native_renderer.cpp` (wider
images), `src/ui/hud_scale.cpp` (HUD placement), `shaders/bink.vert`
(movies). Flags: `aspect_ratio` (0 window, 1 16:9, 2 21:9, 3 32:9),
`widescreen_max_aspect` (3.6), `widescreen_hud` (0 in place, 1 screen edges),
`render_resolution` (a height in pixels, -1 = the window's), and for tests
`debug_widescreen_aspect` (pretend the window has that shape).

## 1. Where the game gets its screen shape

The game asks its display object (`xnDisplay`, pointer at `0x82571B78`):

| Field | 16:9 value | Read by |
|---|---|---|
| +1640 / +1644 (ints) | 1280 / 720 | surfaces, viewports, tiling strips (left alone) |
| +1680 / +1684 | 1 / 0.5625 | slot 0x54 (their ratio): the views (`sub_82379B20`, `sub_82379D40`) |
| +1688 / +1692 | 1 / 0.5625 | slot 0x50 (their ratio): the default projection, the menus' projection, the camera's aspect |
| struct at +1672 | | slot 0x48: the menus' canvas scale `min(+1680 / 640, +1684 / 480) / +1688` (`sub_8237BBE8`), the canvas <-> screen conversions (s.4) |

Found by logging every caller of the getters for a few seconds of play, then
by answering them with a 21:9 aspect on a 16:9 picture (the menus and the
world both changed shape).

For a picture of aspect A the port writes +1680 = (16/9) / A, +1684 = 1 / A,
+1688 = 1, +1692 = 1 / A. The menus' projection (aspect A) and their canvas
scale (1 / (480 A)) then cancel out: every menu page keeps its size and stays
centred, and the extra room is at the sides. (A first try kept +1684 at
0.5625 and widened +1680: the menus came out 1.33 times too big at 21:9.)

## 2. The camera: wider, but the game keeps thinking in 16:9

The projection is set through **pddiBaseContext::v29** (`sub_8243E4B8`: near,
far, HORIZONTAL field of view, aspect; rebuilt by xnContext slot 122
`sub_824319A8`, perspective case: x extent = near x tan(fov / 2), y extent =
x extent / aspect). A handler on it widens only the call from the camera's
view set-up (return address `0x82381BAC`): the field of view becomes
2 atan(tan(fov / 2) x A / (16/9)), the aspect A. The vertical view stays the
game's. The menus' calls and the context's restores (which re-send what was
stored) are left alone.

Writing the camera's own fields (+16 field of view, +28 aspect) before its
matrix rebuild did nothing: the camera's set-up writes them again. That set-up
is **Camera slot 0x14, `sub_82381E78`** (read with xexdis; +28 found with the
debug console's `who`):

* +16 / +20 field of view, +24 the viewport's shape;
* +28 aspect = +212 x +24 x display slot 0x50 (the wide one);
* +40 = 1 / tan(fov / 2), +44 = +28 x +40 (projection scale factors);
* +48..+76 the **view frustum's side planes**, what the game culls with.

The port's wrapper hands it the wide field of view, so the frustum covers the
whole wide picture (with the 16:9 one, scenery at the sides was only drawn
once the 16:9 view reached it: Wumpa Island at 32:9 went from 281 to 497
draws). Then it writes the game's 16:9 view back into +16, +20, +28, +40 and
+44 (forgetting +20 put the camera far back). So everything the game itself
computes with its camera (aiming, positions on screen, markers, the co-op
camera's fit) sees the middle 16:9 of the picture, exactly what the original
shows there.

## 3. The native renderer: one more scale, sideways

The render scale (findings/45) is split: widths and x positions are scaled by
scale x A / (16/9), heights and y by scale. Surfaces, resolve rectangles and
destinations, viewports and clear rectangles follow, and the presenter gets
the picture's real shape (it letterboxes it into the window). The game still
works in its 1280 x 720 pixels.

**Resolution** is now the picture's height in pixels (360-4320, -1 = the
window's; the old values 0-3 still mean 720p, 1080p, 1440p, 4K):
scale = height / 720.

## 4. What else had to follow

* **Canvas <-> screen.** `sub_82265D40` (a 640 x 480 canvas point -> a 0..1
  screen point) and `sub_82265DE8` (back; used by the players' markers,
  `sub_8226B2C8`) read display slot 0x48. They feed the game's 16:9 camera, so
  a handler on that getter (`sub_82433960`) answers them a copy with the
  16:9 numbers. Without it, the markers sat at the screen's edge although the
  players were on screen, and the mojo cursor aimed off.
* **Movies.** The movie player fits its quad to the display's shape: at
  32:9 it already sits in the middle 16:9 sideways (x 320..960 of 1280) but is
  shrunk in height by the same factor (y 180..540). `bink.vert` scales the
  height back up around the centre: full height, bars at the sides.
* **Cutscene bars.** Page `InGame_LetterBox` (two black quads, x -110..750 =
  exactly the 16:9 screen) is drawn with one more matrix that stretches x by
  A / (16/9) (`hud_scale` kStretchWide).
* **Fades and dimmers.** A flat, untextured 2D quad of up to 6 vertices that
  covers the whole 16:9 screen (a fade to black: 6 vertices, x 318..962 of
  1280 at 32:9) gets its x stretched the same way in the renderer
  (`CoversGameScreen`).
* **HUD.** "In Place" (default) keeps it where the 16:9 game has it; "Screen
  Edges" moves the corner pieces sideways to the wide corners (hud_scale,
  output-space shift). HUD Size's anchors follow the picture's height
  (0.5 / A) and the 16:9 corners.
* **A collected mojo's flight.** The game aims 4 units in front of the camera
  at the ORIGINAL portrait's place (findings/26 s.40). Midasm hooks at
  `0x82198ED8` / `0x82198F6C` scale the aim's side and up parts by the
  portrait's move (HUD Size, Screen Edges).
* **Sliding menus** (`ui/slide_fade.*`). The pause menu (and other screens)
  slide a whole page or layer sideways through its TRANSFORM every frame
  (`CPauseScreenAction`'s step `sub_820DA120` -> `sub_82370E98(element, x,
  0)` on the glass layer +32 and the menu page +20, x from the interpolator
  at +52; out to about 850 canvas units). On a 16:9 screen the panel leaves
  the screen before the slide ends; at 32:9 it stayed in view at the side
  and vanished at once. Now a Parent element (page, group, layer: slot 7 =
  `sub_823711A8`) whose canvas centre has moved more than 120 units fades
  out (smoothstep) and is fully see-through at 426.7 units, the 16:9
  screen's half width: the panel's centre at the original screen's edge.
  How: element +16 = its 4 x 4 matrix (row vectors; translation at +64, in
  canvas widths, written by `sub_824239E0` / `sub_82370E98`), +110 bit 0x40
  = the matrix is in use. The colour an element draws with is CACHED at
  +84: its own +80 times its parent's, worked out by the parent's draw
  (`sub_82371270`) only while +84 holds the sentinel `[0x824EA190]`. Slot 7
  stores +84 and resets every child to the sentinel. So the fade = slot 7
  with the faded colour before the element's draw, the old +84 after.
  (Lowering +80 at draw time came one step late: a slid-out panel stayed
  faintly visible.) Only on a picture wider than 16:9; flag
  `menu_slide_fade`.
* **Pointer maths.** The mouse in menus, the Wii-style cursor and the FPS
  counter use `widescreen::Game()` (the middle 16:9 of the picture) or
  `Picture()`.

## 5. Not done

* The main menu's frame (a 16:9 page) at the sides.
* Players' markers for a partner standing in the wide sides (the game's
  "off screen" test is its 16:9 view).
* The emulated picture stays 16:9 (the rows are greyed out without Native
  Only).
* Windows: untested.

## 6. Tested

Console-driven runs on an installed-style test copy at 16:9, 21:9 and 32:9
(`debug_widescreen_aspect` and the fixed choices): Crash's house, Wumpa
Island, N. Gin's lab cutscene, boot and story movies, a level's fade-in,
the pause menu and Options (rows, sizes updating live), two players' markers,
spawned mojo, all resolutions (2560 x 1080, 5120 x 1440, Match Window).
Sliding menus at 32:9: the pause menu opening and closing photographed
frame by frame (gone at the sides, solid at rest), pause -> Options
unchanged, 0 errors.
0 errors.
