// =============================================================================
// debug_console.h -- ask the running game questions (a debugging aid)
// =============================================================================
//
// OFF unless you pass --debug_console=<socket path> (Linux only for now).
//
// Why it exists: the live FIFO (debug_input_script.h) only goes ONE way: a
// test script could press buttons, but had to guess what happened from
// screenshots and log lines, and every memory question ("what's at +80 of the
// save manager?", "where is player 2?") meant a gdb run (slow boot, can't
// attach, pauses the game). The console answers: a command goes in, a reply
// comes back, while the game keeps running.
//
// How: a Unix socket. tools/mom.py is the client:
//     tools/mom.py -s <socket> state
//     tools/mom.py -s <socket> "read [0x8259B190]+80 u32 4"
//     tools/mom.py -s <socket> "wait state 153 30000" a "wait exit"
// One command per line; each reply ends with a line "@end ok" or "@end error
// <why>" (the client turns that into its exit code). A connection may send
// several lines; each is answered in order.
//
// Commands (`help` lists them):
//   state                    level / front end / cheats / uptime in one block
//   players                  per player: character, titan, level, health,
//                            actor address, position
//   frontend                 the front end's current state + last exit
//                            (numbers = notes/scratch-tools/fig_tree.py)
//   eval <expr>              an address expression's value
//   read <expr> [type] [n]   n values at the address; types u8 u16 u32 s32 f32
//                            vec3 str wstr bytes (default u32 1)
//   write <expr> <type> <v>  one value (u8 u16 u32 f32)
//   wait ms <N>              just wait (shell scripts here can't sleep)
//   wait state <N> [ms]      until the front end's current state is N
//   wait exit [ms]           until the front end takes its next exit
//   wait level [ms]          until a level is PLAYABLE (front end GameRunning,
//                            489: loaded, faded in, no pause menu)
//   wait <expr> <type> <op> <value> [ms]   until a value compares true
//                            (op: == != < <= > >=); timeout default 30 s
//   fkey <Key>               a SHORTCUT key as if pressed in the main window
//                            (F5 cheats, F6 controls, F9 renderer, F3...;
//                            the app's binds, not game controls: those are
//                            `key <Name>`)
//   snap <name> <expr> <bytes>   save a memory range under a name
//   diff <name> [update]     every 4-byte word changed since the snap (old ->
//                            new, hex + float); "update" = compare to now next
//                            time. Find a value by what changes when X happens.
//   replay <file> [from_ms] [to_ms]   play back a session's recorded
//                            presses (<log>-inputs.txt, input_record.h);
//                            `replay stop`, `replay` (status), `wait replay`
//   watch <ms> <expr>:<type> [...]  sample values once per GAME FRAME for ms;
//                            a line per change (frame, ms, values). Expressions
//                            may not contain spaces here.
//   pos [p]                  where player p (default 1) is: their titan, else Crash
//   goto <p> <x> <y> <z>     TELEPORT player p (~ = keep, ~5 = current + 5)
//   photo                    F10: a photo of the next frame (both pictures while
//                            ours is drawn); replies with the saved files'
//                            paths once written (PNG + draw list)
//   anything else            a live input line, like the FIFO's ("down",
//                            "key Return", "cheat god on", "p2.start 500")
//
// Address expressions: numbers (0x.. hex or decimal), + - *, ( ), [x] =
// the 32-bit word AT address x (a pointer chase; unreadable = an error, never
// a crash), and names: uber = [0x8259B190] (the game's central object),
// game = 0x825B0008, p1..p4 = that player's Crash (actor), t1..t4 = the titan
// a player rides. Example: [[uber+80]+124] = the save manager's storage
// handler (findings/24).
//
// Reads run on the console's own thread while the game runs: a value can be
// mid-update. Every address is checked readable first (an unmapped guest page
// would kill the game: the SDK's fault handler can't recover it).
// =============================================================================
#pragma once

#include <functional>
#include <string>

#include <rex/cvar.h>

namespace rex::ui {
class Window;
}

REXCVAR_DECLARE(std::string, debug_console);

namespace debug_console {

// Starts the console thread when --debug_console is set (after the kernel's
// memory exists: OnPreLaunchModule). `run_on_ui_thread` posts a function to
// the UI thread and `window` is the main window (both for `fkey`: shortcut
// keys run where a real key press would run them).
using UiPoster = std::function<void(std::function<void()>)>;
void Start(UiPoster run_on_ui_thread, rex::ui::Window* window);

}  // namespace debug_console
