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
struct aabb;

struct pfobj_hdr{
    float    version; 
    unsigned num_verts;
    unsigned num_joints;
    unsigned num_materials;
    unsigned num_as;
    unsigned frame_counts[MAX_ANIM_SETS];
    bool     has_collision;
};

struct pfmap_hdr{
    float    version;
    unsigned num_rows;
    unsigned num_cols;
};


bool           AL_Init(void);
void           AL_Shutdown(void);

struct entity *AL_EntityFromPFObj(const char *base_path, const char *pfobj_name, const char *name);
void           AL_EntityFree(struct entity *entity);

struct map    *AL_MapFromPFMap(const char *base_path, const char *pfmap_name);
struct map    *AL_MapFromPFMapString(const char *str);
void           AL_MapFree(struct map *map);

bool           AL_ReadLine(SDL_RWops *stream, char *outbuff);
bool           AL_ParseAABB(SDL_RWops *stream, struct aabb *out);

#endif
