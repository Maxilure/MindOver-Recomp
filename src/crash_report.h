// =============================================================================
// crash_report.h -- when the game dies, say WHERE (Windows and Linux)
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
// LINUX (2026-10-09): there, a crash didn't even END the game: the SDK's
// SIGSEGV handler declined the fault and returned, the instruction ran again,
// faulted again... forever (a "fault loop": frozen picture, one core at
// 100%). Now AfterRuntimeSetup() puts a LAST handler into the SDK's list
// (rex::arch::ExceptionHandler; it runs only when the MMIO handler, the GPU's
// write watches and our write watch all declined). It retries the same fault
// 20 times 1 ms apart (a state another thread is just changing), then writes
// the same kind of report:
//   - fault kind, host address and the GUEST address it maps to;
//   - the host instruction named from the exe's symbol table
//     (debug/host_symbols.h) = which recompiled game function (sub_X);
//   - the guest registers and the GAME's call stack (debug/guest_stack.h);
//   - the host call stack (backtrace through the signal frame);
//   - the original PowerPC instructions of the fault and of each game frame
//     (llvm-symbolizer + generated sources, in a forked child that is killed
//     after 10 s if stuck): exact even on a tester's own build.
// to user/logs/crash-<date>_<time>.txt, the terminal and the log, then lets
// the game die by the same signal WITHOUT a core dump (PR_SET_DUMPABLE 0: a
// dump of the game's gigabytes of mappings can fill RAM and swap).
// Under a debugger (tools/play.sh --catch) the old repeat is kept so gdb can
// catch it (after the report is written).
// =============================================================================
#pragma once

#include <filesystem>

namespace crash_report {

// Call once, early (CrashMomApp's OnPostInitLogging). `logs` = where the
// crash-<stamp>.txt file goes. Windows: installs the filter. Linux: reads
// the symbol table now (a signal handler can't).
void Install(const std::filesystem::path& logs);

// Linux: call once the runtime (guest memory) exists (OnPostSetup); installs
// the last-resort fault handler AFTER the SDK's own. Windows: nothing.
void AfterRuntimeSetup();

}  // namespace crash_report
