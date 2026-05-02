#!/usr/bin/env python3
import argparse
import array
import csv
import os
import struct
import sys
from collections import Counter


CLEAR_UID = 0
TERRAIN_UID = 0xFFFFFFFF
UNKNOWN_UID = 0xFFFFFFFE


def label_owner(uid):
    if uid == CLEAR_UID:
        return "clear"
    if uid == TERRAIN_UID:
        return "terrain"
    if uid == UNKNOWN_UID:
        return "unknown"
    return "entity:{0}".format(uid)


def parse_texel(value):
    try:
        x_text, y_text = value.split(",", 1)
        return int(x_text), int(y_text)
    except ValueError:
        raise argparse.ArgumentTypeError("texel must be x,y")


def read_u32_dump(path, width, height):
    expected = width * height * 4
    actual = os.path.getsize(path)
    if actual != expected:
        raise SystemExit(
            "bad owner dump size: got {0}, expected {1}".format(actual, expected)
        )
    owners = array.array("I")
    with open(path, "rb") as infile:
        owners.frombytes(infile.read())
    if sys.byteorder != "little":
        owners.byteswap()
    return owners


def read_depth_dump(path, width, height):
    expected = width * height * 4
    actual = os.path.getsize(path)
    if actual != expected:
        raise SystemExit(
            "bad depth dump size: got {0}, expected {1}".format(actual, expected)
        )
    with open(path, "rb") as infile:
        data = infile.read()
    return struct.unpack("<{0}f".format(width * height), data)


def read_manifest(path):
    if not path:
        return {}
    with open(path, newline="") as infile:
        return {int(row["uid"]): row for row in csv.DictReader(infile)}


def manifest_text(uid, manifest):
    row = manifest.get(uid)
    if not row:
        return ""
    return " {0} flags={1} pos=({2},{3})".format(
        row.get("kind", "?"),
        row.get("flags", "?"),
        row.get("x", "?"),
        row.get("z", "?"),
    )


def main():
    parser = argparse.ArgumentParser(
        description="Inspect a Metal uint32 shadow-owner dump."
    )
    parser.add_argument("owner_u32")
    parser.add_argument("--width", type=int, default=2048)
    parser.add_argument("--height", type=int, default=2048)
    parser.add_argument("--depth-f32")
    parser.add_argument("--manifest")
    parser.add_argument("--top", type=int, default=20)
    parser.add_argument("--texels", nargs="*", type=parse_texel, default=[])
    args = parser.parse_args()

    owners = read_u32_dump(args.owner_u32, args.width, args.height)
    depth = None
    if args.depth_f32:
        depth = read_depth_dump(args.depth_f32, args.width, args.height)
    manifest = read_manifest(args.manifest)

    counts = Counter(owners)
    total = args.width * args.height
    clear_count = counts.get(CLEAR_UID, 0)
    terrain_count = counts.get(TERRAIN_UID, 0)
    unknown_count = counts.get(UNKNOWN_UID, 0)
    entity_texels = total - clear_count - terrain_count - unknown_count
    entity_ids = [uid for uid in counts if uid not in (CLEAR_UID, TERRAIN_UID, UNKNOWN_UID)]

    print("OWNER_DUMP {0} {1}x{2} pixels={3}".format(
        args.owner_u32, args.width, args.height, total
    ))
    print("clear={0} terrain={1} unknown={2} entity_texels={3} unique_entity_ids={4}".format(
        clear_count, terrain_count, unknown_count, entity_texels, len(entity_ids)
    ))

    print("top_owners:")
    for uid, count in counts.most_common(args.top):
        pct = 100.0 * float(count) / float(total)
        print("  {0:<18} count={1:<9} pct={2:.4f}{3}".format(
            label_owner(uid), count, pct, manifest_text(uid, manifest)
        ))

    if args.texels:
        print("samples:")
    for x, y in args.texels:
        if x < 0 or y < 0 or x >= args.width or y >= args.height:
            print("  {0},{1} out-of-bounds".format(x, y))
            continue
        idx = y * args.width + x
        line = "  {0},{1} owner={2}".format(x, y, label_owner(owners[idx]))
        line += manifest_text(owners[idx], manifest)
        if depth is not None:
            line += " depth={0:.6f}".format(depth[idx])
        print(line)


if __name__ == "__main__":
    main()
