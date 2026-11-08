// =============================================================================
// native/scan.cpp -- see scan.h (native gaps: banner, photo, counts, file)
// =============================================================================

#include "scan.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "../pddi/trace.h"
#include "ab_capture.h"

REXCVAR_DECLARE(std::string, debug_pddi_trace_dir);  // pddi/trace.cpp

REXCVAR_DEFINE_BOOL(debug_native_gaps, true, "CrashMoM",
                    "Report what the native renderer can't draw yet, loudly: a WARN banner and a "
                    "photo the first time each gap shows up, plus <log name>-native-gaps.txt "
                    "(every gap, numbered, with counts). false = one quiet log line per gap");

namespace native::scan {

namespace {

using Clock = std::chrono::steady_clock;

struct Gap {
  int number = 0;           // 1, 2, ... in the order met (the banner's #N)
  std::string what;
  std::string first_time;   // wall clock, HH:MM:SS
  uint64_t count = 0;
  uint32_t photo_after = 0; // LastPhoto().serial when its photo was asked for
  bool photo_asked = false;
  std::string photo;        // that photo's file names, once taken
};

struct State {
  std::mutex mutex;
  std::unordered_map<std::string, size_t> index;  // what -> gaps[i]
  std::vector<Gap> gaps;
  bool dirty = false;
  Clock::time_point last_write{};
  Clock::time_point last_photo{};
  bool tried_path = false;
  std::filesystem::path path;  // empty = no session log: no file
} g;

std::string WallClock() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char text[16];
  std::strftime(text, sizeof(text), "%H:%M:%S", &tm);
  return text;
}

// <log name>-native-gaps.txt, like input_record's <log name>-inputs.txt.
void WriteFile() {
  if (!g.tried_path) {
    g.tried_path = true;
    const std::string log = rex::cvar::GetFlagByName("log_file");
    if (!log.empty()) {
      g.path = log;
      g.path.replace_filename(g.path.stem().string() + "-native-gaps.txt");
    }
  }
  if (g.path.empty()) return;
  // A photo asked for earlier has been taken since: note its files.
  const ab_capture::PhotoRecord photo = ab_capture::LastPhoto();
  for (Gap& gap : g.gaps) {
    if (gap.photo_asked && gap.photo.empty() && photo.serial > gap.photo_after) {
      gap.photo = photo.files;
    }
  }
  std::FILE* file = std::fopen(g.path.string().c_str(), "w");
  if (!file) return;
  std::fprintf(file,
               "# What the native renderer couldn't draw in this session (native/scan.h).\n"
               "# One line per gap, in the order met: number, first time, how many times,\n"
               "# what, and the photo taken the first time (user/photos/).\n\n");
  for (const Gap& gap : g.gaps) {
    std::fprintf(file, "#%-3d %s  x%-8llu %s\n", gap.number, gap.first_time.c_str(),
                 static_cast<unsigned long long>(gap.count), gap.what.c_str());
    if (!gap.photo.empty()) {
      std::fprintf(file, "      photo: %s\n", gap.photo.c_str());
    }
  }
  std::fclose(file);
  g.dirty = false;
  g.last_write = Clock::now();
}

}  // namespace

void NewGap(std::string_view what) {
  std::lock_guard<std::mutex> lock(g.mutex);
  const Clock::time_point now = Clock::now();
  auto it = g.index.find(std::string(what));
  if (it != g.index.end()) {
    // Met before: count it; keep the file's counts at most 10 s old.
    ++g.gaps[it->second].count;
    g.dirty = true;
    if (REXCVAR_GET(debug_native_gaps) && now - g.last_write > std::chrono::seconds(10)) {
      WriteFile();
    }
    return;
  }
  Gap gap;
  gap.number = int(g.gaps.size()) + 1;
  gap.what = std::string(what);
  gap.first_time = WallClock();
  gap.count = 1;
  g.index.emplace(gap.what, g.gaps.size());

  if (!REXCVAR_GET(debug_native_gaps)) {
    REXLOG_INFO("NativeRenderer: not drawn natively yet: {}", what);
    g.gaps.push_back(std::move(gap));
    return;
  }
  // WARN level: stands out in any log viewer, and play.sh colours it.
  REXLOG_WARN("*** NATIVE GAP #{} (first time): {} ***", gap.number, what);
  // One photo for a burst of new gaps (entering an area): the first one's.
  // Gaps are only met while our picture is recorded, so the photo can have
  // both pictures (and its draw list) when ours is on screen.
  if (now - g.last_photo > std::chrono::seconds(3) && !ab_capture::CaptureUnderway()) {
    gap.photo_after = ab_capture::LastPhoto().serial;
    gap.photo_asked = ab_capture::RequestPhotoLikeF10();
    if (gap.photo_asked) g.last_photo = now;
  }
  // With --trace: every renderer call of the next frame (pddi/trace.h).
  if (!REXCVAR_GET(debug_pddi_trace_dir).empty()) {
    REXLOG_INFO("Scan: first time: {} -> recording the next frame", what);
    pddi::trace::RequestTrace();
  }
  g.gaps.push_back(std::move(gap));
  WriteFile();
}

}  // namespace native::scan
