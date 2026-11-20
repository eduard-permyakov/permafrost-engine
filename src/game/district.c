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
#include "../main.h"
#include "public/game.h"
#include "game_private.h"
#include "position.h"
#include "../event/public/event.h"
#include "../entity.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../phys/public/collision.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"
#include "../lib/public/pqueue.h"
#include "../lib/public/stalloc.h"
#include "../lib/public/vec.h"

#include <assert.h>
#include <string.h>
#include <math.h>

#include "../perf/public/mem.h"

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_GAME, MEM_SUB_GAME_DISTRICT)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_GAME, MEM_SUB_GAME_DISTRICT)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_GAME, MEM_SUB_GAME_DISTRICT)

/* A district cell is 4x4 map tiles */
#define CELL_SIZE           (32.0f)
#define DIAGONAL            (1.41421356f)
#define UNREACHED           (1e30f)
/* Floods wait this long after a change, so that a burst of them is one */
#define DEBOUNCE_TICKS      (20)
#define MAX_SEED_CLUSTERS   (1024)
#define TILES_PER_CELL      (4)
/* Lifted this far over the ground, so as not to fight it */
#define OVERLAY_LIFT        (0.1f)
#define MESH_DISTRICTS      (0)
#define MESH_PREVIEW        (1)
/* A shade of each cell the scripts set, such as the land's fertility */
#define MESH_LENS           (2)
#define MESH_FOCUS          (3)
#define DOTS_PER_SIDE       (8)
#define FOCUS_BORDER_RGB    ((vec3_t){0.78f, 0.15f, 0.18f})
#define HUB_TYPE            "hub"
#define ARR_SIZE(a)         (sizeof(a)/sizeof((a)[0]))
#define MAX(a, b)           ((a) > (b) ? (a) : (b))
#define MIN(a, b)           ((a) < (b) ? (a) : (b))

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

VEC_TYPE(overlay, struct overlay_vert)
VEC_IMPL(static inline, overlay, struct overlay_vert)

static void on_cluster_changed(void *user, void *event);
static void on_20hz_tick(void *user, void *event);
static void on_render_3d(void *user, void *event);
static void preview_flood(void);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map *s_map;
static vec3_t            s_map_pos;
static struct map_resolution s_res;
static int               s_rows, s_cols;
/* Per layer and cell: whether a unit of the layer can be there */
static bool             *s_passable[NUM_LAYERS];
static bool             *s_road;
static struct field      s_fields[MAX_FACTIONS][NUM_LAYERS];
/* Bumped whenever the fields change, so that a copy knows to refresh */
static uint64_t          s_fields_gen;
static bool              s_dirty;
static int               s_dirty_ticks;
static size_t            s_nroads;
static bool              s_show;
/* The district shown alone, or 0 */
static int               s_focus;
/* RGBA per cell, as the scripts set it; a cell with no alpha is not shaded */
static uint8_t          *s_lens;
static bool              s_show_lens;
static bool              s_lens_ready;
/* The cells a hub placed at the preview position would take over */
static bool              s_preview_on;
static int               s_preview_faction;
static vec2_t            s_preview_pos;
static bool             *s_preview_cells;
static bool             *s_preview_prev;
/* The overlay meshes to build again before they are next drawn, and the
 * player factions they were built for */
static bool              s_mesh_dirty[MAP_OVERLAY_MESHES];
static uint16_t          s_mesh_factions[MAP_OVERLAY_MESHES];
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

/* A district is as far as its workers walk in a while, so a road takes it
 * only as much further as they go faster on it */
static float step_cost(int layer, int from, int to, bool diagonal)
{
    float base = diagonal ? DIAGONAL : 1.0f;
    if(layer == LAYER_GROUND && s_road[from] && s_road[to]) {
        base /= G_Roads_SpeedBonus(false);
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

/* A hub reaches as far from its walls as any other, so every cell under its
 * footprint starts the flood. False when none of them is passable. */
static bool seed_footprint(int layer, uint32_t uid, int cluster, float *cost, int *owner, pq_cell_t *pq)
{
    struct obb obb;
    Entity_CurrentOBB(uid, &obb, true);

    float extent_x = 0.0f, extent_z = 0.0f;
    for(int i = 0; i < 3; i++) {
        extent_x += fabsf(obb.axes[i].x) * obb.half_lengths[i];
        extent_z += fabsf(obb.axes[i].z) * obb.half_lengths[i];
    }
    int c_min = MAX((int)floorf((s_map_pos.x - (obb.center.x + extent_x)) / CELL_SIZE), 0);
    int c_max = MIN((int)floorf((s_map_pos.x - (obb.center.x - extent_x)) / CELL_SIZE), s_cols - 1);
    int r_min = MAX((int)floorf(((obb.center.z - extent_z) - s_map_pos.z) / CELL_SIZE), 0);
    int r_max = MIN((int)floorf(((obb.center.z + extent_z) - s_map_pos.z) / CELL_SIZE), s_rows - 1);

    bool seeded = false;
    for(int r = r_min; r <= r_max; r++) {
    for(int c = c_min; c <= c_max; c++) {
        int idx = r * s_cols + c;
        if(!s_passable[layer][idx])
            continue;
        vec2_t center = cell_center(r, c);
        if(!C_PointInsideOBB((vec3_t){center.x, obb.center.y, center.z}, obb))
            continue;
        seed_cell(idx, 0.0f, cluster, cost, owner, pq);
        seeded = true;
    }}
    return seeded;
}

static void seed(int layer, uint32_t uid, int cluster, float *cost, int *owner, pq_cell_t *pq)
{
    int r, c;
    if(!cell_for_pos(G_Pos_GetXZ(uid), &r, &c))
        return;
    int idx = r * s_cols + c;
    if(s_passable[layer][idx]) {
        seed_cell(idx, 0.0f, cluster, cost, owner, pq);
    }
    if(seed_footprint(layer, uid, cluster, cost, owner, pq) || s_passable[layer][idx])
        return;
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
    s_mesh_dirty[MESH_DISTRICTS] = true;
    s_mesh_dirty[MESH_FOCUS] = true;
    s_fields_gen++;
}

static void recompute_preview(void)
{
    const size_t size = s_rows * s_cols * sizeof(bool);
    memcpy(s_preview_prev, s_preview_cells, size);
    preview_flood();
    if(memcmp(s_preview_prev, s_preview_cells, size)) {
        s_mesh_dirty[MESH_PREVIEW] = true;
    }
}

static void preview_flood(void)
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

static vec4_t overlay_color(vec3_t rgb, float alpha)
{
    return (vec4_t){rgb.x, rgb.y, rgb.z, alpha};
}

/* Over water, the overlay lies on its surface */
static float lifted(float y)
{
    return MAX(y, 0.0f) + OVERLAY_LIFT;
}

static void push_vert(vec_overlay_t *out, float x, float y, float z, vec4_t color)
{
    vec_overlay_push(out, (struct overlay_vert){(vec3_t){x, y, z}, color});
}

static void push_flat_rect(vec_overlay_t *tris, vec2_t min, vec2_t max, float y, vec4_t color)
{
    push_vert(tris, max.x, y, min.z, color);
    push_vert(tris, max.x, y, max.z, color);
    push_vert(tris, min.x, y, max.z, color);
    push_vert(tris, max.x, y, min.z, color);
    push_vert(tris, min.x, y, max.z, color);
    push_vert(tris, min.x, y, min.z, color);
}

/* The tile under the centre of each of the cell's tiles, row by row; false
 * where it is off the map */
static void cell_tiles(int r, int c, struct tile_desc descs[], struct tile *tiles[], bool have[])
{
    vec2_t center = cell_center(r, c);
    const float half = CELL_SIZE * 0.5f;
    for(int i = 0; i < TILES_PER_CELL; i++) {
    for(int j = 0; j < TILES_PER_CELL; j++) {
        int k = i * TILES_PER_CELL + j;
        vec2_t point = (vec2_t){
            center.x + half - (j + 0.5f) * X_COORDS_PER_TILE,
            center.z - half + (i + 0.5f) * Z_COORDS_PER_TILE
        };
        have[k] = M_Tile_DescForPoint2D(s_res, s_map_pos, point, &descs[k])
               && M_TileForDesc(s_map, descs[k], &tiles[k]);
    }}
}

static bool cell_level(struct tile *tiles[], const bool have[], int *out_height)
{
    for(int k = 0; k < TILES_PER_CELL * TILES_PER_CELL; k++) {
        if(!have[k] || tiles[k]->type != TILETYPE_FLAT || tiles[k]->base_height != tiles[0]->base_height)
            return false;
    }
    *out_height = tiles[0]->base_height;
    return true;
}

/* The cell's tint over the top faces of its tiles, following the ground up
 * and down cliffs, as one rectangle where the cell is level */
static void emit_cell_surface(vec_overlay_t *tris, int r, int c, vec4_t color)
{
    struct tile_desc descs[TILES_PER_CELL * TILES_PER_CELL];
    struct tile *tiles[TILES_PER_CELL * TILES_PER_CELL];
    bool have[TILES_PER_CELL * TILES_PER_CELL];
    cell_tiles(r, c, descs, tiles, have);

    vec2_t center = cell_center(r, c);
    const float half = CELL_SIZE * 0.5f;
    int height;
    if(cell_level(tiles, have, &height)) {
        push_flat_rect(tris, (vec2_t){center.x - half, center.z - half},
            (vec2_t){center.x + half, center.z + half}, lifted(height * Y_COORDS_PER_TILE), color);
        return;
    }

    for(int k = 0; k < TILES_PER_CELL * TILES_PER_CELL; k++) {
        if(!have[k])
            continue;
        if(tiles[k]->type == TILETYPE_FLAT) {
            int i = k / TILES_PER_CELL, j = k % TILES_PER_CELL;
            vec2_t max = (vec2_t){
                center.x + half - j * X_COORDS_PER_TILE,
                center.z - half + (i + 1) * Z_COORDS_PER_TILE
            };
            vec2_t min = (vec2_t){max.x - X_COORDS_PER_TILE, max.z - Z_COORDS_PER_TILE};
            push_flat_rect(tris, min, max, lifted(tiles[k]->base_height * Y_COORDS_PER_TILE), color);
            continue;
        }
        mat4x4_t model;
        M_ModelMatrixForChunk(s_map, (struct chunkpos){descs[k].chunk_r, descs[k].chunk_c}, &model);
        vec3_t pos[VERTS_PER_TOP_FACE], normals[VERTS_PER_TOP_FACE];
        int nverts = R_TileGetTopFace(s_map, &descs[k], &model, pos, normals);
        for(int v = 0; v < nverts; v++) {
            push_vert(tris, pos[v].x, lifted(pos[v].y), pos[v].z, color);
        }
    }
}

/* A side of a cell, in segments each at the height of the ground just
 * inside the cell, so that it follows the ground over steps and ramps */
static void emit_cell_edge(vec_overlay_t *lines, vec2_t from, vec2_t to, vec2_t inward,
                           int nsegs, bool dotted, vec4_t color)
{
    vec2_t along;
    PFM_Vec2_Sub(&to, &from, &along);
    PFM_Vec2_Normal(&along, &along);

    for(int k = 0; k < nsegs; k++) {
        if(dotted && (k % 2))
            continue;
        float t0 = (float)k / nsegs, t1 = (float)(k + 1) / nsegs;
        vec2_t a = (vec2_t){from.x + (to.x - from.x) * t0, from.z + (to.z - from.z) * t0};
        vec2_t b = (vec2_t){from.x + (to.x - from.x) * t1, from.z + (to.z - from.z) * t1};
        vec2_t sa = (vec2_t){a.x + (along.x + inward.x) * 0.5f, a.z + (along.z + inward.z) * 0.5f};
        vec2_t sb = (vec2_t){b.x + (inward.x - along.x) * 0.5f, b.z + (inward.z - along.z) * 0.5f};
        push_vert(lines, a.x, lifted(M_HeightAtPoint(s_map, sa)), a.z, color);
        push_vert(lines, b.x, lifted(M_HeightAtPoint(s_map, sb)), b.z, color);
    }
}

static void emit_cell(vec_overlay_t *tris, vec_overlay_t *lines, int r, int c, vec3_t rgb)
{
    emit_cell_surface(tris, r, c, overlay_color(rgb, 0.25f));

    struct tile_desc descs[TILES_PER_CELL * TILES_PER_CELL];
    struct tile *tiles[TILES_PER_CELL * TILES_PER_CELL];
    bool have[TILES_PER_CELL * TILES_PER_CELL];
    cell_tiles(r, c, descs, tiles, have);
    int height;
    int nsegs = cell_level(tiles, have, &height) ? 1 : TILES_PER_CELL;

    vec2_t center = cell_center(r, c);
    const float half = CELL_SIZE * 0.5f;
    vec2_t corners[4] = {
        {center.x + half, center.z - half},
        {center.x + half, center.z + half},
        {center.x - half, center.z + half},
        {center.x - half, center.z - half},
    };
    vec4_t color = overlay_color(rgb, 0.75f);
    for(int i = 0; i < 4; i++) {
        vec2_t from = corners[i], to = corners[(i + 1) % 4];
        vec2_t mid = (vec2_t){(from.x + to.x) * 0.5f, (from.z + to.z) * 0.5f};
        vec2_t inward;
        PFM_Vec2_Sub(&center, &mid, &inward);
        PFM_Vec2_Normal(&inward, &inward);
        emit_cell_edge(lines, from, to, inward, nsegs, false, color);
    }
}

static bool focus_cell(const struct field *f, int r, int c, bool core)
{
    if(r < 0 || r >= s_rows || c < 0 || c >= s_cols)
        return false;
    int idx = r * s_cols + c;
    return f->owner[idx] == s_focus && (!core || f->cost[idx] <= G_DISTRICT_TERRITORY_COST);
}

/* A cell of the district shown alone: a faint shade of its colour, stronger
 * over its own territory, and its sides on the district's edge in a solid
 * line, or on its own territory's in a dotted one */
static void emit_focus_cell(vec_overlay_t *tris, vec_overlay_t *lines, const struct field *f, int r, int c)
{
    bool core = focus_cell(f, r, c, true);
    emit_cell_surface(tris, r, c, overlay_color(cluster_color(s_focus, true), core ? 0.15f : 0.07f));

    /* The neighbours across the +X, +Z, -X and -Z sides, in the corners' order */
    static const int neighbours[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
    vec2_t center = cell_center(r, c);
    const float half = CELL_SIZE * 0.5f;
    vec2_t corners[4] = {
        {center.x + half, center.z - half},
        {center.x + half, center.z + half},
        {center.x - half, center.z + half},
        {center.x - half, center.z - half},
    };
    for(int i = 0; i < 4; i++) {
        int nr = r + neighbours[i][0], nc = c + neighbours[i][1];
        bool edge = !focus_cell(f, nr, nc, false);
        if(!edge && !(core && !focus_cell(f, nr, nc, true)))
            continue;
        vec2_t from = corners[i], to = corners[(i + 1) % 4];
        vec2_t mid = (vec2_t){(from.x + to.x) * 0.5f, (from.z + to.z) * 0.5f};
        vec2_t inward;
        PFM_Vec2_Sub(&center, &mid, &inward);
        PFM_Vec2_Normal(&inward, &inward);
        emit_cell_edge(lines, from, to, inward, DOTS_PER_SIDE, !edge, overlay_color(FOCUS_BORDER_RGB, 0.9f));
    }
}

/* Lays the districts, or the cells a hub would take, over the ground once,
 * for the render thread to keep drawing until they change */
static void build_mesh(int slot, uint16_t factions)
{
    vec_overlay_t tris, lines;
    vec_overlay_init(&tris);
    vec_overlay_init(&lines);

    if(slot == MESH_LENS) {
        for(int r = 0; s_lens && r < s_rows; r++) {
        for(int c = 0; c < s_cols; c++) {
            const uint8_t *p = &s_lens[(r * s_cols + c) * 4];
            if(p[3] == 0)
                continue;
            emit_cell_surface(&tris, r, c,
                (vec4_t){p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f});
        }}
    }

    for(int fac = 0; slot != MESH_LENS && fac < MAX_FACTIONS; fac++) {

        if(!(factions & (0x1 << fac)))
            continue;
        if(slot == MESH_PREVIEW && fac != s_preview_faction)
            continue;
        const struct field *f = &s_fields[fac][LAYER_GROUND];
        if((slot == MESH_DISTRICTS || slot == MESH_FOCUS) && !f->owner)
            continue;

        for(int r = 0; r < s_rows; r++) {
        for(int c = 0; c < s_cols; c++) {
            int idx = r * s_cols + c;
            if(slot == MESH_PREVIEW && s_preview_cells[idx]) {
                emit_cell(&tris, &lines, r, c, (vec3_t){1.0f, 1.0f, 1.0f});
            }else if(slot == MESH_DISTRICTS && f->owner[idx]) {
                emit_cell(&tris, &lines, r, c,
                    cluster_color(f->owner[idx], f->cost[idx] <= G_DISTRICT_TERRITORY_COST));
            }else if(slot == MESH_FOCUS && f->owner[idx] == s_focus) {
                emit_focus_cell(&tris, &lines, f, r, c);
            }
        }}
    }

    size_t ntri_verts = vec_size(&tris), nline_verts = vec_size(&lines);
    vec_overlay_concat(&tris, &lines);
    size_t total = vec_size(&tris);
    R_PushCmd((struct rcmd){
        .func = R_GL_MapOverlayMeshSet,
        .nargs = 4,
        .args = {
            R_PushArg(&slot, sizeof(slot)),
            total ? R_PushArg(tris.array, total * sizeof(struct overlay_vert)) : NULL,
            R_PushArg(&ntri_verts, sizeof(ntri_verts)),
            R_PushArg(&nline_verts, sizeof(nline_verts)),
        },
    });
    vec_overlay_destroy(&tris);
    vec_overlay_destroy(&lines);

    s_mesh_dirty[slot] = false;
    s_mesh_factions[slot] = factions;
}

static void draw_mesh(int slot)
{
    uint16_t factions = G_GetPlayerControlledFactions();
    if(s_mesh_dirty[slot] || s_mesh_factions[slot] != factions) {
        build_mesh(slot, factions);
    }
    R_PushCmd((struct rcmd){
        .func = R_GL_MapOverlayMeshDraw,
        .nargs = 1,
        .args = { R_PushArg(&slot, sizeof(slot)) },
    });
}

static void on_render_3d(void *user, void *event)
{
    if(s_show) {
        draw_mesh(MESH_DISTRICTS);
    }
    if(s_show_lens && s_lens) {
        draw_mesh(MESH_LENS);
    }
    if(s_preview_on) {
        draw_mesh(MESH_PREVIEW);
    }
    if(s_focus && !s_show) {
        draw_mesh(MESH_FOCUS);
    }
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
    s_res = res;
    s_cols = (int)ceilf(res.chunk_w * res.field_w / CELL_SIZE);
    s_rows = (int)ceilf(res.chunk_h * res.field_h / CELL_SIZE);
    const size_t ncells = s_rows * s_cols;

    memset(s_fields, 0, sizeof(s_fields));
    s_passable[LAYER_GROUND] = PF_CALLOC(ncells, sizeof(bool));
    s_passable[LAYER_WATER] = PF_CALLOC(ncells, sizeof(bool));
    s_road = PF_CALLOC(ncells, sizeof(bool));
    s_preview_cells = PF_CALLOC(ncells, sizeof(bool));
    s_preview_prev = PF_CALLOC(ncells, sizeof(bool));
    s_lens = PF_CALLOC(ncells, 4);
    s_show_lens = false;
    s_lens_ready = false;
    if(!s_passable[LAYER_GROUND] || !s_passable[LAYER_WATER] || !s_road || !s_preview_cells
    || !s_preview_prev || !s_lens) {
        G_District_Shutdown();
        return false;
    }

    s_dirty = true;
    s_dirty_ticks = DEBOUNCE_TICKS;
    s_nroads = 0;
    s_show = false;
    s_focus = 0;
    s_preview_on = false;
    s_ndistricts = 0;
    for(int i = 0; i < MAP_OVERLAY_MESHES; i++) {
        s_mesh_dirty[i] = true;
    }

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
    PF_FREE(s_preview_prev);
    PF_FREE(s_lens);
    s_passable[LAYER_GROUND] = s_passable[LAYER_WATER] = NULL;
    s_road = NULL;
    s_preview_cells = NULL;
    s_preview_prev = NULL;
    s_lens = NULL;
}

void G_District_Update(void)
{
    if(s_dirty) {
        recompute();
    }
}

/* A copy of the fields for the script thread, which reads them while the
 * main thread may recompute them */
struct district_snapshot{
    uint64_t     gen;
    vec3_t       map_pos;
    int          rows, cols;
    struct field fields[MAX_FACTIONS][NUM_LAYERS];
};

struct district_snapshot *G_District_SnapshotNew(void)
{
    struct district_snapshot *ret = PF_CALLOC(1, sizeof(struct district_snapshot));
    if(ret)
        ret->gen = ~(uint64_t)0;
    return ret;
}

void G_District_SnapshotFree(struct district_snapshot *snap)
{
    if(!snap)
        return;
    for(int fac = 0; fac < MAX_FACTIONS; fac++) {
    for(int layer = 0; layer < NUM_LAYERS; layer++) {
        PF_FREE(snap->fields[fac][layer].owner);
        PF_FREE(snap->fields[fac][layer].cost);
    }}
    PF_FREE(snap);
}

void G_District_SnapshotUpdate(struct district_snapshot *snap)
{
    ASSERT_IN_MAIN_THREAD();
    G_District_Update();
    if(snap->gen == s_fields_gen && snap->rows == s_rows && snap->cols == s_cols)
        return;

    size_t ncells = (size_t)s_rows * s_cols;
    bool resized = (snap->rows != s_rows || snap->cols != s_cols);
    snap->map_pos = s_map_pos;
    snap->rows = s_rows;
    snap->cols = s_cols;

    for(int fac = 0; fac < MAX_FACTIONS; fac++) {
    for(int layer = 0; layer < NUM_LAYERS; layer++) {
        const struct field *src = &s_fields[fac][layer];
        struct field *dst = &snap->fields[fac][layer];
        if(!src->owner || resized) {
            PF_FREE(dst->owner);
            PF_FREE(dst->cost);
            dst->owner = NULL;
            dst->cost = NULL;
        }
        if(!src->owner)
            continue;
        if(!dst->owner) {
            dst->owner = PF_MALLOC(ncells * sizeof(int));
            dst->cost = PF_MALLOC(ncells * sizeof(float));
            if(!dst->owner || !dst->cost)
                continue;
        }
        memcpy(dst->owner, src->owner, ncells * sizeof(int));
        memcpy(dst->cost, src->cost, ncells * sizeof(float));
    }}
    snap->gen = s_fields_gen;
}

int G_District_AtFrom(const struct district_snapshot *snap, int faction, bool water, 
                      vec2_t pos, float *out_cost)
{
    if(out_cost) {
        *out_cost = UNREACHED;
    }
    if(faction < 0 || faction >= MAX_FACTIONS)
        return 0;
    const struct field *f = &snap->fields[faction][water ? LAYER_WATER : LAYER_GROUND];
    int c = (int)floorf((snap->map_pos.x - pos.x) / CELL_SIZE);
    int r = (int)floorf((pos.z - snap->map_pos.z) / CELL_SIZE);
    if(!f->owner || r < 0 || r >= snap->rows || c < 0 || c >= snap->cols)
        return 0;
    int idx = r * snap->cols + c;
    if(out_cost) {
        *out_cost = f->cost[idx];
    }
    return f->owner[idx];
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

int G_District_HubOf(uint32_t building)
{
    int cluster = G_Cluster_Of(building);
    if(cluster == 0 || strcmp(G_Cluster_Type(cluster), HUB_TYPE))
        return 0;
    return cluster;
}

int G_District_Of(uint32_t uid)
{
    return G_District_At(G_GetFactionID(uid), layer_for_entity(uid) == LAYER_WATER, 
        G_Pos_GetXZ(uid), NULL);
}

int G_District_OfNode(int faction, uint32_t node, float *out_cost)
{
    vec2_t pos = G_Pos_GetXZ(node);
    if(!(G_FlagsGet(node) & ENTITY_FLAG_WATER))
        return G_District_At(faction, false, pos, out_cost);

    float reach = G_GetSelectionRadius(node) + X_COORDS_PER_TILE;
    float best_cost = UNREACHED;
    int ret = 0;
    for(int i = 0; i < 16; i++) {
        float angle = i * (2.0f * M_PI / 16);
        vec2_t probe = (vec2_t){pos.x + cosf(angle) * reach, pos.z + sinf(angle) * reach};
        float cost;
        int owner = G_District_At(faction, false, probe, &cost);
        if(owner && cost < best_cost) {
            best_cost = cost;
            ret = owner;
        }
    }
    if(out_cost) {
        *out_cost = best_cost;
    }
    return ret;
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

bool G_District_LensGrid(int *out_rows, int *out_cols)
{
    if(!s_lens)
        return false;
    *out_rows = s_rows;
    *out_cols = s_cols;
    return true;
}

vec2_t G_District_CellCenter(int r, int c)
{
    return cell_center(r, c);
}

bool G_District_SetLens(const uint8_t *rgba, size_t ncells)
{
    if(!s_lens || ncells != (size_t)(s_rows * s_cols))
        return false;
    memcpy(s_lens, rgba, ncells * 4);
    s_lens_ready = true;
    s_mesh_dirty[MESH_LENS] = true;
    return true;
}

bool G_District_LensReady(void)
{
    return s_lens_ready;
}

void G_District_ShowLens(bool on)
{
    s_show_lens = on;
}

bool G_District_LensShown(void)
{
    return s_show_lens;
}

void G_District_SetFocus(int district)
{
    if(district == s_focus)
        return;
    s_focus = district;
    s_mesh_dirty[MESH_FOCUS] = true;
}

int G_District_GetFocus(void)
{
    return s_focus;
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
