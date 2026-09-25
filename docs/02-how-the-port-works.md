# How the port works

This page explains the big idea behind the project. If you only read one
doc, read this one.

## The problem

`default.xex` is machine code for the Xbox 360's CPU: a 3-core PowerPC
chip called **Xenon**. Your PC has an x86-64 (or ARM) CPU that can't run
those instructions. The game also expects:

* the **Xbox 360 kernel** (`xboxkrnl.exe`) for threads, files, memory, and timers
* the **Xbox system layer** (`xam.xex`) for profiles, controllers, saves, and the guide UI
* the **Xenos GPU**, an ATI chip with 10 MB of fast eDRAM, driven by raw command packets
* **XMA audio** hardware
* **big-endian** memory, where every multi-byte number is stored backwards relative to x86

## Three ways to solve it

| Approach | What it means | Examples |
|---|---|---|
| **Emulation** | Translate the CPU instructions *while the game runs* | Xenia |
| **Decompilation** | Humans rewrite the whole game as readable C/C++, function by function, until it rebuilds byte-for-byte | Super Mario 64, Zelda OoT decomps |
| **Static recompilation** | A tool translates *every* function into C++ *ahead of time*. The result compiles into a real native `.exe` | Unleashed Recompiled, **this project** |

We use **static recompilation**. A full hand decompilation of a 4 MB
C++ game is a multi-year team effort. Recompilation gets the game running
natively quickly, and then we replace pieces with hand-written native code
over time (renderer, audio, bug fixes, widescreen, 60 fps...). The
decompilation can happen gradually, one function at a time, on top of a
game that already runs.

## The pipeline

```
 Your disc (.iso)
      |
      |  tools/xiso_extract.py        (our tool: unpacks the Xbox filesystem)
      v
 game/default.xex  +  game/*.rcf, shaders/, script/  (game data)
      |                                  |
      |  rexglue codegen                 |  read at runtime, unchanged
      |  (decrypts, finds every          |  (mounted as the game's "game:\" drive)
      |   function, translates           |
      |   PowerPC -> C++)                |
      v                                  |
 generated/*.cpp   (~ the game's code, as C++)
      |                                  |
      |  clang (normal C++ compiler)     |
      |  + ReXGlue runtime library       |
      |  + OUR code in src/              |
      v                                  |
 crash_mom  (native Linux/Windows program) <---+
```

### What `rexglue codegen` actually does

1. **Decrypts and decompresses** `default.xex` into the raw PowerPC image.
2. **Finds every function.** It follows calls from the entry point, reads
   the exception tables (`.pdata`), walks C++ vtables (RTTI), and
   pattern-matches compiler helpers.
3. **Translates each instruction** to a line of C++. For example:

   ```
   PowerPC:   lwz r3, 0x10(r31)      ; load 32-bit word from memory at r31+0x10
   C++:       ctx.r3.u64 = PPC_LOAD_U32(ctx.r31.u32 + 0x10);   // byteswaps!
   ```

   Every function becomes `void sub_82xxxxxx(PPCContext& ctx, uint8_t* base)`.
   `ctx` holds the emulated CPU registers, and `base` points at a 4 GB block
   of memory that stands in for the 360's address space.
4. **Resolves `switch` jump tables**, calls, and imports.

### What the ReXGlue runtime provides

The runtime is built from the Xenia emulator's code, but everything except
the CPU part:

| Subsystem | Replaces |
|---|---|
| Memory | The 360's 4 GB virtual + 512 MB physical memory map |
| Kernel (xboxkrnl) | Threads, events, mutexes, file I/O, timers... |
| XAM | Profiles, controllers, content/saves, system UI |
| VFS | Maps `game:\default.rcf` -> `game/default.rcf` on your disk |
| GPU | Xenos command processor + shader translation -> **Vulkan** (or D3D12) |
| Audio | XMA decoding + mixing |
| Input | Xbox controller via SDL |

### What WE write (`src/`)

* The app class that boots it all (`src/crash_mom_app.h`)
* **Hooks** that replace specific game functions with native code, e.g.
  `REX_HOOK(sub_82xxxxxx, MyFunction)`. This is how fixes and enhancements
  get in.
* The codegen config (`crash_mom_config.toml`), which is where we fix
  anything the analysis gets wrong (function boundaries, jump tables).

## Endianness, in one paragraph

The 360 stores the number `0x12345678` in memory as bytes
`12 34 56 78` (big-endian). x86 stores it as `78 56 34 12`. Recompiled
code keeps game memory in the original big-endian layout and byteswaps on
every load and store (`PPC_LOAD_U32` etc.). That's why game data files
like `.rcf` can be read completely unmodified. Our own native code has to
remember this whenever it reads or writes game memory: use the SDK's
`be<T>` types.

## Where this is heading

1. **Boot**: the recompiled exe runs and reaches the title screen *(current goal)*
2. **Playable**: every level, cutscene, and menu works; saves work
3. **Native**: replace GPU emulation with a native renderer (the original
   HLSL is in `shaders/*.updb`, see findings), plus unlocked framerate,
   widescreen/resolution options, and keyboard + mouse
4. **Other versions**: the game also shipped on PS2, Wii, PSP and DS (there
   is no Wii U version, since the Wii U came out four years later). Those use
   different CPUs (PS2 = MIPS R5900, Wii = PowerPC "Broadway"), so each would
   need its own recompiler. The Wii is the closest relative, because it's
   PowerPC and big-endian too. Anything we learn about the engine and data
   formats carries over.
