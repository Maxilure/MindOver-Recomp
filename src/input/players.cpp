// =============================================================================
// input/players.cpp -- see players.h
// =============================================================================
#include "players.h"

#include <atomic>

#include <rex/logging.h>
#include <rex/system/xam/user_profile.h>

using rex::input::DeviceId;
using rex::input::DeviceInfo;

namespace kbm {
namespace {

std::atomic<PlayerAssignment*> g_assignment{nullptr};

const char* PlayerText(int player) {
  static const char* kText[] = {"player 1", "player 2", "player 3", "player 4"};
  return player >= 0 && player < 4 ? kText[player] : "nobody (off)";
}

}  // namespace

PlayerAssignment::PlayerAssignment(DeviceId keyboard_id, std::map<std::string, int> choices)
    : keyboard_id_(keyboard_id), choices_(std::move(choices)) {
  g_assignment.store(this);
  // Players 2-4 count as signed in (sharing player 1's profile) while a
  // device plays as them (SDK patch 0011): without that, the game refuses
  // player 2 ("You do not have an active gamer profile").
  rex::system::xam::SetLocalPlayerPresentCallback(
      [this](uint32_t user_index) { return HasDeviceFor(int(user_index)); });
}

PlayerAssignment::~PlayerAssignment() {
  rex::system::xam::SetLocalPlayerPresentCallback(nullptr);
  g_assignment.store(nullptr);
}

bool PlayerAssignment::HasDeviceFor(int player) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return PresentMask() & (1u << player);
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
  // Outside the lock: the broadcast asks HasDeviceFor again.
  REXLOG_INFO("Players: players with a device (signed in with the one profile): {}{}{}{}",
              mask & 1 ? "1 " : "", mask & 2 ? "2 " : "", mask & 4 ? "3 " : "",
              mask & 8 ? "4" : "");
  rex::system::xam::NotifyLocalPlayersChanged();
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
        if (!d.key.empty() && d.key != key && PlayerOf(d) == choice) displaced.push_back(&d);
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

}  // namespace kbm
