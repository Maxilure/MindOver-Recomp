// =============================================================================
// settings_list.h -- --list_settings=<file>: every setting, for the launcher
// =============================================================================
//
// WHY: the launcher's Settings tab (launcher/settings.h) must know every
// setting the game has: its name, type, default, allowed values, range and
// description. They are all in the SDK's flag registry (rex/cvar.h), which
// only exists inside the running game, so the game writes it out on request:
//
//   crash_mom --list_settings=<file.toml>
//
// writes the file and quits at once, BEFORE any window, audio or game code
// (it runs at the end of CrashMomApp::OnPreSetup: by then the GPU plugin is
// loaded, so its flags are in the list too). The launcher runs this once per
// build of the game (it compares the exe's date) and keeps the result.
//
// THE FILE (TOML, one [[setting]] table per flag, sorted by name):
//   [[setting]]
//   name = "fps_cap"
//   type = "int32"          # bool int32 int64 uint32 uint64 double string
//   category = "CrashMoM"   # the defining part: CrashMoM = ours, others = SDK
//   description = "..."
//   default = "30"          # always as text, like settings.toml's parser takes it
//   allowed = ["a", "b"]    # only when the flag lists its allowed values
//   min = 0.0 / max = 1.0   # only when it has a range
//   restart = true          # changes only apply at the next start
//   init_only = true        # can only be set at start (command line / file)
//   debug_only = true
// Command flags (actions, no value) are left out.
// =============================================================================
#pragma once

namespace settings_list {

// If --list_settings was given: writes the file and ends the program
// (exit code 0, or 1 when the file can't be written). Else does nothing.
void WriteAndQuitIfAsked();

}  // namespace settings_list
