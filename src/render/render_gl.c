/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018 Eduard Permyakov 
 *
 *  Permafrost Engine is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Permafrost Engine is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "render_gl.h"
#include "render_private.h"
#include "mesh.h"
#include "vertex.h"
#include "shader.h"
#include "material.h"
#include "gl_assert.h"
#include "gl_uniforms.h"
#include "public/render.h"
#include "../entity.h"
#include "../camera.h"
#include "../config.h"
#include "../anim/public/skeleton.h"
#include "../anim/public/anim.h"
#include "../ui.h"
#include "../map/public/map.h"

#include <GL/glew.h>

#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <string.h>


#define ARR_SIZE(a)                 (sizeof(a)/sizeof(a[0]))

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void r_gl_set_materials(GLuint shader_prog, size_t num_mats, const struct material *mats)
{
    for(size_t i = 0; i < num_mats; i++) {
    
        const struct material *mat = &mats[i];
        const size_t nmembers = 3; 

        const struct member_desc{
            const GLchar *name; 
            size_t        size;
            ptrdiff_t     offset;
        }descs[] = {
            {"ambient_intensity", 1, offsetof(struct material, ambient_intensity) },
            {"diffuse_clr",       3, offsetof(struct material, diffuse_clr)       },
            {"specular_clr",      3, offsetof(struct material, specular_clr)      }
        };

        for(size_t j = 0; j < nmembers; j++) {
        
            char locbuff[64];
            GLuint loc;

            snprintf(locbuff, sizeof(locbuff), "%s[%zu].%s", GL_U_MATERIALS, i, descs[j].name);
            locbuff[sizeof(locbuff)-1] = '\0';

            loc = glGetUniformLocation(shader_prog, locbuff);
            switch(descs[j].size) {
            case 1: glUniform1fv(loc, 1, (void*) ((char*)mat + descs[j].offset) ); break;
            case 3: glUniform3fv(loc, 1, (void*) ((char*)mat + descs[j].offset) ); break;
            default: assert(0);
            }

        }
    }
}

static void r_gl_set_uniform_mat4x4_array(mat4x4_t *data, size_t count, 
                                          const char *uname, const char *shader_name)
{
    GLuint loc, shader_prog;

    shader_prog = R_Shader_GetProgForName(shader_name);
    glUseProgram(shader_prog);

    loc = glGetUniformLocation(shader_prog, uname);
    glUniformMatrix4fv(loc, count, GL_FALSE, (void*)data);
}

static void r_gl_set_uniform_vec4_array(vec4_t *data, size_t count, 
                                        const char *uname, const char *shader_name)
{
    GLuint loc, shader_prog;

    shader_prog = R_Shader_GetProgForName(shader_name);
    glUseProgram(shader_prog);

    loc = glGetUniformLocation(shader_prog, uname);
    glUniform4fv(loc, count, (void*)data);
}

static void r_gl_set_view(const mat4x4_t *view, const char *shader_name)
{
    GLuint loc, shader_prog;

    shader_prog = R_Shader_GetProgForName(shader_name);
    glUseProgram(shader_prog);

    loc = glGetUniformLocation(shader_prog, GL_U_VIEW);
    glUniformMatrix4fv(loc, 1, GL_FALSE, view->raw);
}

static void r_gl_set_proj(const mat4x4_t *proj, const char *shader_name)
{
    GLuint loc, shader_prog;

    shader_prog = R_Shader_GetProgForName(shader_name);
    glUseProgram(shader_prog);

    loc = glGetUniformLocation(shader_prog, GL_U_PROJECTION);
    glUniformMatrix4fv(loc, 1, GL_FALSE, proj->raw);
}

static void r_gl_set_view_pos(const vec3_t *pos, const char *shader_name)
{
    GLuint loc, shader_prog;

    shader_prog = R_Shader_GetProgForName(shader_name);
    glUseProgram(shader_prog);

    loc = glGetUniformLocation(shader_prog, GL_U_VIEW_POS);
    glUniform3fv(loc, 1, pos->raw);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void R_GL_Init(struct render_private *priv, const char *shader, const struct vertex *vbuff)
{
    struct mesh *mesh = &priv->mesh;

    glGenVertexArrays(1, &mesh->VAO);
    glBindVertexArray(mesh->VAO);

    glGenBuffers(1, &mesh->VBO);
    glBindBuffer(GL_ARRAY_BUFFER, mesh->VBO);
    glBufferData(GL_ARRAY_BUFFER, mesh->num_verts * sizeof(struct vertex), vbuff, GL_STATIC_DRAW);

    /* Attribute 0 - position */
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(struct vertex), (void*)0);
    glEnableVertexAttribArray(0);

    /* Attribute 1 - texture coordinates */
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(struct vertex), 
        (void*)offsetof(struct vertex, uv));
    glEnableVertexAttribArray(1);

    /* Attribute 2 - normal */
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(struct vertex), 
        (void*)offsetof(struct vertex, normal));
    glEnableVertexAttribArray(2);

    /* Attribute 3 - material index */
    glVertexAttribIPointer(3, 1, GL_INT, sizeof(struct vertex), 
        (void*)offsetof(struct vertex, material_idx));
    glEnableVertexAttribArray(3);

    if(0 == strcmp("mesh.animated.textured-phong", shader)) {

        /* Here, we use 2 attributes to pass in an array of size 6 since we are 
         * limited to a maximum of 4 components per attribute. */

        /* Attribute 4/5 - joint indices */
        glVertexAttribPointer(4, 3, GL_INT, GL_FALSE, sizeof(struct vertex),
            (void*)offsetof(struct vertex, joint_indices));
        glEnableVertexAttribArray(4);  
        glVertexAttribPointer(5, 3, GL_INT, GL_FALSE, sizeof(struct vertex),
            (void*)offsetof(struct vertex, joint_indices) + 3*sizeof(GLint));
        glEnableVertexAttribArray(6);  

        /* Attribute 6/7 - joint weights */
        glVertexAttribPointer(6, 3, GL_FLOAT, GL_FALSE, sizeof(struct vertex),
            (void*)offsetof(struct vertex, weights));
        glEnableVertexAttribArray(6);  
        glVertexAttribPointer(7, 3, GL_FLOAT, GL_FALSE, sizeof(struct vertex),
            (void*)offsetof(struct vertex, weights) + 3*sizeof(GLfloat));
        glEnableVertexAttribArray(7);  

    }else if(0 == strcmp("terrain", shader)) {

        /* Attribute 4 - tile texture blend mode */
        glVertexAttribIPointer(4, 1, GL_INT, sizeof(struct vertex), 
            (void*)offsetof(struct vertex, blend_mode));
        glEnableVertexAttribArray(4);
         
        /* Attribute 5 - adjacent material indices */
        glVertexAttribIPointer(5, 4, GL_INT, sizeof(struct vertex), 
            (void*)offsetof(struct vertex, adjacent_mat_indices));
        glEnableVertexAttribArray(5);
    }

    priv->shader_prog = R_Shader_GetProgForName(shader);
}

void R_GL_Draw(const void *render_private, mat4x4_t *model)
{
    const struct render_private *priv = render_private;
    GLuint loc;

    glUseProgram(priv->shader_prog);

    loc = glGetUniformLocation(priv->shader_prog, GL_U_MODEL);
    glUniformMatrix4fv(loc, 1, GL_FALSE, model->raw);

    r_gl_set_materials(priv->shader_prog, priv->num_materials, priv->materials);

    for(int i = 0; i < priv->num_materials; i++) {
        R_Texture_GL_Activate(&priv->materials[i].texture, priv->shader_prog);
    }

    glBindVertexArray(priv->mesh.VAO);
    glDrawArrays(GL_TRIANGLES, 0, priv->mesh.num_verts);
}

void R_GL_SetViewMatAndPos(const mat4x4_t *view, const vec3_t *pos)
{
    const char *shaders[] = {
        "mesh.static.colored",
        "mesh.static.textured",
        "mesh.static.textured-phong",
        "mesh.static.tile-outline",
        "mesh.static.normals.colored",
        "mesh.animated.textured-phong",
        "mesh.animated.normals.colored",
        "terrain",
        "terrain-baked",
    };

    for(int i = 0; i < ARR_SIZE(shaders); i++) {

        r_gl_set_view(view, shaders[i]);
        r_gl_set_view_pos(pos, shaders[i]);
    }
}

void R_GL_SetProj(const mat4x4_t *proj)
{
    const char *shaders[] = {
        "mesh.static.colored",
        "mesh.static.textured",
        "mesh.static.textured-phong",
        "mesh.static.tile-outline",
        "mesh.static.normals.colored",
        "mesh.animated.textured-phong",
        "mesh.animated.normals.colored",
        "terrain",
        "terrain-baked",
    };

    for(int i = 0; i < ARR_SIZE(shaders); i++)
        r_gl_set_proj(proj, shaders[i]);
}

void R_GL_SetAnimUniforms(mat4x4_t *inv_bind_poses, mat4x4_t *curr_poses, size_t count)
{
    const char *shaders[] = {
        "mesh.animated.textured-phong",
        "mesh.animated.normals.colored",
    };

    for(int i = 0; i < ARR_SIZE(shaders); i++) {

        r_gl_set_uniform_mat4x4_array(inv_bind_poses, count, GL_U_INV_BIND_MATS, shaders[i]);
        r_gl_set_uniform_mat4x4_array(curr_poses, count, GL_U_CURR_POSE_MATS, shaders[i]);
    }
}

void R_GL_SetAmbientLightColor(vec3_t color)
{
    const char *shaders[] = {
        "mesh.static.textured-phong",
        "mesh.animated.textured-phong",
        "terrain",
        "terrain-baked",
    };

    for(int i = 0; i < ARR_SIZE(shaders); i++) {
    
        GLuint loc, shader_prog;

        shader_prog = R_Shader_GetProgForName(shaders[i]);
        glUseProgram(shader_prog);

        loc = glGetUniformLocation(shader_prog, GL_U_AMBIENT_COLOR);
        glUniform3fv(loc, 1, color.raw);
    }
}

void R_GL_SetLightEmitColor(vec3_t color)
{
    const char *shaders[] = {
        "mesh.static.textured-phong",
        "mesh.animated.textured-phong",
        "terrain",
        "terrain-baked",
    };

    for(int i = 0; i < ARR_SIZE(shaders); i++) {
    
        GLuint loc, shader_prog;

        shader_prog = R_Shader_GetProgForName(shaders[i]);
        glUseProgram(shader_prog);

        loc = glGetUniformLocation(shader_prog, GL_U_LIGHT_COLOR);
        glUniform3fv(loc, 1, color.raw);
    }
}

void R_GL_SetLightPos(vec3_t pos)
{
    const char *shaders[] = {
        "mesh.static.textured-phong",
        "mesh.animated.textured-phong",
        "terrain",
        "terrain-baked",
    };

    for(int i = 0; i < ARR_SIZE(shaders); i++) {

        GLuint loc, shader_prog;
    
        shader_prog = R_Shader_GetProgForName(shaders[i]);
        glUseProgram(shader_prog);

        loc = glGetUniformLocation(shader_prog, GL_U_LIGHT_POS);
        glUniform3fv(loc, 1, pos.raw);
    }
}

void R_GL_DrawSkeleton(const struct entity *ent, const struct skeleton *skel, const struct camera *cam)
{
    vec3_t *vbuff;
    GLint VAO, VBO;
    GLint shader_prog;
    GLuint loc;
    vec3_t green = (vec3_t){0.0f, 1.0f, 0.0f};

    mat4x4_t model;
    Entity_ModelMatrix(ent, &model);

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
        PFM_Mat4x4_Inverse(&skel->inv_bind_poses[i], &bind_pose);

        /* The root of the bone in object space */
        PFM_Mat4x4_Mult4x1(&bind_pose, &homo, &result);
        vbuff[vbuff_idx] = (vec3_t){result.x, result.y ,result.z};
    
        /* The tip of the bone in object space */
        homo = (vec4_t){curr->tip.x, curr->tip.y, curr->tip.z, 1.0f}; 
        PFM_Mat4x4_Mult4x1(&bind_pose, &homo, &result);
        vbuff[vbuff_idx + 1] = (vec3_t){result.x, result.y ,result.z};

        /* Lastly, render a label with the joint's name at the root position */
        if(!cam)
            continue;

        mat4x4_t view, proj;
        Camera_MakeViewMat(cam, &view); 
        Camera_MakeProjMat(cam, &proj);

        vec4_t root_homo = {vbuff[vbuff_idx].x, vbuff[vbuff_idx].y, vbuff[vbuff_idx].z, 1.0f};
        vec4_t clip, tmpa, tmpb;
        PFM_Mat4x4_Mult4x1(&model, &root_homo, &tmpa);
        PFM_Mat4x4_Mult4x1(&view, &tmpa, &tmpb);
        PFM_Mat4x4_Mult4x1(&proj, &tmpb, &clip);
        vec3_t ndc = (vec3_t){ clip.x / clip.w, clip.y / clip.w, clip.z / clip.w };

        float screen_x = (ndc.x + 1.0f) * CONFIG_RES_X/2.0f;
        float screen_y = CONFIG_RES_Y - ((ndc.y + 1.0f) * CONFIG_RES_Y/2.0f);
        UI_DrawText(curr->name, (struct rect){screen_x, screen_y, 100, 25}, (struct rgba){0, 255, 0, 255});
    }
 
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, skel->num_joints * sizeof(vec3_t) * 2, vbuff, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vec3_t), (void*)0);
    glEnableVertexAttribArray(0);  

    shader_prog = R_Shader_GetProgForName("mesh.static.colored");
    glUseProgram(shader_prog);

    /* Set uniforms */
    loc = glGetUniformLocation(shader_prog, GL_U_COLOR);
    glUniform3fv(loc, 1, green.raw);

    loc = glGetUniformLocation(shader_prog, GL_U_MODEL);
    glUniformMatrix4fv(loc, 1, GL_FALSE, model.raw);

    glPointSize(5.0f);

    glBindVertexArray(VAO);
    glDrawArrays(GL_POINTS, 0, skel->num_joints * 2);
    glDrawArrays(GL_LINES, 0, skel->num_joints * 2);

cleanup:
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
    free(vbuff);
}

void R_GL_DrawOrigin(const void *render_private, mat4x4_t *model)
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

    shader_prog = R_Shader_GetProgForName("mesh.static.colored");
    glUseProgram(shader_prog);

    /* Set uniforms */
    loc = glGetUniformLocation(shader_prog, GL_U_MODEL);
    glUniformMatrix4fv(loc, 1, GL_FALSE, model->raw);

    /* Set line width */
    GLfloat old_width;
    glGetFloatv(GL_LINE_WIDTH, &old_width);
    glLineWidth(3.0f);

    /* Render the 3 axis lines at the origin */
    vbuff[0] = (vec3_t){0.0f, 0.0f, 0.0f};
    loc = glGetUniformLocation(shader_prog, GL_U_COLOR);

    for(int i = 0; i < 3; i++) {

        switch(i) {
        case 0:
            vbuff[1] = (vec3_t){1.0f, 0.0f, 0.0f}; 
            glUniform3fv(loc, 1, red.raw);
            break;
        case 1:
            vbuff[1] = (vec3_t){0.0f, 1.0f, 0.0f}; 
            glUniform3fv(loc, 1, green.raw);
            break;
        case 2:
            vbuff[1] = (vec3_t){0.0f, 0.0f, 1.0f}; 
            glUniform3fv(loc, 1, blue.raw);
            break;
        }
    
        glBufferData(GL_ARRAY_BUFFER, 2 * sizeof(vec3_t), vbuff, GL_STATIC_DRAW);

        glBindVertexArray(VAO);
        glDrawArrays(GL_LINES, 0, 2);
    }
    glLineWidth(old_width);

cleanup:
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
}

void R_GL_DrawRay(vec3_t origin, vec3_t dir, mat4x4_t *model, vec3_t color, float t)
{
    vec3_t vbuff[2];
    GLint VAO, VBO;
    GLint shader_prog;
    GLuint loc;

    vbuff[0] = origin; 
    PFM_Vec3_Normal(&dir, &dir);
    PFM_Vec3_Scale(&dir, t, &dir);
    PFM_Vec3_Add(&origin, &dir, &vbuff[1]);

    /* OpenGL setup */
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vec3_t), (void*)0);
    glEnableVertexAttribArray(0);  

    shader_prog = R_Shader_GetProgForName("mesh.static.colored");
    glUseProgram(shader_prog);

    /* Set uniforms */
    loc = glGetUniformLocation(shader_prog, GL_U_MODEL);
    glUniformMatrix4fv(loc, 1, GL_FALSE, model->raw);

    loc = glGetUniformLocation(shader_prog, GL_U_COLOR);
    glUniform3fv(loc, 1, color.raw);

    /* buffer & render */
    glBufferData(GL_ARRAY_BUFFER, 2 * sizeof(vec3_t), vbuff, GL_STATIC_DRAW);

    GLfloat old_width;
    glGetFloatv(GL_LINE_WIDTH, &old_width);
    glLineWidth(5.0f);

    glBindVertexArray(VAO);
    glDrawArrays(GL_LINES, 0, 2);

cleanup:
    glLineWidth(old_width);
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
}

void R_GL_DrawOBB(const struct entity *ent)
{
    if(!(ent->flags & ENTITY_FLAG_COLLISION))
        return;

    GLint VAO, VBO;
    GLint shader_prog;
    GLuint loc;
    vec3_t blue = (vec3_t){0.0f, 0.0f, 1.0f};

    const struct aabb *aabb;
    if(ent->flags & ENTITY_FLAG_ANIMATED)
        aabb = A_GetCurrPoseAABB(ent);
    else
        aabb = &ent->identity_aabb;

    mat4x4_t model;
    Entity_ModelMatrix(ent, &model);

    vec3_t vbuff[24] = {
        [0] = {aabb->x_min, aabb->y_min, aabb->z_min},
        [1] = {aabb->x_min, aabb->y_min, aabb->z_max},
        [2] = {aabb->x_min, aabb->y_max, aabb->z_min},
        [3] = {aabb->x_min, aabb->y_max, aabb->z_max},
        [4] = {aabb->x_max, aabb->y_min, aabb->z_min},
        [5] = {aabb->x_max, aabb->y_min, aabb->z_max},
        [6] = {aabb->x_max, aabb->y_max, aabb->z_min},
        [7] = {aabb->x_max, aabb->y_max, aabb->z_max},
    };
    vbuff[8 ] = vbuff[0];
    vbuff[9 ] = vbuff[2];
    vbuff[10] = vbuff[1];
    vbuff[11] = vbuff[3];
    vbuff[12] = vbuff[4];
    vbuff[13] = vbuff[6];
    vbuff[14] = vbuff[5];
    vbuff[15] = vbuff[7];
    vbuff[16] = vbuff[0];
    vbuff[17] = vbuff[4];
    vbuff[18] = vbuff[1];
    vbuff[19] = vbuff[5];
    vbuff[20] = vbuff[2];
    vbuff[21] = vbuff[6];
    vbuff[22] = vbuff[3];
    vbuff[23] = vbuff[7];

    /* OpenGL setup */
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vec3_t), (void*)0);
    glEnableVertexAttribArray(0);  

    shader_prog = R_Shader_GetProgForName("mesh.static.colored");
    glUseProgram(shader_prog);

    /* Set uniforms */
    loc = glGetUniformLocation(shader_prog, GL_U_MODEL);
    glUniformMatrix4fv(loc, 1, GL_FALSE, model.raw);

    loc = glGetUniformLocation(shader_prog, GL_U_COLOR);
    glUniform3fv(loc, 1, blue.raw);

    /* buffer & render */
    glBufferData(GL_ARRAY_BUFFER, ARR_SIZE(vbuff) * sizeof(vec3_t), vbuff, GL_STATIC_DRAW);

    glBindVertexArray(VAO);
    glDrawArrays(GL_LINES, 0, ARR_SIZE(vbuff));

cleanup:
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
}

void R_GL_DrawBox2D(vec2_t screen_pos, vec2_t signed_size, vec3_t color, float width)
{
    GLint VAO, VBO;
    GLint shader_prog;
    GLuint loc;

    vec3_t vbuff[4] = {
        (vec3_t){screen_pos.x,                 screen_pos.y,                 0.0f},
        (vec3_t){screen_pos.x + signed_size.x, screen_pos.y,                 0.0f},
        (vec3_t){screen_pos.x + signed_size.x, screen_pos.y + signed_size.y, 0.0f},
        (vec3_t){screen_pos.x,                 screen_pos.y + signed_size.y, 0.0f},
    };

    /* Set view and projection matrices for rendering in screen coordinates */
    mat4x4_t ortho;
    PFM_Mat4x4_MakeOrthographic(0.0f, CONFIG_RES_X, CONFIG_RES_Y, 0.0f, -1.0f, 1.0f, &ortho);
    R_GL_SetProj(&ortho);

    mat4x4_t identity;
    PFM_Mat4x4_Identity(&identity);
    vec3_t dummy_pos = (vec3_t){0.0f};
    R_GL_SetViewMatAndPos(&identity, &dummy_pos);

    /* OpenGL setup */
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vec3_t), (void*)0);
    glEnableVertexAttribArray(0);  

    shader_prog = R_Shader_GetProgForName("mesh.static.colored");
    glUseProgram(shader_prog);

    /* Set uniforms */
    loc = glGetUniformLocation(shader_prog, GL_U_MODEL);
    glUniformMatrix4fv(loc, 1, GL_FALSE, identity.raw);

    loc = glGetUniformLocation(shader_prog, GL_U_COLOR);
    glUniform3fv(loc, 1, color.raw);

    /* buffer & render */
    glBufferData(GL_ARRAY_BUFFER, ARR_SIZE(vbuff) * sizeof(vec3_t), vbuff, GL_STATIC_DRAW);

    float old_width;
    glGetFloatv(GL_LINE_WIDTH, &old_width);
    glLineWidth(width);

    glBindVertexArray(VAO);
    glDrawArrays(GL_LINE_LOOP, 0, ARR_SIZE(vbuff));

    glLineWidth(old_width);

cleanup:
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
}

void R_GL_DrawNormals(const void *render_private, mat4x4_t *model, bool anim)
{
    const struct render_private *priv = render_private;


    GLuint normals_shader = anim ? R_Shader_GetProgForName("mesh.animated.normals.colored")
                                 : R_Shader_GetProgForName("mesh.static.normals.colored");
    assert(normals_shader);
    glUseProgram(normals_shader);

    GLuint loc;
    vec3_t yellow = (vec3_t){1.0f, 1.0f, 0.0f};

    loc = glGetUniformLocation(normals_shader, GL_U_COLOR);
    glUniform3fv(loc, 1, yellow.raw);

    loc = glGetUniformLocation(normals_shader, GL_U_MODEL);
    glUniformMatrix4fv(loc, 1, GL_FALSE, model->raw);

    glBindVertexArray(priv->mesh.VAO);
    glDrawArrays(GL_TRIANGLES, 0, priv->mesh.num_verts);
}

void R_GL_DumpFramebuffer_PPM(const char *filename, int width, int height)
{
    long img_size = width * height * 3;
    unsigned char *data = malloc(img_size);
    if(!data) {
        return;
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, data);

    FILE *file = fopen(filename, "wb");
    if(!file) {
        free(data); 
        return;
    }

    fprintf(file, "P6\n%d %d\n%d\n", width, height, 255);
    for(int i = 0; i < height; i++) {
        for(int j = 0; j < width; j++) {

            static unsigned char color[3];
            color[0] = data[3*i*width + 3*j    ];
            color[1] = data[3*i*width + 3*j + 1];
            color[2] = data[3*i*width + 3*j + 2];
            fwrite(color, 1, 3, file);
        }
    }

    fclose(file);
    free(data);
}

void R_GL_DrawSelectionCircle(vec2_t xz, float radius, float width, vec3_t color, 
                              const struct map *map)
{
    GLint VAO, VBO;
    GLint shader_prog;
    GLuint loc;

    const int NUM_SAMPLES = 48;
    vec3_t vbuff[NUM_SAMPLES * 2 + 2];

    for(int i = 0; i < NUM_SAMPLES * 2; i += 2) {

        float theta = (2.0f * M_PI) * ((float)i/NUM_SAMPLES);

        float x_near = xz.raw[0] + radius * cos(theta);
        float z_near = xz.raw[1] - radius * sin(theta);

        float x_far = xz.raw[0] + (radius + width) * cos(theta);
        float z_far = xz.raw[1] - (radius + width) * sin(theta);

        vbuff[i]     = (vec3_t){x_near, M_HeightAtPoint(map, (vec2_t){x_near, z_near}) + 0.1, z_near};
        vbuff[i + 1] = (vec3_t){x_far,  M_HeightAtPoint(map, (vec2_t){x_far,  z_far }) + 0.1, z_far };
    }
    vbuff[NUM_SAMPLES * 2]     = vbuff[0];
    vbuff[NUM_SAMPLES * 2 + 1] = vbuff[1];

    mat4x4_t identity;
    PFM_Mat4x4_Identity(&identity);

    /* OpenGL setup */
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vec3_t), (void*)0);
    glEnableVertexAttribArray(0);  

    shader_prog = R_Shader_GetProgForName("mesh.static.colored");
    glUseProgram(shader_prog);

    /* Set uniforms */
    loc = glGetUniformLocation(shader_prog, GL_U_MODEL);
    glUniformMatrix4fv(loc, 1, GL_FALSE, identity.raw);

    loc = glGetUniformLocation(shader_prog, GL_U_COLOR);
    glUniform3fv(loc, 1, color.raw);

    /* buffer & render */
    glBufferData(GL_ARRAY_BUFFER, ARR_SIZE(vbuff) * sizeof(vec3_t), vbuff, GL_STATIC_DRAW);

    float old_width;
    glGetFloatv(GL_LINE_WIDTH, &old_width);
    glLineWidth(width);

    glBindVertexArray(VAO);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, ARR_SIZE(vbuff));

    glLineWidth(old_width);

cleanup:
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
}

