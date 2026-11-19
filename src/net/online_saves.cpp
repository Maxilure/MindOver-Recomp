// =============================================================================
// net/online_saves.cpp -- every player saves on their own PC (net/online_saves.h)
// =============================================================================

#include "online_saves.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "../players/more_players.h"
#include "net_session.h"

namespace online_saves {
namespace {
constexpr int kHostPlayer = 0;  // the host plays player 1 (net_session.h)
}  // namespace

bool ThisPcWritesSave(PPCContext& ctx, uint8_t* base) {
  if (!net::InSession()) return true;
  const int local = net::LocalPlayer();
  int owner = menu_input::CurrentMenuOwner(ctx, base);
  if (owner < 0) owner = kHostPlayer;  // unknown: the host's save
  // Only the saver's own PC writes (a guest keeps a copy of the session only
  // by saving themself).
  const bool writes = owner == local;
  REXLOG_INFO("Online: player {} saves: {} on player {}'s PC", owner + 1,
              writes ? "written" : "NOT written (not their PC)", local + 1);
  return writes;
}

}  // namespace online_saves
