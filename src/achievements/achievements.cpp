// =============================================================================
// achievements/achievements.cpp -- see achievements.h
// =============================================================================
#include "achievements.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <chrono>
#include <cmath>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string_view>
#include <unordered_map>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/image_decode.h>
#include <rex/system/util/xdbf_utils.h>
#include <toml++/toml.hpp>

#include "../cheats/cheats.h"
#include "../game_folder.h"
#include "../guest_memory.h"
#include "../ui/menu_pictures.h"
#include "../ui/scrooby.h"


extern "C" REX_FUNC(__imp__sub_8226C5E8);  // the game's message bar: add a message (the game's)

REXCVAR_DEFINE_BOOL(achievement_popups, true, "CrashMoM",
                    "A notification at the bottom of the screen when an achievement is earned");
REXCVAR_DEFINE_BOOL(achievement_sound, true, "CrashMoM",
                    "The game's reward jingle with the achievement notification");

namespace achievements {
namespace {

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// The list (title data, through the SDK) and the record (our file)
// ---------------------------------------------------------------------------

std::mutex g_mutex;              // everything below (game thread, UI thread, console)
std::vector<Info> g_list;        // title data order; .unlocked filled from g_record
std::unordered_map<uint32_t, int64_t> g_record;  // id -> Unix seconds
bool g_record_loaded = false;

// The record's file: next to the saves (it IS progress, like them; test runs
// on a copy of the saves (--user_data_root) keep their own).
std::filesystem::path RecordPath() { return game_folder::SavesFolder() / "achievements.toml"; }
// The SDK's old record (before 2026-10-25): copied once, never written.
std::filesystem::path OldRecordPath() { return game_folder::SavesFolder() / "achievements" / "565507FA.toml"; }

// Windows FILETIME (100 ns since 1601) -> Unix seconds.
int64_t FromFileTime(int64_t ft) { return (ft - 116444736000000000LL) / 10000000LL; }

std::string IsoDate(int64_t unix_seconds) {
  const std::time_t t = std::time_t(unix_seconds);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char text[32];
  std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return text;
}

int64_t UnixFromToml(const toml::date_time& dt) {
  std::tm tm{};
  tm.tm_year = dt.date.year - 1900;
  tm.tm_mon = dt.date.month - 1;
  tm.tm_mday = dt.date.day;
  tm.tm_hour = dt.time.hour;
  tm.tm_min = dt.time.minute;
  tm.tm_sec = dt.time.second;
#if defined(_WIN32)
  int64_t t = _mkgmtime(&tm);
#else
  int64_t t = timegm(&tm);
#endif
  if (dt.offset) t -= int64_t(dt.offset->minutes) * 60;
  return t;
}

const Info* FindLocked(uint32_t id) {
  for (const Info& i : g_list) {
    if (i.id == id) return &i;
  }
  return nullptr;
}

// Written whole: temp file + rename (a crash mid-write leaves the old one).
void SaveLocked() {
  std::ostringstream out;
  out << "# Mind over Recomp: the achievements earned in Crash: Mind over Mutant.\n"
         "# One record for the whole game: every save counts, as on the Xbox 360.\n"
         "# Each line: achievement id = when it was earned (UTC). Removing a line locks it again.\n\n"
         "[unlocked]\n";
  std::vector<std::pair<uint32_t, int64_t>> sorted(g_record.begin(), g_record.end());
  std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
  for (const auto& [id, when] : sorted) {
    const Info* info = FindLocked(id);
    out << id << " = " << IsoDate(when);
    if (info) out << "  # " << info->name;
    out << "\n";
  }
  const std::filesystem::path path = RecordPath();
  std::filesystem::path temporary = path;
  temporary += ".tmp";
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  {
    std::ofstream f(temporary, std::ios::binary | std::ios::trunc);
    f << out.str();
    if (!f) {
      REXLOG_WARN("Achievements: can't write {}", temporary.string());
      return;
    }
  }
  std::filesystem::rename(temporary, path, ec);
  if (ec) REXLOG_WARN("Achievements: can't save {} ({})", path.string(), ec.message());
}

// The SDK's old record: [unlocked.<id>] filetime = N.
void ImportOldLocked() {
  const std::filesystem::path old = OldRecordPath();
  std::error_code ec;
  if (!std::filesystem::exists(old, ec)) return;
  try {
    const toml::table t = toml::parse_file(old.string());
    if (const toml::table* unlocked = t["unlocked"].as_table()) {
      for (const auto& [key, value] : *unlocked) {
        const toml::table* entry = value.as_table();
        const auto filetime = entry ? (*entry)["filetime"].value<int64_t>() : std::nullopt;
        const uint32_t id = uint32_t(std::strtoul(std::string(key.str()).c_str(), nullptr, 10));
        if (filetime && id) g_record.emplace(id, FromFileTime(*filetime));
      }
    }
    REXLOG_INFO("Achievements: copied {} earlier unlocks from {} (that file is kept as it is)", g_record.size(),
                old.string());
  } catch (const toml::parse_error& e) {
    REXLOG_WARN("Achievements: can't read the old record {} ({})", old.string(), e.what());
  }
}

void LoadRecordLocked() {
  if (g_record_loaded || game_folder::SavesFolder().empty()) return;
  g_record_loaded = true;
  const std::filesystem::path path = RecordPath();
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    ImportOldLocked();
    SaveLocked();  // from now on ours is the record (even an empty one)
    return;
  }
  try {
    const toml::table t = toml::parse_file(path.string());
    if (const toml::table* unlocked = t["unlocked"].as_table()) {
      for (const auto& [key, value] : *unlocked) {
        const uint32_t id = uint32_t(std::strtoul(std::string(key.str()).c_str(), nullptr, 10));
        int64_t when = 0;
        if (const auto dt = value.value<toml::date_time>()) when = UnixFromToml(*dt);
        else if (const auto n = value.value<int64_t>()) when = *n;
        if (id) g_record.emplace(id, when);
      }
    }
    REXLOG_INFO("Achievements: {} earned (record {})", g_record.size(), path.string());
  } catch (const toml::parse_error& e) {
    // Never overwritten while unreadable: a hand edit gone wrong keeps its
    // data; unlocks this session are kept in memory only.
    REXLOG_WARN("Achievements: can't read {} ({}): unlocks this session won't be saved", path.string(), e.what());
    g_record_loaded = false;
  }
}

// The list from the title data (the SDK reads it at start-up).
void LoadListLocked() {
  if (!g_list.empty()) return;
  rex::system::KernelState* ks = rex::system::kernel_state();
  if (!ks) return;
  for (const rex::system::AchievementInfo& a : ks->achievements().ListAchievements()) {
    Info i;
    i.id = a.id;
    i.name = a.label;
    i.description = a.description;
    i.locked_description = a.unachieved_description;
    i.gamerscore = a.gamerscore;
    i.image_id = a.image_id;
    g_list.push_back(std::move(i));
  }
  if (!g_list.empty()) {
    int score = 0;
    for (const Info& i : g_list) score += int(i.gamerscore);
    REXLOG_INFO("Achievements: {} in the title data, {} gamerscore in all", g_list.size(), score);
  }
}

void EnsureLocked() {
  LoadListLocked();
  LoadRecordLocked();
}

// ---------------------------------------------------------------------------
// THE NOTIFICATION = THE GAME'S OWN MESSAGE BAR (2026-10-25, findings/51 s.4)
// ---------------------------------------------------------------------------
// The game has a pop-up at the bottom of the screen: InGame_MessageBar
// (glass 600 x 180, a title, a message, a counter "2/5"), a queue of
// messages the level scripts add (voodoo doll pick-ups, objectives,
// "concept art unlocked"). An earned achievement becomes one more message
// in that queue, so it is drawn by the game (in photos, under the port's
// ImGui windows), slides like the game's own, waits its turn behind them
// and through cutscenes (the bar's own rules).
// The bar: an object inside the front end at +1428 (vtable 0x8203B128):
//   +4    bit 0x80 = set up (in play; adds are ignored otherwise)
//   +8    the queue: up to 5 entries of 20 bytes, its end pointer at +108;
//         entry +0 f32 seconds, +4 title key, +8 message key, +12 counter
//         value, +16 counter max
//   +128  its page, +132 title text, +136 message text, +140 counter text
//   add   sub_8226C5E8 (r3 bar, r5 flag byte, r6 message key, r7 title key,
//         r8 / r9 counter, f1 seconds): queues, shows the page, starts the
//         slide, plays FE_SFX "back"
//   Update sub_8226C2F8: every frame while a message is up, its title and
//         message from their KEYS (sub_82371C58: text from the bank by key;
//         title key 0 = no title), the counter shown when value >= 0 and
//         max > 0, the time counted down; then the next, or slide away.
// Ours: title key 1, message key 0, counter value -1 (hidden) and max =
// kMarker | id: that's how an entry is known to be ours. While ours is up it
// LOOKS LIKE THE PORT'S ACHIEVEMENT CARD, not the bar: the bar's page has
// our card's elements (options_page.cpp: AchPopGlass, AchBarIcon / Ring,
// AchPopTitle / Name), shown instead of the bar's own glass and texts
// (hidden every frame: the bar's Update shows them again each frame); the
// bar's by-key text calls on its title and message are skipped
// (sub_82371C58 wrapped below). The add's own "back" sound is muted for
// ours: the jingle plays when ours comes up.
constexpr uint32_t kGameGlobal = 0x8259B190;  // the game (front end at +52)
constexpr uint32_t kMessageBar = 1428;        // front end: the message bar object
constexpr uint32_t kBarFlags = 4, kBarQueue = 8, kBarQueueEnd = 108;
constexpr uint32_t kBarPage = 128, kBarTitle = 132, kBarMessage = 136, kBarCounter = 140;
constexpr uint32_t kEntrySize = 20, kEntryCounterMax = 16, kBarCapacity = 5;
constexpr uint32_t kMarker = 0xAC4E0000, kMarkerMask = 0xFFFF0000;
constexpr double kSeconds = 4.0;

std::deque<uint32_t> g_waiting;     // earned, not yet in the bar's queue
uint32_t g_showing_id = 0;          // the achievement the bar shows now (0 = none)
std::atomic<double> g_test_bar_seconds{0};  // console "achievement bar <s>"
// Our card's elements on the bar's page (found once per page), and the
// bar's own glass (hidden while ours shows).
struct Card {
  uint32_t glass = 0, icon = 0, ring = 0, title = 0, name = 0;
};
uint32_t g_icon_page = 0, g_bar_glass = 0;
Card g_card;

uint32_t Read32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = GuestPtr(base, a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// UTF-8 (the title data) -> UTF-16 (the game's texts).
std::u16string Utf16(std::string_view s) {
  std::u16string out;
  for (size_t i = 0; i < s.size();) {
    const uint8_t c = uint8_t(s[i]);
    uint32_t cp = c;
    int extra = 0;
    if (c >= 0xF0) cp = c & 0x07, extra = 3;
    else if (c >= 0xE0) cp = c & 0x0F, extra = 2;
    else if (c >= 0xC0) cp = c & 0x1F, extra = 1;
    ++i;
    for (int k = 0; k < extra && i < s.size(); ++k, ++i) cp = (cp << 6) | (uint8_t(s[i]) & 0x3F);
    if (cp < 0x10000) out.push_back(char16_t(cp));
  }
  return out;
}

uint32_t Bar(const uint8_t* base) {
  const uint32_t game = Read32(base, kGameGlobal);
  const uint32_t fe = game ? Read32(base, game + 52) : 0;
  return fe ? fe + kMessageBar : 0;
}

// The achievement of the bar's current message, 0 = none / not ours.
uint32_t OursUp(const uint8_t* base, uint32_t bar) {
  if (!bar || Read32(base, bar + kBarQueueEnd) == bar + kBarQueue) return 0;
  const uint32_t marker = Read32(base, bar + kBarQueue + kEntryCounterMax);
  return (marker & kMarkerMask) == kMarker ? marker & ~kMarkerMask : 0;
}

// The jingle: a cue of the menus' sound bank FE_SFX (loaded in menus and
// every level). Candidates (findings/51 s.4): objective1 stinger_reward,
// objective3 free_life, objective4 upgrade_reveal + voodoo_doll_reveal_02,
// objective5 stinger_unlock, multiplier mojo_collect_timer_01.
// Console "achievement cue <name>" tries another for the session.
// Picked by ear 2026-10-25 (an unmuted demo of the four): objective1.
std::string g_cue = "objective1";

}  // namespace

std::vector<Info> List() {
  std::lock_guard lock(g_mutex);
  EnsureLocked();
  std::vector<Info> out = g_list;
  for (Info& i : out) {
    const auto it = g_record.find(i.id);
    i.unlocked = it != g_record.end();
    i.unlocked_at = i.unlocked ? it->second : 0;
  }
  return out;
}

void Count(int* earned, int* total, int* score, int* max_score) {
  int e = 0, t = 0, s = 0, m = 0;
  for (const Info& i : List()) {
    ++t;
    m += int(i.gamerscore);
    if (i.unlocked) {
      ++e;
      s += int(i.gamerscore);
    }
  }
  if (earned) *earned = e;
  if (total) *total = t;
  if (score) *score = s;
  if (max_score) *max_score = m;
}

bool Unlock(uint32_t id, const char* why) {
  std::lock_guard lock(g_mutex);
  EnsureLocked();
  const Info* info = FindLocked(id);
  if (!info) {
    REXLOG_WARN("Achievements: unknown achievement {} ({})", id, why);
    return false;
  }
  if (g_record.count(id)) {
    REXLOG_INFO("Achievements: \"{}\" ({}) already earned ({})", info->name, id, why);
    return true;
  }
  g_record.emplace(id, int64_t(std::time(nullptr)));
  if (g_record_loaded) SaveLocked();
  if (REXCVAR_GET(achievement_popups)) g_waiting.push_back(id);
  REXLOG_INFO("Achievements: EARNED \"{}\" ({}, {}G) ({}); {} of {}", info->name, id, info->gamerscore, why,
              g_record.size(), g_list.size());
  return true;
}

void FrontEndFrame(PPCContext& ctx, uint8_t* base) {
  const uint32_t bar = Bar(base);
  if (!bar) return;
  auto add = [&](uint32_t r6, uint32_t r7, uint32_t r8, uint32_t r9, double seconds) {
    const PPCContext saved = ctx;
    ctx.r3.u64 = bar;
    ctx.r5.u64 = 0;
    ctx.r6.u64 = r6;
    ctx.r7.u64 = r7;
    ctx.r8.u64 = r8;
    ctx.r9.u64 = r9;
    ctx.f1.f64 = seconds;
    __imp__sub_8226C5E8(ctx, base);
    ctx = saved;
  };
  // Console "achievement bar <s>": a test message of the game's kind (no
  // words, counter 2/5).
  if (const double seconds = g_test_bar_seconds.exchange(0); seconds > 0) {
    add(Read32(base, 0x825A58E8), 0, 2, 5, seconds);
    REXLOG_INFO("Achievements: test message added to the game's bar ({} s)", seconds);
  }
  const bool set_up = *GuestPtr(base, bar + kBarFlags) & 0x80;
  // 1. An earned one joins the bar's queue once the bar is set up (in play)
  //    and has room.
  uint32_t next = 0;
  {
    std::lock_guard lock(g_mutex);
    if (!REXCVAR_GET(achievement_popups)) g_waiting.clear();
    const uint32_t queued = (Read32(base, bar + kBarQueueEnd) - (bar + kBarQueue)) / kEntrySize;
    if (set_up && !g_waiting.empty() && queued < kBarCapacity) {
      next = g_waiting.front();
      g_waiting.pop_front();
    }
  }
  if (next) {
    scrooby::SetSoundsMuted(true);  // the add's "back": the jingle comes when it shows
    add(0, 1, 0xFFFFFFFFu, kMarker | next, kSeconds);
    scrooby::SetSoundsMuted(false);
    REXLOG_INFO("Achievements: {} queued in the game's message bar", next);
  }
  // 2. Ours up: OUR CARD on the bar's page (options_page.cpp: AchPopGlass,
  //    AchBarIcon / Ring, AchPopTitle / Name) instead of the bar's own wide
  //    glass and texts; its words and picture once per achievement; the
  //    jingle.
  const uint32_t page = Read32(base, bar + kBarPage);
  if (page != g_icon_page) {
    g_icon_page = page;
    auto pic = [&](const char* n) { return page ? scrooby::FindPicture(ctx, base, page, n) : 0u; };
    auto text = [&](const char* n) { return page ? scrooby::FindText(ctx, base, page, n) : 0u; };
    g_card = {pic("AchPopGlass"), pic("AchBarIcon"), pic("AchBarRing"), text("AchPopTitle"), text("AchPopName")};
    g_bar_glass = pic("Glass");
  }
  const uint32_t id = OursUp(base, bar);
  const bool page_shown = page && scrooby::Visible(base, page);
  if (id && page_shown) {
    if (id != g_showing_id) {
      g_showing_id = id;
      std::u16string name;
      {
        std::lock_guard lock(g_mutex);
        if (const Info* info = FindLocked(id)) name = Utf16(info->name);
      }
      // Line 1: the heading, small; line 2: the name, smaller when it's long.
      // Both scaled around their box's centre: the box moves so the LEFT edge
      // stays (canvas units, the card is x 147-493, y 20-87).
      constexpr float kTextX = 214, kTextW = 268;
      auto place = [&](uint32_t t, float top, float scale) {
        const float box_x = kTextX - (1 - scale) * kTextW / 2;
        const float centre = top - scale * 18;  // lines hang from the top of a 36-unit box
        scrooby::SetBox(base, t, box_x, centre - 18, kTextW, 36);
        scrooby::SetScale(ctx, base, t, scale);
      };
      scrooby::SetText(ctx, base, g_card.title, u"Achievement Unlocked");
      place(g_card.title, 81, 0.6f);
      scrooby::SetText(ctx, base, g_card.name, name);
      const float w = scrooby::TextWidth(name, 1.0f);
      place(g_card.name, 63, w * 0.9f > kTextW && w > 0 ? kTextW / w : 0.9f);
      const int frame = SpriteFrame(id);
      if (g_card.icon && frame >= 0) scrooby::SetFrame(ctx, base, g_card.icon, uint32_t(frame));
      if (REXCVAR_GET(achievement_sound)) {
        std::string cue;
        {
          std::lock_guard lock(g_mutex);
          cue = g_cue;
        }
        scrooby::PlayCue(ctx, base, cue);
      }
      REXLOG_INFO("Achievements: the bar shows {}", id);
    }
  } else if (!page_shown || Read32(base, bar + kBarQueueEnd) != bar + kBarQueue) {
    // (After ours the bar slides away still showing our card: it goes with
    // it. Back to the bar's own look when the bar is gone or shows one of
    // the game's messages.)
    g_showing_id = 0;
  }
  // Every frame (the bar's Update shows its own texts again each frame):
  // our card while ours is up or sliding away, else the bar's own.
  const bool ours = g_showing_id != 0 && page_shown;
  for (const uint32_t e : {g_card.glass, g_card.title, g_card.name}) scrooby::SetVisible(base, e, ours);
  const bool picture = ours && SpriteFrame(g_showing_id) >= 0;
  scrooby::SetVisible(base, g_card.icon, picture);
  scrooby::SetVisible(base, g_card.ring, picture);
  if (ours) {
    for (const uint32_t e : {Read32(base, bar + kBarTitle), Read32(base, bar + kBarMessage),
                             Read32(base, bar + kBarCounter)}) {
      scrooby::SetVisible(base, e, false);
    }
  }
  scrooby::SetVisible(base, g_bar_glass, !ours);
}

// THE ACHIEVEMENTS SCREEN's sprites (achievements.h MenuSprites).
const std::vector<MenuSprite>& MenuSprites() {
  static std::mutex mutex;
  static std::vector<MenuSprite> sprites;
  std::lock_guard lock(mutex);
  if (!sprites.empty()) return sprites;
  rex::system::KernelState* ks = rex::system::kernel_state();
  if (!ks) return sprites;
  const auto db = ks->title_xdbf();
  if (!db.is_valid()) return sprites;
  std::vector<MenuSprite> made;
  for (const Info& a : List()) {
    const auto block = db.GetEntry(rex::system::util::XdbfSection::kImage, uint64_t(a.image_id));
    if (!block) continue;
    int w = 0, h = 0;
    const std::vector<uint8_t> rgba = rex::ui::DecodeImageRGBA(block.buffer, block.size, w, h);
    if (rgba.empty() || w <= 0 || h <= 0 || w > 256 || h > 256) continue;
    MenuSprite s;
    s.id = a.id;
    s.name = fmt::format("AchIcon{}.tga", a.id);
    s.chunk = menu_pictures::MakeSprite(s.name, uint32_t(w), uint32_t(h),
                                        menu_pictures::PaletteTga(uint32_t(w), uint32_t(h), rgba.data()));
    made.push_back(std::move(s));
  }
  if (made.empty()) return sprites;
  // The ring: a white square outline (5 of 64 pixels wide), see-through
  // inside. Drawn by the port (no such picture on the disc).
  constexpr int kSide = 64, kBorder = 5;
  std::vector<uint8_t> ring(size_t(kSide) * kSide * 4, 0);
  for (int y = 0; y < kSide; ++y) {
    for (int x = 0; x < kSide; ++x) {
      if (x < kBorder || y < kBorder || x >= kSide - kBorder || y >= kSide - kBorder) {
        std::memset(&ring[(size_t(y) * kSide + x) * 4], 255, 4);
      }
    }
  }
  MenuSprite r;
  r.name = "AchRing.tga";
  r.chunk = menu_pictures::MakeSprite(r.name, kSide, kSide, menu_pictures::PaletteTga(kSide, kSide, ring.data()));
  made.push_back(std::move(r));
  size_t bytes = 0;
  for (const MenuSprite& m : made) bytes += m.chunk.size();
  REXLOG_INFO("Achievements: {} pictures made into menu sprites ({} KB)", made.size() - 1, bytes / 1024);
  sprites = std::move(made);
  return sprites;
}

int SpriteFrame(uint32_t id) {
  int frame = 0;
  for (const MenuSprite& s : MenuSprites()) {
    if (!s.id) continue;
    if (s.id == id) return frame;
    ++frame;
  }
  return -1;
}

bool DebugCommand(std::string_view command) {
  auto number = [](std::string_view s) { return uint32_t(std::strtoul(std::string(s).c_str(), nullptr, 10)); };
  if (command == "list") {
    for (const Info& i : List()) {
      REXLOG_INFO("Achievements: {:3} {:3}G {} \"{}\" image {}", i.id, i.gamerscore,
                  i.unlocked ? IsoDate(i.unlocked_at) : std::string("locked"), i.name, i.image_id);
    }
    return true;
  }
  if (command.substr(0, 5) == "show ") {
    std::lock_guard lock(g_mutex);
    EnsureLocked();
    const uint32_t id = number(command.substr(5));
    if (!FindLocked(id)) return false;
    g_waiting.push_back(id);
    return true;
  }
  if (command.substr(0, 7) == "unlock ") return Unlock(number(command.substr(7)), "console");
  // "bar <seconds>": a test message in the game's own message bar (no text,
  // counter 2/5), added the next front-end frame through the same path as a
  // level script's (so it takes turns like a real one).
  if (command.substr(0, 4) == "cue ") {
    std::lock_guard lock(g_mutex);
    g_cue = std::string(command.substr(4));
    REXLOG_INFO("Achievements: jingle = FE_SFX cue \"{}\"", g_cue);
    return !g_cue.empty();
  }
  if (command.substr(0, 4) == "bar ") {
    g_test_bar_seconds = std::strtod(std::string(command.substr(4)).c_str(), nullptr);
    return g_test_bar_seconds > 0;
  }
  return false;
}

}  // namespace achievements

// =============================================================================
// THE GAME'S ACHIEVEMENT WRITER (sub_82292938), replaced (achievements.h)
// =============================================================================
// r3 = Radical's achievement manager. Its queue's end pointer at +168, 28
// bytes per entry, the writer takes the LAST one (end - 28), as the original
// does: +4 = the achievement id. The entry leaves the queue (end -= 28); the
// copy at +180 the original made for the Xbox call is left alone (nothing
// reads it afterwards: the manager only polls the overlapped at +136, which
// stays idle).
extern "C" REX_FUNC(sub_82292938) {
  const uint32_t manager = ctx.r3.u32;
  auto read32 = [&](uint32_t a) {
    const uint8_t* p = GuestPtr(base, a);
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  };
  const uint32_t end = read32(manager + 168);
  const uint32_t entry = end - 28;
  const uint32_t id = read32(entry + 4);
  uint8_t* p = GuestPtr(base, manager + 168);
  p[0] = uint8_t(entry >> 24), p[1] = uint8_t(entry >> 16), p[2] = uint8_t(entry >> 8), p[3] = uint8_t(entry);
  // After a progress cheat (cheats.h): not earned, as before.
  if (cheats::AchievementsBlocked()) {
    REXLOG_INFO("Achievements: {} not earned (cheats were used this session)", id);
    return;
  }
  achievements::Unlock(id, "the game");
}

// The bar's by-key text setter (sub_82371C58: a text's string from the
// bank by key): skipped for the bar's title and message while OUR entry is
// up (the bar's Update sets them every frame; ours stay).
extern "C" REX_FUNC(__imp__sub_82371C58);
extern "C" REX_FUNC(sub_82371C58) {
  using namespace achievements;
  const uint32_t bar = Bar(base);
  if (bar && OursUp(base, bar)) {
    const uint32_t text = ctx.r3.u32;
    if (text && (text == Read32(base, bar + kBarTitle) || text == Read32(base, bar + kBarMessage))) return;
  }
  __imp__sub_82371C58(ctx, base);
}
