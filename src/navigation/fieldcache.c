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
#include "../event.h"

#include <assert.h>


#define LOS_CACHE_SZ    (2048)
#define FLOW_CAHCE_SZ   (2048)
#define PATH_CACHE_SZ   (16384)

LRU_CACHE_TYPE(los, struct LOS_field)
LRU_CACHE_PROTOTYPES(static, los, struct LOS_field)
LRU_CACHE_IMPL(static, los, struct LOS_field)

LRU_CACHE_TYPE(flow, struct flow_field)
LRU_CACHE_PROTOTYPES(static, flow, struct flow_field)
LRU_CACHE_IMPL(static, flow, struct flow_field)

LRU_CACHE_TYPE(path, ff_id_t)
LRU_CACHE_PROTOTYPES(static, path, ff_id_t)
LRU_CACHE_IMPL(static, path, ff_id_t)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static lru(los)  s_los_cache;
static lru(flow) s_flow_cache;
/* The path cache maps a (dest_id, chunk coordinate) tuple to a flow field ID,
 * which could be used to retreive the relevant field from the flow cache. 
 * The reason for this is that the same flow field chunk can be shared between
 * many different paths. */
static lru(path) s_path_cache;

static struct priv_fc_stats{
    unsigned los_query;
    unsigned los_hit;
    unsigned flow_query;
    unsigned flow_hit;
    unsigned path_query;
    unsigned path_hit;
}s_perfstats = {0};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

uint64_t key_for_dest_and_chunk(dest_id_t id, struct coord chunk)
{
    return ((((uint64_t)id) << 32) | (((uint64_t)chunk.r) << 16) | (((uint64_t)chunk.c) & 0xffff));
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool N_FC_Init(void)
{
    if(!lru_los_init(&s_los_cache, LOS_CACHE_SZ))
        goto fail_los;

    if(!lru_flow_init(&s_flow_cache, FLOW_CAHCE_SZ))
        goto fail_flow;

    if(!lru_path_init(&s_path_cache, PATH_CACHE_SZ))
        goto fail_path;

    return true;

fail_path:
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
    lru_path_destroy(&s_path_cache);
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
    out_stats->flow_used = s_flow_cache.used;
    out_stats->flow_max = s_flow_cache.capacity;
    out_stats->flow_hit_rate = !s_perfstats.flow_query ? 0
                             : ((float)s_perfstats.flow_hit) / s_perfstats.flow_query;
    out_stats->path_used = s_path_cache.used;
    out_stats->path_max = s_path_cache.capacity;
    out_stats->path_hit_rate = !s_perfstats.path_hit ? 0
                             : ((float)s_perfstats.path_hit) / s_perfstats.path_query;
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
    lru_flow_put(&s_flow_cache, ffid, ff);
}

bool N_FC_GetDestFFMapping(dest_id_t id, struct coord chunk_coord, ff_id_t *out_ff)
{
    uint64_t key = key_for_dest_and_chunk(id, chunk_coord);
    bool ret = lru_path_get(&s_path_cache, key, out_ff);

    s_perfstats.path_query++;
    s_perfstats.path_hit += !!ret;
    return ret;
}

void N_FC_PutDestFFMapping(dest_id_t dest_id, struct coord chunk_coord, ff_id_t ffid)
{
    uint64_t key = key_for_dest_and_chunk(dest_id, chunk_coord);
    lru_path_put(&s_path_cache, key, &ffid);
}

