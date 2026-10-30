// =============================================================================
// players/pause_drop_out.cpp -- see pause_drop_out.h
// =============================================================================
#include "pause_drop_out.h"

#include <array>
#include <chrono>
#include <string>
#include <string_view>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../guest_memory.h"
#include "../input_record.h"
#include "../ui/scrooby.h"
#include "lost_controller.h"

extern "C" REX_FUNC(sub_82261800);         // front end: play a sound (the port's wrapper, saves/save_library.cpp)
extern "C" REX_FUNC(__imp__sub_820D9E48);  // menu screen action: the "open" step (A / B / START)

namespace pause_drop_out {
namespace {

constexpr int kPlayers = 4;
constexpr uint16_t kButtonBack = 0x0020;       // XINPUT_GAMEPAD_BACK
constexpr uint32_t kGameGlobal = 0x8259B190;   // the game global (front end at +52)
constexpr uint32_t kFrontEnd = 52;
constexpr uint32_t kOwner = 8536;              // front end: the menu's owner (player)
constexpr uint32_t kTextString = 152;          // Scrooby Text: its UTF-16 string
constexpr uint32_t kInputFlags = 8592;         // front end: bit 0x40 = the menus take input
constexpr uint32_t kResultFlags = 8593;        // front end: bit 0x08 = an item was picked (after the closing)
constexpr uint32_t kScreenState = 0x30;        // menu screen action: 3 open, 4 cancelled, 5 picked
constexpr uint32_t kSoundBank = 0x820222E4;    // "FE_SFX"
constexpr uint32_t kSoundSelect = 0x82022450, kSoundBack = 0x820222DC, kSoundWipe = 0x820222EC;
// The prompt: FE_Buttons' upper-left corner (free on the pause page; the
// save list uses it for "Delete Y" on its own screens). µ = the font's Back
// picture; prompt_icons turns it into the owner's look (Tab on a keyboard).
constexpr std::string_view kButtonsPage = "FE_Buttons";
constexpr std::string_view kPromptText = "UpperLeftText", kPromptButton = "UpperLeftButton";
constexpr std::u16string_view kWords = u"Drop Out", kGlyph = u"µ";
// A pause decision that hasn't run for this long = the pause menu was left
// (or not up): presses are counted afresh (a Back held while the menu opens
// isn't a press).
constexpr auto kGap = std::chrono::milliseconds(250);

std::array<uint16_t, kPlayers> g_held{};
std::chrono::steady_clock::time_point g_last_call{};
bool g_prompt_shown = false;
// The menu owner's Back, step by step like the old item's A:
//   g_owner_back: pressed; the pause screen's next "open" step picks for it
//     (PickForOwner, from the sub_820D9E48 wrapper below);
//   g_drop_picked: picked; the screen plays its closing, then sets the
//     front end's "picked" flag (+8593 bit 3) and the decision answers Drop Out.
bool g_owner_back = false;
bool g_drop_picked = false;

uint32_t Read32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = GuestPtr(base, a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// Whether a Text object already says `words` (its string is UTF-16 BE).
bool Says(const uint8_t* base, uint32_t text, std::u16string_view words) {
  const uint32_t s = Read32(base, text + kTextString);
  if (!s) return words.empty();
  for (size_t i = 0; i <= words.size(); ++i) {
    const uint8_t* p = GuestPtr(base, s + 2 * uint32_t(i));
    if (uint16_t(p[0] << 8 | p[1]) != (i < words.size() ? uint16_t(words[i]) : 0)) return false;
  }
  return true;
}

// The button text's characters (UTF-16 BE) as a string.
std::u16string TextOf(const uint8_t* base, uint32_t text) {
  std::u16string out;
  const uint32_t s = Read32(base, text + kTextString);
  for (uint32_t i = 0; s && i < 16; ++i) {
    const uint8_t* p = GuestPtr(base, s + 2 * i);
    const char16_t c = char16_t(p[0] << 8 | p[1]);
    if (!c) break;
    out.push_back(c);
  }
  return out;
}

// Shows / takes away the prompt. The LABEL ("Drop Out") says whether the
// corner is ours: the button text is only put in when the label goes in (or
// the button was emptied), because prompt_icons rewrites it every frame into
// the menu owner's own picture (a player slot, the keyboard's key). Putting
// the plain Back character back every frame fought that rewrite: the picture
// was lent and taken back over and over and showed BLANK at times (first
// playtest, 2026-10-30). Setting a text allocates a new string in the menus'
// heap: only when it differs.
void ShowPrompt(PPCContext& ctx, uint8_t* base, bool show) {
  const uint32_t page = scrooby::FindPage(ctx, base, kButtonsPage);
  if (!page) return;
  const uint32_t text = scrooby::FindText(ctx, base, page, kPromptText);
  const uint32_t button = scrooby::FindText(ctx, base, page, kPromptButton);
  if (!text || !button) return;
  const bool ours = Says(base, text, kWords);
  if (show) {
    if (!ours) scrooby::SetText(ctx, base, text, kWords);
    if (!ours || TextOf(base, button).empty()) scrooby::SetText(ctx, base, button, kGlyph);
  } else if (ours) {  // another screen's prompt there now: left alone
    scrooby::SetText(ctx, base, text, u"");
    scrooby::SetText(ctx, base, button, u"");
  }
  g_prompt_shown = show;
}

}  // namespace

int32_t AfterPauseDecision(PPCContext& ctx, uint8_t* base, int32_t exit, int32_t drop_out_exit) {
  const auto now = std::chrono::steady_clock::now();
  const bool fresh = now - g_last_call > kGap;
  g_last_call = now;
  if (fresh) g_owner_back = g_drop_picked = false;  // a new visit of the pause menu

  // Every player's Back, as the game last read it (keyboard merged in).
  uint16_t pressed[kPlayers] = {};
  for (int p = 0; p < kPlayers; ++p) {
    uint8_t pad[12] = {};
    const uint16_t buttons = input_record::LastRead(uint32_t(p), pad) ? uint16_t(pad[0] << 8 | pad[1]) : 0;
    pressed[p] = fresh ? 0 : uint16_t(buttons & ~g_held[p]);
    g_held[p] = buttons;
  }

  const uint32_t game = Read32(base, kGameGlobal);
  const uint32_t fe = game ? Read32(base, game + kFrontEnd) : 0;
  const int owner = fe ? int(int32_t(Read32(base, fe + kOwner))) : -1;

  // The owner's Back picked "Drop Out" and the screen has closed: the game's
  // own exit for it (whatever item the cursor was on).
  if (g_drop_picked) {
    if (!fe || !(*GuestPtr(base, fe + kResultFlags) & 0x08)) return exit;  // still closing
    g_drop_picked = false;
    REXLOG_INFO("Pause menu: the menu closed: Drop Out for player {} (exit {})", owner + 1, drop_out_exit);
    return drop_out_exit;
  }

  bool anyone = false;  // someone may drop out: the prompt shows
  for (int p = 0; p < kPlayers; ++p) anyone |= lost_controller::CanDropOut(p);

  // The menu is being left this frame (the game decided): prompt away.
  if (exit >= 0) {
    if (g_prompt_shown) ShowPrompt(ctx, base, false);
    return exit;
  }

  for (int p = 0; p < kPlayers && !g_owner_back; ++p) {
    if (!(pressed[p] & kButtonBack) || !lost_controller::CanDropOut(p)) continue;
    if (p == owner) {
      // Picked like an item (the screen's next step: PickForOwner).
      REXLOG_INFO("Pause menu: player {} (the menu's owner) pressed Back: Drop Out", p + 1);
      g_owner_back = true;
      continue;
    }
    if (lost_controller::DropOut(ctx, base, p, "pressed Back in the pause menu")) {
      scrooby::PlaySound(ctx, base, scrooby::Sound::kChange);
      anyone = false;  // (the one left may be the last now)
      for (int q = 0; q < kPlayers; ++q) anyone |= lost_controller::CanDropOut(q);
    }
  }
  // Set every frame while up (the prompts' own action may wipe the corner).
  if (anyone || g_prompt_shown) ShowPrompt(ctx, base, anyone);
  return exit;
}

bool OwnerPickWaiting() {
  return g_owner_back && std::chrono::steady_clock::now() - g_last_call <= kGap;
}

void PickForOwner(PPCContext& ctx, uint8_t* base, uint32_t action) {
  g_owner_back = false;
  const uint32_t game = Read32(base, kGameGlobal);
  const uint32_t fe = game ? Read32(base, game + kFrontEnd) : 0;
  if (!fe || !action) return;
  // What the screen's own A does (sub_820D9E48): state 5 = picked, its three
  // sounds, the menus' input off; it then closes as usual (sub_820DA010)
  // and sets the "picked" flag the decision waits for.
  uint8_t* state = GuestPtr(base, action + kScreenState);
  state[0] = state[1] = state[2] = 0;
  state[3] = 5;
  for (uint32_t sound : {kSoundSelect, kSoundBack, kSoundWipe}) {
    const PPCContext saved = ctx;
    ctx.r3.u64 = fe;
    ctx.r4.u64 = kSoundBank;
    ctx.r5.u64 = sound;
    ctx.f1.f64 = 1.0;
    ctx.f2.f64 = 0.0;
    ctx.f3.f64 = 1.0;
    sub_82261800(ctx, base);  // (the port's wrapper: muting rules apply as for the game's own call)
    ctx = saved;
  }
  *GuestPtr(base, fe + kInputFlags) &= uint8_t(~0x40);
  ShowPrompt(ctx, base, false);
  g_drop_picked = true;
}

}  // namespace pause_drop_out

// The menu screen action's "open" step (state 3; Update sub_820D9968 runs it
// every frame): reads the owner's A (pick), B / START (cancel). While the
// pause menu's owner has pressed Back, this step picks for them instead
// (pause_drop_out.h); otherwise the game's own.
extern "C" REX_FUNC(sub_820D9E48) {
  if (pause_drop_out::OwnerPickWaiting()) {
    pause_drop_out::PickForOwner(ctx, base, ctx.r3.u32);
    return;
  }
  __imp__sub_820D9E48(ctx, base);
}
