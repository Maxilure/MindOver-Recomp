// =============================================================================
// options/global_options.h -- the game's own options (volumes, Invert Axis)
// kept in user/settings.toml for the whole game, no longer in save files
// =============================================================================
//
// THE ORIGINAL (traced 2026-10-11): the front end is one of the game's
// "saveable" objects (its second vtable, 0x8203AD4C, `this` = front end + 4):
//   slot 0  sub_82262170  new game reset: Music / Dialogue / SFX volume = 1.0
//                         (+ objectives, map, combo...: the game's progress)
//   slot 1  sub_822622F8  load from a save block: 3 floats (Music, Dialogue,
//                         SFX), then a flags word: bits 0-3 = Invert Axis per
//                         controller (front end +8588..8591), bit 4 = the
//                         save handler's "device chosen" (handler +140 0x10)
//   slot 2  sub_82262410  write the same into a save block
// So every save carries its own volumes and Invert Axis: loading a save
// turned the music back up, New Game reset everything to full. On the 360
// that was per profile in practice; on a PC it reads as "my settings keep
// changing".
//
// THE PORT: the values live in user/settings.toml (flags volume_music,
// volume_dialogue, volume_sfx = 0-5 stars, invert_axis_player1-4), set by
// Options -> Audio / Controls or the launcher's Settings tab. Slots 0 and 1
// run as before (the game's progress), then the settings go back over
// whatever the save said (saves made before this still hold their own).
// Slot 2 no longer saves them: the volumes are written as 1.0 and the Invert
// Axis bits as off (the game's New Game values = "no setting"; the bytes
// stay, the block is a fixed-size image, see global_options.cpp).
// The front end's set-up (who_plays.cpp, sub_82266398) applies them at boot.
// =============================================================================
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace global_options {

enum class Volume { kMusic, kDialogue, kSfx };

// The setting, in stars (0-5; one star = 0.2 of full volume).
int Stars(Volume v);
// The flag's name (for options_settings' SetFlag + settings.toml).
const char* VolumeFlag(Volume v);

// Invert Axis of a controller slot (0-3, the front end's byte index).
bool InvertAxis(int slot);
const char* InvertAxisFlag(int slot);

// Puts the settings into the game: the three volumes (the audio object
// *(0x8259B23C) must exist: skipped if not) / the 4 Invert Axis bytes of
// front end `fe`.
void ApplyVolumes(PPCContext& ctx, uint8_t* base);
void ApplyInvertAxis(uint8_t* base, uint32_t fe);

}  // namespace global_options
