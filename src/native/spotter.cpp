// =============================================================================
// native/spotter.cpp -- see spotter.h for the why and how
// =============================================================================

#include "spotter.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ui/keybinds.h>

#include "../guest_memory.h"
#include "../pddi/intercept.h"
#include "../pddi/trace.h"
#include "ab_capture.h"
#include "frame.h"

REXCVAR_DEFINE_STRING(debug_bump_off, "", "CrashMoM",
                      "Debug: bump material features to switch off in BOTH pictures, as pixel "
                      "shader bool numbers (10 rim, 11 see-through, 12 reflection...), e.g. 10,11")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace native::spotter {
namespace {

using Clock = std::chrono::steady_clock;

// What we hunt: a material's setup function (its xn*Shader vtable slot 14),
// its class name, and what it looks like for the banner.
struct Hunted {
  pddi::FnId setup;
  const char* name;
  const char* description;
};
constexpr Hunted kHunted[] = {
    // Written 2026-09-27 (shaders/bumpmega.*). VERIFIED 2026-09-29: the
    // TK blocks and see-through ice, 0.43/255 off the emulated picture
    // (docs/findings/20 section 6).
    {pddi::kFn_82436308, "xnBumpMegaShader", "the bump material"},
};
constexpr size_t kCount = sizeof(kHunted) / sizeof(kHunted[0]);

// "Gone" after this long without a setup; coming back after it = new banner.
constexpr auto kGoneAfter = std::chrono::seconds(10);

struct State {
  std::atomic<uint32_t> setups{0};  // this frame (handler, any thread)
  bool on_screen = false;           // the rest: frame-end listener only
  bool seen_this_session = false;
  Clock::time_point first_seen, last_seen;
  uint32_t most_in_a_frame = 0;
};
State g_state[kCount];

// F11's highlight: which material is painted magenta (spotter.h, Highlight).
std::atomic<Highlight> g_highlight{Highlight::kOff};
// F12's particle experiment: index into kExperiments.
struct Experiment {
  uint32_t flags;
  const char* name;
  ParticleErase erase = ParticleErase::kNone;
};
constexpr Experiment kExperiments[] = {
    {0, "normal"},
    {kParticleNoSoft, "no soft edge"},
    {kParticleNoFog, "no fog"},
    {kParticleWhiteTex, "no texture (white)"},
    {kParticleWhiteColour, "no vertex colour (white)"},
    {kParticleBaseMip, "base mip level only"},
    {kParticleSmallestMip, "smallest mip level only"},
    {0, "BOTH pictures: big particle draws (300+ corners) erased", ParticleErase::kBig},
    {0, "BOTH pictures: additive particles erased", ParticleErase::kAdditive},
    {0, "BOTH pictures: see-through (alpha-blended) particles erased", ParticleErase::kAlpha},
};
std::atomic<uint32_t> g_experiment{0};
bool (*g_native_drawn)(void*) = nullptr;
void* g_user = nullptr;

// BOTH-PICTURES FEATURE SWITCH for the bump material (docs/findings/44):
// --debug_bump_off=10,11 (or the console's `flag debug_bump_off 11`) clears
// those pixel shader bools right after the game's own bump setup, in D3D's
// copy of the constants, and marks them dirty (bit 56 of device +0x20, what
// SetPixelShaderConstantB 0x82312B58 does). The emulated GPU uploads them at
// the draw, our recorder reads them: both pictures lose the feature, so an
// A/B tells whether OUR version of that feature matches. Bools (guest.h,
// frame.h kBumpMega*): 10 rim, 11 see-through (refraction), 12 reflection,
// 13 reflection mask, 14/15 palettized diffuse / normal map, 16 refraction x
// colour. The shader's device: *(*(shader +0x14) +4) (as its setups pass it).
void ClearBumpBools(uint8_t* base, uint32_t shader, const std::string& list) {
  const uint32_t state = rex::memory::load_and_swap<uint32_t>(GuestPtr(base, shader + 0x14));
  const uint32_t dev = rex::memory::load_and_swap<uint32_t>(GuestPtr(base, state + 4));
  for (const char* p = list.c_str(); *p;) {
    const int n = std::atoi(p);
    if (n >= 0 && n < 128) {
      uint8_t* word = GuestPtr(base, dev + 10128 + 4 * uint32_t(n / 32));
      rex::memory::store_and_swap<uint32_t>(
          word, rex::memory::load_and_swap<uint32_t>(word) & ~(1u << (n % 32)));
    }
    while (*p && *p != ',') ++p;
    if (*p == ',') ++p;
  }
  uint8_t* dirty = GuestPtr(base, dev + 0x20);
  rex::memory::store_and_swap<uint64_t>(
      dirty, rex::memory::load_and_swap<uint64_t>(dirty) | 0x100000000000000ull);
}

// One handler per hunted material (the handler gets no "which one", so a
// template instance per row).
template <size_t I>
void OnSetup(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  g_state[I].setups.fetch_add(1, std::memory_order_relaxed);
  const uint32_t shader = ctx.r3.u32;
  original(ctx, base);
  if (I == 0) {  // kHunted[0] = the bump material
    const std::string off = REXCVAR_GET(debug_bump_off);
    if (!off.empty()) ClearBumpBools(base, shader, off);
  }
}
template <size_t... I>
constexpr pddi::Handler HandlerOf(size_t i, std::index_sequence<I...>) {
  constexpr pddi::Handler handlers[] = {&OnSetup<I>...};
  return handlers[i];
}
pddi::Handler HandlerOf(size_t i) { return HandlerOf(i, std::make_index_sequence<kCount>{}); }

void OnFrameEnd(void*) {
  const Clock::time_point now = Clock::now();
  for (size_t i = 0; i < kCount; ++i) {
    State& s = g_state[i];
    const Hunted& h = kHunted[i];
    const uint32_t setups = s.setups.exchange(0, std::memory_order_relaxed);
    if (setups) {
      if (!s.on_screen) {
        s.on_screen = true;
        s.first_seen = now;
        s.most_in_a_frame = 0;
        // WARN level: stands out in any log viewer, and play.sh colours it.
        // "Drawn nearby", not "on screen": the game also draws objects hidden
        // behind others (the GPU's depth test drops them) or just past the
        // screen's edge (it culls by rough bounding boxes). Seen in a
        // playtest: the banner with no TK block in view, one nearby. F11
        // shows where it really is.
        REXLOG_WARN("*** SPOTTED: {} ({}) is being drawn nearby: {} setup(s) this frame (it "
                    "may be hidden behind something; Tools -> Highlight paints it magenta). Compare both "
                    "pictures (tools/play.sh --dual) and take a photo (Tools) if they differ ***",
                    h.description, h.name, setups);
        if (!s.seen_this_session) {
          // First time this session: take a photo (a pair of
          // both pictures when ours is drawn; plus a frame trace with --trace).
          s.seen_this_session = true;
          const bool native = g_native_drawn && g_native_drawn(g_user);
          ab_capture::RequestPhoto(native);
          pddi::trace::RequestTrace();
          REXLOG_WARN("*** SPOTTED: first time this session: an F10 photo is taken for you ***");
        }
      }
      s.last_seen = now;
      if (setups > s.most_in_a_frame) {
        s.most_in_a_frame = setups;
      }
    } else if (s.on_screen && now - s.last_seen >= kGoneAfter) {
      s.on_screen = false;
      const double seconds =
          std::chrono::duration<double>(s.last_seen - s.first_seen).count();
      REXLOG_INFO("Spotter: {} ({}) no longer drawn: drawn for {:.0f} s, at most {} setup(s) "
                  "in one frame",
                  h.description, h.name, seconds, s.most_in_a_frame);
    }
  }
}

}  // namespace

void Install(bool (*native_drawn)(void* user), void* user) {
  g_native_drawn = native_drawn;
  g_user = user;
  for (size_t i = 0; i < kCount; ++i) {
    pddi::SetHandler(kHunted[i].setup, HandlerOf(i));
  }
  pddi::AddFrameEndListener(&OnFrameEnd, nullptr);
  rex::ui::RegisterBind(
      "bind_highlight", "" /* Tools menu (was F11) */,
      "Paint one material magenta in the native picture (cycles: bump, particles, motion trails, "
      "off)",
      [] {
        // off -> bump -> particles -> motion trails -> off
        Highlight next;
        switch (g_highlight.load(std::memory_order_relaxed)) {
          case Highlight::kOff: next = Highlight::kBumpMega; break;
          case Highlight::kBumpMega: next = Highlight::kParticles; break;
          case Highlight::kParticles: next = Highlight::kMotionTrails; break;
          default: next = Highlight::kOff; break;
        }
        g_highlight.store(next, std::memory_order_relaxed);
        static const char* const kWhat[] = {"off: everything drawn normally",
                                            "the bump material is MAGENTA",
                                            "every PARTICLE is a magenta patch (even invisible ones)",
                                            "every MOTION TRAIL is magenta"};
        REXLOG_WARN("Spotter: highlight (F11): {} in the native picture", kWhat[int(next)]);
      });
  rex::ui::RegisterBind(
      "bind_particle_experiment", "" /* Tools menu (was F12) */,
      "Particle experiments in the native picture (cycles: no soft edge, no fog, no texture, "
      "no vertex colour, base mip only, smallest mip only, then erasing groups of particles in "
      "both pictures, normal)",
      [] {
        constexpr uint32_t kCount = sizeof(kExperiments) / sizeof(kExperiments[0]);
        const uint32_t next = (g_experiment.load(std::memory_order_relaxed) + 1) % kCount;
        g_experiment.store(next, std::memory_order_relaxed);
        REXLOG_WARN("Spotter: particle experiment (F12): {} in the native picture",
                    kExperiments[next].name);
      });
}

Highlight Highlighting() { return g_highlight.load(std::memory_order_relaxed); }

uint32_t ParticleExperimentFlags() {
  return kExperiments[g_experiment.load(std::memory_order_relaxed)].flags;
}
ParticleErase ParticleEraseMode() {
  return kExperiments[g_experiment.load(std::memory_order_relaxed)].erase;
}
const char* ParticleExperimentName() {
  return kExperiments[g_experiment.load(std::memory_order_relaxed)].name;
}

void Uninstall() {
  pddi::RemoveFrameEndListener(&OnFrameEnd, nullptr);
  for (size_t i = 0; i < kCount; ++i) {
    pddi::SetHandler(kHunted[i].setup, nullptr);
  }
  g_native_drawn = nullptr;
}

}  // namespace native::spotter
