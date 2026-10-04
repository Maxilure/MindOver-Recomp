// =============================================================================
// input/players.h -- which input device plays as which player (local co-op)
// =============================================================================
//
// WHAT: the game has drop-in co-op for two players (its pause menu offers
// "Join Game" / "Drop Out"; findings/23). The Xbox 360 game asks for each
// player's controller by number (XInputGetState for user 0, 1, 2, 3). This
// decides which of our devices answers for which number: e.g. keyboard and
// mouse = player 1, the controller = player 2. Chosen in the Controls menu's
// Players tab (F6), remembered in controls.toml (input/bindings.h).
//
// WHY OUR OWN: ReXGlue's InputSystem asks a "device assignment" object which
// devices feed each player. Its default (SlotAssignment) is fixed: every
// synthetic device (the keyboard) goes to player 1, real pads by connection
// order (first pad player 1 too, second pad player 2). So with one pad, the
// keyboard and the pad were always the same player.
//
// THE RULE, per device:
//   * Keyboard and mouse: the chosen player (default 1), or off.
//   * A controller: the chosen player, off, or Automatic = the SDK's rule
//     (the Nth controller connected = player N), the default.
//   * Other synthetic devices (the SDK's "None" stand-in that keeps player 1
//     connected, the debug input script): always player 1.
//   * --debug_fake_pads' extra fake controllers ("debug-pad-2".."-4",
//     debug_input_script.h): player 2-4 by default, or the chosen player.
// Several devices on one player are merged (buttons OR'ed, the bigger stick
// push wins), as before.
//
// Controllers are remembered by their SDL GUID (the model; it's the same
// for two identical pads, so the 2nd one connected gets ":2"), keyboard and
// mouse as "keyboard". A choice for a pad that isn't connected stays in the
// file for next time.
//
// SIGNED IN: a 360 game wants a gamer profile signed in on each player's
// controller; a PC has one profile. Players 2-4 count as signed in, sharing
// player 1's profile (same saves, settings, name), while a device plays as
// them: SDK patch 0011 asks HasDeviceFor, and a change of that set is
// announced to the game as "sign-in changed". Found 2026-10-02: player 2's
// Start on "Press START" gave "You do not have an active gamer profile".
//
// Threads: the input system calls in from the game's threads (every poll),
// the Controls menu from the UI thread; one mutex.
// =============================================================================
#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <rex/input/device_assignment.h>

namespace kbm {

// A device's choice: kAuto (controllers only), kOff, or a player 0-3.
constexpr int kPlayerAuto = -2;
constexpr int kPlayerOff = -1;

class PlayerAssignment final : public rex::input::DeviceAssignment {
 public:
  // `keyboard_id`: the keyboard driver's device. `choices`: device key ->
  // choice, from controls.toml (Bindings::players).
  PlayerAssignment(rex::input::DeviceId keyboard_id, std::map<std::string, int> choices);
  ~PlayerAssignment() override;
  // The one installed (null if none): for the Controls menu.
  static PlayerAssignment* Get();

  // DeviceAssignment (game threads).
  void OnDevicesChanged(const std::vector<rex::input::DeviceInfo>& devices) override;
  void DevicesForUser(uint32_t user_index, std::vector<rex::input::DeviceId>& out) const override;

  // For the menu: the devices connected now, keyboard first, then the
  // controllers in connection order.
  struct Device {
    std::string key;   // "keyboard", "pad:<guid>[:n]"
    std::string name;  // "Keyboard and mouse", SDL's controller name
    bool keyboard = false;
    int choice = kPlayerAuto;  // what's chosen (see above)
    int player = kPlayerOff;   // what it plays as right now (0-3 / kPlayerOff)
  };
  std::vector<Device> Devices() const;
  // Changes one device's choice (takes effect at the next poll).
  void SetChoice(const std::string& key, int choice);

 private:
  struct Known {
    rex::input::DeviceId id;
    std::string key, name;
    bool synthetic, keyboard;
    uint32_t ordinal;
  };
  // What `d` plays as with the current choices (mutex held).
  int PlayerOf(const Known& d) const;
  // Bit N = a real device (keyboard or pad) plays as player N (mutex held).
  uint32_t PresentMask() const;
  // For the SDK's sign-in answers (takes the lock).
  bool HasDeviceFor(int player) const;
  void UpdateDevices(const std::vector<rex::input::DeviceInfo>& devices);
  // Tells the game when the set of players with a device changed (a
  // "sign-in changed" notification). Without the lock held.
  void AnnounceIfChanged();

  const rex::input::DeviceId keyboard_id_;
  mutable std::mutex mutex_;
  std::vector<Known> devices_;     // connected, in connection order
  std::map<std::string, int> choices_;
  uint32_t announced_mask_ = 1;  // what the game was last told (boot: player 1)
};

}  // namespace kbm
