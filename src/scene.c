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

#define MEM_FILE_SYS MEM_SYS_SCENE
#define MEM_FILE_SUB 0

#include "scene.h"
#include "sched.h"
#include "asset_load.h"
#include "main.h"
#include "script/public/script.h"
#include "game/public/game.h"
#include "map/public/map.h"
#include "lib/public/pf_string.h"
#include "lib/public/attr.h"

#include <stdio.h>
#include <SDL.h>
#include <assert.h>

#include "mem.h"

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_SCENE, 0)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_SCENE, 0)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_SCENE, 0)


#define PFSCENE_VERSION_MAJOR (1)
#define PFSCENE_VERSION_MINOR (3)

#define VERSION_REACHED(_v, _major, _minor) \
    (((_v).major > _major) || ((_v).major == _major && (_v).minor >= _minor))

#define STR2(val) #val
#define STR(val) STR2(val)
#define ARR_SIZE(a) (sizeof(a)/sizeof(a[0]))

struct version{
    int major;
    int minor;
};

VEC_IMPL(extern, attr, struct attr)
__KHASH_IMPL(attr, extern, kh_cstr_t, struct attr, 1, kh_str_hash_func, kh_str_hash_equal)

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool scene_load_entity(struct version version, SDL_RWops *stream)
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
        PF_FREE(key);
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

static bool scene_load_entities(struct version version, SDL_RWops *stream)
{
    char line[MAX_LINE_LEN];
    unsigned num_ents;

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, " num_entities %u", &num_ents))
        goto fail_parse;

    for(int i = 0; i < num_ents; i++) {
        if(!scene_load_entity(version, stream))
            goto fail_parse;
        Sched_TryYield();
    }
    return true;

fail_parse:
    return false;
}

static bool scene_load_faction(struct version version, SDL_RWops *stream)
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

    int new_id;
    G_AddFaction(name, color.val.as_vec3, &new_id);

    if(VERSION_REACHED(version, 1, 1)) {

        struct attr controllable;
        if(!Attr_Parse(stream, &controllable, true))
            goto fail_parse;
        
        if(controllable.type != TYPE_BOOL)
            goto fail_parse;

        char names[MAX_FACTIONS][MAX_FAC_NAME_LEN];
        vec3_t colors[MAX_FACTIONS];
        bool ctrl[MAX_FACTIONS];

        int i = 0;
        uint16_t facs = G_GetFactions(names, colors, ctrl);
        for(uint16_t fcopy = facs; fcopy; fcopy >>= 1, i++) {
            if((fcopy & 0x1) && (i == new_id)) {
                G_UpdateFaction(i, names[i], colors[i], controllable.val.as_bool);
                break;
            }
        }
    }
    return true;

fail_parse:
    return false;
}

static bool scene_load_factions(struct version version, SDL_RWops *stream)
{
    char line[MAX_LINE_LEN];
    unsigned num_factions;

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, " num_factions %u", &num_factions))
        goto fail_parse;

    for(int i = 0; i < num_factions; i++) {
        if(!scene_load_faction(version, stream))
            goto fail_parse;
    }

    if(VERSION_REACHED(version, 1, 1)) {
        for(int i = 0; i < num_factions * num_factions; i++) {

            int faca, facb, state;
            READ_LINE(stream, line, fail_parse);
            if(!sscanf(line, " diplomacy %u %u %u", &faca, &facb, &state))
                goto fail_parse;
            if((state >= DIPLOMACY_STATE_MAX) || (faca >= MAX_FACTIONS) || (facb >= MAX_FACTIONS))
                goto fail_parse;
            if(faca == facb)
                continue;
            if(!G_SetDiplomacyState(faca, facb, state))
                goto fail_parse;
        }
    }

    return true;

fail_parse:
    return false;
}

static bool scene_load_region(struct version version, SDL_RWops *stream)
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

static bool scene_load_regions(struct version version, SDL_RWops *stream)
{
    char line[MAX_LINE_LEN];
    unsigned num_regions;

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, " num_regions %u", &num_regions))
        goto fail_parse;

    for(int i = 0; i < num_regions; i++) {
        if(!scene_load_region(version, stream))
            goto fail_parse;
    }
    return true;

fail_parse:
    return false;
}

static bool scene_load_decal(struct version version, SDL_RWops *stream)
{
    char line[MAX_LINE_LEN];
    unsigned num_atts;
    struct decal_desc desc = (struct decal_desc){
        .dims  = (vec2_t){ 1.0f, 1.0f },
        .alpha = 1.0f,
    };

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, " decal %255s %u", desc.path, &num_atts))
        goto fail_parse;

    for(int i = 0; i < num_atts; i++) {
        struct attr attr;
        if(!Attr_Parse(stream, &attr, true))
            goto fail_parse;

        if(0 == strcmp(attr.key, "pos")) {
            if(attr.type != TYPE_VEC2)
                goto fail_parse;
            desc.pos = attr.val.as_vec2;
        }else if(0 == strcmp(attr.key, "dimensions")) {
            if(attr.type != TYPE_VEC2)
                goto fail_parse;
            desc.dims = attr.val.as_vec2;
        }else if(0 == strcmp(attr.key, "rotation")) {
            if(attr.type != TYPE_FLOAT)
                goto fail_parse;
            desc.rotation = attr.val.as_float;
        }else if(0 == strcmp(attr.key, "alpha")) {
            if(attr.type != TYPE_FLOAT)
                goto fail_parse;
            desc.alpha = attr.val.as_float;
        }else if(0 == strcmp(attr.key, "hide_foliage")) {
            if(attr.type != TYPE_BOOL)
                goto fail_parse;
            desc.hide_foliage = attr.val.as_bool;
        }else
            goto fail_parse;
    }

    /* A missing image drops just that decal, not the whole scene */
    if(M_Decal_Add(&desc) == NULL_DECAL_ID) {
        fprintf(stderr, "Failed to add decal: %s\n", desc.path);
    }
    return true;

fail_parse:
    return false;
}

static bool scene_load_decals(struct version version, SDL_RWops *stream)
{
    char line[MAX_LINE_LEN];
    unsigned num_decals;

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, " num_decals %u", &num_decals))
        goto fail_parse;

    for(int i = 0; i < num_decals; i++) {
        if(!scene_load_decal(version, stream))
            goto fail_parse;
    }
    return true;

fail_parse:
    return false;
}

static bool scene_load_roads(struct version version, SDL_RWops *stream)
{
    char line[MAX_LINE_LEN];
    unsigned num_roads;

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, " num_roads %u", &num_roads))
        goto fail_parse;

    struct road_desc *roads = PF_MALLOC(sizeof(struct road_desc) * num_roads);
    uint8_t *links = PF_MALLOC(sizeof(uint8_t) * num_roads);
    if(num_roads > 0 && (!roads || !links))
        goto fail_alloc;

    for(int i = 0; i < num_roads; i++) {
        unsigned mask;
        READ_LINE(stream, line, fail_read);
        if(3 != sscanf(line, " road %d %d %u", &roads[i].r, &roads[i].c, &mask))
            goto fail_read;
        links[i] = mask;
    }

    /* Links are made only once every tile is in, and only between tiles that 
     * are both present, so a hand-edited file cannot break the road layout.
     */
    for(int i = 0; i < num_roads; i++) {
        G_Roads_Add(roads[i]);
    }
    for(int i = 0; i < num_roads; i++) {
        struct road_desc rd = roads[i];
        if(links[i] & ROAD_LINK_UP)
            G_Roads_Link(rd, (struct road_desc){rd.r - 1, rd.c});
        if(links[i] & ROAD_LINK_RIGHT)
            G_Roads_Link(rd, (struct road_desc){rd.r, rd.c + 1});
        if(links[i] & ROAD_LINK_DOWN)
            G_Roads_Link(rd, (struct road_desc){rd.r + 1, rd.c});
        if(links[i] & ROAD_LINK_LEFT)
            G_Roads_Link(rd, (struct road_desc){rd.r, rd.c - 1});
    }

    PF_FREE(roads);
    PF_FREE(links);
    return true;

fail_read:
fail_alloc:
    PF_FREE(roads);
    PF_FREE(links);
fail_parse:
    return false;
}

static bool scene_load_general(struct version version, SDL_RWops *stream)
{
    char line[MAX_LINE_LEN];
    char skybox_dir[256], skybox_ext[256];

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, " skybox \"%255[^\"]\" %255s", skybox_dir, skybox_ext))
        goto fail_parse;

    G_SetSkybox(skybox_dir, skybox_ext);

    int r, g, b, a;
    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, " ambient_light_color %d %d %d %d", &r, &g, &b, &a))
        goto fail_parse;

    vec3_t ambient = (vec3_t){
        r / 255.0f,
        g / 255.0f,
        b / 255.0f
    };
    G_SetAmbientLightColor(ambient);

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, " emit_light_color %d %d %d %d", &r, &g, &b, &a))
        goto fail_parse;

    vec3_t emit = (vec3_t){
        r / 255.0f,
        g / 255.0f,
        b / 255.0f
    };
    G_SetEmitLightColor(emit);

    vec3_t pos;
    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, " emit_light_pos %f %f %f", &pos.x, &pos.y, &pos.z))
        goto fail_parse;

    G_SetLightPos(pos);
    return true;

fail_parse:
    return false;
}

static bool scene_load_camera(struct version version, SDL_RWops *stream)
{
    char line[MAX_LINE_LEN];
    char name[128];
    struct attr attr;

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, " camera %127s", name))
        goto fail_parse;

    if(!Attr_Parse(stream, &attr, true))
        goto fail_parse;
    if(attr.type != TYPE_VEC3)
        goto fail_parse;
    vec3_t pos = attr.val.as_vec3;

    if(!Attr_Parse(stream, &attr, true))
        goto fail_parse;
    if(attr.type != TYPE_FLOAT)
        goto fail_parse;
    float pitch = attr.val.as_float;

    if(!Attr_Parse(stream, &attr, true))
        goto fail_parse;
    if(attr.type != TYPE_FLOAT)
        goto fail_parse;
    float yaw = attr.val.as_float;

    if(!S_Camera_ObjFromAtts(name, pos, pitch, yaw))
        goto fail_parse;

    return true;

fail_parse:
    return false;
}

static bool scene_load_cameras(struct version version, SDL_RWops *stream)
{
    char line[MAX_LINE_LEN];
    unsigned num_cameras;

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, " num_cameras %u", &num_cameras))
        goto fail_parse;

    for(int i = 0; i < num_cameras; i++) {
        if(!scene_load_camera(version, stream))
            goto fail_parse;
    }
    return true;

fail_parse:
    return false;
}

static bool scene_load_section(struct version version, SDL_RWops *stream)
{
    char line[MAX_LINE_LEN];
    char name[MAX_LINE_LEN + 1];

    struct{
        const char *name;
        bool (*func)(struct version, SDL_RWops*);
    }map[] = {
        {"general",      scene_load_general},
        {"factions",     scene_load_factions},
        {"entities",     scene_load_entities},
        {"regions",      scene_load_regions },
        {"cameras",      scene_load_cameras},
        {"decals",       scene_load_decals},
        {"roads",        scene_load_roads}
    };

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, " section \"%" STR(MAX_LINE_LEN) "[^\"]", name))
        goto fail_parse;

    for(int i = 0; i < ARR_SIZE(map); i++) {
        if(0 == strcmp(map[i].name, name))
            return map[i].func(version, stream);
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
    int version_major, version_minor;

    stream = SDL_RWFromFile(path, "r");
    if(!stream)
        goto fail_stream;

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, " version %d.%d", &version_major, &version_minor))
        goto fail_parse;

    if((version_major > PFSCENE_VERSION_MAJOR)
    || (version_major == PFSCENE_VERSION_MAJOR && version_minor > PFSCENE_VERSION_MINOR))
        goto fail_parse;

    READ_LINE(stream, line, fail_parse);
    if(!sscanf(line, " num_sections %u", &num_sections))
        goto fail_parse;

    for(int i = 0; i < num_sections; i++) {
        if(!scene_load_section((struct version){version_major, version_minor}, stream))
            goto fail_parse;
    }

    SDL_RWclose(stream);
    return true;
    
fail_parse:
    SDL_RWclose(stream);
fail_stream:
    return false;
}

