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

static bool al_read_joint(FILE *stream, struct joint *out)
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

    for(int i = 0; i < 4; i++) {

        mat4x4_t *ibp = &out->inv_bind_pose;

        string = strtok_r(NULL, " \t", &saveptr);  

        if(!string)
            goto fail;

        if(!sscanf(string, "%f/%f/%f/%f", 
            &ibp->cols[i][0], &ibp->cols[i][1],
            &ibp->cols[i][2], &ibp->cols[i][3])) {
            goto fail;     
        }
    }

    return true;

fail:
    return false;
}

static bool al_read_anim_clip(FILE *stream, struct anim_clip *out, 
                              const struct pfobj_hdr *header)
{
    char line[MAX_LINE_LEN];

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
    ret += header->num_joints * sizeof(struct joint);

    /*
     * For each frame of each animation clip, we also require:
     *
     *    1. a 'struct anim_sample' (for referencing this frame's SQT array)
     *    2. num_joint number of 'struct SQT's (each joint's transform
     *       for the current frame)
     */
    for(unsigned as_idx  = 0; as_idx  < header->num_as; as_idx++) {

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
 *  | struct joint[num_joints]        |
 *  +---------------------------------+
 *  | struct anim_clip[num_as]        |
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

    /* First divide up the buffer betwen 'priv' members */
    priv = unused_base;
    unused_base += sizeof(struct anim_private);

    priv->data = unused_base;
    unused_base += sizeof(struct anim_data);

    priv->ctx = unused_base;
    unused_base += sizeof(struct anim_ctx);

    priv->data->num_anims = header->num_as; 
    priv->data->skel.num_joints = header->num_joints;
    priv->data->skel.joints = unused_base;
    unused_base += sizeof(struct joint) * header->num_joints;
    priv->data->anims = unused_base;
    unused_base += sizeof(struct anim_clip) * header->num_as;

    for(int i = 0; i < header->num_as; i++) {
        for(int f = 0; f < header->frame_counts[i]; f++) {

            priv->data->anims[i].samples[f].local_joint_poses = unused_base;
            unused_base += sizeof(struct SQT) * header->num_joints;
        }
    }

    /* Then we populate priv members with the file data */
    for(int i = 0; i < header->num_joints; i++) {

        if(!al_read_joint(stream, &priv->data->skel.joints[i])) 
            goto fail;
    }

    for(int i = 0; i < header->num_as; i++) {
        
        if(!al_read_anim_clip(stream, &priv->data->anims[i], header))
            goto fail;

    }

    return true;

fail:
    return false;
}

