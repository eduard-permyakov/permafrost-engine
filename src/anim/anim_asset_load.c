#include "public/anim.h"
#include "anim_private.h"
#include "anim_data.h"
#include "anim_ctx.h"

#include "../asset_load.h"

#define __USE_POSIX
#include <string.h>

#define READ_LINE(file, buff, fail_label)       \
    do{                                         \
        if(!fgets(buff, MAX_LINE_LEN, file))    \
            goto fail_label;                    \
        buff[MAX_LINE_LEN - 1] = '\0';          \
    }while(0)

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void euler_to_quat(float euler[3], quat_t *out)
{
    mat4x4_t tmp;
    PFM_Mat4x4_RotFromEuler(euler[0], euler[1], euler[2], &tmp);
    PFM_Quat_FromRotMat(&tmp, out);
}

static bool al_read_joint(FILE *stream, struct joint *out, struct SQT *out_bind)
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
    strtok_r(line, " \t", &saveptr);
    strtok_r(NULL, " \t", &saveptr);
    strtok_r(NULL, " \t", &saveptr);

    string = strtok_r(NULL, " \t", &saveptr);  
    if(!sscanf(string, "%f/%f/%f", 
        &out_bind->scale.x, &out_bind->scale.y, &out_bind->scale.z))
        goto fail;

    float euler[3]; /* XYZ order - in degrees */
    string = strtok_r(NULL, " \t", &saveptr);  
    if(!sscanf(string, "%f/%f/%f", &euler[0], &euler[1], &euler[2]))
        goto fail;
    euler_to_quat(euler, &out_bind->quat_rotation);

    string = strtok_r(NULL, " \t", &saveptr);  
    if(!sscanf(string, "%f/%f/%f", 
        &out_bind->trans.x, &out_bind->trans.y, &out_bind->trans.z))
        goto fail;

    string = strtok_r(NULL, " \t", &saveptr);  
    if(!sscanf(string, "%f/%f/%f", &out->tip.x, &out->tip.y, &out->tip.z))
        goto fail;

    return true;

fail:
    return false;
}

static bool al_read_anim_clip(FILE *stream, struct anim_clip *out, 
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
            float euler[3]; /* XYZ order - in degrees */
        
            READ_LINE(stream, line, fail);
            if(!sscanf(line, "%d %f/%f/%f %f/%f/%f %f/%f/%f",
                &joint_idx, 
                &curr_joint_trans->scale.x,
                &curr_joint_trans->scale.y,
                &curr_joint_trans->scale.z,
                &euler[0],
                &euler[1],
                &euler[2],
                &curr_joint_trans->trans.x,
                &curr_joint_trans->trans.y,
                &curr_joint_trans->trans.z)) {
                goto fail;
            }
        
            euler_to_quat(euler, &curr_joint_trans->quat_rotation);
        }
    }

    return true;

fail:
    return false;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

size_t A_AL_PrivBuffSizeFromHeader(const struct pfobj_hdr *header)
{
    size_t ret;

    ret += sizeof(struct anim_private);
    ret += header->num_as     * sizeof(struct anim_clip);
    ret += header->num_joints * sizeof(struct SQT);
    ret += header->num_joints * sizeof(mat4x4_t);
    ret += header->num_joints * sizeof(struct joint);

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

/*
 * Animation private buff layout:
 *
 *  +---------------------------------+ <-- base
 *  | struct anim_private[1]          |
 *  +---------------------------------+
 *  | struct anim_data[1]             |
 *  +---------------------------------+
 *  | struct anim_ctx[1]              |
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

bool A_AL_InitPrivFromStream(const struct pfobj_hdr *header, FILE *stream, void *priv_buff)
{
    void *unused_base = priv_buff;
    struct anim_private *priv;

    /*-----------------------------------------------------------
     * First divide up the buffer betwen 'priv' members,
     * set counts and pointers 
     *-----------------------------------------------------------
     */
    priv = unused_base;
    unused_base += sizeof(struct anim_private);

    priv->data = unused_base;
    unused_base += sizeof(struct anim_data);

    priv->ctx = unused_base;
    unused_base += sizeof(struct anim_ctx);

    priv->data->num_anims = header->num_as; 
    priv->data->skel.num_joints = header->num_joints;

    priv->data->skel.bind_sqts = unused_base;
    unused_base += sizeof(struct SQT) * header->num_joints;

    priv->data->skel.inv_bind_poses = unused_base;
    unused_base += sizeof(mat4x4_t) * header->num_joints;

    priv->data->skel.joints = unused_base;
    unused_base += sizeof(struct joint) * header->num_joints;

    priv->data->anims = unused_base;
    unused_base += sizeof(struct anim_clip) * header->num_as;

    for(int i = 0; i < header->num_as; i++) {

        priv->data->anims[i].samples = unused_base;
        unused_base += sizeof(struct anim_sample) * header->frame_counts[i];
    }

    for(int i = 0; i < header->num_as; i++) {

        priv->data->anims[i].skel = &priv->data->skel;
        priv->data->anims[i].num_frames = header->frame_counts[i];

        for(int f = 0; f < header->frame_counts[i]; f++) {

            priv->data->anims[i].samples[f].local_joint_poses = unused_base;
            unused_base += sizeof(struct SQT) * header->num_joints;
        }
    }

    /*---------------------------------------------------------------
     * Then we populate priv members with the file data 
     *---------------------------------------------------------------
     */
    for(int i = 0; i < header->num_joints; i++) {

        if(!al_read_joint(stream, &priv->data->skel.joints[i], &priv->data->skel.bind_sqts[i]))
            goto fail;
    }

    for(int i = 0; i < header->num_as; i++) {
        
        if(!al_read_anim_clip(stream, &priv->data->anims[i], header))
            goto fail;
    }

    A_PrepareInvBindMatrices(&priv->data->skel);

    return true;

fail:
    return false;
}

void A_AL_DumpPrivate(FILE *stream, void *priv_data)
{
    struct anim_private *priv = priv_data;

    /* Write joints */
    for(int i = 0; i < priv->data->skel.num_joints; i++) {

        struct joint *j = &priv->data->skel.joints[i];
        struct SQT *bind = &priv->data->skel.bind_sqts[i];

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
    for(int i = 0; i < priv->data->num_anims; i++) {

        struct anim_clip *ac = &priv->data->anims[i];
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

