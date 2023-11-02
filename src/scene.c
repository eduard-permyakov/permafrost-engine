/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2023 Eduard Permyakov 
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
#include "sched.h"
#include "asset_load.h"
#include "main.h"
#include "script/public/script.h"
#include "game/public/game.h"
#include "lib/public/pf_string.h"
#include "lib/public/attr.h"

#include <stdio.h>
#include <SDL.h>
#include <assert.h>


#define PFSCENE_VERSION (1.0)

#define STR2(val) #val
#define STR(val) STR2(val)
#define ARR_SIZE(a) (sizeof(a)/sizeof(a[0]))

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
    unsigned num_atts, ntags = 0;
    char tags[128][MAX_TAGS];

    khash_t(attr) *attr_table = kh_init(attr);
    if(!attr_table)
        goto fail_alloc;

    vec_attr_t constructor_args;
    vec_attr_init(&constructor_args);

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, " entity %127s %255s %u", name, path, &num_atts))
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

            if(attr.type != TYPE_INT)
                goto fail_parse;

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

        if(!strcmp(attr.key, "tags")) {

            if(attr.type != TYPE_INT)
                goto fail_parse;

            ntags = attr.val.as_int;
            for(int j = 0; j < ntags; j++) {

                READ_LINE(stream, line, fail_parse);
                if(!sscanf(line, " tag \"%127[^\"]", tags[j]))
                    goto fail_parse;
            }
        }
    }

    script_opaque_t obj;
    if(!(obj = S_Entity_ObjFromAtts(path, name, attr_table, &constructor_args)))
        goto fail_init;

    uint32_t uid;
    S_Entity_UIDForObj(obj, &uid);

    for(int i = 0; i < ntags; i++) {
        Entity_AddTag(uid, tags[i]);
    }

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

static bool scene_load_entities(SDL_RWops *stream)
{
    char line[MAX_LINE_LEN];
    unsigned num_ents;

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, " num_entities %u", &num_ents))
        goto fail_parse;

    for(int i = 0; i < num_ents; i++) {
        if(!scene_load_entity(stream))
            goto fail_parse;
        Sched_TryYield();
    }
    return true;

fail_parse:
    return false;
}

static bool scene_load_faction(SDL_RWops *stream)
{
    char line[MAX_LINE_LEN];
    char name[32];
    struct attr color;

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, " faction \"%31[^\"]", name))
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

static bool scene_load_factions(SDL_RWops *stream)
{
    char line[MAX_LINE_LEN];
    unsigned num_factions;

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, " num_factions %u", &num_factions))
        goto fail_parse;

    for(int i = 0; i < num_factions; i++) {
        if(!scene_load_faction(stream))
            goto fail_parse;
    }
    return true;

fail_parse:
    return false;
}

static bool scene_load_region(SDL_RWops *stream)
{
    char line[MAX_LINE_LEN];
    char name[128];
    unsigned type, num_atts;
    float radius = 0.0f, xlen = 0.0f, zlen = 0.0f;
    vec2_t pos = {0};

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, " region %127s %u %u", name, &type, &num_atts))
        goto fail_parse;

    for(int i = 0; i < num_atts; i++) {
        struct attr attr;
        if(!Attr_Parse(stream, &attr, true))
            goto fail_parse;

        if(0 == strcmp(attr.key, "radius")) {
            if(attr.type != TYPE_FLOAT)
                goto fail_parse;
            radius = attr.val.as_float;
        }else if(0 == strcmp(attr.key, "dimensions")) {
            if(attr.type != TYPE_VEC2)
                goto fail_parse;
            xlen = attr.val.as_vec2.x;
            zlen = attr.val.as_vec2.z;
        }else if(0 == strcmp(attr.key, "pos")) {
            if(attr.type != TYPE_VEC2)
                goto fail_parse;
            pos = attr.val.as_vec2;
        }else
            goto fail_parse;
    }

    if(!S_Region_ObjFromAtts(name, type, pos, radius, xlen, zlen))
        goto fail_parse;

    return true;

fail_parse:
    return false;
}

static bool scene_load_regions(SDL_RWops *stream)
{
    char line[MAX_LINE_LEN];
    unsigned num_regions;

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, " num_regions %u", &num_regions))
        goto fail_parse;

    for(int i = 0; i < num_regions; i++) {
        if(!scene_load_region(stream))
            goto fail_parse;
    }
    return true;

fail_parse:
    return false;
}

static bool scene_load_section(SDL_RWops *stream)
{
    char line[MAX_LINE_LEN];
    char name[MAX_LINE_LEN + 1];

    struct{
        const char *name;
        bool (*func)(SDL_RWops*);
    }map[] = {
        {"factions",     scene_load_factions},
        {"entities",     scene_load_entities},
        {"regions",      scene_load_regions }
    };

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, " section \"%" STR(MAX_LINE_LEN) "[^\"]", name))
        goto fail_parse;

    for(int i = 0; i < ARR_SIZE(map); i++) {
        if(0 == strcmp(map[i].name, name))
            return map[i].func(stream);
    }

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
    unsigned num_sections;
    float version;

    stream = SDL_RWFromFile(path, "r");
    if(!stream)
        goto fail_stream;

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, " version %f", &version))
        goto fail_parse;

    if(version > PFSCENE_VERSION)
        goto fail_parse;

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, " num_sections %u", &num_sections))
        goto fail_parse;

    for(int i = 0; i < num_sections; i++) {
        if(!scene_load_section(stream))
            goto fail_parse;
    }

    SDL_RWclose(stream);
    return true;
    
fail_parse:
    SDL_RWclose(stream);
fail_stream:
    return false;
}

