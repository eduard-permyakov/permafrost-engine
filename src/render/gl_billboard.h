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

#ifndef GL_BILLBOARD_H
#define GL_BILLBOARD_H

#include "../pf_math.h"
#include "../config.h"
#include "../anim/public/anim.h"

#include <stdint.h>
#include <SDL_atomic.h>
#include <GL/glew.h>

#define BB_CACHE_NAME_LEN (256)

enum bb_state{
    BB_STATE_PENDING = 0, /* registered; bake not yet enqueued */
    BB_STATE_QUEUED,      /* bake command pushed to the render thread */
    BB_STATE_READY,       /* atlas resident; eligible for drawing */
    BB_STATE_FAILED,      /* bake failed; entities keep rendering as meshes */
};

struct bb_clip_desc{
    int first_slice;
    int nkf;
    int nframes;
    /* Pose descriptors for the baked keyframes, resolved at registration so
     * the render thread never touches the animation internals.
     */
    struct anim_pose_data_desc kf_pose[CONFIG_BILLBOARD_MAX_KF];
};

/* The billboard atlas for one model: one texture array slice per cell, laid
 * out [clip][keyframe][azimuth]. All fields except 'state' and 'tex_arr' are
 * written once at registration on the main thread and immutable afterwards.
 */
struct bb_model_desc{
    void        *render_key;               /* the model's base render_private */
    char         cache_name[BB_CACHE_NAME_LEN];
    uint64_t     tag;
    int          nazimuths;
    int          cell_res;
    int          total_slices;
    float        tilt_rad;                 /* bake camera elevation */
    /* View-plane extents of the sprite and the offset of its centre from the
     * model origin, in model space; the per-entity scale applies at draw.
     */
    vec2_t       world_size;
    vec2_t       anchor_off;
    SDL_atomic_t state;                    /* enum bb_state */
    GLuint       tex_arr;                  /* render thread only */
    int          nclips;                   /* 0 for models without animations */
    struct bb_clip_desc clips[];
};

/* Render thread */
bool R_GL_Billboard_InitCtx(void);
void R_GL_Billboard_ShutdownCtx(void);

#endif
