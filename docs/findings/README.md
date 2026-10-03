# Findings

Reverse-engineering notes about *Crash: Mind over Mutant* and the port,
one file per investigation, in the order they happened.

**About these notes:** like most of the project, they were written with an
AI coding assistant working with the maintainer (see
[Development approach](../../README.md#development-approach)).
Every conclusion was tested against the running game (logs, screenshots,
debugger sessions, measurements), and wrong turns are kept in the text
because they're part of how the answer was found. They have not been
reviewed by a human expert yet: corrections and reviews are very welcome
and will be credited.

**Places and names.** "The hub" is **Wumpa Island**, the game's hub
world, with Crash's house and, in its jungle, **N. Gin's lab**. "The
waterfall area" (name TBD) is the second area the game takes you to.
Things whose official name we don't know yet are marked **(name TBD)**:
search for that tag to replace them once the game's own names are found
(asset names, scripts).

| # | Topic |
|---|---|
| [01](01-disc-and-executable.md) | The disc, the archives and the executable |
| [02](02-recompilation.md) | Getting the recompiler to translate the whole game |
| [03](03-first-boot.md) | First boot |
| [04](04-loading-hang-xma.md) | The loading-screen hang: an audio decoder bug in the SDK |
| [05](05-post-intro-freeze-fibers.md) | The freeze after the intro movies: a fiber bug in the SDK |
| [06](06-black-screens-render-target-path.md) | Black movies and the vanishing title logo: the render-target path |
| [07](07-frame-rate.md) | The 30 fps cap, the 60 fps option and above 60 (work in progress); measuring frame rate (and the window's endless repaint) |
| [08](08-renderer-map.md) | How the game draws: Radical's renderer (PDDI) mapped, where the native renderer cuts in |
| [09](09-native-first-screens.md) | The first screens drawn natively: 2D geometry, the simple material, textures, gamma, movies |
| [10](10-native-3d-world.md) | The 3D world drawn natively: meshes, render targets and resolves, stencil shadows, fog |
| [11](11-native-characters.md) | Characters drawn natively: CPU-skinned geometry, the character and lit materials, a same-frame A/B tool |
| [12](12-native-shadows.md) | The characters' soft shadows: stencil volumes, a blurred stencil copy, and two old bugs (blend factors, front faces) |
| [13](13-native-reflections.md) | The reflect material (a sphere map, N. Gin's lab) and the underground material (the marker shown while Crash digs) |
| [14](14-native-water-particles.md) | Water and particles: soft particles, see-through water from mid-frame copies of the scene, textures read from D3D's sampler copy |
| [15](15-native-depth-of-field.md) | Depth of field: one full-screen pass, a 7-sample blur sized by the distance from the focal plane |
| [16](16-native-msaa.md) | MSAA: the game's 2x anti-aliasing, averaging resolves, depth copies of one sample by shader |
| [17](17-native-mipmaps.md) | Mipmaps: the Xbox's mip layout and packed mip tails, trilinear + 16x anisotropic filtering like the game's |
| [18](18-native-fx-grid-and-dual-mode.md) | The double mojo flash (fx grid material), both renderers side by side in two windows (dual mode), F10 frame traces |
| [19](19-missing-dialogue-5-1-xma.md) | A cutscene's missing dialogue: 5.1 XMA streams lost after their first block (SDK fix), a log of every sound the game asks for, and what our renderer costs per frame |
| [20](20-texture-write-watch.md) | Cheaper texture checks: page write-protection tells the renderer which textures changed (3.7 -> 1.2 ms per frame); the GPU-bound fights; the bump material found on screen (the TK blocks, see-through ice) and verified; debug tools (live console, F11/F12, draw lists); the Ratcicle's cyan flash: the simple material's proximity lights |
| [21](21-motion-blur-cut.md) | Motion blur: the engine has it and the game asks for it, but the Xbox 360 renderer's motion blur functions are empty; nothing to draw (a possible future option) |
| [22](22-ground-contact-high-fps.md) | Crash "falling" for a split second above 30 fps: the physics' ground contact (a sweep and a reach check), the fight tree (shipped as Lua in Radical's Lua 5.0 variant, run as compiled code), why 60 fps lets the move logic see drops 30 fps hid, and the fix (a 40 ms grace, a little more than one original frame); a ground trace |
| [23](23-keyboard-and-mouse.md) | Keyboard and mouse: what every 360 button does (the input map script, the prompt font's button pictures), Spin = a stick-circle gesture, our keyboard / mouse driver and the Controls menu (F6), which device plays as which player (local co-op) |
| [24](24-save-system.md) | Saves (research): the save file and its header, the save manager and its three-slot table, the one function that turns a slot into a file name, when the Load Game screen re-reads the list, the New Game name entry screen; then the save library built on it: one scrolling list of any number of saves, which file a load or save uses (the overwrite prompt's trap), the screen's look (cursor, centring, no fade, a preview in "Create New Save"), the prompts (Rename X, Delete Y), renaming with the game's own name screen by jumping through the front end's fight tree |
