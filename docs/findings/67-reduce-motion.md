# 67. Reduce Motion: the game's menu animations, pulses and slow motion

**The question.** Players who get dizzy or sick from movement on screen ask
for a way to cut the movement that is not needed to play: sliding and
bouncing menus, pulsing items, slow-motion moments, motion blur. What does
the game move, and what can be stilled without changing how it plays?

**The answer.** One switch, **Reduce Motion** (Options > Accessibility >
Motion, off by default), covers four things the game does through a few
shared functions. Other moving parts were found and are listed in s.3 with
what they would need.

## 1. How it was found

A locally decrypted copy of the executable (kept out of the repository,
AGENTS.md s.2) was searched for the names of moving effects (transition,
pulse, bounce, wipe, shimmer, slow motion...). Their RTTI classes were turned
into vtables with `tools/rtti_vtables.py`, and code that loads a string's
address was found by scanning for `lis` + `addi` pairs. The functions were
read with a Python disassembler and checked against the function list of
`generated/default/`. Known pieces came from findings/27 (time scale),
findings/31 (the menu pulse, the Options slide) and findings/53 (HUD).

## 2. What Reduce Motion changes

### 2.1 Front-end transitions (slides, springs)

`CFETransition` (no base class, one virtual method: its value) is the front
end's animated number. Subclasses: `CFETransitionLinear` (vtable
`0x8203A0A8`, value `sub_8225EFD8` = start + (end - start) x t) and
`CFETransitionOverShoot` (vtable `0x8203A0B0`, value `sub_8225F050` =
start + (end - start) x (2t - t^3), which passes the end by about 9 % at
t = 0.82 and comes back: the "spring").

| Offset | Meaning |
|---|---|
| `+0x04` | direction: 0 forwards (t to 1), 1 backwards (t to 0) |
| `+0x08` / `+0x0C` | start / end value |
| `+0x10` | progress t, 0..1 |
| `+0x14` | speed (1 / length in seconds; `sub_8225EC10` sets it) |
| `+0x18` | running (byte) |

The step, `sub_8225EAE0(this, f1 = time step)`, moves t by speed x step and
stops at the end (`sub_8225EB60` starts one in a direction, `sub_8225EB70`
asks whether it has finished). 23 front-end functions step transitions this
way, the in-game Options panel's slide (`sub_820CE2C8`, findings/31) among
them.

**Change:** with the setting on, the step is given a time step of 1e6 s:
every transition reaches its end on its first frame, and a spring's value at
t = 1 is exactly the end. Screens that wait for a transition to finish see
it finished at once.

### 2.2 Pulses

* The Scrooby menu's update (Menu vtable slot 5, `sub_8237A070`, findings/31
  s.8) scales the selected item's label and value by
  `1.1 + 0.1 x |sin(timer)|`, the timer at menu `+0x94` advancing 4 per
  second.
* A Scrooby text's pulse (Text vtable slot 5, `sub_823730B0`) does the same
  with its own timer at `+0xAC`.

**Change:** the timer is set to 0 before and after the original call: the
item keeps the size 1.1 (still bigger and yellow, so the cursor stays easy
to see) and stops moving.

### 2.3 Slow motion

`CTimeManager::GetScale` (`sub_822FFA40`, findings/27 s.4) returns a base
from a table times the slow-motion factor at manager `+4` (ignored when bit
0x80 of `+0x18` is set). The scripts turn it on with `EnableSlowMotion`
(string at `0x82046E38`). The port already wraps the function for the cheat
speed (`cheats/cheats.cpp`).

**Change:** a factor between 0 and 1 is divided back out of the result
(0 and values of 1 or more are left alone), so slow-motion moments run at
normal speed.

### 2.4 Motion blur

The port's own motion blur (findings/34, native renderer only, off by
default) reads `reduce_motion::MotionBlurLevel()`: 0 while Reduce Motion is
on, whatever the Motion Blur row says.

## 3. Found but not changed (to do)

| What | Where | What it would need |
|---|---|---|
| HUD pop-ins: the health bar, the hint and the mission displays grow in with a sine when they appear | `CHealthDisplay` slot 3 `sub_822683D8`, `CHintDisplay` slot 3 `sub_8226A418`, `CMissionDisplay` slot 3 `sub_8226CB70` | each class keeps its own animation timer; the end size of each must be read before it can be forced |
| HUD pages sliding in from the side | `sub_82370E98` (findings/53) | check whether it uses `CFETransition` (then 2.1 already covers it) |
| Camera transitions between views (`ECameraTransitionType` CUT, LINEAR, SPHERICAL, ANIMATEDCAMERA, BLENDYAXIS, AUTOSMOOTH, WAIT) | `CCameraTransitionBehaviour` (vtable `0x820298EC`), `CBasicCameraTransition` (`0x820298DC`), the volumes' `AddTransitionOverride` | where the type and length are chosen; a cut can hide where to go next |
| The ground ring after heavy attacks | `CRadialFloorEffectAction`, the fight trees' `CRadialFloorEffectTrack` | find its drawing (Reduce Screen Flashes may already dim it when additive) |
| Heat shimmer (the picture rippling near heat) | `xnHeatShimmerShader` (`heatshimmervp` / `heatshimmerfp`) | find where the game uses it; the native renderer has no material for it yet |

## 4. Tests

To be filled in from the playtest of the pull request.
