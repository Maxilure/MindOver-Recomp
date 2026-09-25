#!/usr/bin/env python3
"""
find_missing_functions.py -- find game functions the recompiler missed.

THE PROBLEM
-----------
`rexglue codegen` discovers functions by following `bl` calls, the
exception table (.pdata) and C++ vtables that carry RTTI. A function that
is ONLY reachable through a pointer stored somewhere, for example
  * a vtable without RTTI (COM-style interfaces, many SDK libraries)
  * a callback handed to the OS/audio system/thread creation
  * a table of function pointers (state machines, script bindings...)
may never get registered. Worse, the "vacancy expansion" step can glue it
onto the END of the function right before it. The recompiled game then
dies the moment anything calls it through the pointer:

    [FATAL] Call to invalid or unregistered function at guest address 0x82480008

HOW THIS SCRIPT FINDS THEM
--------------------------
1. Load the decrypted exe image (dump it with: xexdis game --dump out/default_image.bin)
2. Load every registered function address from generated/default/crash_mom_init.cpp
   (the `{ 0x820B0000, sub_820B0000 },` table).
3. Collect every CODE POINTER the program contains:
     a. DATA refs: any aligned 32-bit big-endian word in a non-code section
        whose value lands in the code section (vtables, callback tables...)
     b. CODE refs: `lis rX,hi` followed by `addi rY,rX,lo` / `ori rY,rX,lo`
        building a code address (how the game passes callbacks to functions)
4. Keep pointers that are NOT a registered function but DO look like a
   function start: the word before is a function end (blr, bctr, a plain
   `b`) or zero padding.
5. Throw away the known false positives (both verified by hand, 2026-09-25):
     * jump tables: a pointer to a list of code pointers
     * computed switches: `lis/addi` of a BASE label, then `add`, `mtctr`,
       `bctr` (jump to base + table[x]*4). The base is a case label INSIDE
       a function. Example: 0x8230040C.
     * SEH scope tables: .rdata holds `count, {begin, end, filter, jumpTarget}
       * count`. begin/end bound the `__try { }` region and jumpTarget is the
       `__except { }` body, all labels INSIDE a function. Examples:
       0x823046A4 (jumpTarget), 0x8246C630 (end). The `filter` field points
       at a real separate funclet, so that one is kept.
     * the XEX resource blob (XDBF: images, strings) is skipped entirely:
       random bytes there can look like code addresses (e.g. 0x8230EE50).
6. Candidates that the containing function ALSO jumps to with a plain
   branch are printed separately, commented out, for manual review. They're
   usually labels whose address is taken for another reason. Example:
   0x8245B790 is a function epilogue passed (with a frame pointer) to an
   unwind routine as its resume address. It is not a function.

It prints candidates as ready-to-paste lines for crash_mom_manifest.toml.
Always eyeball them with xexdis before pasting.

USAGE
-----
    python3 tools/find_missing_functions.py out/default_image.bin \\
        generated/default/crash_mom_init.cpp game/default.xex
"""

import os
import re
import struct
import sys
from bisect import bisect_right
from collections import defaultdict

# Reuse the XEX header parser for the section layout (code vs data ranges).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xex_info import Xex  # noqa: E402

SECTION_CODE = 1

# PowerPC instruction encodings used as "a function ended right before this" markers
BLR = 0x4E800020   # return
BCTR = 0x4E800420  # jump through CTR (tail call via pointer / switch)


def be32(buf, off):
    return struct.unpack_from(">I", buf, off)[0]


def is_function_end(word):
    """Is `word` an instruction/padding that can legitimately precede a new function?"""
    if word in (BLR, BCTR, 0x00000000):
        return True
    # unconditional `b target` (primary opcode 18, LK bit clear): tail call
    return (word >> 26) == 18 and (word & 1) == 0


def feeds_computed_jump(word_at, addr, limit=6):
    """
    Starting just after the instruction at `addr`, does the code reach
    `mtctr` then `bctr` within `limit` instructions? That's how a computed
    switch jumps to (base + offset), so the lis/addi value is a case label.
    """
    saw_mtctr = False
    for i in range(1, limit + 1):
        w = word_at(addr + 4 * i)
        if (w >> 26) == 31 and ((w >> 1) & 0x3FF) == 467 and ((w >> 11) & 0x3FF) == 0x120:
            saw_mtctr = True                    # mtspr CTR, rS  (spr 9, halves swapped)
        elif w == BCTR and saw_mtctr:
            return True
    return False


# Fields of an SEH scope table entry that point INSIDE a function (labels):
# begin (+0), end (+4) and jumpTarget (+12). The filter (+8) is a real funclet.
SEH_LABEL_FIELDS = (0, 4, 12)


def is_seh_label_slot(word_at, in_code, slot_addr, max_entries=64):
    """
    Is the data word at `slot_addr` the begin/end/jumpTarget field of entry
    #k of an SEH scope table?

        table+0            count            (1..64)
        table+4+16*i       begin, end, filter, jumpTarget   for i < count

    We try every field and every k. The table would start at
    slot - field - 16*k - 4, and it only counts if the count covers entry k
    AND every entry has code addresses with begin < end. Checking the whole
    table matters: a plain vtable is also a run of ascending code pointers,
    and a looser check (just begin < end) wrongly threw real vtable slots away.
    """
    for field in SEH_LABEL_FIELDS:
        for k in range(max_entries):
            table = slot_addr - field - 16 * k - 4
            count = word_at(table)
            if count is None or not (k < count <= max_entries):
                continue
            entries_ok = True
            for i in range(count):
                begin, end = word_at(table + 4 + 16 * i), word_at(table + 8 + 16 * i)
                if begin is None or end is None or not (in_code(begin) and in_code(end) and begin < end):
                    entries_ok = False
                    break
            if entries_ok:
                return True
    return False


def plain_branch_target(word, addr):
    """Target of a non-linking b/bc instruction at `addr`, else None."""
    op = word >> 26
    if word & 1:                                # LK bit set: bl/bcl are calls, not jumps
        return None
    if op == 18:                                # b: 24-bit signed displacement
        disp = word & 0x03FFFFFC
        disp -= 0x04000000 if disp & 0x02000000 else 0
    elif op == 16:                              # bc: 14-bit signed displacement
        disp = word & 0xFFFC
        disp -= 0x10000 if disp & 0x8000 else 0
    else:
        return None
    return (disp if word & 2 else addr + disp) & 0xFFFFFFFF   # AA bit = absolute


def load_registered(init_cpp):
    """Every guest address that has a recompiled function."""
    pattern = re.compile(r"\{\s*0x([0-9A-Fa-f]{8}),\s*([A-Za-z_][A-Za-z0-9_]*)\s*\}")
    funcs = {}
    with open(init_cpp) as f:
        for m in pattern.finditer(f.read()):
            funcs[int(m.group(1), 16)] = m.group(2)
    return funcs


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__.split("USAGE")[1])
    image_path, init_cpp, xex_path = sys.argv[1:]

    with open(image_path, "rb") as f:
        image = f.read()
    xex = Xex(xex_path)
    _, ranges = xex.sections()
    base = ranges[0][0]
    code_ranges = [(s, e) for s, e, kind in ranges if kind == SECTION_CODE]
    data_ranges = [(s, e) for s, e, kind in ranges if kind != SECTION_CODE]

    def in_code(addr):
        return any(s <= addr < e for s, e in code_ranges)

    def word_at(addr):
        """Big-endian word at a guest address, or None outside the image."""
        off = addr - base
        if off < 0 or off + 4 > len(image):
            return None
        return be32(image, off)

    registered = load_registered(init_cpp)
    starts = sorted(registered)
    print(f"[scan] {len(registered):,} registered functions, image base 0x{base:08X}")

    # refs[target] = list of (kind, where) describing who points at it
    refs = defaultdict(list)
    skipped = defaultdict(int)

    # Embedded XEX resources (XDBF blob) are opaque data, not pointer tables.
    resource_ranges = [(addr, addr + size) for _, addr, size in xex.resources()]

    def in_resource(addr):
        return any(s <= addr < e for s, e in resource_ranges)

    # --- 3a: code pointers stored in data sections ---------------------------
    for start, end in data_ranges:
        for addr in range(start, end, 4):
            if in_resource(addr):
                continue
            value = word_at(addr)
            if value % 4 == 0 and in_code(value):
                if is_seh_label_slot(word_at, in_code, addr):
                    skipped["SEH scope label"] += 1
                    continue
                refs[value].append(("data", addr))

    # --- 3b: code addresses built in registers: lis + addi/ori ---------------
    for start, end in code_ranges:
        for addr in range(start, end - 4, 4):
            insn = word_at(addr)
            if (insn >> 26) != 15 or ((insn >> 16) & 0x1F) != 0:
                continue                       # not `lis rD,imm` (addis rD,0,imm)
            rd, hi = (insn >> 21) & 0x1F, insn & 0xFFFF
            # look a few instructions ahead for the low half being added in
            for nxt in range(addr + 4, min(addr + 32, end), 4):
                insn2 = word_at(nxt)
                op = insn2 >> 26
                ra = (insn2 >> 16) & 0x1F
                if op == 14 and ra == rd:      # addi rY, rD, lo (signed)
                    lo = insn2 & 0xFFFF
                    value = ((hi << 16) + (lo - 0x10000 if lo & 0x8000 else lo)) & 0xFFFFFFFF
                elif op == 24 and ((insn2 >> 21) & 0x1F) == rd:  # ori rY, rD, lo
                    value = (hi << 16) | (insn2 & 0xFFFF)
                else:
                    continue
                if value % 4 == 0 and in_code(value):
                    if feeds_computed_jump(word_at, nxt):
                        skipped["computed switch base"] += 1
                    else:
                        refs[value].append(("code", addr))
                break

    # --- 4+5: filter to plausible, unregistered function starts --------------
    candidates = []
    for target, who in refs.items():
        if target in registered:
            continue                            # fine: already a function
        first = word_at(target)
        if first == 0 or in_code(first):
            continue                            # padding, or a jump table of code pointers
        if not is_function_end(word_at(target - 4)):
            continue                            # mid-function label (e.g. switch case)
        i = bisect_right(starts, target) - 1
        container_addr = starts[i] if i >= 0 else base
        # --- 6: does the containing function itself jump here? -------------
        internal = any(plain_branch_target(word_at(a), a) == target
                       for a in range(container_addr, target, 4))
        candidates.append((target, who, registered.get(container_addr, "?"), internal))

    candidates.sort()
    print(f"[scan] {sum(len(v) for v in refs.values()):,} code pointers kept "
          f"(ignored: {', '.join(f'{n} {k}' for k, n in skipped.items())})")
    print(f"[scan] {len(candidates)} point at unregistered function-like starts\n")

    def describe(who):
        kinds = defaultdict(list)
        for kind, where in who:
            kinds[kind].append(where)
        return ", ".join(f"{len(v)} {k} ref{'s' if len(v) > 1 else ''} "
                         f"(e.g. 0x{v[0]:08X})" for k, v in sorted(kinds.items()))

    for target, who, container, internal in candidates:
        if not internal:
            print(f"0x{target:08X} = {{}}  # {describe(who)}; was inside {container}")

    review = [c for c in candidates if c[3]]
    if review:
        print("\n# REVIEW MANUALLY: the containing function also jumps to these,")
        print("# so they're probably labels, not functions:")
        for target, who, container, _ in review:
            print(f"# 0x{target:08X} = {{}}  # {describe(who)}; branched to inside {container}")


if __name__ == "__main__":
    main()
