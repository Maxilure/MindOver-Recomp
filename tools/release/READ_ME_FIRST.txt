Crash: Mind over Mutant -- PC port, version @VERSION@ (alpha)
==============================================================

A fan-made native PC port of the Xbox 360 game. It does NOT contain the
game: it is built on your computer from YOUR OWN copy of the disc.
THIS IS AN ALPHA: an early test version. The game is playable, but expect
bugs, crashes and rough edges, and keep a copy of your saves
(user/saves). Many features are experimental and have been tested on only
a few computers.

How to start (Linux)
--------------------

1. Have your disc image ready: an .iso dumped from your own Xbox 360 disc
   of Crash: Mind over Mutant (USA). Putting it in this folder saves a click.
   Keep this folder on a drive with ~25 GB free.

2. Double-click "Crash Mind over Mutant" in this folder.
   (If nothing happens: right-click it, Properties, allow it to run as a
   program. Or start it from a terminal.)

3. The launcher opens on "Setup". It lists any build tools your system
   is missing, with the package names: install those with your system's
   package manager, then press "Check again".
   Then press "Set up everything" and let it work. The first time takes a
   while (it downloads ~550 MB and builds the game on your computer).

4. Press Play. The launcher also puts itself in your applications menu.

What's in this folder
---------------------

  Crash Mind over Mutant   the launcher: play, settings, setup, updates
  user/                    YOUR files: saves, settings, controls, photos
  game/                    the game's files from your disc (made by Setup)
  program/                 the built game (made by Setup)
  source/                  the port's code and build files (you can ignore it)

Updates: the launcher tells you when a new version is out; updating keeps
your saves and settings (user/) and only rebuilds what changed.

Something went wrong? See Troubleshooting:
  https://github.com/Maxilure/MindOver-Recomp/blob/main/docs/06-troubleshooting.md
Found a bug? Report it here (pick "Bug report", attach your log from user/logs):
  https://github.com/Maxilure/MindOver-Recomp/issues/new/choose

Project page: https://github.com/Maxilure/MindOver-Recomp
