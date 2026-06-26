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
#  LOD-generation pass: writes <name>.lod1.pfobj and <name>.lod2.pfobj next to each source
#  mesh using Blender's COLLAPSE decimate. LOD1 keeps 50%, LOD2 targets 25% and backs off no
#  further than LOD1 when the collapse would tear the surface (a coverage gate). The PFOBJ
#  tail is reused verbatim, so materials, joints, animations and bounds are unchanged.
#
#  Run: blender --background --python tools/lod_generation/lod_generation_pass.py -- <dir> [opts]
#

import os
import re
import sys
import argparse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import blender_common as bl                        # noqa: E402
import lodlib as ll                                # noqa: E402


def process(src, args):
    header, nv, vlines, tail = ll.parse_pfobj(src)
    ntris = nv // 3
    name = os.path.basename(src)
    if ntris < args.min_tris:
        print("RESULT\t%s\tskip\t%d" % (name, ntris))
        return

    gate = ll.verify_context(ll.corners_from(vlines, nv), ntris)
    name_to_idx = {n: i for i, n in enumerate(bl.joint_names(tail))}
    base = bl.load_joined(src)
    if base is None or not base.data.uv_layers.active:
        print("RESULT\t%s\terror\t%d" % (name, ntris))
        return

    lod1_eff = 1.0
    lod_tris = {}
    for level, start, cap in ((1, args.lod1, 1.0), (2, args.lod2, None)):
        # LOD2 backs off no further than LOD1: a mesh that cannot decimate cleanly falls back
        # to the LOD1 mesh rather than tearing.
        cap = lod1_eff if cap is None else cap
        blocks, holes, eff = bl.collapse_gated(base, ntris, min(start, cap), cap, args.floor,
                                               gate, args.max_holes, args.max_tries, name_to_idx)
        if level == 1:
            lod1_eff = eff
        out = re.sub(r"\.pfobj$", ".lod%d.pfobj" % level, src, flags=re.I)
        if not args.dry_run:
            ll.write_lod(out, header, len(blocks), blocks, tail)
        lod_tris[level] = len(blocks) // 3
        flag = "MISSED" if holes > args.max_holes else ("relaxed" if eff > start + 0.02 else "ok")
        print("RESULT\t%s\tlod%d\t%d\t%.2f\t%.3f\t%s" % (name, level, lod_tris[level], holes, eff, flag))
    return ntris, lod_tris[1], lod_tris[2]


def main():
    ap = argparse.ArgumentParser(description="Generate PFOBJ LODs with Blender COLLAPSE decimate.")
    ap.add_argument("inputs", nargs="+", help="directories and/or .pfobj files")
    ap.add_argument("--lod1", type=float, default=0.50, help="LOD1 keep-ratio (default 0.50)")
    ap.add_argument("--lod2", type=float, default=0.25, help="LOD2 target keep-ratio (default 0.25)")
    ap.add_argument("--min-tris", type=int, default=150, help="skip meshes below this (default 150)")
    ap.add_argument("--floor", type=int, default=64, help="never reduce a level below this (default 64)")
    ap.add_argument("--max-holes", type=float, default=1.0,
                    help="max uncovered surface %% before backing off; pass a large value to force (default 1.0)")
    ap.add_argument("--max-tries", type=int, default=5)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(bl.args_after_dashes())
    bl.require_blender()

    sources = bl.collect_sources(args.inputs)
    print("PROCESSING %d source meshes (lod1 %.2f, lod2 %.2f, max-holes %.1f%%)" % (
        len(sources), args.lod1, args.lod2, args.max_holes))
    tot_in = tot1 = tot2 = 0
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
            tot1 += res[1]
            tot2 += res[2]
    if tot_in:
        print("SUMMARY\tbase %d  lod1 %d (-%.0f%%)  lod2 %d (-%.0f%%)" % (
            tot_in, tot1, 100.0 * (1 - tot1 / tot_in), tot2, 100.0 * (1 - tot2 / tot_in)))


if __name__ == "__main__":
    main()
