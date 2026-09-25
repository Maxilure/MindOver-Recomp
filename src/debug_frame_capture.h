// =============================================================================
// debug_frame_capture.h -- save what the game shows on screen, every N ms
// =============================================================================
//
// A debugging aid, OFF unless you pass --debug_capture_dir=<folder>.
//
// Why it exists: to chase rendering bugs (black Bink movies, the title
// screen logo vanishing) Claude has to *see* the picture, and it can't look
// at the game window. This saves the picture itself.
//
// How it works: ReXGlue's presenter keeps the "guest output", the final
// 1280x720-ish image the emulated Xbox 360 GPU sent to the TV (before the
// host window scales it). Presenter::CaptureGuestOutput() copies it back
// from the host GPU into a RawImage. The SDK implements it but nothing calls
// it. A background thread here calls it every --debug_capture_interval_ms
// and writes the result as a binary PPM file (header + raw RGB bytes, the
// simplest image format there is, so no image library is needed).
//
// Usage:
//   crash_mom --debug_capture_dir=/some/dir --debug_capture_interval_ms=500
//   -> /some/dir/frame_000123456ms.ppm, named by milliseconds since start.
//   All-black frames aren't saved, only logged ("black frame (not saved)").
//   Convert with e.g. `python3 -c "from PIL import Image; ..."` or any viewer.
//
// The captures are pictures of the game: they're game-derived data. Keep
// them out of the repo (logs/ and the scratchpad are fine, both unversioned).
// =============================================================================

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include <rex/cvar.h>

namespace rex::ui {
class Presenter;
}

// Command-line flags, defined in debug_frame_capture.cpp.
REXCVAR_DECLARE(std::string, debug_capture_dir);
REXCVAR_DECLARE(int32_t, debug_capture_interval_ms);

class DebugFrameCapture {
 public:
  ~DebugFrameCapture() { Stop(); }

  // Starts the capture thread if --debug_capture_dir is set. `presenter`
  // may be null (headless run), in which case this does nothing.
  void Start(rex::ui::Presenter* presenter);

  // Stops and joins the thread. Safe to call more than once.
  void Stop();

 private:
  void ThreadMain();

  rex::ui::Presenter* presenter_ = nullptr;
  std::filesystem::path dir_;
  std::chrono::milliseconds interval_{1000};

  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable wake_;  // lets Stop() interrupt the sleep
  bool stop_requested_ = false;
};
