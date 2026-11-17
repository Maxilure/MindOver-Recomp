// =============================================================================
// net/online_sticks.cpp -- THE STICK IS MEANT IN ITS OWN PLAYER'S CAMERA
// (online plan s.6.2, research R9; milestone M4's first piece, 2026-11-17)
// =============================================================================
//
// HOW THE GAME TURNS A STICK INTO A DIRECTION (found 2026-11-17): every "which
// way does the player push?" question (the fight trees' walk, run, jump, aim,
// the script methods GetSelfRelativeInputDirection & co., ~20 callers) goes
// through the input controller's sub_82272208 -> sub_82271E38(controller,
// out, f1 = stick x, f2 = stick y). That one reads the CAMERA:
//   * normally the game camera, *(*(0x8259B190) + 76) + 444 (the VectorCamera),
//     its virtual slot 14 (+56) -> a record whose +32/+36/+40 = its forward;
//   * while the controller's +520 top bit is set: a forward it HELD (+464 /
//     +472), so a camera cut doesn't swing the stick's meaning;
// stores x (r1+88) and z (r1+96) at 0x82271F40..54 (y zeroed right after),
// normalises it, builds a turn from it (sub_822EBE08) and turns the stick.
//
// WHY THAT MATTERS ONLINE: today every PC shows the same camera, so every PC
// turns every stick the same way. With a camera per PC (M4) the host's camera
// follows player 1 and the guest's player 2: the same stick of player 2 would
// mean "left" on one PC and "back" on the other, and the worlds part at once.
//
// THE FIX: a stick is meant in the camera of the PC it was pushed on. Each PC
// notes the forward ITS game used for ITS OWN player (at 0x82271F58, the
// point both paths join: the held one included) and sends it with its next
// input (net_session: 16-bit facing, 65536 steps a turn). Every PC then turns
// player P's stick with the facing P's PC sent for this frame. The world never
// asks the local camera about movement again. (The facing rides `delay` + 1
// frames behind its camera: a camera turns slowly, the difference can't be felt.)
// Until a PC's first facing arrives (the session's first frames), the local
// camera stays in charge (the same on every PC as long as the cameras are).
//
// Out of a session nothing changes. Debug: --debug_net_facing_offset=<deg>
// turns THIS PC's sent facing (tests: that PC's player then walks turned on
// EVERY PC, still in sync).
// =============================================================================

#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../guest_memory.h"
#include "net_session.h"
#include "online_rules.h"

REXCVAR_DEFINE_INT32(debug_net_facing_offset, 0, "CrashMoM",
                     "Debug: online, turn the camera facing this PC sends with its stick by this many degrees "
                     "(tests that every PC turns a stick by its sender's camera)");

namespace {

constexpr double kTurn = 6.283185307179586;
constexpr uint32_t kStoredX = 88, kStoredZ = 96;  // r1 + 88 / + 96: the forward's x and z

uint8_t* Base() { return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(0); }

float ReadFloat(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, GuestPtr(base, a), 4);
  v = __builtin_bswap32(v);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void WriteFloat(uint8_t* base, uint32_t a, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  v = __builtin_bswap32(v);
  std::memcpy(GuestPtr(base, a), &v, 4);
}

uint16_t FacingOf(float x, float z) {
  double turn = std::atan2(double(z), double(x)) / kTurn;  // -0.5 .. 0.5
  turn += double(REXCVAR_GET(debug_net_facing_offset)) / 360.0;
  turn -= std::floor(turn);                                // 0 .. 1
  return uint16_t(uint32_t(std::lround(turn * 65536.0)) & 0xFFFF);
}

// cos / sin of facing steps, the SAME BITS on every PC: the C library's
// std::cos may differ in the last bit between systems (Linux / Windows =
// milestone M3), and the result goes into the world. Plain IEEE double
// operations only (no fused multiply-add: contraction off), angle reduced to
// one eighth of a turn, Taylor series (measured error 2e-14 over all 65536
// facings: far below a float's).
#pragma clang fp contract(off)
void CosSin(uint16_t facing, double& c, double& s) {
  const uint32_t octant = facing >> 13;          // 0..7
  const int32_t rest = int32_t(facing & 0x1FFF);  // 0..8191 steps into it
  // Angle from the nearest multiple of a quarter turn: [-1/8, 1/8] turn.
  const int32_t quarter = int32_t((octant + 1) >> 1) & 3;
  const int32_t from_quarter = (octant & 1) ? rest - 8192 : rest;
  const double x = double(from_quarter) * (kTurn / 65536.0);
  const double x2 = x * x;
  // Taylor series in nested form: sin to x^13, cos to x^14.
  const double sn = x * (1.0 - x2 / 6.0 * (1.0 - x2 / 20.0 * (1.0 - x2 / 42.0 * (1.0 - x2 / 72.0 *
                    (1.0 - x2 / 110.0 * (1.0 - x2 / 156.0))))));
  const double cs = 1.0 - x2 / 2.0 * (1.0 - x2 / 12.0 * (1.0 - x2 / 30.0 * (1.0 - x2 / 56.0 *
                    (1.0 - x2 / 90.0 * (1.0 - x2 / 132.0 * (1.0 - x2 / 182.0))))));
  switch (quarter) {
    case 0: c = cs; s = sn; break;
    case 1: c = -sn; s = cs; break;
    case 2: c = -cs; s = -sn; break;
    default: c = sn; s = -cs; break;
  }
}
#pragma clang fp contract(on)

}  // namespace

// Codegen declares `extern void OnlineStickFacing(PPCRegister& r1, PPCRegister& r28);`
// (manifest: 0x82271F58, inside sub_82271E38; r28 = the input controller).
void OnlineStickFacing(PPCRegister& r1, PPCRegister& r28) {
  if (!net::InSession()) return;
  uint8_t* base = Base();
  const int player = online_rules::PlayerOfController(base, r28.u32);
  if (player < 0) {
    static bool told = false;
    if (!told) {
      told = true;
      REXLOG_WARN("Online: a stick read by controller {:08X} that belongs to no player: turned by this PC's "
                  "camera (logged once)", r28.u32);
    }
    return;
  }
  const uint32_t sp = r1.u32;
  if (player == net::LocalPlayer()) {
    const float x = ReadFloat(base, sp + kStoredX), z = ReadFloat(base, sp + kStoredZ);
    if (std::isfinite(x) && std::isfinite(z) && (x != 0.0f || z != 0.0f)) {
      net::NoteLocalFacing(FacingOf(x, z));
    }
  }
  uint16_t facing = 0;
  if (!net::FrameFacing(player, facing)) return;  // none sent yet: this PC's camera
  // Exactly what every PC computes from the same 16 bits (the game normalises
  // the vector next).
  double c = 0, sn = 0;
  CosSin(facing, c, sn);
  WriteFloat(base, sp + kStoredX, float(c));
  WriteFloat(base, sp + kStoredZ, float(sn));
}
