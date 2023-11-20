/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2023 Eduard Permyakov 
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

#include "formation.h"
#include "position.h"
#include "../main.h"
#include "../event.h"
#include "../settings.h"
#include "../perf.h"
#include "../camera.h"
#include "../sched.h"
#include "../task.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"
#include "../lib/public/vec.h"
#include "../lib/public/khash.h"
#include "../lib/public/queue.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/stalloc.h"
#include "../navigation/public/nav.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"

#include <SDL.h>
#include <assert.h>

#define COLUMN_WIDTH_RATIO       (4.0f)
#define RANK_WIDTH_RATIO         (0.25f)
#define OCCUPIED_FIELD_RES       (95) /* Must be odd */
#define CELL_ARRIVAL_FIELD_RES   (OCCUPIED_FIELD_RES + 1) /* Must be even */
#define MAX_CHILDREN             (16)
#define CELL_IDX(_r, _c, _ncols) ((_r) * (_ncols) + (_c))
#define ARR_SIZE(a)              (sizeof(a)/sizeof(a[0]))
#define MIN(a, b)                ((a) < (b) ? (a) : (b))
#define MAX(a, b)                ((a) > (b) ? (a) : (b))
#define CLAMP(a, min, max)       (MIN(MAX((a), (min)), (max)))
#define UNIT_BUFFER_DIST         (1.0f)
#define SUBFORMATION_BUFFER_DIST (8.0f)
#define SIGNUM(x)                (((x) > 0) - ((x) < 0))

enum cell_state{
    CELL_NOT_PLACED,
    CELL_OCCUPIED,
    CELL_NOT_OCCUPIED
};

enum tile_state{
    TILE_FREE,
    TILE_VISITED,
    TILE_BLOCKED,
    TILE_ALLOCATED
};

enum direction{
    DIR_FRONT = (1 << 0),
    DIR_BACK = (1 << 1),
    DIR_LEFT = (1 << 2),
    DIR_RIGHT = (1 << 3)
};

struct coord{
    int r, c;
};

struct cell{
    enum cell_state state;
    /* The desired positoin of the cell based 
     * on the positions of the neighbouring 
     * cells and anchor.
     */
    vec2_t          ideal_raw;
    /* The ideal position binned to a tile.
     */
    vec2_t          ideal_binned;
    /* The final position of the cell, taking
     * into account map geometry and blockers.
     */
    vec2_t          pos;
};

struct range2d{
    int min_r, max_r;
    int min_c, max_c;
};

struct cell_arrival_field{
    struct{
        unsigned dir_idx : 4;
    }field[CELL_ARRIVAL_FIELD_RES][CELL_ARRIVAL_FIELD_RES];
};

VEC_TYPE(cell, struct cell)
VEC_IMPL(static inline, cell, struct cell)

struct cell_field_work{
    bool                      consumed;
    uint32_t                  tid;
    uint32_t                  uid;
    struct future             future;
    struct cell_arrival_field result;
};

VEC_TYPE(work, struct cell_field_work)
VEC_IMPL(static inline, work, struct cell_field_work)

KHASH_MAP_INIT_INT(assignment, struct coord)
KHASH_MAP_INIT_INT(result, struct cell_arrival_field*)

QUEUE_TYPE(coord, struct coord)
QUEUE_IMPL(static, coord, struct coord)

enum formation_type{
    FORMATION_RANK,
    FORMATION_COLUMN
};

struct subformation{
    /* Subformations are stored in an acyclic tree structure
     * and are placed relative to their parent with some constaints.
     */
    struct subformation *parent;
    size_t               nchildren;
    struct subformation *children[MAX_CHILDREN];
    float                unit_radius;
    enum nav_layer       layer;
    vec2_t               reachable_target;
    vec2_t               pos;
    vec2_t               orientation;
    size_t               nrows;
    size_t               ncols;
    khash_t(entity)     *ents;
    /* Each cell holds a single unit from the subformation
     */
    vec_cell_t           cells;
    /* A mapping between entities and a cell within the formation 
     */
    khash_t(assignment) *assignment;
    /* A future for the task responsible for computing the 
     * cell arrival field. Each task is responsible for computing
     * a single field, so there is one task per entity/cell.
     */
    khash_t(result)     *results;
    vec_work_t           futures;
};

VEC_TYPE(subformation, struct subformation)
VEC_IMPL(static inline, subformation, struct subformation)

struct formation{
    /* The refcount is the number of movement system
     * 'flocks' associated with this formation.
     */
    int                  refcount;
    enum formation_type  type;
    vec2_t               target;
    vec2_t               orientation;
    vec2_t               center;
    khash_t(entity)     *ents;
    /* A mapping between entities and subformations 
     */
    khash_t(assignment) *sub_assignment;
    /* The subformation tree
     */
    struct subformation *root;
    vec_subformation_t   subformations;
    /* The map tiles which have already been allocated to cells.
     * Centered at the target position.
     */
    uint8_t              occupied[NAV_LAYER_MAX][OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES];
    /* A copy of the navigation 'island' field for the area specified
     * by the 'occupied' field.
     */
    uint16_t             islands[NAV_LAYER_MAX][OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES];
};

KHASH_MAP_INIT_INT(formation, struct formation)
KHASH_MAP_INIT_INT(mapping, formation_id_t);

static void complete_cell_field_work(struct subformation *formation);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map   *s_map;
static khash_t(mapping)   *s_ent_formation_map;
static khash_t(formation) *s_formations;
static formation_id_t      s_next_id;
static SDL_TLSID           s_workspace;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void *get_workspace(void)
{
    void *ret;
    if((ret = SDL_TLSGet(s_workspace)))
        return ret;

    size_t workspace_size = CELL_ARRIVAL_FIELD_RES * CELL_ARRIVAL_FIELD_RES * sizeof(float);
    ret = malloc(workspace_size);
    if(!ret)
        return NULL;
    if(0 != SDL_TLSSet(s_workspace, ret, free)) {
        free(ret);
        return NULL;
    }
    return ret;
}

static size_t ncols(enum formation_type type, size_t nunits)
{
    switch(type) {
    case FORMATION_RANK:
        return MIN(ceilf(sqrtf(nunits / RANK_WIDTH_RATIO)), nunits);
    case FORMATION_COLUMN:
        return MIN(ceilf(sqrtf(nunits / COLUMN_WIDTH_RATIO)), nunits);
    default: assert(0);
    }
}

static size_t nrows(enum formation_type type, size_t nunits)
{
    return ceilf(nunits / ncols(type, nunits));
}

static vec2_t compute_orientation(vec2_t target, const vec_entity_t *ents)
{
    uint32_t curr;
    vec2_t COM = (vec2_t){0.0f, 0.0f};
    for(int i = 0; i < vec_size(ents); i++) {
        uint32_t curr = vec_AT(ents, i);
        vec2_t curr_pos = G_Pos_GetXZ(curr);
        PFM_Vec2_Add(&COM, &curr_pos, &COM);
    }
    size_t nents = vec_size(ents);
    PFM_Vec2_Scale(&COM, 1.0f / nents, &COM);

    vec2_t orientation;
    PFM_Vec2_Sub(&target, &COM, &orientation);
    PFM_Vec2_Normal(&orientation, &orientation);
    return orientation;
}

/* Shift the field center in the opposite direction of the 
 * formations' orientation. Since units are placed 'behind' the 
 * target, this allows to get better utilization of the 
 * field.
 */
static vec2_t field_center(vec2_t target, vec2_t orientation)
{
    struct map_resolution nav_res;
    M_NavGetResolution(s_map, &nav_res);
    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float tile_x_dim = chunk_x_dim / nav_res.tile_w;

    int delta_mag = OCCUPIED_FIELD_RES / 3.0f * tile_x_dim;
    vec2_t delta = orientation;
    PFM_Vec2_Normal(&delta, &delta);
    PFM_Vec2_Scale(&delta, delta_mag, &delta);

    vec2_t center = target;
    PFM_Vec2_Sub(&center, &delta, &center);
    center = M_ClampedMapCoordinate(s_map, center);
    return center;
}

static bool try_occupy_cell(struct coord *curr, vec2_t orientation, uint16_t iid,
                            float radius, enum nav_layer layer, int anchor,
                            uint8_t occupied[NAV_LAYER_MAX][OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES],
                            uint16_t islands[OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES])
{
    struct map_resolution nav_res;
    M_NavGetResolution(s_map, &nav_res);

    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    const float tile_x_dim = chunk_x_dim / nav_res.tile_w;
    const float tile_z_dim = chunk_z_dim / nav_res.tile_h;

    const float field_x_dim = tile_x_dim * OCCUPIED_FIELD_RES;
    const float field_z_dim = tile_z_dim * OCCUPIED_FIELD_RES;

    struct map_resolution res = (struct map_resolution){
        1, 1, OCCUPIED_FIELD_RES, OCCUPIED_FIELD_RES,
        field_x_dim, field_z_dim
    };
    /* Find the center point of the tile, in field-local coordinates */
    vec2_t center = (vec2_t){
        (curr->c + 0.5f) * -tile_x_dim,
        (curr->r + 0.5f) *  tile_z_dim
    };
    vec3_t origin = (vec3_t){0.0f, 0.0f, 0.0f};

    struct tile_desc descs[256];
    size_t ndescs = M_Tile_AllUnderCircle(res, center, radius, origin, descs, ARR_SIZE(descs));
    if(ndescs == 0)
        return false;

    for(int i = 0; i < ndescs; i++) {
        struct coord coord = (struct coord){descs[i].tile_r, descs[i].tile_c};
        if(islands[coord.r][coord.c] != iid)
            return false;
        if(occupied[layer][coord.r][coord.c] != TILE_FREE
        && occupied[layer][coord.r][coord.c] != TILE_VISITED)
            return false;
    }
    for(int i = 0; i < ndescs; i++) {
        struct coord coord = (struct coord){descs[i].tile_r, descs[i].tile_c};
        for(int j = 0; j < NAV_LAYER_MAX; j++) {
            occupied[j][coord.r][coord.c] = TILE_ALLOCATED;
        }
    }
    return true;
}

static vec2_t tile_to_pos(struct coord tile, vec2_t center)
{
    struct map_resolution nav_res;
    M_NavGetResolution(s_map, &nav_res);

    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    const float tile_x_dim = chunk_x_dim / nav_res.tile_w;
    const float tile_z_dim = chunk_z_dim / nav_res.tile_h;

    vec2_t tile_center = (vec2_t){
        ((int)(center.x / tile_x_dim)) * tile_x_dim,
        ((int)(center.z / tile_z_dim)) * tile_z_dim,
    };

    vec2_t offset = (vec2_t) {
         tile_x_dim * (tile.c - OCCUPIED_FIELD_RES/2 + 0.5f * SIGNUM(center.x)),
        -tile_z_dim * (tile.r - OCCUPIED_FIELD_RES/2 - 0.5f * SIGNUM(center.z))
    };

    vec2_t ret = tile_center;
    PFM_Vec2_Add(&ret, &offset, &ret);
    return ret;
}

static struct coord pos_to_tile(vec2_t center, vec2_t pos)
{
    struct map_resolution nav_res;
    M_NavGetResolution(s_map, &nav_res);

    vec2_t tile_center = tile_to_pos((struct coord){
        OCCUPIED_FIELD_RES/2,
        OCCUPIED_FIELD_RES/2
    }, center);

    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    const float tile_x_dim = chunk_x_dim / nav_res.tile_w;
    const float tile_z_dim = chunk_z_dim / nav_res.tile_h;

    vec2_t binned_pos = (vec2_t){
        ((int)(pos.x) / tile_x_dim) * tile_x_dim,
        ((int)(pos.z) / tile_z_dim) * tile_z_dim,
    };
    vec2_t delta;
    PFM_Vec2_Sub(&binned_pos, &tile_center, &delta);

    float dc =  delta.x / tile_x_dim + 0.5f;
    float dr = -delta.z / tile_z_dim + 0.5f;

    return (struct coord){
        OCCUPIED_FIELD_RES/2 + dr,
        OCCUPIED_FIELD_RES/2 + dc
    };
}

static vec2_t bin_to_tile(vec2_t pos, vec2_t center)
{
    struct coord tile = pos_to_tile(center, pos);
    return tile_to_pos(tile, center);
}

/* The distance we need to march in order to 
 * be guaranteed to arrive at a new tile of the
 * grid when going along a particular vector.
 */
static float step_distance(vec2_t orientation, float base)
{
    vec2_t positive = (vec2_t){
        fabsf(orientation.x),
        fabsf(orientation.z)
    };
    vec2_t diagonal = (vec2_t){1.0f, 1.0f};
    float dot = PFM_Vec2_Dot(&positive, &diagonal);
    float max = PFM_Vec2_Dot(&diagonal, &diagonal);
    float fraction = (dot / max) - 0.5f;
    return (1.0f + fraction * sqrtf(2.0f)) * base;
}

static bool nearest_free_tile(struct coord *curr, struct coord *out, uint16_t iid,
                              int direction_mask, vec2_t center, vec2_t orientation,
                              uint8_t occupied[OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES],
                              uint16_t islands[OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES])
{
    if(occupied[curr->r][curr->c] == TILE_FREE) {
        *out = *curr;
        return true;
    }

    /* First, attempt to take a step based on the direction mask.
     * This will more often yield tiles positions that are organized
     * into a perfect grid. 
     */

    struct map_resolution nav_res;
    M_NavGetResolution(s_map, &nav_res);
    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float tile_x_dim = chunk_x_dim / nav_res.tile_w;

    vec2_t unit_orientation = orientation;
    PFM_Vec2_Normal(&unit_orientation, &unit_orientation);
    float ulen = step_distance(orientation, tile_x_dim);
    PFM_Vec2_Scale(&unit_orientation, ulen, &unit_orientation);
    vec2_t unit_perpendicular = (vec2_t){-unit_orientation.z, unit_orientation.x};

    vec2_t delta = (vec2_t){0.0f, 0.0f};
    if(direction_mask & DIR_FRONT) {
        PFM_Vec2_Add(&delta, &unit_orientation, &delta);
    }
    if(direction_mask & DIR_BACK) {
        PFM_Vec2_Sub(&delta, &unit_orientation, &delta);
    }
    if(direction_mask & DIR_LEFT) {
        PFM_Vec2_Sub(&delta, &unit_perpendicular, &delta);
    }
    if(direction_mask & DIR_RIGHT) {
        PFM_Vec2_Add(&delta, &unit_perpendicular, &delta);
    }

    vec2_t candidate_pos = tile_to_pos(*curr, center);
    vec2_t shifted_pos;
    PFM_Vec2_Add(&candidate_pos, &delta, &shifted_pos);
    struct coord test_tile = pos_to_tile(center, shifted_pos);

    if(test_tile.r != curr->r || test_tile.c != curr->c) {
        if((test_tile.r >= 0 && test_tile.r < OCCUPIED_FIELD_RES)
        && (test_tile.c >= 0 && test_tile.c < OCCUPIED_FIELD_RES)
        && (islands[test_tile.r][test_tile.c] == iid)
        && (occupied[test_tile.r][test_tile.c] == TILE_FREE)) {
            *out = test_tile;
            return true;
        }
    }

    /* If we could not place the tile at a specific offset, fall back
     * to a brute-force breadth-first search.
     */
    for(int delta = 1; delta < OCCUPIED_FIELD_RES; delta++) {
        for(int dr = -delta; dr <= +delta; dr++) {
        for(int dc = -delta; dc <= +delta; dc++) {
            if(abs(dr) != delta || abs(dc) != delta)
                continue;

            int abs_r = curr->r + dr;
            int abs_c = curr->c + dc;
            if(abs_r <= 0 || abs_r >= OCCUPIED_FIELD_RES)
                continue;
            if(abs_c <= 0 || abs_c >= OCCUPIED_FIELD_RES)
                continue;

            bool free = (occupied[abs_r][abs_c] == TILE_FREE);
            bool islands_match = (islands[abs_r][abs_c] == iid);
            if(free && islands_match) {
                *out = (struct coord){abs_r, abs_c};
                return true;
            }
        }}
    }
    return false;
}

static bool any_match(struct tile_desc *a, size_t numa, 
                      struct tile_desc *b, size_t numb)
{
    for(int i = 0; i < numa; i++) {
    for(int j = 0; j < numb; j++) {
        if((a[i].tile_r == b[j].tile_r) && (a[i].tile_c == b[j].tile_c))
            return true;
    }}
    return false;
}

/* Find the X and Y offsets between adjacent in a formation, given
 * that there are no obstacles. These cannot be computed from the 
 * unit radiuses because of the grid-based nature of the 'occupied'
 * field. 
 */
static vec2_t target_direction_offsets(vec2_t center, vec2_t orientation, 
                                       struct subformation *formation)
{
    struct map_resolution nav_res;
    M_NavGetResolution(s_map, &nav_res);

    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    const float tile_x_dim = chunk_x_dim / nav_res.tile_w;
    const float tile_z_dim = chunk_z_dim / nav_res.tile_h;

    const float field_x_dim = tile_x_dim * OCCUPIED_FIELD_RES;
    const float field_z_dim = tile_z_dim * OCCUPIED_FIELD_RES;

    struct map_resolution res = (struct map_resolution){
        1, 1, OCCUPIED_FIELD_RES, OCCUPIED_FIELD_RES,
        field_x_dim, field_z_dim
    };

    /* First find the set of tiles occupied by the root tile */
    vec3_t origin = (vec3_t){0.0f, 0.0f, 0.0f};
    struct coord root_tile = (struct coord){
        OCCUPIED_FIELD_RES/2,
        OCCUPIED_FIELD_RES/2
    };
    vec2_t root_center = (vec2_t){
        (root_tile.c + 0.5f) * -tile_x_dim,
        (root_tile.r + 0.5f) *  tile_z_dim
    };
    struct tile_desc descs[256];
    size_t ndescs = M_Tile_AllUnderCircle(res, root_center, formation->unit_radius, 
        origin, descs, ARR_SIZE(descs));

    /* Place a tile immediately to the front of this tile. Start with the
     * minimum possible distance and go forward in unit-sized increments 
     * along the direction vector.
     */
    float minimal_distance = formation->unit_radius * 2 + UNIT_BUFFER_DIST;
    float unit_distance = step_distance(orientation, tile_x_dim);
    float front_distance = INFINITY;

    vec2_t unit_delta = orientation;
    PFM_Vec2_Normal(&unit_delta, &unit_delta);
    PFM_Vec2_Scale(&unit_delta, unit_distance, &unit_delta);

    vec2_t min_delta = orientation;
    PFM_Vec2_Normal(&min_delta, &min_delta);
    PFM_Vec2_Scale(&min_delta, minimal_distance, &min_delta);

    vec2_t candidate = root_center;
    PFM_Vec2_Add(&candidate, &min_delta, &candidate);
    candidate = bin_to_tile(candidate, center);

    do{
        struct tile_desc front_descs[256];
        size_t front_ndescs = M_Tile_AllUnderCircle(res, candidate, formation->unit_radius, 
            origin, front_descs, ARR_SIZE(front_descs));

        if(!any_match(descs, ndescs, front_descs, front_ndescs)) {
            vec2_t diff;
            PFM_Vec2_Sub(&candidate, &root_center, &diff);
            front_distance = PFM_Vec2_Len(&diff);
        }else{
            PFM_Vec2_Add(&candidate, &unit_delta, &candidate);
        }
    }while(front_distance == INFINITY);

    /* Now place a tile immediately to the right */
    float right_distance = INFINITY;

    unit_delta = (vec2_t){-orientation.z, orientation.x};
    PFM_Vec2_Normal(&unit_delta, &unit_delta);
    PFM_Vec2_Scale(&unit_delta, unit_distance, &unit_delta);

    min_delta = (vec2_t){-orientation.z, orientation.x};
    PFM_Vec2_Normal(&min_delta, &min_delta);
    PFM_Vec2_Scale(&min_delta, minimal_distance, &min_delta);

    candidate = root_center;
    PFM_Vec2_Add(&candidate, &min_delta, &candidate);

    do{
        struct tile_desc right_descs[256];
        size_t right_ndescs = M_Tile_AllUnderCircle(res, candidate, formation->unit_radius, 
            origin, right_descs, ARR_SIZE(right_descs));

        if(!any_match(descs, ndescs, right_descs, right_ndescs)) {
            vec2_t diff;
            PFM_Vec2_Sub(&candidate, &root_center, &diff);
            right_distance = PFM_Vec2_Len(&diff);
        }else{
            PFM_Vec2_Add(&candidate, &unit_delta, &candidate);
        }
    }while(right_distance == INFINITY);

    return (vec2_t){front_distance, right_distance};
}

static bool place_cell(struct cell *curr, vec2_t center, vec2_t root, 
                       vec2_t target, vec2_t orientation, float radius, 
                       enum nav_layer layer, vec2_t target_offsets,
                       const struct cell *left, const struct cell *right,
                       const struct cell *front,  const struct cell *back,
                       uint8_t occupied[NAV_LAYER_MAX][OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES],
                       uint16_t islands[NAV_LAYER_MAX][OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES])
{
    int anchor = 0;
    if(left && (left->state != CELL_NOT_PLACED))
        anchor |= DIR_LEFT;
    if(right && (right->state != CELL_NOT_PLACED))
        anchor |= DIR_RIGHT;
    if(front && (front->state != CELL_NOT_PLACED))
        anchor |= DIR_FRONT;
    if(back && (back->state != CELL_NOT_PLACED))
        anchor |= DIR_BACK;

    /* First find a "target" position based on direction and the positions of existing cells 
     */
    vec2_t pos = (vec2_t){0.0f, 0.0f};
    int count = 0;

    if(anchor == 0) {
        pos = bin_to_tile(root, center);
    }

    if(anchor & DIR_LEFT) {

        vec2_t pdir = (vec2_t){-orientation.z, orientation.x};
        PFM_Vec2_Normal(&pdir, &pdir);

        float delta = -target_offsets.y;
        PFM_Vec2_Scale(&pdir, delta, &pdir);

        vec2_t target = left->pos;
        PFM_Vec2_Add(&target, &pdir, &target);

        PFM_Vec2_Add(&target, &pos, &pos);
        count++;
    }

    if(anchor & DIR_RIGHT) {

        vec2_t pdir = (vec2_t){-orientation.z, orientation.x};
        PFM_Vec2_Normal(&pdir, &pdir);

        float delta = target_offsets.y;
        PFM_Vec2_Scale(&pdir, delta, &pdir);

        vec2_t target = right->pos;
        PFM_Vec2_Add(&target, &pdir, &target);

        PFM_Vec2_Add(&target, &pos, &pos);
        count++;
    }

    if(anchor & DIR_FRONT) {

        vec2_t front_dir = orientation;
        PFM_Vec2_Normal(&front_dir, &front_dir);

        float delta = target_offsets.x;
        PFM_Vec2_Scale(&front_dir, delta, &front_dir);

        vec2_t target = front->pos;
        PFM_Vec2_Add(&target, &front_dir, &target);

        PFM_Vec2_Add(&target, &pos, &pos);
        count++;
    }

    if(anchor & DIR_BACK) {

        vec2_t front = orientation;
        PFM_Vec2_Normal(&front, &front);

        float delta = -target_offsets.x;
        PFM_Vec2_Scale(&front, delta, &front);

        vec2_t target = back->pos;
        PFM_Vec2_Add(&target, &front, &target);

        PFM_Vec2_Add(&target, &pos, &pos);
        count++;
    }

    if(count > 0) {
        PFM_Vec2_Scale(&pos, 1.0f / count, &pos);
    }

    /* Find the target tile for the position */
    struct coord target_tile = pos_to_tile(center, pos);
    struct coord curr_tile;

    struct coord dest_coord = pos_to_tile(center, target);
    uint16_t iid = islands[layer][dest_coord.r][dest_coord.c];
    assert(iid != UINT16_MAX);

    bool exists = nearest_free_tile(&target_tile, &curr_tile, iid, anchor, 
        center, orientation, occupied[layer], islands[layer]);
    if(!exists)
        return false;

    size_t nvisited = 0;
    struct coord visited[OCCUPIED_FIELD_RES * OCCUPIED_FIELD_RES];
    /* Do a breath-first traversal of the 'occupied' field and greedily place 
     * each cell. If we are not able to place a cell, mark that candidate tile
     * as 'visited'
     */
    bool success = false;
    do{
        success = try_occupy_cell(&curr_tile, orientation, iid, radius, layer, 
            anchor, occupied, islands[layer]);
        if(!success) {
            occupied[layer][curr_tile.r][curr_tile.c] = TILE_VISITED;
            visited[nvisited++] = curr_tile;
            bool exists = nearest_free_tile(&curr_tile, &curr_tile, iid, anchor, 
                center, orientation, occupied[layer], islands[layer]);
            if(!exists)
                break;
        }
    }while(!success);

    /* Reset the 'visited' tiles */
    for(int i = 0; i < nvisited; i++) {
        if(occupied[layer][visited[i].r][visited[i].c] == TILE_VISITED)
            occupied[layer][visited[i].r][visited[i].c] = TILE_FREE;
    }
    if(success) {
        curr->ideal_raw = pos;
        curr->ideal_binned = tile_to_pos(target_tile, center);
        curr->state = CELL_NOT_OCCUPIED;
        curr->pos = tile_to_pos(curr_tile, center);
    }
    return success;
}

static void init_occupied_field(enum nav_layer layer, vec2_t center,
                                uint8_t occupied[OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES])
{
    PERF_ENTER();

    struct map_resolution res;
    M_NavGetResolution(s_map, &res);
    vec3_t map_pos = M_GetPos(s_map);

    struct tile_desc center_tile;
    M_Tile_DescForPoint2D(res, map_pos, center, &center_tile);

    struct coord center_coord = (struct coord){
        OCCUPIED_FIELD_RES / 2,
        OCCUPIED_FIELD_RES / 2
    };

    memset(occupied, 0, OCCUPIED_FIELD_RES * OCCUPIED_FIELD_RES);
    for(int r = 0; r < OCCUPIED_FIELD_RES; r++) {
    for(int c = 0; c < OCCUPIED_FIELD_RES; c++) {

        int dr = center_coord.r - r;
        int dc = center_coord.c - c;
        struct tile_desc curr = center_tile;
        bool exists = M_Tile_RelativeDesc(res, &curr, dc, dr);
        if(!exists) {
            occupied[r][c] = TILE_BLOCKED;
            continue;
        }

        struct box bounds = M_Tile_Bounds(res, map_pos, curr);
        vec2_t center = (vec2_t){
            bounds.x - bounds.width / 2.0f,
            bounds.z + bounds.height / 2.0f
        };
        if(!M_NavPositionPathable(s_map, layer, center)
        ||  M_NavPositionBlocked(s_map, layer, center)) {
            occupied[r][c] = TILE_BLOCKED;
            continue;
        }
    }}

    PERF_RETURN_VOID();
}

static void init_islands_field(enum nav_layer layer, vec2_t center,
                               uint16_t islands[OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES])
{
    M_NavCopyIslandsFieldView(s_map, center, OCCUPIED_FIELD_RES, OCCUPIED_FIELD_RES,
        layer, (uint16_t*)islands);
}

static vec2_t back_row_average_pos(struct subformation *formation)
{
    size_t row = 0;
    vec2_t total = (vec2_t){0.0f, 0.0f};
    for(int i = 0; i < formation->ncols; i++) {
        struct cell *curr = &vec_AT(&formation->cells, CELL_IDX(row, i, formation->ncols));
        PFM_Vec2_Add(&total, &curr->pos, &total);
    }
    PFM_Vec2_Scale(&total, 1.0f / formation->ncols, &total);
    return total;
}

static float subformation_offset(struct subformation *formation)
{
    struct map_resolution nav_res;
    M_NavGetResolution(s_map, &nav_res);
    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float tile_x_dim = chunk_x_dim / nav_res.tile_w;

    float buffer = step_distance(formation->orientation, 
        formation->unit_radius);
    buffer = ((int)(buffer / tile_x_dim) + 1) * tile_x_dim;
    buffer *= 2;
    buffer += step_distance(formation->orientation, SUBFORMATION_BUFFER_DIST);
    return buffer;
}

static vec2_t subformation_target_pos(struct subformation *formation, vec2_t target, 
                                      vec2_t orientation, vec2_t offsets)
{
    if(!formation->parent)
        return target;

    vec2_t back_pos = back_row_average_pos(formation->parent);
    float offset = subformation_offset(formation->parent);

    vec2_t delta = orientation;
    PFM_Vec2_Normal(&orientation, &orientation);
    PFM_Vec2_Scale(&delta, -offset, &delta);

    vec2_t ret = back_pos;
    PFM_Vec2_Add(&ret, &delta, &ret);
    return ret;
}

static vec2_t formation_center(struct subformation *formation)
{
    vec2_t ret = (vec2_t){0.0f, 0.0f};
    size_t nents = kh_size(formation->ents);

    for(int r = 0; r < formation->nrows; r++) {
    for(int c = 0; c < formation->ncols; c++) {
        size_t cell_idx = CELL_IDX(r, c, formation->ncols);
        struct cell *cell = &vec_AT(&formation->cells, cell_idx);
        if(cell->state != CELL_NOT_OCCUPIED)
            continue;
        PFM_Vec2_Add(&ret, &cell->pos, &ret);
    }}
    PFM_Vec2_Scale(&ret, 1.0f / nents, &ret);
    return ret;
}

static void place_subformation(struct subformation *formation, vec2_t center, 
    vec2_t target, vec2_t orientation,
    uint8_t occupied[NAV_LAYER_MAX][OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES],
    uint16_t islands[NAV_LAYER_MAX][OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES])
{
    PERF_ENTER();

    vec2_t target_orientation = orientation;
    vec2_t target_offsets = target_direction_offsets(center, orientation, formation);
    vec2_t target_pos = subformation_target_pos(formation, target, orientation, target_offsets);

    int nrows = formation->nrows;
    int ncols = formation->ncols;

    /* Start by placing the center-most front row cell, Position the cells on
     * pathable and unbostructed terrain.
     */
    struct coord init_cell = (struct coord){
        .r = nrows - 1,
        .c = ncols / 2
    };

     /* Then traverse the cell grid outwards in a breadth-first 
      * manner.
      */
    queue_coord_t frontier;
    queue_coord_init(&frontier, nrows * ncols);
    queue_coord_push(&frontier, &init_cell);

    struct coord deltas[] = {
        { 0, -1},
        { 0, +1},
        {-1,  0},
        {+1,  0},
    };

    size_t nents = kh_size(formation->ents);
    size_t placed = 0;
    while(queue_size(frontier) > 0 && (placed < (nrows * ncols))) {

        struct coord curr;
        queue_coord_pop(&frontier, &curr);
        struct cell *curr_cell = &vec_AT(&formation->cells, CELL_IDX(curr.r, curr.c, ncols));

        if(curr_cell->state == CELL_NOT_OCCUPIED)
            continue;

        struct coord front = (struct coord){curr.r - 1, curr.c};
        struct coord back = (struct coord){curr.r + 1, curr.c};
        struct coord left = (struct coord){curr.r, curr.c - 1};
        struct coord right = (struct coord){curr.r, curr.c + 1};

        struct cell *front_cell = (front.r >= 0) 
                              ? &vec_AT(&formation->cells, CELL_IDX(front.r, front.c, ncols)) 
                              : NULL;
        struct cell *back_cell = (back.r < nrows) 
                              ? &vec_AT(&formation->cells, CELL_IDX(back.r, back.c, ncols)) 
                              : NULL;
        struct cell *left_cell = (left.c >= 0) 
                               ? &vec_AT(&formation->cells, CELL_IDX(left.r, left.c, ncols))
                               : NULL;
        struct cell *right_cell = (right.c < ncols) 
                                ? &vec_AT(&formation->cells, CELL_IDX(right.r, right.c, ncols))
                                : NULL;

        bool success = place_cell(curr_cell, center, target_pos, 
            formation->reachable_target, orientation, formation->unit_radius, 
            formation->layer, target_offsets, left_cell, right_cell, front_cell, back_cell, 
            occupied, islands);
        if(!success)
            break;

        if(left_cell && left_cell->state == CELL_NOT_PLACED)
            queue_coord_push(&frontier, &left);
        if(right_cell && right_cell->state == CELL_NOT_PLACED)
            queue_coord_push(&frontier, &right);
        if(front_cell && front_cell->state == CELL_NOT_PLACED)
            queue_coord_push(&frontier, &front);
        if(back_cell && back_cell->state == CELL_NOT_PLACED)
            queue_coord_push(&frontier, &back);
        placed++;
    }

    queue_coord_destroy(&frontier);

    formation->pos = formation_center(formation);
    formation->orientation = orientation;
    PERF_RETURN_VOID();
}

static size_t sort_by_type(size_t size, uint32_t *ents, uint64_t *types)
{
    if(size == 0)
        return 0;

    int i = 1;
    while(i < size) {
        int j = i;
        while(j > 0 && types[j-1] < types[j]) {

            /* swap UIDs */
            uint32_t tmp = ents[j-1];
            ents[j-1] = ents[j];
            ents[j] = tmp;

            /* swap types */
            uint64_t tmp2 = types[j-1];
            types[j-1] = types[j];
            types[j] = tmp2;

            j--;
        }
        i++;
    }

    size_t ret = 1;
    for(int i = 1; i < size; i++) {
        uint64_t *a = types + i;
        uint64_t *b = types + i - 1;
        if(*a != *b)
            ret++;
    }
    return ret;
}

static size_t next_type_range(size_t begin, size_t size, 
                              uint64_t *types, size_t *out_count)
{
    size_t count = 0;
    int i = begin;
    for(; i < size; i++) {
        uint64_t *a = types + i;
        uint64_t *b = types + i + 1;
        if(*a != *b)
            break;
        count++;
    }
    *out_count = count + 1;
    return i + 1;
}

static void init_subformation(vec2_t target, struct subformation *formation,
                              struct subformation *parent,
                              size_t nchildren, struct subformation **children,
                              size_t ncols, uint32_t *ents, size_t nents)
{
    size_t nrows = (nents / ncols) + !!(nents % ncols);
    size_t total = nrows * ncols;
    printf("the subformation has %lu rows and %lu cols\n", nrows, ncols);

    enum nav_layer layer = Entity_NavLayer(ents[0]);
    vec2_t first_ent_pos = G_Pos_GetXZ(ents[0]);
    vec2_t reachable_target = M_NavClosestReachableDest(s_map, layer, 
        first_ent_pos, target);

    size_t curr_child = 0;
    while((curr_child < nents) && (curr_child < MAX_CHILDREN)) {
        formation->children[curr_child] = children[curr_child];
        curr_child++;
    }
    formation->nchildren = nchildren;
    formation->parent = parent;
    formation->nrows = nrows;
    formation->ncols = ncols;
    formation->unit_radius = G_GetSelectionRadius(ents[0]);
    formation->layer = layer;
    formation->reachable_target = reachable_target;
    formation->assignment = kh_init(assignment);
    kh_resize(assignment, formation->assignment, nents);

    formation->ents = kh_init(entity);
    kh_resize(entity, formation->ents, nents);
    for(int i = 0; i < nents; i++) {
        int ret;
        khiter_t k = kh_put(entity, formation->ents, ents[i], &ret);
        assert(ret != -1);
    }

    vec_cell_init(&formation->cells);
    vec_cell_resize(&formation->cells, total);
    formation->cells.size = total;
    for(int r = 0; r < nrows; r++) {
    for(int c = 0; c < ncols; c++) {
        size_t idx = r * ncols + c;
        vec_AT(&formation->cells, idx) = (struct cell){CELL_NOT_PLACED};
    }}
    formation->results = kh_init(result);
    vec_work_init(&formation->futures);
}

static void init_subformations(struct formation *formation)
{
    size_t nunits = kh_size(formation->ents);

    uint32_t uid;
    size_t curr = 0;
    STALLOC(uint32_t, ents, nunits);
    STALLOC(uint64_t, types, nunits);

    kh_foreach_key(formation->ents, uid, {
        ents[curr++] = uid;
    });
    for(int i = 0; i < nunits; i++) {
        uint64_t type = Entity_TypeID(ents[i]);
        types[i] = type;
    }
    size_t ntypes = sort_by_type(nunits, ents, types);
    vec_subformation_init(&formation->subformations);
    vec_subformation_resize(&formation->subformations, ntypes);
    formation->subformations.size = ntypes;
    formation->root = &vec_AT(&formation->subformations, 0);

    size_t offset = 0;
    for(int i = 0; i < ntypes; i++) {

        size_t count;
        struct subformation *sub = &vec_AT(&formation->subformations, i);
        struct subformation *parent = (i == 0) ? NULL 
                                    : &vec_AT(&formation->subformations, i-1);
        struct subformation *child = (i == ntypes-1) ? NULL 
                                   : &vec_AT(&formation->subformations, i+1);

        size_t next_offset = next_type_range(offset, nunits, types, &count);
        init_subformation(formation->target, sub, parent, 1, &child, 
            ncols(formation->type, count), ents + offset, count);

        for(int j = offset; j < offset + count; j++) {
            int ret;
            khiter_t k = kh_put(assignment, formation->sub_assignment, ents[j], &ret);
            assert(ret != -1);
            kh_val(formation->sub_assignment, k) = (struct coord){i, 0};
        }
        offset = next_offset;
    }

    STFREE(ents);
    STFREE(types);
}

/* The cost matrix holds the distance between every entity
 * and every cell.
 */
static void create_cost_matrix(struct subformation *formation, int *out_costs)
{
    size_t nents = kh_size(formation->ents);
    int (*out_rows)[nents] = (void*)out_costs;

    int i = 0;
    uint32_t uid;
    kh_foreach_key(formation->ents, uid, {

        vec2_t pos = G_Pos_GetXZ(uid);
        for(int j = 0; j < nents; j++) {
            struct cell *cell = &vec_AT(&formation->cells, j);
            if(cell->state == CELL_NOT_PLACED) {
                out_rows[i][j] = INT_MAX;
            }else{
                vec2_t distance;
                PFM_Vec2_Sub(&cell->pos, &pos, &distance);
                out_rows[i][j] = PFM_Vec2_Len(&distance);
            }
        }
        i++;
    });
}

static int row_minimum(int *costs, int irow, size_t nents)
{
    int (*rows)[nents] = (void*)costs;
    int min = rows[irow][0];
    for(int i = 1; i < nents; i++) {
        min = MIN(min, rows[irow][i]);
    }
    return min;
}

static int column_minimum(int *costs, int icol, size_t nents)
{
    int (*rows)[nents] = (void*)costs;
    int min = rows[0][icol];
    for(int i = 1; i < nents; i++) {
        min = MIN(min, rows[i][icol]);
    }
    return min;
}

static bool assigned_in_column(bool *starred, size_t nents, size_t icol)
{
    bool (*rows)[nents] = (void*)starred;
    for(int i = 0; i < nents; i++) {
        if(rows[i][icol])
            return true;
    }
    return false;
}

static bool row_is_covered(bool *covered, size_t nents, size_t irow)
{
    bool (*rows)[nents] = (void*)covered;
    size_t ncovered = 0;
    for(int i = 0; i < nents; i++) {
        if(rows[irow][i])
            ncovered++;
    }
    return (ncovered == nents);
}

static void cover_column(bool *covered, size_t nents, size_t icol)
{
    bool (*rows)[nents] = (void*)covered;
    for(int i = 0; i < nents; i++) {
        rows[i][icol] = true;
    }
}

static void uncover_column(bool *covered, size_t nents, size_t icol)
{
    bool (*rows)[nents] = (void*)covered;
    for(int i = 0; i < nents; i++) {
        if(!row_is_covered(covered, nents, i)) {
            rows[i][icol] = false;
        }
    }
}

static void cover_row(bool *covered, size_t nents, size_t irow)
{
    bool (*rows)[nents] = (void*)covered;
    for(int i = 0; i < nents; i++) {
        rows[irow][i] = true;
    }
}

static bool row_has_starred(bool *starred, size_t nents, size_t irow, int *out_col)
{
    bool (*rows)[nents] = (void*)starred;
    for(int i = 0; i < nents; i++) {
        if(rows[irow][i]) {
            *out_col = i;
            return true;
        }
    }
    return false;
}

static bool column_is_covered(bool *covered, size_t nents, size_t icol)
{
    bool (*rows)[nents] = (void*)covered;
    size_t ncovered = 0;
    for(int i = 0; i < nents; i++) {
        if(rows[i][icol])
            ncovered++;
    }
    return (ncovered == nents);
}

static bool column_has_starred(bool *starred, size_t nents, size_t icol, int *out_row)
{
    bool (*rows)[nents] = (void*)starred;
    for(int i = 0; i < nents; i++) {
        if(rows[i][icol]) {
            *out_row = i;
            return true;
        }
    }
    return false;
}

static int primed_zero_at_row(bool *primed, size_t nents, size_t irow)
{
    bool (*rows)[nents] = (void*)primed;
    for(int i = 0; i < nents; i++) {
        if(rows[irow][i])
            return i;
    }
    assert(0);
    return -1;
}

static size_t count_covered_rows(bool *covered, size_t nents)
{
    bool (*rows)[nents] = (void*)covered;
    size_t ret = 0;
    for(int i = 0; i < nents; i++) {
        size_t ncovered = 0;
        for(int j = 0; j < nents; j++) {
            if(rows[i][j])
                ncovered++;
        }
        if(ncovered == nents)
            ret++;
    }
    return ret;
}

static size_t count_covered_columns(bool *covered, size_t nents)
{
    bool (*rows)[nents] = (void*)covered;
    size_t ret = 0;
    for(int i = 0; i < nents; i++) {
        size_t ncovered = 0;
        for(int j = 0; j < nents; j++) {
            if(rows[j][i])
                ncovered++;
        }
        if(ncovered == nents)
            ret++;
    }
    return ret;
}

static void dump_cost_matrix(int *costs, size_t nents,
                             bool *starred, bool *covered, bool *primed)
{
    int  (*cost_rows)[nents] = (void*)costs;
    bool (*starred_rows)[nents] = (void*)starred;
    bool (*covered_rows)[nents] = (void*)covered;
    bool (*primed_rows)[nents] = (void*)primed;

    for(int r = 0; r < nents; r++) {
        for(int c = 0; c < nents; c++) {
            printf("%03d%c%c%c", 
                cost_rows[r][c],
                starred_rows[r][c] ? '*'  : ' ',
                primed_rows[r][c]  ? '\'' : ' ',
                covered_rows[r][c] ? 'C'  : ' ');
            if(c != nents-1)
                printf("  ");
        }
        printf("\n");
    }
}

static int min_uncovered_value(int *costs, bool *covered, size_t nents)
{
    int  (*cost_rows)[nents] = (void*)costs;
    bool (*covered_rows)[nents] = (void*)covered;

    int min = INT_MAX;
    for(int r = 0; r < nents; r++) {
        for(int c = 0; c < nents; c++) {
            if(!covered_rows[r][c]) {
                min = MIN(cost_rows[r][c], min);
            }
        }
    }
    return min;
}

static int min_lines_to_cover_zeroes(int *costs, int *out_next, 
                                     struct coord *out_assignment, size_t nents)
{
    PERF_ENTER();

    STALLOC(bool, starred, nents * nents);
    STALLOC(bool, covered, nents * nents);
    STALLOC(bool, primed, nents * nents);

    memset(starred, 0, nents * nents * sizeof(bool));
    memset(covered, 0, nents * nents * sizeof(bool));
    memset(primed, 0, nents * nents * sizeof(bool));

    int  (*cost_rows)[nents] = (void*)costs;
    bool (*starred_rows)[nents] = (void*)starred;
    bool (*covered_rows)[nents] = (void*)covered;
    bool (*primed_rows)[nents] = (void*)primed;

iterate:
    /* For each row, try to assign an arbitrary zero. Assigned tasks
     * are represented by starring a zero. Note that assignments can't 
     * be in the same row or column.
     */
    for(int row = 0; row < nents; row++) {
        for(int col = 0; col < nents; col++) {
            if(starred_rows[row][col])
                break;
            if((cost_rows[row][col] == 0) && !assigned_in_column(starred, nents, col)) {
                starred_rows[row][col] = true;
                break;
            }
        }
    }

    /* Cover all columns containing a (starred) zero.
     */
    for(int row = 0; row < nents; row++) {
        for(int col = 0; col < nents; col++) {
            if(starred_rows[row][col])
                cover_column(covered, nents, col);
        }
    }

    bool has_uncovered;
    int primed_r, primed_c;
    do{
        /* Find a non-covered zero and prime it */
        has_uncovered = false;
        for(int row = 0; row < nents; row++) {
            for(int col = 0; col < nents; col++) {
                if(cost_rows[row][col] == 0 && !covered_rows[row][col]) {
                    has_uncovered = true;
                    primed_rows[row][col] = true;
                    primed_r = row;
                    primed_c = col;
                    break;
                }
            }
            if(has_uncovered)
                break;
        }

        if(has_uncovered) {
            /* If the zero is on the same row as a starred zero, 
             * cover the corresponding row, and uncover the column 
             * of the starred zero 
             */
            int starred_c;
            if(row_has_starred(starred, nents, primed_r, &starred_c)) {
                uncover_column(covered, nents, starred_c);
                cover_row(covered, nents, primed_r);
            }else{
                size_t npath = 0;
                STALLOC(struct coord, path, nents * nents);
                path[npath++] = (struct coord){primed_r, primed_c};
                /* Else the non-covered zero has no assigned zero on its row. 
                 * We make a path starting from the zero by performing the following steps: 
                 *
                 * Substep 1: Find a starred zero on the corresponding column. If there is one, 
                 * go to Substep 2, else, stop.
                 */
            next_path_zero:;
                int starred_r;
                if(column_has_starred(starred, nents, primed_c, &starred_r)) {
                    path[npath++] = (struct coord){starred_r, primed_c};
                    /*
                     * Substep 2: Find a primed zero on the corresponding row (there should always 
                     * be one). Go to Substep 1. 
                     */
                    primed_c = primed_zero_at_row(primed, nents, starred_r);
                    primed_r = starred_r;
                    path[npath++] = (struct coord){primed_r, primed_c};
                    goto next_path_zero;
                }

                /* For all zeros encountered during the path, star primed zeros 
                 * and unstar starred zeros.
                 */
                for(int i = 0; i < npath; i++) {
                    struct coord curr = path[i];
                    assert(starred_rows[curr.r][curr.c] ^ primed_rows[curr.r][curr.c]);
                    if(starred_rows[curr.r][curr.c]) {
                        starred_rows[curr.r][curr.c] = false;
                    }else if(primed_rows[curr.r][curr.c]) {
                        starred_rows[curr.r][curr.c] = true;
                    }
                }

                /* Unprime all primed zeroes and uncover all tiles.
                 */
                for(int i = 0; i < nents; i++) {
                    for(int j = 0; j < nents; j++) {
                        primed_rows[i][j] = false;
                        covered_rows[i][j] = false;
                    }
                }
                STFREE(path);
                goto iterate;
            }
        }
    }while(has_uncovered); 

    size_t ncovered_rows = count_covered_rows(covered, nents);
    size_t ncovered_cols = count_covered_columns(covered, nents);

    size_t ret;
    if((ncovered_rows == nents) || (ncovered_cols == nents)) {
        ret = nents;
    }else{
        ret = (ncovered_rows + ncovered_cols);
    }

    if(ret < nents) {
        /* Find the lowest uncovered value. Subtract this from every 
         * unmarked element and add it to every element covered by two lines.  
         * This is equivalent to subtracting a number from all rows which are 
         * not covered and adding the same number to all columns which are 
         * covered. These operations do not change optimal assignments. 
         */
        int (*next_rows)[nents] = (void*)out_next;
        memcpy(out_next, costs, sizeof(int) * nents * nents);

        int min = min_uncovered_value(costs, covered, nents);
        for(int r = 0; r < nents; r++) {
            if(!row_is_covered(covered, nents, r)) {
                for(int c = 0; c < nents; c++) {
                    next_rows[r][c] -= min;
                }
            }
        }
        for(int c = 0; c < nents; c++) {
            if(column_is_covered(covered, nents, c)) {
                for(int r = 0; r < nents; r++) {
                    next_rows[r][c] += min;
                }
            }
        }
    }else{
        int i = 0;
        for(int r = 0; r < nents; r++) {
            for(int c = 0; c < nents; c++) {
                if(starred_rows[r][c]) {
                    out_assignment[i++] = (struct coord){r, c};
                }
            }
        }
        assert(i == nents);
    }

    STFREE(starred);
    STFREE(covered);
    STFREE(primed);

    PERF_RETURN(ret);
}

/* Use the Hungarian algorithm to find an optimal assignment of entities to cells
 * (minimizing the combined distance that needs to be traveled by the entities).
 */
static void compute_cell_assignment(struct subformation *formation)
{
    PERF_ENTER();

    size_t nents = kh_size(formation->ents);
    STALLOC(int, costs, nents * nents);
    STALLOC(int, next, nents * nents);
    STALLOC(struct coord, assignment, nents);

    create_cost_matrix(formation, costs);
    int (*rows)[nents] = (void*)costs;

    /* Step 1: Subtract row minima
     * For each row, find the lowest element and subtract it from each element in that row.
     */
    for(int i = 0; i < nents; i++) {
        int row_min = row_minimum(costs, i, nents);
        for(int j = 0; j < nents; j++) {
            rows[i][j] -= row_min;
        }
    }

    /* Step  2: Subtract column minima 
     * For each column, find the lowest element and subtract it from each element in that 
     * column.
     */
    for(int i = 0; i < nents; i++) {
        int col_min = column_minimum(costs, i, nents);
        for(int j = 0; j < nents; j++) {
            rows[j][i] -= col_min;
        }
    }

    size_t min_lines;
    do{
        /* Step 3: Cover all zeros with a minimum number of lines
         * Cover all zeros in the resulting matrix using a minimum number of horizontal and 
         * vertical lines. If n lines are required, an optimal assignment exists among the zeros. 
         * The algorithm stops.
         *
         * If less than n lines are required, continue with Step 4.
         */
        min_lines = min_lines_to_cover_zeroes(costs, next, assignment, nents);

        /* Step 4: Create additional zeros
         * Find the smallest element (call it k) that is not covered by a line in Step 3. 
         * Subtract k from all uncovered elements, and add k to all elements that are covered twice.
         */
        if(min_lines < nents) {
            memcpy(costs, next, sizeof(int) * nents * nents);
        }
    }while(min_lines < nents);

    int i = 0;
    uint32_t uid;
    kh_foreach_key(formation->ents, uid, {
        int status;
        khiter_t k = kh_put(assignment, formation->assignment, uid, &status);
        assert(status != -1);
        struct coord cell_coord = (struct coord){
            assignment[i].c / formation->ncols,
            assignment[i].c % formation->ncols
        };
        kh_val(formation->assignment, k) = cell_coord;
        i++;
    });

    STFREE(costs);
    STFREE(next)
    STFREE(assignment);

    PERF_RETURN_VOID();
}

static void render_formations(void)
{
    struct map_resolution res;
    M_GetResolution(s_map, &res);
    vec3_t map_pos = M_GetPos(s_map);
    struct camera *cam = G_GetActiveCamera();

    mat4x4_t view, proj;
    Camera_MakeViewMat(cam, &view); 
    Camera_MakeProjMat(cam, &proj);

    struct formation *formation;
    kh_foreach_ptr(s_formations, formation, {
        const float length = 15.0f;
        const float width = 1.5f;
        const vec3_t green = (vec3_t){0.0, 1.0, 0.0};
        vec2_t origin = formation->target;
        vec2_t delta, end;
        PFM_Vec2_Scale(&formation->orientation, length, &delta);
        PFM_Vec2_Add(&origin, &delta, &end);

        vec2_t endpoints[] = {origin, end};
        R_PushCmd((struct rcmd){
            .func = R_GL_DrawLine,
            .nargs = 4,
            .args = {
                R_PushArg(endpoints, sizeof(endpoints)),
                R_PushArg(&width, sizeof(width)),
                R_PushArg(&green, sizeof(green)),
                (void*)G_GetPrevTickMap()
            }
        });

        for(int i = 0; i < vec_size(&formation->subformations); i++) {

            struct subformation *sub = &vec_AT(&formation->subformations, i);
            const vec3_t magenta = (vec3_t){1.0, 0.0, 1.0};
            const float radius = 0.5f;
            const float width = 1.5f;
            R_PushCmd((struct rcmd){
                .func = R_GL_DrawSelectionCircle,
                .nargs = 5,
                .args = {
                    R_PushArg(&sub->pos, sizeof(sub->pos)),
                    R_PushArg(&radius, sizeof(radius)),
                    R_PushArg(&width, sizeof(width)),
                    R_PushArg(&magenta, sizeof(magenta)),
                    (void*)G_GetPrevTickMap(),
                },
            });

            for(int r = 0; r < sub->nrows; r++) {
            for(int c = 0; c < sub->ncols; c++) {

                struct cell *cell = &vec_AT(&sub->cells, CELL_IDX(r, c, sub->ncols));
                float radius = sub->unit_radius;
                const float width = 0.5f;
                vec3_t blue = (vec3_t){0.0f, 0.0f, 1.0f};
                vec3_t green = (vec3_t){0.0f, 1.0f, 0.0f};
                vec3_t cyan = (vec3_t){0.0f, 1.0f, 1.0f};

                R_PushCmd((struct rcmd){
                    .func = R_GL_DrawSelectionCircle,
                    .nargs = 5,
                    .args = {
                        R_PushArg(&cell->ideal_raw, sizeof(cell->ideal_raw)),
                        R_PushArg(&radius, sizeof(radius)),
                        R_PushArg(&width, sizeof(width)),
                        R_PushArg(&blue, sizeof(blue)),
                        (void*)G_GetPrevTickMap(),
                    },
                });

                R_PushCmd((struct rcmd){
                    .func = R_GL_DrawSelectionCircle,
                    .nargs = 5,
                    .args = {
                        R_PushArg(&cell->ideal_binned, sizeof(cell->ideal_binned)),
                        R_PushArg(&radius, sizeof(radius)),
                        R_PushArg(&width, sizeof(width)),
                        R_PushArg(&cyan, sizeof(cyan)),
                        (void*)G_GetPrevTickMap(),
                    },
                });

                R_PushCmd((struct rcmd){
                    .func = R_GL_DrawSelectionCircle,
                    .nargs = 5,
                    .args = {
                        R_PushArg(&cell->pos, sizeof(cell->pos)),
                        R_PushArg(&radius, sizeof(radius)),
                        R_PushArg(&width, sizeof(width)),
                        R_PushArg(&green, sizeof(green)),
                        (void*)G_GetPrevTickMap(),
                    },
                });

                /* Draw the cell coordinate */
                struct tile_desc td;
                bool exists = M_Tile_DescForPoint2D(res, map_pos, cell->pos, &td);
                assert(exists);

                mat4x4_t model;
                PFM_Mat4x4_Identity(&model);

                struct box bounds = M_Tile_Bounds(res, map_pos, td);
                vec4_t center_homo = (vec4_t) {
                    bounds.x - bounds.width / 2.0,
                    0.0,
                    bounds.z + bounds.height / 2.0,
                    1.0
                };

                char text[16];
                pf_snprintf(text, sizeof(text), "(%d, %d)", r, c);
                N_RenderOverlayText(text, center_homo, &model, &view, &proj);
            }}
        }
    });
}

static bool chunks_compare(struct coord *a, struct coord *b)
{
    if(a->r > b->r)
        return true;
    if(a->c > b->c)
        return true;
    return false;
}

static void swap_corners(vec2_t *corners_buff, size_t a, size_t b)
{
    vec2_t tmp[4];
    memcpy(tmp, corners_buff + (a * 4), sizeof(tmp));
    memcpy(corners_buff + (a * 4), corners_buff + (b * 4), sizeof(tmp));
    memcpy(corners_buff + (b * 4), tmp, sizeof(tmp));
}

static void swap_colors(vec3_t *colors_buff, size_t a, size_t b)
{
    vec3_t tmp = colors_buff[a];
    colors_buff[a] = colors_buff[b];
    colors_buff[b] = tmp;
}

static void swap_chunks(struct coord *chunk_buff, size_t a, size_t b)
{
    struct coord tmp = chunk_buff[a];
    chunk_buff[a] = chunk_buff[b];
    chunk_buff[b] = tmp;
}

static size_t sort_by_chunk(size_t size, vec2_t *corners_buff, 
                            vec3_t *colors_buff, struct coord *chunk_buff)
{
    if(size == 0)
        return 0;

    int i = 1;
    while(i < size) {
        int j = i;
        while(j > 0 && chunks_compare(chunk_buff + j - 1, chunk_buff + j)) {

            swap_corners(corners_buff, j, j-1);
            swap_colors(colors_buff, j, j-1);
            swap_chunks(chunk_buff, j, j-1);

            j--;
        }
        i++;
    }

    size_t ret = 1;
    for(int i = 1; i < size; i++) {
        struct coord *a = chunk_buff + i;
        struct coord *b = chunk_buff + i - 1;
        if(a->r != b->r || a->c != b->c)
            ret++;
    }
    return ret;
}

static size_t next_chunk_range(size_t begin, size_t size, 
                               struct coord *chunk_buff, size_t *out_count)
{
    size_t count = 0;
    int i = begin;
    for(; i < size; i++) {
        struct coord *a = chunk_buff + i;
        struct coord *b = chunk_buff + i + 1;
        if(a->r != b->r || a->c != b->c)
            break;
        count++;
    }
    *out_count = count + 1;
    return i + 1;
}

static size_t chunks_for_field(vec2_t center, size_t maxout, 
                               struct coord *out_chunks, struct range2d *out_ranges)
{
    struct map_resolution res;
    M_NavGetResolution(s_map, &res);
    vec3_t map_pos = M_GetPos(s_map);

    struct tile_desc center_tile;
    M_Tile_DescForPoint2D(res, map_pos, center, &center_tile);

    int min_dr = -OCCUPIED_FIELD_RES / 2;
    int min_dc = -OCCUPIED_FIELD_RES / 2;
    struct tile_desc min_tile = center_tile;
    bool exists = M_Tile_RelativeDesc(res, &min_tile, min_dc, min_dr);
    if(!exists) {
        bool unclipped_r = M_Tile_RelativeDesc(res, &min_tile, 0, min_dr);
        if(unclipped_r) {
            min_tile = (struct tile_desc){
                min_tile.chunk_r, 0, 
                min_tile.tile_r,  0
            };
            goto done_min;
        }
        bool unclipped_c = M_Tile_RelativeDesc(res, &min_tile, min_dc, 0);
        if(unclipped_c) {
            min_tile = (struct tile_desc){
                0, min_tile.chunk_c,
                0, min_tile.tile_c
            };
            goto done_min;
        }
        min_tile = (struct tile_desc){
            0, 0,
            0, 0
        };
    }
done_min:;

    int max_dr = OCCUPIED_FIELD_RES / 2;
    int max_dc = OCCUPIED_FIELD_RES / 2;
    struct tile_desc max_tile = center_tile;
    exists = M_Tile_RelativeDesc(res, &max_tile, max_dr, max_dc);
    if(!exists) {
        bool unclipped_r = M_Tile_RelativeDesc(res, &max_tile, 0, max_dr);
        if(unclipped_r) {
            max_tile = (struct tile_desc){
                max_tile.chunk_r, res.chunk_w-1, 
                max_tile.tile_r,  res.tile_w-1 
            };
            goto done_max;
        }
        bool unclipped_c = M_Tile_RelativeDesc(res, &max_tile, max_dc, 0);
        if(unclipped_c) {
            max_tile = (struct tile_desc){
                res.chunk_h-1, max_tile.chunk_c,
                res.tile_h-1,  max_tile.tile_c
            };
            goto done_max;
        }
        max_tile = (struct tile_desc){
            res.chunk_h-1, res.chunk_w-1,
            res.tile_h-1,  res.tile_h-1
        };
    }
done_max:;

    size_t ret = 0;
    for(int r = min_tile.chunk_r; r <= max_tile.chunk_r; r++) {
    for(int c = min_tile.chunk_c; c <= max_tile.chunk_c; c++) {

        if(ret == maxout)
            break;

        struct coord curr = (struct coord){r, c};
        out_chunks[ret] = curr;

        struct range2d curr_range = (struct range2d){
            0, res.tile_w-1,
            0, res.tile_h-1
        };

        if(r == min_tile.chunk_r) {
            curr_range.min_r = min_tile.tile_r;
        }
        if(r == max_tile.chunk_r) {
            curr_range.max_r = max_tile.tile_r;
        }
        if(c == min_tile.chunk_c) {
            curr_range.min_c = min_tile.tile_c;
        }
        if(c == max_tile.chunk_c) {
            curr_range.max_c = max_tile.tile_c;
        }
        out_ranges[ret] = curr_range;
        ret++;
    }}
    return ret;
}

static void render_islands_field(enum nav_layer layer)
{
    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    struct map_resolution res;
    M_NavGetResolution(s_map, &res);
    struct camera *cam = G_GetActiveCamera();
    vec3_t map_pos = M_GetPos(s_map);

    mat4x4_t view, proj;
    Camera_MakeViewMat(cam, &view); 
    Camera_MakeProjMat(cam, &proj);

    struct formation *formation;
    kh_foreach_ptr(s_formations, formation, {

        struct coord chunks[32];
        struct range2d ranges[32];
        size_t nchunks = chunks_for_field(formation->center, 32, chunks, ranges);

        struct tile_desc center_tile;
        M_Tile_DescForPoint2D(res, map_pos, formation->center, &center_tile);

        for(int i = 0; i < nchunks; i++) {
            struct coord *chunk = &chunks[i];
            struct range2d *range = &ranges[i];

            mat4x4_t chunk_model;
            M_ModelMatrixForChunk(s_map, (struct chunkpos){chunk->r, chunk->c}, &chunk_model);

            for(int r = range->min_r; r <= range->max_r; r++) {
            for(int c = range->min_c; c <= range->max_c; c++) {

                float square_x_len = (1.0f / res.tile_w) * chunk_x_dim;
                float square_z_len = (1.0f / res.tile_h) * chunk_z_dim;
                float square_x = CLAMP(-(((float)c) / res.tile_w) * chunk_x_dim, 
                                       -chunk_x_dim, chunk_x_dim);
                float square_z = CLAMP((((float)r) / res.tile_h) * chunk_z_dim, 
                                       -chunk_z_dim, chunk_z_dim);

                vec4_t center_homo = (vec4_t) {
                    square_x - square_x_len / 2.0,
                    0.0,
                    square_z + square_z_len / 2.0,
                    1.0
                };

                struct tile_desc curr = (struct tile_desc){
                    chunk->r, chunk->c,
                    r, c 
                };
                int dr, dc;
                M_Tile_Distance(res, &curr, &center_tile, &dr, &dc);

                int offset_r = (OCCUPIED_FIELD_RES / 2) + dr;
                int offset_c = (OCCUPIED_FIELD_RES / 2) + dc;
                assert(offset_r >= 0 && offset_r < OCCUPIED_FIELD_RES);
                assert(offset_c >= 0 && offset_c < OCCUPIED_FIELD_RES);
                uint16_t island_id = formation->islands[layer][offset_r][offset_c];

                char text[8];
                pf_snprintf(text, sizeof(text), "%u", island_id);
                N_RenderOverlayText(text, center_homo, &chunk_model, &view, &proj);
            }}
        }
    });
}

static void render_formations_occupied_field(enum nav_layer layer)
{
    struct map_resolution res;
    M_NavGetResolution(s_map, &res);
    vec3_t map_pos = M_GetPos(s_map);

    struct formation *formation;
    kh_foreach_ptr(s_formations, formation, {

        struct tile_desc center_tile;
        M_Tile_DescForPoint2D(res, map_pos, formation->center, &center_tile);

        struct box center_bounds = M_Tile_Bounds(res, map_pos, center_tile);
        vec2_t center = (vec2_t){
            center_bounds.x - center_bounds.width / 2.0f,
            center_bounds.z + center_bounds.height / 2.0f
        };

        const float field_width = center_bounds.width * OCCUPIED_FIELD_RES;
        const float line_width = 1.0f;
        const vec3_t blue = (vec3_t){0.0f, 0.0f, 1.0f};

        vec2_t field_corners[4] = {
            (vec2_t){center.x + field_width/2.0f, center.z - field_width/2.0f},
            (vec2_t){center.x - field_width/2.0f, center.z - field_width/2.0f},
            (vec2_t){center.x - field_width/2.0f, center.z + field_width/2.0f},
            (vec2_t){center.x + field_width/2.0f, center.z + field_width/2.0f},
        };
        R_PushCmd((struct rcmd){
            .func = R_GL_DrawQuad,
            .nargs = 4,
            .args = {
                R_PushArg(field_corners, sizeof(field_corners)),
                R_PushArg(&line_width, sizeof(line_width)),
                R_PushArg(&blue, sizeof(blue)),
                (void*)G_GetPrevTickMap(),
            },
        });

        struct coord center_coord = (struct coord){
            OCCUPIED_FIELD_RES / 2,
            OCCUPIED_FIELD_RES / 2
        };

        const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
        const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

        vec2_t corners_buff[4 * OCCUPIED_FIELD_RES * OCCUPIED_FIELD_RES];
        memset(corners_buff, 0, sizeof(corners_buff));
        vec3_t colors_buff[OCCUPIED_FIELD_RES * OCCUPIED_FIELD_RES];
        struct coord chunk_buff[OCCUPIED_FIELD_RES * OCCUPIED_FIELD_RES];

        vec2_t *corners_base = corners_buff;
        vec3_t *colors_base = colors_buff; 
        struct coord *chunk_base = chunk_buff;
        size_t count = 0;

        for(int r = 0; r < OCCUPIED_FIELD_RES; r++) {
        for(int c = 0; c < OCCUPIED_FIELD_RES; c++) {

            int dr = center_coord.r - r;
            int dc = center_coord.c - c;
            struct tile_desc curr = center_tile;
            bool exists = M_Tile_RelativeDesc(res, &curr, dc, dr);
            if(!exists)
                continue;

            float square_x_len = center_bounds.width;
            float square_z_len = center_bounds.height;

            float square_x = CLAMP(-(((float)curr.tile_c) / res.tile_w) * chunk_x_dim, 
                                   -chunk_x_dim, chunk_x_dim);
            float square_z = CLAMP((((float)curr.tile_r) / res.tile_h) * chunk_z_dim, 
                                   -chunk_z_dim, chunk_z_dim);

            *corners_base++ = (vec2_t){square_x, square_z};
            *corners_base++ = (vec2_t){square_x, square_z + square_z_len};
            *corners_base++ = (vec2_t){square_x - square_x_len, square_z + square_z_len};
            *corners_base++ = (vec2_t){square_x - square_x_len, square_z};

            if(formation->occupied[layer][r][c] == TILE_BLOCKED) {
                *colors_base++ = (vec3_t){1.0f, 0.0f, 0.0f};
            }else if(formation->occupied[layer][r][c] == TILE_ALLOCATED) {
                *colors_base++ = (vec3_t){0.0f, 0.0f, 1.0f};
            }else{
                *colors_base++ = (vec3_t){0.0f, 1.0f, 0.0f};
            }
            *chunk_base++ = (struct coord){curr.chunk_r, curr.chunk_c};
            count++;
        }}

        size_t nchunks = sort_by_chunk(count, corners_buff, colors_buff, chunk_buff);
        size_t offset = 0;
        for(int i = 0; i < nchunks; i++) {

            mat4x4_t chunk_model;
            M_ModelMatrixForChunk(s_map, 
                (struct chunkpos){chunk_buff[offset].r, chunk_buff[offset].c}, &chunk_model);

            size_t num_tiles;
            size_t next_offset = next_chunk_range(offset, count, chunk_buff, &num_tiles);
            R_PushCmd((struct rcmd){
                .func = R_GL_DrawMapOverlayQuads,
                .nargs = 5,
                .args = {
                    R_PushArg(corners_buff + 4 * offset, sizeof(vec2_t) * 4 * num_tiles),
                    R_PushArg(colors_buff + offset, sizeof(vec3_t) * num_tiles),
                    R_PushArg(&num_tiles, sizeof(num_tiles)),
                    R_PushArg(&chunk_model, sizeof(chunk_model)),
                    (void*)G_GetPrevTickMap(),
                },
            });
            offset = next_offset;
        }
    });
}

static void render_formation_assignment(void)
{
    struct formation *formation;
    kh_foreach_ptr(s_formations, formation, {

        for(int i = 0; i < vec_size(&formation->subformations); i++) {
            struct subformation *subformation = &vec_AT(&formation->subformations, i);

            uint32_t uid;
            struct coord coord;
            kh_foreach(subformation->assignment, uid, coord, {
                struct cell *target = &vec_AT(&subformation->cells,
                    CELL_IDX(coord.r, coord.c, subformation->ncols));
                vec2_t from = G_Pos_GetXZ(uid);
                vec2_t to = target->pos;
                vec2_t endpoints[] = {from, to};
                vec3_t yellow = (vec3_t){1.0f, 0.0f, 1.0f};
                const float width = 0.5f;

                R_PushCmd((struct rcmd){
                    .func = R_GL_DrawLine,
                    .nargs = 4,
                    .args = {
                        R_PushArg(endpoints, sizeof(endpoints)),
                        R_PushArg(&width, sizeof(width)),
                        R_PushArg(&yellow, sizeof(yellow)),
                        (void*)G_GetPrevTickMap()
                    }
                });
            });
        }
    });
}

static void on_render_3d(void *user, void *event)
{
    enum nav_layer layer;
    struct sval setting;
    ss_e status;
    (void)status;

    status = Settings_Get("pf.debug.navigation_layer", &setting);
    assert(status == SS_OKAY);
    layer = setting.as_int;

    status = Settings_Get("pf.debug.show_formations", &setting);
    assert(status == SS_OKAY);
    bool enabled = setting.as_bool;

    if(enabled) {
        render_formations();
    }

    status = Settings_Get("pf.debug.show_formations_occupied_field", &setting);
    assert(status == SS_OKAY);
    enabled = setting.as_bool;

    if(enabled) {
        render_formations_occupied_field(layer);
        render_islands_field(layer);
    }

    status = Settings_Get("pf.debug.show_formations_assignment", &setting);
    assert(status == SS_OKAY);
    enabled = setting.as_bool;

    if(enabled) {
        render_formation_assignment();
    }
}

static void destroy_subformation(struct subformation *formation)
{
    complete_cell_field_work(formation);
    vec_work_destroy(&formation->futures);
    vec_cell_destroy(&formation->cells);
    kh_destroy(result, formation->results);
    kh_destroy(assignment, formation->assignment);
    kh_destroy(entity, formation->ents);
}

static void destroy_formation(struct formation *formation)
{
    for(int i = 0; i < vec_size(&formation->subformations); i++) {
        struct subformation *sub = &vec_AT(&formation->subformations, i);
        destroy_subformation(sub);
    }
    kh_destroy(entity, formation->ents);
    vec_subformation_destroy(&formation->subformations);
    kh_destroy(assignment, formation->sub_assignment);
}

static khash_t(entity) *copy_vector(const vec_entity_t *ents)
{
    khash_t(entity) *ret = kh_init(entity);
    if(!ret)
        return NULL;
    if(kh_resize(entity, ret, vec_size(ents)) != 0)
        return NULL;
    for(int i = 0; i < vec_size(ents); i++) {
        uint32_t uid = vec_AT(ents, i);
        int status;
        khiter_t k = kh_put(entity, ret, uid, &status);
        assert(status != -1);
    }
    return ret;
}

static size_t contains_layer(enum nav_layer layer, enum nav_layer *layers, size_t count)
{
    for(int i = 0; i < count; i++) {
        if(layers[i] == layer)
            return true;
    }
    return false;
}

static int compare_layers(const void *a, const void *b)
{
    enum nav_layer layer_a = *(enum nav_layer*)a;
    enum nav_layer layer_b = *(enum nav_layer*)b;
    return (layer_a - layer_b);
}

static size_t formation_layers(vec_subformation_t *subformations, enum nav_layer *out_layers)
{
    size_t ret = 0;
    for(int i = 0; i < vec_size(subformations); i++) {
        struct subformation *curr = &vec_AT(subformations, i);
        if(!contains_layer(curr->layer, out_layers, ret))
            out_layers[ret++] = curr->layer;
    }
    qsort(out_layers, ret, sizeof(enum nav_layer), compare_layers);
    return ret;
}

static struct result cell_field_task(void *arg)
{
    return NULL_RESULT;
}

static void dispatch_cell_field_work(struct subformation *formation)
{
    /* Reserve the appropriate amount of space in the vector. 
     * Futures cannot be moved in memory once the corresponding 
     * task is dispatched. 
     */
    size_t nents = kh_size(formation->ents);
    vec_work_resize(&formation->futures, nents);
    formation->futures.size = nents;

    int i = 0;
    uint32_t uid;
    kh_foreach_key(formation->ents, uid, {
        struct cell_field_work *curr = &vec_AT(&formation->futures, i);
        curr->consumed = false;
        curr->uid = uid;
        SDL_AtomicSet(&curr->future.status, FUTURE_INCOMPLETE);
        curr->tid = Sched_Create(31, cell_field_task, &curr->result, &curr->future, 0);
        if(curr->tid == NULL_TID) {
            cell_field_task(&curr->result);
            SDL_AtomicSet(&curr->future.status, FUTURE_COMPLETE);
        }
        i++;
    });
}

static void complete_cell_field_work(struct subformation *formation)
{
    for(int j = 0; j < vec_size(&formation->futures); j++) {
        struct cell_field_work *curr = &vec_AT(&formation->futures, j);
        if(curr->tid == NULL_TID)
            continue;
        while(!Sched_FutureIsReady(&curr->future)) {
            Sched_RunSync(curr->tid);
        }
    }
}

static void on_update_start(void *user, void *event)
{
    struct formation *formation;
    kh_foreach_ptr(s_formations, formation, {

        for(int i = 0; i < vec_size(&formation->subformations); i++) {
            struct subformation *sub = &vec_AT(&formation->subformations, i);
            for(int j = 0; j < vec_size(&sub->futures); j++) {
                struct cell_field_work *curr = &vec_AT(&sub->futures, j);
                if(!curr->consumed && Sched_FutureIsReady(&curr->future)) {
                    /* Publish the result */
                    int ret;
                    khiter_t k = kh_put(result, sub->results, curr->uid, &ret);
                    assert(ret != -1);
                    kh_val(sub->results, k) = &curr->result;
                    curr->consumed = true;
                }
            }
        }
    });
}

struct cell_arrival_field *cell_get_field(uint32_t uid)
{
    formation_id_t fid = G_Formation_GetForEnt(uid);

    khiter_t k = kh_get(formation, s_formations, fid);
    assert(k != kh_end(s_formations));
    struct formation *formation = &kh_val(s_formations, k);

    khiter_t l = kh_get(assignment, formation->sub_assignment, uid);
    assert(l != kh_end(formation->sub_assignment));
    int idx = kh_val(formation->sub_assignment, l).r;
    struct subformation *sub = &vec_AT(&formation->subformations, idx);

    khiter_t m = kh_get(result, sub->results, uid);
    if(m == kh_end(sub->results))
        return NULL;
    return kh_val(sub->results, m);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Formation_Init(const struct map *map)
{
    ASSERT_IN_MAIN_THREAD();

    if(NULL == (s_ent_formation_map = kh_init(mapping)))
        return false;
    if(NULL == (s_formations = kh_init(formation)))
        goto fail_formations;

    if(s_workspace == 0) {
        s_workspace = SDL_TLSCreate();
    }
    if(s_workspace == 0)
        goto fail_tls;

    s_map = map;
    s_next_id = 0;

    E_Global_Register(EVENT_RENDER_3D_POST, on_render_3d, NULL, 
        G_RUNNING | G_PAUSED_FULL | G_PAUSED_UI_RUNNING);
    E_Global_Register(EVENT_UPDATE_START, on_update_start, NULL, G_RUNNING);
    return true;

fail_tls:
    kh_destroy(formation, s_formations);
fail_formations:
    kh_destroy(mapping, s_ent_formation_map);
    return false;
}

void G_Formation_Shutdown(void)
{
    ASSERT_IN_MAIN_THREAD();
    s_map = NULL;

    struct formation *formation;
    kh_foreach_ptr(s_formations, formation, {
        destroy_formation(formation);
    });

    E_Global_Unregister(EVENT_UPDATE_START, on_update_start);
    E_Global_Unregister(EVENT_RENDER_3D_POST, on_render_3d);
    kh_destroy(formation, s_formations);
    kh_destroy(mapping, s_ent_formation_map);
}

void G_Formation_Create(vec2_t target, const vec_entity_t *ents)
{
    ASSERT_IN_MAIN_THREAD();
    formation_id_t fid = s_next_id++;

    /* Add a mapping from entities to the formation */
    for(int i = 0; i < vec_size(ents); i++) {
        uint32_t uid = vec_AT(ents, i);
        int ret;
        khiter_t k = kh_put(mapping, s_ent_formation_map, uid, &ret);
        assert(ret != -1);
        kh_val(s_ent_formation_map, k) = fid;
    }

    int ret;
    khiter_t k = kh_put(formation, s_formations, fid, &ret);
    assert(ret != -1);
    struct formation *new = &kh_val(s_formations, k);

    vec2_t orientation = compute_orientation(target, ents);
    *new = (struct formation){
        .refcount = kh_size(ents),
        .type = FORMATION_RANK,
        .target = target,
        .orientation = orientation,
        .center = field_center(target, orientation),
        .ents = copy_vector(ents),
        .sub_assignment = kh_init(assignment)
    };
    init_subformations(new);

    enum nav_layer layers[NAV_LAYER_MAX];
    size_t nlayers = formation_layers(&new->subformations, layers);
    for(int i = 0; i < nlayers; i++) {
        init_occupied_field(layers[i], new->center, new->occupied[layers[i]]);
        init_islands_field(layers[i], new->center, new->islands[layers[i]]);
    }

    for(int i = 0; i < vec_size(&new->subformations); i++) {
        struct subformation *sub = &vec_AT(&new->subformations, i);
        place_subformation(sub, new->center, target, new->orientation, 
            new->occupied, new->islands);
        compute_cell_assignment(sub);
        dispatch_cell_field_work(sub);
    }
}

formation_id_t G_Formation_GetForEnt(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(mapping, s_ent_formation_map, uid);
    assert(k != kh_end(s_ent_formation_map));
    return kh_val(s_ent_formation_map, k);
}

void G_Formation_RemoveUnit(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    formation_id_t fid = G_Formation_GetForEnt(uid);
    khiter_t k = kh_get(formation, s_formations, fid);
    assert(k != kh_end(s_formations));
    struct formation *formation = &kh_val(s_formations, k);

    khiter_t l = kh_get(entity, formation->ents, uid);
    assert(l != kh_end(formation->ents));
    kh_del(entity, formation->ents, l);
    /* Remove the entity assignment */

    if(--formation->refcount == 0) {
        destroy_formation(formation);
        kh_del(formation, s_formations, k);
    }
}

