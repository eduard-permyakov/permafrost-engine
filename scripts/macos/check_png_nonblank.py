#!/usr/bin/env python3

import argparse
import struct
import sys
import zlib


def _paeth(a, b, c):
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def _chunks(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a png")
    off = 8
    while off < len(data):
        length = struct.unpack(">I", data[off:off + 4])[0]
        kind = data[off + 4:off + 8]
        payload = data[off + 8:off + 8 + length]
        yield kind, payload
        off += 12 + length


def _stats(path, sample_stride):
    with open(path, "rb") as infile:
        data = infile.read()

    width = height = bit_depth = color_type = None
    idat = []
    for kind, payload in _chunks(data):
        if kind == b"IHDR":
            width, height, bit_depth, color_type, _, _, _ = struct.unpack(">IIBBBBB", payload)
        elif kind == b"IDAT":
            idat.append(payload)

    if bit_depth != 8 or color_type not in (0, 2, 6):
        raise ValueError("unsupported png format bit_depth={0} color_type={1}".format(bit_depth, color_type))

    channels = {0: 1, 2: 3, 6: 4}[color_type]
    row_bytes = width * channels
    raw = zlib.decompress(b"".join(idat))
    prev = bytearray(row_bytes)
    offset = 0
    samples = 0
    nonblack = 0
    total_r = total_g = total_b = 0

    for _ in range(height):
        filter_type = raw[offset]
        offset += 1
        row = bytearray(raw[offset:offset + row_bytes])
        offset += row_bytes
        for i in range(row_bytes):
            left = row[i - channels] if i >= channels else 0
            up = prev[i]
            up_left = prev[i - channels] if i >= channels else 0
            if filter_type == 1:
                row[i] = (row[i] + left) & 0xff
            elif filter_type == 2:
                row[i] = (row[i] + up) & 0xff
            elif filter_type == 3:
                row[i] = (row[i] + ((left + up) >> 1)) & 0xff
            elif filter_type == 4:
                row[i] = (row[i] + _paeth(left, up, up_left)) & 0xff
            elif filter_type != 0:
                raise ValueError("unsupported png filter {0}".format(filter_type))

        for px in range(0, width, sample_stride):
            base = px * channels
            if color_type == 0:
                r = g = b = row[base]
            else:
                r, g, b = row[base], row[base + 1], row[base + 2]
            total_r += r
            total_g += g
            total_b += b
            samples += 1
            if max(r, g, b) > 8:
                nonblack += 1
        prev = row

    return {
        "width": width,
        "height": height,
        "samples": samples,
        "nonblack_ratio": float(nonblack) / samples,
        "mean_r": float(total_r) / samples,
        "mean_g": float(total_g) / samples,
        "mean_b": float(total_b) / samples,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("paths", nargs="+")
    parser.add_argument("--min-nonblack-ratio", type=float, default=0.01)
    parser.add_argument("--sample-stride", type=int, default=8)
    args = parser.parse_args()

    failed = False
    for path in args.paths:
        stats = _stats(path, args.sample_stride)
        print(
            "PNG_NONBLANK {0} {1}x{2} nonblack={3:.4f} mean={4:.2f},{5:.2f},{6:.2f}".format(
                path,
                stats["width"],
                stats["height"],
                stats["nonblack_ratio"],
                stats["mean_r"],
                stats["mean_g"],
                stats["mean_b"],
            )
        )
        if stats["nonblack_ratio"] < args.min_nonblack_ratio:
            failed = True

    if failed:
        print("PNG_NONBLANK_FAIL", file=sys.stderr)
        return 1
    print("PNG_NONBLANK_PASS paths={0}".format(len(args.paths)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
