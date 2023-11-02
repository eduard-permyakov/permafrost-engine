/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020-2023 Eduard Permyakov 
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

#include "region.h"
#include "game_private.h"
#include "position.h"
#include "fog_of_war.h"
#include "../ui.h"
#include "../main.h"
#include "../camera.h"
#include "../event.h"
#include "../sched.h"
#include "../phys/public/collision.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/khash.h"
#include "../lib/public/vec.h"
#include "../lib/public/mem.h"

#include <SDL.h>
#include <assert.h>
#include <math.h>


#define MAX(a, b)    ((a) > (b) ? (a) : (b))
#define ARR_SIZE(a)  (sizeof(a)/sizeof(a[0]))
#define EPSILON      (1.0f/1024)

#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

VEC_TYPE(str, const char*)
VEC_IMPL(static inline, str, const char*)

VEC_TYPE(uid, uint32_t)
VEC_IMPL(static inline, uid, uint32_t)

struct region{
    enum region_type type;
    union{
        float radius;
        struct{
            float xlen; 
            float zlen;
        };
    };
    bool shown;
    vec2_t pos;
    vec_uid_t curr_ents;
    vec_uid_t prev_ents;
};

enum op{
    ADD, REMOVE
};

KHASH_MAP_INIT_STR(region, struct region)
KHASH_SET_INIT_STR(name)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map *s_map;
static khash_t(region)  *s_regions;
static bool              s_render = false;
/* Keep track of which regions intersect every chunk, 
 * making a poor man's 2-level tree */
static vec_str_t        *s_intersecting;
static khash_t(name)    *s_dirty;
/* Keep the event argument strings around for one tick, so that 
 * they can be used by the event handlers safely */
static vec_str_t         s_eventargs;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool region_intersects_chunk(const struct region *reg, struct map_resolution res, struct tile_desc td)
{
    struct box chunk = M_Tile_ChunkBounds(res, M_GetPos(s_map), td.chunk_r, td.chunk_c);

    switch(reg->type) {
    case REGION_CIRCLE: {
        return C_CircleRectIntersection(reg->pos, reg->radius, chunk);
    }
    case REGION_RECTANGLE: {
        struct box bounds = (struct box) {
            reg->pos.x + reg->xlen/2.0,
            reg->pos.z - reg->zlen/2.0,
            reg->xlen,
            reg->zlen
        };
        return C_RectRectIntersection(bounds, chunk);
    }
    default: return (assert(0), false);
    }
}

static bool compare_keys(const char **a, const char **b)
{
    return *a == *b;
}

static bool compare_uids(uint32_t *a, uint32_t *b)
{
    return *a == *b;
}

static int compare_uint32s(const void* a, const void* b)
{
    uint32_t uida = *(uint32_t*)a;
    uint32_t uidb = *(uint32_t*)b;
    return (uida - uidb);
}

static void region_update_intersecting(const char *name, const struct region *reg, int op)
{
    struct map_resolution res;
    M_GetResolution(s_map, &res);

    int delta = 0;
    int chunklen = MAX(X_COORDS_PER_TILE * res.chunk_w * res.tile_w, Z_COORDS_PER_TILE * res.chunk_h * res.tile_h);

    switch(reg->type) {
    case REGION_CIRCLE:
        delta = ceil(reg->radius / chunklen);
        break;
    case REGION_RECTANGLE:
        delta = MAX(ceil(reg->xlen/2.0f / chunklen), ceil(reg->zlen/2.0f / chunklen));
        break;
    default: assert(0);
    }

    struct tile_desc td;
    if(!M_Tile_DescForPoint2D(res, M_GetPos(s_map), reg->pos, &td))
        return;

    for(int dr = -delta; dr <= delta; dr++) {
    for(int dc = -delta; dc <= delta; dc++) {

        struct tile_desc curr = td;
        if(!M_Tile_RelativeDesc(res, &curr, dc * res.tile_h, dr * res.tile_w))
            continue;

        if(!region_intersects_chunk(reg, res, curr))
            continue;

        vec_str_t *chunk = &s_intersecting[curr.chunk_r * res.chunk_w + curr.chunk_c];

        switch(op) {
        case REMOVE: {
            int idx = vec_str_indexof(chunk, name, compare_keys);
            if(idx != -1) {
                vec_str_del(chunk, idx);
            }
            break;
        }
        case ADD: {
            vec_str_push(chunk, name);
            break;
        }
        default: assert(0);
        }
    }}
}

static bool region_add(const char *name, struct region reg)
{
    if(kh_get(region, s_regions, name) != kh_end(s_regions))
        return false;

    const char *key = pf_strdup(name);
    if(!key)
        return false;

    int status;
    khiter_t k = kh_put(region, s_regions, key, &status);
    if(status == -1)
        return false;

    kh_value(s_regions, k) = reg;
    region_update_intersecting(key, &reg, ADD);
    return true;
}

static bool region_contains(const struct region *reg, vec2_t point)
{
    switch(reg->type) {
    case REGION_CIRCLE: {
        return C_PointInsideCircle2D(point, reg->pos, reg->radius);
    }
    case REGION_RECTANGLE: {
        vec2_t corners[4] = {
            (vec2_t){reg->pos.x + reg->xlen/2.0f, reg->pos.z - reg->zlen/2.0f},
            (vec2_t){reg->pos.x - reg->xlen/2.0f, reg->pos.z - reg->zlen/2.0f},
            (vec2_t){reg->pos.x - reg->xlen/2.0f, reg->pos.z + reg->zlen/2.0f},
            (vec2_t){reg->pos.x + reg->xlen/2.0f, reg->pos.z + reg->zlen/2.0f},
        };
        return C_PointInsideRect2D(point, corners[0], corners[2], corners[1], corners[3]);
    }
    default: 
        return (assert(0), false);
    }
}

static size_t regions_at_point(vec2_t point, size_t maxout, struct region *out[], 
                               const char *out_names[])
{
    struct map_resolution res;
    M_GetResolution(s_map, &res);

    struct tile_desc td;
    if(!M_Tile_DescForPoint2D(res, M_GetPos(s_map), point, &td))
        return 0;

    size_t ret = 0;
    vec_str_t *chunk = &s_intersecting[td.chunk_r * res.chunk_w + td.chunk_c];
    for(int i = 0; i < vec_size(chunk); i++) {

        if(ret == maxout)
            break;

        const char *name = vec_AT(chunk, i);
        khiter_t k = kh_get(region, s_regions, name);
        assert(k != kh_end(s_regions));

        struct region *reg = &kh_value(s_regions, k);
        if(!region_contains(reg, point))
            continue;

        out[ret] = reg;
        out_names[ret] = name;
        ret++;
    }
    return ret;
}

static void regions_remove_ent(uint32_t uid, vec2_t pos)
{
    const char *names[512];
    struct region *regs[512];
    size_t nregs = regions_at_point(pos, ARR_SIZE(regs), regs, names);

    for(int i = 0; i < nregs; i++) {
        int idx = vec_uid_indexof(&regs[i]->curr_ents, uid, compare_uids);
        if(idx == -1)
            continue;
        vec_uid_del(&regs[i]->curr_ents, idx);
        kh_put(name, s_dirty, names[i], &(int){0});
    }
}

static void regions_add_ent(uint32_t uid, vec2_t pos)
{
    assert(Sched_UsingBigStack());

    if(!G_EntityExists(uid) || (G_FlagsGet(uid) & (ENTITY_FLAG_ZOMBIE | ENTITY_FLAG_MARKER)))
        return;

    const char *names[512];
    struct region *regs[512];
    size_t nregs = regions_at_point(pos, ARR_SIZE(regs), regs, names);

    for(int i = 0; i < nregs; i++) {

        int idx = vec_uid_indexof(&regs[i]->curr_ents, uid, compare_uids);
        if(idx != -1)
            continue;

        vec_uid_push(&regs[i]->curr_ents, uid);
        kh_put(name, s_dirty, names[i], &(int){0});
    }
}

static void region_update_ents(const char *name, struct region *reg)
{
    uint32_t ents[1024];
    size_t nents = 0;

    switch(reg->type) {
    case REGION_CIRCLE: {
        nents = G_Pos_EntsInCircle(reg->pos, reg->radius, ents, ARR_SIZE(ents));
        break;
    }
    case REGION_RECTANGLE: {
        vec2_t xz_min = (vec2_t){reg->pos.x - reg->xlen/2.0f, reg->pos.z - reg->zlen/2.0f};
        vec2_t xz_max = (vec2_t){reg->pos.x + reg->xlen/2.0f, reg->pos.z + reg->zlen/2.0f};
        nents = G_Pos_EntsInRect(xz_min, xz_max, ents, ARR_SIZE(ents));
        break;
    }
    default: assert(0);
    }

    vec_uid_reset(&reg->curr_ents);
    for(int i = 0; i < nents; i++) {
        uint32_t flags = G_FlagsGet(ents[i]);
        if(flags & ENTITY_FLAG_MARKER)
            continue;
        if(flags & ENTITY_FLAG_ZOMBIE)
            continue;
        vec_uid_push(&reg->curr_ents, ents[i]);
    }

    khiter_t k = kh_get(region, s_regions, name);
    assert(k != kh_end(s_regions));
    kh_put(name, s_dirty, kh_key(s_regions, k), &(int){0});
}

static vec2_t region_ss_pos(vec2_t pos)
{
    int width, height;
    Engine_WinDrawableSize(&width, &height);

    float y = M_HeightAtPoint(s_map, M_ClampedMapCoordinate(s_map, pos));
    vec4_t pos_homo = (vec4_t) { pos.x, y, pos.z, 1.0f };

    const struct camera *cam = G_GetActiveCamera();
    mat4x4_t view, proj;
    Camera_MakeViewMat(cam, &view);
    Camera_MakeProjMat(cam, &proj);

    vec4_t clip, tmp;
    PFM_Mat4x4_Mult4x1(&view, &pos_homo, &tmp);
    PFM_Mat4x4_Mult4x1(&proj, &tmp, &clip);
    vec3_t ndc = (vec3_t){ clip.x / clip.w, clip.y / clip.w, clip.z / clip.w };

    float screen_x = (ndc.x + 1.0f) * width/2.0f;
    float screen_y = height - ((ndc.y + 1.0f) * height/2.0f);
    return (vec2_t){screen_x, screen_y};
}

static void region_notify_changed(const char *name, struct region *reg)
{
    size_t n = reg->curr_ents.size;
    size_t m = reg->prev_ents.size;

    qsort(reg->curr_ents.array, n, sizeof(uint32_t), compare_uint32s);
    qsort(reg->prev_ents.array, m, sizeof(uint32_t), compare_uint32s);

    /* use the algorithm for finding the symmetric difference 
     * of two sorted arrays: */
    size_t nchanged = 0;
    int i = 0, j = 0;
    while(i < n && j < m) {

        if(reg->curr_ents.array[i] < reg->prev_ents.array[j]) {

            const char *arg = pf_strdup(name);
            vec_str_push(&s_eventargs, arg);

            uint32_t uid = reg->curr_ents.array[i];
            E_Entity_Notify(EVENT_ENTERED_REGION, uid, (void*)arg, ES_ENGINE);
            E_Global_Notify(EVENT_ENTERED_REGION, (void*)arg, ES_ENGINE);

            i++;
            nchanged++;

        }else if(reg->prev_ents.array[j] < reg->curr_ents.array[i]) {

            const char *arg = pf_strdup(name);
            vec_str_push(&s_eventargs, arg);

            uint32_t uid = reg->prev_ents.array[j];
            E_Entity_Notify(EVENT_EXITED_REGION, uid, (void*)arg, ES_ENGINE);
            E_Global_Notify(EVENT_EXITED_REGION, (void*)arg, ES_ENGINE);

            j++;
            nchanged++;

        }else{

            i++;
            j++;
        }
    }

    while(i < n) {
    
        const char *arg = pf_strdup(name);
        vec_str_push(&s_eventargs, arg);

        uint32_t uid = reg->curr_ents.array[i];
        E_Entity_Notify(EVENT_ENTERED_REGION, uid, (void*)arg, ES_ENGINE);
        E_Global_Notify(EVENT_ENTERED_REGION, (void*)arg, ES_ENGINE);

        i++;
        nchanged++;
    }

    while(j < m) {
    
        const char *arg = pf_strdup(name);
        vec_str_push(&s_eventargs, arg);

        uint32_t uid = reg->prev_ents.array[j];
        E_Entity_Notify(EVENT_EXITED_REGION, uid, (void*)arg, ES_ENGINE);
        E_Global_Notify(EVENT_EXITED_REGION, (void*)arg, ES_ENGINE);

        j++;
        nchanged++;
    }

    if(nchanged) {
        S_Region_NotifyContentsChanged(name);
    }

    vec_uid_reset(&reg->prev_ents);
    vec_uid_copy(&reg->prev_ents, &reg->curr_ents);
}

static void on_render_3d(void *user, void *event)
{
    const float width = 0.5f;
    const vec3_t red = (vec3_t){1.0f, 0.0f, 0.0f};

    const char *key;
    struct region reg;

    kh_foreach(s_regions, key, reg, {

        if(!s_render && !reg.shown)
            continue;

        bool explored = false;
        G_Region_Explored(key, G_GetPlayerControlledFactions(), &explored);
        if(!explored)
            continue;

        switch(reg.type) {
        case REGION_CIRCLE: {

            R_PushCmd((struct rcmd){
                .func = R_GL_DrawSelectionCircle,
                .nargs = 5,
                .args = {
                    R_PushArg(&reg.pos, sizeof(reg.pos)),
                    R_PushArg(&reg.radius, sizeof(reg.radius)),
                    R_PushArg(&width, sizeof(width)),
                    R_PushArg(&red, sizeof(red)),
                    (void*)G_GetPrevTickMap(),
                },
            });
            break;
        }
        case REGION_RECTANGLE: {

            vec2_t corners[4] = {
                (vec2_t){reg.pos.x + reg.xlen/2.0f, reg.pos.z - reg.zlen/2.0f},
                (vec2_t){reg.pos.x - reg.xlen/2.0f, reg.pos.z - reg.zlen/2.0f},
                (vec2_t){reg.pos.x - reg.xlen/2.0f, reg.pos.z + reg.zlen/2.0f},
                (vec2_t){reg.pos.x + reg.xlen/2.0f, reg.pos.z + reg.zlen/2.0f},
            };
            R_PushCmd((struct rcmd){
                .func = R_GL_DrawQuad,
                .nargs = 4,
                .args = {
                    R_PushArg(corners, sizeof(corners)),
                    R_PushArg(&width, sizeof(width)),
                    R_PushArg(&red, sizeof(red)),
                    (void*)G_GetPrevTickMap(),
                },
            });
            break;
        }
        default: assert(0);
        }

        if(!s_render)
            continue;

        float len = strlen(key) * 7.5f;
        vec2_t ss_pos = region_ss_pos(reg.pos);
        struct rect bounds = (struct rect){ss_pos.x - len/2.0, ss_pos.y, len, 16};
        struct rgba color = (struct rgba){255, 0, 0, 255};
        UI_DrawText(key, bounds, color);
    });
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Region_Init(const struct map *map)
{
    s_regions = kh_init(region);
    if(!s_regions)
        goto fail_regions;

    s_dirty = kh_init(name);
    if(!s_dirty)
        goto fail_dirty;

    struct map_resolution res;
    M_GetResolution(map, &res);

    s_intersecting = calloc(res.chunk_w * res.chunk_h, sizeof(vec_str_t));
    if(!s_intersecting)
        goto fail_intersecting;

    for(int i = 0; i < res.chunk_w * res.chunk_h; i++) {
        vec_str_t *vec = ((vec_str_t*)s_intersecting) + i;
        vec_str_init(vec);
    }

    vec_str_init(&s_eventargs);
    E_Global_Register(EVENT_RENDER_3D_POST, on_render_3d, NULL, G_ALL);
    s_map = map;
    return true;

fail_intersecting:
    kh_destroy(name, s_dirty);
fail_dirty:
    kh_destroy(region, s_regions);
fail_regions:
    return false;
}

void G_Region_Shutdown(void)
{
    struct map_resolution res;
    M_GetResolution(s_map, &res);

    for(int i = 0; i < res.chunk_w * res.chunk_h; i++) {
        vec_str_t *vec = ((vec_str_t*)s_intersecting) + i;
        vec_str_destroy(vec);
    }
    PF_FREE(s_intersecting);

    const char *key;
    struct region reg;

    kh_foreach(s_regions, key, reg, {
        PF_FREE(key);
        vec_uid_destroy(&reg.curr_ents);
        vec_uid_destroy(&reg.prev_ents);
    });

    for(int i = 0; i < vec_size(&s_eventargs); i++) {
        PF_FREE(vec_AT(&s_eventargs, i));
    }
    vec_str_destroy(&s_eventargs);

    kh_destroy(name, s_dirty);
    kh_destroy(region, s_regions);
    E_Global_Unregister(EVENT_RENDER_3D_POST, on_render_3d);
    s_map = NULL;
}

bool G_Region_AddCircle(const char *name, vec2_t pos, float radius)
{
    struct region newreg = (struct region) {
        .type = REGION_CIRCLE,
        .radius = radius,
        .shown = false,
        .pos = pos
    };
    vec_uid_init(&newreg.curr_ents);
    vec_uid_init(&newreg.prev_ents);

    if(!region_add(name, newreg))
        return false;

    region_update_ents(name, &newreg);
    return true;
}

bool G_Region_AddRectangle(const char *name, vec2_t pos, float xlen, float zlen)
{
    struct region newreg = (struct region) {
        .type = REGION_RECTANGLE,
        .xlen = xlen,
        .zlen = zlen,
        .shown = false,
        .pos = pos
    };
    vec_uid_init(&newreg.curr_ents);
    vec_uid_init(&newreg.prev_ents);

    if(!region_add(name, newreg))
        return false;

    region_update_ents(name, &newreg);
    return true;
}

void G_Region_Remove(const char *name)
{
    khiter_t k = kh_get(region, s_regions, name);
    if(k == kh_end(s_regions))
        return;

    const char *key = kh_key(s_regions, k);
    struct region *reg = &kh_value(s_regions, k);

    for(int i = 0; i < vec_size(&reg->curr_ents); i++) {

        const char *arg = pf_strdup(name);
        vec_str_push(&s_eventargs, arg);

        uint32_t uid = vec_AT(&reg->curr_ents, i);
        E_Entity_Notify(EVENT_EXITED_REGION, uid, (void*)arg, ES_ENGINE);
    }

    region_update_intersecting(key, &kh_value(s_regions, k), REMOVE);
    vec_uid_destroy(&kh_val(s_regions, k).curr_ents);
    vec_uid_destroy(&kh_val(s_regions, k).prev_ents);
    kh_del(region, s_regions, k);

    k = kh_get(name, s_dirty, name);
    if(k != kh_end(s_dirty)) {
        kh_del(name, s_dirty, k);
    }

    PF_FREE(key);
}

bool G_Region_SetPos(const char *name, vec2_t pos)
{
    khiter_t k = kh_get(region, s_regions, name);
    if(k == kh_end(s_regions))
        return false;

    const char *key = kh_key(s_regions, k);
    struct region *reg = &kh_value(s_regions, k);

    vec2_t delta;
    PFM_Vec2_Sub(&reg->pos, &pos, &delta);
    if(PFM_Vec2_Len(&delta) <= EPSILON)
        return true;

    region_update_intersecting(key, reg, REMOVE);
    reg->pos = pos;
    region_update_intersecting(key, reg, ADD);

    region_update_ents(key, reg);
    return true;
}

bool G_Region_GetPos(const char *name, vec2_t *out)
{
    khiter_t k = kh_get(region, s_regions, name);
    if(k == kh_end(s_regions))
        return false;

    *out = kh_value(s_regions, k).pos;
    return true;
}

bool G_Region_SetShown(const char *name, bool on)
{
    khiter_t k = kh_get(region, s_regions, name);
    if(k == kh_end(s_regions))
        return false;

    kh_value(s_regions, k).shown = on;
    return true;
}

bool G_Region_GetShown(const char *name, bool *out)
{
    khiter_t k = kh_get(region, s_regions, name);
    if(k == kh_end(s_regions))
        return false;

    *out = kh_value(s_regions, k).shown;
    return true;
}

int G_Region_GetEnts(const char *name, size_t maxout, uint32_t ents[])
{
    khiter_t k = kh_get(region, s_regions, name);
    if(k == kh_end(s_regions))
        return 0;

    const struct region *reg = &kh_value(s_regions, k);
    size_t ret = 0;

    for(int i = 0; i < vec_size(&reg->curr_ents); i++) {

        uint32_t ent = vec_AT(&reg->curr_ents, i);
        if(!G_EntityExists(ent))
            continue;

        if(ret == maxout)
            return ret;
        ents[ret++] = ent;
    }
    return ret;
}

bool G_Region_ContainsEnt(const char *name, uint32_t uid)
{
    khiter_t k = kh_get(region, s_regions, name);
    if(k == kh_end(s_regions))
        return false;

    const struct region *reg = &kh_value(s_regions, k);
    for(int i = 0; i < vec_size(&reg->curr_ents); i++) {
        uint32_t curr = vec_AT(&reg->curr_ents, i);
        if(curr == uid)
            return true;
    }
    return false;
}

void G_Region_RemoveRef(uint32_t uid, vec2_t oldpos)
{
    regions_remove_ent(uid, oldpos);
}

void G_Region_AddRef(uint32_t uid, vec2_t newpos)
{
    regions_add_ent(uid, newpos);
}

void G_Region_RemoveEnt(uint32_t uid)
{
    vec2_t pos = G_Pos_GetXZ(uid);
    regions_remove_ent(uid, pos);
}

void G_Region_SetRender(bool on)
{
    s_render = on;
}

bool G_Region_GetRender(void)
{
    return s_render;
}

void G_Region_Update(void)
{
    for(int i = 0; i < vec_size(&s_eventargs); i++) {
        PF_FREE(vec_AT(&s_eventargs, i));
    }
    vec_str_reset(&s_eventargs);

    for(khiter_t k = kh_begin(s_dirty); k != kh_end(s_dirty); k++) {
        if(!kh_exist(s_dirty, k))
            continue;

        const char *key = kh_key(s_dirty, k);
        khiter_t l = kh_get(region, s_regions, key);
        assert(l != kh_end(s_regions));

        struct region *reg = &kh_value(s_regions, l);
        region_notify_changed(key, reg);
    }
    kh_clear(name, s_dirty);
}
   
bool G_Region_GetRadius(const char *name, float *out)
{
    khiter_t k = kh_get(region, s_regions, name);
    if(k == kh_end(s_regions))
        return false;

    if(kh_value(s_regions, k).type != REGION_CIRCLE)
        return false;

    *out = kh_value(s_regions, k).radius;
    return true;
}

bool G_Region_GetXLen(const char *name, float *out)
{
    khiter_t k = kh_get(region, s_regions, name);
    if(k == kh_end(s_regions))
        return false;

    if(kh_value(s_regions, k).type != REGION_RECTANGLE)
        return false;

    *out = kh_value(s_regions, k).xlen;
    return true;
}

bool G_Region_GetZLen(const char *name, float *out)
{
    khiter_t k = kh_get(region, s_regions, name);
    if(k == kh_end(s_regions))
        return false;

    if(kh_value(s_regions, k).type != REGION_RECTANGLE)
        return false;

    *out = kh_value(s_regions, k).zlen;
    return true;
}

bool G_Region_ExploreFog(const char *name, int faction_id)
{
    khiter_t k = kh_get(region, s_regions, name);
    if(k == kh_end(s_regions))
        return false;

    const struct region *reg = &kh_value(s_regions, k);
    switch(reg->type) {
    case REGION_RECTANGLE:
        G_Fog_ExploreRectangle(reg->pos, faction_id, reg->xlen/2.0f, reg->zlen/2.0f);
        break;
    case REGION_CIRCLE:
        G_Fog_ExploreCircle(reg->pos, faction_id, reg->radius);
        break;
    default:
        assert(0);
        break;
    }
    return true;
}

bool G_Region_Explored(const char *name, uint16_t player_mask, bool *out)
{
    khiter_t k = kh_get(region, s_regions, name);
    if(k == kh_end(s_regions))
        return false;

    const struct region *reg = &kh_value(s_regions, k);
    switch(reg->type) {
    case REGION_RECTANGLE:
        *out = G_Fog_CircleExplored(player_mask, reg->pos, reg->radius);
        break;
    case REGION_CIRCLE:
        *out = G_Fog_RectExplored(player_mask, reg->pos, reg->xlen/2.0f, reg->zlen/2.0f);
        break;
    default:
        assert(0);
        break;
    }
    return true;
}

bool G_Region_SaveState(struct SDL_RWops *stream)
{
    struct attr render = (struct attr){
        .type = TYPE_BOOL,
        .val.as_bool = s_render
    };
    CHK_TRUE_RET(Attr_Write(stream, &render, "render"));

    struct attr num_regions = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_regions)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_regions, "num_regions"));

    Sched_TryYield();

    const char *name;
    struct region curr;

    kh_foreach(s_regions, name, curr, {
    
        struct attr reg_name = (struct attr){ .type = TYPE_STRING, };
        pf_strlcpy(reg_name.val.as_string, name, sizeof(reg_name.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &reg_name, "reg_name"));

        struct attr shown = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = curr.shown
        };
        CHK_TRUE_RET(Attr_Write(stream, &shown, "shown"));

        struct attr pos = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = curr.pos
        };
        CHK_TRUE_RET(Attr_Write(stream, &pos, "pos"));

        struct attr type = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.type
        };
        CHK_TRUE_RET(Attr_Write(stream, &type, "type"));

        switch(curr.type) {
        case REGION_CIRCLE: {

            struct attr radius = (struct attr){
                .type = TYPE_FLOAT,
                .val.as_float = curr.radius
            };
            CHK_TRUE_RET(Attr_Write(stream, &radius, "radius"));
            break;
        }
        case REGION_RECTANGLE: {

            struct attr dims = (struct attr){
                .type = TYPE_VEC2,
                .val.as_vec2 = (vec2_t){curr.xlen, curr.zlen}
            };
            CHK_TRUE_RET(Attr_Write(stream, &dims, "dims"));
            break;
        }
        default: assert(0);
        }

        struct attr num_curr = (struct attr){
            .type = TYPE_INT,
            .val.as_int = vec_size(&curr.curr_ents)
        };
        CHK_TRUE_RET(Attr_Write(stream, &num_curr, "num_curr"));

        for(int i = 0; i < vec_size(&curr.curr_ents); i++) {

            struct attr ent = (struct attr){
                .type = TYPE_INT,
                .val.as_int = vec_AT(&curr.curr_ents, i)
            };
            CHK_TRUE_RET(Attr_Write(stream, &ent, "curr_ent"));
        }

        struct attr num_prev = (struct attr){
            .type = TYPE_INT,
            .val.as_int = vec_size(&curr.prev_ents)
        };
        CHK_TRUE_RET(Attr_Write(stream, &num_prev, "num_prev"));

        for(int i = 0; i < vec_size(&curr.prev_ents); i++) {

            struct attr ent = (struct attr){
                .type = TYPE_INT,
                .val.as_int = vec_AT(&curr.prev_ents, i)
            };
            CHK_TRUE_RET(Attr_Write(stream, &ent, "prev_ent"));
        }

        Sched_TryYield();
    });

    struct attr num_dirty = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_dirty)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_dirty, "num_dirty"));

    Sched_TryYield();

    for(khiter_t k = kh_begin(s_dirty); k != kh_end(s_dirty); k++) {

        if(!kh_exist(s_dirty, k))
            continue;

        struct attr reg_name = (struct attr){ .type = TYPE_STRING, };
        const char *name = kh_key(s_dirty, k);

        pf_strlcpy(reg_name.val.as_string, name, sizeof(reg_name.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &reg_name, "reg_name"));

        Sched_TryYield();
    }

    return true;
}

bool G_Region_LoadState(struct SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_BOOL);
    s_render = attr.val.as_bool;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_regions = attr.val.as_int;
    Sched_TryYield();

    for(int i = 0; i < num_regions; i++) {

        struct attr name;
        CHK_TRUE_RET(Attr_Parse(stream, &name, true));
        CHK_TRUE_RET(name.type == TYPE_STRING);

        struct attr shown;
        CHK_TRUE_RET(Attr_Parse(stream, &shown, true));
        CHK_TRUE_RET(shown.type == TYPE_BOOL);

        struct attr pos;
        CHK_TRUE_RET(Attr_Parse(stream, &pos, true));
        CHK_TRUE_RET(pos.type == TYPE_VEC2);

        struct attr type;
        CHK_TRUE_RET(Attr_Parse(stream, &type, true));
        CHK_TRUE_RET(type.type == TYPE_INT);
        CHK_TRUE_RET(type.val.as_int == REGION_CIRCLE || type.val.as_int == REGION_RECTANGLE);

        switch(type.val.as_int) {
        case REGION_CIRCLE: {

            struct attr radius;
            CHK_TRUE_RET(Attr_Parse(stream, &radius, true));
            CHK_TRUE_RET(radius.type == TYPE_FLOAT);

            CHK_TRUE_RET(G_Region_AddCircle(name.val.as_string, pos.val.as_vec2, radius.val.as_float));
            break;
        }
        case REGION_RECTANGLE: {

            struct attr dims;
            CHK_TRUE_RET(Attr_Parse(stream, &dims, true));
            CHK_TRUE_RET(dims.type == TYPE_VEC2);

            CHK_TRUE_RET(G_Region_AddRectangle(name.val.as_string, pos.val.as_vec2, 
                dims.val.as_vec2.x, dims.val.as_vec2.z));
            break;
        }
        default: assert(0);
        }

        khiter_t k = kh_get(region, s_regions, name.val.as_string);
        assert(k != kh_end(s_regions));
        struct region *reg = &kh_value(s_regions, k);
        reg->shown = shown.val.as_bool;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        const size_t num_curr = attr.val.as_int;

        for(int j = 0; j < num_curr; j++) {

            struct attr curr;
            CHK_TRUE_RET(Attr_Parse(stream, &curr, true));
            CHK_TRUE_RET(curr.type == TYPE_INT);
            vec_uid_push(&reg->curr_ents, curr.val.as_int);
        }

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        const size_t num_prev = attr.val.as_int;

        for(int j = 0; j < num_prev; j++) {

            struct attr curr;
            CHK_TRUE_RET(Attr_Parse(stream, &curr, true));
            CHK_TRUE_RET(curr.type == TYPE_INT);
            vec_uid_push(&reg->prev_ents, curr.val.as_int);
        }
        Sched_TryYield();
    }

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const size_t num_dirty = attr.val.as_int;

    for(int i = 0; i < num_dirty; i++) {

        struct attr name;
        CHK_TRUE_RET(Attr_Parse(stream, &name, true));
        CHK_TRUE_RET(name.type == TYPE_STRING);

        khiter_t k = kh_get(region, s_regions, name.val.as_string);
        CHK_TRUE_RET(k != kh_end(s_regions));

        const char *key = kh_key(s_regions, k);
        kh_put(name, s_dirty, key, &(int){0});
        Sched_TryYield();
    }

    return true;
}

