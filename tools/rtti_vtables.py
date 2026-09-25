#!/usr/bin/env python3
"""
rtti_vtables.py -- turn the game's C++ class names into vtables.

WHY
---
The game was built with Microsoft's C++ compiler with RTTI (run-time type
information) switched on, so the exe still contains the NAME of almost every
C++ class that has virtual functions (2469 names, 2026-09-25). Radical's
renderer is a set of such classes: the abstract interface (pddiDevice,
pddiRenderContext, pddiTexture...) and its Xbox 360 backend (xnDevice,
xnContext, xnTexture, xnShader...). Each class has a VTABLE: an array of
pointers to its virtual methods. Find the vtable and we have the address
of every method of that class, which is where a native renderer can take
over (docs/04-native-renderer.md).

HOW MSVC LAYS OUT RTTI (32-bit, here big-endian)
------------------------------------------------
    TypeDescriptor         (one per class; "td")
      +0  pointer to type_info's vtable
      +4  spare (0)
      +8  mangled name, e.g. ".?AVxnContext@pure3d@@" (= class pure3d::xnContext)

    CompleteObjectLocator  (one per vtable of a class; "col")
      +0  signature (0 on 32-bit)
      +4  offset of this vtable's sub-object inside the full object
          (0 = the main vtable; >0 = a secondary base under multiple inheritance)
      +8  constructor displacement offset
      +12 -> TypeDescriptor
      +16 -> ClassHierarchyDescriptor

    ClassHierarchyDescriptor ("chd")
      +0  signature, +4 attributes, +8 number of base classes (incl. itself),
      +12 -> array of pointers to BaseClassDescriptor
    BaseClassDescriptor: +0 -> TypeDescriptor, +4 contained bases, +8 mdisp, ...

    vtable[-1] -> CompleteObjectLocator      (the word just BEFORE the vtable)
    vtable[0..n] -> the virtual methods, in declaration order

So: name string -> td (string - 8) -> every col whose +12 is td ->
every word equal to that col's address -> vtable = that word + 4.
Slots run until the first word that isn't a code address (usually the next
class's col pointer).

Slot k of a derived class overrides slot k of its base when the pointers
differ. That's how we tell which methods the Xbox backend (xn*) actually
implements versus inherits from the generic pddi* layer.

USAGE
-----
    # dump the decrypted image once (gitignored, it's game code):
    out/build/linux-amd64-relwithdebinfo/xexdis game --dump out/default_image.bin

    python3 tools/rtti_vtables.py out/default_image.bin game/default.xex [regex]

  regex (default "^(xn|pddi)") filters class names (the demangled
  "Class@namespace" form). Prints each matching class: its base classes,
  every vtable (main + secondary) with its slots; slots that differ from
  the first base class's vtable are marked "*" (overridden / new).

  --summary  one line per class only (vtable addresses, slot counts, bases)

The output contains addresses only, no game data, but it's derived from the
game: keep dumps in logs/ or the scratchpad, summarize findings in docs/.
"""

import os
import re
import struct
import sys
from collections import defaultdict

# Reuse the XEX header parser for the section layout (code vs data ranges).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xex_info import Xex  # noqa: E402

IMAGE_BASE = 0x82000000
SECTION_CODE = 1


def be32(buf, off):
    return struct.unpack_from(">I", buf, off)[0]


def demangle_class(mangled):
    """'.?AVxnContext@pure3d@@' -> 'xnContext@pure3d' (good enough to read).

    Templates ('?$pddiStack@V...') are left as they are.
    """
    name = mangled[4:]  # drop ".?AV" / ".?AU"
    return name[:-2] if name.endswith("@@") else name


class Rtti:
    def __init__(self, image, code_ranges):
        self.img = image
        self.code_ranges = code_ranges
        self.end = IMAGE_BASE + len(image)

        # Every aligned word in the image, bucketed by value. RTTI links are
        # plain absolute pointers, so "who points at X" is a dict lookup.
        self.refs = defaultdict(list)
        for off in range(0, len(image) - 3, 4):
            v = be32(image, off)
            if IMAGE_BASE <= v < self.end:
                self.refs[v].append(IMAGE_BASE + off)

        self.type_descriptors = self._find_type_descriptors()
        self.classes = {}  # td address -> dict(name, cols=[...])
        self._find_vtables()

    # -- helpers ------------------------------------------------------------

    def word(self, addr):
        return be32(self.img, addr - IMAGE_BASE)

    def in_image(self, addr):
        return IMAGE_BASE <= addr < self.end

    def is_code(self, addr):
        return any(s <= addr < e for s, e in self.code_ranges)

    # -- step 1: TypeDescriptors from the name strings ------------------------

    def _find_type_descriptors(self):
        tds = {}
        for m in re.finditer(rb"\.\?A[VU][\x21-\x7e]+?@@\x00", self.img):
            name_off = m.start()
            if name_off % 4:  # names start at +8 of an aligned struct
                continue
            td = IMAGE_BASE + name_off - 8
            # +0 is the type_info vtable pointer: must at least be in the image.
            if not self.in_image(self.word(td)):
                continue
            tds[td] = m.group()[:-1].decode()
        return tds

    # -- step 2+3: CompleteObjectLocators, then vtables -----------------------

    def _find_vtables(self):
        for td, mangled in self.type_descriptors.items():
            entry = {"name": demangle_class(mangled), "td": td, "cols": [], "bases": []}
            for ref in self.refs.get(td, []):
                col = ref - 12
                if not self.in_image(col) or self.word(col) != 0:
                    continue  # signature must be 0
                chd = self.word(col + 16)
                if not self.in_image(chd):
                    continue
                vtables = [r + 4 for r in self.refs.get(col, [])]
                if not vtables:
                    continue
                if not entry["bases"]:
                    entry["bases"] = self._bases(chd)
                for vt in vtables:
                    entry["cols"].append(
                        {"col": col, "offset": self.word(col + 4), "vtable": vt,
                         "slots": self._slots(vt)})
            entry["cols"].sort(key=lambda c: (c["offset"], c["vtable"]))
            if entry["cols"]:
                self.classes[td] = entry

    def _bases(self, chd):
        """Base classes from the ClassHierarchyDescriptor, in MSVC's order:
        the class itself first, then its bases depth-first."""
        count = self.word(chd + 8)
        array = self.word(chd + 12)
        if count > 64 or not self.in_image(array):
            return []
        out = []
        for i in range(count):
            bcd = self.word(array + 4 * i)
            if not self.in_image(bcd):
                break
            btd = self.word(bcd)
            name = self.type_descriptors.get(btd)
            out.append((demangle_class(name) if name else f"?td_{btd:08X}",
                        self.word(bcd + 8)))  # (name, mdisp = offset in object)
        return out

    def _slots(self, vt):
        slots = []
        addr = vt
        while self.in_image(addr) and self.is_code(self.word(addr)):
            slots.append(self.word(addr))
            addr += 4
            if len(slots) > 512:
                break
        return slots

    # -- queries --------------------------------------------------------------

    def by_name(self):
        return {c["name"]: c for c in self.classes.values()}


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    summary = "--summary" in sys.argv
    if len(args) < 2:
        print(__doc__)
        sys.exit(1)
    image_path, xex_path = args[0], args[1]
    pattern = re.compile(args[2] if len(args) > 2 else r"^(xn|pddi)")

    with open(image_path, "rb") as f:
        image = f.read()
    _, ranges = Xex(xex_path).sections()
    code_ranges = [(s, e) for s, e, kind in ranges if kind == SECTION_CODE]

    rtti = Rtti(image, code_ranges)
    named = rtti.by_name()
    print(f"# {len(rtti.type_descriptors)} type descriptors, "
          f"{len(rtti.classes)} classes with vtables")

    for name in sorted(named):
        if not pattern.search(name):
            continue
        cls = named[name]
        bases = [b for b, _ in cls["bases"][1:]]
        vts = ", ".join(f"0x{c['vtable']:08X}(+{c['offset']}, {len(c['slots'])} slots)"
                        for c in cls["cols"])
        print(f"\n{name}  : {' -> '.join(bases) or '(no base)'}")
        print(f"  vtables: {vts}")
        if summary:
            continue
        # Compare each vtable with the same-offset vtable of the direct base
        # (the first base listed after the class itself).
        parent = named.get(bases[0]) if bases else None
        for c in cls["cols"]:
            pslots = []
            if parent:
                for pc in parent["cols"]:
                    if pc["offset"] == 0:
                        pslots = pc["slots"]
                        break
            print(f"  vtable 0x{c['vtable']:08X} (sub-object +{c['offset']}):")
            for i, s in enumerate(c["slots"]):
                mark = "*" if i >= len(pslots) or pslots[i] != s else " "
                print(f"    [{i:3d}] +0x{4 * i:03X} {mark} sub_{s:08X}")


if __name__ == "__main__":
    main()
