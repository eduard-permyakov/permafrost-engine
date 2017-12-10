#include "render_gl.h"
#include "render_private.h"
#include "mesh.h"
#include "vertex.h"
#include "shader.h"
#include "../entity.h"
#include "../gl_uniforms.h"
#include "../anim/public/skeleton.h"
#include "../anim/public/anim.h"

#include <GL/glew.h>

#include <stdlib.h>
#include <stddef.h>
#include <assert.h>


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

    /* Attribute 2 - material index */
    glVertexAttribPointer(1, 2, GL_INT, GL_FALSE, sizeof(struct vertex), 
        (void*)offsetof(struct vertex, material_idx));
    glEnableVertexAttribArray(2);

    /* Attribute 3 - joint indices */
    glVertexAttribPointer(3, 4, GL_INT, GL_FALSE, sizeof(struct vertex),
        (void*)offsetof(struct vertex, joint_indices));
    glEnableVertexAttribArray(3);  

    /* Attribute 4 - joint weights */
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(struct vertex),
        (void*)offsetof(struct vertex, weights));
    glEnableVertexAttribArray(4);  

    priv->shader_prog = Shader_GetProgForName("entity_animated");
}

void R_GL_Draw(struct entity *ent)
{
    struct render_private *priv = ent->render_private;
	GLuint loc;
    vec3_t red = (vec3_t){1.0f, 0.0f, 0.0f};

    glUseProgram(priv->shader_prog);

    loc = glGetUniformLocation(priv->shader_prog, GL_U_COLOR);
	glUniform3fv(loc, 1, red.raw);

    loc = glGetUniformLocation(priv->shader_prog, GL_U_MODEL);
	glUniformMatrix4fv(loc, 1, GL_FALSE, ent->model_matrix.raw);

    glBindVertexArray(priv->mesh.VAO);
    glDrawArrays(GL_TRIANGLES, 0, priv->mesh.num_verts);
}

void R_GL_SetView(const mat4x4_t *view, const char *shader_name)
{
    GLuint loc, shader_prog;

    shader_prog = Shader_GetProgForName(shader_name);
    glUseProgram(shader_prog);

    loc = glGetUniformLocation(shader_prog, GL_U_VIEW);
	glUniformMatrix4fv(loc, 1, GL_FALSE, view->raw);
}

void R_GL_SetProj(const mat4x4_t *proj, const char *shader_name)
{
    GLuint loc, shader_prog;

    shader_prog = Shader_GetProgForName(shader_name);
    glUseProgram(shader_prog);

    loc = glGetUniformLocation(shader_prog, GL_U_PROJECTION);
	glUniformMatrix4fv(loc, 1, GL_FALSE, proj->raw);
}

void R_GL_SetUniformMat4x4Array(mat4x4_t *data, size_t count, const char *uname, const char *shader_name)
{
    GLuint loc, shader_prog;

    shader_prog = Shader_GetProgForName(shader_name);
    glUseProgram(shader_prog);

    loc = glGetUniformLocation(shader_prog, uname);
	glUniformMatrix4fv(loc, count, GL_FALSE, (void*)data);
}

void R_GL_DrawSkeleton(const struct entity *ent, const struct skeleton *skel)
{
    vec3_t *vbuff;
    GLint VAO, VBO;
    GLint shader_prog;
	GLuint loc;
    vec3_t green = (vec3_t){0.0f, 1.0f, 0.0f};

    /* Our vbuff looks like this:
     * +----------------+-------------+--------------+-----
     * | joint root 0   | joint tip 0 | joint root 1 | ...
     * +----------------+-------------+--------------+-----
     */
    vbuff = calloc(skel->num_joints * 2, sizeof(vec3_t));

    for(int i = 0, vbuff_idx = 0; i < skel->num_joints; i++, vbuff_idx +=2) {

        struct joint *curr = &skel->joints[i];
        struct SQT *sqt = &skel->bind_sqts[i];

        vec4_t homo = (vec4_t){0.0f, 0.0f, 0.0f, 1.0f}; 
        vec4_t result;

        mat4x4_t bind_pose;
        PFM_mat4x4_inverse(&skel->inv_bind_poses[i], &bind_pose);

        /* The root of the bone in object space */
        PFM_mat4x4_mult4x1(&bind_pose, &homo, &result);
        vbuff[vbuff_idx] = (vec3_t){result.x, result.y ,result.z};
    
        /* The tip of the bone in object space */
        homo = (vec4_t){curr->tip.x, curr->tip.y, curr->tip.z, 1.0f}; 
        PFM_mat4x4_mult4x1(&bind_pose, &homo, &result);
        vbuff[vbuff_idx + 1] = (vec3_t){result.x, result.y ,result.z};
    }
 
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, skel->num_joints * sizeof(vec3_t) * 2, vbuff, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vec3_t), (void*)0);
    glEnableVertexAttribArray(0);  

    shader_prog = Shader_GetProgForName("entity_static");
    glUseProgram(shader_prog);

    /* Set uniforms */
    loc = glGetUniformLocation(shader_prog, GL_U_COLOR);
	glUniform3fv(loc, 1, green.raw);

    loc = glGetUniformLocation(shader_prog, GL_U_MODEL);
	glUniformMatrix4fv(loc, 1, GL_FALSE, ent->model_matrix.raw);

    glPointSize(5.0f);

    glBindVertexArray(VAO);
    glDrawArrays(GL_POINTS, 0, skel->num_joints * 2);
    glDrawArrays(GL_LINES, 0, skel->num_joints * 2);

    free(vbuff);
}

void R_GL_DrawOrigin(const struct entity *ent)
{
    vec3_t vbuff[2];
    GLint VAO, VBO;
    GLint shader_prog;
	GLuint loc;

    vec3_t red   = (vec3_t){1.0f, 0.0f, 0.0f};
    vec3_t green = (vec3_t){0.0f, 1.0f, 0.0f};
    vec3_t blue  = (vec3_t){0.0f, 0.0f, 1.0f};

    /* OpenGL setup */
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vec3_t), (void*)0);
    glEnableVertexAttribArray(0);  

    shader_prog = Shader_GetProgForName("entity_static");
    glUseProgram(shader_prog);

    /* Set uniforms */
    loc = glGetUniformLocation(shader_prog, GL_U_MODEL);
	glUniformMatrix4fv(loc, 1, GL_FALSE, ent->model_matrix.raw);

    /* Set line width */
    GLfloat old_width;
    glGetFloatv(GL_LINE_WIDTH, &old_width);
    glLineWidth(3.0f);

    /* Render the 3 axis lines at the origin */
    vbuff[0] = (vec3_t){0.0f, 0.0f, 0.0f};
    loc = glGetUniformLocation(shader_prog, GL_U_COLOR);

    for(int i = 0; i < 3; i++) {

        switch(i) {
            case 0: { 
                vbuff[1] = (vec3_t){1.0f, 0.0f, 0.0f}; 
	            glUniform3fv(loc, 1, red.raw);
                break;
            }
            case 1: {
                vbuff[1] = (vec3_t){0.0f, 1.0f, 0.0f}; 
	            glUniform3fv(loc, 1, green.raw);
                break;
            }
            case 2: {
                vbuff[1] = (vec3_t){0.0f, 0.0f, 1.0f}; 
	            glUniform3fv(loc, 1, blue.raw);
                break;
            }
        }
    
        glBufferData(GL_ARRAY_BUFFER, 2 * sizeof(vec3_t), vbuff, GL_STATIC_DRAW);

        glBindVertexArray(VAO);
        glDrawArrays(GL_LINES, 0, 2);
    }
    glLineWidth(old_width);
}

