// =============================================================================
// cheats/spawn.h -- the cheat menu's Spawn section: titans (and, untested,
// enemies) next to a player
// =============================================================================
//
// WHAT: pick a titan from the list (or type a template name), pick how many,
// and it appears beyond the chosen player (seen from the camera). Titans can
// come KNOCKED OUT: one hit puts them down, ready to jack (B). What the level
// hasn't loaded is loaded first (its package group, well under a second);
// villagers are refused (they crash this way); other templates by name are an
// experiment.
//
// HOW (findings/27 s.10):
//   * Everything in a level is made from a TEMPLATE named "<Group>:<Name>"
//     ("Characters:Ratcicle", "Collectables:c_mojoXL", "Villagers:RatcicleKid").
//     The game's creation function sub_82168620(template, actor name, 0,
//     position, 0, heap 8) takes a free one from the level's pool of ready-made
//     actors (sub_820B3998) or asks the scripts' GenerateObject to build one.
//     It is what brings a jacked titan along into the next level (the spawn
//     sub_8229C6F8 at 0x8229CA40). The actor is NAMED by the hash of the
//     template name (sub_82357020), like all of the game's: the name picks the
//     inventory section with its assets (see Spawn in spawn.cpp).
//   * KNOCKED OUT = ONE HIT AWAY FROM DOWN: a titan's CJackingBehaviour
//     (vtable 0x8202EF0C) keeps a stun meter (float at *(+40), maximum *(+44);
//     state +1104, 2 = stunned). After the titan's arrival we hold the meter at
//     0.5: the first hit knocks it down the game's own way (stars, the jack
//     prompt B). Putting it straight into the stunned state didn't work: its
//     AI only goes down from a hit (spawn.cpp, StunPending, has the tries).
//     Like a titan beaten in a fight, it gets up again after a while.
//   * The new actor needs its template's inventory section LOADED (spawning
//     without it crashed the game). Every template has one (a package group
//     of the same name); a section that isn't loaded in this level is loaded
//     first by taking a reference on it, then the spawn happens (Spawn,
//     CheckLoading in spawn.cpp). A name with no section is refused.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct PPCContext;

namespace spawn {

// One thing the menu offers.
struct Entry {
  std::string label;          // what the menu shows ("Ratcicle")
  std::string template_name;  // what the game calls it ("Characters:Ratcicle")
  bool titan = false;         // can be knocked out (ready to jack)
};
struct Category {
  std::string label;  // "Titans", "Enemies", ...
  std::vector<Entry> entries;
};
// The menu's list (fixed; names from the game's data, findings/27 s.10).
const std::vector<Category>& Catalogue();

// Queue a spawn (any thread): `count` of `template_name` in front of player
// `player` (0-3), titans stunned when `knocked_out`. Done at the next frame.
void Request(const std::string& template_name, int player, int count, bool knocked_out);

// The last spawn's outcome for the menu ("2 Ratcicle spawned", "... not in
// this level"), empty before any.
std::string LastResult();

// Game thread, once per frame (cheats.cpp's tick).
void Tick(PPCContext& ctx, uint8_t* base, bool in_level);

// Debug FIFO: "spawn <template> [player 1-4] [count] [ko]". False if malformed.
bool DebugCommand(std::string_view arguments);

}  // namespace spawn
