// =============================================================================
// debug_console.cpp -- the debug console's socket, commands and expressions
// =============================================================================
//
// See debug_console.h for what it is and the command list. Layout:
//   1. guest memory reads/writes, every address checked readable first
//   2. address expressions (Parser)
//   3. the commands (Run), one reply text each
//   4. the socket thread (Linux): one thread per connection, so a long
//      `wait` doesn't block another client
// =============================================================================

#include "debug_console.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__unix__)
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/ui_event.h>

#include "cheats/cheats.h"
#include "data/fight_tree.h"
#include "pddi/intercept.h"
#include "debug_input_script.h"
#include "input_record.h"
#include "native/ab_capture.h"
#include "players/more_players.h"

REXCVAR_DEFINE_STRING(debug_console, "", "CrashMoM",
                      "Debug: a Unix socket at this path answers questions about the running game "
                      "(memory, players, front end state, waits; client: tools/mom.py)");

namespace debug_console {
namespace {

using Clock = std::chrono::steady_clock;
const Clock::time_point g_start = Clock::now();

// The front end's state while a level is played with no menu up
// (GameRunning, a child of InGame 486 in Frontend.bfig).
constexpr int32_t kGameRunning = 489;
// uber +52 = the front end; its +144 = the level's fade-in timer (1 -> 0).
constexpr uint32_t kFrontEnd = 52;
constexpr uint32_t kFadeIn = 144;

// From Start(): for `fkey`.
UiPoster g_post;
rex::ui::Window* g_window = nullptr;

// An error inside a command: its text becomes the "@end error <text>" line.
struct Error {
  std::string what;
};

// -----------------------------------------------------------------------------
// 1. Guest memory
// -----------------------------------------------------------------------------

// Is [address, address + size) committed guest memory? (Same check as the
// cheats' Readable: cheats.cpp.)
bool Readable(uint32_t address, uint32_t size) {
  if (address < 0x1000 || size == 0 || address + (size - 1) < address) return false;
  auto* memory = rex::system::kernel_memory();
  if (!memory) return false;
  auto* heap = memory->LookupHeap(address);
  return heap &&
         heap->QueryRangeAccess(address, address + size - 1) != rex::memory::PageAccess::kNoAccess;
}

uint8_t* Guest(uint32_t address, uint32_t size) {
  if (!Readable(address, size)) {
    throw Error{fmt::format("0x{:08X} isn't readable guest memory", address)};
  }
  return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(address);
}

// Big-endian, like everything the game stores.
uint32_t ReadBE(uint32_t address, int bytes) {
  const uint8_t* p = Guest(address, bytes);
  uint32_t v = 0;
  for (int i = 0; i < bytes; ++i) v = v << 8 | p[i];
  return v;
}
float ReadF32(uint32_t address) {
  const uint32_t bits = ReadBE(address, 4);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}
void WriteBE(uint32_t address, int bytes, uint32_t v) {
  uint8_t* p = Guest(address, bytes);
  for (int i = bytes - 1; i >= 0; --i, v >>= 8) p[i] = uint8_t(v);
}

// -----------------------------------------------------------------------------
// 2. Address expressions (grammar in debug_console.h)
// -----------------------------------------------------------------------------

class Parser {
 public:
  explicit Parser(std::string_view text) : s_(text) {}

  uint32_t Parse() {
    const uint32_t v = Sum();
    Skip();
    if (i_ != s_.size()) Fail("unexpected text");
    return v;
  }

 private:
  [[noreturn]] void Fail(const char* why) {
    throw Error{fmt::format("expression \"{}\": {} at column {}", s_, why, i_ + 1)};
  }
  void Skip() {
    while (i_ < s_.size() && s_[i_] == ' ') ++i_;
  }
  bool Eat(char c) {
    Skip();
    if (i_ < s_.size() && s_[i_] == c) {
      ++i_;
      return true;
    }
    return false;
  }
  uint32_t Sum() {
    uint32_t v = Product();
    for (;;) {
      if (Eat('+')) v += Product();
      else if (Eat('-')) v -= Product();
      else return v;
    }
  }
  uint32_t Product() {
    uint32_t v = Atom();
    while (Eat('*')) v *= Atom();
    return v;
  }
  uint32_t Atom() {
    if (Eat('(')) {
      const uint32_t v = Sum();
      if (!Eat(')')) Fail("missing )");
      return v;
    }
    if (Eat('[')) {  // pointer chase: the word at that address
      const uint32_t address = Sum();
      if (!Eat(']')) Fail("missing ]");
      return ReadBE(address, 4);
    }
    Skip();
    const size_t start = i_;
    while (i_ < s_.size() && (std::isalnum(uint8_t(s_[i_])) || s_[i_] == '_')) ++i_;
    const std::string_view word = s_.substr(start, i_ - start);
    if (word.empty()) Fail("expected a number, a name, ( or [");
    if (std::isdigit(uint8_t(word[0]))) {
      uint32_t v = 0;
      const bool hex = word.size() > 2 && word[0] == '0' && (word[1] == 'x' || word[1] == 'X');
      const auto digits = hex ? word.substr(2) : word;
      const auto r = std::from_chars(digits.data(), digits.data() + digits.size(), v, hex ? 16 : 10);
      if (r.ec != std::errc() || r.ptr != digits.data() + digits.size()) Fail("bad number");
      return v;
    }
    if (word == "uber") return ReadBE(0x8259B190, 4);  // the game's central object
    if (word == "game") return 0x825B0008;              // the game object (sub_82270608)
    if (word.size() == 2 && (word[0] == 'p' || word[0] == 't') && word[1] >= '1' && word[1] <= '4') {
      const int p = word[1] - '1';
      return word[0] == 'p' ? more_players::CharacterOfPlayer(p) : more_players::TitanOfPlayer(p);
    }
    Fail("unknown name (uber, game, p1-p4, t1-t4)");
  }

  std::string_view s_;
  size_t i_ = 0;
};

uint32_t Eval(std::string_view text) { return Parser(text).Parse(); }

// -----------------------------------------------------------------------------
// 3. Commands
// -----------------------------------------------------------------------------

std::vector<std::string_view> Words(std::string_view line) {
  std::vector<std::string_view> words;
  while (!line.empty()) {
    const size_t start = line.find_first_not_of(' ');
    if (start == std::string_view::npos) break;
    line.remove_prefix(start);
    const size_t end = std::min(line.find(' '), line.size());
    words.push_back(line.substr(0, end));
    line.remove_prefix(end);
  }
  return words;
}

int64_t Number(std::string_view word, const char* what) {
  int64_t v = 0;
  const auto r = std::from_chars(word.data(), word.data() + word.size(), v);
  if (r.ec != std::errc() || r.ptr != word.data() + word.size()) {
    throw Error{fmt::format("{}: \"{}\" isn't a number", what, word)};
  }
  return v;
}

int64_t UptimeMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_start).count();
}

// Polls `done` every 20 ms until it returns a text (the reply) or the time
// runs out.
std::string WaitFor(int64_t timeout_ms, const std::function<std::optional<std::string>()>& done) {
  const auto end = Clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    if (auto reply = done()) return *reply;
    if (Clock::now() >= end) throw Error{fmt::format("timed out after {} ms", timeout_ms)};
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

std::string FrontEndText() {
  const auto fe = fight_tree::CurrentFrontEnd();
  return fmt::format("front end: state {}, last exit {} -> {}, {} exits so far", fe.state,
                     fe.last_from, fe.last_exit, fe.exits);
}

// One value of `type` at `address`, as text.
std::string Format(std::string_view type, uint32_t address) {
  if (type == "u8") return fmt::format("{}", ReadBE(address, 1));
  if (type == "u16") return fmt::format("{}", ReadBE(address, 2));
  if (type == "u32") {
    const uint32_t v = ReadBE(address, 4);
    return fmt::format("0x{:08X} ({})", v, v);
  }
  if (type == "s32") return fmt::format("{}", int32_t(ReadBE(address, 4)));
  if (type == "f32") return fmt::format("{:g}", ReadF32(address));
  if (type == "vec3") {
    return fmt::format("({:.3f}, {:.3f}, {:.3f})", ReadF32(address), ReadF32(address + 4),
                       ReadF32(address + 8));
  }
  throw Error{fmt::format("unknown type \"{}\"", type)};
}

uint32_t TypeSize(std::string_view type) {
  if (type == "u8") return 1;
  if (type == "u16") return 2;
  if (type == "vec3") return 12;
  return 4;
}

std::string Read(const std::vector<std::string_view>& w) {
  if (w.size() < 2) throw Error{"read <expr> [type] [count]"};
  const uint32_t address = Eval(w[1]);
  const std::string_view type = w.size() > 2 ? w[2] : "u32";
  const int64_t count = w.size() > 3 ? std::clamp<int64_t>(Number(w[3], "count"), 1, 65536) : 1;
  std::string out;
  if (type == "str" || type == "wstr") {  // up to the first 0 (or `count` letters, default 256)
    const int unit = type == "str" ? 1 : 2;
    const int64_t max = w.size() > 3 ? count : 256;
    std::string text;
    for (int64_t i = 0; i < max; ++i) {
      const uint32_t c = ReadBE(address + uint32_t(i * unit), unit);
      if (!c) break;
      text += c < 0x80 && std::isprint(int(c)) ? char(c) : '?';
    }
    return fmt::format("0x{:08X}: \"{}\"\n", address, text);
  }
  if (type == "bytes") {  // a hex dump, 16 per line
    for (int64_t i = 0; i < count; i += 16) {
      out += fmt::format("0x{:08X}:", address + uint32_t(i));
      for (int64_t j = i; j < std::min<int64_t>(count, i + 16); ++j) {
        out += fmt::format(" {:02X}", ReadBE(address + uint32_t(j), 1));
      }
      out += '\n';
    }
    return out;
  }
  const uint32_t size = TypeSize(type);
  for (int64_t i = 0; i < count; ++i) {
    const uint32_t a = address + uint32_t(i) * size;
    out += fmt::format("0x{:08X} (+{}): {}\n", a, uint32_t(i) * size, Format(type, a));
  }
  return out;
}

std::string Write(const std::vector<std::string_view>& w) {
  if (w.size() != 4) throw Error{"write <expr> <type> <value>"};
  const uint32_t address = Eval(w[1]);
  const std::string_view type = w[2];
  const std::string value(w[3]);
  if (type == "f32") {
    const float f = std::strtof(value.c_str(), nullptr);
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    WriteBE(address, 4, bits);
  } else if (type == "u8" || type == "u16" || type == "u32") {
    WriteBE(address, int(TypeSize(type)), Eval(value));
  } else {
    throw Error{"write types: u8 u16 u32 f32"};
  }
  REXLOG_INFO("Debug console: wrote {} {} at 0x{:08X}", type, value, address);
  return fmt::format("0x{:08X} = {}\n", address, Format(type, address));
}

std::string Players() {
  const cheats::Snapshot snap = cheats::GetSnapshot();
  std::string out;
  for (int p = 0; p < cheats::kPlayers; ++p) {
    const uint32_t actor = more_players::CharacterOfPlayer(p);
    const uint32_t titan = more_players::TitanOfPlayer(p);
    if (!actor && !titan && !snap.players[p].present) continue;
    const cheats::PlayerInfo& i = snap.players[p];
    out += fmt::format("p{}: {}, {}{}, level {}/{}, health {:.1f}/{:.1f}, mojo {}, actor 0x{:08X}, titan 0x{:08X}",
                       p + 1, i.present ? "in play" : "not joined", i.name, i.mask ? " (mask)" : "",
                       i.level, i.max_level, i.hitpoints, i.max_hitpoints, i.mojo, actor, titan);
    // Position: the actor's world matrix (actor +44), row 3 (+48).
    const uint32_t body = titan ? titan : actor;
    if (body && Readable(body + 44, 4)) {
      const uint32_t matrix = ReadBE(body + 44, 4);
      if (Readable(matrix + 48, 12)) out += ", at " + Format("vec3", matrix + 48);
    }
    out += '\n';
  }
  return out.empty() ? "no players (not in a level?)\n" : out;
}

std::string State() {
  const cheats::Snapshot snap = cheats::GetSnapshot();
  return fmt::format(
      "uptime: {} ms\nin level: {}{}\n{}\nlocal players (setting): {}\ncheats: speed {:.2f}, frozen {}, god {}, "
      "no AI {}, HUD {}\n",
      UptimeMs(), more_players::InPlay(), more_players::Loading() ? " (loading screen up)" : "",
      FrontEndText(), more_players::LocalPlayerCount(),
      cheats::GameSpeed(), cheats::Frozen(), cheats::GodMode(), cheats::NoAi(),
      cheats::HudHidden() ? "hidden" : "shown");
}

bool Compare(double a, std::string_view op, double b) {
  if (op == "==") return a == b;
  if (op == "!=") return a != b;
  if (op == "<") return a < b;
  if (op == "<=") return a <= b;
  if (op == ">") return a > b;
  if (op == ">=") return a >= b;
  throw Error{fmt::format("unknown comparison \"{}\"", op)};
}

std::string Wait(const std::vector<std::string_view>& w) {
  if (w.size() < 2) throw Error{"wait ms|state|exit|level|<expr> ..."};
  const auto timeout = [&](size_t index) {
    return w.size() > index ? Number(w[index], "timeout") : int64_t(30000);
  };
  if (w[1] == "ms" && w.size() == 3) {
    std::this_thread::sleep_for(std::chrono::milliseconds(Number(w[2], "ms")));
    return "";
  }
  if (w[1] == "state" && w.size() >= 3) {
    const int64_t state = Number(w[2], "state");
    return WaitFor(timeout(3), [&]() -> std::optional<std::string> {
      if (fight_tree::CurrentFrontEnd().state != state) return std::nullopt;
      return FrontEndText() + "\n";
    });
  }
  if (w[1] == "replay") {
    return WaitFor(timeout(2), [&]() -> std::optional<std::string> {
      if (input_record::ReplayRunning()) return std::nullopt;
      return std::string("replay done\n");
    });
  }
  if (w[1] == "exit") {
    const uint32_t before = fight_tree::CurrentFrontEnd().exits;
    return WaitFor(timeout(2), [&]() -> std::optional<std::string> {
      if (fight_tree::CurrentFrontEnd().exits == before) return std::nullopt;
      return FrontEndText() + "\n";
    });
  }
  if (w[1] == "level") {
    return WaitFor(timeout(2), [&]() -> std::optional<std::string> {
      // PLAYABLE: the front end's GameRunning state, faded in. InPlay() is
      // already true under the loading screen, and the picture stays black
      // until the front end takes InGame (486) -> ExitLoadingComplete ->
      // ExitGameRunning -> GameRunning (489); then it fades in over ~1.2 s:
      // front end +144 counts 1 -> 0 (and +148 then counts the time since).
      // Found with this console (2026-10-08): +144 read beside photos
      // (0.98 black, 0.49 dim, 0 = full picture). Pause menus leave 489.
      if (!more_players::InPlay() || fight_tree::CurrentFrontEnd().state != kGameRunning) {
        return std::nullopt;
      }
      try {
        const uint32_t front_end = ReadBE(ReadBE(0x8259B190, 4) + kFrontEnd, 4);
        if (ReadF32(front_end + kFadeIn) > 0.0f) return std::nullopt;
      } catch (const Error&) {
        return std::nullopt;
      }
      return fmt::format("playing (front end GameRunning, faded in) after {} ms uptime\n", UptimeMs());
    });
  }
  // wait <expr> <type> <op> <value> [ms]
  if (w.size() < 5) throw Error{"wait <expr> <type> <op> <value> [ms]"};
  const std::string_view expr = w[1], type = w[2], op = w[3];
  const double want = type == "f32" ? std::strtod(std::string(w[4]).c_str(), nullptr)
                                    : double(Eval(w[4]));
  return WaitFor(timeout(5), [&]() -> std::optional<std::string> {
    uint32_t address;
    double have;
    try {  // an unreadable address can become readable (a level loading)
      address = Eval(expr);
      have = type == "f32" ? ReadF32(address)
             : type == "s32" ? double(int32_t(ReadBE(address, 4)))
                             : double(ReadBE(address, int(TypeSize(type))));
    } catch (const Error&) {
      return std::nullopt;
    }
    if (!Compare(have, op, want)) return std::nullopt;
    return fmt::format("0x{:08X} = {}\n", address, Format(type, address));
  });
}

// snap / diff: "what changed?" Saves a memory range under a name; diff
// lists every 4-byte word that differs now (offset, old -> new, as hex and
// as a float when it looks like one). How the level's fade-in timer (front
// end +144) was found: snap during the fade, diff a moment later.
struct Snap {
  uint32_t address = 0;
  std::vector<uint8_t> bytes;
};
std::mutex g_snaps_mutex;
std::map<std::string, Snap, std::less<>> g_snaps;

std::vector<uint8_t> Copy(uint32_t address, uint32_t size) {
  const uint8_t* p = Guest(address, size);
  return std::vector<uint8_t>(p, p + size);
}

std::string SnapCommand(const std::vector<std::string_view>& w) {
  if (w.size() != 4) throw Error{"snap <name> <expr> <bytes>"};
  Snap snap;
  snap.address = Eval(w[2]);
  const int64_t size = Number(w[3], "bytes");
  if (size < 4 || size > (16 << 20)) throw Error{"bytes: 4 to 16 MiB"};
  snap.bytes = Copy(snap.address, uint32_t(size) & ~3u);
  std::lock_guard lock(g_snaps_mutex);
  g_snaps[std::string(w[1])] = std::move(snap);
  return fmt::format("saved {} bytes at 0x{:08X} as \"{}\"\n", size & ~3, Eval(w[2]), w[1]);
}

std::string FloatText(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  // Only "ordinary" floats (an int or pointer read as a float is usually
  // tiny or huge): 0, or 1e-4 .. 1e7 in size.
  const float a = std::fabs(f);
  return f == 0.0f || (a > 1e-4f && a < 1e7f) ? fmt::format(" ({:g})", f) : "";
}

std::string DiffCommand(const std::vector<std::string_view>& w) {
  if (w.size() < 2) throw Error{"diff <name> [update]"};
  std::lock_guard lock(g_snaps_mutex);
  auto it = g_snaps.find(w[1]);
  if (it == g_snaps.end()) throw Error{fmt::format("no snap \"{}\"", w[1])};
  Snap& snap = it->second;
  const std::vector<uint8_t> now = Copy(snap.address, uint32_t(snap.bytes.size()));
  std::string out;
  int changed = 0;
  for (size_t off = 0; off < now.size(); off += 4) {
    const auto word = [](const uint8_t* p) {
      return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    };
    const uint32_t a = word(&snap.bytes[off]), b = word(&now[off]);
    if (a == b) continue;
    if (++changed <= 400) {
      out += fmt::format("+{} (0x{:08X}): 0x{:08X}{} -> 0x{:08X}{}\n", off, snap.address + off, a,
                         FloatText(a), b, FloatText(b));
    }
  }
  out += fmt::format("{} of {} words changed{}\n", changed, now.size() / 4,
                     changed > 400 ? " (first 400 listed)" : "");
  if (w.size() > 2 && w[2] == "update") snap.bytes = now;  // the next diff compares to now
  return out;
}

// replay <file> [from_ms] [to_ms] / replay stop / replay (input_record.h).
std::string ReplayCommand(const std::vector<std::string_view>& w) {
  if (w.size() == 1) return input_record::ReplayStatus();
  if (w[1] == "stop") return input_record::StopReplay();
  const int64_t from = w.size() > 2 ? Number(w[2], "from_ms") : -1;
  const int64_t to = w.size() > 3 ? Number(w[3], "to_ms") : -1;
  try {
    return input_record::StartReplay(std::string(w[1]), from, to);
  } catch (const std::exception& e) {
    throw Error{e.what()};
  }
}

// watch <ms> <expr>:<type> [<expr>:<type> ...]: every value sampled ONCE
// PER GAME FRAME (a frame-end listener on the game's main thread: frames are
// complete there, nothing is mid-update) for <ms>; one line each time any of
// them changed: frame number, ms since the watch began, the values. E.g.
// "watch 3000 [t1+44]+52:f32 [uber+52]+144:f32" = the titan's height and the
// fade timer, frame by frame.
struct Watch {
  std::vector<std::string> exprs, types;
  std::vector<std::string> last;
  std::string out;
  int lines = 0;
  uint32_t frame = 0;
  Clock::time_point start = Clock::now();
  std::mutex mutex;
};

void WatchFrame(void* user) {
  Watch& w = *static_cast<Watch*>(user);
  std::lock_guard lock(w.mutex);
  ++w.frame;
  std::vector<std::string> values;
  for (size_t i = 0; i < w.exprs.size(); ++i) {
    try {
      values.push_back(Format(w.types[i], Eval(w.exprs[i])));
    } catch (const Error&) {
      values.push_back("?");  // unreadable right now
    }
  }
  if (values == w.last) return;
  w.last = values;
  if (++w.lines > 5000) return;  // the reply stays readable
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - w.start).count();
  w.out += fmt::format("frame {} +{} ms:", w.frame, ms);
  for (const std::string& v : values) w.out += "  " + v;
  w.out += '\n';
}

std::string WatchCommand(const std::vector<std::string_view>& words) {
  if (words.size() < 3) throw Error{"watch <ms> <expr>:<type> [<expr>:<type> ...]"};
  const int64_t ms = std::clamp<int64_t>(Number(words[1], "ms"), 1, 600000);
  auto watch = std::make_unique<Watch>();
  for (size_t i = 2; i < words.size(); ++i) {
    const size_t colon = words[i].rfind(':');
    if (colon == std::string_view::npos) throw Error{fmt::format("\"{}\": <expr>:<type>", words[i])};
    watch->exprs.emplace_back(words[i].substr(0, colon));
    watch->types.emplace_back(words[i].substr(colon + 1));
    Format(watch->types.back(), 0x82000000);  // an unknown type fails now, not per frame
  }
  pddi::AddFrameEndListener(&WatchFrame, watch.get());
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  pddi::RemoveFrameEndListener(&WatchFrame, watch.get());  // waits for a call in progress
  std::string out = std::move(watch->out);
  out += fmt::format("{} frames, {} changes{}\n", watch->frame, watch->lines,
                     watch->lines > 5000 ? " (first 5000 listed)" : "");
  return out;
}

// The body player p (1-4) moves around in: their titan if they ride one,
// else their Crash. Its world matrix (actor +44) holds the position in row 3
// (+48: x, y, z).
uint32_t Body(std::string_view player_word) {
  const int64_t p = Number(player_word, "player") - 1;
  if (p < 0 || p > 3) throw Error{"player: 1-4"};
  const uint32_t titan = more_players::TitanOfPlayer(int(p));
  const uint32_t body = titan ? titan : more_players::CharacterOfPlayer(int(p));
  if (!body) throw Error{fmt::format("player {} has no character", p + 1)};
  return body;
}
uint32_t BodyMatrix(std::string_view player_word) { return ReadBE(Body(player_word) + 44, 4); }

// A body's physics (CPhysicsBehaviour, findings/22) at actor +308, or 0. It
// keeps its OWN position (+140) and velocity (+104) and writes the position
// back into the matrix every frame: Crash on foot snapped right back from a
// matrix-only move. Found with this console (2026-10-08): every pointer in
// the actor followed, +308 -> vtable 0x82032FC4, whose +88 points back.
constexpr uint32_t kActorPhysics = 308, kPhysicsVtable = 0x82032FC4;
constexpr uint32_t kPhysicsActor = 88, kPhysicsVelocity = 104, kPhysicsPosition = 140;
uint32_t PhysicsOf(uint32_t body) {
  try {
    const uint32_t physics = ReadBE(body + kActorPhysics, 4);
    if (ReadBE(physics, 4) == kPhysicsVtable && ReadBE(physics + kPhysicsActor, 4) == body) {
      return physics;
    }
  } catch (const Error&) {
  }
  return 0;
}

// pos [p]: where player p (default 1) is. goto <p> <x> <y> <z>: put them
// there: the world matrix's position AND the physics' own (PhysicsOf), its
// velocity zeroed. Tested 2026-10-08: a titan and Crash on foot stay where
// put and settle on the ground. "~" keeps a coordinate, "~<n>" adds n to it (goto 1 ~ ~5 ~ =
// 5 units up).
std::string PosCommand(const std::vector<std::string_view>& w) {
  const uint32_t m = BodyMatrix(w.size() > 1 ? w[1] : "1");
  return Format("vec3", m + 48) + "\n";
}

std::string GotoCommand(const std::vector<std::string_view>& w) {
  if (w.size() != 5) throw Error{"goto <player> <x> <y> <z> (~ = keep, ~5 = +5)"};
  const uint32_t body = Body(w[1]);
  const uint32_t m = ReadBE(body + 44, 4);
  const uint32_t physics = PhysicsOf(body);
  float xyz[3];
  for (int i = 0; i < 3; ++i) {
    const std::string word(w[2 + i]);
    const float now = ReadF32(m + 48 + 4 * i);
    xyz[i] = word[0] == '~' ? now + (word.size() > 1 ? std::strtof(word.c_str() + 1, nullptr) : 0.0f)
                            : std::strtof(word.c_str(), nullptr);
    if (!std::isfinite(xyz[i])) throw Error{"not a number: " + word};
  }
  for (int i = 0; i < 3; ++i) {
    uint32_t bits;
    std::memcpy(&bits, &xyz[i], 4);
    WriteBE(m + 48 + 4 * i, 4, bits);
    if (physics) {
      WriteBE(physics + kPhysicsPosition + 4 * i, 4, bits);
      WriteBE(physics + kPhysicsVelocity + 4 * i, 4, 0);  // arrive standing still
    }
  }
  REXLOG_INFO("Debug console: player {} put at ({:.2f}, {:.2f}, {:.2f}){}", w[1], xyz[0], xyz[1],
              xyz[2], physics ? "" : " (no physics behaviour: matrix only)");
  return Format("vec3", m + 48) + "\n";
}

// photo: F10, then the saved files (photos/ or --photo_dir) once written.
std::string Photo() {
  const uint32_t before = native::ab_capture::LastPhoto().serial;
  if (!native::ab_capture::RequestPhotoLikeF10()) throw Error{"the renderer isn't up yet"};
  return WaitFor(5000, [&]() -> std::optional<std::string> {
    const auto last = native::ab_capture::LastPhoto();
    if (last.serial == before) return std::nullopt;
    return last.files + "\n";
  });
}

// fkey <Key>: the app's shortcut on that key (rex::ui::RegisterBind), run
// on the UI thread like a real press; replies once it has run.
std::string FKey(const std::vector<std::string_view>& w) {
  if (w.size() != 2) throw Error{"fkey <Key> (F1-F12, Backtick, Delete...)"};
  const rex::ui::VirtualKey key = rex::ui::ParseVirtualKey(w[1]);
  if (key == rex::ui::VirtualKey::kNone) throw Error{fmt::format("unknown key \"{}\"", w[1])};
  if (!g_post || !g_window) throw Error{"no window"};
  auto handled = std::make_shared<std::atomic<int>>(0);  // 0 pending, 1 ran, 2 no bind
  g_post([key, handled] {
    rex::ui::KeyEvent event(g_window, key, 0, false, false, false, false, false);
    *handled = rex::ui::ProcessKeyEvent(event) ? 1 : 2;
  });
  WaitFor(3000, [&]() -> std::optional<std::string> {
    return handled->load() ? std::optional<std::string>("") : std::nullopt;
  });
  if (handled->load() == 2) throw Error{fmt::format("no shortcut on {}", w[1])};
  return "";
}

const char kHelp[] =
    "state | players | frontend | eval <expr> | read <expr> [type] [count] |\n"
    "write <expr> <type> <value> | wait ms <N> | wait state <N> [ms] | wait exit [ms] |\n"
    "wait level [ms] | wait <expr> <type> <op> <value> [ms] | photo | fkey <Key> |\n"
    "snap <name> <expr> <bytes> | diff <name> [update] | replay <file> [from] [to] | replay stop |\n"
    "wait replay [ms] | watch <ms> <expr>:<type> ... | pos [p] | goto <p> <x> <y> <z> | <any FIFO input line>\n"
    "types: u8 u16 u32 s32 f32 vec3 str wstr bytes; names: uber game p1-p4 t1-t4; [x] = word at x\n";

// One command line -> its reply (throws Error).
std::string Run(std::string_view line) {
  const auto w = Words(line);
  if (w.empty()) return "";
  const std::string_view verb = w[0];
  if (verb == "help") return kHelp;
  if (verb == "state") return State();
  if (verb == "players") return Players();
  if (verb == "frontend") return FrontEndText() + "\n";
  if (verb == "eval" && w.size() >= 2) {
    const uint32_t v = Eval(line.substr(line.find("eval") + 4));
    return fmt::format("0x{:08X} ({})\n", v, v);
  }
  if (verb == "read") return Read(w);
  if (verb == "write") return Write(w);
  if (verb == "wait") return Wait(w);
  if (verb == "photo") return Photo();
  if (verb == "replay") return ReplayCommand(w);
  if (verb == "watch") return WatchCommand(w);
  if (verb == "pos") return PosCommand(w);
  if (verb == "goto") return GotoCommand(w);
  if (verb == "snap") return SnapCommand(w);
  if (verb == "diff") return DiffCommand(w);
  if (verb == "fkey") return FKey(w);
  // Everything else: an input line (buttons, keys, cheats).
  ScriptedInputDriver* driver = ScriptedInputDriver::Live();
  if (!driver) throw Error{"no input driver yet"};
  if (!driver->HandleLine(std::string(line))) throw Error{"not understood (see `help`)"};
  return "";
}

// -----------------------------------------------------------------------------
// 4. The socket (Linux)
// -----------------------------------------------------------------------------

#if defined(__unix__)
bool SendAll(int fd, std::string_view text) {
  while (!text.empty()) {
    const ssize_t n = send(fd, text.data(), text.size(), MSG_NOSIGNAL);
    if (n <= 0) return false;
    text.remove_prefix(size_t(n));
  }
  return true;
}

void Serve(int fd) {
  std::string pending;
  char buf[512];
  for (;;) {
    const ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) break;
    pending.append(buf, size_t(n));
    size_t eol;
    while ((eol = pending.find('\n')) != std::string::npos) {
      std::string line = pending.substr(0, eol);
      pending.erase(0, eol + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      std::string reply;
      try {
        reply = Run(line) + "@end ok\n";
      } catch (const Error& e) {
        reply = "@end error " + e.what + "\n";
      }
      if (!SendAll(fd, reply)) {
        close(fd);
        return;
      }
    }
  }
  close(fd);
}

void Listen(std::string path) {
  const int server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (server < 0 || path.size() >= sizeof(address.sun_path)) {
    REXLOG_ERROR("Debug console: can't use {} (path too long, or no socket)", path);
    return;
  }
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  unlink(path.c_str());  // a stale socket from an earlier run
  if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      listen(server, 8) != 0) {
    REXLOG_ERROR("Debug console: can't listen on {}: {}", path, std::strerror(errno));
    close(server);
    return;
  }
  REXLOG_INFO("Debug console: listening on {} (client: tools/mom.py)", path);
  for (;;) {
    const int client = accept4(server, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) {
      if (errno == EINTR) continue;
      REXLOG_ERROR("Debug console: accept failed: {}", std::strerror(errno));
      break;
    }
    // One thread per connection (a `wait` may take a while). Detached: the
    // process ends with _Exit anyway (ReXApp::OnClosing).
    std::thread(Serve, client).detach();
  }
  close(server);
}
#endif

}  // namespace

void Start(UiPoster run_on_ui_thread, rex::ui::Window* window) {
  const std::string& path = REXCVAR_GET(debug_console);
  if (path.empty()) return;
  g_post = std::move(run_on_ui_thread);
  g_window = window;
#if defined(__unix__)
  std::thread(Listen, path).detach();
#else
  REXLOG_ERROR("Debug console: only supported on Linux ({} ignored)", path);
#endif
}

}  // namespace debug_console
