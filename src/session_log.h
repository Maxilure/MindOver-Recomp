// =============================================================================
// session_log.h -- every play session leaves a log a tester can send
// =============================================================================
//
// WHY: when a tester says "it crashed in the ice level" or "player 3 got
// stuck in a menu", the log of that session is the only record of what the
// game was doing. Until now the useful lines (co-op decisions, menu owners,
// front-end screens, sounds, frame rate) were OFF unless a developer turned
// on the matching --debug_* flag, and nothing said which version, system or
// settings the log came from. This file makes every session's log useful
// out of the box, and keeps the logs folder from growing forever.
//
// WHAT IT DOES:
//
//   1. THE HEADER, the first lines of every log (Start, at
//      OnPostInitLogging, the earliest moment logging works):
//        Session: Mind over Recomp 0.1.0-alpha (built Oct  7 2026)
//        Session: system Linux 7.2.9 (CachyOS) x86_64 / Windows 11 build 26200
//        Session: CPU <model> (12 threads), RAM 16.0 GB (9.3 GB free)
//        Session: started with --game_data_root=... --load_save=3
//        Session: event logs on (...)  /  off (event_logs = false)
//      and, once every setting exists (LogSettings, at the end of
//      OnPreSetup: the GPU plugin's flags are registered only then):
//        Session: settings changed from the defaults: fps_cap = 180, ...
//      The graphics card + driver are logged by the SDK a moment later
//      ("Vulkan device '<name>' ..." and "driverInfo: ...").
//
//   2. EVENT LOGS ON BY DEFAULT. These flags now default to true (each is
//      cheap: a line per event, never per frame; together ~25 KB a minute
//      in a co-op session):
//        debug_log_fps          frame rate every 5 s + the session summary
//        debug_audio_trace      the name of every sound the game opens
//        debug_coop_trace       co-op states, joins, masks, controllers
//        debug_menu_input_trace which player a menu answers to, pauses
//        debug_frontend_trace   every screen change in the menus
//      ONE SWITCH turns them all off: --event_logs=false (or the launcher's
//      Settings tab). A flag set on its own still wins (e.g.
//      --event_logs=false --debug_log_fps=true keeps only the fps lines).
//      The per-frame firehoses stay off: --debug_kbm_trace, the ground and
//      camera traces, PDDI frame traces, the kernel trace (log_level=trace).
//
//   3. OLD LOGS CLEANED UP (Start): the logs folder (user/logs) is kept under
//      --logs_budget_mb (default 300 MB) by deleting the OLDEST log / crash
//      files first. The session's own log is never deleted. A normal session
//      writes ~1-2 MB an hour, so that's months of sessions; the budget exists
//      for the rare runaway log (a stuck fault repeating one line: 250 MB once).
// =============================================================================
#pragma once

#include <filesystem>

namespace session_log {

// The header's first part, the event-log switch and the clean-up. Call once
// from OnPostInitLogging. `logs` = the logs folder (user/logs).
void Start(const std::filesystem::path& logs);

// "settings changed from the defaults: ...". Call at the end of OnPreSetup
// (every flag registered and loaded by then).
void LogSettings();

}  // namespace session_log
