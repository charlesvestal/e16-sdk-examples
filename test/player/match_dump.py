#!/usr/bin/env python3
"""Match an OLED dump against an .e16v file's frames.

  tools/console.py screen > dump.txt
  test/player/match_dump.py file.e16v dump.txt [--ignore-note]

Prints the best-matching frame and how many of the 8192 pixels differ (0 =
the screen shows exactly that frame), plus the runner-up for contrast.
--ignore-note leaves out the bottom 11 rows, where the player draws its
one-line note for a second after a control changes.
"""
import argparse
import struct
import sys

W, H = 128, 64
HDR, REC = 32, 1796


def frames(path):
    data = open(path, "rb").read()
    if data[:4] != b"E16V":
        sys.exit(f"{path}: not an .e16v")
    hsize, n = struct.unpack_from("<HH", data, 6)
    rec = struct.unpack_from("<I", data, 16)[0]
    return [data[hsize + i * rec + 4: hsize + i * rec + 4 + 1024] for i in range(n)]


def dump_bits(path):
    rows = [l.strip() for l in open(path) if set(l.strip()) <= set("#.") and len(l.strip()) == W]
    if len(rows) != H:
        sys.exit(f"{path}: expected {H} rows of {W} '#'/'.', got {len(rows)}")
    return [[c == "#" for c in r] for r in rows]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("e16v")
    ap.add_argument("dump")
    ap.add_argument("--ignore-note", action="store_true")
    a = ap.parse_args()
    scr = dump_bits(a.dump)
    rows = H - 11 if a.ignore_note else H
    scores = []
    for i, fb in enumerate(frames(a.e16v)):
        diff = sum(1 for y in range(rows) for x in range(W)
                   if bool(fb[(y >> 3) * W + x] >> (y & 7) & 1) != scr[y][x])
        scores.append((diff, i))
    scores.sort()
    (d0, f0), (d1, f1) = scores[0], scores[1]
    print(f"best frame {f0}: {d0} of {rows * W} pixels differ (next best frame {f1}: {d1})")


if __name__ == "__main__":
    main()
