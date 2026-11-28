#!/usr/bin/env python3
"""Compare two of the engine's binary PPM screenshots (QUAT_SCREENSHOT).

Used by CI's determinism check (issue #128): the same scene rendered twice
with the same build must give the same image. Reports how many pixels differ
(any channel), how many differ by more than --level, and the largest channel
difference. Exits 0 when the images match within the tolerance, 1 when they
do not, 2 on unreadable input. Standard library only.

Usage: compare_ppm.py A.ppm B.ppm [--max-diff N] [--max-pixels N] [--level N]
  --max-diff   largest channel difference allowed (default 0: bit-identical)
  --max-pixels pixels allowed to differ at all (default 0)
  --level      threshold of the "differ by more than" count (default 24)
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ppm_to_png import read_ppm  # noqa: E402


def compare(a, b, level):
    """Returns (differing pixels, pixels over `level`, max channel diff)."""
    differing = over = peak = 0
    for i in range(0, len(a), 3):
        if a[i : i + 3] == b[i : i + 3]:
            continue
        d = max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]))
        differing += 1
        if d > level:
            over += 1
        peak = max(peak, d)
    return differing, over, peak


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("a")
    parser.add_argument("b")
    parser.add_argument("--max-diff", type=int, default=0)
    parser.add_argument("--max-pixels", type=int, default=0)
    parser.add_argument("--level", type=int, default=24)
    args = parser.parse_args()
    try:
        wa, ha, pa = read_ppm(args.a)
        wb, hb, pb = read_ppm(args.b)
    except (OSError, ValueError, IndexError) as e:
        print(f"compare_ppm: {e}", file=sys.stderr)
        return 2
    if (wa, ha) != (wb, hb):
        print(f"compare_ppm: size differs: {wa}x{ha} vs {wb}x{hb}")
        return 1
    if pa == pb:
        print(f"compare_ppm: identical ({wa}x{ha})")
        return 0
    differing, over, peak = compare(pa, pb, args.level)
    print(
        f"compare_ppm: {differing} of {wa * ha} pixels differ, {over} by more than "
        f"{args.level} levels, max difference {peak}"
    )
    return 0 if peak <= args.max_diff and differing <= args.max_pixels else 1


if __name__ == "__main__":
    sys.exit(main())
