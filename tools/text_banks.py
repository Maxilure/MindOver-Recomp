#!/usr/bin/env python3
"""tools/text_banks.py -- the GAME's own menu words in every language, as a
glossary for the people translating the PORT's texts (languages/*.po).

Reads the player's own disc files (game/default.rcf) and writes, for each
language, every string of the game's text banks next to its English one:

    python3 tools/text_banks.py game/default.rcf <out folder>

-> <out folder>/<code>.tsv   bank, key, English, the language's text
   (one line per string; tabs and newlines in a text written as \\t \\n)

The output is the game's own text: it stays on the player's machine and is
NEVER committed (the repo's rule 1). Use it to look up how the game itself
says "Options", "Yes", "Save game", "Controller", "Drop Out"... so the port's
texts use the same words.

The format (findings/30, findings/49):
  default.rcf = RCF 2.1 archive (big-endian header at 0x24: directory
  offset/size, names offset/size, 0, count; directory = hash/offset/size
  sorted by hash; names in data order).
  package\\*.p3d = Pure3D chunks (little-endian id, data size, total size).
  Chunk 0x1800D = a text bank: its name, then one 0x1800E child per
  language. A child's name starts with the language LETTER (findings/49
  s.1: U English US, E UK, F French, ...), then u32 count, u32 modulus,
  u32 size, count keys (u32), count offsets (u32, bytes into the string
  area), then the strings: UTF-16 little-endian, each ended by 0.
  A string's key is the same in every language of a bank, which is what
  pairs the English with its translation (the key -> id name hash is
  unknown, findings/30: the ids aren't needed for this).
"""
import mmap
import os
import struct
import sys

# The game's language letters -> our .po codes (src/language/texts.cpp CodeOf).
LETTERS = {"F": "fr", "Z": "fr_CA", "G": "de", "I": "it", "S": "es", "D": "nl", "A": "da",
           "H": "fi", "N": "nb", "W": "sv", "R": "ru", "U": "en", "E": "en_GB"}


def rcf_files(d):
    """(name, offset, size) of every file in an RCF 2.1 archive."""
    dir_off, dir_size, names_off, names_size, _, count = struct.unpack(">6I", d[0x24:0x3C])
    # The directory is sorted by hash; the names come in DATA order, so sort
    # the entries by offset to pair them.
    ents = sorted(struct.unpack(">3I", d[dir_off + 12 * i:dir_off + 12 * i + 12])[1:] for i in range(count))
    p = names_off + 8
    for i in range(count):
        p += 12
        ln = struct.unpack("<I", d[p:p + 4])[0]
        p += 4
        name = bytes(d[p:p + ln - 1]).decode("latin1")
        p += ln + 3
        yield name, ents[i][0], ents[i][1]


def chunks(d, begin, end):
    p = begin
    while p + 12 <= end:
        cid, ds, ts = struct.unpack("<3I", d[p:p + 12])
        if ts < 12 or ds < 12 or ds > ts or p + ts > end:
            break
        yield cid, p, ds, ts
        p += ts


def chunk_name(d, p):
    n = d[p + 12]
    return bytes(d[p + 13:p + 13 + n]).split(b"\0")[0].decode("latin1"), p + 13 + n


def banks(d, begin, end):
    """{(bank name, letter): {key: text}} of one package."""
    out = {}
    for cid, p, ds, ts in chunks(d, begin, end):
        if cid != 0x1800D:
            continue
        bank, _ = chunk_name(d, p)
        for c, q, cds, cts in chunks(d, p + ds, p + ts):
            if c != 0x1800E:
                continue
            _, o = chunk_name(d, q)
            letter = chr(d[o])
            o += 1
            n, _mod, _size = struct.unpack("<3I", d[o:o + 12])
            o += 12
            keys = struct.unpack("<%dI" % n, d[o:o + 4 * n])
            o += 4 * n
            offs = struct.unpack("<%dI" % n, d[o:o + 4 * n])
            o += 4 * n
            area = bytes(d[o:q + cts])
            strings = {}
            for k, f in zip(keys, offs):
                e = f
                while e + 1 < len(area) and area[e:e + 2] != b"\0\0":
                    e += 2
                strings[k] = area[f:e].decode("utf-16-le", "replace")
            out[(bank, letter)] = strings
    return out


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    rcf, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    with open(rcf, "rb") as f:
        d = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        # bank -> letter -> {key: text}; a bank in several packages (each
        # level group has its in-game copy) keeps its first.
        found = {}
        for name, off, size in rcf_files(d):
            if not name.lower().endswith(".p3d"):
                continue
            # The file is one root chunk ("P3D\xff", 12-byte header): the banks are its children.
            for (bank, letter), strings in banks(d, off + 12, off + size).items():
                found.setdefault(bank, {}).setdefault(letter, strings)
    # One file per language: every bank's strings beside the English (US;
    # else UK) one with the same key.
    def clean(s):
        return s.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n").replace("\r", "")
    per_language = {}
    for bank, by_letter in sorted(found.items()):
        english = by_letter.get("U") or by_letter.get("E") or {}
        for letter, strings in by_letter.items():
            code = LETTERS.get(letter)
            if not code or code == "en":
                continue
            rows = per_language.setdefault(code, [])
            for key, text in strings.items():
                if key in english:
                    rows.append(f"{bank}\t{key}\t{clean(english[key])}\t{clean(text)}")
    for code, rows in sorted(per_language.items()):
        with open(os.path.join(out_dir, code + ".tsv"), "w", encoding="utf-8") as f:
            f.write("bank\tkey\tenglish\ttext\n" + "\n".join(rows) + "\n")
        print(f"{code}.tsv: {len(rows)} strings")
    # Banks whose in-game copy exists in only some languages (the in-game
    # bank lives in one package per language).
    for bank, by_letter in sorted(found.items()):
        print(f"bank {bank}: languages {''.join(sorted(by_letter))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
