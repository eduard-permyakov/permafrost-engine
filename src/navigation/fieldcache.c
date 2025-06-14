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

#include "fieldcache.h"
#include "nav_private.h"
#include "../lib/public/lru_cache.h"
#include "../lib/public/khash.h"
#include "../lib/public/vec.h"
#include "../lib/public/mem.h"
#include "../event.h"
#include "../sched.h"
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

struct priv_fc_stats{
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
};

struct fieldcache_ctx{
    lru(los)          los_cache;       /* key: (dest_id, chunk coord) */
    lru(flow)         flow_cache;      /* key: (ffid) */
    /* The ffid cache maps a (dest_id, chunk coordinate) tuple to a flow field ID,
     * which could be used to retreive the relevant field from the flow cache. 
     * The reason for this is that the same flow field chunk can be shared between
     * many different paths. */
    lru(ffid)         ffid_cache;      /* key: (dest_id, chunk_coord) */
    lru(grid_path)    grid_path_cache; /* key: (chunk coord, tile start coord, tile dest coord) */
    
    /* The following structures are maintained for efficient invalidation of entries:*/
    khash_t(idvec)   *chunk_ffield_map; /* key: (chunk coord) */
    khash_t(idvec)   *chunk_lfield_map; /* key: (chunk coord) */

    /* Statistics */
    struct priv_fc_stats perfstats;
};

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

dest_id_t key_dest(uint64_t key)
{
    return (key >> 32);
}

struct coord key_chunk(uint64_t key)
{
    return (struct coord){
        (key >> 16) & 0xffff,
        (key >>  0) & 0xffff,
    };
}

uint64_t grid_path_key(struct coord local_start, struct coord local_dest,
                       struct coord chunk, enum nav_layer layer)
{
    return ((( ((uint64_t)local_start.r) & 0xff  ) <<  0)
         |  (( ((uint64_t)local_start.c) & 0xff  ) <<  8)
         |  (( ((uint64_t)local_dest.r)  & 0xff  ) << 16)
         |  (( ((uint64_t)local_dest.c)  & 0xff  ) << 24)
         |  (( ((uint64_t)chunk.r)       & 0x3fff) << 32)
         |  (( ((uint64_t)chunk.c)       & 0x3fff) << 46)
         |  (( ((uint64_t)layer)         & 0xf   ) << 60));
}

static void on_grid_path_evict(struct grid_path_desc *victim)
{
    vec_coord_destroy(&victim->path);
}

static void destroy_all_entries(khash_t(idvec) *hash)
{
    uint32_t key;
    vec_id_t curr;
    (void)key;

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

static bool dest_array_contains(dest_id_t *array, size_t size, dest_id_t item)
{
    for(int i = 0; i < size; i++) {
        if(array[i] == item)
            return true;
    }
    return false;
}

static void clear_chunk_los_map(struct fieldcache_ctx *ctx, uint64_t key, enum nav_layer layer)
{
    khiter_t k = kh_get(idvec, ctx->chunk_lfield_map, key);
    if(k == kh_end(ctx->chunk_lfield_map))
        return;

    vec_id_t *keys = &kh_val(ctx->chunk_lfield_map, k);
    for(int i = vec_size(keys)-1; i >= 0; i--) {

        uint64_t key = vec_AT(keys, i);
        if(N_DestLayer(key_dest(key)) != layer)
            continue;

        bool found = lru_los_remove(&ctx->los_cache, key);
        ctx->perfstats.los_invalidated += !!found;
        vec_id_del(keys, i);
    }
    if(vec_size(keys) == 0) {
        vec_id_destroy(keys);
        kh_del(idvec, ctx->chunk_lfield_map, k);
    }
}

static void clear_chunk_flow_map(struct fieldcache_ctx *ctx, uint64_t key, 
                                 enum nav_layer layer, bool enemies_only)
{
    khiter_t k = kh_get(idvec, ctx->chunk_ffield_map, key);
    if(k == kh_end(ctx->chunk_ffield_map))
        return;

    vec_id_t *keys = &kh_val(ctx->chunk_ffield_map, k);
    for(int i = vec_size(keys)-1; i >= 0; i--) {

        ff_id_t key = vec_AT(keys, i);
        if(N_FlowFieldLayer(key) != layer)
            continue;

        if(enemies_only && (N_FlowFieldTargetType(key) != TARGET_ENEMIES))
            continue;

        bool found = lru_flow_remove(&ctx->flow_cache, key);
        ctx->perfstats.flow_invalidated += !!found;
        vec_id_del(keys, i);
    }
    if(vec_size(keys) == 0) {
        vec_id_destroy(keys);
        kh_del(idvec, ctx->chunk_ffield_map, k);
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool N_FC_Init(struct fieldcache_ctx *ctx)
{
    if(!lru_los_init(&ctx->los_cache, CONFIG_LOS_CACHE_SZ, NULL))
        goto fail_los;

    if(!lru_flow_init(&ctx->flow_cache, CONFIG_FLOW_CACHE_SZ, NULL))
        goto fail_flow;

    if(!lru_ffid_init(&ctx->ffid_cache, CONFIG_MAPPING_CACHE_SZ, NULL))
        goto fail_ffid;

    if(!lru_grid_path_init(&ctx->grid_path_cache, CONFIG_GRID_PATH_CACHE_SZ, on_grid_path_evict))
        goto fail_grid_path;

    if(NULL == (ctx->chunk_ffield_map = kh_init(idvec)))
        goto fail_chunk_ffield;

    if(NULL == (ctx->chunk_lfield_map = kh_init(idvec)))
        goto fail_chunk_lfield;

    memset(&ctx->perfstats, 0, sizeof(ctx->perfstats));
    return true;

fail_chunk_lfield:
    kh_destroy(idvec, ctx->chunk_ffield_map);
fail_chunk_ffield:
    lru_grid_path_destroy(&ctx->grid_path_cache);
fail_grid_path:
    lru_ffid_destroy(&ctx->ffid_cache);
fail_ffid:
    lru_flow_destroy(&ctx->flow_cache);
fail_flow:
    lru_los_destroy(&ctx->los_cache);
fail_los:
    return false;
}

void N_FC_Destroy(struct fieldcache_ctx *ctx)
{
    lru_los_destroy(&ctx->los_cache);
    lru_flow_destroy(&ctx->flow_cache);
    lru_ffid_destroy(&ctx->ffid_cache);
    lru_grid_path_destroy(&ctx->grid_path_cache);

    destroy_all_entries(ctx->chunk_ffield_map);
    kh_destroy(idvec, ctx->chunk_ffield_map);

    destroy_all_entries(ctx->chunk_lfield_map);
    kh_destroy(idvec, ctx->chunk_lfield_map);
}

bool N_FC_Clone(struct fieldcache_ctx *from, struct fieldcache_ctx *to)
{
    PERF_ENTER();

    uint32_t key;
    vec_id_t curr;
    struct grid_path_desc path;

    if(!lru_los_clone(&from->los_cache, &to->los_cache))
        goto fail_clone_los;

    if(!lru_flow_clone(&from->flow_cache, &to->flow_cache))
        goto fail_clone_flow;

    if(!lru_ffid_clone(&from->ffid_cache, &to->ffid_cache))
        goto fail_clone_ffid;

    if(!lru_grid_path_init(&to->grid_path_cache, CONFIG_GRID_PATH_CACHE_SZ, on_grid_path_evict))
        goto fail_clone_grid;

    LRU_FOREACH_SAFE_REMOVE(grid_path, &from->grid_path_cache, key, path, {

        struct grid_path_desc copy = path;
        vec_coord_init(&copy.path);
        vec_coord_copy(&path.path, &copy.path);
        lru_grid_path_put(&to->grid_path_cache, key, &copy);
    });

    if(NULL == (to->chunk_ffield_map = kh_init(idvec)))
        goto fail_clone_chunk_ffield;
    if(0 != kh_resize(idvec, to->chunk_ffield_map, kh_size(from->chunk_ffield_map)))
        goto fail_clone_chunk_ffield_entries;

    kh_foreach(from->chunk_ffield_map, key, curr, {
        vec_id_t entry;
        vec_id_init(&entry);
        vec_id_copy(&entry, &curr); 

        int ret;
        kh_put(idvec, to->chunk_ffield_map, key, &ret);
        assert(ret != -1 && ret != 0);
        khiter_t k = kh_get(idvec, to->chunk_ffield_map, key);
        kh_val(to->chunk_ffield_map, k) = entry;
    });

    if(NULL == (to->chunk_lfield_map = kh_init(idvec)))
        goto fail_clone_chunk_lfield;
    if(0 != kh_resize(idvec, to->chunk_lfield_map, kh_size(from->chunk_lfield_map)))
        goto fail_clone_chunk_lfield_entries;

    kh_foreach(from->chunk_lfield_map, key, curr, {
        vec_id_t entry;
        vec_id_init(&entry);
        vec_id_copy(&entry, &curr); 

        int ret;
        kh_put(idvec, to->chunk_lfield_map, key, &ret);
        assert(ret != -1 && ret != 0);
        khiter_t k = kh_get(idvec, to->chunk_lfield_map, key);
        kh_val(to->chunk_lfield_map, k) = entry;
    });

    memcpy(&to->perfstats, &from->perfstats, sizeof(struct priv_fc_stats));
    PERF_RETURN(true);

fail_clone_chunk_lfield_entries:
    kh_destroy(idvec, to->chunk_lfield_map);
fail_clone_chunk_lfield:
    destroy_all_entries(to->chunk_ffield_map);
fail_clone_chunk_ffield_entries:
    kh_destroy(idvec, to->chunk_ffield_map);
fail_clone_chunk_ffield:
    lru_grid_path_destroy(&to->grid_path_cache);
fail_clone_grid:
    lru_ffid_destroy(&to->ffid_cache);
fail_clone_ffid:
    lru_flow_destroy(&to->flow_cache);
fail_clone_flow:
    lru_los_destroy(&to->los_cache);
fail_clone_los:
    PERF_RETURN(false);
}

void N_FC_ClearAll(struct fieldcache_ctx *ctx)
{
    lru_los_clear(&ctx->los_cache);
    lru_flow_clear(&ctx->flow_cache);
    lru_ffid_clear(&ctx->ffid_cache);
    lru_grid_path_clear(&ctx->grid_path_cache);

    destroy_all_entries(ctx->chunk_ffield_map);
    kh_clear(idvec, ctx->chunk_ffield_map);

    destroy_all_entries(ctx->chunk_lfield_map);
    kh_clear(idvec, ctx->chunk_lfield_map);
}

void N_FC_ClearStats(struct fieldcache_ctx *ctx)
{
    memset(&ctx->perfstats, 0, sizeof(ctx->perfstats));
}

void N_FC_GetStats(struct fieldcache_ctx *ctx, struct fc_stats *out_stats)
{
    out_stats->los_used = ctx->los_cache.used;
    out_stats->los_max = ctx->los_cache.capacity;
    out_stats->los_hit_rate = !ctx->perfstats.los_query ? 0
        : ((float)ctx->perfstats.los_hit) / ctx->perfstats.los_query;
    out_stats->los_invalidated = ctx->perfstats.los_invalidated;

    out_stats->flow_used = ctx->flow_cache.used;
    out_stats->flow_max = ctx->flow_cache.capacity;
    out_stats->flow_hit_rate = !ctx->perfstats.flow_query ? 0
        : ((float)ctx->perfstats.flow_hit) / ctx->perfstats.flow_query;
    out_stats->flow_invalidated = ctx->perfstats.flow_invalidated;

    out_stats->ffid_used = ctx->ffid_cache.used;
    out_stats->ffid_max = ctx->ffid_cache.capacity;
    out_stats->ffid_hit_rate = !ctx->perfstats.ffid_hit ? 0
        : ((float)ctx->perfstats.ffid_hit) / ctx->perfstats.ffid_query;

    out_stats->grid_path_used = ctx->grid_path_cache.used;
    out_stats->grid_path_max = ctx->grid_path_cache.capacity;
    out_stats->grid_path_hit_rate = !ctx->perfstats.grid_path_hit ? 0
        : ((float)ctx->perfstats.grid_path_hit) / ctx->perfstats.grid_path_query;
}

bool N_FC_ContainsLOSField(struct fieldcache_ctx *ctx, dest_id_t id, 
                           struct coord chunk_coord)
{
    uint64_t key = key_for_dest_and_chunk(id, chunk_coord);
    bool ret = lru_los_contains(&ctx->los_cache, key);

    ctx->perfstats.los_query++;
    ctx->perfstats.los_hit += !!ret;
    return ret;
}

const struct LOS_field *N_FC_LOSFieldAt(struct fieldcache_ctx *ctx, dest_id_t id, 
                                        struct coord chunk_coord)
{
    uint64_t key = key_for_dest_and_chunk(id, chunk_coord);
    return lru_los_at(&ctx->los_cache, key);
}

void N_FC_PutLOSField(struct fieldcache_ctx *ctx, dest_id_t id, 
                      struct coord chunk_coord, const struct LOS_field *lf)
{
    uint64_t key = key_for_dest_and_chunk(id, chunk_coord);
    lru_los_put(&ctx->los_cache, key, lf);
    field_map_add(ctx->chunk_lfield_map, key_for_chunk(chunk_coord), key);
}

bool N_FC_ContainsFlowField(struct fieldcache_ctx *ctx, ff_id_t ffid)
{
    bool ret = lru_flow_contains(&ctx->flow_cache, ffid);

    ctx->perfstats.flow_query++;
    ctx->perfstats.flow_hit += !!ret;
    return ret;
}

const struct flow_field *N_FC_FlowFieldAt(struct fieldcache_ctx *ctx, ff_id_t ffid)
{
    return lru_flow_at(&ctx->flow_cache, ffid);
}

void N_FC_PutFlowField(struct fieldcache_ctx *ctx, ff_id_t ffid, 
                       const struct flow_field *ff)
{
    lru_flow_put(&ctx->flow_cache, ffid, ff);

    struct coord chunk = (struct coord){(ffid >> 8) & 0xff, ffid & 0xff};
    field_map_add(ctx->chunk_ffield_map, key_for_chunk(chunk), ffid);
}

bool N_FC_GetDestFFMapping(struct fieldcache_ctx *ctx, dest_id_t id, 
                           struct coord chunk_coord, ff_id_t *out_ff)
{
    uint64_t key = key_for_dest_and_chunk(id, chunk_coord);
    bool ret = lru_ffid_get(&ctx->ffid_cache, key, out_ff);

    ctx->perfstats.ffid_query++;
    ctx->perfstats.ffid_hit += !!ret;
    return ret;
}

void N_FC_PutDestFFMapping(struct fieldcache_ctx *ctx, dest_id_t dest_id, 
                           struct coord chunk_coord, ff_id_t ffid)
{
    uint64_t key = key_for_dest_and_chunk(dest_id, chunk_coord);
    lru_ffid_put(&ctx->ffid_cache, key, &ffid);
}

bool N_FC_GetGridPath(struct fieldcache_ctx *ctx, struct coord local_start, 
                      struct coord local_dest, struct coord chunk, enum nav_layer layer, 
                      struct grid_path_desc *out)
{
    uint64_t key = grid_path_key(local_start, local_dest, chunk, layer);
    bool ret = lru_grid_path_get(&ctx->grid_path_cache, key, out);

    ctx->perfstats.grid_path_query++;
    ctx->perfstats.grid_path_hit += !!ret;
    return ret;
}

void N_FC_PutGridPath(struct fieldcache_ctx *ctx, struct coord local_start, 
                      struct coord local_dest, struct coord chunk, enum nav_layer layer, 
                      const struct grid_path_desc *in)
{
    uint64_t key = grid_path_key(local_start, local_dest, chunk, layer);
    lru_grid_path_put(&ctx->grid_path_cache, key, in);
}

void N_FC_InvalidateAllAtChunk(struct fieldcache_ctx *ctx, struct coord chunk, enum nav_layer layer)
{
    /* Note that chunk:field maps simply maintain a list of cache keys for 
     * which entries were set. The entries for these keys may have already 
     * been evicted. As well, keys for which data has been overwritten may
     * appear multiple times. So, not all the fields in the lists will
     * necessarily be in the caches. */

    uint64_t key = key_for_chunk(chunk);
    clear_chunk_los_map(ctx, key, layer);
    clear_chunk_flow_map(ctx, key, layer, false);
}

void N_FC_InvalidateAllThroughChunk(struct fieldcache_ctx *ctx, struct coord chunk, enum nav_layer layer)
{
    assert(Sched_UsingBigStack());

    dest_id_t paths[CONFIG_FLOW_CACHE_SZ];
    size_t npaths = 0;

    uint64_t key;
    ff_id_t ffid_val;

    /* Make sure not to actually query the caches, in order to not mess up the age history */
    /* First find all the paths going through the chunk. */
    LRU_FOREACH_SAFE_REMOVE(ffid, &ctx->ffid_cache, key, ffid_val, {

        (void)ffid_val;
        dest_id_t curr_dest = key_dest(key);
        struct coord curr_chunk = key_chunk(key);

        if(N_DestLayer(curr_dest) != layer)
            continue;

        if(0 == memcmp(&curr_chunk, &chunk, sizeof(chunk))
        && !dest_array_contains(paths, npaths, curr_dest)) {

            paths[npaths++] = curr_dest;
        }
    });

    /* Now that we know all the paths, find and remove all the flow 
     * fields belonging to them */
    struct flow_field ff_val;
    LRU_FOREACH_SAFE_REMOVE(flow, &ctx->flow_cache, key, ff_val, {
    
        (void)ff_val;
        dest_id_t curr_dest = key_dest(key);

        if(dest_array_contains(paths, npaths, curr_dest)) {
        
            bool found = lru_flow_remove(&ctx->flow_cache, key);
            ctx->perfstats.flow_invalidated += !!found;
        }
    });

    /* And remove all the LOS fields as well */
    struct LOS_field los_val;
    LRU_FOREACH_SAFE_REMOVE(los, &ctx->los_cache, key, los_val, {

        (void)los_val;
        dest_id_t curr_dest = key_dest(key);

        if(dest_array_contains(paths, npaths, curr_dest)) {
        
            bool found = lru_los_remove(&ctx->los_cache, key);
            ctx->perfstats.los_invalidated += !!found;
        }
    });
}

void N_FC_InvalidateNeighbourEnemySeekFields(struct fieldcache_ctx *ctx, int width, int height, 
                                             struct coord chunk, enum nav_layer layer)
{
    for(int dr = -1; dr <= +1; dr++) {
    for(int dc = -1; dc <= +1; dc++) {

        if(dr == 0 && dc == 0)
            continue;

        int abs_r = chunk.r + dr;
        int abs_c = chunk.c + dc;

        if(abs_r < 0 || abs_r >= height)
            continue;
        if(abs_c < 0 || abs_c >= width)
            continue;

        struct coord curr = (struct coord){abs_r, abs_c};
        uint64_t key = key_for_chunk(curr);
        clear_chunk_flow_map(ctx, key, layer, true);
    }}
}

void N_FC_InvalidateDynamicSurroundFields(struct fieldcache_ctx *ctx)
{
    uint64_t key;
    struct flow_field ff_val;

    LRU_FOREACH_SAFE_REMOVE(flow, &ctx->flow_cache, key, ff_val, {
    
        int type = N_FlowFieldTargetType(key);
        if(type != TARGET_ENTITY)
            continue;

        uint32_t ent = ff_val.target.ent.target;
        if(!(G_FlagsGet(ent) & ENTITY_FLAG_MOVABLE))
            continue;

        lru_flow_remove(&ctx->flow_cache, key);
    });
}

struct fieldcache_ctx *N_FC_New(void)
{
    return malloc(sizeof(struct fieldcache_ctx));
}

void N_FC_Free(struct fieldcache_ctx *ctx)
{
    assert(ctx);
    PF_FREE(ctx);
}

