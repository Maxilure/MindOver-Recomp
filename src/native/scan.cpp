// =============================================================================
// native/scan.cpp -- see scan.h
// =============================================================================

#include "scan.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "../pddi/trace.h"
#include "ab_capture.h"

REXCVAR_DECLARE(std::string, debug_pddi_trace_dir);  // pddi/trace.cpp

namespace native::scan {

void NewGap(std::string_view what) {
  if (REXCVAR_GET(debug_pddi_trace_dir).empty()) {
    return;
  }
  REXLOG_INFO("Scan: first time: {} -> recording the next frame + a photo", what);
  // Both start at the next frame end. Several gaps met in the same frame
  // (entering an area) share one recording. Gaps are only met while our
  // picture is drawn somewhere, so a photo can have both pictures.
  pddi::trace::RequestTrace();
  ab_capture::RequestPhoto(/*native_shown=*/true);
}

}  // namespace native::scan
