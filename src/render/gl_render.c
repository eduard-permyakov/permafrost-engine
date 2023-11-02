/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2023 Eduard Permyakov 
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

#include "render_private.h"
#include "gl_render.h"
#include "gl_mesh.h"
#include "gl_vertex.h"
#include "gl_shader.h"
#include "gl_material.h"
#include "gl_assert.h"
#include "gl_perf.h"
#include "gl_state.h"
#include "public/render.h"
#include "../entity.h"
#include "../camera.h"
#include "../config.h"
#include "../anim/public/skeleton.h"
#include "../anim/public/anim.h"
#include "../map/public/map.h"
#include "../lib/public/mem.h"
#include "../ui.h"
#include "../main.h"

#include <GL/glew.h>

#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <string.h>


#define EPSILON                     (1.0f/1024)
#define ARR_SIZE(a)                 (sizeof(a)/sizeof(a[0]))
#define MAX(a, b)                   ((a) > (b) ? (a) : (b))
#define MIN(a, b)                   ((a) < (b) ? (a) : (b))

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void R_GL_Init(struct render_private *priv, const char *shader, const struct vertex *vbuff)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();
    struct mesh *mesh = &priv->mesh;

    glGenVertexArrays(1, &mesh->VAO);
    glBindVertexArray(mesh->VAO);

    glGenBuffers(1, &mesh->VBO);
    glBindBuffer(GL_ARRAY_BUFFER, mesh->VBO);
    glBufferData(GL_ARRAY_BUFFER, mesh->num_verts * priv->vertex_stride, vbuff, GL_STATIC_DRAW);

    /* Attribute 0 - position */
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, priv->vertex_stride, (void*)0);
    glEnableVertexAttribArray(0);

    /* Attribute 1 - texture coordinates */
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, priv->vertex_stride, 
        (void*)offsetof(struct vertex, uv));
    glEnableVertexAttribArray(1);

    /* Attribute 2 - normal */
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, priv->vertex_stride, 
        (void*)offsetof(struct vertex, normal));
    glEnableVertexAttribArray(2);

    /* Attribute 3 - material index */
    glVertexAttribIPointer(3, 1, GL_INT, priv->vertex_stride, 
        (void*)offsetof(struct vertex, material_idx));
    glEnableVertexAttribArray(3);

    if(strstr(shader, "animated")) {

        /* Here, we use 2 attributes to pass in an array of size 6 since we are 
         * limited to a maximum of 4 components per attribute. */

        /* Attribute 4/5 - joint indices */
        glVertexAttribIPointer(4, 3, GL_UNSIGNED_BYTE, priv->vertex_stride,
            (void*)offsetof(struct anim_vert, joint_indices));
        glEnableVertexAttribArray(4);  
        glVertexAttribIPointer(5, 3, GL_UNSIGNED_BYTE, priv->vertex_stride,
            (void*)(offsetof(struct anim_vert, joint_indices) + 3*sizeof(GLubyte)));
        glEnableVertexAttribArray(5);

        /* Attribute 6/7 - joint weights */
        glVertexAttribPointer(6, 3, GL_FLOAT, GL_FALSE, priv->vertex_stride,
            (void*)offsetof(struct anim_vert, weights));
        glEnableVertexAttribArray(6);  
        glVertexAttribPointer(7, 3, GL_FLOAT, GL_FALSE, priv->vertex_stride,
            (void*)(offsetof(struct anim_vert, weights) + 3*sizeof(GLfloat)));
        glEnableVertexAttribArray(7);  

    }else if(strstr(shader, "terrain")) {

        /* Attribute 4 - blend mode */
        glVertexAttribIPointer(4, 1, GL_SHORT, priv->vertex_stride, 
            (void*)offsetof(struct terrain_vert, blend_mode));
        glEnableVertexAttribArray(4);

        /* Attribute 5 - middle material indices packed together */
        glVertexAttribIPointer(5, 1, GL_SHORT, priv->vertex_stride, 
          (void*)offsetof(struct terrain_vert, middle_indices));
        glEnableVertexAttribArray(5);

        /* Attribute 6 - corner 1 material indices packed together */
        glVertexAttribIPointer(6, 2, GL_INT, priv->vertex_stride, 
            (void*)offsetof(struct terrain_vert, c1_indices));
        glEnableVertexAttribArray(6);

        /* Attribute 7 - corner 2 material indices packed together */
        glVertexAttribIPointer(7, 2, GL_INT, priv->vertex_stride, 
            (void*)offsetof(struct terrain_vert, c2_indices));
        glEnableVertexAttribArray(7);

        /* Attribute 8 - tile top and bottom material indices packed together */
        glVertexAttribIPointer(8, 1, GL_INT, priv->vertex_stride, 
            (void*)offsetof(struct terrain_vert, tb_indices));
        glEnableVertexAttribArray(8);

        /* Attribute 9 - tile left and right material indices packed together */
        glVertexAttribIPointer(9, 1, GL_INT, priv->vertex_stride, 
            (void*)offsetof(struct terrain_vert, lr_indices));
        glEnableVertexAttribArray(9);
    }

    priv->shader_prog = R_GL_Shader_GetProgForName(shader);

    if(strstr(shader, "animated")) {
        priv->shader_prog_dp = R_GL_Shader_GetProgForName("mesh.animated.depth");
    }else {
        priv->shader_prog_dp = R_GL_Shader_GetProgForName("mesh.static.depth");
    }
    assert(priv->shader_prog != -1 && priv->shader_prog_dp != -1);

    if(priv->num_materials > 0) {
        R_GL_Texture_ArrayMake(priv->materials, priv->num_materials, &priv->material_arr, GL_TEXTURE0);
    }

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_Draw(const void *render_private, mat4x4_t *model, const bool *translucent)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();
    const struct render_private *priv = render_private;

    if(*translucent) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR);
    }

    R_GL_StateSet(GL_U_MODEL, (struct uval){
        .type = UTYPE_MAT4,
        .val.as_mat4 = *model
    });

    R_GL_StateSetComposite(GL_U_MATERIALS, (struct mdesc[]){
        { "ambient_intensity",   UTYPE_FLOAT,    offsetof(struct material, ambient_intensity) },
        { "diffuse_clr",         UTYPE_VEC3,     offsetof(struct material, diffuse_clr)       },
        { "specular_clr",        UTYPE_VEC3,     offsetof(struct material, specular_clr)      },
        {0}
    }, sizeof(struct material), priv->num_materials, priv->materials);

    R_GL_Shader_InstallProg(priv->shader_prog);

    if(priv->num_materials > 0) {
        R_GL_Texture_BindArray(&priv->material_arr, priv->shader_prog);
    }
    R_GL_ShadowMapBind();
    
    glBindVertexArray(priv->mesh.VAO);
    glDrawArrays(GL_TRIANGLES, 0, priv->mesh.num_verts);

    if(*translucent) {
        glDisable(GL_BLEND);
    }

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_BeginFrame(void)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);

    GL_PERF_RETURN_VOID();
}

void R_GL_SetViewMatAndPos(const mat4x4_t *view, const vec3_t *pos)
{
    ASSERT_IN_RENDER_THREAD();

    R_GL_StateSet(GL_U_VIEW, (struct uval){
        .type = UTYPE_MAT4,
        .val.as_mat4 = *view
    });
    R_GL_StateSet(GL_U_VIEW_POS, (struct uval){
        .type = UTYPE_VEC3,
        .val.as_vec3 = *pos
    });
}

void R_GL_SetProj(const mat4x4_t *proj)
{
    ASSERT_IN_RENDER_THREAD();

    R_GL_StateSet(GL_U_PROJECTION, (struct uval){
        .type = UTYPE_MAT4,
        .val.as_mat4 = *proj
    });
}

void R_GL_SetLightSpaceTrans(const mat4x4_t *trans)
{
    ASSERT_IN_RENDER_THREAD();

    R_GL_StateSet(GL_U_LS_TRANS, (struct uval){
        .type = UTYPE_MAT4,
        .val.as_mat4 = *trans
    });
}

void R_GL_SetClipPlane(const vec4_t plane_eq)
{
    ASSERT_IN_RENDER_THREAD();

    R_GL_StateSet(GL_U_CLIP_PLANE0, (struct uval){
        .type = UTYPE_VEC4,
        .val.as_vec4 = plane_eq
    });
}

void R_GL_SetAnimUniforms(mat4x4_t *inv_bind_poses, mat4x4_t *curr_poses, 
                          mat4x4_t *normal_mat, const size_t *count)
{
    ASSERT_IN_RENDER_THREAD();

    R_GL_StateSetArray(GL_U_INV_BIND_MATS, UTYPE_MAT4, *count, inv_bind_poses);
    R_GL_StateSetArray(GL_U_CURR_POSE_MATS, UTYPE_MAT4, *count, curr_poses);
    R_GL_StateSet(GL_U_NORMAL_MAT, (struct uval){
        .type = UTYPE_MAT4, 
        .val.as_mat4 = *normal_mat
    });
}

void R_GL_SetAmbientLightColor(const vec3_t *color)
{
    ASSERT_IN_RENDER_THREAD();

    R_GL_StateSet(GL_U_AMBIENT_COLOR, (struct uval){
        .type = UTYPE_VEC3,
        .val.as_vec3 = *color
    });
}

void R_GL_SetLightEmitColor(const vec3_t *color)
{
    ASSERT_IN_RENDER_THREAD();

    R_GL_StateSet(GL_U_LIGHT_COLOR, (struct uval){
        .type = UTYPE_VEC3,
        .val.as_vec3 = *color
    });
}

void R_GL_SetLightPos(const vec3_t *pos)
{
    ASSERT_IN_RENDER_THREAD();

    R_GL_StateSet(GL_U_LIGHT_POS, (struct uval){
        .type = UTYPE_VEC3,
        .val.as_vec3 = *pos
    });
}

void R_GL_SetScreenspaceDrawMode(void)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

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

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_DrawLoadingScreen(void)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    R_GL_SetScreenspaceDrawMode();

    int width, height;
    Engine_WinDrawableSize(&width, &height);
    struct ui_vert vbuff[] = {
        { .screen_pos = { 0, 0 },           .uv = { 0.0f, 1.0f } },
        { .screen_pos = { width, 0 },       .uv = { 1.0f, 1.0f } },
        { .screen_pos = { width, height },  .uv = { 1.0f, 0.0f } },
        { .screen_pos = { 0, height },      .uv = { 0.0f, 0.0f } }
    };
    for(int i = 0; i < ARR_SIZE(vbuff); i++) {
        vbuff[i].color[0] = 0xff;
        vbuff[i].color[1] = 0xff;
        vbuff[i].color[2] = 0xff;
        vbuff[i].color[3] = 0xff;
    }
    GLuint VAO, VBO;

    /* OpenGL setup */
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(struct ui_vert), (void*)0);
    glEnableVertexAttribArray(0);  

    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(struct ui_vert), 
        (void*)offsetof(struct ui_vert, uv));
    glEnableVertexAttribArray(1);

    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(struct ui_vert), 
        (void*)offsetof(struct ui_vert, color));
    glEnableVertexAttribArray(2);

    /* set state */
    struct texture tex = { .tunit = GL_TEXTURE0, };
    GLuint prog = R_GL_Shader_GetProgForName("ui");
    R_GL_Shader_InstallProg(prog);

    R_GL_Texture_GetOrLoad(g_basepath, CONFIG_LOADING_SCREEN, &tex.id);
    R_GL_Texture_Bind(&tex, prog);

    /* buffer & render */
    glBufferData(GL_ARRAY_BUFFER, sizeof(vbuff), vbuff, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLE_FAN, 0, ARR_SIZE(vbuff));

    /* cleanup */
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_DrawSkeleton(uint32_t uid, const struct skeleton *skel, const struct camera *cam)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    vec3_t *vbuff;
    GLuint VAO, VBO;

    int width, height;
    Engine_WinDrawableSize(&width, &height);

    mat4x4_t model;
    Entity_ModelMatrix(uid, &model);

    /* Our vbuff looks like this:
     * +----------------+-------------+--------------+-----
     * | joint root 0   | joint tip 0 | joint root 1 | ...
     * +----------------+-------------+--------------+-----
     */
    vbuff = calloc(skel->num_joints * 2, sizeof(vec3_t));

    for(int i = 0, vbuff_idx = 0; i < skel->num_joints; i++, vbuff_idx +=2) {

        struct joint *curr = &skel->joints[i];

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
    glBufferData(GL_ARRAY_BUFFER, skel->num_joints * sizeof(vec3_t) * 2, vbuff, GL_STREAM_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vec3_t), (void*)0);
    glEnableVertexAttribArray(0);  

    /* Set uniforms */
    vec4_t green = (vec4_t){0.0f, 1.0f, 0.0f, 1.0f};
    R_GL_StateSet(GL_U_COLOR, (struct uval){
        .type = UTYPE_VEC4,
        .val.as_vec4 = green
    });

    R_GL_StateSet(GL_U_MODEL, (struct uval){
        .type = UTYPE_MAT4,
        .val.as_mat4 = model
    });

    R_GL_Shader_Install("mesh.static.colored");

    glPointSize(5.0f);

    glDrawArrays(GL_POINTS, 0, skel->num_joints * 2);
    glDrawArrays(GL_LINES, 0, skel->num_joints * 2);

    /* cleanup */
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
    PF_FREE(vbuff);

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_DrawOrigin(const void *render_private, mat4x4_t *model)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    vec3_t vbuff[2];
    GLuint VAO, VBO;

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

    /* Set uniforms */
    R_GL_StateSet(GL_U_MODEL, (struct uval){
        .type = UTYPE_MAT4,
        .val.as_mat4 = *model
    });

    R_GL_Shader_Install("mesh.static.colored");

    /* Set line width */
    GLfloat old_width;
    glGetFloatv(GL_LINE_WIDTH, &old_width);
    glLineWidth(3.0f);

    /* Render the 3 axis lines at the origin */
    vbuff[0] = (vec3_t){0.0f, 0.0f, 0.0f};

    for(int i = 0; i < 3; i++) {

        switch(i) {
        case 0:
            vbuff[1] = (vec3_t){1.0f, 0.0f, 0.0f}; 
            R_GL_StateSet(GL_U_COLOR, (struct uval){
                .type = UTYPE_VEC4,
                .val.as_vec4 = red
            });
            break;
        case 1:
            vbuff[1] = (vec3_t){0.0f, 1.0f, 0.0f}; 
            R_GL_StateSet(GL_U_COLOR, (struct uval){
                .type = UTYPE_VEC4,
                .val.as_vec4 = green
            });
            break;
        case 2:
            vbuff[1] = (vec3_t){0.0f, 0.0f, 1.0f}; 
            R_GL_StateSet(GL_U_COLOR, (struct uval){
                .type = UTYPE_VEC4,
                .val.as_vec4 = blue
            });
            break;
        }
    
        R_GL_StateInstall(GL_U_COLOR, R_GL_Shader_GetCurrActive());
        glBufferData(GL_ARRAY_BUFFER, 2 * sizeof(vec3_t), vbuff, GL_STREAM_DRAW);
        glDrawArrays(GL_LINES, 0, 2);
    }
    glLineWidth(old_width);

    /* cleanup */
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_DrawRay(const vec3_t *origin, const vec3_t *dir, mat4x4_t *model, 
                  const vec3_t *color, const float *t)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    vec3_t vbuff[2];
    GLuint VAO, VBO;

    vbuff[0] = *origin; 
    vec3_t dircopy = *dir;
    PFM_Vec3_Normal(&dircopy, &dircopy);
    PFM_Vec3_Scale(&dircopy, *t, &dircopy);
    PFM_Vec3_Add((vec3_t*)origin, &dircopy, &vbuff[1]);

    /* OpenGL setup */
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vec3_t), (void*)0);
    glEnableVertexAttribArray(0);  

    /* Set uniforms */
    R_GL_StateSet(GL_U_MODEL, (struct uval){
        .type = UTYPE_MAT4,
        .val.as_mat4 = *model
    });

    vec4_t color4 = (vec4_t){color->x, color->y, color->z, 1.0f};
    R_GL_StateSet(GL_U_COLOR, (struct uval){
        .type = UTYPE_VEC4,
        .val.as_vec4 = color4
    });

    R_GL_Shader_Install("mesh.static.colored");

    GLfloat old_width;
    glGetFloatv(GL_LINE_WIDTH, &old_width);
    glLineWidth(5.0f);

    /* buffer & render */
    glBufferData(GL_ARRAY_BUFFER, 2 * sizeof(vec3_t), vbuff, GL_STREAM_DRAW);
    glDrawArrays(GL_LINES, 0, 2);

    /* cleanup */
    glLineWidth(old_width);
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_DrawOBB(const struct aabb *aabb, const mat4x4_t *model)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    GLuint VAO, VBO;
    vec4_t blue = (vec4_t){0.0f, 0.0f, 1.0f, 1.0f};

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

    /* Set uniforms */
    R_GL_StateSet(GL_U_MODEL, (struct uval){
        .type = UTYPE_MAT4,
        .val.as_mat4 = *model
    });

    R_GL_StateSet(GL_U_COLOR, (struct uval){
        .type = UTYPE_VEC4,
        .val.as_vec4 = blue
    });

    R_GL_Shader_Install("mesh.static.colored");

    /* buffer & render */
    glBufferData(GL_ARRAY_BUFFER, ARR_SIZE(vbuff) * sizeof(vec3_t), vbuff, GL_STREAM_DRAW);
    glDrawArrays(GL_LINES, 0, ARR_SIZE(vbuff));

    /* cleanup */
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_DrawBox2D(const vec2_t *screen_pos, const vec2_t *signed_size, 
                    const vec3_t *color, const float *width)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    GLuint VAO, VBO;

    vec3_t vbuff[4] = {
        (vec3_t){screen_pos->x,                  screen_pos->y,                  0.0f},
        (vec3_t){screen_pos->x + signed_size->x, screen_pos->y,                  0.0f},
        (vec3_t){screen_pos->x + signed_size->x, screen_pos->y + signed_size->y, 0.0f},
        (vec3_t){screen_pos->x,                  screen_pos->y + signed_size->y, 0.0f},
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

    /* Set uniforms */
    R_GL_StateSet(GL_U_MODEL, (struct uval){
        .type = UTYPE_MAT4,
        .val.as_mat4 = identity
    });

    vec4_t color4 = (vec4_t){color->x, color->y, color->z, 1.0f};
    R_GL_StateSet(GL_U_COLOR, (struct uval){
        .type = UTYPE_VEC4,
        .val.as_vec4 = color4
    });

    R_GL_Shader_Install("mesh.static.colored");

    float old_width;
    glGetFloatv(GL_LINE_WIDTH, &old_width);
    glLineWidth(*width);

    /* buffer & render */
    glBufferData(GL_ARRAY_BUFFER, ARR_SIZE(vbuff) * sizeof(vec3_t), vbuff, GL_STREAM_DRAW);
    glDrawArrays(GL_LINE_LOOP, 0, ARR_SIZE(vbuff));

    glLineWidth(old_width);

    /* cleanup */
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_DrawNormals(const void *render_private, mat4x4_t *model, const bool *anim)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    vec4_t yellow = (vec4_t){1.0f, 1.0f, 0.0f, 1.0f};
    R_GL_StateSet(GL_U_COLOR, (struct uval){
        .type = UTYPE_VEC4,
        .val.as_vec4 = yellow
    });

    R_GL_StateSet(GL_U_MODEL, (struct uval){
        .type = UTYPE_MAT4,
        .val.as_mat4 = *model
    });

    const struct render_private *priv = render_private;
    const char *normals_shader = *anim ? "mesh.animated.normals.colored"
                                       : "mesh.static.normals.colored";
    R_GL_Shader_Install(normals_shader);

    glBindVertexArray(priv->mesh.VAO);
    glDrawArrays(GL_TRIANGLES, 0, priv->mesh.num_verts);

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_DumpFBColor_PPM(const char *filename, const int *width, const int *height)
{
    ASSERT_IN_RENDER_THREAD();

    long img_size = (*width) * (*height) * 3;
    unsigned char *data = malloc(img_size);
    if(!data) {
        return;
    }

    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, *width, *height, GL_RGB, GL_UNSIGNED_BYTE, data);

    FILE *file = fopen(filename, "wb");
    if(!file) {
        PF_FREE(data); 
        return;
    }

    fprintf(file, "P6\n%d %d\n%d\n", *width, *height, 255);
    for(int i = 0; i < *height; i++) {
        for(int j = 0; j < *width; j++) {

            static unsigned char color[3];
            color[0] = data[3*i * (*width) + 3*j    ];
            color[1] = data[3*i * (*width) + 3*j + 1];
            color[2] = data[3*i * (*width) + 3*j + 2];
            fwrite(color, 1, 3, file);
        }
    }

    GL_ASSERT_OK();
    fclose(file);
    PF_FREE(data);
}

void R_GL_DumpFBDepth_PPM(const char *filename, const int *width, const int *height, 
                          const bool *linearize, const GLfloat *nearp, const GLfloat *farp)
{
    ASSERT_IN_RENDER_THREAD();

    long img_size = (*width) * (*height) * sizeof(GLfloat);
    GLfloat *data = malloc(img_size);
    if(!data) {
        return;
    }

    glReadPixels(0, 0, *width, *height, GL_DEPTH_COMPONENT, GL_FLOAT, data);

    FILE *file = fopen(filename, "wb");
    if(!file) {
        PF_FREE(data); 
        return;
    }

    fprintf(file, "P6\n%d %d\n%d\n", *width, *height, 255);
    for(int i = 0; i < *height; i++) {
        for(int j = 0; j < *width; j++) {

            GLfloat norm_depth = data[i * (*width) + j];
            assert(norm_depth >= 0.0f && norm_depth <= 1.0f);

            GLfloat z;
            if(*linearize) {
                z = (2 * (*nearp)) / ((*farp) + (*nearp) - norm_depth * ((*farp) - (*nearp)));
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
    PF_FREE(data);
}

void R_GL_DrawSelectionCircle(const vec2_t *xz, const float *radius, const float *width, 
                              const vec3_t *color, const struct map *map)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    GLuint VAO, VBO;

    const int NUM_SAMPLES = 48;
    const int nverts = NUM_SAMPLES * 2 + 2;
    STALLOC(vec3_t, vbuff, nverts);

    for(int i = 0; i < NUM_SAMPLES * 2; i += 2) {

        float theta = (2.0f * M_PI) * ((float)i/NUM_SAMPLES);

        float x_near = xz->x + (*radius) * cos(theta);
        float z_near = xz->z - (*radius) * sin(theta);

        float x_far = xz->x + (*radius + *width) * cos(theta);
        float z_far = xz->z - (*radius + *width) * sin(theta);
    
        float height_near = M_HeightAtPoint(map, M_ClampedMapCoordinate(map, (vec2_t){x_near, z_near}));
        float height_far  = M_HeightAtPoint(map, M_ClampedMapCoordinate(map, (vec2_t){x_far,  z_far }));

        vbuff[i]     = (vec3_t){x_near, height_near + 0.1f, z_near};
        vbuff[i + 1] = (vec3_t){x_far,  height_far + 0.1f,   z_far };
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

    /* Set uniforms */
    R_GL_StateSet(GL_U_MODEL, (struct uval){
        .type = UTYPE_MAT4,
        .val.as_mat4 = identity
    });

    vec4_t color4 = (vec4_t){color->x, color->y, color->z, 1.0f};
    R_GL_StateSet(GL_U_COLOR, (struct uval){
        .type = UTYPE_VEC4,
        .val.as_vec4 = color4
    });

    R_GL_Shader_Install("mesh.static.colored");

    /* buffer & render */
    glBufferData(GL_ARRAY_BUFFER, nverts * sizeof(vec3_t), vbuff, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, nverts);

    /* cleanup */
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);

    STFREE(vbuff);
    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_DrawSelectionRectangle(const struct obb *box, const float *width, 
                                 const vec3_t *color, const struct map *map)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    GLuint VAO, VBO;
    const float PAD = 1.0f;

    vec2_t corners[4] = {
        {box->corners[0].x, box->corners[0].z},
        {box->corners[1].x, box->corners[1].z},
        {box->corners[5].x, box->corners[5].z},
        {box->corners[4].x, box->corners[4].z},
    };

    float lens[4];
    vec2_t deltas[4];
    float tot_len = 0;

    PFM_Vec2_Sub(&corners[1], &corners[0], &deltas[0]);
    PFM_Vec2_Sub(&corners[2], &corners[1], &deltas[1]);
    PFM_Vec2_Sub(&corners[3], &corners[2], &deltas[2]);
    PFM_Vec2_Sub(&corners[0], &corners[3], &deltas[3]);

    for(int i = 0; i < 4; i++) {
        lens[i] = PFM_Vec2_Len(&deltas[i]);
        tot_len += lens[i];
        PFM_Vec2_Normal(&deltas[i], &deltas[i]);
    }

    float sample_dist = MIN(X_COORDS_PER_TILE, Z_COORDS_PER_TILE);
    size_t nsamples = 0;
    for(int i = 0; i < 4; i++) {
        nsamples += ceil(lens[i] / sample_dist) + 1;
    }

    const int nverts = nsamples * 2 + 2;
    STALLOC(vec3_t, vbuff, nverts);
    int vbuff_idx = 0;

    for(int i = 0; i < 4; i++) {

        vec3_t pdir = (vec3_t){-deltas[i].z, 0.0f, deltas[i].x};
        PFM_Vec3_Scale(&pdir, *width/2.0f, &pdir);

        for(int j = 0; j < ceil(lens[i] / sample_dist) + 1; j++) {

            vec2_t dir = deltas[i];
            PFM_Vec2_Scale(&dir, MIN(j * sample_dist, lens[i]), &dir);

            vec2_t xz;
            PFM_Vec2_Add(&corners[i], &dir, &xz);

            vec3_t point = (vec3_t){
                xz.x, 
                M_HeightAtPoint(map, M_ClampedMapCoordinate(map, xz)) + 0.1f, 
                xz.z
            };

            vec3_t nudge = (vec3_t){-deltas[i].z, 0.0f, deltas[i].x}, nudged;
            PFM_Vec3_Scale(&nudge, PAD, &nudge);
            PFM_Vec3_Add(&point, &nudge, &nudged);

            PFM_Vec3_Sub(&nudged, &pdir, &vbuff[vbuff_idx++]);
            PFM_Vec3_Add(&nudged, &pdir, &vbuff[vbuff_idx++]);
        }
    }
    assert(vbuff_idx == nsamples * 2);

    vbuff[nsamples * 2 + 0] = vbuff[0];
    vbuff[nsamples * 2 + 1] = vbuff[1];

    mat4x4_t identity;
    PFM_Mat4x4_Identity(&identity);

    /* OpenGL setup */
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vec3_t), (void*)0);
    glEnableVertexAttribArray(0);  

    /* Set uniforms */
    R_GL_StateSet(GL_U_MODEL, (struct uval){
        .type = UTYPE_MAT4,
        .val.as_mat4 = identity
    });

    vec4_t color4 = (vec4_t){color->x, color->y, color->z, 1.0f};
    R_GL_StateSet(GL_U_COLOR, (struct uval){
        .type = UTYPE_VEC4,
        .val.as_vec4 = color4
    });

    R_GL_Shader_Install("mesh.static.colored");

    /* buffer & render */
    glBufferData(GL_ARRAY_BUFFER, nverts * sizeof(vec3_t), vbuff, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, nverts);

    /* cleanup */
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);

    STFREE(vbuff);
    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_DrawLine(vec2_t endpoints[], const float *width, const vec3_t *color, const struct map *map)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    vec2_t delta;
    PFM_Vec2_Sub(&endpoints[1], &endpoints[0], &delta);
    const float len = PFM_Vec2_Len(&delta);

    vec2_t perp = (vec2_t){ delta.z, -delta.x };
    PFM_Vec2_Normal(&perp, &perp);
    assert(*width > 0.0f);
    PFM_Vec2_Scale(&perp, *width/2.0f, &perp);

    const int NUM_SAMPLES = ceil(len / 4.0f);
    const int nverts = NUM_SAMPLES * 2 + 2;
    STALLOC(vec3_t, vbuff, nverts);
    float t = 0.0f;

    for(int i = 0; i <= NUM_SAMPLES * 2; i += 2) {

        PFM_Vec2_Sub(&endpoints[1], &endpoints[0], &delta);
        PFM_Vec2_Normal(&delta, &delta);
        PFM_Vec2_Scale(&delta, t, &delta);

        vec2_t point;
        PFM_Vec2_Add(&endpoints[0], &delta, &point);

        vec2_t point_left, point_right;
        PFM_Vec2_Add(&point, &perp, &point_left);
        PFM_Vec2_Sub(&point, &perp, &point_right);

        float height_left = M_HeightAtPoint(map, M_ClampedMapCoordinate(map, point_left));
        float height_right = M_HeightAtPoint(map, M_ClampedMapCoordinate(map, point_right));
        float height = MAX(height_left, height_right);

        vbuff[i + 0] = (vec3_t){point_left.x, height + 0.2f, point_left.z};
        vbuff[i + 1] = (vec3_t){point_right.x, height + 0.2f, point_right.z};

        t += (1.0f / NUM_SAMPLES) * len;
    }

    mat4x4_t identity;
    PFM_Mat4x4_Identity(&identity);

    /* OpenGL setup */
    GLuint VAO, VBO;

    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vec3_t), (void*)0);
    glEnableVertexAttribArray(0);  

    /* Set uniforms */
    R_GL_StateSet(GL_U_MODEL, (struct uval){
        .type = UTYPE_MAT4,
        .val.as_mat4 = identity
    });

    vec4_t color4 = (vec4_t){color->x, color->y, color->z, 1.0f};
    R_GL_StateSet(GL_U_COLOR, (struct uval){
        .type = UTYPE_VEC4,
        .val.as_vec4 = color4
    });

    R_GL_Shader_Install("mesh.static.colored");

    float old_width;
    glGetFloatv(GL_LINE_WIDTH, &old_width);
    glLineWidth(*width);

    /* buffer & render */
    glBufferData(GL_ARRAY_BUFFER, nverts * sizeof(vec3_t), vbuff, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, nverts);

    glLineWidth(old_width);

    /* cleanup */
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);

    STFREE(vbuff);
    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_DrawQuad(vec2_t corners[], const float *width, const vec3_t *color, const struct map *map)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    vec2_t lines[][2] = {
        corners[0], corners[1],
        corners[1], corners[2],
        corners[2], corners[3],
        corners[3], corners[0],
    };

    for(int i = 0; i < ARR_SIZE(lines); i++)
        R_GL_DrawLine(lines[i], width, color, map);

    GL_PERF_RETURN_VOID();
}

void R_GL_DrawMapOverlayQuads(vec2_t *xz_corners, vec3_t *colors, const size_t *count, 
                              mat4x4_t *model, const struct map *map)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    GLuint VAO, VBO;
    const size_t surf_verts = *count * 4 * 3;
    const size_t line_verts = *count * 4 * 2;
    STALLOC(struct colored_vert, surf_vbuff, surf_verts);
    STALLOC(struct colored_vert, line_vbuff, line_verts);

    struct colored_vert *surf_vbuff_base = surf_vbuff;
    struct colored_vert *line_vbuff_base = line_vbuff;

    for(int i = 0; i < *count; i++, xz_corners += 4, colors++) {

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
    
    assert(surf_vbuff_base == surf_vbuff + *count * 4 * 3);
    assert(line_vbuff_base == line_vbuff + *count * 4 * 2);

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

    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Set uniforms */
    R_GL_StateSet(GL_U_MODEL, (struct uval){
        .type = UTYPE_MAT4,
        .val.as_mat4 = *model
    });

    vec4_t color4 = (vec4_t){colors[0].x, colors[0].y, colors[0].z, 0.25f};
    R_GL_StateSet(GL_U_COLOR, (struct uval){
        .type = UTYPE_VEC4,
        .val.as_vec4 = color4
    });

    R_GL_Shader_Install("mesh.static.colored-per-vert");

    /* Render surface */
    glBufferData(GL_ARRAY_BUFFER, surf_verts * sizeof(struct colored_vert), surf_vbuff, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, surf_verts);

    /* Render outline */
    GLfloat old_width;
    glGetFloatv(GL_LINE_WIDTH, &old_width);
    glLineWidth(3.0f);

    glBufferData(GL_ARRAY_BUFFER, line_verts * sizeof(struct colored_vert), line_vbuff, GL_STREAM_DRAW);
    glDrawArrays(GL_LINES, 0, line_verts);
    glLineWidth(old_width);

    /* cleanup */
    glEnable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);

    STFREE(surf_vbuff);
    STFREE(line_vbuff);

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_DrawFlowField(vec2_t *xz_positions, vec2_t *xz_directions, const size_t *count,
                        mat4x4_t *model, const struct map *map)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    GLuint VAO, VBO;
    const size_t line_vbuff_size = *count * 2;
    const size_t point_vbuff_size = *count;
    STALLOC(vec3_t, line_vbuff, *count * 2);
    STALLOC(vec3_t, point_vbuff, *count);

    /* Setup line_vbuff */
    for(size_t i = 0, line_vbuff_idx = 0; i < *count; i++, line_vbuff_idx += 2) {

        vec2_t tip = xz_positions[i];
        vec2_t to_add = xz_directions[i];
        PFM_Vec2_Scale(&to_add, 2.5f, &to_add);
        PFM_Vec2_Add(&tip, &to_add, &tip);

        vec4_t base_homo = (vec4_t){xz_positions[i].x, 0.0f, xz_positions[i].z, 1.0f};
        vec4_t tip_homo = (vec4_t){tip.x, 0.0f, tip.z, 1.0f};

        vec4_t base_ws_homo, tip_ws_homo;

        PFM_Mat4x4_Mult4x1(model, &base_homo, &base_ws_homo);
        base_ws_homo.x /= base_ws_homo.w;
        base_ws_homo.z /= base_ws_homo.w;

        PFM_Mat4x4_Mult4x1(model, &tip_homo, &tip_ws_homo);
        tip_ws_homo.x /= base_ws_homo.w;
        tip_ws_homo.z /= base_ws_homo.w;

        line_vbuff[line_vbuff_idx] = (vec3_t){
            xz_positions[i].x,
            M_HeightAtPoint(map, M_ClampedMapCoordinate(map, (vec2_t){base_ws_homo.x, base_ws_homo.z})) + 0.3, 
            xz_positions[i].z
        };
        line_vbuff[line_vbuff_idx + 1] = (vec3_t){
            tip.x,
            M_HeightAtPoint(map, M_ClampedMapCoordinate(map, (vec2_t){tip_ws_homo.x, tip_ws_homo.z})) + 0.3, 
            tip.z
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

    /* Set uniforms */
    R_GL_StateSet(GL_U_MODEL, (struct uval){
        .type = UTYPE_MAT4,
        .val.as_mat4 = *model
    });

    vec4_t red = (vec4_t){1.0f, 0.0f, 0.0f, 1.0f};
    R_GL_StateSet(GL_U_COLOR, (struct uval){
        .type = UTYPE_VEC4,
        .val.as_vec4 = red
    });

    R_GL_Shader_Install("mesh.static.colored");

    GLfloat old_width;
    glGetFloatv(GL_LINE_WIDTH, &old_width);
    glLineWidth(5.0f);
    glPointSize(10.0f);

    /* buffer & render */
    glBufferData(GL_ARRAY_BUFFER, line_vbuff_size * sizeof(vec3_t), line_vbuff, GL_STREAM_DRAW);
    glDrawArrays(GL_LINES, 0, line_vbuff_size);

    glBufferData(GL_ARRAY_BUFFER, point_vbuff_size * sizeof(vec3_t), point_vbuff, GL_STREAM_DRAW);
    glDrawArrays(GL_POINTS, 0, point_vbuff_size);

    /* cleanup */
    glLineWidth(old_width);
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);

    STFREE(line_vbuff);
    STFREE(point_vbuff);

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_DrawCombinedHRVO(vec2_t *apexes, vec2_t *left_rays, vec2_t *right_rays, 
                           const size_t *num_vos, const struct map *map)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    const float RAY_LEN = 150.0f;
    const int NUM_SAMPLES = 150;

    GLuint VAO, VBO;

    mat4x4_t model;
    PFM_Mat4x4_Identity(&model); /* points are already in world space */

    size_t vbuff_size = *num_vos * (NUM_SAMPLES - 1) * 4;
    STALLOC(vec3_t, ray_vbuff, vbuff_size);
    int vbuff_idx = 0;

    for(int i = 0; i < *num_vos; i++) {
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
    assert(vbuff_idx == vbuff_size);

    /* OpenGL setup */
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vec3_t), (void*)0);
    glEnableVertexAttribArray(0);

    /* Set uniforms */
    R_GL_StateSet(GL_U_MODEL, (struct uval){
        .type = UTYPE_MAT4,
        .val.as_mat4 = model
    });

    vec4_t red = (vec4_t){1.0f, 0.0f, 0.0f, 1.0f};
    R_GL_StateSet(GL_U_COLOR, (struct uval){
        .type = UTYPE_VEC4,
        .val.as_vec4 = red
    });

    R_GL_Shader_Install("mesh.static.colored");

    GLfloat old_width;
    glGetFloatv(GL_LINE_WIDTH, &old_width);
    glLineWidth(2.0f);

    /* buffer & render */
    glBufferData(GL_ARRAY_BUFFER, vbuff_size * sizeof(vec3_t), ray_vbuff, GL_STREAM_DRAW);
    glDrawArrays(GL_LINES, 0, vbuff_size);

    /* cleanup */
    glLineWidth(old_width);
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);

    STFREE(ray_vbuff);
    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_SetViewport(int *x, int *y, int *w, int *h)
{
    GL_PERF_ENTER();
    glViewport(*x, *y, *w, *h);
    GL_PERF_RETURN_VOID();
}

void R_GL_GlobalConfig(void)
{
    GL_PERF_ENTER();

    glProvokingVertex(GL_FIRST_VERTEX_CONVENTION); 
    glFrontFace(GL_CW);
    glCullFace(GL_BACK);

    GL_PERF_RETURN_VOID();
}

void R_GL_TimestampForCookie(uint32_t *cookie, uint64_t *out)
{
    GLuint timer_query = *cookie;

    GLint avail = GL_FALSE;
    glGetQueryObjectiv(timer_query, GL_QUERY_RESULT_AVAILABLE, &avail);

    if(!avail) {
        PRINT("WARNING: Timestamp query result not yet available. "
              "This may negatively impact performance.\n");
    }

    glGetQueryObjectui64v(timer_query, GL_QUERY_RESULT, out);
    glDeleteQueries(1, &timer_query);
}

