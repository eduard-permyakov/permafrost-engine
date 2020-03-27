/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020 Eduard Permyakov 
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

#include "gl_mesh.h"
#include "gl_vertex.h"
#include "gl_texture.h"
#include "gl_shader.h"
#include "gl_uniforms.h"
#include "gl_assert.h"
#include "gl_render.h"
#include "render_private.h"
#include "public/render.h"
#include "../game/public/game.h"
#include "../camera.h"
#include "../settings.h"
#include "../pf_math.h"
#include "../config.h"
#include "../map/public/map.h"
#include "../collision.h"
#include "../main.h"

#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#define MAX(a, b)            ((a) > (b) ? (a) : (b))
#define MIN(a, b)            ((a) < (b) ? (a) : (b))
#define ARR_SIZE(a)          (sizeof(a)/sizeof(a[0])) 
#define MINIMAP_RES          (1024)
#define MINIMAP_BORDER_CLR   ((vec4_t){65.0f/255.0f, 65.0f/255.0f, 65.0f/255.0f, 1.0f})

struct coord{
    int r, c;
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

struct render_minimap_ctx{
    struct texture minimap_texture;
    struct texture water_texture;
    struct mesh    minimap_mesh;
}s_ctx;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

void draw_cam_frustum(const struct camera *cam, mat4x4_t *minimap_model, const struct map *map)
{
    /* First, find the 4 points where the camera frustum intersects the ground plane (y=0).
     * If there is no intersection, exit early.*/
    vec3_t tr, tl, br, bl;

    struct frustum cam_frust;
    Camera_MakeFrustum(cam, &cam_frust);
    vec3_t cam_pos = Camera_GetPos(cam);

    struct plane ground_plane = {
        .point  = {0.0f, 0.0f, 0.0f},
        .normal = {0.0f, 1.0f, 0.0f},
    };

    vec3_t tr_dir, tl_dir, br_dir, bl_dir; 

    PFM_Vec3_Sub(&cam_frust.ftr, &cam_frust.ntr, &tr_dir);
    PFM_Vec3_Sub(&cam_frust.ftl, &cam_frust.ntl, &tl_dir);
    PFM_Vec3_Sub(&cam_frust.fbr, &cam_frust.nbr, &br_dir);
    PFM_Vec3_Sub(&cam_frust.fbl, &cam_frust.nbl, &bl_dir);

    PFM_Vec3_Normal(&tr_dir, &tr_dir);
    PFM_Vec3_Normal(&tl_dir, &tl_dir);
    PFM_Vec3_Normal(&br_dir, &br_dir);
    PFM_Vec3_Normal(&bl_dir, &bl_dir);

    /* When the top part of the frustum doesn't intersect the ground plane, 
     * it is still possible that a part of the map is visible by the camera.
     * In that case, we just take the intersection to be extremely far away 
     * so that we can still draw a partial visible box. */
    float t;
    if(!C_RayIntersectsPlane(cam_pos, tr_dir, ground_plane, &t))
        t = (1e10);
    PFM_Vec3_Scale(&tr_dir, t, &tr_dir);
    PFM_Vec3_Add(&cam_pos, &tr_dir, &tr);

    if(!C_RayIntersectsPlane(cam_pos, tl_dir, ground_plane, &t))
        t = (1e10);
    PFM_Vec3_Scale(&tl_dir, t, &tl_dir);
    PFM_Vec3_Add(&cam_pos, &tl_dir, &tl);

    /* When the bottom part of the frustum doesn't intersect the ground plane,
     * there is nothing to draw. */
    if(!C_RayIntersectsPlane(cam_pos, br_dir, ground_plane, &t))
        return;
    PFM_Vec3_Scale(&br_dir, t, &br_dir);
    PFM_Vec3_Add(&cam_pos, &br_dir, &br);

    if(!C_RayIntersectsPlane(cam_pos, bl_dir, ground_plane, &t))
        return;
    PFM_Vec3_Scale(&bl_dir, t, &bl_dir);
    PFM_Vec3_Add(&cam_pos, &bl_dir, &bl);

    /* Next, normalize the coordinates so that (0,0) is the exact center
     * of the map and coordinates that are visible on the minimap have 
     * components in the range [-1, 1] */
    vec2_t norm_tr = M_WorldCoordsToNormMapCoords(map, (vec2_t){tr.x, tr.z});
    vec2_t norm_tl = M_WorldCoordsToNormMapCoords(map, (vec2_t){tl.x, tl.z});
    vec2_t norm_br = M_WorldCoordsToNormMapCoords(map, (vec2_t){br.x, br.z});
    vec2_t norm_bl = M_WorldCoordsToNormMapCoords(map, (vec2_t){bl.x, bl.z});

    /* Finally, render the visible box outline. */
    const struct vertex box_verts[] = {
        (struct vertex) { .pos = (vec3_t) {norm_tr.raw[0], norm_tr.raw[1], 0.0f} },
        (struct vertex) { .pos = (vec3_t) {norm_tl.raw[0], norm_tl.raw[1], 0.0f} },
        (struct vertex) { .pos = (vec3_t) {norm_bl.raw[0], norm_bl.raw[1], 0.0f} },
        (struct vertex) { .pos = (vec3_t) {norm_br.raw[0], norm_br.raw[1], 0.0f} },
    };

    GLuint VAO, VBO;

    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, ARR_SIZE(box_verts) * sizeof(struct vertex), box_verts, GL_STATIC_DRAW);

    /* Attribute 0 - position */
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(struct vertex), (void*)0);
    glEnableVertexAttribArray(0);

    GLuint shader_prog = R_GL_Shader_GetProgForName("mesh.static.colored");
    glUseProgram(shader_prog);

    GLuint loc = glGetUniformLocation(shader_prog, GL_U_MODEL);
    glUniformMatrix4fv(loc, 1, GL_FALSE, minimap_model->raw);

    vec4_t black = (vec4_t){0.0f, 0.0f, 0.0f, 1.0f};
    vec4_t white = (vec4_t){1.0f, 1.0f, 1.0f, 1.0f};

    loc = glGetUniformLocation(shader_prog, GL_U_COLOR);
    glUniform4fv(loc, 1, black.raw);

    glDrawArrays(GL_LINE_LOOP, 0, 4);

    mat4x4_t one_px_trans, new_model;
    PFM_Mat4x4_MakeTrans(-1.0f, -1.0f, 0.0f, &one_px_trans);
    PFM_Mat4x4_Mult4x4(&one_px_trans, minimap_model, &new_model);

    loc = glGetUniformLocation(shader_prog, GL_U_MODEL);
    glUniformMatrix4fv(loc, 1, GL_FALSE, new_model.raw);
    loc = glGetUniformLocation(shader_prog, GL_U_COLOR);
    glUniform4fv(loc, 1, white.raw);

    glDrawArrays(GL_LINE_LOOP, 0, 4);

    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);
}

static void draw_minimap_terrain(struct render_private *priv, mat4x4_t *chunk_model_mat)
{
    const bool fval = false;
    R_GL_MapBegin(&fval);

    /* Clip everything below the 'Shallow Water' level. The 'Shallow Water' is 
     * rendered as just normal terrain. */
    glEnable(GL_CLIP_DISTANCE0);
    vec4_t plane_eq = (vec4_t){0.0f, 1.0f, 0.0f, Y_COORDS_PER_TILE};
    R_GL_SetClipPlane(plane_eq);

    /* Always use 'terrain' shader for rendering to not draw any shadows */
    GLuint old_shader_prog = priv->shader_prog;
    priv->shader_prog = R_GL_Shader_GetProgForName("terrain");
    R_GL_Draw(priv, chunk_model_mat); 
    priv->shader_prog = old_shader_prog;

    R_GL_MapEnd();
    glDisable(GL_CLIP_DISTANCE0);
}

/* for the minimap, we just blit a pre-rendered water texture. It is too expensive 
 * to actually render the water and still have real-time updates of the minimap. 
 */
static void draw_minimap_water(const struct map *map, struct coord cc)
{
    assert(s_ctx.water_texture.id > 0);

    struct map_resolution res;
    M_GetResolution(map, &res);

    float chunk_width_px = MIN((float)MINIMAP_RES / res.chunk_w, (float)MINIMAP_RES / res.chunk_h);
    vec2_t center = (vec2_t){MINIMAP_RES/2.0f, MINIMAP_RES/2.0f}; 
    float center_rel_r = cc.r - res.chunk_h / 2.0f;
    float center_rel_c = cc.c - res.chunk_w / 2.0f;
    glScissor(center.x + center_rel_c * chunk_width_px, 
              center.y + center_rel_r * chunk_width_px, 
              chunk_width_px, chunk_width_px);

    int viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);

    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, s_ctx.water_texture.id, 0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glReadBuffer(GL_COLOR_ATTACHMENT1);
    GLenum draw_buffs[] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, draw_buffs);

    glEnable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, MINIMAP_RES, MINIMAP_RES, /* source */
                      viewport[0], viewport[1], viewport[2], viewport[3],
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glDisable(GL_SCISSOR_TEST);
}

static void create_minimap_texture(const struct map *map, void **chunk_rprivates, 
                                   mat4x4_t *chunk_model_mats)
{
    struct map_resolution res;
    M_GetResolution(map, &res);

    GLuint fb;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);

    glGenTextures(1, &s_ctx.minimap_texture.id);
    glBindTexture(GL_TEXTURE_2D, s_ctx.minimap_texture.id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, MINIMAP_RES, MINIMAP_RES, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, s_ctx.minimap_texture.id, 0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    for(int r = 0; r < res.chunk_h; r++) {
    for(int c = 0; c < res.chunk_w; c++) {

        struct render_private *priv = chunk_rprivates[r * res.chunk_w + c];
        mat4x4_t *mat = &chunk_model_mats[r * res.chunk_w + c];

        draw_minimap_water(map, (struct coord){r,c});
        draw_minimap_terrain(priv, mat);
    }}

    glDeleteFramebuffers(1, &fb);
    GL_ASSERT_OK();

    s_ctx.minimap_texture.tunit = GL_TEXTURE0;
    R_GL_Texture_AddExisting("__minimap__", s_ctx.minimap_texture.id);
}

static void create_water_texture(const struct map *map)
{
    GLuint fb;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);

    glGenTextures(1, &s_ctx.water_texture.id);
    glBindTexture(GL_TEXTURE_2D, s_ctx.water_texture.id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, MINIMAP_RES, MINIMAP_RES, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, s_ctx.water_texture.id, 0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    vec3_t map_center = M_GetCenterPos(map);

    DECL_CAMERA_STACK(map_cam);
    memset(map_cam, 0, g_sizeof_camera);
    Camera_SetPos((struct camera*)map_cam, map_center);
    Camera_SetPitchAndYaw((struct camera*)map_cam, -90.0f, 90.0f);

    bool fval = false;
    struct render_input in = (struct render_input){
        .cam = (struct camera*)map_cam,
        .map = map,
        .shadows = false,
        .cam_vis_stat = {0},
        .cam_vis_anim = {0},
        .light_vis_stat = {0},
        .light_vis_anim = {0},
    };
    R_GL_DrawWater(&in, &fval, &fval);

    glDeleteFramebuffers(1, &fb);
    GL_ASSERT_OK();

    s_ctx.water_texture.tunit = GL_TEXTURE1;
    R_GL_Texture_AddExisting("__minimap_water__", s_ctx.water_texture.id);
}

static void setup_ortho_view_uniforms(const struct map *map)
{
    struct map_resolution res;
    M_GetResolution(map, &res);
    vec3_t map_center = M_GetCenterPos(map);
    vec2_t map_size = (vec2_t) {
        res.chunk_w * res.tile_w * X_COORDS_PER_TILE, 
        res.chunk_h * res.tile_h * Z_COORDS_PER_TILE
    };

    /* Create a new camera, with orthographic projection, centered 
     * over the map and facing straight down. */
    DECL_CAMERA_STACK(map_cam);
    memset(map_cam, 0, g_sizeof_camera);

    vec3_t offset = (vec3_t){0.0f, 200.0f, 0.0f};
    PFM_Vec3_Add(&map_center, &offset, &map_center);

    Camera_SetPos((struct camera*)map_cam, map_center);
    Camera_SetPitchAndYaw((struct camera*)map_cam, -90.0f, 90.0f);

    float map_dim = MAX(map_size.raw[0], map_size.raw[1]);
    vec2_t bot_left  = (vec2_t){ -(map_dim/2),  (map_dim/2) };
    vec2_t top_right = (vec2_t){  (map_dim/2), -(map_dim/2) };
    Camera_TickFinishOrthographic((struct camera*)map_cam, bot_left, top_right);
}

static void setup_verts(void)
{
    struct vertex map_verts[] = {
        (struct vertex) {
            .pos = (vec3_t) {-1.0f, -1.0f, 0.0f}, 
            .uv =  (vec2_t) {0.0f, 0.0f},
        },
        (struct vertex) {
            .pos = (vec3_t) {-1.0f, 1.0f, 0.0f}, 
            .uv =  (vec2_t) {0.0f, 1.0f},
        },
        (struct vertex) {
            .pos = (vec3_t) {1.0f, 1.0f, 0.0f}, 
            .uv =  (vec2_t) {1.0f, 1.0f},
        },
        (struct vertex) {
            .pos = (vec3_t) {1.0f, -1.0f, 0.0f}, 
            .uv =  (vec2_t) {1.0f, 0.0f},
        },
    };

    glGenVertexArrays(1, &s_ctx.minimap_mesh.VAO);
    glBindVertexArray(s_ctx.minimap_mesh.VAO);

    glGenBuffers(1, &s_ctx.minimap_mesh.VBO);
    glBindBuffer(GL_ARRAY_BUFFER, s_ctx.minimap_mesh.VBO);
    glBufferData(GL_ARRAY_BUFFER, ARR_SIZE(map_verts) * sizeof(struct vertex), map_verts, GL_STATIC_DRAW);

    /* Attribute 0 - position */
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(struct vertex), (void*)0);
    glEnableVertexAttribArray(0);

    /* Attribute 1 - texture coordinates */
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(struct vertex), 
        (void*)offsetof(struct vertex, uv));
    glEnableVertexAttribArray(1);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void R_GL_MinimapBake(const struct map *map, void **chunk_rprivates, 
                      mat4x4_t *chunk_model_mats)
{
    ASSERT_IN_RENDER_THREAD();
    setup_ortho_view_uniforms(map);

    /* Render the map top-down view to the texture. */
    glViewport(0,0, MINIMAP_RES, MINIMAP_RES);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    create_water_texture(map);
    create_minimap_texture(map, chunk_rprivates, chunk_model_mats);

    /* Re-bind the default framebuffer when we're done rendering */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    int width, height;
    Engine_WinDrawableSize(&width, &height);
    glViewport(0,0, width, height);

    setup_verts();

    GL_ASSERT_OK();
}

void R_GL_MinimapUpdateChunk(const struct map *map, void *chunk_rprivate, 
                             mat4x4_t *chunk_model, const int *chunk_r, const int *chunk_c)
{
    ASSERT_IN_RENDER_THREAD();
    setup_ortho_view_uniforms(map);

    /* Render the chunk to the existing minimap texture */
    GLuint fb;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);

    assert(s_ctx.minimap_texture.id > 0);
    assert(s_ctx.water_texture.id > 0);

    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, s_ctx.minimap_texture.id, 0);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    glViewport(0,0, MINIMAP_RES, MINIMAP_RES);
    draw_minimap_water(map, (struct coord){*chunk_r, *chunk_c});
    draw_minimap_terrain(chunk_rprivate, chunk_model);

    int width, height;
    Engine_WinDrawableSize(&width, &height);
    glViewport(0,0, width, height);

    /* Re-bind the default framebuffer when we're done rendering */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fb);

    GL_ASSERT_OK();
}

void R_GL_MinimapRender(const struct map *map, const struct camera *cam, 
                        vec2_t *center_pos, const int *side_len_px)
{
    ASSERT_IN_RENDER_THREAD();

    int width, height;
    Engine_WinDrawableSize(&width, &height);

    mat4x4_t res_scale;
    vec2_t mm_vres;
    M_GetMinimapAdjVres(map, &mm_vres);
    PFM_Mat4x4_MakeScale(width / mm_vres.x, height / mm_vres.y, 1.0f, &res_scale);

    center_pos->x *= (width / mm_vres.x);
    center_pos->y *= (height / mm_vres.y);

    mat4x4_t tmp;
    mat4x4_t tilt, trans, scale, model;
    PFM_Mat4x4_MakeRotZ(DEG_TO_RAD(-45.0f), &tilt);
    PFM_Mat4x4_MakeScale((*side_len_px)/2.0f, (*side_len_px)/2.0f, 1.0f, &tmp);
    PFM_Mat4x4_Mult4x4(&tmp, &res_scale, &scale);
    PFM_Mat4x4_MakeTrans(center_pos->x, center_pos->y, 0.0f, &trans);
    PFM_Mat4x4_Mult4x4(&scale, &tilt, &tmp);
    PFM_Mat4x4_Mult4x4(&trans, &tmp, &model);

    /* We scale up the quad slightly and center it in the same position, then draw it behind 
     * the minimap to create the minimap border */
    float scale_fac = ((*side_len_px) + 2*MINIMAP_BORDER_WIDTH)/2.0f;
    mat4x4_t border_scale, border_model;
    PFM_Mat4x4_MakeScale(scale_fac, scale_fac, scale_fac, &tmp);
    PFM_Mat4x4_Mult4x4(&tmp, &res_scale, &border_scale);
    PFM_Mat4x4_Mult4x4(&border_scale, &tilt, &tmp);
    PFM_Mat4x4_Mult4x4(&trans, &tmp, &border_model);

    GLuint shader_prog;
    glBindVertexArray(s_ctx.minimap_mesh.VAO);

    /* First render a slightly larger colored quad as the border */
    shader_prog = R_GL_Shader_GetProgForName("mesh.static.colored");
    glUseProgram(shader_prog);

    GLuint loc = glGetUniformLocation(shader_prog, GL_U_MODEL);
    glUniformMatrix4fv(loc, 1, GL_FALSE, border_model.raw);

    loc = glGetUniformLocation(shader_prog, GL_U_COLOR);
    glUniform4fv(loc, 1, MINIMAP_BORDER_CLR.raw);

    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

    /* Mask the minimap region in the stencil buffer before drawing the
     * camera frustum so that it is not drawn outside the minimap region. */
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_ALWAYS, 1, 0xff);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

    /* Now draw the minimap texture */
    shader_prog = R_GL_Shader_GetProgForName("mesh.static.textured");
    glUseProgram(shader_prog);

    loc = glGetUniformLocation(shader_prog, GL_U_MODEL);
    glUniformMatrix4fv(loc, 1, GL_FALSE, model.raw);

    R_GL_Texture_Activate(&s_ctx.minimap_texture, shader_prog);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

    /* Draw a box around the visible area*/
    if(cam) {
        glStencilFunc(GL_EQUAL, 1, 0xff);
        draw_cam_frustum(cam, &model, map); 
    }

    glDisable(GL_STENCIL_TEST);
    GL_ASSERT_OK();
}

void R_GL_MinimapFree(void)
{
    ASSERT_IN_RENDER_THREAD();

    assert(s_ctx.minimap_texture.id > 0);
    assert(s_ctx.minimap_mesh.VBO > 0);
    assert(s_ctx.minimap_mesh.VAO > 0);

    R_GL_Texture_Free("__minimap__");
    R_GL_Texture_Free("__minimap_water__");
    glDeleteBuffers(1, &s_ctx.minimap_mesh.VAO);
    glDeleteBuffers(1, &s_ctx.minimap_mesh.VBO);
    memset(&s_ctx, 0, sizeof(s_ctx));
}
