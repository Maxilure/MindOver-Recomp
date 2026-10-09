#!/usr/bin/env python3
"""compare_runs.py -- where do two exact runs of the same replay part ways?

Input: two CSV files written by the debug console's `track` (src/debug_console.cpp)
during a GAME-FRAME replay (fixed step on: src/fixed_step.h). Each row = one
player's body at the END of one game frame:

    frame,replay_frame,t_ms,player,body,x,y,z,vx,vy,vz,ground,steep,mx,my,mz

The runs may use different fixed steps (say 1/30 s and 1/180 s). Rows are
lined up on GAME TIME since the replay started (t_ms): only times both runs
have are compared, so a 30 fps run against a 180 fps run is compared every
1/30 s (every 6th frame of the 180 run). Same fps = every frame.

What it prints:
  * the first moment the positions differ by more than --tolerance (units),
    with a few lines of both runs around it (position, velocity, ground);
  * the biggest gap and where it was;
  * the ground contact: how often each run lost / regained it (a 180 fps run
    that loses the ground more often = the stair-step falls kind of bug);
  * the end positions.

Usage:
  tools/compare_runs.py a.csv b.csv [--player 1] [--tolerance 0.01] [--context 5]
  tools/compare_runs.py a.csv b.csv --csv merged.csv   (side by side, for a plot)

Same fps, two runs, no change in between: they must be IDENTICAL (max gap 0);
anything else means something in the game isn't repeatable (other threads,
streaming) and the 30-vs-180 numbers need that much salt.
"""

import argparse
import csv
import sys


def load(path, player):
    """{time key: row} for one player's rows recorded during the replay."""
    fps = None
    lines = []
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                if "fixed_step_fps=" in line:
                    fps = int(line.split("fixed_step_fps=")[1].split()[0])
                continue
            lines.append(line)
    rows = {}
    for r in csv.DictReader(lines):
        if r["player"] != str(player) or int(r["replay_frame"]) < 0:
            continue
        # Key = game time in microseconds, rounded: 1/30 and 6/180 meet exactly.
        rows[round(float(r["t_ms"]) * 1000)] = r
    return fps, rows


def vec(r, a, b, c):
    return tuple(float(r[k]) for k in (a, b, c))


def dist(p, q):
    return sum((x - y) ** 2 for x, y in zip(p, q)) ** 0.5


def contact_changes(rows):
    """(losses, gains) of the ground contact over the run, in time order."""
    losses = gains = 0
    prev = None
    for t in sorted(rows):
        on = rows[t]["ground"] != "00000000"
        if prev is not None and on != prev:
            if on:
                gains += 1
            else:
                losses += 1
        prev = on
    return losses, gains


def describe(r):
    p = vec(r, "x", "y", "z")
    v = vec(r, "vx", "vy", "vz")
    ground = "ground" if r["ground"] != "00000000" else "AIR   "
    return (f"pos ({p[0]:9.4f} {p[1]:9.4f} {p[2]:9.4f})  vel ({v[0]:8.3f} {v[1]:8.3f} {v[2]:8.3f})"
            f"  {ground}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--player", type=int, default=1)
    ap.add_argument("--tolerance", type=float, default=0.01, help="units (Crash is ~1.5 tall)")
    ap.add_argument("--context", type=int, default=5, help="compared moments shown around the split")
    ap.add_argument("--csv", help="write both runs side by side at the shared times")
    args = ap.parse_args()

    fps_a, a = load(args.a, args.player)
    fps_b, b = load(args.b, args.player)
    if not a or not b:
        sys.exit("no replay rows for that player in one of the files (was a game-frame replay running?)")
    times = sorted(set(a) & set(b))
    if not times:
        sys.exit("the runs share no game times (fps not multiples of each other?)")
    print(f"A: {args.a} (1/{fps_a} s, {len(a)} frames)")
    print(f"B: {args.b} (1/{fps_b} s, {len(b)} frames)")
    print(f"compared at {len(times)} shared moments, {times[0] / 1e6:.3f} .. {times[-1] / 1e6:.3f} s")

    gaps = [dist(vec(a[t], "x", "y", "z"), vec(b[t], "x", "y", "z")) for t in times]
    split = next((i for i, g in enumerate(gaps) if g > args.tolerance), None)
    worst = max(range(len(times)), key=lambda i: gaps[i])
    if split is None:
        print(f"\nSAME PATH: never more than {args.tolerance} apart (biggest {gaps[worst]:.5f} at "
              f"{times[worst] / 1e6:.3f} s)")
    else:
        t = times[split]
        print(f"\nPART WAYS at {t / 1e6:.3f} s of game time (gap {gaps[split]:.4f} > {args.tolerance}):")
        for i in range(max(0, split - args.context), min(len(times), split + args.context + 1)):
            mark = ">>" if i == split else "  "
            tt = times[i]
            print(f"{mark} {tt / 1e6:7.3f} s  gap {gaps[i]:8.4f}")
            print(f"       A {describe(a[tt])}")
            print(f"       B {describe(b[tt])}")
        print(f"biggest gap {gaps[worst]:.4f} at {times[worst] / 1e6:.3f} s")

    la, ga = contact_changes(a)
    lb, gb = contact_changes(b)
    print(f"\nground contact lost / regained: A {la} / {ga}, B {lb} / {gb}"
          f"  (every frame of each run, not only the shared moments)")
    end = times[-1]
    print(f"end at {end / 1e6:.3f} s:\n  A {describe(a[end])}\n  B {describe(b[end])}")

    if args.csv:
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["t_s", "gap", "ax", "ay", "az", "bx", "by", "bz", "a_ground", "b_ground"])
            for t, g in zip(times, gaps):
                w.writerow([t / 1e6, g, *vec(a[t], "x", "y", "z"), *vec(b[t], "x", "y", "z"),
                            a[t]["ground"] != "00000000", b[t]["ground"] != "00000000"])
        print(f"side by side: {args.csv}")


if __name__ == "__main__":
    main()
