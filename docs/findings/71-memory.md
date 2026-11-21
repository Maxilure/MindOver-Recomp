# 71. The game's memory: who holds what, and what runs out

The port has hit the Xbox 360's fixed memory sizes several times: the front
end's ~100 KB heap (save list texts, the question box), the zone lists (32
characters per zone, `src/world/zone_lists.cpp`), the D3D ring buffer (6 MB,
grown to 24 MB), and the whole level of Wumpa Island not fitting when split
screen kept every zone loaded (findings/70 s.9). This file measures the
memory first (a survey tool), names the pool that runs out, and lays out the
plan for the port's own memory manager.

## 1. The survey tool

`src/debug/memory_survey.*`:

* debug console `memory`: every heap's fill and peak, the heap regions, and
  the SDK's guest memory (refreshed every second on the game thread);
* a log line every `--debug_memory_log_s` seconds (default 60, 0 = never):
  `Memory: h0 used/total KB ... | physical free N MB (largest M MB) ...`;
* every refused allocation (`src/system/game_memory.cpp`), named with the
  calling game function:
  Radical's heap alloc `sub_82357758`, XAPI's heap alloc `sub_82303410` and
  XPhysicalAlloc `sub_82300E98`; plus every physical block of 8 MB or more;
* a debug helper for the out-of-memory test (section 3):
  `memory neighbours-all <zone>`.

## 2. Where the memory comes from

ReXGlue gives the game the same address map as the console
(`xmemory.cpp`, `Memory::Initialize`):

| Range | Page size | Size | What lives there |
|---|---|---|---|
| 0x00000000-0x3FFFFFFF | 4 KB | 1 GB | virtual, nearly unused (1,023 MB free in a level) |
| 0x40000000-0x7EFFFFFF | 64 KB | 1,008 MB | virtual: thread stacks, XAPI's heap growth; 985 MB free in a level |
| 0x80000000-0x9FFFFFFF | 64 KB / 4 KB | 512 MB | the XEX image |
| 0xA0000000 / 0xC0000000 / 0xE0000000 | 64 KB / 16 MB / 4 KB | 512 MB each | three WINDOWS onto the same 512 MB of physical memory |

Only the physical memory is shared by the three windows: an allocation in
0xE0000000 takes pages that 0xA0000000 can no longer get. The SDK's per-window
heaps only know their own allocations; the real free number comes from the
shared heap behind them (`Memory::GetPhysicalHeap()`). An older log line
(`outfit_previews.cpp` "physical free") read the 0xE0000000 window alone and
overstated what was left.

**At boot** (log, Wumpa Island save):

| Block | Size | Asked by |
|---|---|---|
| Radical's root heap ("MainMemory", `MemoryDlAllocator`, vtable 0x8200B3F4) at 0xF75A6000 | 135 MB (0x8700000, a constant) | `sub_8235D9D8` |
| a probe: all free physical memory minus 16 MB, freed again at once | 352 MB | `sub_8235D838` (`MemoryAllocatorPhysical`) |
| D3D's secondary ring (port: 6 -> 24 MB) | 24 MB | D3D via XMemAlloc |

**Every one of Radical's heaps lives inside that 135 MB root block**, so the
game's own `new` / `delete` memory is physical memory too. The heap table
(0x8256F1D0, 16 slots; several slots point at the root):

| Heap | Class (RTTI) | Size | In use (Wumpa Island, 1 player) |
|---|---|---|---|
| 2 (and 5, 10-15) | `MemoryDlAllocator` (the root, coreDlMalloc) | 138 MB | 131 MB (the sub-heaps below are carved from it) |
| 0 | `CDLMemoryAllocator` | 110 MB | 110 MB (100 %: a parent of the next ones) |
| 3 | `CCorePermenentAllocator` (PERMANENT) | 92 MB | 1.4 MB |
| 8 | `CDLMemoryAllocator` (the level's) | 91 MB | 3.4 MB; 5.9 MB with every zone loading |
| 9 | `CSlabDummyParentAllocator` | - | (its report is not a size) |
| 6 | `CDLMemoryAllocator` | 10 MB | 4.9 MB |
| 4 | `CDLMemoryAllocator` | 6 MB | 1.2 MB |
| 1 | `CDLMemoryAllocator` | 1 MB | 0.3 MB |
| 7 | `CDLMemoryAllocator` (the front end's) | 102 KB | 11 KB |

The game's `new` (`sub_8227E9C8`) takes from the current heap (0x825711EC):
heap 0 = asserts when asked unless allowed, heap 3 (PERMANENT) asserts once
byte 0x8259B182 (`s_LevelHeapAllocated`) is set, and the first allocation
from heap 8 sets that byte. `delete` (`sub_82357598`) finds the heap from the
address (region table 0x8256D1B0: start, heap id; `sub_82357128`).

**Graphics data is separate**: D3D creates textures and buffers through the
game's XMemAlloc (`sub_8227A500` -> `sub_823005A8`): small objects (the
0x34-byte texture header) from XAPI's heap (`sub_82303410`), the pixel and
vertex data with the "physical" bit from XPhysicalAlloc (`sub_82300E98` ->
MmAllocatePhysicalMemoryEx, ~29,000 calls a session).

In Wumpa Island with one player (6 of 26 zones loaded): physical free
142 MB of 512 (0xE0000000 window 326 MB, 0xA0000000 28 MB, 0xC0000000 16 MB).

## 3. What runs out: the 512 MB of physical memory

Test: save 2 (Wumpa Island), split screen on, `memory neighbours-all
[p1+40]` (player 1's zone lists every zone as its neighbour: split screen's
"every player's zone + neighbours" then wants the whole level). The log:

```
Split screen: zones kept loaded around every player: 26 wanted, 20 to load
Memory: physical allocation of 1048576 bytes (align 1000, protect 00000404) refused
        (physical free 0.3 MB, largest 0.3 MB) for sub_823005A8+0x54
CRASH: access violation (read of guest 0x00000020) at sub_82305140+0x87
```

Physical free fell 142 -> 83 -> 0.3 MB while the zones loaded; the level heap
grew only 2.5 MB. The texture creation `sub_82306478` returned 0
(E_OUTOFMEMORY) and xnTexture's set-up (`sub_8243A3C0`) passed that null on
to `sub_82305508` -> `sub_82305140` = the crash. No Radical heap was full.

So the pool is not one of the game's heaps but the console's whole
physical memory: zone graphics (textures, vertex and index buffers) plus the
135 MB root heap, which holds 85+ MB of unused level heap and 90 MB of unused
permanent heap.

## 4. The plan: the port's own memory manager

Game pointers stay 32-bit guest addresses (the recompiled code knows no
other). The room for more is the guest's VIRTUAL memory: ~2 GB, almost all
free. Steps, smallest first:

1. **The root heap out of physical memory** (native renderer only; BUILT,
   ON BY DEFAULT since 2026-11-21: `src/system/game_memory.cpp`, launch
   setting `main_heap_virtual`): the one
   135 MB XPhysicalAlloc in `sub_8235D9D8` gets memory from 0x40000000
   instead (same 32-bit pointers, nothing else changes), and can be bigger.
   Frees 135 MB of physical memory for graphics: about what the whole of
   Wumpa Island was short. Why native only: with the emulated GPU, game data
   in a heap might be read by the GPU through a physical address. The
   native renderer reads guest memory directly.
   The block comes from the 0x40000000 range (at 0x40100000; with
   `--renderer=emulated` it stays physical, log "Memory: the game's main
   heap (135 MB) in ..."). The game runs as before (0 errors, picture normal);
   Wumpa Island with one player: physical free 142 -> 277 MB; the
   whole-level test of section 3: ALL 26 zones loaded, 130 MB of physical
   memory still free, no refused allocation (also with 2 players in split
   screen). Also checked: 4 players, two Quit -> Load Game cycles into other
   levels, a real level change: 0 errors, physical free 277-296 MB.
2. **No heap is full any more** (BUILT, ON BY DEFAULT since 2026-11-21:
   `src/system/game_memory.cpp` + `src/system/guest_pool.*`, launch setting
   `heap_growth`, native renderer only like step 1). Every game heap keeps
   working exactly as on the console until it is full; then the request is
   answered from the port's own pool instead of failing.

   The heap classes (RTTI + vtables): `MemoryDlAllocator` (vtable
   0x8200B3F4, Radical's dlmalloc) is the root "MainMemory" heap AND the
   inside of every `CDLMemoryAllocator` (vtable 0x8203C4B4: a locked wrapper,
   the dlmalloc heap at +4): Temp (1), Script (4), Sound (6), FrontEnd (7),
   Level (8), heap 0. Names and ids from the game's heap descriptor table
   0x82506D10. Its methods: +0x04 release (refcount; the last one destroys
   it and gives its arena back to the parent heap), +0x08 deleting
   destructor `sub_823601B0`, +0x0C alloc `sub_82360F88`, +0x10 / +0x1C
   free `sub_82360430` / `sub_82360378` (the last block freed resets the
   whole arena), +0x14 / +0x20 contains `sub_823600E8`, +0x18 aligned alloc
   `sub_82360FF8` (r4 bytes, r5 alignment), +0x2C arena range, +0x30
   report, +0x34 size. Others: `CCorePermenentAllocator` (Permanent, 3: a
   bump allocator, free does nothing), `CSlabDummyParentAllocator` (9).

   The pool: guest virtual memory taken in 64 MB pieces from the 0x40000000
   range, best fit with neighbours merged, 16-byte alignment, ALL its
   records in host memory (a game write past a block can't damage them),
   each block tagged with the heap object it stands in for.

   Hooked: alloc and aligned alloc (original first; a 0 = the pool, only
   for heaps in the heap table: a private MemoryDlAllocator fails like the
   original), both frees, contains and the destructor (a heap that goes
   away takes its pool blocks along), `delete` `sub_82357598` and the
   region free `sub_82357540` (they find the heap by the address's REGION,
   which a pool block has none of: the pool answers first). The PERMANENT
   rule: midasm `CrashMomPermanentAfterLevel` 0x8227E92C makes `new` read
   s_LevelHeapAllocated as 0 for its heap-3 check (no DbgBreakPoint; the
   92 MB permanent heap holds ~1.5 MB).

   Tests (copy of the saves, 30 fps), all 0 errors:

   | Run | Result |
   |---|---|
   | `--debug_heap_force_pool=7,8`: EVERY FrontEnd and Level allocation from the pool | Wumpa Island with 4 players (their characters at 0x48xxxxxx = pool), ~48,000 pool blocks, picture normal; Quit -> Load Game into another level (the Level heap's 3.2 MB of pool blocks freed one by one, as the game deletes them); a real level change |
   | normal play, split screen, the whole of Wumpa Island | 26 zones loaded, no heap full: the pool never used (0 pieces) |

   Cost: the original allocator answers everything until a heap is full;
   each `delete` first asks "inside the pool?" without a lock (no pieces =
   one compare). With every FrontEnd + Level allocation forced into the
   pool, the game's frame time went from ~2.6 to ~3.7 ms (std containers
   vs dlmalloc): fine for overflow, not a replacement for the game's own
   allocator.

   Not seen yet: a heap filling by itself in real play (the forced runs
   exercise the same code paths), and the permanent-rule hook firing (split
   screen's zone lists still lift the flag themselves).
3. **Graphics memory past 512 MB** (RESEARCH 2026-11-22, not built).

   WHO HOLDS THE PHYSICAL MEMORY (`--debug_physical_callers`, console
   `memory physical`: every XPhysicalAlloc counted by its callers; XPhysicalFree
   = `sub_82300F20`), Wumpa Island, main heap already in virtual memory:

   | Caller chain (XMemAlloc `sub_823005A8` <) | Level loaded | Whole level (26 zones) |
   |---|---|---|
   | D3D CreateTexture `sub_82306478`+0xB0 < xnTexture create `sub_8243A3C0`+0x15C (pixels) | 98 MB | 192 MB |
   | the same +0xDC (a second block per texture) | 22 MB | 55 MB |
   | the same < `sub_8243A3C0`+0x324 (small: palettes) | 12 MB | 12 MB |
   | D3D's command ring `sub_82309C20` (port: 24 MB) | 24 MB | 24 MB |
   | vertex buffers `sub_82307090` < xnPrimBuffer `sub_82443858` | 3 MB | 12 MB |
   | index buffers `sub_823071B8` < `sub_824438F8` | 1 MB | (few) |
   | four 3.6 MB surfaces (frontbuffers, `sub_82433B00`, `sub_8242FC58`/`FD98`) | 14 MB | 14 MB |

   So the graphics memory is TEXTURES (~260 MB of ~380 MB for the whole
   island). Moving them out of the 512 MB is the whole of step 3.

   WHY IT IS NOT JUST "ALLOCATE THEM ELSEWHERE": Microsoft's D3D gives the
   GPU addresses as `address & 0x1FFFFFFF` (an inline `clrlwi r,r,3` at 197
   places in D3D; only the ring set-up asks MmGetPhysicalAddress). A texture
   at a virtual address 0x4xxxxxxx would get a GPU address that is someone
   else's physical memory. Three readers of those GPU addresses matter:

   * D3D ITSELF turns a GPU address back into a CPU pointer in the
     0xC0000000 window (`addis r,r,-0x4000` after the mask) in 9 functions.
     One is the resource Lock `sub_82306AD8`: with lock flag 0x10 it returns
     that alias instead of the texture's own address; a texture in virtual
     memory locked that way would be written into another allocation. The
     other 8 (`sub_82307D08`, `sub_82307DB8`, `sub_82308850` = the ring
     kick, `sub_8230F170`, `sub_823164E0`, `sub_82316C48`, `sub_82318A50`,
     `sub_82321178`) need the same check.
   * The native renderer: it reads most textures from the D3D texture
     OBJECTS, whose fetch constants keep the VIRTUAL address (fine anywhere),
     but water, bump, particles and the fx grid read D3D's copy of the
     sampler fetch constants (device +1152, GPU addresses: recorder
     `ReadableAddressOfPhysical`). Those copies would have to keep the
     virtual address for such textures (the SetTexture mask site).
   * The texture cache's WRITE WATCH (findings/20) asks the SDK to watch
     PHYSICAL pages; a texture in virtual memory can't be watched and would
     be re-hashed every frame it is drawn (the 3.7 ms/frame of before the
     watch). It needs a watch of its own for the pool's pages (page
     protection + the fault handler the debug console's `who` already
     installs in front of the SDK's).

   Headroom today without step 3 (main heap moved): the whole of Wumpa Island
   leaves 130 MB of the 512 MB free.

## 5. Why not 64-bit pointers (yet)

The program is already a 64-bit x86-64 program. What stays 32-bit is the
game's own ADDRESSES: every pointer the game stores in its objects is a
4-byte number, and "guest memory" is one 4 GB block of host memory that
those numbers index. The generated code turns each PowerPC load or store
into `base + 32-bit address` (+ a byte swap: the data is big-endian), e.g.
`lwz r27,76(r29)` -> `REX_LOAD_U32(ctx.r29.u32 + 76)`, which the compiler
makes ONE x86 instruction (`mov %eax,(%rsi,%rdx,1)` in `sub_82357598`).
That part is not an emulation layer and costs about nothing. What IS the
pretend Xbox is the bookkeeping around it (the SDK's page tables, the
physical windows, the console's pool sizes): sections 2-4 replace that.

Making the addresses 64-bit means changing what the code ASSUMES, not a
switch:

| Where 4-byte pointers are baked in | Size |
|---|---|
| field offsets in every recompiled function (`76(r29)` = the 20th word of an object; a 64-bit layout moves every field after a pointer) | 28,673 functions, ~315,000 loads and stores |
| list steps of 4 (`{begin, end, capacity}` walked by `addi rX,rX,4`) | in thousands of loops |
| vtables and jump tables in the XEX image (32-bit entries), function pointers looked up as guest addresses (`REX_CALL_INDIRECT_FUNC`) | 8,532 indirect calls |
| object sizes passed to `new` (sizes counted for 4-byte pointers) | every allocation site |
| the PowerPC registers' 32-bit address arithmetic (`rlwinm`, `lwzx` with 32-bit wrap) | everywhere |

A recompiled function cannot use a 64-bit layout while any other recompiled
function touches the same object. So 64-bit pointers come only after the
code that touches those objects is hand-written C++ (the full decompilation),
object family by object family, at the very end.

What it would buy for this game: nothing a player sees. The game uses under
500 MB; with section 4's steps the 32-bit space gives it ~2 GB of virtual
memory (0x00000000-0x7EFFFFFF) plus whatever graphics memory the native
renderer allows. The big upgrade that IS reachable now: the port's own
allocator over that 2 GB (step 2), graphics memory freed from the 512 MB
(step 3), the SDK's page-table bookkeeping replaced by a plain mapping.

## 6. Still open

* Which game code, if any, hands heap memory to the emulated GPU (the
  reason step 1 is native-only).
* Heap 9's report (`CSlabDummyParentAllocator`: "used" reads as 4 GB).
* The emulated renderer stays bound by the 512 MB (launch option only).
