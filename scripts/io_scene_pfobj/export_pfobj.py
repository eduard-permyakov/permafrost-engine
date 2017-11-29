import bpy
import math
from bpy import context
from mathutils import Matrix
from mathutils import Quaternion
from mathutils import Euler

PFOBJ_VER = 1.0

def mesh_triangulate(mesh):
    import bmesh
    bm = bmesh.new()
    bm.from_mesh(mesh)
    bmesh.ops.triangulate(bm, faces=bm.faces)
    bm.to_mesh(mesh)
    bm.free()

def save(operator, context, filepath, global_matrix):
    with open(filepath, "w", encoding="ascii") as ofile:

        # Write the header
        mesh_objs = [obj for obj in bpy.context.selected_objects if obj.type == 'MESH']
        meshes = [obj.data for obj in mesh_objs]
        arms   = [obj for obj in bpy.context.selected_objects if obj.type == 'ARMATURE']

        for mesh in meshes:
            mesh_triangulate(mesh)

        num_verts = 0
        for mesh in meshes: 
            num_verts += sum([face.loop_total for face in mesh.polygons])

        num_joints   = sum([len(arm.pose.bones) for arm in arms])
        num_as       = len(bpy.data.actions.items())
        num_faces    = sum([len(mesh.polygons.items()) for mesh in meshes])
        frame_counts = [str(int(a.frame_range[1] - a.frame_range[0] + 1)) for a in bpy.data.actions]

        ofile.write("version        " + str(PFOBJ_VER) + "\n")
        ofile.write("num_verts      " + str(num_verts) + "\n")
        ofile.write("num_joints     " + str(num_joints) + "\n")
        ofile.write("num_faces      " + str(num_faces) + "\n")
        ofile.write("num_as         " + str(num_as) + "\n")
        ofile.write("frame_counts   " + " ".join(frame_counts) + "\n")

        # Iterate over meshes 
        for obj in mesh_objs:
            mesh = obj.data

            # write Vertices
            for face in mesh.polygons:
                for loop_idx in face.loop_indices:

                    #trans = obj.matrix_local if obj.parent is None else obj.parent.matrix_local * obj.matrix_local 
                    #trans = global_matrix * trans
                    trans = global_matrix * obj.matrix_world

                    v = mesh.vertices[mesh.loops[loop_idx].vertex_index]
                    v_co_world = trans * v.co

                    line = "v {v.x:.6f} {v.y:.6f} {v.z:.6f}\n"
                    line = line.format(v=v_co_world)
                    ofile.write(line)

                    uv_coords = mesh.uv_layers.active.data[loop_idx].uv
                    line = "vt {uv.x:.6f} {uv.y:.6f}\n"
                    line = line.format(uv=uv_coords)
                    ofile.write(line)

                    line = "vw ";
                    for vg in v.groups:
                        next_elem = " {g}/{w}"
                        next_elem = next_elem.format(g=vg.group, w=vg.weight)
                        line += next_elem
                    line += "\n"
                    ofile.write(line)

        #TODO: remove faces from export script and engine - they are not used
        for obj in mesh_objs:
            mesh = obj.data

            # write Faces
            for face in mesh.polygons:
                line = "f"
                for loop_idx in face.loop_indices:
                    next_elem = " {idx}"
                    # PFOBJ indices start at 1 like in the classic Waveform OBJ format
                    next_elem = next_elem.format(idx=loop_idx+1)
                    line += next_elem
                line += "\n"
                ofile.write(line)


        # Iterate over armatures 
        for obj in arms:
            arm = obj.data

            # write Joints
            for bone in arm.bones:

                line = "j {parent_idx} {name}" 
                parent_idx = arm.bones.values().index(bone.parent) + 1 if bone.parent is not None else 0
                line = line.format(parent_idx=parent_idx, name=bone.name)

                identity = Matrix.Identity(4)
                mat_final = obj.matrix_world * global_matrix if bone.parent is None else identity

                euler = mat_final.to_euler('XYZ')
                euler[0] = math.degrees(euler[0])
                euler[1] = math.degrees(euler[1])
                euler[2] = math.degrees(euler[2])

                line += " {s.x:.6f}/{s.y:.6f}/{s.z:.6f}"
                line = line.format(s=mat_final.to_scale())

                line += " {e[0]:.6f}/{e[1]:.6f}/{e[2]:.6f}"
                line = line.format(e=euler)

                # Root bones are given in world coordinates - the rest of the bones'
                # positions are given relative to the parent
                loc = obj.matrix_world * bone.head_local if bone.parent is None else bone.head_local - bone.parent.head_local
                line += " {t.x:.6f}/{t.y:.6f}/{t.z:.6f}"
                line = line.format(t=loc)

                tip = (bone.tail_local - bone.head_local)
                line += " {v.x:.6f}/{v.y:.6f}/{v.z:.6f}"
                line = line.format(v=tip)

                line += "\n"
                ofile.write(line)

        # write Animation sets
        for action in bpy.data.actions:

            frame_cnt = action.frame_range[1] - action.frame_range[0] + 1;
            ofile.write("as " + action.name + " " + str(int(frame_cnt)) + "\n")

            # Set the current action
            for obj in arms:
                obj.animation_data.action = action

            for f in range(int(action.frame_range[0]), int(action.frame_range[1]+1)):

                bpy.context.scene.frame_set(f)

                obj = arms[0]
                for pbone in obj.pose.bones:

                    line = "        {idx} {s.x:.6f}/{s.y:.6f}/{s.z:.6f} {e[0]:.6f}/{e[1]:.6f}/{e[2]:.6f} {t.x:.6f}/{t.y:.6f}/{t.z:.6f}\n"
                    idx = obj.pose.bones.values().index(pbone) + 1

                    trans = Matrix.Translation(pbone.bone.head_local)
                    itrans = Matrix.Translation(-pbone.bone.head_local)

                    # The following code for builing the local transformation matrix for the current frame was derived from the 'BVH' exporter script
                    if  pbone.parent:
                        mat_final = pbone.parent.bone.matrix_local * pbone.parent.matrix.inverted() * pbone.matrix * pbone.bone.matrix_local.inverted()
                        mat_final = itrans * mat_final * trans
                    else:
                        mat_final = pbone.matrix * pbone.bone.matrix_local.inverted()
                        mat_final = itrans * mat_final * trans

                    bone_matrix = mat_final
                    euler = bone_matrix.to_euler('XYZ')
                    euler[0] = math.degrees(euler[0])
                    euler[1] = math.degrees(euler[1])
                    euler[2] = math.degrees(euler[2])

                    line = line.format(idx=idx, s=bone_matrix.to_scale(), e=euler, t=bone_matrix.to_translation())
                    ofile.write(line)

    return {'FINISHED'}

