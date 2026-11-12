# 58. Title lifecycle imports answered by the PC process

The game's emulated Xbox-service list includes four title-lifecycle imports:
`XamLoaderLaunchTitle`, `XamLoaderTerminateTitle`, `HalReturnToFirmware` and
`ExRegisterTitleTerminateNotification`. They now have definitions in
`src/native/quit_launch.cpp`, so the generated guest import table resolves to
the port's host code instead of the SDK's Xbox service implementations.

## 1. What the game asks for

The functions were located by disassembling the extracted `default.xex` with
the project's `xexdis` tool and cross-checking the generated import table:

| Xbox import address | Game code that calls it | Observed use |
|---|---|---|
| `0x824BBA6C` `XamLoaderLaunchTitle` | `sub_82300DB0` | Its only caller passes a null title path, requesting the Xbox dashboard. |
| `0x824BBA5C` `XamLoaderTerminateTitle` | `xstart` (`0x82300BA0`) | Ends the running title. |
| `0x824BBC6C` `HalReturnToFirmware` | `sub_82301590` | Its caller passes routine `1` (reboot). |
| `0x824BBE1C` `ExRegisterTitleTerminateNotification` | startup and audio-library functions | Registers or removes guest termination callbacks (the create flag is in `r4`). |

`ExRegisterTitleTerminateNotification` was initially treated like a callback
the PC process needed to invoke. Inspection of ReXGlue's `KernelState` showed
that the runtime only stores and removes registrations; no runtime code
invokes that vector. A PC process also has no title manager to notify. The
native handler therefore records the request for diagnostics and does not
read or retain the guest callback pointer. Process teardown reclaims the
guest memory and any callback state with the rest of the process.

## 2. PC behavior

There is no dashboard or firmware on the PC. A request to launch the Xbox
dashboard, terminate the title, or reboot is mapped to a successful exit of
the one PC process. The handler logs and flushes before `_Exit(0)`, matching
the port's existing close behavior and avoiding a guest-thread shutdown wait.
The observed game caller for `HalReturnToFirmware` uses reboot routine `1`;
other routine values were not found among this game's callers.

The generated import table binds the four guest import addresses to the
`__imp__...` functions defined by `quit_launch.cpp`. No manifest entries,
guest code changes, SDK patches or guest-memory accesses are needed.

## 3. Verification and remaining checks

The Windows AMD64 RelWithDebInfo game build compiled and linked the new
definitions. A separate local harness compiled the same `quit_launch.cpp`
and called each handler in its own process: both registration modes returned
normally, and launch-title, terminate-title and firmware-return each flushed
the expected log message and exited with status 0. This verifies all four
handler bodies and both registration flags.

The game was also launched with a copied test user-data directory, an isolated
cache, `--audio_mute=true`, `--fps_cap=30` and scripted controller input. It
reached live gameplay and ran for more than a minute; closing the test window
completed the SDK's ordinary host shutdown path without a crash. This checks
that the linked definitions do not prevent normal startup. The window close
itself does not call the guest lifecycle imports, and the title's launch,
terminate and firmware branches were not reached from the live game during
this run. Their guest callers were disassembled, and the generated import
table points to the same strong `__imp__...` symbols that the game build
resolves to `quit_launch.cpp`; direct handler tests cover their process-exit
behavior. This leaves the caller branches unexercised, not the native handler
implementations.
