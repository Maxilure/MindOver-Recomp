// =============================================================================
// crash_report.h -- when the game dies on Windows, say WHERE
// =============================================================================
//
// WHY: a crash on Windows only shows an exit code (-1073741819 =
// 0xC0000005, "access violation") and the SDK's last log line ("Unhandled
// guest access violation: write of guest 0x00000000 ..."), but not WHICH
// code did it. Players can't attach a debugger; testers can send a log.
//
// WHAT: Install() sets a Windows "unhandled exception filter": Windows calls
// it only after every other handler declined the exception, i.e. right
// before the process dies. (The SDK's own handler catches the expected
// faults first: memory-mapped hardware, write watches. Those never get here.)
// It writes, to the log and to <user>/logs/crash-<date>_<time>.txt:
//   - the exception code and the faulting address (+ the data address for
//     access violations: read / write of what);
//   - the call stack, walked from the fault (StackWalk64), each frame named
//     from crash_mom.pdb (DbgHelp): recompiled game functions show up as
//     __imp__sub_82XXXXXX = the game's own address of that function, ours
//     by name, with file:line where the symbols have them.
// The crash then continues as before (the filter returns "continue search").
//
// Linux: nothing yet (tools/play.sh --catch runs the game under gdb for this).
// =============================================================================
#pragma once

#include <filesystem>

namespace crash_report {

// Call once, early (CrashMomApp's OnPostInitLogging). `logs` = where the
// crash-<stamp>.txt file goes. Does nothing outside Windows.
void Install(const std::filesystem::path& logs);

}  // namespace crash_report
