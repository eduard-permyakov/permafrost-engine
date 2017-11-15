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

    /* 2 vertices for each joint to draw a line between parent and child,
     * in the case of root or an orphaned joint, the vertex will be duplicated 
     *
     * Our vbuff looks like this:
     * +----------------+--------------------------+----------------
     * | joint vertex 0 | parent joint vertex of 0 | joint vertex 1 ...
     * +----------------+--------------------------+----------------
     * */
    vbuff = calloc(skel->num_joints * 2, sizeof(vec3_t));

    for(int i = 0, vbuff_idx = 0; i < skel->num_joints; i++, vbuff_idx +=2) {

        struct joint *curr = &skel->joints[i];
        vec4_t homo = (vec4_t){0.0f, 0.0f, 0.0f, 1.0f}; 
        vec4_t result;

        PFM_mat4x4_mult4x1(&curr->inv_bind_pose, &homo, &result);
        vbuff[vbuff_idx] = (vec3_t){result.x, result.y ,result.z};

        if(curr->parent_idx < 0){
            vbuff[vbuff_idx + 1] = vbuff[vbuff_idx];
            continue;
        }

        assert(curr->parent_idx >= 0 && curr->parent_idx < skel->num_joints);

        struct joint *parent = &skel->joints[curr->parent_idx];
        PFM_mat4x4_mult4x1(&parent->inv_bind_pose, &homo, &result);
        vbuff[vbuff_idx + 1] = (vec3_t){result.x, result.y ,result.z};
    }
 
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, skel->num_joints * sizeof(vec3_t) * 2, vbuff, GL_STATIC_DRAW);

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

    shader_prog = Shader_GetProgForName("generic");

    /* Set uniforms */
    loc = glGetUniformLocation(shader_prog, GL_U_COLOR);
	glUniform3fv(loc, 1, green.raw);

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

        glUseProgram(shader_prog);

        glBindVertexArray(VAO);
        glDrawArrays(GL_LINES, 0, 2);
    }
    glLineWidth(old_width);
}

