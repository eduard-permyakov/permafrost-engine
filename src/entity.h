/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2018 Eduard Permyakov 
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
#include "collision.h"

#include <stdbool.h>

#define ENTITY_FLAG_ANIMATED    (1 << 0)
#define ENTITY_FLAG_COLLISION   (1 << 1)
#define ENTITY_FLAG_SELECTABLE  (1 << 2)
#define ENTITY_FLAG_STATIC      (1 << 3)

struct entity{
    uint32_t     uid;
    char         name[32];
    char         basedir[64];
    char         filename[32];
    vec3_t       pos;
    vec3_t       scale;
    quat_t       rotation;
    uint32_t     flags;
    void        *render_private;
    void        *anim_private;
    void        *anim_ctx;
    /* For animated entities, this is the bind pose AABB. Each
     * animation sample also has its' own AABB. */
    struct aabb  identity_aabb;
    float        selection_radius;
    float        max_speed; /* units: OpenGL coordinates / second */
    int          faction_id;
};

void     Entity_ModelMatrix(const struct entity *ent, mat4x4_t *out);
uint32_t Entity_NewUID(void);
void     Entity_CurrentOBB(const struct entity *ent, struct obb *out);

#endif
