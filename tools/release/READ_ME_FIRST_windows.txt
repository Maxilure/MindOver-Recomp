Crash: Mind over Mutant -- PC port, version @VERSION@ (alpha)
==============================================================

A fan-made native PC port of the Xbox 360 game. It does NOT contain the
game: it is built on your computer from YOUR OWN copy of the disc.
"Alpha" means: playable, but expect bugs.

How to start (Windows 10 / 11)
------------------------------

1. Have your disc image ready: an .iso dumped from your own Xbox 360 disc
   of Crash: Mind over Mutant (USA). Putting it in this folder saves a click.
   Put this folder on a drive with ~25 GB free (not in Program Files).

2. Double-click "Crash Mind over Mutant.exe" in this folder.
   (Windows may warn about an unknown program the first time: "More info",
   then "Run anyway".)

3. The launcher opens on "Setup". It lists any build tools your PC is
   missing, with their names for winget, Windows' package manager (in a
   terminal: winget install <name>). Visual Studio's C++ tools and the
   Windows SDK come from the Visual Studio Installer. Then press
   "Check again".
   Then press "Set up everything" and let it work. The first time takes a
   while (it downloads ~550 MB and builds the game on your computer).

4. Press Play. The launcher also adds itself to the Start menu.

What's in this folder
---------------------

  Crash Mind over Mutant.exe   the launcher: play, settings, setup, updates
  user\                        YOUR files: saves, settings, controls, photos
  game\                        the game's files from your disc (made by Setup)
  program\                     the built game (made by Setup)
  source\                      the port's code and build files (you can ignore it)

Updates: the launcher tells you when a new version is out; updating keeps
your saves and settings (user\) and only rebuilds what changed.

Project page: https://github.com/Maxilure/MindOver-Recomp
