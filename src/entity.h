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

#ifndef ENTITY_H
#define ENTITY_H

#include "pf_math.h"
#include "lib/public/khash.h"
#include "lib/public/vec.h"
#include "map/public/tile.h"
#include "phys/public/collision.h"

#include <stdbool.h>
#include <stdint.h>

#define MAX_JOINTS  (96)
#define MAX_TAGS    (127)
#define MAX_ICONS   (4)
#define NULL_UID    (~((uint32_t)0))

enum{
    ENTITY_FLAG_ANIMATED            = (1 << 0),
    ENTITY_FLAG_COLLISION           = (1 << 1),
    ENTITY_FLAG_SELECTABLE          = (1 << 2),
    ENTITY_FLAG_MOVABLE             = (1 << 3),
    ENTITY_FLAG_COMBATABLE          = (1 << 4),
    ENTITY_FLAG_INVISIBLE           = (1 << 5),
    /* zombie entities are those that have died in the game simulation, 
     * but are still retained by some scripting variable */
    ENTITY_FLAG_ZOMBIE              = (1 << 6),
    ENTITY_FLAG_MARKER              = (1 << 7),
    ENTITY_FLAG_BUILDING            = (1 << 8),
    ENTITY_FLAG_BUILDER             = (1 << 9),
    ENTITY_FLAG_TRANSLUCENT         = (1 << 10),
    ENTITY_FLAG_RESOURCE            = (1 << 11),
    ENTITY_FLAG_HARVESTER           = (1 << 12),
    ENTITY_FLAG_STORAGE_SITE        = (1 << 13),
    ENTITY_FLAG_WATER               = (1 << 14),
    ENTITY_FLAG_AIR                 = (1 << 15),
    ENTITY_FLAG_GARRISON            = (1 << 16),
    ENTITY_FLAG_GARRISONABLE        = (1 << 17),
    ENTITY_FLAG_GARRISONED          = (1 << 18)
};

struct entity{
    const char  *name;
    const char  *basedir;
    const char  *filename;
    void        *render_private;
    void        *anim_private;
    struct aabb  identity_aabb; /* Bind-pose AABB */
};

/* State needed for rendering a static entity */
struct ent_stat_rstate{
    uint32_t         uid;
    void            *render_private;
    mat4x4_t         model;
    bool             translucent;
    struct tile_desc td; /* For binning to a chunk batch */
};

/* State needed for rendering an animated entity */
struct ent_anim_rstate{
    uint32_t        uid;
    void           *render_private;
    mat4x4_t        model;
    bool            translucent;
    size_t          njoints;
    const mat4x4_t *inv_bind_pose; /* static, use shallow copy */
    mat4x4_t        curr_pose[MAX_JOINTS];
};

struct transform{
    vec3_t scale;
    quat_t rotation;
};

VEC_TYPE(rstat, struct ent_stat_rstate)
VEC_IMPL(static inline, rstat, struct ent_stat_rstate)

VEC_TYPE(ranim, struct ent_anim_rstate)
VEC_IMPL(static inline, ranim, struct ent_anim_rstate)

KHASH_DECLARE(trans, khint32_t, struct transform)

struct map;

bool     Entity_Init(void);
void     Entity_Shutdown(void);
void     Entity_ClearState(void);

void     Entity_ModelMatrix(uint32_t uid, mat4x4_t *out);
uint32_t Entity_NewUID(void);
void     Entity_SetNextUID(uint32_t uid);
void     Entity_CurrentOBB(uint32_t uid, struct obb *out, bool identity);
vec3_t   Entity_CenterPos(uint32_t uid);
vec3_t   Entity_TopCenterPointWS(uint32_t uid);
void     Entity_FaceTowards(uint32_t uid, vec2_t point);
void     Entity_Ping(uint32_t uid);
vec2_t   Entity_TopScreenPos(uint32_t uid, int screenw, int screenh);
/* Coarse-grained test that can give false positives. Use the check to get
 * positives, but confirm positive results with a more precise check 
 */
bool     Entity_MaybeAdjacentFast(uint32_t a, uint32_t b, float buffer);
bool     Entity_AddTag(uint32_t uid, const char *tag);
void     Entity_RemoveTag(uint32_t uid, const char *tag);
bool     Entity_HasTag(uint32_t uid, const char *tag);
void     Entity_ClearTags(uint32_t uid);
size_t   Entity_EntsForTag(const char *tag, size_t maxout, uint32_t out[]);
size_t   Entity_TagsForEnt(uint32_t uid, size_t maxout, const char *out[]);
void     Entity_DisappearAnimated(uint32_t uid, const struct map *map, 
                                  void (*on_finish)(void*), void *arg);
int      Entity_NavLayer(uint32_t uid);
int      Entity_NavLayerWithRadius(uint32_t flags, float radius);

quat_t   Entity_GetRot(uint32_t uid);
void     Entity_SetRot(uint32_t uid, quat_t rot);
vec3_t   Entity_GetScale(uint32_t uid);
void     Entity_SetScale(uint32_t uid, vec3_t scale);
void     Entity_Remove(uint32_t uid);

khash_t(trans) *Entity_CopyTransforms(void);
quat_t          Entity_GetRotFrom(khash_t(trans) *table, uint32_t uid);
vec3_t          Entity_GetScaleFrom(khash_t(trans) *table, uint32_t uid);
void            Entity_ModelMatrixFrom(vec3_t pos, quat_t rot, vec3_t scale, mat4x4_t *out);
void            Entity_CurrentOBBFrom(const struct aabb *aabb, mat4x4_t model, 
                                      vec3_t scale, struct obb *out);

uint64_t        Entity_TypeID(uint32_t uid);

bool   Entity_SetIcons(uint32_t uid, size_t nicons, const char **icons);
size_t Entity_GetIcons(uint32_t uid, size_t maxout, const char *out[]);
void   Entity_ClearIcons(uint32_t uid);

#endif
