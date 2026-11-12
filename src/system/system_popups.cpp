// =============================================================================
// system_popups.cpp -- the last system pop-ups + the network start-up,
// answered by the port
// =============================================================================
//
// WHAT THIS IS
//   The "System pop-ups and notifications" and "Network" rows of README's
//   "Going native" table (plans.md s.9, docs/findings/32 s.8). After profiles,
//   saves and the console settings went native, four Xbox services were still
//   reached through the SDK's fake Xbox. Each one has exactly ONE caller in
//   the game (found 2026-11-13: Xbox calls meter "first X from sub_Y+off" lines
//   + a grep of the generated code for each import + tools/callgraph.py for
//   the callers' callers, every site read with xexdis):
//
//   XNotifyPositionUI (thunk 0x824BB96C), called at 0x8227AE14 in the game's
//   start-up object set-up sub_8227AC58 (`li r3,1` right before): "show the
//   dashboard's notification toasts at position 1". There is no dashboard
//   on a PC (the port's own cards are drawn by the game's message bar,
//   achievements.*), so the call is simply skipped (manifest midasm hook
//   CrashMomNotifyPosition jumps over the `bl`; r3 is overwritten by the next
//   instruction, `mr r3,r27`, so no answer is needed). Meter: once per boot.
//
//   NetDll_WSAStartup (0x824BBF9C), reached once per boot: front-end set-up
//   sub_8225F8A0 -> sub_82322950 (a Microsoft network library's start-up,
//   config 0x824E5D2C) -> sub_823228C8 -> WSAStartup sub_82322B38(version r3,
//   WSADATA r4) -> the dispatcher sub_82322A90 -> the import. The only thing
//   sub_823228C8 does with the answer: r3 == 0 -> carry on laying out its
//   memory block (sub_823229C8, no Xbox calls); the WSADATA is a stack buffer
//   (r1+80) that nobody reads. The game has NO online features, so the port
//   answers WSAStartup itself: 0 = success (what the SDK said on Linux too),
//   no socket library is started. The dispatcher's dashboard-version check
//   (console_settings.cpp hook at 0x82322AB0) is then no longer reached.
//
//   XamShowDirtyDiscErrorUI (0x824BB9DC), in sub_823002E0, called only by
//   Radical's error thread sub_8235B400 (who_plays.cpp: it waits for the file
//   layer's "a read failed" event). On the 360: Microsoft's "the disc is
//   unreadable" box, then sub_82300DB0 = back to the dashboard (never
//   returns). On a PC there is no disc: the files are the player's extracted
//   copy in <game folder>/game, so a read error means a missing or damaged
//   file. PC behaviour: a real message box saying so (+ the installer's
//   repair route), the log line, and a clean exit (same as Quit Game).
//   Never seen in a session; tested with --debug_disc_error_box.
//
//   XamShowMessageBoxUIEx (0x824BBA2C): its only caller is sub_82300878,
//   whose only caller is the start-up cable check sub_823009B8, already
//   replaced as a whole by console_settings.cpp (0 calls since 2026-11-12).
//   Nothing to add here; native_progress.py records it.
//
// SWITCH: --system_popups_native=false lets the SDK answer again (A/B).
// =============================================================================

#include "system_popups.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <SDL3/SDL_messagebox.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "../language/texts.h"

// The originals (the generated code), for the A/B switch.
extern "C" REX_FUNC(__imp__sub_823002E0);
extern "C" REX_FUNC(__imp__sub_82322B38);

REXCVAR_DEFINE_BOOL(system_popups_native, true, "CrashMoM",
                    "The port answers the game's last Xbox pop-ups and its network "
                    "start-up itself (notification position, disc error box, WSAStartup); "
                    "false = the SDK's emulated Xbox answers (for comparisons)");

REXCVAR_DEFINE_BOOL(debug_disc_error_box, false, "CrashMoM/Debug",
                    "Test only: show the port's 'game files unreadable' box at start-up "
                    "and quit, as a real read error would");

namespace {

// Logged once per question, so a session log shows the port answered it.
void Note(const char* question, bool& said) {
  if (said) return;
  said = true;
  REXLOG_INFO("System pop-ups: {} answered by the port", question);
}

// UTF-16 (our translated texts) -> UTF-8 (SDL's message box).
std::string ToUtf8(std::u16string_view text) {
  std::string out;
  for (size_t i = 0; i < text.size(); ++i) {
    uint32_t c = text[i];
    // A surrogate pair = one character above U+FFFF.
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < text.size() && text[i + 1] >= 0xDC00 &&
        text[i + 1] < 0xE000) {
      c = 0x10000 + ((c - 0xD800) << 10) + (text[i + 1] - 0xDC00);
      ++i;
    }
    if (c < 0x80) {
      out += char(c);
    } else if (c < 0x800) {
      out += char(0xC0 | (c >> 6));
      out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += char(0xE0 | (c >> 12));
      out += char(0x80 | ((c >> 6) & 0x3F));
      out += char(0x80 | (c & 0x3F));
    } else {
      out += char(0xF0 | (c >> 18));
      out += char(0x80 | ((c >> 12) & 0x3F));
      out += char(0x80 | ((c >> 6) & 0x3F));
      out += char(0x80 | (c & 0x3F));
    }
  }
  return out;
}

// How to reach the UI thread (Install, from CrashMomApp).
std::function<void(std::function<void()>)> g_run_on_ui;

// The PC's "dirty disc" box: tell the player, then end the program the way
// Quit Game does (flush the log, _Exit). The box opens on the UI thread
// (system_popups.h says why); the calling game thread waits for the exit
// there, like the 360, whose box never came back either.
[[noreturn]] void ShowUnreadableFilesAndQuit() {
  REXLOG_ERROR("System pop-ups: the game couldn't read one of its files: closing");
  std::string message = ToUtf8(
      tr(u"The game couldn't read one of its files.\n\n"
         u"The game files in the \"game\" folder may be missing or damaged. "
         u"Open Mind over Recomp (the installer) and update or repair the game, "
         u"then start it again."));
  std::fprintf(stderr, "[ERROR] %s\n", message.c_str());
  rex::FlushLogging();
  auto show_and_quit = [message = std::move(message)] {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Mind over Recomp", message.c_str(),
                             nullptr);
    REXLOG_INFO("System pop-ups: box closed: quitting");
    rex::FlushLogging();
    std::_Exit(1);
  };
  if (!g_run_on_ui) show_and_quit();  // before setup: no UI thread to ask
  g_run_on_ui(show_and_quit);
  for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}

}  // namespace

void system_popups::Install(std::function<void(std::function<void()>)> run_on_ui_thread) {
  g_run_on_ui = std::move(run_on_ui_thread);
}

// -----------------------------------------------------------------------------
// XNotifyPositionUI at 0x8227AE14 (manifest "SYSTEM POP-UPS"): skipped.
// -----------------------------------------------------------------------------
bool CrashMomNotifyPosition(PPCRegister& r3) {
  if (!REXCVAR_GET(system_popups_native)) return false;
  static bool said = false;
  Note("notification position", said);
  (void)r3;  // position 1; the next instruction overwrites r3
  // The test switch rides on this once-per-boot call: early, game thread.
  if (REXCVAR_GET(debug_disc_error_box)) ShowUnreadableFilesAndQuit();
  return true;  // jump over the call
}

// -----------------------------------------------------------------------------
// WSAStartup wrapper sub_82322B38 (r3 version, r4 WSADATA): 0 = success.
// -----------------------------------------------------------------------------
extern "C" REX_FUNC(sub_82322B38) {
  if (!REXCVAR_GET(system_popups_native)) {
    __imp__sub_82322B38(ctx, base);
    return;
  }
  static bool said = false;
  Note("network start-up (WSAStartup: no online features)", said);
  ctx.r3.u64 = 0;
}

// -----------------------------------------------------------------------------
// XShowDirtyDiscErrorUI wrapper sub_823002E0 (r3 = user): the PC's box + quit.
// -----------------------------------------------------------------------------
extern "C" REX_FUNC(sub_823002E0) {
  if (!REXCVAR_GET(system_popups_native)) {
    __imp__sub_823002E0(ctx, base);
    return;
  }
  ShowUnreadableFilesAndQuit();
}
