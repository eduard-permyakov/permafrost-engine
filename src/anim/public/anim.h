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

#ifndef ANIM_H
#define ANIM_H

#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>

struct pfobj_hdr;
struct entity;
struct skeleton;

enum anim_mode{
    ANIM_MODE_LOOP,
    ANIM_MODE_ONCE
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
 * If anim_mode is 'ANIM_MODE_ONCE', the entity will go back to 
 * playing the 'idle' animtion once the clip has played once. Otherwise, it will
 * keep looping the clip.
 * ---------------------------------------------------------------------------
 */
void                   A_SetActiveClip(const struct entity *ent, const char *name, 
                                       enum anim_mode mode, unsigned key_fps);

/* ---------------------------------------------------------------------------
 * Should be called once per render loop, prior to rendering. Will update OpenGL
 * state based on the current active clip and time.
 * ---------------------------------------------------------------------------
 */
void                   A_Update(const struct entity *ent);

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


/*###########################################################################*/
/* ANIM ASSET LOADING                                                        */
/*###########################################################################*/

/* ---------------------------------------------------------------------------
 * Computes the size (in bytes) that is required to store all the animation subsystem
 * data from a PF Object file.
 * ---------------------------------------------------------------------------
 */
size_t A_AL_PrivBuffSizeFromHeader(const struct pfobj_hdr *header);

/* ---------------------------------------------------------------------------
 * Consumes lines of the stream and uses them to populate the private data stored 
 * in priv_buff.
 * ---------------------------------------------------------------------------
 */
bool   A_AL_InitPrivFromStream(const struct pfobj_hdr *header, FILE *stream, void *priv_buff);

/* ---------------------------------------------------------------------------
 * Dumps private animation data in PF Object format.
 * ---------------------------------------------------------------------------
 */
void   A_AL_DumpPrivate(FILE *stream, void *priv_data);

#endif
