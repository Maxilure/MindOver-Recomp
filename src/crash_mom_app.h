// =============================================================================
// crash_mom_app.h -- our application class
// =============================================================================
//
// CrashMomApp is a rex::ReXApp: ReXGlue's ready-made "recompiled game"
// application (window, ImGui overlays, runtime lifecycle). We customize the
// port by overriding the virtual hooks it calls during startup.
//
// Lifecycle, in order (full details: ReXGlue wiki page "ReXApp"):
//
//   OnConfigurePaths    choose where game data / saves live
//   OnPostInitLogging   logging is ready
//   OnPreSetup          tweak runtime config before the fake 360 is built
//   OnCreateDialogs     add our own ImGui overlay windows
//   OnLoadXexImage      swap which .xex gets loaded
//   OnPostLoadXexImage  XEX is in guest memory: patch data here
//   OnPostSetup         everything is initialized
//   OnPreLaunchModule   last chance to patch before the game's main thread runs
//   OnShutdown          cleanup
//
// Built-in debug overlays: F3 = perf/threads, ` (backtick) = log console,
// F4 = settings (CVars).
// =============================================================================

#pragma once

#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/interfaces/graphics.h>

#include "debug_frame_capture.h"
#include "debug_input_script.h"
#include "native/native_renderer.h"

class CrashMomApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  // Factory called by REX_DEFINE_APP in main.cpp. PPCImageConfig comes from
  // the generated code: it describes the image (base 0x82000000, code range
  // 0x820B0000-0x824D0000) and lists every recompiled function.
  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<CrashMomApp>(
        new CrashMomApp(ctx, "crash_mom", PPCImageConfig));
  }

  // Runs after CVars/command line are read, before the GPU/audio/input
  // backends are created.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    // GPU emulation lives in a separate plugin library, and ReXGlue loads
    // NONE by default. Without one, every Vd* graphics kernel call is
    // ignored and the game can never draw.
    // "xenos" = emulate the Xbox 360's Xenos GPU at the command level
    // (librexgpu-xenos*.so next to the exe, copied by CMakeLists.txt).
    // Can still be overridden with --gpu_plugin=<name>.
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }

    // Emulate the 360's EDRAM with the "FSI" render-target path instead of
    // the default "FBO" one. Fixes the black Bink movies, the black Sierra
    // copyright screen, and the title logo vanishing once its drop-in ends.
    //
    // Background: the 360 GPU draws into 10 MB of on-chip memory (EDRAM),
    // then "resolves" (copies) the result to RAM. ReXGlue has two ways to
    // emulate that. FBO maps EDRAM onto ordinary host render targets (fast,
    // but it has to guess how the game's surfaces overlap); FSI keeps a real
    // EDRAM image in a GPU buffer and has pixel shaders read-modify-write it
    // under "fragment shader interlock" (exact, needs a recent GPU).
    //
    // How we found it (docs/findings/06): screenshots from
    // --debug_capture_dir were pure black; per-draw logs showed the idle title
    // frame draws the logo fine, then two depth-only clears on a 640-wide 4x
    // MSAA surface, then the final copy. In FBO mode, skipping just those
    // clears brought the logo back, and --render_target_path_vulkan=fsi fixed
    // everything at the same 30 fps. The movies break FBO in some other way.
    //
    // Only when the user didn't choose (so --render_target_path_vulkan=fbo
    // still works for comparison). The SDK itself falls back to FBO on GPUs
    // without interlock support.
    //
    // Gotcha: that flag is defined INSIDE the GPU plugin library, so it
    // doesn't exist until the plugin is loaded, and the SDK loads it right
    // AFTER this hook (SetFlagByName on a missing flag silently fails; our
    // first attempt did exactly that). So we load the plugin here ourselves;
    // the SDK skips its own load when config.graphics is already set. Loading
    // registers the plugin's flags and replays anything the user gave on the
    // command line / REX_* env / config file, so GetFlagSource then tells us
    // whether the user picked a path. The render-target cache that reads the
    // flag is only created later (SetupGuestGpu), so setting it now is in time.
    if (!config.graphics && !config.gpu_plugin.empty()) {
      // On failure this stays null and the SDK retries + reports the error.
      config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin);
    }
    if (rex::cvar::GetFlagSource("render_target_path_vulkan") == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName("render_target_path_vulkan", "fsi");
    }
    // Requested path, not necessarily the one used (see the fallback above).
    REXLOG_INFO("CrashMoM: render_target_path_vulkan = \"{}\"",
                rex::cvar::GetFlagByName("render_target_path_vulkan"));
  }

  // Everything is initialized, including the window and the presenter
  // (SetupPresentation runs before the runtime is built).
  void OnPostSetup() override {
    // Debug screenshots, only with --debug_capture_dir (see
    // debug_frame_capture.h). The presenter is the SDK object that holds
    // the final picture the emulated GPU sends to the "TV".
    if (auto* gfx = runtime() ? runtime()->graphics_system() : nullptr) {
      frame_capture_.Start(gfx->presenter());
      // Our own Vulkan renderer (roadmap phase 4, native/native_renderer.h).
      // Draws next to the emulated GPU; F9 switches which picture is shown.
      // Null if the presenter isn't Vulkan: then the game just stays on the
      // emulated picture.
      if (gfx->presenter()) {
        native_renderer_ = NativeRenderer::Create(gfx->presenter());
      }
    }
  }

  // Last hook before the game's main thread starts running.
  void OnPreLaunchModule() override {
    // Debug fake controller, only with --debug_input_script and/or
    // --debug_input_fifo (see debug_input_script.h). Created here so its
    // timeline starts at launch. ReXApp's own code casts input_system() the
    // same way.
    auto* input = static_cast<rex::input::InputSystem*>(runtime()->input_system());
    if (auto driver = ScriptedInputDriver::CreateFromCvars(); driver && input) {
      input->AddDriver(std::move(driver));
    }
  }

  // First thing on shutdown, while the presenter still exists (the game's
  // main thread may still be running: the renderer unhooks itself safely).
  void OnShutdown() override {
    native_renderer_.reset();
    frame_capture_.Stop();
  }

  // Other hooks we can override (uncomment + implement as needed):
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
  // void OnPostInitLogging() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}

 private:
  DebugFrameCapture frame_capture_;
  std::unique_ptr<NativeRenderer> native_renderer_;
};
