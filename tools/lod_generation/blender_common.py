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
#  Blender-side helpers shared by the LOD passes (both run inside `blender --background`):
#  import a PFOBJ, decimate it, and rebuild the v/vt/vn/vw/vm soup in engine space. The PFOBJ
#  tail (materials, joints, animations, bounds) is reused verbatim by the caller, so only the
#  triangle soup is produced here.
#

try:
    import bpy
except ModuleNotFoundError:
    raise SystemExit("error: the lod_generation passes run inside Blender; launch with\n"
                     "  blender --background --python tools/lod_generation/<pass>.py -- <dir>")
import os
import sys
from mathutils import Matrix, Vector
from mathutils.bvhtree import BVHTree
from bpy_extras.io_utils import axis_conversion

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "blender_addon"))
sys.path.insert(0, HERE)
from io_scene_pfobj import import_pfobj            # noqa: E402
import lodlib as ll                                # noqa: E402

# Engine space is left-handed (determinant -1): reflect X, which also flips winding.
GLOBAL_MATRIX = Matrix.Diagonal(Vector((-1.0, 1.0, 1.0, 1.0))) \
    @ axis_conversion(to_forward='-Z', to_up='Y').to_4x4()

MAX_JOINTS_PER_VERT = 6                            # engine vertex format limit


def require_blender():
    if bpy.app.version < (2, 80, 0):
        sys.exit("error: needs Blender >= 2.80 (got %s)" % (bpy.app.version,))


def args_after_dashes():
    return sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []


def collect_sources(inputs):
    sources = []
    for inp in inputs:
        if os.path.isdir(inp):
            sources += ll.walk_sources(inp)
        elif inp.lower().endswith(".pfobj") and not ll.LOD_RE.search(inp):
            sources.append(inp)
        else:
            print("warning: no directory or source .pfobj at %r (wrong working directory?)" % inp)
    return sorted(set(sources))


def load_joined(src):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    import_pfobj.load(None, bpy.context, src, GLOBAL_MATRIX)
    meshes = [o for o in bpy.context.scene.objects if o.type == 'MESH']
    if not meshes:
        return None
    if len(meshes) > 1:
        bpy.ops.object.select_all(action='DESELECT')
        for o in meshes:
            o.select_set(True)
        bpy.context.view_layer.objects.active = meshes[0]
        bpy.ops.object.join()
        meshes = [meshes[0]]
    return meshes[0]


def joint_names(tail):
    return [ln.split()[2] for ln in tail if ln.startswith("j ")]


def weight_line(vert, vgroups, name_to_idx):
    if not name_to_idx:
        return "vw \n"
    pairs = []
    for g in vert.groups:
        if g.weight <= 0.0:
            continue
        idx = name_to_idx.get(vgroups[g.group].name)
        if idx is not None:
            pairs.append((idx, g.weight))
    pairs.sort(key=lambda p: -p[1])
    pairs = pairs[:MAX_JOINTS_PER_VERT]
    total = sum(w for _, w in pairs) or 1.0
    return "vw " + "".join(" %d/%.6f" % (idx, w / total) for idx, w in pairs) + "\n"


def mesh_blocks(obj, name_to_idx):
    me = obj.data
    me.calc_loop_triangles()
    trans = GLOBAL_MATRIX @ obj.matrix_world
    normal_mat = trans.to_3x3().inverted().transposed()
    flip = trans.determinant() < 0.0
    uv = me.uv_layers.active.data
    try:
        corner_n = [Vector(n.vector) for n in me.corner_normals]
    except Exception:
        corner_n = None
    vgroups = obj.vertex_groups
    order = (2, 1, 0) if flip else (0, 1, 2)
    blocks = []
    for tri in me.loop_triangles:
        face_n = (normal_mat @ tri.normal).normalized()
        vm = tri.material_index
        for k in order:
            li = tri.loops[k]
            vi = tri.vertices[k]
            co = trans @ me.vertices[vi].co
            if corner_n is not None and li < len(corner_n):
                no = (normal_mat @ corner_n[li]).normalized()
            else:
                no = face_n
            u = uv[li].uv
            blocks.append(
                "v %.6f %.6f %.6f\n" % (co.x, co.y, co.z) +
                "vt %.6f %.6f\n" % (u.x, u.y) +
                "vn %.6f %.6f %.6f\n" % (no.x, no.y, no.z) +
                weight_line(me.vertices[vi], vgroups, name_to_idx) +
                "vm %d\n" % vm)
    return blocks


def decimate_copy(base, ratio):
    # Apply (not just evaluate) the modifier so the result keeps interpolated vertex weights.
    new = base.copy()
    new.data = base.data.copy()
    bpy.context.collection.objects.link(new)
    for mod in list(new.modifiers):
        new.modifiers.remove(mod)
    dec = new.modifiers.new("decimate", 'DECIMATE')
    dec.decimate_type = 'COLLAPSE'
    dec.ratio = ratio
    dec.use_collapse_triangulate = True
    bpy.ops.object.select_all(action='DESELECT')
    new.select_set(True)
    bpy.context.view_layer.objects.active = new
    bpy.context.view_layer.update()
    bpy.ops.object.modifier_apply(modifier=dec.name)
    return new


def drop(obj):
    me = obj.data
    bpy.data.objects.remove(obj, do_unlink=True)
    if me.users == 0:
        bpy.data.meshes.remove(me)


def collapse_gated(base, ntris, start, cap, floor, gate, max_holes, max_tries, name_to_idx):
    # Collapse toward the keep-ratio; if the coverage gate finds holes, back off toward the
    # cap and retry. Returns (blocks, holes, achieved_ratio) for the best attempt.
    orig_vox, mn, step = gate
    ratio = start
    result = None
    for _ in range(max_tries):
        target = max(int(round(ratio * ntris)), floor)
        eff = min(1.0, target / float(ntris))
        dup = decimate_copy(base, eff)
        blocks = mesh_blocks(dup, name_to_idx)
        drop(dup)
        holes = ll.hole_pct(orig_vox, ll.surface_voxels(ll.blocks_to_tris(blocks), mn, step))
        result = (blocks, holes, eff)
        if holes <= max_holes:
            break
        nxt = min(cap, ratio / 0.6)
        if nxt - ratio < 1e-6:
            break
        ratio = nxt
    return result


def surface_points(obj, cap=2000):
    vs = obj.data.vertices
    step = max(1, len(vs) // cap)
    return [obj.matrix_world @ vs[i].co for i in range(0, len(vs), step)]


def deviation(dup, orig_pts, extent):
    # Largest distance from an original surface point to the decimated surface, over the extent.
    me = dup.data
    coords = [(dup.matrix_world @ v.co)[:] for v in me.vertices]
    polys = [list(p.vertices) for p in me.polygons]
    bvh = BVHTree.FromPolygons(coords, polys)
    md = 0.0
    for co in orig_pts:
        hit = bvh.find_nearest(co)
        if hit[3] is not None and hit[3] > md:
            md = hit[3]
    return md / extent


def collapse_to_error(base, orig_pts, extent, target_err, min_ratio, steps, name_to_idx):
    # Binary-search the lowest keep-ratio whose surface deviation stays within target_err, so a
    # uniform error bound collapses over-tessellated meshes more and lean ones less.
    lo, hi, best = min_ratio, 1.0, 1.0
    for _ in range(steps):
        mid = (lo + hi) / 2.0
        dup = decimate_copy(base, mid)
        err = deviation(dup, orig_pts, extent)
        drop(dup)
        if err <= target_err:
            best, hi = mid, mid
        else:
            lo = mid
    dup = decimate_copy(base, best)
    blocks = mesh_blocks(dup, name_to_idx)
    drop(dup)
    return blocks, best
