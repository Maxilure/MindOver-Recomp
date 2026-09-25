// =============================================================================
// pddi/intercept.cpp -- the wrappers, handler table and frame listeners
// =============================================================================
// See intercept.h for the why and how.
// =============================================================================

#include "intercept.h"

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

#include "trace.h"

namespace pddi {

const FnInfo kFns[kFnCount] = {
#define PDDI_METHOD(hex, name) {0x##hex, name},
#define PDDI_D3D(hex, name) {0x##hex, name},
#define PDDI_VTABLE(hex, name)
#include "functions.inc"
#undef PDDI_METHOD
#undef PDDI_D3D
#undef PDDI_VTABLE
};

namespace {

// Renderer class vtables (PDDI_VTABLE entries, found by tools/rtti_vtables.py).
struct VtableName {
  uint32_t vtable;
  const char* name;
};
constexpr VtableName kVtables[] = {
#define PDDI_METHOD(hex, name)
#define PDDI_D3D(hex, name)
#define PDDI_VTABLE(hex, name) {0x##hex, name},
#include "functions.inc"
#undef PDDI_METHOD
#undef PDDI_D3D
#undef PDDI_VTABLE
};

// One slot per intercepted function; nullptr = call the original.
std::atomic<Handler> g_handlers[kFnCount];

// End-of-frame listeners. Only the main thread calls them, while holding the
// mutex, so RemoveFrameEndListener can wait for a call in progress.
std::mutex g_listeners_mutex;
std::vector<std::pair<FrameListener, void*>> g_listeners;

void RunFrameEndListeners() {
  std::lock_guard<std::mutex> lock(g_listeners_mutex);
  for (const auto& [listener, user] : g_listeners) {
    listener(user);
  }
}

}  // namespace

const char* ClassOfVtable(uint32_t vtable) {
  for (const auto& v : kVtables) {
    if (v.vtable == vtable) {
      return v.name;
    }
  }
  return nullptr;
}

void SetHandler(FnId fn, Handler handler) {
  g_handlers[fn].store(handler, std::memory_order_release);
}

void AddFrameEndListener(FrameListener listener, void* user) {
  std::lock_guard<std::mutex> lock(g_listeners_mutex);
  g_listeners.emplace_back(listener, user);
}

void RemoveFrameEndListener(FrameListener listener, void* user) {
  std::lock_guard<std::mutex> lock(g_listeners_mutex);
  std::erase(g_listeners, std::make_pair(listener, user));
}

// The slow path of every wrapper: tracing, handler, frame end.
// Not in the anonymous namespace: the wrappers below are extern "C".
void InterceptedCall(FnId fn, PPCFunc* original, PPCContext& ctx, uint8_t* base) {
  trace::Token token;
  if (trace::Recording()) {
    token = trace::Enter(fn, ctx);
  }
  if (Handler handler = g_handlers[fn].load(std::memory_order_acquire)) {
    handler(ctx, base, original);
  } else {
    original(ctx, base);
  }
  if (token.index != SIZE_MAX) {
    trace::Exit(token, ctx);
  }
  if (fn == kSwapBuffers) {
    trace::OnFrameEnd();
    RunFrameEndListeners();
  }
}

}  // namespace pddi

// -----------------------------------------------------------------------------
// The wrappers: one strong sub_XXXXXXXX per table entry
// -----------------------------------------------------------------------------
// SwapBuffers always takes the slow path (frame end); the others only with a
// handler installed or a trace running. `kFn_##hex == kSwapBuffers` is a
// compile-time constant, so that test costs nothing.

#define PDDI_WRAP(hex)                                                                   \
  extern "C" REX_FUNC(__imp__sub_##hex);                                                 \
  extern "C" REX_FUNC(sub_##hex) {                                                       \
    if (pddi::kFn_##hex != pddi::kSwapBuffers &&                                         \
        !pddi::g_handlers[pddi::kFn_##hex].load(std::memory_order_relaxed) &&            \
        !pddi::trace::Recording()) {                                                     \
      __imp__sub_##hex(ctx, base);                                                       \
      return;                                                                            \
    }                                                                                    \
    pddi::InterceptedCall(pddi::kFn_##hex, __imp__sub_##hex, ctx, base);                 \
  }
#define PDDI_METHOD(hex, name) PDDI_WRAP(hex)
#define PDDI_D3D(hex, name) PDDI_WRAP(hex)
#define PDDI_VTABLE(hex, name)
#include "functions.inc"
#undef PDDI_METHOD
#undef PDDI_D3D
#undef PDDI_VTABLE
#undef PDDI_WRAP
