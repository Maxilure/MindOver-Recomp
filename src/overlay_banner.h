// =============================================================================
// overlay_banner.h -- a short message box on screen ("Native renderer (Vulkan)")
// =============================================================================
//
// WHAT: a small box near the top of the main window that shows one or two
// lines of text for ~1.5 seconds, fading in and out. Used when F9 switches
// the picture (2026-09-30: which renderer is on screen now) and for other
// switches worth seeing
// without reading the log (dual mode, --native_only / --emulated_only).
//
// HOW: an ImGui dialog, drawn by the SDK's ImGuiDrawer on top of whatever
// picture the main window shows (emulated or ours), exactly like the SDK's
// achievement pop-up (rex/ui/overlay/achievement_toast.*). The dialog exists
// for the whole session but draws nothing while idle, and only asks the
// window to keep repainting while a message is up (WantsContinuousRepaint;
// SDK patch 0006: an always-repainting dialog made the window redraw 600+
// times a second). The second window of dual mode has no ImGui, so the box
// only ever shows in the main window.
//
// Show() is safe from any thread (key callbacks run on the UI thread, the
// renderer's switches can come from the game's main thread).
// =============================================================================
#pragma once

#include <string>

namespace rex::ui {
class ImGuiDrawer;
}

namespace overlay_banner {

// Creates the dialog (CrashMomApp::OnCreateDialogs, once the ImGui drawer
// exists). A message shown before that appears as soon as it exists.
void Create(rex::ui::ImGuiDrawer* drawer);

// Shows `title` (big) and `detail` (small, may be empty) for ~1.5 s,
// replacing any message still showing.
void Show(std::string title, std::string detail = {});

}  // namespace overlay_banner
