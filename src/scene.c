/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018 Eduard Permyakov 
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

#include "scene.h"
#include "asset_load.h"
#include "script/public/script.h"
#include "game/public/game.h"

#include <stdio.h>
#include <SDL.h>
#include <assert.h>


__KHASH_IMPL(attr, extern, kh_cstr_t, struct attr, 1, kh_str_hash_func, kh_str_hash_equal)

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

/* A copy of the key string is stored in the 'struct attr' itself. Make the key (string pointer)
 * be a pointer to that buffer in order to avoid allocating/storing the key strings separately. 
 * All keys must be patched in case rehashing took place. */
static void kh_update_str_keys(khash_t(attr) *table)
{
    for(khiter_t k = kh_begin(table); k != kh_end(table); k++) {
        if(!kh_exist(table, k)) continue;
        kh_key(table, k) = kh_value(table, k).key;
    }
}

static bool scene_parse_att(SDL_RWops *stream, struct attr *out, bool anon)
{
    char line[256];
    READ_LINE(stream, line, fail);
    char *saveptr;
    char *token;

    if(!anon) {
        token = strtok_r(line, " \t", &saveptr);

        strncpy(out->key, token, sizeof(out->key)); 
        out->key[sizeof(out->key)-1] = '\0';

        token = strtok_r(NULL, " \t", &saveptr);
    }else{
        token = strtok_r(line, " \t", &saveptr);
    }

    if(!strcmp(token, "string")) {

        out->type = TYPE_STRING;
        token = strtok_r(NULL, " \t", &saveptr);
        if(!sscanf(token, "%63s", out->val.as_string))
            goto fail;

    }else if(!strcmp(token, "quat")) {

        out->type = TYPE_QUAT;
        token = token + strlen(token) + 1;
        if(!sscanf(token, "%f %f %f %f", 
            &out->val.as_quat.x, &out->val.as_quat.y, &out->val.as_quat.z, &out->val.as_quat.w))
            goto fail;

    }else if(!strcmp(token, "vec3")) {

        out->type = TYPE_VEC3;
        token = token + strlen(token) + 1;
        if(!sscanf(token, "%f %f %f", 
            &out->val.as_vec3.x, &out->val.as_vec3.y, &out->val.as_vec3.z))
            goto fail;

    }else if(!strcmp(token, "bool")) {

        out->type = TYPE_BOOL;
        token = strtok_r(NULL, " \t", &saveptr);
        int tmp;
        if(!sscanf(token, "%d", &tmp))
            goto fail;
        if(tmp != 0 && tmp != 1)
            goto fail;
        out->val.as_bool = tmp;

    }else if(!strcmp(token, "float")) {

        out->type = TYPE_FLOAT;
        token = strtok_r(NULL, " \t", &saveptr);
        if(!sscanf(token, "%f", &out->val.as_float))
            goto fail;

    }else if(!strcmp(token, "int")) {

        out->type = TYPE_INT;
        token = strtok_r(NULL, " \t", &saveptr);
        if(!sscanf(token, "%d", &out->val.as_int))
            goto fail;

    }else {
        goto fail;
    }

    return true;

fail:
    return false;
}

static bool scene_load_entity(SDL_RWops *stream)
{
    char line[256];
    char name[128];
    char path[256];
    size_t num_atts;

    khash_t(attr) *attr_table = kh_init(attr);
    if(!attr_table)
        goto fail_alloc;

    kvec_attr_t constructor_args;
    kv_init(constructor_args);

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, "entity %s %s %lu", name, path, &num_atts))
        goto fail_parse;

    for(int i = 0; i < num_atts; i++) {
        struct attr attr;
        if(!scene_parse_att(stream, &attr, false))
            goto fail_parse;

        int ret;
        khiter_t k = kh_put(attr, attr_table, attr.key, &ret);
        assert(ret != -1 && ret != 0);
        kh_value(attr_table, k) = attr;
        kh_update_str_keys(attr_table);

        if(!strcmp(attr.key, "constructor_arguments")) {

            size_t num_args = attr.val.as_int;
            struct attr const_arg;
            
            for(int j = 0; j < num_args; j++) {
                if(!scene_parse_att(stream, &const_arg, true)) {
                    kv_destroy(constructor_args);
                    goto fail_parse;
                }
                kv_push(struct attr, constructor_args, const_arg);
            }
        }
    }

    if(!S_Entity_ObjFromAtts(path, name, attr_table, &constructor_args))
        goto fail_init;
    
    kh_destroy(attr, attr_table);
    kv_destroy(constructor_args);
    return true;

fail_init:
    kv_destroy(constructor_args);
fail_parse:
    kh_destroy(attr, attr_table);
fail_alloc:
    return false;
}


static bool scene_load_faction(SDL_RWops *stream)
{
    char line[256];
    char name[32];
    struct attr color;

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, "faction \"%31[^\"]", name))
        goto fail_parse;

    if(!scene_parse_att(stream, &color, false))
        goto fail_parse;

    if(color.type != TYPE_VEC3)
        goto fail_parse;

    G_AddFaction(name, color.val.as_vec3);
    return true;

fail_parse:
    return false;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool Scene_Load(const char *path)
{
    SDL_RWops *stream;
    char line[128];
    size_t num_factions, num_ents;

    stream = SDL_RWFromFile(path, "r");
    if(!stream)
        goto fail_stream;

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, "num_factions%lu", &num_factions))
        goto fail_parse;

    for(int i = 0; i < num_factions; i++) {
        if(!scene_load_faction(stream))    
            goto fail_parse;
    }
    
    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, "num_entities %lu", &num_ents))
        goto fail_parse;

    for(int i = 0; i < num_ents; i++) {
        if(!scene_load_entity(stream)){
            printf("didn't parse entity\n");
            goto fail_parse;
         }
    }

    SDL_RWclose(stream);
    return true;
    
fail_parse:
    SDL_RWclose(stream);
fail_stream:
    return false;
}

