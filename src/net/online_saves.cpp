// =============================================================================
// net/online_saves.cpp -- only the host saves online (net/online_saves.h)
// =============================================================================

#include "online_saves.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../achievements/achievements.h"
#include "../language/texts.h"
#include "../players/more_players.h"
#include "net_session.h"

// SavePrompt's decision (front end node 833: the totem's "Do you want to
// save?"): 837 ExitChooseSaveSlot after Yes, 838 ExitSavePromptDone after No.
// The original never returns 836 ExitAutoSaveScreen (no autosave on the 360).
extern "C" REX_FUNC(__imp__sub_82121728);

namespace online_saves {
namespace {

// Front end exits (notes/scratch-tools/fig_tree.py numbering).
constexpr int32_t kTotemAutoSave = 836;     // SavePrompt: ExitAutoSaveScreen -> AutoSaveGameScreen 326
constexpr int32_t kTotemChooseSlot = 837;   // SavePrompt: ExitChooseSaveSlot -> AccessMemoryCard 359
constexpr int32_t kTotemDone = 838;         // SavePrompt: ExitSavePromptDone -> GameRunning 489
// The pause menu (node 868), two-player branch (878) and one-player (890).
constexpr int32_t kPause2Resume = 879, kPause2AutoSave = 885, kPause2ChooseSlot = 886;
constexpr int32_t kPause1Resume = 891, kPause1AutoSave = 896, kPause1ChooseSlot = 897;

constexpr int kHostPlayer = 0;  // the host plays player 1 (net_session.h)

// May the menu's owner save the session? Owner unknown (-1) = yes: the save
// still lands in the host's file on the host and in the copy on a guest.
bool OwnerMaySave(PPCContext& ctx, uint8_t* base, const char* where) {
  const int owner = menu_input::CurrentMenuOwner(ctx, base);
  if (owner < 0 || owner == kHostPlayer) return true;
  REXLOG_INFO("Online: player {} asked to save ({}): only the host saves", owner + 1, where);
  // On every PC (the same decision runs on the same frame everywhere): the
  // message bar card. It joins the game's bar queue, game memory, so it must
  // happen alike on both PCs or the worlds part.
  // TR: Online co-op: a guest tried to save; card heading
  achievements::ShowNotice(tr(u"Saving"),
                           // TR: Online co-op: a guest tried to save; card line
                           tr(u"Only the host can save online"));
  return false;
}

}  // namespace

int32_t AfterPauseDecision(PPCContext& ctx, uint8_t* base, int32_t exit) {
  if (!net::InSession()) return exit;
  const bool two = exit == kPause2AutoSave || exit == kPause2ChooseSlot;
  const bool one = exit == kPause1AutoSave || exit == kPause1ChooseSlot;
  if (!two && !one) return exit;
  if (!OwnerMaySave(ctx, base, "pause menu")) {
    return two ? kPause2Resume : kPause1Resume;  // the menu closes; the card says why
  }
  return exit;  // the storage steps, then straight into the save (save_library.cpp)
}

}  // namespace online_saves

// The totem's question: a guest's Yes is a No (online_saves.h). The host's
// Yes goes the usual way into the storage steps; the save library then saves
// without showing the list.
extern "C" REX_FUNC(sub_82121728) {
  using namespace online_saves;
  __imp__sub_82121728(ctx, base);
  const int32_t exit = int32_t(ctx.r3.u32);
  if (!net::InSession() || (exit != kTotemChooseSlot && exit != kTotemAutoSave)) return;
  if (!OwnerMaySave(ctx, base, "totem")) ctx.r3.u64 = uint32_t(kTotemDone);
}
