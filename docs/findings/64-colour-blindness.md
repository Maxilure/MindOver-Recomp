# 64. Colour blindness correction (native renderer)

Options -> Accessibility -> **Colour Blindness**: a correction for players
with a colour vision deficiency, in the native renderer's last pass. The
emulated picture is left as it is (this project's accessibility work is for
the port's own renderer).

## 1. Where it goes

The native renderer ends every frame with one full-screen pass,
`src/native/shaders/present.frag`, that copies its 1280x720 frame (or the
render resolution's) into the presenter's guest output: the optional
sharpening (findings/41), then the game's display gamma ramp (a 256-entry
table per channel, `NativeRenderer::UpdateGammaRamp`). The correction sits
between the two: after the sharpening, before the ramp, on the display-encoded
colour. One pass that already reads every pixel: no extra render pass, no
copy, no readback, a few dozen ALU instructions per pixel, nothing at all
when it is off (`cvd_mode` 0 skips it).

The ramp itself could not do it: a per-channel table maps red to red, green
to green; a correction has to MIX the channels (a 3x3 matrix).

What it covers: everything the game draws (the world, the HUD, the game's
own menus, the cartoons: they are all in the frame). What it doesn't: what
the port draws over the finished picture with ImGui (subtitles, the Tools
menu, the FPS counter), which keeps its own colours (the subtitles' speaker
colours of findings/63 s.26 included).

## 2. The correction (daltonisation)

1. **Simulate** what the player sees: the frame's colour to linear light
   (a 2.2 power, the TV gamma the ramp targets; close enough here), times a
   3x3 matrix from Machado, Oliveira and Fernandes, *A Physiologically-based
   Model for Simulation of Color Vision Deficiency* (IEEE TVCG, 2009):
   severity 1.0 for the -anopias (one cone type missing), 0.6 for the
   -anomalies (one shifted). Achromatopsia = Rec. 709 luminance;
   achromatomaly = 60 % of the way to it.
2. **The error** = the colour minus its simulation: what that player loses.
3. **Put it back where it can be seen** (the classic daltonise
   redistribution): red-green losses (protan, deutan) into green and blue
   (`G += 0.7 eR + eG`, `B += 0.7 eR + eB`, red unchanged); blue-yellow
   losses (tritan) into red and green.
4. **No colour vision** can't be helped by moving colour around: the lost
   colour becomes a little brightness instead (`+ k (eR - eB)`, k = 0.5,
   0.3 for achromatomaly): reddish colours lighter, bluish darker, so two
   colours of the same brightness don't turn into the same grey.
5. Back to display encoding, then mixed with the original by the strength
   (`colourblindness_intensity` / 100).

Checked numerically (a script with the same matrices): a red (0.8, 0.3,
0.2) and an olive green (0.45, 0.5, 0.2), as a deuteranope sees them, are
0.044 apart in linear RGB before and 0.285 after; for a protanope 0.109 ->
0.432.

## 3. Settings

| Flag | Values | Default |
|---|---|---|
| `colourblindness_mode` | 0 off, 1 protanopia, 2 protanomaly, 3 deuteranopia, 4 deuteranomaly, 5 tritanopia, 6 tritanomaly, 7 achromatopsia, 8 achromatomaly | 0 |
| `colourblindness_intensity` | 0-100 (%; the Options row in steps of 10) | 100 |

Both are REXCVARs in `src/effects/post_effects.cpp`, kept in
`user/settings.toml` like every other setting (a missing entry = the
default, so older settings files keep working), and read every frame by
`NativeRenderer` (the present pass's push constants,
`PresentPushConstants`): the Options rows change the picture at once.
The rows are greyed out while the emulated picture is shown (`NeedsNative`).

**Preview:** the game stays on screen behind the Options screen, so each
mode and strength shows on the live picture as the row changes. The
Options screen has no "apply / cancel" step (every row of the project works
live), so there is no preview that waits for a confirmation.

## 4. Not done / to check

* Built and tried on Windows (native renderer): the modes and the strength
  change the picture behind the Options screen at once. Linux not built.
* The emulated picture has no correction (by choice).
* The ImGui layer (subtitles, tools) is not corrected.
* The 2.2 power is an approximation of the display encoding.
