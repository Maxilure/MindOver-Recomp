// =============================================================================
// system_popups.h -- the last Xbox pop-ups + the network start-up, answered by
// the port (system_popups.cpp has the whole story; docs/findings/32 s.8)
// =============================================================================
#pragma once

#include <functional>

namespace system_popups {

// At setup (CrashMomApp): how to run work on the UI thread. The PC's "game
// files unreadable" box must open there: SDL's message box called from the
// game thread never returned (it hung, 2026-11-13 test), the UI thread owns
// SDL's windows and event loop.
void Install(std::function<void(std::function<void()>)> run_on_ui_thread);

}  // namespace system_popups
