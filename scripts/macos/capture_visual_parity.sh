#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
STAMP="$(date +%Y-%m-%d-%H%M%S)"
OUT_DIR="${1:-"$ROOT_DIR/visual_parity_captures/$STAMP"}"

mkdir -p "$OUT_DIR"
cd "$ROOT_DIR"

export PF_RTS_TIME_OF_DAY_PHASE="${PF_RTS_TIME_OF_DAY_PHASE:-baseline}"
export PF_RTS_TIME_OF_DAY_DYNAMIC="${PF_RTS_TIME_OF_DAY_DYNAMIC:-0}"
export PF_RENDER_WATER_MOVE_FACTOR="${PF_RENDER_WATER_MOVE_FACTOR:-0.0}"
export PF_VISUAL_PARITY_INCLUDE_SKYBOX="${PF_VISUAL_PARITY_INCLUDE_SKYBOX:-1}"
export PF_VISUAL_PARITY_DISABLE_VSYNC="${PF_VISUAL_PARITY_DISABLE_VSYNC:-1}"
export PF_VISUAL_PARITY_ACTIVATE_WINDOW="${PF_VISUAL_PARITY_ACTIVATE_WINDOW:-1}"
export PF_PARITY_MODE="${PF_PARITY_MODE:-1}"
if [[ "$PF_VISUAL_PARITY_INCLUDE_SKYBOX" == "1" ]]; then
    export PF_GL_ENABLE_APPLE_ARM64_SKYBOX="${PF_GL_ENABLE_APPLE_ARM64_SKYBOX:-1}"
fi

for backend in OPENGL METAL; do
    make pf PLAT=MACOS_ARM64 MACOS_ARM64_BUILD_READY=1 RENDER_BACKEND="$backend"
    ./bin/pf-arm64 ./ ./scripts/macos/pf_visual_parity_probe.py \
        --output-dir "$OUT_DIR" \
        --expect-backend "$backend"
done

make pf PLAT=MACOS_ARM64 MACOS_ARM64_BUILD_READY=1 RENDER_BACKEND=METAL

python3 - "$OUT_DIR" <<'PY'
import json
import os
import sys

out_dir = sys.argv[1]
with open(os.path.join(out_dir, "summary_opengl.json")) as infile:
    gl = json.load(infile)
with open(os.path.join(out_dir, "summary_metal.json")) as infile:
    metal = json.load(infile)

gl_records = gl.get("records", [])
metal_records = metal.get("records", [])
if len(gl_records) != len(metal_records):
    print("CAMERA MISMATCH scene-count gl={0} metal={1}".format(
        len(gl_records), len(metal_records)
    ), file=sys.stderr)
    sys.exit(2)

max_delta = 0.0
for gl_record, metal_record in zip(gl_records, metal_records):
    gl_name = gl_record.get("name")
    metal_name = metal_record.get("name")
    if gl_name != metal_name:
        print("CAMERA MISMATCH scene-name gl={0} metal={1}".format(
            gl_name, metal_name
        ), file=sys.stderr)
        sys.exit(2)
    gl_position = gl_record.get("camera_position", [])
    metal_position = metal_record.get("camera_position", [])
    if len(gl_position) != 3 or len(metal_position) != 3:
        print("CAMERA MISMATCH scene={0} invalid-position".format(gl_name),
              file=sys.stderr)
        sys.exit(2)
    for axis, gl_value, metal_value in zip("xyz", gl_position, metal_position):
        delta = abs(gl_value - metal_value)
        max_delta = max(max_delta, delta)
        if delta > 0.1:
            print("CAMERA MISMATCH scene={0} axis={1} gl={2} metal={3}".format(
                gl_name, axis, gl_value, metal_value
            ), file=sys.stderr)
            sys.exit(2)

print("CAMERAS MATCH scenes={0} max_position_delta={1:.6f}".format(
    len(gl_records), max_delta
))
PY

printf '%s\n' "$OUT_DIR"
