#!/usr/bin/env python3
"""Convert the engine's binary PPM screenshots (QUAT_SCREENSHOT) to PNG.

Standard library only, so CI needs no image packages. Optionally downscales by
an integer factor with a box filter to keep published screenshots small.

Usage: ppm_to_png.py INPUT.ppm OUTPUT.png [--downscale N]
"""

import argparse
import struct
import sys
import zlib


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    tokens = []
    pos = 0
    while len(tokens) < 4:
        while data[pos : pos + 1].isspace():
            pos += 1
        if data[pos : pos + 1] == b"#":
            pos = data.index(b"\n", pos) + 1
            continue
        start = pos
        while not data[pos : pos + 1].isspace():
            pos += 1
        tokens.append(data[start:pos])
    pos += 1  # the single whitespace byte after maxval
    magic, width, height, maxval = tokens[0], int(tokens[1]), int(tokens[2]), int(tokens[3])
    if magic != b"P6" or maxval != 255:
        raise ValueError(f"{path}: expected binary P6 with maxval 255")
    pixels = data[pos : pos + width * height * 3]
    if len(pixels) != width * height * 3:
        raise ValueError(f"{path}: truncated pixel data")
    return width, height, pixels


def downscale(width, height, pixels, factor):
    if factor <= 1:
        return width, height, pixels
    out_w, out_h = width // factor, height // factor
    out = bytearray(out_w * out_h * 3)
    area = factor * factor
    for y in range(out_h):
        for x in range(out_w):
            r = g = b = 0
            for dy in range(factor):
                row = ((y * factor + dy) * width + x * factor) * 3
                for dx in range(factor):
                    i = row + dx * 3
                    r += pixels[i]
                    g += pixels[i + 1]
                    b += pixels[i + 2]
            o = (y * out_w + x) * 3
            out[o] = r // area
            out[o + 1] = g // area
            out[o + 2] = b // area
    return out_w, out_h, bytes(out)


def write_png(path, width, height, pixels):
    stride = width * 3
    raw = b"".join(b"\x00" + pixels[y * stride : (y + 1) * stride] for y in range(height))

    def chunk(kind, body):
        return (
            struct.pack(">I", len(body))
            + kind
            + body
            + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
        )

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)  # 8-bit RGB
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", header))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("input")
    parser.add_argument("output")
    parser.add_argument("--downscale", type=int, default=1)
    args = parser.parse_args()
    try:
        width, height, pixels = read_ppm(args.input)
    except (OSError, ValueError) as e:
        print(f"ppm_to_png: {e}", file=sys.stderr)
        return 1
    width, height, pixels = downscale(width, height, pixels, args.downscale)
    write_png(args.output, width, height, pixels)
    return 0


if __name__ == "__main__":
    sys.exit(main())
