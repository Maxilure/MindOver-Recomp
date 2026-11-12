// Port-owned controller polling, assignment and vibration. The guest
// wrappers use the existing Xbox-sized records at the boundary (findings/38).
#pragma once

#include <functional>
#include <memory>

#include <rex/input/input_driver.h>

namespace kbm { class PlayerAssignment; }

namespace native_controllers {

bool Enabled();
void Start(std::unique_ptr<kbm::PlayerAssignment> assignment,
           std::unique_ptr<rex::input::InputDriver> keyboard,
           std::unique_ptr<rex::input::InputDriver> scripted);
void Stop();
void SetActiveCheck(std::function<bool()> check);
// Stops every controller's motors now (the Vibration setting turned to Off).
void StopRumble();
rex::X_RESULT Read(uint32_t user, rex::input::X_INPUT_STATE* state);
bool ReadDevice(rex::input::DeviceId id, rex::input::X_INPUT_STATE* state);

// Validates the writable guest range before a wrapper or filter touches it.
bool Writable(uint32_t address, uint32_t size);

}  // namespace native_controllers
