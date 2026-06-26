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
#  Linking this software statically or dynamically with other modules is making
#  a combined work based on this software. Thus, the terms and conditions of
#  the GNU General Public License cover the whole combination.
#
#  As a special exception, the copyright holders of Permafrost Engine give
#  you permission to link Permafrost Engine with independent modules to produce
#  an executable, regardless of the license terms of these independent
#  modules, and to copy and distribute the resulting executable under
#  terms of your choice, provided that you also meet, for each linked
#  independent module, the terms and conditions of the license of that
#  module. An independent module is a module which is not derived from
#  or based on Permafrost Engine. If you modify Permafrost Engine, you may
#  extend this exception to your version of Permafrost Engine, but you are not
#  obliged to do so. If you do not wish to do so, delete this exception
#  statement from your version.
#
#
#  Targets Blender 2.80 and later.
#

import bpy
import os
from mathutils import Matrix, Vector, Quaternion


def sqt_to_local(s, q, t):
    # Inverse of the exporter's decompose_sqt: the rotation quaternion is stored inverted,
    # so invert it again to rebuild the matrix that was decomposed. Stored order is x/y/z/w.
    rot = Quaternion((q[3], q[0], q[1], q[2]))
    rot.invert()
    return Matrix.LocRotScale(Vector(t), rot, Vector(s))


class Reader:
    # Sequential cursor over the file's non-blank lines.
    def __init__(self, text):
        self.lines = text.splitlines()
        self.i = 0

    def next(self):
        while self.i < len(self.lines):
            line = self.lines[self.i]
            self.i += 1
            if line.strip():
                return line
        raise EOFError("unexpected end of PFOBJ file")


def parse(text):
    r = Reader(text)

    def header(key):
        toks = r.next().split()
        if toks[0] != key:
            raise ValueError("expected '%s', got '%s'" % (key, toks[0]))
        return toks

    header("version")
    num_verts     = int(header("num_verts")[1])
    num_joints    = int(header("num_joints")[1])
    num_materials = int(header("num_materials")[1])
    num_as        = int(header("num_as")[1])
    header("frame_counts")
    has_collision = int(header("has_collision")[1])

    # Each vertex is a triangle corner: position, uv, normal, joint weights, material index.
    verts = []
    for _ in range(num_verts):
        v  = r.next().split()
        vt = r.next().split()
        vn = r.next().split()
        vw = r.next().split()
        vm = r.next().split()
        weights = []
        for pair in vw[1:]:
            joint, weight = pair.split("/")
            weights.append((int(joint), float(weight)))
        verts.append({
            "pos":      (float(v[1]),  float(v[2]),  float(v[3])),
            "uv":       (float(vt[1]), float(vt[2])),
            "normal":   (float(vn[1]), float(vn[2]), float(vn[3])),
            "weights":  weights,
            "material": int(vm[1]),
        })

    materials = []
    for _ in range(num_materials):
        name = r.next().split(None, 1)[1].strip()
        r.next()                                            # ambient (constant, unused)
        diffuse  = [float(x) for x in r.next().split()[1:4]]
        specular = [float(x) for x in r.next().split()[1:4]]
        tex_toks = r.next().split(None, 1)
        texture  = tex_toks[1].strip() if len(tex_toks) > 1 else None
        materials.append({"name": name, "diffuse": diffuse,
                          "specular": specular, "texture": texture})

    joints = []
    for _ in range(num_joints):
        toks = r.next().split()
        joints.append({
            "parent": int(toks[1]),                         # 1-based, 0 means root
            "name":   toks[2],
            "s":      [float(x) for x in toks[3].split("/")],
            "q":      [float(x) for x in toks[4].split("/")],
            "t":      [float(x) for x in toks[5].split("/")],
            "tip":    [float(x) for x in toks[6].split("/")],
        })

    anims = []
    for _ in range(num_as):
        toks    = r.next().split()
        nframes = int(toks[-1])
        name    = " ".join(toks[1:-1])
        frames  = []
        for _frame in range(nframes):
            pose = {}
            for _bone in range(num_joints):
                jt = r.next().split()
                pose[int(jt[0])] = (
                    [float(x) for x in jt[1].split("/")],
                    [float(x) for x in jt[2].split("/")],
                    [float(x) for x in jt[3].split("/")],
                )
            if has_collision:
                for _bound in range(3):
                    r.next()                                # per-frame bounds, not needed
            frames.append(pose)
        anims.append({"name": name, "frames": frames})

    return {
        "num_joints": num_joints,
        "verts":      verts,
        "materials":  materials,
        "joints":     joints,
        "anims":      anims,
    }


def build_mesh(data, name, inv_global, scale):
    nrm_mat = inv_global.to_3x3().inverted().transposed()
    verts = data["verts"]

    # Weld the triangle soup back into shared vertices. The key is position plus normal:
    # the exporter writes a per-vertex normal, so hard-edge splits must be kept distinct,
    # whereas UV seams need not split the vertex since UVs are stored per loop.
    pool = {}
    positions    = []
    pool_weights = []
    corner_vert  = [0] * len(verts)
    for ci, vert in enumerate(verts):
        co  = (inv_global @ Vector(vert["pos"])) * scale
        n   = vert["normal"]
        key = (round(co.x, 5), round(co.y, 5), round(co.z, 5),
               round(n[0], 4), round(n[1], 4), round(n[2], 4))
        vi  = pool.get(key)
        if vi is None:
            vi = len(positions)
            pool[key] = vi
            positions.append(co)
            pool_weights.append(vert["weights"])
        corner_vert[ci] = vi

    faces        = []
    face_corners = []
    face_material = []
    for tri in range(len(verts) // 3):
        a, b, c = 3 * tri, 3 * tri + 1, 3 * tri + 2
        va, vb, vc = corner_vert[a], corner_vert[b], corner_vert[c]
        if va == vb or vb == vc or va == vc:
            continue                                        # degenerate after welding
        # The exporter reverses winding for the mirrored coordinate system; undo it.
        faces.append((vc, vb, va))
        face_corners.append((c, b, a))
        face_material.append(verts[a]["material"])

    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata([tuple(p) for p in positions], [], faces)
    mesh.update()

    uv_layer     = mesh.uv_layers.new(name="UVMap")
    loop_normals = [(0.0, 0.0, 1.0)] * len(mesh.loops)
    for poly, corners in zip(mesh.polygons, face_corners):
        poly.use_smooth = True
        for loop_idx, corner in zip(poly.loop_indices, corners):
            uv_layer.data[loop_idx].uv = verts[corner]["uv"]
            n = nrm_mat @ Vector(verts[corner]["normal"])
            n.normalize()
            loop_normals[loop_idx] = (n.x, n.y, n.z)

    try:
        mesh.normals_split_custom_set(loop_normals)
    except Exception as exc:
        print("PFOBJ import: custom normals not applied (%s)" % exc)
    mesh.update()
    return mesh, face_material, pool_weights


def build_materials(mesh, materials, model_dir):
    for mat in materials:
        bmat = bpy.data.materials.new(mat["name"])
        bmat.use_nodes = True
        bsdf = bmat.node_tree.nodes.get("Principled BSDF")
        if bsdf is not None:
            d = mat["diffuse"]
            bsdf.inputs["Base Color"].default_value = (d[0], d[1], d[2], 1.0)
            spec = bsdf.inputs.get("Specular IOR Level") or bsdf.inputs.get("Specular")
            if spec is not None:
                spec.default_value = mat["specular"][0]
            if mat["texture"]:
                path = os.path.join(model_dir, mat["texture"])
                if os.path.exists(path):
                    image = bpy.data.images.load(path, check_existing=True)
                    tex = bmat.node_tree.nodes.new("ShaderNodeTexImage")
                    tex.image = image
                    bmat.node_tree.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
        mesh.materials.append(bmat)


def build_armature(data, name, inv_global, scale, collection):
    joints = data["joints"]

    # Rest matrix in armature space: the root is placed by the inverse global transform,
    # every child is composed onto its (already resolved) parent.
    rest = [None] * len(joints)
    def rest_matrix(i):
        if rest[i] is None:
            j = joints[i]
            local = sqt_to_local(j["s"], j["q"], j["t"])
            if j["parent"] == 0:
                rest[i] = inv_global @ local
            else:
                rest[i] = rest_matrix(j["parent"] - 1) @ local
        return rest[i]
    for i in range(len(joints)):
        rest_matrix(i)

    arm = bpy.data.armatures.new(name + "_arm")
    arm_obj = bpy.data.objects.new(name + "_arm", arm)
    collection.objects.link(arm_obj)

    bpy.context.view_layer.objects.active = arm_obj
    bpy.ops.object.mode_set(mode="EDIT")
    edit_bones = []
    bone_names = []
    for i, j in enumerate(joints):
        eb = arm.edit_bones.new(j["name"])
        eb.head   = (0.0, 0.0, 0.0)
        eb.tail   = (0.0, 1.0, 0.0)
        m = Matrix.Scale(scale, 4) @ rest[i]
        eb.matrix = m
        eb.length = max((m.to_3x3() @ Vector(j["tip"])).length, 1e-4)
        edit_bones.append(eb)
        bone_names.append(eb.name)                          # Blender may dedupe names
    for i, j in enumerate(joints):
        if j["parent"] != 0:
            edit_bones[i].parent = edit_bones[j["parent"] - 1]
            edit_bones[i].use_connect = False
    bpy.ops.object.mode_set(mode="OBJECT")
    return arm_obj, bone_names


def assign_weights(obj, joints, bone_names, pool_weights):
    groups = {}
    for name in bone_names:
        if name not in groups:
            groups[name] = obj.vertex_groups.new(name=name)
    for vi, weights in enumerate(pool_weights):
        for joint, weight in weights:
            if weight == 0.0 or not (0 <= joint < len(joints)):
                continue
            groups[bone_names[joint]].add([vi], weight, "REPLACE")


def build_animations(arm_obj, data, bone_names):
    joints = data["joints"]
    # The global and parent factors cancel between the rest and posed relative matrices,
    # so each bone's local pose basis is just rest_relative^-1 @ pose_relative.
    rest_rel_inv = [sqt_to_local(j["s"], j["q"], j["t"]).inverted() for j in joints]

    bpy.context.view_layer.objects.active = arm_obj
    bpy.ops.object.mode_set(mode="POSE")
    if arm_obj.animation_data is None:
        arm_obj.animation_data_create()

    for anim in data["anims"]:
        action = bpy.data.actions.new(anim["name"])
        arm_obj.animation_data.action = action
        for frame_idx, pose in enumerate(anim["frames"]):
            frame_no = frame_idx + 1
            for i in range(len(joints)):
                s, q, t = pose[i + 1]
                pbone = arm_obj.pose.bones[bone_names[i]]
                pbone.matrix_basis = rest_rel_inv[i] @ sqt_to_local(s, q, t)
                pbone.keyframe_insert("location", frame=frame_no)
                pbone.keyframe_insert("rotation_quaternion", frame=frame_no)
                pbone.keyframe_insert("scale", frame=frame_no)
    bpy.ops.object.mode_set(mode="OBJECT")


def load(operator, context, filepath, global_matrix):
    name       = os.path.splitext(os.path.basename(filepath))[0]
    model_dir  = os.path.dirname(filepath)
    inv_global = global_matrix.inverted()

    if context.mode != "OBJECT":
        bpy.ops.object.mode_set(mode="OBJECT")

    with open(filepath, "r", encoding="ascii", errors="replace") as f:
        text = f.read()
    try:
        data = parse(text)
    except Exception as exc:
        if operator is not None:
            operator.report({"ERROR"}, "Failed to parse PFOBJ: %s" % exc)
        return {"CANCELLED"}

    collection = context.collection

    # A rig authored at a non-unit object scale (e.g. a Mixamo 0.01 import) bakes that
    # uniform scale into every bone, but edit bones cannot store scale. Factor it out onto
    # the armature object: build bones and mesh at unit-bone scale, then scale the object
    # back down. The pose deltas are scale-free, so the animation is unaffected.
    obj_scale = 1.0
    joints = data["joints"]
    if joints:
        root_rest = inv_global @ sqt_to_local(joints[0]["s"], joints[0]["q"], joints[0]["t"])
        sc = root_rest.to_scale()
        mag = (abs(sc[0]) + abs(sc[1]) + abs(sc[2])) / 3.0
        if mag > 1e-9 and not (0.99 < mag < 1.01):
            obj_scale = mag
    scale = 1.0 / obj_scale

    mesh, face_material, pool_weights = build_mesh(data, name, inv_global, scale)
    build_materials(mesh, data["materials"], model_dir)
    for poly, material in zip(mesh.polygons, face_material):
        if material < len(mesh.materials):
            poly.material_index = material
    mesh.update()

    obj = bpy.data.objects.new(name, mesh)
    collection.objects.link(obj)

    if data["num_joints"] > 0:
        arm_obj, bone_names = build_armature(data, name, inv_global, scale, collection)
        arm_obj.scale = (obj_scale, obj_scale, obj_scale)
        assign_weights(obj, data["joints"], bone_names, pool_weights)
        modifier = obj.modifiers.new("Armature", "ARMATURE")
        modifier.object = arm_obj
        obj.parent = arm_obj
        obj.matrix_parent_inverse = Matrix.Identity(4)
        if data["anims"]:
            build_animations(arm_obj, data, bone_names)

    for selected in list(context.selected_objects):
        selected.select_set(False)
    obj.select_set(True)
    context.view_layer.objects.active = obj
    return {"FINISHED"}
