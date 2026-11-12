// =============================================================================
// native/quit_launch.cpp -- PC implementations of the game's title lifecycle
// imports
// =============================================================================
//
// WHAT: Replaces four imported Xbox lifecycle services with host process
//       behavior. The generated functions call the `__imp__...` symbols
//       directly; defining those symbols here makes the game use these
//       implementations instead of the SDK's Xbox-facing ones.
//
// HOW: xexdis on the player's `default.xex` found the only launch-title caller
//      in `sub_82300DB0`: it passes a null title path, which means “return to
//      the dashboard” on the console. On PC that request closes the process.
//      `XamLoaderTerminateTitle` and `HalReturnToFirmware` also end the
//      process. `ExRegisterTitleTerminateNotification` only stores guest
//      callbacks in the SDK; the current runtime never invokes them, and
//      process exit lets the OS reclaim their resources. The PC implementation
//      therefore keeps no Xbox callback registry and does not follow guest
//      pointers.
//
// CALL SITES: `xstart` (0x82300BA0), `sub_82300DB0`, `sub_82301590`,
//             `sub_82311A20`, `sub_82311A80`, and the audio library's
//             `sub_8247E760` / `sub_8247F2D8` / `sub_8247FB28`.
//             Findings/58 records the disassembly and runtime checks.
// =============================================================================

#include <cstdlib>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

namespace {

// Exit the PC process as the port's existing “Quit Game” and window-close
// paths do. Flushing first preserves the lifecycle log before `_Exit` skips
// static destruction; the OS reclaims the remaining process resources.
[[noreturn]] void QuitProcess(const char *reason) {
  REXLOG_INFO("Native title lifecycle: {}", reason);
  rex::FlushLogging();
  std::_Exit(0);
}

} // namespace

// Xbox import 0x824BBA6C. The game's sole caller requests the dashboard with a
// null title path; on PC there is no dashboard, so the native action is exit.
extern "C" REX_FUNC(__imp__XamLoaderLaunchTitle) {
  QuitProcess("launch-title request mapped to process exit");
}

// Xbox import 0x824BBA5C. Ending a title ends this one PC process; it does not
// return to an Xbox shell or leave a window running without the game thread.
extern "C" REX_FUNC(__imp__XamLoaderTerminateTitle) {
  QuitProcess("terminate-title request");
}

// Xbox import 0x824BBC6C. Firmware return/reboot has no PC firmware target;
// the game's observed routine value is 1 (reboot), which also closes the app.
extern "C" REX_FUNC(__imp__HalReturnToFirmware) {
  REXLOG_DEBUG("Native firmware-return request: routine {}", ctx.r3.u32);
  QuitProcess("firmware-return request mapped to process exit");
}

// Xbox import 0x824BBE1C. Guest registration records contain a callback
// pointer and a priority, but this port exits the process instead of returning
// to a title manager. Avoid reading the guest record: it has no PC consumer.
extern "C" REX_FUNC(__imp__ExRegisterTitleTerminateNotification) {
  // Keep this at info level: unlike the other three imports this registration
  // can run during startup, so a test session can verify the native dispatch.
  REXLOG_INFO(
      "Native title-termination callback registration ignored (create={})",
      ctx.r4.u32);
}
