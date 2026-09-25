# Findings: how the game draws, mapped (native renderer, milestone 1)

Session of 2026-09-25 (sixth). Roadmap phase 4, milestone 1 of
[04-native-renderer.md](../04-native-renderer.md): "map PDDI and the D3D calls
per frame, decide where to cut in". **Result:** every GPU call the game makes
goes through one small, named layer, the Xbox 360 backend of Radical's
renderer (classes `xnContext`, `xnTexture`, `xnShader`...). Nothing else in
the game talks to Microsoft's D3D. That layer is where the native renderer
should cut in (recommendation at the end).

New tools, all reusable:

| Tool | What it does |
|---|---|
| `tools/rtti_vtables.py` | C++ class names → vtables → every virtual method's address; marks what a class overrides |
| `tools/callgraph.py` | who calls whom (direct calls), for all ~29,000 functions, in half a second |
| `tools/gen_pddi_table.py` | writes `src/pddi/functions.inc`: the renderer functions to intercept, with the names we know |
| `--debug_pddi_trace_dir` (`src/pddi/trace.cpp`) | logs every renderer and D3D call of chosen frames, in order, with arguments, the object's real class and the caller |

(File names as of the end of the session: the wrappers first lived in the
tracer, then moved to `src/pddi/intercept.cpp` so the native renderer can
use them too.)

## 1. The class names are still in the exe

The game was compiled with RTTI on (C++ run-time type information), so
`default.xex` still names 2,469 classes, e.g. `.?AVxnContext@pure3d@@`
(class `pure3d::xnContext`). From a name, MSVC's RTTI structures lead to the
class's **vtable**, the table of its virtual methods (layout explained in
the header of `tools/rtti_vtables.py`). The renderer comes in three layers:

```
game code, pure3d scene graph (Drawable, PrimGroup, Shader, Texture...)
   │  virtual calls through pddiRenderContext*, pddiTexture*, ...
   ▼
pddi*   the engine's abstract renderer interface ("Pure3D Device Interface")
        + pddiBaseContext: generic parts (render-state stacks, matrices)
   │
   ▼
xn*     its Xbox 360 backend          ◄── the native renderer replaces this
   │
   ▼
D3D     Microsoft's Direct3D for 360 (linked into the exe, recompiled)
   │  command buffer (PM4 packets)
   ▼
ReXGlue's Xenos GPU emulation → Vulkan   (today, including EDRAM emulation)
```

The Xbox backend, all in the code block `0x82420000–0x82460000`:

| Class | Role | vtable | Slots (own) |
|---|---|---|---|
| `xnContext` | the drawing API: frames, clears, states, matrices, immediate geometry | `0x82016744` | 126 (42 differ from `pddiBaseContext`) |
| `xnDisplay` | the screen: size, aspect, swap | `0x820169CC` | 27 (23) |
| `xnDevice` | factory: creates textures, shaders, buffers | `0x8201701C` | 16 (12) |
| `xnTexture` | textures | `0x820171EC` | 23 (18) |
| `xnPrimBuffer` | static meshes (vertex + index buffers) | `0x82017F64` | 17 (12) |
| `xnPrimBufferStream` | writing vertices (immediate mode, buffer fill) | `0x82017FAC` | 32 (all) |
| `xnShader` + 18 subclasses | one class per material / effect | e.g. `xnSimpleShader` `0x82017E64` | 23 each |
| `xnExtUnLitColourTint` | an extension (colour tint for unlit materials) | `0x8201672C` | 5 |

The shader subclasses: `xnSimpleShader` (and `xnErrorShader`),
`xnBumpMegaShader`, `xnCharacterRimShader`, `xnReflectShader`,
`xnRefractShader`, `xnWaterSpecularShader`, `xnUndergroundShader`,
`xnShadowShader`, `xnParticleShader`, `xnFxGridShader`, `xnMotionTrailShader`,
`xnHeatShimmerShader`, `xnBloomShader`, `xnDOFShader`,
`xnColourMatrixShader`, `xnRTRestoreShader`, `xnBinkShader` (movies).
Full slot lists: `python3 tools/rtti_vtables.py out/default_image.bin game/default.xex '^xn'`.

Other classes in the same code block, directly on top of PDDI:
`FramebufferFxExt` (framebuffer effects extension), and the Bink movie
player (`PlayerBink`, `RenderStrategyBink`).

## 2. Nothing but the renderer calls D3D

`tools/callgraph.py` over the whole program: the D3D library
(`0x82305000–0x82323000`, plus its draw functions in a separate block at
`0x82476000–0x82478000`) has **no direct callers outside the renderer
block**. Its only outside "callers" in a first pass were four functions at
`0x82476DA0–0x82477B58`, which turned out to be D3D's own draw functions
(they read D3D's dirty-state masks and write the command buffer), themselves
called only from PDDI. Everything below `0x82305000` that looked like D3D is
XAPI/CRT (critical sections, `SetLastError`...).

So gameplay code, UI code and the movie player never bypass PDDI. A
replacement of the `xn*` backend sees every draw of every frame. (Caveat:
this covers direct calls. Code that writes D3D's device structure inline or
calls D3D through a pointer would not show up; none has been seen in the
traces either.)

## 3. What a frame looks like (the tracer)

The tracer wraps 459 renderer methods and 88 D3D functions
(same override trick as `src/frame_rate.cpp`: the generated `sub_X` is a
weak alias, ours wins, `__imp__sub_X` is still the original). Frames are
cut at `xnDisplay::SwapBuffers`. With no trace running, a wrapper costs one
atomic load; the game runs normally.

Calls per frame (one frame each):

| Screen | Calls | Notes |
|---|---|---|
| Sierra copyright card (5 s) | 1,888 | |
| Loading screen → title (15.5 s) | 746 | 10 immediate-mode quads |
| Main menu (26 s) | 5,313 | 103 immediate-mode batches, no meshes |
| **Hub level, Crash standing** | **17,550** | 289 mesh draws, 44 immediate batches, 28 resolves |

The hub frame, in order (from `D3D::` calls and their parent methods):

1. `xnContext::BeginFrame`: colour + depth targets for 1280×720,
   `D3D::BeginTiling?` with **3 strips**.
2. The 3D scene, recorded once and replayed by the GPU per strip: clear,
   ~200 draws; ~140 of them between `xnContext::v104`/`v105` pairs whose D3D
   calls look like conditional (occlusion-query) rendering (unverified).
3. Effects that read what's already drawn: particles, shadows and water each
   **resolve** (copy EDRAM → texture) the current strip, once per strip,
   selected with `D3D::SetPredication?` masks 2/8/0x20 (strip 0/1/2).
4. `D3D::EndTiling?`: resolves each strip's colour into the scene texture.
5. Full-screen post-processing without tiling: `xnDOFShader` (depth of
   field) and friends, 17 draws. Then the HUD.
6. `xnContext::EndFrame` resolves to the frontbuffer; `SwapBuffers`.

Steps 1, 3 and 4 exist only because the 360 renders into 10 MB of EDRAM.
A native renderer draws the frame once at full resolution, and a "resolve"
becomes a copy (or just sampling the previous render target).

Shader classes seen in the hub frame: `xnSimpleShader` (49 materials, most
of the world), `xnReflectShader` (1 object, 267 calls), `xnParticleShader`
(14), `xnCharacterRimShader` (2, Crash), `xnWaterSpecularShader` (2),
`xnShadowShader`, `xnDOFShader`. `xnBumpMegaShader` wasn't used there.

### Materials take named parameters

`pddiBaseShader::SetTexture?` is called with `r4 = 0x00584554`, which is the
text `"TEX"`, plus a texture object; `pddiBaseShader::SetInt?` with
`0x0054494C` = `"LIT"`. Pure3D materials are a shader *type* plus named
parameters (texture, lighting on/off, colours...). The `xn*Shader` class of
each type turns those into D3D shaders and constants. For a native renderer
this means we can implement each material type by what its parameters
**mean**, instead of emulating Xbox shader registers.

## 4. Names so far

Recorded in `KNOWN_NAMES` of `tools/gen_pddi_table.py`, so traces
print them. `?` = inferred from arguments and call order, not proven.

| Address | Name | Evidence |
|---|---|---|
| `0x82431470` | `xnContext::BeginFrame` | first call after each swap; sets RT + depth, begins tiling |
| `0x82431F40` | `xnContext::EndFrame` | last before swap; ends tiling, resolves |
| `0x824306A0` | `xnContext::Clear` | its only D3D call is `D3D::Clear?` |
| `0x82430780` / `0x82430870` | `xnContext::BeginPrims` / `EndPrims` | immediate geometry; D3D hands a command-buffer write pointer |
| `0x82430E60` | `xnContext::GetExtension?` | `(0x107)` → the colour-tint extension object |
| `0x82430990`–`0x82430C98` | `xnContext::v56`–`v82` (even) | render-state setters, one D3D state call each; which state is which: todo |
| `0x82433EE8` / `EE0` / `3970` | `xnDisplay::GetWidth?` / `GetHeight?` / `GetAspect?` | return 1280, 720, 1.7778 |
| `0x82443BD0` | `xnPrimBuffer::Draw?` | SetIndices, SetVertexDeclaration, SetStreamSource (stride 24), DrawIndexedVertices |
| `0x8243BAC8` / `0x8243BB80` | `pddiBaseShader::SetTexture?` / `SetInt?` | `"TEX"` + texture, `"LIT"` + int |
| `0x8230DD40`, `0x8230DA68` | `D3D::SetRenderTarget?`, `SetDepthStencilSurface?` | index 0/1 + surface |
| `0x823193D0`, `0x82319860` | `D3D::BeginTiling?`, `EndTiling?` | count 3 + strip rects; EndTiling resolves per strip |
| `0x82319260` | `D3D::SetPredication?` | masks 2/8/0x20 around per-strip resolves |
| `0x8231EE00` | `D3D::Resolve?` | the EDRAM → texture copies |
| `0x82322808` | `D3D::Clear?` | flags `0x3F` = every target + depth + stencil |
| `0x82476DA0`, `0x82477238` | `D3D::BeginVertices?`, `EndVertices?` | (prim type 4, 6 vertices, stride 16) → pointer |
| `0x82477B58` | `D3D::DrawIndexedVertices?` | (prim type, base, start, 1632 indices) |
| `0x8230CF78`, `0x8230CE58`, `0x82313040` | `D3D::SetIndices?`, `SetStreamSource?`, `SetVertexDeclaration?` | see `xnPrimBuffer::Draw?` |
| `0x82312E70`, `0x82312BB8` | `D3D::SetVertexShader?`, `SetPixelShader?` | called by every material's setup (slot 14 of the shader classes) |

Two curiosities: the `xnContext` object lives at guest `0xF9B4C3A0` (a
physically-mapped address, so the engine allocates it with physical
memory), and the D3D device at `0x40005200` starts with the word
`0xFFFFFFFF` (not a vtable: D3D is plain C).

## 5. Decision: cut in at the Xbox backend (`xn*`)

Recommended: **replace the `xn*` backend classes** (Radical's PDDI device
layer) with our own Vulkan implementation. Why:

* **Complete.** It's the only door to the GPU (section 2).
* **Bounded and object-shaped.** 298 functions with their own code: 42 in
  `xnContext`, 29 in `xnPrimBufferStream`, 21 in `xnDisplay`, 18 in
  `xnTexture`, 12 each in `xnPrimBuffer` and `xnDevice`, ~9 per shader
  class. Each does one engine-level thing: "draw this mesh", "set this
  material's texture", "clear". Many are tiny (getters, setters). No command
  buffers, no EDRAM, no tiling.
* **Features fall out.** The frame size comes from `xnDisplay::GetWidth?` /
  `GetHeight?` / `GetAspect?`; render targets are ours to size, so any
  resolution and aspect ratio (plus MSAA) is a renderer setting. Materials
  are named parameters, so we can write our own shaders per material type
  (better lighting, higher-resolution shadows later) instead of emulating
  Xbox microcode.
* **Game stays playable.** We can switch per frame between the original
  backend and ours.

Rejected for now:

* **D3D level** (the *Unleashed Recompiled* approach): well-known API, but
  much of 360 D3D works through inline writes into the device structure plus
  dirty masks, flushed at draw time. We'd be decoding Xbox GPU register
  state again, and still dealing with tiling, predication and resolves (the
  trace shows how much of the frame is about EDRAM). It stays useful as
  knowledge: the D3D calls under each `xn*` method document what it does.
* **Improving ReXGlue's emulator**: decided against earlier (see
  [04-native-renderer.md](../04-native-renderer.md)).

Shaders remain the biggest job either way. Plan: start with the few simple
material types the title and menus use (`xnSimpleShader`, the palette
sprite shader, `xnBinkShader` for movies), written by us; evaluate the SDK's
Xenos→SPIR-V translator as a stop-gap for the rest.

## 6. Proposed architecture (milestone 2 onward)

* **Host objects keyed by guest `this`.** The guest objects stay (the game
  allocates `xnTexture` etc. and calls them through guest vtables). We
  override each `xn*` method (`extern "C" REX_FUNC(sub_X)`, like the tracer)
  and keep our Vulkan state in host-side objects found by the guest pointer.
* **Shadow mode first.** During development our overrides call the original
  *and* mirror the call into our renderer, which draws into its own image.
  The emulated path keeps working untouched, and a hotkey switches the
  window between the emulated and the native picture (A/B comparison).
  When the native renderer covers a screen fully, the originals stop being
  called for it: that's when the EDRAM emulation's cost disappears.
* **Presenting.** ReXGlue's presenter already has the hand-off:
  `Presenter::RefreshGuestOutput(width, height, ...)` gives a callback a
  `VkImage` (format `A2B10G10R10`, left in `SHADER_READ_ONLY_OPTIMAL`, graphics
  queue 0 of the SDK's `VulkanDevice`) and accepts **any size**. We share the
  SDK's Vulkan device. Needed: a small SDK patch so the emulated command
  processor skips its own `RefreshGuestOutput` while the native picture is
  shown (it presents at `src/graphics/vulkan/command_processor.cpp`, in the
  swap handling).

## Open questions

* Which render state each `xnContext::v56`–`v82` setter is (blend, depth,
  cull, alpha test, stencil...): read the D3D state function under each.
* `xnContext::v104`/`v105` (occlusion queries?), `v103` (called with 0x46
  before the post-processing), `v121`/`v123` (called hundreds of times,
  no D3D: dirty flags?).
* How textures are created and filled (`xnDevice::v9` creates one during
  loading; `xnTexture::v13`/`v14` fill it) and their formats (tiled,
  big-endian; the SDK's texture code can untile).
* Vertex formats (the `0x2020` flags passed to `BeginPrims`; declarations of
  mesh buffers).
* `xnBumpMegaShader` and the other effects: need traces in other levels.
* The Bink movie path (`PlayerBink`, `xnBinkShader`) and `FramebufferFxExt`.

## Reproduce

```bash
# once, if out/default_image.bin is missing
out/build/linux-amd64-relwithdebinfo/xexdis game --dump out/default_image.bin
python3 tools/rtti_vtables.py out/default_image.bin game/default.xex '^xn' --summary
python3 tools/callgraph.py out/default_image.bin generated/default/crash_mom_init.cpp \
    game/default.xex callers 0x82476DA0

# trace frames at fixed times (title ~16 s, menu ~26 s with this script)
out/build/linux-amd64-relwithdebinfo/crash_mom --game_data_root=$PWD/game \
    --debug_input_script="8000:start,9500:start,11000:start,12500:start,14000:start,17000:start" \
    --debug_pddi_trace_dir=<scratchpad>/trace --debug_pddi_trace_ms=15500,26000
# or live: add --debug_pddi_trace_trigger=<scratchpad>/trace.now, then `touch` it
```

The hub trace was taken on a copy of the user's save (Load Game → slot 2),
driven through `--debug_input_fifo`.
