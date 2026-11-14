#!/usr/bin/env python3
# =============================================================================
# tools/subtitles.py -- the voice-line inventory for subtitles (findings/63)
# =============================================================================
#
# WHAT IT IS FOR
#   The disc has no dialogue text (findings/49 s.8, findings/63 s.1), so every
#   spoken line needs a transcription. This tool lists the lines that exist:
#   it reads the player's OWN voice archive (english.rcf, or any <language>.rcf)
#   and writes one row per voice clip, with its sound ID (the subtitle key),
#   the sound designer's file name, the speaker folder, a family guess and its
#   length. It also reads the port's session logs and lists which clips the
#   game really played, and how often.
#
#   The output is game content (names of the game's files): it goes to a
#   folder of the player's choice, normally <game folder>/user/subtitles/,
#   NEVER into the repository (AGENTS.md s.2).
#
# FORMATS IT RELIES ON (findings/19 s.1, src/audio_trace.cpp)
#   RCF 2.1 archive, big-endian, magic "ATG CORE CEMENT LIBRARY":
#     0x24: u32 directory offset, directory size, names offset, names size,
#           (0), file count
#     directory: per file u32 name hash, data offset, size (sorted by HASH)
#     names: 8 bytes, then per file 12 bytes, a LITTLE-endian u32 length, the
#           name (length bytes incl. its NUL), 3 bytes of padding; listed in
#           DATA order, so sorting the directory's offsets pairs them up.
#     A name "sound\rsd\english\8\8150b53fbe2b48d3.rsd": the 16 hex digits
#     before ".rsd" are the sound's 64-bit ID, the SAME in every language.
#   RSD6 sound file:
#     +0x00 "RSD6", +0x04 codec "XMA ", +0x08 channels, +0x0C bits,
#     +0x10 sample rate (little-endian u32s), +0x14 "****",
#     +0x18 the designer's file name (NUL-terminated).
#     +0x800 three big-endian u32 sizes, then the XMA header H at +0x80C
#     (findings/54 s.5): H+1 stream count, H+4 lead-in samples, H+8 end
#     sample, H+0x0C rate (big-endian). Length = end sample / rate.
#
# USAGE
#   python3 -I tools/subtitles.py inventory <voice.rcf> <out dir> [--logs <play-*.log> ...]
#   -> <out dir>/voice_inventory.csv   one row per clip
#      <out dir>/voice_summary.txt     counts per family and speaker folder
#   (python -I: the archive is untrusted data; nothing is imported from its folder.)
# =============================================================================

import argparse
import collections
import csv
import os
import re
import struct
import sys

# The designer names start with this; it says nothing, so it is cut off.
PREFIX = "in_game_art\\sound\\sounds\\"


def read_rcf_index(f):
    """[(sound_id, data_offset)] for every .rsd in an open RCF archive."""
    header = f.read(0x3C)
    if header[:23] != b"ATG CORE CEMENT LIBRARY":
        raise ValueError("not an RCF archive")
    dir_off, _dir_size, names_off, names_size, _zero, count = struct.unpack(">6I", header[0x24:0x3C])
    f.seek(dir_off)
    directory = f.read(count * 12)
    # Data offsets in data order = the order of the name list.
    offsets = sorted(struct.unpack_from(">I", directory, i * 12 + 4)[0] for i in range(count))
    f.seek(names_off)
    names = f.read(names_size)
    out = []
    p = 8
    for i in range(count):
        if p + 16 > len(names):
            break
        p += 12
        (length,) = struct.unpack_from("<I", names, p)
        p += 4
        name = names[p:p + length - 1].decode("latin-1")
        p += length + 3
        m = re.search(r"([0-9a-fA-F]{16})\.rsd$", name)
        if m:
            out.append((int(m.group(1), 16), offsets[i]))
    return out


def read_rsd(f, offset):
    """(designer name, channels, rate, seconds) of the RSD6 file at `offset`."""
    f.seek(offset)
    b = f.read(0x830)
    if b[:4] != b"RSD6":
        return None
    channels, _bits, rate = struct.unpack_from("<3I", b, 8)
    name = b[0x18:b.index(b"\0", 0x18)].decode("latin-1")
    if name.startswith(PREFIX):
        name = name[len(PREFIX):]
    seconds = 0.0
    h = 0x80C  # the XMA header H
    if len(b) >= h + 0x10:
        end, xrate = struct.unpack_from(">2I", b, h + 8)
        if xrate:
            seconds = end / xrate
    return name, channels, rate, seconds


def family_of(name):
    """The group of findings/63 s.13.1, from folder and file name (a guess
    until the line is heard; the scope column of that table follows it)."""
    low = name.lower().replace("/", "\\")
    parts = low.split("\\")
    file = parts[-1]
    if "\\nis\\" in low:
        return "cutscene"            # in-engine cutscene track (family B)
    if any(p.startswith("aku") for p in parts):
        return "aku_aku"             # family D
    if any("tutorial" in p for p in parts):
        return "tutorial"            # family D
    if "credits" in parts:
        return "credits"             # voiced credits (optional)
    if file.startswith("loudspeaker"):
        return "loudspeaker"         # Cortex's announcements (family C, in scope)
    if file.startswith("ingame"):
        return "story"               # NPC / story lines (family C, in scope)
    return "combat"                  # fight barks (out of scope, findings/63 s.11)


def speaker_folder(name):
    """The folder after character\\<language>\\ (the voice's owner), else ''."""
    parts = name.replace("/", "\\").split("\\")
    if len(parts) >= 4 and parts[0].lower() == "character":
        return parts[2]
    return parts[-2] if len(parts) >= 2 else ""


# "Audio: <kind> <designer name> (<16 hex>)" -- audio_trace.cpp's line.
TRACE = re.compile(r"Audio: (\w+) (.+?) \(([0-9a-f]{16})\)")


def played_counts(log_paths):
    counts = collections.Counter()
    kinds = collections.defaultdict(collections.Counter)
    for path in log_paths:
        with open(path, encoding="utf-8", errors="replace") as log:
            for line in log:
                m = TRACE.search(line)
                if m:
                    sid = int(m.group(3), 16)
                    counts[sid] += 1
                    kinds[sid][m.group(1)] += 1
    return counts, kinds


def inventory(args):
    os.makedirs(args.out, exist_ok=True)
    counts, kinds = played_counts(args.logs or [])
    rows = []
    with open(args.archive, "rb") as f:
        for sid, offset in read_rcf_index(f):
            info = read_rsd(f, offset)
            if not info:
                continue
            name, channels, rate, seconds = info
            rows.append({
                "sound_id": f"{sid:016x}",
                "family": family_of(name),
                "speaker_folder": speaker_folder(name),
                "designer_name": name,
                "channels": channels,
                "rate": rate,
                "seconds": f"{seconds:.2f}",
                "seen_in_logs": counts.get(sid, 0),
                "log_kinds": " ".join(f"{k}:{v}" for k, v in sorted(kinds.get(sid, {}).items())),
            })
    rows.sort(key=lambda r: r["designer_name"].lower())
    with open(os.path.join(args.out, "voice_inventory.csv"), "w", newline="", encoding="utf-8") as out:
        w = csv.DictWriter(out, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)

    # The summary: what a finding may quote (counts and folder names only).
    fam = collections.Counter(r["family"] for r in rows)
    fam_s = collections.Counter()
    for r in rows:
        fam_s[r["family"]] += float(r["seconds"])
    spk = collections.Counter((r["family"], r["speaker_folder"]) for r in rows)
    spk_s = collections.Counter()
    spk_seen = collections.Counter()
    for r in rows:
        spk_s[(r["family"], r["speaker_folder"])] += float(r["seconds"])
        spk_seen[(r["family"], r["speaker_folder"])] += 1 if r["seen_in_logs"] else 0
    with open(os.path.join(args.out, "voice_summary.txt"), "w", encoding="utf-8") as out:
        out.write(f"{len(rows)} clips, {sum(fam_s.values()) / 60:.1f} minutes\n\n")
        out.write("family        clips   minutes\n")
        for k, v in fam.most_common():
            out.write(f"{k:12} {v:6} {fam_s[k] / 60:9.1f}\n")
        out.write("\nfamily / speaker folder        clips  minutes  seen in logs\n")
        for (f_, s), v in sorted(spk.items()):
            out.write(f"{f_:10} {s:20} {v:6} {spk_s[(f_, s)] / 60:8.1f} {spk_seen[(f_, s)]:8}\n")
    print(f"{len(rows)} clips -> {args.out}")


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[2] if __doc__ else "")
    sub = p.add_subparsers(dest="cmd", required=True)
    inv = sub.add_parser("inventory", help="list every voice clip of an archive")
    inv.add_argument("archive")
    inv.add_argument("out")
    inv.add_argument("--logs", nargs="*")
    args = p.parse_args()
    if args.cmd == "inventory":
        inventory(args)


if __name__ == "__main__":
    sys.exit(main())
