// =============================================================================
// input/players.cpp -- see players.h
// =============================================================================
#include "players.h"

#include <atomic>

#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#include "../players/who_plays.h"

using rex::input::DeviceId;
using rex::input::DeviceInfo;

namespace kbm {
namespace {

std::atomic<PlayerAssignment*> g_assignment{nullptr};

// ProbeState's socket: past the guest's four (the SDK only keeps "used last"
// for sockets 0-3), answered with g_probe alone while a probe runs on this
// thread.
constexpr uint32_t kProbeUser = rex::input::kMaxGuestUsers;
thread_local DeviceId g_probe = DeviceId::kInvalid;

const char* PlayerText(int player) {
  static const char* kText[] = {"player 1", "player 2", "player 3", "player 4"};
  return player >= 0 && player < 4 ? kText[player] : "nobody (off)";
}

}  // namespace

PlayerAssignment::PlayerAssignment(DeviceId keyboard_id, std::map<std::string, int> choices)
    : keyboard_id_(keyboard_id), choices_(std::move(choices)) {
  // Players 2-4 play while a device plays as them: the game's "who plays"
  // fields are filled from HasDeviceFor (players/who_plays.h). On the 360
  // that was a profile sign-in per controller; without a player-2 answer the
  // game refuses player 2 ("You do not have an active gamer profile").
  g_assignment.store(this);
}

PlayerAssignment::~PlayerAssignment() {
  g_assignment.store(nullptr);
}

bool PlayerAssignment::HasDeviceFor(int player) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return PresentMask() & (1u << player);
}

std::string PlayerAssignment::PadKeyFor(uint32_t user) const {
  std::lock_guard<std::mutex> lock(mutex_);
  // Same rule as DevicesForUser: outside play every controller answers as
  // player 1 (any of them works the menus), so the first one counts as
  // player 1's there. IN PLAY only player 1's own controller counts: with
  // player 1 on the keyboard and a PlayStation pad on player 2, player 1's
  // "Automatic" must not borrow player 2's PlayStation buttons (a 2026-11-04
  // change did, and the Options -> Controller Icons preview then showed
  // another player's controller). No controller of their own = "" = the
  // game's own 360 buttons.
  const bool everyone_is_player_one = user == 0 && in_play_ && !in_play_();
  for (const Known& d : devices_) {
    // Real controllers ("pad:<guid>") and --debug_fake_pads' ("debug-pad-N":
    // prompt_icons gives those the styles of --debug_fake_pad_styles).
    if (d.keyboard || (d.key.rfind("pad:", 0) != 0 && d.key.rfind("debug-pad-", 0) != 0)) continue;
    const int player = PlayerOf(d);
    if (player == int(user) || (everyone_is_player_one && player >= 0)) return d.key;
  }
  return {};
}

bool PlayerAssignment::IsConnected(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const Known& d : devices_) {
    if (d.key == key) return true;
  }
  return false;
}

bool PlayerAssignment::KeyboardAnswersFor(uint32_t user) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const bool everyone_is_player_one = user == 0 && in_play_ && !in_play_();
  for (const Known& d : devices_) {
    if (!d.keyboard) continue;
    const int player = PlayerOf(d);
    return player == int(user) || (everyone_is_player_one && player >= 0);
  }
  return false;
}

uint32_t PlayerAssignment::PresentMask() const {
  uint32_t mask = 0;
  for (const Known& d : devices_) {
    // A real device (keyboard or pad), not the SDK's "None" stand-in.
    if (!d.key.empty()) {
      const int player = PlayerOf(d);
      if (player >= 0) mask |= 1u << player;
    }
  }
  return mask;
}

void PlayerAssignment::AnnounceIfChanged() {
  uint32_t mask;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    mask = PresentMask() | 1u;  // player 1 is always signed in (the profile itself)
    if (mask == announced_mask_) {
      return;
    }
    announced_mask_ = mask;
  }
  // Outside the lock: the game's notification step asks HasDeviceFor again.
  REXLOG_INFO("Players: players with a device (signed in with the one profile): {}{}{}{}",
              mask & 1 ? "1 " : "", mask & 2 ? "2 " : "", mask & 4 ? "3 " : "",
              mask & 8 ? "4" : "");
  who_plays::PlayersChanged();
}

PlayerAssignment* PlayerAssignment::Get() {
  return g_assignment.load();
}

void PlayerAssignment::OnDevicesChanged(const std::vector<DeviceInfo>& devices) {
  UpdateDevices(devices);
  AnnounceIfChanged();
}

void PlayerAssignment::UpdateDevices(const std::vector<DeviceInfo>& devices) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> previous;  // who was connected before this change
  for (const Known& d : devices_) previous.push_back(d.key);
  devices_.clear();
  std::map<std::string, int> seen_guids;  // for ":2" on a second identical pad
  for (const DeviceInfo& info : devices) {
    Known d{info.id, "", info.name, info.synthetic, info.id == keyboard_id_, info.ordinal};
    if (d.keyboard) {
      d.key = "keyboard";
    } else if (info.guid.rfind("debug-pad-", 0) == 0) {
      d.key = info.guid;  // --debug_fake_pads' extra fake controllers (debug_input_script.h)
    } else if (!d.synthetic) {
      const int n = ++seen_guids[info.guid];
      d.key = "pad:" + info.guid + (n > 1 ? ":" + std::to_string(n) : "");
      if (d.name.empty()) d.name = "Controller";
    }
    devices_.push_back(std::move(d));
  }
  // SAFETY NET, one controller per player (2026-11-05): two connected
  // controllers on one player (a controls.toml from before the SetChoice
  // rule, or a choice meeting the automatic "Nth connected = player N") ->
  // the one connected before keeps it, the newcomer plays as nobody (in
  // memory: a button press then joins it, which writes the file). The
  // keyboard doesn't count (player 1 = the keyboard plus one controller).
  for (size_t i = 0; i < devices_.size(); ++i) {
    Known& d = devices_[i];
    if (d.key.empty() || d.keyboard) continue;
    const int player = PlayerOf(d);
    if (player < 0) continue;
    for (size_t j = 0; j < devices_.size(); ++j) {
      const Known& e = devices_[j];
      if (j == i || e.key.empty() || e.keyboard || PlayerOf(e) != player) continue;
      // Who was here first: connected before this change (then the lower ordinal).
      bool d_before = false, e_before = false;
      for (const std::string& k : previous) {
        d_before |= k == d.key;
        e_before |= k == e.key;
      }
      const bool d_loses = (e_before && !d_before) || (e_before == d_before && j < i);
      if (!d_loses) continue;
      choices_[d.key] = kPlayerOff;
      REXLOG_INFO("Players: {} and {} were both {}: {} keeps it, {} = nobody (press a button to join)",
                  e.name, d.name, PlayerText(player), e.name, d.name);
      break;
    }
  }
  // The log says who plays as whom, every time a device comes or goes.
  for (const Known& d : devices_) {
    if (!d.key.empty()) {
      REXLOG_INFO("Players: {} = {}{}", d.name, PlayerText(PlayerOf(d)),
                  choices_.count(d.key) ? "" : " (automatic)");
    }
  }
}

int PlayerAssignment::PlayerOf(const Known& d) const {
  const bool fake_pad = d.key.rfind("debug-pad-", 0) == 0;
  if (d.synthetic && !d.keyboard && !fake_pad) {
    return 0;  // the SDK's "None" stand-in, the debug script: player 1
  }
  auto it = choices_.find(d.key);
  int choice = it != choices_.end() ? it->second : kPlayerAuto;
  if (d.keyboard) {
    // THE KEYBOARD AND MOUSE ARE ALWAYS PLAYER 1 (2026-11-08). Every
    // keyboard and mouse on the PC is this ONE device (SDL merges them), so a
    // choice of player 2-4 (an old controls.toml line, or the Controls
    // window's old player list) left player 1 with no keys while player 2
    // played on them: seen on a stream (Players screen: player 1 = a pad
    // only, player 2 = "Keyboard and Mouse"). Only "off" is kept, for test
    // runs that must not hear the keyboard (bindings.cpp drops 2-4 too).
    return choice == kPlayerOff ? kPlayerOff : 0;
  }
  if (fake_pad && choice == kPlayerAuto) {
    return d.key.back() - '1';  // "debug-pad-3" = player 3
  }
  if (choice == kPlayerAuto) {
    // Keyboard: player 1. Controllers: the SDK's rule, Nth connected = player N.
    return d.keyboard ? 0 : (d.ordinal < rex::input::kMaxGuestUsers ? int(d.ordinal) : kPlayerOff);
  }
  return choice;
}

void PlayerAssignment::SetInPlayCheck(std::function<bool()> in_play) {
  std::lock_guard<std::mutex> lock(mutex_);
  in_play_ = std::move(in_play);
}

void PlayerAssignment::DevicesForUser(uint32_t user_index, std::vector<DeviceId>& out) const {
  out.clear();
  if (user_index == kProbeUser) {  // ProbeState's socket: only the probed device
    if (g_probe != DeviceId::kInvalid) out.push_back(g_probe);
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  // Outside play every real device also answers on player 1's socket: the
  // title takes START from any socket but makes that socket player 1, and the
  // main menu then listens to player 1 only. With this, whatever pressed
  // START, player 1 is socket 1 and every device can work the menus.
  const bool everyone_is_player_one = user_index == 0 && in_play_ && !in_play_();
  for (const Known& d : devices_) {
    const int player = PlayerOf(d);
    if (player == int(user_index) || (everyone_is_player_one && player >= 0 && !d.key.empty())) {
      out.push_back(d.id);
    }
  }
}

std::vector<PlayerAssignment::Device> PlayerAssignment::Devices() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<Device> out;
  for (int pass = 0; pass < 2; ++pass) {  // keyboard first, then the pads
    for (const Known& d : devices_) {
      if (d.key.empty() || d.keyboard != (pass == 0)) {
        continue;
      }
      Device dev;
      dev.key = d.key;
      dev.name = d.name;
      dev.keyboard = d.keyboard;
      auto it = choices_.find(d.key);
      dev.choice = it != choices_.end() ? it->second : kPlayerAuto;
      dev.player = PlayerOf(d);
      out.push_back(std::move(dev));
    }
  }
  return out;
}

std::vector<PlayerAssignment::Change> PlayerAssignment::SetChoice(const std::string& key,
                                                                  int choice) {
  std::vector<Change> changes;
  // The keyboard and mouse are player 1's or off, never players 2-4 (PlayerOf).
  if (key == "keyboard" && choice != kPlayerOff && choice != kPlayerAuto) choice = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // What the moved device plays as BEFORE the change (its old player).
    int old_player = kPlayerOff;
    std::string name = key;
    for (const Known& d : devices_) {
      if (d.key == key) {
        old_player = PlayerOf(d);
        name = d.name;
      }
    }
    // Who is on the target player now (connected devices other than this one).
    std::vector<const Known*> displaced;
    if (choice >= 0 && old_player >= 0 && old_player != choice) {
      for (const Known& d : devices_) {
        // (Never the keyboard: it stays player 1's, players.h SetChoice.)
        if (!d.key.empty() && d.key != key && !d.keyboard && PlayerOf(d) == choice) displaced.push_back(&d);
      }
    }
    if (choice == kPlayerAuto) {
      choices_.erase(key);
    } else {
      choices_[key] = choice;
    }
    changes.push_back({key, name, choice});
    // The swap: they take the moved device's old player, so no player is
    // left without its device and none ends up with two by accident.
    for (const Known* d : displaced) {
      choices_[d->key] = old_player;
      changes.push_back({d->key, d->name, old_player});
    }
    // ONE CONTROLLER PER PLAYER, unplugged ones included (2026-11-05,
    // playtest): a controller remembered for this player but not connected
    // now kept that choice, so when it came back the player had TWO (the
    // new one given the player meanwhile, and the old one). It plays as
    // nobody now: back, a button press joins it like any new controller.
    // (Not the keyboard: player 1 = the keyboard plus one controller.)
    const bool moved_keyboard = key == "keyboard";
    if (choice >= 0 && !moved_keyboard) {
      for (auto& [other, other_choice] : choices_) {
        if (other == key || other == "keyboard" || other_choice != choice) continue;
        bool connected = false;
        for (const Known& d : devices_) connected |= d.key == other;
        if (connected) continue;
        other_choice = kPlayerOff;
        changes.push_back({other, other, kPlayerOff});
        REXLOG_INFO("Players: {} (not connected) no longer = {}: {} took it", other, PlayerText(choice), name);
      }
    }
    for (const Known& d : devices_) {
      for (const Change& c : changes) {
        if (d.key == c.key) {
          REXLOG_INFO("Players: {} now = {}", d.name, PlayerText(PlayerOf(d)));
        }
      }
    }
  }
  AnnounceIfChanged();
  return changes;
}

bool PlayerAssignment::ProbeState(const std::string& key, rex::input::X_INPUT_STATE* out) const {
  DeviceId id = DeviceId::kInvalid;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Known& d : devices_) {
      if (d.key == key) id = d.id;
    }
  }
  auto* input = static_cast<rex::input::InputSystem*>(rex::Runtime::instance()->input_system());
  if (id == DeviceId::kInvalid || !input) return false;
  g_probe = id;
  rex::input::X_INPUT_STATE state{};
  const bool ok = input->GetState(kProbeUser, &state) == 0;  // X_ERROR_SUCCESS
  g_probe = DeviceId::kInvalid;
  if (ok && out) *out = state;
  return ok;
}

}  // namespace kbm
