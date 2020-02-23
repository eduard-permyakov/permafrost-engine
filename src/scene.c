/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2020 Eduard Permyakov 
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
#include "lib/public/pf_string.h"
#include "lib/public/attr.h"

#include <stdio.h>
#include <SDL.h>
#include <assert.h>


VEC_IMPL(extern, attr, struct attr)
__KHASH_IMPL(attr, extern, kh_cstr_t, struct attr, 1, kh_str_hash_func, kh_str_hash_equal)

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool scene_load_entity(SDL_RWops *stream)
{
    char line[MAX_LINE_LEN];
    char name[128];
    char path[256];
    unsigned num_atts;

    khash_t(attr) *attr_table = kh_init(attr);
    if(!attr_table)
        goto fail_alloc;

    vec_attr_t constructor_args;
    vec_attr_init(&constructor_args);

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, "entity %s %s %u", name, path, &num_atts))
        goto fail_parse;

    for(int i = 0; i < num_atts; i++) {
        struct attr attr;
        if(!Attr_Parse(stream, &attr, true))
            goto fail_parse;

        int ret;
        khiter_t k = kh_put(attr, attr_table, pf_strdup(attr.key), &ret);
        assert(ret != -1 && ret != 0);
        kh_value(attr_table, k) = attr;

        if(!strcmp(attr.key, "constructor_arguments")) {

            size_t num_args = attr.val.as_int;
            struct attr const_arg;
            
            for(int j = 0; j < num_args; j++) {
                if(!Attr_Parse(stream, &const_arg, false)) {
                    vec_attr_destroy(&constructor_args);
                    goto fail_parse;
                }
                vec_attr_push(&constructor_args, const_arg);
            }
        }
    }

    if(!S_Entity_ObjFromAtts(path, name, attr_table, &constructor_args))
        goto fail_init;

    const char *key;
    struct attr val;
    kh_foreach(attr_table, key, val, { 
        (void)val;
        free((void*)key);
    });
    
    kh_destroy(attr, attr_table);
    vec_attr_destroy(&constructor_args);
    return true;

fail_init:
    vec_attr_destroy(&constructor_args);
fail_parse:
    kh_destroy(attr, attr_table);
fail_alloc:
    return false;
}


static bool scene_load_faction(SDL_RWops *stream)
{
    char line[MAX_LINE_LEN];
    char name[32];
    struct attr color;

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, "faction \"%31[^\"]", name))
        goto fail_parse;

    if(!Attr_Parse(stream, &color, true))
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
    char line[MAX_LINE_LEN];
    unsigned num_factions, num_ents;

    stream = SDL_RWFromFile(path, "r");
    if(!stream)
        goto fail_stream;

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, "num_factions %u", &num_factions))
        goto fail_parse;

    for(int i = 0; i < num_factions; i++) {
        if(!scene_load_faction(stream))    
            goto fail_parse;
    }
    
    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, "num_entities %u", &num_ents))
        goto fail_parse;

    for(int i = 0; i < num_ents; i++) {
        if(!scene_load_entity(stream))
            goto fail_parse;
    }

    SDL_RWclose(stream);
    return true;
    
fail_parse:
    SDL_RWclose(stream);
fail_stream:
    return false;
}

