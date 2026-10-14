/*
 *  This file is part of Permafrost Engine.
 *  Copyright (C) 2026 Eduard Permyakov
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

#define MEM_FILE_SYS MEM_SYS_GAME
#define MEM_FILE_SUB MEM_SUB_GAME_DISTRICT

#include "district.h"
#include "public/game.h"
#include "game_private.h"
#include "position.h"
#include "../event.h"
#include "../entity.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"
#include "../lib/public/pqueue.h"
#include "../lib/public/stalloc.h"

#include <assert.h>
#include <string.h>
#include <math.h>

#include "../mem.h"

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_GAME, MEM_SUB_GAME_DISTRICT)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_GAME, MEM_SUB_GAME_DISTRICT)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_GAME, MEM_SUB_GAME_DISTRICT)

/* A district cell is 4x4 map tiles */
#define CELL_SIZE           (32.0f)
#define ROAD_COST_FACTOR    (0.375f)
#define DIAGONAL            (1.41421356f)
#define UNREACHED           (1e30f)
/* Floods wait this long after a change, so that a burst of them is one */
#define DEBOUNCE_TICKS      (20)
#define MAX_SEED_CLUSTERS   (1024)
#define OVERLAY_BATCH       (1024)
#define HUB_TYPE            "hub"
#define ARR_SIZE(a)         (sizeof(a)/sizeof((a)[0]))

enum{
    LAYER_GROUND = 0,
    LAYER_WATER,
    NUM_LAYERS
};

struct field{
    /* Per cell: the cluster whose hub is nearest by path, and how far */
    int   *owner;
    float *cost;
};

struct cell_node{
    int   idx;
    float cost;
};

struct district{
    int cluster;
    int slot;
};

PQUEUE_TYPE(cell, struct cell_node)
PQUEUE_IMPL(static, cell, struct cell_node)

static void on_cluster_changed(void *user, void *event);
static void on_20hz_tick(void *user, void *event);
static void on_render_3d(void *user, void *event);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map *s_map;
static vec3_t            s_map_pos;
static int               s_rows, s_cols;
/* Per layer and cell: whether a unit of the layer can be there */
static bool             *s_passable[NUM_LAYERS];
static bool             *s_road;
static struct field      s_fields[MAX_FACTIONS][NUM_LAYERS];
static bool              s_dirty;
static int               s_dirty_ticks;
static size_t            s_nroads;
static bool              s_show;
/* The cells a hub placed at the preview position would take over */
static bool              s_preview_on;
static int               s_preview_faction;
static vec2_t            s_preview_pos;
static bool             *s_preview_cells;
/* Hues far enough apart that no two read as shades of one colour, the most
 * distinct first */
static const float       s_palette[] = {0.0f, 130.0f, 230.0f, 60.0f, 305.0f,
                                        180.0f, 30.0f, 270.0f, 90.0f, 340.0f};
/* The districts with cells in a field, by id, each with its palette slot */
static struct district   s_districts[MAX_SEED_CLUSTERS];
static size_t            s_ndistricts;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static vec2_t cell_center(int r, int c)
{
    return (vec2_t){
        s_map_pos.x - (c + 0.5f) * CELL_SIZE,
        s_map_pos.z + (r + 0.5f) * CELL_SIZE
    };
}

static bool cell_for_pos(vec2_t pos, int *out_r, int *out_c)
{
    int c = (int)floorf((s_map_pos.x - pos.x) / CELL_SIZE);
    int r = (int)floorf((pos.z - s_map_pos.z) / CELL_SIZE);
    if(r < 0 || r >= s_rows || c < 0 || c >= s_cols)
        return false;
    *out_r = r;
    *out_c = c;
    return true;
}

static int layer_for_entity(uint32_t uid)
{
    return (G_FlagsGet(uid) & ENTITY_FLAG_WATER) && !(G_FlagsGet(uid) & ENTITY_FLAG_BUILDING)
        ? LAYER_WATER : LAYER_GROUND;
}

static void update_terrain(void)
{
    for(int r = 0; r < s_rows; r++) {
    for(int c = 0; c < s_cols; c++) {
        int idx = r * s_cols + c;
        vec2_t center = cell_center(r, c);
        s_passable[LAYER_GROUND][idx] = M_NavPositionPathable(s_map, NAV_LAYER_GROUND_1X1, center);
        s_passable[LAYER_WATER][idx] = M_NavPositionPathable(s_map, NAV_LAYER_WATER_1X1, center);
        struct road_desc rd;
        uint8_t links = 0;
        s_road[idx] = G_Roads_DescForPoint(center, &rd) && G_Roads_Get(rd, &links);
    }}
}

static bool field_alloc(struct field *f)
{
    if(f->owner)
        return true;
    f->owner = PF_CALLOC(s_rows * s_cols, sizeof(int));
    f->cost = PF_MALLOC(s_rows * s_cols * sizeof(float));
    return (f->owner && f->cost);
}

static void field_free(struct field *f)
{
    PF_FREE(f->owner);
    PF_FREE(f->cost);
    f->owner = NULL;
    f->cost = NULL;
}

static float step_cost(int layer, int from, int to, bool diagonal)
{
    float base = diagonal ? DIAGONAL : 1.0f;
    if(layer == LAYER_GROUND && s_road[from] && s_road[to]) {
        base *= ROAD_COST_FACTOR;
    }
    return base * CELL_SIZE;
}

/* Expands a Dijkstra flood from the cells already queued, keeping for every 
 * cell the cheapest cost found. With 'only_better', a cell is only taken 
 * where the flood beats the field's cost, and 'out_taken' marks it. */
static void flood(int layer, float *cost, int *owner, pq_cell_t *pq, 
                  const float *better_than, bool *out_taken)
{
    static const int dr[8] = {-1, 1, 0, 0, -1, -1, 1, 1};
    static const int dc[8] = {0, 0, -1, 1, -1, 1, -1, 1};

    while(pq_size(pq)) {

        struct cell_node node;
        pq_cell_pop(pq, &node);
        if(node.cost > cost[node.idx])
            continue;

        int r = node.idx / s_cols, c = node.idx % s_cols;
        for(int i = 0; i < 8; i++) {
            int nr = r + dr[i], nc = c + dc[i];
            if(nr < 0 || nr >= s_rows || nc < 0 || nc >= s_cols)
                continue;
            int nidx = nr * s_cols + nc;
            if(!s_passable[layer][nidx])
                continue;
            /* No cutting corners past impassable cells */
            if(i >= 4 && (!s_passable[layer][r * s_cols + nc] || !s_passable[layer][nr * s_cols + c]))
                continue;
            float ncost = node.cost + step_cost(layer, node.idx, nidx, i >= 4);
            if(ncost >= cost[nidx])
                continue;
            if(better_than && ncost >= better_than[nidx])
                continue;
            cost[nidx] = ncost;
            if(owner) {
                owner[nidx] = owner[node.idx];
            }
            if(out_taken) {
                out_taken[nidx] = true;
            }
            pq_cell_push(pq, ncost, (struct cell_node){nidx, ncost});
        }
    }
}

static void seed_cell(int idx, float at, int cluster, float *cost, int *owner, pq_cell_t *pq)
{
    if(at >= cost[idx])
        return;
    cost[idx] = at;
    if(owner) {
        owner[idx] = cluster;
    }
    pq_cell_push(pq, at, (struct cell_node){idx, at});
}

static void seed(int layer, uint32_t uid, int cluster, float *cost, int *owner, pq_cell_t *pq)
{
    int r, c;
    if(!cell_for_pos(G_Pos_GetXZ(uid), &r, &c))
        return;
    int idx = r * s_cols + c;
    if(s_passable[layer][idx]) {
        seed_cell(idx, 0.0f, cluster, cost, owner, pq);
        return;
    }
    /* One standing where the layer cannot go, such as a dock out on the
     * water, reaches it from the cells about its footprint */
    int reach = (int)ceilf((G_GetSelectionRadius(uid) + CELL_SIZE) / CELL_SIZE);
    for(int dr = -reach; dr <= reach; dr++) {
    for(int dc = -reach; dc <= reach; dc++) {
        int nr = r + dr, nc = c + dc;
        if(nr < 0 || nr >= s_rows || nc < 0 || nc >= s_cols)
            continue;
        int nidx = nr * s_cols + nc;
        if(!s_passable[layer][nidx])
            continue;
        seed_cell(nidx, sqrtf(dr * dr + dc * dc) * CELL_SIZE, cluster, cost, owner, pq);
    }}
}

static int district_index(const struct district *list, size_t n, int cluster)
{
    for(int i = 0; i < n; i++) {
        if(list[i].cluster == cluster)
            return i;
    }
    return -1;
}

static void district_add(int cluster)
{
    if(district_index(s_districts, s_ndistricts, cluster) >= 0 || s_ndistricts == MAX_SEED_CLUSTERS)
        return;
    int i = s_ndistricts++;
    while(i > 0 && s_districts[i - 1].cluster > cluster) {
        s_districts[i] = s_districts[i - 1];
        i--;
    }
    s_districts[i] = (struct district){cluster, 0};
}

static void mark_adjacent(uint32_t *adj, size_t words, int a, int b)
{
    if(!a || !b || a == b)
        return;
    int i = district_index(s_districts, s_ndistricts, a);
    int j = district_index(s_districts, s_ndistricts, b);
    if(i < 0 || j < 0)
        return;
    adj[i * words + j / 32] |= (1u << (j % 32));
    adj[j * words + i / 32] |= (1u << (i % 32));
}

static bool adjacent(const uint32_t *adj, size_t words, int i, int j)
{
    return adj[i * words + j / 32] & (1u << (j % 32));
}

/* Gives bordering districts different palette slots, keeping a district's
 * old slot wherever it still fits so colours stay put as the map changes */
static void recolour(void)
{
    STALLOC(struct district, prev, MAX_SEED_CLUSTERS);
    size_t nprev = s_ndistricts;
    memcpy(prev, s_districts, nprev * sizeof(struct district));
    s_ndistricts = 0;

    for(int fac = 0; fac < MAX_FACTIONS; fac++) {
        const int *owner = s_fields[fac][LAYER_GROUND].owner;
        if(!owner)
            continue;
        for(int i = 0; i < s_rows * s_cols; i++) {
            if(owner[i]) {
                district_add(owner[i]);
            }
        }
    }

    const size_t words = (s_ndistricts + 31) / 32;
    uint32_t *adj = PF_CALLOC(s_ndistricts * words, sizeof(uint32_t));
    if(!adj) {
        STFREE(prev);
        return;
    }

    for(int fac = 0; fac < MAX_FACTIONS; fac++) {
        const int *owner = s_fields[fac][LAYER_GROUND].owner;
        if(!owner)
            continue;
        for(int r = 0; r < s_rows; r++) {
        for(int c = 0; c < s_cols; c++) {
            int idx = r * s_cols + c;
            if(c + 1 < s_cols) {
                mark_adjacent(adj, words, owner[idx], owner[idx + 1]);
            }
            if(r + 1 < s_rows) {
                mark_adjacent(adj, words, owner[idx], owner[idx + s_cols]);
            }
        }}
    }

    for(int i = 0; i < s_ndistricts; i++) {
        int uses[ARR_SIZE(s_palette)] = {0};
        for(int j = 0; j < i; j++) {
            if(adjacent(adj, words, i, j)) {
                uses[s_districts[j].slot]++;
            }
        }
        int p = district_index(prev, nprev, s_districts[i].cluster);
        int slot = (p >= 0) ? prev[p].slot : 0;
        if(p < 0 || uses[slot] > 0) {
            for(int k = 0; k < ARR_SIZE(s_palette); k++) {
                if(uses[k] < uses[slot]) {
                    slot = k;
                }
            }
        }
        s_districts[i].slot = slot;
    }
    PF_FREE(adj);
    STFREE(prev);
}

static void recompute(void)
{
    update_terrain();
    const size_t ncells = s_rows * s_cols;

    int clusters[MAX_SEED_CLUSTERS];
    size_t nclusters = G_Cluster_GetAll(MAX_SEED_CLUSTERS, clusters);

    for(int fac = 0; fac < MAX_FACTIONS; fac++) {
    for(int layer = 0; layer < NUM_LAYERS; layer++) {

        struct field *f = &s_fields[fac][layer];
        pq_cell_t pq;
        pq_cell_init(&pq);
        bool any = false;

        for(int i = 0; i < nclusters; i++) {
            if(G_Cluster_Faction(clusters[i]) != fac)
                continue;
            if(strcmp(G_Cluster_Type(clusters[i]), HUB_TYPE))
                continue;

            uint32_t members[64];
            size_t nmembers = G_Cluster_Members(clusters[i], 64, members);
            for(int j = 0; j < nmembers; j++) {
                /* A dock reaches both the water and the land */
                bool water = (G_FlagsGet(members[j]) & ENTITY_FLAG_WATER);
                if(layer == LAYER_WATER && !water)
                    continue;
                if(!any) {
                    if(!field_alloc(f))
                        break;
                    for(int k = 0; k < ncells; k++) {
                        f->cost[k] = UNREACHED;
                        f->owner[k] = 0;
                    }
                    any = true;
                }
                seed(layer, members[j], clusters[i], f->cost, f->owner, &pq);
            }
        }

        if(any) {
            flood(layer, f->cost, f->owner, &pq, NULL, NULL);
        }else{
            field_free(f);
        }
        pq_cell_destroy(&pq);
    }}
    recolour();
    s_dirty = false;
}

static void recompute_preview(void)
{
    memset(s_preview_cells, 0, s_rows * s_cols * sizeof(bool));
    if(!s_preview_on)
        return;
    int r, c;
    if(!cell_for_pos(s_preview_pos, &r, &c))
        return;

    const size_t ncells = s_rows * s_cols;
    float *cost = PF_MALLOC(ncells * sizeof(float));
    if(!cost)
        return;
    for(int i = 0; i < ncells; i++) {
        cost[i] = UNREACHED;
    }

    struct field *f = &s_fields[s_preview_faction][LAYER_GROUND];
    int idx = r * s_cols + c;
    cost[idx] = 0.0f;
    s_preview_cells[idx] = true;

    pq_cell_t pq;
    pq_cell_init(&pq);
    pq_cell_push(&pq, 0.0f, (struct cell_node){idx, 0.0f});
    flood(LAYER_GROUND, cost, NULL, &pq, f->cost, s_preview_cells);
    pq_cell_destroy(&pq);
    PF_FREE(cost);
}

static vec3_t cluster_color(int cluster, bool core)
{
    int i = district_index(s_districts, s_ndistricts, cluster);
    float h = s_palette[i >= 0 ? s_districts[i].slot : 0] / 60.0f;
    float x = 1.0f - fabsf(fmodf(h, 2.0f) - 1.0f);
    vec3_t rgb;
    switch((int)h) {
    case 0:  rgb = (vec3_t){1.0f, x, 0.0f}; break;
    case 1:  rgb = (vec3_t){x, 1.0f, 0.0f}; break;
    case 2:  rgb = (vec3_t){0.0f, 1.0f, x}; break;
    case 3:  rgb = (vec3_t){0.0f, x, 1.0f}; break;
    case 4:  rgb = (vec3_t){x, 0.0f, 1.0f}; break;
    default: rgb = (vec3_t){1.0f, 0.0f, x}; break;
    }
    float scale = core ? 1.0f : 0.35f;
    return (vec3_t){rgb.x * scale, rgb.y * scale, rgb.z * scale};
}

static void flush_quads(vec2_t *corners, vec3_t *colors, size_t count)
{
    if(count == 0)
        return;
    mat4x4_t model;
    PFM_Mat4x4_Identity(&model);
    bool on_water_surface = true;

    R_PushCmd((struct rcmd){
        .func = R_GL_DrawMapOverlayQuads,
        .nargs = 6,
        .args = {
            R_PushArg(corners, sizeof(vec2_t) * 4 * count),
            R_PushArg(colors, sizeof(vec3_t) * count),
            R_PushArg(&count, sizeof(count)),
            R_PushArg(&model, sizeof(model)),
            R_PushArg(&on_water_surface, sizeof(bool)),
            (void*)G_GetPrevTickMap(),
        },
    });
}

static void on_render_3d(void *user, void *event)
{
    if(!s_show && !s_preview_on)
        return;

    STALLOC(vec2_t, corners, 4 * OVERLAY_BATCH);
    STALLOC(vec3_t, colors, OVERLAY_BATCH);
    size_t count = 0;

    uint16_t pfacs = G_GetPlayerControlledFactions();
    for(int fac = 0; fac < MAX_FACTIONS; fac++) {

        if(!(pfacs & (0x1 << fac)))
            continue;
        const struct field *f = &s_fields[fac][LAYER_GROUND];
        if(!f->owner)
            continue;

        for(int r = 0; r < s_rows; r++) {
        for(int c = 0; c < s_cols; c++) {

            int idx = r * s_cols + c;
            bool preview = s_preview_on && s_preview_cells[idx] && fac == s_preview_faction;
            if(!preview && (!s_show || f->owner[idx] == 0))
                continue;

            vec3_t color = preview ? (vec3_t){1.0f, 1.0f, 1.0f}
                                   : cluster_color(f->owner[idx], f->cost[idx] <= G_DISTRICT_TERRITORY_COST);
            vec2_t center = cell_center(r, c);
            const float half = CELL_SIZE * 0.5f;
            vec2_t min = (vec2_t){center.x - half, center.z - half};
            vec2_t max = (vec2_t){center.x + half, center.z + half};
            corners[count * 4 + 0] = (vec2_t){max.x, min.z};
            corners[count * 4 + 1] = (vec2_t){max.x, max.z};
            corners[count * 4 + 2] = (vec2_t){min.x, max.z};
            corners[count * 4 + 3] = (vec2_t){min.x, min.z};
            colors[count] = color;
            if(++count == OVERLAY_BATCH) {
                flush_quads(corners, colors, count);
                count = 0;
            }
        }}
    }
    flush_quads(corners, colors, count);
    STFREE(corners);
    STFREE(colors);
}

static void on_cluster_changed(void *user, void *event)
{
    s_dirty = true;
    s_dirty_ticks = 0;
}

static void on_20hz_tick(void *user, void *event)
{
    size_t nroads = G_Roads_Count();
    if(nroads != s_nroads) {
        s_nroads = nroads;
        s_dirty = true;
        s_dirty_ticks = 0;
    }
    if(!s_dirty)
        return;
    if(++s_dirty_ticks < DEBOUNCE_TICKS)
        return;
    recompute();
    if(s_preview_on) {
        recompute_preview();
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_District_Init(const struct map *map)
{
    struct map_resolution res;
    M_GetResolution(map, &res);

    s_map = map;
    s_map_pos = M_GetPos(map);
    s_cols = (int)ceilf(res.chunk_w * res.field_w / CELL_SIZE);
    s_rows = (int)ceilf(res.chunk_h * res.field_h / CELL_SIZE);
    const size_t ncells = s_rows * s_cols;

    memset(s_fields, 0, sizeof(s_fields));
    s_passable[LAYER_GROUND] = PF_CALLOC(ncells, sizeof(bool));
    s_passable[LAYER_WATER] = PF_CALLOC(ncells, sizeof(bool));
    s_road = PF_CALLOC(ncells, sizeof(bool));
    s_preview_cells = PF_CALLOC(ncells, sizeof(bool));
    if(!s_passable[LAYER_GROUND] || !s_passable[LAYER_WATER] || !s_road || !s_preview_cells) {
        G_District_Shutdown();
        return false;
    }

    s_dirty = true;
    s_dirty_ticks = DEBOUNCE_TICKS;
    s_nroads = 0;
    s_show = false;
    s_preview_on = false;
    s_ndistricts = 0;

    E_Global_Register(EVENT_CLUSTER_CHANGED, on_cluster_changed, NULL, G_ALL);
    E_Global_Register(EVENT_20HZ_TICK, on_20hz_tick, NULL, G_RUNNING);
    E_Global_Register(EVENT_RENDER_3D_POST, on_render_3d, NULL, G_ALL);
    return true;
}

void G_District_Shutdown(void)
{
    E_Global_Unregister(EVENT_CLUSTER_CHANGED, on_cluster_changed);
    E_Global_Unregister(EVENT_20HZ_TICK, on_20hz_tick);
    E_Global_Unregister(EVENT_RENDER_3D_POST, on_render_3d);

    for(int fac = 0; fac < MAX_FACTIONS; fac++) {
        for(int layer = 0; layer < NUM_LAYERS; layer++) {
            field_free(&s_fields[fac][layer]);
        }
    }
    PF_FREE(s_passable[LAYER_GROUND]);
    PF_FREE(s_passable[LAYER_WATER]);
    PF_FREE(s_road);
    PF_FREE(s_preview_cells);
    s_passable[LAYER_GROUND] = s_passable[LAYER_WATER] = NULL;
    s_road = NULL;
    s_preview_cells = NULL;
}

void G_District_Update(void)
{
    if(s_dirty) {
        recompute();
    }
}

int G_District_At(int faction, bool water, vec2_t pos, float *out_cost)
{
    if(out_cost) {
        *out_cost = UNREACHED;
    }
    if(faction < 0 || faction >= MAX_FACTIONS)
        return 0;
    const struct field *f = &s_fields[faction][water ? LAYER_WATER : LAYER_GROUND];
    int r, c;
    if(!f->owner || !cell_for_pos(pos, &r, &c))
        return 0;
    int idx = r * s_cols + c;
    if(out_cost) {
        *out_cost = f->cost[idx];
    }
    return f->owner[idx];
}

int G_District_Of(uint32_t uid)
{
    return G_District_At(G_GetFactionID(uid), layer_for_entity(uid) == LAYER_WATER, 
        G_Pos_GetXZ(uid), NULL);
}

bool G_District_InTerritory(int faction, bool water, vec2_t pos)
{
    float cost;
    int owner = G_District_At(faction, water, pos, &cost);
    return owner && cost <= G_DISTRICT_TERRITORY_COST;
}

void G_District_SetShow(bool on)
{
    s_show = on;
}

bool G_District_GetShow(void)
{
    return s_show;
}

int G_District_SetPreview(bool on, int faction, vec2_t pos)
{
    s_preview_on = on && faction >= 0 && faction < MAX_FACTIONS;
    s_preview_faction = faction;
    s_preview_pos = pos;
    if(s_dirty) {
        recompute();
    }
    recompute_preview();

    int ret = 0;
    if(s_preview_on) {
        for(int i = 0; i < s_rows * s_cols; i++) {
            ret += s_preview_cells[i];
        }
    }
    return ret;
}
