# Enhancements: what only this port adds

Some of this project is what any good PC port is expected to do: run
natively, at higher frame rates, with keyboard and mouse, drawn by a
renderer of its own. That work is tracked in the
[README's status table](../README.md#status) and the
[roadmap](03-roadmap.md).

This page is for the rest: **big additions the Xbox 360 game never had**,
unique to this port. Each one is optional; the game still plays as the
original if you don't use it.

Status: ✅ done · 🧪 in testing · 🔧 in progress · 📋 planned

| Enhancement | Original game | This port | Status |
|---|---|---|---|
| [More saves, with names](#more-saves-with-names) | 3 save slots; a save's name is typed once, at New Game | as many saves as you like, in the game's own Load / Save Game screen; rename and delete | 🧪 in testing |
| [Up to four players](#up-to-four-players) | two-player co-op | four players together on one PC | 🔧 in progress |

## More saves, with names

🧪 Built, in testing ([findings/24](findings/24-save-system.md) section 6).

The original Load Game / Save Game screen shows three slots, and a save's
name (shown at the top of its slot) is the name typed when starting a New
Game. In the port:

* **As many saves as you like, in one list.** The screen keeps its three
  panels, but they are a window onto a list of every save, the one played
  most recently on top: move down past the bottom panel and the list
  slides up. With one or two saves, they sit in the middle of the screen.
* **Saving**: "Create New Save" comes first and previews the new save
  (today's date, your play time, %, difficulty and the picture of where you
  are); the cursor starts on the save you're playing. Picking it opens the
  name screen first, with your game's name in it to keep or change (Back =
  don't save). Overwriting a save keeps that save's name. **New Game**
  creates its own save at once, named what you typed: no slot to pick,
  nothing to forget.
* **Rename**: on a save, press **X**: the game's own name screen opens
  (the on-screen keyboard of New Game) with the save's name in it, up to
  16 letters, so a controller can type too. It works in every level too:
  there the keyboard shows over the level, like the in-game save list (the
  game keeps that screen only in its menus and in Crash's house; the port
  adds a rename screen of its own to the game's menu logic and the keyboard
  to the in-game menus, built from your own game files at startup). On the
  name screen, **X** erases a letter. On a keyboard, **F2** opens a quick
  box to type a name instead.
* **Delete**: press **Y** (or the Delete key) and answer the game's own
  question ("Are you sure you wish to delete this save file?"). The save isn't
  destroyed: it moves to a "Deleted saves" folder next to the saves, out of
  the game's sight, and can be moved back by hand.
* The screen's button prompts say so: "Rename X" replaces the Xbox's
  "Storage Device", which means nothing on a PC, and "Delete Y" fills the
  empty spot above "Back".

![The save list: five saves, after sliding down, and "Create New Save" with its preview](images/save-library-list.jpg)

![The new prompts, and the game's own name screen opened by X](images/save-library-rename.jpg)

![In game: the same keyboard over the level](images/save-library-rename-in-game.jpg)

![Create New Save: name it first, X erases a letter](images/save-library-name-new-save.jpg)

Why it was possible without rebuilding the screen: the game only ever
thinks in slots 1-3, but a slot becomes a file in a single small function,
and the screen re-reads its three files every time it opens. Changing which
files sit behind the three slots is enough; the list slides by changing
them and redrawing the panels.

Off: `--save_library=false` (the game's own three slots).

## Up to four players

🔧 In progress: research done, a first experiment
([findings/26](findings/26-more-local-players.md)).

The original has drop-in co-op for two: a second player presses START to
join, as the mask floating next to Crash, and leaves again from their pause
menu (Drop Out). The goal is four players together on one PC.

What's already in place: the Controls menu (**F6**, Players tab) decides
which device plays as which player, and players 2-4 share player 1's
profile ([findings/23](findings/23-keyboard-and-mouse.md) section 5).

What's known now: the game keeps each player's state and controller in
small tables built for exactly two, read in a few dozen places (joining,
the mask, spawning, the HUD, gameplay). An experiment spawned a third player
character into a level, drawn and stable, but not yet answering to a
controller. Since then the co-op tables have room for four players, and
`--local_players=3` (or 4) gives every level a third (and fourth) player,
who joins with START like player 2. Three players can already run around
together on foot ([findings/26](findings/26-more-local-players.md) section
11). Masks work for more than two: a player turning into a mask rides the
nearest Crash on foot, and one Crash carries up to three masks (section
12). Level changes work with four players; masks follow their host when it
turns into a mask, return to the same player in the next level, and any
player can turn into a mask while another one already is (section 15). An
audit opened 17 "for each player" loops (cutscenes, trigger volumes,
rumble, unlocks) to all four players (section 16). Players 3 and 4 have
their own HUD in the bottom corners (portrait, bars, mojo count, combo
multiplier, "Join Game"; nothing changes with two players), their own combo
meters, lock-on arrows, counter prompts and aiming reticles (sections
17-19). With three or four players, each player's "counter now" Y (a
titan's heavy attack) stands beside that player's own HUD, a little smaller;
the pause menu says "P3 Paused" / "P4 Paused"; players 3-4's "Please Wait"
countdown shows in their corner (sections 26-27). Players 3 and 4 wear
markers of their own over their heads, a "3" and a "4" drawn for the port
in the style of the game's "1" and "2" (after *Crash of the Titans*'
save-slot numbers; `assets/markers/`, section 29), and get their own paw
prints on the loading screen, green and purple, once they have joined
(section 28). The camera frames every player in game: it looks at the
centre of the group and backs off until everyone is in the picture, instead
of following one player; with three or four players it leans toward the
biggest group while still keeping the others in view (experimental, needs
extensive testing: [findings/28](findings/28-coop-camera.md)). Any player can drop out and join again, player 1 included,
as long as someone else stays in game (sections 30-31). A player whose
controller disconnects (or is moved to another player in F6) no longer
leaves a pause menu only that controller could close: the game pauses and
its own question box says so; any player can drop that player out, and once
the controller is back that player presses a button to resume (section 33). Next: more camera testing (fights, titans, bosses, tight
spaces) and the game's off-screen catch-up for players 3-4.

![Players 1, 4 and 3's markers](images/markers-players-three-four.jpg)

![Four players, each "counter now" Y beside its own HUD](images/four-players-counter-prompts.jpg)
