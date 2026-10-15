// =============================================================================
// gameplay/game_mods.cpp -- game modifications: friendly fire, combos kept on a
// partner's hit, simple spinning (what each does and why: game_mods.h)
// =============================================================================
#include "game_mods.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../guest_memory.h"
#include "../input/keyboard_mouse.h"
#include "../input/players.h"
#include "../players/more_players.h"

REXCVAR_DEFINE_BOOL(friendly_fire, true, "CrashMoM",
                    "Co-op: players' attacks hurt each other, as in the original game. false = a "
                    "partner's hit passes harmlessly (no damage, no stagger, combos untouched)");
// Default false (the port's choice, 2026-10-15): partners may hit each other
// (friendly fire stays on, as in the original), but a partner's hit no longer
// costs your combo. true = the original game.
REXCVAR_DEFINE_BOOL(friendly_hits_break_combo, false, "CrashMoM",
                    "Co-op: a partner's hit ends your hit combo, as in the original game. false "
                    "(the default) = it doesn't");
REXCVAR_DEFINE_BOOL(simple_spinning, false, "CrashMoM",
                    "Spin with one button: LB on a controller (no use in play otherwise), the Spin "
                    "key on the keyboard, each doing what a full circle of the left stick does; "
                    "held = a spin after each one. false = the original (circle the stick; the "
                    "Spin key does nothing)");
REXCVAR_DEFINE_BOOL(keep_spinning, false, "CrashMoM",
                    "With simple_spinning: hold the spin (LB, the Spin key, or a stick kept circling) "
                    "to keep spinning: the game's Old Skool spin (spin, then Light attack again and "
                    "again) with the Light attack presses made for you. false = the original");
REXCVAR_DECLARE(bool, debug_coop_trace);

namespace game_mods {
namespace {

using Clock = std::chrono::steady_clock;

// --- Guest memory (big-endian) ------------------------------------------------
uint32_t Read32(uint8_t* base, uint32_t address) {
  const uint8_t* p = GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

uint32_t CallGame(void (*function)(PPCContext&, uint8_t*), PPCContext& ctx, uint8_t* base,
                  uint32_t r3, uint32_t r4 = 0) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  function(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

}  // namespace
}  // namespace game_mods

extern "C" REX_FUNC(__imp__sub_8226FCD8);  // player number of an actor (r3 = game object, r4 = actor)
extern "C" REX_FUNC(__imp__sub_82132FF8);  // CDamageableBehaviour slot 5: a message
extern "C" REX_FUNC(__imp__sub_82166198);  // CScoringBehaviour slot 5: a message

namespace game_mods {
namespace {

constexpr uint32_t kGameObject = 0x825B0008;  // sub_82270608's answer: the players' lists

// Which player (0-3) is this actor: their Crash, the titan they ride or the
// mask (the game's own sub_8226FCD8, 4 players with more_players' hooks);
// -1 = not a player's.
int PlayerOf(PPCContext& ctx, uint8_t* base, uint32_t actor) {
  return actor ? int(int32_t(CallGame(__imp__sub_8226FCD8, ctx, base, kGameObject, actor))) : -1;
}

// --- Messages (findings/40 s.1) -------------------------------------------------
constexpr uint32_t kMsgType = 4;      // s32
constexpr uint32_t kMsgSubtype = 16;  // s32
constexpr uint32_t kMsgData = 20;     // -> +4 = the hit's record
constexpr int kMsgDamage = 12;        // subtypes 0 = a hit, 3 = a hitpoint change
constexpr int kMsgScore = 31;         // subtypes 0 = "you landed one", 1 = "you were hit"
constexpr int kDamageHit = 0, kDamageChange = 3;
constexpr int kDamageBreakCombo = 1;  // 12 / 1: the scoring behaviour breaks the combo too
constexpr uint32_t kHitFrom = 20;     // the actor that hit
constexpr uint32_t kHitScorer = 40;   // the actor that gets the combo point

// A partner's hit that went through (friendly fire on) while combos are kept:
// the hit one's next "you were hit" combo message is dropped. Kept 0.25 s (the
// messages come right after the hit; a stale entry must not eat a real one).
struct Pending {
  uint32_t actor = 0;
  int type = 0;  // the scoring message to drop: kMsgScore subtype 1
  int subtype = 0;
  Clock::time_point until{};
};
std::mutex g_mutex;  // guards g_pending and g_spin (game threads)
Pending g_pending[8];

void Expect(uint32_t actor, int subtype, Clock::time_point now) {
  if (!actor) return;
  Pending* slot = &g_pending[0];
  for (Pending& p : g_pending) {
    if (p.until < slot->until) slot = &p;  // the oldest (or an empty one)
  }
  *slot = {actor, kMsgScore, subtype, now + std::chrono::milliseconds(250)};
}

// Drops one expected message: true = drop it.
bool TakeExpected(uint32_t actor, int type, int subtype, Clock::time_point now) {
  for (Pending& p : g_pending) {
    if (p.actor != actor || now > p.until) continue;
    // "You were hit" may come as 31 / 1 or 12 / 1 (both break the combo).
    const bool match = (type == kMsgScore && subtype == p.subtype) ||
                       (type == kMsgDamage && subtype == kDamageBreakCombo && p.subtype == 1);
    if (match) {
      p = Pending{};
      return true;
    }
  }
  return false;
}

// --- Simple spinning --------------------------------------------------------------
// Per player (0-3).
struct Spin {
  Clock::time_point started{};     // the last spin that began (SpinStarted)
  bool feeding = false;            // Light attack presses being made
  Clock::time_point feed_from{};   // the first press's time; then every kPressEvery
  // The left stick's turning (controllers): its angle at the last poll and
  // a smoothed turning speed in turns per second (signed).
  bool have_angle = false;
  float angle = 0.0f;
  float turns_per_second = 0.0f;
  Clock::time_point last_poll{};
  Clock::time_point last_turning{};  // the last poll that saw a fast enough turn
  // Simple spinning's button (LB): held at the last poll, a circle waiting
  // for the fight tree's question (and when the next one comes while held).
  bool button_held = false;
  Clock::time_point button_circle{};
  Clock::time_point next_button_circle{};
};
Spin g_spin[4];

// A spin that began this recently can be taken over (the player must keep
// spinning right after it starts: a held Spin key or a stick still circling).
constexpr auto kTakeOverWithin = std::chrono::milliseconds(500);
// Light attack presses: first one this long after the spin starts, then one
// every kPressEvery, each held kPressHeld (the game reads a press as a change
// from one frame to the next: 60 ms covers a frame even at 30 fps).
constexpr auto kFirstPress = std::chrono::milliseconds(150);
constexpr auto kPressEvery = std::chrono::milliseconds(150);
constexpr auto kPressHeld = std::chrono::milliseconds(60);
// A controller "keeps spinning" while its stick is pushed at least this far
// and turns at least this fast (one circle a second; a spin needs ~1.5).
constexpr float kStickPushed = 0.6f;
constexpr float kStickTurning = 1.0f;
constexpr auto kStillCircling = std::chrono::milliseconds(300);

constexpr uint16_t kButtonX = 0x4000;   // X_INPUT_GAMEPAD_X (Light attack)
constexpr uint16_t kButtonLB = 0x0100;  // X_INPUT_GAMEPAD_LEFT_SHOULDER (simple spinning's Spin)
// The spin button, like the keyboard's Spin key: a press = one circle, kept
// for kCircleLife (a stale one would start a spin long after the press, e.g.
// on landing); held = one more every kCircleEvery (3 a second, the keyboard's
// default) while not feeding.
constexpr auto kCircleLife = std::chrono::milliseconds(400);
constexpr auto kCircleEvery = std::chrono::milliseconds(333);

int16_t ReadS16(const uint8_t* p) { return int16_t(uint16_t(p[0]) << 8 | p[1]); }

// Follows the left stick's angle (controllers: rotating it = the spin). A
// thumb's circle isn't perfect (it can cut the corner through the middle, or
// pause): the speed only fades while the stick rests, and "still circling"
// lasts kStillCircling after the last fast turn.
void TrackStick(Spin& s, const uint8_t* gamepad, Clock::time_point now) {
  const float x = ReadS16(gamepad + 4) / 32767.0f, y = ReadS16(gamepad + 6) / 32767.0f;
  const float dt = std::chrono::duration<float>(now - s.last_poll).count();
  s.last_poll = now;
  if (dt <= 0.0f || dt > 0.2f) {  // the first poll, or a long gap: start over
    s.have_angle = false;
    s.turns_per_second = 0.0f;
  }
  if (std::sqrt(x * x + y * y) < kStickPushed) {
    s.have_angle = false;
    s.turns_per_second *= std::max(0.0f, 1.0f - dt / 0.3f);
  } else {
    const float angle = std::atan2(y, x);
    if (s.have_angle) {
      float d = angle - s.angle;
      const float kPi = 3.14159265f;
      while (d > kPi) d -= 2 * kPi;
      while (d < -kPi) d += 2 * kPi;
      const float rate = d / (2 * kPi) / dt;
      // Smoothed over ~0.15 s: a stick polled at 30-180 Hz.
      const float k = std::min(1.0f, dt / 0.15f);
      s.turns_per_second += (rate - s.turns_per_second) * k;
    }
    s.angle = angle;
    s.have_angle = true;
  }
  if (std::fabs(s.turns_per_second) >= kStickTurning) s.last_turning = now;
}

}  // namespace

void SpinStarted(int p) {
  if (p < 0 || p >= 4) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_spin[p].started = Clock::now();
}

bool TakeButtonSpin(int p) {
  if (p < 0 || p >= 4) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  Spin& s = g_spin[p];
  if (s.button_circle == Clock::time_point{} || Clock::now() - s.button_circle > kCircleLife) return false;
  s.button_circle = {};
  return true;
}

bool Feeding(int p) {
  if (p < 0 || p >= 4) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_spin[p].feeding;
}

void FilterPad(uint32_t user, uint8_t* gamepad, bool connected) {
  if (user >= 4 || !gamepad) return;
  const int p = int(user);
  const auto now = Clock::now();
  // Outside the lock: the keyboard driver and the menu check take their own.
  kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get();
  kbm::PlayerAssignment* assignment = kbm::PlayerAssignment::Get();
  const bool key_held = driver && assignment && assignment->KeyboardAnswersFor(user) && driver->SpinHeld();
  // In a level, no menu: LB is Spin (on foot or riding a titan: titans have
  // spin moves too, e.g. the Spin Dismount). Feeding Light attacks: on foot only.
  const bool playing = more_players::InPlay() && !more_players::Loading() && !menu_input::MenuOnScreen() &&
                       (more_players::CharacterOfPlayer(p) != 0 || more_players::TitanOfPlayer(p) != 0);
  const bool on_foot = playing && more_players::TitanOfPlayer(p) == 0;

  std::lock_guard<std::mutex> lock(g_mutex);
  Spin& s = g_spin[p];
  if (connected) TrackStick(s, gamepad, now);
  const bool simple = REXCVAR_GET(simple_spinning);
  const uint16_t buttons = uint16_t(gamepad[0] << 8 | gamepad[1]);
  // SIMPLE SPINNING: LB = Spin in play. The game hears no LB there (its LB is
  // a debug button in play: nothing to lose); menus keep theirs (Options tabs).
  const bool button = simple && playing && (buttons & kButtonLB);
  if (simple && playing) {
    const uint16_t without = buttons & ~kButtonLB;
    gamepad[0] = uint8_t(without >> 8);
    gamepad[1] = uint8_t(without);
  }
  if (button && !s.button_held) {
    s.button_circle = now;
    s.next_button_circle = now + kCircleEvery;
  } else if (button && now >= s.next_button_circle && !s.feeding) {
    s.button_circle = now;
    s.next_button_circle = now + kCircleEvery;
  }
  s.button_held = button;
  const bool was_feeding = s.feeding;
  // KEEP SPINNING needs simple spinning (the Options row is greyed without it).
  if (!simple || !REXCVAR_GET(keep_spinning) || !on_foot) {
    s.feeding = false;
  } else {
    const bool keeps_spinning = key_held || button || now - s.last_turning <= kStillCircling;
    if (!keeps_spinning) {
      s.feeding = false;
    } else if (!s.feeding && s.started != Clock::time_point{} && now - s.started <= kTakeOverWithin) {
      s.feeding = true;
      s.feed_from = s.started + kFirstPress;
    }
  }
  if (s.feeding != was_feeding && REXCVAR_GET(debug_coop_trace)) {
    REXLOG_INFO("Gameplay: keep spinning {} for player {}", s.feeding ? "keeps the spin going" : "stopped",
                p + 1);
  }
  if (!s.feeding || now < s.feed_from) return;
  const auto phase = (now - s.feed_from) % kPressEvery;
  if (phase < kPressHeld) {
    // OR into the buttons (big-endian u16 at +0): the player's own presses stay.
    const uint16_t buttons = uint16_t(gamepad[0] << 8 | gamepad[1]) | kButtonX;
    gamepad[0] = uint8_t(buttons >> 8);
    gamepad[1] = uint8_t(buttons);
  } else {
    // Between presses X reads released, so the next one is a new press even
    // if the player holds Light attack themselves.
    const uint16_t buttons = uint16_t(gamepad[0] << 8 | gamepad[1]) & ~kButtonX;
    gamepad[0] = uint8_t(buttons >> 8);
    gamepad[1] = uint8_t(buttons);
  }
}

}  // namespace game_mods

// -----------------------------------------------------------------------------
// A character's damageable gets a message (findings/40 s.1). A partner's hit:
// dropped (friendly fire off), or let through with its combo messages marked
// to be dropped (combos kept). Everything else: the original, untouched.
// -----------------------------------------------------------------------------
extern "C" REX_FUNC(sub_82132FF8) {
  using namespace game_mods;
  const bool friendly_fire = REXCVAR_GET(friendly_fire);
  const bool keep_combo = !REXCVAR_GET(friendly_hits_break_combo);
  const uint32_t victim = ctx.r4.u32, message = ctx.r5.u32;
  if ((friendly_fire && !keep_combo) || !message) {
    __imp__sub_82132FF8(ctx, base);
    return;
  }
  const int type = int32_t(Read32(base, message + kMsgType));
  const int subtype = int32_t(Read32(base, message + kMsgSubtype));
  if (type != kMsgDamage || (subtype != kDamageHit && subtype != kDamageChange)) {
    __imp__sub_82132FF8(ctx, base);
    return;
  }
  // The original reads this record for every damage message (0x82133104).
  const uint32_t hit = Read32(base, Read32(base, message + kMsgData) + 4);
  const uint32_t from = Read32(base, hit + kHitFrom), scorer = Read32(base, hit + kHitScorer);
  const int hurt = PlayerOf(ctx, base, victim);
  int hitter = hurt >= 0 ? PlayerOf(ctx, base, from) : -1;
  if (hurt >= 0 && hitter < 0) hitter = PlayerOf(ctx, base, scorer);  // a projectile: its owner
  if (hurt < 0 || hitter < 0 || hitter == hurt) {
    __imp__sub_82132FF8(ctx, base);
    return;
  }
  if (!friendly_fire) {
    // One line a second per pair: a punch arrives as several hit messages.
    static Clock::time_point last_logged[4][4];
    const auto now = Clock::now();
    if (REXCVAR_GET(debug_coop_trace) && now - last_logged[hitter][hurt] > std::chrono::seconds(1)) {
      last_logged[hitter][hurt] = now;
      REXLOG_INFO("Gameplay: player {} hit player {}: no friendly fire, the hit is dropped", hitter + 1, hurt + 1);
    }
    return;
  }
  // Friendly fire on, combos kept: the hit lands; its combo messages don't.
  if (subtype == kDamageHit) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto now = Clock::now();
    // Only the hit one's "you were hit": the game gives the hitter no combo
    // point for a partner (measured: their meter stays at 0), so nothing to
    // drop there, and a pending entry could eat a real point.
    Expect(victim, 1, now);
  }
  if (REXCVAR_GET(debug_coop_trace)) {
    REXLOG_INFO("Gameplay: player {} hit player {}: combos kept", hitter + 1, hurt + 1);
  }
  __imp__sub_82132FF8(ctx, base);
}

// A player's scoring behaviour gets a message (r4 = its actor): the combo
// messages of a partner's hit (marked above) are dropped.
extern "C" REX_FUNC(sub_82166198) {
  using namespace game_mods;
  const uint32_t actor = ctx.r4.u32, message = ctx.r5.u32;
  if (message && !REXCVAR_GET(friendly_hits_break_combo)) {
    const int type = int32_t(Read32(base, message + kMsgType));
    const int subtype = int32_t(Read32(base, message + kMsgSubtype));
    if (type == kMsgScore || type == kMsgDamage) {
      bool drop;
      {
        std::lock_guard<std::mutex> lock(g_mutex);
        drop = TakeExpected(actor, type, subtype, Clock::now());
      }
      if (drop) {
        if (REXCVAR_GET(debug_coop_trace)) {
          REXLOG_INFO("Gameplay: combo message {}/{} for {:08X} dropped (a partner's hit)", type, subtype, actor);
        }
        return;
      }
    }
  }
  __imp__sub_82166198(ctx, base);
}


