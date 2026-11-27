#!/usr/bin/env python3
"""Check that a screenshot (binary PPM) shows the editor UI, not a blank,
uniform or synthetic image.

Theme-agnostic: editor panels and the main menu bar are filled with flat UI
colours, so somewhere in the top band of the frame a single exact colour
covers most of a row, and somewhere in the outer quarters a single colour
covers most of a column (a docked side panel). A rendered 3D scene, a
gradient or noise has no such runs; a uniform image is caught by its colour
count.

Usage: check_editor_capture.py INPUT.ppm   (exit 0 = looks like the editor,
1 = not, with the reason on stderr; 2 = unreadable input)
"""

import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ppm_to_png import read_ppm  # noqa: E402

MIN_COLOURS = 64          # fewer: blank / uniform / placeholder
MENU_BAND_ROWS = 40       # the menu bar lives in the top rows
FLAT_SHARE = 0.5          # one exact colour over half a row / column


def dominant_share(pixels):
    if not pixels:
        return 0.0
    return collections.Counter(pixels).most_common(1)[0][1] / len(pixels)


def check(width, height, data):
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


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        width, height, data = read_ppm(sys.argv[1])
    except (OSError, ValueError) as e:
        print(f"check_editor_capture: {e}", file=sys.stderr)
        return 2
    reason = check(width, height, data)
    if reason is not None:
        print(f"check_editor_capture: {sys.argv[1]}: {reason}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
