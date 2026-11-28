#!/usr/bin/env python3
"""Sanity checks for an engine screenshot (binary PPM, QUAT_SCREENSHOT).

Every capture: no large exact-black, screen-aligned rectangle. A NaN or Inf
in the HDR chain renders as exact black (0,0,0) and spreads as a block
through bloom's mip chain or TAA history (issue #45); lit scenes never
produce exact black over an area that size.

With --editor, additionally that the frame shows the editor UI, not a blank,
uniform or synthetic image. Theme-agnostic: editor panels and the main menu
bar are filled with flat UI colours, so in the top band some row is mostly a
single exact colour, and in the outer quarters some column is (a docked side
panel). A rendered 3D scene, a gradient or noise has no such runs; a uniform
image is caught by its colour count.

Usage: check_capture.py [--editor] INPUT.ppm
Exit 0 = passed, 1 = failed (reason on stderr), 2 = unreadable input.
"""

import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ppm_to_png import read_ppm  # noqa: E402

MIN_COLOURS = 64          # fewer: blank / uniform / placeholder
MENU_BAND_ROWS = 40       # the menu bar lives in the top rows
FLAT_SHARE = 0.5          # one exact colour over half a row / column
MAX_BLACK_SHARE = 0.01    # largest exact-black rectangle, share of the frame
MIN_BLACK_SIDE = 16       # ...counted when both sides are at least this long


def largest_black_rectangle(width, height, data):
    """Area and bounds of the largest axis-aligned rectangle of (0,0,0)."""
    heights = [0] * width
    best = (0, None)
    for y in range(height):
        row = y * width * 3
        for x in range(width):
            i = row + 3 * x
            black = data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 0
            heights[x] = heights[x] + 1 if black else 0
        # Largest rectangle under the histogram (monotonic stack).
        stack = []
        for x in range(width + 1):
            h = heights[x] if x < width else 0
            start = x
            while stack and stack[-1][1] >= h:
                start, sh = stack.pop()
                w = x - start
                if sh >= MIN_BLACK_SIDE and w >= MIN_BLACK_SIDE and sh * w > best[0]:
                    best = (sh * w, (start, y - sh + 1, x - 1, y))
            stack.append((start, h))
    return best


def dominant_share(pixels):
    if not pixels:
        return 0.0
    return collections.Counter(pixels).most_common(1)[0][1] / len(pixels)


def check_editor(width, height, data):
    px = [data[i:i + 3] for i in range(0, len(data), 3)]
    colours = len(set(px[:: max(1, len(px) // 200_000)]))
    if colours < MIN_COLOURS:
        return f"only {colours} distinct colours (blank or uniform image)"
    rows = range(min(MENU_BAND_ROWS, height))
    if not any(dominant_share(px[y * width:(y + 1) * width]) >= FLAT_SHARE for y in rows):
        return (f"no flat UI row in the top {len(rows)} rows "
                "(menu bar missing: not the editor, or ImGui not drawn)")
    quarter = max(1, width // 4)
    columns = list(range(quarter)) + list(range(width - quarter, width))
    step = max(1, len(columns) // 64)
    if not any(dominant_share(px[x::width]) >= FLAT_SHARE for x in columns[::step]):
        return "no flat UI column in the outer quarters (no docked panel)"
    return None


def check(width, height, data, editor):
    area, box = largest_black_rectangle(width, height, data)
    if area > MAX_BLACK_SHARE * width * height:
        x0, y0, x1, y1 = box
        return (f"exact-black rectangle x {x0}-{x1}, y {y0}-{y1} "
                f"({100.0 * area / (width * height):.1f}% of the frame): "
                "a NaN/Inf in the HDR chain?")
    return check_editor(width, height, data) if editor else None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--editor", action="store_true", help="the frame must show the editor UI")
    parser.add_argument("input")
    args = parser.parse_args()
    try:
        width, height, data = read_ppm(args.input)
    except (OSError, ValueError) as e:
        print(f"check_capture: {e}", file=sys.stderr)
        return 2
    reason = check(width, height, data, args.editor)
    if reason is not None:
        print(f"check_capture: {args.input}: {reason}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
