// =============================================================================
// saves/rename_screen.cpp -- see rename_screen.h
// =============================================================================
#include "rename_screen.h"
#include "../guest_memory.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/logging.h>

#include "../data/data_patcher.h"
#include "../data/fight_tree.h"
#include "../data/pure3d.h"
#include "save_library.h"

namespace rename_screen {
namespace {

using Bytes = std::vector<uint8_t>;

// ---------------------------------------------------------------------------
// The tree (fighttrees/Frontend.bfig)
// ---------------------------------------------------------------------------

// The original nodes the patch builds on (findings/24 s.6.5), checked by
// name before anything is changed: a tree that differs (another release)
// is left alone.
constexpr int kNodeCount = 994;           // nodes in the 360 release's tree
constexpr int kNameEntryState = 93;       // NameEntryScreen (New Game's name)
constexpr int kDifficultyBack = 112;      // DifficultyScreen's ExitBack -> 93
constexpr int kMenuSlotsEntry = 159;      // ReadingCard's ExitHasValidSaveFiles -> 160 (no scripts)
constexpr int kMenuSlots = 160;           // GameSlotScreen (menu)
constexpr int kGameSlotsEntry = 385;      // in game: ReadingCard's ExitOperationDone -> 386 (no scripts)
constexpr int kGameSlots = 386;           // GameSlotScreen (in game)
// Slot 1's ExitAvailableSlot: the save into an empty slot ("Create New
// Save" is always panel 1): menu 185 -> SaveGameScreen 289 (its script
// saves slot 0), in game 408 -> AutoSaveGameScreen 326 (the saving screen).
constexpr int kMenuSaveExit = 185;
constexpr int kMenuSaveState = 289;
constexpr int kGameSaveExit = 408;
constexpr int kGameSaveState = 326;
// The tree's last top-level state (a child of the root "Frontend", node 1),
// a test screen whose subtree ends the file: the new nodes become its
// children (data/fight_tree.h: the file is the tree in pre-order).
constexpr int kEndOfTree = 992;
// The tree's path as the game builds it (the loader's node count lookup
// is keyed by it, data/fight_tree.h).
constexpr char kTreePath[] = "fighttrees/Frontend.bfig";

// The numbers the new nodes got (0 = the tree wasn't patched).
struct Route {
  int32_t exit_in = 0;  // ExitRenameMenu / ExitRenameInGame (the list's decision returns it)
  int32_t state = 0;    // RenameScreen
  int32_t done = 0;     // its ExitDone
  int32_t back = 0;     // its ExitBack
  int32_t save = 0;     // its ExitSave (naming a new save: Done saves it)
};
Route g_menu;
Route g_game;
std::atomic<bool> g_tree_patched{false};
std::atomic<bool> g_page_added{false};

// The front end's result flags (the byte at *(0x8259B190)+52 -> +8593):
// bit 3 = the name screen's Done, bit 2 = its Cancel.
constexpr uint32_t kGameGlobal = 0x8259B190;
constexpr uint32_t kFrontEndOffset = 52;
constexpr uint32_t kFrontEndResultFlags = 8593;
constexpr uint8_t kFlagDone = 0x08;
constexpr uint8_t kFlagCancel = 0x04;

uint32_t Read32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = GuestPtr(base, address);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// The decision of a RenameScreen state: Done / Cancel on the keyboard ->
// its exit (the save library renames the save on Done; when naming a new
// save, Done takes ExitSave into the saving screen), else stay.
int32_t Decide(uint8_t* base, const Route& route) {
  const uint32_t game = Read32(base, kGameGlobal);
  const uint32_t front_end = game ? Read32(base, game + kFrontEndOffset) : 0;
  if (!front_end) {
    return -1;
  }
  const uint8_t flags = (*GuestPtr(base, front_end + kFrontEndResultFlags));
  if (flags & kFlagDone) {
    return save_library::OnGameRenameFinished(base, true) ? route.save : route.done;
  }
  if (flags & kFlagCancel) {
    save_library::OnGameRenameFinished(base, false);
    return route.back;
  }
  return -1;
}

// Appends one route (5 nodes) back to the list state `slots`; `in_game`
// leaves the menus' frame and background out of the state. In the tree:
//   EndOfTree (992)
//     ExitRename...       -> RenameScreen   (taken from the list: the engine
//       RenameScreen                          follows any exit's target)
//         ExitDone        -> slots
//         ExitBack        -> slots
//         ExitSave        -> the saving screen (a copy of slot 1's
//                            ExitAvailableSlot `save_exit`, its script too)
bool AppendRoute(fight_tree::Tree& tree, int slots, int slots_entry, int save_exit, bool in_game,
                 Route* route) {
  using fight_tree::Tree;
  const int first = tree.Count() + 1;
  route->exit_in = first;
  route->state = first + 1;
  route->done = first + 2;
  route->back = first + 3;
  route->save = first + 4;

  Bytes exit_in = tree.Record(kDifficultyBack);  // its script runs on the way in
  // Siblings need different names (the loader refuses a child whose name
  // its parent already has: sub_82402560 -> sub_824024C8; both entries are
  // children of EndOfTree).
  Tree::SetName(exit_in, in_game ? "ExitRenameInGame" : "ExitRenameMenu");
  Tree::SetParent(exit_in, uint32_t(kEndOfTree));
  Tree::SetChildCount(exit_in, 1);
  Tree::SetTarget(exit_in, uint32_t(route->state));

  Bytes state = tree.Record(kNameEntryState);
  Tree::SetName(state, "RenameScreen");
  Tree::SetParent(state, uint32_t(route->exit_in));
  Tree::SetChildCount(state, 3);
  if (in_game && !Tree::RemoveAction(state, fight_tree::kActionNVBackground)) {
    return false;
  }

  Bytes done = tree.Record(slots_entry);  // no scripts
  Tree::SetName(done, "ExitDone");
  Tree::SetParent(done, uint32_t(route->state));
  Tree::SetChildCount(done, 0);
  Tree::SetTarget(done, uint32_t(slots));
  Bytes back = done;
  Tree::SetName(back, "ExitBack");
  Bytes save = tree.Record(save_exit);
  Tree::SetName(save, "ExitSave");
  Tree::SetParent(save, uint32_t(route->state));

  tree.AddChild(kEndOfTree);
  tree.Append(exit_in);
  tree.Append(state);
  tree.Append(done);
  tree.Append(back);
  tree.Append(save);
  return true;
}

bool PatchTree(const Bytes& original, Bytes* patched) {
  fight_tree::Tree tree;
  if (!tree.Load(original) || tree.Count() != kNodeCount ||
      tree.NameOf(kNameEntryState) != "NameEntryScreen" ||
      tree.TargetOf(kDifficultyBack) != kNameEntryState ||
      tree.NameOf(kMenuSlots) != "GameSlotScreen" || tree.TargetOf(kMenuSlotsEntry) != kMenuSlots ||
      tree.NameOf(kGameSlots) != "GameSlotScreen" || tree.TargetOf(kGameSlotsEntry) != kGameSlots ||
      tree.NameOf(kEndOfTree) != "EndOfTree" || tree.ParentOf(kEndOfTree) != 1 ||
      tree.NameOf(kMenuSaveExit) != "ExitAvailableSlot" || tree.TargetOf(kMenuSaveExit) != kMenuSaveState ||
      tree.NameOf(kGameSaveExit) != "ExitAvailableSlot" || tree.TargetOf(kGameSaveExit) != kGameSaveState) {
    REXLOG_WARN("Rename screen: Frontend.bfig isn't the tree it was made for ({} nodes): not changed",
                tree.Count());
    return false;
  }
  Route menu, game;
  if (!AppendRoute(tree, kMenuSlots, kMenuSlotsEntry, kMenuSaveExit, false, &menu) ||
      !AppendRoute(tree, kGameSlots, kGameSlotsEntry, kGameSaveExit, true, &game)) {
    REXLOG_WARN("Rename screen: the name entry state isn't as expected: Frontend.bfig not changed");
    return false;
  }
  *patched = tree.Save();
  // Numbers start at 1: the loader's array needs one slot more than nodes.
  fight_tree::SetNodeCount(kTreePath, tree.Count() + 1);
  g_menu = menu;
  g_game = game;
  fight_tree::SetFrontEndDecision(menu.state, [](PPCContext&, uint8_t* base) {
    return Decide(base, g_menu);
  });
  fight_tree::SetFrontEndDecision(game.state, [](PPCContext&, uint8_t* base) {
    return Decide(base, g_game);
  });
  g_tree_patched = true;
  REXLOG_INFO("Rename screen: nodes {}-{} (menu) and {}-{} (in game) added to the front end's tree",
              menu.exit_in, menu.save, game.exit_in, game.save);
  return true;
}

// ---------------------------------------------------------------------------
// The page (the in-game menus' package)
// ---------------------------------------------------------------------------

constexpr std::string_view kMenuPackage = "package\\cdd70a8c.p3d";  // Fe_Frontend
constexpr std::string_view kHostProject = "InGame.prj";
constexpr std::string_view kKeptProjectParts[] = {"GameStart_NameEntry.pag",
                                                  "GameStart_NameEntry.scr"};

// The keyboard's parts out of the menu package: the page and its screen
// (whole chunks), then the pictures and fonts the page names.
struct Parts {
  std::vector<std::pair<std::string, Bytes>> project_parts;
  std::vector<std::pair<std::string, Bytes>> resources;
};

bool CutParts(Parts* parts, std::string* error) {
  using namespace pure3d;
  Bytes d;
  if (!data_patcher::ReadArchiveFile(kMenuPackage, &d, error)) {
    return false;
  }
  Chunk root;
  std::vector<Chunk> top;
  if (!Parse(d, &root, &top)) {
    *error = "the menu package isn't a Pure3D file";
    return false;
  }
  Bytes pages;  // the kept page and screen, to find the pictures they name
  for (const Chunk& c : top) {
    if (c.id != kChunkProject) continue;
    for (const Chunk& k : ChildrenOf(d, c)) {
      const std::string_view name = NameOf(d, k);
      if (std::find(std::begin(kKeptProjectParts), std::end(kKeptProjectParts), name) !=
          std::end(kKeptProjectParts)) {
        parts->project_parts.emplace_back(std::string(name), Copy(d, k));
        pages.insert(pages.end(), d.begin() + k.offset, d.begin() + k.offset + k.total_size);
      }
    }
  }
  if (parts->project_parts.size() != std::size(kKeptProjectParts)) {
    *error = "the name entry page isn't in the menu package";
    return false;
  }
  // Pictures and fonts are named as padded strings inside the page (not
  // always NUL-ended right after the name): matched as plain text. A name
  // inside a longer one would only add a picture, which is harmless.
  const std::string_view page(reinterpret_cast<const char*>(pages.data()), pages.size());
  for (const Chunk& c : top) {
    if (c.id != kChunkSprite && c.id != kChunkFont) continue;
    const std::string_view name = NameOf(d, c);
    if (!name.empty() && page.find(name) != std::string_view::npos) {
      parts->resources.emplace_back(std::string(name), Copy(d, c));
    }
  }
  return true;
}

// The in-game menus package with the keyboard: the page and screen at the
// end of InGame.prj, the pictures / fonts it lacks before the project (as in
// the game's own packages: resources first, the project last). False for
// any other package of the category (no InGame.prj).
bool PatchInGameMenus(const Bytes& host, Bytes* out) {
  using namespace pure3d;
  Chunk root;
  std::vector<Chunk> top;
  if (!Parse(host, &root, &top)) {
    return false;
  }
  const auto project = std::find_if(top.begin(), top.end(), [&](const Chunk& c) {
    return c.id == kChunkProject && NameOf(host, c) == kHostProject;
  });
  if (project == top.end()) {
    return false;
  }
  static Parts parts;  // cut once (only the in-game menus of one language load per run)
  static bool cut = false;
  if (!cut) {
    std::string error;
    if (!CutParts(&parts, &error)) {
      REXLOG_WARN("Rename screen: no keyboard in game ({})", error);
      return false;
    }
    cut = true;
  }
  Bytes body;
  for (const Chunk& c : top) {
    if (&c != &*project) {
      const Bytes chunk = Copy(host, c);
      body.insert(body.end(), chunk.begin(), chunk.end());
    }
  }
  std::string added;
  for (const auto& [name, chunk] : parts.resources) {
    const bool present = std::any_of(top.begin(), top.end(), [&](const Chunk& c) {
      return (c.id == kChunkSprite || c.id == kChunkFont) && NameOf(host, c) == name;
    });
    if (!present) {
      body.insert(body.end(), chunk.begin(), chunk.end());
      added += (added.empty() ? "" : ", ") + name;
    }
  }
  Bytes project_children;
  for (const Chunk& k : ChildrenOf(host, *project)) {
    const Bytes chunk = Copy(host, k);
    project_children.insert(project_children.end(), chunk.begin(), chunk.end());
  }
  for (const auto& [name, chunk] : parts.project_parts) {
    project_children.insert(project_children.end(), chunk.begin(), chunk.end());
  }
  const Bytes new_project = WithChildren(host, *project, project_children);
  body.insert(body.end(), new_project.begin(), new_project.end());
  *out = WithChildren(host, root, body);
  g_page_added = true;
  REXLOG_INFO("Rename screen: the keyboard page added to the in-game menus, with {}", added);
  return true;
}

}  // namespace

void Register() {
  data_patcher::Register("rename screen route", data_patcher::kCategoryFightTree, "Frontend",
                         PatchTree);
  data_patcher::Register("keyboard page", data_patcher::kCategoryFrontend, "", PatchInGameMenus);
}

bool Available(bool in_game) {
  return g_tree_patched.load() &&
         data_patcher::Served(data_patcher::kCategoryFightTree, "Frontend") &&
         (!in_game || g_page_added.load());
}

int32_t EntryExit(bool in_game) { return in_game ? g_game.exit_in : g_menu.exit_in; }

}  // namespace rename_screen

