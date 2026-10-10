// =============================================================================
// xbox_calls.h -- the "Xbox calls meter": which Xbox services the game uses
// =============================================================================
//
// The game's own code runs as recompiled C++, but it still talks to a FAKE
// Xbox 360: 155 system services (xboxkrnl + xam imports: threads, files,
// profiles, saves, the GPU's video driver, sound...) answered by the SDK.
// Going native (notes/plans.md section 9) = answering them ourselves, one
// piece at a time, until only the kernel basics are left. This meter shows
// what is still in use: a checklist and a progress measure, like the area
// scan did for the native renderer.
//
// HOW: every call to an import goes to a C function named __imp__<Name>
// (defined in the SDK's runtime library; the generated code calls it
// directly). On Linux the linker's --wrap option redirects each of those
// references to __wrap___imp__<Name>, a tiny function generated at configure
// time (CMakeLists.txt reads the names out of generated/default/
// crash_mom_init.cpp) that calls Hit() and then the real one
// (__real___imp__<Name>). Cost: one relaxed atomic add per call.
// Windows' linker has no --wrap: the meter is a Linux-only dev tool.
//
// WHAT YOU GET (on by default, --xbox_calls_meter=false turns it off):
//   - the log: "Xbox calls: first XamContentCreateEx from sub_823687A0+0x5C"
//     the first time each service is called (with the game function that
//     called it: where to cut in when replacing it);
//   - <log name>-xbox-calls.txt next to the session log, rewritten every 30 s:
//     every service called this session with its count and up to 8 distinct
//     calling functions, plus the list of imports never called;
//   - the debug console's `xbox` command: the same table, live.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, xbox_calls_meter);

namespace xbox_calls {

// Called by the generated wrappers (xbox_calls_wraps.cpp in the build folder)
// before each real import: `index` = position in the name table, `lr` = the
// guest return address (who called it).
void Hit(uint32_t index, uint32_t lr);

// The generated file hands over its name table at static-init time.
void SetNames(const char* const* names, uint32_t count);

// The table as text (console `xbox`, the report file).
std::string Report();

}  // namespace xbox_calls
