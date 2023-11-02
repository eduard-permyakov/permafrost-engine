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

#include "public/anim.h"
#include "anim_private.h"
#include "anim_data.h"
#include "anim_ctx.h"
#include "../entity.h"
#include "../event.h"
#include "../perf.h"
#include "../asset_load.h"
#include "../lib/public/attr.h"
#include "../lib/public/pf_string.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"

#include <SDL.h>

#include <string.h>
#include <assert.h>

#define CHK_TRUE_RET(_pred)   \
    do{                       \
        if(!(_pred))          \
            return false;     \
    }while(0)

KHASH_MAP_INIT_INT(ctx, struct anim_ctx)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(ctx) *s_anim_ctx;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static struct anim_ctx *a_ctx_for_uid(uint32_t uid)
{
    khiter_t k = kh_get(ctx, s_anim_ctx, uid);
    if(k == kh_end(s_anim_ctx))
        return NULL;
    return &kh_value(s_anim_ctx, k);
}

static const struct anim_clip *a_clip_for_name(const struct anim_data *data, const char *name)
{
    for(int i = 0; i < data->num_anims; i++) {

        const struct anim_clip *curr = &data->anims[i];
        if(!strcmp(curr->name, name)) 
            return curr;
    }

    return NULL;
}

static void a_mat_from_sqt(const struct SQT *sqt, mat4x4_t *out)
{
    mat4x4_t rot, trans, scale;
    mat4x4_t tmp;

    PFM_Mat4x4_MakeScale(sqt->scale.x, sqt->scale.y, sqt->scale.z, &scale);
    PFM_Mat4x4_MakeTrans(sqt->trans.x, sqt->trans.y, sqt->trans.z, &trans);
    PFM_Mat4x4_RotFromQuat(&sqt->quat_rotation, &rot);

    /*  (T * R * S) 
     */
    PFM_Mat4x4_Mult4x4(&rot, &scale, &tmp);
    PFM_Mat4x4_Mult4x4(&trans, &tmp, out);
}

static void a_make_bind_mat(int joint_idx, const struct skeleton *skel, mat4x4_t *out)
{
    mat4x4_t bind_trans;
    PFM_Mat4x4_Identity(&bind_trans);

    /* Walk up the bone heirarchy, multiplying our bind transform matrix by the parent-relative
     * transform of each bone we visit. In the end, this the bind matrix will hold a transformation
     * from the object's space to the current joint's space. Since each joint is positioned at the
     * origin of its' local space, this gives us the object-space position of this joint in the bind
     * pose.
     */
    while(joint_idx >= 0) {

        struct joint *joint = &skel->joints[joint_idx];
        struct SQT   *bind_sqt = &skel->bind_sqts[joint_idx];
        mat4x4_t to_parent, to_curr = bind_trans;

        a_mat_from_sqt(bind_sqt, &to_parent);
        PFM_Mat4x4_Mult4x4(&to_parent, &to_curr, &bind_trans);

        joint_idx = joint->parent_idx;
    }

    *out = bind_trans;
}

static void a_make_pose_mat(uint32_t uid, int joint_idx, const struct skeleton *skel, mat4x4_t *out)
{
    struct anim_ctx *ctx = a_ctx_for_uid(uid);
    struct anim_sample *sample = &ctx->active->samples[ctx->curr_frame];

    mat4x4_t pose_trans;
    PFM_Mat4x4_Identity(&pose_trans);

    /* Same as a_make_bind_mat, except for the current pose. */
    while(joint_idx >= 0) {

        struct joint *joint = &skel->joints[joint_idx];
        struct SQT   *pose_sqt = &sample->local_joint_poses[joint_idx];
        mat4x4_t to_parent, to_curr = pose_trans;

        a_mat_from_sqt(pose_sqt, &to_parent);
        PFM_Mat4x4_Mult4x4(&to_parent, &to_curr, &pose_trans);

        joint_idx = joint->parent_idx;
    }

    *out = pose_trans;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void A_SetIdleClip(uint32_t uid, const char *name, unsigned key_fps)
{
    struct anim_ctx *ctx = a_ctx_for_uid(uid);
    const struct anim_clip *clip = a_clip_for_name(ctx->data, name);
    assert(clip);

    ctx->idle = clip;
    A_SetActiveClip(uid, name, ANIM_MODE_LOOP, key_fps);
}

void A_SetActiveClip(uint32_t uid, const char *name, 
                     enum anim_mode mode, unsigned key_fps)
{
    struct anim_ctx *ctx = a_ctx_for_uid(uid);
    const struct anim_clip *clip = a_clip_for_name(ctx->data, name);
    assert(clip);

    ctx->active = clip;
    ctx->mode = mode;
    ctx->key_fps = key_fps;
    ctx->curr_frame = 0;
    ctx->curr_frame_start_ticks = SDL_GetTicks();
}

void A_Update(void)
{
    uint32_t curr_ticks = SDL_GetTicks();
    uint32_t uid;

    kh_foreach(s_anim_ctx, uid, (struct anim_ctx){0}, {

        struct anim_ctx *ctx = a_ctx_for_uid(uid);
        float frame_period_secs = 1.0f/ctx->key_fps;
        float elapsed_secs = (curr_ticks - ctx->curr_frame_start_ticks)/1000.0f;

        if(elapsed_secs > frame_period_secs) {

            ctx->curr_frame = (ctx->curr_frame + 1) % ctx->active->num_frames;
            ctx->curr_frame_start_ticks = curr_ticks;

            if(ctx->curr_frame == ctx->active->num_frames - 1) {

                E_Entity_Notify(EVENT_ANIM_CYCLE_FINISHED, uid, NULL, ES_ENGINE);
                if(ctx->mode == ANIM_MODE_ONCE) {
                    E_Entity_Notify(EVENT_ANIM_FINISHED, uid, NULL, ES_ENGINE);
                }
            }

            if(ctx->curr_frame == 0 && ctx->mode == ANIM_MODE_ONCE) {
                A_SetActiveClip(uid, ctx->idle->name, ANIM_MODE_LOOP, ctx->key_fps);
            }
        }
    });
}

void A_GetRenderState(uint32_t uid, size_t *out_njoints, 
                      mat4x4_t *out_curr_pose, const mat4x4_t **out_inv_bind_pose)
{
    PERF_ENTER();

    struct anim_ctx *ctx = a_ctx_for_uid(uid);
    const struct anim_data *data = ctx->data;

    for(int j = 0; j < data->skel.num_joints; j++) {
        a_make_pose_mat(uid, j, &data->skel, out_curr_pose + j);
    }

    *out_njoints = data->skel.num_joints;
    *out_inv_bind_pose = data->skel.inv_bind_poses;

    PERF_RETURN_VOID();
}

const struct skeleton *A_GetBindSkeleton(uint32_t uid)
{
    struct anim_ctx *ctx = a_ctx_for_uid(uid);
    return &ctx->data->skel;
}

const struct skeleton *A_GetCurrPoseSkeleton(uint32_t uid)
{
    struct anim_ctx *ctx = a_ctx_for_uid(uid);
    const struct anim_data *data = ctx->data;
    size_t num_joints = data->skel.num_joints;

    /* We make a copy of the skeleton structure, the joints, and the inverse bind poses.
     * Returned buffer layout:
     *  +---------------------------------+ <-- base
     *  | struct skeleton[1]              |
     *  +---------------------------------+
     *  | struct joint[num_joints]        |
     *  +---------------------------------+
     *  | struct SQT[num_joints]          |
     *  +---------------------------------+
     *  | mat4x4_t[num_joints]            |
     *  +---------------------------------+
     */
    size_t alloc_sz = sizeof(struct skeleton) + 
                      num_joints * (sizeof(struct joint) + sizeof(struct SQT) + sizeof(mat4x4_t));

    struct skeleton *ret = malloc(alloc_sz);
    if(!ret)
        return NULL;

    ret->num_joints = data->skel.num_joints;
    ret->joints = (void*)(ret + 1);
    memcpy(ret->joints, data->skel.joints, num_joints * sizeof(struct joint));

    ret->bind_sqts = (void*)((char*)ret->joints + num_joints * sizeof(struct joint));
    memcpy(ret->bind_sqts, data->skel.bind_sqts, num_joints * sizeof(struct SQT));

    ret->inv_bind_poses = (void*)((char*)ret->bind_sqts + num_joints * sizeof(struct SQT));

    for(int i = 0; i < ret->num_joints; i++) {
    
        /* Update the inverse bind matrices for the current frame */
        mat4x4_t pose_mat;
        a_make_pose_mat(uid, i, ret, &pose_mat);
        PFM_Mat4x4_Inverse(&pose_mat, &ret->inv_bind_poses[i]);
    }

    return ret;
}

void A_PrepareInvBindMatrices(const struct skeleton *skel)
{
    assert(skel->inv_bind_poses);

    for(int i = 0; i < skel->num_joints; i++) {

        mat4x4_t bind_mat;
        a_make_bind_mat(i, skel, &bind_mat);
        PFM_Mat4x4_Inverse(&bind_mat, &skel->inv_bind_poses[i]);
    }
}

const struct aabb *A_GetCurrPoseAABB(uint32_t uid)
{
    struct anim_ctx *ctx = a_ctx_for_uid(uid);
    return &ctx->active->samples[ctx->curr_frame].sample_aabb;
}

void A_AddTimeDelta(uint32_t uid, uint32_t dt)
{
    struct anim_ctx *ctx = a_ctx_for_uid(uid);
    ctx->curr_frame_start_ticks += dt;
}

const char *A_GetIdleClip(uint32_t uid)
{
    struct anim_ctx *ctx = a_ctx_for_uid(uid);
    return ctx->idle->name;
}

const char *A_GetCurrClip(uint32_t uid)
{
    struct anim_ctx *ctx = a_ctx_for_uid(uid);
    return ctx->active->name;
}

const char *A_GetClip(uint32_t uid, int idx)
{
    struct anim_ctx *ctx = a_ctx_for_uid(uid);
    if(idx >= ctx->data->num_anims)
        return NULL;
    return ctx->data->anims[0].name;
}

bool A_HasClip(uint32_t uid, const char *name)
{
    struct anim_ctx *ctx = a_ctx_for_uid(uid);
    const struct anim_clip *clip = a_clip_for_name(ctx->data, name);
    return (clip != NULL);
}

bool A_SaveState(struct SDL_RWops *stream, uint32_t uid)
{
    struct anim_ctx *ctx = a_ctx_for_uid(uid);

    struct attr active = (struct attr){ .type = TYPE_STRING };
    pf_snprintf(active.val.as_string, sizeof(active.val.as_string), "%s", ctx->active->name);
    CHK_TRUE_RET(Attr_Write(stream, &active, "active_clip"));

    struct attr idle = (struct attr){ .type = TYPE_STRING };
    pf_snprintf(idle.val.as_string, sizeof(idle.val.as_string), "%s", ctx->idle->name);
    CHK_TRUE_RET(Attr_Write(stream, &idle, "idle_clip"));

    struct attr mode = (struct attr){
        .type = TYPE_INT,
        .val.as_int = ctx->mode
    };
    CHK_TRUE_RET(Attr_Write(stream, &mode, "mode"));

    struct attr key_fps = (struct attr){
        .type = TYPE_INT,
        .val.as_int = ctx->key_fps
    };
    CHK_TRUE_RET(Attr_Write(stream, &key_fps, "key_fps"));

    struct attr curr_frame = (struct attr){
        .type = TYPE_INT,
        .val.as_int = ctx->curr_frame
    };
    CHK_TRUE_RET(Attr_Write(stream, &curr_frame, "curr_frame"));

    struct attr curr_frame_ticks_elapsed = (struct attr){
        .type = TYPE_INT,
        .val.as_int = SDL_GetTicks() - ctx->curr_frame_start_ticks
    };
    CHK_TRUE_RET(Attr_Write(stream, &curr_frame_ticks_elapsed, "curr_frame_ticks_elapsed"));

    return true;
}

bool A_LoadState(struct SDL_RWops *stream, uint32_t uid)
{
    struct anim_ctx *ctx = a_ctx_for_uid(uid);
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_STRING);
    const struct anim_clip *active_clip = a_clip_for_name(ctx->data, attr.val.as_string);
    CHK_TRUE_RET(active_clip);
    ctx->active = active_clip;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_STRING);
    const struct anim_clip *idle_clip = a_clip_for_name(ctx->data, attr.val.as_string);
    CHK_TRUE_RET(idle_clip);
    ctx->idle = idle_clip;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    ctx->mode = attr.val.as_int;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    ctx->key_fps = attr.val.as_int;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    ctx->curr_frame = attr.val.as_int;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    ctx->curr_frame_start_ticks = SDL_GetTicks() - attr.val.as_int;

    return true;
}

void A_ClearState(void)
{
    kh_clear(ctx, s_anim_ctx);
}

bool A_Init(void)
{
    s_anim_ctx = kh_init(ctx);
    return (s_anim_ctx != NULL);
}

void A_Shutdown(void)
{
    kh_destroy(ctx, s_anim_ctx);
}

bool A_AddEntity(uint32_t uid)
{
    int status;
    khiter_t k = kh_put(ctx, s_anim_ctx, uid, &status);
    if(status == -1 || status == 0)
        return false;

    struct anim_ctx *ctx = &kh_value(s_anim_ctx, k);
    const struct entity *ent = AL_EntityGet(uid);
    ctx->data = ent->anim_private;

    A_SetIdleClip(uid, A_GetClip(uid, 0), 24);
    return true;
}

void A_RemoveEntity(uint32_t uid)
{
    khiter_t k = kh_get(ctx, s_anim_ctx, uid);
    if(k == kh_end(s_anim_ctx))
        return;
    kh_del(ctx, s_anim_ctx, k);
}

