// =============================================================================
// debug_frame_capture.cpp -- see debug_frame_capture.h for the why and how
// =============================================================================

#include "debug_frame_capture.h"

#include <algorithm>
#include <vector>

#include <rex/logging.h>
#include <rex/ui/presenter.h>

REXCVAR_DEFINE_STRING(debug_capture_dir, "", "CrashMoM",
                      "Debug: save the displayed frame as PPM files into this "
                      "folder (empty = off)");
REXCVAR_DEFINE_INT32(debug_capture_interval_ms, 1000, "CrashMoM",
                     "Debug: time between two frame captures, in milliseconds");

void DebugFrameCapture::Start(rex::ui::Presenter* presenter) {
  const std::string& dir = REXCVAR_GET(debug_capture_dir);
  if (dir.empty() || !presenter || thread_.joinable()) {
    return;
  }
  presenter_ = presenter;
  dir_ = dir;
  interval_ = std::chrono::milliseconds(
      std::max<int32_t>(REXCVAR_GET(debug_capture_interval_ms), 50));
  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
  REXLOG_INFO("DebugFrameCapture: saving a frame every {} ms to {}",
              interval_.count(), dir_.string());
  thread_ = std::thread([this] { ThreadMain(); });
}

void DebugFrameCapture::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_requested_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void DebugFrameCapture::ThreadMain() {
  const auto start = std::chrono::steady_clock::now();
  rex::ui::RawImage image;
  for (;;) {
    {
      // Sleep one interval, but wake up at once if Stop() is called.
      std::unique_lock<std::mutex> lock(mutex_);
      if (wake_.wait_for(lock, interval_, [this] { return stop_requested_; })) {
        return;
      }
    }
    // CaptureGuestOutput is documented as callable from any thread. It
    // returns false until the game has presented its first frame.
    if (!presenter_->CaptureGuestOutput(image)) {
      continue;
    }
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count();
    // An all-black frame carries no information but its timestamp: log it
    // instead of writing 2.7 MB of zeros (black screens are what we chase).
    bool all_black = true;
    for (uint32_t y = 0; y < image.height && all_black; ++y) {
      const uint8_t* src = image.data.data() + y * image.stride;
      for (uint32_t x = 0; x < image.width; ++x) {
        if (src[x * 4 + 0] | src[x * 4 + 1] | src[x * 4 + 2]) {
          all_black = false;
          break;
        }
      }
    }
    if (all_black) {
      REXLOG_INFO("DebugFrameCapture: t={} ms black frame (not saved)", (long long)ms);
      continue;
    }
    char name[64];
    std::snprintf(name, sizeof(name), "frame_%09lldms.ppm", (long long)ms);
    std::FILE* f = std::fopen((dir_ / name).string().c_str(), "wb");
    if (!f) {
      continue;
    }
    // PPM "P6": ASCII header "P6 <width> <height> <maxval>\n", then 3 bytes
    // (R, G, B) per pixel, rows top to bottom. RawImage rows are R8 G8 B8 X8
    // with `stride` bytes per row, so we drop every 4th byte.
    std::fprintf(f, "P6\n%u %u\n255\n", image.width, image.height);
    std::vector<uint8_t> row(size_t(image.width) * 3);
    for (uint32_t y = 0; y < image.height; ++y) {
      const uint8_t* src = image.data.data() + y * image.stride;
      for (uint32_t x = 0; x < image.width; ++x) {
        row[x * 3 + 0] = src[x * 4 + 0];
        row[x * 3 + 1] = src[x * 4 + 1];
        row[x * 3 + 2] = src[x * 4 + 2];
      }
      std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
  }
}
