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

#include "../lib/public/pf_math.h"
#include "../config.h"
#include "../anim/public/anim.h"

#include <stdint.h>
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

/* The bake-time inputs that change how the sprites look without changing the
 * cell layout. A variant change swaps the atlas texture in place; the resident
 * variant is tracked by generation number.
 */
struct bb_variant{
    bool   shadowed;
    int    elev_deg;   /* bake camera elevation above the horizon */
    vec3_t light_pos;
    vec3_t ambient_color;
    vec3_t emit_color;
};

/* The textures of one baked atlas, each an array with a slice per cell */
struct bb_atlas{
    GLuint color;   /* RGBA, mipmapped */
    GLuint depth;   /* the bake's window depths */
    GLuint surface; /* mipmapped; octahedral normal, ambient and specular strengths */
    GLuint sil;     /* the model's depths as seen from the light, at 'sil_res' */
};

/* The view-plane framing of an atlas baked at one elevation: the extents of
 * the sprite and the offset of its centre from the model origin, in model
 * space, and the linear decoding of its baked depths into offsets from the
 * card plane.
 */
struct bb_frame{
    vec2_t world_size;
    vec2_t anchor_off;
    float  depth_extent;
    vec2_t depth_decode;   /* offset = depth * x + y */
    /* From the silhouette's light clip space back to the bake's model space */
    mat4x4_t sil_unproj;
    /* How far toward the light a sprite looks up the shadows it receives, in
     * model units: past its own silhouette's texel error
     */
    float    shadow_offset;
};

/* The billboard atlas for one model: one texture array slice per cell, laid
 * out [clip][keyframe][azimuth]. The layout fields are written once at
 * registration on the main thread and immutable afterwards; the resident
 * atlas and its framing belong to the render thread.
 */
struct bb_model_desc{
    void        *render_key;               /* the model's base render_private */
    void        *bake_private;             /* the render_private the sprites are baked from */
    char         cache_name[BB_CACHE_NAME_LEN];
    uint64_t     tag;
    int          nazimuths;
    int          cell_res;
    int          sil_res;
    int          total_slices;
    /* The largest view-plane extent at the RTS elevation of registration, which
     * sized the cells
     */
    float        ref_extent;
    /* Rotation-invariant model-space bounds of the baked samples: the
     * horizontal radius about the Y axis and the vertical range
     */
    float        radius;
    float        ymin;
    float        ymax;
    int state;                    /* enum bb_state */
    int baked_gen;                /* variant generation of the resident atlas */
    int baked_elev;               /* elevation of the resident atlas, degrees */
    int          inflight_gen;             /* main thread: generation of the enqueued bake */
    uint32_t     demand_frame;             /* main thread: last frame it was wanted on screen */
    struct bb_frame frame;                 /* render thread only */
    struct bb_atlas atlas;                 /* render thread only */
    int          nclips;                   /* 0 for models without animations */
    struct bb_clip_desc clips[];
};

/* Render thread */
bool R_GL_Billboard_InitCtx(void);
void R_GL_Billboard_ShutdownCtx(void);

#endif
