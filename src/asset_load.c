/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2023 Eduard Permyakov 
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
#include "main.h"

#include "render/public/render_al.h"
#include "anim/public/anim.h"
#include "game/public/game.h"
#include "lib/public/attr.h"
#include "map/public/map.h"
#include "lib/public/khash.h"
#include "lib/public/pf_string.h"
#include "lib/public/mpool_allocator.h"
#include "lib/public/mem.h"

#include <SDL.h>

#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h> 


#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

struct shared_resource{
    uint32_t     ent_flags;
    void        *render_private;
    void        *anim_private;
    const char  *basedir;
    const char  *filename;
    struct aabb  aabb;
};

KHASH_MAP_INIT_STR(entity_res, struct shared_resource)
KHASH_MAP_INIT_INT(uid_ent, struct entity*)

MPOOL_ALLOCATOR_TYPE(ent, struct entity)
MPOOL_ALLOCATOR_PROTOTYPES(static, ent, struct entity)
MPOOL_ALLOCATOR_IMPL(static, ent, struct entity)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static khash_t(entity_res) *s_name_resource_table;
static khash_t(uid_ent)    *s_uid_ent_table;
static mpa_ent_t            s_mpool;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

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

static bool al_get_resource(const char *path, const char *basedir, 
                            const char *pfobj_name, struct shared_resource *out)
{
    SDL_RWops *stream;
    struct pfobj_hdr header;

    khiter_t k = kh_get(entity_res, s_name_resource_table, path);
    if(k != kh_end(s_name_resource_table)) {

        *out = kh_value(s_name_resource_table, k);
        return true;
    }

    stream = SDL_RWFromFile(path, "r");
    if(!stream)
        goto fail_init; 

    if(!al_parse_pfobj_header(stream, &header))
        goto fail_parse;

    char abs_basedir[512];
    pf_snprintf(abs_basedir, sizeof(abs_basedir), "%s/%s", g_basepath, basedir);

    out->ent_flags = 0;
    out->render_private = R_AL_PrivFromStream(abs_basedir, &header, stream);
    if(!out->render_private)
        goto fail_parse;

    out->anim_private = A_AL_PrivFromStream(&header, stream);
    if(!out->anim_private)
        goto fail_parse;

    if(header.num_as > 0) {
        out->ent_flags |= ENTITY_FLAG_ANIMATED;
    }

    if(!header.has_collision) {
        fprintf(stderr, "Imported entities required to have bounding boxes.\n");
        goto fail_parse;
    }

    if(!AL_ParseAABB(stream, &out->aabb))
        goto fail_parse;

    out->basedir = pf_strdup(basedir);
    out->filename = pf_strdup(pfobj_name);

    int put_ret;
    k = kh_put(entity_res, s_name_resource_table, pf_strdup(path), &put_ret);
    assert(put_ret != -1 && put_ret != 0);
    kh_value(s_name_resource_table, k) = *out;

    SDL_RWclose(stream);
    return true;

fail_parse:
    SDL_RWclose(stream);
fail_init:
    return false;
}

static void al_save_mapping(uint32_t uid, struct entity *ent)
{
    int ret;
    khiter_t k = kh_put(uid_ent, s_uid_ent_table, uid, &ret);
    assert(ret != -1);
    kh_value(s_uid_ent_table, k) = ent;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/


bool AL_EntityFromPFObj(const char *base_path, const char *pfobj_name, 
                        const char *name, uint32_t uid, uint32_t *out_flags)
{
    struct shared_resource res;
    char pfobj_path[512];
    pf_snprintf(pfobj_path, sizeof(pfobj_path), "%s/%s/%s", g_basepath, base_path, pfobj_name);

    struct entity *newent = mpa_ent_alloc(&s_mpool);
    if(!newent)
        goto fail_alloc;

    newent->name = pf_strdup(name);
    newent->filename = pf_strdup(pfobj_name);
    newent->basedir = pf_strdup(base_path);

    if(!newent->name || !newent->filename || !newent->basedir)
        goto fail_init;

    if(!al_get_resource(pfobj_path, base_path, pfobj_name, &res))
        goto fail_init;

    newent->render_private = res.render_private;
    newent->anim_private = res.anim_private;
    newent->identity_aabb = res.aabb;

    Entity_SetRot(uid, (quat_t){0.0f, 0.0f, 0.0f, 1.0f});
    Entity_SetScale(uid, (vec3_t){1.0f, 1.0f, 1.0f});
    *out_flags = res.ent_flags;

    al_save_mapping(uid, newent);
    return true;

fail_init:
    PF_FREE(newent->basedir);
    PF_FREE(newent->filename);
    PF_FREE(newent->name);
    mpa_ent_free(&s_mpool, newent);
fail_alloc:
    return false;
}

struct entity *AL_EntityGet(uint32_t uid)
{
    khiter_t k = kh_get(uid_ent, s_uid_ent_table, uid);
    if(k == kh_end(s_uid_ent_table))
        return NULL;

    return kh_value(s_uid_ent_table, k);
}

bool AL_EntitySetPFObj(uint32_t uid, const char *base_path, const char *pfobj_name)
{
    struct shared_resource old_res, new_res;
    char old_pfobj_path[512], new_pfobj_path[512];

    struct entity *ent = AL_EntityGet(uid);
    if(!ent)
        return false;

    pf_snprintf(old_pfobj_path, sizeof(old_pfobj_path), "%s/%s/%s", 
        g_basepath, ent->basedir, ent->filename);
    pf_snprintf(new_pfobj_path, sizeof(new_pfobj_path), "%s/%s/%s", 
        g_basepath, base_path, pfobj_name);

    if(!al_get_resource(old_pfobj_path, ent->basedir, ent->filename, &old_res))
        goto fail_init;
    if(!al_get_resource(new_pfobj_path, base_path, pfobj_name, &new_res))
        goto fail_init;

    const char *newdir = pf_strdup(base_path);
    const char *newobj = pf_strdup(pfobj_name);

    if(!newdir || !newobj)
        goto fail_alloc;

    if(G_FlagsGet(uid) & ENTITY_FLAG_ANIMATED) {
        A_RemoveEntity(uid);
    }
    uint32_t flags = G_FlagsGet(uid);
    flags &= ~old_res.ent_flags;

    PF_FREE(ent->basedir);
    PF_FREE(ent->filename);
    ent->basedir = newdir;
    ent->filename = newobj;

    ent->render_private = new_res.render_private;
    ent->anim_private = new_res.anim_private;
    ent->identity_aabb = new_res.aabb;

    flags |= new_res.ent_flags;
    if(flags & ENTITY_FLAG_ANIMATED) {
        A_AddEntity(uid);
    }
    G_FlagsSet(uid, flags);
    return true;

fail_alloc:
    PF_FREE(newdir);
    PF_FREE(newobj);
fail_init:
    return false;
}

void AL_EntityFree(uint32_t uid)
{
    struct entity *entity = AL_EntityGet(uid);

    PF_FREE(entity->basedir);
    PF_FREE(entity->filename);
    PF_FREE(entity->name);

    khiter_t k = kh_get(uid_ent, s_uid_ent_table, uid);
    assert(k != kh_end(s_uid_ent_table));
    kh_del(uid_ent, s_uid_ent_table, k);
    mpa_ent_free(&s_mpool, entity);
}

void AL_ClearState(void)
{
    kh_clear(uid_ent, s_uid_ent_table);
    mpa_ent_clear(&s_mpool);
}

void *AL_RenderPrivateForName(const char *base_path, const char *pfobj_name)
{
    struct shared_resource res;
    char pfobj_path[512];
    pf_snprintf(pfobj_path, sizeof(pfobj_path), "%s/%s/%s", g_basepath, base_path, pfobj_name);

    if(!al_get_resource(pfobj_path, base_path, pfobj_name, &res))
        return NULL;

    return res.render_private;
}

bool AL_NameForRenderPrivate(void *render_private, char out_dir[], 
                             char out_name[])
{
    struct shared_resource curr;
    bool found = false;

    kh_foreach(s_name_resource_table, (const char*){NULL}, curr, {
        if(curr.render_private == render_private) {
            found = true;
            break;
        }
    });

    if(!found)
        return false;

    pf_strlcpy(out_dir, curr.basedir, 512);
    pf_strlcpy(out_name, curr.filename, 512);
    return true;
}

bool AL_PreloadPFObj(const char *base_path, const char *pfobj_name)
{
    struct shared_resource res;
    char pfobj_path[512];
    pf_snprintf(pfobj_path, sizeof(pfobj_path), "%s/%s/%s", g_basepath, base_path, pfobj_name);

    if(!al_get_resource(pfobj_path, base_path, pfobj_name, &res))
        return false;

    return true;
}

struct map *AL_MapFromPFMapStream(SDL_RWops *stream, bool update_navgrid)
{
    struct map *ret;
    struct pfmap_hdr header;

    if(!al_parse_pfmap_header(stream, &header))
        goto fail_parse;

    ret = malloc(M_AL_BuffSizeFromHeader(&header));
    if(!ret)
        goto fail_alloc;

    if(!M_AL_InitMapFromStream(&header, g_basepath, stream, ret, update_navgrid))
        goto fail_init;

    return ret;

fail_init:
    PF_FREE(ret);
fail_alloc:
fail_parse:
    return NULL;
}

size_t AL_MapShallowCopySize(SDL_RWops *stream)
{
    struct pfmap_hdr header;
    size_t ret = 0;
    size_t pos = SDL_RWseek(stream, 0, RW_SEEK_CUR);

    if(!al_parse_pfmap_header(stream, &header))
        goto fail_parse;

    ret = M_AL_ShallowCopySize(header.num_rows, header.num_cols);

fail_parse:
    SDL_RWseek(stream, pos, RW_SEEK_SET);
    return ret;
}

void AL_MapFree(struct map *map)
{
    M_AL_FreePrivate(map);
    PF_FREE(map);
}

bool AL_ReadLine(SDL_RWops *stream, char *outbuff)
{
    int idx = 0;
    do { 
        if(!SDL_RWread(stream, outbuff + idx, 1, 1))
            return false; 

        if(outbuff[idx] == '\n') {
            /* nuke the carriage return before the newline - to give a consistent 
             * output to client code regardless of platform */
            if(idx && outbuff[idx-1] == '\r') {
                outbuff[idx-1] = '\n';
                outbuff[idx] = '\0';
            }
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
    if(!s_name_resource_table)
        goto fail_name_res_table;

    s_uid_ent_table = kh_init(uid_ent);
    if(!s_uid_ent_table)
        goto fail_uid_ent_table;

    mpa_ent_init(&s_mpool, 1024, 1024);
    if(!mpa_ent_reserve(&s_mpool, 1024))
        goto fail_mpool;

    return true;

fail_mpool:
    kh_destroy(uid_ent, s_uid_ent_table);
fail_uid_ent_table:
    kh_destroy(entity_res, s_name_resource_table);
fail_name_res_table:
    return false;
}

void AL_Shutdown(void)
{
    const char *key;
    struct shared_resource curr;

    kh_foreach(s_name_resource_table, key, curr, {
        PF_FREE(key);
        PF_FREE(curr.render_private);
        PF_FREE(curr.anim_private);
        PF_FREE(curr.basedir);
        PF_FREE(curr.filename);
    });
    kh_destroy(uid_ent, s_uid_ent_table);
    kh_destroy(entity_res, s_name_resource_table);
    mpa_ent_destroy(&s_mpool);
}

bool AL_SaveOBB(SDL_RWops *stream, const struct obb *obb)
{
    struct attr curr;

    curr = (struct attr){.type = TYPE_VEC3, .val.as_vec3 = obb->center };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_center"));

    curr = (struct attr){.type = TYPE_VEC3, .val.as_vec3 = obb->axes[0] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_x_axis"));
    curr = (struct attr){.type = TYPE_VEC3, .val.as_vec3 = obb->axes[1] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_y_axis"));
    curr = (struct attr){.type = TYPE_VEC3, .val.as_vec3 = obb->axes[2] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_z_axis"));

    curr = (struct attr){.type = TYPE_FLOAT, .val.as_float = obb->half_lengths[0] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_x_half_len"));
    curr = (struct attr){.type = TYPE_FLOAT, .val.as_float = obb->half_lengths[1] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_y_half_len"));
    curr = (struct attr){.type = TYPE_FLOAT, .val.as_float = obb->half_lengths[2] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_z_half_len"));

    curr = (struct attr){.type = TYPE_VEC3, .val.as_vec3 = obb->corners[0] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_0_corner"));
    curr = (struct attr){.type = TYPE_VEC3, .val.as_vec3 = obb->corners[1] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_1_corner"));
    curr = (struct attr){.type = TYPE_VEC3, .val.as_vec3 = obb->corners[2] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_2_corner"));
    curr = (struct attr){.type = TYPE_VEC3, .val.as_vec3 = obb->corners[3] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_3_corner"));
    curr = (struct attr){.type = TYPE_VEC3, .val.as_vec3 = obb->corners[4] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_4_corner"));
    curr = (struct attr){.type = TYPE_VEC3, .val.as_vec3 = obb->corners[5] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_5_corner"));
    curr = (struct attr){.type = TYPE_VEC3, .val.as_vec3 = obb->corners[6] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_6_corner"));
    curr = (struct attr){.type = TYPE_VEC3, .val.as_vec3 = obb->corners[7] };
    CHK_TRUE_RET(Attr_Write(stream, &curr, "obb_7_corner"));

    return true;
}

bool AL_LoadOBB(SDL_RWops *stream, struct obb *out)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC3);
    out->center = attr.val.as_vec3;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC3);
    out->axes[0] = attr.val.as_vec3;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC3);
    out->axes[1] = attr.val.as_vec3;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC3);
    out->axes[2] = attr.val.as_vec3;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_FLOAT);
    out->half_lengths[0] = attr.val.as_float;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_FLOAT);
    out->half_lengths[1] = attr.val.as_float;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_FLOAT);
    out->half_lengths[2] = attr.val.as_float;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC3);
    out->corners[0] = attr.val.as_vec3;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC3);
    out->corners[1] = attr.val.as_vec3;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC3);
    out->corners[2] = attr.val.as_vec3;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC3);
    out->corners[3] = attr.val.as_vec3;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC3);
    out->corners[4] = attr.val.as_vec3;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC3);
    out->corners[5] = attr.val.as_vec3;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC3);
    out->corners[6] = attr.val.as_vec3;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC3);
    out->corners[7] = attr.val.as_vec3;

    return true;
}

