#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
STAMP="$(date +%Y-%m-%d-%H%M%S)"
OUT_DIR="${1:-"$ROOT_DIR/visual_parity_captures/gameplay-smoke-$STAMP"}"

mkdir -p "$OUT_DIR"
cd "$ROOT_DIR"

export PF_RTS_TIME_OF_DAY_PHASE="${PF_RTS_TIME_OF_DAY_PHASE:-baseline}"
export PF_RTS_TIME_OF_DAY_DYNAMIC="${PF_RTS_TIME_OF_DAY_DYNAMIC:-0}"
export PF_RENDER_WATER_MOVE_FACTOR="${PF_RENDER_WATER_MOVE_FACTOR:-0.0}"
export PF_VISUAL_PARITY_INCLUDE_SKYBOX="${PF_VISUAL_PARITY_INCLUDE_SKYBOX:-1}"
if [[ "$PF_VISUAL_PARITY_INCLUDE_SKYBOX" == "1" ]]; then
    export PF_GL_ENABLE_APPLE_ARM64_SKYBOX="${PF_GL_ENABLE_APPLE_ARM64_SKYBOX:-1}"
fi

for backend in OPENGL METAL; do
    make pf PLAT=MACOS_ARM64 MACOS_ARM64_BUILD_READY=1 RENDER_BACKEND="$backend"
    ./bin/pf-arm64 ./ ./scripts/macos/pf_metal_gameplay_smoke_probe.py \
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

required = ("camera", "selection", "move", "pause", "attack")
for label, summary in (("OPENGL", gl), ("METAL", metal)):
    if summary.get("status") != "pass":
        print("GAMEPLAY SMOKE MISMATCH {0} status={1} reason={2}".format(
            label, summary.get("status"), summary.get("reason")
        ), file=sys.stderr)
        sys.exit(2)
    checks = summary.get("checks", {})
    missing = [name for name in required if not checks.get(name)]
    if missing:
        print("GAMEPLAY SMOKE MISMATCH {0} missing={1}".format(
            label, ",".join(missing)
        ), file=sys.stderr)
        sys.exit(2)

gl_selected = len(gl.get("selected", []))
metal_selected = len(metal.get("selected", []))
if gl_selected != metal_selected:
    print("GAMEPLAY SMOKE MISMATCH selected gl={0} metal={1}".format(
        gl_selected, metal_selected
    ), file=sys.stderr)
    sys.exit(2)

print("GAMEPLAY SMOKE MATCH checks={0} selected={1} gl_frames={2} metal_frames={3}".format(
    len(required),
    gl_selected,
    gl.get("frame_ms", {}).get("count"),
    metal.get("frame_ms", {}).get("count"),
))
PY

printf '%s\n' "$OUT_DIR"
