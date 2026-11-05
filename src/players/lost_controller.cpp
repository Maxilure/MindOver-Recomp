// players/lost_controller.cpp -- see lost_controller.h.
#include "lost_controller.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../input/keyboard_mouse.h"
#include "../input/players.h"
#include "../input/prompt_icons.h"
#include "more_players.h"
#include "../language/texts.h"

REXCVAR_DEFINE_BOOL(lost_controller, true, "CrashMoM",
                    "A player in a level loses its controller: pause with the game's question box, any "
                    "player can drop that player out, the player presses a button to resume once it's "
                    "back (instead of the game's own pause, which only the missing controller could "
                    "close; findings/26)");

extern "C" REX_FUNC(__imp__sub_82262D78);  // the front end's "controller missing" check (r3 = front end)
extern "C" REX_FUNC(__imp__sub_82266040);  // the pause menu's Drop Out (drops the menu owner, +8536)
extern "C" REX_FUNC(sub_82266040);         // ... through the port's wrapper below (the rules + unjacking)
extern "C" REX_FUNC(__imp__sub_8221F1E8);  // CheckInputEvent (r3 = behaviour controller, r4 map, r5 event)
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
// countdown text, +28 state (0 / 1 open, 2 closing: input/mouse_menus.cpp),
// +36 fade (0..1, grows with the frame time in its update sub_8225DE68), +40
// the timer, +44 answer flags;
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
constexpr uint16_t kButtonStart = 0x0010;
constexpr char16_t kGlyphY = u'´';
constexpr char16_t kGlyphStart = u'¶';  // drawn in player 1's look (prompt_icons.h)

// TAKING OVER (2026-10-30, playtest): player 1 dropped out, player 2 played
// on alone, then player 2's controller went: the box said "Reconnect your
// controller to continue" (the last player in game can't drop out) and
// player 1, keyboard in hand, could do nothing: the game's own join (front
// end per-frame update sub_82264988, START or B from a controller of a free
// player) refuses while the game is paused (+168 bit 0x20 at 0x82265050), and
// the box keeps it paused. Now, when the box is about the ONLY player in game
// and another player with a device is out ("a joiner"):
//   * the box says "... or Player 1: press START to take over.";
//   * that START closes the box and the game runs again; the lost player is
//     left out of the box for up to kJoinWindow. The START, still held, now
//     reaches the running game (the box hid it until then: a fresh press for
//     the game) and the game's own join takes it, as in play (no press faked);
//   * once the joiner is in game, the player without a controller drops out
//     (the box's usual Y): play goes on with the joiner (player 1 comes back
//     as the mask; the drop-out forces riders out: on foot).
//   * no join within kJoinWindow (a tap too short): the box comes back.
// TRIED FIRST: lifting the pause bit (and +8594 bit 0x80, "menu on screen",
// whose path at 0x82264B34 skips the join) around the update while paused,
// with that START let through: the join picked player 1 but never finished
// (no controller stored: it completes through a message to the player's
// character, unanswered while the game is paused).
constexpr auto kJoinWindow = std::chrono::seconds(2);  // the press's chance to be taken

// What the box says (game's main thread writes, input poll reads).
enum Phase : int { kNone, kLost, kBack };
std::atomic<int> g_phase{kNone};
std::atomic<int> g_lost{-1};          // the player concerned
std::atomic<bool> g_can_drop{false};  // someone else is in game: "drop out" offered
// Presses taken by FilterPad, carried out by Update on the next frame.
std::atomic<bool> g_want_drop{false}, g_want_resume{false};
// Taking over: the player the box offers it to (-1 = none; input thread
// reads), their START seen (FilterPad -> Update).
std::atomic<int> g_offered_joiner{-1};
std::atomic<bool> g_want_takeover{false};
// Game's main thread: a take-over under way (box closed, game running):
// who joins, who drops out once they're in, and until when.
int g_handover_joiner = -1, g_handover_lost = -1;
std::chrono::steady_clock::time_point g_handover_until;
// STEPPING IN (2026-11-05, playtest idea): the box is about the LAST PLAYER ON
// FOOT and the others ride them as masks (MayLeave says no: a mask needs a
// Crash). Any of those masks with a device presses Y: that player comes OUT
// of the mask (the game's own "forced out of the mask", sub-state 4 = what
// its Drop Out does to a leaving player's riders, sub_822357B0 path B), the
// box closes and the game runs; once they're on foot (co-op state 2) the
// lost player drops out (now allowed: someone else is on foot). Their other
// masks move onto the player who stepped in (more_players::MoveMasks). Not on foot
// within kStepInWindow (the game kept the mask): the box asks again.
// Who pressed: FilterPad (input poll) -> Update. The lost player is left out
// of the box meanwhile (Waiting).
constexpr auto kStepInWindow = std::chrono::seconds(3);
constexpr uint32_t kSubState = 0x824F3C38;  // co-op sub-state per player (4 entries, more_players)
constexpr uint32_t kSubForcedOutOfMask = 4;
std::atomic<int> g_drop_presser{-1};
// A MOMENT BEFORE THE BOX LISTENS (2026-11-05, playtest): presses in the first
// kListenDelay after the box opens (or moves on to another player) are
// ignored, so a button someone was mashing in the fight doesn't drop a player
// out the instant it pops up. (Steady-clock nanoseconds; FilterPad's thread
// reads it.)
constexpr auto kListenDelay = std::chrono::milliseconds(1000);
std::atomic<int64_t> g_listen_from{0};
bool Listening() { return std::chrono::steady_clock::now().time_since_epoch().count() >= g_listen_from.load(); }
int g_stepin_player = -1, g_stepin_lost = -1;
bool g_stepin_keyboard = false;  // ... about player 1's controller (they still have the keyboard)
std::chrono::steady_clock::time_point g_stepin_until;
// The buttons each socket held at its last read: only a NEW press counts.
std::array<uint16_t, kMaxPlayers> g_held{};

// Game's main thread only.
uint32_t g_text = 0;          // our UTF-16 text in guest memory
bool g_we_paused = false;     // the game was running when the box opened: unpause at the end
bool g_menu_was_hidden = false;
std::u16string g_shown_text;  // what the box currently says

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

// PLAYER 1'S CONTROLLER (2026-11-05, for couch play): player 1 is the
// keyboard and mouse plus, optionally, one controller (players/players_setup.h),
// so player 1 never "has no device" and unplugging their controller went
// unnoticed: the game ran on while they sat away from the keyboard. Now, when
// the controller player 1 played with is UNPLUGGED (gone from the device list;
// moving it to another player on the Players screen is not this) while player
// 1 is in a level and the keyboard is theirs, the box pauses the game like for
// any player: "Player 1's controller disconnected. Reconnect it, or press any
// key to continue on the keyboard." Any key / mouse button (the keyboard
// driver's last_key_down moves) resumes on the keyboard; the controller back
// = the usual "press any button to resume". No drop out offered (player 1
// still has the keyboard). Game main thread.
std::string g_p1_pad;            // the controller player 1 played with (last frame), "" = none
bool g_p1_pad_lost = false;      // ... was unplugged: the box is (or will be) about it
int64_t g_p1_key_seen = 0;       // the keyboard's last_key_down() when the box opened
bool KeyboardCase(int p) { return p == 0 && g_p1_pad_lost; }
// Does player p have what the box waits for: their device (player 1 in the
// keyboard case: a controller again)?
bool DeviceBack(int p) {
  if (!KeyboardCase(p)) return HasDevice(p);
  const kbm::PlayerAssignment* assignment = kbm::PlayerAssignment::Get();
  return assignment && !assignment->PadKeyFor(0).empty();
}
// Once per frame in a level: notices player 1's controller going away.
void WatchPlayerOnePad() {
  const kbm::PlayerAssignment* assignment = kbm::PlayerAssignment::Get();
  if (!assignment) return;
  const std::string pad = assignment->PadKeyFor(0);
  if (!pad.empty()) {
    g_p1_pad = pad;
    return;
  }
  if (g_p1_pad.empty()) return;
  if (!assignment->IsConnected(g_p1_pad) && Read32(kCoOpState) != 0 && assignment->KeyboardAnswersFor(0)) {
    REXLOG_INFO("Lost controller: player 1's controller ({}) was unplugged: the keyboard can carry on", g_p1_pad);
    g_p1_pad_lost = true;
  }
  g_p1_pad.clear();  // (moved to another player / off: simply forgotten)
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
// `others` = someone else is in game, but p can't drop out (the last player
// on foot, the others are masks): numbered, no drop out.
std::u16string Message(int phase, int p, bool solo, int joiner = -1, bool others = false,
                       bool step_in = false) {
  const std::u16string n(1, char16_t(u'1' + p));
  if (KeyboardCase(p) && phase == kLost && (!solo || step_in)) {
    // ... and the others may drop player 1 out (or step in, as masks).
    // TR: the question box, 5 lines broken by hand (keep each about as short as the English): player 1's controller was unplugged; any key = they play on with the keyboard and mouse; {0} = the Y button's picture, pressed by another player.
    // The Y in the look of the OTHER players' controllers (they press it):
    // player 1's own look would be the keyboard's (Backspace), which here
    // means "any key". The first other player in game with a device.
    int presser = -1;
    for (int q = 1; q < more_players::LocalPlayerCount() && presser < 0; ++q) {
      if (InGame(q) && HasDevice(q)) presser = q;
    }
    const std::u16string y(1, prompt_icons::MenuGlyph(kGlyphY));
    return trf(tr(u"Player 1's controller\ndisconnected.\nReconnect it, or press any\nkey to use the keyboard.\nOr {0} to drop out Player 1."),
               {presser > 0 ? prompt_icons::TextForPlayer(y, presser) : y});
  }
  if (KeyboardCase(p) && phase == kLost) {
    // TR: the question box, 4 lines broken by hand (keep each about as short as the English): player 1's controller was unplugged; they can play on with the keyboard and mouse.
    // Four even lines, broken by hand to fit the box (about 26 letters a
    // line): the first wording wrapped into five ragged lines.
    return tr(u"Player 1's controller\ndisconnected.\nReconnect it, or press any\nkey to use the keyboard.");
  }
  if (KeyboardCase(p)) solo = false;  // back: the numbered "press any button" lines
  // STEPPING IN: a mask's Y drops p out (that mask comes out on foot): the
  // usual drop-out line, said further down.
  if (solo && others && phase == kLost && step_in) solo = false;
  if (solo && others && phase == kLost) {
    // TR: the question box (2 lines): {0} = the player's number (the last player on foot: no drop out).
    return trf(tr(u"Player {0}'s controller disconnected.\nReconnect it to continue."), {n});
  }
  if (solo && others) solo = false;  // back: the numbered "press any button" lines
  if (solo && phase == kLost && joiner >= 0) {
    // Someone out of the game can take over (TAKING OVER above).
    // TR: the question box (2 lines): {0} = the player whose controller went, {1} = the player who can take over, {2} = the START button's picture.
    return trf(tr(u"Player {0}'s controller disconnected.\nReconnect it, or Player {1}: press {2} to take over."),
               {n, std::u16string(1, char16_t(u'1' + joiner)), std::u16string(1, kGlyphStart)});
  }
  if (solo) {
    return phase == kBack ? tr(u"Controller reconnected.\nPress any button to resume.")
                          : tr(u"Controller disconnected.\nReconnect your controller to continue.");
  }
  if (phase == kBack) {
    // TR: the question box (2 lines): {0} = the player's number.
    return trf(tr(u"Player {0}'s controller is back.\nPlayer {0}: press any button to resume."), {n});
  }
  // The MENU's Y picture, not the play prompt's: the box is a menu, so on the
  // keyboard its Y is the menu Y key (Backspace by default), while the play
  // prompt "´" showed Heavy attack's key (right mouse button), which in a menu
  // means Back: pressing what the box showed did nothing (2026-10-15).
  // TR: the question box (2 lines): {0} = the player's number, {1} = the Y button's picture.
  return trf(tr(u"Player {0}'s controller disconnected.\nReconnect it, or press {1} to drop out Player {0}."),
             {n, std::u16string(1, prompt_icons::MenuGlyph(kGlyphY))});
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
bool Waiting(int p) {
  return InGame(p) && (!HasDevice(p) || KeyboardCase(p)) && !g_dropping[p] && p != g_handover_lost &&
         p != g_stepin_lost;
}
// Is anyone else in game?
bool SomeoneElse(int p) {
  for (int q = 0; q < more_players::LocalPlayerCount(); ++q) {
    if (q != p && InGame(q) && !g_dropping[q]) return true;
  }
  return false;
}
// Is player p a mask (co-op state 1) or turning into one (3)?
bool IsMask(int p) {
  const uint32_t s = Read32(kCoOpState + 4 * p);
  return s == 1 || s == 3;
}
// Does anyone else stay ON FOOT (co-op state 2: a Crash of their own, walking
// or riding a titan)?
bool SomeoneElseOnFoot(int p) {
  for (int q = 0; q < more_players::LocalPlayerCount(); ++q) {
    if (q != p && Read32(kCoOpState + 4 * q) == 2 && !g_dropping[q]) return true;
  }
  return false;
}
// May player p leave the game? THE RULE (2026-11-05, from a playtest): someone
// else stays in game, and a player ON FOOT needs another player on foot to
// stay: the masks ride a Crash, so the last Crash can't go. Before, player 1
// dropping out while everyone else rode them as masks left the masks with no
// body: the game's Drop Out forced them out of their masks (sub-state 4, the
// original's two-player path B too) and they stood frozen where they were.
// A mask can always leave while someone else stays.
bool MayLeave(int p) { return SomeoneElse(p) && (IsMask(p) || SomeoneElseOnFoot(p)); }

// May player q step in for `lost` (STEPPING IN above)? q rides as a mask
// (state 1: settled, not turning into one), plays with a device, and `lost`
// can't leave only because nobody else is on foot.
bool CanStepIn(int lost, int q) {
  return q >= 0 && q < more_players::LocalPlayerCount() && q != lost && !g_dropping[q] &&
         Read32(kCoOpState + 4 * q) == 1 && HasDevice(q) && !MayLeave(lost) && SomeoneElse(lost);
}
bool AnyoneCanStepIn(int lost) {
  for (int q = 0; q < more_players::LocalPlayerCount(); ++q) {
    if (CanStepIn(lost, q)) return true;
  }
  return false;
}

// UNJACK FIRST (2026-11-05, from a playtest): a player riding a titan who
// dropped out took the TITAN with them (the mutant vanished). The game's own
// drop out ejects the rider with sub_82235678 (messages 5 + 1; the fight tree
// step sub_820C0EF0 does it for players 1-2's titans): tried first, the titan
// then vanished ~1 s later (photos) even with the drop out held back. The
// player's own dismount (B on a titan) leaves it standing, knocked down, free
// to be jacked again. So now Drop Out on a titan rider DISMOUNTS first, the
// way B does: the titan's compiled fight tree asks CheckInputEvent
// (sub_8221F1E8), through the titan's AI controller, about the rider's B: (4, 2) in the decision at 0x821FC1D8 (next to its
// "FORCE_DISMOUNT" check) and (6, 23) at 0x821FB998 / 0x821FBC38 (found by
// logging every "yes" of sub_8221F1E8 around one B press on the Ratcicle).
// Those questions, for THAT titan only, hear "yes" (the wrapper at the end):
// no button is faked, the tree goes its own dismount way (a script,
// CJackingBehaviour sub_8214D778, clears the titan slot). The player drops
// out once on foot (TitanOfPlayer = 0) plus kOffTime for the jump off. While
// waiting the player counts as dropping (not "someone else on foot"). The
// tree runs only while the game runs (the pause menu's Back for a player who
// isn't its owner asks while paused): kUnjackTime counts running time only;
// past it the drop out is given up (logged; the player stays in).
constexpr auto kUnjackTime = std::chrono::seconds(3);
constexpr auto kOffTime = std::chrono::milliseconds(1000);
std::array<bool, kMaxPlayers> g_unjacking{};
std::array<std::chrono::steady_clock::duration, kMaxPlayers> g_unjack_waited{}, g_unjack_off{};
std::chrono::steady_clock::time_point g_unjack_last_tick{};

// The game's Drop Out for player p (the menu owner is p for the call).
void CallDropOut(PPCContext& ctx, uint8_t* base, uint32_t fe, int p, bool rules) {
  const uint32_t owner = Read32(fe + kOwner);
  Write32(fe + kOwner, uint32_t(p));
  CallGame(rules ? sub_82266040 : __imp__sub_82266040, ctx, base, fe);
  Write32(fe + kOwner, owner);
}

// Once per frame: an unjacked player is on foot -> the drop out goes on.
void TickUnjacking(PPCContext& ctx, uint8_t* base, uint32_t fe) {
  const auto now = std::chrono::steady_clock::now();
  const auto dt = now - g_unjack_last_tick;
  g_unjack_last_tick = now;
  const uint32_t manager = Read32(kGameGlobal);
  const bool paused = manager && (*Guest(manager + kPauseBits) & kGamePaused);
  for (int p = 0; p < kMaxPlayers; ++p) {
    if (!g_unjacking[p]) continue;
    if (!InGame(p)) {
      g_unjacking[p] = false;
      continue;
    }
    const bool running = !paused && dt < std::chrono::milliseconds(250);
    if (!more_players::TitanOfPlayer(p)) {
      // Off the titan: the jump off plays first (kOffTime of running game).
      if (running) g_unjack_off[p] += dt;
      if (g_unjack_off[p] < kOffTime) continue;
      g_unjacking[p] = false;
      REXLOG_INFO("Lost controller: player {} is off the titan: drops out", p + 1);
      CallDropOut(ctx, base, fe, p, false);
      g_dropping[p] = true;
      continue;
    }
    if (running) g_unjack_waited[p] += dt;
    if (g_unjack_waited[p] > kUnjackTime) {
      g_unjacking[p] = g_dropping[p] = false;
      REXLOG_WARN("Lost controller: player {}'s titan didn't let go: drop out given up", p + 1);
    }
  }
}
// A player out of the game who has a device and could join (the box offers
// them to take over), -1 = none. The first one in player order.
int Joiner(int lost) {
  for (int q = 0; q < more_players::LocalPlayerCount(); ++q) {
    if (q != lost && !InGame(q) && HasDevice(q)) return q;
  }
  return -1;
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
  g_listen_from = (std::chrono::steady_clock::now() + kListenDelay).time_since_epoch().count();
  g_can_drop = MayLeave(p);
  g_lost = p;
  g_phase = kLost;
  if (const auto* driver = kbm::KeyboardMouseDriver::Get()) g_p1_key_seen = driver->last_key_down();
  g_want_takeover = false;
  // Taking over only when nobody else is in game (masks need their Crash).
  g_offered_joiner = g_can_drop.load() || SomeoneElse(p) || KeyboardCase(p) ? -1 : Joiner(p);
  g_drop_presser = -1;
  g_shown_text = Message(kLost, p, !g_can_drop.load(), g_offered_joiner.load(), SomeoneElse(p), AnyoneCanStepIn(p));
  SetText(ctx, base, fe, g_shown_text);
  REXLOG_INFO("Lost controller: player {} has no controller: game paused{}", p + 1,
              KeyboardCase(p)   ? (g_can_drop.load() || AnyoneCanStepIn(p) ? " (reconnect / any key = the keyboard / Y = drop out)"
                                                                         : " (reconnect / any key = the keyboard)")
              : g_can_drop.load() ? " (reconnect / drop out)"
              : SomeoneElse(p) ? " (reconnect; the last player on foot)"
                               : " (reconnect; the only player in game)");
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
  if (g_lost.load() == 0) g_p1_pad_lost = false;  // (player 1's keyboard case: dealt with)
  const int next = FirstWaiting();
  if (next >= 0) return ShowFor(ctx, base, fe, next);
  REXLOG_INFO("Lost controller: nobody else waiting: game goes on");
  const uint32_t heap = UseHeap(ctx, base, kFrontEndHeap);
  CallGame(__imp__sub_8225E1D8, ctx, base, fe + kBox);
  UseHeap(ctx, base, heap);
  HideMenu(fe, false);
  g_phase = kNone;
  g_lost = -1;
  g_shown_text.clear();
  g_want_drop = g_want_resume = false;
  g_offered_joiner = -1;
  g_want_takeover = false;
  if (g_we_paused) CallGame(__imp__sub_8227C8E8, ctx, base, Read32(kGameGlobal), 0);
  g_we_paused = false;
}

// Once per game frame in a level (the frame function's call of the game's
// own check, replaced below). `fe` = the front end.
void Update(PPCContext& ctx, uint8_t* base, uint32_t fe) {
  for (int p = 0; p < kMaxPlayers; ++p) {
    if (g_unjacking[p]) continue;  // (counts as dropping until it's off the titan)
    if (!InGame(p) || HasDevice(p)) g_dropping[p] = false;  // dropped out (or back after all)
  }
  TickUnjacking(ctx, base, fe);
  WatchPlayerOnePad();
  if (g_p1_pad_lost && (!InGame(0) || g_dropping[0])) g_p1_pad_lost = false;
  // A take-over under way: the joiner is in -> the lost player drops out;
  // the lost player's controller came back, or no join in time -> over (the
  // box asks again if it's still needed).
  if (g_handover_lost >= 0) {
    const int joiner = g_handover_joiner, lost = g_handover_lost;
    if (InGame(joiner) || !InGame(lost) || HasDevice(lost) || std::chrono::steady_clock::now() > g_handover_until) {
      g_handover_joiner = g_handover_lost = -1;
      if (InGame(joiner) && InGame(lost) && !HasDevice(lost)) {
        DropOut(ctx, base, lost, ("player " + std::to_string(joiner + 1) + " took over").c_str());
      } else if (!InGame(joiner)) {
        REXLOG_INFO("Lost controller: player {} didn't join in time: asking again", joiner + 1);
      }
    }
  }
  // Stepping in under way: the mask is on foot -> the lost player drops out;
  // the controller came back / the lost player is out / too long -> over.
  if (g_stepin_lost >= 0) {
    const int q = g_stepin_player, gone = g_stepin_lost;
    const bool on_foot = Read32(kCoOpState + 4 * q) == 2;
    // Still without it: no device / (player 1's controller case) no controller.
    // (HasDevice alone said "has one" for player 1's keyboard: the step-in
    // happened but player 1 never dropped out, first test.)
    const kbm::PlayerAssignment* assignment = kbm::PlayerAssignment::Get();
    const bool still_without = g_stepin_keyboard ? assignment && assignment->PadKeyFor(0).empty() : !HasDevice(gone);
    if (on_foot || !InGame(gone) || !still_without || std::chrono::steady_clock::now() > g_stepin_until) {
      g_stepin_player = g_stepin_lost = -1;
      if (on_foot && InGame(gone) && still_without) {
        // The other masks follow the player who stepped in (they'd all be
        // forced on foot by the Drop Out otherwise).
        more_players::MoveMasks(ctx, base, gone, q);
        DropOut(ctx, base, gone, ("player " + std::to_string(q + 1) + " stepped in on foot").c_str());
      } else if (!on_foot) {
        REXLOG_INFO("Lost controller: player {} didn't come out of the mask in time: asking again", q + 1);
      }
    }
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
  const int phase = DeviceBack(lost) ? kBack : kLost;
  if (phase != g_phase.load()) {
    if (phase == kBack) {
      for (uint16_t& held : g_held) held = 0xFFFF;  // a fresh press, after reconnecting
      g_want_resume = false;
    }
    g_phase = phase;
  }
  g_can_drop = MayLeave(lost);
  // Player 1 on the keyboard: any key / mouse button resumes (KeyboardCase).
  if (phase == kLost && KeyboardCase(lost)) {
    const auto* driver = kbm::KeyboardMouseDriver::Get();
    if (driver && !Listening()) g_p1_key_seen = driver->last_key_down();  // (the first second: ignored)
    if (driver && driver->last_key_down() != g_p1_key_seen) {
      return Finish(ctx, base, fe, "pressed a key: carries on with the keyboard and mouse");
    }
  }
  // Taking over: the box closes and the game runs, for the join (TAKING OVER).
  if (phase == kLost && g_want_takeover.exchange(false)) {
    const int joiner = g_offered_joiner.load();
    if (!g_can_drop.load() && joiner >= 0 && joiner == Joiner(lost)) {
      g_handover_joiner = joiner;
      g_handover_lost = lost;
      g_handover_until = std::chrono::steady_clock::now() + kJoinWindow;
      return Finish(ctx, base, fe,
                    ("is taken over by player " + std::to_string(joiner + 1) + ": game runs for the join").c_str());
    }
  }
  // Stepping in: a mask's Y (STEPPING IN above).
  if (phase == kLost && !g_can_drop.load() && g_want_drop.load()) {
    g_want_drop = false;
    const int q = g_drop_presser.exchange(-1);
    if (CanStepIn(lost, q)) {
      Write32(kSubState + 4 * q, kSubForcedOutOfMask);
      g_stepin_player = q;
      g_stepin_lost = lost;
      g_stepin_keyboard = KeyboardCase(lost);
      g_stepin_until = std::chrono::steady_clock::now() + kStepInWindow;
      return Finish(ctx, base, fe,
                    ("will drop out: player " + std::to_string(q + 1) + " comes out of the mask first").c_str());
    }
  }
  if (phase == kLost && g_want_drop.exchange(false) && g_can_drop.load() && g_drop_presser.exchange(-1) != lost) {
    // The pause menu's Drop Out, for this player: it acts on the menu owner,
    // so the owner is this player for the call. It completes once the game
    // runs again: until then the player counts as being dropped.
    CallDropOut(ctx, base, fe, lost, true);
    g_dropping[lost] = true;
    return Finish(ctx, base, fe, "was dropped out");
  }
  if (phase == kBack && g_want_resume.exchange(false)) {
    return Finish(ctx, base, fe, "pressed a button on its controller");
  }
  const bool solo = !g_can_drop.load(), others = SomeoneElse(lost);
  const int offered = solo && !others && phase == kLost && !KeyboardCase(lost) ? Joiner(lost) : -1;
  g_offered_joiner = offered;
  std::u16string text = Message(phase, lost, solo, offered, others, AnyoneCanStepIn(lost));
  if (text != g_shown_text) {  // (setting a text allocates in the menus' heap: only on a change)
    SetText(ctx, base, fe, text);
    g_shown_text = std::move(text);
  }
  // Shown at full strength while it's ours (it fades in with the frame time).
  WriteFloat(fe + kBox + kBoxFade, 1.0f);
}

}  // namespace

bool Active() { return g_phase.load() != kNone; }

bool CanDropOut(int p) { return p >= 0 && p < kMaxPlayers && InGame(p) && !g_dropping[p] && MayLeave(p); }

bool DropOut(PPCContext& ctx, uint8_t* base, int p, const char* why) {
  if (!CanDropOut(p)) return false;
  const uint32_t game = Read32(kGameGlobal);
  const uint32_t fe = game ? Read32(game + 52) : 0;
  if (!fe) return false;
  CallDropOut(ctx, base, fe, p, true);
  g_dropping[p] = true;
  REXLOG_INFO("Lost controller: player {} dropped out ({})", p + 1, why);
  return true;
}

void FilterPad(int user, uint8_t* gamepad, bool connected) {
  const int phase = g_phase.load();
  if (phase == kNone || user < 0 || user >= kMaxPlayers || !gamepad) return;
  const uint16_t buttons = connected ? uint16_t(gamepad[0] << 8 | gamepad[1]) : 0;
  const uint16_t pressed = Listening() ? uint16_t(buttons & ~g_held[user]) : 0;  // (the first second: ignored)
  g_held[user] = buttons;
  if (phase == kLost && (pressed & kButtonY)) {  // anyone (stepping in: who pressed counts)
    g_drop_presser = user;
    g_want_drop = true;
  }
  if (phase == kBack && pressed && user == g_lost.load()) g_want_resume = true;  // that player
  // Taking over: the offered player's START (TAKING OVER above).
  if (phase == kLost && user == g_offered_joiner.load() && (pressed & kButtonStart)) g_want_takeover = true;
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


// The game's Drop Out (r3 = front end; drops the menu owner, +8536: it sends
// the owner's Crash message 48/7, CCoOpBehaviour sub_822357B0). Every way in
// goes through here: the pause menu's own Drop Out pick (Back, exits 882 /
// 914: pause_drop_out.h), the box's Y and the Players screen (DropOut above).
// In a level the port's RULES first (MayLeave: the last player on foot stays),
// and a titan rider is unjacked first (UNJACK FIRST above).
extern "C" REX_FUNC(sub_82266040) {
  using namespace lost_controller;
  const uint32_t fe = ctx.r3.u32;
  const int p = int(int32_t(Read32(fe + kOwner)));
  if (!more_players::InPlay() || p < 0 || p >= kMaxPlayers || !InGame(p)) {
    __imp__sub_82266040(ctx, base);
    return;
  }
  if (!MayLeave(p)) {
    REXLOG_INFO("Lost controller: player {} can't drop out (the last player on foot / in game)", p + 1);
    return;
  }
  if (more_players::TitanOfPlayer(p)) {
    if (!g_unjacking[p]) {
      REXLOG_INFO("Lost controller: player {} rides a titan: off it first, then drops out", p + 1);
      g_unjacking[p] = g_dropping[p] = true;
      g_unjack_waited[p] = g_unjack_off[p] = {};
    }
    return;
  }
  __imp__sub_82266040(ctx, base);
}


// CheckInputEvent (sub_8221F1E8; r3 = a behaviour controller, r4 = the input
// map, r5 = the event; returns a bool). A rider being dropped out (UNJACK
// FIRST above): the dismount questions of the titan they ride are answered
// "yes", nothing else. WHO ASKS: the titan's own AI behaviour controller (an
// NPCBaseJackable, e.g. class Strong for the Ratcicle, vtable 0x82035B1C),
// which keeps its titan at +148 (and +156). Two earlier matches were each
// right for one Ratcicle only (2026-11-05 playtests): "the asker is titan +
// 0x40" (the save's own Ratcicle: its controller sits there) and "the asker's
// input controller is titan + 0x40" (the first spawned one); a second spawned
// Ratcicle had neither, and players stayed on their titan. +148 held for all
// three (console reads of the asker). Checked since on every jackable titan
// of the spawn list (Ratcicle, Spike, Scorporilla, Sludge, Yuktopus, TK,
// Shurtle, Roller, Battler, Phantom, Stinky, Parafox) with players 1-4, the
// stick held through the drop out; another player's titan stays ridden.
constexpr uint32_t kNpcTitan = 148;
extern "C" REX_FUNC(sub_8221F1E8) {
  using namespace lost_controller;
  const uint32_t controller = ctx.r3.u32, map = ctx.r4.u32, event = ctx.r5.u32;
  __imp__sub_8221F1E8(ctx, base);
  if (ctx.r3.u32 & 0xFF) return;
  const bool dismount = (map == 4 && event == 2) || (map == 6 && event == 23);
  if (!dismount) return;
  for (int p = 0; p < kMaxPlayers; ++p) {
    const uint32_t titan = g_unjacking[p] ? more_players::TitanOfPlayer(p) : 0;
    if (titan && Read32(controller + kNpcTitan) == titan) {
      ctx.r3.u64 = 1;
      return;
    }
  }
}
