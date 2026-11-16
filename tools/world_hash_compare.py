#!/usr/bin/env python3
"""world_hash_compare.py -- where two runs of the game stop being identical.

Reads two files written by the debug console's `worldhash <file>` (format in
src/debug/world_hash.h: a per-game-frame fingerprint of the random generator,
the players' bodies and every 4 KB page of the game's global data), lines the
frames up and reports:

  * frames compared, and how many matched in every part;
  * the FIRST frame where the random generator differs (a random draw more or
    less: an enemy decided differently, an effect ran once more...);
  * the FIRST frame where the players differ;
  * pages that differ (address + first/last frame + how many frames), split
    into NOISY pages (differ on most frames from the start: real-time counters
    on other threads) and the rest, earliest first;
  * with --dump-a/--dump-b (the `worldhash ... dump <frame>` files of the same
    frame): the differing 32-bit words inside the differing pages.

Frames line up on the replay frame when both runs have one (the `replay`
command's own count), else on the fixed-step frame number.

  python3 tools/world_hash_compare.py a.whash b.whash [--ignore-page 0x8258E000 ...]
      [--dump-a a.whash.frame600.bin --dump-b b.whash.frame600.bin]
"""

import argparse
import struct
import sys

HEADER = struct.Struct("<8sIIII Q")
RECORD_HEAD = struct.Struct("<qqIIIIQQ")


def read(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, version, pages, first, page_size, _ = HEADER.unpack_from(data, 0)
    if magic != b"MORWHASH" or version != 1:
        sys.exit(f"{path}: not a world hash file (v1)")
    size = RECORD_HEAD.size + 8 * pages
    frames = {}
    for off in range(HEADER.size, len(data) - size + 1, size):
        frame, replay, rx, ry, present, _, players, total = RECORD_HEAD.unpack_from(data, off)
        page_hashes = struct.unpack_from(f"<{pages}Q", data, off + RECORD_HEAD.size)
        frames.setdefault((frame, replay), (rx, ry, present, players, total, page_hashes))
    return frames, pages, first, page_size


def key_map(frames, use_replay):
    out = {}
    for (frame, replay), rec in frames.items():
        k = replay if use_replay else frame
        if use_replay and replay < 0:
            continue
        out.setdefault(k, rec)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--ignore-page", action="append", default=[], help="page address to leave out")
    ap.add_argument("--dump-a")
    ap.add_argument("--dump-b")
    ap.add_argument("--noisy", type=float, default=0.5,
                    help="a page that differs on more than this share of frames counts as noisy")
    args = ap.parse_args()

    fa, pages, first, page_size = read(args.a)
    fb, pages_b, first_b, _ = read(args.b)
    if (pages, first) != (pages_b, first_b):
        sys.exit("the two files cover different memory")
    use_replay = any(r >= 0 for _, r in fa) and any(r >= 0 for _, r in fb)
    ma, mb = key_map(fa, use_replay), key_map(fb, use_replay)
    keys = sorted(set(ma) & set(mb))
    if not keys:
        sys.exit("no frames in common")
    ignored = {int(x, 16) for x in args.ignore_page}

    print(f"lined up on {'replay frame' if use_replay else 'frame'}: {len(keys)} frames in common "
          f"({keys[0]}..{keys[-1]}); a has {len(ma)}, b has {len(mb)}")

    first_rng = first_players = None
    page_diff = {}  # page index -> list of keys
    fully_equal = 0
    for k in keys:
        ra, rb = ma[k], mb[k]
        if (ra[0], ra[1]) != (rb[0], rb[1]) and first_rng is None:
            first_rng = (k, ra[:2], rb[:2])
        if (ra[2], ra[3]) != (rb[2], rb[3]) and first_players is None:
            first_players = k
        same = (ra[0], ra[1], ra[2], ra[3]) == (rb[0], rb[1], rb[2], rb[3])
        for i, (ha, hb) in enumerate(zip(ra[5], rb[5])):
            if ha != hb and first + i * page_size not in ignored:
                page_diff.setdefault(i, []).append(k)
                same = False
        fully_equal += same

    print(f"frames identical in every part: {fully_equal} of {len(keys)}")
    if first_rng:
        k, a, b = first_rng
        print(f"RANDOM GENERATOR first differs at {k}: a {a[0]:08X} {a[1]:08X}, b {b[0]:08X} {b[1]:08X}")
    else:
        print("random generator: identical on every frame")
    print(f"players: {'first differ at ' + str(first_players) if first_players is not None else 'identical on every frame'}")

    noisy, real = [], []
    for i, ks in page_diff.items():
        (noisy if len(ks) > args.noisy * len(keys) else real).append((ks[0], i, ks))
    for title, items in (("pages that differ (earliest first)", sorted(real)),
                         ("NOISY pages (differ on most frames)", sorted(noisy))):
        print(f"\n{title}: {len(items)}")
        for k0, i, ks in items[:40]:
            print(f"  0x{first + i * page_size:08X}  first {k0}, last {ks[-1]}, {len(ks)} frames")

    if args.dump_a and args.dump_b:
        da, db = open(args.dump_a, "rb").read(), open(args.dump_b, "rb").read()
        print("\nwords that differ in the dumps:")
        shown = 0
        for off in range(0, min(len(da), len(db)) - 3, 4):
            if da[off:off + 4] != db[off:off + 4]:
                a = struct.unpack(">I", da[off:off + 4])[0]
                b = struct.unpack(">I", db[off:off + 4])[0]
                print(f"  0x{first + off:08X}: a {a:08X}  b {b:08X}")
                shown += 1
                if shown >= 200:
                    print("  ... (200 shown)")
                    break


if __name__ == "__main__":
    main()
