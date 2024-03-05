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
#include "movement.h"
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
#include "../lib/public/attr.h"
#include "../navigation/public/nav.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../phys/public/collision.h"

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
#define MOVE_BUFFER_DIST         (5.0f)
#define SUBFORMATION_BUFFER_DIST (8.0f)
#define SIGNUM(x)                (((x) > 0) - ((x) < 0))
#define EPSILON                  (1.0f/1024)
#define FIELD_RECOMPUTE_INTERVAL (1.0f) /* seconds */
#define MAX_CELL_ASSIGNMENT_WORK (256)
#define IDX(r, width, c)         (r * width + c)

#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

#define CHK_TRUE_JMP(_pred, _label)     \
    do{                                 \
        if(!(_pred))                    \
            goto _label;                \
    }while(0)

enum cell_state{
    CELL_NOT_PLACED,
    CELL_OCCUPIED,
    CELL_NOT_OCCUPIED,
    CELL_NOT_USED
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
    /* The last known reachable position 
     * that is maximally close to 'pos'.
     */
    vec2_t          reachable_pos;
};

struct range2d{
    int min_r, max_r;
    int min_c, max_c;
};

struct cell_arrival_field{
    uint8_t raw[CELL_ARRIVAL_FIELD_RES * CELL_ARRIVAL_FIELD_RES / 2];
};

VEC_TYPE(cell, struct cell)
VEC_IMPL(static inline, cell, struct cell)

struct cell_field_work_input{
    enum nav_layer   layer;
    uint16_t         enemy_faction_mask;
    struct tile_desc cell_tile;
    struct tile_desc center_tile;
    struct tile_desc curr_tile;
};

struct refcounted_map{
    SDL_atomic_t  refcount;
    struct map   *snapshot;
};

struct cell_field_work{
    bool                         consumed;
    bool                         recompute_pending;
    uint32_t                     last_update_ticks;
    struct refcounted_map       *map;
    uint32_t                     tid;
    uint32_t                     uid;
    struct future                future;
    struct cell_field_work_input input;
    struct cell_arrival_field    result;
};

VEC_TYPE(work, struct cell_field_work)
VEC_IMPL(static inline, work, struct cell_field_work)

KHASH_MAP_INIT_INT(assignment, struct coord)
KHASH_MAP_INIT_INT(reverse, uint32_t);
KHASH_MAP_INIT_INT(result, struct cell_arrival_field*)
KHASH_MAP_INIT_INT64(map, struct refcounted_map*)

QUEUE_TYPE(coord, struct coord)
QUEUE_IMPL(static, coord, struct coord)

struct block_event{
    enum eventtype type;
    void          *arg;
    uint32_t       tick_recorded;
};

QUEUE_TYPE(event, struct block_event)
QUEUE_IMPL(static, event, struct block_event)

enum subformation_state{
    SUBFORMATION_COMPUTING_ASSIGNMENT,
    SUBFORMATION_READY
};

struct subformation{
    enum subformation_state state;
    /* Subformations are stored in an acyclic tree structure
     * and are placed relative to their parent with some constaints.
     */
    struct subformation *parent;
    size_t               nchildren;
    struct subformation *children[MAX_CHILDREN];
    float                unit_radius;
    enum nav_layer       layer;
    int                  faction_id;
    vec2_t               reachable_target;
    vec2_t               pos;
    vec2_t               orientation;
    size_t               nrows;
    size_t               ncols;
    /* Each bit correspongs to an index in the subformation array.
     * Indicates which lower-priority subformation cells should act
     * as blockers that the entities go around.
     */
    uint64_t             blocked;
    khash_t(entity)     *ents;
    /* Each cell holds a single unit from the subformation
     */
    vec_cell_t           cells;
    /* A mapping between entities and a cell within the formation 
     */
    khash_t(assignment) *assignment;
    /* Reverse mapping between cells and entities
     */
    khash_t(reverse)    *reverse;
    /* A future for the task responsible for computing the 
     * cell arrival field. Each task is responsible for computing
     * a single field, so there is one task per entity/cell.
     */
    khash_t(result)     *results;
    vec_work_t           futures;
};

VEC_TYPE(subformation, struct subformation)
VEC_IMPL(static inline, subformation, struct subformation)

struct cell_assignment_work{
    bool                 destroyed;
    /* Input */
    khash_t(entity)     *ents;
    khash_t(pos)        *positions;
    size_t               nrows, ncols;
    formation_id_t       fid;
    size_t               subformation_idx;
    /* Input/output */
    vec_cell_t           cells;
    /* Output */
    khash_t(assignment) *assignment;
    khash_t(reverse)    *reverse;
    /* Job data */
    uint32_t             tid;
    struct future        future;
};

VEC_TYPE(assignment_work, struct cell_assignment_work)
VEC_IMPL(static inline, assignment_work, struct cell_assignment_work)

struct formation{
    /* The refcount is the number of movement system
     * entities associated with this formation.
     */
    int                  refcount;
    enum formation_type  type;
    vec2_t               target;
    vec2_t               orientation;
    vec2_t               center;
    khash_t(entity)     *ents;
    /* The minimum speed of all units in the formation,
     * used by all units moving in this formation.
     */
    float                speed;
    /* The tick during which this formation was created.
     */
    uint32_t             created_tick;
    /* A mapping between entities and subformations 
     */
    khash_t(assignment) *sub_assignment;
    /* The subformation tree
     */
    struct subformation *root;
    vec_subformation_t   subformations;
    /* Map snapshots to be used for asynchronous field computation.
     */
    khash_t(map)        *map_snapshots;
    /* The map tiles which have already been allocated to cells.
     * Centered at the target position.
     */
    uint8_t              occupied[NAV_LAYER_MAX][OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES];
    /* A copy of the navigation 'island' field for the area specified
     * by the 'occupied' field.
     */
    uint16_t             islands[NAV_LAYER_MAX][OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES];
    /* State associated with outstanding cell assignment computations 
     */
    vec_assignment_work_t work;
};

KHASH_MAP_INIT_INT(formation, struct formation)
KHASH_MAP_INIT_INT(mapping, formation_id_t);
KHASH_MAP_INIT_INT(type, enum formation_type);

static void complete_cell_assignment_work(struct cell_assignment_work *work, bool yield);
static void cell_assignment_work_destroy(struct cell_assignment_work *work);
static void collect_cell_assignment_result(const struct cell_assignment_work *work, 
                                           struct subformation *out);

static void complete_cell_field_work(struct subformation *formation, bool yield);
static struct cell_arrival_field *cell_get_field(uint32_t uid);
static enum flow_dir cell_get_dir(const struct cell_arrival_field *field, int r, int c);
static void recompute_cell_arrival_fields(struct formation *parent, vec2_t center, 
                                          struct subformation *formation);

static uint32_t subformation_leader(struct subformation *formation);
static void subformation_anchor_and_heading(uint32_t leader,
                                            vec2_t *out_anchor, vec2_t *out_heading);
static bool in_front_row(uint32_t uid, uint32_t leader, struct subformation *formation, 
                         int *out_col_offset);
static vec2_t entity_target_position(vec2_t anchor, vec2_t heading, float distance);
static uint32_t unit_in_front(uint32_t uid, struct subformation *formation);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map   *s_map;
static khash_t(mapping)   *s_ent_formation_map;
static khash_t(formation) *s_formations;
static khash_t(type)      *s_preferred;
static formation_id_t      s_next_id;
static SDL_TLSID           s_workspace;
static queue_event_t       s_events;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static size_t workspace_size(void)
{
    size_t padding = 64;
    size_t count = CELL_ARRIVAL_FIELD_RES * CELL_ARRIVAL_FIELD_RES;
    return count * (sizeof(float) + sizeof(bool) + sizeof(struct tile_desc) * 2) + padding;
}

static void *get_workspace(void)
{
    void *ret;
    if((ret = SDL_TLSGet(s_workspace)))
        return ret;

    size_t sz = workspace_size();
    ret = malloc(sz);
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
	return 0;
}

static size_t nrows(enum formation_type type, size_t nunits)
{
    return ceilf(nunits / ncols(type, nunits));
}

static float formation_speed(const vec_entity_t *ents)
{
    float min = INFINITY;
    for(int i = 0; i < vec_size(ents); i++) {
        uint32_t uid = vec_AT(ents, i);
        float speed = 0.0f;
        G_Move_GetMaxSpeed(uid, &speed);
        min = MIN(min, speed);
    }
    return min;
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
                            float radius, enum nav_layer layer, int anchor, bool commit,
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
    if(commit) {
        for(int i = 0; i < ndescs; i++) {
            struct coord coord = (struct coord){descs[i].tile_r, descs[i].tile_c};
            for(int j = 0; j < NAV_LAYER_MAX; j++) {
                occupied[j][coord.r][coord.c] = TILE_ALLOCATED;
            }
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

static vec2_t bin_to_tile_clamped(vec2_t pos, vec2_t center)
{
    struct coord tile = pos_to_tile(center, pos);
    tile.r = CLAMP(tile.r, 0, OCCUPIED_FIELD_RES-1);
    tile.c = CLAMP(tile.c, 0, OCCUPIED_FIELD_RES-1);
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
    int direction_mask, vec2_t center, vec2_t orientation, float radius, enum nav_layer layer,
    uint8_t occupied[NAV_LAYER_MAX][OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES],
    uint16_t islands[OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES])
{
    if(try_occupy_cell(curr, orientation, iid, radius, layer,
                       direction_mask, false, occupied, islands)) {
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
        && (try_occupy_cell(&test_tile, orientation, iid, radius, layer,
                            (int)direction_mask, false, occupied, islands))) {
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
            if(abs(dr) != delta && abs(dc) != delta)
                continue;

            int abs_r = curr->r + dr;
            int abs_c = curr->c + dc;
            if(abs_r < 0 || abs_r >= OCCUPIED_FIELD_RES)
                continue;
            if(abs_c < 0 || abs_c >= OCCUPIED_FIELD_RES)
                continue;

            struct coord curr = (struct coord){abs_r, abs_c};
            if(try_occupy_cell(&curr, orientation, iid, radius, layer,
                               direction_mask, false, occupied, islands)) {
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

/* Find the X and Y offsets between adjacent cells in a formation, given
 * that there are no obstacles. These cannot be computed from the 
 * unit radiuses because of the grid-based nature of the 'occupied'
 * field. 
 */
static vec2_t target_direction_offsets(vec2_t center, vec2_t orientation, 
                                       float unit_radius)
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

    /* First find the set of tiles occupied by the root cell */
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
    size_t ndescs = M_Tile_AllUnderCircle(res, root_center, unit_radius, 
        origin, descs, ARR_SIZE(descs));

    /* Place a tile immediately to the front of this tile. Start with the
     * minimum possible distance and go forward in unit-sized increments 
     * along the direction vector.
     */
    float minimal_distance = unit_radius * 2 + UNIT_BUFFER_DIST;
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
        size_t front_ndescs = M_Tile_AllUnderCircle(res, candidate, unit_radius, 
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
        size_t right_ndescs = M_Tile_AllUnderCircle(res, candidate, unit_radius, 
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
    /* This case should not be hit under normal conditions, as 
     * the 'target' position should be on the same island as the 
     * formation units. 
     */
    if(iid == UINT16_MAX)
        return false;

    bool exists = nearest_free_tile(&target_tile, &curr_tile, iid, anchor, 
        center, orientation, radius, layer, occupied, islands[layer]);
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
            anchor, true, occupied, islands[layer]);
        if(!success) {
            occupied[layer][curr_tile.r][curr_tile.c] = TILE_VISITED;
            visited[nvisited++] = curr_tile;
            bool exists = nearest_free_tile(&curr_tile, &curr_tile, iid, anchor, 
                center, orientation, radius, layer, occupied, islands[layer]);
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
        curr->reachable_pos = curr->pos;
    }
    return success;
}

static void init_occupied_field(const struct map *map, enum nav_layer layer, vec2_t center,
                                uint8_t occupied[OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES])
{
    PERF_ENTER();

    struct map_resolution res;
    M_NavGetResolution(map, &res);
    vec3_t map_pos = M_GetPos(map);

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
        if(!M_NavPositionPathable(map, layer, center)
        ||  M_NavPositionBlocked(map, layer, center)) {
            occupied[r][c] = TILE_BLOCKED;
            continue;
        }
    }}

    PERF_RETURN_VOID();
}

static void init_islands_field(const struct map *map, enum nav_layer layer, vec2_t center,
                               uint16_t islands[OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES])
{
    M_NavCopyIslandsFieldView(map, center, OCCUPIED_FIELD_RES, OCCUPIED_FIELD_RES,
        layer, (uint16_t*)islands);
}

static vec2_t back_row_average_pos(struct subformation *formation)
{
    size_t row = 0;
    vec2_t total = (vec2_t){0.0f, 0.0f};
    size_t nadded = 0;
    for(int i = 0; i < formation->ncols; i++) {
        struct cell *curr = &vec_AT(&formation->cells, CELL_IDX(row, i, formation->ncols));
        if(curr->state == CELL_NOT_PLACED || curr->state == CELL_NOT_USED)
            continue;
        PFM_Vec2_Add(&total, &curr->pos, &total);
        nadded++;
    }
    if(nadded > 0) {
        PFM_Vec2_Scale(&total, 1.0f / nadded, &total);
    }
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

static struct subformation *subformation_for_ent(struct formation *formation, uint32_t uid)
{
    khiter_t k = kh_get(assignment, formation->sub_assignment, uid);
    assert(k != kh_end(formation->sub_assignment));
    size_t sub_idx = kh_val(formation->sub_assignment, k).r;
    return &vec_AT(&formation->subformations, sub_idx);
}

static vec2_t subformation_center(struct subformation *formation)
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

static vec2_t formation_center_of_mass(vec_entity_t *ents)
{
    vec2_t ret = (vec2_t){0.0f, 0.0f};
    size_t nents = vec_size(ents);

    uint32_t uid;
    for(int i = 0; i < nents; i++) {
        uint32_t uid = vec_AT(ents, i);
        vec2_t pos = G_Pos_GetXZ(uid);
        PFM_Vec2_Add(&ret, &pos, &ret);
    }
    PFM_Vec2_Scale(&ret, 1.0f / nents, &ret);
    return ret;
}

static vec2_t formation_average_orientation(vec_entity_t *ents)
{
    vec4_t front = (vec4_t){0.0f, 0.0f, 1.0f, 1.0f};
    vec2_t ret = (vec2_t){0.0f, 0.0f};
    size_t nents = vec_size(ents);

    uint32_t uid;
    for(int i = 0; i < nents; i++) {

        uint32_t uid = vec_AT(ents, i);
        quat_t rot = Entity_GetRot(uid); 

        mat4x4_t rot_mat;
        PFM_Mat4x4_RotFromQuat(&rot, &rot_mat);

        vec4_t dir;
        PFM_Mat4x4_Mult4x1(&rot_mat, &front, &dir);
        vec2_t dir_xz = (vec2_t){dir.x, dir.z};
        PFM_Vec2_Add(&ret, &dir_xz, &ret);
    }
    PFM_Vec2_Normal(&ret, &ret);
    return ret;
}

static vec2_t target_position(vec_entity_t *ents)
{
    if(vec_size(ents) == 1) {
        uint32_t uid = vec_AT(ents, 0);
        return G_Pos_GetXZ(uid);
    }

    vec2_t com = formation_center_of_mass(ents);
    vec2_t dir = formation_average_orientation(ents);
    float theta = atan2(dir.z, dir.x) - M_PI/2.0f;

    float minx = INT_MAX, minz = INT_MAX, maxx = INT_MIN, maxz = INT_MIN;
    for(int i = 0; i < vec_size(ents); i++) {
        uint32_t uid = vec_AT(ents, i);
        vec2_t pos = G_Pos_GetXZ(uid);
        vec2_t delta;
        PFM_Vec2_Sub(&pos, &com, &delta);

        vec2_t rotated = (vec2_t){
            -delta.x * cos(theta) - delta.z * sin(theta),
            -delta.x * sin(theta) + delta.z * cos(theta)
        };

        minx = MIN(minx, rotated.x);
        minz = MIN(minz, rotated.z);
        maxx = MAX(maxx, rotated.x);
        maxz = MAX(maxz, rotated.z);
    }

    struct box box = (struct box){
        maxx, minz,
        maxx - minx, 
        maxz - minz
    };
    struct line_seg_2d seg = (struct line_seg_2d){
        0, 0,
        0, 500
    };
    vec2_t isec[2] = {0};
    C_LineBoxIntersection(seg, box, isec);

    vec2_t delta = dir;
    PFM_Vec2_Normal(&delta, &delta);
    PFM_Vec2_Scale(&dir, PFM_Vec2_Len(&isec[0]), &delta);

    vec2_t ret = com;
    PFM_Vec2_Add(&ret, &delta, &ret);
    return ret;
}

static void place_subformation(struct subformation *formation, vec2_t center, 
    vec2_t target, vec2_t orientation,
    uint8_t occupied[NAV_LAYER_MAX][OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES],
    uint16_t islands[NAV_LAYER_MAX][OCCUPIED_FIELD_RES][OCCUPIED_FIELD_RES])
{
    PERF_ENTER();

    vec2_t target_orientation = orientation;
    vec2_t target_offsets = target_direction_offsets(center, orientation, formation->unit_radius);
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

    formation->pos = subformation_center(formation);
    formation->orientation = orientation;
    PERF_RETURN_VOID();
}

static uint64_t map_table_key(struct formation *formation, struct subformation *sub)
{
    uint64_t idx = 0;
    for(int i = 0; i < vec_size(&formation->subformations); i++) {
        struct subformation *curr = &vec_AT(&formation->subformations, i);
        if(curr == sub) {
            idx = i;
            break;
        }
    }
    return (g_frame_idx << 8) | idx;
}

static float average_distance_to_target(struct subformation *formation)
{
    /* Find the entity center of mass for the subformation */
    vec2_t ent_com = (vec2_t){0.0f, 0.0f};
    size_t nents = kh_size(formation->ents);

    uint32_t uid;
    kh_foreach_key(formation->ents, uid, {
        vec2_t pos = G_Pos_GetXZ(uid);
        PFM_Vec2_Add(&ent_com, &pos, &ent_com);
    });
    if(nents == 0)
        return 0.0f;
    PFM_Vec2_Scale(&ent_com, 1.0f / nents, &ent_com);

    /* Find the cell center of mass for the target cells */
    vec2_t cell_com = (vec2_t){0.0f, 0.0f};
    size_t ncells = 0;
    for(int i = 0; i < vec_size(&formation->cells); i++) {
        struct cell *cell = &vec_AT(&formation->cells, i);
        if(cell->state != CELL_OCCUPIED)
            continue;
        PFM_Vec2_Add(&cell_com, &cell->pos, &cell_com);
        ncells++;
    }
    if(ncells > 0) {
        PFM_Vec2_Scale(&cell_com, 1.0f / ncells, &cell_com);
    }

    vec2_t delta;
    PFM_Vec2_Sub(&cell_com, &ent_com, &delta);
    return PFM_Vec2_Len(&delta);
}

static void map_block_cells(struct map *map, struct subformation *sub)
{
    int faction_id = sub->faction_id;
    float radius = sub->unit_radius;

    for(int i = 0; i < vec_size(&sub->cells); i++) {
        struct cell *cell = &vec_AT(&sub->cells, i);
        if(cell->state != CELL_OCCUPIED)
            continue;
        M_NavBlockersIncref(cell->pos, radius, faction_id, 0, map);
        M_NavBlockersIncref(cell->pos, radius, faction_id, ENTITY_FLAG_WATER, map);
    }
}

/* Block out all cells for lower priority subformations that are (on average) 
 * closer to their target location than the current subformation. This will
 * cause entities to go around the whole subformation area, instead of attempting
 * to go directly through it and resulting in a lot of unit collisions and field 
 * re-computations that will then, anyways, lead the units around the lower priority 
 * subformation.
 */
static void map_add_blockers(struct map *map, struct formation *formation, struct subformation *sub)
{
    for(int i = 0; i < vec_size(&formation->subformations); i++) {
        struct subformation *curr = &vec_AT(&formation->subformations, i);
        if(sub->blocked & (((uint64_t)0x1) << i)) {
            map_block_cells(map, curr);
        }
    }
}

static uint64_t compute_blocked(struct formation *formation, struct subformation *sub)
{
    uint64_t ret = 0;
    int idx = 0;
    for(int i = 0; i < vec_size(&formation->subformations); i++) {
        struct subformation *curr = &vec_AT(&formation->subformations, i);
        if(curr == sub) {
            idx = i;
            break;
        }
    }

    float sub_distance = average_distance_to_target(sub);
    const float BUFFER = 25.0f;
    for(int i = idx + 1; i < vec_size(&formation->subformations); i++) {
        if(i >= 64)
            break;
        struct subformation *curr = &vec_AT(&formation->subformations, i);
        float curr_distance = average_distance_to_target(curr);
        if(curr_distance + BUFFER < sub_distance) {
            ret |= ((uint64_t)0x1) << i;
        }
    }
    return ret;
}

static void compute_all_blocked(struct formation *formation)
{
    for(int i = 0; i < vec_size(&formation->subformations); i++) {
        struct subformation *curr = &vec_AT(&formation->subformations, i);
        curr->blocked = compute_blocked(formation, curr);
    }
}

static struct refcounted_map *map_snapshot_get(struct formation *formation, 
                                               struct subformation *sub)
{
    ASSERT_IN_MAIN_THREAD();

    uint64_t key = map_table_key(formation, sub);
    khiter_t k = kh_get(map, formation->map_snapshots, key);
    if(k == kh_end(formation->map_snapshots)) {
        int ret;
        k = kh_put(map, formation->map_snapshots, key, &ret);
        assert(ret != -1);

        struct refcounted_map *rmap = malloc(sizeof(struct refcounted_map));
        rmap->snapshot = M_AL_CopyWithFields(s_map);
        map_add_blockers(rmap->snapshot, formation, sub);
        SDL_AtomicSet(&rmap->refcount, 0);
        kh_val(formation->map_snapshots, k) = rmap;
    }
    struct refcounted_map *rmap = kh_val(formation->map_snapshots, k);
    SDL_AtomicIncRef(&rmap->refcount);
    return rmap;
}

static void clean_up_map_snapshots(struct formation *formation)
{
    ASSERT_IN_MAIN_THREAD();

    uint64_t todel[1024];
    size_t ndel = 0;

    uint64_t key;
    struct refcounted_map *rmap;
    kh_foreach(formation->map_snapshots, key, rmap, {
        if(ndel == ARR_SIZE(todel))
            break;
        int refcnt = SDL_AtomicGet(&rmap->refcount);
        if(refcnt == 0) {
            todel[ndel++] = key;
        }
    });

    for(int i = 0; i < ndel; i++) {
        khiter_t k = kh_get(map, formation->map_snapshots, todel[i]);
        assert(k != kh_end(formation->map_snapshots));
        struct refcounted_map *rmap = kh_val(formation->map_snapshots, k);
        M_AL_FreeCopyWithFields(rmap->snapshot);
        PF_FREE(rmap);
        kh_del(map, formation->map_snapshots, k);
    }
}

static void mark_unused_cells(struct subformation *formation)
{
    size_t ncells = formation->nrows * formation->ncols;
    size_t nents = kh_size(formation->ents);
    if(nents == ncells)
        return;

    size_t nplaced = ncells;
    for(int i = 0; i < ncells; i++) {
        if(vec_AT(&formation->cells, i).state == CELL_NOT_PLACED)
            nplaced--;
    }

    if(nplaced <= nents)
        return;

    /* Make all not placed cells as not used */
    for(int i = 0; i < ncells; i++) {
        struct cell *cell = &vec_AT(&formation->cells, i);
        if(cell->state == CELL_NOT_PLACED)
            cell->state = CELL_NOT_USED;
    }

    size_t nexcess = nplaced - nents;
    size_t left = 0, right = 0;
    while(nexcess > 0) {
        if(left <= right) {
            /* Mark left-most back row cell */
            size_t idx = CELL_IDX(0, left, formation->ncols);
            vec_AT(&formation->cells, idx).state = CELL_NOT_USED;
            left++;
        }else{
            /* Mark right-most back row cell */
            size_t idx = CELL_IDX(0, formation->ncols - 1 - right, formation->ncols);
            vec_AT(&formation->cells, idx).state = CELL_NOT_USED;
            right++;
        }
        nexcess--;
    }
}

static void render_cells(struct subformation *formation)
{
    for(int i = 0; i < vec_size(&formation->cells); i++) {
        struct cell *curr = &vec_AT(&formation->cells, i);
        if(curr->state == CELL_NOT_PLACED
        || curr->state == CELL_NOT_USED)
            continue;

        const vec2_t pos = curr->pos;
        const vec3_t white = (vec3_t){1.0f, 1.0f, 1.0f};
        const float radius = formation->unit_radius;
        const float width = 0.5f;

        R_PushCmd((struct rcmd){
            .func = R_GL_DrawSelectionCircle,
            .nargs = 5,
            .args = {
                R_PushArg(&pos, sizeof(pos)),
                R_PushArg(&radius, sizeof(radius)),
                R_PushArg(&width, sizeof(width)),
                R_PushArg(&white, sizeof(white)),
                (void*)G_GetPrevTickMap(),
            },
        });
    }
}

static bool compare_priorities(uint32_t a, uint32_t b, uint64_t typea, uint64_t typeb)
{
    if(S_FormationPriority(a) == S_FormationPriority(b))
        return (typea > typeb);
    return (S_FormationPriority(a) > S_FormationPriority(b));
}

static size_t sort_by_type(size_t size, uint32_t *ents, uint64_t *types)
{
    if(size == 0)
        return 0;

    int i = 1;
    while(i < size) {
        int j = i;
        while(j > 0 && compare_priorities(ents[j-1], ents[j], types[j-1], types[j])) {

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
    if(size == 1) {
        *out_count = 1;
        return 1;
    }
    size_t count = 0;
    int i = begin;
    for(; i < size-1; i++) {
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

    enum nav_layer layer = Entity_NavLayer(ents[0]);
    vec2_t first_ent_pos = G_Pos_GetXZ(ents[0]);
    vec2_t reachable_target = M_NavClosestReachableDest(s_map, layer, 
        first_ent_pos, target);

    size_t curr_child = 0;
    while((curr_child < nchildren) && (curr_child < MAX_CHILDREN)) {
        formation->children[curr_child] = children[curr_child];
        curr_child++;
    }
    formation->nchildren = nchildren;
    formation->parent = parent;
    formation->nrows = nrows;
    formation->ncols = ncols;
    formation->unit_radius = G_GetSelectionRadius(ents[0]);
    formation->layer = layer;
    formation->faction_id = G_GetFactionID(ents[0]);
    formation->reachable_target = reachable_target;
    formation->assignment = kh_init(assignment);
    formation->reverse = kh_init(reverse);
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
static void create_cost_matrix(struct cell_assignment_work *work, int *out_costs, 
                               struct coord *out_idx_to_cell)
{
    size_t nents = kh_size(work->ents);
    int *out_rows = out_costs;

    size_t cell_idx = 0;
    for(int i = 0; i < nents; i++) {
        struct cell *cell;
        do{
            cell = &vec_AT(&work->cells, cell_idx);
            cell_idx++;
        }while(cell->state == CELL_NOT_USED);
        out_idx_to_cell[i] = (struct coord){
            (cell_idx-1) / work->ncols,
            (cell_idx-1) % work->ncols
        };
    }
    assert(cell_idx == work->nrows * work->ncols);

    int i = 0;
    uint32_t uid;
    kh_foreach_key(work->ents, uid, {

        vec2_t pos = G_Pos_GetXZFrom(work->positions, uid);
        size_t cell_idx = 0;
        for(int j = 0; j < nents; j++) {
            struct coord cell_coord = out_idx_to_cell[j];
            size_t cell_idx = CELL_IDX(cell_coord.r, cell_coord.c, work->ncols);
            struct cell *cell = &vec_AT(&work->cells, cell_idx);
            if(cell->state == CELL_NOT_PLACED) {
                out_rows[IDX(i, nents, j)] = INT_MAX;
            }else{
                vec2_t delta;
                PFM_Vec2_Sub(&cell->pos, &pos, &delta);
                /* Scale the resolution by 100 to keep 2 points of precision
                 * after the decimal in the integer distance. Squaring the 
                 * distance adds an additional penalty for a unit 'overtaking'
                 * another one in the formation. */
                float squared_distance = powf(PFM_Vec2_Len(&delta) * 100, 2);
                out_rows[IDX(i, nents, j)] = squared_distance;
            }
        }
        i++;
    });
}

static int row_minimum(int *costs, int irow, size_t nents)
{
    int *rows = costs;
    int min = rows[IDX(irow, nents, 0)];
    for(int i = 1; i < nents; i++) {
        min = MIN(min, rows[IDX(irow, nents, i)]);
    }
    return min;
}

static int column_minimum(int *costs, int icol, size_t nents)
{
    int *rows = costs;
    int min = rows[IDX(0, nents, icol)];
    for(int i = 1; i < nents; i++) {
        min = MIN(min, rows[IDX(i, nents, icol)]);
    }
    return min;
}

static bool assigned_in_column(bool *starred, size_t nents, size_t icol)
{
    bool *rows = (void*)starred;
    for(int i = 0; i < nents; i++) {
        if(rows[IDX(i, nents, icol)])
            return true;
    }
    return false;
}

static bool row_is_covered(bool *covered, size_t nents, size_t irow)
{
    bool *rows = covered;
    size_t ncovered = 0;
    for(int i = 0; i < nents; i++) {
        if(rows[IDX(irow, nents, i)])
            ncovered++;
    }
    return (ncovered == nents);
}

static void cover_column(bool *covered, size_t nents, size_t icol)
{
    bool *rows = covered;
    for(int i = 0; i < nents; i++) {
        rows[IDX(i, nents, icol)] = true;
    }
}

static void uncover_column(bool *covered, size_t nents, size_t icol)
{
    bool *rows = covered;
    for(int i = 0; i < nents; i++) {
        if(!row_is_covered(covered, nents, i)) {
            rows[IDX(i, nents, icol)] = false;
        }
    }
}

static void cover_row(bool *covered, size_t nents, size_t irow)
{
    bool *rows = (void*)covered;
    for(int i = 0; i < nents; i++) {
        rows[IDX(irow, nents, i)] = true;
    }
}

static bool row_has_starred(bool *starred, size_t nents, size_t irow, int *out_col)
{
    bool *rows = (void*)starred;
    for(int i = 0; i < nents; i++) {
        if(rows[IDX(irow, nents, i)]) {
            *out_col = i;
            return true;
        }
    }
    return false;
}

static bool column_is_covered(bool *covered, size_t nents, size_t icol)
{
    bool *rows = (void*)covered;
    size_t ncovered = 0;
    for(int i = 0; i < nents; i++) {
        if(rows[IDX(i, nents, icol)])
            ncovered++;
    }
    return (ncovered == nents);
}

static bool column_has_starred(bool *starred, size_t nents, size_t icol, int *out_row)
{
    bool *rows = (void*)starred;
    for(int i = 0; i < nents; i++) {
        if(rows[IDX(i, nents, icol)]) {
            *out_row = i;
            return true;
        }
    }
    return false;
}

static int primed_zero_at_row(bool *primed, size_t nents, size_t irow)
{
    bool *rows = (void*)primed;
    for(int i = 0; i < nents; i++) {
        if(rows[IDX(irow, nents, i)])
            return i;
    }
    assert(0);
    return -1;
}

static size_t count_covered_rows(bool *covered, size_t nents)
{
    bool *rows = (void*)covered;
    size_t ret = 0;
    for(int i = 0; i < nents; i++) {
        size_t ncovered = 0;
        for(int j = 0; j < nents; j++) {
            if(rows[IDX(i, nents, j)])
                ncovered++;
        }
        if(ncovered == nents)
            ret++;
    }
    return ret;
}

static size_t count_covered_columns(bool *covered, size_t nents)
{
    bool *rows = covered;
    size_t ret = 0;
    for(int i = 0; i < nents; i++) {
        size_t ncovered = 0;
        for(int j = 0; j < nents; j++) {
            if(rows[IDX(j, nents, i)])
                ncovered++;
        }
        if(ncovered == nents)
            ret++;
    }
    return ret;
}

static void dump_cost_matrix_only(int *costs, size_t nents)
{
    int *cost_rows = costs;
    for(int r = 0; r < nents; r++) {
        for(int c = 0; c < nents; c++) {
            printf("%03d", cost_rows[IDX(r, nents, c)]);
            if(c != nents-1)
                printf("  ");
        }
        printf("\n");
    }
}

static void dump_cost_matrix(int *costs, size_t nents,
                             bool *starred, bool *covered, bool *primed)
{
    int  *cost_rows = costs;
    bool *starred_rows = starred;
    bool *covered_rows = covered;
    bool *primed_rows = primed;

    for(int r = 0; r < nents; r++) {
        for(int c = 0; c < nents; c++) {
            printf("%03d%c%c%c", 
                cost_rows[IDX(r, nents, c)],
                starred_rows[IDX(r, nents, c)] ? '*'  : ' ',
                primed_rows[IDX(r, nents, c)]  ? '\'' : ' ',
                covered_rows[IDX(r, nents, c)] ? 'C'  : ' ');
            if(c != nents-1)
                printf("  ");
        }
        printf("\n");
    }
}

static int min_uncovered_value(int *costs, bool *covered, size_t nents)
{
    int  *cost_rows = costs;
    bool *covered_rows = (void*)covered;

    int min = INT_MAX;
    for(int r = 0; r < nents; r++) {
        for(int c = 0; c < nents; c++) {
            if(!covered_rows[IDX(r, nents, c)]) {
                min = MIN(cost_rows[IDX(r, nents, c)], min);
            }
        }
    }
    return min;
}

static int min_lines_to_cover_zeroes(int *costs, int *out_next, 
                                     struct coord *out_assignment, size_t nents)
{
    STALLOC(bool, starred, nents * nents);
    STALLOC(bool, covered, nents * nents);
    STALLOC(bool, primed, nents * nents);

    memset(starred, 0, nents * nents * sizeof(bool));
    memset(covered, 0, nents * nents * sizeof(bool));
    memset(primed, 0, nents * nents * sizeof(bool));

    int  *cost_rows = costs;
    bool *starred_rows = starred;
    bool *covered_rows = covered;
    bool *primed_rows = primed;

iterate:
    /* For each row, try to assign an arbitrary zero. Assigned tasks
     * are represented by starring a zero. Note that assignments can't 
     * be in the same row or column.
     */
    for(int row = 0; row < nents; row++) {
        for(int col = 0; col < nents; col++) {
            if(starred_rows[IDX(row, nents, col)])
                break;
            if((cost_rows[IDX(row, nents, col)] == 0) && !assigned_in_column(starred, nents, col)) {
                starred_rows[IDX(row, nents, col)] = true;
                break;
            }
        }
    }

    /* Cover all columns containing a (starred) zero.
     */
    for(int row = 0; row < nents; row++) {
        for(int col = 0; col < nents; col++) {
            if(starred_rows[IDX(row, nents, col)])
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
                if(cost_rows[IDX(row, nents, col)] == 0 && !covered_rows[IDX(row, nents, col)]) {
                    has_uncovered = true;
                    primed_rows[IDX(row, nents, col)] = true;
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
                    assert(starred_rows[IDX(curr.r, nents, curr.c)] ^ primed_rows[IDX(curr.r, nents, curr.c)]);
                    if(starred_rows[IDX(curr.r, nents, curr.c)]) {
                        starred_rows[IDX(curr.r, nents, curr.c)] = false;
                    }else if(primed_rows[IDX(curr.r, nents, curr.c)]) {
                        starred_rows[IDX(curr.r, nents, curr.c)] = true;
                    }
                }

                /* Unprime all primed zeroes and uncover all tiles.
                 */
                for(int i = 0; i < nents; i++) {
                    for(int j = 0; j < nents; j++) {
                        primed_rows[IDX(i, nents, j)] = false;
                        covered_rows[IDX(i, nents, j)] = false;
                    }
                }
                STFREE(path);
                Sched_TryYield();
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
        int *next_rows = out_next;
        memcpy(out_next, costs, sizeof(int) * nents * nents);

        int min = min_uncovered_value(costs, covered, nents);
        for(int r = 0; r < nents; r++) {
            if(!row_is_covered(covered, nents, r)) {
                for(int c = 0; c < nents; c++) {
                    next_rows[IDX(r, nents, c)] -= min;
                }
            }
        }
        for(int c = 0; c < nents; c++) {
            if(column_is_covered(covered, nents, c)) {
                for(int r = 0; r < nents; r++) {
                    next_rows[IDX(r, nents, c)] += min;
                }
            }
        }
    }else{
        int i = 0;
        for(int r = 0; r < nents; r++) {
            for(int c = 0; c < nents; c++) {
                if(starred_rows[IDX(r, nents, c)]) {
                    out_assignment[i++] = (struct coord){r, c};
                }
            }
        }
        assert(i == nents);
    }

    STFREE(starred);
    STFREE(covered);
    STFREE(primed);

    return ret;
}

/* Use the Hungarian algorithm to find an optimal assignment of entities to cells
 * (minimizing the combined distance that needs to be traveled by the entities).
 */
static void compute_cell_assignment(struct cell_assignment_work *work)
{
    size_t nents = kh_size(work->ents);
    STALLOC(int, costs, nents * nents);
    STALLOC(int, next, nents * nents);
    STALLOC(struct coord, assignment, nents);
    STALLOC(struct coord, idx_to_cell, nents);

    create_cost_matrix(work, costs, idx_to_cell);
    int *rows = costs;

    /* Step 1: Subtract row minima
     * For each row, find the lowest element and subtract it from each element in that row.
     */
    for(int i = 0; i < nents; i++) {
        int row_min = row_minimum(costs, i, nents);
        for(int j = 0; j < nents; j++) {
            rows[IDX(i, nents, j)] -= row_min;
        }
    }

    /* Step  2: Subtract column minima 
     * For each column, find the lowest element and subtract it from each element in that 
     * column.
     */
    for(int i = 0; i < nents; i++) {
        int col_min = column_minimum(costs, i, nents);
        for(int j = 0; j < nents; j++) {
            rows[IDX(j, nents, i)] -= col_min;
        }
    }

    size_t min_lines;
    do{
        Sched_TryYield();
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
    kh_foreach_key(work->ents, uid, {
        /* Add an entity:cell mapping */
        int status;
        khiter_t k = kh_put(assignment, work->assignment, uid, &status);
        assert(status != -1);
        size_t meta_idx = assignment[i].c;
        struct coord cell_coord = idx_to_cell[meta_idx];
        kh_val(work->assignment, k) = cell_coord;
        size_t cell_idx = CELL_IDX(cell_coord.r, cell_coord.c, work->ncols);
        struct cell *cell = &vec_AT(&work->cells, cell_idx);
        if(cell->state != CELL_NOT_PLACED) {
            cell->state = CELL_OCCUPIED;
        }
        /* Add a cell:entity mapping */
        khiter_t l = kh_put(reverse, work->reverse, cell_idx, &status);
        assert(status != -1);
        kh_val(work->reverse, l) = uid;

        i++;
    });

    STFREE(costs);
    STFREE(next);
    STFREE(assignment);
    STFREE(idx_to_cell);
}

static mat4x4_t cell_field_model_matrix(vec2_t center)
{
    struct map_resolution nav_res;
    M_NavGetResolution(s_map, &nav_res);

    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    const float tile_x_dim = chunk_x_dim / nav_res.tile_w;
    const float tile_z_dim = chunk_z_dim / nav_res.tile_h;

    const float field_x_dim = tile_x_dim * CELL_ARRIVAL_FIELD_RES;
    const float field_z_dim = tile_z_dim * CELL_ARRIVAL_FIELD_RES;

    vec2_t binned_center = bin_to_tile(center, center);
    binned_center.x += tile_x_dim / 2.0f;
    binned_center.z -= tile_z_dim / 2.0f;

    vec2_t base;
    vec2_t delta = (vec2_t){field_x_dim/2.0f, -field_z_dim/2.0f};
    PFM_Vec2_Add(&binned_center, &delta, &base);

    mat4x4_t ret;
    PFM_Mat4x4_MakeTrans(base.x, 0.0f, base.z, &ret);
    return ret;
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

        /* Draw the entity's UID over each entity */
        uint32_t uid;
        kh_foreach_key(formation->ents, uid, {

            vec4_t center = (vec4_t){0.0f, 0.0f, 0.0f, 1.0f};
            mat4x4_t model;
            Entity_ModelMatrix(uid, &model);

            char text[16];
            pf_snprintf(text, sizeof(text), "UID: %u", uid);
            N_RenderOverlayText(text, center, &model, &view, &proj);
        });
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
            bool on_water_surface = false;
            R_PushCmd((struct rcmd){
                .func = R_GL_DrawMapOverlayQuads,
                .nargs = 6,
                .args = {
                    R_PushArg(corners_buff + 4 * offset, sizeof(vec2_t) * 4 * num_tiles),
                    R_PushArg(colors_buff + offset, sizeof(vec3_t) * num_tiles),
                    R_PushArg(&num_tiles, sizeof(num_tiles)),
                    R_PushArg(&chunk_model, sizeof(chunk_model)),
                    R_PushArg(&on_water_surface, sizeof(bool)),
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

static void render_cell_arrival_field(struct formation *formation, int index)
{
    if(index >= kh_size(formation->ents))
        return;

    int i = 0;
    uint32_t uid = 0;
    kh_foreach_key(formation->ents, uid, {
        if(i++ == index)
            break;
    });

    struct cell_arrival_field *field = cell_get_field(uid);
    if(!field)
        return;

    assert(sizeof(*field) == ((CELL_ARRIVAL_FIELD_RES * CELL_ARRIVAL_FIELD_RES) / 2));
    struct map_resolution res;
    M_NavGetResolution(s_map, &res);

    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    const float tile_x_dim = chunk_x_dim / res.tile_w;
    const float tile_z_dim = chunk_z_dim / res.tile_h;

    const float field_x_dim = tile_x_dim * CELL_ARRIVAL_FIELD_RES;
    const float field_z_dim = tile_z_dim * CELL_ARRIVAL_FIELD_RES;

    vec2_t positions_buff[CELL_ARRIVAL_FIELD_RES * CELL_ARRIVAL_FIELD_RES];
    vec2_t dirs_buff[CELL_ARRIVAL_FIELD_RES * CELL_ARRIVAL_FIELD_RES];

    size_t count = 0;
    mat4x4_t model = cell_field_model_matrix(formation->center);

    for(int r = 0; r < CELL_ARRIVAL_FIELD_RES; r++) {
    for(int c = 0; c < CELL_ARRIVAL_FIELD_RES; c++) {

        float square_x_len = (1.0f / res.tile_w) * chunk_x_dim;
        float square_z_len = (1.0f / res.tile_h) * chunk_z_dim;
        float square_x = CLAMP(-(((float)c) / CELL_ARRIVAL_FIELD_RES) * field_x_dim, 
                               -field_x_dim, field_x_dim);
        float square_z = CLAMP((((float)r) / CELL_ARRIVAL_FIELD_RES) * field_z_dim, 
                               -field_z_dim, field_z_dim);

        vec2_t pos = (vec2_t){
            square_x - square_x_len / 2.0f,
            square_z + square_z_len / 2.0f
        };
        vec4_t point = (vec4_t){pos.x, 0.0f, pos.z, 1.0f}, raw;
        PFM_Mat4x4_Mult4x1(&model, &point, &raw);
        vec2_t transformed = (vec2_t){raw.x, raw.z};
        if(!M_PointInsideMap(s_map, transformed))
            continue;

        positions_buff[count] = pos;
        enum flow_dir dir = cell_get_dir(field, r, c);
        dirs_buff[count] = N_FlowDir(dir);
        count++;
    }}

    R_PushCmd((struct rcmd){
        .func = R_GL_DrawFlowField,
        .nargs = 5,
        .args = {
            R_PushArg(positions_buff, sizeof(positions_buff)),
            R_PushArg(dirs_buff, sizeof(dirs_buff)),
            R_PushArg(&count, sizeof(count)),
            R_PushArg(&model, sizeof(model)),
            (void*)G_GetPrevTickMap(),
        },
    });
}

static void render_formation_forces(void)
{
    struct formation *formation;
    kh_foreach_ptr(s_formations, formation, {

        for(int i = 0; i < vec_size(&formation->subformations); i++) {
            struct subformation *curr = &vec_AT(&formation->subformations, i);
            uint32_t leader = subformation_leader(curr);
            if(leader == NULL_UID)
                continue;

            vec2_t leader_pos = G_Pos_GetXZ(leader);
            float radius = G_GetSelectionRadius(leader);
            const float width = 1.5f;
            const vec3_t green = (vec3_t){0.0, 1.0, 0.0};

            R_PushCmd((struct rcmd){
                .func = R_GL_DrawSelectionCircle,
                .nargs = 5,
                .args = {
                    R_PushArg(&leader_pos, sizeof(leader_pos)),
                    R_PushArg(&radius, sizeof(radius)),
                    R_PushArg(&width, sizeof(width)),
                    R_PushArg(&green, sizeof(green)),
                    (void*)G_GetPrevTickMap(),
                },
            });

            vec2_t anchor, heading;
            subformation_anchor_and_heading(leader, &anchor, &heading);
            vec2_t perpendicular = (vec2_t){-heading.z, heading.x};
            float len = curr->ncols * (2 * radius + MOVE_BUFFER_DIST) + 8.0f;
            PFM_Vec2_Normal(&perpendicular, &perpendicular);
            PFM_Vec2_Scale(&perpendicular, len / 2.0f, &perpendicular);

            vec2_t a, b;
            PFM_Vec2_Add(&anchor, &perpendicular, &a);
            PFM_Vec2_Sub(&anchor, &perpendicular, &b);
            vec2_t endpoints[] = {a, b};
            vec3_t blue = (vec3_t){0.0f, 0.0f, 1.0f};

            R_PushCmd((struct rcmd){
                .func = R_GL_DrawLine,
                .nargs = 4,
                .args = {
                    R_PushArg(endpoints, sizeof(endpoints)),
                    R_PushArg(&width, sizeof(width)),
                    R_PushArg(&blue, sizeof(blue)),
                    (void*)G_GetPrevTickMap()
                }
            });

            uint32_t uid;
            kh_foreach_key(curr->ents, uid, {

                int col_offset;
                vec2_t ent_pos = G_Pos_GetXZ(uid);

                if((uid != leader) && in_front_row(uid, leader, curr, &col_offset)) {
                    float distance = -col_offset * (2 * radius + MOVE_BUFFER_DIST);
                    vec2_t target = entity_target_position(anchor, heading, distance);
                    const vec3_t magenta = (vec3_t){1.0, 0.0, 1.0};
                    const float radius = 0.5f;

                    R_PushCmd((struct rcmd){
                        .func = R_GL_DrawSelectionCircle,
                        .nargs = 5,
                        .args = {
                            R_PushArg(&target, sizeof(target)),
                            R_PushArg(&radius, sizeof(radius)),
                            R_PushArg(&width, sizeof(width)),
                            R_PushArg(&magenta, sizeof(magenta)),
                            (void*)G_GetPrevTickMap(),
                        },
                    });

                    vec2_t endpoints[] = {ent_pos, target};
                    float width = 0.5f;

                    R_PushCmd((struct rcmd){
                        .func = R_GL_DrawLine,
                        .nargs = 4,
                        .args = {
                            R_PushArg(endpoints, sizeof(endpoints)),
                            R_PushArg(&width, sizeof(width)),
                            R_PushArg(&magenta, sizeof(magenta)),
                            (void*)G_GetPrevTickMap()
                        }
                    });
                }else if(uid != leader) {
                    uint32_t unit = unit_in_front(uid, curr);
                    if(unit != NULL_UID) {

                        vec2_t from = ent_pos;
                        vec2_t to = G_Pos_GetXZ(unit);
                        vec2_t endpoints[] = {from, to};
                        vec3_t green = (vec3_t){0.0f, 1.0f, 0.0f};
                        float width = 0.5f;

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
                    }
                }

                /* Entities currently experiencing a drag force 
                 * will have a red circle around them */
                vec2_t drag_force = G_Formation_DragForce(uid);
                if(PFM_Vec2_Len(&drag_force) > EPSILON) {
                    
                    vec3_t red = (vec3_t){1.0f, 0.0f, 0.0f};
                    float marker_radius = radius;
                    if(uid == leader) {
                        marker_radius += 1.0f;
                    }

                    R_PushCmd((struct rcmd){
                        .func = R_GL_DrawSelectionCircle,
                        .nargs = 5,
                        .args = {
                            R_PushArg(&ent_pos, sizeof(ent_pos)),
                            R_PushArg(&marker_radius, sizeof(marker_radius)),
                            R_PushArg(&width, sizeof(width)),
                            R_PushArg(&red, sizeof(red)),
                            (void*)G_GetPrevTickMap(),
                        },
                    });
                }
            });
        }
    });
}

static void on_render_3d(void *user, void *event)
{
    enum nav_layer layer;
    struct sval setting;
    int cell_index;
    ss_e status;
    (void)status;

    status = Settings_Get("pf.debug.navigation_layer", &setting);
    assert(status == SS_OKAY);
    layer = setting.as_int;

    status = Settings_Get("pf.debug.formation_cell_index", &setting);
    assert(status == SS_OKAY);
    cell_index = setting.as_int;

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

    status = Settings_Get("pf.debug.show_formations_cell_arrival_field", &setting);
    assert(status == SS_OKAY);
    enabled = setting.as_bool;

    if(enabled) {
        formation_id_t fid;
        enum selection_type type;
        const vec_entity_t *sel = G_Sel_Get(&type);
        if(vec_size(sel) > 0 && ((fid = G_Formation_GetForEnt(vec_AT(sel, 0))) != NULL_FID)) {
            khiter_t k = kh_get(formation, s_formations, fid);
            if(k != kh_end(s_formations)) {
                struct formation *formation = &kh_val(s_formations, k);
                render_cell_arrival_field(formation, cell_index);
            }
        }
    }

    status = Settings_Get("pf.debug.show_formations_forces", &setting);
    assert(status == SS_OKAY);
    enabled = setting.as_bool;

    if(enabled) {
        render_formation_forces();
    }
}

static bool event_triggered_recalculate(struct formation *formation, struct block_event *event)
{
    struct entity_block_desc *desc = event->arg;
    if(!G_EntityExists(desc->uid))
        return false;

    khiter_t k;
    if((k = kh_get(entity, formation->ents, desc->uid)) != kh_end(formation->ents))
        return false;

    struct map_resolution nav_res;
    M_NavGetResolution(s_map, &nav_res);

    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;

    const float tile_x_dim = chunk_x_dim / nav_res.tile_w;
    const float tile_z_dim = chunk_z_dim / nav_res.tile_h;

    const float field_x_dim = tile_x_dim * CELL_ARRIVAL_FIELD_RES;
    const float field_z_dim = tile_z_dim * CELL_ARRIVAL_FIELD_RES;

    vec2_t base = formation->center;
    vec2_t delta = (vec2_t){field_x_dim/2.0f, -field_z_dim/2.0f};
    PFM_Vec2_Add(&base, &delta, &base);

    struct box field_bounds = (struct box){
        base.x,
        base.z,
        field_x_dim,
        field_z_dim
    };
    return C_CircleRectIntersection(desc->pos, desc->radius, field_bounds);
}

static void on_entity_unblock(void *user, void *event)
{
    uint32_t uid = (uintptr_t)event;
    uint32_t tick = SDL_GetTicks();
    struct block_event block_event = (struct block_event){
        .type = EVENT_MOVABLE_ENTITY_UNBLOCK,
        .arg = event,
        .tick_recorded = tick
    };
    queue_event_push(&s_events, &block_event);
}

static void on_entity_block(void *user, void *event)
{
    uint32_t tick = SDL_GetTicks();
    struct block_event block_event = (struct block_event){
        .type = EVENT_MOVABLE_ENTITY_BLOCK,
        .arg = event,
        .tick_recorded = tick
    };
    queue_event_push(&s_events, &block_event);
}

static void on_building_found(void *user, void *event)
{
    uint32_t tick = SDL_GetTicks();
    struct block_event block_event = (struct block_event){
        .type = EVENT_MOVABLE_ENTITY_BLOCK,
        .arg = event,
        .tick_recorded = tick
    };
    queue_event_push(&s_events, &block_event);
}

static void on_building_remove(void *user, void *event)
{
    uint32_t tick = SDL_GetTicks();
    struct block_event block_event = (struct block_event){
        .type = EVENT_MOVABLE_ENTITY_UNBLOCK,
        .arg = event,
        .tick_recorded = tick
    };
    queue_event_push(&s_events, &block_event);
}

static void on_1hz_tick(void *user, void *event)
{
    khash_t(entity) *need_recompute = kh_init(entity);
    uint32_t ticks = SDL_GetTicks();
    struct block_event block_event;
    while(queue_event_pop(&s_events, &block_event)) {

        formation_id_t fid;
        struct formation *formation;
        kh_foreach_val_ptr(s_formations, fid, formation, {
            khiter_t k;
            if((k = kh_get(entity, need_recompute, fid)) != kh_end(need_recompute))
                continue;
            if(!SDL_TICKS_PASSED(block_event.tick_recorded, formation->created_tick))
                continue;
            if(!event_triggered_recalculate(formation, &block_event))
                continue;
            int status;
            k = kh_put(entity, need_recompute, fid, &status);
            assert(status != -1);
        });
    }
    formation_id_t fid;
    kh_foreach_key(need_recompute, fid, {
        khiter_t k = kh_get(formation, s_formations, fid);
        assert(k != kh_end(s_formations));
        struct formation *formation = &kh_val(s_formations, k);
        for(int i = 0; i < vec_size(&formation->subformations); i++) {
            struct subformation *curr = &vec_AT(&formation->subformations, i);
            if(curr->state != SUBFORMATION_READY)
                continue;
            recompute_cell_arrival_fields(formation, formation->center, curr);
        }
    });
    kh_destroy(entity, need_recompute);
}

static void destroy_subformation(struct subformation *formation)
{
    complete_cell_field_work(formation, false);
    vec_work_destroy(&formation->futures);
    vec_cell_destroy(&formation->cells);
    kh_destroy(result, formation->results);
    kh_destroy(assignment, formation->assignment);
    kh_destroy(reverse, formation->reverse);
    kh_destroy(entity, formation->ents);
}

static void clear_mappings(struct formation *formation)
{
    uint32_t uid;
    kh_foreach_key(formation->ents, uid, {
        khiter_t k = kh_get(formation, s_formations, uid);
        assert(k != kh_end(s_formations));
        kh_del(formation, s_formations, k);
    });
}

static void destroy_formation(struct formation *formation)
{
    for(int i = 0; i < vec_size(&formation->work); i++) {
        struct cell_assignment_work *work = &vec_AT(&formation->work, i);
        complete_cell_assignment_work(work, false);
        cell_assignment_work_destroy(work);
    }
    for(int i = 0; i < vec_size(&formation->subformations); i++) {
        struct subformation *sub = &vec_AT(&formation->subformations, i);
        destroy_subformation(sub);
    }
    vec_assignment_work_destroy(&formation->work);
    clean_up_map_snapshots(formation);
    assert(kh_size(formation->map_snapshots) == 0);
    kh_destroy(map, formation->map_snapshots);
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
    PERF_ENTER();

    struct cell_field_work *work = arg;
    struct cell_field_work_input *input = &work->input;
    struct cell_arrival_field *result = &work->result;
    struct refcounted_map *map = work->map;
    void *workspace = get_workspace();
    size_t size = workspace_size();

    M_NavCellArrivalFieldCreate(map->snapshot, CELL_ARRIVAL_FIELD_RES, CELL_ARRIVAL_FIELD_RES, 
        input->layer, input->enemy_faction_mask, input->cell_tile, input->center_tile,
        (uint8_t*)result, workspace, size);

    bool ret = SDL_AtomicDecRef(&map->refcount);
    PERF_RETURN(NULL_RESULT);
}

static struct result cell_field_fixup_task(void *arg)
{
    PERF_ENTER();

    struct cell_field_work *work = arg;
    struct cell_field_work_input *input = &work->input;
    struct cell_arrival_field *result = &work->result;
    struct refcounted_map *map = work->map;
    void *workspace = get_workspace();
    size_t size = workspace_size();

    M_NavCellArrivalFieldCreate(map->snapshot, CELL_ARRIVAL_FIELD_RES, CELL_ARRIVAL_FIELD_RES, 
        input->layer, input->enemy_faction_mask, input->cell_tile, input->center_tile,
        (uint8_t*)result, workspace, size);
    M_NavCellArrivalFieldUpdateToNearestPathable(map->snapshot, 
        CELL_ARRIVAL_FIELD_RES, CELL_ARRIVAL_FIELD_RES, input->layer, input->enemy_faction_mask,
        input->curr_tile, input->center_tile, (uint8_t*)result, workspace, size);

    bool ret = SDL_AtomicDecRef(&map->refcount);
    PERF_RETURN(NULL_RESULT);
}

static void dispatch_cell_task(struct formation *parent, vec2_t center, uint32_t uid,
                               struct subformation *formation, struct cell_field_work *work, 
                               struct cell *cell, struct result (*func)(void*))
{
    struct refcounted_map *rmap = map_snapshot_get(parent, formation);
    struct map_resolution res;
    M_NavGetResolution(rmap->snapshot, &res);
    vec3_t map_pos = M_GetPos(rmap->snapshot);

    vec2_t bpos = bin_to_tile_clamped(cell->reachable_pos, center);
    bpos = M_ClampedMapCoordinate(rmap->snapshot, bpos);

    struct tile_desc cell_td;
    bool exists = M_Tile_DescForPoint2D(res, map_pos, bpos, &cell_td);
	(void)exists;
    assert(exists);

    struct tile_desc center_td;
    exists = M_Tile_DescForPoint2D(res, map_pos, bin_to_tile(center, center), &center_td);
    assert(exists);

    work->consumed = false;
    work->recompute_pending = false;
    work->map = rmap;
    work->uid = uid;
    work->last_update_ticks = SDL_GetTicks();

    work->input.layer = formation->layer;
    work->input.enemy_faction_mask = G_GetEnemyFactions(formation->faction_id);
    work->input.cell_tile = cell_td;
    work->input.center_tile = center_td;

    SDL_AtomicSet(&work->future.status, FUTURE_INCOMPLETE);
    work->tid = Sched_Create(31, func, work, &work->future, 0);
    if(work->tid == NULL_TID) {
        func(&work->result);
        SDL_AtomicSet(&work->future.status, FUTURE_COMPLETE);
    }
}

static void dispatch_cell_field_work(struct formation *parent, vec2_t center, 
                                     struct subformation *formation)
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

        khiter_t k = kh_get(assignment, formation->assignment, uid);
        assert(k != kh_end(formation->assignment));
        struct coord coord = kh_val(formation->assignment, k);
        struct cell *cell = &vec_AT(&formation->cells,
            CELL_IDX(coord.r, coord.c, formation->ncols));
        dispatch_cell_task(parent, center, uid, formation, curr, cell, cell_field_task);
        i++;
    });
}

static void complete_cell_field_work(struct subformation *formation, bool yield)
{
    for(int j = 0; j < vec_size(&formation->futures); j++) {
        struct cell_field_work *curr = &vec_AT(&formation->futures, j);
        if(curr->tid == NULL_TID)
            continue;
        while(!Sched_FutureIsReady(&curr->future)) {
            Sched_RunSync(curr->tid);
            if(yield) {
                Sched_TryYield();
            }
        }
    }
}

static void on_update_start(void *user, void *event)
{
    /* Consume cell assignment work results 
    */
    struct formation *formation;
    kh_foreach_ptr(s_formations, formation, {

        for(int i = 0; i < vec_size(&formation->work); i++) {
            struct cell_assignment_work *work = &vec_AT(&formation->work, i);
            if(work->destroyed)
                continue;
            struct subformation *sub = &vec_AT(&formation->subformations, i);
            if(Sched_FutureIsReady(&work->future)) {
                collect_cell_assignment_result(work, sub);
                cell_assignment_work_destroy(work);

                compute_all_blocked(formation);
                dispatch_cell_field_work(formation, formation->center, sub);
            }
        }
    });

    /* Consume cell field work results 
     */
    kh_foreach_ptr(s_formations, formation, {

        clean_up_map_snapshots(formation);

        for(int i = 0; i < vec_size(&formation->subformations); i++) {
            struct subformation *sub = &vec_AT(&formation->subformations, i);
            for(int j = 0; j < vec_size(&sub->futures); j++) {
                struct cell_field_work *curr = &vec_AT(&sub->futures, j);
                uint32_t uid = curr->uid;
                if(curr->recompute_pending) {
                    khiter_t k = kh_get(assignment, sub->assignment, uid);
                    assert(k != kh_end(sub->assignment));
                    struct coord coord = kh_val(sub->assignment, k);
                    struct cell *cell = &vec_AT(&sub->cells,
                        CELL_IDX(coord.r, coord.c, sub->ncols));
                    dispatch_cell_task(formation, formation->center, uid, sub, curr, cell,
                        cell_field_task);
                }
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

static struct cell_arrival_field *cell_get_field(uint32_t uid)
{
    formation_id_t fid = G_Formation_GetForEnt(uid);
    if(fid == NULL_FID)
        return NULL;

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

static struct cell_field_work *cell_get_work(uint32_t uid)
{
    formation_id_t fid = G_Formation_GetForEnt(uid);
    if(fid == NULL_FID)
        return NULL;

    khiter_t k = kh_get(formation, s_formations, fid);
    assert(k != kh_end(s_formations));
    struct formation *formation = &kh_val(s_formations, k);

    khiter_t l = kh_get(assignment, formation->sub_assignment, uid);
    assert(l != kh_end(formation->sub_assignment));
    int idx = kh_val(formation->sub_assignment, l).r;
    struct subformation *sub = &vec_AT(&formation->subformations, idx);

    for(int i = 0; i < vec_size(&sub->futures); i++) {
        struct cell_field_work *curr = &vec_AT(&sub->futures, i);
        if(curr->uid == uid)
            return curr;
    }
    assert(0);
    return NULL;
}

static enum flow_dir cell_get_dir(const struct cell_arrival_field *field, int r, int c)
{
    assert(r >= 0 && r < CELL_ARRIVAL_FIELD_RES);
    assert(c >= 0 && c < CELL_ARRIVAL_FIELD_RES);

    size_t row_size = CELL_ARRIVAL_FIELD_RES / 2;
    size_t aligned_c = (c - (c % 2)) / 2;
    size_t byte_index = r * row_size + aligned_c;
    enum flow_dir dir;
    if(c % 2 == 1) {
        dir = (field->raw[byte_index] & 0x0f) >> 0;
    }else{
        dir = (field->raw[byte_index] & 0xf0) >> 4;
    }
    return dir;
}

static bool inside_arrival_field_bounds(struct formation *formation, vec2_t pos)
{
    struct map_resolution nav_res;
    M_NavGetResolution(s_map, &nav_res);

    const float chunk_x_dim = TILES_PER_CHUNK_WIDTH * X_COORDS_PER_TILE;
    const float chunk_z_dim = TILES_PER_CHUNK_HEIGHT * Z_COORDS_PER_TILE;
    const float buffer = 10.0f;

    const float tile_x_dim = chunk_x_dim / nav_res.tile_w;
    const float tile_z_dim = chunk_z_dim / nav_res.tile_h;

    const float field_x_dim = tile_x_dim * OCCUPIED_FIELD_RES - buffer;
    const float field_z_dim = tile_z_dim * OCCUPIED_FIELD_RES - buffer;

    vec2_t center = formation->center;
    vec2_t corners[] = {
        (vec2_t){center.x + field_x_dim / 2.0f, center.z - field_z_dim / 2.0f},
        (vec2_t){center.x - field_x_dim / 2.0f, center.z - field_z_dim / 2.0f},
        (vec2_t){center.x - field_x_dim / 2.0f, center.z + field_z_dim / 2.0f},
        (vec2_t){center.x + field_x_dim / 2.0f, center.z + field_z_dim / 2.0f}
    };
    return C_PointInsideRect2D(pos, corners[0], corners[1], corners[2], corners[3]);
}

static struct formation *formation_for_ent(uint32_t uid)
{
    formation_id_t fid = G_Formation_GetForEnt(uid);
    if(fid == NULL_FID)
        return NULL;

    khiter_t k = kh_get(formation, s_formations, fid);
    assert(k != kh_end(s_formations));
    return &kh_val(s_formations, k);
}

static struct cell *cell_for_ent(struct formation *formation, uint32_t uid)
{
    khiter_t k = kh_get(assignment, formation->sub_assignment, uid);
    assert(k != kh_end(formation->sub_assignment));
    size_t sub_idx = kh_val(formation->sub_assignment, k).r;
    struct subformation *sub = &vec_AT(&formation->subformations, sub_idx);

    khiter_t l = kh_get(assignment, sub->assignment, uid);
    assert(l != kh_end(sub->assignment));
    struct coord cell_coord = kh_val(sub->assignment, l);
    size_t cell_idx = cell_coord.r * sub->ncols + cell_coord.c;
    return &vec_AT(&sub->cells, cell_idx);
}

static void recompute_cell_arrival_fields(struct formation *parent, vec2_t center, 
                                          struct subformation *formation)
{
    int i = 0;
    uint32_t uid;
    kh_foreach_key(formation->ents, uid, {
        struct cell_field_work *curr = &vec_AT(&formation->futures, i);
        if(!curr->consumed && !Sched_FutureIsReady(&curr->future)) {
            curr->recompute_pending = true;
            continue;
        }

        khiter_t k = kh_get(assignment, formation->assignment, uid);
        assert(k != kh_end(formation->assignment));
        struct coord coord = kh_val(formation->assignment, k);
        struct cell *cell = &vec_AT(&formation->cells,
            CELL_IDX(coord.r, coord.c, formation->ncols));
        dispatch_cell_task(parent, center, uid, formation, curr, cell, cell_field_task);

        i++;
    });
}

static quat_t quat_from_vec(vec2_t dir)
{
    assert(PFM_Vec2_Len(&dir) > EPSILON);

    float angle_rad = atan2(dir.z, dir.x) - M_PI/2.0f;
    return (quat_t) {
        0.0f, 
        1.0f * sin(angle_rad / 2.0f),
        0.0f,
        cos(angle_rad / 2.0f)
    };
}

static void filter_selection_movable(const vec_entity_t *in_sel, vec_entity_t *out_sel)
{
    vec_entity_init(out_sel);
    for(int i = 0; i < vec_size(in_sel); i++) {

        uint32_t uid = vec_AT(in_sel, i);
        uint32_t flags = G_FlagsGet(uid);
        if(!(flags & ENTITY_FLAG_MOVABLE))
            continue;
        vec_entity_push(out_sel, uid);
    }
}

static bool all_same_preferred(const vec_entity_t *ents)
{
    assert(vec_size(ents) > 0);
    uint32_t first_uid = vec_AT(ents, 0);
    enum formation_type first = G_Formation_GetPreferred(first_uid);
    for(int i = 1; i < vec_size(ents); i++) {
        uint32_t uid = vec_AT(ents, i);
        uint32_t flags = G_FlagsGet(uid);
        enum formation_type curr = G_Formation_GetPreferred(uid);
        if(curr != first)
            return false;
    }
    return true;
}

static void clear_for_ent(uint32_t uid)
{
    khiter_t k = kh_get(mapping, s_ent_formation_map, uid);
    assert(k != kh_end(s_ent_formation_map));
    kh_del(mapping, s_ent_formation_map, k);
}

/* Returns true if the current cell arrival field for the entity
 * will guide it towards a blocked tile.
 */
static bool will_collide(struct cell_arrival_field *field, enum nav_layer layer, 
                         struct coord coord, vec2_t pos)
{
    struct map_resolution res;
    M_NavGetResolution(s_map, &res);
    vec3_t map_pos = M_GetPos(s_map);

    enum flow_dir dir = cell_get_dir(field, coord.r, coord.c);
    vec2_t vec_dir = N_FlowDir(dir);

    float magnitude = 4.0f;
    if(vec_dir.x != 0.0f && vec_dir.z != 0.0f)
        magnitude *= sqrt(2.0f);
    PFM_Vec2_Scale(&vec_dir, magnitude, &vec_dir);

    vec2_t next_pos;
    PFM_Vec2_Add(&pos, &vec_dir, &next_pos);

    if(!M_PointInsideMap(s_map, next_pos))
        return true;

    return M_NavPositionBlocked(s_map, layer, next_pos);
}

static uint32_t subformation_leader(struct subformation *formation)
{
    /* Find the closest entity to the front row center. This is the tentative
     * formation 'leader'.
     */
    size_t nrows = formation->nrows;
    size_t ncols = formation->ncols;

    /* Iterate rows from front to back */
    for(int row = nrows-1; row >= 0; row--) {
        /* Iterate the columns from the center outwards */
        int left = 0, right = 1;
        int remaining = ncols;
        int center = ncols / 2; /* start at the center */

        while(remaining > 0) {
            int col;
            if(left <= right && (center - left) >= 0) {
                col = center - left;
                left++;
            }else{
                col = center + right;
                right++;
            }
            remaining--;

            /* Try to get the entity for the current cell */
            int cell_idx = CELL_IDX(row, col, formation->ncols);
            khiter_t k = kh_get(reverse, formation->reverse, cell_idx);
            if(k != kh_end(formation->reverse)) {
                return kh_val(formation->reverse, k);
            }
        }
    }
    return NULL_UID;
}

static void subformation_anchor_and_heading(uint32_t leader,
                                            vec2_t *out_anchor, vec2_t *out_heading)
{
    vec4_t front = (vec4_t){0.0f, 0.0f, 1.0f, 1.0f};
    quat_t rot = Entity_GetRot(leader);

    mat4x4_t rot_mat;
    PFM_Mat4x4_RotFromQuat(&rot, &rot_mat);

    vec4_t dir;
    PFM_Mat4x4_Mult4x1(&rot_mat, &front, &dir);

    *out_anchor = G_Pos_GetXZ(leader);
    *out_heading = (vec2_t){dir.x, dir.z};
}

static uint32_t unit_in_front(uint32_t uid, struct subformation *formation)
{
    khiter_t k = kh_get(assignment, formation->assignment, uid);
    assert(k != kh_end(formation->assignment));
    struct coord cell_coord = kh_val(formation->assignment, k);

    int c = cell_coord.c;
    for(int r = cell_coord.r + 1; r < formation->nrows; r++) {
        int idx = CELL_IDX(r, c, formation->ncols);
        struct cell *cell = &vec_AT(&formation->cells, idx);
        if(cell->state != CELL_OCCUPIED)
            continue;
        khiter_t l = kh_get(reverse, formation->reverse, idx);
        if(l != kh_end(formation->reverse))
            return kh_val(formation->reverse, l);
    }
    return NULL_UID;
}

static int front_row_idx(struct subformation *formation)
{
    size_t front = formation->nrows - 1;
    for(int r = front; r >= 0; r--) {
    for(int c = 0; c < formation->ncols; c++) {
        size_t cell_idx = CELL_IDX(r, c, formation->ncols);
        khiter_t k = kh_get(reverse, formation->reverse, cell_idx);
        if(k != kh_end(formation->reverse))
            return r;
    }}
    return 0;
}

static bool in_front_row(uint32_t uid, uint32_t leader, struct subformation *formation, 
                         int *out_col_offset)
{
    khiter_t k = kh_get(assignment, formation->assignment, uid);
    assert(k != kh_end(formation->assignment));
    struct coord cell_coord = kh_val(formation->assignment, k);

    int front_idx = front_row_idx(formation);
    if(cell_coord.r != front_idx)
        return false;

    k = kh_get(assignment, formation->assignment, leader);
    assert(k != kh_end(formation->assignment));
    struct coord leader_coord = kh_val(formation->assignment, k);
    *out_col_offset = cell_coord.c - leader_coord.c;
    return true;
}

static bool ahead_of_target(uint32_t uid, vec2_t target, vec2_t anchor, vec2_t heading,
                            float *out_dist)
{
    /* project pos on heading starting from anchor */
    vec2_t pos = G_Pos_GetXZ(uid);
    vec2_t anchor_to_pos;
    PFM_Vec2_Sub(&pos, &anchor, &anchor_to_pos);
    float ent_proj = PFM_Vec2_Dot(&anchor_to_pos, &heading);

    /* project target on heading starting from anchor */
    vec2_t anchor_to_target;
    PFM_Vec2_Sub(&target, &anchor, &anchor_to_target);
    float target_proj = PFM_Vec2_Dot(&anchor_to_target, &heading);

    const float TOLERANCE = 2.5f;
    *out_dist = ent_proj - target_proj;
    return ((ent_proj - TOLERANCE) > target_proj);
}

static vec2_t entity_target_position(vec2_t anchor, vec2_t heading, float distance)
{
    vec2_t perpendicular = (vec2_t){-heading.z, heading.x};
    PFM_Vec2_Normal(&perpendicular, &perpendicular);
    PFM_Vec2_Scale(&perpendicular, distance, &perpendicular);

    vec2_t ret;
    PFM_Vec2_Add(&anchor, &perpendicular, &ret);
    return ret;
}

static bool leader_should_slow_dowm(uint32_t leader, struct subformation *formation)
{
    vec2_t anchor, heading;
    subformation_anchor_and_heading(leader, &anchor, &heading);
    float radius = G_GetSelectionRadius(leader);

    khiter_t k = kh_get(assignment, formation->assignment, leader);
    assert(k != kh_end(formation->assignment));
    struct coord leader_coord = kh_val(formation->assignment, k);

    int front_row = front_row_idx(formation);
    for(int c = 0; c < formation->ncols; c++) {
        int idx = CELL_IDX(front_row, c, formation->ncols);
        khiter_t k = kh_get(reverse, formation->reverse, idx);
        if(k == kh_end(formation->reverse))
            continue;

        uint32_t uid = kh_val(formation->reverse, k);
        int col_offset = c - leader_coord.c;
        float distance = -col_offset * (2 * radius + MOVE_BUFFER_DIST);
        vec2_t target = entity_target_position(anchor, heading, distance);

        float amount;
        if(!ahead_of_target(uid, target, anchor, heading, &amount) 
        && (fabsf(amount) > 5.0f)) {
            return true;
        }
    }
    return false;
}

static vec2_t follow_force(uint32_t uid, struct subformation *formation)
{
    uint32_t unit = unit_in_front(uid, formation);
    if(unit == NULL_UID)
        return (vec2_t){0.0f, 0.0f};

    vec2_t ent_pos = G_Pos_GetXZ(uid);
    vec2_t front_pos = G_Pos_GetXZ(unit);

    vec2_t delta;
    PFM_Vec2_Sub(&front_pos, &ent_pos, &delta);
    if(PFM_Vec2_Len(&delta) > EPSILON) {
        PFM_Vec2_Normal(&delta, &delta);
    }
    return delta;
}

static size_t subformation_index(struct formation *formation, struct subformation *sub)
{
    for(int i = 0; i < vec_size(&formation->subformations); i++) {
        struct subformation *curr = &vec_AT(&formation->subformations, i);
        if(curr == sub)
            return i;
    }
    return 0;
}

static bool subformation_save_state(struct formation *parent, struct subformation *sub, 
                                    struct SDL_RWops *stream)
{
    size_t parent_idx = subformation_index(parent, sub->parent);
    struct attr parent_attr = (struct attr){
        .type = TYPE_INT,
        .val.as_int = parent_idx
    };
    CHK_TRUE_RET(Attr_Write(stream, &parent_attr, "parent_idx"));

    struct attr nchildren = (struct attr){
        .type = TYPE_INT,
        .val.as_int = sub->nchildren
    };
    CHK_TRUE_RET(Attr_Write(stream, &nchildren, "nchildren"));

    for(int i = 0; i < sub->nchildren; i++) {

        struct attr child_attr = (struct attr){
            .type = TYPE_INT,
            .val.as_int = subformation_index(parent, sub->children[i])
        };
        CHK_TRUE_RET(Attr_Write(stream, &child_attr, "child_idx"));
    }

    struct attr unit_radius = (struct attr){
        .type = TYPE_FLOAT,
        .val.as_float = sub->unit_radius
    };
    CHK_TRUE_RET(Attr_Write(stream, &unit_radius, "unit_radius"));

    struct attr layer = (struct attr){
        .type = TYPE_INT,
        .val.as_int = sub->layer
    };
    CHK_TRUE_RET(Attr_Write(stream, &layer, "layer"));

    struct attr faction_id = (struct attr){
        .type = TYPE_INT,
        .val.as_int = sub->faction_id
    };
    CHK_TRUE_RET(Attr_Write(stream, &faction_id, "faction_id"));

    struct attr reachable_target = (struct attr){
        .type = TYPE_VEC2,
        .val.as_vec2 = sub->reachable_target
    };
    CHK_TRUE_RET(Attr_Write(stream, &reachable_target, "reachable_target"));

    struct attr pos = (struct attr){
        .type = TYPE_VEC2,
        .val.as_vec2 = sub->pos
    };
    CHK_TRUE_RET(Attr_Write(stream, &pos, "pos"));

    struct attr orientation = (struct attr){
        .type = TYPE_VEC2,
        .val.as_vec2 = sub->orientation
    };
    CHK_TRUE_RET(Attr_Write(stream, &orientation, "orientation"));

    struct attr nrows = (struct attr){
        .type = TYPE_INT,
        .val.as_int = sub->nrows
    };
    CHK_TRUE_RET(Attr_Write(stream, &nrows, "nrows"));

    struct attr ncols = (struct attr){
        .type = TYPE_INT,
        .val.as_int = sub->ncols
    };
    CHK_TRUE_RET(Attr_Write(stream, &ncols, "ncols"));

    struct attr blocked_low = (struct attr){
        .type = TYPE_INT,
        .val.as_int = (uint32_t)sub->blocked
    };
    CHK_TRUE_RET(Attr_Write(stream, &blocked_low, "blocked_low"));

    struct attr blocked_high = (struct attr){
        .type = TYPE_INT,
        .val.as_int = (uint32_t)(sub->blocked >> 32)
    };
    CHK_TRUE_RET(Attr_Write(stream, &blocked_high, "blocked_high"));

    struct attr nents = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(sub->ents)
    };
    CHK_TRUE_RET(Attr_Write(stream, &nents, "num_entities"));

    uint32_t uid;
    kh_foreach_key(sub->ents, uid, {

        struct attr uid_attr = (struct attr){
            .type = TYPE_INT,
            .val.as_int = uid
        };
        CHK_TRUE_RET(Attr_Write(stream, &uid_attr, "uid"));
    });

    struct attr ncells = (struct attr){
        .type = TYPE_INT,
        .val.as_int = vec_size(&sub->cells)
    };
    CHK_TRUE_RET(Attr_Write(stream, &ncells, "ncells"));

    for(int i = 0; i < vec_size(&sub->cells); i++) {
        struct cell *cell = &vec_AT(&sub->cells, i);

        struct attr cell_state = (struct attr){
            .type = TYPE_INT,
            .val.as_int = cell->state
        };
        CHK_TRUE_RET(Attr_Write(stream, &cell_state, "cell_state"));
        
        struct attr cell_ideal_raw = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = cell->ideal_raw
        };
        CHK_TRUE_RET(Attr_Write(stream, &cell_ideal_raw, "cell_ideal_raw"));

        struct attr cell_ideal_binned = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = cell->ideal_binned
        };
        CHK_TRUE_RET(Attr_Write(stream, &cell_ideal_binned, "cell_ideal_binned"));

        struct attr cell_pos = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = cell->pos
        };
        CHK_TRUE_RET(Attr_Write(stream, &cell_pos, "cell_pos"));

        struct attr cell_reachable_pos = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = cell->reachable_pos
        };
        CHK_TRUE_RET(Attr_Write(stream, &cell_reachable_pos, "cell_reachable_pos"));
    }

    struct attr nassigned = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(sub->assignment)
    };
    CHK_TRUE_RET(Attr_Write(stream, &nassigned, "nassigned"));

    struct coord coord;
    kh_foreach(sub->assignment, uid, coord, {

        struct attr uid_attr = (struct attr){
            .type = TYPE_INT,
            .val.as_int = uid
        };
        CHK_TRUE_RET(Attr_Write(stream, &uid_attr, "uid"));

        struct attr coord_r = (struct attr){
            .type = TYPE_INT,
            .val.as_int = coord.r
        };
        CHK_TRUE_RET(Attr_Write(stream, &coord_r, "coord_r"));

        struct attr coord_c = (struct attr){
            .type = TYPE_INT,
            .val.as_int = coord.c
        };
        CHK_TRUE_RET(Attr_Write(stream, &coord_c, "coord_c"));
    });

    struct attr nreverse = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(sub->reverse)
    };
    CHK_TRUE_RET(Attr_Write(stream, &nreverse, "nreverse"));

    int idx;
    kh_foreach(sub->reverse, idx, uid, {

        struct attr idx_attr = (struct attr){
            .type = TYPE_INT,
            .val.as_int = idx
        };
        CHK_TRUE_RET(Attr_Write(stream, &idx_attr, "idx"));

        struct attr uid_attr = (struct attr){
            .type = TYPE_INT,
            .val.as_int = uid
        };
        CHK_TRUE_RET(Attr_Write(stream, &uid_attr, "uid"));
    });

    /* results and futures are re-created at loading time */
    return true;
}

static bool subformation_load_state(struct formation *parent, struct subformation *sub, 
                                    struct SDL_RWops *stream)
{
    sub->results = kh_init(result);
    sub->assignment = kh_init(assignment);
    sub->reverse = kh_init(reverse);
    sub->ents = kh_init(entity);
    vec_cell_init(&sub->cells);
    vec_work_init(&sub->futures);

    struct attr attr;
    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    size_t parent_idx = attr.val.as_int;
    sub->parent = &vec_AT(&parent->subformations, parent_idx);

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    size_t nchildren = attr.val.as_int;
    sub->nchildren = nchildren;

    for(int i = 0; i < nchildren; i++) {

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        size_t child_idx = attr.val.as_int;
        sub->children[i] = &vec_AT(&parent->subformations, child_idx);
    }

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_FLOAT);
    sub->unit_radius = attr.val.as_float;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    sub->layer = attr.val.as_int;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    sub->faction_id = attr.val.as_int;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC2);
    sub->reachable_target = attr.val.as_vec2;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC2);
    sub->pos = attr.val.as_vec2;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_VEC2);
    sub->orientation = attr.val.as_vec2;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    sub->nrows = attr.val.as_int;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    sub->ncols = attr.val.as_int;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    uint32_t blocked_low = attr.val.as_int;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    uint32_t blocked_high = attr.val.as_int;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    size_t nents = attr.val.as_int;

    for(int i = 0; i < nents; i++) {

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        uint32_t uid = attr.val.as_int;

        int ret;
        khiter_t k = kh_put(entity, sub->ents, uid, &ret);
        CHK_TRUE_RET(ret != -1);
    }

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    size_t ncells = attr.val.as_int;

    vec_cell_resize(&sub->cells, ncells);
    sub->cells.size = ncells;

    for(int i = 0; i < ncells; i++) {
        struct cell *cell = &vec_AT(&sub->cells, i);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        cell->state = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC2);
        cell->ideal_raw = attr.val.as_vec2;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC2);
        cell->ideal_binned = attr.val.as_vec2;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC2);
        cell->pos = attr.val.as_vec2;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC2);
        cell->reachable_pos = attr.val.as_vec2;
    }
    Sched_TryYield();

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    size_t nassigned = attr.val.as_int;

    for(int i = 0; i < nassigned; i++) {

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        uint32_t uid = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        int r = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        int c = attr.val.as_int;

        int ret;
        khiter_t k = kh_put(assignment, sub->assignment, uid, &ret);
        CHK_TRUE_RET(ret != -1);
        kh_val(sub->assignment, k) = (struct coord){r, c};
    }
    Sched_TryYield();

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    size_t nreverse = attr.val.as_int;

    for(int i = 0; i < nreverse; i++) {

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        size_t idx = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        uint32_t uid = attr.val.as_int;

        int ret;
        khiter_t k = kh_put(reverse, sub->reverse, idx, &ret);
        CHK_TRUE_RET(ret != -1);
        kh_val(sub->reverse, k) = uid;
    }
    return true;
}

static khash_t(pos) *copy_entity_positions(const struct subformation *sub)
{
    khash_t(pos) *ret = kh_init(pos);
    if(!ret)
        return NULL;

    uint32_t uid;
    kh_foreach_key(sub->ents, uid, {
        int status;
        khiter_t k = kh_put(pos, ret, uid, &status);
        assert(status!= -1);
        kh_val(ret, k) = G_Pos_Get(uid);
    });
    return ret;
}

static void cell_assignment_work_init(struct cell_assignment_work *work, 
                                      const struct subformation *sub,
                                      formation_id_t fid, int idx)
{
    work->destroyed = false;
    work->ents = kh_copy(entity, sub->ents);
    work->positions = copy_entity_positions(sub);
    work->assignment = kh_init(assignment);
    work->reverse = kh_init(reverse);
    vec_cell_init(&work->cells);
    vec_cell_copy(&work->cells, &(((struct subformation*)sub)->cells));
    work->ncols = sub->ncols;
    work->nrows = sub->nrows;
    work->fid = fid;
    work->subformation_idx = idx;
}

static void cell_assignment_work_destroy(struct cell_assignment_work *work)
{
    if(work->destroyed)
        return;
    kh_destroy(entity, work->ents);
    kh_destroy(pos, work->positions);
    kh_destroy(assignment, work->assignment);
    kh_destroy(reverse, work->reverse);
    vec_cell_destroy(&work->cells);
    work->destroyed = true;
}

static void collect_cell_assignment_result(const struct cell_assignment_work *work, 
                                           struct subformation *out)
{
    assert(kh_size(work->ents) == kh_size(work->reverse));
    assert(kh_size(work->ents) == kh_size(work->assignment));
    assert(kh_size(out->ents) <= kh_size(work->ents));

    /* Account for any entities that have been removed from the 
     * subformation since the cell assignement work has been kicked off
     */
    if(kh_size(out->ents) < kh_size(work->ents)) {

        vec_entity_t removed;
        vec_entity_init(&removed);

        uint32_t uid;
        kh_foreach_key(work->ents, uid, {
            if(kh_get(entity, out->ents, uid) == kh_end(out->ents)) {
                vec_entity_push(&removed, uid);
            }
        });
        assert(kh_size(out->ents) + vec_size(&removed) == kh_size(work->ents));

        for(int i = 0; i < vec_size(&removed); i++) {
            uint32_t uid = vec_AT(&removed, i);

            /* First get the cell index from the assignment */
            khiter_t l = kh_get(assignment, work->assignment, uid);
            assert(l != kh_end(work->assignment));
            struct coord coord = kh_val(work->assignment, l);
            int idx = CELL_IDX(coord.r, coord.c, work->ncols);

            /* Clear assignment */
            khiter_t k = kh_get(assignment, work->assignment, uid);
            assert(k != kh_end(work->assignment));
            kh_del(assignment, work->assignment, k);

            /* Remove reverse assignment */
            khiter_t m = kh_get(reverse, work->reverse, idx);
            assert(m != kh_end(work->reverse));
            kh_del(reverse, work->reverse, m);
        }
        vec_entity_destroy(&removed);
    }

    kh_destroy(reverse, out->reverse);
    out->reverse = kh_copy(reverse, work->reverse);

    kh_destroy(assignment, out->assignment);
    out->assignment = kh_copy(assignment, work->assignment);

    vec_cell_reset(&out->cells);
    vec_cell_copy(&out->cells, &(((struct cell_assignment_work*)work)->cells));
    out->state = SUBFORMATION_READY;
}

static struct result cell_assignment_task(void *arg)
{
    PERF_ENTER();

    struct cell_assignment_work *work = arg;
    compute_cell_assignment(work);

    PERF_RETURN(NULL_RESULT);
}

static void complete_cell_assignment_work(struct cell_assignment_work *work, bool yield)
{
    while(!Sched_FutureIsReady(&work->future)) {
        Sched_RunSync(work->tid);
        if(yield) {
            Sched_TryYield();
        }
    }
}

static void dispatch_cell_assignment_task(struct cell_assignment_work *work)
{
    ASSERT_IN_MAIN_THREAD();

    SDL_AtomicSet(&work->future.status, FUTURE_INCOMPLETE);
    work->tid = Sched_Create(16, cell_assignment_task, work, &work->future, TASK_BIG_STACK);
    if(work->tid == NULL_TID) {
        cell_assignment_task(work);
        SDL_AtomicSet(&work->future.status, FUTURE_COMPLETE);
    }
}

static void dispatch_cell_assignment_work(struct formation *parent)
{
    ASSERT_IN_MAIN_THREAD();
    for(int i = 0; i < vec_size(&parent->work); i++) {
        struct cell_assignment_work *work = &vec_AT(&parent->work, i);
        dispatch_cell_assignment_task(work);
    }
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
    if(NULL == (s_preferred = kh_init(type)))
        goto fail_preferred;

    if(s_workspace == 0) {
        s_workspace = SDL_TLSCreate();
    }
    if(s_workspace == 0)
        goto fail_tls;

    if(!queue_event_init(&s_events, 512))
        goto fail_tls;

    s_map = map;
    s_next_id = 0;

    E_Global_Register(EVENT_RENDER_3D_POST, on_render_3d, NULL, 
        G_RUNNING | G_PAUSED_FULL | G_PAUSED_UI_RUNNING);
    E_Global_Register(EVENT_UPDATE_START, on_update_start, NULL, G_RUNNING);
    E_Global_Register(EVENT_MOVABLE_ENTITY_BLOCK, on_entity_block, NULL, G_RUNNING);
    E_Global_Register(EVENT_MOVABLE_ENTITY_UNBLOCK, on_entity_unblock, NULL, G_RUNNING);
    E_Global_Register(EVENT_BUILDING_FOUNDED, on_building_found, NULL, G_RUNNING);
    E_Global_Register(EVENT_BUILDING_REMOVED, on_building_remove, NULL, G_RUNNING);
    E_Global_Register(EVENT_1HZ_TICK, on_1hz_tick, NULL, G_RUNNING);
    return true;

fail_tls:
    kh_destroy(type, s_preferred);
fail_preferred:
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
        clear_mappings(formation);
        destroy_formation(formation);
    });

    E_Global_Unregister(EVENT_1HZ_TICK, on_1hz_tick);
    E_Global_Unregister(EVENT_BUILDING_FOUNDED, on_building_found);
    E_Global_Unregister(EVENT_BUILDING_REMOVED, on_building_remove);
    E_Global_Unregister(EVENT_MOVABLE_ENTITY_UNBLOCK, on_entity_unblock);
    E_Global_Unregister(EVENT_MOVABLE_ENTITY_BLOCK, on_entity_block);
    E_Global_Unregister(EVENT_UPDATE_START, on_update_start);
    E_Global_Unregister(EVENT_RENDER_3D_POST, on_render_3d);

    queue_event_destroy(&s_events);
    kh_destroy(type, s_preferred);
    kh_destroy(formation, s_formations);
    kh_destroy(mapping, s_ent_formation_map);
}

vec2_t G_Formation_AutoOrientation(vec2_t target, const vec_entity_t *ents)
{
    ASSERT_IN_MAIN_THREAD();

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

    if(PFM_Vec2_Len(&orientation) < EPSILON) {

        vec4_t front = (vec4_t){0.0f, 0.0f, 1.0f, 1.0f};
        quat_t rot = Entity_GetRot(vec_AT(ents, 0)); 

        mat4x4_t rot_mat;
        PFM_Mat4x4_RotFromQuat(&rot, &rot_mat);

        vec4_t dir;
        PFM_Mat4x4_Mult4x1(&rot_mat, &front, &dir);
        return (vec2_t){dir.x, dir.z};
    }
    PFM_Vec2_Normal(&orientation, &orientation);
    return orientation;
}

void G_Formation_Create(vec2_t target, vec2_t orientation, 
                        const vec_entity_t *ents, enum formation_type type)
{
    ASSERT_IN_MAIN_THREAD();
    if(type == FORMATION_NONE)
        return;

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

    *new = (struct formation){
        .refcount = vec_size(ents),
        .type = type,
        .target = target,
        .orientation = orientation,
        .center = field_center(target, orientation),
        .ents = copy_vector(ents),
        .speed = formation_speed(ents),
        .created_tick = SDL_GetTicks(),
        .sub_assignment = kh_init(assignment),
        .map_snapshots = kh_init(map)
    };
    init_subformations(new);

    enum nav_layer layers[NAV_LAYER_MAX];
    size_t nlayers = formation_layers(&new->subformations, layers);
    for(int i = 0; i < nlayers; i++) {
        init_occupied_field(s_map, layers[i], new->center, new->occupied[layers[i]]);
        init_islands_field(s_map, layers[i], new->center, new->islands[layers[i]]);
    }

    vec_assignment_work_init(&new->work);
    vec_assignment_work_resize(&new->work, vec_size(&new->subformations));
    new->work.size = vec_size(&new->subformations);

    for(int i = 0; i < vec_size(&new->subformations); i++) {
        struct subformation *sub = &vec_AT(&new->subformations, i);
        sub->state = SUBFORMATION_COMPUTING_ASSIGNMENT;
        place_subformation(sub, new->center, target, new->orientation, 
            new->occupied, new->islands);
        mark_unused_cells(sub);

        struct cell_assignment_work *work = &vec_AT(&new->work, i);
        cell_assignment_work_init(work, sub, fid, i);
    }
    dispatch_cell_assignment_work(new);
}

formation_id_t G_Formation_GetForEnt(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(mapping, s_ent_formation_map, uid);
    if(k == kh_end(s_ent_formation_map))
        return NULL_FID;
    return kh_val(s_ent_formation_map, k);
}

void G_Formation_RemoveUnit(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    formation_id_t fid = G_Formation_GetForEnt(uid);
    if(fid == NULL_FID)
        return;

    khiter_t k = kh_get(formation, s_formations, fid);
    assert(k != kh_end(s_formations));
    struct formation *formation = &kh_val(s_formations, k);
    struct subformation *sub = subformation_for_ent(formation, uid);

    /* Remove from the formation and subformation entity set */
    khiter_t e = kh_get(entity, formation->ents, uid);
    assert(e != kh_end(formation->ents));
    kh_del(entity, formation->ents, e);

    e = kh_get(entity, sub->ents, uid);
    assert(e != kh_end(sub->ents));
    kh_del(entity, sub->ents, e);

    if(sub->state == SUBFORMATION_READY) {
        /* Clear assignment */
        khiter_t l = kh_get(assignment, sub->assignment, uid);
        assert(l != kh_end(sub->assignment));
        struct coord coord = kh_val(sub->assignment, l);
        int idx = CELL_IDX(coord.r, coord.c, sub->ncols);
        kh_del(assignment, sub->assignment, l);

        /* Clear reverse assignment */
        khiter_t m = kh_get(reverse, sub->reverse, idx);
        assert(m != kh_end(sub->reverse));
        kh_del(reverse, sub->reverse, m);
    }

    clear_for_ent(uid);
    if(--formation->refcount == 0) {
        destroy_formation(formation);
        kh_del(formation, s_formations, k);
    }
}

void G_Formation_RemoveEntity(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    G_Formation_RemoveUnit(uid);

    khiter_t m = kh_get(type, s_preferred, uid);
    if(m != kh_end(s_preferred)) {
        kh_del(type, s_preferred, m);
    }
}

bool G_Formation_CanUseArrivalField(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    struct cell_arrival_field *field = cell_get_field(uid);
    if(!field)
        return false;

    struct formation *formation = formation_for_ent(uid);
    vec2_t pos = G_Pos_GetXZ(uid);
    return inside_arrival_field_bounds(formation, pos);
}

bool G_Formation_InRangeOfCell(uint32_t uid)
{
    struct formation *formation = formation_for_ent(uid);
    vec2_t pos = G_Pos_GetXZ(uid);
    return inside_arrival_field_bounds(formation, pos);
}

vec2_t G_Formation_DesiredArrivalVelocity(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    
    struct formation *formation = formation_for_ent(uid);
    assert(formation);

    vec2_t pos = G_Pos_GetXZ(uid);
    struct coord coord = pos_to_tile(pos, formation->center);
    /* Account for the difference between OCCUPIED_FIELD_RES
     * and CELL_ARRIVAL_FIELD_RES */
    coord.r += 1;
    coord.c += 1;

    struct cell_arrival_field *field = cell_get_field(uid);
    enum flow_dir dir = cell_get_dir(field, coord.r, coord.c);
    return N_FlowDir(dir);
}

vec2_t G_Formation_ApproximateDesiredArrivalVelocity(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    struct formation *formation = formation_for_ent(uid);
    assert(formation);
    struct cell *cell = cell_for_ent(formation, uid);
    assert(cell);

    vec2_t cell_pos = cell->reachable_pos;
    vec2_t ent_pos = G_Pos_GetXZ(uid);
    vec2_t delta;
    PFM_Vec2_Sub(&cell_pos, &ent_pos, &delta);
    PFM_Vec2_Normal(&delta, &delta);
    return delta;
}

bool G_Formation_ArrivedAtCell(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    struct formation *formation = formation_for_ent(uid);
    assert(formation);
    struct cell *cell = cell_for_ent(formation, uid);
    assert(cell);

    /* Check if we are within tolerance of the cell position */
    float radius = G_GetSelectionRadius(uid);
    float arrive_thresh = MIN(radius * 1.5f, 10.0f);

    vec2_t cell_pos = cell->reachable_pos;
    vec2_t ent_pos = G_Pos_GetXZ(uid);
    vec2_t delta;
    PFM_Vec2_Sub(&ent_pos, &cell_pos, &delta);
    return (PFM_Vec2_Len(&delta) <= arrive_thresh);
}

bool G_Formation_AssignmentReady(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    struct formation *formation = formation_for_ent(uid);
    assert(formation);
    /* We consider the assignment ready when all units in front of the 
     * subformation have an assignment and thus can start moving.
     */
    struct subformation *sub = subformation_for_ent(formation, uid);
    for(int i = 0; i < vec_size(&formation->subformations); i++) {
        struct subformation *curr = &vec_AT(&formation->subformations, i);
        if(curr == sub)
            return (curr->state == SUBFORMATION_READY);
        if(curr->state != SUBFORMATION_READY)
            return false;
    }
    assert(0);
    return false;
}

bool G_Formation_AssignedToCell(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    struct formation *formation = formation_for_ent(uid);
    assert(formation);
    struct cell *cell = cell_for_ent(formation, uid);
    assert(cell);
    return (cell->state == CELL_OCCUPIED);
}

vec2_t G_Formation_CellPosition(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    struct formation *formation = formation_for_ent(uid);
    assert(formation);
    struct cell *cell = cell_for_ent(formation, uid);
    assert(cell);
    return cell->reachable_pos;
}

void G_Formation_Arrange(enum formation_type type, vec_entity_t *ents)
{
    ASSERT_IN_MAIN_THREAD();

    vec_entity_t filtered;
    filter_selection_movable(ents, &filtered);

    for(int i = 0; i < vec_size(&filtered); i++) {
        uint32_t curr = vec_AT(&filtered, i);
        G_Formation_SetPreferred(curr, type);
    }

    if(type == FORMATION_NONE) {
        vec_entity_destroy(&filtered);
        return;
    }

    vec2_t target = target_position(&filtered);
    vec2_t orientation = G_Formation_AutoOrientation(target, &filtered);
    G_Move_ArrangeInFormation(&filtered, target, orientation, type);
    vec_entity_destroy(&filtered);
}

quat_t G_Formation_TargetOrientation(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    struct formation *formation = formation_for_ent(uid);
    assert(formation);
    return quat_from_vec(formation->orientation);
}

void G_Formation_SetPreferred(uint32_t uid, enum formation_type type)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(type, s_preferred, uid);
    if(k == kh_end(s_preferred)) {
        int ret;
        k = kh_put(type, s_preferred, uid, &ret);
        assert(ret != -1);
    }
    kh_val(s_preferred, k) = type;

    if(type == FORMATION_NONE) {
        G_Formation_RemoveUnit(uid);
    }
}

enum formation_type G_Formation_GetPreferred(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(type, s_preferred, uid);
    assert(k != kh_end(s_preferred));
    return kh_val(s_preferred, k);
}

enum formation_type G_Formation_PreferredForSet(const vec_entity_t *ents)
{
    ASSERT_IN_MAIN_THREAD();

    vec_entity_t filtered;
    filter_selection_movable(ents, &filtered);
    if(vec_size(&filtered) > 0 && all_same_preferred(ents)) {
        uint32_t first = G_Formation_GetPreferred(vec_AT(ents, 0));
        vec_entity_destroy(&filtered);
        return first;
    }
    vec_entity_destroy(&filtered);
    return FORMATION_NONE;
}

void G_Formation_UpdateFieldIfNeeded(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    PERF_ENTER();

    struct cell_arrival_field *field = cell_get_field(uid);
    if(!field)
        PERF_RETURN_VOID();

    struct formation *formation = formation_for_ent(uid);
    struct subformation *sub = subformation_for_ent(formation, uid);

    struct refcounted_map *rmap = map_snapshot_get(formation, sub);
    struct map_resolution res;
    M_NavGetResolution(rmap->snapshot, &res);
    vec3_t map_pos = M_GetPos(rmap->snapshot);

    vec2_t pos = G_Pos_GetXZ(uid);
    struct coord coord = pos_to_tile(pos, formation->center);
    /* Account for the difference between OCCUPIED_FIELD_RES
     * and CELL_ARRIVAL_FIELD_RES */
    coord.r += 1;
    coord.c += 1;

    enum nav_layer layer = Entity_NavLayer(uid);
    struct cell_field_work *work = cell_get_work(uid);
    struct cell_field_work_input *input = &work->input;
    void *workspace = get_workspace();
    size_t ws_size = workspace_size();

    /* Out of performance considerations, don't re-compute the 
     * field too often. Wait for a number of changes to pile up
     * and do it perfodically, as necessary.
     */
    uint32_t curr = SDL_GetTicks();
    float elapsed = (curr - work->last_update_ticks) / 1000.0f;
    if(elapsed < FIELD_RECOMPUTE_INTERVAL) {
        bool ret = SDL_AtomicDecRef(&rmap->refcount);
        PERF_RETURN_VOID();
    }

    /* The target cell location got blocked. Try to get as close as possible. 
     */
    struct cell *cell = cell_for_ent(formation, uid);
    if(M_NavPositionBlocked(rmap->snapshot, layer, cell->reachable_pos)) {

        vec2_t new_reachable = cell->reachable_pos;
        M_NavClosestPathable(rmap->snapshot, layer, cell->reachable_pos, &new_reachable);

        cell->reachable_pos = new_reachable;
        dispatch_cell_task(formation, formation->center, uid, sub, work, cell, cell_field_task);

        work->last_update_ticks = curr;
        bool ret = SDL_AtomicDecRef(&rmap->refcount);
        PERF_RETURN_VOID();
    }

    /* The entity got blocked in by other units, such that it's trapped 
     * on an 'island' and can no longer reach its' target tile. In that
     * case, resort to stopping the unit at its' location.
     */
    if(!M_NavPositionBlocked(rmap->snapshot, layer, pos)
    && cell_get_dir(field, coord.r, coord.c) == FD_NONE) {
        cell->reachable_pos = pos;
        bool ret = SDL_AtomicDecRef(&rmap->refcount);
        PERF_RETURN_VOID();
    }

    struct tile_desc td;
    bool exists = M_Tile_DescForPoint2D(res, map_pos, pos, &td);
    assert(exists);

    /* The entity may end up on a blocked tile (which is possible if a unit 
     * right next to it has 'stopped' and occupied a set of tiles under it). 
     */
    if(M_NavPositionBlocked(rmap->snapshot, layer, pos)) {

        work->input.curr_tile = td;
        dispatch_cell_task(formation, formation->center, uid, sub, work, cell, 
            cell_field_fixup_task);
        work->last_update_ticks = curr;
    }
    /* If the current field is leading the entity towards
     * a blocked tile, it must be outdated. Recompute it.
     */
    else if(will_collide(field, layer, coord, pos)) {

        dispatch_cell_task(formation, formation->center, uid, sub, work, cell, cell_field_task);
        work->last_update_ticks = curr;
    }

    bool ret = SDL_AtomicDecRef(&rmap->refcount);
    PERF_RETURN_VOID();
}

float G_Formation_Speed(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    struct formation *formation = formation_for_ent(uid);
    assert(formation);
    return formation->speed;
}

enum formation_type G_Formation_Type(formation_id_t fid)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(formation, s_formations, fid);
    assert(k != kh_end(s_formations));
    struct formation *formation = &kh_val(s_formations, k);
    return formation->type;
}

vec2_t G_Formation_AlignmentForce(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    vec2_t total = (vec2_t){0.0f, 0.0f};
    size_t nents = 0;

    struct formation *formation = formation_for_ent(uid);
    struct subformation *sub = subformation_for_ent(formation, uid);
    if(sub->state == SUBFORMATION_COMPUTING_ASSIGNMENT)
        return (vec2_t){0.0f, 0.0f};

    uint32_t leader = subformation_leader(sub);
    assert(leader != NULL_FID);

    int col_offset;
    if(in_front_row(uid, leader, sub, &col_offset)) {

        int front_row = front_row_idx(sub);
        for(int c = 0; c < sub->ncols; c++) {
            int idx = CELL_IDX(front_row, c, sub->ncols);
            struct cell *cell = &vec_AT(&sub->cells, idx);
            if(cell->state != CELL_OCCUPIED)
                continue;

            khiter_t k = kh_get(reverse, sub->reverse, idx);
            if(k == kh_end(sub->reverse))
                continue;

            uint32_t ent = kh_val(sub->reverse, k);
            if(ent == uid)
                continue;

            quat_t rot = Entity_GetRot(ent);
            vec4_t front = (vec4_t){0.0f, 0.0f, 1.0f, 1.0f};
            mat4x4_t rot_mat;
            PFM_Mat4x4_RotFromQuat(&rot, &rot_mat);

            vec4_t dir;
            PFM_Mat4x4_Mult4x1(&rot_mat, &front, &dir);

            vec2_t dir_xz = (vec2_t){dir.x, dir.z};
            PFM_Vec2_Add(&total, &dir_xz, &total);
            nents++;
        }
    }

    if(nents > 0) {
        PFM_Vec2_Normal(&total, &total);
    }
    return total;
}

vec2_t G_Formation_CohesionForce(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    struct formation *formation = formation_for_ent(uid);
    struct subformation *sub = subformation_for_ent(formation, uid);
    if(sub->state == SUBFORMATION_COMPUTING_ASSIGNMENT)
        return (vec2_t){0.0f, 0.0f};

    float radius = G_GetSelectionRadius(uid);
    vec2_t pos = G_Pos_GetXZ(uid);
    uint32_t leader = subformation_leader(sub);
    assert(leader != NULL_FID);

    vec2_t anchor, heading;
    subformation_anchor_and_heading(leader, &anchor, &heading);

    int col_offset;
    if(in_front_row(uid, leader, sub, &col_offset)) {
        float distance = -col_offset * (2 * radius + MOVE_BUFFER_DIST);
        vec2_t target = entity_target_position(anchor, heading, distance);

        vec2_t ret = (vec2_t){0.0f, 0.0f};
        PFM_Vec2_Sub(&target, &pos, &ret);
        if(PFM_Vec2_Len(&ret) > EPSILON) {
            PFM_Vec2_Normal(&ret, &ret);
        }
        return ret;
    }

    return follow_force(uid, sub);
}

vec2_t G_Formation_DragForce(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    struct formation *formation = formation_for_ent(uid);
    struct subformation *sub = subformation_for_ent(formation, uid);
    if(sub->state == SUBFORMATION_COMPUTING_ASSIGNMENT)
        return (vec2_t){0.0f, 0.0f};

    float radius = G_GetSelectionRadius(uid);
    vec2_t pos = G_Pos_GetXZ(uid);
    uint32_t leader = subformation_leader(sub);
    assert(leader != NULL_FID);

    vec2_t anchor, heading;
    subformation_anchor_and_heading(leader, &anchor, &heading);

    int col_offset;
    bool front_row = in_front_row(uid, leader, sub, &col_offset);

    if((uid == leader) && !leader_should_slow_dowm(leader, sub)) {
        return (vec2_t){0.0f, 0.0f};
    }

    float distance = -col_offset * (2 * radius + MOVE_BUFFER_DIST);
    vec2_t target = entity_target_position(anchor, heading, distance);

    float amount;
    if((uid == leader) || ahead_of_target(uid, target, anchor, heading, &amount)) {
        /* The drag force will act in the direction 
         * opposite of the entity's current orientation.
         */
        vec2_t ent_anchor, ent_heading;
        subformation_anchor_and_heading(uid, &ent_anchor, &ent_heading);
        PFM_Vec2_Scale(&ent_heading, -1.0f, &ent_heading);
        if(PFM_Vec2_Len(&ent_heading) > EPSILON) {
            PFM_Vec2_Normal(&ent_heading, &ent_heading);
        }
        return ent_heading;
    }

    /* Back-row units will experience drag if the unit directly 
     * in front of them is affected by drag */
    uint32_t front;
    if((front = unit_in_front(uid, sub)) != NULL_UID) {
        vec2_t delta;
        vec2_t front_pos = G_Pos_GetXZ(front);
        PFM_Vec2_Sub(&pos, &front_pos, &delta);
        if(PFM_Vec2_Len(&delta) < (2 * radius + 5.0f)) {
            return G_Formation_DragForce(front);
        }
    }
    return (vec2_t){0.0f, 0.0f};
}

void G_Formation_RenderPlacement(const vec_entity_t *ents, vec2_t target, vec2_t orientation)
{
    ASSERT_IN_MAIN_THREAD();

    if(PFM_Vec2_Len(&orientation) < EPSILON) {
        orientation = G_Formation_AutoOrientation(target, ents);
    }

    struct map *map = M_AL_CopyWithFields(s_map);
    for(int i = 0; i < vec_size(ents); i++) {
        uint32_t uid = vec_AT(ents, i);
        float radius = G_GetSelectionRadius(uid);
        vec2_t pos = G_Pos_GetXZ(uid);
        int faction_id = G_GetFactionID(uid);
        uint32_t flags = G_FlagsGet(uid);
        if(G_Move_Still(uid)) {
            M_NavBlockersDecref(pos, radius, faction_id, flags, map);
        }
    }

    enum formation_type type = G_Formation_PreferredForSet(ents);
    struct formation formation = (struct formation){
        .refcount = vec_size(ents),
        .type = type,
        .target = target,
        .orientation = orientation,
        .center = field_center(target, orientation),
        .ents = copy_vector(ents),
        .speed = formation_speed(ents),
        .created_tick = SDL_GetTicks(),
        .sub_assignment = kh_init(assignment),
        .map_snapshots = kh_init(map)
    };
    init_subformations(&formation);
    vec_assignment_work_init(&formation.work);

    enum nav_layer layers[NAV_LAYER_MAX];
    size_t nlayers = formation_layers(&formation.subformations, layers);
    for(int i = 0; i < nlayers; i++) {
        init_occupied_field(map, layers[i], formation.center, formation.occupied[layers[i]]);
        init_islands_field(map, layers[i], formation.center, formation.islands[layers[i]]);
    }

    for(int i = 0; i < vec_size(&formation.subformations); i++) {
        struct subformation *sub = &vec_AT(&formation.subformations, i);
        place_subformation(sub, formation.center, target, formation.orientation, 
            formation.occupied, formation.islands);
        mark_unused_cells(sub);
        render_cells(sub);
    }
    destroy_formation(&formation);
    M_AL_FreeCopyWithFields(map);
}

bool G_Formation_SaveState(struct SDL_RWops *stream)
{
    /* Save next formation ID */
    struct attr next_fid = (struct attr){
        .type = TYPE_INT,
        .val.as_int = s_next_id
    };
    CHK_TRUE_RET(Attr_Write(stream, &next_fid, "next_fid"));
    
    /* Save entity:formation map */
    struct attr nents = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_ent_formation_map)
    };
    CHK_TRUE_RET(Attr_Write(stream, &nents, "num_entities"));

    uint32_t uid;
    formation_id_t ent_fid;
    kh_foreach(s_ent_formation_map, uid, ent_fid, {
        struct attr uid_attr = (struct attr){
            .type = TYPE_INT,
            .val.as_int = uid
        };
        CHK_TRUE_RET(Attr_Write(stream, &uid_attr, "uid"));

        struct attr fid_attr = (struct attr){
            .type = TYPE_INT,
            .val.as_int = ent_fid
        };
        CHK_TRUE_RET(Attr_Write(stream, &fid_attr, "fid"));
    });
    Sched_TryYield();

    /* Save formation structures */
    struct attr nformations = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_formations)
    };
    CHK_TRUE_RET(Attr_Write(stream, &nformations, "num_formations"));

    formation_id_t fid;
    struct formation *formation;
    kh_foreach_val_ptr(s_formations, fid, formation, {

        struct attr fid_attr = (struct attr){
            .type = TYPE_INT,
            .val.as_int = fid
        };
        CHK_TRUE_RET(Attr_Write(stream, &fid_attr, "fid"));

        struct attr refcount = (struct attr){
            .type = TYPE_INT,
            .val.as_int = formation->refcount
        };
        CHK_TRUE_RET(Attr_Write(stream, &refcount, "refcount"));

        struct attr type = (struct attr){
            .type = TYPE_INT,
            .val.as_int = formation->type
        };
        CHK_TRUE_RET(Attr_Write(stream, &type, "type"));

        struct attr target = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = formation->target
        };
        CHK_TRUE_RET(Attr_Write(stream, &target, "target"));

        struct attr orientation = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = formation->orientation
        };
        CHK_TRUE_RET(Attr_Write(stream, &orientation, "orientation"));

        struct attr center = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = formation->center
        };
        CHK_TRUE_RET(Attr_Write(stream, &center, "center"));

        struct attr nents = (struct attr){
            .type = TYPE_INT,
            .val.as_int = kh_size(formation->ents)
        };
        CHK_TRUE_RET(Attr_Write(stream, &nents, "num_entities"));

        uint32_t uid;
        kh_foreach_key(formation->ents, uid, {
            struct attr uid_attr = (struct attr){
                .type = TYPE_INT,
                .val.as_int = uid
            };
            CHK_TRUE_RET(Attr_Write(stream, &uid_attr, "entity"));
        });
        Sched_TryYield();

        struct attr speed = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float = formation->speed
        };
        CHK_TRUE_RET(Attr_Write(stream, &speed, "speed"));

        /* created_tick is not saved */

        /* Save sub assignment */
        struct attr nassigned = (struct attr){
            .type = TYPE_INT,
            .val.as_int = kh_size(formation->sub_assignment)
        };
        CHK_TRUE_RET(Attr_Write(stream, &nassigned, "num_assigned"));

        struct coord coord;
        kh_foreach(formation->sub_assignment, uid, coord, {

            struct attr uid_attr = (struct attr){
                .type = TYPE_INT,
                .val.as_int = uid
            };
            CHK_TRUE_RET(Attr_Write(stream, &uid_attr, "uid"));

            struct attr coord_r = (struct attr){
                .type = TYPE_INT,
                .val.as_int = coord.r
            };
            CHK_TRUE_RET(Attr_Write(stream, &coord_r, "coord_r"));

            struct attr coord_c = (struct attr){
                .type = TYPE_INT,
                .val.as_int = coord.c
            };
            CHK_TRUE_RET(Attr_Write(stream, &coord_c, "coord_c"));
        });
        Sched_TryYield();

        /* Save subformations */
        struct attr nsubformations = (struct attr){
            .type = TYPE_INT,
            .val.as_int = vec_size(&formation->subformations)
        };
        CHK_TRUE_RET(Attr_Write(stream, &nsubformations, "nsubformations"));

        for(int i = 0; i < vec_size(&formation->subformations); i++) {
            struct subformation *curr = &vec_AT(&formation->subformations, i);
            CHK_TRUE_RET(subformation_save_state(formation, curr, stream));
            Sched_TryYield();
        }

        /* Save subformation pointers by index/reference */
        size_t root_idx = subformation_index(formation, formation->root);
        struct attr root = (struct attr){
            .type = TYPE_INT,
            .val.as_int = root_idx
        };
        CHK_TRUE_RET(Attr_Write(stream, &root, "root"));

        /* map_snapshots is not saved */

        for(int l = 0; l < NAV_LAYER_MAX; l++) {
        for(int r = 0; r < OCCUPIED_FIELD_RES; r++) {
        for(int c = 0; c < OCCUPIED_FIELD_RES; c++) {

            struct attr occupied_state = (struct attr){
                .type = TYPE_INT,
                .val.as_int = formation->occupied[l][r][c]
            };
            CHK_TRUE_RET(Attr_Write(stream, &occupied_state, "occupied_state"));
        }}}
        Sched_TryYield();

        for(int l = 0; l < NAV_LAYER_MAX; l++) {
        for(int r = 0; r < OCCUPIED_FIELD_RES; r++) {
        for(int c = 0; c < OCCUPIED_FIELD_RES; c++) {

            struct attr island_state = (struct attr){
                .type = TYPE_INT,
                .val.as_int = formation->islands[l][r][c]
            };
            CHK_TRUE_RET(Attr_Write(stream, &island_state, "island_state"));
        }}}
        Sched_TryYield();
    });
    return true;
}

bool G_Formation_LoadState(struct SDL_RWops *stream)
{
    uint32_t curr_tick = SDL_GetTicks();
    struct attr attr;

    /* Load next formation ID */
    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    s_next_id = attr.val.as_int;

    /* Load entity:formation map */
    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    size_t nents = attr.val.as_int;

    for(int i = 0; i < nents; i++) {

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        uint32_t uid = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        formation_id_t fid = attr.val.as_int;

        int ret;
        khiter_t k = kh_put(mapping, s_ent_formation_map, uid, &ret);
        assert(ret != -1);
        kh_val(s_ent_formation_map, k) = fid;
    }
    Sched_TryYield();

    /* Load formation structures */
    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    size_t nformations = attr.val.as_int;

    for(int i = 0; i < nformations; i++) {

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        formation_id_t fid = attr.val.as_int;

        int ret;
        khiter_t k = kh_put(formation, s_formations, fid, &ret);
        assert(ret != -1);
        struct formation *new = &kh_val(s_formations, k);

        *new = (struct formation){
            .ents = kh_init(entity),
            .sub_assignment = kh_init(assignment),
            .map_snapshots = kh_init(map)
        };
        vec_subformation_init(&new->subformations);

        CHK_TRUE_JMP(Attr_Parse(stream, &attr, true), fail_load_formation);
        CHK_TRUE_JMP(attr.type == TYPE_INT, fail_load_formation);
        new->refcount = attr.val.as_int;

        CHK_TRUE_JMP(Attr_Parse(stream, &attr, true), fail_load_formation);
        CHK_TRUE_JMP(attr.type == TYPE_INT, fail_load_formation);
        new->type = attr.val.as_int;

        CHK_TRUE_JMP(Attr_Parse(stream, &attr, true), fail_load_formation);
        CHK_TRUE_JMP(attr.type == TYPE_VEC2, fail_load_formation);
        new->target = attr.val.as_vec2;

        CHK_TRUE_JMP(Attr_Parse(stream, &attr, true), fail_load_formation);
        CHK_TRUE_JMP(attr.type == TYPE_VEC2, fail_load_formation);
        new->orientation = attr.val.as_vec2;

        CHK_TRUE_JMP(Attr_Parse(stream, &attr, true), fail_load_formation);
        CHK_TRUE_JMP(attr.type == TYPE_VEC2, fail_load_formation);
        new->center = attr.val.as_vec2;

        CHK_TRUE_JMP(Attr_Parse(stream, &attr, true), fail_load_formation);
        CHK_TRUE_JMP(attr.type == TYPE_INT, fail_load_formation);
        size_t nents = attr.val.as_int;

        for(int i = 0; i < nents; i++) {

            CHK_TRUE_JMP(Attr_Parse(stream, &attr, true), fail_load_formation);
            CHK_TRUE_JMP(attr.type == TYPE_INT, fail_load_formation);
            uint32_t uid = attr.val.as_int;

            int ret;
            khiter_t k = kh_put(entity, new->ents, uid, &ret);
            CHK_TRUE_JMP(ret != -1, fail_load_formation);
        }
        Sched_TryYield();

        CHK_TRUE_JMP(Attr_Parse(stream, &attr, true), fail_load_formation);
        CHK_TRUE_JMP(attr.type == TYPE_FLOAT, fail_load_formation);
        new->speed = attr.val.as_float;
        new->created_tick = curr_tick;

        CHK_TRUE_JMP(Attr_Parse(stream, &attr, true), fail_load_formation);
        CHK_TRUE_JMP(attr.type == TYPE_INT, fail_load_formation);
        size_t nassigned = attr.val.as_int;

        for(int i = 0; i < nassigned; i++) {

            CHK_TRUE_JMP(Attr_Parse(stream, &attr, true), fail_load_formation);
            CHK_TRUE_JMP(attr.type == TYPE_INT, fail_load_formation);
            uint32_t uid = attr.val.as_int;

            CHK_TRUE_JMP(Attr_Parse(stream, &attr, true), fail_load_formation);
            CHK_TRUE_JMP(attr.type == TYPE_INT, fail_load_formation);
            int r = attr.val.as_int;

            CHK_TRUE_JMP(Attr_Parse(stream, &attr, true), fail_load_formation);
            CHK_TRUE_JMP(attr.type == TYPE_INT, fail_load_formation);
            int c = attr.val.as_int;

            int ret;
            khiter_t k = kh_put(assignment, new->sub_assignment, uid, &ret);
            CHK_TRUE_JMP(ret != -1, fail_load_formation);
            kh_val(new->sub_assignment, k) = (struct coord){r, c};
        }
        Sched_TryYield();

        /* Load subformations */
        CHK_TRUE_JMP(Attr_Parse(stream, &attr, true), fail_load_formation);
        CHK_TRUE_JMP(attr.type == TYPE_INT, fail_load_formation);
        size_t nsub = attr.val.as_int;
        vec_subformation_resize(&new->subformations, nsub);
        new->subformations.size = nsub;

        size_t nloaded = 0;
        for(int i = 0; i < nsub; i++) {
            nloaded++;
            struct subformation *next = &vec_AT(&new->subformations, i);
            CHK_TRUE_JMP(subformation_load_state(new, next, stream), fail_load_subformations);
            Sched_TryYield();
        }

        CHK_TRUE_JMP(Attr_Parse(stream, &attr, true), fail_load_subformations);
        CHK_TRUE_JMP(attr.type == TYPE_INT, fail_load_subformations);
        new->root = &vec_AT(&new->subformations, attr.val.as_int);

        /* Load occupied fields */
        for(int l = 0; l < NAV_LAYER_MAX; l++) {
        for(int r = 0; r < OCCUPIED_FIELD_RES; r++) {
        for(int c = 0; c < OCCUPIED_FIELD_RES; c++) {

            CHK_TRUE_JMP(Attr_Parse(stream, &attr, true), fail_load_subformations);
            CHK_TRUE_JMP(attr.type == TYPE_INT, fail_load_subformations);
            new->occupied[l][r][c] = attr.val.as_int;
        }}}
        Sched_TryYield();

        /* Load islands fields */
        for(int l = 0; l < NAV_LAYER_MAX; l++) {
        for(int r = 0; r < OCCUPIED_FIELD_RES; r++) {
        for(int c = 0; c < OCCUPIED_FIELD_RES; c++) {

            CHK_TRUE_JMP(Attr_Parse(stream, &attr, true), fail_load_subformations);
            CHK_TRUE_JMP(attr.type == TYPE_INT, fail_load_subformations);
            new->islands[l][r][c] = attr.val.as_int;
        }}}

        Sched_TryYield();
        continue;

    fail_load_subformations:
        for(int i = 0; i < nloaded; i++) {
            struct subformation *curr = &vec_AT(&new->subformations, i);
            destroy_subformation(curr);
        }
    fail_load_formation:
        kh_destroy(map, new->map_snapshots);
        kh_destroy(entity, new->ents);
        vec_subformation_destroy(&new->subformations);
        kh_destroy(assignment, new->sub_assignment);
        kh_del(formation, s_formations, k);
        return false;
    }

    /* Re-compute all necessary cell fields */
    formation_id_t fid;
    struct formation *formation;
    kh_foreach_val_ptr(s_formations, fid, formation, {

        size_t nsubformations = vec_size(&formation->subformations);
        vec_assignment_work_init(&formation->work);
        vec_assignment_work_resize(&formation->work, nsubformations);
        formation->work.size = nsubformations;

        for(int i = 0; i < vec_size(&formation->subformations); i++) {

            struct subformation *sub = &vec_AT(&formation->subformations, i);
            struct cell_assignment_work *work = &vec_AT(&formation->work, i);

            cell_assignment_work_init(work, sub, fid, i);
            dispatch_cell_assignment_task(work);
            complete_cell_assignment_work(work, true);
            collect_cell_assignment_result(work, sub);
            cell_assignment_work_destroy(work);
        }
    });
    kh_foreach_ptr(s_formations, formation, {
        for(int i = 0; i < vec_size(&formation->subformations); i++) {
            struct subformation *sub = &vec_AT(&formation->subformations, i);
            dispatch_cell_field_work(formation, formation->center, sub);
        }
    });
    kh_foreach_ptr(s_formations, formation, {
        for(int i = 0; i < vec_size(&formation->subformations); i++) {
            struct subformation *sub = &vec_AT(&formation->subformations, i);
            complete_cell_field_work(sub, true);
        }
    });
    return true;
}

