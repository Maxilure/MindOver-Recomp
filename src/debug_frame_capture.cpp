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
                     "Debug: time between two frame captures, in milliseconds (at least 5: "
                     "5-10 catches every frame at 30-60 fps)");
REXCVAR_DEFINE_INT32(debug_capture_shrink, 1, "CrashMoM",
                     "Debug: save every Nth pixel of every Nth row (4 = 320x180 from 720p); "
                     "above 1, a capture identical to the previous one is skipped and black "
                     "frames are saved too (for frame-by-frame flash hunts: small files, no repeats)");

void DebugFrameCapture::Start(rex::ui::Presenter* presenter) {
  const std::string& dir = REXCVAR_GET(debug_capture_dir);
  if (dir.empty() || !presenter || thread_.joinable()) {
    return;
  }
  presenter_ = presenter;
  dir_ = dir;
  interval_ = std::chrono::milliseconds(
      std::max<int32_t>(REXCVAR_GET(debug_capture_interval_ms), 5));
  shrink_ = uint32_t(std::clamp<int32_t>(REXCVAR_GET(debug_capture_shrink), 1, 16));
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
  uint64_t last_hash = 0;  // of the previous saved capture (shrink > 1: repeats skipped)
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
    // Shrunk captures: hash what would be written (FNV-1a over the kept
    // pixels); the same picture as last time isn't written again.
    if (shrink_ > 1) {
      uint64_t hash = 1469598103934665603ull;
      for (uint32_t y = 0; y < image.height; y += shrink_) {
        const uint8_t* src = image.data.data() + y * image.stride;
        for (uint32_t x = 0; x < image.width; x += shrink_) {
          for (int c = 0; c < 3; ++c) hash = (hash ^ src[x * 4 + c]) * 1099511628211ull;
        }
      }
      if (hash == last_hash) continue;
      last_hash = hash;
    }
    if (all_black && shrink_ == 1) {
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
    // Shrunk: every shrink_-th pixel of every shrink_-th row (no filtering:
    // a flash of one frame must stay as bright as it was).
    const uint32_t out_w = (image.width + shrink_ - 1) / shrink_;
    const uint32_t out_h = (image.height + shrink_ - 1) / shrink_;
    std::fprintf(f, "P6\n%u %u\n255\n", out_w, out_h);
    std::vector<uint8_t> row(size_t(out_w) * 3);
    for (uint32_t y = 0; y < image.height; y += shrink_) {
      const uint8_t* src = image.data.data() + y * image.stride;
      for (uint32_t x = 0, o = 0; x < image.width; x += shrink_, ++o) {
        row[o * 3 + 0] = src[x * 4 + 0];
        row[o * 3 + 1] = src[x * 4 + 1];
        row[o * 3 + 2] = src[x * 4 + 2];
      }
      std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
  }
}
