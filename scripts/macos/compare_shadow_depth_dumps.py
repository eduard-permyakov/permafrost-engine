#!/usr/bin/env python3
import argparse
import array
import math
import os
import struct
import sys
import zlib


WIDTH = 2048
HEIGHT = 2048
COUNT = WIDTH * HEIGHT
EXPECTED_BYTES = COUNT * 4


def read_f32(path):
    size = os.path.getsize(path)
    if size != EXPECTED_BYTES:
        raise ValueError("{0}: expected {1} bytes, got {2}".format(path, EXPECTED_BYTES, size))
    with open(path, "rb") as infile:
        data = infile.read()
    values = array.array("f")
    values.frombytes(data)
    if sys.byteorder != "little":
        values.byteswap()
    return values


def write_png_rgba(path, width, height, rgba):
    def chunk(kind, payload):
        body = kind + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xffffffff)

    rows = []
    row_bytes = width * 4
    for y in range(height):
        start = y * row_bytes
        rows.append(b"\x00" + rgba[start:start + row_bytes])
    payload = b"".join(rows)
    png = [
        b"\x89PNG\r\n\x1a\n",
        chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)),
        chunk(b"IDAT", zlib.compress(payload, 9)),
        chunk(b"IEND", b""),
    ]
    with open(path, "wb") as outfile:
        outfile.write(b"".join(png))


def stats(values, clear_threshold):
    total = len(values)
    finite = [v for v in values if math.isfinite(v)]
    nonclear = [v for v in finite if v < clear_threshold]
    hist = [0] * 10
    for value in finite:
        clamped = min(max(value, 0.0), 0.999999)
        hist[int(clamped * 10.0)] += 1
    if nonclear:
        nonclear_min = min(nonclear)
        nonclear_max = max(nonclear)
        nonclear_mean = sum(nonclear) / float(len(nonclear))
    else:
        nonclear_min = None
        nonclear_max = None
        nonclear_mean = None
    return {
        "total": total,
        "finite": len(finite),
        "nonclear": len(nonclear),
        "min": min(finite) if finite else None,
        "max": max(finite) if finite else None,
        "nonclear_min": nonclear_min,
        "nonclear_max": nonclear_max,
        "nonclear_mean": nonclear_mean,
        "hist": hist,
    }


def delta_stats(gl_values, metal_values, clear_threshold):
    max_abs = 0.0
    sum_abs = 0.0
    count_gt_1e4 = 0
    count_gt_1e3 = 0
    union_nonclear = 0
    union_sum_abs = 0.0
    union_max_abs = 0.0
    closer_gl = 0
    closer_metal = 0

    for gl, metal in zip(gl_values, metal_values):
        delta = metal - gl
        abs_delta = abs(delta)
        max_abs = max(max_abs, abs_delta)
        sum_abs += abs_delta
        if abs_delta > 0.0001:
            count_gt_1e4 += 1
        if abs_delta > 0.001:
            count_gt_1e3 += 1
        if gl < clear_threshold or metal < clear_threshold:
            union_nonclear += 1
            union_sum_abs += abs_delta
            union_max_abs = max(union_max_abs, abs_delta)
            if gl < metal:
                closer_gl += 1
            elif metal < gl:
                closer_metal += 1

    total = len(gl_values)
    return {
        "mean_abs": sum_abs / float(total),
        "max_abs": max_abs,
        "count_gt_0.0001": count_gt_1e4,
        "count_gt_0.001": count_gt_1e3,
        "union_nonclear": union_nonclear,
        "union_mean_abs": union_sum_abs / float(union_nonclear) if union_nonclear else 0.0,
        "union_max_abs": union_max_abs,
        "closer_gl": closer_gl,
        "closer_metal": closer_metal,
    }


def write_depth_preview(path, values):
    rgba = bytearray(COUNT * 4)
    for idx, value in enumerate(values):
        d = min(max(value if math.isfinite(value) else 1.0, 0.0), 1.0)
        g = int(d * 255.0 + 0.5)
        out = idx * 4
        rgba[out + 0] = g
        rgba[out + 1] = g
        rgba[out + 2] = g
        rgba[out + 3] = 255
    write_png_rgba(path, WIDTH, HEIGHT, bytes(rgba))


def write_delta_heatmap(path, gl_values, metal_values):
    max_abs = max(abs(metal - gl) for gl, metal in zip(gl_values, metal_values))
    if max_abs <= 0.0:
        max_abs = 1.0
    rgba = bytearray(COUNT * 4)
    for idx, (gl, metal) in enumerate(zip(gl_values, metal_values)):
        delta = metal - gl
        intensity = min(int(abs(delta) / max_abs * 255.0 + 0.5), 255)
        out = idx * 4
        if delta > 0.0:
            rgba[out + 0] = 255
            rgba[out + 1] = 255 - intensity
            rgba[out + 2] = 255 - intensity
        elif delta < 0.0:
            rgba[out + 0] = 255 - intensity
            rgba[out + 1] = 255 - intensity
            rgba[out + 2] = 255
        else:
            rgba[out + 0] = 255
            rgba[out + 1] = 255
            rgba[out + 2] = 255
        rgba[out + 3] = 255
    write_png_rgba(path, WIDTH, HEIGHT, bytes(rgba))


def print_stats(label, data):
    print("{0}: total={1} finite={2} nonclear={3} min={4} max={5} nonclear_min={6} nonclear_max={7} nonclear_mean={8}".format(
        label,
        data["total"],
        data["finite"],
        data["nonclear"],
        data["min"],
        data["max"],
        data["nonclear_min"],
        data["nonclear_max"],
        data["nonclear_mean"],
    ))
    print("{0}: hist_0.1_bins={1}".format(label, ",".join(str(v) for v in data["hist"])))


def main():
    parser = argparse.ArgumentParser(description="Compare GL and Metal float32 shadow depth dumps.")
    parser.add_argument("gl_f32")
    parser.add_argument("metal_f32")
    parser.add_argument("--clear-threshold", type=float, default=0.999999)
    parser.add_argument("--points", nargs="*", default=[])
    parser.add_argument("--delta-png")
    parser.add_argument("--gl-preview-png")
    parser.add_argument("--metal-preview-png")
    args = parser.parse_args()

    gl_values = read_f32(args.gl_f32)
    metal_values = read_f32(args.metal_f32)

    print_stats("gl", stats(gl_values, args.clear_threshold))
    print_stats("metal", stats(metal_values, args.clear_threshold))
    print("delta: {0}".format(delta_stats(gl_values, metal_values, args.clear_threshold)))

    for point in args.points:
        x_text, y_text = point.split(",", 1)
        x = int(x_text)
        y = int(y_text)
        idx = y * WIDTH + x
        print("point {0},{1}: gl={2:.9f} metal={3:.9f} delta={4:.9f}".format(
            x, y, gl_values[idx], metal_values[idx], metal_values[idx] - gl_values[idx]))

    if args.delta_png:
        write_delta_heatmap(args.delta_png, gl_values, metal_values)
        print("wrote {0}".format(args.delta_png))
    if args.gl_preview_png:
        write_depth_preview(args.gl_preview_png, gl_values)
        print("wrote {0}".format(args.gl_preview_png))
    if args.metal_preview_png:
        write_depth_preview(args.metal_preview_png, metal_values)
        print("wrote {0}".format(args.metal_preview_png))


if __name__ == "__main__":
    main()
