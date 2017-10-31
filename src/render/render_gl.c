#include "render_gl.h"
#include "render_private.h"
#include "mesh.h"
#include "vertex.h"
#include "shader.h"

#include <GL/glew.h>

#include <stddef.h>

void GL_Init(struct render_private *priv)
{
    struct mesh *mesh = &priv->mesh;

    glGenBuffers(1, &mesh->VBO);
    glBindBuffer(GL_ARRAY_BUFFER, mesh->VBO);
    glBufferData(GL_ARRAY_BUFFER, mesh->num_verts * sizeof(struct vertex), mesh->vbuff, GL_STATIC_DRAW);

    /* Attribute 0 - position */
    glVertexAttribPointer(0, sizeof(vec3_t), GL_FLOAT, GL_FALSE, sizeof(struct vertex), (void*)0);
    glEnableVertexAttribArray(0);  

    /* Attribute 1 - texture coordinates */
    glVertexAttribPointer(1, sizeof(vec2_t), GL_FLOAT, GL_FALSE, sizeof(struct vertex), 
        (void*)offsetof(struct vertex, uv));
    glEnableVertexAttribArray(1);  

    /* Attribute 2 - joint weights */
    glVertexAttribPointer(2, 4 * sizeof(struct joint_weight), GL_FLOAT, GL_FALSE, sizeof(struct vertex), 
        (void*)offsetof(struct vertex, weights));
    glEnableVertexAttribArray(2);  

    priv->shader_prog = Shader_GetProgForName("generic");
}

void R_GL_Draw(struct render_private *priv)
{
    glUseProgram(priv->shader_prog);
}

