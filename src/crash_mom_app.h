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
// F4 = settings (CVars). Ours: F2 / Delete = rename / delete a save on the Load /
// Save Game screen (saves/save_library.h), F6 = Controls menu (keyboard + mouse keys,
// input/controls_menu.h), F8 = the native renderer's picture in a second
// window (native/native_window.h), F9 = emulated <-> native picture,
// F10 = photo (native/ab_capture.h).
// =============================================================================

#pragma once

#include <atomic>
#include <map>
#include <string>

#include <rex/graphics/graphics_system.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>

#include "audio_trace.h"
#include "debug_frame_capture.h"
#include "debug_input_script.h"
#include "frame_rate.h"
#include "input/controls_menu.h"
#include "input/keyboard_mouse.h"
#include "input/players.h"
#include "native/native_renderer.h"
#include "overlay_banner.h"
#include "saves/save_library.h"
#include "native/native_window.h"

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
      // The emulated GPU is also asked for the game's display gamma ramp.
      // static_cast: GraphicsSystem is the SDK's only implementation of the
      // interface (a dynamic_cast won't link: its type info lives in the GPU
      // plugin library, which is loaded at runtime).
      if (gfx->presenter()) {
        auto* gpu = static_cast<rex::graphics::GraphicsSystem*>(gfx);
        native_renderer_ = NativeRenderer::Create(gfx->presenter(), gpu->command_processor());
      }
      // Dual mode (native/native_window.h): F8 opens / closes a second window
      // with our picture, the main one keeps the emulated picture.
      // --native_window opens it right away.
      if (native_renderer_) {
        rex::ui::RegisterBind("bind_native_window", "F8",
                              "Open / close a second window with the native renderer's picture "
                              "(the main window then shows the emulated one)",
                              [this] { ToggleNativeWindow(); });
        if (REXCVAR_GET(native_window)) {
          ToggleNativeWindow();
        }
      }
    }
    // F6: the Controls menu (input/controls_menu.h), keyboard + mouse keys.
    rex::ui::RegisterBind("bind_controls_menu", "F6",
                          "Open / close the Controls menu (keyboard and mouse keys)",
                          [] { controls_menu::Toggle(); });
    // F2 / Delete on the Load / Save Game screen: rename / delete the save
    // under the cursor (saves/save_library.h; X / Y do the same).
    rex::ui::RegisterBind("bind_save_rename", "F2",
                          "On the Load / Save Game screen: rename the save under the cursor",
                          [] { save_library::RequestDialog(false); });
    rex::ui::RegisterBind("bind_save_delete", "Delete",
                          "On the Load / Save Game screen: delete the save under the cursor",
                          [] { save_library::RequestDialog(true); });
    // Frame statistics, only with --debug_log_fps / --debug_fps_csv (frame_rate.h).
    frame_rate::StartFrameStats();
    // Sound requests by name, only with --debug_audio_trace (audio_trace.h).
    audio_trace::Start(game_data_root());
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
    // Keyboard + mouse as a virtual controller (input/keyboard_mouse.h), on
    // unless --keyboard_mouse=false. Merged with a real pad into player 1.
    if (auto kbm_driver = kbm::KeyboardMouseDriver::Create(); kbm_driver && input) {
      kbm_driver->SetPausedCheck([this] { return KeyboardInputPaused(); });
      kbm_driver->Attach(window());
      // Which device plays as which player (input/players.h; the Controls
      // menu's Players tab), replacing the SDK's fixed rule (keyboard + first
      // pad = player 1). Before the game's first poll, as the SDK asks.
      std::map<std::string, int> choices;
      for (const auto& [key, choice] : kbm_driver->bindings().players) {
        choices[key] = choice.player;
      }
      input->SetDeviceAssignment(
          std::make_unique<kbm::PlayerAssignment>(kbm::KeyboardMouseDriver::kDeviceId, choices));
      input->AddDriver(std::move(kbm_driver));
    }
  }

  // First thing on shutdown, while the presenter still exists (the game's
  // main thread may still be running: the renderer unhooks itself safely).
  // (Closing the main window ends the process right away instead: ReXApp's
  // OnClosing hard-exits, so this runs only on other ways out.)
  void OnShutdown() override {
    frame_rate::StopFrameStats();
    rex::ui::UnregisterBind("bind_native_window");
    // Renderer first: until it's gone it may be drawing into the native
    // window's presenter (native_window.h).
    native_renderer_.reset();
    native_window_.reset();
    frame_capture_.Stop();
  }

  // Our ImGui overlay windows, drawn over the main window's picture.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    // The short message shown when F9 / F8 switch pictures (overlay_banner.h).
    overlay_banner::Create(drawer);
    // F6: keyboard + mouse keys (input/controls_menu.h), closed at first.
    controls_menu::Create(drawer);
    // The save library's rename / delete box (saves/save_library.h).
    save_library::Create(drawer);
  }

  // Other hooks we can override (uncomment + implement as needed):
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}

 private:
  // F8 / --native_window (UI thread): the second window is made the first
  // time, then only opened and closed (native/native_window.h).
  void ToggleNativeWindow() {
    // --emulated_only: our renderer never draws, so no window for it.
    if (native_renderer_->emulated_only()) {
      overlay_banner::Show("EMULATED ONLY", "F8 is off (--emulated_only)");
      return;
    }
    if (!native_window_) {
      auto* gfx = runtime() ? runtime()->graphics_system() : nullptr;
      auto* provider = gfx ? gfx->provider() : nullptr;
      if (!provider) {
        REXLOG_WARN("NativeWindow: no graphics provider, no second window");
        return;
      }
      native_window_ = NativeWindow::Create(app_context(), *provider, *native_renderer_, window(),
                                            native_window_focused_);
      if (!native_window_) {
        return;
      }
      UseEitherWindowForInput();
    }
    native_window_->Toggle();
    // Keys typed into the second window play too. Closing it detaches the
    // keyboard driver by itself (KeyboardMouseDriver::OnClosing), so attach
    // again on every opening (twice = once).
    if (auto* kbm_driver = kbm::KeyboardMouseDriver::Get(); kbm_driver && native_window_->is_open()) {
      kbm_driver->Attach(native_window_->window());
    }
  }

  // Keyboard + mouse give the game a neutral pad while no game window has the
  // focus, or while an ImGui overlay (F4 settings, the console) is using the
  // keyboard. While ImGui only wants the mouse (pointer over an overlay or a
  // pop-up), just the mouse buttons stop counting: an achievement pop-up
  // under the pointer mustn't freeze a keyboard player mid-fight. Asked from
  // the game's threads at every poll (reading ImGui's flags there is what the
  // SDK's own check does too).
  uint32_t KeyboardInputPaused() {
    using Driver = kbm::KeyboardMouseDriver;
    const bool focused = (window() && window()->HasFocus()) ||
                         native_window_focused_.load(std::memory_order_relaxed);
    if (!focused) {
      return Driver::kPauseAll;
    }
    rex::ui::ImGuiDrawer* drawer = imgui_drawer();
    if (!drawer) {
      return 0;
    }
    return (drawer->GetIO().WantCaptureKeyboard ? Driver::kPauseAll : 0) |
           (drawer->GetIO().WantCaptureMouse ? Driver::kPauseMouse : 0);
  }

  // The SDK feeds the game NO controller input while its window isn't
  // focused (ReXApp::ConstructRuntime's "active" check). With two windows,
  // clicking (or just moving) the native one would freeze Crash. So once the
  // native window exists, the check accepts either window.
  // The SDK's check also pauses input while an overlay (F3/F4/console/F7)
  // is open and ImGui wants the mouse. Which overlays are open is private to
  // ReXApp, so this asks ImGui directly: the same, except that hovering the
  // mouse over the achievement pop-up while it shows pauses input too.
  // Keyboard-as-controller (--mnk_mode) keeps its own focus check on the
  // main window.
  void UseEitherWindowForInput() {
    auto* input = static_cast<rex::input::InputSystem*>(runtime()->input_system());
    if (!input) {
      return;
    }
    input->SetActiveCallback([this]() {
      const bool focused = (window() && window()->HasFocus()) ||
                           native_window_focused_.load(std::memory_order_relaxed);
      if (!focused) {
        return false;
      }
      rex::ui::ImGuiDrawer* drawer = imgui_drawer();
      return !drawer || !drawer->GetIO().WantCaptureMouse;
    });
  }

  DebugFrameCapture frame_capture_;
  // Whether the native window has the keyboard focus (NativeWindow keeps it
  // up to date; the input check above reads it from input threads). Declared
  // before the window, so it outlives it.
  std::atomic<bool> native_window_focused_{false};
  std::unique_ptr<NativeRenderer> native_renderer_;
  std::unique_ptr<NativeWindow> native_window_;
};
