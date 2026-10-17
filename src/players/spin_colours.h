// =============================================================================
// players/spin_colours.h -- each coloured Crash spins in his own colour
// =============================================================================
//
// THE GAME (docs/findings/46 s.4): the spin attack and the spin jump are
// EFFECTS (Pure3D effect sets in the always-loaded package bca1bc0b), started
// by Crash's fight tree: a CharacterEffect track (type hash of
// "CharacterEffect" = 0x2B4155FA in Crash.bfig) names the effect and the
// joint it rides; CCharacterEffectAction's start (vtable 0x820221D4 slot 4,
// sub_820C47B8) spawns the effect the track names (its name key at track
// +0xC) on the action's character (action +0xC -> +0x10). Crash's are warm:
// yellow / orange sparks, an orange glow; Coco has her own, purple
// (cha_fx_coco_*, her own package).
//
// THE PORT: a data patch copies every chunk of Crash's spin effects once per
// coloured Crash (players/outfits.h, 12-17) under SAME-LENGTH names
// ("crash_spin" -> "crshN_spin", "Crash_Spin" -> "CrshN_Spin",
// "crash_coopswitch1" -> "crshN_coopswitch1", N = 1-6), so no name field
// changes size, and turns their warm colours toward the Crash's: the meshes'
// vertex colours (0x10008), the sparks' colour keys (0x121109, colour over
// life and start colour) and the glow texture (Crash_Spin.tga: PNG pictures).
// The blue parts stay (Coco's effect has the same blue). When a coloured
// Crash's action starts one of those effects, the track names the copy for
// the length of the call. Orange Crash, the white Carbon and Coco keep the
// game's own.
// =============================================================================
#pragma once

namespace spin_colours {

// Registers the data patch and nothing else (the spawn hook is a function
// override). Call before the game runs (CrashMomApp).
void Register();

}  // namespace spin_colours
