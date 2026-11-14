// =============================================================================
// input/key_events.cpp -- see key_events.h (findings/62)
// =============================================================================
#include "key_events.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../guest_memory.h"
#include "keyboard_mouse.h"
#include "players.h"
#include "../players/more_players.h"

REXCVAR_DEFINE_BOOL(debug_key_events_check, false, "CrashMoM",
                    "Debug: work out every input map's events a second time through the game's own rules, on a "
                    "copy of the controller's values, and compare with the game's (log lines \"Key events "
                    "check:\"; findings/62)");

REXCVAR_DEFINE_BOOL(keyboard_events, true, "CrashMoM",
                    "The keyboard and mouse reach the game as its own input EVENTS (jump, heavy attack, menu "
                    "accept...), worked out by the game's rules from the keys; false = the old way, the keys "
                    "pressed as controller buttons (findings/62)");

REXCVAR_DEFINE_BOOL(debug_key_events_trace, false, "CrashMoM",
                    "Debug: log every input event the keyboard makes (\"Key events: map 5 event 24\"; numbers in "
                    "findings/62 s.1)");

extern "C" REX_FUNC(__imp__sub_82275E20);  // an input map's events for this frame (r3 map, r5 controller, f1 dt)
extern "C" REX_FUNC(__imp__sub_82275DC8);  // reset ONE event of a map (r3 map, r4 event): "consumed"
extern "C" REX_FUNC(__imp__sub_82275C90);  // reset EVERY event of a map (r3 map)
extern "C" REX_FUNC(__imp__sub_82273328);  // add a sample to a value channel (r3 channel, f1 value, r5 time ms)
extern "C" REX_FUNC(__imp__sub_8235ABC8);  // the game's clock in ms (r3)
extern "C" REX_FUNC(__imp__sub_8235D188);  // Radical's input poll of one socket (r3 = its controller object)
extern "C" REX_FUNC(__imp__sub_8235CAE8);  // one input object reads its number from the state (r3 object, r4 state)
extern "C" REX_FUNC(__imp__sub_8235C9F0);  // the sticks' dead zone (f1 size, r3 &x, r4 &y: int16 pairs)

namespace key_events {
namespace {

// The game's layouts (findings/62 s.1, read with Ghidra 2026-11-14).
constexpr uint32_t kMapEvents = 0x1C;     // map: pointer to the event vector {begin, end}
constexpr uint32_t kMapType = 0x18;       // map: its type (1 MENU, 4 GENERAL, 5 PLAYER, ...)
constexpr uint32_t kEntryBytes = 28;      // one event
constexpr uint32_t kEntryInputs = 4;      // event: pointer to its inputs
constexpr uint32_t kEntryInputCount = 0xD;  // event: high nibble = how many inputs
constexpr uint32_t kInputBytes = 12;      // one input: u32 value (top 16 bits) + kind (bits 12-15), flags...
constexpr uint32_t kParent = 0x24;        // controller: parent (the rules walk up to the root)
constexpr uint32_t kState20 = 0x20;       // controller: read by sub_82273E10 (a sign test)
constexpr uint32_t kChannels = 0x2C;      // controller: value channels, 12 bytes each
constexpr uint32_t kChannelBytes = 12;    // {samples, ring size, newest}
constexpr uint32_t kSampleBytes = 16;     // {value f32, time ms, flags (bit 0x80 down), -}
constexpr int kValueCount = 18;           // EControllerValue 0 MOVEX .. 17 SELECT (findings/62 s.5)
constexpr uint32_t kLastUpdate = 0x194;   // controller: time (ms) of its previous update
constexpr uint32_t kControllerBytes = 0x200;

uint32_t Read32(uint8_t* base, uint32_t a) {
  const uint8_t* p = GuestPtr(base, a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Write32(uint8_t* base, uint32_t a, uint32_t v) {
  uint8_t* p = GuestPtr(base, a);
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
uint32_t Alloc(uint32_t bytes) {
  const uint32_t a = rex::system::kernel_memory()->SystemHeapAlloc(bytes);
  return a;
}
void Free(uint32_t a) {
  if (a) rex::system::kernel_memory()->SystemHeapFree(a);
}

// ---------------------------------------------------------------------------
// A map's shadow: a fake map whose event vector points at OUR copy of the
// entries, each entry's inputs pointer at OUR copy of its inputs. One guest
// block: [0, 0x20) the fake map (only +0x1C is read), [0x20, 0x28) the
// vector, then the entries, then the inputs.
// ---------------------------------------------------------------------------
struct ShadowMap {
  uint32_t block = 0, entries = 0;
  uint32_t real_begin = 0, real_end = 0;
  int type = -1;
  std::vector<uint32_t> real_inputs;  // per entry, the real inputs pointer (a rebuilt map changes them)
  std::vector<uint32_t> shadow_inputs;
  std::vector<uint32_t> input_counts;
  // Keyboard shadows: the real events whose happened bit THE KEYS set last
  // frame (taken back before the next update: see KeyboardEvents).
  std::vector<uint8_t> set_by_keys;
};

// A controller's shadow: the fields the rules read + rings of our own.
struct ShadowController {
  uint32_t block = 0;
  uint32_t rings[kValueCount] = {};
  uint32_t ring_sizes[kValueCount] = {};
};

std::unordered_map<uint32_t, ShadowMap> g_maps;               // by the real map
std::unordered_map<uint32_t, ShadowController> g_controllers;  // by the real controller

// Does `s` still mirror the real map (same table, same inputs)?
bool StillMatches(uint8_t* base, const ShadowMap& s, uint32_t map) {
  const uint32_t vec = Read32(base, map + kMapEvents);
  if (Read32(base, vec) != s.real_begin || Read32(base, vec + 4) != s.real_end) return false;
  for (size_t i = 0; i < s.real_inputs.size(); ++i) {
    if (Read32(base, s.real_begin + uint32_t(i) * kEntryBytes + kEntryInputs) != s.real_inputs[i]) return false;
  }
  return true;
}

// Makes (or remakes) the shadow of `map` as it is NOW (before this frame's
// update): the rules' state starts the same as the game's. False = a map we
// can't mirror (an input on a value past kValueCount: none seen).
bool Build(uint8_t* base, ShadowMap& s, uint32_t map) {
  Free(s.block);
  s = ShadowMap{};
  const uint32_t vec = Read32(base, map + kMapEvents);
  s.real_begin = Read32(base, vec);
  s.real_end = Read32(base, vec + 4);
  s.type = int(int32_t(Read32(base, map + kMapType)));
  if (s.real_end < s.real_begin || (s.real_end - s.real_begin) % kEntryBytes ||
      s.real_end - s.real_begin > kEntryBytes * 256) {
    return false;
  }
  const uint32_t count = (s.real_end - s.real_begin) / kEntryBytes;
  uint32_t inputs_total = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t e = s.real_begin + i * kEntryBytes;
    const uint32_t n = *GuestPtr(base, e + kEntryInputCount) >> 4;
    const uint32_t inputs = Read32(base, e + kEntryInputs);
    for (uint32_t k = 0; k < n; ++k) {
      if (int(int16_t(Read32(base, inputs + k * kInputBytes) >> 16)) >= kValueCount) return false;
    }
    s.real_inputs.push_back(inputs);
    s.input_counts.push_back(n);
    inputs_total += n;
  }
  const uint32_t bytes = 0x28 + count * kEntryBytes + inputs_total * kInputBytes;
  s.block = Alloc(bytes);
  if (!s.block) return false;
  std::memset(GuestPtr(base, s.block), 0, bytes);
  s.entries = s.block + 0x28;
  Write32(base, s.block + kMapEvents, s.block + 0x20);
  Write32(base, s.block + 0x20, s.entries);
  Write32(base, s.block + 0x24, s.entries + count * kEntryBytes);
  std::memcpy(GuestPtr(base, s.entries), GuestPtr(base, s.real_begin), count * kEntryBytes);
  uint32_t next = s.entries + count * kEntryBytes;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t n = s.input_counts[i];
    if (n) std::memcpy(GuestPtr(base, next), GuestPtr(base, s.real_inputs[i]), n * kInputBytes);
    Write32(base, s.entries + i * kEntryBytes + kEntryInputs, n ? next : 0);
    s.shadow_inputs.push_back(n ? next : 0);
    next += n * kInputBytes;
  }
  return true;
}

// The root of a controller (the rules read the root's fields).
uint32_t Root(uint8_t* base, uint32_t controller) {
  for (int guard = 0; guard < 16; ++guard) {
    const uint32_t parent = Read32(base, controller + kParent);
    if (!parent) break;
    controller = parent;
  }
  return controller;
}

// CHECK MODE: the shadow controller = a copy of the real one's fields and
// rings, as they are this frame.
bool CopyRealValues(uint8_t* base, ShadowController& c, uint32_t real_root) {
  if (!c.block) {
    c.block = Alloc(kControllerBytes);
    if (!c.block) return false;
    std::memset(GuestPtr(base, c.block), 0, kControllerBytes);
  }
  Write32(base, c.block + kParent, 0);
  Write32(base, c.block + kState20, Read32(base, real_root + kState20));
  Write32(base, c.block + kLastUpdate, Read32(base, real_root + kLastUpdate));
  for (int v = 0; v < kValueCount; ++v) {
    const uint32_t ch = real_root + kChannels + uint32_t(v) * kChannelBytes;
    const uint32_t samples = Read32(base, ch), size = Read32(base, ch + 4), newest = Read32(base, ch + 8);
    if (!samples || size == 0 || size > 256) return false;
    if (c.ring_sizes[v] != size) {
      Free(c.rings[v]);
      c.rings[v] = Alloc(size * kSampleBytes);
      c.ring_sizes[v] = c.rings[v] ? size : 0;
      if (!c.rings[v]) return false;
    }
    std::memcpy(GuestPtr(base, c.rings[v]), GuestPtr(base, samples), size * kSampleBytes);
    const uint32_t sh = c.block + kChannels + uint32_t(v) * kChannelBytes;
    Write32(base, sh, c.rings[v]);
    Write32(base, sh + 4, size);
    Write32(base, sh + 8, newest);
  }
  return true;
}

struct Stats {
  uint64_t runs = 0, events = 0, happened = 0, differ = 0, rebuilt = 0, skipped = 0;
  int logged = 0;
  std::set<std::pair<int, int>> seen;  // (map type, event) seen happening, both sides agreeing
  std::chrono::steady_clock::time_point last{};
} g_stats;

// Every byte the rules keep: the entry (but its inputs pointer) and the inputs.
void Compare(uint8_t* base, const ShadowMap& s) {
  const uint32_t count = uint32_t(s.real_inputs.size());
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t real = s.real_begin + i * kEntryBytes, ours = s.entries + i * kEntryBytes;
    ++g_stats.events;
    if (*GuestPtr(base, real + 0x14) & 0x80) {  // (a check of "nothing" proves little)
      ++g_stats.happened;
      g_stats.seen.insert({s.type, int(int32_t(Read32(base, real)))});
    }
    bool same = std::memcmp(GuestPtr(base, real), GuestPtr(base, ours), kEntryInputs) == 0 &&
                std::memcmp(GuestPtr(base, real + 8), GuestPtr(base, ours + 8), kEntryBytes - 8) == 0;
    const uint32_t n = s.input_counts[i];
    if (same && n) {
      same = std::memcmp(GuestPtr(base, s.real_inputs[i]), GuestPtr(base, s.shadow_inputs[i]), n * kInputBytes) == 0;
    }
    if (same) continue;
    ++g_stats.differ;
    if (g_stats.logged < 16) {
      ++g_stats.logged;
      REXLOG_WARN("Key events check: map type {} event {} DIFFERS: game happened {} ours {} (entry +0x14 {:02X} / {:02X})",
                  s.type, int32_t(Read32(base, real)), *GuestPtr(base, real + 0x14) >> 7,
                  *GuestPtr(base, ours + 0x14) >> 7, *GuestPtr(base, real + 0x14), *GuestPtr(base, ours + 0x14));
    }
  }
}

void Summary() {
  const auto now = std::chrono::steady_clock::now();
  if (g_stats.last == std::chrono::steady_clock::time_point{}) g_stats.last = now;
  if (now - g_stats.last < std::chrono::seconds(10)) return;
  g_stats.last = now;
  REXLOG_INFO("Key events check: {} map updates, {} events compared ({} happened), {} differ ({} shadows (re)built, "
              "{} skipped)",
              g_stats.runs, g_stats.events, g_stats.happened, g_stats.differ, g_stats.rebuilt, g_stats.skipped);
  std::string seen;
  for (const auto& [type, event] : g_stats.seen) seen += " " + std::to_string(type) + ":" + std::to_string(event);
  REXLOG_INFO("Key events check: seen happening (map type:event):{}", seen);
}


// ---------------------------------------------------------------------------
// THE KEYBOARD (step 3, findings/62 s.7). Per keyboard socket: two shadow
// controllers, MENU (what the keys mean in menus) and PLAY, fed every frame
// with the driver's EventImage through the game's own sample writer; per
// input map, the keyboard's own shadow of it (its own sequence progress),
// worked out by the game's rules on the right shadow controller; its
// "happened" bits are ADDED to the real map's. A controller on the same
// player keeps its own events (the real map's update ran first, on the
// controller's values only: the game no longer gets the keyboard's
// buttons, save_library.cpp's XInputGetState wrapper takes them out).
// ---------------------------------------------------------------------------
enum Image { kMenuImage, kPlayImage, kBothImages };

// Which image a map type hears: MENU (1) and CHEAT (3, the original's D-pad
// codes) = the menu keys; MOVIE (2) = either (any key may skip); the rest
// (GENERAL, PLAYER, ENEMY, PROJECTILE, CAMERA) = the play keys.
Image ImageForMap(int type) {
  if (type == 1 || type == 3) return kMenuImage;
  if (type == 2) return kBothImages;
  return kPlayImage;
}

std::unordered_map<uint32_t, ShadowMap> g_kb_maps;               // by the real map
std::unordered_map<uint64_t, ShadowController> g_kb_controllers;  // by (real root, image)
std::unordered_map<uint64_t, uint32_t> g_kb_fed;                  // ... the real +0x194 it was fed for

// The socket (0-3) whose input controller `root` is, or -1. The front end's
// table: *(*(uber + 0x38) + 8)[socket] (IsButtonPressed asks it).
int SocketOf(uint8_t* base, uint32_t root) {
  const uint32_t uber = Read32(base, 0x8259B190);
  if (!uber) return -1;
  const uint32_t list = Read32(base, uber + 0x38);
  if (!list) return -1;
  const uint32_t table = Read32(base, list + 8);
  if (!table) return -1;
  for (int s = 0; s < 4; ++s) {
    if (Read32(base, table + 4 * uint32_t(s)) == root) return s;
  }
  return -1;
}

// The value (0 or 1) the keyboard gives controller value v (EControllerValue,
// findings/62 s.5) in this image.
float ValueOf(const kbm::KeyboardMouseDriver::EventImage& img, Image which, int v) {
  auto bits = [&](bool menu) { return menu ? img.menu : img.play; };
  auto lt = [&](bool menu) { return menu ? img.menu_lt : img.play_lt; };
  auto rt = [&](bool menu) { return menu ? img.menu_rt : img.play_rt; };
  auto one = [&](bool menu) -> bool {
    const uint16_t b = bits(menu);
    switch (v) {
      case 4: return b & 0x0004;   // DPADLEFT
      case 5: return b & 0x0008;   // DPADRIGHT
      case 6: return b & 0x0001;   // DPADUP
      case 7: return b & 0x0002;   // DPADDOWN
      case 8: return b & 0x1000;   // X = A
      case 9: return b & 0x2000;   // CIRCLE = B
      case 10: return b & 0x8000;  // TRIANGLE = Y
      case 11: return b & 0x4000;  // SQUARE = X
      case 12: return rt(menu);    // R1 = RT
      case 13: return lt(menu);    // L1 = LT
      case 14: return b & 0x0200;  // R2 = RB
      case 15: return b & 0x0100;  // L2 = LB
      case 16: return b & 0x0010;  // START
      case 17: return b & 0x0020;  // SELECT = Back
      default: return false;       // 0-3 = the sticks: still the controller state's
    }
  };
  if (which == kBothImages) return (one(true) || one(false)) ? 1.0f : 0.0f;
  return one(which == kMenuImage) ? 1.0f : 0.0f;
}

// A keyboard shadow controller: made with rings the size of the real ones
// (empty: every sample "up" at time 0), then the keys' values added each
// frame, once (the first map of the frame; the game's own +0x194 tells the
// frames apart).
uint32_t FedKeyboardController(PPCContext& ctx, uint8_t* base, uint32_t real_root, Image which) {
  const uint64_t key = uint64_t(real_root) << 2 | uint32_t(which);
  ShadowController& c = g_kb_controllers[key];
  if (!c.block) {
    c.block = Alloc(kControllerBytes);
    if (!c.block) return 0;
    std::memset(GuestPtr(base, c.block), 0, kControllerBytes);
    for (int v = 0; v < kValueCount; ++v) {
      const uint32_t size = Read32(base, real_root + kChannels + uint32_t(v) * kChannelBytes + 4);
      if (size == 0 || size > 256) return 0;
      c.rings[v] = Alloc(size * kSampleBytes);
      if (!c.rings[v]) return 0;
      c.ring_sizes[v] = size;
      std::memset(GuestPtr(base, c.rings[v]), 0, size * kSampleBytes);
      const uint32_t sh = c.block + kChannels + uint32_t(v) * kChannelBytes;
      Write32(base, sh, c.rings[v]);
      Write32(base, sh + 4, size);
      Write32(base, sh + 8, 0);
    }
  }
  const uint32_t last_update = Read32(base, real_root + kLastUpdate);
  Write32(base, c.block + kParent, 0);
  Write32(base, c.block + kState20, Read32(base, real_root + kState20));
  Write32(base, c.block + kLastUpdate, last_update);
  uint32_t& fed = g_kb_fed[key];
  if (fed == last_update && fed != 0) return c.block;  // (this frame's values are in)
  fed = last_update;
  const kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  auto image = driver ? driver->LastEventImage() : kbm::KeyboardMouseDriver::EventImage{};
  // WHEN THE PLAY KEYS ALSO COUNT FOR THE MENUS. Two moments where nothing
  // of this player plays, so a play key can only mean a menu question:
  // * A 3D CUTSCENE (the root controller LOCKED: +0x204 top byte > 0,
  //   sub_82272D68): START shows the game's "Skip (A)" and the MENU map's
  //   ACCEPT skips. Escape (Start) is a menu key too, so the prompt came up,
  //   but Space / Enter were play keys there (no menu on screen): nothing
  //   answered ACCEPT and keys couldn't skip.
  // * THE PLAYER ISN'T IN THE GAME (dropped out, never joined) in a running
  //   level: the join pick sub_82264988 asks MENU PAUSE (Start), ACCEPT (A),
  //   CO_OP_JOIN (B) (findings/62 s.2). A pad's B is one button for every
  //   map, so B joins; on keys Jack / Use (E) is a play action: only Escape
  //   joined.
  // A pad gives every map the same buttons, so this is what a controller
  // does there. Only while no menu is up (keys in a menu are menu keys).
  if (which == kMenuImage && !menu_input::MenuOnScreen()) {
    uint32_t root = real_root;
    for (int guard = 0; guard < 16; ++guard) {  // (the lock is the ROOT's)
      const uint32_t parent = Read32(base, root + kParent);
      if (!parent) break;
      root = parent;
    }
    const bool locked = *GuestPtr(base, root + 0x204) > 0;
    const int socket = SocketOf(base, real_root);
    const bool out = socket >= 0 && more_players::InPlay() &&
                     Read32(base, 0x8259B11C + 4 * uint32_t(socket)) == 0;  // co-op state 0 = not joined
    if (locked || out) {
      image.menu |= image.play;
      image.menu_lt |= image.play_lt;
      image.menu_rt |= image.play_rt;
    }
  }
  PPCContext call = ctx;
  __imp__sub_8235ABC8(call, base);
  const uint32_t now = call.r3.u32;
  for (int v = 4; v < kValueCount; ++v) {
    call = ctx;
    call.r3.u64 = c.block + kChannels + uint32_t(v) * kChannelBytes;
    call.f1.f64 = ValueOf(image, which, v);
    call.r5.u64 = now;
    __imp__sub_82273328(call, base);  // (adds a sample only when the value moved)
  }
  return c.block;
}

// The keyboard's events for `map` this frame, added to the real map's bits.
void KeyboardEvents(PPCContext& saved, uint8_t* base, uint32_t map, uint32_t real_root) {
  ShadowMap& s = g_kb_maps[map];
  if (!s.block || !StillMatches(base, s, map)) {
    if (!Build(base, s, map)) {
      Free(s.block);
      g_kb_maps.erase(map);
      return;
    }
    // A clean start for the keyboard's sequences (Build copied the game's).
    PPCContext reset = saved;
    reset.r3.u64 = s.block;
    __imp__sub_82275C90(reset, base);
  }
  const uint32_t controller = FedKeyboardController(saved, base, real_root, ImageForMap(s.type));
  if (!controller) return;
  PPCContext ours = saved;
  ours.r3.u64 = s.block;
  ours.r5.u64 = controller;
  __imp__sub_82275E20(ours, base);
  const uint32_t count = uint32_t(s.real_inputs.size());
  s.set_by_keys.assign(count, 0);
  for (uint32_t i = 0; i < count; ++i) {
    if (*GuestPtr(base, s.entries + i * kEntryBytes + 0x14) & 0x80) {
      uint8_t& real_bit = *GuestPtr(base, s.real_begin + i * kEntryBytes + 0x14);
      // (STAND = "A not held" happens every frame: not worth a line.)
      const int event = int(int32_t(Read32(base, s.entries + i * kEntryBytes)));
      if (REXCVAR_GET(debug_key_events_trace) && event != 44) {
        REXLOG_INFO("Key events: map {} event {} from the keys{}", s.type, event,
                    (real_bit & 0x80) ? " (the controller too)" : "");
      }
      if (!(real_bit & 0x80)) s.set_by_keys[i] = 1;
      real_bit |= 0x80;
    }
  }
}

}  // namespace
}  // namespace key_events

bool key_events::Active() { return REXCVAR_GET(keyboard_events); }

// An input map's events for this frame (sub_822710D8's loop, every map of
// every input controller): the game's own first; then, in check mode, the
// same code on the shadow map with a copy of the controller's values.
extern "C" REX_FUNC(sub_82275E20) {
  using namespace key_events;
  const bool check = REXCVAR_GET(debug_key_events_check);
  const uint32_t map = ctx.r3.u32, controller = ctx.r5.u32;
  ShadowMap* shadow = nullptr;
  if (check && map && controller) {
    // The shadow is made BEFORE the game's update (same starting state).
    ShadowMap& s = g_maps[map];
    if (!s.block || !StillMatches(base, s, map)) {
      ++g_stats.rebuilt;
      if (!Build(base, s, map)) {
        ++g_stats.skipped;
        Free(s.block);
        g_maps.erase(map);
      }
    }
    auto it = g_maps.find(map);
    if (it != g_maps.end()) shadow = &it->second;
  }
  // THE KEYS' BITS LAST ONE FRAME (2026-11-14, first keyboard test: a held
  // heavy attack, Start in the pause menu, kept "happening" frame after
  // frame). The game clears an event's bit through its own rules, which go by
  // the event's sequence state; a bit the keys set has no such state behind
  // it in the real map, so nothing cleared it. Taken back here, before the
  // game's update: the game's own bit for this frame is then its own again.
  if (const auto it = g_kb_maps.find(map); it != g_kb_maps.end() && it->second.block) {
    ShadowMap& s = it->second;
    for (size_t i = 0; i < s.set_by_keys.size(); ++i) {
      if (s.set_by_keys[i] && StillMatches(base, s, map)) {
        *GuestPtr(base, s.real_begin + uint32_t(i) * kEntryBytes + 0x14) &= 0x7F;
      }
    }
    s.set_by_keys.assign(s.set_by_keys.size(), 0);
  }
  const PPCContext saved = ctx;
  __imp__sub_82275E20(ctx, base);
  // The keyboard's own events for this map (KeyboardEvents above).
  if (REXCVAR_GET(keyboard_events) && map && controller) {
    const uint32_t root = Root(base, controller);
    const int socket = SocketOf(base, root);
    const kbm::PlayerAssignment* assignment = kbm::PlayerAssignment::Get();
    if (socket >= 0 && assignment && assignment->KeyboardAnswersFor(uint32_t(socket))) {
      PPCContext kb = saved;
      KeyboardEvents(kb, base, map, root);
    }
  }
  if (!shadow) return;
  ShadowController& c = g_controllers[controller];
  if (CopyRealValues(base, c, Root(base, controller))) {
    PPCContext ours = saved;
    ours.r3.u64 = shadow->block;
    ours.r5.u64 = c.block;
    __imp__sub_82275E20(ours, base);
    ++g_stats.runs;
    Compare(base, *shadow);
  } else {
    ++g_stats.skipped;
  }
  Summary();
}

// THE GAME CONSUMES EVENTS (found by the check, 2026-11-14: a titan's
// ATTACK_HEAVY, Y held, came out in the shadow for 3 frames and not in the
// game's map): besides the rules, two functions reset an event's state from
// outside: sub_82275DC8(map, event) one event (its only caller sub_82273E98,
// "consume": IsButtonPressed with its consume flag, for every socket) and
// sub_82275C90(map) all of them (from sub_822729A8). The shadow gets the
// same reset, through the same code on the shadow map, so it stays in step;
// and a keyboard's event (step 3) is consumed exactly like the game's own.
extern "C" REX_FUNC(sub_82275DC8) {
  using namespace key_events;
  const PPCContext saved = ctx;
  __imp__sub_82275DC8(ctx, base);
  for (auto* maps : {&g_maps, &g_kb_maps}) {
    const auto it = maps->find(saved.r3.u32);
    if (it == maps->end() || !it->second.block) continue;
    // Its lookup sub_82276CE0 ADDS an event that isn't there (a map-style
    // "find or insert" through the game's allocator): never let it grow our
    // vector. An event the shadow doesn't have = the real map changed: the
    // next update remakes the shadow anyway.
    const ShadowMap& sh = it->second;
    bool known = false;
    for (size_t i = 0; i < sh.real_inputs.size() && !known; ++i) {
      known = int32_t(Read32(base, sh.entries + uint32_t(i) * kEntryBytes)) == int32_t(saved.r4.u32);
    }
    if (!known) continue;
    PPCContext ours = saved;
    ours.r3.u64 = sh.block;
    __imp__sub_82275DC8(ours, base);
  }
}
extern "C" REX_FUNC(sub_82275C90) {
  using namespace key_events;
  const PPCContext saved = ctx;
  __imp__sub_82275C90(ctx, base);
  for (auto* maps : {&g_maps, &g_kb_maps}) {
    const auto it = maps->find(saved.r3.u32);
    if (it == maps->end() || !it->second.block) continue;
    PPCContext ours = saved;
    ours.r3.u64 = it->second.block;
    __imp__sub_82275C90(ours, base);
  }
}

// -----------------------------------------------------------------------------
// THE STICKS (2026-11-14, findings/62 s.8): WASD and the right-stick keys go
// into the game's own MOVEMENT VALUES, not into the controller state.
//
// The game has no "walk forward" event: its movement is a direction and how
// far, the value channels MOVEX / MOVEY / CAMERAX / CAMERAY (0-3). Radical's
// poll (sub_8235D188, r3 = the socket's controller object, socket at +0x10)
// reads the controller state, gives each stick its dead zone (sub_8235C9F0,
// 0.25 at 0x820037BC), then every INPUT OBJECT of the controller (+0x1C,
// 60 bytes each) reads its own number out of the state (sub_8235CAE8 ->
// sub_8235C200: the object's entry in the table at 0x824E8B1C {name, kind,
// state offset, size, mask}; entries 16-19 = LeftStickX / Y, RightStickX / Y
// at offsets 4 / 6 / 8 / 10) and, when it moved, its listener adds the
// movement value.
//
// For the keyboard's socket, the four stick objects read the KEYS' numbers
// instead while the keys push that stick (they replace the controller's
// stick, as before): the same raw numbers the keyboard used to type into the
// state (the driver's pad: diagonals on the circle, Walk's lean), given the
// game's own dead zone and conversion here, so the movement is exactly what
// it was; only the way in changed. The XInputGetState wrapper takes the
// keyboard's sticks out of the state like its buttons (save_library.cpp).
// -----------------------------------------------------------------------------
namespace key_events {
namespace {
thread_local int g_poll_socket = -1;  // the socket sub_8235D188 is polling, -1 = none
uint32_t g_stick_state = 0;           // guest scratch: a copy of the state with the keys' stick (16 bytes)
constexpr uint32_t kInputTable = 0x824E8B1C;  // per controller type (28 bytes each): its input entries
constexpr uint32_t kDeadZone = 0x820037BC;    // the sticks' dead zone (0.25)
}  // namespace
}  // namespace key_events

extern "C" REX_FUNC(sub_8235D188) {
  using namespace key_events;
  g_poll_socket = int(Read32(base, ctx.r3.u32 + 0x10));
  __imp__sub_8235D188(ctx, base);
  g_poll_socket = -1;
}

extern "C" REX_FUNC(sub_8235CAE8) {
  using namespace key_events;
  const kbm::PlayerAssignment* assignment = kbm::PlayerAssignment::Get();
  const kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  if (!REXCVAR_GET(keyboard_events) || g_poll_socket < 0 || g_poll_socket > 3 || !assignment || !driver ||
      !assignment->KeyboardAnswersFor(uint32_t(g_poll_socket))) {
    __imp__sub_8235CAE8(ctx, base);
    return;
  }
  // Which number this object reads (sub_8235C200's own lookup).
  const uint32_t object = ctx.r3.u32;
  const uint32_t entries = Read32(base, kInputTable + Read32(base, object + 0x10) * 28);
  const uint32_t offset = entries ? Read32(base, entries + Read32(base, object + 0x14) * 20 + 8) : 0;
  if (offset != 4 && offset != 6 && offset != 8 && offset != 10) {
    __imp__sub_8235CAE8(ctx, base);
    return;
  }
  const auto pad = driver->last_pad();
  const bool left = offset <= 6;
  const int16_t x = left ? int16_t(pad.thumb_lx) : int16_t(pad.thumb_rx);
  const int16_t y = left ? int16_t(pad.thumb_ly) : int16_t(pad.thumb_ry);
  if (x == 0 && y == 0) {  // the keys don't push this stick: the controller's
    __imp__sub_8235CAE8(ctx, base);
    return;
  }
  if (!g_stick_state) g_stick_state = Alloc(16);
  if (!g_stick_state) {
    __imp__sub_8235CAE8(ctx, base);
    return;
  }
  std::memcpy(GuestPtr(base, g_stick_state), GuestPtr(base, ctx.r4.u32), 16);
  const uint32_t at = g_stick_state + (left ? 4u : 8u);
  uint8_t* p = GuestPtr(base, at);
  p[0] = uint8_t(uint16_t(x) >> 8), p[1] = uint8_t(uint16_t(x));
  p[2] = uint8_t(uint16_t(y) >> 8), p[3] = uint8_t(uint16_t(y));
  // The game's dead zone on the keys' stick, as on a controller's.
  PPCContext dz = ctx;
  uint32_t bits = Read32(base, kDeadZone);
  float zone;
  std::memcpy(&zone, &bits, 4);
  dz.f1.f64 = zone;
  dz.r3.u64 = at;
  dz.r4.u64 = at + 2;
  __imp__sub_8235C9F0(dz, base);
  PPCContext ours = ctx;
  ours.r4.u64 = g_stick_state;
  __imp__sub_8235CAE8(ours, base);
  ctx.r3 = ours.r3;
  // What the controller state itself said for this stick (the keyboard's
  // must be out of it by now: save_library.cpp takes it back out).
  static bool s_logged = false;
  if (!s_logged) {
    s_logged = true;
    const uint8_t* st = GuestPtr(base, ctx.r4.u32 + (left ? 4u : 8u));
    REXLOG_INFO("Key events: the keys' {} stick goes into the movement values ({}, {}); the controller state "
                "holds ({}, {})",
                left ? "left" : "right", x, y, int16_t(st[0] << 8 | st[1]), int16_t(st[2] << 8 | st[3]));
  }
}


