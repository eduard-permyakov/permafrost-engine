#version 330 core

     in vec2 uv;
flat in int  uv_idx;

out vec4 o_frag_color;

uniform vec3 color;

uniform sampler2D texture0;
uniform sampler2D texture1;
uniform sampler2D texture2;
uniform sampler2D texture3;
uniform sampler2D texture4;
uniform sampler2D texture5;
uniform sampler2D texture6;
uniform sampler2D texture7;

void main()
{
    switch(uv_idx) {
    case 0: o_frag_color = texture(texture0, uv); break;
    case 1: o_frag_color = texture(texture1, uv); break;
    case 2: o_frag_color = texture(texture2, uv); break;
    case 3: o_frag_color = texture(texture3, uv); break;
    case 4: o_frag_color = texture(texture4, uv); break;
    case 5: o_frag_color = texture(texture5, uv); break;
    case 6: o_frag_color = texture(texture6, uv); break;
    case 7: o_frag_color = texture(texture7, uv); break;
    }
    //o_frag_color = vec4(color, 1.0);
}

