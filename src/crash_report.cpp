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

}  // namespace crash_report

#else  // not Windows

namespace crash_report {
void Install(const std::filesystem::path&) {}
}  // namespace crash_report

#endif
