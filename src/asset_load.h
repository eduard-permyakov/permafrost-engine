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
 */

#ifndef ASSET_LOAD_H
#define ASSET_LOAD_H

#include <stddef.h>
#include <stdbool.h>

#include <SDL.h> /* for SDL_RWops */


#define MAX_ANIM_SETS 16
#define MAX_LINE_LEN  256

#if defined(_WIN32)
    #define strtok_r strtok_s
#endif

#define READ_LINE(rwops, buff, fail_label)              \
    do{                                                 \
        if(!AL_ReadLine(rwops, buff))                   \
            goto fail_label;                            \
        buff[MAX_LINE_LEN - 1] = '\0';                  \
    }while(0)


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

struct pfmap_hdr{
    float    version;
    unsigned num_rows;
    unsigned num_cols;
};

struct entity *AL_EntityFromPFObj(const char *base_path, const char *pfobj_name, const char *name);
void           AL_EntityFree(struct entity *entity);

struct map    *AL_MapFromPFMap(const char *base_path, const char *pfmap_name);
struct map    *AL_MapFromPFMapString(const char *str);
void           AL_MapFree(struct map *map);

bool           AL_ReadLine(SDL_RWops *stream, char *outbuff);

#endif
