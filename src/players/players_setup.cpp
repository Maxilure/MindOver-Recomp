// =============================================================================
// players/players_setup.cpp -- see players_setup.h
// =============================================================================
#include "players_setup.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>

#include <fmt/format.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/image_decode.h>

#include "../guest_memory.h"
#include "../input/bindings.h"
#include "../input/keyboard_mouse.h"
#include "../input/players.h"
#include "../input/prompt_icons.h"
#include "../options/options_menu.h"
#include "../options/options_settings.h"
#include "../ui/menu_pictures.h"
#include "lost_controller.h"
#include "more_players.h"


namespace players_setup {
namespace {

using kbm::PlayerAssignment;

constexpr uint32_t kGameGlobal = 0x8259B190;  // the game (+56 the controllers' holder)
constexpr uint32_t kCoOpState = 0x8259B11C;   // per player (4 entries, more_players.h): 0 = not joined

// Game main thread only (the screen's update).
std::map<std::string, bool> g_held;          // per controller: a button held at the last look
std::atomic<bool> g_open{false};
std::atomic<bool> g_open_next{false};

uint32_t Read32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = GuestPtr(base, a);
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

bool InGame(const uint8_t* base, int p) { return Read32(base, kCoOpState + 4 * uint32_t(p)) != 0; }

// The players a level makes room for (2-4).
int Room() { return std::clamp(more_players::LocalPlayerCount(), 1, kPlayers); }

// Is a controller ("pad:..." or a fake "debug-pad-2".."4")? NOT the debug
// tools' own input ("debug-pad-1", "Debug input script": the console /
// --debug_input_script / --debug_input_fifo press buttons through it, and it
// exists whenever one of those is on): it showed on player 1's card and in
// the chooser as if it were a controller (2026-10-29). It still plays as
// player 1; the screen just never lists it.
bool IsPad(const PlayerAssignment::Device& d) {
  if (d.key == "debug-pad-1") return false;
  return !d.keyboard && (d.key.rfind("pad:", 0) == 0 || d.key.rfind("debug-pad-", 0) == 0);
}

// Does a press on this controller join? (Plays as nobody, or as a player the
// levels have no room for.)
bool Unassigned(const PlayerAssignment::Device& d) { return d.player < 0 || d.player >= Room(); }

Picture PictureOfStyle(int style) {
  switch (style) {
    case 3: return kPicPs5;
    case 4: return kPicPs4;
    case 5: return kPicSwitch;
    default: return kPicXbox;  // 1 = the 360's own, 2 = Xbox Series
  }
}

// Remembers the moved devices in the keyboard driver's bindings (which
// hold controls.toml's contents) and writes controls.toml.
void Save(const std::vector<PlayerAssignment::Change>& changes) {
  auto* driver = kbm::KeyboardMouseDriver::Get();
  const PlayerAssignment* a = PlayerAssignment::Get();
  if (!driver || !a || changes.empty()) return;
  kbm::Bindings b = driver->bindings();
  for (const PlayerAssignment::Change& c : changes) {
    if (c.choice == kbm::kPlayerAuto) {
      b.players.erase(c.key);
    } else {
      b.players[c.key] = kbm::PlayerChoice{c.choice, c.name};
    }
  }
  driver->SetBindings(b);
  if (!kbm::SaveBindings(b, kbm::ControlsFilePath())) {
    REXLOG_WARN("Players: couldn't save the controls file (see above)");
  }
}

// Anyone in the game besides p?
bool SomeoneElseInGame(const uint8_t* base, int p) {
  for (int q = 0; q < kPlayers; ++q) {
    if (q != p && InGame(base, q)) return true;
  }
  return false;
}

// After a change in a level: a player in the game without a device drops out
// (the pause menu's Drop Out). (Getting a device doesn't join anyone: the
// player joins the game's own way, START / B in play.)
void Settle(PPCContext& ctx, uint8_t* base) {
  const PlayerAssignment* a = PlayerAssignment::Get();
  if (!a || !more_players::InPlay()) return;
  for (int p = 0; p < kPlayers; ++p) {
    if (InGame(base, p) && !a->HasDeviceFor(p)) lost_controller::DropOut(ctx, base, p);
  }
}

}  // namespace

const char* StyleName(int style) {
  static const char* kNames[] = {"Automatic", "Xbox 360", "Xbox Series", "PlayStation 5", "PlayStation 4", "Switch"};
  return style >= 0 && style < int(std::size(kNames)) ? kNames[style] : "?";
}

int PadIcons(int p) { return prompt_icons::PadIconsSetting(p); }

void SetPadIcons(int p, int style) {
  if (p < 0 || p >= kPlayers || style < 0 || style > 5) return;
  if (p == 0) {  // player 1's = the Options row Controller Icons (user/settings.toml)
    options_settings::SetFlagToSave("controller_icons", std::to_string(style));
    options_settings::SaveChanged();
  } else if (auto* driver = kbm::KeyboardMouseDriver::Get()) {  // players 2-4's: controls.toml
    kbm::Bindings b = driver->bindings();
    b.settings.pad_icons[size_t(p)] = style;
    driver->SetBindings(b);
    if (!kbm::SaveBindings(b, kbm::ControlsFilePath())) {
      REXLOG_WARN("Players: couldn't save the controls file (see above)");
    }
  }
  REXLOG_INFO("Players: player {}'s button pictures = {}", p + 1, StyleName(style));
}

std::array<Box, kPlayers> Boxes() {
  std::array<Box, kPlayers> boxes;
  const PlayerAssignment* a = PlayerAssignment::Get();
  if (!a) return boxes;
  rex::system::KernelState* ks = rex::system::kernel_state();
  const uint8_t* base = ks ? ks->memory()->virtual_membase() : nullptr;
  const bool in_play = more_players::InPlay() && base;
  for (const PlayerAssignment::Device& d : a->Devices()) {
    if (d.player < 0 || d.player >= kPlayers) continue;
    if (!d.keyboard && !IsPad(d)) continue;
    Box& box = boxes[size_t(d.player)];
    ShownDevice s;
    s.key = d.key;
    s.name = d.keyboard ? "Keyboard and Mouse" : d.name;
    s.keyboard = d.keyboard;
    if (!d.keyboard) {
      const int detected = prompt_icons::DetectedPadStyle(d.key);
      s.picture = PictureOfStyle(detected);
      if (!box.has_pad) box.detected_style = detected;
      box.has_pad = true;
    } else {
      s.picture = kPicKeyboard;
    }
    // The keyboard first.
    if (d.keyboard) {
      box.devices.insert(box.devices.begin(), std::move(s));
    } else {
      box.devices.push_back(std::move(s));
    }
  }
  for (int p = 0; p < kPlayers; ++p) {
    Box& box = boxes[size_t(p)];
    const bool has_device = !box.devices.empty();
    if (p >= Room() && !has_device) {
      box.state = State::kUnavailable;
    } else if (!has_device) {
      box.state = State::kEmpty;
    } else if (!in_play) {
      box.state = State::kMenus;
    } else if (InGame(base, p)) {
      box.state = State::kPlaying;
    } else {
      box.state = State::kNotPlaying;
    }
  }
  return boxes;
}

void OpenNext() {
  g_open_next = true;
  REXLOG_INFO("Players: the screen opens next");
}
bool TakeOpenNext() { return g_open_next.exchange(false); }
bool OpenNextPending() { return g_open_next.load(); }

void ForgetPresses();  // (below: the press indicator's first look)

void Opened() {
  g_open = true;
  g_held.clear();  // the first look at each controller only notes what's held
  ForgetPresses();
  REXLOG_INFO("Players: screen opened");
}

void Closed() {
  g_open = false;
  REXLOG_INFO("Players: screen closed");
}

int PollJoins(PPCContext& ctx, uint8_t* base, uint16_t* buttons) {
  PlayerAssignment* a = PlayerAssignment::Get();
  if (!a) return -1;
  for (const PlayerAssignment::Device& d : a->Devices()) {
    if (!IsPad(d) || !Unassigned(d)) {
      g_held.erase(d.key);
      continue;
    }
    rex::input::X_INPUT_STATE state{};
    if (!a->ProbeState(d.key, &state)) continue;
    const uint16_t held = uint16_t(state.gamepad.buttons);
    const bool down = held || state.gamepad.left_trigger > 64 || state.gamepad.right_trigger > 64;
    auto [it, first_look] = g_held.try_emplace(d.key, down);
    const bool was_down = it->second;
    it->second = down;
    if (first_look || was_down || !down) continue;
    // A new press: the first of players 2-4 (with room) that has no device.
    int target = -1;
    for (int p = 1; p < Room() && target < 0; ++p) {
      if (!a->HasDeviceFor(p)) target = p;
    }
    if (target < 0) {
      REXLOG_INFO("Players: {} pressed a button, but every player has a device", d.name);
      continue;
    }
    const auto changes = a->SetChoice(d.key, target);
    Save(changes);
    Settle(ctx, base);
    REXLOG_INFO("Players: {} joined as player {} (pressed a button{})", d.name, target + 1,
                g_open.load() ? " in the Players screen" : "");
    if (buttons) *buttons = held;
    return target;
  }
  return -1;
}

namespace {
std::map<std::string, bool> g_press_held;  // PollPresses: each controller held at the last call
int64_t g_press_key_down = -1;             // ... and the keyboard's last_key_down() then (-1 = first look)
}  // namespace

uint32_t PollPresses(const std::string& offered_key, int offered_card) {
  uint32_t cards = 0;
  if (const kbm::KeyboardMouseDriver* driver = kbm::KeyboardMouseDriver::Get()) {
    const int64_t down = driver->last_key_down();
    if (g_press_key_down >= 0 && down != g_press_key_down) cards |= 1u;  // player 1's keyboard
    g_press_key_down = down;
  }
  PlayerAssignment* a = PlayerAssignment::Get();
  if (!a) return cards;
  std::map<std::string, bool> seen;
  for (const PlayerAssignment::Device& d : a->Devices()) {
    if (!IsPad(d)) continue;
    rex::input::X_INPUT_STATE state{};
    if (!a->ProbeState(d.key, &state)) continue;
    const bool down = uint16_t(state.gamepad.buttons) || state.gamepad.left_trigger > 64 ||
                      state.gamepad.right_trigger > 64;
    const auto it = g_press_held.find(d.key);
    seen[d.key] = down;
    if (it == g_press_held.end() || it->second || !down) continue;  // first look / held / up
    if (!Unassigned(d) && d.player < kPlayers) cards |= 1u << d.player;
    if (offered_card >= 0 && offered_card < kPlayers && d.key == offered_key) cards |= 1u << offered_card;
  }
  g_press_held = std::move(seen);  // (unplugged ones forgotten)
  return cards;
}

void ForgetPresses() {
  g_press_held.clear();
  g_press_key_down = -1;
}

bool ScreenOpen() { return g_open.load(); }

void PollJoinsInPlay(PPCContext& ctx, uint8_t* base) {
  if (g_open.load()) return;  // the screen polls (and hides the press) itself
  uint16_t buttons = 0;
  const int joined = PollJoins(ctx, base, &buttons);
  if (joined >= 0) options_menu::HideHeldButtons(joined, buttons);
}

bool HasController(int p) {
  const PlayerAssignment* a = PlayerAssignment::Get();
  return !a || a->HasDeviceFor(p);  // (no assignment yet: as before, shown)
}

std::vector<Controller> Controllers() {
  std::vector<Controller> out;
  const PlayerAssignment* a = PlayerAssignment::Get();
  if (!a) return out;
  for (const PlayerAssignment::Device& d : a->Devices()) {
    if (!IsPad(d)) continue;
    const int detected = prompt_icons::DetectedPadStyle(d.key);
    out.push_back({d.key, d.name, PictureOfStyle(detected), Unassigned(d) ? -1 : d.player});
  }
  return out;
}

bool AssignController(PPCContext& ctx, uint8_t* base, int p, const std::string& key) {
  PlayerAssignment* a = PlayerAssignment::Get();
  if (!a || p < 0 || p >= Room()) return false;
  int from = -1;
  std::string name = key;
  std::vector<std::string> others;  // p's other controllers
  for (const PlayerAssignment::Device& d : a->Devices()) {
    if (!IsPad(d)) continue;
    if (d.key == key) {
      from = Unassigned(d) ? -1 : d.player;
      name = d.name;
    } else if (d.player == p) {
      others.push_back(d.key);
    }
  }
  if (from == p) return false;  // already theirs
  // The last player in the game keeps a device: taking theirs (player 1
  // always has the keyboard) would leave nobody.
  if (more_players::InPlay() && from > 0 && InGame(base, from) && !SomeoneElseInGame(base, from) &&
      others.empty()) {
    return false;
  }
  // SetChoice SWAPS: p's controllers go to the chosen one's old player.
  // A controller that played as nobody: p's others are set to nobody, so p
  // ends up with the chosen one alone.
  std::vector<PlayerAssignment::Change> all = a->SetChoice(key, p);
  if (from < 0) {
    for (const std::string& k : others) {
      for (const auto& c : a->SetChoice(k, kbm::kPlayerOff)) all.push_back(c);
    }
  }
  Save(all);
  Settle(ctx, base);
  REXLOG_INFO("Players: {} chosen for player {}{}", name, p + 1,
              from >= 0 ? fmt::format(" (was player {}'s)", from + 1) : std::string());
  return true;
}

bool RemoveControllers(PPCContext& ctx, uint8_t* base, int p) {
  PlayerAssignment* a = PlayerAssignment::Get();
  if (!a || p < 0 || p >= kPlayers) return false;
  std::vector<std::string> pads;
  bool keyboard = false;
  for (const PlayerAssignment::Device& d : a->Devices()) {
    if (d.player != p) continue;
    if (IsPad(d)) pads.push_back(d.key);
    keyboard |= d.keyboard;
  }
  if (pads.empty()) return false;
  // Without a device the player drops out of the game: not the last one in.
  if (!keyboard && more_players::InPlay() && InGame(base, p) && !SomeoneElseInGame(base, p)) return false;
  std::vector<PlayerAssignment::Change> all;
  for (const std::string& key : pads) {
    for (const auto& c : a->SetChoice(key, kbm::kPlayerOff)) all.push_back(c);
  }
  Save(all);
  Settle(ctx, base);
  REXLOG_INFO("Players: player {}'s controller removed (press any button on it to join again)", p + 1);
  return true;
}

// --- The pictures --------------------------------------------------------------
// Baked once by tools/make_player_pictures.py into
// assets/prompts/controllers/players/ (filling the outlines here at their
// 128-pixel size gave hard, uneven edges; the script works at 4x and
// averages down): <pad>_p<N>.png, empty.png, ring_selected.png,
// ring_p<N>.png, card_back.png, aura_p<N>.png. Plus Xelu's keyboard-mouse.png as it is.
namespace {

constexpr const char* kPadFiles[4] = {"xbox", "ps5", "ps4", "switch"};  // Picture kPicXbox..kPicSwitch

std::string PadName(int pad, int p) { return fmt::format("PlyPad_{}_{}.tga", kPadFiles[pad], p + 1); }
constexpr const char* kKeyboardName = "PlyKeyboard.tga";
constexpr const char* kEmptyName = "PlyPadEmpty.tga";
constexpr const char* kCardBackName = "PlyCardBack.tga";
std::string AuraFileName(int p) { return fmt::format("PlyAura_p{}.tga", p + 1); }
std::string RingName(int c) { return c == 0 ? std::string("PlyRing_selected.tga") : fmt::format("PlyRing_p{}.tga", c); }

// One PNG -> a menu sprite named `name`. False (logged) if it can't be read.
bool AddSprite(std::vector<Sprite>& out, const std::filesystem::path& path, std::string name) {
  std::ifstream file(path, std::ios::binary);
  const std::vector<uint8_t> png((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  int w = 0, h = 0;
  const std::vector<uint8_t> rgba =
      png.empty() ? std::vector<uint8_t>{} : rex::ui::DecodeImageRGBA(png.data(), png.size(), w, h);
  if (rgba.empty() || w <= 0 || h <= 0 || w > 256 || h > 256) {
    REXLOG_WARN("Players: can't read {}: no Players screen", path.string());
    return false;
  }
  Sprite s;
  s.name = std::move(name);
  s.chunk = menu_pictures::MakeSprite(s.name, uint32_t(w), uint32_t(h),
                                      menu_pictures::PaletteTga(uint32_t(w), uint32_t(h), rgba.data()));
  out.push_back(std::move(s));
  return true;
}

}  // namespace

const std::vector<Sprite>& MenuSprites() {
  static std::mutex mutex;
  static std::vector<Sprite> sprites;
  static bool tried = false;
  std::lock_guard lock(mutex);
  if (tried) return sprites;
  tried = true;
  const auto folder = rex::filesystem::GetExecutableFolder() / "assets" / "prompts" / "controllers";
  const auto baked = folder / "players";
  std::vector<Sprite> made;
  bool ok = AddSprite(made, folder / "keyboard-mouse.png", kKeyboardName);
  for (int p = 0; p < kPlayers && ok; ++p) {
    for (int i = 0; i < 4 && ok; ++i) {
      ok = AddSprite(made, baked / fmt::format("{}_p{}.png", kPadFiles[i], p + 1), PadName(i, p));
    }
  }
  ok = ok && AddSprite(made, baked / "empty.png", kEmptyName);
  for (int c = 0; c <= kPlayers && ok; ++c) {
    ok = AddSprite(made, baked / (c == 0 ? std::string("ring_selected.png") : fmt::format("ring_p{}.png", c)),
                   RingName(c));
  }
  ok = ok && AddSprite(made, baked / "card_back.png", kCardBackName);
  for (int p = 0; p < kPlayers && ok; ++p) {
    ok = AddSprite(made, baked / fmt::format("aura_p{}.png", p + 1), AuraFileName(p));
  }
  if (!ok) return sprites;
  size_t bytes = 0;
  for (const Sprite& m : made) bytes += m.chunk.size();
  REXLOG_INFO("Players: {} pictures made into menu sprites ({} KB)", made.size(), bytes / 1024);
  sprites = std::move(made);
  return sprites;
}

std::vector<std::string> PictureFrames(int p) {
  std::vector<std::string> f = {kKeyboardName};
  for (int i = 0; i < 4; ++i) f.push_back(PadName(i, p));
  f.push_back(kEmptyName);
  return f;
}

std::vector<std::string> RingFrames() {
  std::vector<std::string> f;
  for (int c = 0; c <= kPlayers; ++c) f.push_back(RingName(c));
  return f;
}

std::string CardBackName() { return kCardBackName; }
std::string AuraName(int p) { return AuraFileName(p); }

bool DebugCommand(std::string_view command) {
  if (!command.empty() && command != "list") return false;
  const auto boxes = Boxes();
  for (int p = 0; p < kPlayers; ++p) {
    const Box& b = boxes[size_t(p)];
    static const char* kStates[] = {"menus", "playing", "joining", "not playing", "empty", "unavailable"};
    std::string devices;
    for (const ShownDevice& d : b.devices) devices += (devices.empty() ? "" : ", ") + d.name;
    REXLOG_INFO("Players: player {} [{}] {}{}", p + 1, kStates[int(b.state)],
                devices.empty() ? std::string("(no device)") : devices,
                b.has_pad ? fmt::format(" | controller looks like {}", StyleName(b.detected_style)) : "");
  }
  return true;
}

}  // namespace players_setup
