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
uniform vec4 anim_curr_pose_rot_quats[MAX_JOINTS];

/*****************************************************************************/
/* PROGRAM                                                                   */
/*****************************************************************************/

void main()
{
    to_fragment.uv = in_uv;
    to_fragment.mat_idx = in_material_idx;

    /* TODO: compute normal matrix on CPU once per model each frame and pass as uniform 
     */
    mat3 normal_matrix = mat3(transpose(inverse(view * model)));
    to_geometry.normal = normalize(vec3(projection * vec4(normal_matrix * in_normal, 1.0)));

    float tot_weight = in_joint_weights[0] + in_joint_weights[1]
                     + in_joint_weights[2] + in_joint_weights[3];

    /* If all weights are 0, treat this vertex as a static one */
    if(tot_weight == 0.0f) {
    
        gl_Position = projection * view * model * vec4(in_pos, 1.0);
        return;
    }

    vec3 new_pos =  vec3(0.0f, 0.0f, 0.0f);

    for(int w_idx = 0; w_idx < 4; w_idx++) {

        int joint_idx = int(in_joint_indices[w_idx]);

        mat4 inv_bind_mat = anim_inv_bind_mats [joint_idx];
        mat4 pose_mat     = anim_curr_pose_mats[joint_idx];

        float fraction = in_joint_weights[w_idx] / tot_weight;
        
        vec4 to_add_homo = fraction * pose_mat * inv_bind_mat * vec4(in_pos, 1.0f);
        new_pos += to_add_homo.xyz;
    }

    gl_Position = projection * view * model * vec4(new_pos, 1.0f);
}

