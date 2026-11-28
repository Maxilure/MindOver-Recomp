// =============================================================================
// net/leave_session.cpp -- an online guest goes back to the main menu
// (see leave_session.h for the route and why it is the game's own)
// =============================================================================
#include "leave_session.h"

#include <atomic>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../guest_memory.h"

#include "../players/more_players.h"
#include "state_sync.h"

namespace net::leave_session {
namespace {

// Front-end exits (Frontend.bfig node numbers, notes/scratch-tools/fig_tree.py).
constexpr int32_t kStay = -1;
constexpr int32_t kExitPause = 516;          // GameRunning 489 -> PauseScreen 868 (START's own exit)
constexpr int32_t kExitQuit = 875;           // PauseScreen 868: ExitGame / ExitXenon -> 53 (title)
constexpr int32_t kExitQuitNoSave = 907;     // NoSavePauseScreen 901: ExitSaveAndExit / ExitXenon -> 53
constexpr int32_t kExitUnpause = 899;        // PauseScreen 868: ExitOnePlayer / ExitUnpause -> 489
constexpr int32_t kExitUnpauseNoSave = 925;  // NoSavePauseScreen 901: ExitOnePlayer / ExitUnpause -> 489

// The front end's MENU OWNER (+8536, "who paused"; players/menu_input.cpp).
// The pause screen's quit runs the game's Drop Out on it (sub_82267030 ->
// sub_82266040): a pause opened by us has none (-1) and that Drop Out read a
// null player (crash at sub_820B0B70, first test 2026-11-28). So the quit
// says it's THIS PC's player who leaves.
constexpr uint32_t kGameGlobal = 0x8259B190, kFrontEnd = 52, kOwner = 8536;
void SetOwnerToUs() {
  uint8_t* base = rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(0);
  auto read32 = [&](uint32_t a) {
    const uint8_t* p = GuestPtr(base, a);
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  };
  const uint32_t game = read32(kGameGlobal);
  const uint32_t fe = game ? read32(game + kFrontEnd) : 0;
  const int slot = LocalSlot();
  if (!fe || slot < 0) return;
  uint8_t* p = GuestPtr(base, fe + kOwner);
  const uint32_t v = uint32_t(slot);
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

std::atomic<bool> g_wanted{false};
std::atomic<bool> g_saw_level_go{false};

bool HostLinkBad() {
  const HostLink link = GetHostLink();
  return link == HostLink::kSilent || link == HostLink::kGone || link == HostLink::kEnded ||
         link == HostLink::kRefused;
}

}  // namespace

void Request(const char* why) {
  if (g_wanted.exchange(true)) return;
  g_saw_level_go = false;
  REXLOG_INFO("Online: back to the main menu ({})", why);
}

bool Wanted() { return g_wanted.load(); }

int32_t GameRunningExit(int32_t game_exit) {
  if (!g_wanted.load() || game_exit != kStay) return game_exit;
  if (!more_players::InPlay() || more_players::Loading()) return game_exit;
  REXLOG_INFO("Online: leaving: pause screen first (the game's own way out)");
  return kExitPause;
}

int32_t PauseExit(int32_t game_exit, bool no_save) {
  if (g_wanted.load()) {
    REXLOG_INFO("Online: leaving: the pause screen's quit exit (as player {})", LocalSlot() + 1);
    SetOwnerToUs();
    return no_save ? kExitQuitNoSave : kExitQuit;
  }
  if (HostLinkBad() && game_exit == kStay) {
    // The host box shows only with no menu up: this PC's pause gives way.
    static bool logged = false;
    if (!logged) REXLOG_INFO("Online: the host link is bad: the pause menu closes for the host box");
    logged = true;
    return no_save ? kExitUnpauseNoSave : kExitUnpause;
  }
  return game_exit;
}

void FrontEndFrame() {
  if (!g_wanted.load()) return;
  if (more_players::InPlay()) return;  // still in the level (or its pause / quit fade)
  g_wanted = false;
  REXLOG_INFO("Online: the level is gone: closing the session");
  net::Leave();
}

}  // namespace net::leave_session
