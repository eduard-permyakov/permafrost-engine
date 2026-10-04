/*
 *  This file is part of Permafrost Engine.
 *  Copyright (C) 2026 Eduard Permyakov
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
#define MEM_FILE_SUB MEM_SUB_RENDER_GL_BILLBOARD

#include "gl_billboard.h"
#include "gl_assert.h"

#define GPU_MEM_FILE_SYS GPU_MEM_SYS_GL_BILLBOARD
#include "gl_mem.h"
#include "gl_shader.h"
#include "gl_state.h"
#include "gl_perf.h"
#include "public/render.h"
#include "public/render_ctrl.h"
#include "../asset_cache.h"
#include "../entity.h"
#include "../settings.h"
#include "../main.h"
#include "../mem.h"
#include "../game/public/game.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/khash.h"

#include <assert.h>
#include <string.h>
#include <stddef.h>
#include <math.h>

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_BILLBOARD)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_BILLBOARD)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_BILLBOARD)

#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#define MAX(a, b)           ((a) > (b) ? (a) : (b))
#define ARR_SIZE(a)         (sizeof(a)/sizeof(a[0]))

/* Conservative floor mandated by GL 3.3 for GL_MAX_ARRAY_TEXTURE_LAYERS,
 * used until the render context has published the real limit.
 */
#define FALLBACK_MAX_LAYERS (256)
#define BB_BAKE_NEAR        (0.1f)

/* The GPU-side per-instance record streamed to the instance VBO */
struct bb_gpu_inst{
    vec3_t  pos;
    float   yaw;
    vec2_t  scale;
    int32_t cell_base;
    int32_t pad;
};

KHASH_MAP_INIT_INT64(bbdesc, struct bb_model_desc*)

static uint64_t bb_mix(uint64_t hash, uint64_t val);
static int      bb_azimuth_count(const struct aabb *aabb, int nclips);
static vec3_t   bb_bake_frame_light(vec3_t light_pos);
static float    bb_depth_extent(const struct bb_model_desc *desc);
static bool     bb_alloc_atlas(struct bb_model_desc *desc, const void *pixels,
                               const void *depth);
static bool     bb_bake_atlas(struct bb_model_desc *desc, const void *render_private,
                              const struct bb_variant *var, const char *cache_name,
                              uint64_t tag);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

/* Main thread. The wanted variant is refreshed lazily; its generation number
 * only advances on a material change, so light jitter below the thresholds
 * never triggers a re-bake.
 */
static khash_t(bbdesc) *s_desc_table;
static struct bb_variant s_wanted;
static int              s_wanted_gen;

/* Written once by the render thread at context init */
static SDL_atomic_t     s_max_layers;

/* Render thread */
static GLuint           s_quad_vbo;
static GLuint           s_inst_vbo;
static GLuint           s_vao;
static size_t           s_inst_cap;
static struct bb_gpu_inst *s_inst_scratch;
static size_t           s_inst_scratch_cap;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static uint64_t bb_mix(uint64_t hash, uint64_t val)
{
    return (hash ^ val) * 1099511628211ull;
}

static int bb_pow2_cell_res(float px)
{
    int res = CONFIG_BILLBOARD_MIN_RES;
    while(res < px && res < CONFIG_BILLBOARD_MAX_RES) {
        res *= 2;
    }
    return res;
}

/* The bake camera's elevation: the complement of the tilt setting, which
 * counts up from looking straight down.
 */
static float bb_tilt_rad(void)
{
    struct sval tilt;
    float degrees = 45.0f;
    if(Settings_Get("pf.game.camera_tilt", &tilt) == SS_OKAY) {
        degrees = tilt.as_int;
    }
    return DEG_TO_RAD(90.0f - degrees);
}

/* Accumulate one model-space AABB into the rotation-invariant framing metrics:
 * the horizontal radius about the Y axis and the vertical range.
 */
static void bb_extend_metrics(const struct aabb *aabb, float *inout_radius,
                              float *inout_ymin, float *inout_ymax)
{
    float xext = MAX(fabsf(aabb->x_min), fabsf(aabb->x_max));
    float zext = MAX(fabsf(aabb->z_min), fabsf(aabb->z_max));
    float radius = sqrtf(xext * xext + zext * zext);

    *inout_radius = MAX(*inout_radius, radius);
    *inout_ymin = MIN(*inout_ymin, aabb->y_min);
    *inout_ymax = MAX(*inout_ymax, aabb->y_max);
}

/* Elongated static footprints take the finer azimuth set: a coarse bin
 * visibly rotates a long sprite about its centre. Animated bind-pose
 * bounds are T-poses, so those keep the coarse set.
 */
static int bb_azimuth_count(const struct aabb *aabb, int nclips)
{
    if(nclips > 0)
        return CONFIG_BILLBOARD_AZIMUTHS;

    float xspan = aabb->x_max - aabb->x_min;
    float zspan = aabb->z_max - aabb->z_min;
    float major = MAX(xspan, zspan);
    float minor = MAX(MIN(xspan, zspan), 1e-3f);

    if(major / minor >= CONFIG_BILLBOARD_ELONGATED_ASPECT)
        return CONFIG_BILLBOARD_AZIMUTHS_ELONGATED;
    return CONFIG_BILLBOARD_AZIMUTHS;
}

/* The frame index baked for a keyframe slot: the start of its bucket, matching
 * the runtime frame-to-keyframe mapping in R_Billboard_CellBase.
 */
static int bb_bake_frame(int kf, int nkf, int nframes)
{
    return MIN((kf * nframes) / nkf, nframes - 1);
}

static struct bb_model_desc *bb_desc_for_key(void *render_key)
{
    khiter_t k = kh_get(bbdesc, s_desc_table, (khint64_t)(uintptr_t)render_key);
    if(k == kh_end(s_desc_table))
        return NULL;
    return kh_value(s_desc_table, k);
}

/* The bake camera views from azimuth pi while the RTS camera views from
 * atan(front.x, front.z); the light takes the same Y rotation that carries
 * one onto the other, so its direction relative to the camera is kept. The
 * fixed RTS yaw is used rather than the active camera's: a cinematic camera
 * would otherwise re-bake every atlas on each switch.
 */
static vec3_t bb_bake_frame_light(vec3_t light_pos)
{
    float yaw = DEG_TO_RAD(CONFIG_RTS_CAMERA_YAW_DEG);
    float delta = M_PI - atan2f(cosf(yaw), -sinf(yaw));
    float c = cosf(delta), s = sinf(delta);

    return (vec3_t){
        light_pos.x * c + light_pos.z * s,
        light_pos.y,
        -light_pos.x * s + light_pos.z * c,
    };
}

static struct bb_variant bb_current_variant(void)
{
    struct sval setting;
    bool shadowed = (Settings_Get("pf.video.shadows_enabled", &setting) == SS_OKAY)
                  && setting.as_bool;
    if(!shadowed) {
        /* Without shadows the sprites bake with a canonical light, keeping the
         * atlases valid across maps.
         */
        return (struct bb_variant){
            .shadowed = false,
            .light_pos = (vec3_t){500.0f, 1000.0f, 500.0f},
            .ambient_color = (vec3_t){1.0f, 1.0f, 1.0f},
            .emit_color = (vec3_t){1.0f, 1.0f, 1.0f},
        };
    }
    /* Self-shadowing bakes the shadow direction in, so the shadowed variant
     * must follow the map's actual light.
     */
    return (struct bb_variant){
        .shadowed = true,
        .light_pos = bb_bake_frame_light(G_GetLightPos()),
        .ambient_color = G_GetAmbientLightColor(),
        .emit_color = G_GetEmitLightColor(),
    };
}

static bool bb_variant_differs(const struct bb_variant *a, const struct bb_variant *b)
{
    if(a->shadowed != b->shadowed)
        return true;

    vec3_t da = a->light_pos, db = b->light_pos;
    PFM_Vec3_Normal(&da, &da);
    PFM_Vec3_Normal(&db, &db);
    if(PFM_Vec3_Dot(&da, &db) < cosf(DEG_TO_RAD(CONFIG_BILLBOARD_LIGHT_REBAKE_DEG)))
        return true;

    for(int i = 0; i < 3; i++) {
        if(fabsf(a->ambient_color.raw[i] - b->ambient_color.raw[i]) > 0.1f)
            return true;
        if(fabsf(a->emit_color.raw[i] - b->emit_color.raw[i]) > 0.1f)
            return true;
    }
    return false;
}

static int bb_wanted_refresh(void)
{
    struct bb_variant cur = bb_current_variant();
    if(s_wanted_gen == 0 || bb_variant_differs(&cur, &s_wanted)) {
        s_wanted = cur;
        s_wanted_gen++;
    }
    return s_wanted_gen;
}

static uint64_t bb_variant_tag(const struct bb_model_desc *desc, const struct bb_variant *var)
{
    uint64_t tag = bb_mix(desc->tag, var->shadowed);

    vec3_t dir = var->light_pos;
    PFM_Vec3_Normal(&dir, &dir);
    for(int i = 0; i < 3; i++) {
        tag = bb_mix(tag, (uint64_t)(int64_t)(dir.raw[i] * 256.0f));
        tag = bb_mix(tag, (uint64_t)(var->ambient_color.raw[i] * 256.0f));
        tag = bb_mix(tag, (uint64_t)(var->emit_color.raw[i] * 256.0f));
    }
    return tag;
}

static void bb_variant_cache_name(const struct bb_model_desc *desc, const struct bb_variant *var,
                                  char *out, size_t size)
{
    pf_snprintf(out, size, "%s#%08x", desc->cache_name,
        (uint32_t)(bb_variant_tag(desc, var) & 0xffffffff));
}

static void bb_enqueue_bake(struct bb_model_desc *desc, int gen)
{
    desc->inflight_gen = gen;
    R_PushCmd((struct rcmd){
        .func = R_GL_Billboard_EnsureBaked,
        .nargs = 4,
        .args = {
            desc,
            desc->render_key,
            R_PushArg(&s_wanted, sizeof(s_wanted)),
            R_PushArg(&gen, sizeof(gen)),
        },
    });
}

/* A bound on how far any baked point lies from the sprite's card plane, which
 * passes through the model origin; also the scale of the baked depth offsets.
 */
static float bb_depth_extent(const struct bb_model_desc *desc)
{
    return desc->world_size.x / 2.0f + desc->world_size.y / 2.0f + fabsf(desc->anchor_off.y);
}

static float bb_bake_cam_dist(const struct bb_model_desc *desc)
{
    return 2.0f * bb_depth_extent(desc) + 10.0f;
}

static float bb_bake_far(const struct bb_model_desc *desc)
{
    return 2.0f * bb_bake_cam_dist(desc) + 10.0f;
}

/* The bake camera looks at the model origin from the registered tilt, along
 * the -Z world axis; the model itself is rotated per azimuth cell.
 */
static void bb_bake_view_proj(const struct bb_model_desc *desc, float tilt_rad,
                              mat4x4_t *out_view, vec3_t *out_pos, mat4x4_t *out_proj)
{
    float half_w = desc->world_size.x / 2.0f;
    float half_h = desc->world_size.y / 2.0f;
    float dist = bb_bake_cam_dist(desc);

    vec3_t pos = (vec3_t){0.0f, dist * sinf(tilt_rad), dist * cosf(tilt_rad)};
    vec3_t target = (vec3_t){0.0f, 0.0f, 0.0f};
    /* MakeLookAt uses 'up' directly as the view basis row, so it must be the
     * camera's true (tilted) up rather than the world up.
     */
    vec3_t up = (vec3_t){0.0f, cosf(tilt_rad), -sinf(tilt_rad)};
    PFM_Mat4x4_MakeLookAt(&pos, &target, &up, out_view);

    PFM_Mat4x4_MakeOrthographic(-half_w, half_w,
        desc->anchor_off.y - half_h, desc->anchor_off.y + half_h,
        BB_BAKE_NEAR, bb_bake_far(desc), out_proj);
    *out_pos = pos;
}

/* Convert one cell's window depths from the orthographic bake into signed
 * offsets from the card plane along the view direction, in units of the
 * depth extent (negative is toward the camera).
 */
static void bb_encode_depth(const struct bb_model_desc *desc, const float *depth,
                            size_t ntexels, int8_t *out)
{
    float dist = bb_bake_cam_dist(desc);
    float far = bb_bake_far(desc);
    float extent = bb_depth_extent(desc);

    for(size_t i = 0; i < ntexels; i++) {
        if(depth[i] >= 1.0f) {
            out[i] = 0;
            continue;
        }
        float zview = BB_BAKE_NEAR + depth[i] * (far - BB_BAKE_NEAR);
        float off = (zview - dist) / extent;
        off = MAX(-1.0f, MIN(1.0f, off));
        out[i] = (int8_t)lrintf(off * 127.0f);
    }
}

/* One light frustum serves every cell of an atlas: the extents cover the
 * model's rotation-invariant bounding sphere, and the depth window is kept
 * wide regardless of model size so the shaders' constant depth bias stays
 * sane in world units and the model sits well inside the shadow lookup's
 * valid depth band.
 */
static void bb_light_space_trans(const struct bb_model_desc *desc, const vec3_t *light_pos,
                                 mat4x4_t *out)
{
    vec3_t dir = *light_pos;
    PFM_Vec3_Normal(&dir, &dir);
    PFM_Vec3_Scale(&dir, -1.0f, &dir);

    vec3_t right = (vec3_t){-1.0f, 0.0f, 0.0f};
    if(fabsf(dir.x) > 0.99f) {
        right = (vec3_t){0.0f, 0.0f, -1.0f};
    }
    vec3_t up;
    PFM_Vec3_Cross(&dir, &right, &up);
    PFM_Vec3_Normal(&up, &up);

    vec3_t center = (vec3_t){0.0f, (desc->ymin + desc->ymax) / 2.0f, 0.0f};
    float yhalf = (desc->ymax - desc->ymin) / 2.0f;
    float radius = desc->world_size.x / 2.0f;
    float sphere = sqrtf(radius * radius + yhalf * yhalf) + 2.0f;

    vec3_t pos, delta;
    PFM_Vec3_Scale(&dir, -CONFIG_BILLBOARD_SHADOW_DEPTH_RANGE / 2.0f, &delta);
    PFM_Vec3_Add(&center, &delta, &pos);

    vec3_t target;
    PFM_Vec3_Add(&pos, &dir, &target);

    mat4x4_t view, proj;
    PFM_Mat4x4_MakeLookAt(&pos, &target, &up, &view);
    PFM_Mat4x4_MakeOrthographic(-sphere, sphere, sphere, -sphere,
        1.0f, CONFIG_BILLBOARD_SHADOW_DEPTH_RANGE, &proj);
    PFM_Mat4x4_Mult4x4(&proj, &view, out);
}

static void bb_free_atlas(struct bb_model_desc *desc)
{
    glDeleteTextures(1, &desc->tex_arr);
    glDeleteTextures(1, &desc->depth_arr);
    desc->tex_arr = 0;
    desc->depth_arr = 0;
}

/* Leaves the colour array bound */
static bool bb_alloc_atlas(struct bb_model_desc *desc, const void *pixels, const void *depth)
{
    ASSERT_IN_RENDER_THREAD();

    /* Depth offsets must not be blended across the silhouette */
    glGenTextures(1, &desc->depth_arr);
    glBindTexture(GL_TEXTURE_2D_ARRAY, desc->depth_arr);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8_SNORM, desc->cell_res, desc->cell_res,
        desc->total_slices, 0, GL_RED, GL_BYTE, depth);

    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &desc->tex_arr);
    glBindTexture(GL_TEXTURE_2D_ARRAY, desc->tex_arr);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, desc->cell_res, desc->cell_res,
        desc->total_slices, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    if(glGetError() != GL_NO_ERROR) {
        bb_free_atlas(desc);
        return false;
    }
    return true;
}

static bool bb_bake_atlas(struct bb_model_desc *desc, const void *render_private,
                          const struct bb_variant *var, const char *cache_name,
                          uint64_t tag)
{
    ASSERT_IN_RENDER_THREAD();

    /* Save GL and uniform state */
    GLint saved_viewport[4];
    GLint saved_fb, saved_rb;

    glGetIntegerv(GL_VIEWPORT, saved_viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &saved_fb);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &saved_rb);

    const char *saved_unames[] = {
        GL_U_VIEW, GL_U_VIEW_POS, GL_U_PROJECTION, GL_U_LIGHT_POS,
        GL_U_AMBIENT_COLOR, GL_U_LIGHT_COLOR, GL_U_SHADOWS_ON, GL_U_LS_TRANS
    };
    struct uval saved_uvals[ARR_SIZE(saved_unames)];
    bool have_uval[ARR_SIZE(saved_unames)];
    for(int i = 0; i < ARR_SIZE(saved_unames); i++) {
        have_uval[i] = R_GL_StateGet(saved_unames[i], &saved_uvals[i]);
    }

    size_t cell_texels = (size_t)desc->cell_res * desc->cell_res;
    float *cell_depth = PF_MALLOC(cell_texels * sizeof(float));
    int8_t *depth = PF_MALLOC(cell_texels * desc->total_slices);
    if(!cell_depth || !depth || !bb_alloc_atlas(desc, NULL, NULL)) {
        if(cell_depth)
            PF_FREE(cell_depth);
        if(depth)
            PF_FREE(depth);
        return false;
    }

    GLuint fb;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);

    GLuint depth_rb;
    glGenRenderbuffers(1, &depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, desc->cell_res, desc->cell_res);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_rb);

    GLenum draw_buffs[1] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(ARR_SIZE(draw_buffs), draw_buffs);

    float tilt_rad = desc->tilt_rad;
    mat4x4_t view, proj;
    vec3_t cam_pos;
    bb_bake_view_proj(desc, tilt_rad, &view, &cam_pos, &proj);
    R_GL_SetViewMatAndPos(&view, &cam_pos);
    R_GL_SetProj(&proj);
    R_GL_SetLightPos(&var->light_pos);
    R_GL_SetAmbientLightColor(&var->ambient_color);
    R_GL_SetLightEmitColor(&var->emit_color);
    /* Mode 2 tells the mesh shaders to keep the ambient floor under the
     * baked self-shadow
     */
    R_GL_StateSet(GL_U_SHADOWS_ON, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = var->shadowed ? 2 : 0
    });

    mat4x4_t ls_trans;
    PFM_Mat4x4_Identity(&ls_trans);
    if(var->shadowed) {
        bb_light_space_trans(desc, &var->light_pos, &ls_trans);
    }

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glViewport(0, 0, desc->cell_res, desc->cell_res);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);

    bool translucent = false;
    bool ok = true;
    int nclips = MAX(desc->nclips, 1);

    for(int c = 0; ok && (c < nclips); c++) {

        const struct bb_clip_desc *clip = (desc->nclips > 0) ? &desc->clips[c] : NULL;
        int first_slice = clip ? clip->first_slice : 0;
        int nkf = clip ? clip->nkf : 1;

        for(int k = 0; ok && (k < nkf); k++) {
        for(int a = 0; ok && (a < desc->nazimuths); a++) {

            int slice = first_slice + k * desc->nazimuths + a;
            glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                desc->tex_arr, 0, slice);
            if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                ok = false;
                break;
            }

            mat4x4_t model;
            PFM_Mat4x4_MakeRotY(a * (2.0f * M_PI / desc->nazimuths), &model);

            if(clip) {
                /* The inverse transpose of a pure rotation is the rotation */
                mat4x4_t normal = model;
                R_GL_AnimSetUniforms(&normal, (struct anim_pose_data_desc*)&clip->kf_pose[k]);
            }
            if(var->shadowed) {
                R_GL_DepthPassBeginCustom(&ls_trans);
                R_GL_RenderDepthMap(render_private, &model);
                R_GL_DepthPassEnd();
            }
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            R_GL_Draw(render_private, &model, &translucent);

            glReadPixels(0, 0, desc->cell_res, desc->cell_res, GL_DEPTH_COMPONENT,
                GL_FLOAT, cell_depth);
            bb_encode_depth(desc, cell_depth, cell_texels, depth + slice * cell_texels);
        }}
    }

    /* Detach the atlas from the framebuffer before reading it back */
    glBindFramebuffer(GL_FRAMEBUFFER, saved_fb);
    glDeleteFramebuffers(1, &fb);
    glDeleteRenderbuffers(1, &depth_rb);

    if(ok) {
        glBindTexture(GL_TEXTURE_2D_ARRAY, desc->depth_arr);
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, desc->cell_res, desc->cell_res,
            desc->total_slices, GL_RED, GL_BYTE, depth);

        glBindTexture(GL_TEXTURE_2D_ARRAY, desc->tex_arr);
        glGenerateMipmap(GL_TEXTURE_2D_ARRAY);

        size_t nbytes = (size_t)desc->cell_res * desc->cell_res * desc->total_slices * 4;
        void *pixels = PF_MALLOC(nbytes);
        if(pixels) {
            glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            AssetCache_ImpostorStore(cache_name, tag, &(struct impostor_cache){
                .cell_res = desc->cell_res,
                .nslices = desc->total_slices,
                .pixels = pixels,
                .depth = depth,
            });
            PF_FREE(pixels);
        }
    }else{
        bb_free_atlas(desc);
    }
    PF_FREE(cell_depth);
    PF_FREE(depth);

    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    glViewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, saved_fb);
    glBindRenderbuffer(GL_RENDERBUFFER, saved_rb);

    for(int i = 0; i < ARR_SIZE(saved_unames); i++) {
        if(have_uval[i]) {
            R_GL_StateSet(saved_unames[i], saved_uvals[i]);
        }
    }

    GL_ASSERT_OK();
    return ok;
}

static void bb_point_inst_attrs(size_t base_inst)
{
    const GLsizei stride = sizeof(struct bb_gpu_inst);
    size_t base = base_inst * stride;

    glBindBuffer(GL_ARRAY_BUFFER, s_inst_vbo);

    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
        (void*)(base + offsetof(struct bb_gpu_inst, pos)));
    glEnableVertexAttribArray(1);
    glVertexAttribDivisor(1, 1);

    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride,
        (void*)(base + offsetof(struct bb_gpu_inst, yaw)));
    glEnableVertexAttribArray(2);
    glVertexAttribDivisor(2, 1);

    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride,
        (void*)(base + offsetof(struct bb_gpu_inst, scale)));
    glEnableVertexAttribArray(3);
    glVertexAttribDivisor(3, 1);

    glVertexAttribIPointer(4, 1, GL_INT, stride,
        (void*)(base + offsetof(struct bb_gpu_inst, cell_base)));
    glEnableVertexAttribArray(4);
    glVertexAttribDivisor(4, 1);
}

/*****************************************************************************/
/* PUBLIC FUNCTIONS                                                          */
/*****************************************************************************/

bool R_Billboard_Init(void)
{
    ASSERT_IN_MAIN_THREAD();
    s_desc_table = kh_init(bbdesc);
    return (s_desc_table != NULL);
}

void R_Billboard_Shutdown(void)
{
    ASSERT_IN_MAIN_THREAD();

    if(!s_desc_table)
        return;

    /* The atlas textures are freed with the GL context */
    struct bb_model_desc *desc;
    kh_foreach_value(s_desc_table, desc, {
        PF_FREE(desc);
    });
    kh_destroy(bbdesc, s_desc_table);
    s_desc_table = NULL;
}

void R_Billboard_Register(void *render_key, const char *basedir, const char *filename,
                          const struct aabb *aabb, const struct anim_data *anim_data)
{
    ASSERT_IN_MAIN_THREAD();

    if(bb_desc_for_key(render_key))
        return;

    int nclips = anim_data ? A_GetNumClips(anim_data) : 0;
    struct bb_model_desc *desc = PF_CALLOC(1,
        sizeof(struct bb_model_desc) + nclips * sizeof(struct bb_clip_desc));
    if(!desc)
        return;

    desc->render_key = render_key;
    desc->nazimuths = bb_azimuth_count(aabb, nclips);
    desc->nclips = nclips;
    desc->tilt_rad = bb_tilt_rad();
    SDL_AtomicSet(&desc->state, BB_STATE_PENDING);

    /* Reduce the per-clip keyframe count until the atlas fits within the
     * texture array layer limit.
     */
    int max_layers = SDL_AtomicGet(&s_max_layers);
    if(max_layers <= 0) {
        max_layers = FALLBACK_MAX_LAYERS;
    }

    /* The keyframe clamp below cannot shrink the azimuth set */
    if(desc->nazimuths > max_layers) {
        desc->nazimuths = CONFIG_BILLBOARD_AZIMUTHS;
    }

    int kf_cap = CONFIG_BILLBOARD_MAX_KF;
    do{
        desc->total_slices = 0;
        for(int c = 0; c < nclips; c++) {
            int nframes = A_GetClipFrameCount(anim_data, c);
            desc->clips[c].nframes = nframes;
            desc->clips[c].nkf = MIN(kf_cap, nframes);
            desc->clips[c].first_slice = desc->total_slices;
            desc->total_slices += desc->clips[c].nkf * desc->nazimuths;
        }
        if(nclips == 0) {
            desc->total_slices = desc->nazimuths;
        }
    }while(desc->total_slices > max_layers && kf_cap-- > 1);

    /* Framing: the union of the baked samples' extents, rotation-invariant
     * about the Y axis. The bind-pose AABB is a safety net for models whose
     * samples carry no AABBs.
     */
    float radius = 0.0f;
    float ymin = 0.0f, ymax = 0.0f;
    bb_extend_metrics(aabb, &radius, &ymin, &ymax);

    for(int c = 0; c < nclips; c++) {
        for(int k = 0; k < desc->clips[c].nkf; k++) {

            int frame = bb_bake_frame(k, desc->clips[c].nkf, desc->clips[c].nframes);
            A_GetPoseDesc(anim_data, c, frame, &desc->clips[c].kf_pose[k]);
            bb_extend_metrics(A_GetClipFrameAABB(anim_data, c, frame),
                &radius, &ymin, &ymax);
        }
    }

    float cos_t = cosf(desc->tilt_rad);
    float sin_t = sinf(desc->tilt_rad);
    float vmin = ymin * cos_t - radius * sin_t;
    float vmax = ymax * cos_t + radius * sin_t;

    desc->world_size = (vec2_t){2.0f * radius, vmax - vmin};
    desc->anchor_off = (vec2_t){0.0f, (vmax + vmin) / 2.0f};
    desc->ymin = ymin;
    desc->ymax = ymax;
    desc->cell_res = bb_pow2_cell_res(
        MAX(desc->world_size.x, desc->world_size.y) * CONFIG_BILLBOARD_PX_PER_WU);

    float max_extent = MAX(desc->world_size.x, desc->world_size.y);
    if(desc->cell_res < max_extent * CONFIG_BILLBOARD_MIN_PX_PER_WU) {
        PF_FREE(desc);
        return;
    }

    /* The cache is keyed by the model's base-path-relative path and
     * invalidated by either a source or a bake-parameter change.
     */
    char full_path[512];
    pf_snprintf(full_path, sizeof(full_path), "%s/%s/%s", g_basepath, basedir, filename);
    pf_snprintf(desc->cache_name, sizeof(desc->cache_name), "%s/%s", basedir, filename);

    uint64_t tag = AssetCache_SourceTag(full_path);
    tag = bb_mix(tag, CONFIG_BILLBOARD_CACHE_VER);
    tag = bb_mix(tag, desc->nazimuths);
    tag = bb_mix(tag, desc->total_slices);
    tag = bb_mix(tag, desc->cell_res);
    tag = bb_mix(tag, (uint64_t)(CONFIG_BILLBOARD_PX_PER_WU * 16.0f));
    tag = bb_mix(tag, (uint64_t)(desc->tilt_rad * 1024.0f));
    desc->tag = tag;

    int status;
    khiter_t k = kh_put(bbdesc, s_desc_table, (khint64_t)(uintptr_t)render_key, &status);
    if(status == -1) {
        PF_FREE(desc);
        return;
    }
    kh_value(s_desc_table, k) = desc;
}

const struct bb_model_desc *R_Billboard_Get(void *render_key)
{
    ASSERT_IN_MAIN_THREAD();

    struct bb_model_desc *desc = bb_desc_for_key(render_key);
    if(!desc || SDL_AtomicGet(&desc->state) != BB_STATE_READY)
        return NULL;
    return desc;
}

bool R_Billboard_ScaleEligible(const struct bb_model_desc *desc, vec2_t scale)
{
    float smax = MAX(scale.x, scale.y);
    float extent = MAX(desc->world_size.x, desc->world_size.y);
    return (desc->cell_res >= extent * smax * CONFIG_BILLBOARD_MIN_PX_PER_WU);
}

int R_Billboard_CellBase(const struct bb_model_desc *desc, int clip_idx, int frame_idx)
{
    if(desc->nclips == 0)
        return 0;
    if(clip_idx < 0 || clip_idx >= desc->nclips) {
        clip_idx = 0;
    }

    const struct bb_clip_desc *clip = &desc->clips[clip_idx];
    int kf = (frame_idx * clip->nkf) / MAX(clip->nframes, 1);
    kf = MAX(MIN(kf, clip->nkf - 1), 0);
    return clip->first_slice + kf * desc->nazimuths;
}

void R_Billboard_EnsureBaked(void *render_key)
{
    ASSERT_IN_MAIN_THREAD();

    struct sval setting;
    bool enabled = (Settings_Get("pf.video.billboards_enabled", &setting) == SS_OKAY)
                 && setting.as_bool;
    if(!enabled)
        return;

    struct bb_model_desc *desc = bb_desc_for_key(render_key);
    if(!desc || SDL_AtomicGet(&desc->state) != BB_STATE_PENDING)
        return;

    int gen = bb_wanted_refresh();
    SDL_AtomicSet(&desc->state, BB_STATE_QUEUED);
    bb_enqueue_bake(desc, gen);
}

void R_Billboard_Tick(void)
{
    ASSERT_IN_MAIN_THREAD();

    struct sval setting;
    bool enabled = (Settings_Get("pf.video.billboards_enabled", &setting) == SS_OKAY)
                 && setting.as_bool;
    if(!enabled || !s_desc_table)
        return;

    int gen = bb_wanted_refresh();
    int budget = CONFIG_BILLBOARD_REBAKES_PER_FRAME;

    struct bb_model_desc *desc;
    kh_foreach_value(s_desc_table, desc, {
        if(budget == 0)
            break;
        if(SDL_AtomicGet(&desc->state) != BB_STATE_READY)
            continue;
        if(SDL_AtomicGet(&desc->baked_gen) == gen)
            continue;
        if(desc->inflight_gen == gen)
            continue;
        bb_enqueue_bake(desc, gen);
        budget--;
    });
}

void R_Billboard_EnsureAllBaked(void)
{
    ASSERT_IN_MAIN_THREAD();

    if(!s_desc_table)
        return;

    struct bb_model_desc *desc;
    kh_foreach_value(s_desc_table, desc, {
        R_Billboard_EnsureBaked(desc->render_key);
    });
}

bool R_GL_Billboard_InitCtx(void)
{
    ASSERT_IN_RENDER_THREAD();

    GLint max_layers = 0;
    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &max_layers);
    SDL_AtomicSet(&s_max_layers, max_layers);

    const GLfloat corners[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f,
    };

    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);

    glGenBuffers(1, &s_quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(GLfloat), (void*)0);
    glEnableVertexAttribArray(0);

    /* The instance attributes are pointed at the streamed VBO per draw */
    glGenBuffers(1, &s_inst_vbo);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    GL_ASSERT_OK();
    return true;
}

void R_GL_Billboard_ShutdownCtx(void)
{
    ASSERT_IN_RENDER_THREAD();

    if(s_vao) {
        glDeleteVertexArrays(1, &s_vao);
        s_vao = 0;
    }
    if(s_quad_vbo) {
        glDeleteBuffers(1, &s_quad_vbo);
        s_quad_vbo = 0;
    }
    if(s_inst_vbo) {
        glDeleteBuffers(1, &s_inst_vbo);
        s_inst_vbo = 0;
    }
    if(s_inst_scratch) {
        PF_FREE(s_inst_scratch);
        s_inst_scratch = NULL;
        s_inst_scratch_cap = 0;
    }
    s_inst_cap = 0;
}

void R_GL_Billboard_EnsureBaked(struct bb_model_desc *desc, const void *render_private,
                                const struct bb_variant *var, const int *gen)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    if(SDL_AtomicGet(&desc->baked_gen) == *gen)
        GL_PERF_RETURN_VOID();

    GLuint old_tex = desc->tex_arr;
    GLuint old_depth = desc->depth_arr;
    desc->tex_arr = 0;
    desc->depth_arr = 0;

    char cache_name[BB_CACHE_NAME_LEN];
    bb_variant_cache_name(desc, var, cache_name, sizeof(cache_name));
    uint64_t tag = bb_variant_tag(desc, var);

    /* Warm path: upload the cached atlas without any GL baking */
    bool ok = false;
    struct impostor_cache cache;
    if(AssetCache_ImpostorLoad(cache_name, tag, &cache)) {

        ok = (cache.cell_res == desc->cell_res)
          && (cache.nslices == desc->total_slices)
          && bb_alloc_atlas(desc, cache.pixels, cache.depth);
        if(ok) {
            glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
            glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        }
        AssetCache_ImpostorRelease(&cache);
    }

    if(!ok) {
        R_GL_LoadingScreenPushModel(cache_name);
        ok = bb_bake_atlas(desc, render_private, var, cache_name, tag);
        R_GL_LoadingScreenPopModel();
    }

    if(ok) {
        if(old_tex) {
            glDeleteTextures(1, &old_tex);
            glDeleteTextures(1, &old_depth);
        }
        SDL_AtomicSet(&desc->state, BB_STATE_READY);
    }else if(old_tex) {
        /* Keep drawing the previous atlas rather than dropping to meshes;
         * marking the generation baked stops the retries.
         */
        desc->tex_arr = old_tex;
        desc->depth_arr = old_depth;
    }else{
        SDL_AtomicSet(&desc->state, BB_STATE_FAILED);
    }
    SDL_AtomicSet(&desc->baked_gen, *gen);

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

static size_t bb_upload_instances(const vec_rbill_t *list)
{
    size_t nents = vec_size(list);
    if(nents > s_inst_scratch_cap) {
        PF_FREE(s_inst_scratch);
        s_inst_scratch_cap = nents * 2;
        s_inst_scratch = PF_MALLOC(s_inst_scratch_cap * sizeof(struct bb_gpu_inst));
        if(!s_inst_scratch) {
            s_inst_scratch_cap = 0;
            return 0;
        }
    }

    for(size_t i = 0; i < nents; i++) {
        const struct ent_bill_rstate *curr = &vec_AT(list, i);
        s_inst_scratch[i] = (struct bb_gpu_inst){
            .pos = curr->pos,
            .yaw = curr->yaw,
            .scale = curr->scale,
            .cell_base = curr->cell_base,
        };
    }

    glBindBuffer(GL_ARRAY_BUFFER, s_inst_vbo);
    if(nents > s_inst_cap) {
        s_inst_cap = nents * 2;
    }
    /* Orphan the previous contents */
    glBufferData(GL_ARRAY_BUFFER, s_inst_cap * sizeof(struct bb_gpu_inst), NULL, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, nents * sizeof(struct bb_gpu_inst), s_inst_scratch);
    return nents;
}

static void bb_draw_desc_runs(const vec_rbill_t *list, GLuint prog, bool depth_unit)
{
    /* One instanced draw per run of entities sharing a descriptor */
    size_t nents = vec_size(list);
    size_t run_start = 0;
    while(run_start < nents) {

        const struct bb_model_desc *desc = vec_AT(list, run_start).desc;
        size_t run_end = run_start + 1;
        while(run_end < nents && vec_AT(list, run_end).desc == desc) {
            run_end++;
        }

        if(!desc->tex_arr) {
            run_start = run_end;
            continue;
        }

        R_GL_StateSet(GL_U_BB_WORLD_SIZE, (struct uval){
            .type = UTYPE_VEC2,
            .val.as_vec2 = desc->world_size
        });
        R_GL_StateInstall(GL_U_BB_WORLD_SIZE, prog);

        R_GL_StateSet(GL_U_BB_ANCHOR_OFF, (struct uval){
            .type = UTYPE_VEC2,
            .val.as_vec2 = desc->anchor_off
        });
        R_GL_StateInstall(GL_U_BB_ANCHOR_OFF, prog);

        R_GL_StateSet(GL_U_BB_NAZIMUTHS, (struct uval){
            .type = UTYPE_INT,
            .val.as_int = desc->nazimuths
        });
        R_GL_StateInstall(GL_U_BB_NAZIMUTHS, prog);

        if(depth_unit) {
            R_GL_StateSet(GL_U_BB_DEPTH_EXTENT, (struct uval){
                .type = UTYPE_FLOAT,
                .val.as_float = bb_depth_extent(desc)
            });
            R_GL_StateInstall(GL_U_BB_DEPTH_EXTENT, prog);

            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D_ARRAY, desc->depth_arr);
            glActiveTexture(GL_TEXTURE0);
        }
        glBindTexture(GL_TEXTURE_2D_ARRAY, desc->tex_arr);
        bb_point_inst_attrs(run_start);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)(run_end - run_start));

        run_start = run_end;
    }
}

void R_GL_Billboard_Draw(struct render_input *in)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    if(vec_size(&in->cam_vis_bill) == 0)
        GL_PERF_RETURN_VOID();

    GL_PERF_PUSH_GROUP(0, "billboards");

    if(!bb_upload_instances(&in->cam_vis_bill)) {
        GL_PERF_POP_GROUP();
        GL_PERF_RETURN_VOID();
    }

    R_GL_StateSet(GL_U_TEX_ARRAY0, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = 0
    });
    R_GL_StateSet(GL_U_TEX_ARRAY1, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = 1
    });
    R_GL_Shader_Install("billboard");
    GLuint prog = R_GL_Shader_GetProgForName("billboard");

    /* The engine's front faces are clockwise; sidestep the quad's winding */
    glDisable(GL_CULL_FACE);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(s_vao);

    bb_draw_desc_runs(&in->cam_vis_bill, prog, true);

    glEnable(GL_CULL_FACE);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    GL_PERF_POP_GROUP();
    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_Billboard_DrawDepth(struct render_input *in)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    if(vec_size(&in->light_vis_bill) == 0)
        GL_PERF_RETURN_VOID();

    GL_PERF_PUSH_GROUP(0, "billboards::depth");

    if(!bb_upload_instances(&in->light_vis_bill)) {
        GL_PERF_POP_GROUP();
        GL_PERF_RETURN_VOID();
    }

    /* Pin the azimuth-binning light to this frame's snapshot */
    R_GL_SetLightPos(&in->light_pos);
    R_GL_StateSet(GL_U_TEX_ARRAY0, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = 0
    });
    R_GL_Shader_Install("billboard.depth");
    GLuint prog = R_GL_Shader_GetProgForName("billboard.depth");

    glDisable(GL_CULL_FACE);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(s_vao);

    bb_draw_desc_runs(&in->light_vis_bill, prog, false);

    /* Re-enabling leaves the depth pass's GL_FRONT cull mode in place */
    glEnable(GL_CULL_FACE);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    GL_PERF_POP_GROUP();
    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}
