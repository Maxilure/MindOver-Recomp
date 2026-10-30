// =============================================================================
// input/title_any_button.cpp -- see title_any_button.h
// =============================================================================
#include "title_any_button.h"

#include <array>
#include <chrono>
#include <mutex>
#include <string>

#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "../data/fight_tree.h"
#include "../guest_memory.h"
#include "../ui/scrooby.h"
#include "keyboard_mouse.h"
#include "movie_skip.h"
#include "players.h"
#include "../language/texts.h"

namespace title_any_button {
namespace {

using Clock = std::chrono::steady_clock;

// The game object (*0x8259B190) +52 = the front end; its +8594 bit 0x10 =
// the title waits for START (the check at 0x822651DC reads it).
constexpr uint32_t kGameGlobal = 0x8259B190, kFrontEnd = 52;
constexpr uint32_t kFrontEndFlags2 = 8594;
constexpr uint8_t kTitleWaits = 0x10;
// The title screen's front-end state (Frontend.bfig node 55, findings/25).
constexpr int32_t kTitleState = 55;

// X_INPUT_GAMEPAD (guest, big-endian): u16 buttons at +0, u8 left / right
// trigger at +2 / +3, then the sticks (left alone: a stick isn't a button).
constexpr uint32_t kTriggerLeft = 2, kTriggerRight = 3;
// A trigger counts as pressed past this (XInput's own threshold).
constexpr uint8_t kTriggerPressed = 30;
// How long the START press the title gets lasts (a few polls at any frame
// rate; the title acts on the first one it sees).
constexpr auto kStartHold = std::chrono::milliseconds(150);

// The title's text, in every language (the port's own words, src/language/texts.h).
constexpr std::u16string_view kOurText = trn(u"Press Any Button");  // shown as tr()
constexpr uint32_t kTextString = 156;  // a text element's string (UTF-16 BE)

uint32_t Read32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = GuestPtr(base, a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

uint32_t FrontEnd(const uint8_t* base) {
  const uint32_t game = Read32(base, kGameGlobal);
  return game ? Read32(base, game + kFrontEnd) : 0;
}

// Per socket (game threads; one lock, the polls are a few per frame).
struct Socket {
  uint16_t last_buttons = 0;      // the buttons at the last poll (what's NEW)
  bool last_trigger = false;      // either trigger past the threshold then
  int64_t seen_key_down = 0;      // the keyboard's last_key_down() seen then
  Clock::time_point start_until{};  // our START press lasts until then
  bool start_from_key = false;    // that START stands for a key / mouse press
  uint16_t hidden = 0;            // buttons held from the title: hidden until let go
};
std::mutex g_mutex;
std::array<Socket, 4> g_sockets;

}  // namespace

uint16_t FilterPad(uint32_t user, uint8_t* gamepad, uint8_t* base) {
  if (!gamepad || user >= g_sockets.size()) return 0;
  const uint32_t fe = FrontEnd(base);
  const bool title = fe && (*GuestPtr(base, fe + kFrontEndFlags2) & kTitleWaits);
  // A movie in a level with its Skip prompt hidden waits for START the same
  // way (input/movie_skip.h): any press brings the prompt up.
  const bool movie = !title && movie_skip::WaitsForAnyButton();
  const uint16_t buttons = uint16_t(gamepad[0] << 8 | gamepad[1]);
  const bool trigger =
      gamepad[kTriggerLeft] > kTriggerPressed || gamepad[kTriggerRight] > kTriggerPressed;
  // The keyboard and mouse: ANY key or mouse button, bound or not (an
  // unbound key adds nothing to the pad), on the socket the keyboard plays on.
  int64_t key_down = 0;
  uint16_t kb_buttons = 0;  // the keyboard's own buttons merged into this state
  const kbm::PlayerAssignment* assignment = kbm::PlayerAssignment::Get();
  const kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  if (driver && assignment && assignment->KeyboardAnswersFor(user)) {
    key_down = driver->last_key_down();
    kb_buttons = driver->last_pad().buttons;
  }

  std::lock_guard lock(g_mutex);
  Socket& s = g_sockets[user];
  const auto now = Clock::now();
  const uint16_t new_buttons = buttons & ~s.last_buttons;
  const bool trigger_pressed = trigger && !s.last_trigger;
  const bool key_pressed = key_down != 0 && key_down != s.seen_key_down;
  const bool pressed = new_buttons != 0 || trigger_pressed || key_pressed;
  s.last_buttons = buttons;
  s.last_trigger = trigger;
  s.seen_key_down = key_down;
  if (title || movie) {
    if (pressed && now >= s.start_until) {
      s.start_until = now + kStartHold;
      // A key, with no new controller button beside it (the keyboard's own
      // buttons are merged into this state: Escape = B during a movie).
      s.start_from_key = key_pressed && (new_buttons & ~kb_buttons) == 0 && !trigger_pressed;
      REXLOG_INFO("{}: a button on socket {}: START", title ? "Title" : "Movie skip", user);
    }
    // Only START reaches the title / the hidden Skip prompt (B, Back... do
    // nothing of their own here); what's held now stays hidden afterwards
    // until let go (the A that brought the prompt up doesn't also skip).
    const uint16_t out = now < s.start_until ? rex::input::X_INPUT_GAMEPAD_START : 0;
    gamepad[0] = uint8_t(out >> 8);
    gamepad[1] = uint8_t(out);
    gamepad[kTriggerLeft] = gamepad[kTriggerRight] = 0;
    s.hidden = buttons;
    // A START standing for a key press isn't a controller (prompt pictures).
    return s.start_from_key ? out : 0;
  }
  s.start_until = {};
  if (s.hidden) {
    s.hidden &= buttons;  // let go = shown again from the next press on
    const uint16_t out = buttons & ~s.hidden;
    gamepad[0] = uint8_t(out >> 8);
    gamepad[1] = uint8_t(out);
  }
  return 0;
}

void FrontEndFrame(PPCContext& ctx, uint8_t* base) {
  if (fight_tree::CurrentFrontEnd().state != kTitleState) return;
  const uint32_t page = scrooby::FindPage(ctx, base, "GameStart_Title");
  const uint32_t text = page ? scrooby::FindText(ctx, base, page, "PressStart") : 0;
  if (!text) return;
  const uint32_t string = Read32(base, text + kTextString);
  if (!string) return;
  std::u16string now;
  for (uint32_t i = 0; i < 64; ++i) {
    const uint8_t* c = GuestPtr(base, string + 2 * i);
    const char16_t ch = char16_t(c[0] << 8 | c[1]);
    if (!ch) break;
    now.push_back(ch);
  }
  // Any language: the element is the title's own "Press START" line (in
  // the game's text language), and ours is in the port's one (texts.h).
  if (now == tr(kOurText)) return;  // ours already
  scrooby::SetText(ctx, base, text, tr(kOurText));
  REXLOG_INFO("Title: the \"Press START\" line -> \"Press Any Button\" (in the text language)");
}

}  // namespace title_any_button
