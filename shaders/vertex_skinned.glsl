#version 330 core

#define MAX_JOINTS 128

layout (location = 0) in vec3 in_pos;
layout (location = 1) in vec2 in_uv;
layout (location = 2) in vec3 in_normal;
layout (location = 3) in int  in_material_idx;
layout (location = 4) in vec4 in_joint_indices;
layout (location = 5) in vec4 in_joint_weights;

/*****************************************************************************/
/* OUTPUTS                                                                   */
/*****************************************************************************/

out VertexToFrag {
         vec2 uv;
    flat int  mat_idx;
}to_fragment;

out VertexToGeo {
    vec3 normal;
}to_geometry;

/*****************************************************************************/
/* UNIFORMS                                                                  */
/*****************************************************************************/

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

uniform mat4 anim_curr_pose_mats[MAX_JOINTS];
uniform mat4 anim_inv_bind_mats [MAX_JOINTS];

/*****************************************************************************/
/* HELPER FUNCTIONS 
/*****************************************************************************/

vec4 quaternion_mult(vec4 a, vec4 b)
{
    /* Algorithm from: 
     * http://www.euclideanspace.com/maths/algebra/realNormedAlgebra/quaternions/arithmetic/ 
     */
    float sa = a.w;
    vec3  va = a.xyz;
    
    float sb = b.w;
    vec3  vb = b.xyz;

    vec3  rv = cross(va, vb) + (sa * vb) + (sb * va);
    float rs = (sa * sb) - dot(va,vb);
    return vec4( rv, rs );
}

vec4 quaternion_inverse(vec4 quat)
{
    return vec4(-quat.xyz, quat.w)/length(quat);
}

/*****************************************************************************/
/* PROGRAM
/*****************************************************************************/

void main()
{
    to_fragment.uv = in_uv;
    to_fragment.mat_idx = in_material_idx;

    /* TODO: compute normal matrix on CPU once per model each frame and pass as uniform 
     */
    mat3 normal_matrix = mat3(transpose(inverse(view * model)));

    float tot_weight = in_joint_weights[0] + in_joint_weights[1]
                     + in_joint_weights[2] + in_joint_weights[3];

    /* If all weights are 0, treat this vertex as a static one.
     * Non-animated vertices will have their weights explicitly zeroed out. 
     */
    if(tot_weight == 0.0) {

        to_geometry.normal = normalize(vec3(projection * vec4(normal_matrix * in_normal, 1.0)));
        gl_Position = projection * view * model * vec4(in_pos, 1.0);

    }else {

        vec3 new_pos =  vec3(0.0, 0.0, 0.0);
        vec3 new_normal = vec3(0.0, 0.0, 0.0);

        for(int w_idx = 0; w_idx < 4; w_idx++) {

            int joint_idx = int(in_joint_indices[w_idx]);

            mat4 inv_bind_mat = anim_inv_bind_mats [joint_idx];
            mat4 pose_mat     = anim_curr_pose_mats[joint_idx];

            float fraction = in_joint_weights[w_idx] / tot_weight;

            mat4 bone_mat = fraction * pose_mat * inv_bind_mat;
            mat3 rot_mat = fraction * mat3(transpose(inverse(pose_mat * inv_bind_mat)));
            
            new_pos += (bone_mat * vec4(in_pos, 1.0)).xyz;
            new_normal += rot_mat * in_normal;
        }

        to_geometry.normal = normalize(vec3(projection * vec4(normal_matrix * new_normal, 1.0)));
        gl_Position = projection * view * model * vec4(new_pos, 1.0f);

    }
}

