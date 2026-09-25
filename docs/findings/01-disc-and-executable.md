# Findings: the disc and the executable

What we learned by taking the USA Xbox 360 disc apart (2026-09-25).
Every fact below was produced by the tools in `tools/`, so you can re-check
any of it yourself.

## The disc image

| Property | Value | How we know |
|---|---|---|
| File | `Crash - Mind over Mutant (USA).iso`, 7,835,492,352 bytes | That size is a full dual-layer 360 dump |
| Disc format | **XGD2** | `xiso_extract.py` found the XDVDFS magic at `0xFD90000` |
| Game data | 83 files, 3 folders, 6.0 GB | `xiso_extract.py --list` |

### What's on the disc

| File(s) | Size | What it is |
|---|---|---|
| `default.xex` | 5.7 MB | **The game executable.** This is the thing we recompile. |
| `default.rcf` | 946 MB | Main game data archive (levels, models, textures...) |
| `english.rcf`, `french.rcf`, ... (11 languages) | ~250 MB each | Per-language data: dialogue audio, localized text/UI |
| `movie1.rcf`, `movie2.rcf` | 1.3 GB each | Cutscene videos (Bink, see `shaders/binkdecompress.out`) |
| `script/ahoy.blua` | 1 KB | A Lua script. **Encrypted or compressed**: no `\x1bLua` header, looks like random bytes |
| `shaders/*.out` | tiny | Compiled Xbox 360 (Xenos) GPU shaders |
| `shaders/*.updb` | tiny | **Shader debug info (XML)**, see below |
| `$SystemUpdate/su20076000_00000000` | 6.9 MB | Console dashboard update. Not part of the game, ignore it |

### `.rcf` = Radical "Cement" archives

Every `.rcf` begins with the ASCII magic `ATG CORE CEMENT LIBRARY`. ATG was
Radical Entertainment's Advanced Technology Group, and "Cement" is their
archive format. It's the same family used by *The Simpsons: Hit & Run* and
*Prototype*, so the modding scene for those games already has partial
format documentation. On 360 the header fields are **big-endian**.

We don't need to parse these to get the game running, because the
recompiled game reads them itself exactly like it did on the console. They
matter later for modding and for any asset-replacement work.

### `.updb` shader PDBs still contain the HLSL source

The `.updb` files are XML shader debug databases (`<shader-pdb ... version="6995">`)
that shipped by accident. They embed the **original HLSL source** of each
shader, along with the developers' build path:

    C:\Crash008\sdks\atg\runtime\code\pure3d\pddi\src\xenon\shaders\...

This tells us:
* the engine is **Pure3D** (Radical's in-house engine, later called
  "Titanium"), and its renderer layer is **PDDI** (Pure3D Device Driver
  Interface), with a `xenon` (Xbox 360) backend.
* when we eventually write a native renderer instead of emulating the
  360 GPU, the original shader logic is right there to port. We won't
  have to reverse it from GPU microcode.

(We keep that source on your machine only and never copy it into this repo.)

## The executable (`default.xex`)

Output of `python3 tools/xex_info.py game/default.xex`:

| Property | Value | Meaning |
|---|---|---|
| Title ID | `565507FA` | `VU` prefix = Vivendi Universal (Sierra) |
| Original PE name | `Crash008f.exe` | Internal project name "Crash008". The same name appears in the shader build paths |
| Version | 0.0.0.1 | Never patched (no title update) |
| Encryption | AES (retail) | ReXGlue decrypts it, so we don't have to |
| Compression | **basic** | Simple zero-run packing (not LZX), the easy case |
| Region | `0xFFFFFFFF` | Region-free |
| Image base | `0x82000000` | Standard for 360 games |
| Image size | 6.3 MB | |
| Entry point | `0x82300BA0` | First function the recompiled game runs |
| Stack | 256 KB default | |
| TLS | 64 slots | Thread-local storage the runtime must provide |

### Memory layout

    0x82000000-0x820B0000  read-only data    0.69 MB
    0x820B0000-0x824D0000  code              4.12 MB   <- everything we recompile
    0x824D0000-0x825C0000  data              0.94 MB
    0x825C0000-0x82650000  read-only data    0.56 MB   (XDBF resource: title name, achievements, icons)

About **4.1 MB of PowerPC code**, which is a moderate size. (For
comparison, big open-world 360 games often have 10 MB+ of code.)

### Linked SDK libraries (XDK 2.0.6995, a 2008 SDK)

| Library | What it is | What it means for the port |
|---|---|---|
| `XAPILIB` | Xbox Win32-style API layer | Wraps kernel calls, handled by the runtime |
| `XBOXKRNL` | Kernel import stubs | See the imports below |
| `LIBCMT` | Microsoft C runtime | memcpy/strlen/etc. Candidates for native replacements (ReXCRT) |
| `D3D9` | Xbox 360 Direct3D 9 | **Rendering.** Emulated at the GPU level by ReXGlue at first |
| `XGRAPHC` | D3D helper library (texture tiling etc.) | |
| `XAUD` | XAudio (v1) | **Audio.** Goes through the runtime's XMA/APU emulation |
| `XMP` | Xbox Music Player | Custom soundtrack support, can be stubbed |
| `XONLINE` | Xbox Live | Can be stubbed (the game has no real online play) |

No XACT and no XHV (voice chat). Bink video is linked in too, but
third-party libraries don't show up in this list.

### Imports (functions the OS has to provide)

| Library | Import records |
|---|---|
| `xboxkrnl.exe` (kernel: threads, files, memory, sync) | 250 |
| `xam.xex` (system: profiles, UI, achievements, input) | 70 |

Functions use 2 records each and variables use 1, so that works out to
roughly 125 kernel and 35 XAM functions. ReXGlue already implements the
common ones. The recompile step tells us exactly which are missing.

## Open questions / leads

* **Lua**: the game embeds a Lua VM. Lua reports errors using
  `setjmp`/`longjmp`, which static recompilation has to handle specially
  (config keys `setjmp_address` / `longjmp_address`). We need to find
  those two functions in the exe.
* **`.blua` encryption**: find the decrypt routine in the recompiled code
  (look for what reads `script/*.blua`).
* **Crash of the Titans** (2007) runs on the same engine. Anything we learn
  here probably carries over.
