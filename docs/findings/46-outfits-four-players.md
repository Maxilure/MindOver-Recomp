# 46. Outfits for players 1-4: the chest, Coco for anyone, Carbon Crash in colours

The chest in Crash's house opens the game's outfit screen. On the Xbox 360,
player 1 picks one of Crash's outfits there, player 2 picks between Carbon
Crash and Coco's two looks, and players 3-4 (this port's addition) get
nothing. The port turns it into one wardrobe for all four players: every
outfit for everyone, Coco for player 1 too, and Carbon Crash in each
player's colour.

![Four players and the chest](../images/outfits-four-players.jpg)

## 1. What the game has

An outfit is a **skin number**, and each one is its own character template
("Characters:" + name, table 0x824F3A30) with its own package:

| # | Name in the game | Template |
|---|---|---|
| 0 | Crash | Crash |
| 1-3 | Crash Greeny / Bluey / Reddy (coloured shorts, shoes and hair tips) | Crash_Var01-03 |
| 4 | Snipe | Crash_Parafox |
| 5 | Ratcicle | Crash_Ratcicle |
| 6 | Magmadon | Crash_Shurtle |
| 7 | Skeleton | Crash_Skeleton |
| 8 | Spike | Crash_Spike |
| 9 | Coco | Coco |
| 10 | Old School (Coco) | Coco_Alternate |

- **Who wears what**: one number per player in a 2-entry table (0x8259B1C0),
  read and written only through `sub_8229F8B8` / `sub_8229F8A0`. A player's
  spawn event (`sub_8229C550`) builds them from it; a save stores players
  1-2's numbers next to the list of unlocked skins (0x825A72A8, 11 entries,
  -1 = end; unlocking Coco also makes player 2 Coco).
- **Carbon Crash is not a skin.** Crash's model carries two texture sets,
  `Crash_Player1.bmp` (orange fur, blue jeans) and `Crash_Player2.bmp` (white
  fur, green shorts). Its render behaviour (`CCharacterRenderBehaviour`)
  blends between them by +0xE4 (the script's `SetCharacterSkin`): set 1 for
  player 1, set 2 for anyone else. So player 2 in skin 0 is Carbon Crash.
  The other outfits' packages only have set 1.
- **The outfit screen** (`CSkinScreenAction`) is a carousel: the outfit's
  name, description and a 3D preview; A = "Wear Skin" (then a Yes / No
  question). Its page belongs to one player (this +0x84, 0 or 1), which also
  picks the page's layout (player 2's is mirrored), its list (player 1: skins
  0-8; player 2: 0 "Carbon Crash", 9, 10) and the preview's animations.
  With two players on foot, Back ("Goto Player 2" / "Goto Player 1") turns
  the page. "Wear Skin" (`sub_82123880`) does nothing for anyone above
  player 2, and only lets player 2 change character (Crash <-> Coco: every
  player is respawned from their skin).

## 2. One wardrobe for four players

`src/players/outfits.cpp`:

- **Four outfits.** The get / set functions are overridden: players 1-2 keep
  the game's table (saves are written as before), players 3-4 get the port's.
  An outfit picked at the chest is kept per player in `user/settings.toml`
  (`outfit_player1`-`4`, -1 = as the game decides), so it doesn't depend on
  which save is loaded. It only counts while the loaded save has that outfit
  UNLOCKED (the game's unlock list 0x825A72A8, 11 skin numbers ending at -1,
  filled from the save; its own test is `sub_8229F858`; Crash's model is
  always unlocked). Otherwise the player wears what the save says, and the
  pick comes back in saves that have it. Without this check, Coco picked in
  a late save was played in early saves and before the story unlocks her.
  Checked: an early save's list holds 0-3 (Crash, Greeny, Bluey, Reddy) and
  player 1 stays Crash with Coco picked; a desert save's list adds 9 and 10
  (Coco, Old School) and player 1 is Coco.
- **Carbon as an outfit of its own** (number 11 inside the port): toward the
  game it is skin 0, so its name, base character and "unlocked" lookups
  answer like Crash. Which texture set a character shows is decided every
  frame from its player's outfit (the render behaviour's Update, slot 4,
  `sub_821858C0`, which turns +0xE4 into the blend). The first attempt set
  it in slot 7 alone: that runs once, when the character appears, so a pick
  only showed after the character was made again.
- **One list on every page**: Crash, Carbon Crash, Greeny, Bluey, Reddy,
  Snipe, Ratcicle, Magmadon, Skeleton, Spike, Coco, Old School, copied from
  the game's two lists at the first opening. The page keeps its layout
  (player & 1) and the port keeps the real player: midasm hooks where the
  page's player is stored on opening (0x820E0088) and on Back (0x820E18FC),
  where Back now goes to the next player on foot (1 -> 2 -> 3 -> 4 -> 1). The
  "Goto Player N" text gets the real number.
- **Wear Skin for anyone** (`sub_82123880` rewritten with the game's own
  steps): Crash <-> Coco = the game's respawn of the players; another model of
  the same character = the game's in-place swap on the actor (its outfit
  package looked up by name, `sub_822E4000` with the name in r4); then the
  game's store (`sub_822665B0`: loads the new package, releases the old).
- **The preview follows the outfit, not the page.** The screen gave the
  preview Crash's animations on page 1 and Coco's on page 2. Coco on page 1
  then had no animations (null read in `sub_82351340`, a crash). The
  animation set (midasm hook at the controller's creation, 0x820E0AA8) and
  the random idle list (`sub_820E0C98`) now follow the shown outfit.

**The respawn and players 3-4's HUD.** With 3-4 players, the first
Crash <-> Coco pick crashed the game (read of address 0 in `sub_82268770`).
Each HUD display (`sub_82268660`, state at +148) notices that its player's
character changed, fades out, then binds the new one in state 5
(`sub_82268D60`: +128 = the character, +132 = a pointer to its health
variable), and state 1 reads through +132. Players 1-2's characters exist
the moment the respawn makes them; players 3-4's arrive a few frames later
through the port's spawn events, so their displays bound nothing and state 1
read address 0. Now a display of player 3-4 waits in state 5 while its
player has neither a character nor a titan (`more_players_hud.cpp`), and one
bound to nothing goes back to state 5. Reproduced and checked with the debug
console's `cheat respawn` (the same game call as the chest's) with four
players: crash before, none after, all four HUDs back.

**Coco for player 1** needs nothing else: player 1 respawns as Coco, moves,
jumps, attacks, gets her HUD portrait, and keeps her look through a level
change and a restart. The story's cutscenes and set pieces with player 1 as
Coco are still to be played through.

## 3. Crash in colours, and every outfit's hair

### 3.1 Crash's textures

Crash's package holds four textures: his two texture sets for the BODY
material (`crash_body_shader`): `Crash_Player1.bmp` (orange) and
`Crash_Player2.bmp` (Carbon), 512 x 512 DXT1, no mip levels; a normal map;
and `crash_fur_color.tga` for his HAIR, a material of its own
(`crash_hair_color_shader`, the spikes on his head and down his back): 256 x
256, 8-bit with a 256-entry 32-bit palette, dark red-brown spikes with
orange roots. The game swaps only the body's texture set, so its own Carbon
Crash has orange-rooted spikes on white fur.

### 3.2 The colours

Six coloured Crashes, for anyone at the chest: **Red, Yellow, Green, Blue,
Purple, Pink** (port numbers 12-17, in that order on the chest's pages from
red: 12 red, 13 blue, 14 green, 15 purple, 16 yellow, 17 pink; the white
Carbon Crash stays 11). They follow the many-coloured Crashes of the Crash
of the Titans short "Have Another". By default player 2 is still the game's
own white Carbon Crash, and players 3-4, who have no original look, are
Green and Purple (their markers' colours). `carbon_colours = false` hides
the colours and gives players 3-4 the white Carbon.

A data patch (`data/data_patcher.h`, category 10, Crash's package
`b06ecd92`) adds, made from the player's own disc data, per colour a body
texture `Crash_Player2_<colour>.bmp` and a hair texture
`crash_fur_<colour>.tga` (plus `crash_fur_carbon.tga` for the white Carbon).

The bodies are recoloured from the ORANGE texture: the first version tinted
the white Carbon texture, where fur and chest are nearly the same cream, so
the colour ran into the chest. In the orange texture the parts are apart:

| Part | Hue | Saturation | Value |
|---|---|---|---|
| fur | 22-27 | 0.88-0.92 | 0.94 |
| chest, muzzle, fingers | 36-37 | 0.54-0.58 | 1.0 |
| eyebrows, tattoos, the head's dark fur | 14-27 | 0.9 | 0.33-0.62 |
| paw pads | 44-45 | 0.29-0.38 | 0.21-0.59 |
| shoes, mouth | 355-359 | 0.7-0.85 | 0.34-0.55 |

One function of the colour alone maps every pixel (in DXT1, the two end
colours of each 4 x 4 block; when the new pair's order flips, the pair
swaps and the 2-bit choices are renumbered, as the order picks the block's
mode): only Crash's range takes part (hue 6-56, saturation above ~0.2, not
the paw pads), then DARK (value 0.62 -> 0.40) takes the colour's eyebrow
shade, or its tattoo shade inside the tattoo patch (the swirls of the
shoulder and back, pixels 275-410 x 15-135), CREAM (saturation 0.80 ->
0.58) its chest shade, and the rest its fur shade, each keeping the
texture's own shading. Jeans, shoes, eyes and teeth stay. The hair's
palette goes through the same function without the cream part.

The colours (hue, saturation, value of fur / chest / eyebrows / tattoos /
hair) were read from stills of the short, scaled for its washed-out video
(the original Crash there measures s 0.68 v 0.81 where his texture is s 0.88
v 0.95), then checked on the characters in the game. The yellow Crash's
tattoo stripes are blue-violet as in the short; the others' a deeper shade
of their fur.

A Crash names its body texture: the render behaviour keeps set 2's texture
name at +0x9C and binds it in its one-time set-up (slot 7, `sub_82185E60`);
before that runs, the name is replaced with the outfit's copy (the chest's
preview: the page's player), and the coloured outfits draw with set 2 like
Carbon. Because the texture is bound once, a change between two outfits of
that slot swaps the model in place onto Crash's own, the same step as for
Greeny, so the character is set up again with the new texture (the new
outfit already counts as the player's during that step). A respawn of the
players did the job too, but with partners on foot it brought player 1 back
as a mask.

![The six colours, Carbon Crash with its own hair, and the original](../images/outfits-crash-colours.jpg)

### 3.3 The hair

The hair material is ONE object shared by every Crash of the package, so
its texture can't be set once per character, and the game draws it from
inside each character's model draw: `sub_822B6FB8` (from CCharacterObject's
vtable 0x82041734 slots 6 and 8) and `sub_822B6DB0` (slot 11), r3 = the
CCharacterObject (= the render behaviour's +0x98), found from the call stack
at the hair's draw set-up. For the length of those two calls the hair
material's texture (`xnSimpleShader` +92, read by its draw set-up
`0x82441EA8` for every draw that uses it: a pddi handler) is the outfit's
copy, and it's given back after (+92 holds a reference: the material must
end up with its own texture). The textures are found the way the set-up
finds its body textures: `sub_820D8908(section, 64-bit name key)` -> a
tTexture, its pddiTexture at +0x18; the section is slot 7's r5, recorded per
CCharacterObject, and the outfit is recorded by the render behaviour's
Update. The first try hooked slots 7 and 8 themselves: the scene draws
through slot 6, and the hair kept its old texture. The orange Crash keeps the
original hair; the white Carbon gets cream roots and dusty-brown spikes.

## 4. Not done yet

- HUD portraits: the game has four (Crash, Crash as a mask, Coco, Coco as a
  mask) and picks them by character, so Carbon and the colours show Crash's
  face, as player 2's Carbon always did.
