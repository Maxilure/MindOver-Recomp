#!/usr/bin/env python3
"""
xiso_extract.py -- pull the game files out of an Xbox 360 disc image.

WHY THIS EXISTS
---------------
The port never runs from the .iso directly. We need two things out of it:
  1. default.xex  -> the game's executable. `rexglue codegen` translates it
                     into C++ (see docs/02-how-the-port-works.md).
  2. everything else (models, textures, sounds, levels) -> the recompiled
     game reads these at runtime, exactly like it read them off the disc.
     ReXGlue mounts this folder as the game's `game:\\` / `d:\\` drive.

HOW AN XBOX 360 DISC IS LAID OUT
--------------------------------
Xbox discs use Microsoft's own filesystem called XDVDFS (a.k.a. "XISO").
It is NOT ISO-9660, which is why 7-Zip / normal mount tools show you only a
tiny "video" partition and not the game.

A full disc dump contains more than just the game partition. Depending on
the disc generation, the XDVDFS partition starts at a different offset:

    Disc type          | Game partition starts at
    -------------------+-------------------------
    XGD1 (orig. Xbox)  | 0x18300000
    XGD2 (most 360)    | 0x0FD90000   <- Crash: Mind over Mutant is this one
    XGD3 (late 360)    | 0x02080000
    "Trimmed"/rebuilt  | 0x00000000

Everything inside the partition is addressed in 2048-byte SECTORS counted
from the partition start (so byte offset = partition_offset + sector*2048).

The volume descriptor ("superblock") sits at sector 32 of the partition:

    offset  size  field
    0x000   20    magic  "MICROSOFT*XBOX*MEDIA"
    0x014   4     root directory: first sector      (little-endian u32)
    0x018   4     root directory: size in bytes     (little-endian u32)
    0x01C   8     creation time (Windows FILETIME)
    0x7EC   20    magic again (sanity check)

DIRECTORIES ARE BINARY TREES
----------------------------
A directory is a blob of "dirent" records. Instead of being a flat list,
each record has a LEFT and RIGHT child offset: the entries form a binary
search tree sorted by (case-insensitive) name. To list a directory you
start at offset 0 and walk the tree.

    offset  size  field
    0x00    2     left child offset   (in 4-byte units, 0 = no child)
    0x02    2     right child offset  (in 4-byte units, 0 = no child)
    0x04    4     first sector of the file/subdirectory data
    0x08    4     size in bytes
    0x0C    1     attributes (0x10 = directory, like Windows FILE_ATTRIBUTE_*)
    0x0D    1     name length
    0x0E    n     name (ASCII, not null-terminated)
    ...           padded with 0xFF up to a 4-byte boundary

Records never straddle a sector boundary; unused space at the end of a
sector is filled with 0xFF. We never read that padding because we only
follow tree links, never scan linearly.

All multi-byte numbers in XDVDFS are LITTLE-endian (the original Xbox was
x86). This is the opposite of the game code itself (PowerPC = big-endian).

USAGE
-----
    python3 tools/xiso_extract.py "Crash - Mind over Mutant (USA).iso" game/
    python3 tools/xiso_extract.py --list "Crash - Mind over Mutant (USA).iso"
"""

import argparse
import os
import struct
import sys

SECTOR_SIZE = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
VOLUME_DESCRIPTOR_SECTOR = 32  # 32 * 2048 = 0x10000 bytes into the partition

# Where the XDVDFS partition might begin, most likely first for 360 games.
PARTITION_OFFSETS = {
    "XGD2": 0x0FD90000,
    "XGD3": 0x02080000,
    "XGD1": 0x18300000,
    "plain": 0x00000000,
}

ATTR_DIRECTORY = 0x10

# Dirent header = left(u16) right(u16) sector(u32) size(u32) attr(u8) namelen(u8)
DIRENT_HEADER = struct.Struct("<HHIIBB")


class XisoImage:
    """Read-only view of the XDVDFS partition inside a disc image file."""

    def __init__(self, path):
        self.f = open(path, "rb")
        self.disc_type, self.base = self._find_partition()

        # Parse the volume descriptor (see module docstring for the layout).
        vd = self._read_at(VOLUME_DESCRIPTOR_SECTOR * SECTOR_SIZE, SECTOR_SIZE)
        self.root_sector, self.root_size = struct.unpack_from("<II", vd, 0x14)
        if vd[0x7EC:0x7EC + 20] != MAGIC:
            raise ValueError("volume descriptor trailing magic mismatch -- corrupt image?")

    def _find_partition(self):
        """Probe each known partition offset for the XDVDFS magic."""
        for disc_type, offset in PARTITION_OFFSETS.items():
            self.f.seek(offset + VOLUME_DESCRIPTOR_SECTOR * SECTOR_SIZE)
            if self.f.read(len(MAGIC)) == MAGIC:
                return disc_type, offset
        raise ValueError("no XDVDFS partition found -- is this an Xbox/Xbox 360 image?")

    def _read_at(self, partition_offset, size):
        """Read `size` bytes at a byte offset relative to the partition start."""
        self.f.seek(self.base + partition_offset)
        return self.f.read(size)

    def walk(self, dir_sector=None, dir_size=None, prefix=""):
        """
        Yield (path, attributes, first_sector, size) for every entry, depth-first.
        Paths use '/' separators and are relative to the disc root.
        """
        if dir_sector is None:
            dir_sector, dir_size = self.root_sector, self.root_size
        if dir_size == 0:
            return  # empty directory: no tree at all

        table = self._read_at(dir_sector * SECTOR_SIZE, dir_size)

        # Walk the binary tree with an explicit stack (in-order, so output
        # comes out alphabetically -- handy for diffing listings).
        stack, node, seen = [], 0, set()
        while stack or node is not None:
            while node is not None:
                if node in seen or node + DIRENT_HEADER.size > len(table):
                    raise ValueError(f"corrupt directory tree in {prefix or '/'}")
                seen.add(node)
                stack.append(node)
                left = DIRENT_HEADER.unpack_from(table, node)[0]
                node = left * 4 if left not in (0, 0xFFFF) else None

            node = stack.pop()
            left, right, sector, size, attr, name_len = DIRENT_HEADER.unpack_from(table, node)
            name_start = node + DIRENT_HEADER.size
            name = table[name_start:name_start + name_len].decode("ascii")
            path = f"{prefix}{name}"

            yield path, attr, sector, size
            if attr & ATTR_DIRECTORY:
                yield from self.walk(sector, size, prefix=path + "/")

            node = right * 4 if right not in (0, 0xFFFF) else None

    def extract_file(self, sector, size, dest_path, chunk=8 * 1024 * 1024):
        """Copy one file's bytes out in chunks (some game archives are GBs)."""
        self.f.seek(self.base + sector * SECTOR_SIZE)
        remaining = size
        with open(dest_path, "wb") as out:
            while remaining:
                data = self.f.read(min(chunk, remaining))
                if not data:
                    raise IOError(f"unexpected end of image while writing {dest_path}")
                out.write(data)
                remaining -= len(data)


def human(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024


def main():
    ap = argparse.ArgumentParser(description="Extract an Xbox 360 (XDVDFS) disc image.")
    ap.add_argument("iso", help="path to the .iso disc image")
    ap.add_argument("out_dir", nargs="?", default="game", help="where to extract (default: game/)")
    ap.add_argument("--list", action="store_true", help="only list files, extract nothing")
    args = ap.parse_args()

    img = XisoImage(args.iso)
    print(f"[xiso] {args.iso}")
    print(f"[xiso] disc type {img.disc_type}, game partition at 0x{img.base:X}")

    files = dirs = total = 0
    for path, attr, sector, size in img.walk():
        if attr & ATTR_DIRECTORY:
            dirs += 1
            if args.list:
                print(f"  {'<DIR>':>10}  {path}/")
            else:
                os.makedirs(os.path.join(args.out_dir, path), exist_ok=True)
            continue

        files += 1
        total += size
        if args.list:
            print(f"  {human(size):>10}  {path}")
        else:
            dest = os.path.join(args.out_dir, path)
            os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
            print(f"  {human(size):>10}  {path}", flush=True)
            img.extract_file(sector, size, dest)

    verb = "listed" if args.list else f"extracted to {args.out_dir}/"
    print(f"[xiso] {files} files in {dirs} directories, {human(total)} {verb}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, IOError) as e:
        sys.exit(f"[xiso] error: {e}")
