# 25. Starting straight into a save (`--load_save`)

2026-10-03. For testing (the physics above 30 fps first of all), every run
should start at the same spot, quickly, with no menu inputs to script. The
normal way from launch to gameplay takes about two minutes: a copyright
screen, four boot movies (Dolby, Radical, Sierra, the attract movie), the
title, the main menu, the Load Game list, "Load successful". With
`--load_save=<save>` the game is in the level about 20 seconds after
launch:

```
 7.8 s  save 11 'Ice Prison' chosen, boot movie 'dolby' skipped
 8.0 s  boot done, into the menus' game state
10.5 s  into Load Game        (the title's loading done; "Press START" done for it)
15.7 s  loading save 11       (the game's device check and save scan ran first)
18.7 s  save loaded, starting the level
~21 s   in game (the level's own loading screen, then gameplay)
```

The save is named by its number (`--load_save=7`), its name or a part of
the name only one save has (`--load_save=prison`, case ignored), or `last`
(the most recently played save, the top of the Load Game list). A name that
matches nothing, or several saves, is reported in the log with the list of
saves, and the game boots normally. Without the option nothing changes.

Short version of how: every step is the game's own. The front end's screens
are a fight tree whose compiled decisions return the exit to take
([findings/24](24-save-system.md), section 7). The route changes four
answers, skips the movies, and presses START once in code.

## 1. The route through the front end's tree

`--debug_frontend_trace` (new) logs every exit the front end takes, as
"state -> exit". A normal Load Game, as numbered in the 360 release's
`fighttrees/Frontend.bfig`:

```
3 BootUp -> 27 -> 41 LicenseScreen -> 42 -> 44 LoadPersistentPostBootUp
44 -> 50 -> 51 Movie -> 52 -> 53 LoadingTitleScreen -> 54 -> 55 TitleScreen
55 -> 63 (START) -> 68 MainMenuScreen -> 87 ExitLoadGame -> 113 AccessMemoryCard
113 -> 124 -> 125 DeviceSelectorOpen -> 126 -> 127 StateBeginAccess -> 143 -> 113
113 -> 146 -> 153 ReadingCard -> 159 -> 160 GameSlotScreen (the list)
160 -> 187 (slot 1, load) -> 278 LoadGameScreen -> 281 -> 282 LoadComplete
282 -> 283 (a button) -> 486 InGame -> 488 -> 489 GameRunning
```

States and exits run numbered functions of the tree's Lua chunk (read with
the Lua 5.0 tools of [findings/22](22-ground-contact-high-fps.md)). The
ones that matter here:

| Node | Script | What it does |
|---|---|---|
| 44 (state) | 11 | `GotoGameState(EGameState_LOAD_PERSISTENT_POST_BOOTUP)` |
| 51 Movie (state) | 14 | `GotoGameState(EGameState_MOVIE)` |
| 52 ExitTitleScreen | 15 | `GotoGameState(EGameState_LEVEL_SELECT)`: the menus' game state |
| 87 ExitLoadGame | 24, 27 | profile flag; `SaveGameManager` ClearSelectedDevice, EndAccess, ClearLastResult, BeginAccess(ESaveMode_LOAD) |
| 187 ExitSlotNotEmpty | 45 | `SaveGameManager:LoadGame(0)` |
| 278 LoadGameScreen | 56 | front end ResetToDefaults |
| 283 ExitToLevel | 57 | EndAccess, ClearLastResult, `StartLevelFromLastCheckPoint` |

The engine follows any exit a decision returns, child of the current state
or not, so the quick route is:

| State | Decision | Normally | Quick load |
|---|---|---|---|
| 44 LoadPersistentPostBootUp | `sub_8211B4E0` | 50 (Movie) at once; 49 with Radical's skip-to-level switch (`sub_821235F8(_, 5)`) | **52**: the movie state's own way out, with its script |
| 53 LoadingTitleScreen | `sub_8211B5A8` | 54 once the title has loaded (`sub_821235F8(_, 4)`) | START for controller 0 (section 3), then **87** |
| 153 ReadingCard | `sub_8211BE58` | 159 (open the list) once the scan is done | the save picked in slot 0, then **187** |
| 282 LoadComplete | `sub_8211CE30` | 283 when a button is pressed (`sub_822652B0`) | **283** at once |

The save library ([findings/24](24-save-system.md), section 6) turns slot
0 into any save file: the quick load makes the chosen save the "pick" for
slot 0, as a choice on the list would, and the LoadGame hook reads that
file. After the load the game's file is that save, so saving at a save totem
writes to it, as after a normal load.

Two first attempts, and why they failed:

* **44 -> 87 straight away** left the game in its boot state (the menus'
  game state is set by exit 52's script, never run): AccessMemoryCard
  waited forever on a black screen. Its decision (`sub_8211B9E8`) only
  answers after 1.0 s in the state AND with the save manager idle (+8 = 0).
* **Through 52 but without the title**: AccessMemoryCard answered 115
  "no profile" (section 3).

## 2. The boot movies

The four boot movies don't belong to the tree: they play while state 44
is up, and its decision is only asked after them (113 s into a normal boot).
One sequencer object plays them:

* `sub_82188448(object, name)` plays one movie: it builds
  `d:\movies\<name>.bik` (prefix at `0x82033498`).
* The names are a table at `0x82503018` (dolby, radical, sierra,
  crash_mom_attract), just before the front end's decision table.
* The "finished" handler `sub_821882B8` (r3 = object + 28) moves the
  counter at object +44 on: 0 plays table[1], 1 table[2], 2 table[3]. At 3
  it ends the sequence (`sub_822B4868(_, 0)`, then the object's vtable slot
  18), and the boot goes on to state 44's decision.

With a save chosen, the play function plays nothing: it sets the counter to
3 and calls the "finished" handler itself. The first movie (Dolby) is
skipped, and the boot is at state 44's decision 0.1 s later. The copyright
screen (state 41, 5 s) is left as it is.

## 3. "Press START", done in code

AccessMemoryCard's decision reads the game object's +352, the controller
that pressed START on the title (-1 = nobody yet); -1 takes exit 115
"no profile". The title's START is handled by the front end's input update
(`sub_82264988`, at `0x8226520C`/`0x82265218`), which does two things for
controller *c*:

* `sub_82266150(front end, 0, c)`: player 1 = controller *c* (stored at
  front end + (2131 + slot) x 4);
* `sub_8227CEA8(game, c)`: signs in controller *c*'s profile; it sets +352
  (and +356), or -1 if the controller has no signed-in profile.

The quick load calls both for controller 0 (the keyboard and mouse play as
it too) right before taking 87. The profile is the runtime's own, so the
device selector (125-127) and the save scan then run as in a normal Load
Game.

## 4. Checks (on a copy of a profile with 11 saves)

* `last`, `prison` (part of one save's name) and the number: in the level
  after ~21 s. Scripted stick and A presses move and jump the character
  (player 1's controller works).
* `nosuchsave` and `99`: a warning listing the saves, then the normal boot
  (copyright, movies, title).
* No option: the normal boot, no quick-load lines in the log.
* An empty `--load_save=` makes the SDK's flag parser take the NEXT
  argument as its value (seen: `--load_save=--debug_frontend_trace`).
  That's harmless (no save of that name), but pass a value or leave the
  option out.
