# Findings: recompiling default.xex

How the first `rexglue codegen` runs went, what broke, and why each fix in
`crash_mom_manifest.toml` is there. (2026-09-25, SDK nightly 0.10.0.9)

## Result

| | |
|---|---|
| Analysis time | ~9 s |
| Output | 139 files, ~71 MB of C++ |
| Manual fixes needed | 52 function entries (10 by hand + 42 from the scanner) + 5 CRT mappings |
| Unresolved branches left | **0** |

A clean result for a first pass. Most 360 games need dozens to hundreds of
fixes at this stage.

## How to investigate a codegen error

1. Read `logs/codegen.log`. Look for `ANALYSIS ERRORS` (fatal) **and**
   `Unresolved ...` lines during the `Write` phase. Those aren't fatal, but
   the code they produce crashes (`REX_FATAL`).
2. Disassemble around the address:
   `out/build/linux-amd64-relwithdebinfo/xexdis game <addr-0x20> <addr+0x40>`
3. Figure out where the real function starts and ends. Useful signs:
   * `mflr r12` = the prologue of a non-leaf function (a function start)
   * `blr` = return; `b <addr>` with no link = jump or tail call
   * `00000000` (`.long`) = alignment padding between functions
   * `bl` to `0x8245C1xx` = the compiler's register save/restore helpers
4. Add a `[entrypoint.functions]` entry with a comment explaining it.
5. Re-run codegen and check for `grep 'Unresolved branch' generated/default/*.cpp`.

## Error class 1: functions reached only by tail calls

The analyzer discovers functions by following `bl` calls, vtables, and the
exception table. Tiny functions that are only reached by a plain `b`
(a tail call) are never registered, so the jump into them is "unresolved".

All of the ones we found are compiler-generated glue:

| Address | Size | Pattern |
|---|---|---|
| `0x82170160` | 8 | `li r5,1 ; b ...`: fills in a default argument, then forwards |
| `0x8213EA50` | 8 | `lwz r3,0x28(r3) ; b ...`: forwards to a member object |
| `0x820C0AC8` | 8 | `lwz r3,0x14(r3) ; b ...`: next hop of the same chain |
| `0x82401D30` | 4 | `b ...`: a one-instruction thunk |
| `0x8248BA50` | 8 | `addi r3,r3,4 ; b ...`: C++ this-adjustor thunk (multiple inheritance) |
| `0x8212D638` | 16 | loads two float args, then falls into the next function |
| `0x8212D648` | 24 | the shared function that the previous one (and `0x8212D628`) falls into |

Fixing one layer can expose the next (the thunk's own target), so it took
two rounds.

## Error class 2: a switch statement split wrong

`0x8212C430` is an event handler: `switch (event->type - 2)` over 46
cases through a jump table at `0x8212C458`. Every case is a small block
ending in a tail call. The analyzer picked up the last few case blocks by
linear sweep instead of by following branches, so two conditional
branches (`0x8212C550`, `0x8212C558`) were never matched to their targets.
Codegen then emitted:

```cpp
if (ctx.cr6.eq) REX_FATAL("Unresolved branch from 0x8212C550 to 0x8212C574");
```

...even though `loc_8212C574:` existed a few lines further down. That's
arguably an SDK bug worth reporting upstream. Fix:
`0x8212C430 = { end = 0x8212C57C }`.

## The game uses FIBERS

This is the most important discovery so far. The exe statically links the
XAPI fiber functions, and the game really calls them: `sub_8235B218`
records a task object as "current" and then calls `SwitchToFiber` on the
fiber stored in that object. That looks like a cooperative task/coroutine
scheduler, and it could be driving game objects, scripts, or loading.

| Address | Function |
|---|---|
| `0x82473E08` | `ConvertThreadToFiber` |
| `0x82473E98` | `ConvertFiberToThread` |
| `0x82473EE8` | `CreateFiber` |
| `0x82473FC8` | `DeleteFiber` |
| `0x82474028` | `SwitchToFiber` (thunk → body at `0x82476040`) |
| `0x82474038` | internal: calls the fiber's start routine, exits the thread if it returns |
| `0x82476030` | internal: initial return address of every new fiber |

How we identified them (no symbols, all from behavior):
* On the 360, `r13` points at the processor control block. `*(r13+0x100)`
  is the current thread, and `thread+0x164` is its current fiber.
* The error codes passed to `SetLastError` (`0x823048E8`) give them away:
  `0x500` = `ERROR_ALREADY_FIBER`, `0x501` = `ERROR_ALREADY_THREAD`.
* `0x82476040` saves r1, r14–r31, CR, LR, FPSCR, f14–f31 and **all
  VMX128 registers v64–v127** into the current fiber, then loads the same
  set from the fiber passed in `r3`. That's a full context switch.

**Why this matters.** Recompiled code runs on the host's C++ stack, and
only *part* of its state lives in guest memory. Swapping guest registers
alone would leave the host stack pointing at the wrong fiber's frames.
ReXGlue solves this by running every guest fiber on its own real host
fiber. Mapping these addresses in `[entrypoint.rexcrt]` routes the game's
calls to those native versions.

## Error class 3: functions only reachable through pointers

This one showed up at *runtime*, not in codegen:

    [FATAL] Call to invalid or unregistered function at guest address 0x82480008

`0x82480008` is `return ++this->refCount;`, a COM-style `AddRef`. It's
only ever called through a vtable, and that class has no C++ RTTI, so the
vtable scanner didn't see it. There was also no padding before it, so
"vacancy expansion" glued it onto the end of the previous function.

Rather than wait for these to crash one by one, `tools/find_missing_functions.py`
scans the whole image for code pointers (in data, and built in registers
with `lis`+`addi`) that land on unregistered function starts. First run:
70 hits. After hand review, three kinds of false positive turned up and
are now filtered automatically:

| False positive | Example | Why it's not a function |
|---|---|---|
| Computed switch base | `0x8230040C` | `lis/addi base; add; mtctr; bctr` jumps to base + table[x]*4, a case label |
| SEH scope table fields | `0x823046A4`, `0x8246C630` | `{begin, end, filter, jumpTarget}` entries: `__try` bounds and `__except` bodies are labels |
| Bytes inside the XDBF resource | `0x8230EE50` | Random data that happens to look like a code address |
| Unwind resume address | `0x8245B790` | An epilogue passed (with a frame pointer) to an unwind routine; the function also jumps there itself |

Result: **42 real functions** added (13 XAudio vtable slots and 29
wrappers/thunks passed as callbacks), and the game ran 60 s+ without crashing.

Re-run the scanner after any change to the manifest: new boundaries can
change what gets absorbed.

```bash
out/build/linux-amd64-relwithdebinfo/xexdis game --dump out/default_image.bin
python3 tools/find_missing_functions.py out/default_image.bin \
    generated/default/crash_mom_init.cpp game/default.xex
```

## Other leads spotted along the way

* `0x8245C120`–`0x8245C17C`: compiler register save/restore helpers
  (`__savegprlr_*` / `__restgprlr_*`). Recognized automatically.
* `0x824BBxxx`–`0x824BCxxx`: kernel import thunks (end of `.text`).
* `0x8227A500` / `0x8227A508`: allocate / free with a 4-byte tag (`0x6483....`).
  Probably the XDK's `XMemAlloc` / `XMemFree`.
* `0x823048E8`: `SetLastError`.
