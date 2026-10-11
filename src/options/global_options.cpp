// =============================================================================
// options/global_options.cpp -- see global_options.h
// =============================================================================
#include "global_options.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

#include "../guest_memory.h"

REXCVAR_DEFINE_INT32(volume_music, 5, "CrashMoM",
                     "Music volume in stars, 0-5 (Options -> Audio; the same for every save)")
    .range(0, 5);
REXCVAR_DEFINE_INT32(volume_dialogue, 5, "CrashMoM",
                     "Dialogue volume in stars, 0-5 (Options -> Audio; the same for every save)")
    .range(0, 5);
REXCVAR_DEFINE_INT32(volume_sfx, 5, "CrashMoM",
                     "Sound effects volume in stars, 0-5 (Options -> Audio; the same for every save)")
    .range(0, 5);
REXCVAR_DEFINE_BOOL(invert_axis_player1, false, "CrashMoM",
                    "Player 1: up and down flipped when aiming the reticle (Options -> Controls)");
REXCVAR_DEFINE_BOOL(invert_axis_player2, false, "CrashMoM",
                    "Player 2: up and down flipped when aiming the reticle (Options -> Controls)");
REXCVAR_DEFINE_BOOL(invert_axis_player3, false, "CrashMoM",
                    "Player 3: up and down flipped when aiming the reticle (Options -> Controls)");
REXCVAR_DEFINE_BOOL(invert_axis_player4, false, "CrashMoM",
                    "Player 4: up and down flipped when aiming the reticle (Options -> Controls)");

// The game's volume setters (f1 = 0.0-1.0; r3 ignored: they find the audio
// object themselves) and the front end's saveable slots (global_options.h).
extern "C" REX_FUNC(__imp__sub_82266218);  // Music volume: set
extern "C" REX_FUNC(__imp__sub_82266268);  // Dialogue volume: set
extern "C" REX_FUNC(__imp__sub_822662B8);  // SFX volume: set
extern "C" REX_FUNC(__imp__sub_82262170);  // front end saveable: new game reset
extern "C" REX_FUNC(__imp__sub_822622F8);  // front end saveable: load from a save block
extern "C" REX_FUNC(__imp__sub_82262410);  // front end saveable: write into a save block

namespace global_options {
namespace {

// The audio settings object (sub_822D8130 returns it): +4 / +8 / +12 =
// music / dialogue / SFX buses, volume float at bus +36. Null before the
// audio starts.
constexpr uint32_t kAudioSettings = 0x8259B23C;
// Front end +8588 + controller slot: Invert Axis (1 = on), 4 bytes.
constexpr uint32_t kInvertAxis = 8588;

uint32_t Load32(uint8_t* base, uint32_t address) {
  const uint8_t* p = GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  uint8_t* p = GuestPtr(base, address);
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

}  // namespace

int Stars(Volume v) {
  switch (v) {
    case Volume::kMusic: return REXCVAR_GET(volume_music);
    case Volume::kDialogue: return REXCVAR_GET(volume_dialogue);
    case Volume::kSfx: return REXCVAR_GET(volume_sfx);
  }
  return 5;
}

const char* VolumeFlag(Volume v) {
  switch (v) {
    case Volume::kMusic: return "volume_music";
    case Volume::kDialogue: return "volume_dialogue";
    case Volume::kSfx: return "volume_sfx";
  }
  return "";
}

bool InvertAxis(int slot) {
  switch (slot) {
    case 0: return REXCVAR_GET(invert_axis_player1);
    case 1: return REXCVAR_GET(invert_axis_player2);
    case 2: return REXCVAR_GET(invert_axis_player3);
    case 3: return REXCVAR_GET(invert_axis_player4);
  }
  return false;
}

const char* InvertAxisFlag(int slot) {
  static const char* const kNames[] = {"invert_axis_player1", "invert_axis_player2",
                                       "invert_axis_player3", "invert_axis_player4"};
  return slot >= 0 && slot < 4 ? kNames[slot] : "";
}

void ApplyVolumes(PPCContext& ctx, uint8_t* base) {
  if (!Load32(base, kAudioSettings)) return;  // no audio yet: the set-up applies them later
  const PPCContext saved = ctx;
  const struct {
    Volume v;
    void (*set)(PPCContext&, uint8_t*);
  } kSetters[] = {{Volume::kMusic, __imp__sub_82266218},
                  {Volume::kDialogue, __imp__sub_82266268},
                  {Volume::kSfx, __imp__sub_822662B8}};
  for (const auto& s : kSetters) {
    ctx.f1.f64 = Stars(s.v) * 0.2;  // as the original screen: one star = 0.2
    s.set(ctx, base);
  }
  ctx = saved;
}

void ApplyInvertAxis(uint8_t* base, uint32_t fe) {
  if (!fe) return;
  for (int slot = 0; slot < 4; ++slot) {
    *GuestPtr(base, fe + kInvertAxis + slot) = InvertAxis(slot) ? 1 : 0;
  }
}

}  // namespace global_options

// -----------------------------------------------------------------------------
// Front end saveable, slot 0: a new game's reset (r3 = front end + 4).
// 360: the progress AND the volumes back to full. PC: the progress as before,
// the volumes stay the player's.
// -----------------------------------------------------------------------------
extern "C" REX_FUNC(sub_82262170) {
  const uint32_t fe = ctx.r3.u32 - 4;
  __imp__sub_82262170(ctx, base);
  global_options::ApplyVolumes(ctx, base);
  global_options::ApplyInvertAxis(base, fe);
}

// -----------------------------------------------------------------------------
// Front end saveable, slot 1: loading a save (r3 = front end + 4, r4 = its
// part of the save block). 360: the save's volumes and Invert Axis come back
// with it. PC: the rest of the block as before, then the settings go back.
// -----------------------------------------------------------------------------
extern "C" REX_FUNC(sub_822622F8) {
  const uint32_t fe = ctx.r3.u32 - 4;
  __imp__sub_822622F8(ctx, base);
  global_options::ApplyVolumes(ctx, base);
  global_options::ApplyInvertAxis(base, fe);
  REXLOG_INFO("Options: the save's own volumes and Invert Axis ignored (user/settings.toml's kept)");
}

// -----------------------------------------------------------------------------
// Front end saveable, slot 2: saving (r3 = front end + 4, r4 = its part of
// the save block). 360: block +0 / +4 / +8 = Music / Dialogue / SFX volume
// (floats), +12 = flags (bits 0-3 Invert Axis per controller slot, bit 4 the
// save handler's "device chosen"), then the progress. PC: the settings are
// NOT saved: the volumes are written as 1.0 and the Invert Axis bits as off,
// the game's own New Game values (= "no setting"). The bytes stay: the block
// is a fixed-size image read at fixed positions, so cutting them would shift
// everything after them and break every existing save (and the 360 / Xenia
// format). Bit 4 isn't a setting and stays as the game wrote it.
// -----------------------------------------------------------------------------
extern "C" REX_FUNC(sub_82262410) {
  const uint32_t block = ctx.r4.u32;
  __imp__sub_82262410(ctx, base);
  constexpr uint32_t kOne = 0x3F800000;  // 1.0f
  global_options::Store32(base, block + 0, kOne);
  global_options::Store32(base, block + 4, kOne);
  global_options::Store32(base, block + 8, kOne);
  global_options::Store32(base, block + 12, global_options::Load32(base, block + 12) & ~0xFu);
}
