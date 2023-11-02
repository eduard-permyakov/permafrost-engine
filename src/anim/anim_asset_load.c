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

#include "../asset_load.h"
#include "../lib/public/pf_string.h"

#include <string.h>


/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool al_read_joint(SDL_RWops *stream, struct joint *out, struct SQT *out_bind)
{
    char line[MAX_LINE_LEN];
    int unfixed_idx;
    
    READ_LINE(stream, line, fail);
    if(!sscanf(line, "j %d %s", &unfixed_idx, out->name))
        goto fail;
    /* Convert to a 0 base index system; the root's parent_idx will be -1 */
    out->parent_idx = unfixed_idx - 1;

    char *string = line;
    char *saveptr;

    /* Consume the first 3 tokens, 'j', '<parent idx>', '<name>' */
    pf_strtok_r(line, " \t", &saveptr);
    pf_strtok_r(NULL, " \t", &saveptr);
    pf_strtok_r(NULL, " \t", &saveptr);

    string = pf_strtok_r(NULL, " \t", &saveptr);  
    if(!sscanf(string, "%f/%f/%f", 
        &out_bind->scale.x, &out_bind->scale.y, &out_bind->scale.z))
        goto fail;

    string = pf_strtok_r(NULL, " \t", &saveptr);  
    if(!sscanf(string, "%f/%f/%f/%f", &out_bind->quat_rotation.x, &out_bind->quat_rotation.y, 
        &out_bind->quat_rotation.z, &out_bind->quat_rotation.w))
        goto fail;

    string = pf_strtok_r(NULL, " \t", &saveptr);  
    if(!sscanf(string, "%f/%f/%f", 
        &out_bind->trans.x, &out_bind->trans.y, &out_bind->trans.z))
        goto fail;

    string = pf_strtok_r(NULL, " \t", &saveptr);  
    if(!sscanf(string, "%f/%f/%f", &out->tip.x, &out->tip.y, &out->tip.z))
        goto fail;

    return true;

fail:
    return false;
}

static bool al_read_anim_clip(SDL_RWops *stream, struct anim_clip *out, 
                              const struct pfobj_hdr *header)
{
    char line[MAX_LINE_LEN];

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "as %s %u", out->name, &out->num_frames))
        goto fail;

    for(int f = 0; f < out->num_frames; f++) {
        for(int j = 0; j < header->num_joints; j++) {

            int joint_idx;  /* unused */
            struct SQT *curr_joint_trans = &out->samples[f].local_joint_poses[j];
        
            READ_LINE(stream, line, fail);
            if(!sscanf(line, "%d %f/%f/%f %f/%f/%f/%f %f/%f/%f",
                &joint_idx, 
                &curr_joint_trans->scale.x,
                &curr_joint_trans->scale.y,
                &curr_joint_trans->scale.z,
                &curr_joint_trans->quat_rotation.x,
                &curr_joint_trans->quat_rotation.y,
                &curr_joint_trans->quat_rotation.z,
                &curr_joint_trans->quat_rotation.w,
                &curr_joint_trans->trans.x,
                &curr_joint_trans->trans.y,
                &curr_joint_trans->trans.z)) {
                goto fail;
            }
        
        }

        if(!header->has_collision)
            continue;

        if(!AL_ParseAABB(stream, &out->samples[f].sample_aabb))
            goto fail;
    }

    return true;

fail:
    return false;
}

size_t al_data_buffsize_from_header(const struct pfobj_hdr *header)
{
    size_t ret = 0;

    ret += sizeof(struct anim_data);

    ret += header->num_joints * sizeof(struct SQT);
    ret += header->num_joints * sizeof(mat4x4_t);
    ret += header->num_joints * sizeof(struct joint);
    ret += header->num_as     * sizeof(struct anim_clip);

    /*
     * For each frame of each animation clip, we also require:
     *
     *    1. a 'struct anim_sample' (for referencing this frame's SQT array)
     *    2. num_joint number of 'struct SQT's (each joint's transform
     *       for the current frame)
     */
    for(unsigned as_idx  = 0; as_idx < header->num_as; as_idx++) {

        ret += header->frame_counts[as_idx] * 
               (sizeof(struct anim_sample) + header->num_joints * sizeof(struct SQT));
    }

    return ret;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

size_t A_AL_CtxBuffSize(void)
{
    return sizeof(struct anim_ctx);
}

/*
 * Animation data buff layout:
 *
 *  +---------------------------------+ <-- base
 *  | struct anim_data[1]             |
 *  +---------------------------------+
 *  | struct SQT[num_joints] (bind)   |
 *  +---------------------------------+
 *  | mat4x4_t[num_joints] (inv. bind)|
 *  +---------------------------------+
 *  | struct joint[num_joints]        |
 *  +---------------------------------+
 *  | struct anim_clip[num_as]        |
 *  +---------------------------------+
 *  | struct anim_samples[num_as      |
 *  |    * num_frames]                |
 *  +---------------------------------+
 *  | struct SQT[num_as * num_joints] |
 *  |    (stored in clip-major order) |
 *  +---------------------------------+
 *
 */

void *A_AL_PrivFromStream(const struct pfobj_hdr *header, SDL_RWops *stream)
{
    struct anim_data *ret = malloc(al_data_buffsize_from_header(header));
    if(!ret)
        goto fail_alloc;

    /*-----------------------------------------------------------
     * First divide up the buffer betwen data members,
     * set counts and pointers 
     *-----------------------------------------------------------
     */

    char *unused_base = (char*)(ret + 1);

    ret->num_anims = header->num_as; 
    ret->skel.num_joints = header->num_joints;

    ret->skel.bind_sqts = (void*)unused_base;
    unused_base += sizeof(struct SQT) * header->num_joints;

    ret->skel.inv_bind_poses = (void*)unused_base;
    unused_base += sizeof(mat4x4_t) * header->num_joints;

    ret->skel.joints = (void*)unused_base;
    unused_base += sizeof(struct joint) * header->num_joints;

    ret->anims = (void*)unused_base;
    unused_base += sizeof(struct anim_clip) * header->num_as;

    for(int i = 0; i < header->num_as; i++) {

        ret->anims[i].samples = (void*)unused_base;
        unused_base += sizeof(struct anim_sample) * header->frame_counts[i];
    }

    for(int i = 0; i < header->num_as; i++) {

        ret->anims[i].skel = &ret->skel;
        ret->anims[i].num_frames = header->frame_counts[i];

        for(int f = 0; f < header->frame_counts[i]; f++) {

            ret->anims[i].samples[f].local_joint_poses = (void*)unused_base;
            unused_base += sizeof(struct SQT) * header->num_joints;
        }
    }

    /*---------------------------------------------------------------
     * Then we populate priv members with the file data 
     *---------------------------------------------------------------
     */
    for(int i = 0; i < header->num_joints; i++) {

        if(!al_read_joint(stream, &ret->skel.joints[i], &ret->skel.bind_sqts[i]))
            goto fail_parse;
    }

    for(int i = 0; i < header->num_as; i++) {
        
        if(!al_read_anim_clip(stream, &ret->anims[i], header))
            goto fail_parse;
    }

    A_PrepareInvBindMatrices(&ret->skel);
    return ret;

fail_parse:
    free(ret);
fail_alloc:
    return NULL;
}

void A_AL_DumpPrivate(FILE *stream, void *priv_data)
{
    struct anim_data *priv = priv_data;

    /* Write joints */
    for(int i = 0; i < priv->skel.num_joints; i++) {

        struct joint *j = &priv->skel.joints[i];
        struct SQT *bind = &priv->skel.bind_sqts[i];

        float roll, pitch, yaw;
        PFM_Quat_ToEuler(&bind->quat_rotation, &roll, &pitch, &yaw);

        fprintf(stream, "j %d %s ", j->parent_idx + 1, j->name); 
        fprintf(stream, "%.6f/%.6f/%.6f %.6f/%.6f/%.6f %.6f/%.6f/%.6f %.6f/%.6f/%.6f\n",
            bind->scale.x, bind->scale.y, bind->scale.z, 
            roll,          pitch,         yaw,
            bind->trans.x, bind->trans.y, bind->trans.z,
            j->tip.x,      j->tip.y,      j->tip.z);
    }

    /* Write animation sets */
    for(int i = 0; i < priv->num_anims; i++) {

        struct anim_clip *ac = &priv->anims[i];
        fprintf(stream, "as %s %d\n", ac->name, ac->num_frames); 

        for(int f = 0; f < ac->num_frames; f++) {
            for(int j = 0; j < ac->skel->num_joints; j++) {

                struct SQT *sqt = &ac->samples[f].local_joint_poses[j]; 

                float roll, pitch, yaw;
                PFM_Quat_ToEuler(&sqt->quat_rotation, &roll, &pitch, &yaw);

                fprintf(stream, "\t%d %f/%f/%f %f/%f/%f %f/%f/%f\n",
                    j + 1,
                    sqt->scale.x, sqt->scale.y, sqt->scale.z,
                    roll,         pitch,        yaw,
                    sqt->trans.x, sqt->trans.y, sqt->trans.z);
            }
        }
    } 
}

