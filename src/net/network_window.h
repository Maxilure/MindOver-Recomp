// =============================================================================
// net/network_window.h -- the F1 Tools menu's "Network" window (LAN co-op test)
// =============================================================================
//
// A developer window (ImGui, English, like the Cheats menu) for the lockstep
// sessions of net/net_session.h, milestone M1 of the online plan:
//   HOST / JOIN (address + port), LEAVE;
//   the peer, ping, input delay, the session frame, waits at the frame gate;
//   the world settings' fingerprints (here / there: alike or not);
//   "in sync (checked through frame N)" or "DRIFTED at frame N" from the
//   per-frame world fingerprints, and "Dump both sides" for a drift.
// The game-look "Play Together" menu comes later (milestone M2).
//
// It repaints with the game's frames, and continuously while the frame gate
// waits (the game's frames stop then).
// =============================================================================
#pragma once

namespace rex::ui {
class ImGuiDrawer;
}

namespace network_window {

// Creates the dialog, closed (CrashMomApp::OnCreateDialogs).
void Create(rex::ui::ImGuiDrawer* drawer);
// The Tools menu's button (UI thread): open / close.
void Toggle();

}  // namespace network_window
