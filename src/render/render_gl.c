#include "render_gl.h"
#include "render_private.h"
#include "mesh.h"
#include "vertex.h"
#include "shader.h"
#include "../entity.h"
#include "../gl_uniforms.h"
#include "../anim/public/skeleton.h"

#include <GL/glew.h>

#include <stdlib.h>
#include <stddef.h>

void GL_Init(struct render_private *priv)
{
    struct mesh *mesh = &priv->mesh;

    glGenVertexArrays(1, &mesh->VAO);
    glBindVertexArray(mesh->VAO);

    glGenBuffers(1, &mesh->VBO);
    glBindBuffer(GL_ARRAY_BUFFER, mesh->VBO);
    glBufferData(GL_ARRAY_BUFFER, mesh->num_verts * sizeof(struct vertex), mesh->vbuff, GL_STATIC_DRAW);

    /* Attribute 0 - position */
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(struct vertex), (void*)0);
    glEnableVertexAttribArray(0);  

    /* Attribute 1 - texture coordinates */
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(struct vertex), 
        (void*)offsetof(struct vertex, uv));
    glEnableVertexAttribArray(1);  

    //TODO: figure out attribute size constraints here
    /* Attribute 2 - joint weights */
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(struct vertex), 
        (void*)offsetof(struct vertex, weights));
    glEnableVertexAttribArray(2);  

    priv->shader_prog = Shader_GetProgForName("generic");
}

void R_GL_Draw(struct entity *ent)
{
    struct render_private *priv = ent->render_private;
	GLuint loc;
    vec3_t red = (vec3_t){1.0f, 0.0f, 0.0f};

    loc = glGetUniformLocation(priv->shader_prog, GL_U_COLOR);
	glUniform3fv(loc, 1, red.raw);

    loc = glGetUniformLocation(priv->shader_prog, GL_U_MODEL);
	glUniformMatrix4fv(loc, 1, GL_FALSE, ent->model_matrix.raw);

    glUseProgram(priv->shader_prog);

    glBindVertexArray(priv->mesh.VAO);
    glDrawArrays(GL_TRIANGLES, 0, priv->mesh.num_verts);
}

void R_GL_SetView(const mat4x4_t *view, const char *shader_name)
{
    GLuint loc, shader_prog;

    shader_prog = Shader_GetProgForName(shader_name);

    loc = glGetUniformLocation(shader_prog, GL_U_VIEW);
	glUniformMatrix4fv(loc, 1, GL_FALSE, view->raw);
}

void R_GL_SetProj(const mat4x4_t *proj, const char *shader_name)
{
    GLuint loc, shader_prog;

    shader_prog = Shader_GetProgForName(shader_name);

    loc = glGetUniformLocation(shader_prog, GL_U_PROJECTION);
	glUniformMatrix4fv(loc, 1, GL_FALSE, proj->raw);
}

void R_GL_DrawSkeleton(const struct entity *ent, const struct skeleton *skel)
{
    vec3_t *vbuff;
    GLint VAO, VBO;
    GLint shader_prog;
	GLuint loc;
    vec3_t green = (vec3_t){0.0f, 1.0f, 0.0f};

    vbuff = calloc(skel->num_joints, sizeof(vec3_t));

    for(int i = 0; i < skel->num_joints; i++) {
        vec4_t homo = (vec4_t){0.0f, 0.0f, 0.0f, 1.0f}; 
        vec4_t result;

        PFM_mat4x4_mult4x1(&skel->joints[i].inv_bind_pose, &homo, &result);
        vbuff[i].x = result.x;
        vbuff[i].y = result.y;
        vbuff[i].z = result.z;
    }
 
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, skel->num_joints * sizeof(vec3_t), vbuff, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vec3_t), (void*)0);
    glEnableVertexAttribArray(0);  

    shader_prog = Shader_GetProgForName("generic");

    /* Set uniforms */
    loc = glGetUniformLocation(shader_prog, GL_U_COLOR);
	glUniform3fv(loc, 1, green.raw);

    loc = glGetUniformLocation(shader_prog, GL_U_MODEL);
	glUniformMatrix4fv(loc, 1, GL_FALSE, ent->model_matrix.raw);

    glPointSize(5.0f);

    glUseProgram(shader_prog);

    glBindVertexArray(VAO);
    glDrawArrays(GL_POINTS, 0, skel->num_joints);

    free(vbuff);
}

