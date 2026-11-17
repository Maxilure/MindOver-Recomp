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
// reads its camera's facing at the start of every frame (CameraFacing) and
// sends it with that frame's input (net_session: 16-bit facing, 65536 steps a
// turn). Every PC then turns
// player P's stick with the facing P's PC sent for this frame. The world never
// asks the local camera about movement again. (The facing is the camera of
// the frame the input was taken on, `delay` frames before it plays: a camera
// turns slowly, the difference can't be felt.)
// Until a PC's first facing arrives (the session's first frames), the local
// camera stays in charge (the same on every PC as long as the cameras are).
//
// Out of a session nothing changes. Debug: --debug_net_facing_offset=<deg>
// turns THIS PC's sent facing (tests: that PC's player then walks turned on
// EVERY PC, still in sync).
// =============================================================================

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <fmt/format.h>
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

namespace online_sticks {
// This PC's camera facing on the ground, read straight from the game camera
// (VectorCamera +228 / +236: the direction sub_822718E8 reads too). Sent with
// EVERY input, not only while the stick is pushed: the conversion below skips
// the camera while the stick is centred, and the first frames of a push would
// otherwise go out without a facing and be turned by each PC's own camera
// (found 2026-11-17: player 2's first step half a degree apart on the two PCs).
bool CameraFacing(uint16_t& facing) {
  uint8_t* base = Base();
  constexpr uint32_t kGameGlobal = 0x8259B190, kCameraHolder = 76, kCamera = 444, kDirection = 228;
  auto load = [&](uint32_t a, uint32_t& out) {
    if (!guest_memory::Readable(a, 4)) return false;
    uint32_t v;
    std::memcpy(&v, GuestPtr(base, a), 4);
    out = __builtin_bswap32(v);
    return out != 0;
  };
  uint32_t game = 0, holder = 0, camera = 0;
  if (!load(kGameGlobal, game) || !load(game + kCameraHolder, holder) || !load(holder + kCamera, camera) ||
      !guest_memory::Readable(camera + kDirection, 12)) {
    return false;
  }
  const float x = ReadFloat(base, camera + kDirection), z = ReadFloat(base, camera + kDirection + 8);
  if (!std::isfinite(x) || !std::isfinite(z) || (x == 0.0f && z == 0.0f)) return false;
  facing = FacingOf(x, z);
  return true;
}
}  // namespace online_sticks

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
  uint16_t facing = 0;
  if (!net::FrameFacing(player, facing)) return;  // none sent yet: this PC's camera
  // Exactly what every PC computes from the same 16 bits (the game normalises
  // the vector next).
  double c = 0, sn = 0;
  CosSin(facing, c, sn);
  WriteFloat(base, sp + kStoredX, float(c));
  WriteFloat(base, sp + kStoredZ, float(sn));
}

// THE HELD FORWARD (found 2026-11-17 by the own-camera experiment: the worlds
// still parted the moment a player walked). sub_822718E8, called first by
// sub_82271E38, keeps "the forward the stick means" steady through camera
// cuts: it reads the game camera's direction (VectorCamera +228/+232/+236 ->
// r1+88..+96 at 0x82271914..28) and
//   * while the stick is let go (or nothing is held yet): copies it into the
//     controller's held forward (+464/+468/+472);
//   * when the camera turns away from the held forward while the stick stays
//     put: sets the hold bit (+520 0x80), stores the stick (+476/+480) and
//     sends the controller an event (+504);
//   * clears the hold once the stick moves.
// All decided from THIS PC's camera = world state that differs per PC. Here
// (0x8227192C, right before it normalises the copy; r31 = the controller) the
// camera's direction is replaced by the facing the controller's player's PC
// sent, flat (y 0): every PC holds, releases and sends the event alike.
void OnlineHeldFacing(PPCRegister& r1, PPCRegister& r31) {
  if (!net::InSession()) return;
  uint8_t* base = Base();
  const int player = online_rules::PlayerOfController(base, r31.u32);
  if (player < 0) return;
  uint16_t facing = 0;
  if (!net::FrameFacing(player, facing)) return;
  double c = 0, sn = 0;
  CosSin(facing, c, sn);
  const uint32_t sp = r1.u32;
  WriteFloat(base, sp + 88, float(c));
  WriteFloat(base, sp + 92, 0.0f);
  WriteFloat(base, sp + 96, float(sn));
}


// =============================================================================
// A PLAYER'S OWN CAMERA FOR WHAT BELONGS TO THEM (2026-11-17, own-camera
// experiment, round 4). Some game code works out something of ONE player from
// the camera: the aim of a mask (CReticleAimingBehaviour: a ray from the
// camera through the player's 2D reticle spot, sub_822AC668), and with it the
// facing the mask turns to on its host (sub_822380E0 asks it). With a camera
// per PC every PC would aim player P differently (found: a masked player 1
// turned a little differently on its host on each PC, frame 1307).
// Each input carries its PC's whole camera POSE (CameraPose: position,
// direction, up, field of view, the game camera's own words). While such code
// runs for player P, the game camera holds the pose P's PC sent: its bytes
// +16..+251 (projection values, the two matrices at +80 / +144, the "matrices
// current" byte +208, position / direction / up) are saved, the pose is
// written, +208 cleared (the camera's getters rebuild the matrices from it),
// and afterwards the saved bytes go back exactly: this PC's view is untouched.
// =============================================================================
namespace online_sticks {
namespace {
constexpr uint32_t kGameGlobal = 0x8259B190, kCameraHolder = 76, kCameraField = 444;
constexpr uint32_t kSaveFrom = 16, kSaveSize = 252 - 16;   // camera bytes +16..+251
constexpr uint32_t kPose = 216, kFov = 16, kCurrent = 208;  // position/direction/up; field of view; matrices current

uint32_t GameCamera() {
  uint8_t* base = Base();
  auto load = [&](uint32_t a, uint32_t& out) {
    if (!guest_memory::Readable(a, 4)) return false;
    uint32_t v;
    std::memcpy(&v, GuestPtr(base, a), 4);
    out = __builtin_bswap32(v);
    return out != 0;
  };
  uint32_t game = 0, holder = 0, camera = 0;
  if (!load(kGameGlobal, game) || !load(game + kCameraHolder, holder) || !load(holder + kCameraField, camera) ||
      !guest_memory::Readable(camera + kSaveFrom, kSaveSize)) {
    return 0;
  }
  return camera;
}

uint32_t Word(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, GuestPtr(base, a), 4);
  return __builtin_bswap32(v);
}
void SetWord(uint8_t* base, uint32_t a, uint32_t w) {
  w = __builtin_bswap32(w);
  std::memcpy(GuestPtr(base, a), &w, 4);
}

struct Swap {
  bool active = false;
  uint32_t camera = 0;
  uint8_t saved[kSaveSize] = {};
} g_swap;
bool g_in_aim = false;
}  // namespace

bool CameraPose(uint32_t words[10]) {
  const uint32_t camera = GameCamera();
  if (!camera) return false;
  uint8_t* base = Base();
  for (uint32_t k = 0; k < 9; ++k) words[k] = Word(base, camera + kPose + 4 * k);
  words[9] = Word(base, camera + kFov);
  return true;
}

// Player `player`'s camera in the game camera until EndPlayerCamera. False =
// not online, nothing sent for them yet, or a swap is already on.
bool BeginPlayerCamera(int player) {
  if (g_swap.active || !net::InSession()) return false;
  uint32_t words[10];
  if (!net::FrameCamera(player, words)) return false;
  const uint32_t camera = GameCamera();
  if (!camera) return false;
  uint8_t* base = Base();
  std::memcpy(g_swap.saved, GuestPtr(base, camera + kSaveFrom), kSaveSize);
  for (uint32_t k = 0; k < 9; ++k) SetWord(base, camera + kPose + 4 * k, words[k]);
  SetWord(base, camera + kFov, words[9]);
  *GuestPtr(base, camera + kCurrent) = 0;
  g_swap.camera = camera;
  g_swap.active = true;
  return true;
}

void EndPlayerCamera() {
  if (!g_swap.active) return;
  std::memcpy(GuestPtr(Base(), g_swap.camera + kSaveFrom), g_swap.saved, kSaveSize);
  g_swap.active = false;
}

bool InAim() { return g_in_aim; }

// The aim update learned whose aim it is (players/reticle_idle.cpp's
// sub_822AD0A0 wrapper: the first thing the update does, before it reads the
// camera).
void OnAimPlayer(int player) {
  if (g_in_aim && player >= 0) BeginPlayerCamera(player);
}
}  // namespace online_sticks

// CReticleAimingBehaviour's aim update (r3 = its aim state): the aim ray and
// target search run with the aiming player's camera online (above).
extern "C" REX_FUNC(__imp__sub_822AC668);
extern "C" REX_FUNC(sub_822AC668) {
  if (!net::InSession()) {
    __imp__sub_822AC668(ctx, base);
    return;
  }
  online_sticks::g_in_aim = true;
  __imp__sub_822AC668(ctx, base);
  online_sticks::g_in_aim = false;
  online_sticks::EndPlayerCamera();
}

// THE STICK'S DIRECTION SECTOR (2026-11-17, own-camera experiment round 7:
// player 2's facing differed per PC as a light attack started, 2.6 minutes
// into a random two-player script). sub_8221DD70 (9 calls from the compiled
// fight trees, sub_8219E228: which way an attack / move goes) takes the RAW
// stick (sub_82271D20), Crash's backward vector (-forward of *(r31+156)+44)
// and the game CAMERA's direction (+228, passed in r5 to sub_822E7F30 at
// 0x8221DE00) and answers a direction sector. Online the camera direction is
// the one the actor's player's PC sent (its camera pose words 3-5).
#include "../players/more_players.h"
namespace {
uint32_t g_sent_direction = 0;  // guest buffer: 3 floats
}
void OnlineStickSector(PPCRegister& r5, PPCRegister& r31) {
  if (!net::InSession()) return;
  uint8_t* base = Base();
  if (!guest_memory::Readable(r31.u32 + 156, 4)) return;
  uint32_t v;
  std::memcpy(&v, GuestPtr(base, r31.u32 + 156), 4);
  const uint32_t actor = __builtin_bswap32(v);
  int player = -1;
  for (int p = 0; p < 4 && player < 0; ++p) {
    if (actor && (actor == more_players::CharacterOfPlayer(p) || actor == more_players::TitanOfPlayer(p))) player = p;
  }
  static int told = 0;
  if (told < 3) {
    ++told;
    REXLOG_INFO("Online: stick sector asked for actor {:08X} = player {}", actor, player + 1);
  }
  if (player < 0) return;
  uint32_t words[10];
  if (!net::FrameCamera(player, words)) return;
  if (!g_sent_direction) g_sent_direction = rex::system::kernel_memory()->SystemHeapAlloc(16);
  if (!g_sent_direction) return;
  for (uint32_t k = 0; k < 3; ++k) {
    const uint32_t w = __builtin_bswap32(words[3 + k]);  // direction = pose words 3..5
    std::memcpy(GuestPtr(base, g_sent_direction + 4 * k), &w, 4);
  }
  r5.u64 = g_sent_direction;
}

// THE HELD FORWARD AT AN UNLOCK (2026-11-17, own-camera round 8). When a
// controller's lock count (+516) drops (the level's fade-in, a cutscene, a
// menu) and a camera condition holds (sub_8223B2F0 on *(game)+44),
// sub_82270FE8 clears the hold bit (+520 0x80) and copies the GAME CAMERA's
// direction (+228..+236) into the held forward (+464..+472) directly. That
// value then stays in the controller and the hold logic (sub_822718E8)
// compares against it every frame: a different start on each PC made a hold
// begin one frame apart minutes later (player 2's turn, frame 9430). Online
// the held forward written there becomes the sent facing of the controller's
// player (flat), as OnlineHeldFacing does for the per-frame copy.
extern "C" REX_FUNC(__imp__sub_82270FE8);
extern "C" REX_FUNC(sub_82270FE8) {
  const uint32_t controller = ctx.r3.u32;
  uint8_t* gb = Base();
  uint8_t before[12];
  const bool readable = guest_memory::Readable(controller + 464, 12);
  if (readable) std::memcpy(before, GuestPtr(gb, controller + 464), 12);
  __imp__sub_82270FE8(ctx, base);
  if (!readable || !net::InSession() || std::memcmp(before, GuestPtr(gb, controller + 464), 12) == 0) return;
  const int player = online_rules::PlayerOfController(gb, controller);
  uint16_t facing = 0;
  if (player < 0 || !net::FrameFacing(player, facing)) return;
  double c = 0, sn = 0;
  CosSin(facing, c, sn);
  WriteFloat(gb, controller + 464, float(c));
  WriteFloat(gb, controller + 468, 0.0f);
  WriteFloat(gb, controller + 472, float(sn));
  REXLOG_INFO("Online: player {}'s held forward at an unlock = their PC's sent facing", player + 1);
}

// Debug (--debug_net_fingerprint_log): every player's root controller state:
// lock word +516, hold byte +520, held forward +464..+472 and the stick stored
// at a hold +476/+480, raw words.
namespace online_sticks {
std::string ControllerParts() {
  std::string out;
  uint8_t* gb = Base();
  for (int p = 0; p < 2; ++p) {
    const uint32_t c = online_rules::RootOf(p);
    if (!c || !guest_memory::Readable(c + 464, 60)) {
      out += "c- ";
      continue;
    }
    char line[160];
    std::snprintf(line, sizeof(line), "c%d lock %08X hold %02X held %08X %08X %08X stk %08X %08X ", p + 1,
                  Word(gb, c + 516), *GuestPtr(gb, c + 520), Word(gb, c + 464), Word(gb, c + 468),
                  Word(gb, c + 472), Word(gb, c + 476), Word(gb, c + 480));
    out += line;
  }
  return out;
}
}  // namespace online_sticks

// Debug (--debug_net_fingerprint_log, own-camera hunt 2026-11-17): player 2's
// behaviours: every pointer in the actor's slots +108..+347 that points at an
// object with a vtable in the image, as "vtable:hash of its first 256 bytes".
namespace online_sticks {
std::string BehaviourParts() {
  std::string out;
  uint8_t* gb = Base();
  const uint32_t actor = more_players::CharacterOfPlayer(1);
  if (!actor || !guest_memory::Readable(actor + 108, 240)) return out;
  for (uint32_t off = 108; off < 348; off += 4) {
    const uint32_t obj = Word(gb, actor + off);
    if (obj < 0x40000000 || !guest_memory::Readable(obj, 256)) continue;
    const uint32_t vt = Word(gb, obj);
    if (vt < 0x82000000 || vt >= 0x82100000) continue;
    uint64_t h = 1469598103934665603ull;
    const uint8_t* p = GuestPtr(gb, obj);
    for (int i = 0; i < 256; ++i) h = (h ^ p[i]) * 1099511628211ull;
    char item[48];
    std::snprintf(item, sizeof(item), "%X:%08X:%04llX ", off, vt, (unsigned long long)(h & 0xFFFF));
    out += item;
    // Raw words of the suspects (physics, platform rider, intersection, targeting).
    if (vt == 0x82032FC4 || vt == 0x8202FC0C || vt == 0x8202ECBC || vt == 0x82031634) {
      out += "[";
      for (uint32_t k = 0; k < 256; k += 4) {
        char w[10];
        std::snprintf(w, sizeof(w), "%08X ", Word(gb, obj + k));
        out += w;
      }
      out += "] ";
      // Targeting +0xAC = the chosen target; physics +0xA8 = what the body
      // stands on: what they ARE (vtable, and an actor's world position via
      // +44 -> +48) rather than where they sit in memory.
      const uint32_t ref_off = vt == 0x82031634 ? 0xAC : vt == 0x82032FC4 ? 0xA8 : 0;
      const uint32_t ref = ref_off ? Word(gb, obj + ref_off) : 0;
      if (ref && guest_memory::Readable(ref, 48)) {
        char r[96];
        const uint32_t m = Word(gb, ref + 44);
        if (m && guest_memory::Readable(m + 48, 12)) {
          std::snprintf(r, sizeof(r), "{ref vt %08X at %08X %08X %08X} ", Word(gb, ref), Word(gb, m + 48),
                        Word(gb, m + 52), Word(gb, m + 56));
        } else {
          std::snprintf(r, sizeof(r), "{ref vt %08X} ", Word(gb, ref));
        }
        out += r;
      }
    }
  }
  return out;
}
}  // namespace online_sticks

// Debug (own-camera hunt 2026-11-17, --debug_net_fingerprint_log set): every
// break (Breakable slot 28, sub_82195E30) logs its session frame and the
// game's call stack, to compare the two PCs.
#include "../debug/guest_stack.h"
#include "../fixed_step.h"
REXCVAR_DECLARE(std::string, debug_net_fingerprint_log);
extern "C" REX_FUNC(__imp__sub_82195E30);
extern "C" REX_FUNC(sub_82195E30) {
  if (net::InSession() && !REXCVAR_GET(debug_net_fingerprint_log).empty()) {
    uint32_t calls[8] = {};
    const int n = guest_stack::Callers(ctx.r1.u32, calls, 8);
    std::string chain = fmt::format("lr {:08X}", uint32_t(ctx.lr));
    for (int i = 0; i < n; ++i) chain += fmt::format(" {:08X}", calls[i]);
    REXLOG_INFO("Online debug: break by {:08X} at fixed-step frame {}: {}", ctx.r3.u32, fixed_step::Frame(), chain);
  }
  __imp__sub_82195E30(ctx, base);
}
