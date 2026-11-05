#!/usr/bin/env python3
# =============================================================================
# tools/make_icon.py -- the port's icon, taken from the PLAYER'S OWN DISC
# =============================================================================
#
# WHAT: the game exe's icon (Windows Start menu, taskbar, Apps list) and the
# Linux menu entry's picture: the game's own HUD PORTRAIT of Crash as a mask
# ("MugshotCrashMask.tga", the face shown in the co-op corner for a player
# riding as a mask).
#
# WHY FROM THE DISC: the picture is game art (Radical Entertainment's). The
# port never ships game files, so the icon is made on the player's computer
# from the copy of the game they extracted, like the game itself is built
# there. Nothing of it is in the repository.
#
# WHERE IT IS: default.rcf -> package\c1e387c7.p3d (the in-game menus
# package; every level group's InGame package holds a copy too). Inside
# (Pure3D chunks, little-endian: u32 id, u32 header+data size, u32 total
# size, then a u8-length name):
#   0x19000 texture "MugshotCrashMask.tga" (64 x 64)
#     0x19001 image
#       0x19002 image data: u32 size + a TGA file: type 1 (colour-mapped,
#               uncompressed), a 256-entry palette of 32-bit B G R A, 8-bit
#               indices, descriptor bit 0x20 = rows top to bottom.
# The RCF archive (RCF 2.1, big-endian header; src/audio_trace.cpp header):
# 0x24 directory offset/size, names offset/size, 0, file count; directory =
# (hash, offset, size) sorted by hash; names in DATA order.
#
# OUT (Python standard library only: the build already needs Python, nothing
# else is installed for this): the portrait at 16-256 px (smaller sizes
# averaged down, 128/256 smoothly enlarged from its 64 px), one PNG of 256 px
# and one .ico holding every size (PNG-compressed entries, Vista and later).
#
# USE: make_icon.py <default.rcf> <out folder>
#   -> <out>/crash_mom.png (256 px) + <out>/crash_mom.ico
# Run by the launcher/installer's setup after the game is copied into
# program/ (launcher/setup.cpp); exits 1 with a message when the disc doesn't
# have what it expects (the game then simply has no icon).
# =============================================================================

import os
import struct
import sys
import zlib

# Where to look, in order (the first package that has it wins).
PACKAGES = ("package\\c1e387c7.p3d", "package\\7a8185b0.p3d")
TEXTURE = b"MugshotCrashMask.tga"
SIZES = (16, 24, 32, 48, 64, 128, 256)


# --- the archive --------------------------------------------------------------

def read_from_rcf(rcf_path, wanted):
    """Files' bytes out of an RCF 2.1 archive, by name: {name: bytes}."""
    wanted = {w.lower() for w in wanted}
    found = {}
    with open(rcf_path, "rb") as f:
        head = f.read(0x3C)
        dir_off, _dir_size, names_off, names_size, _zero, count = struct.unpack(">6I", head[0x24:0x3C])
        f.seek(dir_off)
        directory = f.read(12 * count)
        # The directory is sorted by hash; the names come in DATA order, so
        # the entries sorted by offset line up with the names.
        entries = sorted(struct.unpack(">3I", directory[12 * i:12 * i + 12])[1:] for i in range(count))
        f.seek(names_off)
        names = f.read(names_size)
        p = 8
        for i in range(count):
            p += 12
            (length,) = struct.unpack("<I", names[p:p + 4])
            p += 4
            name = names[p:p + length - 1].decode("latin1")
            p += length + 3
            if name.lower() in wanted:
                off, size = entries[i]
                f.seek(off)
                found[name.lower()] = f.read(size)
    return found


# --- the package --------------------------------------------------------------

def chunks(data, begin, end):
    """(id, start, data end, total end) of the chunks directly in [begin, end)."""
    p = begin
    while p + 12 <= end:
        cid, data_size, total = struct.unpack("<3I", data[p:p + 12])
        if total < 12 or data_size < 12 or p + total > end:
            return
        yield cid, p, p + data_size, p + total
        p += total


def chunk_name(data, start, data_end):
    if data_end - start <= 13:
        return b""
    length = data[start + 12]
    return data[start + 13:start + 13 + length].split(b"\0")[0]


def find_tga(data, begin, end):
    """The texture's TGA file (searches the whole tree), or None."""
    for cid, start, data_end, total_end in chunks(data, begin, end):
        if cid == 0x19000 and chunk_name(data, start, data_end) == TEXTURE:
            for c1, _s1, de1, e1 in chunks(data, data_end, total_end):
                if c1 != 0x19001:
                    continue
                for c2, s2, _de2, _e2 in chunks(data, de1, e1):
                    if c2 == 0x19002:
                        (size,) = struct.unpack("<I", data[s2 + 12:s2 + 16])
                        return data[s2 + 16:s2 + 16 + size]
        found = find_tga(data, data_end, total_end)
        if found:
            return found
    return None


def decode_tga(t):
    """Colour-mapped TGA (8-bit indices, 32-bit B G R A palette) -> (w, h, [(r, g, b, a)])."""
    id_length, cmap_type, image_type = t[0], t[1], t[2]
    cmap_start, cmap_length, cmap_bits = struct.unpack("<HHB", t[3:8])
    w, h, bits, descriptor = struct.unpack("<HHBB", t[12:18])
    if cmap_type != 1 or image_type != 1 or cmap_bits != 32 or bits != 8:
        raise SystemExit("make_icon: the portrait has an unexpected format")
    p = 18 + id_length
    palette = []
    for i in range(cmap_length):
        b, g, r, a = t[p + 4 * i:p + 4 * i + 4]
        palette.append((r, g, b, a))
    p += 4 * cmap_length
    pixels = [palette[i - cmap_start] for i in t[p:p + w * h]]
    if not descriptor & 0x20:  # rows stored bottom to top
        pixels = [px for y in range(h - 1, -1, -1) for px in pixels[y * w:(y + 1) * w]]
    return w, h, pixels


# --- resizing (premultiplied alpha: no dark fringe at the edges) --------------

def shrink(pixels, size, new):
    """Box filter to a smaller square."""
    out = []
    f = size / new
    for y in range(new):
        for x in range(new):
            r = g = b = a = cnt = 0.0
            for sy in range(int(y * f), max(int((y + 1) * f), int(y * f) + 1)):
                for sx in range(int(x * f), max(int((x + 1) * f), int(x * f) + 1)):
                    pr, pg, pb, pa = pixels[sy * size + sx]
                    r += pr * pa
                    g += pg * pa
                    b += pb * pa
                    a += pa
                    cnt += 1
            out.append((0, 0, 0, 0) if a == 0 else
                       (int(r / a + 0.5), int(g / a + 0.5), int(b / a + 0.5), int(a / cnt + 0.5)))
    return out


def enlarge(pixels, size, new):
    """Bilinear to a bigger square (pixel centres lined up, edges clamped)."""
    out = []
    f = size / new
    for y in range(new):
        fy = min(max((y + 0.5) * f - 0.5, 0.0), size - 1.0)
        y0 = int(fy)
        y1, ty = min(y0 + 1, size - 1), fy - y0
        for x in range(new):
            fx = min(max((x + 0.5) * f - 0.5, 0.0), size - 1.0)
            x0 = int(fx)
            x1, tx = min(x0 + 1, size - 1), fx - x0
            r = g = b = a = 0.0
            for (sx, sy, k) in ((x0, y0, (1 - tx) * (1 - ty)), (x1, y0, tx * (1 - ty)),
                                (x0, y1, (1 - tx) * ty), (x1, y1, tx * ty)):
                pr, pg, pb, pa = pixels[sy * size + sx]
                r += pr * pa * k
                g += pg * pa * k
                b += pb * pa * k
                a += pa * k
            out.append((0, 0, 0, 0) if a <= 0 else
                       (min(255, int(r / a + 0.5)), min(255, int(g / a + 0.5)),
                        min(255, int(b / a + 0.5)), int(a + 0.5)))
    return out


# --- PNG and ICO out ----------------------------------------------------------

def encode_png(w, h, pixels):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for r, g, b, a in pixels[y * w:(y + 1) * w]:
            raw += bytes((r, g, b, a))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def encode_ico(pngs):
    """[(size, png bytes)] -> .ico with PNG entries."""
    out = struct.pack("<HHH", 0, 1, len(pngs))
    offset = 6 + 16 * len(pngs)
    body = b""
    for size, png in pngs:
        dim = 0 if size >= 256 else size  # 0 = 256
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(png), offset + len(body))
        body += png
    return out + body


def main():
    if len(sys.argv) != 3:
        print("usage: make_icon.py <default.rcf> <out folder>", file=sys.stderr)
        return 2
    rcf, out_dir = sys.argv[1], sys.argv[2]
    packages = read_from_rcf(rcf, PACKAGES)
    tga = None
    for name in PACKAGES:
        data = packages.get(name.lower())
        if data:
            tga = find_tga(data, 12, len(data))
            if tga:
                break
    if not tga:
        print("make_icon: no %s in %s" % (TEXTURE.decode(), rcf), file=sys.stderr)
        return 1
    w, h, pixels = decode_tga(tga)
    if w != h:
        print("make_icon: the portrait isn't square (%dx%d)" % (w, h), file=sys.stderr)
        return 1
    os.makedirs(out_dir, exist_ok=True)
    pngs = []
    for size in SIZES:
        if size == w:
            px = pixels
        elif size < w:
            px = shrink(pixels, w, size)
        else:
            px = enlarge(pixels, w, size)
        pngs.append((size, encode_png(size, size, px)))
    with open(os.path.join(out_dir, "crash_mom.png"), "wb") as f:
        f.write(pngs[-1][1])
    with open(os.path.join(out_dir, "crash_mom.ico"), "wb") as f:
        f.write(encode_ico(pngs))
    print("make_icon: wrote crash_mom.png + crash_mom.ico (%s px) in %s"
          % (", ".join(str(s) for s in SIZES), out_dir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
