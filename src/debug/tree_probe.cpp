// =============================================================================
// FIGHT TREE PROBE (research, 2026-11-27): can we PUT a character into a state?
//
// Why: online co-op sends the other player's BUTTONS and lets this PC's copy of
// their Crash decide what to do with them. The copy then decides differently
// (attacks another enemy, keeps jumping after the owner died). The better way is
// to send ACTIONS ("heavy attack now", "died") and make the copy obey. This
// probe tests the game's own way of jumping a character's fight tree to a node.
//
// What the game has (decompiled 2026-11-27, Ghidra):
//   * a fight tree INSTANCE (one per character; r3 of the functions below):
//       +0x10 actor, +0x1C -> pointer to the node array (node N = *(**(tree+0x1C) + 4N): sub_820C0738),
//       +0x48 current STATE node, +0x4C its first exit, +0x90 the exit the
//       decision picked this frame, +0x7C state time, +0x28 the compiled decision
//   * a node: +0x1C u16 type (3/4/5 = exits: their target via sub_820C0D98,
//       6 = a sub-tree), +0x1E u16 node number (Crash.bfig file order + 1),
//       +0x28 an exit's target
//   * sub_820C1170(f1 dt, r3 tree) = the tree's per-frame update (decision ->
//       +0x90; called from CFightTreeBehaviour slot 18 sub_8213E6E0)
//   * sub_820C1578(r3 tree, r4 node, r5 exit) = CHANGE STATE: previous state
//       kept (+0x58/+0x5C/+0x60), old state's exit actions, new state's enter
//       actions (sub_820C1A38), animation (sub_820C17F0)
//   * sub_820C0DF8(r3 tree, r4 node) = "GO TO NODE": an exit node -> its
//       target state; a state node -> that state. The character behaviour's
//       message handler (CCharacterFightTreeBehaviour slot 5 sub_8212EF40,
//       message type 1) uses it: sub-type 3 = by node NUMBER, 1 = by name.
//       = the scripts' own way to force a state; we call it the same way.
//
// Console (debug_console.cpp):
//   tree                 -> every player's body: current node number
//   tree <p> <node>      -> next frame, player p's tree goes to node <node>
//                           (game numbers: Crash.bfig, e.g. 110 Jump, 285 Idle,
//                           351 AttackLightA, 430 AttackHeavyAWindup, 960 Dead)
//   tree trace on|off    -> log every state change of the players' trees
// Log lines start with "Tree probe:".
// =============================================================================

#include <algorithm>
#include <array>
#include <map>
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "../guest_memory.h"
#include "../players/more_players.h"

extern "C" REX_FUNC(__imp__sub_820C0DF8);  // go to node (original)

namespace tree_probe {
namespace {

constexpr int kPlayers = 4;
constexpr uint32_t kTreeActor = 0x10, kTreeNodes = 0x1C, kTreeState = 0x48;
constexpr uint32_t kNodeType = 0x1C, kNodeNumber = 0x1E;

uint8_t* Guest(uint32_t a) { return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(a); }
uint32_t Read32(uint32_t a) {
  const uint8_t* p = Guest(a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint16_t Read16(uint32_t a) {
  const uint8_t* p = Guest(a);
  return uint16_t(p[0] << 8 | p[1]);
}

// Node number of a node pointer (-1 = none / unreadable).
int NodeNumber(uint32_t node) {
  if (!node || !guest_memory::Readable(node, 0x20)) return -1;
  return Read16(node + kNodeNumber);
}

std::mutex g_mutex;
std::array<uint32_t, kPlayers> g_tree{};       // last tree seen per player body
std::map<uint32_t, int> g_last_state;  // per TREE (a rider and their titan = two)
std::array<int, kPlayers> g_force{-1, -1, -1, -1};  // queued node per player
std::atomic<bool> g_trace{false};

int PlayerOfActor(uint32_t actor) {
  if (!actor) return -1;
  for (int p = 0; p < kPlayers; ++p) {
    if (actor == more_players::CharacterOfPlayer(p) || actor == more_players::TitanOfPlayer(p)) return p;
  }
  return -1;
}

}  // namespace

// Console entry (debug_console.cpp "tree").
std::string Command(const std::vector<std::string_view>& w) {
  std::lock_guard lock(g_mutex);
  if (w.size() >= 3 && w[1] == "trace") {
    g_trace = w[2] == "on";
    return fmt::format("tree trace {}\n", g_trace ? "on" : "off");
  }
  if (w.size() >= 3) {
    const int p = std::atoi(std::string(w[1]).c_str()) - 1;
    const int n = std::atoi(std::string(w[2]).c_str());
    if (p < 0 || p >= kPlayers || n <= 0) return "usage: tree <player 1-4> <node>\n";
    g_force[p] = n;
    return fmt::format("P{}: node {} queued\n", p + 1, n);
  }
  std::string out;
  for (int p = 0; p < kPlayers; ++p) {
    const uint32_t t = g_tree[p];
    if (!t || !guest_memory::Readable(t, 0xA0)) { out += fmt::format("P{}: no tree seen\n", p + 1); continue; }
    out += fmt::format("P{}: tree {:08X} state {}\n", p + 1, t, NodeNumber(Read32(t + kTreeState)));
  }
  return out;
}


// Called from the tree update wrapper (net/action_sync.cpp, which owns
// sub_820C1170): note players' trees, apply a queued force first (game thread,
// the tree's own moment: as if a script message arrived).
void BeforeUpdate(PPCContext& ctx, uint8_t* base, uint32_t tree) {
  const int p = guest_memory::Readable(tree + kTreeActor, 4) ? PlayerOfActor(Read32(tree + kTreeActor)) : -1;
  int force = -1;
  if (p >= 0) {
    std::lock_guard lock(g_mutex);
    g_tree[p] = tree;
    force = g_force[p];
    g_force[p] = -1;
  }
  if (force <= 0) return;
  const uint32_t node = Read32(Read32(Read32(tree + kTreeNodes)) + 4 * uint32_t(force));
  const int before = NodeNumber(Read32(tree + kTreeState));
  if (NodeNumber(node) != force) {
    REXLOG_WARN("Tree probe: P{} node {} not found (array gave {:08X} = number {})", p + 1, force, node,
                NodeNumber(node));
    return;
  }
  const PPCContext saved = ctx;
  ctx.r3.u64 = tree;
  ctx.r4.u64 = node;
  __imp__sub_820C0DF8(ctx, base);
  ctx = saved;
  REXLOG_INFO("Tree probe: P{} forced to node {} (type {}): state {} -> {}", p + 1, force,
              Read16(node + kNodeType), before, NodeNumber(Read32(tree + kTreeState)));
}

void AfterUpdate(uint32_t tree) {
  if (!g_trace) return;
  const int p = guest_memory::Readable(tree + kTreeActor, 4) ? PlayerOfActor(Read32(tree + kTreeActor)) : -1;
  if (p < 0) return;
  // state and its variant (+0x4C) together: a variant change alone shows too
  const int state = NodeNumber(Read32(tree + kTreeState));
  const int now = state * 4096 + std::max(0, NodeNumber(Read32(tree + 0x4C)));
  const bool titan = Read32(tree + kTreeActor) == more_players::TitanOfPlayer(p);
  std::lock_guard lock(g_mutex);
  const auto it = g_last_state.find(tree);
  const int was = it == g_last_state.end() ? -1 : it->second;
  if (now != was) {
    REXLOG_INFO("Tree probe: P{}{} state {} -> {} (variant {} -> {})", p + 1, titan ? " titan" : "",
                was < 0 ? -1 : was / 4096, state, was < 0 ? -1 : was % 4096, now % 4096);
    g_last_state[tree] = now;
  }
}

}  // namespace tree_probe
