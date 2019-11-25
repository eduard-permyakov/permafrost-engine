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

#include "asset_load.h"
#include "entity.h"

#include "render/public/render.h"
#include "anim/public/anim.h"
#include "game/public/game.h"
#include "map/public/map.h"
#include "lib/public/khash.h"
#include "lib/public/pf_string.h"

#include <SDL.h>

#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h> 


struct shared_resource{
    char         key[64];
    uint32_t     ent_flags;
    void        *render_private;
    void        *anim_private;
    struct aabb  aabb;
};

KHASH_MAP_INIT_STR(entity_res, struct shared_resource)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(entity_res) *s_name_resource_table;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

/* A copy of the key string is stored in the 'struct shared_resource' itself. Make the key 
 * (string pointer) be a pointer to that buffer in order to avoid allocating/storing the key 
 * strings separately. All keys must be patched in case rehashing took place. */
static void kh_update_str_keys(khash_t(entity_res) *table)
{
    for(khiter_t k = kh_begin(table); k != kh_end(table); k++) {
        if(!kh_exist(table, k)) continue;
        kh_key(table, k) = kh_value(table, k).key;
    }
}

static bool al_parse_pfobj_header(SDL_RWops *stream, struct pfobj_hdr *out)
{
    char line[MAX_LINE_LEN];

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "version %f", &out->version))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "num_verts %d", &out->num_verts))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "num_joints %d", &out->num_joints))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "num_materials %d", &out->num_materials))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "num_as %d", &out->num_as))
        goto fail;

    if(out->num_as > MAX_ANIM_SETS)
        goto fail;

    READ_LINE(stream, line, fail);
    if(!(strstr(line, "frame_counts")))
        goto fail;

    char *string = line;
    char *saveptr;

    /* Consume the first token, the property name 'frame_counts' */
    string = pf_strtok_r(line, " \t", &saveptr);
    for(int i = 0; i < out->num_as; i++) {

        string = pf_strtok_r(NULL, " \t", &saveptr);
        if(!string)
            goto fail;

        if(!sscanf(string, "%d", &out->frame_counts[i]))
            goto fail;
    }

    int tmp;
    READ_LINE(stream, line, fail);
    if(!sscanf(line, "has_collision %d", &tmp))
        goto fail;
    out->has_collision = tmp;

    return true;

fail:
    return false;
}

static bool al_parse_pfmap_header(SDL_RWops *stream, struct pfmap_hdr *out)
{
    char line[MAX_LINE_LEN];

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "version %f", &out->version))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "num_materials %d", &out->num_materials))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "num_rows %d", &out->num_rows))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, "num_cols %d", &out->num_cols))
        goto fail;

    return true;

fail:
    return false;
}

static struct map *al_map_from_stream(const char *base_path, SDL_RWops *stream)
{
    struct map *ret;
    struct pfmap_hdr header;

    if(!al_parse_pfmap_header(stream, &header))
        goto fail_parse;

    ret = malloc(M_AL_BuffSizeFromHeader(&header));
    if(!ret)
        goto fail_alloc;

    if(!M_AL_InitMapFromStream(&header, base_path, stream, ret))
        goto fail_init;

    return ret;

fail_init:
    free(ret);
fail_alloc:
fail_parse:
    return NULL;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

struct entity *AL_EntityFromPFObj(const char *base_path, const char *pfobj_name, const char *name)
{
    struct shared_resource res;
    SDL_RWops *stream;

    size_t alloc_size = sizeof(struct entity) + A_AL_CtxBuffSize();
    struct entity *ret = malloc(alloc_size);
    if(!ret)
        goto fail_alloc;

    ret->flags = 0;
    ret->max_hp = 0;
    ret->scale = (vec3_t){1.0f, 1.0f, 1.0f};
    ret->rotation = (quat_t){0.0f, 0.0f, 0.0f, 1.0f};
    ret->selection_radius = 0.0f;
    ret->max_speed = 0.0f;
    ret->faction_id = 0; 
    ret->anim_ctx = (void*)(ret + 1);

    if(strlen(name) >= sizeof(ret->name))
        return NULL;
    strcpy(ret->name, name);

    if(strlen(name) >= sizeof(ret->name))
        return NULL;
    strcpy(ret->name, name);

    if(strlen(pfobj_name) >= sizeof(ret->filename))
        return NULL;
    strcpy(ret->filename, pfobj_name);

    assert(strlen(base_path) < sizeof(ret->basedir));
    strcpy(ret->basedir, base_path);
    strcpy(res.key, pfobj_name);

    khiter_t k = kh_get(entity_res, s_name_resource_table, pfobj_name);
    if(k != kh_end(s_name_resource_table)) {

        res = kh_value(s_name_resource_table, k);
    }else{

        struct pfobj_hdr header;

        char pfobj_path[128];
        assert( strlen(base_path) + strlen(pfobj_name) + 1 < sizeof(pfobj_path) );
        strcpy(pfobj_path, base_path);
        strcat(pfobj_path, "/");
        strcat(pfobj_path, pfobj_name);

        stream = SDL_RWFromFile(pfobj_path, "r");
        if(!stream)
            goto fail_stream; 

        if(!al_parse_pfobj_header(stream, &header))
            goto fail_parse;

        res.ent_flags = 0;
        res.render_private = R_AL_PrivFromStream(base_path, &header, stream);
        if(!res.render_private)
            goto fail_parse;

        res.anim_private = A_AL_PrivFromStream(&header, stream);
        if(!res.anim_private)
            goto fail_parse;

        /* Entities with no animation sets are considered static. */
        if(header.num_as > 0) {
            res.ent_flags |= ENTITY_FLAG_ANIMATED;
        }

        if(!header.has_collision) {
            fprintf(stderr, "Imported entities required to have bounding boxes.\n");
            goto fail_parse;
        }

        res.ent_flags |= ENTITY_FLAG_COLLISION;
        if(!AL_ParseAABB(stream, &res.aabb))
            goto fail_parse;

        SDL_RWclose(stream);

        int put_ret;
        k = kh_put(entity_res, s_name_resource_table, pfobj_name, &put_ret);
        assert(put_ret != -1 && put_ret != 0);
        kh_value(s_name_resource_table, k) = res;
        kh_update_str_keys(s_name_resource_table);
    }

    ret->flags |= res.ent_flags;
    ret->render_private = res.render_private;
    ret->anim_private = res.anim_private;
    ret->identity_aabb = res.aabb;
    ret->uid = Entity_NewUID();

    return ret;

fail_parse:
    SDL_RWclose(stream);
fail_stream:
    free(ret);
fail_alloc:
    return NULL;
}

void AL_EntityFree(struct entity *entity)
{
    free(entity);
}

struct map *AL_MapFromPFMap(const char *base_path, const char *pfmap_name)
{
    struct map *ret;
    SDL_RWops *stream;

    char pfmap_path[128];
    assert( strlen(base_path) + strlen(pfmap_name) + 1 < sizeof(pfmap_path) );
    strcpy(pfmap_path, base_path);
    strcat(pfmap_path, "/");
    strcat(pfmap_path, pfmap_name);

    stream = SDL_RWFromFile(pfmap_path, "r");
    ret = al_map_from_stream(base_path, stream);
    if(!ret)
        goto fail_parse;

    SDL_RWclose(stream);
    return ret;

fail_parse:
    SDL_RWclose(stream);
fail_open:
    return NULL;
}

struct map *AL_MapFromPFMapString(const char *str)
{
    struct map *ret;
    SDL_RWops *stream;

    stream = SDL_RWFromConstMem(str, strlen(str));
    ret = al_map_from_stream(NULL, stream);
    if(!ret)
        goto fail_parse;

    SDL_RWclose(stream);
    return ret;

fail_parse:
    SDL_RWclose(stream);
fail_open:
    return NULL;
}

void AL_MapFree(struct map *map)
{
    M_AL_FreePrivate(map);
    free(map);
}

bool AL_ReadLine(SDL_RWops *stream, char *outbuff)
{
    bool done = false;
    int idx = 0;
    do { 
        if(!SDL_RWread(stream, outbuff + idx, 1, 1))
            return false; 

        if(outbuff[idx] == '\n') {
            outbuff[++idx] = '\0';
            return true;
        }
        
        idx++; 
    }while(idx < MAX_LINE_LEN-1);

    return false;
}

bool AL_ParseAABB(SDL_RWops *stream, struct aabb *out)
{
    char line[MAX_LINE_LEN];

    READ_LINE(stream, line, fail);
    if(!sscanf(line, " x_bounds %f %f", &out->x_min, &out->x_max))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, " y_bounds %f %f", &out->y_min, &out->y_max))
        goto fail;

    READ_LINE(stream, line, fail);
    if(!sscanf(line, " z_bounds %f %f", &out->z_min, &out->z_max))
        goto fail;

    return true;

fail:
    return false;
}

bool AL_Init(void)
{
    s_name_resource_table = kh_init(entity_res);
    return (s_name_resource_table != NULL);
}

void AL_Shutdown(void)
{
    kh_destroy(entity_res, s_name_resource_table);
}

