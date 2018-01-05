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

#ifndef ASSET_LOAD_H
#define ASSET_LOAD_H

#include <stddef.h>
#include <stdbool.h>


#define MAX_ANIM_SETS 16
#define MAX_LINE_LEN  256

struct entity;
struct map;

struct pfobj_hdr{
    float    version; 
    unsigned num_verts;
    unsigned num_joints;
    unsigned num_materials;
    unsigned num_as;
    unsigned frame_counts[MAX_ANIM_SETS];
};

struct entity *AL_EntityFromPFObj(const char *base_path, const char *pfobj_name, const char *name);
void           AL_EntityFree(struct entity *entity);

/* TODO: eventually we will only need to pass the pfmap_path - it will hold everything */
bool           AL_InitMapFromPFMap(const char *pfchunk_path, const char *pfmat_path, size_t num_mats,
                                   struct map *out);

#endif
