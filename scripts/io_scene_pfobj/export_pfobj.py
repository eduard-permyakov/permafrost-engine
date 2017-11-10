import bpy
from bpy import context
from mathutils import Matrix

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

        # Transform all objects to OpenGL coordinate system
        #for obj in bpy.data.objects:
        #    obj.matrix_world = global_matrix * obj.matrix_world

        # Write the header
        meshes = [obj.data for obj in bpy.data.objects if obj.type == 'MESH']
        arms   = [obj for obj in bpy.data.objects if obj.type == 'ARMATURE']

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
        for mesh in meshes:

            mesh.transform(global_matrix)

            # write Vertices
            for face in mesh.polygons:
                for loop_idx in face.loop_indices:

                    line = "v {v.x:.6f} {v.y:.6f} {v.z:.6f}\n"
                    v = mesh.vertices[mesh.loops[loop_idx].vertex_index]
                    line = line.format(v=v.co)
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

            # write Faces
            for face in mesh.polygons:
                line = "f"
                for loop_idx in face.loop_indices:
                    next_elem = " {idx}"
                    # PFOBJ indices start at 1 linke in the classic Waveform OBJ format
                    next_elem = next_elem.format(idx=loop_idx+1)
                    line += next_elem
                line += "\n"
                ofile.write(line)

            mesh.transform(global_matrix.inverted())

        # Iterate over armatures 
        for obj in arms:
            arm = obj.data

            # write Joints
            for bone in arm.bones:

                line = "j {parent_idx} {name}" 
                parent_idx = arm.bones.values().index(bone.parent) + 1 if bone.parent is not None else 0
                line = line.format(parent_idx=parent_idx, name=bone.name)

                for c in range(0,4):
                    gl_matrix = global_matrix * bone.matrix_local
                    line += " {r1:.6f}/{r2:.6f}/{r3:.6f}/{r4:.6f}"
                    line = line.format(r1=gl_matrix[0][c],
                                       r2=gl_matrix[1][c],
                                       r3=gl_matrix[2][c],
                                       r4=gl_matrix[3][c])

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

                obj = [obj for obj in bpy.data.objects if obj.pose is not None][0]
                for pbone in obj.pose.bones:
                    line = "        {idx} {s.x:.6f}/{s.y:.6f}/{s.z:.6f} {q.x:.6f}/{q.y:.6f}/{q.z:.6f}/{q.w:.6f} {t.x:.6f}/{t.y:.6f}/{t.z:.6f}\n"

                    idx = obj.pose.bones.values().index(pbone) + 1

                    line = line.format(idx=idx, s=pbone.scale, q=pbone.rotation_quaternion, t=pbone.location)
                    ofile.write(line)

        # Now transfrom all objects back to Blender coordinate system
        #for obj in bpy.data.objects:
        #    obj.matrix_world = global_matrix.inverted() * obj.matrix_world

    return {'FINISHED'}
