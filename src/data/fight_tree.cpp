// =============================================================================
// data/fight_tree.cpp -- see fight_tree.h
// =============================================================================
#include <cstdlib>
#include "fight_tree.h"
#include "../guest_memory.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <map>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

#include "data_patcher.h"

extern "C" REX_FUNC(__imp__sub_8211AF40);  // the front end's decision dispatcher

namespace fight_tree {
namespace {

uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void PutBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
bool IsNodeType(uint32_t type) {
  return type == kState || type == kExit || type == kRoot || type == kBranch;
}
// Where a record's fields start (after its type, name length and name).
size_t FieldsOffset(const Bytes& record) {
  const uint32_t length = Be32(&record[4]);
  return 8 + ((length + 3) & ~3u);
}
// The offset in `record` of the first aligned word `value` at or after
// `from` (records keep 4-byte alignment; names are padded), or 0.
size_t FindWord(const Bytes& record, size_t from, uint32_t value) {
  for (size_t p = from; p + 4 <= record.size(); p += 4) {
    if (Be32(&record[p]) == value) return p;
  }
  return 0;
}

std::mutex g_mutex;
std::map<int32_t, Decision> g_decisions;  // front end node number -> ours
std::map<std::string, int, std::less<>> g_node_counts;  // patched tree path -> its node count

// The node count table filled from fighttrees/branchcount.txt
// (sub_820C2830 adds an entry): entries of 36 bytes, a 32-byte path then a
// u32 count, at 0x82597DA0; how many at 0x825AAD74.
constexpr uint32_t kBranchCountTable = 0x82597DA0;
constexpr uint32_t kBranchCountEntries = 0x8259AD74;
constexpr uint32_t kBranchCountEntrySize = 36;
constexpr uint32_t kBranchCountPathSize = 32;

}  // namespace

bool Tree::Load(const Bytes& file) {
  file_ = file;
  records_.clear();
  appended_.clear();
  if (file_.size() < 8) return false;
  // The Lua chunk first (u32 size + the chunk), then the records. They are
  // found by their type word followed by a printable name (the way the
  // records were first mapped, findings/24 s.6.5; a patch then checks the
  // count and the names it relies on).
  const size_t start = size_t(Be32(&file_[0])) + 4;
  std::vector<size_t> starts;
  for (size_t p = (start + 3) & ~size_t(3); p + 12 <= file_.size(); p += 4) {
    if (!IsNodeType(Be32(&file_[p]))) continue;
    const uint32_t length = Be32(&file_[p + 4]);
    if (length < 1 || length >= 80 || p + 8 + length > file_.size()) continue;
    bool printable = true;
    for (uint32_t i = 0; i < length && printable; ++i) {
      printable = file_[p + 8 + i] >= 32 && file_[p + 8 + i] < 127;
    }
    if (printable) starts.push_back(p);
  }
  for (size_t i = 0; i < starts.size(); ++i) {
    const size_t end = i + 1 < starts.size() ? starts[i + 1] : file_.size();
    records_.push_back({starts[i], end - starts[i]});
  }
  return !records_.empty();
}

uint32_t Tree::TypeOf(int number) const {
  return Be32(&file_[records_[number - 1].offset]);
}
std::string Tree::NameOf(int number) const {
  const size_t p = records_[number - 1].offset;
  return std::string(reinterpret_cast<const char*>(&file_[p + 8]), Be32(&file_[p + 4]));
}
uint32_t Tree::ParentOf(int number) const {
  const Bytes record = Record(number);
  return Be32(&record[FieldsOffset(record)]);
}
uint32_t Tree::TargetOf(int number) const {
  if (TypeOf(number) != kExit) return 0;
  const Bytes record = Record(number);
  return Be32(&record[FieldsOffset(record) + 24]);
}
Bytes Tree::Record(int number) const {
  const Span& s = records_[number - 1];
  return Bytes(file_.begin() + s.offset, file_.begin() + s.offset + s.size);
}

void Tree::AddChild(int number) {
  const size_t p = records_[number - 1].offset;
  const size_t fields = p + 8 + ((Be32(&file_[p + 4]) + 3) & ~3u);
  PutBe32(&file_[fields + 4], Be32(&file_[fields + 4]) + 1);
}

int Tree::Append(const Bytes& record) {
  // Appended records live in their own buffer; Save() puts them after the
  // last original record (the end of the file).
  records_.push_back({0, 0});  // a placeholder: numbers only (not readable)
  appended_.insert(appended_.end(), record.begin(), record.end());
  return Count();
}

Bytes Tree::Save() const {
  Bytes out = file_;
  out.insert(out.end(), appended_.begin(), appended_.end());
  return out;
}

void Tree::SetName(Bytes& record, std::string_view name) {
  const size_t old_fields = FieldsOffset(record);
  Bytes out(8, 0);
  PutBe32(&out[0], Be32(&record[0]));
  PutBe32(&out[4], uint32_t(name.size()));
  out.insert(out.end(), name.begin(), name.end());
  out.resize(8 + ((name.size() + 3) & ~size_t(3)), 0);
  out.insert(out.end(), record.begin() + old_fields, record.end());
  record = std::move(out);
}
void Tree::SetParent(Bytes& record, uint32_t parent) {
  PutBe32(&record[FieldsOffset(record)], parent);
}
void Tree::SetTarget(Bytes& record, uint32_t target) {
  PutBe32(&record[FieldsOffset(record) + 24], target);
}
void Tree::SetChildCount(Bytes& record, uint32_t count) {
  PutBe32(&record[FieldsOffset(record) + 4], count);
}

bool Tree::RemoveAction(Bytes& record, uint32_t kind) {
  // The action list: kind, byte size, float count, items. Only actions
  // without parameters can be found this way (the item is "kind, 0"; the
  // others carry strings whose length we'd have to know).
  const size_t list = FindWord(record, FieldsOffset(record) + 8, kListActions);
  if (!list) return false;
  const uint32_t size = Be32(&record[list + 4]);
  const size_t end = list + 8 + size;
  if (size < 12 || end > record.size()) return false;
  for (size_t p = list + 12; p + 8 <= end; p += 4) {
    if (Be32(&record[p]) != kind || Be32(&record[p + 4]) != 0) continue;
    record.erase(record.begin() + p, record.begin() + p + 8);
    PutBe32(&record[list + 4], size - 8);
    float count;
    uint32_t bits = Be32(&record[list + 8]);
    std::memcpy(&count, &bits, 4);
    count -= 1.0f;
    std::memcpy(&bits, &count, 4);
    PutBe32(&record[list + 8], bits);
    return true;
  }
  return false;
}

void SetNodeCount(std::string path, int count) {
  std::lock_guard lock(g_mutex);
  g_node_counts[std::move(path)] = count;
}

void SetFrontEndDecision(int32_t number, Decision decision) {
  std::lock_guard lock(g_mutex);
  g_decisions[number] = std::move(decision);
}

// Written by the dispatcher (game thread), read by the debug console.
std::atomic<int32_t> g_fe_state{-1}, g_fe_from{-1}, g_fe_exit{-1};
std::atomic<uint32_t> g_fe_exits{0};

FrontEndPosition CurrentFrontEnd() {
  FrontEndPosition p;
  p.state = g_fe_state.load();
  p.last_from = g_fe_from.load();
  p.last_exit = g_fe_exit.load();
  p.exits = g_fe_exits.load();
  return p;
}

}  // namespace fight_tree

// ON BY DEFAULT: one of the session log's event logs (session_log.h;
// --event_logs=false turns them all off).
REXCVAR_DEFINE_BOOL(debug_frontend_trace, true, "CrashMoM",
                    "Debug: log every exit the front end's screen tree takes (state number -> "
                    "exit number; names: notes/scratch-tools/fig_tree.py)");

// The front end's decision dispatcher (r4 = the current state's node; its
// number = s16 at +30, read as the low half of the word at +28): a node
// with a decision of ours runs it, the others the game's compiled table
// (0x82503030; our numbers are "none" there).
extern "C" REX_FUNC(sub_8211AF40) {
  using namespace fight_tree;
  const uint32_t node = ctx.r4.u32;
  if (node) {
    const uint8_t* p = GuestPtr(base, node + 30);
    const int32_t number = int16_t(uint16_t(p[0]) << 8 | p[1]);
    Decision decision;
    {
      std::lock_guard lock(g_mutex);
      if (const auto it = g_decisions.find(number); it != g_decisions.end()) {
        decision = it->second;
      }
    }
    if (decision) {
      ctx.r3.u64 = uint32_t(decision(ctx, base));
    } else {
      __imp__sub_8211AF40(ctx, base);
    }
    g_fe_state = number;
    if (int32_t(ctx.r3.u32) >= 0) {
      g_fe_from = number;
      g_fe_exit = int32_t(ctx.r3.u32);
      ++g_fe_exits;
    }
    // --debug_frontend_trace: one line per exit taken (-1 = stay, not logged).
    if (REXCVAR_GET(debug_frontend_trace) && int32_t(ctx.r3.u32) >= 0) {
      REXLOG_INFO("Front end: state {} -> exit {}", number, int32_t(ctx.r3.u32));
    }
    return;
  }
  __imp__sub_8211AF40(ctx, base);
}

// The node count of a tree file (r3 = its path as the game opens it; returns
// the count, 0 = unknown). The game's own (sub_820C27A8) compares the path
// with each entry (strcmp); ours does the same with the game's own path of
// a file we serve (data/data_patcher.h), and never gives a patched tree
// less room than its nodes need.
extern "C" REX_FUNC(sub_820C27A8) {
  using namespace fight_tree;
  const char* path = reinterpret_cast<const char*>(GuestPtr(base, ctx.r3.u32));
  std::string key = data_patcher::OriginalPath(path);
  if (key.empty()) {
    key = path;
  }
  int count = 0;
  const uint32_t entries = Be32(GuestPtr(base, kBranchCountEntries));
  for (uint32_t i = 0; i < entries; ++i) {
    const uint32_t entry = kBranchCountTable + i * kBranchCountEntrySize;
    const char* name = reinterpret_cast<const char*>(GuestPtr(base, entry));
    if (strnlen(name, kBranchCountPathSize) < kBranchCountPathSize && key == name) {
      count = int(Be32(GuestPtr(base, entry + kBranchCountPathSize)));
      break;
    }
  }
  {
    std::lock_guard lock(g_mutex);
    if (const auto it = g_node_counts.find(key); it != g_node_counts.end()) {
      count = std::max(count, it->second);
    }
  }
  if (count == 0) {
    // The loader (sub_820C23C0) writes through a null node array next: say
    // which tree, while there's still a log line to write.
    REXLOG_WARN("Fight tree: no node count for '{}' ({} entries in the table): the game will crash",
                key, entries);
  }
  ctx.r3.u64 = uint32_t(count);
}
