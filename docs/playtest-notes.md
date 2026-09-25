# Playtest notes

Your notebook while playing. Claude reads this at the start of a session,
so rough notes are fine: what you saw matters more than how it's written.

## How to play a test session

```bash
tools/play.sh
```

It prints the session name (e.g. `2026-09-25_1130`) and writes
`logs/play-<session>.log`. When the game closes it says whether it
**crashed** and how many `[error]` / `[warning]` lines the log has. A few
warnings at startup are normal (Vulkan device list, gamecontrollerdb.txt,
`D:\sku.txt`).

## Trying 60 fps

```bash
tools/play.sh --fps_cap=60
```

Same game, but menus and gameplay run at up to 60 fps instead of 30. Write
down anything that looks or feels different from 30: things moving too fast
or too slow, jittery animation, cutscenes out of sync with their sound,
jumps or physics behaving oddly. It's safe for your saves: frame rate isn't
stored in them. Add `--debug_log_fps` and the log records the frame rate
every 5 seconds.

## What to write down

For each problem, one entry. The most useful facts are **when** (session
name + roughly how long into it, so we can find the log lines) and **what
you were doing**:

```
### <short title>
- Session: 2026-09-25_1130, ~20 min in
- Where: <chapter / area / menu>
- Doing: <what you pressed / what was happening>
- Saw: <what went wrong; screenshot file if you took one>
- Again?: <happens every time / once / not tried>
```

Kinds of problems to watch for:

* **Crash or freeze**: the window closes, or the picture stops. For a
  freeze, note the time *before* closing it. (If it repeats, we'll catch it
  under a debugger with `tools/guest_stacks.sh`.)
* **Picture**: black or missing things, flicker, wrong colours, stretched
  or garbage textures, missing effects (compare with memories or YouTube
  videos of the 360 version).
* **Sound**: missing sounds or music, crackles, voices out of sync.
* **Speed**: slowdowns (F3 shows the frame rate), stutters when entering
  new areas.
* **Controls**: buttons not responding, stuck inputs, rumble.
* **Anything that just seems off.** Trust your memory of the game.

## Checklist: things worth trying on purpose

- [ ] **Save, quit the game, restart, load**: does the save survive?
      (Saves: `~/.local/share/crash_mom/B13EBABEBABEBABE/565507FA/`)
- [ ] Pause menu and the in-game options (volume sliders, subtitles)
- [ ] Every story cutscene plays with picture and sound (movies
      `Crash_01`–`Crash_19`)
- [ ] Each new area and the loading between them
- [ ] Boss fights
- [ ] Jacking (taking control of) each kind of mutant
- [ ] Main menu extras: Load Game, Credits, Calibration
- [ ] **Two controllers**: does player 2 drop in? (matters for online
      multiplayer later)
- [ ] A long session (an hour+): does it slow down or get unstable?

## Findings

<!-- Add entries below, newest first. -->
