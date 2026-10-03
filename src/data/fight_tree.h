// =============================================================================
// data/fight_tree.h -- the game's fight trees (.bfig): editing nodes, native decisions
// =============================================================================
//
// WHAT: Radical's "fight trees" are state machines stored as data
// (fighttrees/*.bfig in default.rcf): every character's moves, and the front
// end's screens (Frontend.bfig: which screen follows which, findings/24
// section 6.5). This module can add nodes to a tree file (for a data patch,
// data/data_patcher.h) and give the new FRONT END states decision functions
// written in C++.
//
// THE FILE (big-endian): a u32-size-prefixed Lua chunk (the tree's
// scripts), then the nodes, one record after another. A record =
//   u32 type   0x83A93633 state, 0xBC27359D exit, 0xE056B923 root,
//              0xE8CB721B branch (platform: Xenon / NotXenon)
//   u32 name length, the name (padded to 4 bytes; no NUL when the length is
//              a multiple of 4)
//   state:     u32 parent node, u32 number of children, then its lists
//   exit:      u32 parent node, u32 number of children, 0, -1.0f, 0, -1.0f,
//              u32 TARGET state (-1: a group of exits), 0, then its list
// A list = u32 kind, u32 byte size, then (when not empty) a float count and
// its items: 0x04F3A980 / 0x0A870F46 scripts run on the way (items
// 0xAE6D7C53 naming a script of the Lua chunk), 0x27DE7629 the state's
// actions (0x861F0934 = CNameEntryScreenAction, 0x90548355 =
// CNVBackgroundAction: the menus' purple frame and blue swirl, only in
// menu states; 0x7B641ABE CGameSlotScreenAction + its page names, 0x03221AF6
// the button prompts' texts, ...). Records follow each other with no gaps:
// a record ends where the next one starts.
//
// THE ORDER IS THE TREE, PRE-ORDER: the loader reads a node, then its
// children (as many as its count says), recursively; the parent numbers
// don't place a node (a first version appended children of node 160 at the
// end of the file and gave 160 one more child: the loader then took the
// wrong record as that child and wrote through a null pointer, 2026-10-03).
// New nodes can only go at the end as part of the LAST subtree: children
// of a node on the file's last path (the root, its last child, ...).
// SIBLINGS NEED DIFFERENT NAMES: attaching a child (sub_82402560) first
// looks its name up among the parent's children (sub_824024C8) and refuses
// a duplicate; the node then hangs loose (no parent, its exits' targets
// never resolved) and taking it reads through a null pointer (found
// 2026-10-03: two "ExitRename" children of one node).
//
// NODE NUMBERS = file order, from 1, per tree (the loader's counter starts
// at 1 for every file, traced 2026-10-03). A node keeps its number at +30
// (s16) of its object. The front end's COMPILED decisions are a table of
// function pointers at 0x82503030 indexed by that number (0x821174B0 =
// none); its dispatcher sub_8211AF40 (r4 = the state's node) jumps there.
// The table has room past the tree's 994 nodes (995 up are "none"): new
// nodes go at the END of the file, so no existing number moves (the
// compiled decisions return exit numbers as constants).
//
// HOW MANY NODES: the loader makes room for a tree's nodes BEFORE reading
// it, from fighttrees/branchcount.txt ("fighttrees/Frontend.bfig 1004"),
// looked up BY THE FILE'S PATH (sub_820C27A8; 0 = unknown -> no room ->
// the loader writes through a null pointer at 0x820C25B0, found
// 2026-10-03 when the patched tree came from "crashmom/Frontend.bfig").
// That lookup is replaced here: a path we serve counts as the game's own,
// and a patched tree gets at least its new node count (SetNodeCount).
// =============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <rex/ppc/context.h>

namespace fight_tree {

using Bytes = std::vector<uint8_t>;

constexpr uint32_t kState = 0x83A93633;
constexpr uint32_t kExit = 0xBC27359D;
constexpr uint32_t kRoot = 0xE056B923;
constexpr uint32_t kBranch = 0xE8CB721B;

// Lists inside records, and the actions we look for.
constexpr uint32_t kListActions = 0x27DE7629;
constexpr uint32_t kActionNameEntryScreen = 0x861F0934;  // CNameEntryScreenAction
constexpr uint32_t kActionNVBackground = 0x90548355;     // CNVBackgroundAction

// A tree file, its records found.
class Tree {
 public:
  // False if `file` doesn't look like a fight tree.
  bool Load(const Bytes& file);

  int Count() const { return int(records_.size()); }
  uint32_t TypeOf(int number) const;
  std::string NameOf(int number) const;
  uint32_t ParentOf(int number) const;
  // An exit's target state (0 if `number` isn't an exit).
  uint32_t TargetOf(int number) const;
  // The whole record of node `number`.
  Bytes Record(int number) const;

  // One more child for node `number` (its child count; call when a child
  // of it is appended: it must be on the file's last path, see above).
  void AddChild(int number);
  // Appends a record (it gets number Count() + 1, returned).
  int Append(const Bytes& record);
  // The file with the changes.
  Bytes Save() const;

  // Record editing (any record from Record()):
  static void SetName(Bytes& record, std::string_view name);
  static void SetParent(Bytes& record, uint32_t parent);
  static void SetTarget(Bytes& record, uint32_t target);  // exits
  static void SetChildCount(Bytes& record, uint32_t count);  // states
  // Removes action `kind` from a state's action list; false if not there.
  static bool RemoveAction(Bytes& record, uint32_t kind);

 private:
  struct Span {
    size_t offset;
    size_t size;
  };
  Bytes file_;
  std::vector<Span> records_;
  Bytes appended_;
};

// A patched tree, by the game's own path ("fighttrees/Frontend.bfig"), has
// `count` nodes: the loader makes room for at least that many.
void SetNodeCount(std::string path, int count);

// A decision written in C++ for front end node `number` (one we appended):
// returns the exit to take, -1 = stay. Runs on the game's main thread.
using Decision = std::function<int32_t(PPCContext& ctx, uint8_t* base)>;
void SetFrontEndDecision(int32_t number, Decision decision);

}  // namespace fight_tree
