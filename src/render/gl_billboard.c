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
#include "gl_render.h"

#define GPU_MEM_FILE_SYS GPU_MEM_SYS_GL_BILLBOARD
#include "gl_mem.h"
#include "gl_shader.h"
#include "gl_state.h"
#include "gl_perf.h"
#include "gl_postprocess.h"
#include "public/render.h"
#include "public/render_ctrl.h"
#include "../asset/public/asset_cache.h"
#include "../entity.h"
#include "../settings.h"
#include "../main.h"
#include "../perf/public/mem.h"
#include "../game/public/game.h"
#include "../camera/public/camera.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/khash.h"

#include <assert.h>
#include <string.h>
#include <stddef.h>
#include <math.h>
#include <limits.h>

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_BILLBOARD)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_BILLBOARD)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_RENDER, MEM_SUB_RENDER_GL_BILLBOARD)

/* The texture units of the atlas arrays, and of the look's grade past them */
#define COLOR_TUNIT (GL_TEXTURE0)
#define DEPTH_TUNIT (GL_TEXTURE1)
#define SURFACE_TUNIT (GL_TEXTURE2)
#define SIL_TUNIT   (GL_TEXTURE0)
#define GRADE_TUNIT (GL_TEXTURE3)

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
    uint32_t uid;
};

/* One variant of a model's atlas, baked into textures of its own and swapped
 * in for the resident atlas once every cell is done.
 */
struct bb_job{
    struct bb_model_desc *desc;
    const void           *render_private;
    struct bb_variant     var;
    int                   gen;
    bool                  cached;     /* the variant is kept in the disk cache */
    bool                  loaded;     /* the atlas came from the disk cache */
    bool                  mipped;     /* each cell's mip chain is built as it is baked */
    char                  cache_name[BB_CACHE_NAME_LEN];
    uint64_t              tag;
    struct bb_frame       frame;
    mat4x4_t              ls_trans;
    struct bb_atlas       atlas;
    GLuint                fb;
    GLuint                sil_fb;
    GLuint                mip_fbs[2]; /* read and draw targets of the mip downsampling */
    int                   next_slice;
};

/* The uniforms a bake overwrites */
static const char *s_bake_unames[] = {
    GL_U_VIEW, GL_U_VIEW_POS, GL_U_PROJECTION, GL_U_LIGHT_POS,
    GL_U_AMBIENT_COLOR, GL_U_LIGHT_COLOR, GL_U_SHADOWS_ON, GL_U_LS_TRANS
};

struct bb_saved_state{
    GLint       viewport[4];
    GLint       fb;
    struct uval uvals[ARR_SIZE(s_bake_unames)];
    bool        have_uval[ARR_SIZE(s_bake_unames)];
};

KHASH_MAP_INIT_INT64(bbdesc, struct bb_model_desc*)

static uint64_t bb_mix(uint64_t hash, uint64_t val);
static int      bb_azimuth_count(const struct aabb *aabb, int nclips);
static vec3_t   bb_bake_frame_light(vec3_t light_pos);
static float    bb_bake_cam_dist(const struct bb_frame *frame);
static float    bb_bake_far(const struct bb_frame *frame);
static bool     bb_atlas_alloc(const struct bb_model_desc *desc, const struct impostor_cache *data,
                               struct bb_atlas *out);

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
/* The camera elevation the atlases follow: the last one the camera rested
 * at, in whole degrees; 0 until first sampled.
 */
static int              s_settled_elev;
static int              s_last_elev;
static int              s_still_frames;
static uint32_t         s_frame;

/* Written once by the render thread at context init */
static SDL_atomic_t     s_max_layers;
/* Background bakes enqueued by the main thread and not yet ended by the
 * render thread
 */
static SDL_atomic_t     s_jobs_inflight;

/* Render thread */
static GLuint           s_quad_vbo;
static GLuint           s_inst_vbo;
static GLuint           s_vao;
static size_t           s_inst_cap;
static struct bb_gpu_inst *s_inst_scratch;
static size_t           s_inst_scratch_cap;
static struct bb_job    s_jobs[CONFIG_BILLBOARD_JOBS_INFLIGHT];
static int              s_njobs;

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

static int bb_persp_offset(int projection)
{
    return (projection == CAM_PROJ_PERSPECTIVE) ? CONFIG_BILLBOARD_PERSP_ELEV_OFFSET_DEG : 0;
}

/* The elevation baked for the RTS camera: the complement of the tilt setting,
 * which counts up from looking straight down, flattened for a perspective
 * projection.
 */
static int bb_rts_elev_deg(void)
{
    struct sval setting;
    int degrees = 45;
    if(Settings_Get("pf.game.camera_tilt", &setting) == SS_OKAY) {
        degrees = setting.as_int;
    }
    int projection = CAM_PROJ_PERSPECTIVE;
    if(Settings_Get("pf.game.camera_projection", &setting) == SS_OKAY) {
        projection = setting.as_int;
    }
    return 90 - degrees - bb_persp_offset(projection);
}

static int bb_camera_elev_deg(void)
{
    struct camera *cam = G_GetActiveCamera();
    if(!cam)
        return bb_rts_elev_deg();

    int elev = (int)lrintf(-Camera_GetPitch(cam)) - bb_persp_offset(Camera_GetProjection(cam));
    return MAX(CONFIG_BILLBOARD_MIN_ELEV_DEG, MIN(90, elev));
}

static void bb_track_camera(void)
{
    int elev = bb_camera_elev_deg();
    if(elev != s_last_elev) {
        s_last_elev = elev;
        s_still_frames = 0;
    }else if(s_still_frames < CONFIG_BILLBOARD_TILT_SETTLE_FRAMES) {
        s_still_frames++;
    }
    if(s_still_frames == CONFIG_BILLBOARD_TILT_SETTLE_FRAMES) {
        s_settled_elev = elev;
    }
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
static float bb_bake_frame_delta(void)
{
    float yaw = DEG_TO_RAD(CONFIG_RTS_CAMERA_YAW_DEG);
    return M_PI - atan2f(cosf(yaw), -sinf(yaw));
}

static vec3_t bb_bake_frame_light(vec3_t light_pos)
{
    float delta = bb_bake_frame_delta();
    float c = cosf(delta), s = sinf(delta);

    return (vec3_t){
        light_pos.x * c + light_pos.z * s,
        light_pos.y,
        -light_pos.x * s + light_pos.z * c,
    };
}

static struct bb_variant bb_current_variant(void)
{
    if(s_settled_elev == 0) {
        s_settled_elev = s_last_elev = bb_camera_elev_deg();
    }

    struct sval setting;
    bool shadowed = (Settings_Get("pf.video.shadows_enabled", &setting) == SS_OKAY)
                  && setting.as_bool;
    if(!shadowed) {
        /* Without shadows the sprites bake with a canonical light, keeping the
         * atlases valid across maps.
         */
        return (struct bb_variant){
            .shadowed = false,
            .elev_deg = s_settled_elev,
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
        .elev_deg = s_settled_elev,
        .light_pos = bb_bake_frame_light(G_GetLightPos()),
        .ambient_color = G_GetAmbientLightColor(),
        .emit_color = G_GetEmitLightColor(),
    };
}

static bool bb_variant_differs(const struct bb_variant *a, const struct bb_variant *b)
{
    if(a->shadowed != b->shadowed)
        return true;
    if(a->elev_deg != b->elev_deg)
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

/* Only the RTS camera's elevation goes to the disk cache: any other one is
 * a passing cinematic or free camera, and would grow the cache per pitch.
 * The cache serves the bakes at load only; a background re-bake costs a
 * couple of milliseconds a frame, less than reading and uploading an atlas.
 */
static bool bb_variant_cacheable(const struct bb_variant *var)
{
    return (var->elev_deg == bb_rts_elev_deg());
}

static uint64_t bb_variant_tag(const struct bb_model_desc *desc, const struct bb_variant *var)
{
    uint64_t tag = bb_mix(desc->tag, var->shadowed);
    tag = bb_mix(tag, (uint64_t)var->elev_deg);

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

static void bb_enqueue_job(struct bb_model_desc *desc, int gen)
{
    SDL_AtomicIncRef(&s_jobs_inflight);
    desc->inflight_gen = gen;
    R_PushCmd((struct rcmd){
        .func = R_GL_Billboard_BakeBegin,
        .nargs = 4,
        .args = {
            desc,
            desc->bake_private,
            R_PushArg(&s_wanted, sizeof(s_wanted)),
            R_PushArg(&gen, sizeof(gen)),
        },
    });
}

static bool bb_stale(struct bb_model_desc *desc, int gen)
{
    return (SDL_AtomicGet(&desc->state) == BB_STATE_READY)
        && (SDL_AtomicGet(&desc->baked_gen) != gen)
        && (desc->inflight_gen != gen);
}

static bool bb_job_slot_free(void)
{
    return (SDL_AtomicGet(&s_jobs_inflight) < CONFIG_BILLBOARD_JOBS_INFLIGHT);
}

/* The sprite's framing at one elevation. The depth extent bounds how far any
 * baked point lies from the card plane, which passes through the model origin.
 */
static void bb_frame_for_elev(const struct bb_model_desc *desc, int elev_deg,
                              struct bb_frame *out)
{
    float elev = DEG_TO_RAD((float)elev_deg);
    float cos_e = cosf(elev);
    float sin_e = sinf(elev);
    float vmin = desc->ymin * cos_e - desc->radius * sin_e;
    float vmax = desc->ymax * cos_e + desc->radius * sin_e;

    out->world_size = (vec2_t){2.0f * desc->radius, vmax - vmin};
    out->anchor_off = (vec2_t){0.0f, (vmax + vmin) / 2.0f};
    out->depth_extent = out->world_size.x / 2.0f + out->world_size.y / 2.0f
                      + fabsf(out->anchor_off.y);

    /* The orthographic bake's window depth is linear in the view distance */
    float dist = bb_bake_cam_dist(out);
    float far = bb_bake_far(out);
    out->depth_decode = (vec2_t){far - BB_BAKE_NEAR, BB_BAKE_NEAR - dist};
}

static float bb_bake_cam_dist(const struct bb_frame *frame)
{
    return 2.0f * frame->depth_extent + 10.0f;
}

static float bb_bake_far(const struct bb_frame *frame)
{
    return 2.0f * bb_bake_cam_dist(frame) + 10.0f;
}

/* The bake camera looks at the model origin from the given elevation, along
 * the -Z world axis; the model itself is rotated per azimuth cell.
 */
static void bb_bake_view_proj(const struct bb_frame *frame, int elev_deg,
                              mat4x4_t *out_view, vec3_t *out_pos, mat4x4_t *out_proj)
{
    float elev = DEG_TO_RAD((float)elev_deg);
    float half_w = frame->world_size.x / 2.0f;
    float half_h = frame->world_size.y / 2.0f;
    float dist = bb_bake_cam_dist(frame);

    vec3_t pos = (vec3_t){0.0f, dist * sinf(elev), dist * cosf(elev)};
    vec3_t target = (vec3_t){0.0f, 0.0f, 0.0f};
    /* MakeLookAt uses 'up' directly as the view basis row, so it must be the
     * camera's true (tilted) up rather than the world up.
     */
    vec3_t up = (vec3_t){0.0f, cosf(elev), -sinf(elev)};
    PFM_Mat4x4_MakeLookAt(&pos, &target, &up, out_view);

    PFM_Mat4x4_MakeOrthographic(-half_w, half_w,
        frame->anchor_off.y - half_h, frame->anchor_off.y + half_h,
        BB_BAKE_NEAR, bb_bake_far(frame), out_proj);
    *out_pos = pos;
}

static float bb_light_half_extent(const struct bb_model_desc *desc)
{
    float yhalf = (desc->ymax - desc->ymin) / 2.0f;
    return sqrtf(desc->radius * desc->radius + yhalf * yhalf) + 2.0f;
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
    float sphere = bb_light_half_extent(desc);

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

static void bb_atlas_free(struct bb_atlas *atlas)
{
    glDeleteTextures(1, &atlas->color);
    glDeleteTextures(1, &atlas->depth);
    glDeleteTextures(1, &atlas->surface);
    glDeleteTextures(1, &atlas->sil);
    *atlas = (struct bb_atlas){0};
}

/* An array with every mip level allocated, the first filled from 'data' if
 * given; leaves it bound
 */
static void bb_alloc_mipped(GLuint *out, GLint ifmt, GLenum fmt, int cell_res, int nslices,
                            const void *data)
{
    glGenTextures(1, out);
    glBindTexture(GL_TEXTURE_2D_ARRAY, *out);
    for(int level = 0, res = cell_res; res >= 1; level++, res /= 2) {
        glTexImage3D(GL_TEXTURE_2D_ARRAY, level, ifmt, res, res, nslices, 0,
            fmt, GL_UNSIGNED_BYTE, (level == 0) ? data : NULL);
    }
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* A depth array read back as plain values: the baked depths must not be
 * blended across the silhouette's edge
 */
static void bb_alloc_depth(GLuint *out, int res, int nslices, const void *data)
{
    glGenTextures(1, out);
    glBindTexture(GL_TEXTURE_2D_ARRAY, *out);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT16, res, res,
        nslices, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, data);

    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_NONE);
}

static bool bb_atlas_alloc(const struct bb_model_desc *desc, const struct impostor_cache *data,
                           struct bb_atlas *out)
{
    ASSERT_IN_RENDER_THREAD();

    int cell_res = desc->cell_res;
    int nslices = desc->total_slices;
    bb_alloc_depth(&out->depth, cell_res, nslices, data ? data->depth : NULL);
    bb_alloc_depth(&out->sil, desc->sil_res, nslices, data ? data->sil : NULL);

    bb_alloc_mipped(&out->color, GL_RGBA8, GL_RGBA, cell_res, nslices,
        data ? data->pixels : NULL);
    bb_alloc_mipped(&out->surface, GL_RGBA8, GL_RGBA, cell_res, nslices,
        data ? data->surface : NULL);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    if(glGetError() != GL_NO_ERROR) {
        bb_atlas_free(out);
        return false;
    }
    return true;
}

static void bb_state_save(struct bb_saved_state *out)
{
    glGetIntegerv(GL_VIEWPORT, out->viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &out->fb);
    for(int i = 0; i < ARR_SIZE(s_bake_unames); i++) {
        out->have_uval[i] = R_GL_StateGet(s_bake_unames[i], &out->uvals[i]);
    }
}

static void bb_state_restore(const struct bb_saved_state *in)
{
    R_GL_ShadowMapUseScene();
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    glViewport(in->viewport[0], in->viewport[1], in->viewport[2], in->viewport[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, in->fb);

    for(int i = 0; i < ARR_SIZE(s_bake_unames); i++) {
        if(in->have_uval[i]) {
            R_GL_StateSet(s_bake_unames[i], in->uvals[i]);
        }
    }
}

static void bb_job_init(struct bb_job *job, struct bb_model_desc *desc,
                        const void *render_private, const struct bb_variant *var,
                        int gen, bool cached)
{
    *job = (struct bb_job){
        .desc = desc,
        .render_private = render_private,
        .var = *var,
        .gen = gen,
        .cached = cached,
    };
    bb_variant_cache_name(desc, var, job->cache_name, sizeof(job->cache_name));
    job->tag = bb_variant_tag(desc, var);
    bb_frame_for_elev(desc, var->elev_deg, &job->frame);

    PFM_Mat4x4_Identity(&job->ls_trans);
    if(var->shadowed) {
        bb_light_space_trans(desc, &var->light_pos, &job->ls_trans);
    }
    PFM_Mat4x4_Inverse(&job->ls_trans, &job->frame.sil_unproj);
    job->frame.shadow_offset = 2.0f * (2.0f * bb_light_half_extent(desc) / desc->sil_res);
}

static bool bb_job_load_cached(struct bb_job *job)
{
    if(!job->cached)
        return false;

    struct impostor_cache cache;
    if(!AssetCache_ImpostorLoad(job->cache_name, job->tag, &cache))
        return false;

    const struct bb_model_desc *desc = job->desc;
    bool ok = (cache.cell_res == desc->cell_res)
           && (cache.sil_res == desc->sil_res)
           && (cache.nslices == desc->total_slices)
           && bb_atlas_alloc(desc, &cache, &job->atlas);
    AssetCache_ImpostorRelease(&cache);

    if(ok) {
        job->loaded = true;
        job->next_slice = desc->total_slices;
    }
    return ok;
}

static bool bb_job_begin(struct bb_job *job)
{
    const struct bb_model_desc *desc = job->desc;
    if(!bb_atlas_alloc(desc, NULL, &job->atlas))
        return false;

    glGenFramebuffers(1, &job->fb);
    glGenFramebuffers(2, job->mip_fbs);

    /* The silhouette target is depth only */
    GLint old;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old);
    glGenFramebuffers(1, &job->sil_fb);
    glBindFramebuffer(GL_FRAMEBUFFER, job->sil_fb);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, old);
    job->mipped = true;
    return true;
}

static void bb_job_free_fbs(struct bb_job *job)
{
    glDeleteFramebuffers(1, &job->fb);
    glDeleteFramebuffers(1, &job->sil_fb);
    glDeleteFramebuffers(2, job->mip_fbs);
    job->fb = 0;
    job->sil_fb = 0;
    job->mip_fbs[0] = job->mip_fbs[1] = 0;
}

static void bb_blit_mips(GLuint tex, int cell_res, int slice)
{
    int src = cell_res;
    for(int level = 1; src > 1; level++, src /= 2) {
        int dst = src / 2;
        glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, level - 1, slice);
        glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, level, slice);
        glBlitFramebuffer(0, 0, src, src, 0, 0, dst, dst, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
}

/* Downsamples one freshly baked cell through its mip chains, so that no
 * whole-atlas mipmap generation is needed when the job ends.
 */
static void bb_job_cell_mips(struct bb_job *job, int slice)
{
    glBindFramebuffer(GL_READ_FRAMEBUFFER, job->mip_fbs[0]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, job->mip_fbs[1]);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    GLenum draw_buffs[1] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(ARR_SIZE(draw_buffs), draw_buffs);

    bb_blit_mips(job->atlas.color, job->desc->cell_res, slice);
    bb_blit_mips(job->atlas.surface, job->desc->cell_res, slice);
}

/* The clip (NULL for a model without animations), keyframe and azimuth an
 * atlas slice holds
 */
static const struct bb_clip_desc *bb_slice_cell(const struct bb_model_desc *desc, int slice,
                                                int *out_kf, int *out_az)
{
    const struct bb_clip_desc *clip = NULL;
    int rel = slice;
    for(int c = 0; c < desc->nclips; c++) {
        const struct bb_clip_desc *curr = &desc->clips[c];
        if(slice < curr->first_slice + curr->nkf * desc->nazimuths) {
            clip = curr;
            rel = slice - curr->first_slice;
            break;
        }
    }
    *out_kf = rel / desc->nazimuths;
    *out_az = rel % desc->nazimuths;
    return clip;
}

/* The cell as the light sees it, for casting into the scene's shadow map. An
 * unshadowed variant leaves it clear, as no shadow map is drawn for it.
 */
static bool bb_job_cell_silhouette(struct bb_job *job, int slice, mat4x4_t *model)
{
    GLint old;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old);
    glBindFramebuffer(GL_FRAMEBUFFER, job->sil_fb);
    glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, job->atlas.sil, 0, slice);
    bool complete = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glBindFramebuffer(GL_FRAMEBUFFER, old);
    if(!complete)
        return false;

    R_GL_DepthPassBeginTarget(&job->ls_trans, job->sil_fb, job->desc->sil_res);
    if(job->var.shadowed) {
        R_GL_RenderDepthMap(job->render_private, model);
    }
    R_GL_DepthPassEnd();
    return true;
}

/* Bakes up to 'max_cells' further cells of the job's atlas. Returns the number
 * baked, or -1 on failure. Leaves the bake's GL and uniform state in place.
 */
static int bb_job_cells(struct bb_job *job, int max_cells)
{
    ASSERT_IN_RENDER_THREAD();

    const struct bb_model_desc *desc = job->desc;
    const struct bb_variant *var = &job->var;

    mat4x4_t view, proj;
    vec3_t cam_pos;
    bb_bake_view_proj(&job->frame, var->elev_deg, &view, &cam_pos, &proj);
    R_GL_SetViewMatAndPos(&view, &cam_pos);
    R_GL_SetProj(&proj);
    R_GL_SetLightPos(&var->light_pos);
    R_GL_SetAmbientLightColor(&var->ambient_color);
    R_GL_SetLightEmitColor(&var->emit_color);
    /* Mode 2 tells the mesh shaders to write the surface rather than its
     * lighting, which the sprite applies when drawn
     */
    R_GL_StateSet(GL_U_SHADOWS_ON, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = 2
    });

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glViewport(0, 0, desc->cell_res, desc->cell_res);

    /* Past the silhouette the cells are clear: zero coverage, and a zero
     * encoded normal, so that minified texels carry their surface scaled by
     * their coverage like their colour
     */
    const GLfloat clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const GLfloat clear_surface[4] = {0.5f, 0.5f, 0.0f, 0.0f};
    bool translucent = false;
    int nbaked = 0;

    while(nbaked < max_cells && job->next_slice < desc->total_slices) {

        int slice = job->next_slice;
        glBindFramebuffer(GL_FRAMEBUFFER, job->fb);
        GLenum draw_buffs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
        glDrawBuffers(ARR_SIZE(draw_buffs), draw_buffs);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            job->atlas.color, 0, slice);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
            job->atlas.surface, 0, slice);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
            job->atlas.depth, 0, slice);
        if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            return -1;

        int kf, az;
        const struct bb_clip_desc *clip = bb_slice_cell(desc, slice, &kf, &az);

        mat4x4_t model;
        PFM_Mat4x4_MakeRotY(az * (2.0f * M_PI / desc->nazimuths), &model);

        if(clip) {
            /* The inverse transpose of a pure rotation is the rotation */
            mat4x4_t normal = model;
            R_GL_AnimSetUniforms(&normal, (struct anim_pose_data_desc*)&clip->kf_pose[kf]);
        }
        if(!bb_job_cell_silhouette(job, slice, &model))
            return -1;
        glClearBufferfv(GL_COLOR, 0, clear_color);
        glClearBufferfv(GL_COLOR, 1, clear_surface);
        glClear(GL_DEPTH_BUFFER_BIT);
        R_GL_Draw(job->render_private, &model, &translucent);
        bb_job_cell_mips(job, slice);

        job->next_slice++;
        nbaked++;
    }
    return nbaked;
}

static void bb_job_store(const struct bb_job *job)
{
    const struct bb_model_desc *desc = job->desc;
    size_t ntexels = (size_t)desc->cell_res * desc->cell_res * desc->total_slices;
    size_t nsil = (size_t)desc->sil_res * desc->sil_res * desc->total_slices;
    void *pixels = PF_MALLOC(ntexels * 4);
    void *depth = PF_MALLOC(ntexels * sizeof(uint16_t));
    void *surface = PF_MALLOC(ntexels * 4);
    void *sil = PF_MALLOC(nsil * sizeof(uint16_t));

    if(pixels && depth && surface && sil) {
        glBindTexture(GL_TEXTURE_2D_ARRAY, job->atlas.color);
        glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glBindTexture(GL_TEXTURE_2D_ARRAY, job->atlas.depth);
        glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, depth);
        glBindTexture(GL_TEXTURE_2D_ARRAY, job->atlas.surface);
        glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, GL_UNSIGNED_BYTE, surface);
        glBindTexture(GL_TEXTURE_2D_ARRAY, job->atlas.sil);
        glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, sil);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

        AssetCache_ImpostorStore(job->cache_name, job->tag, &(struct impostor_cache){
            .cell_res = desc->cell_res,
            .nslices = desc->total_slices,
            .sil_res = desc->sil_res,
            .pixels = pixels,
            .depth = depth,
            .surface = surface,
            .sil = sil,
        });
    }
    if(pixels)
        PF_FREE(pixels);
    if(depth)
        PF_FREE(depth);
    if(surface)
        PF_FREE(surface);
    if(sil)
        PF_FREE(sil);
}

static void bb_job_abandon(struct bb_job *job)
{
    bb_job_free_fbs(job);
    bb_atlas_free(&job->atlas);
}

/* Swaps a finished atlas in for the resident one. A failed bake keeps the
 * resident atlas drawing rather than dropping to meshes; either way the
 * generation is marked baked, which stops the retries.
 */
static void bb_job_end(struct bb_job *job, bool ok)
{
    struct bb_model_desc *desc = job->desc;
    bb_job_free_fbs(job);

    if(ok) {
        if(!job->mipped) {
            glBindTexture(GL_TEXTURE_2D_ARRAY, job->atlas.color);
            glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
            glBindTexture(GL_TEXTURE_2D_ARRAY, job->atlas.surface);
            glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
            glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        }

        if(job->cached && !job->loaded) {
            bb_job_store(job);
        }

        bb_atlas_free(&desc->atlas);
        desc->atlas = job->atlas;
        desc->frame = job->frame;
        SDL_AtomicSet(&desc->baked_elev, job->var.elev_deg);
        SDL_AtomicSet(&desc->state, BB_STATE_READY);
    }else{
        bb_atlas_free(&job->atlas);
        if(!desc->atlas.color) {
            SDL_AtomicSet(&desc->state, BB_STATE_FAILED);
        }
    }
    SDL_AtomicSet(&desc->baked_gen, job->gen);
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

    glVertexAttribIPointer(5, 1, GL_UNSIGNED_INT, stride,
        (void*)(base + offsetof(struct bb_gpu_inst, uid)));
    glEnableVertexAttribArray(5);
    glVertexAttribDivisor(5, 1);
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

void R_Billboard_Register(void *render_key, void *bake_private, const char *basedir,
                          const char *filename, const char *bake_filename,
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
    desc->bake_private = bake_private;
    desc->nazimuths = bb_azimuth_count(aabb, nclips);
    desc->nclips = nclips;
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

    desc->radius = radius;
    desc->ymin = ymin;
    desc->ymax = ymax;

    /* The cells are sized for the RTS camera's elevation */
    struct bb_frame ref;
    bb_frame_for_elev(desc, bb_rts_elev_deg(), &ref);
    desc->ref_extent = MAX(ref.world_size.x, ref.world_size.y);
    desc->cell_res = bb_pow2_cell_res(desc->ref_extent * CONFIG_BILLBOARD_PX_PER_WU);
    desc->sil_res = MAX(desc->cell_res / CONFIG_BILLBOARD_SIL_RES_DIV, 16);

    if(desc->cell_res < desc->ref_extent * CONFIG_BILLBOARD_MIN_PX_PER_WU) {
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
    if(bake_filename) {
        pf_snprintf(full_path, sizeof(full_path), "%s/%s/%s", g_basepath, basedir, bake_filename);
        tag = bb_mix(tag, AssetCache_SourceTag(full_path));
    }
    tag = bb_mix(tag, CONFIG_BILLBOARD_CACHE_VER);
    tag = bb_mix(tag, desc->nazimuths);
    tag = bb_mix(tag, desc->total_slices);
    tag = bb_mix(tag, desc->cell_res);
    tag = bb_mix(tag, (uint64_t)(CONFIG_BILLBOARD_PX_PER_WU * 16.0f));
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

    desc->demand_frame = s_frame;
    return desc;
}

bool R_Billboard_ElevationMatches(const struct bb_model_desc *desc, float elev_deg)
{
    float baked = (float)SDL_AtomicGet((SDL_atomic_t*)&desc->baked_elev);
    return (fabsf(baked - elev_deg) <= CONFIG_BILLBOARD_MAX_PITCH_DEV_DEG);
}

bool R_Billboard_ViewMatches(const struct bb_model_desc *desc, vec3_t view_dir)
{
    float max_pitch_dev = (desc->nclips > 0) ? CONFIG_BILLBOARD_UNIT_MAX_PITCH_DEV_DEG
                                             : CONFIG_BILLBOARD_MAX_PITCH_DEV_DEG;

    float hlen = sqrtf(view_dir.x * view_dir.x + view_dir.z * view_dir.z);
    float elev = RAD_TO_DEG(atan2f(-view_dir.y, hlen));
    float baked = (float)SDL_AtomicGet((SDL_atomic_t*)&desc->baked_elev);
    return (fabsf(baked - elev) <= max_pitch_dev);
}

bool R_Billboard_ScaleEligible(const struct bb_model_desc *desc, vec2_t scale)
{
    return (R_Billboard_Density(desc, scale) >= CONFIG_BILLBOARD_MIN_PX_PER_WU);
}

float R_Billboard_Density(const struct bb_model_desc *desc, vec2_t scale)
{
    float smax = MAX(scale.x, scale.y);
    return desc->cell_res / (desc->ref_extent * smax);
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
    bool cached = bb_variant_cacheable(&s_wanted);
    SDL_AtomicSet(&desc->state, BB_STATE_QUEUED);
    desc->inflight_gen = gen;
    R_PushCmd((struct rcmd){
        .func = R_GL_Billboard_EnsureBaked,
        .nargs = 5,
        .args = {
            desc,
            desc->bake_private,
            R_PushArg(&s_wanted, sizeof(s_wanted)),
            R_PushArg(&gen, sizeof(gen)),
            R_PushArg(&cached, sizeof(cached)),
        },
    });
}

void R_Billboard_Tick(void)
{
    ASSERT_IN_MAIN_THREAD();

    struct sval setting;
    bool enabled = (Settings_Get("pf.video.billboards_enabled", &setting) == SS_OKAY)
                 && setting.as_bool;
    if(!enabled || !s_desc_table)
        return;

    s_frame++;
    bb_track_camera();
    int gen = bb_wanted_refresh();

    /* Models wanted on screen last frame re-bake ahead of the rest */
    struct bb_model_desc *desc;
    for(int pass = 0; pass < 2 && bb_job_slot_free(); pass++) {
        kh_foreach_value(s_desc_table, desc, {
            if(!bb_job_slot_free())
                break;
            if(pass == 0 && (s_frame - desc->demand_frame) > 1)
                continue;
            if(!bb_stale(desc, gen))
                continue;
            bb_enqueue_job(desc, gen);
        });
    }

    if(SDL_AtomicGet(&s_jobs_inflight) == 0)
        return;

    R_PushCmd((struct rcmd){
        .func = R_GL_Billboard_BakePump,
        .nargs = 1,
        .args = { R_PushArg(&gen, sizeof(gen)) },
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

    for(int i = 0; i < s_njobs; i++) {
        bb_job_abandon(&s_jobs[i]);
    }
    s_njobs = 0;
    SDL_AtomicSet(&s_jobs_inflight, 0);

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
                                const struct bb_variant *var, const int *gen, const bool *cached)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    if(SDL_AtomicGet(&desc->baked_gen) == *gen)
        GL_PERF_RETURN_VOID();

    struct bb_job job;
    bb_job_init(&job, desc, render_private, var, *gen, *cached);

    /* Warm path: upload the cached atlas without any GL baking */
    bool ok = bb_job_load_cached(&job);
    if(!ok) {
        R_GL_LoadingScreenPushModel(job.cache_name);
        struct bb_saved_state saved;
        bb_state_save(&saved);
        ok = bb_job_begin(&job) && (bb_job_cells(&job, INT_MAX) >= 0);
        bb_state_restore(&saved);
        R_GL_LoadingScreenPopModel();
    }
    bb_job_end(&job, ok);

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_Billboard_BakeBegin(struct bb_model_desc *desc, const void *render_private,
                              const struct bb_variant *var, const int *gen)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();
    assert(s_njobs < CONFIG_BILLBOARD_JOBS_INFLIGHT);

    struct bb_job *job = &s_jobs[s_njobs];
    bb_job_init(job, desc, render_private, var, *gen, false);

    if(!bb_job_begin(job)) {
        bb_job_end(job, false);
        SDL_AtomicAdd(&s_jobs_inflight, -1);
    }else{
        s_njobs++;
    }

    GL_ASSERT_OK();
    GL_PERF_RETURN_VOID();
}

void R_GL_Billboard_BakePump(const int *gen)
{
    GL_PERF_ENTER();
    ASSERT_IN_RENDER_THREAD();

    /* Drop the jobs for a variant no longer wanted */
    int nkept = 0;
    for(int i = 0; i < s_njobs; i++) {
        if(s_jobs[i].gen != *gen) {
            bb_job_abandon(&s_jobs[i]);
            SDL_AtomicAdd(&s_jobs_inflight, -1);
            continue;
        }
        s_jobs[nkept++] = s_jobs[i];
    }
    s_njobs = nkept;

    if(s_njobs == 0)
        GL_PERF_RETURN_VOID();

    struct bb_saved_state saved;
    bb_state_save(&saved);

    int budget = CONFIG_BILLBOARD_BAKE_CELLS_PER_FRAME;
    while(budget > 0 && s_njobs > 0) {

        struct bb_job *job = &s_jobs[0];
        int nbaked = bb_job_cells(job, budget);
        if(nbaked >= 0 && job->next_slice < job->desc->total_slices)
            break;

        budget -= MAX(nbaked, 0);
        bb_job_end(job, nbaked >= 0);
        SDL_AtomicAdd(&s_jobs_inflight, -1);
        memmove(&s_jobs[0], &s_jobs[1], (s_njobs - 1) * sizeof(struct bb_job));
        s_njobs--;
    }

    bb_state_restore(&saved);
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
            .uid = curr->uid,
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

static void bb_draw_desc_runs(const vec_rbill_t *list, GLuint prog, bool colour_pass)
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

        if(!desc->atlas.color) {
            run_start = run_end;
            continue;
        }

        R_GL_StateSet(GL_U_BB_NAZIMUTHS, (struct uval){
            .type = UTYPE_INT,
            .val.as_int = desc->nazimuths
        });
        R_GL_StateInstall(GL_U_BB_NAZIMUTHS, prog);

        if(!colour_pass) {
            R_GL_StateSet(GL_U_BB_SIL_UNPROJ, (struct uval){
                .type = UTYPE_MAT4,
                .val.as_mat4 = desc->frame.sil_unproj
            });
            R_GL_StateInstall(GL_U_BB_SIL_UNPROJ, prog);

            glActiveTexture(SIL_TUNIT);
            glBindTexture(GL_TEXTURE_2D_ARRAY, desc->atlas.sil);
        }else{
            R_GL_StateSet(GL_U_BB_WORLD_SIZE, (struct uval){
                .type = UTYPE_VEC2,
                .val.as_vec2 = desc->frame.world_size
            });
            R_GL_StateInstall(GL_U_BB_WORLD_SIZE, prog);

            R_GL_StateSet(GL_U_BB_ANCHOR_OFF, (struct uval){
                .type = UTYPE_VEC2,
                .val.as_vec2 = desc->frame.anchor_off
            });
            R_GL_StateInstall(GL_U_BB_ANCHOR_OFF, prog);

            R_GL_StateSet(GL_U_BB_DEPTH_EXTENT, (struct uval){
                .type = UTYPE_FLOAT,
                .val.as_float = desc->frame.depth_extent
            });
            R_GL_StateInstall(GL_U_BB_DEPTH_EXTENT, prog);

            R_GL_StateSet(GL_U_BB_DEPTH_DECODE, (struct uval){
                .type = UTYPE_VEC2,
                .val.as_vec2 = desc->frame.depth_decode
            });
            R_GL_StateInstall(GL_U_BB_DEPTH_DECODE, prog);

            R_GL_StateSet(GL_U_BB_SHADOW_OFFSET, (struct uval){
                .type = UTYPE_FLOAT,
                .val.as_float = desc->frame.shadow_offset
            });
            R_GL_StateInstall(GL_U_BB_SHADOW_OFFSET, prog);

            glActiveTexture(DEPTH_TUNIT);
            glBindTexture(GL_TEXTURE_2D_ARRAY, desc->atlas.depth);
            glActiveTexture(SURFACE_TUNIT);
            glBindTexture(GL_TEXTURE_2D_ARRAY, desc->atlas.surface);
            glActiveTexture(COLOR_TUNIT);
            glBindTexture(GL_TEXTURE_2D_ARRAY, desc->atlas.color);
        }
        bb_point_inst_attrs(run_start);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)(run_end - run_start));

        run_start = run_end;
    }
}

void R_GL_Billboard_Draw(struct render_input *in)
{
    R_GL_Billboard_DrawGraded(in, false);
}

void R_GL_Billboard_DrawGraded(struct render_input *in, bool graded)
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
        .val.as_int = COLOR_TUNIT - GL_TEXTURE0
    });
    R_GL_StateSet(GL_U_TEX_ARRAY1, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = DEPTH_TUNIT - GL_TEXTURE0
    });
    R_GL_StateSet(GL_U_TEX_ARRAY2, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = SURFACE_TUNIT - GL_TEXTURE0
    });
    R_GL_PostProcess_GradeBind(GRADE_TUNIT, graded);
    R_GL_Shader_Install("billboard");
    GLuint prog = R_GL_Shader_GetProgForName("billboard");
    R_GL_ShadowMapBind();

    /* The engine's front faces are clockwise; sidestep the quad's winding */
    glDisable(GL_CULL_FACE);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(s_vao);

    bb_draw_desc_runs(&in->cam_vis_bill, prog, true);

    R_GL_PostProcess_GradeBind(GRADE_TUNIT, false);
    glEnable(GL_CULL_FACE);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glActiveTexture(DEPTH_TUNIT);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    glActiveTexture(SURFACE_TUNIT);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    glActiveTexture(COLOR_TUNIT);
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

    R_GL_StateSet(GL_U_TEX_ARRAY0, (struct uval){
        .type = UTYPE_INT,
        .val.as_int = SIL_TUNIT - GL_TEXTURE0
    });
    /* The atlases' bake frame turned back to the world's */
    R_GL_StateSet(GL_U_BB_SIL_DELTA, (struct uval){
        .type = UTYPE_FLOAT,
        .val.as_float = bb_bake_frame_delta()
    });
    R_GL_Shader_Install("billboard.depth");
    GLuint prog = R_GL_Shader_GetProgForName("billboard.depth");

    glDisable(GL_CULL_FACE);
    glActiveTexture(SIL_TUNIT);
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
