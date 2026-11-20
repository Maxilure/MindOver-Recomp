// =============================================================================
// players/skip_vote.cpp -- a cutscene is skipped once every playing player has
// pressed to skip it (see skip_vote.h for the game's skip code and the rules)
// =============================================================================
#include "skip_vote.h"

#include <cstring>
#include <string>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "guest_memory.h"
#include "more_players.h"
#include "../ui/scrooby.h"
#include "../input/players.h"
#include "../net/net_session.h"

extern "C" REX_FUNC(sub_823731D0);  // point a text at a borrowed string (0 = the bank's own)

REXCVAR_DEFINE_BOOL(coop_skip_vote, true, "CrashMoM",
                    "Co-op: a cutscene or movie is skipped only when every playing player has pressed A on its "
                    "Skip prompt (the prompt counts the votes); off = anyone's A skips, as in the original "
                    "(players/skip_vote.h)");

namespace skip_vote {
namespace {

constexpr uint32_t kGameGlobal = 0x8259B190;
constexpr uint32_t kFrontEnd = 52;              // game global: the front end
constexpr uint32_t kButtonA = 1;                // IsButtonPressed: A
// The questions the vote answers (return addresses, skip_vote.h).
constexpr uint32_t kSceneSkipAsk = 0x82242E1C;  // scene step: "A from anyone?"
constexpr uint32_t kMovieStartAsk = 0x82188028; // movie in play: "START from any controller?"
constexpr uint32_t kMovieSkipAsk = 0x82188040;  // movie in play: "A from any controller?"
// Prompt timers kept up while a vote is open (seconds; the game sets 2.0).
constexpr uint32_t kSceneTimer = 420;           // CMasterCinematicAction: prompt time left
constexpr uint32_t kMovieTimer = 52;            // CMoviePlayerBehaviour: prompt time left
constexpr float kKeepUp = 1.0f;
// The movie's prompt: behaviour +36 = its CMoviePlayer, +296 bit 0x20 = shown
// (input/movie_skip.cpp).
constexpr uint32_t kMoviePlayer = 36, kMoviePlayerFlags = 296;
constexpr uint8_t kMovieSkipShown = 0x20;

uint32_t Load32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, GuestPtr(base, a), 4);
  return __builtin_bswap32(v);
}
float LoadFloat(uint8_t* base, uint32_t a) {
  const uint32_t bits = Load32(base, a);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
void StoreFloat(uint8_t* base, uint32_t a, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  bits = __builtin_bswap32(bits);
  std::memcpy(GuestPtr(base, a), &bits, 4);
}

// One vote at a time (one cutscene on screen): keyed by the asking object
// (the scene step or the movie behaviour); a new key starts a new vote.
struct Vote {
  uint32_t key = 0;
  bool voted[4] = {};
  int count = 0;
  uint32_t text = 0;           // the prompt's "Skip" text, once found
  std::u16string original;     // its string before we added the count
  int shown_count = -1;        // the count currently in the text
  int shown_needed = -1;       // and the "of how many" (a player leaving changes it)
} g_vote;

// Players who have a say: everyone playing right now who CAN press: not a
// player whose PC is gone online (their character stays a while after a
// disconnect) and not one whose controller is unplugged (players/
// lost_controller.h); player 1 always has the keyboard. Else a vote could
// never finish (2026-11-20: tested by killing a copy and unplugging a pad
// mid-vote).
bool HasSay(int p) {
  if (!menu_input::PlayerPlays(p) || net::PlayerAway(p)) return false;
  // Online the other PC's player has no device HERE (their inputs come over
  // the network): only the "PC gone" rule, the same on both PCs.
  if (net::Configured()) return true;
  const kbm::PlayerAssignment* assignment = kbm::PlayerAssignment::Get();
  return p == 0 || !assignment || assignment->HasDeviceFor(p);
}
int Needed() {
  int n = 0;
  for (int p = 0; p < more_players::LocalPlayerCount() && p < 4; ++p) n += HasSay(p) ? 1 : 0;
  return n;
}

// The text's drawn string (+156: UTF-16, big-endian, 0-ended).
std::u16string DrawnString(uint8_t* base, uint32_t text) {
  std::u16string s;
  const uint32_t at = Load32(base, text + 156);
  if (!at || !guest_memory::Readable(at, 2)) return s;
  for (uint32_t i = 0; i < 64 && guest_memory::Readable(at + 2 * i, 2); ++i) {
    uint16_t c;
    std::memcpy(&c, GuestPtr(base, at + 2 * i), 2);
    c = __builtin_bswap16(c);
    if (!c) break;
    s.push_back(char16_t(c));
  }
  return s;
}

// The prompt's text shows the count ("Skip (A) 1/2"), or its own string again.
void ShowCount(PPCContext& ctx, uint8_t* base, int needed) {
  if (!g_vote.text) {
    const uint32_t page = scrooby::FindPage(ctx, base, "InGame_MovieSkip");
    g_vote.text = page ? scrooby::FindTextElement(ctx, base, page, "Skip") : 0;
    if (!g_vote.text) return;
    g_vote.original = DrawnString(base, g_vote.text);
  }
  if (g_vote.shown_count == g_vote.count && g_vote.shown_needed == needed) return;
  g_vote.shown_count = g_vote.count;
  g_vote.shown_needed = needed;
  scrooby::SetText(ctx, base, g_vote.text,
                   g_vote.original + u" " + std::u16string(1, char16_t(u'0' + g_vote.count)) + u"/" +
                       std::u16string(1, char16_t(u'0' + needed)));
}
void PutTextBack(PPCContext& ctx, uint8_t* base) {
  if (g_vote.text && g_vote.shown_count >= 0) {
    const PPCContext saved = ctx;
    ctx.r3.u64 = g_vote.text;
    ctx.r4.u64 = 0;  // back to the text bank's own string
    sub_823731D0(ctx, base);
    ctx = saved;
  }
  g_vote.text = 0;
  g_vote.original.clear();
  g_vote.shown_count = -1;
  g_vote.shown_needed = -1;
}

// Polls every playing player's A for the vote keyed `key`; true = everyone
// has voted (the cutscene may be skipped). `needed` >= 2.
bool Poll(PPCContext& ctx, uint8_t* base, uint32_t key, int needed) {
  if (g_vote.key != key) {
    PutTextBack(ctx, base);
    g_vote = Vote{};
    g_vote.key = key;
  }
  const uint32_t game = Load32(base, kGameGlobal);
  const uint32_t fe = game ? Load32(base, game + kFrontEnd) : 0;
  if (!fe) return false;
  for (int p = 0; p < more_players::LocalPlayerCount() && p < 4; ++p) {
    if (g_vote.voted[p] || !HasSay(p)) continue;
    if (!menu_input::PressedBy(ctx, base, fe, kButtonA, p)) continue;
    g_vote.voted[p] = true;
    ++g_vote.count;
    REXLOG_INFO("Skip vote: player {} wants to skip ({}/{})", p + 1, g_vote.count, needed);
  }
  // A voter who left the game (or lost their controller / PC) no longer counts.
  int count = 0;
  for (int p = 0; p < 4; ++p) count += g_vote.voted[p] && HasSay(p) ? 1 : 0;
  g_vote.count = count;
  if (count >= needed) {
    REXLOG_INFO("Skip vote: everyone agreed: skipped");
    PutTextBack(ctx, base);
    g_vote = Vote{};
    return true;
  }
  if (count > 0) ShowCount(ctx, base, needed);
  return false;
}

// The vote answers while at least two players have a say, and also while a
// vote is already open when that drops to one (a voter's partner left
// mid-vote: their earlier vote then finishes it, instead of being forgotten
// by the original "anyone" rule; tested by killing a copy mid-vote).
bool Active(int& needed) {
  needed = Needed();
  return REXCVAR_GET(coop_skip_vote) && (needed >= 2 || (needed >= 1 && g_vote.count > 0));
}

}  // namespace

bool AnswerSceneSkip(PPCContext& ctx, uint8_t* base) {
  int needed = 0;
  if (uint32_t(ctx.lr) != kSceneSkipAsk || !Active(needed)) return false;
  const uint32_t step = ctx.r31.u32;  // sub_82242D50's r31 = the step (its `this`)
  const PPCContext saved = ctx;
  const bool skip = Poll(ctx, base, step, needed);
  const int count = g_vote.count;
  ctx = saved;
  if (!skip && count > 0 && LoadFloat(base, step + kSceneTimer) < kKeepUp) {
    StoreFloat(base, step + kSceneTimer, kKeepUp);  // the prompt stays up while the vote is open
  }
  ctx.r3.u64 = skip ? 1 : 0;
  return true;
}

}  // namespace skip_vote

// The movie's "did any controller press ..." (sub_82274BE8): START and A in
// play answered by the vote (skip_vote.h); every other caller untouched.
extern "C" REX_FUNC(__imp__sub_82274BE8);
extern "C" REX_FUNC(sub_82274BE8) {
  using namespace skip_vote;
  const uint32_t lr = uint32_t(ctx.lr);
  int needed = 0;
  if ((lr != kMovieSkipAsk && lr != kMovieStartAsk) || !Active(needed)) {
    __imp__sub_82274BE8(ctx, base);
    return;
  }
  const uint32_t behaviour = ctx.r29.u32;  // sub_82187EF0's r29 = the behaviour
  if (lr == kMovieStartAsk) {
    // START while a vote is open would hide the prompt: not then.
    if (g_vote.key == behaviour && g_vote.count > 0) {
      ctx.r3.u64 = 0;
      return;
    }
    __imp__sub_82274BE8(ctx, base);
    return;
  }
  // A counts only while the prompt is up (the movie asks every frame: a
  // stray A with the prompt hidden must not vote, as the original ignores it).
  const uint32_t player = Load32(base, behaviour + kMoviePlayer);
  if (!player || !(*GuestPtr(base, player + kMoviePlayerFlags) & kMovieSkipShown)) {
    ctx.r3.u64 = 0;
    return;
  }
  const PPCContext saved = ctx;
  const bool skip = Poll(ctx, base, behaviour, needed);
  const int count = g_vote.count;
  ctx = saved;
  if (!skip && count > 0 && LoadFloat(base, behaviour + kMovieTimer) < kKeepUp) {
    StoreFloat(base, behaviour + kMovieTimer, kKeepUp);
  }
  ctx.r3.u64 = skip ? 1 : 0;
}
