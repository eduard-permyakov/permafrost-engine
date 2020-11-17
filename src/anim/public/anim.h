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

#ifndef ANIM_H
#define ANIM_H

#include "../../pf_math.h"

#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>

#include <SDL.h> /* for SDL_RWops */

struct pfobj_hdr;
struct entity;
struct skeleton;

enum anim_mode{
    ANIM_MODE_LOOP,
    ANIM_MODE_ONCE,
    ANIM_MODE_ONCE_HIDE_ON_FINISH,
};


/*###########################################################################*/
/* ANIM GENERAL                                                              */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Perform one-time context initialization and set the animation clip that will 
 * play when no other animation clips are active.
 *
 * Note that 'key_fps' is the number of key frames to cycle through each
 * second, not the frame rendering rate.
 * ---------------------------------------------------------------------------
 */
void                   A_InitCtx(const struct entity *ent, const char *idle_clip, 
                                 unsigned key_fps);

/* ---------------------------------------------------------------------------
 * Set the animation clip that will play when no other animation clips are active.
 * ---------------------------------------------------------------------------
 */
void                   A_SetIdleClip(const struct entity *ent, const char *name, 
                                     unsigned key_fps);

/* ---------------------------------------------------------------------------
 * If anim_mode is 'ANIM_MODE_ONCE', the entity will fire an 'EVENT_ANIM_FINISHED'
 * event and go back to playing the 'idle' animtion once the clip has played once. 
 * Otherwise, it will keep looping the clip.
 * ---------------------------------------------------------------------------
 */
void                   A_SetActiveClip(const struct entity *ent, const char *name, 
                                       enum anim_mode mode, unsigned key_fps);

/* ---------------------------------------------------------------------------
 * Should be called once per render loop, prior to rendering. Will update the
 * animation context based on the current time.
 * ---------------------------------------------------------------------------
 */
void                   A_Update(struct entity *ent);

/* ---------------------------------------------------------------------------
 * Retreive a copy of the state needed to render an animated entity.
 * ---------------------------------------------------------------------------
 */
void                   A_GetRenderState(const struct entity *ent, size_t *out_njoints, 
                                        mat4x4_t *out_curr_pose, const mat4x4_t **out_inv_bind_pose);

/* ---------------------------------------------------------------------------
 * Simple utility to get a reference to the skeleton structure in its' default
 * bind pose. The skeleton structure shoould not be modified or freed.
 * ---------------------------------------------------------------------------
 */
const struct skeleton *A_GetBindSkeleton(const struct entity *ent);

/* ---------------------------------------------------------------------------
 * Create a new skeleton for the current frame of the active clip.
 *
 * Note that this is a _debugging_ function, creating a skeleton to render based
 * on context. It will allocate a new buffer for the skeleton, which should be 
 * 'free'd by the caller. It should _not_ be called outside of debugging.
 * ---------------------------------------------------------------------------
 */
const struct skeleton *A_GetCurrPoseSkeleton(const struct entity *ent);

/* ---------------------------------------------------------------------------
 * Returns a pointer to the AABB for the current sample. The pointer should 
 * not be freed. 
 * ---------------------------------------------------------------------------
 */
const struct aabb     *A_GetCurrPoseAABB(const struct entity *ent);

/* ---------------------------------------------------------------------------
 * Add a time delta (in SDL ticks) to the start time of the previous frame.
 * This is used to shift the timestamps after pausing and resuming the game.
 * ---------------------------------------------------------------------------
 */
void                   A_AddTimeDelta(const struct entity *ent, uint32_t dt);

/* ---------------------------------------------------------------------------
 * Get the name of the idle clip for the entity. The returned string must
 * not be freed.
 * ---------------------------------------------------------------------------
 */
const char            *A_GetIdleClip(const struct entity *ent);

/* ---------------------------------------------------------------------------
 * Get the name of the idle clip for the entity. The returned string must
 * not be freed.
 * ---------------------------------------------------------------------------
 */
const char            *A_GetCurrClip(const struct entity *ent);

/* ---------------------------------------------------------------------------
 * Get the name of the clip at a specific index.
 * ---------------------------------------------------------------------------
 */
const char            *A_GetClip(const struct entity *ent, int idx);

/* ---------------------------------------------------------------------------
 * Returns true if the entity has the animation clip with the specified name.
 * ---------------------------------------------------------------------------
 */
bool                   A_HasClip(const struct entity *ent, const char *name);

/* ---------------------------------------------------------------------------
 * Serialize the animation context for the entity to the stream.
 * ---------------------------------------------------------------------------
 */
bool                   A_SaveState(struct SDL_RWops *stream, const struct entity *ent);

/* ---------------------------------------------------------------------------
 * Deserialize the animation context that was previously written by A_SaveState.
 * ---------------------------------------------------------------------------
 */
bool                   A_LoadState(struct SDL_RWops *stream, const struct entity *ent);

/*###########################################################################*/
/* ANIM ASSET LOADING                                                        */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Returns the size (in bytes) that is required to store private animation 
 * context for a single entity.
 * ---------------------------------------------------------------------------
 */
size_t A_AL_CtxBuffSize(void);

/* ---------------------------------------------------------------------------
 * Consumes lines of the stream and uses them to populate the private data, 
 * which is then returned in a malloc'd buffer.
 * ---------------------------------------------------------------------------
 */
void  *A_AL_PrivFromStream(const struct pfobj_hdr *header, SDL_RWops *stream);

/* ---------------------------------------------------------------------------
 * Dumps private animation data in PF Object format.
 * ---------------------------------------------------------------------------
 */
void   A_AL_DumpPrivate(FILE *stream, void *priv_data);

#endif
