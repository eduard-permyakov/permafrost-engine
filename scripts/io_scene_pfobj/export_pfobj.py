#
#  This file is part of Permafrost Engine. 
#  Copyright (C) 2017-2020 Eduard Permyakov 
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
import bpy
import math
from bpy import context
from mathutils import Matrix
from mathutils import Quaternion
from mathutils import Euler
from mathutils import Vector

PFOBJ_VER = 1.0

def mesh_triangulate(mesh):
    import bmesh
    bm = bmesh.new()
    bm.from_mesh(mesh)
    bmesh.ops.triangulate(bm, faces=bm.faces)
    bm.to_mesh(mesh)
    bm.free()

def current_pose_bbox(global_matrix, mesh_objs, local_origin):
    # We build a single bounding box encapsulating all selected objects - this only works
    # if a single entity is composed of multiple objects - otherwise they must be 
    # exported one at a time
    bbox_verts = []
    for obj in mesh_objs:
        ws_mat = Matrix.Identity(4) if local_origin else obj.matrix_world
        bbox_verts += [global_matrix * ws_mat * Vector(b) for b in obj.bound_box]

    min_x = min(p.x for p in bbox_verts)
    max_x = max(p.x for p in bbox_verts)
    min_y = min(p.y for p in bbox_verts)
    max_y = max(p.y for p in bbox_verts)
    min_z = min(p.z for p in bbox_verts)
    max_z = max(p.z for p in bbox_verts)

    return min_x, max_x, min_y, max_y, min_z, max_z

def save(operator, context, filepath, global_matrix, export_bbox, local_origin):
    with open(filepath, "w", encoding="ascii") as ofile:

        mesh_objs = [obj for obj in bpy.context.selected_objects if obj.type == 'MESH']
        meshes = [obj.data for obj in mesh_objs]
        arms   = [obj for obj in bpy.context.selected_objects if obj.type == 'ARMATURE']

        textured_mats = []
        for mesh in meshes:
            for mat in mesh.materials:
                if mat.active_texture:
                    textured_mats.append(mat)

        for mesh in meshes:
            mesh_triangulate(mesh)
            mesh.calc_normals_split()

        num_verts = 0
        for mesh in meshes: 
            num_verts += sum([face.loop_total for face in mesh.polygons])

        num_joints    = sum([len(arm.pose.bones) for arm in arms])
        num_as        = len(bpy.data.actions.items())
        num_materials = len(textured_mats)
        frame_counts  = [str(int(a.frame_range[1] - a.frame_range[0] + 1)) for a in bpy.data.actions]

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

        for obj in mesh_objs:
            mesh = obj.data

            for face in mesh.polygons:
                for loop_idx in face.loop_indices:

                    ws_mat = Matrix.Identity(4) if local_origin else obj.matrix_world
                    trans = global_matrix * ws_mat

                    v = mesh.vertices[mesh.loops[loop_idx].vertex_index]
                    v_co_world = trans * v.co

                    line = "v {v.x:.6f} {v.y:.6f} {v.z:.6f}\n"
                    line = line.format(v=v_co_world)
                    ofile.write(line)

                    uv_coords = mesh.uv_layers.active.data[loop_idx].uv
                    line = "vt {uv.x:.6f} {uv.y:.6f}\n"
                    line = line.format(uv=uv_coords)
                    ofile.write(line)

                    # The following line will give per-face normals instead
                    # Make it an option at some point ...
                    #normal = global_matrix * mesh.loops[loop_idx].normal
                    normal = global_matrix * v.normal
                    line = "vn {n[0]:.6f} {n[1]:.6f} {n[2]:.6f}\n"
                    line = line.format(n=normal)
                    ofile.write(line)

                    line = "vw ";
                    joint_idx_weight_map = {}
                    for vg in v.groups:

                        if vg.weight == 0:
                            continue

                        bone_name = obj.vertex_groups[vg.group].name
                        if bone_name not in arms[0].data.bones.keys():
                            continue

                        joint = arms[0].data.bones[bone_name]
                        joint_idx = arms[0].data.bones.values().index(joint)

                        joint_idx_weight_map[joint_idx] = vg.weight

                    # Write the joints ordered by weight - we only use the top 6 highest weights
                    # in the engine
                    from operator import itemgetter
                    for tuple in sorted(joint_idx_weight_map.items(), key=itemgetter(1), reverse=True):
                        next_elem = " {g}/{w:.6f}"
                        next_elem = next_elem.format(g=tuple[0], w=tuple[1])
                        line += next_elem

                    line += "\n"
                    ofile.write(line)

                    mat_idx = textured_mats.index( mesh.materials[face.material_index] )
                    line = "vm {idx}\n"
                    line = line.format(idx=mat_idx)
                    ofile.write(line)

        #####################################################################
        # Write materials 
        #####################################################################

        for material in textured_mats:

            ofile.write("material " + material.name + "\n")

            line = "    ambient {a:.6f}\n"
            line = line.format(a=material.ambient)
            ofile.write(line)

            line = "    diffuse {c[0]:.6f} {c[1]:.6f} {c[2]:.6f}\n"
            line = line.format(c=(material.diffuse_intensity * material.diffuse_color))
            ofile.write(line)

            line = "    specular {c[0]:.6f} {c[1]:.6f} {c[2]:.6f}\n"
            line = line.format(c=(material.specular_intensity * material.specular_color))
            ofile.write(line)

            line = "    texture " + material.active_texture.image.name + "\n"
            ofile.write(line)

        #####################################################################
        # Write joints
        #####################################################################

        for obj in arms:
            arm = obj.data
            ws_mat = Matrix.Identity(4) if local_origin else obj.matrix_world

            # write Joints
            for bone in arm.bones:

                line = "j {parent_idx} {name}" 
                parent_idx = arm.bones.values().index(bone.parent) + 1 if bone.parent is not None else 0
                line = line.format(parent_idx=parent_idx, name=bone.name)

                # Root bones are given in world coordinates - the rest of the bones'
                # positions are given relative to the parent
                if bone.parent is not None:
                    mat_final = bone.parent.matrix_local.inverted() * bone.matrix_local
                else:
                    mat_final = global_matrix * ws_mat * bone.matrix_local

                line += " {s.x:.6f}/{s.y:.6f}/{s.z:.6f}"
                line = line.format(s=mat_final.to_scale())

                # Unsure why we have to invert the quaternion here...
                # If we convert the quaternion to a matrix using standard algorithms in the engine
                # we get the transpose of the matrix we were supposed to get 
                line += " {q.x:.6f}/{q.y:.6f}/{q.z:.6f}/{q.w:.6f}"
                line = line.format(q=mat_final.to_quaternion().inverted())

                line += " {t.x:.6f}/{t.y:.6f}/{t.z:.6f}"
                line = line.format(t=mat_final.to_translation())

                tip = bone.matrix_local.inverted() * bone.tail_local
                line += " {v.x:.6f}/{v.y:.6f}/{v.z:.6f}"
                line = line.format(v=tip)

                line += "\n"
                ofile.write(line)

        #####################################################################
        # Write animation sets
        #####################################################################

        for action in bpy.data.actions:

            if len(arms) == 0:
                break

            frame_cnt = action.frame_range[1] - action.frame_range[0] + 1;
            ofile.write("as " + action.name + " " + str(int(frame_cnt)) + "\n")

            # Set the current action
            for obj in arms:
                obj.animation_data.action = action

            for f in range(int(action.frame_range[0]), int(action.frame_range[1]+1)):

                bpy.context.scene.frame_set(f)

                obj = arms[0]
                ws_mat = Matrix.Identity(4) if local_origin else obj.matrix_world

                for bone in obj.data.bones:
                    pbone = obj.pose.bones[bone.name]

                    line = "        {idx} {s.x:.6f}/{s.y:.6f}/{s.z:.6f} {q.x:.6f}/{q.y:.6f}/{q.z:.6f}/{q.w:.6f} {t.x:.6f}/{t.y:.6f}/{t.z:.6f}\n"
                    idx = obj.data.bones.values().index(bone) + 1

                    # Root bones are given in world coordinates - the rest of the bones'
                    # positions are given relative to the parent
                    if bone.parent is not None:
                        mat_final = pbone.parent.matrix.inverted() * pbone.matrix
                    else:
                        mat_final = global_matrix * ws_mat * pbone.matrix

                    line = line.format(idx=idx, s=mat_final.to_scale(), q=mat_final.to_quaternion().inverted(), t=mat_final.to_translation())

                    ofile.write(line)

                if not export_bbox:
                    continue

                # Write bbox for current animation frame
                min_x, max_x, min_y, max_y, min_z, max_z = current_pose_bbox(global_matrix, mesh_objs, local_origin)
                line = "\tx_bounds {a:.6f} {b:.6f}\n".format(a=min_x, b=max_x)
                ofile.write(line)
                line = "\ty_bounds {a:.6f} {b:.6f}\n".format(a=min_y, b=max_y)
                ofile.write(line)
                line = "\tz_bounds {a:.6f} {b:.6f}\n".format(a=min_z, b=max_z)
                ofile.write(line)

        #####################################################################
        # Write bounding box data
        #####################################################################

        bpy.ops.object.mode_set(mode='EDIT')
        min_x, max_x, min_y, max_y, min_z, max_z = current_pose_bbox(global_matrix, mesh_objs, local_origin)
        bpy.ops.object.mode_set(mode='OBJECT')

        line = "x_bounds {a:.6f} {b:.6f}\n".format(a=min_x, b=max_x)
        ofile.write(line)
        line = "y_bounds {a:.6f} {b:.6f}\n".format(a=min_y, b=max_y)
        ofile.write(line)
        line = "z_bounds {a:.6f} {b:.6f}\n".format(a=min_z, b=max_z)
        ofile.write(line)

        return {'FINISHED'}

