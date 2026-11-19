// =============================================================================
// game_bugs/held_props.cpp -- a held prop swings out of the hand (a bug of the original)
// =============================================================================
//
// THE BUG: Coco on Wumpa Island (the villager near the big blue pipe) holds a
// wrench. When Crash walks up to her and the talk prompt shows, she plays her
// "noticed you" animation and the wrench leaves her hand: it turns about
// 100 degrees and ends up behind her shoulder. Talking to her (the
// conversation's own animation) or walking away (her idle) puts it back. The
// same happens in the emulated picture, at 30 fps, and with every native
// replacement off: it is in the game's data, not in the port.
//
// WHERE IT IS (found 2026-11-19, findings/61 s.3):
// * The wrench is its own actor (model npc_pr_cocoWrenchShape, package
//   7a175631, a one-joint skeleton). Coco carries it with a
//   CAccessoryBehaviour (vtable 0x8202B88C, a CAttacherBehaviour): its
//   message handler sub_82126510, on Coco's transform message (type 39),
//   builds the wrench's matrix from Coco's POSE (world matrix of joint
//   *(acc +44), pose = *(*(acc +48))) times Coco's own matrix, and sends it
//   to the wrench (type 4 sub 2 -> sub_82125610 -> sub_82125960). That
//   joint is 40 "Prop_Root", a child of 31 "Wrist_R" (skeleton
//   "cocovillager", 57 joints, package 6e2be690).
// * Measured with the console: the wrench sits on Prop_Root in both states
//   (within a frame), and Prop_Root stays on the wrist. What changes is
//   Prop_Root's LOCAL rotation: the idle animation keys it at
//   (0.981, -0.017, 0.192, 0.034); near Crash it is (0.685, 0.230, 0.304,
//   -0.621) = exactly the skeleton's REST pose (its matrix matches the rest
//   matrix in the skeleton record to 3 decimals). The wrist itself barely
//   moves.
// * The animation writer sub_823A24F8 (r3 = the animation driver, pose at
//   +0x104; f2 = the driver's blend weight; r7 = the per-joint channel
//   table, 24 bytes per joint: +0 rotation channel, +4 translation channel,
//   then scale channels) blends each joint toward the animation's value,
//   and a joint with NO channel toward the skeleton's rest pose. Coco's
//   "noticed you" animation has no Prop_Root channel (an animation made
//   without the prop), so the wrench's joint is pulled to rest.
//   (CHeadTrackingBehaviour on Coco, the first suspect, is idle here: both
//   its JointPointingLayers are off.)
//
// THE FIX: for a joint some accessory hangs a prop on, an animation with
// neither a rotation nor a translation channel for that joint leaves the
// joint as it was (its value from the earlier drivers of this frame, or the
// last frame's): sub_823A24F8 is wrapped, the joint's local rotation (16
// bytes at local base + 16 j) and translation (12 bytes at local base +
// 16 n + 12 j, n = joint count) are saved before and put back after. The
// prop keeps the last pose an animation gave it, in the hand. Animations
// that DO key the joint (the idle, the conversation) are untouched, and
// joints nobody hangs anything on keep the original behaviour.
// Which joints: the accessory handler sub_82126510 is wrapped too and notes
// (pose, joint) every time it runs (every frame for a carried prop); an
// entry not seen for 2 s is forgotten (the character left, its pose memory
// may be reused).
// --fix_held_props=false = the original (the wrench swings out).
//
// HOW IT WAS CHECKED: photos of Coco from the same spot, away and near, with
// the wrench lifted 1 unit by a temporary probe to find it; the console's
// pose reads (Prop_Root local and world, the wrench's matrix) before and
// after.
// =============================================================================

#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "guest_memory.h"

REXCVAR_DEFINE_BOOL(fix_held_props, true, "CrashMoM",
                    "A prop a character holds (Coco's wrench) stays in the hand during animations "
                    "made without it (the original swings it out of the hand); off = the original");

extern "C" REX_FUNC(__imp__sub_82126510);  // CAccessoryBehaviour message handler
extern "C" REX_FUNC(__imp__sub_823A24F8);  // animation driver -> pose (per-joint blend)

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kNoJoint = 0xFFFF;          // the game's "joint not found"
constexpr uint32_t kChannelStride = 24;        // channel table entry per joint (r7)
constexpr auto kForgetAfter = std::chrono::seconds(2);

// One prop joint: the pose an accessory reads, the joint it hangs its prop on.
struct PropJoint {
  uint32_t pose;
  uint32_t joint;
  Clock::time_point seen;
};

std::mutex g_mutex;
std::vector<PropJoint> g_joints;  // a handful at most (carried props near the player)
bool g_logged_once = false;

uint32_t Load32(uint8_t* base, uint32_t address) {
  const uint8_t* p = GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// Reads a word only from memory that is really there (a stale pointer from
// a freed object must never fault the game).
bool SafeLoad32(uint8_t* base, uint32_t address, uint32_t& out) {
  if (!address || !guest_memory::Readable(address, 4)) return false;
  out = Load32(base, address);
  return true;
}

// The accessory's (pose, joint), as its own code reads them (sub_82126510 at
// 0x821267C4: pose = *(*(this +48)), joint = *(this +44)).
void NoteAccessory(uint8_t* base, uint32_t accessory) {
  uint32_t holder = 0, pose = 0, joint = 0;
  if (!SafeLoad32(base, accessory + 48, holder) || !SafeLoad32(base, holder, pose) ||
      !SafeLoad32(base, accessory + 44, joint) || !pose || joint >= kNoJoint) {
    return;
  }
  const Clock::time_point now = Clock::now();
  std::lock_guard<std::mutex> lock(g_mutex);
  for (PropJoint& j : g_joints) {
    if (j.pose == pose && j.joint == joint) {
      j.seen = now;
      return;
    }
  }
  g_joints.push_back({pose, joint, now});
}

}  // namespace

// CAccessoryBehaviour's message handler (vtable 0x8202B88C slot 5): only
// notes which joint carries a prop, then the original as always.
extern "C" REX_FUNC(sub_82126510) {
  if (REXCVAR_GET(fix_held_props)) NoteAccessory(base, ctx.r3.u32);
  __imp__sub_82126510(ctx, base);
}

// The animation writer: prop joints this animation has no channel for keep
// their value (see the header).
extern "C" REX_FUNC(sub_823A24F8) {
  if (!REXCVAR_GET(fix_held_props)) {
    __imp__sub_823A24F8(ctx, base);
    return;
  }
  const uint32_t driver = ctx.r3.u32;
  const uint32_t channels = ctx.r7.u32;
  uint32_t pose = 0;
  if (!SafeLoad32(base, driver + 0x104, pose) || !pose || !channels) {
    __imp__sub_823A24F8(ctx, base);
    return;
  }

  // Up to a few joints of this pose to keep (normally none or one).
  struct Kept {
    uint32_t rot_address, trans_address;
    uint8_t rot[16], trans[12];
  };
  Kept kept[4];
  int kept_count = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const Clock::time_point now = Clock::now();
    for (size_t i = 0; i < g_joints.size();) {
      if (now - g_joints[i].seen > kForgetAfter) {
        g_joints[i] = g_joints.back();
        g_joints.pop_back();
        continue;
      }
      const PropJoint& j = g_joints[i++];
      if (j.pose != pose || kept_count == 4) continue;
      uint32_t info = 0, locals = 0, rot_channel = 1, trans_channel = 1;
      if (!SafeLoad32(base, pose + 0xC, info) || !SafeLoad32(base, pose + 0x1C, locals)) continue;
      const uint32_t joint_count = info >> 21;  // pose +0xC bits 21-31
      if (j.joint >= joint_count) continue;
      const uint32_t entry = channels + j.joint * kChannelStride;
      if (!SafeLoad32(base, entry, rot_channel) || !SafeLoad32(base, entry + 4, trans_channel)) continue;
      if (rot_channel || trans_channel) continue;  // this animation keys the joint: original
      Kept& k = kept[kept_count];
      k.rot_address = locals + 16 * j.joint;
      k.trans_address = locals + 16 * joint_count + 12 * j.joint;
      if (!guest_memory::Readable(k.rot_address, 16) || !guest_memory::Readable(k.trans_address, 12)) {
        continue;
      }
      std::memcpy(k.rot, GuestPtr(base, k.rot_address), 16);
      std::memcpy(k.trans, GuestPtr(base, k.trans_address), 12);
      ++kept_count;
    }
  }

  __imp__sub_823A24F8(ctx, base);

  for (int i = 0; i < kept_count; ++i) {
    std::memcpy(GuestPtr(base, kept[i].rot_address), kept[i].rot, 16);
    std::memcpy(GuestPtr(base, kept[i].trans_address), kept[i].trans, 12);
  }
  if (kept_count && !g_logged_once) {
    g_logged_once = true;
    REXLOG_INFO("Held props: an animation without the prop's joint left it in the hand "
                "(pose {:08X}; logged once)", pose);
  }
}
