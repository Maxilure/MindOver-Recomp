// =============================================================================
// language/game_text_banks.cpp -- see game_text_banks.h
// =============================================================================
#include "game_text_banks.h"

#include <mutex>
#include <unordered_map>

#include <rex/logging.h>

#include "../data/pure3d.h"
#include "language.h"
#include "texts.h"

namespace game_text_banks {

namespace {

constexpr uint32_t kChunkBank = 0x1800D;          // a text bank
constexpr uint32_t kChunkBankLanguage = 0x1800E;  // one language of it
// The package with the "ingame" bank in every language (findings/51 s.8).
constexpr std::string_view kAllLanguagesPackage = "package\\c1e387c7.p3d";

// Banks are found inside the root chunk and, to be safe, one level further down.
void Collect(const data_patcher::Bytes& d, const std::vector<pure3d::Chunk>& chunks, int depth,
             std::vector<BankLanguage>* out) {
  for (const pure3d::Chunk& c : chunks) {
    if (c.id != kChunkBank) {
      if (depth > 0) Collect(d, pure3d::ChildrenOf(d, c), depth - 1, out);
      continue;
    }
    const std::string bank(pure3d::NameOf(d, c));
    for (const pure3d::Chunk& l : pure3d::ChildrenOf(d, c)) {
      if (l.id != kChunkBankLanguage || l.data_size < 13) continue;
      // name (length byte + text), then the letter, then count / modulus / size.
      size_t p = l.offset + 13 + d[l.offset + 12];
      const size_t end = l.offset + l.total_size;
      if (p + 13 > end) continue;
      BankLanguage b;
      b.bank = bank;
      b.letter = char(d[p]);
      b.count = pure3d::Le32(&d[p + 1]);
      p += 13;
      b.keys_at = p;
      b.offsets_at = p + 4 * size_t(b.count);
      b.area_at = b.offsets_at + 4 * size_t(b.count);
      b.area_end = end;
      if (b.area_at > end) continue;  // not what we expect: leave it
      out->push_back(std::move(b));
    }
  }
}

using Swap = std::unordered_map<texts::GameTextKey, std::u16string, texts::GameTextKeyHash>;

// One language sub-chunk's data (name, letter, count / modulus / size, keys,
// offsets, strings) with `swap`'s strings put in. Returns how many it swapped.
size_t RebuildLanguage(pure3d::Node& node, const std::string& bank, const Swap& swap) {
  pure3d::Bytes& d = node.data;
  const size_t head = pure3d::NameEnd(node) + 1;  // after the letter: count, modulus, size
  if (head + 12 > d.size()) return 0;
  const uint32_t count = pure3d::Le32(&d[head]);
  const size_t keys_at = head + 12;
  const size_t offsets_at = keys_at + 4 * size_t(count);
  const size_t area_at = offsets_at + 4 * size_t(count);
  if (area_at > d.size()) return 0;
  std::vector<std::u16string> strings(count);
  size_t swapped = 0;
  texts::GameTextKey look{bank, 0};
  for (uint32_t i = 0; i < count; ++i) {
    look.key = pure3d::Le32(&d[keys_at + 4 * size_t(i)]);
    if (const auto it = swap.find(look); it != swap.end()) {
      strings[i] = it->second;
      ++swapped;
      continue;
    }
    size_t p = area_at + pure3d::Le32(&d[offsets_at + 4 * size_t(i)]);
    for (; p + 1 < d.size() && (d[p] | d[p + 1]) != 0; p += 2) strings[i] += char16_t(d[p] | d[p + 1] << 8);
  }
  if (!swapped) return 0;
  // The new string area: each string UTF-16 LE + a 0 character, in key
  // order, padded to 4 bytes.
  pure3d::Bytes area;
  std::vector<uint32_t> offsets(count);
  for (uint32_t i = 0; i < count; ++i) {
    offsets[i] = uint32_t(area.size());
    for (const char16_t c : strings[i]) {
      area.push_back(uint8_t(c));
      area.push_back(uint8_t(c >> 8));
    }
    area.push_back(0);
    area.push_back(0);
  }
  while (area.size() % 4) area.push_back(0);
  pure3d::Bytes out(d.begin(), d.begin() + ptrdiff_t(offsets_at));
  for (const uint32_t o : offsets) pure3d::PutLe32(out, o);
  out.insert(out.end(), area.begin(), area.end());
  node.data = std::move(out);
  pure3d::SetNodeLe32(node, head + 8, uint32_t(area.size()));  // "size" = the string area's bytes
  return swapped;
}

// Every bank inside `n` (and one level further down, like Find).
size_t RebuildBanks(pure3d::Node& n, char letter, int depth, const Swap& swap) {
  size_t swapped = 0;
  for (pure3d::Node& c : n.children) {
    if (c.id != kChunkBank) {
      if (depth > 0) swapped += RebuildBanks(c, letter, depth - 1, swap);
      continue;
    }
    const std::string bank(pure3d::NodeName(c));
    for (pure3d::Node& l : c.children) {
      const size_t at = pure3d::NameEnd(l);
      if (l.id == kChunkBankLanguage && at < l.data.size() && char(l.data[at]) == letter) {
        swapped += RebuildLanguage(l, bank, swap);
      }
    }
  }
  return swapped;
}

// Data patch for every menu package (data_patcher.h): runs after the save
// wording one, so it starts from that one's strings.
bool PatchTranslation(const data_patcher::Bytes& original, data_patcher::Bytes* patched) {
  const Swap& swap = texts::GameTranslation();
  if (swap.empty()) return false;
  pure3d::Chunk root;
  std::vector<pure3d::Chunk> top;
  if (!pure3d::Parse(original, &root, &top)) return false;
  bool has_bank = false;
  for (const pure3d::Chunk& c : top) has_bank |= c.id == kChunkBank;
  if (!has_bank) return false;  // most packages: pictures and pages only
  pure3d::Node tree = pure3d::ToNode(original, root);
  const size_t swapped = RebuildBanks(tree, RunningLetter(), 1, swap);
  if (!swapped) return false;
  *patched = pure3d::ToBytes(tree);
  REXLOG_INFO("Game texts: {} strings of a package in the player's language ({} -> {} bytes)", swapped,
              original.size(), patched->size());
  return true;
}

}  // namespace

void Install() {
  data_patcher::Register("the game's text in the player's language", data_patcher::kCategoryFrontend, "",
                         &PatchTranslation);
}

uint32_t BankLanguage::KeyAt(const data_patcher::Bytes& d, uint32_t i) const {
  return pure3d::Le32(&d[keys_at + 4 * size_t(i)]);
}

void BankLanguage::SpanOf(const data_patcher::Bytes& d, uint32_t i, size_t* begin, size_t* end) const {
  size_t b = area_at + pure3d::Le32(&d[offsets_at + 4 * size_t(i)]);
  size_t e = b;
  while (e + 1 < area_end && (d[e] | d[e + 1]) != 0) e += 2;
  if (b > area_end) b = e = area_end;
  *begin = b;
  *end = e;
}

std::u16string BankLanguage::StringAt(const data_patcher::Bytes& d, uint32_t i) const {
  size_t b, e;
  SpanOf(d, i, &b, &e);
  std::u16string s;
  for (size_t p = b; p < e; p += 2) s += char16_t(d[p] | d[p + 1] << 8);
  return s;
}

std::vector<BankLanguage> Find(const data_patcher::Bytes& file) {
  std::vector<BankLanguage> out;
  pure3d::Chunk root;
  std::vector<pure3d::Chunk> top;
  if (!pure3d::Parse(file, &root, &top)) return out;
  Collect(file, top, 1, &out);
  return out;
}

char RunningLetter() {
  const auto& texts = language::TextLanguages();
  const int i = language::RunningText();
  return i >= 0 && i < int(texts.size()) ? texts[i].letter : 'U';
}

std::u16string InGame(uint32_t key, char letter) {
  // letter -> key -> text, read once.
  static std::mutex mutex;
  static bool loaded = false;
  static std::unordered_map<char, std::unordered_map<uint32_t, std::u16string>> strings;
  // A translation of the game's text (Brazilian Portuguese) answers first.
  if (!letter) {
    const auto& swap = texts::GameTranslation();
    if (const auto it = swap.find(texts::GameTextKey{"ingame", key}); it != swap.end()) return it->second;
  }
  std::lock_guard lock(mutex);
  if (!loaded) {
    loaded = true;
    data_patcher::Bytes d;
    std::string error;
    if (!data_patcher::ReadArchiveFile(kAllLanguagesPackage, &d, &error)) {
      REXLOG_WARN("Game texts: can't read {} ({}): the game's own words won't be used", kAllLanguagesPackage,
                  error);
    } else {
      for (const BankLanguage& b : Find(d)) {
        if (b.bank != "ingame") continue;
        auto& table = strings[b.letter];
        for (uint32_t i = 0; i < b.count; ++i) table.emplace(b.KeyAt(d, i), b.StringAt(d, i));
      }
      REXLOG_INFO("Game texts: in-game bank in {} languages", strings.size());
    }
  }
  const auto lang = strings.find(letter ? letter : RunningLetter());
  if (lang == strings.end()) return {};
  const auto it = lang->second.find(key);
  return it == lang->second.end() ? std::u16string() : it->second;
}

}  // namespace game_text_banks
