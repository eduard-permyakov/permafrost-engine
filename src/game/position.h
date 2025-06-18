/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019-2023 Eduard Permyakov 
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

#ifndef POSITION_H
#define POSITION_H

#include "game_private.h"
#include "../pf_math.h"
#include "../lib/public/khash.h"
#include "../lib/public/quadtree.h"

struct map;

QUADTREE_TYPE(ent, uint32_t)
QUADTREE_PROTOTYPES(extern, ent, uint32_t)

KHASH_DECLARE(pos, khint32_t, vec3_t)

bool      G_Pos_Init(const struct map *map);
void      G_Pos_Shutdown(void);
void      G_Pos_Delete(uint32_t uid);
void      G_Pos_Upload(void);

qt_ent_t *G_Pos_CopyQuadTree(void);
void      G_Pos_DestroyQuadTree(qt_ent_t *tree);
int       G_Pos_EntsInCircleFrom(qt_ent_t *tree, khash_t(id) *flags, vec2_t xz_point, float range, 
                                 uint32_t *out, size_t maxout);
int       G_Pos_EntsInCircleWithPredFrom(qt_ent_t *tree, khash_t(id) *flags, 
                                         vec2_t xz_point, float range, 
                                         uint32_t *out, size_t maxout,
                                         bool (*predicate)(uint32_t ent, void *arg), void *arg);

khash_t(pos) *G_Pos_CopyTable(void);

void      G_Pos_Garrison(uint32_t uid);
void      G_Pos_Ungarrison(uint32_t uid, vec3_t pos);

#endif

