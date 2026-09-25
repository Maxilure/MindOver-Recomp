#!/usr/bin/env python3
"""
callgraph.py -- who calls whom in the game's code (direct calls only).

WHY
---
To decide where a native renderer cuts in (docs/04-native-renderer.md) we
need questions like "which game functions call into Microsoft's D3D library
directly, bypassing Radical's renderer (PDDI)?" answered for all ~29,000
functions at once. xexdis shows one function at a time; this builds the
whole graph in a second.

HOW
---
* Function starts come from the recompiler's own table
  (generated/default/crash_mom_init.cpp: `{ 0x820B0000, sub_820B0000 },`).
  A function is taken to run until the next function start: close enough
  for call-graph purposes (padding and data islands are rare in code).
* Every instruction in the code section is decoded just enough to spot
  direct branches:
      bl  target      (opcode 18, LK=1)  -> a call
      b   target      (opcode 18, LK=0)  -> a tail call IF target is a
                                            function start other than our own
  AA=1 (absolute) forms aren't used by this game's compiler; they're
  handled anyway.
* NOT seen: calls through pointers (virtual methods, callbacks: `bctrl`).
  rtti_vtables.py covers the virtual ones.

USAGE (as a script)
-------------------
    python3 tools/callgraph.py out/default_image.bin \\
        generated/default/crash_mom_init.cpp game/default.xex  CMD ADDR...

    callers ADDR...   functions that call ADDR directly
    callees ADDR...   functions ADDR calls directly
    reach ADDR...     everything reachable from ADDR (transitive callees)

Or `from callgraph import CallGraph` in another script.
"""

import os
import re
import struct
import sys
from bisect import bisect_right
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xex_info import Xex  # noqa: E402

IMAGE_BASE = 0x82000000
SECTION_CODE = 1


def load_function_starts(init_cpp):
    """Sorted list of every function address the recompiler registered."""
    starts = set()
    with open(init_cpp) as f:
        for m in re.finditer(r"\{ 0x([0-9A-Fa-f]{8}), sub_", f.read()):
            starts.add(int(m.group(1), 16))
    return sorted(starts)


class CallGraph:
    def __init__(self, image_path, init_cpp, xex_path):
        with open(image_path, "rb") as f:
            self.img = f.read()
        _, ranges = Xex(xex_path).sections()
        self.code_ranges = [(s, e) for s, e, kind in ranges if kind == SECTION_CODE]
        self.starts = load_function_starts(init_cpp)
        self.start_set = set(self.starts)
        self.callees = defaultdict(set)   # func -> {func}
        self.callers = defaultdict(set)   # func -> {func}
        self.call_sites = defaultdict(list)  # callee -> [(caller, site address)]
        self._build()

    def func_of(self, addr):
        """The function containing addr (the closest start at or below it)."""
        i = bisect_right(self.starts, addr) - 1
        return self.starts[i] if i >= 0 else None

    def _build(self):
        for cs, ce in self.code_ranges:
            for addr in range(cs, ce, 4):
                ins = struct.unpack_from(">I", self.img, addr - IMAGE_BASE)[0]
                if ins >> 26 != 18:  # I-form branch: b / bl / ba / bla
                    continue
                li = ins & 0x03FFFFFC
                if li & 0x02000000:  # sign-extend the 26-bit displacement
                    li -= 0x04000000
                aa, lk = (ins >> 1) & 1, ins & 1
                target = (li & 0xFFFFFFFF) if aa else (addr + li) & 0xFFFFFFFF
                caller = self.func_of(addr)
                if caller is None:
                    continue
                if lk:
                    callee = self.func_of(target) if target not in self.start_set else target
                elif target in self.start_set and target != caller:
                    callee = target  # tail call
                else:
                    continue  # ordinary jump inside a function
                if callee is None:
                    continue
                self.callees[caller].add(callee)
                self.callers[callee].add(caller)
                self.call_sites[callee].append((caller, addr))

    def reach(self, roots, stop=lambda f: False):
        """Transitive callees of roots. `stop(f)` = don't descend into f
        (it's still included in the result)."""
        seen, todo = set(), list(roots)
        while todo:
            f = todo.pop()
            if f in seen:
                continue
            seen.add(f)
            if not stop(f):
                todo.extend(self.callees.get(f, ()))
        return seen


def main():
    if len(sys.argv) < 6:
        print(__doc__)
        sys.exit(1)
    image, init_cpp, xex, cmd = sys.argv[1:5]
    addrs = [int(a, 16) for a in sys.argv[5:]]
    g = CallGraph(image, init_cpp, xex)
    for a in addrs:
        if cmd == "callers":
            for caller, site in sorted(g.call_sites.get(a, [])):
                print(f"sub_{caller:08X}  (call at 0x{site:08X})")
        elif cmd == "callees":
            for c in sorted(g.callees.get(a, ())):
                print(f"sub_{c:08X}")
        elif cmd == "reach":
            for c in sorted(g.reach([a])):
                print(f"sub_{c:08X}")
        else:
            print(f"unknown command {cmd}")
            sys.exit(1)


if __name__ == "__main__":
    main()
