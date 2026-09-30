// =============================================================================
// native/native_window.cpp -- see native_window.h for the why and how
// =============================================================================

#include "native_window.h"

#include <rex/logging.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/windowed_app_context.h>

#include "native_renderer.h"

REXCVAR_DEFINE_BOOL(native_window, false, "CrashMoM",
                    "Dual mode at start: a second window shows the native renderer's picture "
                    "live while the main window shows the emulated one. F8 opens/closes it");

std::unique_ptr<NativeWindow> NativeWindow::Create(rex::ui::WindowedAppContext& app_context,
                                                   rex::ui::GraphicsProvider& provider,
                                                   NativeRenderer& renderer,
                                                   rex::ui::Window* main_window,
                                                   std::atomic<bool>& focused) {
  std::unique_ptr<NativeWindow> self(new NativeWindow(renderer, main_window, focused));
  // Same provider as the main window's presenter = same Vulkan device. The
  // default GPU-loss callback ends the game with an error, like the main
  // window's would (nothing could draw any more anyway).
  self->presenter_ = provider.CreatePresenter();
  if (!self->presenter_) {
    REXLOG_ERROR("NativeWindow: couldn't create a presenter for the second window");
    return nullptr;
  }
  self->presenter_->AddUIDrawerFromUIThread(&self->no_overlay_, 0);
  // Same size as the main window (window_width / window_height, 1280x720 by
  // default); where it appears is up to the desktop.
  self->window_ = rex::ui::Window::Create(app_context, "crash_mom: native renderer");
  if (!self->window_) {
    REXLOG_ERROR("NativeWindow: couldn't create the second window");
    self->presenter_->RemoveUIDrawerFromUIThread(&self->no_overlay_);
    return nullptr;
  }
  self->window_->AddListener(self.get());
  self->window_->AddInputListener(self.get(), 0);
  return self;
}

NativeWindow::~NativeWindow() {
  // The app destroys the renderer BEFORE this (it may be drawing into our
  // presenter until then), so nothing here may call into it: stop listening
  // first (closing would call OnClosing -> the renderer), then detach the
  // presenter, destroy the window (which closes it) and the presenter.
  window_->RemoveListener(this);
  window_->RemoveInputListener(this);
  window_->SetPresenter(nullptr);
  window_.reset();
  presenter_->RemoveUIDrawerFromUIThread(&no_overlay_);
  presenter_.reset();
}

void NativeWindow::Toggle() {
  if (is_open()) {
    // OnClosing below does the rest (same path as the close button).
    window_->RequestClose();
  } else {
    Open();
  }
}

void NativeWindow::Open() {
  if (!window_->Open() || !is_open()) {
    REXLOG_ERROR("NativeWindow: couldn't open the second window");
    return;
  }
  // Like ReXApp does with the main window: attach the presenter once the
  // window is open (it then makes the Vulkan surface). It stays attached
  // while the window is closed, and the window reconnects it on reopening.
  window_->SetPresenter(presenter_.get());
  renderer_.SetWindowPresenter(presenter_.get());
  // Say which window is which in the main window's title bar too.
  if (main_window_) {
    main_title_ = main_window_->GetTitle();
    main_window_->SetTitle(main_title_ + " (emulated picture; native in the second window)");
  }
}

void NativeWindow::OnClosing(rex::ui::UIEvent& e) {
  (void)e;
  // Our picture leaves this presenter: from the next frame on the renderer
  // draws for the main window again (if F9 chose native) or not at all. The
  // presenter itself stays alive (native_window.h, HOW).
  renderer_.SetWindowPresenter(nullptr);
  focused_.store(false, std::memory_order_relaxed);
  if (main_window_ && !main_title_.empty()) {
    main_window_->SetTitle(main_title_);
  }
}

void NativeWindow::OnGotFocus(rex::ui::UISetupEvent& e) {
  (void)e;
  focused_.store(true, std::memory_order_relaxed);
}

void NativeWindow::OnLostFocus(rex::ui::UISetupEvent& e) {
  (void)e;
  focused_.store(false, std::memory_order_relaxed);
}

void NativeWindow::OnKeyDown(rex::ui::KeyEvent& e) {
  // The main window's ReXApp::OnKeyDown does the same: F8 / F9 / F10 / F3 /
  // F4 / backtick work whichever of the two windows is focused.
  rex::ui::ProcessKeyEvent(e);
}
