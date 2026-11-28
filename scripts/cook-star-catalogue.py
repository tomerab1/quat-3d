#!/usr/bin/env python3
"""Cook the Yale Bright Star Catalogue into assets/sky/bright_stars.qsc.

ROADMAP R22.2.1. Offline only: the engine never downloads star data.

Source: the Yale Bright Star Catalogue, 5th revised edition (Hoffleit &
Warren 1991; CDS / NASA ADC catalogue V/50), as converted to JSON by
Bretton Wade (https://github.com/brettonw/YaleBrightStarCatalog, MIT
licence for the conversion). The catalogue itself is distributed by the
NASA Astronomical Data Center and CDS without restriction; the fields we
keep (positions, V magnitudes, B-V colours) are measured facts. See
assets/sky/README.md for the licence record.

    python3 scripts/cook-star-catalogue.py bsc5.json assets/sky/bright_stars.qsc

Output format (little-endian), read by engine::astro::parse_star_catalogue:
    char[4]  magic "QSC1"
    uint32   version (1)
    uint32   star count N
    uint32   record size (20)
    N x {float32 x, y, z;   J2000 equatorial unit vector (x to the equinox,
                            z to the north celestial pole)
         float32 v_mag;     Johnson V magnitude
         float32 b_v}       Johnson B-V colour index
Records are sorted brightest first.
"""

import json
import math
import re
import struct
import sys

MAGIC = b"QSC1"
VERSION = 1
RECORD = struct.Struct("<5f")

# Ballesteros' formula and the black-body colours are fitted to normal
# stars; a handful of catalogue entries (peculiar carbon stars, one 5.74)
# lie far outside. Their colour is clamped to the fitted range.
B_V_RANGE = (-0.4, 2.5)

RA_RE = re.compile(r"(\d+)h\s*(\d+)m\s*([\d.]+)s")
DEC_RE = re.compile(r"([+-])(\d+)°\s*(\d+)′\s*([\d.]+)″")


def parse_ra(text):
    m = RA_RE.fullmatch(text.strip())
    if not m:
        return None
    h, mi, s = int(m.group(1)), int(m.group(2)), float(m.group(3))
    return math.radians((h + mi / 60.0 + s / 3600.0) * 15.0)


def parse_dec(text):
    m = DEC_RE.fullmatch(text.strip())
    if not m:
        return None
    sign = -1.0 if m.group(1) == "-" else 1.0
    d, mi, s = int(m.group(2)), int(m.group(3)), float(m.group(4))
    return math.radians(sign * (d + mi / 60.0 + s / 3600.0))


def b_v_from_temperature(t):
    """Inverse of Ballesteros (2012), EPL 97 34008:
    T = 4600 (1 / (0.92 x + 1.7) + 1 / (0.92 x + 0.62))."""
    k = t / 4600.0
    b = 2.32 * k - 2.0
    c = 1.054 * k - 2.32
    disc = b * b - 4.0 * k * c
    if k <= 0.0 or disc < 0.0:
        return None
    a = (-b + math.sqrt(disc)) / (2.0 * k)
    return a / 0.92


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    with open(argv[1], encoding="utf-8") as f:
        catalogue = json.load(f)

    stars = []
    skipped = 0
    colour_from_temperature = 0
    colour_default = 0
    colour_clamped = 0
    for entry in catalogue:
        ra = parse_ra(entry.get("RA", ""))
        dec = parse_dec(entry.get("Dec", ""))
        try:
            v_mag = float(entry["Vmag"])
        except (KeyError, ValueError):
            v_mag = None
        if ra is None or dec is None or v_mag is None:
            skipped += 1  # removed objects (novae, galaxies) carry no position
            continue
        try:
            b_v = float(entry["B-V"])
        except (KeyError, ValueError):
            b_v = None
        if b_v is None:
            try:
                b_v = b_v_from_temperature(float(entry["K"]))
                colour_from_temperature += 1
            except (KeyError, ValueError):
                b_v = None
        if b_v is None:
            b_v = 0.65  # a solar-type colour
            colour_default += 1
        if not B_V_RANGE[0] <= b_v <= B_V_RANGE[1]:
            b_v = min(max(b_v, B_V_RANGE[0]), B_V_RANGE[1])
            colour_clamped += 1
        x = math.cos(dec) * math.cos(ra)
        y = math.cos(dec) * math.sin(ra)
        z = math.sin(dec)
        stars.append((v_mag, x, y, z, b_v))

    stars.sort(key=lambda s: s[0])
    with open(argv[2], "wb") as out:
        out.write(MAGIC)
        out.write(struct.pack("<III", VERSION, len(stars), RECORD.size))
        for v_mag, x, y, z, b_v in stars:
            out.write(RECORD.pack(x, y, z, v_mag, b_v))
    print(
        f"{len(stars)} stars (V {stars[0][0]:+.2f} .. {stars[-1][0]:+.2f}), "
        f"{skipped} entries without a position skipped, B-V from temperature for "
        f"{colour_from_temperature}, default colour for {colour_default}, "
        f"B-V clamped for {colour_clamped}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
