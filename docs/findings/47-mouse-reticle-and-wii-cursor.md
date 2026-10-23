# 47. The aiming reticle on the mouse, and the Wii version's gameplay cursor

A player riding as a mask (Aku Aku) can aim: a reticle on screen, where the
mask's projectiles fly. The Xbox 360 version moves it with the left stick,
so a keyboard player moves it with WASD. The Wii version has a pointer
cursor instead: it follows the remote, leaves a trail when it moves fast and
grows over things that can be broken. This page covers what the 360 build
still holds of that cursor, and how the reticle now follows the mouse.

## 1. The reticle

Each player's reticle is a `CReticleController` (vtable `0x820396D4`, one
slot) inside the front end, at `+8420 + 52 x player` (players 3-4's are the
port's own objects, findings/26 s.19).

| Offset | What |
|---|---|
| `+4` | the shown position: a two-axis smoother, `0x14` bytes per axis, current value at `+0x10` of each (x at `+0x14`, y at `+0x28` of the reticle) |
| `+44` | its player, 0-3 |
| `+48` bit `0x80` | on: a masked player aims (set by `sub_8213FF18`, cleared by `sub_8213FFA8`) |

The constructor (`sub_822550D0`) starts it at (320, 240), the middle of the
640 x 480 menu canvas (y up), drawn 4:3 in the middle of the 16:9 picture.

Its Update (`sub_82255160`, f1 = frame time), while the reticle is on and
the game isn't paused (game object `+168` bit `0x20`):

1. reads the player's stick (`sub_82271D20`: a direction and its strength);
2. above a dead zone, adds direction x strength x time x a speed constant
   (`0x820471CC`) to the target; y is flipped when the player's Invert Axis is
   on (front end `+8588 + player`);
3. clamps the target to the picture (`sub_82255360`: x from
   `-(width - 640) / 2` to `640 + (width - 640) / 2`, the canvas width at 16:9
   being 853.3, so -106.7 to 746.7; y 0 to 480);
4. eases the shown position toward it (`sub_822EAA48`).

Without a controller for its player it snaps back to its start
(`sub_822EAA18`, which sets both the target and the shown value of each axis).

## 2. Following the mouse

`input/mouse_reticle.cpp` wraps that Update. The game's Update runs first,
unchanged; then, for the player the keyboard plays as, with the reticle on
the game not paused and none of the game's menus on screen, the pointer's spot on the picture (0..1) becomes a
canvas position by the same mapping the menu mouse uses
(x = (1280 px - 160) / 1.5, y = 480 (1 - py)), goes through the game's own
clamp (`sub_82255360`), and is set with `sub_822EAA18`: the reticle sits on
the pointer, without easing. Pads and the other players are untouched.

Setting `mouse_reticle`: 0 = WASD as in the original, 1 = the mouse
(default), 2 = the mouse plus the gameplay cursor (section 4).

Checked through the debug console with the reticle switched on by hand
(`write ... +48 u8 0x80`, then `mouse x y`): pointer (0.25, 0.25) -> reticle
(106.7, 360); (0.9, 0.8) -> (661.3, 96); the bottom-left corner -> (-106.5, 0),
the game's clamp.

## 3. The Wii cursor in the 360 build

The class is still there: `CGameplayCursorBehaviour` (vtable `0x8202DCEC`,
18 slots, script constructor `sub_821404B0`, one script method taking a
float).

- Set-up (slot 2, `sub_8213FEA0` -> `sub_82140178`): builds a scene object
  (`sub_82251A60`) named by the key of `"MOJO_VAC"` (64-bit name key,
  `sub_8236ACB8`: h = h x 65599 xor c), scale from `+48`, and registers it.
- Update (slot 4, `0x82140038`): for its player (the game's player actors at
  game object `+16` / `+20`), when that player's reticle is NOT on
  (`sub_822552A8`), casts a ray through the reticle's screen spot
  (`sub_822EAA98` + `sub_8224C6C0`), places the object there and turns the
  reticle's `+48` bit on. On the Wii the pointer spot is the reticle spot.
- Slots 15 / 16 (`sub_8213FF18` / `sub_8213FFA8`) turn the player's reticle
  on and off.

The scripts create it only on the Wii: every `m_CursorBehaviour` in the
level scripts sits behind a `RAD_WII` check. On the 360 nothing creates one.

Its LOOK is not in the 360 data: neither `MOJO_VAC` nor its name key
(`0xFA39FF699F5565B2`) appears in any of the 360 archives. The reticle
textures that are there (`HUD_Reticule_Bullet`, `_Cross`, `_Tattoo`) are
pieces of the 360's own aiming reticle. A round bubble texture,
`bubble01.bmp` (32 x 32, PNG, no alpha: drawn additively), is in
`default.rcf` and is a candidate for the port's version of the cursor.

## 4. The port's gameplay cursor

`ui/wii_cursor.cpp`: while a keyboard plays, the pointer is a bubble in every
menu (the front end, and in a level any of the game's menus on screen),
whatever the setting, and in play too with `mouse_reticle = 2`.
`bubble01.bmp` (read from the player's disc at run time,
alpha = its brightness, tinted cyan) sits on the pointer in place of the
system pointer, 28 pixels across at 720p, drawn in layers: a soft cyan halo
(`glow01.bmp`, same package, 2.6x), a dark blue outline (the bubble at 1.22x,
twice: without it the light bubble vanished over the menus' light glass),
the bubble in cyan, and again in near-white for its rim. In motion it STRETCHES along its
direction (length x (1 + speed / 2500 px/s), at most 1.8, width / sqrt of
that: about the same area), from a velocity smoothed over ~0.05 s, so it
relaxes back to round when the mouse stops. The stretch is a pure (symmetric) scale of the upright square: the picture never turns. Only very fast flicks (over 1600
pixels a second) leave a short, faint trail of smaller copies (0.1 s). It is drawn over the finished picture, like the
frame-rate counter, so it follows the pointer with no frame of delay.

When it shows is decided once per game frame: a socket the keyboard plays
on; in a menu always, in play only in crosshair mode; not while the
keyboard's player is a mask in a level (they aim: the reticle shows, the Wii
version also hid its cursor then), nor while one of the port's own ImGui
windows is open (F5, F6, F2, the SDK's F4 / console).

The SYSTEM pointer follows one rule, also decided per game frame
(`KeyboardMouseDriver::SetPointerRule`), in this order:

1. one of our interactive ImGui windows is open (hovered or not): **shown**.
   A window counts if it was drawn last frame or this one and takes mouse
   input; banners, toasts and the build stamp all carry `NoInputs`, and
   ImGui's fallback window is skipped;
2. the cursor or the reticle stands in for it, or a level is played with
   none of the game's menus on screen: **hidden** (whatever Mouse Aiming
   says);
3. otherwise (the game's menus while only pads play): the SDK's auto-hide.

The first version only forced it visible for F6 and only hid it while
standing in: over F5 the pointer stayed hidden by the bubble, and the SDK's
auto-hide (it hides right away and reveals on the next mouse move) brought it
back in play with Mouse Aiming Off at every move. The log line
`Mouse pointer: shown / hidden / auto-hidden` marks each change. Checked
(Mouse Aiming Off, keyboard player): play = hidden, F5 open = shown, closed =
hidden, pause menu = hidden (bubble), F5 over it = shown, crosshair = hidden.

## 5. The game's own cursor, switched on

Every Crash on the 360 is built with a `CGameplayCursorBehaviour`: players
1-4 alike, constructed and set up (slot 2, `sub_8213FEA0`: r3 the behaviour,
r4 its Crash) with each level. Crash's template lists `m_CursorBehaviour` with
a parameter `collector_volume_min_size_number`. Its Update never runs: the
scripts call `m_CursorBehaviour:SetDesiredActive` only under `RAD_WII`
(entering / leaving the mask, the ground state, blocking...).

Switching a behaviour on: byte `+24` holds bit `0x40` = active and `0x20` =
desired. `SetDesiredActive` (`sub_82102BB0`, registered at `0x82103050`) only
sets the wish; `sub_82102FA0` compares the two bits and calls
`sub_82102F28(behaviour, on)`, which runs slot 15 (on) or 16 (off) and sets
both bits. Setting the wish alone did nothing (checked: the Update never ran);
the port calls `sub_82102F28` itself, once per frame from the reticle's
Update wrapper, for the keyboard player's Crash on foot in crosshair mode, and
switches it back off otherwise (only ones it switched on).

Switched on, the cursor's slot 15 registers its collector object (`+56`) with
the game object's `+48` and turns the player's reticle on (`+48` bit `0x80`);
the reticle then follows the mouse (section 2). Its Update casts a ray through
the reticle's spot and places its `"MOJO_VAC"` collector where it lands: the
Wii pointer's mojo vacuum. (The object has no look on the 360 disc.)

The ray (`sub_8224C6C0`: r3 the game's `+48`, r4 the screen spot, r5 a query,
r6 the result; the test itself `sub_8224C7F8`): query `+0` a list of actors
to ignore, its end at `+0x10`, `+0x14` a target-type mask (`sub_8216EB88`:
types 0-6 as bits, 8 = all), `+0x1C` flags (`0x80` = the scenery, `0x20` =
TARGETABLES: the nearest in front of the scenery wins), `+0x20` the reach.
Result `+0x1C` = the targetable hit. The cursor asks `0x90` with mask 0:
scenery only, never an object. The aiming reticle asks `0xA0` with its mask.

Growing over targets: right after the cursor's ray (return address
`0x82140124`), a second ray from the same spot with a copy of its query,
flags `| 0xA0`, mask all, the keyboard player's own Crash ignored. A target in
its result = the bubble grows to 1.5x (eased over ~0.06 s, back the same).
Checked with a Roller titan spawned in view (F5 cheats): the pointer over it
reports it, anywhere else (Crash included) nothing.
