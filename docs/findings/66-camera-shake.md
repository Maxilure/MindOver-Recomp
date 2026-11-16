# 66. Camera shake: how the game shakes the camera, and the Camera Shake setting

**The question.** For players who get motion sick or find a shaking picture
hard to follow (an accessibility request), can the camera's shaking be made
smaller or switched off, without changing how long it lasts or anything
else about the camera?

**The answer.** Yes. Every gameplay shake goes through one function of the
camera's behaviour controller. A new setting, **Camera Shake** (Options >
Accessibility > Motion: Off / Low / Medium / Original, default Original),
scales the size of the wobble there. It is the game's camera logic, so it
works with both pictures (emulated and native).

## 1. How it was found

* The executable's strings name the feature: a level-script function
  `CameraShake`, an objective event class `CActionCameraShake` (its data
  struct `CCameraAction::SCameraShakeData`), an event name `DO_CameraShake`
  and an enum value `CAMERA_SHAKE`. Code that loads each string's address
  was found by scanning for `lis` + `addi` pairs, and the RTTI class names
  were turned into vtables with `tools/rtti_vtables.py`.
* The script registration (around `0x8221ECAC`, next to `ManualOverride`
  and `CameraZoom`) gives `CameraShake`'s handler, `sub_8221F378`: it passes
  the script's arguments to the camera manager's **shake entry,
  `sub_8223A950`** (the manager is `*(*(0x8259B190)+0x2C)`).
* `CActionCameraShake`'s run method (vtable `0x8203F340`, slot 2,
  `sub_82293B60`) finds its actor's position and calls the same
  `sub_8223A950`.
* `sub_8223A950(manager, position, strength)` builds a camera message
  (type 7, subtype 5, payload: the position and the strength) and sends it
  to every listener in the manager's list (`+0x234`, end at `+0x244`).
* Searching the message handlers for subtype 5: the camera volume behaviour
  (findings/28, `sub_82108548`) ignores it. The handler that keeps it is
  **`CCameraBehaviourController`'s** (vtable `0x8202973C`, slot 5,
  `sub_82104E20`). Its update (slot 4, `sub_82104B70`) uses the stored
  values every frame.

Disassembly was read from a locally decrypted copy of the executable (the
retail XEX's AES key and basic compression; the copy stays out of the
repository, AGENTS.md s.2) with a Python disassembler, cross-checked against
the function list of `generated/default/`.

## 2. What the controller does

Fields of `CCameraBehaviourController` used by the shake:

| Offset | Type | Meaning |
|---|---|---|
| `+0xAC` | 3 floats | the current shake's point in the world |
| `+0xB8` | float | its strength (0 = no shake) |
| `+0xBC` | float | its phase: random at the start, + the time step each frame |

* **New shake** (`sub_82104E20`, subtype 5): the strength *as felt at the
  camera* is `strength x falloff(distance)` (`sub_82105058`):
  `falloff = ((40 - distance) x 0.025)^2`, 0 at 40 units or more. The new
  shake replaces the current one only if it is felt more strongly. The
  phase gets a random start (the game's random generator at `0x825A14F4`).
* **Each frame** (`sub_82104B70`, only in mode 3): with a strength above 0,
  `felt^2` sets the size of two waves. One pushes the camera's position along
  its axes (sines of the phase x 47 and x 37), the other turns its direction
  (a sine of the phase x 33; the direction is normalised again). Then the
  strength drops by 10 per second (`+0xB8 -= time step x 10`) and the phase
  moves on. The same function also runs the scripts' camera zoom (`+0xC0` to
  `+0xDC`, the `CameraZoom` script function), which is not touched here.

So the wobble's size goes with the **square** of the strength, and the
shake's length is `strength / 10` seconds.

## 3. The change

`src/gameplay/camera_shake.cpp` overrides `sub_82104B70` (the generated one
is a weak alias, AGENTS.md s.5) and calls the original from it:

1. Before the original runs, the stored strength (`+0xB8`) is multiplied by
   `sqrt(level)`, so the wobble (strength^2) becomes `level` of the
   original: Low 0.3, Medium 0.6, Original 1 (untouched, no other work).
2. After the original runs, the real strength is put back minus the amount
   the game's decay took off. The shake fades at the original pace and lasts
   as long; only its size changes.
3. Off uses a scale of 1e-6, not 0: with a strength of 0 the game also skips
   the decay, and a frozen shake would come back the moment the setting is
   raised again.

Setting: `camera_shake` (REXCVAR, 0 off, 1 low, 2 medium, 3 original),
saved in `user/settings.toml`; the Options row is live.

## 4. Not changed / open

* The controller's rumble (vibration) is separate (`SetControllerRumble`,
  the `ShakeStrength*` values of the input code) and is not touched.
* Cutscenes and other camera modes that move the camera by their own
  animation (NIS, MAYA, scripted cameras) may contain baked-in shaking that
  does not go through this message (to check).
* Tested so far: see the pull request.
