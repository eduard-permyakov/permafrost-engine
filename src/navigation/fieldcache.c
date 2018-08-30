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
#include "../lib/public/khash.h"
#include "../event.h"

#include <assert.h>


#define EVICTION_NUM_SECS (30)

struct LOS_entry{
    unsigned int     age;
    struct LOS_field lf;
};

struct flow_entry{
    unsigned int      age;
    struct flow_field ff;
};

struct path_entry{
    unsigned int age;
    ff_id_t      id;
};

KHASH_MAP_INIT_INT64(los, struct LOS_entry)
KHASH_MAP_INIT_INT64(flow, struct flow_entry)
KHASH_MAP_INIT_INT64(dest_flow, struct path_entry)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

khash_t(los)         *s_los_table;
khash_t(flow)        *s_flow_table;
/* The dest_flow table maps a (dest_id, chunk coordinate) tuple to a flow field ID,
 * which could be used to retreive the relevant field from the flow table. 
 * The reason for this is that the same flow field chunk can be shared between
 * many different paths. */
khash_t(dest_flow)   *s_dest_flow_table;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void on_1hz_tick(void *unused1, void *unused2)
{
    for(khiter_t k = kh_begin(s_los_table); k != kh_end(s_los_table); k++) {
        if(!kh_exist(s_los_table, k))
            continue;
        if(--kh_value(s_los_table, k).age == 0)
            kh_del(los, s_los_table, k);
    }

    for(khiter_t k = kh_begin(s_flow_table); k != kh_end(s_flow_table); k++) {
        if(!kh_exist(s_flow_table, k))
            continue;
        if(--kh_value(s_flow_table, k).age == 0)
            kh_del(flow, s_flow_table, k);
    }

    for(khiter_t k = kh_begin(s_dest_flow_table); k != kh_end(s_dest_flow_table); k++) {
        if(!kh_exist(s_dest_flow_table, k))
            continue;
        if(--kh_value(s_dest_flow_table, k).age == 0)
            kh_del(dest_flow, s_dest_flow_table, k);
    }
}

uint64_t key_for_dest_and_chunk(dest_id_t id, struct coord chunk)
{
    return ((((uint64_t)id) << 32) | (((uint64_t)chunk.r) << 16) | (((uint64_t)chunk.c) & 0xffff));
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool N_FC_Init(void)
{
    s_los_table = kh_init(los);
    if(!s_los_table)
        goto fail_los;

    s_flow_table = kh_init(flow);
    if(!s_flow_table)
        goto fail_flow;

    s_dest_flow_table = kh_init(dest_flow);
    if(!s_dest_flow_table)
        goto fail_dest_flow;

    E_Global_Register(EVENT_1HZ_TICK, on_1hz_tick, NULL);
    return true;

fail_dest_flow:
    kh_destroy(flow, s_flow_table);
fail_flow:
    kh_destroy(los, s_los_table);
fail_los:
    return false;
}

void N_FC_Shutdown(void)
{
    E_Global_Unregister(EVENT_1HZ_TICK, on_1hz_tick);

    kh_destroy(los, s_los_table);
    kh_destroy(flow, s_flow_table);
    kh_destroy(dest_flow, s_dest_flow_table);
}

bool N_FC_ContainsLOSField(dest_id_t id, struct coord chunk_coord)
{
    khiter_t k = kh_get(los, s_los_table, key_for_dest_and_chunk(id, chunk_coord));
    if(k == kh_end(s_los_table))
        return false;

    return true;
}

const struct LOS_field *N_FC_LOSFieldAt(dest_id_t id, struct coord chunk_coord)
{
    khiter_t k = kh_get(los, s_los_table, key_for_dest_and_chunk(id, chunk_coord));
    assert(k != kh_end(s_los_table));

    kh_value(s_los_table, k).age = EVICTION_NUM_SECS;
    return &kh_value(s_los_table, k).lf;
}

void N_FC_SetLOSField(dest_id_t id, struct coord chunk_coord, const struct LOS_field *lf)
{
    int ret;
    khiter_t k = kh_put(los, s_los_table, key_for_dest_and_chunk(id, chunk_coord), &ret);
    assert(ret != -1 && ret != 0);
    kh_value(s_los_table, k) = (struct LOS_entry){
        .age = EVICTION_NUM_SECS,
        .lf = *lf
    };
}

bool N_FC_ContainsFlowField(dest_id_t id, struct coord chunk_coord, ff_id_t *out_ffid)
{
    khiter_t k;

    k = kh_get(dest_flow, s_dest_flow_table, key_for_dest_and_chunk(id, chunk_coord));
    if(k == kh_end(s_dest_flow_table))
        return false;

    ff_id_t key = kh_value(s_dest_flow_table, k).id;
    k = kh_get(flow, s_flow_table, key);
    if(k == kh_end(s_flow_table))
        return false;

    *out_ffid = key;
    return true;
}

const struct flow_field *N_FC_FlowFieldAt(dest_id_t id, struct coord chunk_coord)
{
    khiter_t k;

    k = kh_get(dest_flow, s_dest_flow_table, key_for_dest_and_chunk(id, chunk_coord));
    assert(k != kh_end(s_dest_flow_table));

    ff_id_t key = kh_value(s_dest_flow_table, k).id;
    kh_value(s_dest_flow_table, k).age = EVICTION_NUM_SECS;

    k = kh_get(flow, s_flow_table, key);
    assert(k != kh_end(s_flow_table));

    kh_value(s_flow_table, k).age = EVICTION_NUM_SECS;
    return &kh_value(s_flow_table, k).ff;
}

void N_FC_SetFlowField(dest_id_t id, struct coord chunk_coord, 
                       ff_id_t field_id, const struct flow_field *ff)
{
    khiter_t k;
    int ret;

    k = kh_put(dest_flow, s_dest_flow_table, key_for_dest_and_chunk(id, chunk_coord), &ret);
    assert(ret != -1);
    kh_value(s_dest_flow_table, k) = (struct path_entry){
        .age = EVICTION_NUM_SECS,
        .id = field_id
    };

    k = kh_put(flow, s_flow_table, field_id, &ret);
    assert(ret != -1);
    kh_value(s_flow_table, k) = (struct flow_entry){
        .age = EVICTION_NUM_SECS,
        .ff = *ff
    };
}

