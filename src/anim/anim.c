/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017 Eduard Permyakov 
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
 */

#include "public/anim.h"
#include "anim_private.h"
#include "anim_data.h"
#include "anim_ctx.h"
#include "../entity.h"
#include "../gl_uniforms.h"
#include "../render/public/render.h"

#include <SDL.h>

#include <string.h>
#include <assert.h>


/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static const struct anim_clip *a_clip_for_name(const struct entity *ent, const char *name)
{
    struct anim_private *priv = ent->anim_private;
    for(int i = 0; i < priv->data->num_anims; i++) {

        const struct anim_clip *curr = &priv->data->anims[i];
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

static void a_make_pose_mat(const struct entity *ent, int joint_idx, const struct skeleton *skel, mat4x4_t *out)
{
    struct anim_private *priv = ent->anim_private;
    struct anim_sample *sample =  &priv->ctx->active->samples[priv->ctx->curr_frame];

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

void a_set_uniforms_curr_frame(const struct entity *ent)
{
    struct anim_private *priv = (struct anim_private*)ent->anim_private;

    size_t float_per_mat = sizeof(mat4x4_t) / sizeof(float);
    size_t num_joints = priv->data->skel.num_joints;

    mat4x4_t curr_pose_mats[num_joints];
    for(int j = 0; j < num_joints; j++) {
        a_make_pose_mat(ent, j, &priv->data->skel, &curr_pose_mats[j]);
    }

    R_GL_SetAnimUniformMat4x4Array(priv->data->skel.inv_bind_poses, num_joints, GL_U_INV_BIND_MATS);
    R_GL_SetAnimUniformMat4x4Array(curr_pose_mats, num_joints, GL_U_CURR_POSE_MATS);
}


/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void A_InitCtx(const struct entity *ent, const char *idle_clip, unsigned key_fps)
{
    struct anim_private *priv = ent->anim_private;
    struct anim_ctx *ctx = priv->ctx;

    const struct anim_clip *idle = a_clip_for_name(ent, idle_clip);
    assert(idle);

    ctx->idle = idle;

    A_SetActiveClip(ent, idle_clip, ANIM_MODE_LOOP, key_fps);
}

void A_SetActiveClip(const struct entity *ent, const char *name, 
                     enum anim_mode mode, unsigned key_fps)
{
    struct anim_private *priv = ent->anim_private;
    struct anim_ctx *ctx = priv->ctx;

    const struct anim_clip *clip = a_clip_for_name(ent, name);
    assert(clip);

    ctx->active = clip;
    ctx->mode = mode;
    ctx->key_fps = key_fps;
    ctx->curr_frame = 0;
    ctx->curr_frame_start_ticks = SDL_GetTicks();
}

void A_Update(const struct entity *ent)
{
    struct anim_private *priv = ent->anim_private;
    struct anim_ctx *ctx = priv->ctx;

    a_set_uniforms_curr_frame(ent);

    float frame_period_secs = 1.0f/ctx->key_fps;
    uint32_t curr_ticks = SDL_GetTicks();
    float elapsed_secs = (curr_ticks - ctx->curr_frame_start_ticks)/1000.0f;

    if(elapsed_secs > frame_period_secs) {

        ctx->curr_frame = (ctx->curr_frame + 1) % ctx->active->num_frames;
        ctx->curr_frame_start_ticks = curr_ticks;
    }
}

const struct skeleton *A_GetBindSkeleton(const struct entity *ent)
{
    struct anim_private *priv = ent->anim_private;
    return &priv->data->skel;
}

const struct skeleton *A_GetCurrPoseSkeleton(const struct entity *ent)
{
    struct anim_private *priv = ent->anim_private;
    size_t num_joints = priv->data->skel.num_joints;

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

    ret->num_joints = priv->data->skel.num_joints;
    ret->joints = (void*)(ret + 1);
    memcpy(ret->joints, priv->data->skel.joints, num_joints * sizeof(struct joint));

    ret->bind_sqts = (void*)((char*)ret->joints + num_joints * sizeof(struct joint));
    memcpy(ret->bind_sqts, priv->data->skel.bind_sqts, num_joints * sizeof(struct SQT));

    ret->inv_bind_poses = (void*)((char*)ret->bind_sqts + num_joints * sizeof(struct SQT));

    struct anim_sample *sample =  &priv->ctx->active->samples[priv->ctx->curr_frame];

    for(int i = 0; i < ret->num_joints; i++) {
    
        /* Update the inverse bind matrices for the current frame */
        mat4x4_t pose_mat;
        a_make_pose_mat(ent, i, ret, &pose_mat);
        PFM_Mat4x4_Inverse(&pose_mat, &ret->inv_bind_poses[i]);
    }

    return ret;
}

void A_PrepareInvBindMatrices(const struct skeleton *skel)
{
    assert(skel->inv_bind_poses);

    for(int i = 0; i < skel->num_joints; i++) {

        const struct joint *joint = &skel->joints[i];

        mat4x4_t bind_mat;
        a_make_bind_mat(i, skel, &bind_mat);
        PFM_Mat4x4_Inverse(&bind_mat, &skel->inv_bind_poses[i]);
    }
}

const struct aabb *A_GetCurrPoseAABB(const struct entity *ent)
{
    assert(ent->flags & ENTITY_FLAG_COLLISION);
    struct anim_private *priv = ent->anim_private;
    struct anim_ctx *ctx = priv->ctx;

    return &ctx->active->samples[ctx->curr_frame].sample_aabb;
}

