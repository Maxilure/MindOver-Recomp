// =============================================================================
// native/ab_capture.cpp -- see ab_capture.h for the why and how
// =============================================================================

#include "ab_capture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>

#include "../debug_frame_capture.h"
#include "../game_folder.h"
#include "../png_writer.h"
#include "guest.h"
#include "spotter.h"
#include "texture_cache.h"

REXCVAR_DEFINE_STRING(debug_native_ab_ms, "", "CrashMoM",
                      "Debug: at these times (ms since start, comma-separated) save the same "
                      "frame from both renderers into --debug_capture_dir (ab_<ms>_native.ppm, "
                      "ab_<ms>_emulated.ppm). Needs --renderer=native --readback_resolve=full");
REXCVAR_DEFINE_STRING(debug_native_ab_trigger, "", "CrashMoM",
                      "Debug: like --debug_native_ab_ms, whenever this file appears (`touch` it; "
                      "it is deleted once the frame is saved)");
REXCVAR_DEFINE_STRING(photo_dir, "", "CrashMoM",
                      "Where F10 saves photos (PNG): the same frame from both renderers while "
                      "the native picture is shown, else the screen. Empty = user/photos in "
                      "the game folder; a relative path starts at the folder the game was "
                      "started from");

namespace native::ab_capture {

namespace {

// Where we are in capturing one frame (see ab_capture.h).
enum class Stage {
  kIdle,            // waiting for the next time
  kMarkedPrevious,  // the frame before the captured one has its marker
  kMarkedTarget,    // the captured frame's frontbuffer has its marker
};

// The marker: a 32-bit pattern in 8 pixels of the first row and 8 of the
// last (whole pixels the resolve certainly writes: a tiled texture's memory
// also holds padding rows nobody writes). No picture the game draws is going
// to hold exactly these values in all 8 pixels of a group.
constexpr uint32_t kMarkerPixels = 8;
constexpr uint32_t kMarker = 0xA55AC33C;

struct Region {
  // Guest addresses of the marker pixels: [0..7] first row, [8..15] last row.
  uint32_t pixels[2 * kMarkerPixels] = {};
};

struct State {
  bool parsed = false;
  std::vector<int64_t> times;  // pending, ascending
  Stage stage = Stage::kIdle;
  // The capture under way is an F10 photo (PNG into --photo_dir), not a
  // debug capture (PPM into --debug_capture_dir).
  bool photo = false;
  // --readback_resolve as it was before a photo switched it to "full".
  bool readback_switched = false;
  std::string readback_before;
  Region previous, target;
  GuestTexture target_texture;
  int64_t target_ms = 0;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
} g;

// A pending F10 photo: 0 none, 1 both pictures (the native one is on
// screen), 2 the screen only. Set by the key (UI thread), taken by the main
// thread.
std::atomic<int> g_photo_request{0};
constexpr int kPhotoPair = 1, kPhotoScreen = 2;

// A photo needs the emulated GPU to copy its picture back into guest
// memory (ab_capture.h): --readback_resolve=full for the frames it takes.
// That flag lives in the GPU plugin, which is loaded at runtime (not linked):
// reached by name through the flag registry.
void SwitchReadbackOn() {
  const std::string current = rex::cvar::GetFlagByName("readback_resolve");
  if (current != "full" && !g.readback_switched) {
    g.readback_before = current;
    g.readback_switched = true;
    rex::cvar::SetFlagByName("readback_resolve", "full");
  }
}
void RestoreReadback() {
  if (g.readback_switched) {
    rex::cvar::SetFlagByName("readback_resolve", g.readback_before);
    g.readback_switched = false;
  }
}

// The emulated GPU draws nothing right now (--native_only with our picture
// in the main window: NativeRenderer::UpdateEmulatedDrawing sets the GPU
// plugin's "skip_draws", SDK patch 0009). Its frontbuffer then never gets a
// new picture, so a marker would never be overwritten (ab_capture.h,
// NATIVE ONLY). Read by name like readback_resolve; only called while a
// capture is wanted (Active()), so the registry lookup costs nothing in play.
bool EmulatedGpuIdle() { return rex::cvar::Query<bool>("skip_draws"); }

// --emulated_draw_every (native_renderer.cpp): the emulated GPU may skip
// every other frame, and a skipped frame never lands its marker. A capture
// starts mid-frame (at the frontbuffer resolve, before the GPU gets there),
// so it switches the GPU back to every frame right now: the SDK reads the
// flag at every draw, so the rest of this frame and the captured next one
// are drawn. NativeRenderer::UpdateEmulatedFrameRate puts it back once
// CaptureUnderway() is false. (The frame the switch lands in may show its
// first half stale in the emulated window: one frame, never in a photo.)
void HoldEmulatedEveryFrame() {
  if (rex::cvar::Query<int32_t>("draw_every_nth_frame") > 1) {
    rex::cvar::SetFlagByName("draw_every_nth_frame", "1");
  }
}

// photos/photo_<date>_<time>_<which>.png (local time, with milliseconds so
// two quick presses don't collide), creating the folder if needed.
std::filesystem::path PhotoPath(const std::string& stamp, const char* which) {
  std::filesystem::path dir = REXCVAR_GET(photo_dir);
  if (dir.empty()) dir = game_folder::UserFolder() / "photos";  // game_folder.h
  std::error_code error;
  std::filesystem::create_directories(dir, error);
  return dir / ("photo_" + stamp + "_" + which + ".png");
}
std::string PhotoStamp() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  const int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(
                         now.time_since_epoch())
                         .count() %
                     1000);
  std::tm local{};
  localtime_r(&t, &local);
  char buffer[48];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d_%H%M%S", &local);
  char with_ms[64];
  std::snprintf(with_ms, sizeof(with_ms), "%s-%03d", buffer, ms);
  return with_ms;
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                               g.start)
      .count();
}

// The trigger file exists (--debug_native_ab_trigger): capture now. Checked
// once per frame, only when the flag is set.
bool Triggered() {
  const std::string& trigger = REXCVAR_GET(debug_native_ab_trigger);
  std::error_code error;
  if (trigger.empty() || !std::filesystem::exists(trigger, error)) {
    return false;
  }
  std::filesystem::remove(trigger, error);
  return true;
}

bool Active() {
  if (!g.parsed) {
    g.parsed = true;
    const std::string& list = REXCVAR_GET(debug_native_ab_ms);
    size_t pos = 0;
    while (pos < list.size()) {
      const size_t comma = std::min(list.find(',', pos), list.size());
      const std::string item = list.substr(pos, comma - pos);
      if (!item.empty()) {
        g.times.push_back(std::stoll(item));
      }
      pos = comma + 1;
    }
    std::sort(g.times.begin(), g.times.end());
  }
  return !g.times.empty() || !REXCVAR_GET(debug_native_ab_trigger).empty() ||
         g_photo_request.load(std::memory_order_relaxed) == kPhotoPair ||
         g.stage != Stage::kIdle;
}

// Where the marker pixels of `texture` are (see Region).
bool FindMarkerPixels(const TextureCache& textures, const GuestTexture& texture, Region& out) {
  // The size, from the fetch constant (bits 0-12 / 13-25 of word 2 = width - 1
  // / height - 1 for 2D textures, as in xenos::xe_gpu_texture_fetch_t).
  const uint32_t width = (texture.fetch[2] & 0x1FFF) + 1;
  const uint32_t height = ((texture.fetch[2] >> 13) & 0x1FFF) + 1;
  if (width < kMarkerPixels) {
    return false;
  }
  for (uint32_t i = 0; i < kMarkerPixels; ++i) {
    if (!textures.TexelAddress(texture, i, 0, out.pixels[i]) ||
        !textures.TexelAddress(texture, width - 1 - i, height - 1,
                               out.pixels[kMarkerPixels + i])) {
      return false;
    }
  }
  return true;
}

void WriteMarker(uint8_t* base, const Region& r) {
  for (uint32_t address : r.pixels) {
    std::memcpy(base + address, &kMarker, 4);
  }
}

bool MarkerGone(const uint8_t* base, const Region& r) {
  bool intact[2] = {true, true};
  for (uint32_t i = 0; i < 2 * kMarkerPixels; ++i) {
    uint32_t v;
    std::memcpy(&v, base + r.pixels[i], 4);
    intact[i / kMarkerPixels] &= v == kMarker;
  }
  return !intact[0] && !intact[1];
}

// Waits (up to 3 s) until the emulated GPU has overwritten the marker.
bool AwaitMarker(const uint8_t* base, const Region& r) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!MarkerGone(base, r)) {
    if (std::chrono::steady_clock::now() > deadline) {
      REXLOG_WARN(
          "ab_capture: the emulated frontbuffer never arrived in guest memory "
          "(is --readback_resolve=full set?)");
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  // The copy may still be finishing between the two ends: give it a moment.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  return true;
}

// Writes R, G, B of `pixels` (4 bytes per pixel, `stride` bytes per row) as
// a binary PPM (the same format as DebugFrameCapture's).
void WritePpm(const std::filesystem::path& path, const uint8_t* pixels, uint32_t width,
              uint32_t height, size_t stride) {
  std::FILE* f = std::fopen(path.string().c_str(), "wb");
  if (!f) {
    REXLOG_WARN("ab_capture: can't write {}", path.string());
    return;
  }
  std::fprintf(f, "P6\n%u %u\n255\n", width, height);
  std::vector<uint8_t> row(size_t(width) * 3);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* src = pixels + y * stride;
    for (uint32_t x = 0; x < width; ++x) {
      row[x * 3 + 0] = src[x * 4 + 0];
      row[x * 3 + 1] = src[x * 4 + 1];
      row[x * 3 + 2] = src[x * 4 + 2];
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
}

// One capture is over (saved or given up): back to waiting; a time that
// has come is used up.
void FinishOne() {
  g.stage = Stage::kIdle;
  if (!g.times.empty() && NowMs() >= g.times.front()) {
    g.times.erase(g.times.begin());
  }
}

// The renderer's texture cache (it knows where a texture lives in memory),
// remembered from AfterPresent, which runs every frame.
const TextureCache* g_textures = nullptr;

}  // namespace

void BeforeFrontbufferResolve(uint8_t* base, const GuestTexture& frontbuffer) {
  if (!Active() || !g_textures) {
    return;
  }
  if (EmulatedGpuIdle()) {
    // Nothing of the emulated GPU's will land: no marker, no waiting.
    // AfterPresent takes a pending photo with our picture alone. A capture
    // already under way (F9 into native-only between its two frames) can't
    // finish: drop it.
    if (g.stage != Stage::kIdle) {
      g.stage = Stage::kIdle;
      g.photo = false;
      RestoreReadback();
    }
    return;
  }
  Region region;
  if (!FindMarkerPixels(*g_textures, frontbuffer, region)) {
    return;
  }
  switch (g.stage) {
    case Stage::kIdle: {
      int pair = kPhotoPair;
      const bool photo = g_photo_request.compare_exchange_strong(pair, 0);
      if (photo || (!g.times.empty() && NowMs() >= g.times.front()) || Triggered()) {
        // A photo: the emulated GPU must copy its pictures back from now on
        // (this resolve included: the GPU runs it after this call).
        g.photo = photo;
        if (photo) {
          SwitchReadbackOn();
        }
        // This frame's frontbuffer gets a marker; the NEXT frame is captured.
        HoldEmulatedEveryFrame();
        WriteMarker(base, region);
        g.previous = region;
        g.stage = Stage::kMarkedPrevious;
      }
      break;
    }
    case Stage::kMarkedPrevious:
      // Once the previous frame's picture has landed, nothing older is
      // still on its way: mark this one.
      if (!AwaitMarker(base, g.previous)) {
        FinishOne();
        g.photo = false;
        RestoreReadback();
        break;
      }
      WriteMarker(base, region);
      g.target = region;
      g.target_texture = frontbuffer;
      g.target_ms = NowMs();
      g.stage = Stage::kMarkedTarget;
      break;
    case Stage::kMarkedTarget:
      break;  // AfterPresent didn't run (native picture off?): wait for it
  }
}

void WriteDrawList(const Frame& frame, const std::filesystem::path& path,
                   const char* (*material_name)(Material), const TextureCache& textures) {
  std::FILE* file = std::fopen(path.string().c_str(), "w");
  if (!file) {
    REXLOG_WARN("Photo (F10): can't write {}", path.string());
    return;
  }
  std::fprintf(file,
               "# Native renderer draw list of the photo's frame %llu (ab_capture.h, "
               "WriteDrawList)\n"
               "# index material | geometry | blend on op src dst | depth test/write/func | "
               "cull | alpha test func ref | fade fade_rgb | fog start end | textured | "
               "screen rect x0 y0 x1 y1 (immediate only), vertices behind the camera\n",
               (unsigned long long)frame.number);
  std::fprintf(file, "# F11 highlight %d, F12 particle experiment: %s\n",
               int(spotter::Highlighting()), spotter::ParticleExperimentName());
  for (size_t i = 0; i < frame.draws.size(); ++i) {
    const DrawCommand& d = frame.draws[i];
    std::fprintf(file, "%4zu %-22s | %s %u%s | blend %d %u %u %u | depth %d %d %u | cull %u | "
                       "atest %d %u %.3f | fade %.3f %d | fog %d %.1f %.1f | tex %d",
                 i, material_name(d.material), d.mesh ? "mesh" : "imm",
                 d.index_count ? d.index_count : d.vertex_count, d.index_count ? " idx" : "",
                 d.blend.enable, d.blend.op, d.blend.src, d.blend.dst, d.depth_test, d.depth_write,
                 d.depth_func, d.cull, d.alpha_test, d.alpha_func, d.alpha_ref, d.fade, d.fade_rgb,
                 d.fog, d.fog_start, d.fog_end, d.textured);
    // Where immediate geometry lands: its vertices through the MVP matrix
    // (column-major, D3D clip space: y up), in 1280x720 pixels. Vertices
    // with w <= 0 are behind the camera (the GPU clips those triangles).
    if (!d.mesh && d.layout.stride && d.vertex_count) {
      float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
      uint32_t behind = 0;
      const uint8_t* bytes = reinterpret_cast<const uint8_t*>(frame.vertex_data.data());
      const size_t size = frame.vertex_data.size() * 4;
      for (uint32_t v = 0; v < d.vertex_count; ++v) {
        const size_t at = size_t(d.vertex_offset) + size_t(v) * d.layout.stride;
        if (at + 12 > size) {
          break;
        }
        float p[4] = {0, 0, 0, 1};
        std::memcpy(p, bytes + at, 12);
        float c[4];
        for (int r = 0; r < 4; ++r) {
          c[r] = d.mvp[r] * p[0] + d.mvp[4 + r] * p[1] + d.mvp[8 + r] * p[2] + d.mvp[12 + r];
        }
        if (c[3] <= 0.0f) {
          ++behind;
          continue;
        }
        const float sx = (0.5f + 0.5f * c[0] / c[3]) * 1280.0f;
        const float sy = (0.5f - 0.5f * c[1] / c[3]) * 720.0f;
        x0 = std::min(x0, sx), y0 = std::min(y0, sy), x1 = std::max(x1, sx), y1 = std::max(y1, sy);
      }
      if (x0 <= x1) {
        std::fprintf(file, " | rect %.0f %.0f %.0f %.0f", x0, y0, x1, y1);
      } else {
        std::fprintf(file, " | rect none");
      }
      std::fprintf(file, " behind %u", behind);
    }
    // The unlit tint (VS c100) and, for lit materials, the ambient light and
    // the first light's colour (the game's lighting code applies its colour
    // tint extension to those): to see a scene-wide colour change.
    std::fprintf(file, " | tint %.3f %.3f %.3f", d.tint[0], d.tint[1], d.tint[2]);
    if ((d.material == Material::kLitSimple || d.material == Material::kCharacter ||
         d.material == Material::kReflect) &&
        d.block < frame.blocks.size()) {
      const LitParams& l = frame.blocks[d.block].lit;
      std::fprintf(file, " | ambient %.3f %.3f %.3f light0 %.3f %.3f %.3f", l.ambient[0],
                   l.ambient[1], l.ambient[2], l.light_colour[0][0], l.light_colour[0][1],
                   l.light_colour[0][2]);
    }
    // The simple materials' proximity lights and Fx shadows (docs/findings/20):
    // count, then per light its colour, position and parameters (brightness,
    // radius, intensity).
    if ((d.material == Material::kSimple || d.material == Material::kLitSimple) &&
        d.block < frame.blocks.size()) {
      const LitParams& l = frame.blocks[d.block].lit;
      std::fprintf(file, " | proximity %u", l.counts[1]);
      for (uint32_t i = 0; i < l.counts[1] && i < 4; ++i) {
        std::fprintf(file, " [colour %.3f %.3f %.3f at %.1f %.1f %.1f params %.2f %.2f %.2f]",
                     l.point_colour[i][0], l.point_colour[i][1], l.point_colour[i][2],
                     l.point_position[i][0], l.point_position[i][1], l.point_position[i][2],
                     l.point_params[i][0], l.point_params[i][1], l.point_params[i][2]);
      }
      std::fprintf(file, " fxshadows %u", l.counts[2]);
    }
    // Particles: the soft edge's numbers (particle.frag).
    if (d.material == Material::kParticle && d.block < frame.blocks.size()) {
      const ParticleParams& p = frame.blocks[d.block].particle;
      std::fprintf(file, " | particle flags 0x%X fade %.3f falloff %.3f nearfar %.4f %.4f",
                   p.flags[0], p.fade_falloff_near_far[0], p.fade_falloff_near_far[1],
                   p.fade_falloff_near_far[2], p.fade_falloff_near_far[3]);
      // The sprite's texture: fetch constant words 0-1 (word 1 bits 0-5 =
      // format; 2 = k_8 palettized, 6 = k_8_8_8_8, 18-20 = DXT) and the
      // first corners' raw colour words (host order, read as B8G8R8A8:
      // 0xAARRGGBB) and UVs: to check channel order and palettes.
      std::fprintf(file, " | texfetch %08X %08X %08X %08X %08X %08X pal %d", d.texture.fetch[0],
                   d.texture.fetch[1], d.texture.fetch[2], d.texture.fetch[3], d.texture.fetch[4],
                   d.texture.fetch[5], d.palette.present());
      std::vector<uint8_t> rgba;
      uint32_t tw = 0, th = 0;
      if (d.textured && textures.ReadRgba8(d.texture, rgba, tw, th) && !rgba.empty()) {
        double mean[4] = {};
        for (size_t t = 0; t + 3 < rgba.size(); t += 4) {
          for (int c = 0; c < 4; ++c) mean[c] += rgba[t + c];
        }
        const double n = double(rgba.size() / 4);
        std::fprintf(file, " | texmean %.0f %.0f %.0f %.0f", mean[0] / n, mean[1] / n,
                     mean[2] / n, mean[3] / n);
      }
      if (!d.mesh && d.layout.colour != VertexLayout::kAbsent) {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(frame.vertex_data.data());
        const size_t size = frame.vertex_data.size() * 4;
        std::fprintf(file, " | colours");
        for (uint32_t v = 0; v < std::min<uint32_t>(d.vertex_count, 6); ++v) {
          const size_t at = size_t(d.vertex_offset) + size_t(v) * d.layout.stride;
          if (at + d.layout.stride > size) {
            break;
          }
          uint32_t colour = 0;
          float uv[2] = {};
          std::memcpy(&colour, bytes + at + d.layout.colour, 4);
          if (d.layout.uv != VertexLayout::kAbsent) {
            std::memcpy(uv, bytes + at + d.layout.uv, 8);
          }
          std::fprintf(file, " %08X(%.2f,%.2f)", colour, uv[0], uv[1]);
        }
      }
    }
    std::fprintf(file, "\n");
  }
  std::fclose(file);
}

namespace {

// A photo while the emulated GPU draws nothing (EmulatedGpuIdle): our
// picture, as the window shows it, and its draw list; no emulated half.
void SaveNativeOnlyPhoto(rex::ui::Presenter* presenter, const TextureCache& textures,
                         const Frame& frame, const char* (*material_name)(Material)) {
  rex::ui::RawImage image;
  if (!presenter->CaptureGuestOutput(image)) {
    REXLOG_WARN("Photo (F10): couldn't capture the native picture");
    return;
  }
  const std::string stamp = PhotoStamp();
  const std::filesystem::path path = PhotoPath(stamp, "native");
  if (!WritePng(path, image.data.data(), image.width, image.height, image.stride)) {
    REXLOG_WARN("Photo (F10): can't write {}", path.string());
    return;
  }
  std::filesystem::path list = PhotoPath(stamp, "draws");
  list.replace_extension(".txt");
  WriteDrawList(frame, list, material_name, textures);
  REXLOG_INFO("Photo (F10): {} (native only: the emulated GPU is off, so there is no "
              "emulated picture to pair it with)",
              path.string());
}

}  // namespace

void AfterPresent(rex::ui::Presenter* presenter, const TextureCache& textures,
                  const std::array<uint32_t, 256>& gamma_ramp, const Frame& frame,
                  const char* (*material_name)(Material)) {
  g_textures = &textures;
  // F10 was pressed while the emulated picture was shown, then F9: now
  // both pictures can be had.
  int screen = kPhotoScreen;
  g_photo_request.compare_exchange_strong(screen, kPhotoPair);
  if (!Active()) {
    return;
  }
  if (EmulatedGpuIdle()) {
    // --native_only (ab_capture.h, NATIVE ONLY): BeforeFrontbufferResolve
    // left the request alone; take it here with our picture only.
    int pair = kPhotoPair;
    if (g_photo_request.compare_exchange_strong(pair, 0)) {
      SaveNativeOnlyPhoto(presenter, textures, frame, material_name);
    }
    // A debug A/B capture that came due has nothing to pair with: use it up.
    if ((!g.times.empty() && NowMs() >= g.times.front()) || Triggered()) {
      REXLOG_WARN("ab_capture: skipped (--native_only: the emulated GPU draws nothing)");
      FinishOne();
    }
    return;
  }
  if (g.stage != Stage::kMarkedTarget) {
    return;
  }
  FinishOne();
  const bool photo = g.photo;
  g.photo = false;
  // Whatever happens below, the emulated GPU stops copying back once this
  // frame's picture has landed (or never will).
  struct Restore {
    ~Restore() { RestoreReadback(); }
  } restore;
  const std::filesystem::path dir = REXCVAR_GET(debug_capture_dir);
  if (!photo && dir.empty()) {
    REXLOG_WARN("ab_capture: needs --debug_capture_dir");
    return;
  }
  const uint8_t* base = guest::Base();
  if (!AwaitMarker(base, g.target)) {
    return;
  }
  const std::string stamp = photo ? PhotoStamp() : std::string();
  char name[64];
  // 1. Ours, as the window shows it (gamma applied by our present pass).
  rex::ui::RawImage image;
  if (presenter->CaptureGuestOutput(image)) {
    if (photo) {
      WritePng(PhotoPath(stamp, "native"), image.data.data(), image.width, image.height,
               image.stride);
      std::filesystem::path list = PhotoPath(stamp, "draws");
      list.replace_extension(".txt");
      WriteDrawList(frame, list, material_name, textures);
    } else {
      std::snprintf(name, sizeof(name), "ab_%09lldms_native.ppm", (long long)g.target_ms);
      WritePpm(dir / name, image.data.data(), image.width, image.height, image.stride);
      std::snprintf(name, sizeof(name), "ab_%09lldms_draws.txt", (long long)g.target_ms);
      WriteDrawList(frame, dir / name, material_name, textures);
    }
  }
  // 2. The emulated GPU's, from the frontbuffer, through the gamma ramp
  //    (each entry: 10 bits blue | green << 10 | red << 20).
  std::vector<uint8_t> rgba;
  uint32_t width, height;
  if (!textures.ReadRgba8(g.target_texture, rgba, width, height)) {
    REXLOG_WARN("ab_capture: can't decode the frontbuffer");
    return;
  }
  auto ramp = [&](uint8_t v, uint32_t shift) {
    return uint8_t((((gamma_ramp[v] >> shift) & 1023) * 255 + 511) / 1023);
  };
  for (size_t i = 0; i < size_t(width) * height; ++i) {
    uint8_t* p = rgba.data() + 4 * i;
    p[0] = ramp(p[0], 20);
    p[1] = ramp(p[1], 10);
    p[2] = ramp(p[2], 0);
  }
  if (photo) {
    const std::filesystem::path path = PhotoPath(stamp, "emulated");
    WritePng(path, rgba.data(), width, height, size_t(width) * 4);
    REXLOG_INFO("Photo (F10): {} (+ _native.png, the same frame from both renderers)",
                path.string());
    return;
  }
  std::snprintf(name, sizeof(name), "ab_%09lldms_emulated.ppm", (long long)g.target_ms);
  WritePpm(dir / name, rgba.data(), width, height, size_t(width) * 4);
  REXLOG_INFO("ab_capture: saved frame at {} ms (native + emulated)", g.target_ms);
}

bool CaptureUnderway() { return g.stage != Stage::kIdle; }

void RequestPhoto(bool native_shown) {
  g_photo_request.store(native_shown ? kPhotoPair : kPhotoScreen);
}

void WhileEmulatedShown(rex::ui::Presenter* presenter) {
  // A two-picture photo under way can't finish without our picture.
  if (g.photo && g.stage != Stage::kIdle) {
    g.stage = Stage::kIdle;
    g.photo = false;
    RestoreReadback();
  }
  // Any pending photo: the screen, as the window shows it.
  if (g_photo_request.exchange(0) == 0) {
    return;
  }
  rex::ui::RawImage image;
  if (!presenter->CaptureGuestOutput(image)) {
    REXLOG_WARN("Photo (F10): couldn't capture the screen");
    return;
  }
  const std::filesystem::path path = PhotoPath(PhotoStamp(), "emulated");
  if (WritePng(path, image.data.data(), image.width, image.height, image.stride)) {
    REXLOG_INFO("Photo (F10): {} (the emulated picture; F9 first for both)", path.string());
  } else {
    REXLOG_WARN("Photo (F10): can't write {}", path.string());
  }
}

}  // namespace native::ab_capture
