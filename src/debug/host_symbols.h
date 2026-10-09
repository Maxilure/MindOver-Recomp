// =============================================================================
// debug/host_symbols.h -- turn a HOST code address into "which game function"
// =============================================================================
//
// WHY
//   Two debugging aids need to name the code that was running at some moment:
//   the Linux crash report (crash_report.cpp: where did the game die?) and the
//   memory write watch (write_watchpoints.h: who wrote this value?). Both only
//   get a HOST instruction address (x86-64 RIP) from the fault. That address
//   lies inside our compiled executable, usually inside a recompiled game
//   function (`__imp__sub_82XXXXXX`), sometimes in our own code or an SDK
//   library.
//
// HOW
//   - Names: the executable's own ELF symbol table (.symtab; the build keeps
//     it, ~62,000 functions, every recompiled one with its size). Load() reads
//     it once from /proc/self/exe into a sorted table; lookups after that never
//     allocate, so the crash handler (a signal handler) may call Describe().
//     Addresses outside the exe (the SDK's .so files) are named with dladdr().
//   - The EXACT original PowerPC instruction: GuestInstructions() asks
//     llvm-addr2line which line of generated/default/*.cpp an address came
//     from, then reads that file: every guest instruction is a `// <asm>`
//     comment line followed by its C++; counting comments from the nearest
//     anchor (`DEFINE_REX_FUNC(sub_X)` or a `loc_X:` label) gives its address
//     (anchor + 4 x index). Checked against the `ctx.lr = 0x...` lines the
//     generator writes after every call: 7,881 of 7,881 matched (2026-10-09).
//     Needs a developer build (debug info + the generated sources on disk);
//     elsewhere it just answers "unknown". Not for signal handlers (it runs a
//     program).
//
// Linux only. On Windows, crash_report.cpp names frames with DbgHelp instead,
// and these functions answer "unknown".
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace host_symbols {

// Reads the executable's symbol table (once; later calls return at once).
// Call it from normal code before anything may need Describe() in a signal
// handler. Returns false if the table couldn't be read (stripped build).
bool Load();

// "__imp__sub_82177D38+0x1a4" (exe), "librexruntimerd.so!name+0x10" (a
// library) or "0x7f...". Writes into `out` (always NUL-terminated); no
// allocation once Load() ran. Names stay mangled (C++ names start with _Z).
void Describe(uint64_t address, char* out, size_t size);

// The game function whose recompiled body contains the address: 0x82XXXXXX
// from a symbol named __imp__sub_X or sub_X, else 0. Signal-safe after Load().
uint32_t GuestFunction(uint64_t address);

// Address minus the executable's load address (PIE): what addr2line wants.
// 0 if the address isn't inside the executable.
uint64_t ExeOffset(uint64_t address);

// The original PowerPC instruction behind each host address (see HOW).
struct GuestInstruction {
  uint32_t address = 0;  // 0 = unknown (not recompiled game code, or no debug info)
  std::string text;      // the instruction as the generator wrote it, e.g. "stw r11,80(r31)"
};
// One llvm-addr2line run for the whole list. For a RETURN address (a caller
// in a stack), pass address - 1 to land on the call itself.
std::vector<GuestInstruction> GuestInstructions(const std::vector<uint64_t>& host_addresses);

}  // namespace host_symbols
