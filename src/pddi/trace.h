// =============================================================================
// pddi/trace.h -- the frame tracer, as seen by the interception layer
// =============================================================================
//
// The tracer (trace.cpp, --debug_pddi_trace_dir) writes every renderer call
// of chosen frames to a text file. intercept.cpp calls these three functions
// around every intercepted call; nothing else needs them. User docs: the
// header of trace.cpp and docs/findings/08.
// =============================================================================

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <rex/ppc/context.h>

#include "intercept.h"

namespace pddi::trace {

namespace detail {
extern std::atomic<bool> g_recording;
}

// True while a frame is being recorded (the wrappers' fast-path check).
inline bool Recording() {
  return detail::g_recording.load(std::memory_order_relaxed);
}

// Returned by Enter, handed back to Exit.
struct Token {
  size_t index = SIZE_MAX;  // SIZE_MAX = this call isn't being recorded
  uint32_t generation = 0;  // which traced frame the record belongs to
};

// Before / after an intercepted call (only called while Recording()).
Token Enter(FnId fn, const PPCContext& ctx);
void Exit(const Token& token, const PPCContext& ctx);

// Once per frame, on the main thread, after xnDisplay::SwapBuffers: starts
// and stops recordings, writes the files.
void OnFrameEnd();

}  // namespace pddi::trace
