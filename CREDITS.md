# Credits

Everyone and everything that went into **Mind over Recomp**, with what each
contributed. The short version is in the [README](README.md#credits).

New contributors are added here by name, with what they did: code, reverse
engineering, testing, bug reports, corrections, art. See
[Contributing](README.md#contributing).

## People

### Maxilure: project lead

- Started the project, sets its direction and suggested many of its
  features (dual mode, the save library, four-player co-op, the launcher).
- Playtests the port on Linux and Windows and reports bugs.
- Drew the "3" and "4" marker pictures for players 3 and 4
  (`assets/markers/`), traced after the save-slot numbers of
  *Crash of the Titans*.

### Claude (Anthropic): AI assistant

- Wrote most of the code and the documentation (via Claude Code). This
  includes the recompilation fixes, SDK patches, native renderer, input,
  co-op, save system, launcher and debugging tools.
- Did the reverse engineering written up in [docs/findings](docs/findings/).
  See [Development approach](README.md#development-approach): this work has
  not been reviewed by domain experts.

## Projects this port is built on

- **[ReXGlue](https://github.com/rexglue/rexglue-sdk)**: the static
  recompiler and runtime (kernel, fibers, GPU emulation, audio, input) that
  make the port possible. The port's fixes to it are in
  `patches/rexglue-sdk/`.
- **[Xenia](https://github.com/xenia-project/xenia)**: the Xbox 360 emulator
  that ReXGlue's runtime and GPU emulation build on.

### Libraries used directly by the port's own code

- **[SDL3](https://github.com/libsdl-org/SDL)**: windows, input and dialogs
  (game and launcher).
- **[Dear ImGui](https://github.com/ocornut/imgui)**: the in-game menus
  (controls, cheats) and the launcher.
- **[{fmt}](https://github.com/fmtlib/fmt)**: text formatting.
- **[toml++](https://github.com/marzer/tomlplusplus)**: settings and controls
  files.
- **[stb_image](https://github.com/nothings/stb)**: loading the marker
  pictures.
- **[Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator)**:
  GPU memory for the native renderer.
- **Vulkan** and **glslc** (Vulkan SDK): the native renderer and its
  shaders.

The SDK brings more libraries of its own. They are listed in
`thirdparty/rexglue-sdk`.

## The game

*Crash: Mind over Mutant* was made by **Radical Entertainment** and
published by **Sierra Entertainment** / **Activision** (2008). This port
contains none of the game's code or data: it is built on each player's
computer from their own disc. See the [Disclaimer](README.md#disclaimer).

## References

- The **Crash Bandicoot fan wiki**: names of places, enemies and titans used
  in the documentation.
- ***Crash of the Titans*** (Radical Entertainment, 2007): its save-slot
  numbers were the model for the player 3 and 4 markers.
