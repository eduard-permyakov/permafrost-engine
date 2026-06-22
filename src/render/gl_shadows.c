/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2026 Eduard Permyakov 
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

#define MEM_FILE_SYS MEM_SYS_RENDER
#define MEM_FILE_SUB MEM_SUB_RENDER_GL_SHADOWS

#include "public/render.h"
#include "render_private.h"
#include "gl_render.h"
#include "gl_batch.h"
#include "gl_state.h"
#include "gl_assert.h"

#define GPU_MEM_FILE_SYS GPU_MEM_SYS_GL_SHADOWS
#include "gl_mem.h"
#include "gl_shader.h"
#include "gl_perf.h"
#include "gl_anim.h"
#include "../main.h"
#include "../pf_math.h"
#include "../config.h"
#include "../settings.h"
#include "../camera.h"
#include "../phys/public/collision.h"
#include "../game/public/game.h"

#include <GL/glew.h>
#include <SDL.h>
#include <assert.h>

#include "../mem.h"

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_SHADOWS)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_SHADOWS)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_SHADOWS)


#define LIGHT_EXTRA_HEIGHT    (300.0f)

/* Extra world-space padding added to each side of the fitted light frustum so
 * that objects just outside the visible area can still cast shadows into it.
 */
#define SHADOW_XY_BUFFER      (64.0f)

/* Padding along the light direction so the near plane clears tall casters above
 * the visible area and the far plane clears terrain dipping below it.
 */
#define SHADOW_DEPTH_BUFFER   (150.0f)

struct shadow_gl_state{
    GLint viewport[4];
    GLint fb;
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static GLuint         s_depth_map_FBO;
static GLuint         s_depth_map_tex;
static bool           s_depth_pass_active = false;
static struct shadow_gl_state s_saved;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

/* The light's view-space basis and the orthographic half-extents fitted to the
 * current camera's visible area. The depth-pass projection and the shadow-caster
 * cull frustum are both derived from this, so they always agree. */
struct light_frustum_fit{
    vec3_t   origin;
    vec3_t   up;
    vec3_t   dir;
    mat4x4_t view;
    float    half_x;
    float    half_y;
    float    near_d;
    float    far_d;
};

/* The four points where the camera's view frustum meets the ground plane - the
 * area the player currently sees, which the shadow frustum must cover. */
static void cam_ground_footprint(const struct camera *cam, vec3_t out[4])
{
    struct frustum frust;
    Camera_MakeFrustum(cam, &frust);

    const struct plane ground = (struct plane){
        .point  = (vec3_t){0.0f, 0.0f, 0.0f},
        .normal = (vec3_t){0.0f, 1.0f, 0.0f},
    };

    vec3_t near_c[4] = {frust.ntl, frust.ntr, frust.nbl, frust.nbr};
    vec3_t far_c[4]  = {frust.ftl, frust.ftr, frust.fbl, frust.fbr};

    for(int i = 0; i < 4; i++) {
        vec3_t dir;
        PFM_Vec3_Sub(&far_c[i], &near_c[i], &dir);
        PFM_Vec3_Normal(&dir, &dir);

        float t;
        if(!C_RayIntersectsPlane(near_c[i], dir, ground, &t))
            t = CONFIG_SHADOW_DRAWDIST;

        vec3_t delta;
        PFM_Vec3_Scale(&dir, t, &delta);
        PFM_Vec3_Add(&near_c[i], &delta, &out[i]);
    }
}

static struct light_frustum_fit make_light_fit(vec3_t light_pos, const struct camera *cam)
{
    vec3_t cam_pos = Camera_GetPos(cam);
    vec3_t cam_dir = Camera_GetDir(cam);

    float t = cam_pos.y / cam_dir.y;
    vec3_t cam_ray_ground_isec = (vec3_t){cam_pos.x - t * cam_dir.x, 0.0f, cam_pos.z - t * cam_dir.z};

    vec3_t light_dir = light_pos;
    PFM_Vec3_Normal(&light_dir, &light_dir);
    PFM_Vec3_Scale(&light_dir, -1.0f, &light_dir);

    vec3_t right = (vec3_t){-1.0f, 0.0f, 0.0f}, up;
    PFM_Vec3_Cross(&light_dir, &right, &up);
    PFM_Vec3_Normal(&up, &up);

    t = fabs((cam_pos.y + LIGHT_EXTRA_HEIGHT) / light_dir.y);
    vec3_t light_origin, delta;
    PFM_Vec3_Scale(&light_dir, -t, &delta);
    PFM_Vec3_Add(&cam_ray_ground_isec, &delta, &light_origin);

    /* Since, for shadow mapping, we treat our light source as a directional light,
     * we only care about direction of the light rays, not the absolute position of
     * the light source. Thus, we render the shadow map from a fixed height, looking
     * at the position where the camera ray intersects the ground plane.
     */
    vec3_t target;
    PFM_Vec3_Add(&light_origin, &light_dir, &target);
    mat4x4_t view;
    PFM_Mat4x4_MakeLookAt(&light_origin, &target, &up, &view);

    /* Fit the orthographic extents to the visible ground area (transformed into
     * light space), plus a buffer, capped to a sane maximum. */
    vec3_t footprint[4];
    cam_ground_footprint(cam, footprint);

    float half_x = 0.0f, half_y = 0.0f;
    float min_d = 1e30f, max_d = 0.0f;
    for(int i = 0; i < 4; i++) {
        vec4_t world = (vec4_t){footprint[i].x, footprint[i].y, footprint[i].z, 1.0f};
        vec4_t ls;
        PFM_Mat4x4_Mult4x1(&view, &world, &ls);
        half_x = fmax(half_x, fabs(ls.x));
        half_y = fmax(half_y, fabs(ls.y));
        /* The light looks down -Z in view space, so distance along the light
         * direction is -ls.z. */
        min_d = fmin(min_d, -ls.z);
        max_d = fmax(max_d, -ls.z);
    }
    half_x = fmin(half_x + SHADOW_XY_BUFFER, CONFIG_SHADOW_MAX_EXTENT);
    half_y = fmin(half_y + SHADOW_XY_BUFFER, CONFIG_SHADOW_MAX_EXTENT);

    float near_d = fmax(1.0f, min_d - SHADOW_DEPTH_BUFFER);
    float far_d  = fmin(max_d + SHADOW_DEPTH_BUFFER, CONFIG_SHADOW_DRAWDIST);

    return (struct light_frustum_fit){
        .origin = light_origin,
        .up     = up,
        .dir    = light_dir,
        .view   = view,
        .half_x = half_x,
        .half_y = half_y,
        .near_d = near_d,
        .far_d  = far_d,
    };
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void R_GL_InitShadows(void)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    glGenTextures(1, &s_depth_map_tex);
    glBindTexture(GL_TEXTURE_2D, s_depth_map_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32, 
                 CONFIG_SHADOW_MAP_RES, CONFIG_SHADOW_MAP_RES, 
                 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    /* Don't enable deptph comparisons as we will use a sampler2D and 
     * manually perform comparison and filtering in the shader.
     */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    GLint old;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old);

    glGenFramebuffers(1, &s_depth_map_FBO);
    glBindFramebuffer(GL_FRAMEBUFFER, s_depth_map_FBO);

    glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, s_depth_map_tex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    glBindFramebuffer(GL_FRAMEBUFFER, old);  
    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_DepthPassBegin(const vec3_t *light_pos, const struct camera *cam)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();
    GL_PERF_PUSH_GROUP(0, "depth pass");

    assert(!s_depth_pass_active);
    s_depth_pass_active = true;

    glGetIntegerv(GL_VIEWPORT, s_saved.viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s_saved.fb);

    struct light_frustum_fit fit = make_light_fit(*light_pos, cam);

    mat4x4_t light_proj;
    PFM_Mat4x4_MakeOrthographic(-fit.half_x, fit.half_x,
        fit.half_y, -fit.half_y, fit.near_d, fit.far_d, &light_proj);

    mat4x4_t light_space_trans;
    PFM_Mat4x4_Mult4x4(&light_proj, &fit.view, &light_space_trans);
    R_GL_SetLightSpaceTrans(&light_space_trans);

    glViewport(0, 0, CONFIG_SHADOW_MAP_RES, CONFIG_SHADOW_MAP_RES);
    glBindFramebuffer(GL_FRAMEBUFFER, s_depth_map_FBO);
    glClear(GL_DEPTH_BUFFER_BIT);
    glCullFace(GL_FRONT);

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_DepthPassEnd(void)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    assert(s_depth_pass_active);
    s_depth_pass_active = false;

    glViewport(s_saved.viewport[0], s_saved.viewport[1], s_saved.viewport[2], s_saved.viewport[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, s_saved.fb);
    glCullFace(GL_BACK);

    GL_PERF_POP_GROUP();
    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_RenderDepthMap(const void *render_private, mat4x4_t *model)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();
    assert(s_depth_pass_active);

    R_GL_StateSet(GL_U_MODEL, (struct uval){
        .type = UTYPE_MAT4,
        .val.as_mat4 = *model
    });

    const struct render_private *priv = render_private;
    R_GL_CALL(R_GL_Shader_InstallProg(priv->shader_prog_dp));
    R_GL_CALL(R_GL_AnimBindPoseBuff());

    GLint first = 0;
    if(priv->mesh.type == MESH_TYPE_BATCHED_INDIRECT) {
        first = R_GL_Batch_MeshBindVAO(priv);
    }else{
        R_GL_CALL(glBindVertexArray(priv->mesh.VAO));
    }
    R_GL_CALL(glDrawArrays(GL_TRIANGLES, first, priv->mesh.num_verts));

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_ShadowMapBind(void)
{
    R_GL_StateSet(GL_U_SHADOW_MAP, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = SHADOW_MAP_TUNIT - GL_TEXTURE0
    });
    R_GL_StateInstall(GL_U_SHADOW_MAP, R_GL_Shader_GetCurrActive());
    glActiveTexture(SHADOW_MAP_TUNIT);
    glBindTexture(GL_TEXTURE_2D, s_depth_map_tex);
    GL_ASSERT_OK();
}

void R_GL_SetShadowsEnabled(const bool *on)
{
    R_GL_StateSet(GL_U_SHADOWS_ON, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = *on
    });
}

void R_LightVisibilityFrustum(const struct camera *cam, vec3_t light_pos, struct frustum *out)
{
    /* The set of shadow casters is exactly the geometry inside the light's
     * orthographic frustum, so cull against the same box the depth pass uses. */
    struct light_frustum_fit fit = make_light_fit(light_pos, cam);
    C_MakeFrustumOrthographic(fit.origin, fit.up, fit.dir,
        fit.half_x, fit.half_y, fit.near_d, fit.far_d, out);
}

