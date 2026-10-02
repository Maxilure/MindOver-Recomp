// =============================================================================
// native/native_window.h -- dual mode: a second window with our picture
// =============================================================================
//
// WHAT (2026-09-27)
//   One game, TWO windows: the main window shows the emulated Xbox 360 GPU's
//   picture, a second window ("crash_mom: native renderer") shows our native
//   Vulkan renderer's picture of the same frames, live. Differences between
//   the two renderers can then be spotted while playing, anywhere in the
//   game, without the F9 back-and-forth.
//
//   Open it at start with --native_window (tools/play.sh --dual), or any
//   time with F8 (keybind "bind_native_window"); F8 again, or the window's
//   close button, closes it. Closing it does NOT quit the game: the main
//   window just goes back to what F9 had chosen.
//
// HOW
//   * The window is an ordinary SDK window (rex::ui::Window, an SDL window
//     on Linux), created on the UI thread like the main one.
//   * It gets its OWN presenter (the object that puts a picture into a
//     window), made by the same graphics provider as the main window's, so
//     both share one Vulkan device and our renderer's images work in either.
//   * NativeRenderer::SetWindowPresenter points the renderer at it: from the
//     next frame on our picture goes into this presenter, and the main
//     window's presenter is handed back to the emulated GPU.
//   * The window and its presenter are made the first time it opens and kept
//     until the game exits (closing only hides the window; F8 reopens it), so
//     the game's main thread, which may be refreshing the presenter at that
//     very moment, never sees it destroyed.
//   * An empty "UI drawer" is attached to the presenter. With none, the SDK
//     would present each new picture straight from the thread that made it
//     (the game's main thread), which can then wait for the desktop's
//     compositor (e.g. a Wayland window on another workspace doesn't give its
//     images back). With one, the window repaints on the UI thread, like the
//     main window (whose overlays are UI drawers too).
//   * Key presses in this window go through the same key binds as in the
//     main one (F8, F9, F10, F3, F4...).
//   * The controller keeps working while THIS window is focused: the app
//     widens the SDK's "is the game window focused?" input check to both
//     windows (crash_mom_app.h). Keyboard-as-controller (--mnk_mode) still
//     needs the main window focused.
//
// TIMING
//   The two pictures show the same game frame, but not at the same moment:
//   ours is drawn when the game's main thread finishes a frame, while the
//   emulated GPU works through that frame's commands a little later. So the
//   emulated window can lag ours by up to about one frame. For an exact
//   comparison of one frame, F10 saves both pictures of the same frame.
// =============================================================================

#pragma once

#include <atomic>
#include <memory>
#include <string>

#include <rex/cvar.h>
#include <rex/ui/presenter.h>
#include <rex/ui/ui_drawer.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

REXCVAR_DECLARE(bool, native_window);

namespace rex::ui {
class GraphicsProvider;
class WindowedAppContext;
}  // namespace rex::ui

class NativeRenderer;

class NativeWindow : public rex::ui::WindowListener, public rex::ui::WindowInputListener {
 public:
  // UI thread. `main_window`: the game's window (its title says "emulated"
  // while this one is open). `focused`: kept up to date with whether THIS
  // window has the keyboard focus (read by the app's input check). The main
  // window and the flag must outlive this object. Null (logged) if the
  // window or its presenter can't be made. Doesn't open it yet (Toggle).
  static std::unique_ptr<NativeWindow> Create(rex::ui::WindowedAppContext& app_context,
                                              rex::ui::GraphicsProvider& provider,
                                              NativeRenderer& renderer,
                                              rex::ui::Window* main_window,
                                              std::atomic<bool>& focused);
  // UI thread. Destroy the RENDERER FIRST: until then the game's main thread
  // may be drawing into this window's presenter.
  ~NativeWindow() override;

  NativeWindow(const NativeWindow&) = delete;
  NativeWindow& operator=(const NativeWindow&) = delete;

  // UI thread. Opens the window if it's closed, closes it if it's open.
  void Toggle();
  bool is_open() const { return window_->phase() == rex::ui::Window::Phase::kOpen; }
  // The window itself (the keyboard driver listens to its keys too).
  rex::ui::Window* window() const { return window_.get(); }

 private:
  // Draws nothing: its presence makes the presenter paint on the UI thread
  // (see HOW above).
  class NoOverlay : public rex::ui::UIDrawer {
   public:
    void Draw(rex::ui::UIDrawContext&) override {}
  };

  NativeWindow(NativeRenderer& renderer, rex::ui::Window* main_window, std::atomic<bool>& focused)
      : renderer_(renderer), main_window_(main_window), focused_(focused) {}
  void Open();

  // WindowListener: every event arrives on the UI thread.
  void OnClosing(rex::ui::UIEvent& e) override;
  void OnGotFocus(rex::ui::UISetupEvent& e) override;
  void OnLostFocus(rex::ui::UISetupEvent& e) override;
  // WindowInputListener: F-key binds work in this window too.
  void OnKeyDown(rex::ui::KeyEvent& e) override;

  NativeRenderer& renderer_;
  rex::ui::Window* main_window_;
  std::atomic<bool>& focused_;
  std::string main_title_;  // the main window's own title, restored on close
  // Made in this order, destroyed by hand in the destructor (window first:
  // it has to let go of the presenter).
  std::unique_ptr<rex::ui::Presenter> presenter_;
  NoOverlay no_overlay_;
  std::unique_ptr<rex::ui::Window> window_;
};
