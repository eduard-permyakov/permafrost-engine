#version 330 core

#define MAX_MATERIALS 16

/*****************************************************************************/
/* INPUTS                                                                    */
/*****************************************************************************/

in VertexToFrag {
         vec2 uv;
    flat int  mat_idx;
}from_vertex;

/*****************************************************************************/
/* OUTPUTS                                                                   */
/*****************************************************************************/

out vec4 o_frag_color;

/*****************************************************************************/
/* UNIFORMS                                                                  */
/*****************************************************************************/

uniform vec3 ambient_color;

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

    vec3 ambient = materials[from_vertex.mat_idx].ambient_intensity * ambient_color;
    o_frag_color = vec4(ambient * tex_color.xyz, 1.0);
}

