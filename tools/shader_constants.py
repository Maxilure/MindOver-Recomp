#!/usr/bin/env python3
"""
shader_constants.py -- list the named constants of a game shader (.out file)

WHY
    To rewrite one of the game's materials natively (docs/04-native-renderer.md)
    we need to know which register each of its settings lives in: pixel shader
    constant c103 is "RefractBumpiness", bool b11 "IsEnableRefract", sampler 4
    "BackGroundMap"... Most of the game's pixel shaders ship as microcode only
    (no debug source), but every compiled D3D shader keeps its CONSTANT TABLE:
    the names of its parameters with their register numbers. First used for
    xnBumpMegaShader (2026-09-27): its table confirmed the registers read off
    the microcode, and named the samplers.

FORMAT
    A D3DXSHADER_CONSTANTTABLE, big-endian like everything on the Xbox 360,
    near the start of the .out file:
        u32 Size, Creator, Version, Constants, ConstantInfo, Flags, Target
    then `Constants` entries of 20 bytes at table + ConstantInfo:
        u32 Name (offset from the table), u16 RegisterSet (0 bool, 1 int,
        2 float, 3 sampler), u16 RegisterIndex, u16 RegisterCount,
        u16 Reserved, u32 TypeInfo, u32 DefaultValue
    The table's offset isn't stored anywhere we rely on: we try each 4-byte
    aligned position and keep the first whose entries point at readable
    names.

    Pixel shader bools are numbered here as D3D does (b10); in the
    disassembled microcode (--dump_shaders) the same bool is b138 (128 + N).

USAGE
    python3 tools/shader_constants.py game/shaders/bumpmegafp.out [more.out...]

The .out files are game content: this only prints names and numbers for
local use (describe them in the docs, never commit the files).
"""

import struct
import sys

REGISTER_SETS = ["bool", "int", "float", "sampler"]


def find_table(data):
    """Returns (offset, [(name, set, index, count)]) of the constant table, or None."""
    for table in range(0, min(len(data), 4096) - 28, 4):
        size, creator, version, count, info, flags, target = struct.unpack_from(">7I", data, table)
        if not (1 <= count <= 256 and 28 <= info < 4096 and table + info + 20 * count <= len(data)):
            continue
        entries = []
        for i in range(count):
            name, reg_set, index, reg_count, _reserved, _type, _default = struct.unpack_from(
                ">IHHHHII", data, table + info + 20 * i)
            if reg_set >= len(REGISTER_SETS) or not 0 < name < len(data) - table:
                break
            text = data[table + name:table + name + 64].split(b"\0")[0]
            if not text or not all(32 < c < 127 for c in text):
                break
            entries.append((text.decode(), REGISTER_SETS[reg_set], index, reg_count))
        if len(entries) == count:
            return table, entries
    return None


def main(paths):
    for path in paths:
        with open(path, "rb") as f:
            data = f.read()
        found = find_table(data)
        print(f"== {path}")
        if not found:
            print("   no constant table found")
            continue
        _, entries = found
        prefix = {"bool": "b", "int": "i", "float": "c", "sampler": "s"}
        for name, reg_set, index, count in sorted(entries, key=lambda e: (e[1], e[2])):
            span = f"{prefix[reg_set]}{index}" + (f"-{prefix[reg_set]}{index + count - 1}" if count > 1 else "")
            print(f"   {span:10} {name}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    main(sys.argv[1:])
