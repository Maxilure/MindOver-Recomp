# 29. The debug console: asking the running game questions

Status: a developer tool (Linux), off unless `--debug_console=<socket>` is
given. The input recording it replays is on by default. Section 4 adds the
write watch (`who`: which code writes a value). Section 5 covers the Linux
crash reports built on the same pieces.

Test runs used to work one way only. A script could press buttons
(`--debug_input_script`, the live FIFO), but what happened next had to be read
from screenshots and log lines. Any question about memory ("what's at +80 of
the save manager?", "where is player 2?") needed a gdb run, which boots
slowly, can't attach to a running game, and pauses it. The debug console
answers instead: a command goes in, a reply comes back, and the game keeps
running.

![Teleport through the debug console](../images/debug-console-teleport.jpg)

*Crash on Wumpa Island, moved 10 units sideways and 6 up by `goto 1 ~10 ~6 ~`;
1.5 s later he has landed on the ground and the camera has followed. Both
pictures were taken by the console's `photo` command.*

## 1. How it works

`src/debug_console.cpp` listens on a Unix socket. `tools/mom.py` is the
client: each argument is one command, and the replies are printed. Every
reply ends with a line `@end ok` or `@end error <why>`, which the client
turns into its exit code, so shell scripts can chain commands safely:

```bash
tools/mom.py -s /tmp/mom.sock --wait-socket 20 "wait level 60000" players \
  "read [t1+44]+48 vec3" "lsright 1500" "wait ms 1600" photo
```

Each connection gets its own thread, so a long `wait` or `watch` on one
connection doesn't block commands sent on another.

| Command | What it does |
|---|---|
| `state`, `players`, `frontend` | level / front end / cheats; per player: character, titan, level, health, actor, position; the front end's current state and last exit |
| `eval`, `read`, `write` | address expressions; values (`u8 u16 u32 s32 f32 vec3 str wstr bytes`) |
| `wait ms / state N / exit / level / replay / <expr> <type> <op> <value>` | block until it's true (default timeout 30 s) |
| `snap <name> <expr> <bytes>`, `diff <name>` | save a memory range, then list every word that changed since (hex and float) |
| `watch <ms> <expr>:<type> ...` | values sampled once per game frame; one line per change |
| `pos [p]`, `goto <p> <x> <y> <z>` | where a player is; teleport (`~` keeps a coordinate, `~5` adds 5) |
| `photo` | F10; replies with the saved files once they're written |
| `fkey <Key>` | an app shortcut (F5 cheats, F6 controls, F9 renderer...) as if pressed |
| `replay <file> [from] [to]` | play back recorded presses (section 3) |
| anything else | a live input line, exactly like the FIFO's (`down`, `key Return`, `cheat god on`, `p2.start`) |

**Address expressions** are numbers, `+ - *`, `( )` and `[x]`, where `[x]` is
the 32-bit word at address x (a pointer chase). Names: `uber` =
`[0x8259B190]` (the game's central object), `game` = `0x825B0008`, `p1`-`p4`
= a player's Crash, `t1`-`t4` = the titan they ride. Every address is checked
readable first, because a read of an unmapped guest page would kill the game.
A bad address becomes an error reply.

Reads run on the console's own thread while the game runs, so a value can be
caught mid-update. `watch` avoids that: it samples at the end of each frame,
on the game's main thread (a PDDI frame-end listener, `src/pddi/intercept.h`).

## 2. What the console found while it was being built

* **When a level is playable.** "A level is played" (game state 5) turns true
  under the loading screen, and a photo taken then is the loading screen. The
  front end has a state for it: after a load it goes InGame (486) →
  ExitLoadingComplete → ExitGameRunning → **GameRunning (489)**, and pause
  menus leave 489 (pause = 868 in the test save). `frontend` reports the
  current state: the front end's decision dispatcher (`sub_8211AF40`) is
  asked about it every frame. Node numbers and names come from
  `fighttrees/Frontend.bfig` ([findings/24](24-save-system.md) s.6.4).
* **The level's fade-in.** At GameRunning the picture is still black. The
  front end (`uber +52`) **+144 counts from 1 down to 0** over about 1.2 s
  while the level fades in, and **+148** then counts the seconds since. Found
  with `snap`/`diff` of the front end during the fade, then confirmed by
  reading +144 next to photos: 0.98 = black, 0.49 = dim, 0 = full picture.
  `wait level` waits for GameRunning and +144 = 0.
* **The loading screen** is drawn by `CLoadingScreen`'s draw (`sub_8226BDA0`);
  `more_players::Loading()` is true while it was drawn in the last 250 ms.
* **Where a character's physics lives.** Writing a titan's world matrix
  position (actor +44 → +48) moves it, but Crash on foot snapped back on the
  next frame. Following every pointer in his actor found **actor +308 →
  `CPhysicsBehaviour`** (vtable `0x82032FC4`; its +88 points back at the actor,
  the same for player 2). It keeps its own position (+140) and velocity
  (+104), see [findings/22](22-ground-contact-high-fps.md). `goto` writes the matrix
  and the physics' position, and zeroes the velocity.

## 3. Input recording and replay

`src/input_record.*` wraps the game's controller read (`sub_824742F0`, the
XInputGetState wrapper; the save library already wrapped it). Each session
writes `<log name>-inputs.txt` next to its log, one line each time a
player's controller state changes, exactly as the game read it. This happens
after the port's own filters, so the device doesn't matter (pad, keyboard,
fake controller). Lines with `fe` mark the front end's exits, which helps to
find a moment in a long session:

```
20993 1 0000 0 0 32767 0 0 0      ms since launch, player, buttons, LT, RT, LX, LY, RX, RY
22593 1 1000 0 0 0 32767 0 0
19950 fe 486                      the front end took an exit to state 486
```

`replay <file> [from_ms] [to_ms]` feeds that part of a recording back in,
starting at once. Players the recording mentions get the recorded state, and
their real controllers are ignored meanwhile. A test that recorded a scripted
walk and replayed it into a fresh run of the same save showed every press
arriving within one frame (at most 20 ms) of its original time. The game runs
on real time, so where the character ends up is close but not identical
(about 5 units apart after 5 s of walking). A replay is good for "does this
sequence of moves trigger it", not for frame-exact reproduction. The
launcher's Report a problem adds each chosen session's recording to the zip.

## 4. Who writes there? (`who`)

The question that came up most while hunting bugs: some code changes a value
(a position, a camera distance, a flag), and which code is it? The console's
`snap`/`diff` show THAT a value changed, not WHO changed it. Before, the answer
meant gdb and a hardware watchpoint. Now:

```
who [p1+44]+48 12      watch player 1's position (x, y, z)
wait who 5000          until something writes there
who                    every writer so far
who stop               stop watching
```

`who` lists the writers, most frequent first. Walking right for 1.5 s:

```
watching: 0xF9FCCC80 (12 bytes); 291 write(s) so far
    97x  8217E9C8 (sub_8217E850+0x178)  stfs f0,48(r11)
         host __imp__sub_8217E850+0x772
         last: write #289 to 0xF9FCCC80 at 28545 ms, thread 69482: word 43B077BD -> 43B06F5C
         game stack: lr 8217E9A4 (sub_8217E850+0x154)
                     82103D38 (sub_82103C88+0xB0)
                     8221DD00 (sub_8221D730+0x5D0)
                     ...
                     8227C72C (sub_8227C5D8+0x154)     the frame function
                     8227B0D8 (sub_8227AEE0+0x1F8)     the main loop
    97x  8217E9D0 (sub_8217E850+0x180)  stfs f13,52(r11)
    97x  8217E9D8 (sub_8217E850+0x188)  stfs f12,56(r11)
```

One writer per coordinate, once per game frame, and the instructions are
exactly what the disassembler shows at those addresses. A second check: the
vblank counter `0x8258E460` (findings/07) named its writer as
`stw r11,-7072(r10)` in `sub_82433380` (the game's vblank callback), called
from D3D's vblank interrupt `sub_82310628`, 60 times a second. Both match what
was known.

### How it works (`src/debug/write_watchpoints.*`)

1. The 4 KB host page(s) holding the range are made read-only (`mprotect`).
   Reads go on as normal.
2. A write to such a page faults (SIGSEGV). Our signal handler checks the
   address. A write inside a watched range is recorded: old value, the host
   instruction, the guest's LR and its call stack. Either way, the page is
   made writable again and the CPU's trap flag (x86 EFLAGS.TF) is set in the
   faulting thread.
3. The write runs again, succeeds, and the trap flag stops the thread right
   after that one instruction (SIGTRAP). That handler protects the page again
   and records the new value.

Every write is caught, not only the first, and the game never pauses for
more than two signals. Graphics memory (the physical heaps, where the
characters' objects live) is also watched by the emulated GPU and the
texture cache. There the handler first runs the SDK's watchers
(`Memory::TriggerPhysicalMemoryCallbacks`, as the SDK's own handler does),
so their bookkeeping stays right.

The handler sits in front of the SDK's at the signal level. Joining the SDK's
handler list (`rex::arch::ExceptionHandler`) seemed the obvious way, but ours
had to come first in that list, so it had to be added before guest memory
exists. The first `Install()` into that list is what takes over SIGSEGV, and
`Runtime::Setup` installs its SEH emulation's SIGSEGV handler ("not inside a
`__try`: back to the default action and raise") right before it creates
memory. With the early install that handler stayed on top, and the game's
first ordinary GPU write-watch fault killed it.

### Naming the writer (`src/debug/host_symbols.*`, `guest_stack.*`)

- **The function**: the executable keeps its symbol table (about 62,000
  functions, each recompiled one as `__imp__sub_X` with its size).
- **The exact PowerPC instruction**: `llvm-symbolizer` maps the host address
  to a line of `generated/default/*.cpp`. In those files, every guest
  instruction is a `// <asm>` comment line followed by its C++. Counting the
  comments from the nearest anchor (`DEFINE_REX_FUNC(sub_X)` or a `loc_X:`
  label) gives the instruction's address: anchor + 4 x index. Checked against
  the `ctx.lr = 0x...` line the generator writes after every call: 7,881 of
  7,881 agreed. This needs a build with debug info and the generated sources
  on disk, which every build has, since the game is always built on the
  player's own computer.
- **The game's call stack**: the PowerPC back chain in guest memory (each
  frame stores the caller's stack pointer, and the return address sits 8
  bytes below it). It is read with `process_vm_readv`, which returns an error
  for unmapped memory instead of faulting. A frameless leaf function doesn't
  show up; its caller is the `lr` line.

### Limits

- Linux x86-64 only, and not under gdb (the debugger takes the SIGTRAPs).
- The kernel writing into a watched page (a file read straight into a buffer)
  doesn't fault: the read fails instead. Don't watch a buffer that is being
  loaded.
- Another thread writing to the same page during the single step isn't seen.
- At most 8 ranges and 64 pages. A page that also holds busy data costs a
  fault and a trap per write to it.

## 5. Crash reports on Linux

On Windows, a crash already wrote a named call stack to
`user/logs/crash-<date>_<time>.txt` (`src/crash_report.cpp`, DbgHelp). On
Linux a crash didn't even end the game. The SDK's SIGSEGV handler declined a
fault it couldn't place and returned, so the instruction ran again and
faulted again, forever. The picture froze and one CPU core sat at 100%
(`tools/play.sh --catch` was the way to find out where).

Now a last handler joins the SDK's list once the runtime is set up, so it
runs only when every other handler (MMIO, the GPU's write watches, the write
watch above) declined the fault. It retries the same fault 20 times, 1 ms
apart, in case another thread is just changing that page's state. Then it
writes the report to the same file, the terminal and the log, and lets the
game die by the same signal, **without a core dump**: `PR_SET_DUMPABLE 0`.
A core dump of the game's gigabytes of mappings can fill RAM and swap
while the system's dump service processes it. Under a debugger, the old repeat stays
so gdb can catch it.

A test crash, made by pointing the game's central object pointer
(`0x8259B190`) at `0x10` through the console:

```
CRASH: access violation (read of host 0x10000005c = guest 0x0000005C)
  thread 69482 "Main XThread (F"
  at sub_8227AEE0+0x6eb  [crash_mom+0xd0df2b]
  = inside the game's function sub_8227AEE0
Guest registers: r1 7018FB50  lr 8227B054  ctr 823687C8
  r3 825A56C0  r4 9DF3CA80  r5 00000035  r6 82369C20  r30 03327710  r31 825B0308
Game call stack (return addresses, innermost first; lr = the last call made):
  #0  8227A420  (sub_8227A3C0+0x60)
  #1  82300D3C  (sub_82300BA0+0x19C)
Host call stack (innermost first; the first frames are the fault handlers):
  ...
Original PowerPC instructions (generated sources + debug info):
  fault  0x8227B058  lwz r3,76(r8)
  called 0x8227A41C  bl 0x8227aee0
```

`lwz r3,76(r8)` with r8 = 0x10 reads guest 0x5C: the broken pointer plus 76.
Everything up to the file write runs inside the signal handler, without
allocating: the symbol table is loaded at startup, and text is built with
`snprintf` into a static buffer. The PowerPC instructions need
`llvm-symbolizer` and file reads, so a forked child appends them to the
already-written file. It is killed after 10 s if it gets stuck. The
launcher's Report a problem already packs `crash-*.txt` files.
