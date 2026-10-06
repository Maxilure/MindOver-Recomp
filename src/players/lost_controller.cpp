// players/lost_controller.cpp -- see lost_controller.h.
#include "lost_controller.h"

#include <array>
#include <atomic>
#include <cstring>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../input/players.h"
#include "more_players.h"

REXCVAR_DEFINE_BOOL(lost_controller, true, "CrashMoM",
                    "A player in a level loses its controller: pause with the game's question box, any "
                    "player can drop that player out, the player presses a button to resume once it's "
                    "back (instead of the game's own pause, which only the missing controller could "
                    "close; findings/26)");

extern "C" REX_FUNC(__imp__sub_82262D78);  // the front end's "controller missing" check (r3 = front end)
extern "C" REX_FUNC(__imp__sub_82266040);  // the pause menu's Drop Out (drops the menu owner, +8536)
extern "C" REX_FUNC(__imp__sub_8225F758);  // ShowConfirmMessage (r3 = front end, r4 = text name, f1 = timer)
extern "C" REX_FUNC(__imp__sub_8225E1D8);  // the question box: close (done, hidden)
extern "C" REX_FUNC(__imp__sub_82373360);  // text element: set its string (UTF-16, copied)
extern "C" REX_FUNC(__imp__sub_823574E8);  // make heap r3 the current one, returns the previous
extern "C" REX_FUNC(__imp__sub_8227C8E8);  // the game manager: pause / unpause the game (r4)

namespace lost_controller {
namespace {

constexpr uint32_t kGameGlobal = 0x8259B190;  // the game global (front end at +52)
constexpr uint32_t kFrontEnd = 52;
constexpr uint32_t kCoOpState = 0x8259B11C;   // co-op state per player (4 entries, more_players)
constexpr uint32_t kOwner = 8536;             // front end: the menu's owner (player, -1 = none)
// The game manager (*kGameGlobal): byte +168 = pause bits: 0x20 = the game is
// paused (sub_8227C8E8(manager, on) sets it, pauses the sound and the
// rumble). The question box closes itself in a level unless 0x20 is set (its
// update, 0x8225DEB4).
constexpr uint32_t kPauseBits = 168;
constexpr uint8_t kGamePaused = 0x20;
// The front end's "a menu is on screen in play" (+8594 bit 0x80: the pause
// menu, the totem's question and save list; menu_input.cpp). Bit 0x10 of the
// pause bits looked like "pause menu up" but stays clear behind it: a test
// opened the box over player 2's pause menu, and dropping player 2 out from
// there made the game read null + 0x14 in a loop (2026-10-06).
constexpr uint32_t kMenuOnScreen = 8594;
constexpr uint8_t kMenuOnScreenBit = 0x80;
constexpr int kMaxPlayers = 4;

// THE GAME'S QUESTION BOX ("Do you want to save the game?" at a save totem),
// the front end's object at +172. Script methods ShowConfirmMessage
// (sub_8225F758: text by name into the text element front end +192, f1 = an
// automatic answer after that many seconds, -1 = none, then shown),
// ResetConfirmMessage, IsConfirmMessageDone. The box: +4 flags (0x80 set up,
// 0x40 showing), +16 its Yes / No menu (selection byte +152), +24 the
// countdown text, +28 state (1 open, 2 closing), +36 fade (0..1, grows with
// the frame time in its update sub_8225DE68), +40 the timer, +44 answer flags;
// sub_8225E1D8 = done + hidden. Its update reads left / right / A through
// IsButtonPressed: with every pad muted here it never answers by itself.
constexpr uint32_t kBox = 172;
constexpr uint32_t kBoxText = 192;            // front end: the box's text element
constexpr uint32_t kBoxFlags = 4, kBoxMenu = 16, kBoxFade = 36;
constexpr uint8_t kBoxShowing = 0x40;
constexpr uint32_t kElementFlags = 110;       // a Scrooby element: bit 0x80 = VISIBLE (save_library.cpp)
constexpr uint8_t kElementVisible = 0x80;
constexpr uint32_t kAnyTextName = 0x82023570;  // "InGame_Pause_Paused": a name the box can look up
constexpr float kNoTimer = -1.0f;
constexpr uint32_t kTextChars = 255;           // our text's room (+ the NUL)
constexpr uint32_t kFrontEndHeap = 7;          // the heap the box's own text changes use

// XINPUT_GAMEPAD buttons (big-endian u16 at +0 of the gamepad); prompt
// glyphs in the game's fonts (findings/23): U+00B4 = the Y button.
constexpr uint16_t kButtonY = 0x8000;
constexpr char16_t kGlyphY = u'´';

// What the box says (game's main thread writes, input poll reads).
enum Phase : int { kNone, kLost, kBack };
std::atomic<int> g_phase{kNone};
std::atomic<int> g_lost{-1};          // the player concerned
std::atomic<bool> g_can_drop{false};  // someone else is in game: "drop out" offered
// Presses taken by FilterPad, carried out by Update on the next frame.
std::atomic<bool> g_want_drop{false}, g_want_resume{false};
// The buttons each socket held at its last read: only a NEW press counts.
std::array<uint16_t, kMaxPlayers> g_held{};

// Game's main thread only.
uint32_t g_text = 0;          // our UTF-16 text in guest memory
bool g_we_paused = false;     // the game was running when the box opened: unpause at the end
bool g_menu_was_hidden = false;
int g_shown_phase = kNone;    // what the box currently says
bool g_shown_solo = false;

uint8_t* Guest(uint32_t a) { return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(a); }
uint32_t Read32(uint32_t a) {
  const uint8_t* p = Guest(a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Write32(uint32_t a, uint32_t v) {
  uint8_t* p = Guest(a);
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
void WriteFloat(uint32_t a, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  Write32(a, v);
}

bool InGame(int p) { return Read32(kCoOpState + 4 * p) != 0; }
bool HasDevice(int p) {
  const kbm::PlayerAssignment* assignment = kbm::PlayerAssignment::Get();
  return !assignment || assignment->HasDeviceFor(p);  // no assignment: never "lost"
}

// Calls a game function with r3/r4 (and f1); the caller's registers are put back.
void CallGame(void (*function)(PPCContext&, uint8_t*), PPCContext& ctx, uint8_t* base, uint32_t r3,
              uint32_t r4 = 0, double f1 = 0.0) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.f1.f64 = f1;
  function(ctx, base);
  ctx = saved;
}

// Makes heap `heap` the current one (the game's sub_823574E8); returns the
// previous one, to put back the same way.
uint32_t UseHeap(PPCContext& ctx, uint8_t* base, uint32_t heap) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = heap;
  __imp__sub_823574E8(ctx, base);
  const uint32_t previous = ctx.r3.u32;
  ctx = saved;
  return previous;
}

// The box's text (the game's own font, wrapped by the text element; "´" is
// drawn as the Y button). `solo` = nobody else is in game: no player number,
// no drop out (the last player in game can't drop out).
std::u16string Message(int phase, int p, bool solo) {
  const std::u16string n(1, char16_t(u'1' + p));
  if (solo) {
    return phase == kBack ? u"Controller reconnected.\nPress any button to resume."
                          : u"Controller disconnected.\nReconnect your controller to continue.";
  }
  if (phase == kBack) {
    return u"Player " + n + u"'s controller is back.\nPlayer " + n +
           u": press any button to resume.";
  }
  return u"Player " + n + u"'s controller disconnected.\nReconnect it, or press " +
         std::u16string(1, kGlyphY) + u" to drop out Player " + n + u".";
}

void SetText(PPCContext& ctx, uint8_t* base, uint32_t fe, const std::u16string& text) {
  if (!g_text) g_text = rex::system::kernel_memory()->SystemHeapAlloc(2 * (kTextChars + 1));
  if (!g_text) return;
  size_t i = 0;
  for (; i < text.size() && i < kTextChars; ++i) {  // big-endian UTF-16
    Guest(g_text + 2 * uint32_t(i))[0] = uint8_t(text[i] >> 8);
    Guest(g_text + 2 * uint32_t(i))[1] = uint8_t(text[i]);
  }
  Guest(g_text + 2 * uint32_t(i))[0] = Guest(g_text + 2 * uint32_t(i))[1] = 0;
  const uint32_t element = Read32(fe + kBoxText);
  if (!element) return;
  // The text element copies the string into memory from the CURRENT heap.
  // Called from where this runs it was a full one: the copy failed, the game
  // wrote an AssertLog and stopped (2026-10-06, gdb on the assert writer
  // sub_8227A7A0: ... sub_8227E890 <- sub_82373360). The box's own update
  // makes heap 7 current around its text change (0x8225E160) and puts the
  // previous one back: the same here (and around opening / closing the box).
  const uint32_t heap = UseHeap(ctx, base, kFrontEndHeap);
  CallGame(__imp__sub_82373360, ctx, base, element, g_text);
  UseHeap(ctx, base, heap);
}

// The Yes / No row has nothing to do here: hidden while the box is ours.
void HideMenu(uint32_t fe, bool hide) {
  const uint32_t menu = Read32(fe + kBox + kBoxMenu);
  if (!menu) return;
  uint8_t& flags = *Guest(menu + kElementFlags);
  if (hide) {
    g_menu_was_hidden = !(flags & kElementVisible);
    flags &= ~kElementVisible;
  } else if (!g_menu_was_hidden) {
    flags |= kElementVisible;
  }
}

// Players dropped out from the box whose Drop Out hasn't finished yet (it
// completes once the game runs again): not "waiting", not "someone else".
// Game's main thread only.
std::array<bool, kMaxPlayers> g_dropping{};

// A player in game without a device, not being dropped out: needs the box.
bool Waiting(int p) { return InGame(p) && !HasDevice(p) && !g_dropping[p]; }
// Is anyone else in game (so p may be dropped out)?
bool SomeoneElse(int p) {
  for (int q = 0; q < more_players::LocalPlayerCount(); ++q) {
    if (q != p && InGame(q) && !g_dropping[q]) return true;
  }
  return false;
}
int FirstWaiting() {
  for (int p = 0; p < more_players::LocalPlayerCount(); ++p) {
    if (Waiting(p)) return p;
  }
  return -1;
}

// The box is about player p now (it lost its controller).
void ShowFor(PPCContext& ctx, uint8_t* base, uint32_t fe, int p) {
  for (uint16_t& held : g_held) held = 0xFFFF;  // wait for every button to be let go first
  g_want_drop = g_want_resume = false;
  g_can_drop = SomeoneElse(p);
  g_lost = p;
  g_phase = kLost;
  SetText(ctx, base, fe, Message(kLost, p, !g_can_drop.load()));
  g_shown_phase = kLost;
  g_shown_solo = !g_can_drop.load();
  REXLOG_INFO("Lost controller: player {} has no controller: game paused{}", p + 1,
              g_can_drop.load() ? " (reconnect / drop out)" : " (reconnect; the only player in game)");
}

void Open(PPCContext& ctx, uint8_t* base, uint32_t fe, int p) {
  const uint32_t manager = Read32(kGameGlobal);
  g_we_paused = !(*Guest(manager + kPauseBits) & kGamePaused);
  if (g_we_paused) CallGame(__imp__sub_8227C8E8, ctx, base, manager, 1);
  const uint32_t heap = UseHeap(ctx, base, kFrontEndHeap);
  CallGame(__imp__sub_8225F758, ctx, base, fe, kAnyTextName, kNoTimer);
  UseHeap(ctx, base, heap);
  HideMenu(fe, true);
  ShowFor(ctx, base, fe, p);
}

// Player g_lost is dealt with. Another player waiting (two controllers gone
// at once): the box goes straight on to them, the game stays paused.
// Else the box closes and the game goes on.
void Finish(PPCContext& ctx, uint8_t* base, uint32_t fe, const char* why) {
  REXLOG_INFO("Lost controller: player {} {}", g_lost.load() + 1, why);
  const int next = FirstWaiting();
  if (next >= 0) return ShowFor(ctx, base, fe, next);
  REXLOG_INFO("Lost controller: nobody else waiting: game goes on");
  const uint32_t heap = UseHeap(ctx, base, kFrontEndHeap);
  CallGame(__imp__sub_8225E1D8, ctx, base, fe + kBox);
  UseHeap(ctx, base, heap);
  HideMenu(fe, false);
  g_phase = kNone;
  g_lost = -1;
  g_shown_phase = kNone;
  g_want_drop = g_want_resume = false;
  if (g_we_paused) CallGame(__imp__sub_8227C8E8, ctx, base, Read32(kGameGlobal), 0);
  g_we_paused = false;
}

// Once per game frame in a level (the frame function's call of the game's
// own check, replaced below). `fe` = the front end.
void Update(PPCContext& ctx, uint8_t* base, uint32_t fe) {
  for (int p = 0; p < kMaxPlayers; ++p) {
    if (!InGame(p) || HasDevice(p)) g_dropping[p] = false;  // dropped out (or back after all)
  }
  const int lost = g_lost.load();
  if (lost < 0) {
    const int p = FirstWaiting();
    if (p < 0) return;
    // The box may be busy with the game's own question (a save totem), and
    // does nothing behind the pause menu: wait for both (the pause menu of
    // a player without a controller answers to anyone: menu_input.cpp).
    if (*Guest(fe + kBox + kBoxFlags) & kBoxShowing) return;
    if (*Guest(fe + kMenuOnScreen) & kMenuOnScreenBit) return;
    return Open(ctx, base, fe, p);
  }
  if (!InGame(lost)) return Finish(ctx, base, fe, "is out of the game");
  // Phase: lost <-> back, as the device comes and goes.
  const int phase = HasDevice(lost) ? kBack : kLost;
  if (phase != g_phase.load()) {
    if (phase == kBack) {
      for (uint16_t& held : g_held) held = 0xFFFF;  // a fresh press, after reconnecting
      g_want_resume = false;
    }
    g_phase = phase;
  }
  g_can_drop = SomeoneElse(lost);
  if (phase == kLost && g_want_drop.exchange(false) && g_can_drop.load()) {
    // The pause menu's Drop Out, for this player: it acts on the menu owner,
    // so the owner is this player for the call. It completes once the game
    // runs again: until then the player counts as being dropped.
    const uint32_t owner = Read32(fe + kOwner);
    Write32(fe + kOwner, uint32_t(lost));
    CallGame(__imp__sub_82266040, ctx, base, fe);
    Write32(fe + kOwner, owner);
    g_dropping[lost] = true;
    return Finish(ctx, base, fe, "was dropped out");
  }
  if (phase == kBack && g_want_resume.exchange(false)) {
    return Finish(ctx, base, fe, "pressed a button on its controller");
  }
  const bool solo = !g_can_drop.load();
  if (g_shown_phase != phase || g_shown_solo != solo) {
    SetText(ctx, base, fe, Message(phase, lost, solo));
    g_shown_phase = phase;
    g_shown_solo = solo;
  }
  // Shown at full strength while it's ours (it fades in with the frame time).
  WriteFloat(fe + kBox + kBoxFade, 1.0f);
}

}  // namespace

bool Active() { return g_phase.load() != kNone; }

void FilterPad(int user, uint8_t* gamepad, bool connected) {
  const int phase = g_phase.load();
  if (phase == kNone || user < 0 || user >= kMaxPlayers || !gamepad) return;
  const uint16_t buttons = connected ? uint16_t(gamepad[0] << 8 | gamepad[1]) : 0;
  const uint16_t pressed = buttons & ~g_held[user];
  g_held[user] = buttons;
  if (phase == kLost && (pressed & kButtonY)) g_want_drop = true;              // anyone
  if (phase == kBack && pressed && user == g_lost.load()) g_want_resume = true;  // that player
  std::memset(gamepad, 0, 12);  // the game sees a connected, untouched pad
}

}  // namespace lost_controller

// The front end's per-frame "controller missing" check (r3 = front end;
// called by the frame function sub_8227C5D8 at 0x8227C7CC, by sub_822B3D60
// (the frame function's call in a level) and by the device-change handler
// sub_82274CF8). In a level it is ours (lost_controller.h); in the menus it
// stays the game's.
extern "C" REX_FUNC(sub_82262D78) {
  using namespace lost_controller;
  if (!REXCVAR_GET(lost_controller) || !more_players::InPlay() ||
      ctx.r3.u32 != Read32(Read32(kGameGlobal) + kFrontEnd)) {
    __imp__sub_82262D78(ctx, base);
    return;
  }
  Update(ctx, base, ctx.r3.u32);
}
