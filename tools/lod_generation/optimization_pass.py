#!/usr/bin/env python3
#
#  This file is part of Permafrost Engine.
#  Copyright (C) 2026 Eduard Permyakov
#
#  Permafrost Engine is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, either version 3 of the License, or
#  (at your option) any later version.
#
#  Permafrost Engine is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
#
#  Base-mesh optimization pass: collapses each source mesh (LOD0) as far as a quadric-error
#  bound allows, so over-tessellated meshes reduce a lot and lean ones barely change. The
#  bound is the largest surface deviation a collapse introduces, as a fraction of the model
#  extent, binary-searched per mesh to the lowest keep-ratio that stays within it. Rewrites
#  each .pfobj in place and keeps a one-time .orig backup; the original is always re-read from
#  .orig so re-runs don't compound.
#
#  Run: blender --background --python tools/lod_generation/optimization_pass.py -- <dir> [--error 0.0006]
#

import os
import sys
import shutil
import argparse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import blender_common as bl                        # noqa: E402
import lodlib as ll                                # noqa: E402
from mathutils import Vector                       # noqa: E402


def process(src, args):
    backup = src + ".orig"
    pristine = backup if os.path.exists(backup) else src
    header, nv, vlines, tail = ll.parse_pfobj(pristine)
    ntris = nv // 3
    name = os.path.basename(src)
    if ntris < args.min_tris:
        print("RESULT\t%s\tskip\t%d" % (name, ntris))
        return ntris, ntris

    base = bl.load_joined(pristine)
    if base is None or not base.data.uv_layers.active:
        print("RESULT\t%s\terror\t%d" % (name, ntris))
        return ntris, ntris

    bb = [base.matrix_world @ Vector(c) for c in base.bound_box]
    extent = max(max(p[i] for p in bb) - min(p[i] for p in bb) for i in range(3)) or 1.0
    pts = bl.surface_points(base)
    name_to_idx = {n: i for i, n in enumerate(bl.joint_names(tail))}
    blocks, ratio = bl.collapse_to_error(base, pts, extent, args.error,
                                         args.min_ratio, args.steps, name_to_idx)
    if not args.dry_run:
        if not os.path.exists(backup):
            shutil.copy2(src, backup)
        ll.write_lod(src, header, len(blocks), blocks, tail)
    out = len(blocks) // 3
    print("RESULT\t%s\tok\t%d\t%d\t%.0f%%\t%.2f" % (name, ntris, out, 100.0 * out / ntris, ratio))
    return ntris, out


def main():
    ap = argparse.ArgumentParser(description="Optimize PFOBJ base meshes to a quadric-error bound.")
    ap.add_argument("inputs", nargs="+", help="directories and/or .pfobj files")
    ap.add_argument("--error", type=float, default=0.0006,
                    help="max surface deviation as a fraction of model extent (default 0.0006)")
    ap.add_argument("--min-ratio", type=float, default=0.5,
                    help="never collapse below this keep-ratio (default 0.50)")
    ap.add_argument("--steps", type=int, default=7, help="binary-search iterations (default 7)")
    ap.add_argument("--min-tris", type=int, default=150, help="skip meshes below this (default 150)")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(bl.args_after_dashes())
    bl.require_blender()

    sources = bl.collect_sources(args.inputs)
    print("PROCESSING %d source meshes (error %.4f)" % (len(sources), args.error))
    tot_in = tot_out = 0
    for src in sources:
        try:
            res = process(src, args)
        except Exception as exc:
            import traceback
            traceback.print_exc()
            print("RESULT\t%s\terror\t%s" % (os.path.basename(src), repr(exc)[:60]))
            res = None
        if res:
            tot_in += res[0]
            tot_out += res[1]
    if tot_in:
        print("SUMMARY\t%d -> %d tris  (-%.0f%%)" % (tot_in, tot_out, 100.0 * (1 - tot_out / tot_in)))


if __name__ == "__main__":
    main()
