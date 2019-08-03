/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2018 Eduard Permyakov 
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
 *  Linking this software statically or dynamically with other modules is making 
 *  a combined work based on this software. Thus, the terms and conditions of 
 *  the GNU General Public License cover the whole combination. 
 *  
 *  As a special exception, the copyright holders of Permafrost Engine give 
 *  you permission to link Permafrost Engine with independent modules to produce 
 *  an executable, regardless of the license terms of these independent 
 *  modules, and to copy and distribute the resulting executable under 
 *  terms of your choice, provided that you also meet, for each linked 
 *  independent module, the terms and conditions of the license of that 
 *  module. An independent module is a module which is not derived from 
 *  or based on Permafrost Engine. If you modify Permafrost Engine, you may 
 *  extend this exception to your version of Permafrost Engine, but you are not 
 *  obliged to do so. If you do not wish to do so, delete this exception 
 *  statement from your version.
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
#include "../main.h"

#include <GL/glew.h>

#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <string.h>


#define EPSILON                     (1.0f/1024)
#define ARR_SIZE(a)                 (sizeof(a)/sizeof(a[0]))

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static vec3_t s_light_pos = (vec3_t){0.0f, 0.0f, 0.0f};

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

static void r_gl_set_mat4(const mat4x4_t *trans, const char *shader_name, const char *uname)
{
    GLuint loc, shader_prog;

    shader_prog = R_Shader_GetProgForName(shader_name);
    glUseProgram(shader_prog);

    loc = glGetUniformLocation(shader_prog, uname);
    glUniformMatrix4fv(loc, 1, GL_FALSE, trans->raw);
}

static void r_gl_set_vec3(const vec3_t *vec, const char *shader_name, const char *uname)
{
    GLuint loc, shader_prog;

    shader_prog = R_Shader_GetProgForName(shader_name);
    glUseProgram(shader_prog);

    loc = glGetUniformLocation(shader_prog, uname);
    glUniform3fv(loc, 1, vec->raw);
}

static void r_gl_set_vec4(const vec4_t *vec, const char *shader_name, const char *uname)
{
    GLuint loc, shader_prog;

    shader_prog = R_Shader_GetProgForName(shader_name);
    glUseProgram(shader_prog);

    loc = glGetUniformLocation(shader_prog, uname);
    glUniform4fv(loc, 1, vec->raw);
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

    if(strstr(shader, "animated")) {

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

    }else if(strstr(shader, "terrain")) {

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

    if(strstr(shader, "animated")) {
        priv->shader_prog_dp = R_Shader_GetProgForName("mesh.animated.depth");
    }else {
        priv->shader_prog_dp = R_Shader_GetProgForName("mesh.static.depth");
    }

    assert(priv->shader_prog != -1 && priv->shader_prog_dp != -1);
    GL_ASSERT_OK();
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

    GL_ASSERT_OK();
}

void R_GL_SetViewMatAndPos(const mat4x4_t *view, const vec3_t *pos)
{
    const char *shaders[] = {
        "mesh.static.colored",
        "mesh.static.colored-per-vert",
        "mesh.static.textured",
        "mesh.static.textured-phong",
        "mesh.static.textured-phong-shadowed",
        "mesh.static.tile-outline",
        "mesh.static.normals.colored",
        "mesh.animated.textured-phong",
        "mesh.animated.textured-phong-shadowed",
        "mesh.animated.normals.colored",
        "terrain",
        "terrain-shadowed",
        "statusbar",
        "water"
    };

    for(int i = 0; i < ARR_SIZE(shaders); i++) {

        r_gl_set_mat4(view, shaders[i], GL_U_VIEW);
        r_gl_set_vec3(pos, shaders[i], GL_U_VIEW_POS);
    }

    GL_ASSERT_OK();
}

void R_GL_SetProj(const mat4x4_t *proj)
{
    const char *shaders[] = {
        "mesh.static.colored",
        "mesh.static.colored-per-vert",
        "mesh.static.textured",
        "mesh.static.textured-phong",
        "mesh.static.textured-phong-shadowed",
        "mesh.static.tile-outline",
        "mesh.static.normals.colored",
        "mesh.animated.textured-phong",
        "mesh.animated.textured-phong-shadowed",
        "mesh.animated.normals.colored",
        "terrain",
        "terrain-shadowed",
        "statusbar",
        "water"
    };

    for(int i = 0; i < ARR_SIZE(shaders); i++)
        r_gl_set_mat4(proj, shaders[i], GL_U_PROJECTION);

    GL_ASSERT_OK();
}

void R_GL_SetLightSpaceTrans(const mat4x4_t *trans)
{
    const char *shaders[] = {
        "mesh.static.depth",
        "mesh.animated.depth",
        "mesh.static.textured-phong-shadowed",
        "mesh.animated.textured-phong-shadowed",
        "terrain-shadowed",
    };

    for(int i = 0; i < ARR_SIZE(shaders); i++)
        r_gl_set_mat4(trans, shaders[i], GL_U_LS_TRANS);

    GL_ASSERT_OK();
}

void R_GL_SetShadowMap(const GLuint shadow_map_tex_id)
{
    const char *shaders[] = {
        "mesh.static.textured-phong-shadowed",
        "mesh.animated.textured-phong-shadowed",
        "terrain-shadowed",
    };

    for(int i = 0; i < ARR_SIZE(shaders); i++) {

        GLuint shader_prog, sampler_loc;

        shader_prog = R_Shader_GetProgForName(shaders[i]);
        glUseProgram(shader_prog);

        sampler_loc = glGetUniformLocation(shader_prog, GL_U_SHADOW_MAP);
        glActiveTexture(SHADOW_MAP_TUNIT);
        glBindTexture(GL_TEXTURE_2D, shadow_map_tex_id);
        glUniform1i(sampler_loc, SHADOW_MAP_TUNIT - GL_TEXTURE0);
    }

    GL_ASSERT_OK();
}

void R_GL_SetClipPlane(vec4_t plane_eq)
{
    const char *shaders[] = {
        "terrain",
        "terrain-shadowed",
        "mesh.static.textured-phong",
        "mesh.static.textured-phong-shadowed",
        "mesh.animated.textured-phong",
        "mesh.animated.textured-phong-shadowed",
    };

    for(int i = 0; i < ARR_SIZE(shaders); i++)
        r_gl_set_vec4(&plane_eq, shaders[i], GL_U_CLIP_PLANE0);

    GL_ASSERT_OK();
}

void R_GL_SetAnimUniforms(mat4x4_t *inv_bind_poses, mat4x4_t *curr_poses, 
                          mat4x4_t *normal_mat, size_t count)
{
    const char *shaders[] = {
        "mesh.animated.depth",
        "mesh.animated.textured-phong",
        "mesh.animated.textured-phong-shadowed",
        "mesh.animated.normals.colored",
    };

    for(int i = 0; i < ARR_SIZE(shaders); i++) {

        r_gl_set_uniform_mat4x4_array(inv_bind_poses, count, GL_U_INV_BIND_MATS, shaders[i]);
        r_gl_set_uniform_mat4x4_array(curr_poses, count, GL_U_CURR_POSE_MATS, shaders[i]);
        r_gl_set_mat4(normal_mat, shaders[i], GL_U_NORMAL_MAT);
    }

    GL_ASSERT_OK();
}

void R_GL_SetAmbientLightColor(vec3_t color)
{
    const char *shaders[] = {
        "mesh.static.textured-phong",
        "mesh.static.textured-phong-shadowed",
        "mesh.animated.textured-phong",
        "mesh.animated.textured-phong-shadowed",
        "terrain",
        "terrain-shadowed",
    };

    for(int i = 0; i < ARR_SIZE(shaders); i++) {
    
        GLuint loc, shader_prog;

        shader_prog = R_Shader_GetProgForName(shaders[i]);
        glUseProgram(shader_prog);

        loc = glGetUniformLocation(shader_prog, GL_U_AMBIENT_COLOR);
        glUniform3fv(loc, 1, color.raw);
    }

    GL_ASSERT_OK();
}

void R_GL_SetLightEmitColor(vec3_t color)
{
    const char *shaders[] = {
        "mesh.static.textured-phong",
        "mesh.static.textured-phong-shadowed",
        "mesh.animated.textured-phong",
        "mesh.animated.textured-phong-shadowed",
        "terrain",
        "terrain-shadowed",
        "water",
    };

    for(int i = 0; i < ARR_SIZE(shaders); i++) {
    
        GLuint loc, shader_prog;

        shader_prog = R_Shader_GetProgForName(shaders[i]);
        glUseProgram(shader_prog);

        loc = glGetUniformLocation(shader_prog, GL_U_LIGHT_COLOR);
        glUniform3fv(loc, 1, color.raw);
    }

    GL_ASSERT_OK();
}

void R_GL_SetLightPos(vec3_t pos)
{
    const char *shaders[] = {
        "mesh.static.textured-phong",
        "mesh.static.textured-phong-shadowed",
        "mesh.animated.textured-phong",
        "mesh.animated.textured-phong-shadowed",
        "terrain",
        "terrain-shadowed",
        "water",
    };

    for(int i = 0; i < ARR_SIZE(shaders); i++) {

        GLuint loc, shader_prog;
    
        shader_prog = R_Shader_GetProgForName(shaders[i]);
        glUseProgram(shader_prog);

        loc = glGetUniformLocation(shader_prog, GL_U_LIGHT_POS);
        glUniform3fv(loc, 1, pos.raw);
    }

    s_light_pos = pos;
    GL_ASSERT_OK();
}

vec3_t R_GL_GetLightPos(void)
{
    return s_light_pos;
}

void R_GL_SetScreenspaceDrawMode(void)
{
    int width, height;
    Engine_WinDrawableSize(&width, &height);

    mat4x4_t ortho;
    PFM_Mat4x4_MakeOrthographic(0.0f, width, height, 0.0f, -1.0f, 1.0f, &ortho);
    R_GL_SetProj(&ortho);

    mat4x4_t identity;
    PFM_Mat4x4_Identity(&identity);
    vec3_t dummy_pos = (vec3_t){0.0f};
    R_GL_SetViewMatAndPos(&identity, &dummy_pos);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
}

void R_GL_DrawSkeleton(const struct entity *ent, const struct skeleton *skel, const struct camera *cam)
{
    vec3_t *vbuff;
    GLint VAO, VBO;
    GLint shader_prog;
    GLuint loc;
    vec4_t green = (vec4_t){0.0f, 1.0f, 0.0f, 1.0f};

    int width, height;
    Engine_WinDrawableSize(&width, &height);

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

        float screen_x = (ndc.x + 1.0f) * width/2.0f;
        float screen_y = height - ((ndc.y + 1.0f) * height/2.0f);
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
    glUniform4fv(loc, 1, green.raw);

    loc = glGetUniformLocation(shader_prog, GL_U_MODEL);
    glUniformMatrix4fv(loc, 1, GL_FALSE, model.raw);

    glPointSize(5.0f);

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

    vec4_t red   = (vec4_t){1.0f, 0.0f, 0.0f, 1.0f};
    vec4_t green = (vec4_t){0.0f, 1.0f, 0.0f, 1.0f};
    vec4_t blue  = (vec4_t){0.0f, 0.0f, 1.0f, 1.0f};

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
            glUniform4fv(loc, 1, red.raw);
            break;
        case 1:
            vbuff[1] = (vec3_t){0.0f, 1.0f, 0.0f}; 
            glUniform4fv(loc, 1, green.raw);
            break;
        case 2:
            vbuff[1] = (vec3_t){0.0f, 0.0f, 1.0f}; 
            glUniform4fv(loc, 1, blue.raw);
            break;
        }
    
        glBufferData(GL_ARRAY_BUFFER, 2 * sizeof(vec3_t), vbuff, GL_STATIC_DRAW);
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

    vec4_t color4 = (vec4_t){color.x, color.y, color.z, 1.0f};
    loc = glGetUniformLocation(shader_prog, GL_U_COLOR);
    glUniform4fv(loc, 1, color4.raw);

    GLfloat old_width;
    glGetFloatv(GL_LINE_WIDTH, &old_width);
    glLineWidth(5.0f);

    /* buffer & render */
    glBufferData(GL_ARRAY_BUFFER, 2 * sizeof(vec3_t), vbuff, GL_STATIC_DRAW);
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
    vec4_t blue = (vec4_t){0.0f, 0.0f, 1.0f, 1.0f};

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
    glUniform4fv(loc, 1, blue.raw);

    /* buffer & render */
    glBufferData(GL_ARRAY_BUFFER, ARR_SIZE(vbuff) * sizeof(vec3_t), vbuff, GL_STATIC_DRAW);
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

    int win_width, win_height;
    Engine_WinDrawableSize(&win_width, &win_height);

    /* Set view and projection matrices for rendering in screen coordinates */
    mat4x4_t ortho;
    PFM_Mat4x4_MakeOrthographic(0.0f, win_width, win_height, 0.0f, -1.0f, 1.0f, &ortho);
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

    vec4_t color4 = (vec4_t){color.x, color.y, color.z, 1.0f};
    loc = glGetUniformLocation(shader_prog, GL_U_COLOR);
    glUniform4fv(loc, 1, color4.raw);

    float old_width;
    glGetFloatv(GL_LINE_WIDTH, &old_width);
    glLineWidth(width);

    /* buffer & render */
    glBufferData(GL_ARRAY_BUFFER, ARR_SIZE(vbuff) * sizeof(vec3_t), vbuff, GL_STATIC_DRAW);
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
    vec4_t yellow = (vec4_t){1.0f, 1.0f, 0.0f, 1.0f};

    loc = glGetUniformLocation(normals_shader, GL_U_COLOR);
    glUniform4fv(loc, 1, yellow.raw);

    loc = glGetUniformLocation(normals_shader, GL_U_MODEL);
    glUniformMatrix4fv(loc, 1, GL_FALSE, model->raw);

    glBindVertexArray(priv->mesh.VAO);
    glDrawArrays(GL_TRIANGLES, 0, priv->mesh.num_verts);
}

void R_GL_DumpFBColor_PPM(const char *filename, int width, int height)
{
    long img_size = width * height * 3;
    unsigned char *data = malloc(img_size);
    if(!data) {
        return;
    }

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
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

    GL_ASSERT_OK();
    fclose(file);
    free(data);
}

void R_GL_DumpFBDepth_PPM(const char *filename, int width, int height, 
                          bool linearize, GLfloat near, GLfloat far)
{
    long img_size = width * height * sizeof(GLfloat);
    GLfloat *data = malloc(img_size);
    if(!data) {
        return;
    }

    glReadPixels(0, 0, width, height, GL_DEPTH_COMPONENT, GL_FLOAT, data);

    FILE *file = fopen(filename, "wb");
    if(!file) {
        free(data); 
        return;
    }

    fprintf(file, "P6\n%d %d\n%d\n", width, height, 255);
    for(int i = 0; i < height; i++) {
        for(int j = 0; j < width; j++) {

            GLfloat norm_depth = data[i * width + j];
            assert(norm_depth >= 0.0f && norm_depth <= 1.0f);

            GLfloat z;
            if(linearize) {
                z = (2 * near) / (far + near - norm_depth * (far - near));
            }else{
                z = norm_depth; 
            }
            assert(z >= 0 && z <= 1.0f);

            static unsigned char color[3];
            color[0] = z * 255;
            color[1] = z * 255;
            color[2] = z * 255; 
            fwrite(color, 1, 3, file);
        }
    }

    GL_ASSERT_OK();
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
    
        float height_near = M_HeightAtPoint(map, M_ClampedMapCoordinate(map, (vec2_t){x_near, z_near}));
        float height_far  = M_HeightAtPoint(map, M_ClampedMapCoordinate(map, (vec2_t){x_far,  z_far }));

        vbuff[i]     = (vec3_t){x_near, height_near + 0.0f, z_near};
        vbuff[i + 1] = (vec3_t){x_far,  height_far + 0.1,   z_far };
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

    vec4_t color4 = (vec4_t){color.x, color.y, color.z, 1.0f};
    loc = glGetUniformLocation(shader_prog, GL_U_COLOR);
    glUniform4fv(loc, 1, color4.raw);

    float old_width;
    glGetFloatv(GL_LINE_WIDTH, &old_width);
    glLineWidth(width);

    /* buffer & render */
    glBufferData(GL_ARRAY_BUFFER, ARR_SIZE(vbuff) * sizeof(vec3_t), vbuff, GL_STATIC_DRAW);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, ARR_SIZE(vbuff));

    glLineWidth(old_width);

cleanup:
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
}

void R_GL_DrawMapOverlayQuads(vec2_t *xz_corners, vec3_t *colors, size_t count, mat4x4_t *model, const struct map *map)
{
    struct colored_vert surf_vbuff[count * 4 * 3];
    struct colored_vert line_vbuff[count * 4 * 2];
    GLint VAO, VBO;
    GLint shader_prog;
    GLuint loc;

    vec2_t *corners_base = xz_corners;
    struct colored_vert *surf_vbuff_base = surf_vbuff;
    struct colored_vert *line_vbuff_base = line_vbuff;

    for(int i = 0; i < count; i++, xz_corners += 4, colors++) {

        vec2_t center = (vec2_t){
            (xz_corners[0].raw[0] + xz_corners[1].raw[0] + xz_corners[2].raw[0] + xz_corners[3].raw[0]) / 4.0f,
            (xz_corners[0].raw[1] + xz_corners[1].raw[1] + xz_corners[2].raw[1] + xz_corners[3].raw[1]) / 4.0f,
        };
        
        vec2_t verts[5] = {
            center,
            xz_corners[0], xz_corners[1],
            xz_corners[2], xz_corners[3], 
        };

        vec3_t verts_3d[5];

        for(int i = 0; i < ARR_SIZE(verts); i++) {
            vec4_t xz_homo = (vec4_t){verts[i].raw[0], 0.0f, verts[i].raw[1], 1.0f};
            vec4_t ws_xz_homo;
            PFM_Mat4x4_Mult4x1(model, &xz_homo, &ws_xz_homo);
            ws_xz_homo.x /= ws_xz_homo.w;
            ws_xz_homo.z /= ws_xz_homo.w;
            verts_3d[i] = (vec3_t){
                verts[i].raw[0], 
                M_HeightAtPoint(map, (vec2_t){ws_xz_homo.x, ws_xz_homo.z}) + 0.1, 
                verts[i].raw[1]
            };
        }

        vec4_t surf_color = (vec4_t){colors->x, colors->y, colors->z, 0.25};
        vec4_t line_color = (vec4_t){colors->x, colors->y, colors->z, 0.75};

        /* 4 triangles per tile */
        *surf_vbuff_base++ = (struct colored_vert){verts_3d[0], surf_color};
        *surf_vbuff_base++ = (struct colored_vert){verts_3d[1], surf_color};
        *surf_vbuff_base++ = (struct colored_vert){verts_3d[2], surf_color};

        *surf_vbuff_base++ = (struct colored_vert){verts_3d[0], surf_color};
        *surf_vbuff_base++ = (struct colored_vert){verts_3d[2], surf_color};
        *surf_vbuff_base++ = (struct colored_vert){verts_3d[3], surf_color};

        *surf_vbuff_base++ = (struct colored_vert){verts_3d[0], surf_color};
        *surf_vbuff_base++ = (struct colored_vert){verts_3d[3], surf_color};
        *surf_vbuff_base++ = (struct colored_vert){verts_3d[4], surf_color};

        *surf_vbuff_base++ = (struct colored_vert){verts_3d[0], surf_color};
        *surf_vbuff_base++ = (struct colored_vert){verts_3d[4], surf_color};
        *surf_vbuff_base++ = (struct colored_vert){verts_3d[1], surf_color};

        /* 4 lines per tile */
        *line_vbuff_base++ = (struct colored_vert){verts_3d[1], line_color};
        *line_vbuff_base++ = (struct colored_vert){verts_3d[2], line_color};

        *line_vbuff_base++ = (struct colored_vert){verts_3d[2], line_color};
        *line_vbuff_base++ = (struct colored_vert){verts_3d[3], line_color};

        *line_vbuff_base++ = (struct colored_vert){verts_3d[3], line_color};
        *line_vbuff_base++ = (struct colored_vert){verts_3d[4], line_color};

        *line_vbuff_base++ = (struct colored_vert){verts_3d[4], line_color};
        *line_vbuff_base++ = (struct colored_vert){verts_3d[1], line_color};
    }
    
    assert(surf_vbuff_base == surf_vbuff + ARR_SIZE(surf_vbuff));
    assert(line_vbuff_base == line_vbuff + ARR_SIZE(line_vbuff));

    /* OpenGL setup */
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(struct colored_vert), 
        (void*)offsetof(struct colored_vert, pos));
    glEnableVertexAttribArray(0);  

    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(struct colored_vert), 
        (void*)offsetof(struct colored_vert, color));
    glEnableVertexAttribArray(1);  

    shader_prog = R_Shader_GetProgForName("mesh.static.colored-per-vert");
    glUseProgram(shader_prog);

    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Set uniforms */
    loc = glGetUniformLocation(shader_prog, GL_U_MODEL);
    glUniformMatrix4fv(loc, 1, GL_FALSE, model->raw);

    vec4_t color4 = (vec4_t){colors[0].x, colors[0].y, colors[0].z, 0.25f};
    loc = glGetUniformLocation(shader_prog, GL_U_COLOR);
    glUniform4fv(loc, 1, color4.raw);

    /* Render surface */
    glBufferData(GL_ARRAY_BUFFER, ARR_SIZE(surf_vbuff) * sizeof(struct colored_vert), surf_vbuff, GL_STATIC_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, ARR_SIZE(surf_vbuff));

    /* Render outline */
    GLfloat old_width;
    glGetFloatv(GL_LINE_WIDTH, &old_width);
    glLineWidth(3.0f);

    glBufferData(GL_ARRAY_BUFFER, ARR_SIZE(line_vbuff) * sizeof(struct colored_vert), line_vbuff, GL_STATIC_DRAW);
    glDrawArrays(GL_LINES, 0, ARR_SIZE(line_vbuff));
    glLineWidth(old_width);

cleanup:
    glEnable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
}

void R_GL_DrawFlowField(vec2_t *xz_positions, vec2_t *xz_directions, size_t count,
                        mat4x4_t *model, const struct map *map)
{
    GLint VAO, VBO;
    GLint shader_prog;
    GLuint loc;
    vec3_t line_vbuff[count * 2];
    vec3_t point_vbuff[count];

    /* Setup line_vbuff */
    for(size_t i = 0, line_vbuff_idx = 0; i < count; i++, line_vbuff_idx += 2) {

        vec2_t tip = xz_positions[i];
        vec2_t to_add = xz_directions[i];
        PFM_Vec2_Scale(&to_add, 2.5f, &to_add);
        PFM_Vec2_Add(&tip, &to_add, &tip);

        vec4_t base_homo = (vec4_t){xz_positions[i].raw[0], 0.0f, xz_positions[i].raw[1], 1.0f};
        vec4_t tip_homo = (vec4_t){tip.raw[0], 0.0f, tip.raw[1], 1.0f};

        vec4_t base_ws_homo, tip_ws_homo;

        PFM_Mat4x4_Mult4x1(model, &base_homo, &base_ws_homo);
        base_ws_homo.x /= base_ws_homo.w;
        base_ws_homo.z /= base_ws_homo.w;

        PFM_Mat4x4_Mult4x1(model, &tip_homo, &tip_ws_homo);
        tip_ws_homo.x /= base_ws_homo.w;
        tip_ws_homo.z /= base_ws_homo.w;

        line_vbuff[line_vbuff_idx] = (vec3_t){
            xz_positions[i].raw[0],
            M_HeightAtPoint(map, (vec2_t){base_ws_homo.x, base_ws_homo.z}) + 0.3, 
            xz_positions[i].raw[1]
        };
        line_vbuff[line_vbuff_idx + 1] = (vec3_t){
            tip.raw[0],
            M_HeightAtPoint(map, (vec2_t){tip_ws_homo.x, tip_ws_homo.z}) + 0.3, 
            tip.raw[1]
        };
        point_vbuff[i] = line_vbuff[line_vbuff_idx];
    }

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

    vec4_t red = (vec4_t){1.0f, 0.0f, 0.0f, 1.0f};
    loc = glGetUniformLocation(shader_prog, GL_U_COLOR);
    glUniform4fv(loc, 1, red.raw);

    GLfloat old_width;
    glGetFloatv(GL_LINE_WIDTH, &old_width);
    glLineWidth(5.0f);
    glPointSize(10.0f);

    /* buffer & render */
    glBufferData(GL_ARRAY_BUFFER, ARR_SIZE(line_vbuff) * sizeof(vec3_t), line_vbuff, GL_STATIC_DRAW);
    glDrawArrays(GL_LINES, 0, ARR_SIZE(line_vbuff));

    glBufferData(GL_ARRAY_BUFFER, ARR_SIZE(point_vbuff) * sizeof(vec3_t), point_vbuff, GL_STATIC_DRAW);
    glDrawArrays(GL_POINTS, 0, ARR_SIZE(line_vbuff));

cleanup:
    glLineWidth(old_width);
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
}

const char *R_GL_GetInfo(enum render_info attr)
{
    switch(attr) {
    case RENDER_INFO_VENDOR:        return glGetString(GL_VENDOR);
    case RENDER_INFO_RENDERER:      return glGetString(GL_RENDERER);
    case RENDER_INFO_VERSION:       return glGetString(GL_VERSION);
    case RENDER_INFO_SL_VERSION:    return glGetString(GL_SHADING_LANGUAGE_VERSION);
    default: assert(0);             return NULL;
    }
}

void R_GL_DrawCombinedHRVO(vec2_t *apexes, vec2_t *left_rays, vec2_t *right_rays, 
                           size_t num_vos, const struct map *map)
{
    const float RAY_LEN = 150.0f;
    const int NUM_SAMPLES = 150;

    GLint VAO, VBO;
    GLint shader_prog;
    GLuint loc;

    mat4x4_t model;
    PFM_Mat4x4_Identity(&model); /* points are already in world space */

    vec3_t ray_vbuff[num_vos * (NUM_SAMPLES - 1) * 4];
    int vbuff_idx = 0;

    for(int i = 0; i < num_vos; i++) {
        for(int s = 0; s < NUM_SAMPLES-1; s++) {
        
            /* Left ray segment */
            assert(fabs(PFM_Vec2_Len(&left_rays[i]) - 1.0) < EPSILON);
            vec2_t xz_off_a = left_rays[i], xz_off_b = left_rays[i];
            PFM_Vec2_Scale(&xz_off_a, ((float)s / NUM_SAMPLES) * RAY_LEN, &xz_off_a);
            PFM_Vec2_Scale(&xz_off_b, ((float)(s + 1) / NUM_SAMPLES) * RAY_LEN, &xz_off_b);

            vec2_t xz_pos_a, xz_pos_b;
            PFM_Vec2_Add(&apexes[i], &xz_off_a, &xz_pos_a);
            PFM_Vec2_Add(&apexes[i], &xz_off_b, &xz_pos_b);
            xz_pos_a = M_ClampedMapCoordinate(map, xz_pos_a);
            xz_pos_b = M_ClampedMapCoordinate(map, xz_pos_b);

            ray_vbuff[vbuff_idx + 0] = (vec3_t){xz_pos_a.raw[0], M_HeightAtPoint(map, xz_pos_a) + 0.1, xz_pos_a.raw[1]};
            ray_vbuff[vbuff_idx + 1] = (vec3_t){xz_pos_b.raw[0], M_HeightAtPoint(map, xz_pos_b) + 0.1, xz_pos_b.raw[1]};

            /* Right ray segment */
            assert(fabs(PFM_Vec2_Len(&right_rays[i]) - 1.0) < EPSILON);
            xz_off_a = right_rays[i], xz_off_b = right_rays[i];
            PFM_Vec2_Scale(&xz_off_a, ((float)s / NUM_SAMPLES) * RAY_LEN, &xz_off_a);
            PFM_Vec2_Scale(&xz_off_b, ((float)(s + 1) / NUM_SAMPLES) * RAY_LEN, &xz_off_b);

            PFM_Vec2_Add(&apexes[i], &xz_off_a, &xz_pos_a);
            PFM_Vec2_Add(&apexes[i], &xz_off_b, &xz_pos_b);
            xz_pos_a = M_ClampedMapCoordinate(map, xz_pos_a);
            xz_pos_b = M_ClampedMapCoordinate(map, xz_pos_b);

            ray_vbuff[vbuff_idx + 2] = (vec3_t){xz_pos_a.raw[0], M_HeightAtPoint(map, xz_pos_a) + 0.1, xz_pos_a.raw[1]};
            ray_vbuff[vbuff_idx + 3] = (vec3_t){xz_pos_b.raw[0], M_HeightAtPoint(map, xz_pos_b) + 0.1, xz_pos_b.raw[1]};

            vbuff_idx += 4;
        }
    }
    assert(vbuff_idx == ARR_SIZE(ray_vbuff));

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

    vec4_t red = (vec4_t){1.0f, 0.0f, 0.0f, 1.0f};
    loc = glGetUniformLocation(shader_prog, GL_U_COLOR);
    glUniform4fv(loc, 1, red.raw);

    GLfloat old_width;
    glGetFloatv(GL_LINE_WIDTH, &old_width);
    glLineWidth(2.0f);

    /* buffer & render */
    glBufferData(GL_ARRAY_BUFFER, ARR_SIZE(ray_vbuff) * sizeof(vec3_t), ray_vbuff, GL_STATIC_DRAW);
    glDrawArrays(GL_LINES, 0, ARR_SIZE(ray_vbuff));

cleanup:
    glLineWidth(old_width);
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
}

