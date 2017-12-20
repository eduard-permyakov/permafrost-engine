#version 330 core

#define MAGNITUDE 0.4

layout (triangles) in;
layout (line_strip) out;
layout (max_vertices = 6) out;

in VertexToGeo {
    vec3 normal;
}from_vertex[];

void main()
{
    int i;
    for(i = 0; i < gl_in.length(); i++) {

        gl_Position = gl_in[i].gl_Position;
        EmitVertex();

        gl_Position = gl_in[i].gl_Position + vec4(from_vertex[i].normal, 0.0) * MAGNITUDE;
        EmitVertex();

        EndPrimitive();
    }
}

