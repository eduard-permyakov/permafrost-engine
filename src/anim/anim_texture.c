/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2025 Eduard Permyakov 
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
#include "anim_texture.h"
#include "anim_private.h"
#include "anim_data.h"
#include "anim_ctx.h"
#include "../entity.h"
#include "../asset_load.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../lib/public/khash.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/string_intern.h"

#include <assert.h>
#include <string.h>

#define MIN(a, b)   ((a) < (b) ? (a) : (b))

struct anim_data_desc{
    uint32_t base_offset;
    uint32_t size;
    uint32_t njoints;
    uint32_t nanims;
    uint32_t anim_set_offsets[MAX_ANIM_SETS];
};

KHASH_MAP_INIT_STR(id, uint32_t)
KHASH_MAP_INIT_INT(desc, struct anim_data_desc)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static uint32_t          s_next_id = 0;
static uint32_t          s_next_offset = 0;
static khash_t(id)      *s_pfobj_id_map;
static khash_t(desc)    *s_id_desc_map;

static khash_t(stridx)  *s_stridx;
static mp_strbuff_t      s_stringpool;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static size_t anim_buff_size(const struct anim_data *data)
{
    size_t njoints = MIN(data->skel.num_joints, MAX_JOINTS_EXTENDED);
    size_t ret = sizeof(mat4x4_t) * njoints;

    for(int i = 0; i < data->num_anims; i++) {
        struct anim_clip *curr = &data->anims[i];
        ret += sizeof(mat4x4_t) * njoints * curr->num_frames;
    }
    return ret;
}

static size_t anim_buff_pose_offset(const struct anim_data_desc *ddesc,
                                    int clip_idx, int frame_idx)
{
    assert(clip_idx >= 0 && clip_idx < MAX_ANIM_SETS);
    size_t frame_size = ddesc->njoints * sizeof(mat4x4_t);
    size_t frame_offset = frame_size * frame_idx;
    return ddesc->anim_set_offsets[clip_idx] + frame_offset;
}

static struct anim_data_desc anim_copy_data(const struct anim_data *data, GLfloat *out)
{
    size_t njoints = MIN(data->skel.num_joints, MAX_JOINTS_EXTENDED);
    struct anim_data_desc ret = (struct anim_data_desc){
        .njoints = njoints,
        .base_offset = s_next_offset,
        .nanims = MIN(data->num_anims, MAX_ANIM_SETS)
    };
    size_t size = 0;

    memcpy(out, data->skel.inv_bind_poses, sizeof(mat4x4_t) * njoints);
    out += (16 * njoints);
    size += sizeof(mat4x4_t) * njoints;

    for(int i = 0; i < MIN(data->num_anims, MAX_ANIM_SETS); i++) {

        struct anim_clip *curr = &data->anims[i];
        ret.anim_set_offsets[i] = s_next_offset + size;

        for(int j = 0; j < curr->num_frames; j++) {
            size_t read_joints = 0;
            A_GetPoseRenderState(data, i, j, &read_joints, (mat4x4_t*)out);
            assert(read_joints == njoints);
            out += (16 * read_joints);
            size += sizeof(mat4x4_t) * read_joints;
        }
    }
    ret.size = size;
    return ret;
}

static void add_mappings(const char *pfobj, uint32_t id, const struct anim_data_desc *desc)
{
    int status;
    khiter_t k = kh_put(id, s_pfobj_id_map, pfobj, &status);
    assert(status != -1);
    kh_value(s_pfobj_id_map, k) = id;

    k = kh_put(desc, s_id_desc_map, id, &status);
    assert(status != -1);
    kh_value(s_id_desc_map, k) = *desc;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool A_Texture_Init(void)
{
    if(!si_init(&s_stringpool, &s_stridx, 2048))
        goto fail_strintern;

    s_pfobj_id_map = kh_init(id);
    if(!s_pfobj_id_map)
        goto fail_pfobj_id_map;

    s_id_desc_map = kh_init(desc);
    if(!s_id_desc_map)
        goto fail_id_desc_map;

    return true;

fail_id_desc_map:
    kh_destroy(id, s_pfobj_id_map);
fail_pfobj_id_map:
    si_shutdown(&s_stringpool, s_stridx);
fail_strintern:
    return false;
}

void A_Texture_Shutdown(void)
{
    kh_destroy(desc, s_id_desc_map);
    kh_destroy(id, s_pfobj_id_map);
    si_shutdown(&s_stringpool, s_stridx);
}

bool A_Texture_AppendData(const char *pfobj, const struct anim_data *data, uint32_t *out_id)
{
    khiter_t k = kh_get(id, s_pfobj_id_map, pfobj);
    if(k != kh_end(s_pfobj_id_map)) {
        *out_id = kh_val(s_pfobj_id_map, k);
        return true;
    }

    const char *str = si_intern(pfobj, &s_stringpool, s_stridx);
    if(!str)
        return false;

    size_t size = anim_buff_size(data);
    GLfloat *buff = R_AllocArg(size);
    if(!buff)
        return false;

    struct anim_data_desc desc = anim_copy_data(data, buff);
    assert(desc.size == size);
    uint32_t new_id = s_next_id++;
    s_next_offset += desc.size;

    add_mappings(str, new_id, &desc);
    R_PushCmd((struct rcmd){
        .func = R_GL_AnimAppendData,
        .nargs = 2,
        .args = {
            buff,
            R_PushArg(&size, sizeof(size))
        },
    });

    *out_id = new_id;
    return true;
}

bool A_Texture_CurrPoseDesc(const struct anim_ctx *ctx, struct anim_pose_data_desc *out)
{
    uint32_t id = ctx->data->texture_desc_id;
    int clip_idx = ctx->curr_clip_idx;
    int frame_idx = ctx->curr_frame;

    khiter_t k = kh_get(desc, s_id_desc_map, id);
    if(k == kh_end(s_id_desc_map))
        return false;

    const struct anim_data_desc *ddesc = &kh_val(s_id_desc_map, k);
    out->inv_bind_pose_offset = ddesc->base_offset;
    out->curr_pose_offset = anim_buff_pose_offset(ddesc, clip_idx, frame_idx);
    return true;
}

