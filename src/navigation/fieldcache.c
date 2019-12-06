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

#include "fieldcache.h"
#include "../lib/public/lru_cache.h"
#include "../lib/public/khash.h"
#include "../lib/public/vec.h"
#include "../event.h"
#include "../config.h"

#include <assert.h>


LRU_CACHE_TYPE(los, struct LOS_field)
LRU_CACHE_PROTOTYPES(static, los, struct LOS_field)
LRU_CACHE_IMPL(static, los, struct LOS_field)

LRU_CACHE_TYPE(flow, struct flow_field)
LRU_CACHE_PROTOTYPES(static, flow, struct flow_field)
LRU_CACHE_IMPL(static, flow, struct flow_field)

LRU_CACHE_TYPE(ffid, ff_id_t)
LRU_CACHE_PROTOTYPES(static, ffid, ff_id_t)
LRU_CACHE_IMPL(static, ffid, ff_id_t)

LRU_CACHE_TYPE(grid_path, struct grid_path_desc)
LRU_CACHE_PROTOTYPES(static, grid_path, struct grid_path_desc)
LRU_CACHE_IMPL(static, grid_path, struct grid_path_desc)

VEC_TYPE(id, uint64_t)
VEC_PROTOTYPES(static, id, uint64_t)
VEC_IMPL(static, id, uint64_t)

KHASH_MAP_INIT_INT64(idvec, vec_id_t)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static lru(los)          s_los_cache;       /* key: (dest_id, chunk coord) */
static lru(flow)         s_flow_cache;      /* key: (ffid) */
/* The ffid cache maps a (dest_id, chunk coordinate) tuple to a flow field ID,
 * which could be used to retreive the relevant field from the flow cache. 
 * The reason for this is that the same flow field chunk can be shared between
 * many different paths. */
static lru(ffid)         s_ffid_cache;      /* key: (dest_id, chunk_coord) */
static lru(grid_path)    s_grid_path_cache; /* key: (chunk coord, tile start coord, tile dest coord) */

/* The following structures are maintained for efficient invalidation of entries:*/
static vec_id_t          s_enemy_seek_ffids;
static khash_t(idvec)   *s_chunk_ffield_map; /* key: (chunk coord) */
static khash_t(idvec)   *s_chunk_lfield_map; /* key: (chunk coord) */

static struct priv_fc_stats{
    unsigned los_query;
    unsigned los_hit;
    unsigned los_invalidated;
    unsigned flow_query;
    unsigned flow_hit;
    unsigned flow_invalidated;
    unsigned ffid_query;
    unsigned ffid_hit;
    unsigned grid_path_query;
    unsigned grid_path_hit;
}s_perfstats = {0};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

uint32_t key_for_chunk(struct coord chunk)
{
    return (((uint64_t)chunk.r & 0xffff) << 16) | ((uint64_t)chunk.c & 0xffff);
}

uint64_t key_for_dest_and_chunk(dest_id_t id, struct coord chunk)
{
    return ((((uint64_t)id) << 32) | key_for_chunk(chunk));
}

uint64_t grid_path_key(struct coord local_start, struct coord local_dest,
                       struct coord chunk)
{
    return ((( ((uint64_t)local_start.r) & 0xff  ) <<  0)
         |  (( ((uint64_t)local_start.c) & 0xff  ) <<  8)
         |  (( ((uint64_t)local_dest.r)  & 0xff  ) << 16)
         |  (( ((uint64_t)local_dest.c)  & 0xff  ) << 24)
         |  (( ((uint64_t)chunk.r)       & 0xffff) << 32)
         |  (( ((uint64_t)chunk.c)       & 0xffff) << 48));
}

static void on_grid_path_evict(struct grid_path_desc *victim)
{
    vec_coord_destroy(&victim->path);
}

static void destroy_all_entries(khash_t(idvec) *hash)
{
    uint32_t key;
    vec_id_t curr;
    kh_foreach(hash, key, curr, {
        vec_id_destroy(&curr); 
    });
}

static void field_map_add(khash_t(idvec) *hash, uint64_t key, uint64_t id)
{
    khiter_t k = kh_get(idvec, hash, key);
    if(k != kh_end(hash)) {

        vec_id_t *curr = &kh_val(hash, k);
        vec_id_push(curr, id);
    }else{
        vec_id_t newvec;
        vec_id_init(&newvec);
        vec_id_push(&newvec, id);

        int ret;
        kh_put(idvec, hash, key, &ret);
        assert(ret != -1 && ret != 0);
        k = kh_get(idvec, hash, key);
        kh_val(hash, k) = newvec;
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool N_FC_Init(void)
{
    if(!lru_los_init(&s_los_cache, CONFIG_LOS_CACHE_SZ, NULL))
        goto fail_los;

    if(!lru_flow_init(&s_flow_cache, CONFIG_FLOW_CAHCE_SZ, NULL))
        goto fail_flow;

    if(!lru_ffid_init(&s_ffid_cache, CONFIG_MAPPING_CACHE_SZ, NULL))
        goto fail_ffid;

    if(!lru_grid_path_init(&s_grid_path_cache, CONFIG_GRID_PATH_CACHE_SZ, on_grid_path_evict))
        goto fail_grid_path;

    if(NULL == (s_chunk_ffield_map = kh_init(idvec)))
        goto fail_chunk_ffield;

    if(NULL == (s_chunk_lfield_map = kh_init(idvec)))
        goto fail_chunk_lfield;

    vec_id_init(&s_enemy_seek_ffids);
    return true;

fail_chunk_lfield:
    kh_destroy(idvec, s_chunk_ffield_map);
fail_chunk_ffield:
    lru_grid_path_destroy(&s_grid_path_cache);
fail_grid_path:
    lru_ffid_destroy(&s_ffid_cache);
fail_ffid:
    lru_flow_destroy(&s_flow_cache);
fail_flow:
    lru_los_destroy(&s_los_cache);
fail_los:
    return false;
}

void N_FC_Shutdown(void)
{
    lru_los_destroy(&s_los_cache);
    lru_flow_destroy(&s_flow_cache);
    lru_ffid_destroy(&s_ffid_cache);
    lru_grid_path_destroy(&s_grid_path_cache);

    destroy_all_entries(s_chunk_ffield_map);
    kh_destroy(idvec, s_chunk_ffield_map);

    destroy_all_entries(s_chunk_lfield_map);
    kh_destroy(idvec, s_chunk_lfield_map);

    vec_id_destroy(&s_enemy_seek_ffids);
}

void N_FC_ClearAll(void)
{
    lru_los_clear(&s_los_cache);
    lru_flow_clear(&s_flow_cache);
    lru_ffid_clear(&s_ffid_cache);
    lru_grid_path_clear(&s_grid_path_cache);

    destroy_all_entries(s_chunk_ffield_map);
    kh_clear(idvec, s_chunk_ffield_map);

    destroy_all_entries(s_chunk_lfield_map);
    kh_clear(idvec, s_chunk_lfield_map);

    vec_id_reset(&s_enemy_seek_ffids);
}

void N_FC_ClearStats(void)
{
    memset(&s_perfstats, 0, sizeof(s_perfstats));
}

void N_FC_GetStats(struct fc_stats *out_stats)
{
    out_stats->los_used = s_los_cache.used;
    out_stats->los_max = s_los_cache.capacity;
    out_stats->los_hit_rate = !s_perfstats.los_query ? 0
        : ((float)s_perfstats.los_hit) / s_perfstats.los_query;
    out_stats->los_invalidated = s_perfstats.los_invalidated;

    out_stats->flow_used = s_flow_cache.used;
    out_stats->flow_max = s_flow_cache.capacity;
    out_stats->flow_hit_rate = !s_perfstats.flow_query ? 0
        : ((float)s_perfstats.flow_hit) / s_perfstats.flow_query;
    out_stats->flow_invalidated = s_perfstats.flow_invalidated;

    out_stats->ffid_used = s_ffid_cache.used;
    out_stats->ffid_max = s_ffid_cache.capacity;
    out_stats->ffid_hit_rate = !s_perfstats.ffid_hit ? 0
        : ((float)s_perfstats.ffid_hit) / s_perfstats.ffid_query;

    out_stats->grid_path_used = s_grid_path_cache.used;
    out_stats->grid_path_max = s_grid_path_cache.capacity;
    out_stats->grid_path_hit_rate = !s_perfstats.grid_path_hit ? 0
        : ((float)s_perfstats.grid_path_hit) / s_perfstats.grid_path_query;
}

bool N_FC_ContainsLOSField(dest_id_t id, struct coord chunk_coord)
{
    uint64_t key = key_for_dest_and_chunk(id, chunk_coord);
    bool ret = lru_los_contains(&s_los_cache, key);

    s_perfstats.los_query++;
    s_perfstats.los_hit += !!ret;
    return ret;
}

const struct LOS_field *N_FC_LOSFieldAt(dest_id_t id, struct coord chunk_coord)
{
    uint64_t key = key_for_dest_and_chunk(id, chunk_coord);
    return lru_los_at(&s_los_cache, key);
}

void N_FC_PutLOSField(dest_id_t id, struct coord chunk_coord, const struct LOS_field *lf)
{
    uint64_t key = key_for_dest_and_chunk(id, chunk_coord);
    lru_los_put(&s_los_cache, key, lf);
    field_map_add(s_chunk_lfield_map, key_for_chunk(chunk_coord), key);
}

bool N_FC_ContainsFlowField(ff_id_t ffid)
{
    bool ret = lru_flow_contains(&s_flow_cache, ffid);

    s_perfstats.flow_query++;
    s_perfstats.flow_hit += !!ret;
    return ret;
}

const struct flow_field *N_FC_FlowFieldAt(ff_id_t ffid)
{
    return lru_flow_at(&s_flow_cache, ffid);
}

void N_FC_PutFlowField(ff_id_t ffid, const struct flow_field *ff)
{
    if((ffid >> 56) == TARGET_ENEMIES)
        vec_id_push(&s_enemy_seek_ffids, ffid);

    lru_flow_put(&s_flow_cache, ffid, ff);

    struct coord chunk = (struct coord){(ffid >> 8) & 0xff, ffid & 0xff};
    field_map_add(s_chunk_ffield_map, key_for_chunk(chunk), ffid);
}

bool N_FC_GetDestFFMapping(dest_id_t id, struct coord chunk_coord, ff_id_t *out_ff)
{
    uint64_t key = key_for_dest_and_chunk(id, chunk_coord);
    bool ret = lru_ffid_get(&s_ffid_cache, key, out_ff);

    s_perfstats.ffid_query++;
    s_perfstats.ffid_hit += !!ret;
    return ret;
}

void N_FC_PutDestFFMapping(dest_id_t dest_id, struct coord chunk_coord, ff_id_t ffid)
{
    uint64_t key = key_for_dest_and_chunk(dest_id, chunk_coord);
    lru_ffid_put(&s_ffid_cache, key, &ffid);
}

void N_FC_ClearAllEnemySeekFields(void)
{
    for(int i = 0; i < vec_size(&s_enemy_seek_ffids); i++) {

        ff_id_t curr = vec_AT(&s_enemy_seek_ffids, i);
        /* The field may have already been invalidated */
        lru_flow_remove(&s_flow_cache, curr);
    }
    vec_id_reset(&s_enemy_seek_ffids);
}

bool N_FC_GetGridPath(struct coord local_start, struct coord local_dest,
                      struct coord chunk, struct grid_path_desc *out)
{
    uint64_t key = grid_path_key(local_start, local_dest, chunk);
    bool ret = lru_grid_path_get(&s_grid_path_cache, key, out);

    s_perfstats.grid_path_query++;
    s_perfstats.grid_path_hit += !!ret;
    return ret;
}

void N_FC_PutGridPath(struct coord local_start, struct coord local_dest,
                      struct coord chunk, const struct grid_path_desc *in)
{
    uint64_t key = grid_path_key(local_start, local_dest, chunk);
    lru_grid_path_put(&s_grid_path_cache, key, in);
}

void N_FC_InvalidateAllAtChunk(struct coord chunk)
{
    /* Note that chunk:field maps simply maintain a list of cache keys for 
     * which entries were set. The entries for these keys may have already 
     * been evicted. As well, keys for which data has been overwritten may
     * appear multiple times. So, not all the fields in the lists will
     * necessarily be in the caches. */

    uint64_t key = key_for_chunk(chunk);
    extern unsigned long long g_frame_idx;

    khiter_t k = kh_get(idvec, s_chunk_lfield_map, key);
    if(k != kh_end(s_chunk_lfield_map)) {

        vec_id_t *keys = &kh_val(s_chunk_lfield_map, k);
        for(int i = 0; i < vec_size(keys); i++) {
            bool found = lru_los_remove(&s_los_cache, vec_AT(keys, i));
            s_perfstats.los_invalidated += !!found;
        }
        vec_id_destroy(keys);
        kh_del(idvec, s_chunk_lfield_map, k);
    }

    k = kh_get(idvec, s_chunk_ffield_map, key);
    if(k != kh_end(s_chunk_ffield_map)) {

        vec_id_t *keys = &kh_val(s_chunk_ffield_map, k);
        for(int i = 0; i < vec_size(keys); i++) {
            bool found = lru_flow_remove(&s_flow_cache, vec_AT(keys, i));
            s_perfstats.flow_invalidated += !!found;
        }
        vec_id_destroy(keys);
        kh_del(idvec, s_chunk_ffield_map, k);
    }
}

