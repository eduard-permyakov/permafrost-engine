#
#  This file is part of Permafrost Engine. 
#  Copyright (C) 2017-2026 Eduard Permyakov 
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
#  Targets Blender 2.80 and later.
#
import bpy
import bmesh
from mathutils import Matrix, Vector

PFOBJ_VER = 1.0

def mesh_triangulate(mesh):
    bm = bmesh.new()
    bm.from_mesh(mesh)
    bmesh.ops.triangulate(bm, faces=bm.faces)
    bm.to_mesh(mesh)
    bm.free()

def material_texture_name(mat):
    # The texture is the image feeding the Principled Base Color, or failing that any
    # Image Texture node in the material.
    if not mat.use_nodes or mat.node_tree is None:
        return None
    nodes = mat.node_tree.nodes
    bsdf = next((n for n in nodes if n.type == 'BSDF_PRINCIPLED'), None)
    if bsdf is not None:
        base = bsdf.inputs.get("Base Color")
        if base is not None and base.is_linked:
            src = base.links[0].from_node
            if src.type == 'TEX_IMAGE' and src.image is not None:
                return src.image.name
    for node in nodes:
        if node.type == 'TEX_IMAGE' and node.image is not None:
            return node.image.name
    return None

def material_phong(mat):
    # Ambient has no node-based equivalent and stays constant; the diffuse and specular
    # colours come from the Principled BSDF inputs.
    diffuse = (0.8, 0.8, 0.8)
    specular = (0.5, 0.5, 0.5)
    if mat.use_nodes and mat.node_tree is not None:
        bsdf = next((n for n in mat.node_tree.nodes if n.type == 'BSDF_PRINCIPLED'), None)
        if bsdf is not None:
            base = bsdf.inputs.get("Base Color")
            if base is not None:
                diffuse = tuple(base.default_value[:3])
            # The input is "Specular IOR Level" on newer Blender, "Specular" on older.
            spec = bsdf.inputs.get("Specular IOR Level") or bsdf.inputs.get("Specular")
            if spec is not None:
                value = spec.default_value
                specular = (value, value, value)
    return 1.0, diffuse, specular

def face_tex_index(mesh, face, tex_names):
    # Index into the textured-material list, or None for a face whose material is missing
    # or untextured. The engine requires every material to carry a texture, so faces
    # without one are skipped.
    mi = face.material_index
    mat = mesh.materials[mi] if (mi < len(mesh.materials) and mesh.materials[mi] is not None) else None
    if mat is not None and mat.name in tex_names:
        return tex_names.index(mat.name)
    return None

def current_pose_bbox(global_matrix, mesh_objs, local_origin):
    # Encapsulate all selected objects in one bounding box. A multi-object entity must be
    # exported together; separate entities are exported one at a time.
    bbox_verts = []
    for obj in mesh_objs:
        ws_mat = Matrix.Identity(4) if local_origin else obj.matrix_world
        bbox_verts += [global_matrix @ ws_mat @ Vector(b) for b in obj.bound_box]

    min_x = min(p.x for p in bbox_verts)
    max_x = max(p.x for p in bbox_verts)
    min_y = min(p.y for p in bbox_verts)
    max_y = max(p.y for p in bbox_verts)
    min_z = min(p.z for p in bbox_verts)
    max_z = max(p.z for p in bbox_verts)

    return min_x, max_x, min_y, max_y, min_z, max_z

def joint_local_matrix(global_matrix, ws_mat, this_mat, parent_mat):
    # Root joints are stored in world space, child joints relative to their parent.
    # Relative transforms are invariant under global_matrix, so it only touches the root.
    if parent_mat is not None:
        return parent_mat.inverted() @ this_mat
    return global_matrix @ ws_mat @ this_mat

def decompose_sqt(mat):
    # The engine builds a rotation matrix from the transpose of the quaternion, so invert
    # it here to round-trip the rotation. A reflected matrix yields a signed scale, which
    # the engine reconstructs faithfully.
    return mat.to_scale(), mat.to_quaternion().inverted(), mat.to_translation()

def save(operator, context, filepath, global_matrix, export_bbox, local_origin, idle_clip=None):
    with open(filepath, "w", encoding="ascii") as ofile:

        mesh_objs = [obj for obj in bpy.context.selected_objects if obj.type == 'MESH']
        meshes = [obj.data for obj in mesh_objs]
        arms   = [obj for obj in bpy.context.selected_objects if obj.type == 'ARMATURE']

        seen_names = set()
        textured_mats = []
        for mesh in meshes:
            for mat in mesh.materials:
                if mat is None or material_texture_name(mat) is None:
                    continue
                if mat.name not in seen_names:
                    textured_mats.append(mat)
                    seen_names.add(mat.name)

        for mesh in meshes:
            mesh_triangulate(mesh)

        tex_names = [mat.name for mat in textured_mats]

        # Count only the corners of textured faces, matching the geometry written below.
        num_verts = 0
        skipped_faces = 0
        for obj in mesh_objs:
            for face in obj.data.polygons:
                if face_tex_index(obj.data, face, tex_names) is None:
                    skipped_faces += 1
                else:
                    num_verts += face.loop_total

        num_joints    = sum([len(arm.pose.bones) for arm in arms])
        num_materials = len(textured_mats)

        # With no armature there are no animation sets. A freshly-added entity defaults to
        # clip index 0, so the requested idle clip is emitted first and the rest by name.
        actions_ordered = sorted(bpy.data.actions, key=lambda a: (a.name != idle_clip, a.name)) if len(arms) > 0 else []
        num_as        = len(actions_ordered)
        frame_counts  = [str(int(a.frame_range[1] - a.frame_range[0] + 1)) for a in actions_ordered]

        if num_materials == 0:
            if operator is not None:
                operator.report({'ERROR'}, "No materials with an Image Texture node were found.")
            return {'CANCELLED'}

        if skipped_faces > 0 and operator is not None:
            operator.report({'WARNING'}, "Skipped %d face(s) with no textured material." % skipped_faces)

        #####################################################################
        # Write header
        #####################################################################

        ofile.write("version        " + str(PFOBJ_VER) + "\n")
        ofile.write("num_verts      " + str(num_verts) + "\n")
        ofile.write("num_joints     " + str(num_joints) + "\n")
        ofile.write("num_materials  " + str(num_materials) + "\n")
        ofile.write("num_as         " + str(num_as) + "\n")
        ofile.write("frame_counts   " + " ".join(frame_counts) + "\n")
        ofile.write("has_collision  " + str(1 if export_bbox is True else 0) + "\n")

        #####################################################################
        # Write vertices and their attributes
        #####################################################################

        # A negative-determinant global matrix mirrors the mesh, so reverse the winding to
        # keep front faces front-facing under backface culling.
        winding_flip = global_matrix.determinant() < 0.0

        for obj in mesh_objs:
            mesh = obj.data

            ws_mat = Matrix.Identity(4) if local_origin else obj.matrix_world
            trans = global_matrix @ ws_mat
            # Normals use the inverse-transpose of the linear part so rotations, scales
            # and the reflection are applied correctly.
            normal_mat = trans.to_3x3().inverted().transposed()

            for face in mesh.polygons:
                vm = face_tex_index(mesh, face, tex_names)
                if vm is None:
                    continue
                loop_order = reversed(face.loop_indices) if winding_flip else face.loop_indices
                for loop_idx in loop_order:

                    v = mesh.vertices[mesh.loops[loop_idx].vertex_index]
                    v_co_world = trans @ v.co
                    ofile.write("v {0:.6f} {1:.6f} {2:.6f}\n".format(v_co_world.x, v_co_world.y, v_co_world.z))

                    uv_coords = mesh.uv_layers.active.data[loop_idx].uv
                    ofile.write("vt {0:.6f} {1:.6f}\n".format(uv_coords.x, uv_coords.y))

                    normal = normal_mat @ v.normal
                    normal.normalize()
                    ofile.write("vn {0:.6f} {1:.6f} {2:.6f}\n".format(normal[0], normal[1], normal[2]))

                    joint_idx_weight_map = {}
                    if len(arms) > 0:
                        for vg in v.groups:

                            # Skip zero weights and stale group indices; static meshes
                            # often keep leftover vertex groups with no matching bone.
                            if vg.weight == 0 or vg.group >= len(obj.vertex_groups):
                                continue

                            bone_name = obj.vertex_groups[vg.group].name
                            if bone_name not in arms[0].data.bones.keys():
                                continue

                            joint = arms[0].data.bones[bone_name]
                            joint_idx = arms[0].data.bones.values().index(joint)

                            joint_idx_weight_map[joint_idx] = vg.weight

                    # Order joints by weight; the engine keeps only the top 6.
                    line = "vw "
                    for joint_idx, weight in sorted(joint_idx_weight_map.items(), key=lambda kv: kv[1], reverse=True):
                        line += " {0}/{1:.6f}".format(joint_idx, weight)

                    line += "\n"
                    ofile.write(line)

                    ofile.write("vm {0}\n".format(vm))

        #####################################################################
        # Write materials
        #####################################################################

        for material in textured_mats:

            ambient, diffuse, specular = material_phong(material)

            ofile.write("material " + material.name + "\n")
            ofile.write("    ambient {0:.6f}\n".format(ambient))
            ofile.write("    diffuse {0:.6f} {1:.6f} {2:.6f}\n".format(diffuse[0], diffuse[1], diffuse[2]))
            ofile.write("    specular {0:.6f} {1:.6f} {2:.6f}\n".format(specular[0], specular[1], specular[2]))
            ofile.write("    texture " + material_texture_name(material) + "\n")

        #####################################################################
        # Write joints
        #####################################################################

        for obj in arms:
            arm = obj.data
            ws_mat = Matrix.Identity(4) if local_origin else obj.matrix_world

            for bone in arm.bones:

                parent_idx = arm.bones.values().index(bone.parent) + 1 if bone.parent is not None else 0
                parent_mat = bone.parent.matrix_local if bone.parent is not None else None

                mat_final = joint_local_matrix(global_matrix, ws_mat, bone.matrix_local, parent_mat)
                s, q, t = decompose_sqt(mat_final)
                tip = bone.matrix_local.inverted() @ bone.tail_local

                ofile.write("j {0} {1} {2:.6f}/{3:.6f}/{4:.6f} {5:.6f}/{6:.6f}/{7:.6f}/{8:.6f} "
                            "{9:.6f}/{10:.6f}/{11:.6f} {12:.6f}/{13:.6f}/{14:.6f}\n".format(
                    parent_idx, bone.name, s.x, s.y, s.z, q.x, q.y, q.z, q.w,
                    t.x, t.y, t.z, tip.x, tip.y, tip.z))

        #####################################################################
        # Write animation sets
        #####################################################################

        for action in actions_ordered:

            if len(arms) == 0:
                break

            frame_cnt = action.frame_range[1] - action.frame_range[0] + 1
            ofile.write("as " + action.name + " " + str(int(frame_cnt)) + "\n")

            for obj in arms:
                obj.animation_data.action = action
                # Slotted actions need an explicit slot binding before they evaluate.
                if hasattr(action, "slots") and len(action.slots) > 0:
                    obj.animation_data.action_slot = action.slots[0]

            for f in range(int(action.frame_range[0]), int(action.frame_range[1]+1)):

                bpy.context.scene.frame_set(f)

                obj = arms[0]
                ws_mat = Matrix.Identity(4) if local_origin else obj.matrix_world

                for bone in obj.data.bones:
                    pbone = obj.pose.bones[bone.name]
                    idx = obj.data.bones.values().index(bone) + 1

                    parent_mat = pbone.parent.matrix if bone.parent is not None else None
                    mat_final = joint_local_matrix(global_matrix, ws_mat, pbone.matrix, parent_mat)
                    s, q, t = decompose_sqt(mat_final)

                    ofile.write("        {0} {1:.6f}/{2:.6f}/{3:.6f} {4:.6f}/{5:.6f}/{6:.6f}/{7:.6f} "
                                "{8:.6f}/{9:.6f}/{10:.6f}\n".format(
                        idx, s.x, s.y, s.z, q.x, q.y, q.z, q.w, t.x, t.y, t.z))

                if not export_bbox:
                    continue

                min_x, max_x, min_y, max_y, min_z, max_z = current_pose_bbox(global_matrix, mesh_objs, local_origin)
                ofile.write("\tx_bounds {0:.6f} {1:.6f}\n".format(min_x, max_x))
                ofile.write("\ty_bounds {0:.6f} {1:.6f}\n".format(min_y, max_y))
                ofile.write("\tz_bounds {0:.6f} {1:.6f}\n".format(min_z, max_z))

        #####################################################################
        # Write bounding box data
        #####################################################################

        bpy.ops.object.mode_set(mode='EDIT')
        min_x, max_x, min_y, max_y, min_z, max_z = current_pose_bbox(global_matrix, mesh_objs, local_origin)
        bpy.ops.object.mode_set(mode='OBJECT')

        ofile.write("x_bounds {0:.6f} {1:.6f}\n".format(min_x, max_x))
        ofile.write("y_bounds {0:.6f} {1:.6f}\n".format(min_y, max_y))
        ofile.write("z_bounds {0:.6f} {1:.6f}\n".format(min_z, max_z))

        return {'FINISHED'}
