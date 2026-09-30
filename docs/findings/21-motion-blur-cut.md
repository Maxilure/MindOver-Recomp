# 21. Motion blur: switched off in the Xbox 360 game itself

2026-09-30. Since findings/11, the to-do
list carried "render target 1: the lit material's motion-blur velocity".
Before writing it, we checked whether the game ever USES that velocity.
It doesn't: motion blur is present in the engine, but the Xbox 360 renderer
turns every request for it into nothing. On the real console the game has
no motion blur, so there's nothing to copy, and our picture is already
right without it.

## 1. What made it look like a to-do

* The lit simple material's vertex shader (`simpleshaderlitvp`, debug info)
  works out a screen-space **velocity** per vertex from this frame's and the
  previous frame's matrices (c16-c19), and its pixel shader writes it as a
  second output (render target 1). findings/11 section 3.
* `xnContext::Clear` (`0x824306A0`) always follows its normal clear with a
  second D3D clear of render target 1 ALONE, in grey `0x808080`. Grey is
  "velocity zero" (0.5 in each channel = no movement), so this is the clear
  of a velocity buffer.
* The disc has a motion blur shader: `shaders/motionblurfp.out`. Its
  constant table (`tools/shader_constants.py`) names `Params`,
  `Dimensions`, `CurrentFramebuffer` (s0), `CurPixelVelMap` (s2),
  `PrevPixelVelMap` (s3): a post pass that smears the picture along the
  velocity map.
* The game code has a class `CMotionBlurRenderer` (RTTI, vtable
  `0x82041AEC`, built by `0x822BB960`), one of the game's effect renderers
  (`CEffectRenderer`).

## 2. Why nothing comes of it

1. **Render target 1 is never given a surface.** D3D's `SetRenderTarget`
   (`0x8230DD40`) has exactly five callers (`tools/callgraph.py`). Four set
   target 0. The fifth, in the post-processing setup (`0x824316A4`), sets
   target **1 to nothing** (r4 = 1, r5 = 0). All 132 traced frames in
   `logs/trace-*` (every area traced so far: Wumpa Island, the waterfall
   area (name TBD), N. Gin's lab, the Ratcicle Kingdom, fights, cutscenes) agree: target 1 only
   ever becomes 0. With no target 1, the GPU throws the shader's velocity
   output away, and the grey clear of target 1 clears nothing.
2. **The Xbox backend's motion blur functions are empty.**
   `CMotionBlurRenderer` asks the renderer for its framebuffer effects
   extension (`GetExtension(0x103)`, `FramebufferFxExt`, vtable
   `0x82017B4C`) and calls two of its slots: slot 1 with "on" or "off" plus
   two strengths (methods `0x822BBA18` / `0x822BBA88` / `0x822BBAF8`),
   and slot 4 (`0x822BBBF8`). In the Xbox backend, slot 1 is `0x821B9718`
   and slot 4 is `0x82440680`: **both a single `blr`** (return right
   away). Compare depth of field, which works: slot 30, `0x82440A50`,
   stores its values and draws (findings/15).
3. **No material for the shader.** None of the 18 `xn*Shader` classes loads
   `motionblurfp.out` (findings/08's class list has no motion blur class;
   `xnMotionTrailShader` is a different thing: `motiontrail.out`, the
   `fight` system's `CMotionTrailTrack` = swoosh ribbons behind attacks).

So Radical's engine can do motion blur, the gameplay code even asks for it,
but the Xbox 360 renderer they shipped was built with it disabled. The only
traces left are a velocity computed per lit vertex and a clear that clears
nothing.

## 3. What that means for us

* **Nothing to match.** The native renderer already draws what the console
  shows. The emulated GPU agrees (it runs the same empty functions).
  "Render target 1" is off the to-do list.
* Our lit shader doesn't compute the velocity (it would be thrown away).
  The recorder still draws only target 0, and that is correct.
* **A possible upgrade, later.** The pieces for real motion blur exist: the
  velocities the game's shader already knows how to compute, and the
  designers' own blur shader on the disc (`motionblurfp.out`: we would
  write our own GLSL from what its microcode does, never copying the
  file). That makes it a candidate for the future graphics settings (an
  option, off by default, since the original game doesn't have it). No
  work planned now.

## 4. How we found it (repeatable)

```bash
# who sets render targets, and with what (r4 = index, r5 = surface)
python3 tools/callgraph.py out/default_image.bin generated/default/crash_mom_init.cpp game/default.xex callers 0x8230DD40
grep -h "SetRenderTarget?" logs/trace-*/*   # index 1 only ever with surface 0
# the motion blur shader's inputs
python3 tools/shader_constants.py game/shaders/motionblurfp.out
# CMotionBlurRenderer's vtable, and the extension's slots
python3 tools/rtti_vtables.py out/default_image.bin game/default.xex 'MotionBlur|FramebufferFx'
out/build/linux-amd64-relwithdebinfo/xexdis game 0x822BBA18 0x822BBC40   # calls slot 1 / slot 4
out/build/linux-amd64-relwithdebinfo/xexdis game 0x82440680 0x82440684   # slot 4 = blr
out/build/linux-amd64-relwithdebinfo/xexdis game 0x821B9718 0x821B971C   # slot 1 = blr
```
