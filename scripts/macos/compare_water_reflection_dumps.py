#!/usr/bin/env python3
import argparse
import os
import struct
import zlib


def read_rgba(path, width, height):
    expected = width * height * 4
    actual = os.path.getsize(path)
    if actual != expected:
        raise ValueError("{0}: expected {1} bytes, got {2}".format(path, expected, actual))
    with open(path, "rb") as infile:
        return bytearray(infile.read())


def bgra_to_rgba(data):
    out = bytearray(len(data))
    for i in range(0, len(data), 4):
        out[i + 0] = data[i + 2]
        out[i + 1] = data[i + 1]
        out[i + 2] = data[i + 0]
        out[i + 3] = data[i + 3]
    return out


def write_png_rgba(path, width, height, rgba, flip_y=False):
    def chunk(kind, payload):
        body = kind + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xffffffff)

    rows = []
    row_bytes = width * 4
    for y in range(height):
        src_y = height - 1 - y if flip_y else y
        start = src_y * row_bytes
        rows.append(b"\x00" + bytes(rgba[start:start + row_bytes]))
    payload = b"".join(rows)
    png = [
        b"\x89PNG\r\n\x1a\n",
        chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)),
        chunk(b"IDAT", zlib.compress(payload, 9)),
        chunk(b"IEND", b""),
    ]
    with open(path, "wb") as outfile:
        outfile.write(b"".join(png))


def block_average(data, width, height, x, y, half):
    lo_x = max(0, x - half)
    hi_x = min(width - 1, x + half)
    lo_y = max(0, y - half)
    hi_y = min(height - 1, y + half)
    total = [0, 0, 0]
    count = 0
    for yy in range(lo_y, hi_y + 1):
        row = yy * width * 4
        for xx in range(lo_x, hi_x + 1):
            idx = row + xx * 4
            total[0] += data[idx + 0]
            total[1] += data[idx + 1]
            total[2] += data[idx + 2]
            count += 1
    return tuple(value / float(count) for value in total)


def ratio(a, b):
    out = []
    for av, bv in zip(a, b):
        out.append(float("nan") if av == 0.0 else bv / av)
    return tuple(out)


def print_points(label, gl_data, metal_data, width, height, points, half):
    print("{0}: {1}x{2} half={3}".format(label, width, height, half))
    for x, y in points:
        gl_avg = block_average(gl_data, width, height, x, y, half)
        metal_avg = block_average(metal_data, width, height, x, y, half)
        ratios = ratio(gl_avg, metal_avg)
        print("{0},{1} gl={2:.1f},{3:.1f},{4:.1f} metal={5:.1f},{6:.1f},{7:.1f} ratio={8:.2f},{9:.2f},{10:.2f}".format(
            x, y,
            gl_avg[0], gl_avg[1], gl_avg[2],
            metal_avg[0], metal_avg[1], metal_avg[2],
            ratios[0], ratios[1], ratios[2],
        ))


def parse_points(items):
    points = []
    for item in items:
        x_text, y_text = item.split(",", 1)
        points.append((int(x_text), int(y_text)))
    return points


def main():
    parser = argparse.ArgumentParser(description="Compare GL/Metal raw RGBA8 water reflection dumps.")
    parser.add_argument("gl_rgba8")
    parser.add_argument("metal_bgra8")
    parser.add_argument("--width", type=int, required=True)
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument("--half", type=int, default=4)
    parser.add_argument("--points", nargs="*", default=[])
    parser.add_argument("--gl-png")
    parser.add_argument("--metal-png")
    parser.add_argument("--flip-y-png", action="store_true")
    args = parser.parse_args()

    gl_data = read_rgba(args.gl_rgba8, args.width, args.height)
    metal_data = bgra_to_rgba(read_rgba(args.metal_bgra8, args.width, args.height))
    points = parse_points(args.points)
    if points:
        print_points("water_reflection", gl_data, metal_data,
                     args.width, args.height, points, args.half)
    if args.gl_png:
        write_png_rgba(args.gl_png, args.width, args.height, gl_data, args.flip_y_png)
        print("wrote {0}".format(args.gl_png))
    if args.metal_png:
        write_png_rgba(args.metal_png, args.width, args.height, metal_data, args.flip_y_png)
        print("wrote {0}".format(args.metal_png))


if __name__ == "__main__":
    main()
