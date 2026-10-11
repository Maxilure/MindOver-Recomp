# 33. The save screens' artificial waits, the "successful" screens and the console wording

2026-10-11. Opening Save Game or Load Game, saving and loading a save all
held their storage messages for fixed times (1 to 3 seconds each, about 3 to
5 seconds per action), even though the PC save drive (findings/32) finishes
in a few milliseconds. The Xbox 360 game also asked for A on a "Load
successful." / "Save successful." screen after each load and save, and its
messages asked not to turn off the Xbox 360 console. All three are gone by
default: `src/saves/save_screen_waits.*`.

## 1. Measured before

With the console (findings/29) and `--debug_frontend_trace` (one line per
front-end exit, with its time), on a test copy of the saves:

| Action | States on the way (node: what it shows) | Time |
|---|---|---|
| Pause -> Save Game | 359 AccessMemoryCard, 366 ShowDeviceSelector, 368, 359 again, 384 ReadingCard: "Accessing Crash: Mind Over Mutant save game files. Please do not turn off the Xbox 360 console." | 3.07 s |
| Save (in game) | 326 AutoSaveGameScreen: "Saving content. Please do not turn off your console." | 3.0 s (the file was written after 32 ms) |
| Main menu -> Load Game | 113 AccessMemoryCard, 125 DeviceSelectorOpen, 127, 113 again, 153 ReadingCard | about 4 s |
| Load a save | 278 LoadGameScreen | 3.0 s |
| After loading / saving | 282 LoadComplete, 293 SaveComplete (New Game's first save), 329 AutoSaveSuccessful: "Load successful." / "Save successful.", Continue | until A |

## 2. Where the waits are

Each front-end state is a node of the fight tree
`fighttrees/Frontend.bfig`; its compiled DECISION (a function pointer in the
table at 0x82503030, by node number) is asked every frame by the dispatcher
`sub_8211AF40` and returns an exit to take, or -1 to stay (findings/24
s.6.5). The dispatcher passes its r3 (the front end's state machine) on to
the decision.

The waiting decisions all read the state's **time in state**, the float at
`*(*(r3 + 176) + 28) + 132`, and compare it with a constant: 1.0
(0x8201EE28), 2.0 (0x8201FB74) or 3.0 (0x8201FD24). For example ReadingCard
in game (`sub_8211DA80`, node 384): exit 385 when the save manager is idle
(`*(*(0x8259B190) + 80) + 8 == 0`) AND the time in state is over 2.0.

A scan of every decision in the table (each function's extent taken from
the recompiler's function table; scratch `frontend_time_gates.py`) found 19 that read
the time in state, each exactly once, only for this comparison. The ones
about storage:

| Node | State | Decision | Minimum |
|---|---|---|---|
| 4 | CheckMemoryCard (boot) | sub_8211B020 | 1 s |
| 8 | ReadingCard (boot) | sub_8211B118 | 2 s |
| 113 | AccessMemoryCard (menus) | sub_8211B9E8 | 1 s |
| 125 | DeviceSelectorOpen | sub_8211BC50 | 1 s |
| 153 | ReadingCard | sub_8211BE58 | 2 s |
| 278 | LoadGameScreen | sub_8211CD80 | 3 s |
| 289 | SaveGameScreen | sub_8211CEE0 | 3 s |
| 326 | AutoSaveGameScreen (in game) | sub_8211D2D8 | 3 s |
| 359 | AccessMemoryCard (in game) | sub_8211D738 | 1 s |
| 366 | ShowDeviceSelector | sub_8211D920 | 1 s |
| 384 | ReadingCard (in game) | sub_8211DA80 | 2 s |

The others are left alone: the warning, licence and title screens' timers
(not storage), FormattingScreen (never reached), FadeOut 296 (a real
fade), the extras' loading screens and the attract demo.

## 3. The change

The port already runs C++ decisions for nodes it adds (data/fight_tree.h,
`SetFrontEndDecision`); the same hook takes existing nodes. For the 11
states above, ours sets the time in state to an hour, calls the original
dispatcher (so the game's own decision runs with every one of its real
checks: manager idle, device chosen, load finished...), and puts the real
time back. The screens now move on as soon as the storage work is done.
`--save_screen_waits=true` brings the Xbox's minimum times back.

**The "successful" screens.** Their decisions (282 `sub_8211CE30`, 293
`sub_8211CF90`, 329 `sub_8211D3F8`) ask the menus' IsButtonPressed
(`sub_822652B0`) for A and return one exit when it's down. Ours asks the
original first (its other exits, the attract demo's 979/980 via
`sub_82122728`, keep working) and, when it would go on waiting, returns the
exit A takes: 282 -> 283 ExitToLevel, 293 -> 295 ExitFINAL (then the FadeOut
into the level), 329 -> 331 ExitEndAccessFirst while the save manager's mode
(+4) is 2, else 332 ExitToInGame, like its two A branches.
`--save_success_screens=true` shows them again.

| Action | Before | After |
|---|---|---|
| Pause -> Save Game | 3.07 s | 0.22 s (the scan of the saves) |
| Save (in game) | 3.0 s + "Save successful." | 0.07 s, straight back to play |
| Main menu -> Load Game | about 4 s | 0.23 s |
| Load a save | 3.0 s + "Load successful." | 0.07 s, then the level loads |
| New Game's first save | 3.0 s + "Save successful." | 0.05 s, then the fade into the level |

`--load_save` (findings/25) runs the same states, so it reaches gameplay
about 5 seconds sooner too.

## 4. The wording

The messages are strings of the menu packages' text banks (findings/30):
UTF-16 big-endian, each ended by a 0 character, packed one after another
and found through an offset table. The English ones:

* "Accessing Crash: Mind Over Mutant save game files. Please do not turn off
  the Xbox 360 console." (the M and O of the title are special characters of
  the font)
* "Saving Crash: Mind Over Mutant game, please do not turn off the Xbox 360
  console."
* "Saving content.  Please do not turn off your console."
* "Loading content.  Please do not turn off your console."

They sit in `package/cdd70a8c.p3d` (main menus) and the in-game menus'
packages (7a8185b0, 7a88cba0, 97597d1b, c1e387c7). A data patch
(data/data_patcher.h, every menu package) finds them by their endings and
writes "Reading save files...", "Saving..." or "Loading..." over them in
place, followed by 0 characters up to the old end: no offset or chunk size
changes. Other languages are left as they are. The "Please do not touch the
Power Button or RESET" forms belong to other consoles and never show.

## 5. The message, a moment late

With the waits gone, reading the saves takes a few frames (0.13 s on a
fast machine), and the message page blinked by: a full-screen black
`Background` polygon fading in to dim the picture, and the `Message` text
(pages `FE_TRG_Message_InGame`, `FE_TRG_Message_Frontend`,
`FE_TRG_Message`, findings/30). Every storage message has a `Wait...` text
id in the tree (WaitChecking: nodes 8, 113, 125, 127, 153, 359, 366, 368,
384; WaitSaving 289, WaitAutoSaving 326, WaitLoading 278).

In those states' decisions, the two elements are now kept hidden (element
visibility, +110 bit 0x80) until the storage work has gone on for
`--save_message_delay_ms` (1500 by default): a quick read or save shows
nothing, a slow one shows the message after a second and a half, for as long as
it takes. The "route" (in game 359 -> 366 -> 368 -> 359 -> 384) counts as
one: it ends after 100 ms without any of those states (checked at every
frame end), and then both elements are shown again, because the overwrite
question and the error messages use the same page.

The game has no by-name lookup for a polygon; the page's typed lookups sit
together: Menu `0x82371488`, Layer `0x82371588`, **Polygon `0x82371688`**,
Sprite `0x82371788`, Text `0x82371888` (each a dynamic_cast to its
`pure3d::frontend` class, by the name's hash from `sub_8232B438`).
`scrooby::FindPolygon` hashes the name and asks `0x82371688`.

## 6. Tested

On a copy of the saves, with the console and photos every 50-150 ms:
pause -> Save Game -> overwrite Yes (back in play, Crash walks); main menu
-> Load Game -> a save -> the level; New Game -> name -> difficulty -> its
own save -> the opening; `--load_save`; `--save_screen_waits=true` (the
1 s + 2 s holds back; "Reading save files..." and the dim appear after the
delay, tested with 500 ms before the default became 1500). Photos every 40 ms of the in-game route: no dim, no message;
the overwrite question right after still dims the picture. No errors in any
log.
