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
# USAGE (python -I: the archives are untrusted data; nothing is imported from
# their folder). Every output goes to a folder of the player's choice, normally
# <game folder>/user/subtitles/, NEVER into the repository.
#
#   inventory <voice.rcf> <out dir> [--logs <play-*.log> ...]
#       -> voice_inventory.csv (one row per clip), voice_summary.txt (counts)
#   extract-movies <movie1.rcf> [<movie2.rcf> ...] <out dir> [--only Crash_]
#       the Bink movies (.bik) out of the movie archives, as they are. Python's
#       standard library only, so it runs on any player's PC.
#   extract-voices <voice.rcf> <out dir>
#       every in-scope clip (findings/63 s.11) as <sound ID>.wav, 16 kHz mono
#       (for speech recognition), + voices.csv; needs the `ffmpeg` program
#       (its XMA2 decoder: the clip's data wrapped in an XMA2WAVEFORMATEX RIFF,
#       findings/19 s.7).
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


def read_rcf_files(f):
    """[(name, data_offset, size)] of every file in an open RCF archive (any kind)."""
    f.seek(0)
    header = f.read(0x3C)
    if header[:23] != b"ATG CORE CEMENT LIBRARY":
        raise ValueError("not an RCF archive")
    dir_off, _dir_size, names_off, names_size, _zero, count = struct.unpack(">6I", header[0x24:0x3C])
    f.seek(dir_off)
    directory = f.read(count * 12)
    # (offset, size) in data order = the order of the names.
    entries = sorted(struct.unpack_from(">2I", directory, i * 12 + 4) for i in range(count))
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
        out.append((name, entries[i][0], entries[i][1]))
    return out


def extract_movies(args):
    os.makedirs(args.out, exist_ok=True)
    n = 0
    for archive in args.archives:
        with open(archive, "rb") as f:
            for name, offset, size in read_rcf_files(f):
                base = re.split(r"[\\/]", name)[-1]
                if not base.lower().endswith(".bik") or not base.startswith(args.only):
                    continue
                f.seek(offset)
                with open(os.path.join(args.out, base), "wb") as out:
                    left = size
                    while left:
                        chunk = f.read(min(left, 1 << 22))
                        if not chunk:
                            break
                        out.write(chunk)
                        left -= len(chunk)
                print(f"{base}  {size / 1e6:.1f} MB")
                n += 1
    print(f"{n} movie(s) -> {args.out}")


def xma2_riff(f, offset):
    """The .rsd at `offset` as an XMA2 WAV file (bytes) ffmpeg can decode, + seconds."""
    f.seek(offset)
    head = f.read(0x900)
    h_size, table_size, data_size = struct.unpack_from(">3I", head, 0x800)
    h = head[0x80C:0x80C + h_size]
    streams = h[1]
    end, rate, block = struct.unpack_from(">I", h, 8)[0], struct.unpack_from(">I", h, 12)[0], struct.unpack_from(">I", h, 0x18)[0]
    channels = sum(h[0x28 + 4 * k] for k in range(streams))
    mask = {1: 0x4, 2: 0x3, 6: 0x3F}.get(channels, 0)
    f.seek(offset + 0x80C + h_size + table_size)
    data = f.read(data_size)
    blocks = (data_size + block - 1) // block
    fmt = struct.pack("<HHIIHHH", 0x166, channels, rate, rate * 2 * channels, 2048, 16, 34)
    fmt += struct.pack("<HIIIIIIIBBH", streams, mask, end, block, 0, end, 0, 0, 0, 4, blocks)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", len(body)) + body, end / rate if rate else 0.0


def extract_voices(args):
    import subprocess
    import tempfile
    os.makedirs(args.out, exist_ok=True)
    rows = []
    with open(args.archive, "rb") as f:
        for sid, offset in read_rcf_index(f):
            info = read_rsd(f, offset)
            if not info:
                continue
            name, channels, _rate, seconds = info
            fam = family_of(name)
            if fam in ("combat", "credits"):
                continue
            riff, seconds = xma2_riff(f, offset)
            wav = os.path.join(args.out, f"{sid:016x}.wav")
            with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as t:
                t.write(riff)
            r = subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", t.name, "-ac", "1", "-ar", "16000", wav],
                               capture_output=True, text=True)
            os.unlink(t.name)
            if r.returncode != 0:
                print(f"can't decode {name}: {r.stderr.strip()[:200]}")
                continue
            rows.append({"sound_id": f"{sid:016x}", "family": fam, "speaker_folder": speaker_folder(name),
                         "designer_name": name, "channels": channels, "seconds": f"{seconds:.2f}"})
    rows.sort(key=lambda r: r["designer_name"].lower())
    with open(os.path.join(args.out, "voices.csv"), "w", newline="", encoding="utf-8") as out:
        w = csv.DictWriter(out, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print(f"{len(rows)} clips -> {args.out}")


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
    mov = sub.add_parser("extract-movies", help="the Bink movies out of the movie archives")
    mov.add_argument("archives", nargs="+", help="movie1.rcf movie2.rcf ... then the out dir")
    mov.add_argument("--only", default="", help="only movies whose name starts with this (e.g. Crash_)")
    voc = sub.add_parser("extract-voices", help="in-scope clips as 16 kHz WAV files (needs ffmpeg)")
    voc.add_argument("archive")
    voc.add_argument("out")
    args = p.parse_args()
    if args.cmd == "inventory":
        inventory(args)
    elif args.cmd == "extract-movies":
        if len(args.archives) < 2:
            p.error("give at least one archive and the out dir")
        args.out = args.archives.pop()
        extract_movies(args)
    elif args.cmd == "extract-voices":
        extract_voices(args)


if __name__ == "__main__":
    sys.exit(main())
