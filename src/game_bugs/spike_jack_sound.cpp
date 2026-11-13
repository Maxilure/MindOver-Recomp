// =============================================================================
// game_bugs/spike_jack_sound.cpp -- Spike's jacking sound played twice (a bug of the original)
// =============================================================================
//
// THE BUG: jacking Spike (also Spike in the Hero levels) plays the jacking
// sound "ratcicle_jack_sfx" (bank "jacking_sfx") twice, 20 frames apart.
// Every other titan plays it once. The Xbox 360 release does the same: it is
// in the game's data, not in the port.
//
// WHERE IT IS (found 2026-11-13, findings/61): characters' moves are fight
// trees, fighttrees/<type>.bfig in default.rcf (data/fight_tree.h). Spike is
// a "Strong" titan: fighttrees/Strong.bfig. Each titan has a node
// "Strong<Name>" (record type 0x602941B8 = hash of "MasterContentGrapple")
// for the moment Crash jacks it; the node's lists of TRACKS run while the
// state plays (names are h*31+c hashes, decoded from strings in the exe):
//   0x04F3A980 enterTracks, 0x27DE7629 duringTracks, 0x0A870F46 exitTracks.
// A list = u32 kind, u32 byte size (counted from the next word), f32 item
// count, then the items one after another. Items seen in the jack lists:
//   0xBA1D3B64 the animation ("PTRN_spike_jack"), 0x73EE692F "Grapple",
//   0x2B4155FA "CharacterEffect" (start/end frame, effect, bone),
//   0x04C5D8EF "Sound" = CSoundTrack (vtable 0x820286FC; at run time it
//   makes a CSoundAction, vtable 0x82024DDC, whose slot 4 sub_820E24B0 starts
//   the sound): u32 type, f32 frame, f32 end (-1), three strings (u32 length
//   + the letters padded to 4 bytes): cue, bank, cue again, then four words
//   (0, a 0/1 flag, 0x14 = 20, the default the constructor sub_820FAED0
//   writes at +44, and 0).
// The sound items of the normal (not slow-motion) jack, per titan:
//   Battler  whoosh 22, jack sound 38          effects from 54
//   Phantom  whooshes 3, 8, jack sound 15      effects from 36
//   Ratcicle whoosh 10, teleport 24, jack 43   effects from 43
//   Shurtle  jack sound 30                     effects from 44
//   Sludge   whoosh 40, punch 42, jack 50      effects from 50
//   Spike    whooshes 3, 8, jack sound 10 AND 30   effects from 54
// Spike's list is the Phantom's (the same two whooshes at 3 and 8, a jack
// sound near 15) with the effects moved later for Spike's longer animation,
// like the Battler's (54). The jack sound was added again at 30 to follow,
// and the copied one at 10 stayed. (The slow-motion "first time" child
// state has one jack sound, at 60, for every titan, Spike too.)
//
// THE FIX: a data patch of Strong.bfig (data/data_patcher.h; the player's
// own file, changed at run time) takes the frame-10 item out of that list:
// list size minus the item's bytes, count minus 1. Records aren't size-
// prefixed and nothing points at their offsets (fight_tree.h), so the rest
// of the file just moves up. The patch checks the exact shape it expects
// (two jack sounds at 10 and 30, side by side) and otherwise leaves the
// file alone. --fix_spike_jack_sound=false = the original's double sound.
//
// HOW IT WAS CHECKED: --debug_sound_track_trace logs every sound a fight
// tree starts (sub_820E24B0 wrapped below); jacking a spawned Spike logged
// the jack sound (names ... 6C1651E9 = "jacking_sfx") at frames 10 and 30
// before, at 30 only after.
// =============================================================================

#include "game_bugs.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../data/data_patcher.h"
#include "../guest_memory.h"

REXCVAR_DEFINE_BOOL(fix_spike_jack_sound, true, "CrashMoM",
                    "Original game bug: Spike's jacking sound plays once (the original plays it twice)");
REXCVAR_DEFINE_BOOL(debug_sound_track_trace, false, "CrashMoM",
                    "Log every sound a fight tree starts (cue names; game_bugs/spike_jack_sound.cpp)");

namespace game_bugs {
namespace {

using Bytes = data_patcher::Bytes;

// Record and list type hashes (h*31+c of the names, see above).
constexpr uint32_t kMasterContentGrapple = 0x602941B8;
constexpr uint32_t kDuringTracks = 0x27DE7629;
constexpr uint32_t kSoundTrack = 0x04C5D8EF;

uint32_t Be32(const Bytes& d, size_t at) {
  return (uint32_t(d[at]) << 24) | (uint32_t(d[at + 1]) << 16) | (uint32_t(d[at + 2]) << 8) | d[at + 3];
}
void PutBe32(Bytes& d, size_t at, uint32_t v) {
  d[at] = uint8_t(v >> 24);
  d[at + 1] = uint8_t(v >> 16);
  d[at + 2] = uint8_t(v >> 8);
  d[at + 3] = uint8_t(v);
}
float BeFloat(const Bytes& d, size_t at) {
  const uint32_t bits = Be32(d, at);
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}
void PutBeFloat(Bytes& d, size_t at, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof(bits));
  PutBe32(d, at, bits);
}

// A "Sound" item at `at` (its type word) inside [.., end): its size in
// bytes and cue name, false if it doesn't parse.
struct SoundItem {
  size_t at = 0;
  size_t size = 0;
  float frame = 0;
  std::string cue;
};
bool ParseSound(const Bytes& d, size_t at, size_t end, SoundItem* item) {
  if (at + 12 > end || Be32(d, at) != kSoundTrack) return false;
  item->at = at;
  item->frame = BeFloat(d, at + 4);
  size_t p = at + 12;
  for (int s = 0; s < 3; ++s) {  // cue, bank, cue
    if (p + 4 > end) return false;
    const uint32_t length = Be32(d, p);
    if (length == 0 || length > 64 || p + 4 + length > end) return false;
    if (s == 0) item->cue.assign(reinterpret_cast<const char*>(&d[p + 4]), length);
    p += 4 + ((length + 3) & ~size_t(3));
  }
  p += 16;  // the four trailing words
  if (p > end) return false;
  item->size = p - at;
  return true;
}

// Strong.bfig: drop the frame-10 jack sound of Spike's jack list.
bool PatchStrongTree(const Bytes& original, Bytes* patched) {
  if (!REXCVAR_GET(fix_spike_jack_sound)) return false;
  static constexpr char kName[] = "StrongSpike";  // 11 letters + NUL = 12 bytes
  static constexpr std::string_view kAnimation = "PTRN_spike_jack";
  Bytes d = original;
  int fixed = 0;
  // Walk the records named StrongSpike from the END, so edits don't move
  // the ones still to look at.
  for (size_t i = d.size() >= 24 ? d.size() - 24 : 0; i-- > 0;) {
    if (i % 4 != 0 || Be32(d, i) != kMasterContentGrapple || Be32(d, i + 4) != 11 ||
        std::memcmp(&d[i + 8], kName, sizeof(kName)) != 0) {
      continue;
    }
    // Its duringTracks list, within the first lists after the name.
    size_t list = 0;
    for (size_t p = i + 20; p + 12 <= d.size() && p < i + 20 + 64; p += 4) {
      if (Be32(d, p) == kDuringTracks) {
        list = p;
        break;
      }
    }
    if (!list) continue;
    const uint32_t size = Be32(d, list + 4);
    const size_t body = list + 8;  // the size counts from here (count + items)
    const size_t end = body + size;
    if (end > d.size()) continue;
    // Only the jack list (its animation item names PTRN_spike_jack).
    const auto first = d.begin() + body;
    if (std::search(first, d.begin() + end, kAnimation.begin(), kAnimation.end()) == d.begin() + end) {
      continue;
    }
    std::vector<SoundItem> jack_sounds;
    for (size_t p = body + 4; p + 4 <= end; p += 4) {
      SoundItem item;
      if (ParseSound(d, p, end, &item) && item.cue == "ratcicle_jack_sfx") jack_sounds.push_back(item);
    }
    // Exactly the original's shape: frame 10 directly followed by frame 30.
    if (jack_sounds.size() != 2 || jack_sounds[0].frame != 10.0f || jack_sounds[1].frame != 30.0f ||
        jack_sounds[0].at + jack_sounds[0].size != jack_sounds[1].at) {
      REXLOG_WARN("Game bugs: Spike's jack list isn't the one expected ({} jack sounds); left alone",
                  jack_sounds.size());
      continue;
    }
    const SoundItem& stray = jack_sounds[0];
    d.erase(d.begin() + stray.at, d.begin() + stray.at + stray.size);
    PutBe32(d, list + 4, size - uint32_t(stray.size));
    PutBeFloat(d, body, BeFloat(d, body) - 1.0f);
    ++fixed;
  }
  if (!fixed) return false;
  REXLOG_INFO("Game bugs: Spike's second jacking sound removed ({} list{})", fixed, fixed == 1 ? "" : "s");
  *patched = std::move(d);
  return true;
}

}  // namespace

void Register() {
  data_patcher::Register("Spike's jacking sound once (original game bug)", data_patcher::kCategoryFightTree,
                         "Strong", &PatchStrongTree);
}

}  // namespace game_bugs

// -----------------------------------------------------------------------------
// --debug_sound_track_trace: CSoundAction slot 4 (vtable 0x82024DDC), the start
// of a sound a fight tree's "Sound" track asks for. this+8 = the CSoundTrack
// (48 bytes, vtable 0x820286FC), read from a running game (2026-11-13):
//   +8   u16 the item's frame x 16 (0x00A0 = frame 10)
//   +16, +20, +24  name hashes (h*31+c): +24 = the item's 2nd string, e.g.
//        0x6C1651E9 "jacking_sfx", 0x652DD367 "crash_light_whoosh"
//        (the jack sound items' 2nd string is the bank, the whooshes' the cue)
//   +44  u16 20 (the constructor's default)
// The tracks of one list sit 48 bytes apart, in the list's order.
// -----------------------------------------------------------------------------
namespace {
uint32_t Read32(uint8_t* base, uint32_t address) {
  const uint8_t* p = GuestPtr(base, address);
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
}  // namespace

extern "C" REX_FUNC(__imp__sub_820E24B0);
extern "C" REX_FUNC(sub_820E24B0) {
  if (REXCVAR_GET(debug_sound_track_trace)) {
    const uint32_t track = Read32(base, ctx.r3.u32 + 8);
    if (track) {
      const uint32_t frame16 = Read32(base, track + 8) >> 16;
      REXLOG_INFO("Sound track: start, track {:08X}, frame {:.2f}, names {:08X} {:08X} {:08X}", track,
                  frame16 / 16.0, Read32(base, track + 16), Read32(base, track + 20), Read32(base, track + 24));
    }
  }
  __imp__sub_820E24B0(ctx, base);
}
