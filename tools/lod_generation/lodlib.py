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
#  Pure-Python PFOBJ helpers shared by the LOD passes. A PFOBJ mesh is an unindexed triangle
#  soup with five lines per corner (v/vt/vn/vw/vm), preceded by a 7-line header and followed
#  by a tail of materials, joints, animations and bounds. The passes only rewrite the soup,
#  so the header and tail are copied verbatim. A voxel surface-coverage metric detects when a
#  decimation has torn holes in the surface.
#

import math
import os
import re

LOD_RE = re.compile(r"\.lod\d.*\.pfobj$", re.I)


def parse_pfobj(path):
    with open(path, "r") as f:
        lines = f.readlines()
    num_verts = None
    for line in lines[:7]:
        if line.startswith("num_verts"):
            num_verts = int(line.split()[1])
    if num_verts is None:
        raise ValueError("%s: missing num_verts header" % path)
    end = 7 + num_verts * 5
    return lines[:7], num_verts, lines[7:end], lines[end:]


def corners_from(vlines, num_verts):
    corners = []
    for i in range(num_verts):
        block = vlines[i*5:i*5+5]
        v, vt, vn = block[0].split(), block[1].split(), block[2].split()
        corners.append({
            "pos": (float(v[1]), float(v[2]), float(v[3])),
            "nrm": (float(vn[1]), float(vn[2]), float(vn[3])),
            "uv":  (float(vt[1]), float(vt[2])),
            "mat": int(block[4].split()[1]),
        })
    return corners


def write_lod(out_path, header, new_num_verts, out_blocks, tail):
    with open(out_path, "w") as f:
        for line in header:
            if line.startswith("num_verts"):
                line = re.sub(r"\d+", str(new_num_verts), line, count=1)
            f.write(line)
        f.writelines(out_blocks)
        f.writelines(tail)


def walk_sources(root):
    out = []
    for dirpath, _, files in os.walk(root):
        for f in files:
            if f.lower().endswith(".pfobj") and not LOD_RE.search(f):
                out.append(os.path.join(dirpath, f))
    return sorted(out)


def blocks_to_tris(blocks):
    tris = []
    for i in range(0, len(blocks) - 2, 3):
        P = []
        for j in range(3):
            v = blocks[i+j].split("\n", 1)[0].split()
            P.append((float(v[1]), float(v[2]), float(v[3])))
        tris.append(P)
    return tris


def grid_params(tris):
    pts = [p for T in tris for p in T]
    mn = [min(p[i] for p in pts) for i in range(3)]
    step = (max(max(p[i] for p in pts) - mn[i] for i in range(3)) / 120.0) or 1.0
    return mn, step


def surface_voxels(tris, mn, step):
    # The set of voxels the surface passes through, triangles sampled finer than a voxel.
    vox = set()
    for a, b, c in tris:
        e1 = [b[i]-a[i] for i in range(3)]; e2 = [c[i]-a[i] for i in range(3)]
        n = max(1, int(math.ceil(max(math.sqrt(sum(x*x for x in e1)),
                                     math.sqrt(sum(x*x for x in e2))) / (step * 0.6))))
        for i in range(n + 1):
            for j in range(n + 1 - i):
                u, v = i / n, j / n
                vox.add(tuple(int((a[k] + u*e1[k] + v*e2[k] - mn[k]) / step) for k in range(3)))
    return vox


def hole_pct(orig_vox, lod_vox):
    # Percentage of original surface voxels with no LOD surface in their 1-voxel neighbourhood.
    nbr = [(dx, dy, dz) for dx in (-1, 0, 1) for dy in (-1, 0, 1) for dz in (-1, 0, 1)]
    miss = sum(1 for v in orig_vox if not any((v[0]+d[0], v[1]+d[1], v[2]+d[2]) in lod_vox for d in nbr))
    return 100.0 * miss / max(len(orig_vox), 1)


def verify_context(corners, ntris):
    tris = [[corners[3*t+j]["pos"] for j in range(3)] for t in range(ntris)]
    mn, step = grid_params(tris)
    return surface_voxels(tris, mn, step), mn, step

