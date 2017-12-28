#version 330 core

#define MAX_MATERIALS 16

/*****************************************************************************/
/* INPUTS                                                                    */
/*****************************************************************************/

in VertexToFrag {
         vec2 uv;
    flat int  mat_idx;
         vec3 world_pos;
         vec3 normal;
}from_vertex;

/*****************************************************************************/
/* OUTPUTS                                                                   */
/*****************************************************************************/

out vec4 o_frag_color;

/*****************************************************************************/
/* UNIFORMS                                                                  */
/*****************************************************************************/

uniform vec3 ambient_color;
uniform vec3 light_pos;

uniform sampler2D texture0;
uniform sampler2D texture1;
uniform sampler2D texture2;
uniform sampler2D texture3;
uniform sampler2D texture4;
uniform sampler2D texture5;
uniform sampler2D texture6;
uniform sampler2D texture7;

struct material{
    float ambient_intensity;
    vec3  diffuse_clr;
    vec3  specular_clr;
};

uniform material materials[MAX_MATERIALS];

/*****************************************************************************/
/* PROGRAM                                                                   */
/*****************************************************************************/

void main()
{
    vec4 tex_color;

    switch(from_vertex.mat_idx) {
    case 0: tex_color = texture(texture0, from_vertex.uv); break;
    case 1: tex_color = texture(texture1, from_vertex.uv); break;
    case 2: tex_color = texture(texture2, from_vertex.uv); break;
    case 3: tex_color = texture(texture3, from_vertex.uv); break;
    case 4: tex_color = texture(texture4, from_vertex.uv); break;
    case 5: tex_color = texture(texture5, from_vertex.uv); break;
    case 6: tex_color = texture(texture6, from_vertex.uv); break;
    case 7: tex_color = texture(texture7, from_vertex.uv); break;
    }

    /* Ambient calculations */
    vec3 ambient = materials[from_vertex.mat_idx].ambient_intensity * ambient_color;

    /* Diffuse calculations */
    vec3 light_dir = normalize(light_pos - from_vertex.world_pos);  
    float diff = max(dot(from_vertex.normal, light_dir), 0.0);
    vec3 diffuse = diff * materials[from_vertex.mat_idx].diffuse_clr;

    o_frag_color = vec4( (ambient + diffuse) * tex_color.xyz, 1.0);
}

