// =============================================================================
// crash_report.cpp -- see crash_report.h
// =============================================================================
#include "crash_report.h"

#if defined(_WIN32)

#include <windows.h>
// (after windows.h:)
#include <dbghelp.h>

#include <cstdio>
#include <ctime>
#include <string>

#include <rex/logging.h>

#pragma comment(lib, "dbghelp.lib")

namespace crash_report {
namespace {

std::filesystem::path g_logs;

const char* CodeName(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case 0xC0000409: return "stack buffer overrun / fail-fast";
    default: return "exception";
  }
}

// "module!symbol+0x12 (file.cpp:34)" for one address.
std::string Describe(HANDLE process, DWORD64 address) {
  char buffer[sizeof(SYMBOL_INFO) + 512] = {};
  auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = 511;
  std::string text;
  char module_name[MAX_PATH] = "?";
  IMAGEHLP_MODULE64 module{};
  module.SizeOfStruct = sizeof(module);
  if (SymGetModuleInfo64(process, address, &module)) {
    std::snprintf(module_name, sizeof(module_name), "%s", module.ModuleName);
  }
  DWORD64 displacement = 0;
  char line[1024];
  if (SymFromAddr(process, address, &displacement, symbol)) {
    std::snprintf(line, sizeof(line), "%s!%s+0x%llX", module_name, symbol->Name,
                  static_cast<unsigned long long>(displacement));
  } else {
    std::snprintf(line, sizeof(line), "%s!0x%llX", module_name,
                  static_cast<unsigned long long>(address));
  }
  text = line;
  IMAGEHLP_LINE64 source{};
  source.SizeOfStruct = sizeof(source);
  DWORD column = 0;
  if (SymGetLineFromAddr64(process, address, &column, &source)) {
    std::snprintf(line, sizeof(line), " (%s:%lu)", source.FileName, source.LineNumber);
    text += line;
  }
  return text;
}

LONG WINAPI OnUnhandled(EXCEPTION_POINTERS* info) {
  static volatile LONG once = 0;
  if (InterlockedExchange(&once, 1) != 0) {
    return EXCEPTION_CONTINUE_SEARCH;  // a second crash while reporting: just die
  }
  const EXCEPTION_RECORD& record = *info->ExceptionRecord;
  HANDLE process = GetCurrentProcess();
  HANDLE thread = GetCurrentThread();
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
  SymInitialize(process, nullptr, TRUE);  // the exe's folder: crash_mom.pdb

  std::string report;
  char line[512];
  std::snprintf(line, sizeof(line), "CRASH: %s (code 0x%08lX) at %s\n", CodeName(record.ExceptionCode),
                record.ExceptionCode,
                Describe(process, reinterpret_cast<DWORD64>(record.ExceptionAddress)).c_str());
  report += line;
  if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
    const ULONG_PTR kind = record.ExceptionInformation[0];
    std::snprintf(line, sizeof(line), "  %s of host address 0x%llX\n",
                  kind == 0 ? "read" : kind == 1 ? "write" : "execute",
                  static_cast<unsigned long long>(record.ExceptionInformation[1]));
    report += line;
  }
  std::snprintf(line, sizeof(line), "  thread %lu; call stack (innermost first):\n",
                GetCurrentThreadId());
  report += line;

  // Walk the stack from the faulting context (a copy: StackWalk64 changes it).
  CONTEXT context = *info->ContextRecord;
  STACKFRAME64 frame{};
  frame.AddrPC.Offset = context.Rip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = context.Rbp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = context.Rsp;
  frame.AddrStack.Mode = AddrModeFlat;
  for (int depth = 0; depth < 48; ++depth) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr,
                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
        frame.AddrPC.Offset == 0) {
      break;
    }
    std::snprintf(line, sizeof(line), "  #%-2d ", depth);
    report += line + Describe(process, frame.AddrPC.Offset) + "\n";
  }

  // The log (it may not get flushed: the file below is the reliable copy).
  REXLOG_ERROR("{}", report);
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  char name[64];
  std::strftime(name, sizeof(name), "crash-%Y-%m-%d_%H%M%S.txt", &local);
  std::error_code ec;
  std::filesystem::create_directories(g_logs, ec);
  if (FILE* file = _wfopen((g_logs / name).wstring().c_str(), L"wb")) {
    std::fwrite(report.data(), 1, report.size(), file);
    std::fclose(file);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void Install(const std::filesystem::path& logs) {
  g_logs = logs;
  SetUnhandledExceptionFilter(OnUnhandled);
}

void AfterRuntimeSetup() {}  // Windows: the filter above covers everything

}  // namespace crash_report

#elif defined(__linux__) && defined(__x86_64__)

// -----------------------------------------------------------------------------
// Linux (see crash_report.h). Everything in OnUnhandled runs inside a signal
// handler: no allocation, no locks until the report file is safely written.
// Text is built with snprintf into a static buffer and written with write().
// -----------------------------------------------------------------------------

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <rex/exception_handler.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>

#include "debug/guest_stack.h"
#include "debug/host_symbols.h"

namespace crash_report {
namespace {

char g_logs[1024];                 // the logs folder (set by Install)
std::atomic<bool> g_reported{false};
uint8_t* g_membase = nullptr;      // guest memory base (AfterRuntimeSetup)

// The fault the SDK couldn't place, retried a few times first: see
// OnUnhandled. Per thread: the same pc + address as last time.
thread_local uint64_t t_last_pc = 0, t_last_address = 0;
thread_local int t_repeats = 0;
constexpr int kRetries = 20;       // x 1 ms

// The report, built in place.
char g_text[64 * 1024];
size_t g_length = 0;
void Add(const char* format, ...) __attribute__((format(printf, 1, 2)));
void Add(const char* format, ...) {
  if (g_length >= sizeof(g_text) - 1) return;
  va_list args;
  va_start(args, format);
  const int n = std::vsnprintf(g_text + g_length, sizeof(g_text) - g_length, format, args);
  va_end(args);
  if (n > 0) g_length = std::min(sizeof(g_text) - 1, g_length + size_t(n));
}

void WriteAll(int fd, const char* p, size_t n) {
  while (n) {
    const ssize_t w = write(fd, p, n);
    if (w <= 0) return;
    p += w, n -= size_t(w);
  }
}

// Is a debugger attached? (TracerPid in /proc/self/status, read with plain
// syscalls.) Under gdb (tools/play.sh --catch) the old behaviour stays: the
// fault repeats and the debugger catches it.
bool Traced() {
  const int fd = open("/proc/self/status", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  char buf[4096];
  const ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) return false;
  buf[n] = 0;
  const char* p = std::strstr(buf, "TracerPid:");
  return p && std::strtol(p + 10, nullptr, 10) != 0;
}

// The thread's name ("Main XThread (F..." etc.).
void ThreadName(char* out, size_t size) {
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/self/task/%ld/comm", long(syscall(SYS_gettid)));
  out[0] = 0;
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  const ssize_t n = read(fd, out, size - 1);
  close(fd);
  out[n > 0 ? n : 0] = 0;
  if (char* nl = std::strchr(out, '\n')) *nl = 0;
}

void AddFrame(const char* label, uint64_t address) {
  char name[512];
  host_symbols::Describe(address, name, sizeof(name));
  Add("  %s %s", label, name);
  if (const uint64_t offset = host_symbols::ExeOffset(address)) {
    Add("  [crash_mom+0x%llx]", static_cast<unsigned long long>(offset));
  }
  Add("\n");
}

// The exact PowerPC instructions behind the host addresses (llvm-symbolizer +
// the generated sources: host_symbols.h). That allocates and runs a program,
// so it happens in a CHILD process (fork is allowed in a signal handler):
// if it gets stuck, only the child does, and it's killed after 10 s. The
// child appends its section to the already-written report file.
void AppendGuestInstructions(const char* path, const uint64_t* addresses, int count) {
  const pid_t child = fork();
  if (child == 0) {
    std::vector<uint64_t> list(addresses, addresses + count);
    const auto found = host_symbols::GuestInstructions(list);
    std::string text = "\nOriginal PowerPC instructions (generated sources + debug info):\n";
    for (int i = 0; i < count; ++i) {
      char line[256];
      if (found[i].address) {
        std::snprintf(line, sizeof(line), "  %s 0x%08X  %s\n", i == 0 ? "fault " : "called",
                      found[i].address, found[i].text.c_str());
        text += line;
      }
    }
    if (const int fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC); fd >= 0) {
      WriteAll(fd, text.data(), text.size());
      close(fd);
    }
    _exit(0);
  }
  if (child < 0) return;
  for (int waited = 0; waited < 1000; ++waited) {  // 10 s
    if (waitpid(child, nullptr, WNOHANG) == child) return;
    const timespec ms10{0, 10 * 1000 * 1000};
    nanosleep(&ms10, nullptr);
  }
  kill(child, SIGKILL);
  waitpid(child, nullptr, 0);
}

bool OnUnhandled(rex::arch::Exception* ex, void*) {
  using Ex = rex::arch::Exception;
  // 1. The SDK's handlers all declined this fault. Before, the instruction
  //    just ran again, forever (a "fault loop": the game froze at 100% CPU).
  //    Give a passing state a chance first (another thread mid-mprotect):
  //    retry the same fault up to 20 times, 1 ms apart.
  if (ex->pc() == t_last_pc && ex->fault_address() == t_last_address) {
    ++t_repeats;
  } else {
    t_last_pc = ex->pc(), t_last_address = ex->fault_address(), t_repeats = 0;
  }
  if (t_repeats < kRetries || g_reported.exchange(true)) {
    const timespec ms1{0, 1000 * 1000};
    nanosleep(&ms1, nullptr);  // (a 2nd thread crashing while we report waits here)
    return true;               // run it again
  }

  // 2. The report.
  char thread_name[64];
  ThreadName(thread_name, sizeof(thread_name));
  const bool av = ex->code() == Ex::Code::kAccessViolation;
  const uint64_t address = ex->fault_address();
  Add("CRASH: %s", av ? "access violation" : "illegal instruction");
  if (av) {
    Add(" (%s of host 0x%llx",
        ex->access_violation_operation() == Ex::AccessViolationOperation::kWrite ? "write" : "read",
        static_cast<unsigned long long>(address));
    if (g_membase && address >= uint64_t(g_membase) && address - uint64_t(g_membase) < 0x100000000ull) {
      Add(" = guest 0x%08llX", static_cast<unsigned long long>(address - uint64_t(g_membase)));
    }
    Add(")");
  }
  Add("\n  thread %ld \"%s\"\n", long(syscall(SYS_gettid)), thread_name);
  AddFrame("at", ex->pc());
  if (const uint32_t fn = host_symbols::GuestFunction(ex->pc())) {
    Add("  = inside the game's function sub_%08X\n", fn);
  }

  // The guest side: registers and the game's own call stack.
  if (auto* ts = rex::runtime::ThreadState::Get()) {
    const PPCContext* ctx = ts->context();
    Add("Guest registers: r1 %08X  lr %08X  ctr %08X\n", ctx->r1.u32, uint32_t(ctx->lr),
        ctx->ctr.u32);
    Add("  r3 %08X  r4 %08X  r5 %08X  r6 %08X  r30 %08X  r31 %08X\n", ctx->r3.u32, ctx->r4.u32,
        ctx->r5.u32, ctx->r6.u32, ctx->r30.u32, ctx->r31.u32);
    uint32_t callers[24];
    const int n = guest_stack::Callers(ctx->r1.u32, callers, 24);
    Add("Game call stack (return addresses, innermost first; lr = the last call made):\n");
    for (int i = 0; i < n; ++i) {
      const uint32_t fn = guest_stack::FunctionContaining(callers[i]);
      Add("  #%-2d %08X", i, callers[i]);
      if (fn) Add("  (sub_%08X+0x%X)", fn, callers[i] - fn);
      Add("\n");
    }
  } else {
    Add("(not a game thread: no guest registers)\n");
  }

  // The host side: the native call stack (unwound through the signal frame).
  void* frames[48];
  const int depth = backtrace(frames, 48);
  Add("Host call stack (innermost first; the first frames are the fault handlers):\n");
  for (int i = 0; i < depth; ++i) {
    char label[16];
    std::snprintf(label, sizeof(label), "#%-2d", i);
    AddFrame(label, uint64_t(frames[i]));
  }

  // 3. Write it: the file first (the reliable copy), then the log.
  char path[1200];
  {
    const time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    char name[64];
    strftime(name, sizeof(name), "crash-%Y-%m-%d_%H%M%S.txt", &local);
    std::snprintf(path, sizeof(path), "%s/%s", g_logs, name);
  }
  if (g_logs[0]) {
    if (const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); fd >= 0) {
      WriteAll(fd, g_text, g_length);
      close(fd);
      // Exact instructions: the fault + the host frames that are game code
      // (return addresses - 1 = the call itself).
      uint64_t asked[49];
      int count = 0;
      asked[count++] = ex->pc();
      // backtrace() lists the handlers, the signal frame, then the faulting
      // instruction itself (not a return address): callers start after it.
      int first = 0;
      for (int i = 0; i < depth; ++i) {
        if (uint64_t(frames[i]) == ex->pc()) first = i + 1;
      }
      for (int i = first; i < depth; ++i) {
        if (host_symbols::GuestFunction(uint64_t(frames[i]))) asked[count++] = uint64_t(frames[i]) - 1;
      }
      AppendGuestInstructions(path, asked, count);
    }
  }
  WriteAll(2, g_text, g_length);  // the terminal (play.sh / the launcher's output)
  REXLOG_ERROR("{}(also in {})", g_text, path);
  rex::FlushLogging();

  // 4. End the game. Under a debugger: keep the old repeat (it catches it).
  //    Otherwise die by the same signal, WITHOUT a core dump: dumping the
  //    game's gigabytes of mappings can fill RAM + swap while the system's
  //    dump service chews on it. A non-dumpable process is never dumped.
  if (!Traced()) {
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    signal(av ? SIGSEGV : SIGILL, SIG_DFL);
  }
  return true;  // re-run: with the default action back, that kills the game
}

}  // namespace

void Install(const std::filesystem::path& logs) {
  std::snprintf(g_logs, sizeof(g_logs), "%s", logs.string().c_str());
  host_symbols::Load();  // now: the handler can't read files into memory
  void* warm[2];
  backtrace(warm, 2);    // loads the unwinder now (its first call allocates)
}

void AfterRuntimeSetup() {
  auto* memory = rex::system::kernel_memory();
  if (!memory) return;
  g_membase = memory->virtual_membase();
  guest_stack::SetMembase(g_membase);
  guest_stack::FunctionContaining(0);  // sizes its table now (a static: not in a handler)
  // LAST in the SDK's handler list: called only when every other handler
  // (MMIO, GPU write watches, our write watch) declined the fault.
  rex::arch::ExceptionHandler::Install(OnUnhandled, nullptr);
}

}  // namespace crash_report

#else  // other systems: nothing yet

namespace crash_report {
void Install(const std::filesystem::path&) {}
void AfterRuntimeSetup() {}
}  // namespace crash_report

#endif
