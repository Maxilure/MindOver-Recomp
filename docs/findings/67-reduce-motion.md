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
it finished at once. The caller's time step (`f1`) is put back after the
call: the announcement display (`CAnnouncementDisplay`, update
`sub_8225B750`) subtracts it from its display time right after stepping its
slide, and a first version that left 1e6 in `f1` would have ended every
announcement at once.

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

### 2.2b Floating icons (button prompts, the "!")

The icons over things to use or people to talk to (a door's button prompt,
the "!" with its flash) are `CIconBehaviour`s (vtable `0x8203248C`). Their
update (slot 4, `sub_82178ED8`) advances a timer at `+0x6C` by 3 x the time
step and turns the icon by `sin(timer) x 0.5` and `sin(timer x 2) x 0.2`
around its resting place (`+0x60`, 3 floats); then it passes the time step
to the icon's drawable (`+0x74`, vtable slot 5), whose own animation is the
flash growing and shrinking. **Change:** the update runs with a time step
of 0 (the caller's is put back after): the icon stays upright and its
animation stays on its first frame.

### 2.2c Pick-ups flying to the player

A pick-up (mojo, health, power-ups) is a `Collectable` (a script behaviour
controller, vtable `0x82034DF4`); its update (slot 21, `sub_82198738`)
adds the time step to a timer at `+0xC4`, then runs a state machine at
`+0xC8` (jump table at `0x8219881C`, states 1-6). **State 4** is the flight
to the player who picked it up: from the pick-up point (`+0xD0`, set when the
state starts) to the player's position along a small arc, progress = timer
x 2 (the constant `0x8201FB74`, cached at `0x825A0270`), so half a second;
at progress 1 the pick-up counts as collected (`sub_821994D0`) and moves on.
**Change:** in state 4 the timer is set to 0.5 s minus this frame's step
(plus 1e-4 against rounding), so the progress reaches 1 on this frame: the
pick-up is collected at once.

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

Playtested on Windows (one player) with the setting on and off: menus and
panels, the selected item, button prompts and the "!" icon, mojo pick-ups,
announcements. Not tested: Linux, co-op, every front-end screen.
