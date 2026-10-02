# Enhancements: what only this port adds

Some of this project is what any good PC port is expected to do: run
natively, at higher frame rates, with keyboard and mouse, drawn by a
renderer of its own. That work is tracked in the
[README's status table](../README.md#status) and the
[roadmap](03-roadmap.md).

This page is for the rest: **big additions the Xbox 360 game never had**,
unique to this port. Each one is optional; the game still plays as the
original if you don't use it.

Status: ✅ done · 🔧 in progress · 📋 planned

| Enhancement | Original game | This port | Status |
|---|---|---|---|
| [More saves, with names](#more-saves-with-names) | 3 save slots; a save's name is typed once, at New Game | as many saves as you like, in the game's own Load Game screen; rename and delete | 📋 research done |
| [Up to four players](#up-to-four-players) | two-player co-op | four players together on one PC | 📋 |

## More saves, with names

📋 Planned; the research is done ([findings/24](findings/24-save-system.md)).

The original Load Game / Save Game screen shows three slots, and a save's
name (shown at the top of its slot) is the name typed when starting a New
Game. The plan:

* **As many saves as you like.** The screen keeps its three slots, but
  they become pages: moving down past the last slot shows the next three
  saves, moving up past the first shows the previous three. Today's three
  saves are the first page, untouched.
* **Rename** a save from the Load Game screen: first with a text box of the
  port's own, later with the game's own name-entry screen (the on-screen
  keyboard of New Game).
* **Delete** a save, with a confirmation.

Why it's possible without rebuilding the screen: the game only ever thinks
in slots 1-3, but a slot becomes a file in a single small function, and the
screen re-reads its three files every time it opens. Changing which files
sit behind the three slots is enough.

## Up to four players

📋 Planned.

The original has drop-in co-op for two: a second player joins from the
pause menu (Join Game) and leaves again (Drop Out). The goal is four
players together on one PC, whenever the game can be taught to handle them.

What's already in place: the Controls menu (**F6**, Players tab) decides
which device plays as which player, and players 2-4 share player 1's
profile ([findings/23](findings/23-keyboard-and-mouse.md) section 5). Not
studied yet: how much of the game is built for exactly two players (who
can join, the HUD, the camera, the co-op rules).
