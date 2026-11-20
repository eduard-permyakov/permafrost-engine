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
import math
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
MAT_BOUNDARY_GROUP = "__mat_boundary__"


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


class NormalSource:
    # The source mesh's corner normals, for carrying its hard and soft edges over to a
    # decimated copy: COLLAPSE drops custom split normals, which flattens faceted roofs and
    # rounds off hard corners.

    def __init__(self, obj):
        me = obj.data
        me.calc_loop_triangles()
        self.tris = [tuple(t.vertices) for t in me.loop_triangles]
        self.coords = [v.co.copy() for v in me.vertices]
        try:
            corner_n = [Vector(n.vector) for n in me.corner_normals]
        except Exception:
            corner_n = None
        self.normals = []
        for t in me.loop_triangles:
            if corner_n is None:
                self.normals.append((t.normal.copy(),) * 3)
            else:
                self.normals.append(tuple(corner_n[li] for li in t.loops))
        self.bvh = BVHTree.FromPolygons(self.coords, self.tris)

    def sample(self, co, toward, face_n):
        # The source normal at a point just inside the decimated face from its corner, so a
        # corner on a hard edge takes the normal of its own side of the edge
        p = co.lerp(toward, 0.15)
        hit = self.bvh.find_nearest(p)
        if hit[2] is None:
            return None
        a, b, c = (self.coords[i] for i in self.tris[hit[2]])
        w = barycentric(hit[0], a, b, c)
        na, nb, nc = self.normals[hit[2]]
        n = (na * w[0] + nb * w[1] + nc * w[2])
        if n.length < 1e-6 or n.dot(face_n) <= 0.0:
            return None
        return n.normalized()


def barycentric(p, a, b, c):
    v0, v1, v2 = b - a, c - a, p - a
    d00, d01, d11 = v0.dot(v0), v0.dot(v1), v1.dot(v1)
    d20, d21 = v2.dot(v0), v2.dot(v1)
    den = d00 * d11 - d01 * d01
    if abs(den) < 1e-12:
        return (1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0)
    v = (d11 * d20 - d01 * d21) / den
    w = (d00 * d21 - d01 * d20) / den
    return (1.0 - v - w, v, w)


def mesh_blocks(obj, name_to_idx, normal_src=None):
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
        centre = sum((me.vertices[v].co for v in tri.vertices), Vector()) / 3.0
        for k in order:
            li = tri.loops[k]
            vi = tri.vertices[k]
            co = trans @ me.vertices[vi].co
            src_n = None
            if normal_src is not None:
                src_n = normal_src.sample(me.vertices[vi].co, centre, tri.normal)
            if src_n is not None:
                no = (normal_mat @ src_n).normalized()
            elif corner_n is not None and li < len(corner_n):
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


def tag_material_boundary(obj, uv_seams=False):
    # COLLAPSE has no material delimiter, so a collapse may weld two materials together and
    # hand the survivors one material index: a thin two-sided sheet, such as a cape lined with
    # the faction colour, loses its outer surface to the lining. Pin the seam vertices instead.
    # A collapse across a UV seam likewise stretches the texture between two islands.
    me = obj.data
    mats = {}
    for poly in me.polygons:
        for vi in poly.vertices:
            mats.setdefault(vi, set()).add(poly.material_index)
    seam = {vi for vi, m in mats.items() if len(m) > 1}
    if uv_seams:
        uv = me.uv_layers.active.data
        uvs = {}
        for loop in me.loops:
            u = uv[loop.index].uv
            uvs.setdefault(loop.vertex_index, set()).add((round(u.x, 4), round(u.y, 4)))
        seam |= {vi for vi, s in uvs.items() if len(s) > 1}
    seam = sorted(seam)
    if not seam:
        return None
    vg = obj.vertex_groups.new(name=MAT_BOUNDARY_GROUP)
    vg.add(seam, 1.0, 'REPLACE')
    return vg.name


def decimate_copy(base, ratio, uv_seams=False):
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
    seam = tag_material_boundary(new, uv_seams)
    if seam:
        dec.vertex_group = seam
        dec.vertex_group_factor = 1.0
        dec.invert_vertex_group = True
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


def surface_points(obj, cap=2000):
    vs = obj.data.vertices
    step = max(1, len(vs) // cap)
    return [obj.matrix_world @ vs[i].co for i in range(0, len(vs), step)]


def surface_points_by_material(obj, cap=4000, min_share=0.02):
    # Corners and centres of the faces, each tagged with its face's material. A material
    # covering less than 'min_share' of the surface, such as a small emblem, is tagged None:
    # losing it costs a speck of colour, and it would otherwise hold the whole model back.
    me = obj.data
    polys = me.polygons
    area = {}
    for poly in polys:
        area[poly.material_index] = area.get(poly.material_index, 0.0) + poly.area
    total = sum(area.values()) or 1.0
    step = max(1, len(polys) // (cap // 4))
    mw = obj.matrix_world
    coords = [(mw @ v.co) for v in me.vertices]
    bvh = BVHTree.FromPolygons([c[:] for c in coords], [list(p.vertices) for p in polys])
    out = []
    every = []
    for i in range(0, len(polys), step):
        poly = polys[i]
        tag = poly.material_index if area[poly.material_index] / total >= min_share else None
        centre = mw @ poly.center
        for co in [centre] + [coords[vi].lerp(centre, 0.1) for vi in poly.vertices]:
            every.append((co, tag))
            if seen_from_above(bvh, co):
                out.append((co, tag))
    return out or every


# The directions an RTS camera or the sun looks at a model from, pointing away from it, in
# Blender's Z-up space the importer converts to
SKY_DIRS = [Vector((math.cos(e) * math.sin(a), math.cos(e) * math.cos(a), math.sin(e)))
            for e in (math.radians(20), math.radians(45), math.radians(70))
            for a in (k * math.pi / 4.0 for k in range(8))] + [Vector((0.0, 0.0, 1.0))]


def seen_from_above(bvh, co):
    # A point no ray toward the sky escapes from is inside or under the model, where its loss
    # cannot show. Faces are not trusted to point outward.
    for d in SKY_DIRS:
        if bvh.ray_cast(co + d * 1e-3, d)[0] is None:
            return True
    return False


MATERIAL_PERCENTILE = 0.98


def deviation(dup, orig_pts, extent):
    # Largest distance from an original surface point to the decimated surface, over the extent.
    # Points tagged with a material also measure to that material's surface alone, so trim that
    # collapses into the wall behind it counts as lost; a high percentile rather than the
    # largest distance, so that one small detail lost in a colour costs no more than its shape.
    me = dup.data
    coords = [(dup.matrix_world @ v.co)[:] for v in me.vertices]
    tagged = bool(orig_pts) and isinstance(orig_pts[0], tuple)
    if not tagged:
        orig_pts = [(co, None) for co in orig_pts]
    by_mat = {None: []}
    for p in me.polygons:
        by_mat[None].append(list(p.vertices))
        if tagged:
            by_mat.setdefault(p.material_index, []).append(list(p.vertices))
    bvhs = {m: BVHTree.FromPolygons(coords, polys) for m, polys in by_mat.items()}
    md = 0.0
    per_mat = {}
    for co, mat in orig_pts:
        hit = bvhs[None].find_nearest(co)
        if hit[3] is not None and hit[3] > md:
            md = hit[3]
        if mat is None:
            continue
        bvh = bvhs.get(mat)
        d = float("inf") if bvh is None else bvh.find_nearest(co)[3]
        per_mat.setdefault(mat, []).append(d if d is not None else 0.0)
    for ds in per_mat.values():
        ds.sort()
        md = max(md, ds[min(len(ds) - 1, int(len(ds) * MATERIAL_PERCENTILE))])
    return md / extent


def collapse_to_error(base, orig_pts, extent, target_err, min_ratio, steps, name_to_idx,
                      max_ratio=1.0, uv_seams=False, normal_src=None):
    # Binary-search the lowest keep-ratio whose surface deviation stays within target_err, so a
    # uniform error bound collapses over-tessellated meshes more and lean ones less.
    lo, hi, best = min_ratio, max_ratio, max_ratio
    for _ in range(steps):
        mid = (lo + hi) / 2.0
        dup = decimate_copy(base, mid, uv_seams)
        err = deviation(dup, orig_pts, extent)
        drop(dup)
        if err <= target_err:
            best, hi = mid, mid
        else:
            lo = mid
    dup = decimate_copy(base, best, uv_seams)
    err = deviation(dup, orig_pts, extent)
    blocks = mesh_blocks(dup, name_to_idx, normal_src)
    drop(dup)
    return blocks, best, err
