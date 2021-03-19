/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2020 Eduard Permyakov 
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
#include "render/public/render.h"
#include "render/public/render_ctrl.h"
#include "navigation/public/nav.h"
#include "game/public/game.h"
#include "anim/public/anim.h"
#include "lib/public/mpool.h"
#include "lib/public/khash.h"
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
    struct entity *ent;
    const struct map *map;
    void (*on_finish)(void*);
    void *arg;
};

MPOOL_TYPE(taglist, struct taglist)
MPOOL_PROTOTYPES(static, taglist, struct taglist)
MPOOL_IMPL(static, taglist, struct taglist)
KHASH_MAP_INIT_INT(tags, struct taglist)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static uint32_t          s_next_uid = 0;
static khash_t(stridx)  *s_stridx;
static mp_strbuff_t      s_stringpool;

static kh_ents_t        *s_tag_ent_map;
static kh_tags_t        *s_ent_tag_map;

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
    kh_del(uid, set, uid);
}

static struct result ping_task(void *arg)
{
    struct entity *ent = arg;

    /* Cache all the params */
    vec2_t pos = G_Pos_GetXZ(ent->uid);
    float radius = G_GetSelectionRadius(ent->uid);
    const float width = 0.4f;
    vec3_t color = (vec3_t){1.0f, 1.0f, 0.0f};
    uint32_t uid = ent->uid;

    struct obb obb;
    Entity_CurrentOBB(ent, &obb, false);

    uint32_t elapsed = 0;
    uint32_t start = SDL_GetTicks();
    int source;

    while(elapsed < 1200) {
        Task_AwaitEvent(EVENT_RENDER_3D_POST, &source);

        uint32_t curr = SDL_GetTicks();
        elapsed = curr - start;

        if((elapsed / 400) == 1)
            continue;

        if(G_EntityExists(uid)) {
            pos = G_Pos_GetXZ(ent->uid);
        }

        if(ent->flags & ENTITY_FLAG_BUILDING) {

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
    vec3_t start_pos = G_Pos_Get(darg.ent->uid);
    uint32_t uid = darg.ent->uid;
    int faction_id = G_GetFactionID(uid);

    struct obb obb;
    Entity_CurrentOBB(darg.ent, &obb, false);

    if(darg.map) {
        M_NavBlockersIncrefOBB(darg.map, faction_id, &obb);
    }

    int height = obb.half_lengths[1] * 2.0f;

    vec2_t curr_shift = (vec2_t){0, 0};
    vec2_t prev_shift = (vec2_t){0, 0};

    darg.ent->flags |= ENTITY_FLAG_TRANSLUCENT;

    const float duration = 2500.0f;
    uint32_t elapsed = 0;
    uint32_t start = SDL_GetTicks();

    while(elapsed < duration) {
    
        Task_AwaitEvent(EVENT_UPDATE_START, &(int){0});
        uint32_t curr = SDL_GetTicks();

        /* The entity can theoretically be forecefully removed during the 
         * disappearing animation. Make sure we don't crap out if this happens
         */
        if(!G_EntityExists(uid))
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
        G_Pos_Set(darg.ent, curr_pos);
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

void Entity_ModelMatrix(const struct entity *ent, mat4x4_t *out)
{
    mat4x4_t trans, scale, rot, tmp;
    vec3_t pos = G_Pos_Get(ent->uid);

    PFM_Mat4x4_MakeTrans(pos.x, pos.y, pos.z, &trans);
    PFM_Mat4x4_MakeScale(ent->scale.x, ent->scale.y, ent->scale.z, &scale);
    PFM_Mat4x4_RotFromQuat(&ent->rotation, &rot);

    PFM_Mat4x4_Mult4x4(&scale, &rot, &tmp);
    PFM_Mat4x4_Mult4x4(&trans, &tmp, out);
}

uint32_t Entity_NewUID(void)
{
    return s_next_uid++;
}

void Entity_SetNextUID(uint32_t uid)
{
    s_next_uid = uid;
}

void Entity_CurrentOBB(const struct entity *ent, struct obb *out, bool identity)
{
    const struct aabb *aabb;
    if((ent->flags & ENTITY_FLAG_ANIMATED) && !identity) {
        aabb = A_GetCurrPoseAABB(ent->uid);
    }else {
        aabb = &ent->identity_aabb;
    }

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

    mat4x4_t model;
    Entity_ModelMatrix(ent, &model);

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
    out->half_lengths[0] = (aabb->x_max - aabb->x_min) / 2.0f * ent->scale.x;
    out->half_lengths[1] = (aabb->y_max - aabb->y_min) / 2.0f * ent->scale.y;
    out->half_lengths[2] = (aabb->z_max - aabb->z_min) / 2.0f * ent->scale.z;

    vec3_t axis0, axis1, axis2;   
    PFM_Vec3_Sub(&out->corners[4], &out->corners[0], &axis0);
    PFM_Vec3_Sub(&out->corners[2], &out->corners[0], &axis1);
    PFM_Vec3_Sub(&out->corners[1], &out->corners[0], &axis2);

    PFM_Vec3_Normal(&axis0, &out->axes[0]);
    PFM_Vec3_Normal(&axis1, &out->axes[1]);
    PFM_Vec3_Normal(&axis2, &out->axes[2]);
}

vec3_t Entity_CenterPos(const struct entity *ent)
{
    struct obb obb;
    Entity_CurrentOBB(ent, &obb, false);
    return obb.center;
}

vec3_t Entity_TopCenterPointWS(const struct entity *ent)
{
    const struct aabb *aabb = &ent->identity_aabb;
    vec4_t top_center_homo = (vec4_t) {
        (aabb->x_min + aabb->x_max) / 2.0f,
        aabb->y_max,
        (aabb->z_min + aabb->z_max) / 2.0f,
        1.0f
    };

    mat4x4_t model; 
    vec4_t out_ws_homo;

    Entity_ModelMatrix(ent, &model);
    PFM_Mat4x4_Mult4x1(&model, &top_center_homo, &out_ws_homo);

    return (vec3_t) {
        out_ws_homo.x / out_ws_homo.w,
        out_ws_homo.y / out_ws_homo.w,
        out_ws_homo.z / out_ws_homo.w,
    };
}

void Entity_FaceTowards(struct entity *ent, vec2_t point)
{
    vec2_t delta;
    vec2_t pos = G_Pos_GetXZ(ent->uid);
    PFM_Vec2_Sub(&point, &pos, &delta);

    float radians = atan2(delta.x, delta.z);

    mat4x4_t rotmat;
    PFM_Mat4x4_MakeRotY(radians, &rotmat);

    quat_t rot;
    PFM_Quat_FromRotMat(&rotmat, &rot);
    ent->rotation = rot;
}

void Entity_Ping(const struct entity *ent)
{
    uint32_t tid = Sched_Create(1, ping_task, (void*)ent, NULL, TASK_MAIN_THREAD_PINNED);
    Sched_RunSync(tid);
}

vec2_t Entity_TopScreenPos(const struct entity *ent, int screenw, int screenh)
{
    vec3_t pos = Entity_TopCenterPointWS(ent);
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

bool Entity_MaybeAdjacentFast(const struct entity *a, const struct entity *b, float buffer)
{
    struct obb obb_a, obb_b;
    Entity_CurrentOBB(a, &obb_a, false);
    Entity_CurrentOBB(b, &obb_b, false);

    vec2_t apos = G_Pos_GetXZ(a->uid);
    vec2_t bpos = G_Pos_GetXZ(b->uid);

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

size_t Entity_EntsForTag(const char *tag, size_t maxout, uint32_t out[static maxout])
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

size_t Entity_TagsForEnt(uint32_t uid, size_t maxout, const char *out[static maxout])
{
    struct taglist *tl = entity_taglist(uid);
    if(!tl)
        return 0;
    size_t ret = MIN(tl->ntags, maxout);
    memcpy(out, tl->tags, ret * sizeof(char*));
    return ret;
}

void Entity_DisappearAnimated(struct entity *ent, const struct map *map, void (*on_finish)(void*), void *arg)
{
    /* It's safe to pass a pointer to stack data here due to the 'Shced_RunSync' 
     * call later - by the time we return from it, we will have copied the data.
     */
    struct dis_arg darg = (struct dis_arg){
        .ent = ent,
        .map = map,
        .on_finish = on_finish,
        .arg = arg,
    };
    uint32_t tid = Sched_Create(1, disappear_task, &darg, NULL, TASK_MAIN_THREAD_PINNED | TASK_BIG_STACK);
    Sched_RunSync(tid);
}

int Entity_NavLayer(const struct entity *ent)
{
    if(G_GetSelectionRadius(ent->uid) >= 5.0f)
        return NAV_LAYER_GROUND_3X3;
    return NAV_LAYER_GROUND_1X1;
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

    return true;

fail_ent_tag_map:
    kh_destroy(ents, s_tag_ent_map);
fail_tag_ent_map:
    si_shutdown(&s_stringpool, s_stridx);
fail_strintern:
    return false;
}

void Entity_Shutdown(void)
{
    kh_destroy(tags, s_ent_tag_map);
    kh_destroy(ents, s_tag_ent_map);
    si_shutdown(&s_stringpool, s_stridx);
}

void Entity_ClearState(void)
{
    kh_clear(tags, s_ent_tag_map);
    kh_clear(ents, s_tag_ent_map);
    si_clear(&s_stringpool, s_stridx);
}

