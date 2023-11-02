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

#include "entity.h" 
#include "sched.h"
#include "task.h"
#include "event.h"
#include "main.h"
#include "camera.h"
#include "asset_load.h"
#include "render/public/render.h"
#include "render/public/render_ctrl.h"
#include "navigation/public/nav.h"
#include "game/public/game.h"
#include "anim/public/anim.h"
#include "lib/public/mpool.h"
#include "lib/public/string_intern.h"

#include <assert.h>


#define EPSILON     (1.0/1024)
#define MIN(a, b)   ((a) < (b) ? (a) : (b))
#define MAX(a, b)   ((a) > (b) ? (a) : (b))

KHASH_SET_INIT_INT(uid)
KHASH_MAP_INIT_STR(ents, kh_uid_t)

struct taglist{
    uint64_t ntags;
    const char *tags[MAX_TAGS];
};

struct dis_arg{
    uint32_t uid;
    const struct map *map;
    void (*on_finish)(void*);
    void *arg;
};

MPOOL_TYPE(taglist, struct taglist)
MPOOL_PROTOTYPES(static, taglist, struct taglist)
MPOOL_IMPL(static, taglist, struct taglist)

KHASH_MAP_INIT_INT(tags, struct taglist)
__KHASH_IMPL(trans, extern, khint32_t, struct transform, 1, kh_int_hash_func, kh_int_hash_equal)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static uint32_t          s_next_uid = 0;
static khash_t(stridx)  *s_stridx;
static mp_strbuff_t      s_stringpool;

static kh_ents_t        *s_tag_ent_map;
static kh_tags_t        *s_ent_tag_map;
static kh_trans_t       *s_ent_trans_map;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static struct taglist *entity_taglist(uint32_t uid)
{
    khiter_t k = kh_get(tags, s_ent_tag_map, uid);
    if(k == kh_end(s_ent_tag_map)) {
        int status;
        k = kh_put(tags, s_ent_tag_map, uid, &status);
        if(status == -1)
            return NULL;
        kh_value(s_ent_tag_map, k).ntags = 0;
    }
    return &kh_value(s_ent_tag_map, k);
}

static bool tag_add_entity(const char *tag, uint32_t uid)
{
    const char *str = si_intern(tag, &s_stringpool, s_stridx);
    if(!str)
        return false;

    khiter_t k = kh_get(ents, s_tag_ent_map, str);
    if(k == kh_end(s_tag_ent_map)) {
        int status;
        k = kh_put(ents, s_tag_ent_map, str, &status);
        if(status == -1)
            return false;
        memset(&kh_value(s_tag_ent_map, k), 0, sizeof(kh_uid_t));
    }

    int status;
    kh_uid_t *set = &kh_value(s_tag_ent_map, k);
    kh_put(uid, set, uid, &status);
    return (status != -1);
}

static void tag_remove_entity(const char *tag, uint32_t uid)
{
    const char *str = si_intern(tag, &s_stringpool, s_stridx);
    if(!str)
        return;

    khiter_t k = kh_get(ents, s_tag_ent_map, str);
    if(k == kh_end(s_tag_ent_map))
        return;

    kh_uid_t *set = &kh_value(s_tag_ent_map, k);

    khiter_t iter = kh_get(uid, set, uid);
    if(iter != kh_end(set)) {
        kh_del(uid, set, iter);
    }
}

static struct result ping_task(void *arg)
{
    uint32_t uid = (uintptr_t)arg;

    /* Cache all the params - the entity can die on us */
    vec2_t pos = G_Pos_GetXZ(uid);
    float radius = G_GetSelectionRadius(uid);
    const float width = 0.4f;
    vec3_t color = (vec3_t){1.0f, 1.0f, 0.0f};
    uint32_t flags = G_FlagsGet(uid);

    struct obb obb;
    Entity_CurrentOBB(uid, &obb, false);

    uint32_t elapsed = 0;
    uint32_t start = SDL_GetTicks();
    int source;

    while(elapsed < 1200) {
        Task_AwaitEvent(EVENT_RENDER_3D_POST, &source);

        uint32_t curr = SDL_GetTicks();
        elapsed = curr - start;

        if((elapsed / 400) == 1)
            continue;

        if(!G_EntityExists(uid))
            break;

        pos = G_Pos_GetXZ(uid);
        if(flags & ENTITY_FLAG_BUILDING) {

            R_PushCmd((struct rcmd){
                .func = R_GL_DrawSelectionRectangle,
                .nargs = 4,
                .args = {
                    R_PushArg(&obb, sizeof(obb)),
                    R_PushArg(&width, sizeof(width)),
                    R_PushArg(&color, sizeof(color)),
                    (void*)G_GetPrevTickMap(),
                },
            });
        }else{

            R_PushCmd((struct rcmd){
                .func = R_GL_DrawSelectionCircle,
                .nargs = 5,
                .args = {
                    R_PushArg(&pos, sizeof(pos)),
                    R_PushArg(&radius, sizeof(radius)),
                    R_PushArg(&width, sizeof(width)),
                    R_PushArg(&color, sizeof(color)),
                    (void*)G_GetPrevTickMap(),
                },
            });
        }
    }

    /* Ensure render thread is no longer touching our stack */
    Task_AwaitEvent(EVENT_UPDATE_START, &source);
    return NULL_RESULT;
}

static struct result disappear_task(void *arg)
{
    struct dis_arg darg = *(struct dis_arg*)arg;
    vec3_t start_pos = G_Pos_Get(darg.uid);
    int faction_id = G_GetFactionID(darg.uid);

    struct obb obb;
    Entity_CurrentOBB(darg.uid, &obb, false);

    if(darg.map) {
        M_NavBlockersIncrefOBB(darg.map, faction_id, &obb);
    }

    int height = obb.half_lengths[1] * 2.0f;

    vec2_t curr_shift = (vec2_t){0, 0};
    vec2_t prev_shift = (vec2_t){0, 0};

    uint32_t newflags = G_FlagsGet(darg.uid);
    newflags |= ENTITY_FLAG_TRANSLUCENT;
    G_FlagsSet(darg.uid, newflags);

    const float duration = 2500.0f;
    uint32_t elapsed = 0;
    uint32_t start = SDL_GetTicks();

    while(elapsed < duration) {
    
        Task_AwaitEvent(EVENT_UPDATE_START, &(int){0});
        uint32_t curr = SDL_GetTicks();

        /* The entity can theoretically be forecefully removed during the 
         * disappearing animation. Make sure we don't crap out if this happens
         */
        if(!G_EntityExists(darg.uid))
            return NULL_RESULT;

        uint32_t prev_long = elapsed / 250;
        uint32_t curr_long =  (curr - start) / 250;
        elapsed = curr - start;

        /* Add a slight shake */
        if(curr_long != prev_long) {
            prev_shift = curr_shift;
            curr_shift.x = ((float)rand()) / RAND_MAX * 2.5f;
            curr_shift.y = ((float)rand()) / RAND_MAX * 2.5f;
        }

        float pc = (elapsed - (prev_long * 250)) / 250;
        vec3_t curr_pos = (vec3_t){
            start_pos.x + (curr_shift.x * pc + prev_shift.x * (1.0f - pc)) / 2.0f, 
            start_pos.y - (elapsed / duration) * height,
            start_pos.z + (curr_shift.y * pc + prev_shift.y * (1.0f - pc)) / 2.0f,
        };
        G_Pos_Set(darg.uid, curr_pos);
    }

    if(darg.map) {
        M_NavBlockersDecrefOBB(darg.map, faction_id, &obb);
    }

    if(darg.on_finish) {
        darg.on_finish(darg.arg);
    }
    return NULL_RESULT;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void Entity_ModelMatrix(uint32_t uid, mat4x4_t *out)
{
    vec3_t pos = G_Pos_Get(uid);
    vec3_t scale = Entity_GetScale(uid);
    quat_t rot = Entity_GetRot(uid);

    Entity_ModelMatrixFrom(pos, rot, scale, out);
}

uint32_t Entity_NewUID(void)
{
    return s_next_uid++;
}

void Entity_SetNextUID(uint32_t uid)
{
    s_next_uid = uid;
}

void Entity_CurrentOBB(uint32_t uid, struct obb *out, bool identity)
{
    const struct aabb *aabb;
    uint32_t flags = G_FlagsGet(uid);

    if((flags & ENTITY_FLAG_ANIMATED) && !identity) {
        aabb = A_GetCurrPoseAABB(uid);
    }else {
        struct entity *ent = AL_EntityGet(uid);
        aabb = &ent->identity_aabb;
    }

    mat4x4_t model;
    Entity_ModelMatrix(uid, &model);
    vec3_t scale = Entity_GetScale(uid);
    Entity_CurrentOBBFrom(aabb, model, scale, out);
}

vec3_t Entity_CenterPos(uint32_t uid)
{
    struct obb obb;
    Entity_CurrentOBB(uid, &obb, false);
    return obb.center;
}

vec3_t Entity_TopCenterPointWS(uint32_t uid)
{
    const struct entity *ent = AL_EntityGet(uid);
    const struct aabb *aabb = &ent->identity_aabb;

    vec4_t top_center_homo = (vec4_t) {
        (aabb->x_min + aabb->x_max) / 2.0f,
        aabb->y_max,
        (aabb->z_min + aabb->z_max) / 2.0f,
        1.0f
    };

    mat4x4_t model; 
    vec4_t out_ws_homo;

    Entity_ModelMatrix(uid, &model);
    PFM_Mat4x4_Mult4x1(&model, &top_center_homo, &out_ws_homo);

    return (vec3_t) {
        out_ws_homo.x / out_ws_homo.w,
        out_ws_homo.y / out_ws_homo.w,
        out_ws_homo.z / out_ws_homo.w,
    };
}

void Entity_FaceTowards(uint32_t uid, vec2_t point)
{
    vec2_t delta;
    vec2_t pos = G_Pos_GetXZ(uid);
    PFM_Vec2_Sub(&point, &pos, &delta);

    float radians = atan2(delta.x, delta.z);

    mat4x4_t rotmat;
    PFM_Mat4x4_MakeRotY(radians, &rotmat);

    quat_t rot;
    PFM_Quat_FromRotMat(&rotmat, &rot);
    Entity_SetRot(uid, rot);
}

void Entity_Ping(uint32_t uid)
{
    uint32_t tid = Sched_Create(1, ping_task, (void*)(uintptr_t)uid, NULL, TASK_MAIN_THREAD_PINNED);
    Sched_RunSync(tid);
}

vec2_t Entity_TopScreenPos(uint32_t uid, int screenw, int screenh)
{
    vec3_t pos = Entity_TopCenterPointWS(uid);
    vec4_t pos_homo = (vec4_t) { pos.x, pos.y, pos.z, 1.0f };

    const struct camera *cam = G_GetActiveCamera();
    mat4x4_t view, proj;
    Camera_MakeViewMat(cam, &view);
    Camera_MakeProjMat(cam, &proj);

    vec4_t clip, tmp;
    PFM_Mat4x4_Mult4x1(&view, &pos_homo, &tmp);
    PFM_Mat4x4_Mult4x1(&proj, &tmp, &clip);
    vec3_t ndc = (vec3_t){ clip.x / clip.w, clip.y / clip.w, clip.z / clip.w };

    float screen_x = (ndc.x + 1.0f) * screenw/2.0f;
    float screen_y = screenh - ((ndc.y + 1.0f) * screenh/2.0f);
    return (vec2_t){screen_x, screen_y};
}

bool Entity_MaybeAdjacentFast(uint32_t a, uint32_t b, float buffer)
{
    struct obb obb_a, obb_b;
    Entity_CurrentOBB(a, &obb_a, false);
    Entity_CurrentOBB(b, &obb_b, false);

    vec2_t apos = G_Pos_GetXZ(a);
    vec2_t bpos = G_Pos_GetXZ(b);

    float alen = MAX(obb_a.half_lengths[0], obb_a.half_lengths[2]);
    float blen = MAX(obb_b.half_lengths[0], obb_b.half_lengths[2]);
    /* Take the longest possible (diagonal) distance to the edge */
    float len = (alen + blen) * sqrt(2);

    vec2_t diff;
    PFM_Vec2_Sub(&apos, &bpos, &diff);
    return (PFM_Vec2_Len(&diff) < len + buffer);
}

bool Entity_AddTag(uint32_t uid, const char *tag)
{
    struct taglist *tl = entity_taglist(uid);
    if(!tl || tl->ntags == MAX_TAGS)
        return false;
    if(Entity_HasTag(uid, tag))
        return true;
    const char *str = si_intern(tag, &s_stringpool, s_stridx);
    if(!str)
        return false;
    if(!tag_add_entity(str, uid))
        return false;
    tl->tags[tl->ntags++] = str;
    return true;
}

void Entity_RemoveTag(uint32_t uid, const char *tag)
{
    struct taglist *tl = entity_taglist(uid);
    if(!tl)
        return;
    if(!Entity_HasTag(uid, tag))
        return;
    for(int i = 0; i < tl->ntags; i++) {
        if(0 == strcmp(tl->tags[i], tag)) {
            tl->tags[i] = tl->tags[--tl->ntags];
            break;
        }
    }
    tag_remove_entity(tag, uid);
}

bool Entity_HasTag(uint32_t uid, const char *tag)
{
    khiter_t k = kh_get(ents, s_tag_ent_map, tag);
    if(k == kh_end(s_tag_ent_map))
        return false;

    kh_uid_t *set = &kh_value(s_tag_ent_map, k);
    khiter_t l = kh_get(uid, set, uid);
    return (l != kh_end(set));
}

void Entity_ClearTags(uint32_t uid)
{
    struct taglist *tl = entity_taglist(uid);
    if(!tl)
        return;
    for(int i = 0; i < tl->ntags; i++) {
        tag_remove_entity(tl->tags[i], uid);
    }
    tl->ntags = 0;
}

size_t Entity_EntsForTag(const char *tag, size_t maxout, uint32_t out[])
{
    khiter_t k = kh_get(ents, s_tag_ent_map, tag);
    if(k == kh_end(s_tag_ent_map))
        return 0;

    size_t ret = 0;
    kh_uid_t *set = &kh_value(s_tag_ent_map, k);
    for(khiter_t l = kh_begin(set); l != kh_end(set); l++) {
        if(!kh_exist(set, l))
            continue;
        if(ret == maxout)
            return ret;
        out[ret++] = kh_key(set, l);
    }
    return ret;
}

size_t Entity_TagsForEnt(uint32_t uid, size_t maxout, const char *out[])
{
    struct taglist *tl = entity_taglist(uid);
    if(!tl)
        return 0;
    size_t ret = MIN(tl->ntags, maxout);
    memcpy(out, tl->tags, ret * sizeof(char*));
    return ret;
}

void Entity_DisappearAnimated(uint32_t uid, const struct map *map, void (*on_finish)(void*), void *arg)
{
    /* It's safe to pass a pointer to stack data here due to the 'Shced_RunSync' 
     * call later - by the time we return from it, we will have copied the data.
     */
    struct dis_arg darg = (struct dis_arg){
        .uid = uid,
        .map = map,
        .on_finish = on_finish,
        .arg = arg,
    };
    uint32_t tid = Sched_Create(1, disappear_task, &darg, NULL, 
        TASK_MAIN_THREAD_PINNED | TASK_BIG_STACK);
    Sched_RunSync(tid);
}

int Entity_NavLayer(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    float radius = G_GetSelectionRadius(uid);
    return Entity_NavLayerWithRadius(radius);
}

int Entity_NavLayerWithRadius(float radius)
{
    if(radius >= 15.0f) {
        return NAV_LAYER_GROUND_7X7;
    }else if(radius >= 10.0f) {
        return NAV_LAYER_GROUND_5X5;
    }else if(radius >= 5.0f) {
        return NAV_LAYER_GROUND_3X3;
    }else{
        return NAV_LAYER_GROUND_1X1;
    }
}

bool Entity_Init(void)
{
    if(!si_init(&s_stringpool, &s_stridx, 2048))
        goto fail_strintern;

    s_tag_ent_map = kh_init(ents);
    if(!s_tag_ent_map)
        goto fail_tag_ent_map;

    s_ent_tag_map = kh_init(tags);
    if(!s_ent_tag_map)
        goto fail_ent_tag_map;

    s_ent_trans_map = kh_init(trans);
    if(!s_ent_trans_map)
        goto fail_ent_trans_map;

    return true;

fail_ent_trans_map:
    kh_destroy(tags, s_ent_tag_map);
fail_ent_tag_map:
    kh_destroy(ents, s_tag_ent_map);
fail_tag_ent_map:
    si_shutdown(&s_stringpool, s_stridx);
fail_strintern:
    return false;
}

void Entity_Shutdown(void)
{
    kh_destroy(trans, s_ent_trans_map);
    kh_destroy(tags, s_ent_tag_map);
    kh_destroy(ents, s_tag_ent_map);
    si_shutdown(&s_stringpool, s_stridx);
}

void Entity_ClearState(void)
{
    kh_clear(trans, s_ent_trans_map);
    kh_clear(tags, s_ent_tag_map);
    kh_clear(ents, s_tag_ent_map);
    si_clear(&s_stringpool, s_stridx);
}

quat_t Entity_GetRot(uint32_t uid)
{
    khiter_t k = kh_get(trans, s_ent_trans_map, uid);
    assert(k != kh_end(s_ent_trans_map));
    return kh_value(s_ent_trans_map, k).rotation;
}

void Entity_SetRot(uint32_t uid, quat_t rot)
{
    khiter_t k = kh_get(trans, s_ent_trans_map, uid);
    if(k == kh_end(s_ent_trans_map)) {
        int status;
        k = kh_put(trans, s_ent_trans_map, uid, &status);
        assert(status != -1);
    }
    kh_value(s_ent_trans_map, k).rotation = rot;
    G_UpdateBounds(uid);
}

vec3_t Entity_GetScale(uint32_t uid)
{
    khiter_t k = kh_get(trans, s_ent_trans_map, uid);
    assert(k != kh_end(s_ent_trans_map));
    return kh_value(s_ent_trans_map, k).scale;
}

void Entity_SetScale(uint32_t uid, vec3_t scale)
{
    khiter_t k = kh_get(trans, s_ent_trans_map, uid);
    if(k == kh_end(s_ent_trans_map)) {
        int status;
        k = kh_put(trans, s_ent_trans_map, uid, &status);
        assert(status != -1);
    }
    kh_value(s_ent_trans_map, k).scale = scale;
    G_UpdateBounds(uid);
}

void Entity_Remove(uint32_t uid)
{
    khiter_t k = kh_get(trans, s_ent_trans_map, uid);
    if(k != kh_end(s_ent_trans_map)) {
        kh_del(trans, s_ent_trans_map, k);
    }

    Entity_ClearTags(uid);
    k = kh_get(tags, s_ent_tag_map, uid);
    if(k != kh_end(s_ent_tag_map)) {
        kh_del(tags, s_ent_tag_map, k);
    }
}

khash_t(trans) *Entity_CopyTransforms(void)
{
    return kh_copy_trans(s_ent_trans_map);
}

quat_t Entity_GetRotFrom(khash_t(trans) *table, uint32_t uid)
{
    khiter_t k = kh_get(trans, table, uid);
    assert(k != kh_end(table));
    return kh_value(table, k).rotation;
}

vec3_t Entity_GetScaleFrom(khash_t(trans) *table, uint32_t uid)
{
    khiter_t k = kh_get(trans, table, uid);
    assert(k != kh_end(table));
    return kh_value(table, k).scale;
}

void Entity_ModelMatrixFrom(vec3_t pos, quat_t rot, vec3_t scale, mat4x4_t *out)
{
    mat4x4_t mtrans, mscale, mrot, mtmp;

    PFM_Mat4x4_MakeTrans(pos.x, pos.y, pos.z, &mtrans);
    PFM_Mat4x4_MakeScale(scale.x, scale.y, scale.z, &mscale);
    PFM_Mat4x4_RotFromQuat(&rot, &mrot);

    PFM_Mat4x4_Mult4x4(&mscale, &mrot, &mtmp);
    PFM_Mat4x4_Mult4x4(&mtrans, &mtmp, out);
}

void Entity_CurrentOBBFrom(const struct aabb *aabb, mat4x4_t model, vec3_t scale, struct obb *out)
{
    vec4_t identity_verts_homo[8] = {
        {aabb->x_min, aabb->y_min, aabb->z_min, 1.0f},
        {aabb->x_min, aabb->y_min, aabb->z_max, 1.0f},
        {aabb->x_min, aabb->y_max, aabb->z_min, 1.0f},
        {aabb->x_min, aabb->y_max, aabb->z_max, 1.0f},
        {aabb->x_max, aabb->y_min, aabb->z_min, 1.0f},
        {aabb->x_max, aabb->y_min, aabb->z_max, 1.0f},
        {aabb->x_max, aabb->y_max, aabb->z_min, 1.0f},
        {aabb->x_max, aabb->y_max, aabb->z_max, 1.0f},
    };

    vec4_t identity_center_homo = (vec4_t){
        (aabb->x_min + aabb->x_max) / 2.0f,
        (aabb->y_min + aabb->y_max) / 2.0f,
        (aabb->z_min + aabb->z_max) / 2.0f,
        1.0f
    };

    vec4_t obb_verts_homo[8];
    for(int i = 0; i < 8; i++) {
        PFM_Mat4x4_Mult4x1(&model, identity_verts_homo + i, obb_verts_homo + i);
        out->corners[i] = (vec3_t){
            obb_verts_homo[i].x / obb_verts_homo[i].w,
            obb_verts_homo[i].y / obb_verts_homo[i].w,
            obb_verts_homo[i].z / obb_verts_homo[i].w,
        };
    }

    vec4_t obb_center_homo;
    PFM_Mat4x4_Mult4x1(&model, &identity_center_homo, &obb_center_homo);
    out->center = (vec3_t){
        obb_center_homo.x / obb_center_homo.w,
        obb_center_homo.y / obb_center_homo.w,
        obb_center_homo.z / obb_center_homo.w,
    };

    out->half_lengths[0] = (aabb->x_max - aabb->x_min) / 2.0f * scale.x;
    out->half_lengths[1] = (aabb->y_max - aabb->y_min) / 2.0f * scale.y;
    out->half_lengths[2] = (aabb->z_max - aabb->z_min) / 2.0f * scale.z;

    vec3_t axis0, axis1, axis2;   
    PFM_Vec3_Sub(&out->corners[4], &out->corners[0], &axis0);
    PFM_Vec3_Sub(&out->corners[2], &out->corners[0], &axis1);
    PFM_Vec3_Sub(&out->corners[1], &out->corners[0], &axis2);

    PFM_Vec3_Normal(&axis0, &out->axes[0]);
    PFM_Vec3_Normal(&axis1, &out->axes[1]);
    PFM_Vec3_Normal(&axis2, &out->axes[2]);
}

