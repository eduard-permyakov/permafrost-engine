/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2026 Eduard Permyakov 
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
#define MEM_FILE_SUB MEM_SUB_GAME_MOVEMENT

#include "movement.h"
#include "arrival.h"
#include "game_private.h"
#include "formation.h"
#include "combat.h"
#include "clearpath.h"
#include "position.h"
#include "fog_of_war.h"
#include "public/game.h"
#include "../config.h"
#include "../camera.h"
#include "../asset_load.h"
#include "../event.h"
#include "../entity.h"
#include "../cursor.h"
#include "../settings.h"
#include "../ui.h"
#include "../perf.h"
#include "../sched.h"
#include "../task.h"
#include "../lib/public/simd.h"
#include "../main.h"
#include "../navigation/public/nav.h"
#include "../lib/public/queue.h"
#include "../phys/public/collision.h"
#include "../script/public/script.h"
#include "../render/public/render.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"
#include "../lib/public/vec.h"
#include "../lib/public/attr.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/stalloc.h"
#include "../anim/public/anim.h"
#include "../lib/public/stalloc.h"

#include <assert.h>
#include <SDL.h>

#include "../mem.h"

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_GAME, MEM_SUB_GAME_MOVEMENT)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_GAME, MEM_SUB_GAME_MOVEMENT)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_GAME, MEM_SUB_GAME_MOVEMENT)

static int hz_count(enum movement_hz hz);

/* For the purpose of movement simulation, all entities have the same mass,
 * meaning they are accelerate the same amount when applied equal forces. */
#define ENTITY_MASS           (1.0f)
#define EPSILON               (1.0f/1024)
#define MAX_FORCE             (0.75f)
#define SCALED_MAX_FORCE      (MAX_FORCE / hz_count(s_move_work.hz) * 20.0)
#define VEL_HIST_LEN          (14)
#define MAX_MOVE_TASKS        (64)
#define MAX_REBUILDS_PER_TICK (64)
#define MAX_GPU_FLOCK_MEMBERS (1024)  /* Must match movement.glsl */

#define SIGNUM(x)    (((x) > 0) - ((x) < 0))
#define MAX(a, b)    ((a) > (b) ? (a) : (b))
#define MIN(a, b)    ((a) < (b) ? (a) : (b))
#define ARR_SIZE(a)  (sizeof(a)/sizeof(a[0]))
#define CHUNK_WIDTH  (X_COORDS_PER_TILE * TILES_PER_CHUNK_WIDTH)
#define CHUNK_HEIGHT (Z_COORDS_PER_TILE * TILES_PER_CHUNK_HEIGHT)
#define STR(a)       #a

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

enum move_state{
    /* Entity is moving towards the flock's destination point */
    STATE_MOVING,
    /* Like STATE_MOVING, but the entity is also constrained by
     * a number of forces that give it a tendency to occupy its' 
     * relative position in a formation. */
    STATE_MOVING_IN_FORMATION,
    /* Entity is considered to have arrived and no longer moving. */
    STATE_ARRIVED,
    /* Entity is approaching the nearest enemy entity */
    STATE_SEEK_ENEMIES,
    /* The navigation system was unable to guide the entity closer
     * to the goal. It stops and waits. */
    STATE_WAITING,
    /* Move towards the closest point touching the target entity, but 
     * stop before actually stepping on it's tiles. */
    STATE_SURROUND_ENTITY,
    /* Move towards the closest position that will take us within the
     * specified range of the target entity */
    STATE_ENTER_ENTITY_RANGE,
    /* Entity is turning until it faces a particular direction */
    STATE_TURNING,
    /* For entities that are a part of a formation, the final stage
     * of the path will have the entity move to its' dedicated cell
     * in the formation. */
    STATE_ARRIVING_TO_CELL,
    /* Running directly away from a threat position; entered and exited only
     * by the combat system, keeping flock and formation membership. */
    STATE_FLEEING
};

struct movestate{
    enum move_state state;
    /* The speed the entity actually moves at, in units of OpenGL coords /
     * second: the authored base speed plus whatever the combat modifiers add.
     * Everything that steers reads this one, so a buff needs no other change.
     */
    float              max_speed;
    float              base_speed;
    float              speed_bonus;
    /* The current velocity 
     */
    vec2_t             velocity;
    /* State tracking variables for interpolating between movement ticks.
     * During a single movement tick, the entity's position is moved to
     * intermediate points between 'prev_pos' and 'next_pos', in increments
     * expressed by 'step'.
     */
    vec3_t             next_pos;    /* The computed next movement tick position */
    vec3_t             prev_pos;    /* The position at the start of the previous tick */
    quat_t             next_rot;    /* The computed next movement tick rotation */
    quat_t             prev_rot;    /* The rotation at the start of the previous tick */
    float              step;        /* The fraction of the distance covered in a single step 
                                     * (nsteps = 1.0/step) */
    int                left;        /* The number of interpolation steps left (0 means the entity is at next_pos) */
    /* Flag to track whether the entiy is currently acting as a 
     * navigation blocker, and the last position where it became a blocker. 
     */
    bool               blocking;
    vec2_t             last_stop_pos;
    float              last_stop_radius;
    /* Entity that we're moving towards when in the 'SURROUND_STATIC_ENTITY' state 
     */
    uint32_t           surround_target_uid;
    /* Flag indicating that we are now using the 'surround' field rather than the 
     * 'target seek' field to get to path to our target. This kicks in once we pass
     * the distance 'low water' threshold and is turned off if we pass the 'high water' 
     * threshold again - this is to prevent 'toggling' at a boundary where we switch 
     * from one field to another. 
     */
    bool               using_surround_field;
};

/* State needed only by specific movement state machines, split from the hot
 * movestate so the many cheap probes across the engine pull a small value.
 */
struct movestate_aux{
    /* Information for waking up from the 'WAITING' state 
     */
    enum move_state wait_prev;
    int                wait_ticks_left;
    /* History of the previous ticks' velocities. Used for velocity smoothing. 
     */
    vec2_t             vel_hist[VEL_HIST_LEN];
    int                vel_hist_idx;
    vec2_t             surround_target_prev;
    vec2_t             surround_nearest_prev;
    /* Additional state for entities in 'ENTER_ENTITY_RANGE' state 
     */
    vec2_t             target_prev_pos;
    float              target_range;
    /* The target direction for 'turning' entities 
     */
    quat_t             target_dir;
    /* Heading a combat-held unit pivots to; separate from target_dir (also written by formation). 
     */
    quat_t             combat_facing;
    /* The position a 'FLEEING' entity is running away from, and the state to
     * restore once the flee is over.
     */
    vec2_t             flee_threat_pos;
    enum move_state    flee_prev;
    /* Target a pinned seeker presses straight at instead of following the
     * enemy-seek field; NULL_UID when unpinned. Transient. */
    uint32_t           seek_pin_target;
    /* Whether the pin held last tick (the way round was worth waiting out). */
    bool               seek_pin_held;
    /* Side of the last ClearPath deflection, kept CP_SIDE_HOLD_TICKS */
    int8_t             cp_side;
    uint8_t            cp_side_ticks;
    /* A jammed unit registers as a temporary nav blocker so the floods route
     * the units behind it around the clump. Transient. */
    uint16_t           seek_stuck_ticks;
    uint16_t           seek_clear_ticks;
    /* Consecutive ticks coasting through a stale hole in the flow field */
    uint16_t           field_void_ticks;
    /* Consecutive stand-still solves, and the relaxed solves they buy. Transient. */
    uint16_t           cp_stall_ticks;
    uint16_t           cp_relax_ticks;
    /* Set once the unit has been flowing since its last order; only then may
     * it wall itself as jam-stuck (a queued launch rear is not a jam). */
    bool               seek_progressed;
    /* Standing on its formation cell while the formation is not yet ready to
     * take its tiles: visually settled, but neither blocking nor immovable.
     */
    bool               parked;
    /* Left outside by a formation that has already taken its tiles: walks
     * through its own side's bodies and stamps to reach its cell.
     */
    bool               phasing;
    uint16_t           phase_ticks;
    bool               soft_blocking;
    vec2_t             soft_block_pos;
    float              soft_block_radius;
    /* Keeps EVENT_MOTION_START/END strictly alternating; clients assert the
     * pairing. Transient. */
    bool               motion_stopped;
    /* Per-unit fine-arrival state. 
     */
    struct arrival_unit_state arrival;
};

struct flock{
    khash_t(entity) *ents;
    vec2_t           target_xz; 
    dest_id_t        dest_id;
    /* Group-arrival state, computed per nav layer present in the flock. */
    struct arrival_group arrival;
};

struct formation_state{
    formation_id_t fid;
    bool           assignment_ready;
    bool           assigned_to_cell;
    bool           in_range_of_cell;
    bool           arrived_at_cell;
    bool           may_settle;
    bool           at_cell;
    vec2_t         normal_cohesion_force;
    vec2_t         normal_align_force;
    vec2_t         normal_drag_force;
    quat_t         target_orientation;
};

enum movestate_flags{
    UPDATE_SET_STATE        = (1 << 0),
    UPDATE_SET_VELOCITY     = (1 << 1),
    UPDATE_SET_POSITION     = (1 << 2),
    UPDATE_SET_ROTATION     = (1 << 3),
    UPDATE_SET_NEXT_POS     = (1 << 4),
    UPDATE_SET_PREV_POS     = (1 << 5),
    UPDATE_SET_STEP         = (1 << 6),
    UPDATE_SET_LEFT         = (1 << 7),
    UPDATE_SET_NEXT_ROT     = (1 << 8),
    UPDATE_SET_PREV_ROT     = (1 << 9),
    UPDATE_SET_DEST         = (1 << 10),
    UPDATE_SET_TARGET_PREV  = (1 << 11),
    UPDATE_SET_MOVING       = (1 << 12),
    UPDATE_SET_TARGET_DIR   = (1 << 13),
    UPDATE_TURNING_IN_PLACE = (1 << 14),
    /* Diagnostic only: translation was zeroed by the heading gate this tick */
    UPDATE_HEADING_GATED    = (1 << 15),
    /* A seeking unit made no progress this tick / has a thin local crowd */
    UPDATE_SEEK_STUCK       = (1 << 16),
    UPDATE_SEEK_CLEAR       = (1 << 17),
    /* The seek pin held this tick */
    UPDATE_SEEK_PINNED      = (1 << 18),
    UPDATE_FIELD_VOID       = (1 << 19),
    /* Diagnostic only: the step was refused by the landing tile */
    UPDATE_VETO_UNPATHABLE  = (1 << 19),
    UPDATE_VETO_BLOCKED     = (1 << 20)
};

struct movestate_patch{
    enum movestate_flags flags;
    enum move_state   next_state;
    vec2_t               next_velocity;
    vec3_t               next_pos;
    quat_t               next_rot;
    bool                 next_block;
    vec3_t               next_ppos;
    vec3_t               next_npos;
    float                next_step;
    float                next_left;
    quat_t               next_nrot;
    quat_t               next_prot;
    vec2_t               next_dest;
    bool                 next_attack;
    vec2_t               next_target_prev;
    quat_t               next_target_dir;
};

/* Dense per-flock snapshot of member uids and positions, in flock iteration
 * order, so the O(flock) cohesion scan reads packed arrays instead of probing
 * two hash tables per member. */
struct flock_snap{
    const uint32_t *uids;
    const float    *pos_x;
    const float    *pos_z;
    size_t          nents;
};

struct move_work_in{
    uint32_t       ent_uid;
    /* The unit's flock (and its snapshot), resolved once at submit time;
     * flock membership is stable for the duration of the tick. */
    struct flock            *flock;
    const struct flock_snap *fsnap;
    vec2_t         ent_des_v;
    float          speed;
    vec2_t         cell_pos;
    struct cp_ent  cp_ent;
    bool           save_debug;
    /* Scratch slices of s_move_work.neighb_mem, capacity MAX_NEIGHBOURS each,
     * filled by find_neighbours in the velocity phase. */
    struct cp_ent *stat_neighbs;
    struct cp_ent *dyn_neighbs;
    /* Blocked-tile centres near the unit, walls for the velocity solve */
    vec2_t        *tile_obs;
    size_t         nstat;
    size_t         ndyn;
    size_t         ntiles;
    /* In-reach neighbours in non-still movement states (jam evidence) */
    size_t         njam;
    /* Nearest neighbour in reach, and whether this unit gives it the lane */
    uint32_t       nn_uid;
    float          nn_dist;
    bool           hug;
    /* Terrain-impassable subset of the tile obstacles */
    size_t         nterrain;
    /* The field sample was void and the unit is coasting toward the goal */
    bool           field_void;
    bool           has_dest_los;
    bool           needs_los_build;
    bool           los_queried;
    bool           los_built;
    /* The LOS answer awaits a chain build on the worker pool; resolved by a
     * cache peek once the chains are joined and published. */
    bool           los_deferred;
    /* The rebuild budget ran out before this unit's field was serviced; a zero
     * desired velocity then means "not served yet", not "can't be routed". */
    bool           field_starved;
    /* The seek pin held for this tick's desired velocity */
    bool           seek_pinned;
    int8_t         cp_side;
    struct formation_state fstate;
    vec2_t         cell_arrival_vdes;
};

/* How the velocity solve resolved, for the per-tick mechanism counters. */
enum cp_out_flags{
    CP_OUT_GAVE_UP    = (1 << 0),
    CP_OUT_RETRY_OK   = (1 << 1),
    CP_OUT_STALLED    = (1 << 2),
    CP_OUT_SEEK_VDES0 = (1 << 3),
};

struct move_work_out{
    uint32_t ent_uid;
    vec2_t   ent_des_v;
    vec2_t   ent_vel;
    uint8_t  cp_flags;
    int8_t   cp_side;
    struct movestate_patch patch;
};

struct move_task_arg{
    size_t begin_idx;
    size_t end_idx;
};

/* The subset of the gamestate that is necessary 
 * to derive the new entity velocities and positions. 
 * We make a copy of this state so that movement 
 * computations can safely be done asynchronously,
 * or even be spread over multiple frames. 
 */
struct move_gamestate{
    khash_t(id)           *flags;
    khash_t(pos)          *positions;
    bg_ent_t              *postree;
    khash_t(range)        *sel_radiuses;
    khash_t(id)           *faction_ids;
    khash_t(id)           *ent_gpu_id_map;
    khash_t(id)           *gpu_id_ent_map;
    struct map            *map;
    /* Additional state needed for nav_unit_query_ctx */
    struct kh_aabb_s      *aabbs;
    void                  *transforms;
    bool                  fog_enabled;
    uint32_t              *fog_state;
    struct kh_id_s        *dying_set;
    enum diplomacy_state (*diptable)[MAX_FACTIONS];
    uint16_t               player_controllable;
};

enum move_work_type{
    WORK_TYPE_CPU,
    WORK_TYPE_GPU
};

enum move_work_status{
    WORK_COMPLETE,
    WORK_INCOMPLETE
};

struct move_work{
    struct memstack           mem;
    struct move_gamestate     gamestate;
    enum move_work_type       type;
    struct nav_unit_query_ctx unit_query_ctx;
    enum movement_hz          hz;
    struct move_work_in      *in;
    struct move_work_out     *out;
    struct cp_ent            *neighb_mem;
    vec2_t                   *tile_mem;
    struct flock_snap        *flock_snaps;
    /* Parallel to in/out, NULL unless tracing */
    struct move_trace        *trace;
    /* Largest movable radius seen, for the centre-based neighbour queries */
    float                     max_radius;
    size_t                    nwork;
    size_t                    ntasks;
    uint32_t                  tids[MAX_MOVE_TASKS];
    SDL_atomic_t              gpu_velocities_ready;
    vec2_t                   *gpu_velocities;
    struct future             futures[MAX_MOVE_TASKS];
};

/* Must match movement.glsl */
struct gpu_flock_desc{
    GLuint  ents[MAX_GPU_FLOCK_MEMBERS];
    GLuint  nmembers;
    GLfloat target_x;
    GLfloat target_z;
};

/* Must match movement.glsl */
struct gpu_ent_desc{
    vec2_t   dest;
    vec2_t   vdes;
    vec2_t   cell_pos;
    vec2_t   formation_cohesion_force;
    vec2_t   formation_align_force;
    vec2_t   formation_drag_force;
    vec2_t   pos;
    vec2_t   velocity;
    uint32_t movestate;
    uint32_t flock_id;
    uint32_t flags;
    float    speed;
    float    max_speed;
    float    radius;
    uint32_t layer;
    uint32_t has_dest_los;
    uint32_t formation_assignment_ready;
    uint32_t __pad0; /* Keep aligned to vec2 size */
};

enum move_cmd_type{
    MOVE_CMD_ADD,
    MOVE_CMD_REMOVE,
    MOVE_CMD_STOP,
    MOVE_CMD_SET_DEST,
    MOVE_CMD_CHANGE_DIRECTION,
    MOVE_CMD_SET_ENTER_RANGE,
    MOVE_CMD_SET_SEEK_ENEMIES,
    MOVE_CMD_SET_SURROUND_ENTITY,
    MOVE_CMD_UPDATE_POS,
    MOVE_CMD_UPDATE_FACTION_ID,
    MOVE_CMD_UPDATE_SELECTION_RADIUS,
    MOVE_CMD_SET_MAX_SPEED,
    MOVE_CMD_SET_SPEED_BONUS,
    MOVE_CMD_MAKE_FLOCKS,
    MOVE_CMD_UNBLOCK,
    MOVE_CMD_BLOCK,
    MOVE_CMD_SET_COMBAT_FACING,
    MOVE_CMD_SET_COMBAT_HELD,
    MOVE_CMD_SET_FLEE,
    MOVE_CMD_STOP_FLEE,
    MOVE_CMD_SET_SEEK_PIN
};

/* Commands carry small typed payloads; the uid is hoisted out of the union so
 * the queue-snooping helpers can read it uniformly (0 for MAKE_FLOCKS).
 */
struct move_cmd{
    bool               deleted;
    enum move_cmd_type type;
    uint32_t           uid;
    union{
        struct{
            vec3_t pos;
            float  radius;
            int    faction_id;
        }add;
        struct{
            vec2_t dest_xz;
            bool   attack;
        }set_dest;
        struct{
            quat_t target;
        }change_direction;
        struct{
            uint32_t target;
            float    range;
        }enter_range;
        struct{
            uint32_t target;
        }surround;
        struct{
            vec2_t pos;
        }update_pos;
        struct{
            int oldfac;
            int newfac;
        }update_faction;
        struct{
            float radius;
        }update_radius;
        struct{
            float speed;
        }max_speed;
        struct{
            float bonus;
        }speed_bonus;
        struct{
            vec_entity_t       *sel;
            vec2_t              target_xz;
            enum formation_type type;
            bool                attack;
            vec2_t              target_orientation;
        }make_flocks;
        struct{
            vec3_t pos;
        }block;
        struct{
            quat_t dir;
        }combat_facing;
        struct{
            bool held;
        }combat_held;
        struct{
            vec2_t threat_xz;
        }flee;
        struct{
            uint32_t target;
        }seek_pin;
    }u;
};

KHASH_MAP_INIT_INT(state, struct movestate)
KHASH_MAP_INIT_INT(auxstate, struct movestate_aux)
KHASH_MAP_INIT_INT(aabb, struct aabb)
KHASH_MAP_INIT_INT(findex, int)

QUEUE_TYPE(cmd, struct move_cmd)
QUEUE_IMPL(static, cmd, struct move_cmd)

VEC_TYPE(flock, struct flock)
VEC_IMPL(static inline, flock, struct flock)

SHARED_PTR_ASSERT_LAYOUT(struct refcounted_map, sp);

static void move_push_cmd(struct move_cmd cmd);
static void do_set_dest(uint32_t uid, vec2_t dest_xz, bool attack);
static void do_stop(uint32_t uid);
static void do_set_seek_pin(uint32_t uid, uint32_t target);
static void clear_seek_pin(uint32_t uid);
static bool ent_still(const struct movestate *ms);
static void move_notify_motion_start(uint32_t uid, struct movestate *ms);
static void move_notify_motion_end(uint32_t uid);
static void do_update_pos(uint32_t uid, vec2_t pos);
static struct move_work_in *work_input_for_uid(uint32_t uid);
static void resume_waiting_units(void);
static void move_tick(void *user, void *event);
static struct result navigation_tick_task(void *arg);

/* Parameters controlling steering/flocking behaviours */
#define SEPARATION_FORCE_SCALE          (0.6f)
#define MOVE_ARRIVE_FORCE_SCALE         (0.5f)
#define MOVE_COHESION_FORCE_SCALE       (0.15f)
#define ALIGNMENT_FORCE_SCALE           (0.15f)

#define SEPARATION_BUFFER_DIST          (0.0f)
#define COHESION_NEIGHBOUR_RADIUS       (50.0f)
#define ARRIVE_SLOWING_RADIUS           (10.0f)
#define ADJACENCY_SEP_DIST              (5.0f)
#define SEPARATION_NEIGHB_RADIUS        (30.0f)
/* Beyond this multiple of the radius sum the separation term is e^-10 of its
 * contact value; the floor keeps the soldier query as it was. */
#define SEPARATION_NEIGHB_SCALE         (1.3f)
/* Deeper interpenetration than this keeps the pure radial push-out */
#define SEP_REDIRECT_MIN_GAP            (-2.0f)
#define CELL_ARRIVAL_RADIUS             (30.0f)

#define COLLISION_MAX_SEE_AHEAD         (10.0f)
#define WAIT_TICKS                      (60)
#define MAX_TURN_RATE                   (15.0f) /* degree/tick */
#define SCALED_MAX_TURN_RATE            (MAX_TURN_RATE / hz_count(s_move_work.hz) * 20.0)
#define MOVE_HEADING_HALT               (90.0f) /* degrees; halt a moving unit to re-aim past this */
#define MOVE_HEADING_RESUME             (10.0f) /* degrees; resume/start a halted unit within this */
#define MAX_NEIGHBOURS                  (32)
#define CLEARPATH_STILL_SPEED           (0.3f)  /* A neighbour slower than this is treated as static (full, non-reciprocal avoidance) so a settling unit is not passed through */

#define SURROUND_LOW_WATER_X            (CHUNK_WIDTH/3.0f)
#define SURROUND_HIGH_WATER_X           (CHUNK_WIDTH/2.0f)
#define SURROUND_LOW_WATER_Z            (CHUNK_HEIGHT/3.0f)
#define SURROUND_HIGH_WATER_Z           (CHUNK_HEIGHT/2.0f)

/* How long a unit heads for its goal across a stale hole in the field before
 * treating the destination as genuinely unreachable.
 */
#define FIELD_VOID_COAST_TICKS          (40)

/* A straggler gives up phasing after this long, so that one which can never
 * reach its cell does not walk through its own side for good.
 */
#define PHASE_MAX_TICKS                 (400)
/* A unit standing still this long inside a crowd becomes a soft blocker,
 * released once the local crowd thins for the hold duration. */
#define SEEK_STUCK_BLOCK_TICKS          (10)
/* Neighbours in ClearPath reach that are themselves trying to move: a jam is
 * mutual obstruction, so parked bodies are not evidence of one.
 */
#define SEEK_RELEASE_NEIGHBS            (6)
#define SEEK_RELEASE_HOLD_TICKS         (20)
/* Stagger registrations so a mass jam doesn't spike the invalidations */
#define SEEK_BLOCK_BUDGET_PER_TICK      (16)
/* Ticks without a deflection before the passing side is forgotten */
#define CP_SIDE_HOLD_TICKS              (10)
/* Stand-still answers before the solve is relaxed, and the relaxation's hold */
#define CP_STALL_RELAX_TICKS            (20)
#define CP_STALL_RELAX_HOLD             (20)
/* Walked but got nowhere, over the velocity history */
#define CP_WOBBLE_NET_FRACTION          (0.25f)
#define CP_WOBBLE_NET_MAX               (0.5f)
/* Wall-tile window beyond one step: a tile just outside the step can still
 * clip the corner of a fast diagonal */
#define CLEARPATH_TILE_MARGIN           (4.0f)
/* A neighbour this far beyond touching, and this square ahead, is an encounter */
#define HUG_CONTACT_MARGIN              (2.0f)
#define HUG_AHEAD_DOT                   (0.3f)
#define HUG_WALL_MARGIN                 (1.0f)
/* G_Pos_Set forwards every position write here, including movement's own
 * per-tick ones; only a genuine teleport invalidates the jam counters. */
#define MOVE_TELEPORT_RESET_DIST        (8.0f)
/* A shuttle jams against its destination's edge, where the building takes up
 * half the contact ring; the crowd test is correspondingly lower. */
#define SURROUND_RELEASE_NEIGHBS        (3)
/* Terrain walls take contact-ring slots a unit could never fill; credit them
 * toward the crowd test so jams against walls can arm the drain. Blocker
 * stamps get none: a stamp is the output of walling, not evidence for it.
 */
#define SEEK_WALL_NEIGHB_CREDIT         (3)
/* A pin candidate only holds when the field's way round costs this much more
 * than the straight line; a few ranks behind a short front spread instead. */
#define SEEK_PIN_DETOUR                 (40.0f)
#define SEEK_PIN_DETOUR_HYST            (8.0f)
#define SEEK_PIN_WALK_MAX               (64)

/* Dense-crowd velocity-solve captures for offline bench replay */
#define CP_CAPTURE_MAX_PER_TICK         (32)
#define CP_CAPTURE_MIN_NEIGHBS          (24)

struct cp_capture{
    uint32_t      uid;
    struct cp_ent self;
    vec2_t        vpref;
    bool          gave_up;
    uint8_t       ndyn;
    uint8_t       nstat;
    struct cp_ent dyn[MAX_NEIGHBOURS];
    struct cp_ent stat[MAX_NEIGHBOURS];
};

/* Per-unit movement trace for offline crowd-behaviour analysis
 * (pf.debug.log_move_trace_min_radius). The nearest-neighbour query is wide
 * enough to see contact at any unit radius.
 */
#define MV_TRACE_NN_RADIUS              (64.0f)

enum move_trace_flags{
    MV_TRACE_HEADING_GATED   = (1 << 0),
    MV_TRACE_CP_GAVE_UP      = (1 << 1),
    MV_TRACE_CP_RETRY_OK     = (1 << 2),
    MV_TRACE_VETO_UNPATHABLE = (1 << 3),
    MV_TRACE_VETO_BLOCKED    = (1 << 4),
    MV_TRACE_SOFT_BLOCKING   = (1 << 5),
    MV_TRACE_SEEK_STUCK      = (1 << 6),
    MV_TRACE_SEEK_CLEAR      = (1 << 7),
    MV_TRACE_HAS_DEST_LOS    = (1 << 8),
    MV_TRACE_FIELD_STARVED   = (1 << 9),
    MV_TRACE_SURROUND_FIELD  = (1 << 10),
    MV_TRACE_COMBAT_HELD     = (1 << 11),
    MV_TRACE_CP_STALLED      = (1 << 12),
    MV_TRACE_HUGGING         = (1 << 13),
};

struct move_trace{
    bool     traced;
    vec2_t   vpref;
    vec2_t   sep;
    int      sep_n;
    int      retries;
    uint32_t nn_uid;
    vec2_t   nn_pos;
    float    nn_dist;
    float    nn_radius;
    bool     nn_seen;
    /* The nullify_impass_components probes, for a zero vpref with a live vdes:
     * bit 0 own tile blocked, then (pathable, blocked) pairs for +x, -x, +z, -z */
    bool     probed;
    uint16_t probe;
    struct tile_desc td;
    /* Raw flow dirs of the 3x3 tile block around the unit, row-major from -z,-x */
    int8_t   dirs[9];
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map       *s_map;
static bool                    s_attack_on_lclick = false;
static bool                    s_move_on_lclick = false;
static bool                    s_click_move_enabled = true;

static bool                    s_mouse_dragged = false;
static vec3_t                  s_drag_begin_pos;
static vec3_t                  s_drag_end_pos;
static bool                    s_drag_attacking;

static vec_entity_t            s_move_markers;
static vec_flock_t             s_flocks;
/* Per-tick uid -> flock index, rebuilt alongside the flock snapshots */
static khash_t(findex)        *s_flock_index;
/* uid -> identity aabb, persistent across ticks (the aabb never changes for a
 * model); referenced directly by the gamestate snapshot. Entries for removed
 * entities linger harmlessly until the periodic clear.
 */
static khash_t(aabb)          *s_aabb_cache;
static size_t                  s_fog_snap_ntiles;
static khash_t(state)         *s_entity_state_table;
static khash_t(auxstate)      *s_entity_aux_table;

/* Store the most recently issued move command location for debug rendering */
static bool                    s_last_cmd_dest_valid = false;
static dest_id_t               s_last_cmd_dest;

static struct move_work        s_move_work;
static queue_cmd_t             s_move_commands;
static struct memstack         s_eventargs;

/* pf.debug.log_cp_captures, hoisted once per tick for the worker phase */
static bool                    s_log_cp_captures;
static struct cp_capture       s_cp_captures[CP_CAPTURE_MAX_PER_TICK];
static SDL_atomic_t            s_cp_ncaptures;

/* pf.debug.log_move_trace_min_radius, hoisted once per tick; negative = off */
static float                   s_move_trace_min_radius = -1.0f;
/* Traced whatever their radius, so a session can be driven by the selection */
static const vec_entity_t     *s_move_trace_sel;
static unsigned                s_move_trace_tick;

static int                     s_soft_block_budget;
/* Monotone: a departed unit leaves the queries slightly wide, never narrow */
static float                   s_max_sel_radius;

static unsigned long           s_last_tick = 0;
static unsigned long           s_last_interpolate_tick = 0;

static enum movement_hz        s_move_hz = MOVE_HZ_20;
static struct refcounted_map  *s_nav_snapshot;
static bool                    s_move_hz_dirty = false;
static bool                    s_use_gpu = true;
static bool                    s_move_tick_queued = false;

/* Per-tick budget on full field rebuilds (n_request_path), consumed by the
 * serial LOS-build and path-request loops. Invalidation storms (war start,
 * mass settling) otherwise rebuild thousands of fields in a single tick,
 * blowing the tick budget many times over; capped, the storm amortises over
 * consecutive ticks. Starved units coast on their previous field for a tick
 * and are served first on the next one via the round-robin start index.
 */
static size_t                  s_rebuild_budget = 0;
static size_t                  s_rebuild_start = 0;
static size_t                  s_first_starved = (size_t)-1;

static uint32_t                s_tick_task_tid = NULL_TID;
/* Published by the navigation task onto itself at entry and cleared at exit, */
static uint32_t                s_nav_task_active_tid = NULL_TID;
static struct future           s_tick_task_future;

/* Per-tick nav stats. The fiber phases are published by the nav fiber, the
 * main-thread fields by move_do_tick; read by the main thread at the join
 * (valid once s_tick_task_future is ready).
 */
static struct nav_tick_sample  s_last_nav_tick_stats;

static const char *s_state_str[] = {
    [STATE_MOVING]              = STR(STATE_MOVING),
    [STATE_MOVING_IN_FORMATION] = STR(STATE_MOVING_IN_FORMATION),
    [STATE_ARRIVED]             = STR(STATE_ARRIVED),
    [STATE_SEEK_ENEMIES]        = STR(STATE_SEEK_ENEMIES),
    [STATE_WAITING]             = STR(STATE_WAITING),
    [STATE_SURROUND_ENTITY]     = STR(STATE_SURROUND_ENTITY),
    [STATE_ENTER_ENTITY_RANGE]  = STR(STATE_ENTER_ENTITY_RANGE),
    [STATE_TURNING]             = STR(STATE_TURNING),
    [STATE_ARRIVING_TO_CELL]    = STR(STATE_ARRIVING_TO_CELL),
    [STATE_FLEEING]             = STR(STATE_FLEEING)
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static uint32_t perf_ticks_to_us(uint64_t delta_ticks)
{
    uint64_t freq = SDL_GetPerformanceFrequency();
    return freq ? (uint32_t)(delta_ticks * 1000000ull / freq) : 0;
}

/* The returned pointer is guaranteed to be valid to write to for
 * so long as we don't add anything to the table. At that point, there
 * is a case that a 'realloc' might take place. */
static struct movestate *movestate_get(uint32_t uid)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return NULL;
    return &kh_value(s_entity_state_table, k);
}

static struct movestate_aux *movestate_aux_get(uint32_t uid)
{
    khiter_t k = kh_get(auxstate, s_entity_aux_table, uid);
    if(k == kh_end(s_entity_aux_table))
        return NULL;
    return &kh_value(s_entity_aux_table, k);
}

static void flock_try_remove(struct flock *flock, uint32_t uid)
{
    khiter_t k;
    if((k = kh_get(entity, flock->ents, uid)) != kh_end(flock->ents)) {
        kh_del(entity, flock->ents, k);
        G_Formation_RemoveUnit(uid);
    }
}

static void flock_add(struct flock *flock, uint32_t uid)
{
    int ret;
    khiter_t k = kh_put(entity, flock->ents, uid, &ret);
    assert(ret != -1 && ret != 0);
}

static bool flock_contains(const struct flock *flock, uint32_t uid)
{
    khiter_t k = kh_get(entity, flock->ents, uid);
    if(k != kh_end(flock->ents))
        return true;
    return false;
}

static struct flock *flock_for_ent(uint32_t uid)
{
    for(int i = 0; i < vec_size(&s_flocks); i++) {

        struct flock *curr_flock = &vec_AT(&s_flocks, i);            
        if(flock_contains(curr_flock, uid))
            return curr_flock;
    }
    return NULL;
}

uint32_t flock_id_for_ent(uint32_t uid, const struct flock **out)
{
    for(int i = 0; i < vec_size(&s_flocks); i++) {

        struct flock *curr_flock = &vec_AT(&s_flocks, i);            
        khiter_t k = kh_get(entity, curr_flock->ents, uid);
        if(k != kh_end(curr_flock->ents)) {
            *out = curr_flock;
            return (i + 1);
        }
    }
    *out = NULL;
    return 0;
}

static struct flock *flock_for_dest(dest_id_t id)
{
    for(int i = 0; i < vec_size(&s_flocks); i++) {

        struct flock *curr_flock = &vec_AT(&s_flocks, i);
        if(curr_flock->dest_id == id)
            return curr_flock;
    }
    return NULL;
}

static struct arrival_state *flock_arrival_for_ent(const struct flock *flock, uint32_t uid)
{
    float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    return G_ArrivalGroup_ForLayer(&flock->arrival, Entity_NavLayerWithRadius(flags, radius));
}

static void entity_block(uint32_t uid)
{
    float sel_radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
    vec2_t pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    M_NavBlockersIncref(pos, sel_radius, 
        G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid), flags, s_map);
    M_NavInvalidateZoneFieldsAt(s_move_work.gamestate.map, pos,
        Entity_NavLayerWithRadius(flags, sel_radius));

    struct movestate *ms = movestate_get(uid);
    assert(!ms->blocking);

    ms->blocking = true;
    ms->last_stop_pos = pos;
    ms->last_stop_radius = sel_radius;
    struct entity_block_desc *desc = stalloc(&s_eventargs, sizeof(struct entity_block_desc));
    *desc = (struct entity_block_desc){
        .uid = uid,
        .radius = sel_radius,
        .pos = pos
    };
    E_Global_Notify(EVENT_MOVABLE_ENTITY_BLOCK, desc, ES_ENGINE);
}

static void entity_unblock(uint32_t uid)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms->blocking);

    int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    M_NavBlockersDecref(ms->last_stop_pos, ms->last_stop_radius, faction_id, flags, s_map);
    M_NavInvalidateZoneFieldsAt(s_move_work.gamestate.map, ms->last_stop_pos,
        Entity_NavLayerWithRadius(flags, ms->last_stop_radius));
    ms->blocking = false;

    struct entity_block_desc *desc = stalloc(&s_eventargs, sizeof(struct entity_block_desc));
    *desc = (struct entity_block_desc){
        .uid = uid,
        .radius = ms->last_stop_radius,
        .pos = ms->last_stop_pos
    };
    E_Global_Notify(EVENT_MOVABLE_ENTITY_UNBLOCK, desc, ES_ENGINE);
}

static void entity_soft_block(uint32_t uid)
{
    struct movestate_aux *aux = movestate_aux_get(uid);
    assert(!aux->soft_blocking);

    float sel_radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
    vec2_t pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    M_NavBlockersIncref(pos, sel_radius,
        G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid), flags, s_map);

    aux->soft_blocking = true;
    aux->cp_stall_ticks = 0;
    aux->soft_block_pos = pos;
    aux->soft_block_radius = sel_radius;

    /* Let clients switch to idle; the held path announces its own end. */
    if(!(G_FlagsGet(uid) & ENTITY_FLAG_COMBAT_HELD))
        move_notify_motion_end(uid);
}

static void entity_soft_unblock(uint32_t uid)
{
    struct movestate_aux *aux = movestate_aux_get(uid);
    if(!aux || !aux->soft_blocking)
        return;

    int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    M_NavBlockersDecref(aux->soft_block_pos, aux->soft_block_radius, faction_id, flags, s_map);

    aux->soft_blocking = false;
    aux->seek_stuck_ticks = 0;
    aux->seek_clear_ticks = 0;
    aux->seek_progressed = false;

    /* Motion resumes unless settling or still combat-held. */
    struct movestate *ms = movestate_get(uid);
    if(ms && !ent_still(ms))
        move_notify_motion_start(uid, ms);
}

/* A new order or forced state change starts a fresh jam evaluation. */
static void entity_reset_seek_counters(uint32_t uid)
{
    entity_soft_unblock(uid);

    struct movestate_aux *aux = movestate_aux_get(uid);
    if(!aux)
        return;
    aux->seek_stuck_ticks = 0;
    aux->seek_clear_ticks = 0;
    aux->seek_progressed = false;
}

static bool stationary(uint32_t uid)
{
    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return true;

    if(ms->max_speed == 0.0f)
        return true;

    return false;
}

static bool entities_equal(uint32_t *a, uint32_t *b)
{
    return (*a == *b);
}

static void vec2_truncate(vec2_t *inout, float max_len)
{
    if(PFM_Vec2_Len(inout) > max_len) {

        PFM_Vec2_Normal(inout, inout);
        PFM_Vec2_Scale(inout, max_len, inout);
    }
}

static bool ent_still(const struct movestate *ms)
{
    return (ms->state == STATE_ARRIVED || ms->state == STATE_WAITING);
}

static float entity_speed(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    if(G_Formation_GetForEnt(uid) != NULL_FID) {
        return G_Formation_Speed(uid);
    }

    struct movestate *ms = movestate_get(uid);
    return ms->max_speed;
}

/* Callers emit these only at motion transitions, in matched start/end pairs. A start is
 * suppressed while COMBAT_HELD so a held unit keeps its idle/attack clip, not its walk clip.
 */
static void move_notify_motion_start(uint32_t uid, struct movestate *ms)
{
    /* Pivots and scripts rotate a still unit behind the shadow's back; motion
     * resumes from the rotation the unit is showing.
     */
    if(ent_still(ms))
        ms->prev_rot = ms->next_rot = Entity_GetRot(uid);

    if(G_FlagsGet(uid) & ENTITY_FLAG_COMBAT_HELD)
        return;
    struct movestate_aux *aux = movestate_aux_get(uid);
    if(!aux->motion_stopped)
        return;
    aux->motion_stopped = false;
    memset(aux->vel_hist, 0, sizeof(aux->vel_hist));
    E_Entity_Notify(EVENT_MOTION_START, uid, NULL, ES_ENGINE);
}

static void move_notify_motion_end(uint32_t uid)
{
    struct movestate_aux *aux = movestate_aux_get(uid);
    if(aux->motion_stopped)
        return;
    aux->motion_stopped = true;
    E_Entity_Notify(EVENT_MOTION_END, uid, NULL, ES_ENGINE);
}

/* A pause (soft block, combat hold) may already have announced an end; a stint
 * that truly finishes announces again so its watchers hear the arrival.
 */
static void move_notify_motion_finished(uint32_t uid)
{
    movestate_aux_get(uid)->motion_stopped = true;
    E_Entity_Notify(EVENT_MOTION_END, uid, NULL, ES_ENGINE);
}

static void entity_finish_moving(uint32_t uid, enum move_state newstate, bool block)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    assert(!ent_still(ms));
    uint32_t flags = G_FlagsGet(uid);

    if(!(flags & ENTITY_FLAG_COMBAT_HELD))
        move_notify_motion_finished(uid);
    if(flags & ENTITY_FLAG_COMBATABLE
    && (newstate != STATE_TURNING)) {
        G_Combat_SetStance(uid, COMBAT_STANCE_AGGRESSIVE);
    }

    if(newstate == STATE_WAITING) {
        struct movestate_aux *aux = movestate_aux_get(uid);
        aux->wait_prev = ms->state;
        aux->wait_ticks_left = WAIT_TICKS;
    }

    ms->state = newstate;
    ms->velocity = (vec2_t){0.0f, 0.0f};

    if(block) {
        entity_block(uid);
    }
    assert(ent_still(ms));
}

static void on_marker_anim_finish(void *user, void *event)
{
    ASSERT_IN_MAIN_THREAD();
    uint32_t ent = (uintptr_t)user;

    int idx = vec_entity_indexof(&s_move_markers, ent, entities_equal);
    assert(idx != -1);
    vec_entity_del(&s_move_markers, idx);

    E_Entity_Unregister(EVENT_ANIM_FINISHED, ent, on_marker_anim_finish);
    G_RemoveEntity(ent);
    G_FreeEntity(ent);
}

static void remove_from_flocks(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    /* Remove any flocks which may have become empty. Iterate vector in backwards order 
     * so that we can delete while iterating, since the last element in the vector takes
     * the place of the deleted one. 
     */
    for(int i = vec_size(&s_flocks)-1; i >= 0; i--) {

        struct flock *curr_flock = &vec_AT(&s_flocks, i);
        flock_try_remove(curr_flock, uid);

        if(kh_size(curr_flock->ents) == 0) {
            kh_destroy(entity, curr_flock->ents);
            G_ArrivalGroup_Destroy(&curr_flock->arrival);
            vec_flock_del(&s_flocks, i);
        }
    }
    assert(NULL == flock_for_ent(uid));
}

static void filter_selection_pathable(const vec_entity_t *in_sel, vec_entity_t *out_sel)
{
    ASSERT_IN_MAIN_THREAD();

    vec_entity_init(out_sel);
    for(int i = 0; i < vec_size(in_sel); i++) {

        uint32_t curr = vec_AT(in_sel, i);
        struct movestate *ms = movestate_get(curr);
        if(!ms)
            continue;

        vec2_t xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, curr);
        float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, curr);
        uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, curr);
        if(!M_NavPositionPathable(s_map, Entity_NavLayerWithRadius(flags, radius), xz_pos))
            continue;
        vec_entity_push(out_sel, curr);
    }
}

static void split_into_layers(const vec_entity_t *sel, vec_entity_t layer_flocks[])
{
    ASSERT_IN_MAIN_THREAD();

    for(int i = 0; i < NAV_LAYER_MAX; i++) {
        vec_entity_init(layer_flocks + i);
    }

    for(int i = 0; i < vec_size(sel); i++) {

        uint32_t curr = vec_AT(sel, i);
        float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, curr);
        uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, curr);
        enum nav_layer layer = Entity_NavLayerWithRadius(flags, radius);
        vec_entity_push(&layer_flocks[layer], curr);
    }
}

static bool make_flock(const vec_entity_t *units, vec2_t target_xz, 
                       enum nav_layer layer, bool attack, enum formation_type type)
{
    ASSERT_IN_MAIN_THREAD();

    if(vec_size(units) == 0)
        return true;

    bool ret;
    uint32_t first = vec_AT(units, 0);

    /* The following won't be optimal when the entities in the unitsection are on different 
     * 'islands'. Handling that case is not a top priority. 
     */
    vec2_t first_ent_pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, first);
    target_xz = M_NavClosestReachableDest(s_map, layer, first_ent_pos_xz, target_xz);

    /* First remove the entities in the unitsection from any active flocks */
    for(int i = 0; i < vec_size(units); i++) {

        uint32_t curr_ent = vec_AT(units, i);
        remove_from_flocks(curr_ent);
    }

    struct flock new_flock = (struct flock) {
        .ents = kh_init(entity),
        .target_xz = target_xz,
    };

    if(!new_flock.ents)
        return false;

    for(int i = 0; i < vec_size(units); i++) {

        uint32_t curr_ent = vec_AT(units, i);
        if(stationary(curr_ent))
            continue;

        struct movestate *ms = movestate_get(curr_ent);
        assert(ms);

        entity_reset_seek_counters(curr_ent);
        if(ent_still(ms)) {
            entity_unblock(curr_ent); 
            move_notify_motion_start(curr_ent, ms);
        }

        flock_add(&new_flock, curr_ent);
        ms->state = (type == FORMATION_NONE) ? STATE_MOVING : STATE_MOVING_IN_FORMATION;
        G_Arrival_InitUnit(&movestate_aux_get(curr_ent)->arrival,
            G_Pos_GetXZFrom(s_move_work.gamestate.positions, curr_ent));
    }

    /* The flow fields will be computed on-demand during the next movement update tick */
    new_flock.target_xz = target_xz;
    if(attack) {
        int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, first);
        new_flock.dest_id = M_NavDestIDForPosAttacking(s_map, target_xz, layer, faction_id);
    }else{
        new_flock.dest_id = M_NavDestIDForPos(s_map, target_xz, layer);
    }

    if(kh_size(new_flock.ents) == 0) {
        kh_destroy(entity, new_flock.ents);
        return false;
    }

    /* If there is another flock with the same dest_id, then we merge the two flocks. */
    struct flock *merge_flock = flock_for_dest(new_flock.dest_id);
    if(merge_flock) {

        uint32_t curr;
        kh_foreach_key(new_flock.ents, curr, { flock_add(merge_flock, curr); });
        kh_destroy(entity, new_flock.ents);
        G_ArrivalGroup_Reset(&merge_flock->arrival);

    }else{
        formation_id_t fid;
        int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, first);
        vec_flock_push(&s_flocks, new_flock);
    }

    s_last_cmd_dest_valid = true;
    s_last_cmd_dest = new_flock.dest_id;
    return true;
}

/* Build a plain-data snapshot of the flock's live members (existing entities only) for the
 * arrival module, which never touches the movement gamestate itself. Returns the count. */
static int build_arrival_members(const struct flock *flock, struct arrival_member *out, int max)
{
    int n = 0;
    uint32_t uid;
    kh_foreach_key(flock->ents, uid, {
        if(n >= max)
            break;
        if(!G_EntityExists(uid))
            continue;
        struct movestate *ms = movestate_get(uid);
        if(!ms)
            continue;
        float radius = G_GetSelectionRadius(uid);
        out[n].pos = G_Pos_GetXZ(uid);
        out[n].settled = (ms->state == STATE_ARRIVED);
        out[n].radius = radius;
        out[n].layer = Entity_NavLayerWithRadius(G_FlagsGet(uid), radius);
        out[n].us = &movestate_aux_get(uid)->arrival;
        n++;
    });
    return n;
}

static void update_flock_arrival_fields(void)
{
    ASSERT_IN_MAIN_THREAD();
    for(int i = 0; i < vec_size(&s_flocks); i++) {
        struct flock *flock = &vec_AT(&s_flocks, i);

        /* The arrival logic is for the bare-movement 'flock' case only; units moving in a
         * formation arrive via the formation's own cell fields. */
        uint32_t first = NULL_UID;
        kh_foreach_key(flock->ents, first, { break; });
        if(first == NULL_UID || G_Formation_GetForEnt(first) != NULL_FID) {
            G_ArrivalGroup_Deactivate(&flock->arrival);
            continue;
        }

        STALLOC(struct arrival_member, members, kh_size(flock->ents));
        int n = build_arrival_members(flock, members, kh_size(flock->ents));
        G_ArrivalGroup_Update(&flock->arrival, s_map, flock->target_xz, members, n);
        STFREE(members);
    }
}

static void request_flock_arrival_fields(void)
{
    for(int i = 0; i < vec_size(&s_flocks); i++) {
        const struct flock *flock = &vec_AT(&s_flocks, i);
        G_ArrivalGroup_RequestFields(&flock->arrival, s_move_work.gamestate.map);
    }
}

static void make_flocks(const vec_entity_t *sel, vec2_t target_xz, vec2_t target_orientation,
                        enum formation_type type, bool attack)
{
    ASSERT_IN_MAIN_THREAD();

    vec_entity_t fsel;
    filter_selection_pathable(sel, &fsel);

    if(vec_size(&fsel) == 0)
        return;

    if(type != FORMATION_NONE && vec_size(&fsel) > MAX_FORMATION_UNITS) {
        G_Sel_RemoveMany(&vec_AT(&fsel, MAX_FORMATION_UNITS),
            vec_size(&fsel) - MAX_FORMATION_UNITS);
        fsel.size = MAX_FORMATION_UNITS;
    }

    vec_entity_t layer_flocks[NAV_LAYER_MAX];
    split_into_layers(&fsel, layer_flocks);

    for(int i = 0; i < NAV_LAYER_MAX; i++) {
        make_flock(layer_flocks + i, target_xz, i, attack, type);
        vec_entity_destroy(layer_flocks + i);
    }

    G_Formation_Create(target_xz, target_orientation, &fsel, type);
    vec_entity_destroy(&fsel);
}

/* Bounded spatial query rather than a flock-set walk: a 10,000-strong flock
 * makes the per-member walk quadratic across the update phase.
 */
static size_t adjacent_flock_members(uint32_t uid, const struct flock *flock, 
                                     uint32_t out[], size_t maxout)
{
    vec2_t ent_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    float radius_uid = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
    float search_radius = MAX(SEPARATION_NEIGHB_RADIUS,
        2.0f * radius_uid + ADJACENCY_SEP_DIST);

    uint32_t near_ents[128];
    int num_near = G_Pos_EntsInCircleFrom(s_move_work.gamestate.postree,
        s_move_work.gamestate.flags, ent_xz_pos, search_radius, near_ents,
        ARR_SIZE(near_ents));

    size_t ret = 0;
    for(int i = 0; i < num_near && ret < maxout; i++) {

        uint32_t curr = near_ents[i];
        if(curr == uid)
            continue;
        khiter_t k = kh_get(entity, flock->ents, curr);
        if(k == kh_end(flock->ents))
            continue;

        vec2_t diff;
        vec2_t curr_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, curr);
        PFM_Vec2_Sub(&ent_xz_pos, &curr_xz_pos, &diff);
        float radius_curr = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, curr);

        if(PFM_Vec2_Len(&diff) <= radius_uid + radius_curr + ADJACENCY_SEP_DIST) {
            out[ret++] = curr;  
        }
    }
    return ret;
}

/* Count the settled (arrived) flock members touching 'uid'. A unit boxed in by a
 * couple of settled neighbours can "just stop".
 */
static int adjacent_settled_count(uint32_t uid)
{
    /* Cross-flock: count any settled neighbour, not only same-flock members. 
     * A unit boxed in by another layer's settled ball (a separate flock) has 
     * none of its own to touch and would otherwise never satisfy the settle cascade. 
     */
    vec2_t pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    float radius_uid = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
    uint32_t ent_flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);

    /* The acceptance test scales its cutoff with both radii, so the query radius must too,
     * else two large units never register as adjacent and the settle cascade starves.
     */
    float search_radius = MAX(SEPARATION_NEIGHB_RADIUS, 2.0f * radius_uid + ADJACENCY_SEP_DIST);

    uint32_t near_ents[128];
    int num_near = G_Pos_EntsInCircleFrom(s_move_work.gamestate.postree,
        s_move_work.gamestate.flags, pos, search_radius, near_ents, ARR_SIZE(near_ents));

    int count = 0;
    for(int i = 0; i < num_near; i++) {
        uint32_t curr = near_ents[i];
        if(curr == uid)
            continue;
        uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, curr);
        if(!(flags & ENTITY_FLAG_MOVABLE))
            continue;
        if((ent_flags & ENTITY_FLAG_AIR) != (flags & ENTITY_FLAG_AIR))
            continue;
        const struct movestate *ams = movestate_get(curr);
        if(!ams || ams->state != STATE_ARRIVED)
            continue;
        vec2_t cpos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, curr);
        float radius_curr = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, curr);
        vec2_t diff;
        PFM_Vec2_Sub(&pos, &cpos, &diff);
        if(PFM_Vec2_Len(&diff) <= radius_uid + radius_curr + ADJACENCY_SEP_DIST)
            count++;
    }
    return count;
}

static void move_marker_add(vec3_t pos, bool attack)
{
    uint32_t flags;
    const uint32_t uid = Entity_NewUID();
    bool loaded = attack 
                ? AL_EntityFromPFObj("assets/models/arrow", "arrow-red.pfobj", 
                                     "__move_marker__", uid, &flags) 
                : AL_EntityFromPFObj("assets/models/arrow", "arrow-green.pfobj", 
                                     "__move_marker__", uid, &flags);
    if(!loaded)
        return;

    flags |= ENTITY_FLAG_MARKER;
    G_AddEntity(uid, flags, pos);

    Entity_SetScale(uid, (vec3_t){2.0f, 2.0f, 2.0f});
    E_Entity_Register(EVENT_ANIM_FINISHED, uid, on_marker_anim_finish, 
        (void*)(uintptr_t)uid, G_RUNNING);
    A_SetActiveClip(uid, "Converge", ANIM_MODE_ONCE, 48);

    vec_entity_push(&s_move_markers, uid);
}

static void move_order(const vec_entity_t *sel, bool attack, vec3_t mouse_coord,
                       vec2_t orientation)
{
    enum formation_type type = G_Formation_PreferredForSet(sel);

    /* Cap formation orders before any per-unit processing: the excess
     * units are deselected and must not be stopped or notified. The
     * capped copy also keeps the iteration safe from the deselection
     * mutating the live selection vector. The deselection is batched,
     * as G_Sel_Remove fires a selection-changed event for every
     * removed unit.
     */
    vec_entity_t capped;
    vec_entity_init(&capped);
    vec_entity_copy(&capped, (vec_entity_t*)sel);
    if(type != FORMATION_NONE && vec_size(&capped) > MAX_FORMATION_UNITS) {
        G_Sel_RemoveMany(&vec_AT(&capped, MAX_FORMATION_UNITS),
            vec_size(&capped) - MAX_FORMATION_UNITS);
        capped.size = MAX_FORMATION_UNITS;
    }

    size_t nmoved = 0;
    for(int i = 0; i < vec_size(&capped); i++) {

        uint32_t curr = vec_AT(&capped, i);
        uint32_t flags = G_FlagsGet(curr);
        if(!(flags & ENTITY_FLAG_MOVABLE))
            continue;

        G_StopEntity(curr, false, true);
        E_Entity_Notify(EVENT_MOVE_ISSUED, curr, NULL, ES_ENGINE);
        G_NotifyOrderIssued(curr, true);
        nmoved++;

        if(flags & ENTITY_FLAG_COMBATABLE) {
            G_Combat_SetStance(curr, 
                attack ? COMBAT_STANCE_AGGRESSIVE : COMBAT_STANCE_NO_ENGAGEMENT);
        }
    }

    if(nmoved) {
        move_marker_add(mouse_coord, attack);
        vec_entity_t *copy = PF_MALLOC(sizeof(vec_entity_t));
        vec_entity_init(copy);
        vec_entity_copy(copy, &capped);
        move_push_cmd((struct move_cmd){
            .type = MOVE_CMD_MAKE_FLOCKS,
            .u.make_flocks = {
                .sel = copy,
                .target_xz = (vec2_t){mouse_coord.x, mouse_coord.z},
                .type = type,
                .attack = attack,
                .target_orientation = orientation
            }
        });
    }
    vec_entity_destroy(&capped);
}

static void on_mousedown(void *user, void *event)
{
    SDL_MouseButtonEvent *mouse_event = &(((SDL_Event*)event)->button);

    bool targeting = G_Move_InTargetMode();
    bool attack = s_attack_on_lclick && (mouse_event->button == SDL_BUTTON_LEFT);
    bool move = s_move_on_lclick ? mouse_event->button == SDL_BUTTON_LEFT
                                 : mouse_event->button == SDL_BUTTON_RIGHT;

    assert(!s_move_on_lclick || !s_attack_on_lclick);
    assert(!attack || !move);

    s_attack_on_lclick = false;
    s_move_on_lclick = false;

    if(!s_click_move_enabled)
        return;

    if(S_UI_MouseOverWindow(mouse_event->x, mouse_event->y))
        return;

    if((mouse_event->button == SDL_BUTTON_RIGHT) && targeting)
        return;

    if(!attack && !move)
        return;

    if(G_CurrContextualAction() != CTX_ACTION_NONE)
        return;

    if(G_MouseInTargetMode() && !targeting)
        return;

    vec3_t mouse_coord;
    if(!M_MinimapMouseMapCoords(s_map, &mouse_coord)
    && !M_Raycast_MouseIntersecCoord(&mouse_coord))
        return;

    enum selection_type sel_type;
    const vec_entity_t *sel = G_Sel_Get(&sel_type);

    vec_entity_t fsel;
    filter_selection_pathable(sel, &fsel);

    if(vec_size(&fsel) == 0 || sel_type != SELECTION_TYPE_PLAYER)
        return;

    /* Allow dragging the mouse to orient the formation around 
     * the clicked location. The move orders will be issued when
     * the mouse is released. 
     */
    if(G_Formation_PreferredForSet(&fsel) != FORMATION_NONE) {
        s_mouse_dragged = true;
        s_drag_begin_pos = mouse_coord;
        s_drag_end_pos = mouse_coord;
        s_drag_attacking = attack;
        return;
    }

    move_order(&fsel, attack, mouse_coord, (vec2_t){0.0f, 0.0f});
    vec_entity_destroy(&fsel);
}

static void on_mouseup(void *user, void *event)
{
    if(!s_mouse_dragged)
        return;
    s_mouse_dragged = false;

    enum selection_type seltype;
    const vec_entity_t *sel = G_Sel_Get(&seltype);

    vec2_t endpoints[] = {
        (vec2_t){s_drag_begin_pos.x, s_drag_begin_pos.z},
        (vec2_t){s_drag_end_pos.x, s_drag_end_pos.z}
    };

    vec2_t orientation;
    PFM_Vec2_Sub(&endpoints[1], &endpoints[0], &orientation);
    if(PFM_Vec2_Len(&orientation) < 0.1f) {
        orientation = G_Formation_AutoOrientation(endpoints[0], sel);
    }else{
        PFM_Vec2_Normal(&orientation, &orientation);
    }
    move_order(sel, s_drag_attacking, s_drag_begin_pos, orientation);
}

static void on_mousemotion(void *user, void *event)
{
    if(!s_mouse_dragged)
        return;

    vec3_t mouse_coord = (vec3_t){0.0f, 0.0f, 0.0f};
    if(!M_Raycast_MouseIntersecCoord(&mouse_coord))
        return;

    s_drag_end_pos = mouse_coord;
}

static void render_formation_orientation(void)
{
    vec2_t endpoints[] = {
        (vec2_t){s_drag_begin_pos.x, s_drag_begin_pos.z},
        (vec2_t){s_drag_end_pos.x, s_drag_end_pos.z}
    };

    vec2_t delta;
    PFM_Vec2_Sub(&endpoints[1], &endpoints[0], &delta);
    if(PFM_Vec2_Len(&delta) > EPSILON) {
        PFM_Vec2_Normal(&delta, &delta);
    }

    float width = 1.0f;
    vec3_t green = (vec3_t){140.0f / 255.0f, 240.0f / 255.0f, 140.0f / 255.0f};
    vec3_t red = (vec3_t){230.0f / 255.0f, 64.0f / 255.0f, 85.0f / 255.0f};

    R_PushCmd((struct rcmd){
        .func = R_GL_DrawLine,
        .nargs = 4,
        .args = {
            R_PushArg(endpoints, sizeof(endpoints)),
            R_PushArg(&width, sizeof(width)),
            R_PushArg(s_drag_attacking ? &red : &green, sizeof(vec3_t)),
            (void*)G_GetPrevTickMap()
        }
    });
    enum selection_type seltype;
    const vec_entity_t *sel = G_Sel_Get(&seltype);
    
    G_Formation_RenderPlacement(sel, endpoints[0], delta);
}

static bool move_nav_field_overlay_enabled(void)
{
    static const char *const keys[] = {
        "pf.debug.show_last_cmd_flow_field",
        "pf.debug.show_first_sel_movestate",
        "pf.debug.show_enemy_seek_fields",
        "pf.debug.show_group_arrival_field",
    };
    for(int i = 0; i < ARR_SIZE(keys); i++) {
        struct sval setting;
        if(Settings_Get(keys[i], &setting) == SS_OKAY && setting.as_bool)
            return true;
    }
    return false;
}

static void on_render_3d(void *user, void *event)
{
    if(s_mouse_dragged) {
        render_formation_orientation();
    }

    /* The field-visualisation overlays below read the task-owned field cache on
     * the main thread; drain the nav task so it is quiescent. Skip those
     * overlays if it cannot be drained this frame (GPU work in flight).
     */
    bool cache_ok = !move_nav_field_overlay_enabled() || G_Move_NavQuiesce();

    const struct camera *cam = G_GetActiveCamera();
    enum nav_layer layer;

    struct sval setting;
    ss_e status;
    (void)status;

    status = Settings_Get("pf.debug.navigation_layer", &setting);
    assert(status == SS_OKAY);
    layer = setting.as_int;

    status = Settings_Get("pf.debug.show_last_cmd_flow_field", &setting);
    assert(status == SS_OKAY);
    if(setting.as_bool && s_last_cmd_dest_valid && cache_ok) {
        M_NavRenderVisiblePathFlowField(s_map, cam, s_last_cmd_dest);
    }

    status = Settings_Get("pf.debug.show_first_sel_movestate", &setting);
    assert(status == SS_OKAY);

    enum selection_type seltype;
    const vec_entity_t *sel = G_Sel_Get(&seltype);

    if(setting.as_bool && vec_size(sel) > 0 && cache_ok) {

        uint32_t ent = vec_AT(sel, 0);
        struct movestate *ms = movestate_get(ent);
        if(ms) {

            char strbuff[256];
            pf_snprintf(strbuff, ARR_SIZE(strbuff), "Arrival State: %s Velocity: (%f, %f)", 
                s_state_str[ms->state], ms->velocity.x, ms->velocity.z);
            struct rgba text_color = (struct rgba){255, 0, 0, 255};
            UI_DrawText(strbuff, (struct rect){5,50,600,50}, text_color);

            const struct camera *cam = G_GetActiveCamera();
            struct flock *flock = flock_for_ent(ent);

            switch(ms->state) {
            case STATE_MOVING:
            case STATE_MOVING_IN_FORMATION:
            case STATE_ENTER_ENTITY_RANGE:
                assert(flock);
                M_NavRenderVisiblePathFlowField(s_map, cam, flock->dest_id);
                break;
            case STATE_SURROUND_ENTITY: {

                if(!G_EntityExists(ms->surround_target_uid))
                    break;

                if(ms->using_surround_field) {
                    float radius = G_GetSelectionRadiusFrom(
                        s_move_work.gamestate.sel_radiuses, ent);
                    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, ent);
                    int layer = Entity_NavLayerWithRadius(flags, radius); 
                    M_NavRenderVisibleSurroundField(s_map, cam, layer, ms->surround_target_uid);
                    UI_DrawText("(Surround Field)", (struct rect){5,75,600,50}, text_color);
                }else{
                    M_NavRenderVisiblePathFlowField(s_map, cam, flock->dest_id);
                    UI_DrawText("(Path Field)", (struct rect){5,75,600,50}, text_color);
                }
                break;
            }
            case STATE_ARRIVED:
            case STATE_WAITING:
            case STATE_TURNING:
            case STATE_FLEEING:
                break;
            case STATE_SEEK_ENEMIES: {
                float radius = G_GetSelectionRadiusFrom(
                    s_move_work.gamestate.sel_radiuses, ent);
                uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, ent);
                int layer = Entity_NavLayerWithRadius(flags, radius); 
                int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, ent);
                M_NavRenderVisibleEnemySeekField(s_map, cam, layer, faction_id);
                break;
            }
            case STATE_ARRIVING_TO_CELL:
                /* Following the cell arrival field */
                break;
            default: 
                assert(0);
            }
        }
    }

    status = Settings_Get("pf.debug.show_enemy_seek_fields", &setting);
    assert(status == SS_OKAY);
    if(setting.as_bool && cache_ok) {

        status = Settings_Get("pf.debug.enemy_seek_fields_faction_id", &setting);
        assert(status == SS_OKAY);
    
        M_NavRenderVisibleEnemySeekField(s_map, cam, layer, setting.as_int);
    }

    status = Settings_Get("pf.debug.show_navigation_blockers", &setting);
    assert(status == SS_OKAY);
    if(setting.as_bool) {
        M_NavRenderNavigationBlockers(s_map, cam, layer);
    }

    status = Settings_Get("pf.debug.show_navigation_portals", &setting);
    assert(status == SS_OKAY);
    if(setting.as_bool) {
        M_NavRenderNavigationPortals(s_map, cam, layer);
    }

    status = Settings_Get("pf.debug.show_navigation_cost_base", &setting);
    assert(status == SS_OKAY);
    if(setting.as_bool) {
        M_RenderVisiblePathableLayer(s_map, cam, layer);
    }

    status = Settings_Get("pf.debug.show_chunk_boundaries", &setting);
    assert(status == SS_OKAY);
    if(setting.as_bool) {
        M_RenderChunkBoundaries(s_map, cam);
    }

    status = Settings_Get("pf.debug.show_navigation_island_ids", &setting);
    assert(status == SS_OKAY);
    if(setting.as_bool) {
        M_NavRenderNavigationIslandIDs(s_map, cam, layer);
    }

    status = Settings_Get("pf.debug.show_navigation_local_island_ids", &setting);
    assert(status == SS_OKAY);
    if(setting.as_bool) {
        M_NavRenderNavigationLocalIslandIDs(s_map, cam, layer);
    }

    status = Settings_Get("pf.debug.show_group_arrival_field", &setting);
    assert(status == SS_OKAY);
    if(setting.as_bool && cache_ok) {
        static const vec3_t dbg_flock_pal[] = {   /* DEBUG: one colour per flock so layers separate */
            {0.1f, 0.3f, 1.0f}, {1.0f, 0.5f, 0.0f}, {0.1f, 0.9f, 0.3f},
            {1.0f, 1.0f, 0.0f}, {0.7f, 0.0f, 1.0f},
        };
        for(int i = 0; i < vec_size(&s_flocks); i++) {
            const struct flock *flock = &vec_AT(&s_flocks, i);
            if(!G_ArrivalGroup_IsActive(&flock->arrival))
                continue;
            STALLOC(struct arrival_member, members, kh_size(flock->ents));
            int n = build_arrival_members(flock, members, kh_size(flock->ents));
            G_ArrivalGroup_RenderDebug(&flock->arrival, cam, s_map, flock->dest_id,
                dbg_flock_pal[i % ARR_SIZE(dbg_flock_pal)], members, n);
            STFREE(members);
        }
    }
}

static quat_t dir_quat_from_velocity(vec2_t velocity)
{
    assert(PFM_Vec2_Len(&velocity) > EPSILON);

    float angle_rad = atan2(velocity.raw[1], velocity.raw[0]) - M_PI/2.0f;
    return (quat_t) {
        0.0f, 
        1.0f * sin(angle_rad / 2.0f),
        0.0f,
        cos(angle_rad / 2.0f)
    };
}

static bool entity_exists(uint32_t uid)
{
    khiter_t k = kh_get(pos, s_move_work.gamestate.positions, uid);
    return (k != kh_end(s_move_work.gamestate.positions));
}

static void request_async_field(uint32_t uid)
{
    struct movestate *ms = movestate_get(uid);
    if(!ms || ent_still(ms))
        return;

    vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);

    switch(ms->state) {
    case STATE_SEEK_ENEMIES:  {
        float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
        uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
        int layer = Entity_NavLayerWithRadius(flags, radius);
        int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
        return M_NavRequestAsyncEnemySeekField(s_move_work.gamestate.map, 
            layer, pos_xz, faction_id);
    }
    case STATE_SURROUND_ENTITY: {

        if(!entity_exists(ms->surround_target_uid))
            return;

        if(ms->using_surround_field) {
            float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
            uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
            int layer = Entity_NavLayerWithRadius(flags, radius);
            int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
            return M_NavRequestAsyncSurroundField(s_move_work.gamestate.map, layer, pos_xz, 
                ms->surround_target_uid, faction_id);
        }
        break;
    }
    default:;
        /* No-op */
    }
}

static struct target build_target(uint32_t uid, const struct flock *fl)
{
    const struct movestate *ms = movestate_get(uid);
    float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    int layer = Entity_NavLayerWithRadius(flags, radius);
    int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);

    if(ms->state == STATE_SEEK_ENEMIES) {
        return (struct target){.kind = TARGET_KIND_ENEMY_SEEK,
                               .enemy_seek = {.layer = layer, .faction_id = faction_id}};
    }
    if(ms->state == STATE_SURROUND_ENTITY
    && entity_exists(ms->surround_target_uid) && ms->using_surround_field) {
        return (struct target){.kind = TARGET_KIND_SURROUND,
                               .surround = {.layer = layer, .faction_id = faction_id,
                                            .uid = ms->surround_target_uid}};
    }
    assert(fl);
    return (struct target){.kind = TARGET_KIND_POINT_SEEK,
                           .point_seek = {.dest_id = fl->dest_id, .dest_xz = fl->target_xz}};
}

/* The way round costs enough more than the straight line that waiting in line
 * beats walking it. Sticky on a tile without flow (a fresh blocker before the
 * service patch).
 */
static bool seek_pin_detoured(uint32_t uid, vec2_t pos_xz, vec2_t target_xz,
                              float straight, bool held)
{
    vec2_t cell = N_TileDims();
    int max_steps = MIN((int)ceilf((straight + SEEK_PIN_DETOUR) / cell.x) + 4,
                        SEEK_PIN_WALK_MAX);

    float len;
    vec2_t end;
    bool capped;
    if(!M_NavFlowFieldPathLength(s_move_work.gamestate.map, build_target(uid, NULL),
        pos_xz, max_steps, &len, &end, &capped))
        return held;
    if(capped)
        return true;

    vec2_t rest;
    PFM_Vec2_Sub(&target_xz, &end, &rest);
    float excess = len + PFM_Vec2_Len(&rest) - straight;
    return excess > (held ? SEEK_PIN_DETOUR - SEEK_PIN_DETOUR_HYST : SEEK_PIN_DETOUR);
}

/* A pinned seeker waits in line: it presses straight at its target while the
 * way is open and stands once something is in the way, so the ranks behind
 * the front jam up and wall rather than slide along it.
 */
static bool seek_pin_vdes(uint32_t uid, vec2_t pos_xz, vec2_t *out, bool *out_held)
{
    struct movestate_aux *aux = movestate_aux_get(uid);
    uint32_t pin = aux->seek_pin_target;
    if(pin == NULL_UID || !entity_exists(pin)
    || (G_FlagsGetFrom(s_move_work.gamestate.flags, pin) & ENTITY_FLAG_ZOMBIE))
        return false;

    vec2_t target_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, pin);
    vec2_t dir;
    PFM_Vec2_Sub(&target_xz, &pos_xz, &dir);
    float straight = PFM_Vec2_Len(&dir);
    if(straight < EPSILON)
        return false;
    PFM_Vec2_Normal(&dir, &dir);

    if(!seek_pin_detoured(uid, pos_xz, target_xz, straight, aux->seek_pin_held))
        return false;
    *out_held = true;

    const struct map *map = s_move_work.gamestate.map;
    float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    enum nav_layer layer = Entity_NavLayerWithRadius(flags, radius);

    vec2_t probe = (vec2_t){pos_xz.x + dir.x * radius, pos_xz.z + dir.z * radius};
    bool open = M_NavPositionPathable(map, layer, probe)
             && (M_NavPositionBlocked(map, layer, pos_xz)
              || !M_NavPositionBlocked(map, layer, probe));

    *out = open ? dir : (vec2_t){0.0f, 0.0f};
    return true;
}

static vec2_t ent_desired_velocity(struct move_work_in *in)
{
    uint32_t uid = in->ent_uid;
    struct flock *fl = in->flock;
    vec2_t cell_arrival_vdes = in->cell_arrival_vdes;
    bool has_dest_los = in->has_dest_los;
    bool *out_seek_pinned = &in->seek_pinned;

    const struct movestate *ms = movestate_get(uid);
    vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    const struct map *map = s_move_work.gamestate.map;
    *out_seek_pinned = false;

    switch(ms->state) {
    case STATE_TURNING:
        return (vec2_t){0.0f, 0.0f};

    case STATE_ARRIVING_TO_CELL:
        return cell_arrival_vdes;

    case STATE_SEEK_ENEMIES: {
        vec2_t pinned;
        if(seek_pin_vdes(uid, pos_xz, &pinned, out_seek_pinned))
            return pinned;
        return M_NavDesiredVelocityForTargetCached(map, build_target(uid, fl), pos_xz);
    }
    case STATE_SURROUND_ENTITY:
        return M_NavDesiredVelocityForTargetCached(map, build_target(uid, fl), pos_xz);

    case STATE_FLEEING: {
        vec2_t dir;
        PFM_Vec2_Sub(&pos_xz, &movestate_aux_get(uid)->flee_threat_pos, &dir);
        if(PFM_Vec2_Len(&dir) < EPSILON)
            return (vec2_t){0.0f, 0.0f};
        PFM_Vec2_Normal(&dir, &dir);
        return dir;
    }

    default: {
        assert(fl);
        struct movestate *mms = movestate_get(uid);
        struct arrival_state *as = flock_arrival_for_ent(fl, uid);
        vec2_t arrival_vel;
        if(as && G_Arrival_DesiredVelocity(as, &movestate_aux_get(uid)->arrival, s_map,
            map, pos_xz, mms->velocity, has_dest_los, &arrival_vel))
            return arrival_vel;

        vec2_t field_vdes = M_NavDesiredVelocityForTargetCached(map, build_target(uid, fl), pos_xz);
        if(PFM_Vec2_Len(&field_vdes) > EPSILON)
            return field_vdes;

        /* A void sample on free, pathable ground is a stale hole rather than
         * an unreachable destination; hold a heading at the goal to cross it.
         */
        float vradius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
        uint32_t vflags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
        enum nav_layer vlayer = Entity_NavLayerWithRadius(vflags, vradius);
        struct movestate_aux *vaux = movestate_aux_get(uid);
        if(vaux->field_void_ticks >= FIELD_VOID_COAST_TICKS
        || M_NavPositionBlocked(map, vlayer, pos_xz)
        || !M_NavPositionPathable(map, vlayer, pos_xz))
            return field_vdes;

        vec2_t togoal;
        PFM_Vec2_Sub((vec2_t*)&fl->target_xz, &pos_xz, &togoal);
        if(PFM_Vec2_Len(&togoal) < EPSILON)
            return field_vdes;
        PFM_Vec2_Normal(&togoal, &togoal);
        in->field_void = true;
        return togoal;
    }
    }
}

/* Seek behaviour makes the entity target and approach a particular destination point.
 */
static vec2_t seek_force(uint32_t uid, vec2_t target_xz)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    vec2_t ret, desired_velocity;
    vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);

    PFM_Vec2_Sub(&target_xz, &pos_xz, &desired_velocity);
    if(PFM_Vec2_Len(&desired_velocity) >= EPSILON) {
        PFM_Vec2_Normal(&desired_velocity, &desired_velocity);
    }
    PFM_Vec2_Scale(&desired_velocity, ms->max_speed / hz_count(s_move_work.hz), &desired_velocity);

    PFM_Vec2_Sub(&desired_velocity, &ms->velocity, &ret);
    return ret;
}

/* Arrival behaviour is like 'seek' but the entity decelerates and comes to a halt when it is 
 * within a threshold radius of the destination point.
 * 
 * When not within line of sight of the destination, this will steer the entity along the 
 * flow field.
 */
static vec2_t arrive_force_point(uint32_t uid, vec2_t target_xz, vec2_t vdes, bool has_dest_los)
{
    vec2_t ret, desired_velocity;
    vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    float distance;

    struct movestate *ms = movestate_get(uid);
    assert(ms);

    if(has_dest_los) {

        PFM_Vec2_Sub(&target_xz, &pos_xz, &desired_velocity);
        distance = PFM_Vec2_Len(&desired_velocity);
        /* Standing on the goal point; normalising the zero offset would NaN. */
        if(distance >= EPSILON) {
            PFM_Vec2_Normal(&desired_velocity, &desired_velocity);
        }
        PFM_Vec2_Scale(&desired_velocity, ms->max_speed / hz_count(s_move_work.hz), 
            &desired_velocity);

        if(distance < ARRIVE_SLOWING_RADIUS) {
            PFM_Vec2_Scale(&desired_velocity, distance / ARRIVE_SLOWING_RADIUS, &desired_velocity);
        }
    }else{
        PFM_Vec2_Scale(&vdes, ms->max_speed / hz_count(s_move_work.hz), &desired_velocity);
    }

    PFM_Vec2_Sub(&desired_velocity, &ms->velocity, &ret);
    vec2_truncate(&ret, SCALED_MAX_FORCE);
    return ret;
}

static vec2_t arrive_force_cell(uint32_t uid, vec2_t cell_xz, vec2_t vdes)
{
    struct movestate *ms = movestate_get(uid);
    vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    float distance;

    vec2_t desired_velocity;
    PFM_Vec2_Sub(&cell_xz, &pos_xz, &desired_velocity);
    distance = PFM_Vec2_Len(&desired_velocity);

    if(distance < ARRIVE_SLOWING_RADIUS) {
        PFM_Vec2_Scale(&desired_velocity, distance / ARRIVE_SLOWING_RADIUS, &desired_velocity);
    }else{
        PFM_Vec2_Scale(&vdes, ms->max_speed / hz_count(s_move_work.hz), &desired_velocity);
    }
    return desired_velocity;
}

static vec2_t arrive_force_enemies(uint32_t uid, vec2_t vdes)
{
    vec2_t ret, desired_velocity;
    vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    float distance;

    const struct movestate *ms = movestate_get(uid);
    assert(ms);

    PFM_Vec2_Scale(&vdes, ms->max_speed / hz_count(s_move_work.hz), &desired_velocity);
    PFM_Vec2_Sub(&desired_velocity, (vec2_t*)&ms->velocity, &ret);
    vec2_truncate(&ret, SCALED_MAX_FORCE);
    return ret;
}

/* Cohesion is a behaviour that causes agents to steer towards the center of mass of nearby agents.
 */
static vec2_t cohesion_force(uint32_t uid, const struct flock_snap *snap)
{
    vec2_t COM = (vec2_t){0.0f};
    size_t neighbour_count = 0;
    vec2_t ent_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);

    for(size_t i = 0; i < snap->nents; i++) {

        if(snap->uids[i] == uid)
            continue;

        vec2_t diff;
        vec2_t curr_xz_pos = (vec2_t){snap->pos_x[i], snap->pos_z[i]};
        PFM_Vec2_Sub(&curr_xz_pos, &ent_xz_pos, &diff);

        float t = (PFM_Vec2_Len(&diff) - COHESION_NEIGHBOUR_RADIUS*0.75)
                / COHESION_NEIGHBOUR_RADIUS;
        float scale = exp(-6.0f * t);

        PFM_Vec2_Scale(&curr_xz_pos, scale, &curr_xz_pos);
        PFM_Vec2_Add(&COM, &curr_xz_pos, &COM);
        neighbour_count++;
    }

    if(0 == neighbour_count)
        return (vec2_t){0.0f};

    vec2_t ret;
    PFM_Vec2_Scale(&COM, 1.0f / neighbour_count, &COM);
    PFM_Vec2_Sub(&COM, &ent_xz_pos, &ret);
    vec2_truncate(&ret, SCALED_MAX_FORCE);
    return ret;
}

/* Cephes-derived polynomial exp over 8 lanes (relative error ~1e-7; the
 * result only weighs flock members in a steering heuristic). */
SIMD_TARGET_AVX2
static inline __m256 exp256_ps(__m256 x)
{
    const __m256 exp_hi = _mm256_set1_ps( 88.3762626647949f);
    const __m256 exp_lo = _mm256_set1_ps(-88.3762626647949f);
    const __m256 log2e  = _mm256_set1_ps(1.44269504088896341f);
    const __m256 c1     = _mm256_set1_ps(0.693359375f);
    const __m256 c2     = _mm256_set1_ps(-2.12194440e-4f);
    const __m256 one    = _mm256_set1_ps(1.0f);

    x = _mm256_min_ps(x, exp_hi);
    x = _mm256_max_ps(x, exp_lo);

    __m256 fx = _mm256_floor_ps(_mm256_add_ps(_mm256_mul_ps(x, log2e),
        _mm256_set1_ps(0.5f)));
    x = _mm256_sub_ps(x, _mm256_mul_ps(fx, c1));
    x = _mm256_sub_ps(x, _mm256_mul_ps(fx, c2));

    __m256 z = _mm256_mul_ps(x, x);
    __m256 y = _mm256_set1_ps(1.9875691500e-4f);
    y = _mm256_add_ps(_mm256_mul_ps(y, x), _mm256_set1_ps(1.3981999507e-3f));
    y = _mm256_add_ps(_mm256_mul_ps(y, x), _mm256_set1_ps(8.3334519073e-3f));
    y = _mm256_add_ps(_mm256_mul_ps(y, x), _mm256_set1_ps(4.1665795894e-2f));
    y = _mm256_add_ps(_mm256_mul_ps(y, x), _mm256_set1_ps(1.6666665459e-1f));
    y = _mm256_add_ps(_mm256_mul_ps(y, x), _mm256_set1_ps(5.0000001201e-1f));
    y = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(y, z), x), one);

    __m256i emm0 = _mm256_cvttps_epi32(fx);
    emm0 = _mm256_slli_epi32(_mm256_add_epi32(emm0, _mm256_set1_epi32(0x7f)), 23);
    return _mm256_mul_ps(y, _mm256_castsi256_ps(emm0));
}

SIMD_TARGET_AVX2
static vec2_t cohesion_force_avx2(uint32_t uid, const struct flock_snap *snap)
{
    vec2_t ent_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);

    const __m256 ex = _mm256_set1_ps(ent_xz_pos.x);
    const __m256 ez = _mm256_set1_ps(ent_xz_pos.z);
    const __m256 k_off = _mm256_set1_ps(COHESION_NEIGHBOUR_RADIUS * 0.75f);
    const __m256 k_scale = _mm256_set1_ps(-6.0f / COHESION_NEIGHBOUR_RADIUS);
    const __m256i vuid = _mm256_set1_epi32((int)uid);

    __m256 sum_x = _mm256_setzero_ps();
    __m256 sum_z = _mm256_setzero_ps();

    size_t nblock = snap->nents & ~7ull;
    for(size_t i = 0; i < nblock; i += 8) {

        __m256 px = _mm256_loadu_ps(&snap->pos_x[i]);
        __m256 pz = _mm256_loadu_ps(&snap->pos_z[i]);
        __m256i ids = _mm256_loadu_si256((const __m256i*)&snap->uids[i]);

        __m256 dx = _mm256_sub_ps(px, ex);
        __m256 dz = _mm256_sub_ps(pz, ez);
        __m256 dist = _mm256_sqrt_ps(_mm256_add_ps(_mm256_mul_ps(dx, dx),
            _mm256_mul_ps(dz, dz)));

        __m256 scale = exp256_ps(_mm256_mul_ps(_mm256_sub_ps(dist, k_off), k_scale));

        __m256 self = _mm256_castsi256_ps(_mm256_cmpeq_epi32(ids, vuid));
        scale = _mm256_andnot_ps(self, scale);

        sum_x = _mm256_add_ps(sum_x, _mm256_mul_ps(px, scale));
        sum_z = _mm256_add_ps(sum_z, _mm256_mul_ps(pz, scale));
    }

    float lanes_x[8], lanes_z[8];
    _mm256_storeu_ps(lanes_x, sum_x);
    _mm256_storeu_ps(lanes_z, sum_z);

    vec2_t COM = (vec2_t){0.0f};
    for(int i = 0; i < 8; i++) {
        COM.x += lanes_x[i];
        COM.z += lanes_z[i];
    }

    size_t neighbour_count = 0;
    for(size_t i = 0; i < nblock; i++) {
        neighbour_count += (snap->uids[i] != uid);
    }
    for(size_t i = nblock; i < snap->nents; i++) {

        if(snap->uids[i] == uid)
            continue;

        vec2_t diff;
        vec2_t curr_xz_pos = (vec2_t){snap->pos_x[i], snap->pos_z[i]};
        PFM_Vec2_Sub(&curr_xz_pos, &ent_xz_pos, &diff);

        float t = (PFM_Vec2_Len(&diff) - COHESION_NEIGHBOUR_RADIUS*0.75)
                / COHESION_NEIGHBOUR_RADIUS;
        float scale = exp(-6.0f * t);

        COM.x += curr_xz_pos.x * scale;
        COM.z += curr_xz_pos.z * scale;
        neighbour_count++;
    }

    if(0 == neighbour_count)
        return (vec2_t){0.0f};

    vec2_t ret;
    PFM_Vec2_Scale(&COM, 1.0f / neighbour_count, &COM);
    PFM_Vec2_Sub(&COM, &ent_xz_pos, &ret);
    vec2_truncate(&ret, SCALED_MAX_FORCE);
    return ret;
}

/* Set at init to the widest supported cohesion kernel. */
static vec2_t (*s_cohesion_force)(uint32_t uid, const struct flock_snap *snap) = cohesion_force;

/* Separation is a behaviour that causes agents to steer away from nearby agents.
 */
static float separation_reach(float radius_sum)
{
    return MAX(SEPARATION_NEIGHB_RADIUS, SEPARATION_NEIGHB_SCALE * radius_sum);
}

static float clearpath_reach(float radius_sum)
{
    return MAX(CLEARPATH_NEIGHBOUR_RADIUS, CLEARPATH_NEIGHBOUR_SCALE * radius_sum);
}

static vec2_t separation_force_gap(uint32_t uid, float buffer_dist, float *out_min_gap)
{
    vec2_t ret = (vec2_t){0.0f};
    uint32_t ent_flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    float ent_radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
    int ent_faction = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
    bool phasing = movestate_aux_get(uid)->phasing;

    uint32_t near_ents[256];
    int num_near = G_Pos_EntsInCircleFrom(s_move_work.gamestate.postree,
        s_move_work.gamestate.flags,
        G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid), 
        separation_reach(ent_radius + s_move_work.max_radius), near_ents, ARR_SIZE(near_ents));

    for(int i = 0; i < num_near; i++) {

        uint32_t curr = near_ents[i];
        uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, curr);
        if(curr == uid)
            continue;
        if(!(flags & ENTITY_FLAG_MOVABLE))
            continue;
        if((ent_flags & ENTITY_FLAG_AIR) != (flags & ENTITY_FLAG_AIR))
            continue;

        if(phasing
        && G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, curr) == ent_faction
        && ent_still(movestate_get(curr)))
            continue;

        vec2_t diff;
        vec2_t ent_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
        vec2_t curr_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, curr);

        float radius_sum = ent_radius
                         + G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, curr);
        float radius = radius_sum + buffer_dist;
        PFM_Vec2_Sub(&curr_xz_pos, &ent_xz_pos, &diff);

        if(PFM_Vec2_Len(&diff) < EPSILON)
            continue;
        if(PFM_Vec2_Len(&diff) > separation_reach(radius_sum))
            continue;

        float gap = PFM_Vec2_Len(&diff) - radius_sum;
        if(gap < *out_min_gap)
            *out_min_gap = gap;

        /* Exponential decay with y=1 when diff = radius*0.85 
         * Use smooth decay curves in order to curb the 'toggling' or oscillating 
         * behaviour that may arise when there are discontinuities in the forces. 
         */
        float eq = 0.85f;
        float steep = 20.0f;
        float t = (PFM_Vec2_Len(&diff) - radius*eq) / PFM_Vec2_Len(&diff);
        float scale = exp(MIN(-steep * t, 40.0f));
        PFM_Vec2_Scale(&diff, scale, &diff);

        PFM_Vec2_Add(&ret, &diff, &ret);
    }

    if(0 == num_near)
        return (vec2_t){0.0f};

    PFM_Vec2_Scale(&ret, -1.0f, &ret);
    vec2_truncate(&ret, SCALED_MAX_FORCE);
    return ret;
}

static vec2_t separation_force(uint32_t uid, float buffer_dist)
{
    float min_gap = INFINITY;
    return separation_force_gap(uid, buffer_dist, &min_gap);
}

/* A separation push against a live desired direction backs the unit out of
 * the way it wants to go; redirect that component sideways (the remembered
 * passing side) so nose-to-nose units step around each other instead of
 * bouncing along the line of centres. The redirected share blends smoothly
 * to zero as interpenetration deepens (a hard cut oscillates at the
 * boundary), so a wedged pair keeps the radial push-out.
 */
static vec2_t redirect_backward(vec2_t force, vec2_t vdes, int side, float min_gap)
{
    if(PFM_Vec2_Len(&vdes) < EPSILON || min_gap <= SEP_REDIRECT_MIN_GAP)
        return force;

    float frac = MIN(1.0f, (min_gap - SEP_REDIRECT_MIN_GAP) / -SEP_REDIRECT_MIN_GAP);

    vec2_t vhat;
    PFM_Vec2_Normal(&vdes, &vhat);
    float back = PFM_Vec2_Dot(&force, &vhat);
    if(back >= 0.0f)
        return force;

    vec2_t lat = (vec2_t){-vhat.z * side, vhat.x * side};
    vec2_t backward, lateral;
    PFM_Vec2_Scale(&vhat, back * frac, &backward);
    PFM_Vec2_Sub(&force, &backward, &force);
    PFM_Vec2_Scale(&lat, -back * frac, &lateral);
    PFM_Vec2_Add(&force, &lateral, &force);
    return force;
}

static vec2_t point_seek_total_force(uint32_t uid, const struct flock *flock,
                                     const struct flock_snap *fsnap,
                                     vec2_t vdes, bool has_dest_los, int side)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    struct arrival_state *as = flock_arrival_for_ent(flock, uid);
    vec2_t seek = as ? G_Arrival_SeekTarget(as, &movestate_aux_get(uid)->arrival,
        flock->target_xz) : flock->target_xz;
    vec2_t arrive = arrive_force_point(uid, seek, vdes, has_dest_los);
    vec2_t cohesion = s_cohesion_force(uid, fsnap);
    float min_gap = INFINITY;
    vec2_t separation = separation_force_gap(uid, SEPARATION_BUFFER_DIST, &min_gap);
    separation = redirect_backward(separation, vdes, side, min_gap);

    PFM_Vec2_Scale(&arrive,     MOVE_ARRIVE_FORCE_SCALE,   &arrive);
    PFM_Vec2_Scale(&cohesion,   MOVE_COHESION_FORCE_SCALE, &cohesion);
    PFM_Vec2_Scale(&separation, SEPARATION_FORCE_SCALE,    &separation);

    vec2_t ret = (vec2_t){0.0f};
    PFM_Vec2_Add(&ret, &arrive, &ret);
    PFM_Vec2_Add(&ret, &separation, &ret);
    PFM_Vec2_Add(&ret, &cohesion, &ret);

    vec2_truncate(&ret, SCALED_MAX_FORCE);
    return ret;
}

static vec2_t cell_seek_total_force(uint32_t uid, vec2_t cell_pos, vec2_t vdes,
                                    vec2_t cohesion, vec2_t alignment)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    vec2_t delta;
    vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    PFM_Vec2_Sub(&cell_pos, &pos_xz, &delta);

    vec2_t arrive = arrive_force_cell(uid, cell_pos, vdes);
    vec2_t separation = separation_force(uid, SEPARATION_BUFFER_DIST);

    PFM_Vec2_Scale(&arrive,     MOVE_ARRIVE_FORCE_SCALE,   &arrive);
    PFM_Vec2_Scale(&separation, SEPARATION_FORCE_SCALE,    &separation);
    PFM_Vec2_Scale(&cohesion,   MOVE_COHESION_FORCE_SCALE, &cohesion);
    PFM_Vec2_Scale(&alignment,  ALIGNMENT_FORCE_SCALE,     &alignment);

    vec2_t ret = (vec2_t){0.0f};
    PFM_Vec2_Add(&ret, &arrive, &ret);
    PFM_Vec2_Add(&ret, &separation, &ret);

    if(PFM_Vec2_Len(&delta) > CELL_ARRIVAL_RADIUS) {
        PFM_Vec2_Add(&ret, &cohesion, &ret);
        PFM_Vec2_Add(&ret, &alignment, &ret);
    }

    vec2_truncate(&ret, SCALED_MAX_FORCE);
    return ret;
}

static vec2_t enemy_seek_total_force(uint32_t uid, vec2_t vdes)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    vec2_t arrive = arrive_force_enemies(uid, vdes);
    vec2_t separation = separation_force(uid, SEPARATION_BUFFER_DIST);

    PFM_Vec2_Scale(&arrive,     MOVE_ARRIVE_FORCE_SCALE,   &arrive);
    PFM_Vec2_Scale(&separation, SEPARATION_FORCE_SCALE,    &separation);

    vec2_t ret = (vec2_t){0.0f, 0.0f};
    PFM_Vec2_Add(&ret, &arrive, &ret);
    PFM_Vec2_Add(&ret, &separation, &ret);

    vec2_truncate(&ret, SCALED_MAX_FORCE);
    return ret;
}

static vec2_t new_pos_for_vel(uint32_t uid, vec2_t velocity)
{
    vec2_t xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    vec2_t new_pos;

    PFM_Vec2_Add(&xz_pos, &velocity, &new_pos);
    return new_pos;
}

/* Nullify the components of the force which would guide
 * the entity towards an impassable tile. */
static void nullify_impass_components(uint32_t uid, vec2_t *inout_force)
{
    vec2_t nt_dims = N_TileDims();
    float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    enum nav_layer layer = Entity_NavLayerWithRadius(flags, radius);

    vec2_t pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    vec2_t left =  (vec2_t){pos.x + nt_dims.x, pos.z};
    vec2_t right = (vec2_t){pos.x - nt_dims.x, pos.z};
    vec2_t top =   (vec2_t){pos.x, pos.z + nt_dims.z};
    vec2_t bot =   (vec2_t){pos.x, pos.z - nt_dims.z};

    /* A unit already sitting on a blocked tile must be allowed to steer toward
     * blocked neighbours so it can slide off the blocked region, and one phasing
     * back into its formation must be allowed to steer into it; only impassable
     * terrain is forbidden in those cases.
     */
    bool on_blocked = movestate_aux_get(uid)->phasing
                   || M_NavPositionBlocked(s_move_work.gamestate.map, layer, pos);

    if(inout_force->x > 0 
    && (!M_NavPositionPathable(s_move_work.gamestate.map, layer, left)  
      || (!on_blocked && M_NavPositionBlocked(s_move_work.gamestate.map, layer, left))))
        inout_force->x = 0.0f;

    if(inout_force->x < 0 
    && (!M_NavPositionPathable(s_move_work.gamestate.map, layer, right) 
     || (!on_blocked && M_NavPositionBlocked(s_move_work.gamestate.map, layer, right))))
        inout_force->x = 0.0f;

    if(inout_force->z > 0 
    && (!M_NavPositionPathable(s_move_work.gamestate.map, layer, top) 
      || (!on_blocked && M_NavPositionBlocked(s_move_work.gamestate.map, layer, top))))
        inout_force->z = 0.0f;

    if(inout_force->z < 0 
    && (!M_NavPositionPathable(s_move_work.gamestate.map, layer, bot) 
      || (!on_blocked && M_NavPositionBlocked(s_move_work.gamestate.map, layer, bot))))
        inout_force->z = 0.0f;
}

static vec2_t point_seek_vpref(uint32_t uid, const struct flock *flock,
                               const struct flock_snap *fsnap,
                               vec2_t vdes, bool has_dest_los, float speed, int side)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    vec2_t steer_force;
    for(int prio = 0; prio < 3; prio++) {

        switch(prio) {
        case 0:
            steer_force = point_seek_total_force(uid, flock, fsnap, vdes, has_dest_los, side);
            break;
        case 1: {
            float min_gap = INFINITY;
            steer_force = separation_force_gap(uid, SEPARATION_BUFFER_DIST, &min_gap);
            steer_force = redirect_backward(steer_force, vdes, side, min_gap);
            break;
        }
        case 2: {
            struct arrival_state *as = flock_arrival_for_ent(flock, uid);
            vec2_t seek = as ? G_Arrival_SeekTarget(as, &movestate_aux_get(uid)->arrival,
                flock->target_xz) : flock->target_xz;
            steer_force = arrive_force_point(uid, seek, vdes, has_dest_los);
            break;
        }
        }

        /* Walls live in the velocity solve as tile obstacles; the force gets
         * clipped along the wall tangent there instead of axis-zeroed here. */
        if(PFM_Vec2_Len(&steer_force) > SCALED_MAX_FORCE * 0.01)
            break;
    }

    vec2_t accel, new_vel; 
    PFM_Vec2_Scale(&steer_force, 1.0f / ENTITY_MASS, &accel);

    PFM_Vec2_Add(&ms->velocity, &accel, &new_vel);
    vec2_truncate(&new_vel, speed / hz_count(s_move_work.hz));

    return new_vel;
}

static vec2_t cell_arrival_seek_vpref(uint32_t uid, vec2_t cell_pos, float speed, vec2_t vdes,
                                      vec2_t cohesion, vec2_t alignment, vec2_t drag)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    vec2_t steer_force;
    for(int prio = 0; prio < 3; prio++) {

        switch(prio) {
        case 0: 
            steer_force = cell_seek_total_force(uid, cell_pos, vdes, cohesion, alignment); 
            break;
        case 1: 
            steer_force = separation_force(uid, SEPARATION_BUFFER_DIST); 
            break;
        case 2: 
            steer_force = arrive_force_cell(uid, cell_pos, vdes); 
            break;
        }

        nullify_impass_components(uid, &steer_force);
        if(PFM_Vec2_Len(&steer_force) > SCALED_MAX_FORCE * 0.01)
            break;
    }

    vec2_t accel, new_vel; 
    PFM_Vec2_Scale(&steer_force, 1.0f / ENTITY_MASS, &accel);

    PFM_Vec2_Add(&ms->velocity, &accel, &new_vel);
    vec2_truncate(&new_vel, speed / hz_count(s_move_work.hz));
    if(PFM_Vec2_Len(&drag) > EPSILON) {
        vec2_truncate(&new_vel, (speed * 0.75) / hz_count(s_move_work.hz));
    }

    return new_vel;
}

static vec2_t enemy_seek_vpref(uint32_t uid, float speed, vec2_t vdes, bool pinned)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    /* A pinned unit with nowhere to press stands dead still; the separation
     * jostle would otherwise keep it from ever reading as jammed. */
    if(pinned && PFM_Vec2_Len(&vdes) < EPSILON)
        return (vec2_t){0.0f, 0.0f};

    vec2_t steer_force = enemy_seek_total_force(uid, vdes);

    vec2_t accel, new_vel; 
    PFM_Vec2_Scale(&steer_force, 1.0f / ENTITY_MASS, &accel);

    PFM_Vec2_Add(&ms->velocity, &accel, &new_vel);
    vec2_truncate(&new_vel, speed / hz_count(s_move_work.hz));

    return new_vel;
}

/* Run directly away from a threat along 'vdes', separating from neighbours.
 */
static vec2_t flee_total_force(uint32_t uid, vec2_t vdes)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    vec2_t arrive = arrive_force_enemies(uid, vdes);
    vec2_t separation = separation_force(uid, SEPARATION_BUFFER_DIST);

    PFM_Vec2_Scale(&arrive,     MOVE_ARRIVE_FORCE_SCALE,   &arrive);
    PFM_Vec2_Scale(&separation, SEPARATION_FORCE_SCALE,    &separation);

    vec2_t ret = (vec2_t){0.0f, 0.0f};
    PFM_Vec2_Add(&ret, &arrive, &ret);
    PFM_Vec2_Add(&ret, &separation, &ret);

    vec2_truncate(&ret, SCALED_MAX_FORCE);
    return ret;
}

static vec2_t flee_vpref(uint32_t uid, float speed, vec2_t vdes)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    vec2_t steer_force = flee_total_force(uid, vdes);
    /* No flow field guides a flee; clip so a cornered unit slides along
     * obstructions. */
    nullify_impass_components(uid, &steer_force);

    vec2_t accel, new_vel;
    PFM_Vec2_Scale(&steer_force, 1.0f / ENTITY_MASS, &accel);

    PFM_Vec2_Add(&ms->velocity, &accel, &new_vel);
    vec2_truncate(&new_vel, speed / hz_count(s_move_work.hz));

    return new_vel;
}

static vec2_t formation_point_seek_total_force(uint32_t uid, const struct flock *flock, vec2_t vdes,
                                               vec2_t cohesion, vec2_t alignment, bool has_dest_los)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    vec2_t arrive = arrive_force_point(uid, flock->target_xz, vdes, has_dest_los);
    vec2_t separation = separation_force(uid, SEPARATION_BUFFER_DIST);

    PFM_Vec2_Scale(&arrive,     MOVE_ARRIVE_FORCE_SCALE,   &arrive);
    PFM_Vec2_Scale(&cohesion,   MOVE_COHESION_FORCE_SCALE, &cohesion);
    PFM_Vec2_Scale(&separation, SEPARATION_FORCE_SCALE,    &separation);
    PFM_Vec2_Scale(&alignment,  ALIGNMENT_FORCE_SCALE,     &alignment);

    vec2_t ret = (vec2_t){0.0f};
    PFM_Vec2_Add(&ret, &arrive, &ret);
    PFM_Vec2_Add(&ret, &separation, &ret);
    PFM_Vec2_Add(&ret, &cohesion, &ret);

    vec2_truncate(&ret, SCALED_MAX_FORCE);
    return ret;
}

static vec2_t formation_seek_vpref(uint32_t uid, const struct flock *flock, float speed,
                                   vec2_t vdes, vec2_t cohesion, vec2_t alignment, vec2_t drag,
                                   bool has_dest_los)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    vec2_t steer_force;
    for(int prio = 0; prio < 3; prio++) {

        switch(prio) {
        case 0: 
            steer_force = formation_point_seek_total_force(uid, flock, 
                vdes, cohesion, alignment, has_dest_los); 
            break;
        case 1: 
            steer_force = separation_force(uid, SEPARATION_BUFFER_DIST); 
            break;
        case 2: 
            steer_force = arrive_force_point(uid, flock->target_xz, vdes, has_dest_los); 
            break;
        }

        nullify_impass_components(uid, &steer_force);
        if(PFM_Vec2_Len(&steer_force) > SCALED_MAX_FORCE * 0.01)
            break;
    }

    vec2_t accel, new_vel; 
    PFM_Vec2_Scale(&steer_force, 1.0f / ENTITY_MASS, &accel);

    PFM_Vec2_Add(&ms->velocity, &accel, &new_vel);
    vec2_truncate(&new_vel, speed / hz_count(s_move_work.hz));
    if(PFM_Vec2_Len(&drag) > EPSILON) {
        vec2_truncate(&new_vel, (speed * 0.75) / hz_count(s_move_work.hz));
    }

    return new_vel;
}

static void update_vel_hist(struct movestate_aux *aux, vec2_t vnew)
{
    ASSERT_IN_MAIN_THREAD();

    assert(aux->vel_hist_idx >= 0 && aux->vel_hist_idx < VEL_HIST_LEN);
    aux->vel_hist[aux->vel_hist_idx] = vnew;
    aux->vel_hist_idx = ((aux->vel_hist_idx+1) % VEL_HIST_LEN);
}

static bool vel_hist_empty(const struct movestate_aux *aux)
{
    for(int i = 0; i < VEL_HIST_LEN; i++)
        if(PFM_Vec2_Len((vec2_t*)&aux->vel_hist[i]) > EPSILON)
            return false;
    return true;
}

static vec2_t facing_dir(quat_t rot)
{
    float theta = 2.0 * atan2(rot.y, rot.w);
    return (vec2_t){-sin(theta), cos(theta)};
}

static void seed_vel_hist_facing(struct movestate *ms, struct movestate_aux *aux)
{
    vec2_t dir = facing_dir(ms->next_rot);
    PFM_Vec2_Scale(&dir, PFM_Vec2_Len(&ms->velocity), &dir);
    for(int i = 0; i < VEL_HIST_LEN; i++)
        aux->vel_hist[i] = dir;
}

/* Simple Moving Average */
static vec2_t vel_sma(const struct movestate_aux *aux)
{
    vec2_t ret = {0};
    for(int i = 0; i < VEL_HIST_LEN; i++)
        PFM_Vec2_Add(&ret, (vec2_t*)&aux->vel_hist[i], &ret);
    PFM_Vec2_Scale(&ret, 1.0f/VEL_HIST_LEN, &ret);
    return ret;
}

/* Weighted Moving Average */
static vec2_t vel_wma(const struct movestate_aux *aux)
{
    vec2_t ret = {0};
    float denom = 0.0f;

    for(int i = 0; i < VEL_HIST_LEN; i++) {

        vec2_t term = aux->vel_hist[(aux->vel_hist_idx + i) % VEL_HIST_LEN];
        PFM_Vec2_Scale(&term, VEL_HIST_LEN-i, &term);
        PFM_Vec2_Add(&ret, &term, &ret);
        denom += (VEL_HIST_LEN-i);
    }

    if(denom > EPSILON) {
        PFM_Vec2_Scale(&ret, 1.0f/denom, &ret);
    }
    return ret;
}

static bool uids_match(void *arg, struct move_cmd *cmd)
{
    uint32_t desired_uid = (uintptr_t)arg;
    return (desired_uid == cmd->uid);
}

static struct move_cmd *snoop_most_recent_command(enum move_cmd_type type, void *arg,
                                                  bool (*pred)(void*, struct move_cmd*),
                                                  bool remove)
{
    if(queue_size(s_move_commands) == 0)
        return NULL;

    size_t left = queue_size(s_move_commands);
    for(int i = s_move_commands.itail; left > 0;) {
        struct move_cmd *curr = &s_move_commands.mem[i];
        if(!curr->deleted && curr->type == type) {
            if(pred(arg, curr)) {
                curr->deleted = remove;
                return curr;
            }
        }
        i--;
        left--;
        if(i < 0) {
            i = s_move_commands.capacity - 1; /* Wrap around */
        }
    }
    return NULL;
}

static bool snoop_still(uint32_t uid)
{
    if(queue_size(s_move_commands) == 0) {
        struct movestate *ms = movestate_get(uid);
        assert(ms);
        return (ms->state == STATE_ARRIVED);
    }

    size_t left = queue_size(s_move_commands);
    for(int i = s_move_commands.itail; left > 0;) {
        struct move_cmd *curr = &s_move_commands.mem[i];
        switch(curr->type) {
        case MOVE_CMD_SET_DEST:
        case MOVE_CMD_CHANGE_DIRECTION:
        case MOVE_CMD_SET_ENTER_RANGE:
        case MOVE_CMD_SET_SEEK_ENEMIES:
        case MOVE_CMD_SET_SURROUND_ENTITY:
        case MOVE_CMD_SET_FLEE: {
            if(curr->uid == uid)
                return false;
            break;
        }
        case MOVE_CMD_STOP:
            if(curr->uid == uid)
                return true;
            break;
        default:
            break;
        }
        i--;
        left--;
        if(i < 0) {
            i = s_move_commands.capacity - 1; /* Wrap around */
        }
    }

    struct movestate *ms = movestate_get(uid);
    assert(ms);
    return (ms->state == STATE_ARRIVED);
}

static bool arrived(uint32_t uid, vec2_t xz_pos)
{
    vec2_t diff_to_target;
    struct flock *flock = flock_for_ent(uid);
    assert(flock);

    PFM_Vec2_Sub((vec2_t*)&flock->target_xz, &xz_pos, &diff_to_target);
    float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
    float arrive_thresh = radius * 1.5f;
    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    enum nav_layer layer = Entity_NavLayerWithRadius(flags, radius);

    if(PFM_Vec2_Len(&diff_to_target) < arrive_thresh
    || (M_NavIsAdjacentToImpassable(s_map, layer, xz_pos) 
        && M_NavIsMaximallyClose(s_map, layer, xz_pos, flock->target_xz, arrive_thresh))) {
        return true;
    }

    vec2_t nearest;
    if(M_NavClosestPathable(s_map, layer, flock->target_xz, &nearest)) {
        vec2_t delta;
        PFM_Vec2_Sub(&nearest, &xz_pos, &delta);
        if(PFM_Vec2_Len(&delta) < arrive_thresh)
            return true;
    }

    return false;
}

static float unit_height(uint32_t uid, vec2_t pos)
{
    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    if(flags & ENTITY_FLAG_WATER)
        return 0.0f;
    if(flags & ENTITY_FLAG_AIR) {
        return M_HeightAtPoint(s_map, pos) + AIR_UNIT_HEIGHT;
    }
    return M_HeightAtPoint(s_map, pos);
}

static int hz_count(enum movement_hz hz)
{
    switch(hz) {
    case MOVE_HZ_20:    return 20;
    case MOVE_HZ_10:    return 10;
    case MOVE_HZ_5:     return 5;
    case MOVE_HZ_1:     return 1;
    default: assert(0);
    }
    return 0;
}

static vec3_t interpolate_positions(vec3_t from, vec3_t to, float fraction)
{
    assert(fraction >= 0.0f && fraction <= 1.0f);

    if(fabs(1.0 - fraction) < EPSILON)
        return to;

    vec3_t delta;
    PFM_Vec3_Sub(&to, &from, &delta);
    PFM_Vec3_Scale(&delta, fraction, &delta);

    vec3_t ret;
    PFM_Vec3_Add(&from, &delta, &ret);
    return ret;
}

static quat_t interpolate_rotations(quat_t from, quat_t to, float fraction)
{
    assert(fraction >= 0.0f && fraction <= 1.0f);

    if(fabs(1.0 - fraction) < EPSILON)
        return to;

    return PFM_Quat_Slerp(&from, &to, fraction);
}

/* Rotate 'cur' towards 'target' by at most 'max_deg' degrees about the Y axis,
 * picking a stable direction even when the orientations are antipodal.
 */
static quat_t turn_toward(quat_t cur, quat_t target, float max_deg)
{
    float angle_deg = RAD_TO_DEG(PFM_Quat_PitchDiff(&cur, &target));

    /* Near 180 degrees the shortest-arc direction is undefined: the cross-product
     * term collapses to zero and its sign is pure noise. Force a fixed sweep so the
     * unit never flips.
     */
    if(180.0f - fabs(angle_deg) < 1.0f)
        angle_deg = 180.0f;

    float turn_deg = MIN(max_deg, fabs(angle_deg)) * -SIGNUM(angle_deg);
    mat4x4_t rotmat;
    PFM_Mat4x4_MakeRotY(DEG_TO_RAD(turn_deg), &rotmat);

    quat_t rot, final;
    PFM_Quat_FromRotMat(&rotmat, &rot);
    PFM_Quat_MultQuat(&rot, &cur, &final);
    PFM_Quat_Normal(&final, &final);
    return final;
}

static bool move_gated_by_heading(enum move_state state)
{
    switch(state) {
    case STATE_MOVING:
    case STATE_SEEK_ENEMIES:
    case STATE_SURROUND_ENTITY:
    case STATE_ENTER_ENTITY_RANGE:
    case STATE_FLEEING:
        return true;
    default:
        return false;
    }
}

static vec2_t intended_heading(vec2_t vdes, vec2_t new_vel)
{
    return (PFM_Vec2_Len(&vdes) > EPSILON) ? vdes : new_vel;
}

static quat_t orient_to_velocity_history(const struct movestate *ms,
                                         const struct movestate_aux *aux)
{
    vec2_t wma = vel_wma(aux);
    if(PFM_Vec2_Len(&wma) > EPSILON)
        return turn_toward(ms->next_rot, dir_quat_from_velocity(wma), SCALED_MAX_TURN_RATE);
    return ms->next_rot;
}

/* Derive the patch that should be applied onto the movestate 
 * as a result of the current navigation tick. The patch can be
 * generated asynchronously, but applied synchronously.
 */
static void entity_compute_update(enum movement_hz hz, uint32_t uid, vec2_t new_vel, vec2_t vdes,
                                  const struct move_work_in *in, struct movestate_patch *out)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);
    struct movestate_aux *aux = movestate_aux_get(uid);
    assert(aux);
    out->flags = in->seek_pinned ? UPDATE_SEEK_PINNED : 0;
    out->flags |= in->field_void ? UPDATE_FIELD_VOID : 0;

    /* Flush the interpolation if was not completed */
    if(ms->left > 0) {
        out->flags |= UPDATE_SET_POSITION | UPDATE_SET_ROTATION | UPDATE_SET_LEFT;
        out->next_pos = ms->next_pos;
        out->next_rot = ms->next_rot;
        out->next_left = 0;
    }

    assert(hz_count(hz) <= 20);
    assert(20 % hz_count(hz) == 0);

    /* Gate translation on heading so a unit never slides sideways out of a stop.
     */
    bool turn_to_move = false;
    quat_t travel_dir = ms->next_rot;
    if(PFM_Vec2_Len(&new_vel) > EPSILON && move_gated_by_heading(ms->state)) {
        travel_dir = dir_quat_from_velocity(intended_heading(vdes, new_vel));
        float heading_err = fabs(RAD_TO_DEG(PFM_Quat_PitchDiff(&ms->next_rot, &travel_dir)));
        float tolerance = (PFM_Vec2_Len(&ms->velocity) > EPSILON) ? MOVE_HEADING_HALT
                                                                  : MOVE_HEADING_RESUME;
        if(heading_err > tolerance) {
            turn_to_move = true;
            new_vel = (vec2_t){0.0f, 0.0f};
            out->flags |= UPDATE_HEADING_GATED;
        }
    }

    vec2_t new_pos_xz = new_pos_for_vel(uid, new_vel);
    float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    enum nav_layer layer = Entity_NavLayerWithRadius(flags, radius);

    if(flags & ENTITY_FLAG_GARRISONED) {
        if(!ent_still(ms)) {
            out->flags |= UPDATE_SET_STATE;
            out->next_state = STATE_ARRIVED;
            out->next_block = false;
        }
        return;
    }

    /* Refuse to land on a dynamically-blocked tile (a building and the like). 
     * A unit already on a blocker may still step off it, and one phasing back
     * into its formation may step onto its own side's stamps.
     */
    vec2_t curr_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    bool on_blocked = aux->phasing
                   || M_NavPositionBlocked(s_move_work.gamestate.map, layer, curr_xz);

    if(PFM_Vec2_Len(&new_vel) > 0
    && M_NavPositionPathable(s_move_work.gamestate.map, layer, new_pos_xz)
    && (on_blocked || !M_NavPositionBlocked(s_move_work.gamestate.map, layer, new_pos_xz))) {

        vec3_t new_pos = (vec3_t){new_pos_xz.x, unit_height(uid, new_pos_xz), new_pos_xz.z};

        out->flags |= UPDATE_SET_PREV_POS | UPDATE_SET_NEXT_POS | UPDATE_SET_STEP | UPDATE_SET_LEFT;
        out->next_ppos = ms->next_pos;
        out->next_npos = new_pos;
        out->next_step = 1.0f / (20 / hz_count(hz));
        out->next_left = (20 / hz_count(hz)) - 1;

        if(out->next_left == 0) {
            out->flags |= UPDATE_SET_POSITION;
            out->next_pos = new_pos;
        }else{
            vec3_t intermediate = interpolate_positions(out->next_ppos, out->next_npos, ms->step);
            new_pos_xz = (vec2_t){intermediate.x, intermediate.z};
            out->flags |= UPDATE_SET_POSITION;
            out->next_pos = intermediate;
        }

        out->flags |= UPDATE_SET_VELOCITY;
        out->next_velocity = new_vel;

        /* Orient off a weighted average of past velocities, so the visible facing
         * lags the true heading slightly but turns smoothly. The gate above has
         * already ensured the unit is roughly aligned before it got moving.
         */
        out->flags |= UPDATE_SET_PREV_ROT;
        out->next_prot = ms->next_rot;

        out->flags |= UPDATE_SET_NEXT_ROT;
        /* Being shoved off the cell to let a neighbour by is not a heading. */
        out->next_nrot = aux->parked
            ? turn_toward(ms->next_rot, in->fstate.target_orientation, SCALED_MAX_TURN_RATE)
            : orient_to_velocity_history(ms, aux);
        out->flags |= UPDATE_SET_ROTATION;
        out->next_rot = (out->next_left == 0) ? out->next_nrot : ms->next_rot;

    }else{
        out->flags |= UPDATE_SET_VELOCITY;
        out->next_velocity = (vec2_t){0.0f, 0.0f};

        if(PFM_Vec2_Len(&new_vel) > 0) {
            out->flags |= M_NavPositionPathable(s_move_work.gamestate.map, layer, new_pos_xz)
                        ? UPDATE_VETO_BLOCKED : UPDATE_VETO_UNPATHABLE;
        }

        /* A combat-held unit pivots toward its combat facing, not its travel heading. The
         * rotation patch is set only inside a branch that actually computes next_nrot; with no
         * branch taken the unit keeps its current rotation (else next_nrot is left unset). */
        bool held = G_FlagsGetFrom(s_move_work.gamestate.flags, uid) & ENTITY_FLAG_COMBAT_HELD;
        if(held || aux->parked) {
            quat_t facing = held ? aux->combat_facing : in->fstate.target_orientation;
            out->flags |= UPDATE_SET_PREV_ROT | UPDATE_SET_NEXT_ROT | UPDATE_SET_ROTATION;
            out->flags |= UPDATE_TURNING_IN_PLACE;
            out->next_prot = ms->next_rot;
            out->next_nrot = turn_toward(ms->next_rot, facing, SCALED_MAX_TURN_RATE);
            out->next_rot = ms->next_rot;
        }else if(turn_to_move) {
            out->flags |= UPDATE_SET_PREV_ROT | UPDATE_SET_NEXT_ROT | UPDATE_SET_ROTATION;
            out->flags |= UPDATE_TURNING_IN_PLACE;
            out->next_prot = ms->next_rot;
            out->next_nrot = turn_toward(ms->next_rot, travel_dir, SCALED_MAX_TURN_RATE);
            out->next_rot = ms->next_rot;
        }
    }

    /* If the entity's current position isn't pathable, simply keep it 'stuck' there in
     * the same state it was in before. Under normal conditions, no entity can move from 
     * pathable terrain to non-pathable terrain, but an this violation is possible by 
     * forcefully setting the entity's position from a scripting call. 
     */
    if(!M_NavPositionPathable(s_move_work.gamestate.map, layer, new_pos_xz))
        return;

    switch(ms->state) {
    case STATE_MOVING:
    case STATE_MOVING_IN_FORMATION: {

        /* A jammed plain mover is soft-blocker material too; the formation
         * machinery owns its members' spacing. */
        if(ms->state == STATE_MOVING && in->fstate.fid == NULL_FID) {
            /* A heading-gated pivot is not a jam. */
            if(!(out->flags & UPDATE_HEADING_GATED)
            && PFM_Vec2_Len(&out->next_velocity) < EPSILON)
                out->flags |= UPDATE_SEEK_STUCK;
            if(in->njam + MIN((int)in->nterrain, SEEK_WALL_NEIGHB_CREDIT)
               < SEEK_RELEASE_NEIGHBS)
                out->flags |= UPDATE_SEEK_CLEAR;
        }

        if((in->fstate.fid != NULL_FID) && !in->fstate.assignment_ready)
            break;

        if(in->fstate.fid != NULL_FID
        && in->fstate.assigned_to_cell
        && in->fstate.in_range_of_cell) {

            out->flags |= UPDATE_SET_STATE;
            out->next_state = STATE_ARRIVING_TO_CELL;
            break;
        }

        struct flock *flock = flock_for_ent(uid);
        assert(flock);

        struct arrival_state *as = G_ArrivalGroup_ForLayer(&flock->arrival, layer);
        if(as && G_Arrival_IsActive(as)) {
            int n_settled = adjacent_settled_count(uid);
            if(G_Arrival_ShouldSettle(as, &aux->arrival, s_map,
                s_move_work.gamestate.map, new_pos_xz, ms->velocity, radius, n_settled)) {
                out->flags |= UPDATE_SET_STATE;
                out->next_state = STATE_ARRIVED;
                out->next_block = true;
                break;
            }
        }else{

            if(arrived(uid, new_pos_xz)) {
                out->flags |= UPDATE_SET_STATE;
                out->next_state = STATE_ARRIVED;
                out->next_block = true;
                break;
            }

            uint32_t adjacent[128];
            size_t num_adj = adjacent_flock_members(uid, flock, adjacent,
                ARR_SIZE(adjacent));

            bool done = false;
            for(int j = 0; j < num_adj; j++) {

                struct movestate *adj_ms = movestate_get(adjacent[j]);
                assert(adj_ms);

                if(adj_ms->state == STATE_ARRIVED) {

                    out->flags |= UPDATE_SET_STATE;
                    out->next_state = STATE_ARRIVED;
                    out->next_block = true;
                    done = true;
                    break;
                }
            }
            STFREE(adjacent);

            if(done) {
                break;
            }
        }

        /* If we've not hit a condition to stop or give up but our desired velocity
         * is zero, that means the navigation system is currently not able to guide
         * the entity any closer to its' goal. Stop and wait, re-requesting the  path
         * after some time. A starved unit is an exception: its field simply hasn't
         * been rebuilt yet this tick, so it stays MOVING and is served by the
         * round-robin rebuild cursor within the next few ticks.
         */
        if(PFM_Vec2_Len(&vdes) < EPSILON) {

            if(in->field_starved)
                break;

            /* The arrive force steers straight at a destination in sight, so
             * an empty field sample is no reason to park.
             */
            if(in->has_dest_los)
                break;

            /* A soft blocker already stands and walls its tiles. */
            if(aux->soft_blocking)
                break;

            assert(flock_for_ent(uid));
            out->flags |= UPDATE_SET_STATE;
            out->next_state = STATE_WAITING;
            out->next_block = true;
            break;
        }
        break;
    }
    case STATE_SEEK_ENEMIES: {

        /* When the seek field can't route us we retry next tick rather than
         * full-stopping into WAITING; a persistently stuck, crowded seeker
         * becomes a soft blocker in the apply phase.
         */
        if(!(out->flags & UPDATE_HEADING_GATED)
        && PFM_Vec2_Len(&out->next_velocity) < EPSILON)
            out->flags |= UPDATE_SEEK_STUCK;
        if(in->njam + MIN((int)in->nterrain, SEEK_WALL_NEIGHB_CREDIT)
           < SEEK_RELEASE_NEIGHBS)
            out->flags |= UPDATE_SEEK_CLEAR;
        break;
    }
    case STATE_SURROUND_ENTITY: {

        /* A shuttle wedged in a crowd at its destination walls like a jammed
         * seeker: the field then routes the rest of the crowd around it and
         * the blob drains from the rim.
         */
        if(!(out->flags & UPDATE_HEADING_GATED)
        && PFM_Vec2_Len(&out->next_velocity) < EPSILON)
            out->flags |= UPDATE_SEEK_STUCK;
        if(in->njam + MIN((int)in->nterrain, SEEK_WALL_NEIGHB_CREDIT)
           < SURROUND_RELEASE_NEIGHBS)
            out->flags |= UPDATE_SEEK_CLEAR;


        if(ms->surround_target_uid == NULL_UID) {
            out->flags |= UPDATE_SET_STATE;
            out->next_state = STATE_ARRIVED;
            out->next_block = true;
            break;
        }

        if(!entity_exists(ms->surround_target_uid)
        ||  M_NavObjAdjacentFrom(s_move_work.gamestate.map, uid, ms->surround_target_uid,
                                 &s_move_work.unit_query_ctx)) {
            out->flags |= UPDATE_SET_STATE;
            out->next_state = STATE_ARRIVED;
            out->next_block = true;
            break;
        }

        vec2_t target_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, 
            ms->surround_target_uid);
        vec2_t dest = aux->surround_nearest_prev;

        vec2_t delta;
        PFM_Vec2_Sub(&target_pos, &aux->surround_target_prev, &delta);
        if(PFM_Vec2_Len(&delta) > EPSILON || PFM_Vec2_Len(&ms->velocity) < EPSILON) {

            bool hasdest = M_NavClosestReachableAdjacentPosFrom(s_move_work.gamestate.map, layer,
                new_pos_xz, ms->surround_target_uid, &s_move_work.unit_query_ctx, &dest);

            if(!hasdest) {
                out->flags |= UPDATE_SET_STATE;
                out->next_state = STATE_ARRIVED;
                out->next_block = true;
                break;
            }
        }

        struct flock *flock = flock_for_ent(uid);
        assert(flock);

        vec2_t diff;
        PFM_Vec2_Sub(&flock->target_xz, &dest, &diff);
        aux->surround_target_prev = target_pos;
        aux->surround_nearest_prev = dest;

        if(PFM_Vec2_Len(&diff) > EPSILON) {
            out->flags |= UPDATE_SET_DEST | UPDATE_SET_STATE;
            out->next_dest = dest;
            out->next_attack = false;
            out->next_state = STATE_SURROUND_ENTITY;
            break;
        }

        if(PFM_Vec2_Len(&vdes) < EPSILON) {
            out->flags |= UPDATE_SET_STATE;
            out->next_state = STATE_WAITING;
            out->next_block = true;
        }
        break;
    }
    case STATE_ENTER_ENTITY_RANGE: {

        if(ms->surround_target_uid == NULL_UID) {
            out->flags |= UPDATE_SET_STATE;
            out->next_state = STATE_ARRIVED;
            out->next_block = true;
            break;
        }

        vec2_t xz_target = G_Pos_GetXZFrom(s_move_work.gamestate.positions, 
            ms->surround_target_uid);

        vec2_t delta;
        PFM_Vec2_Sub(&new_pos_xz, &xz_target, &delta);

        if(PFM_Vec2_Len(&delta) <= aux->target_range
        || (M_NavIsAdjacentToImpassable(s_move_work.gamestate.map, layer, new_pos_xz) 
            && M_NavIsMaximallyClose(s_move_work.gamestate.map, layer, new_pos_xz, xz_target, 0.0f))) {
        
            out->flags |= UPDATE_SET_STATE;
            out->next_state = STATE_WAITING;
            out->next_block = true;
            break;
        }

        vec2_t target_delta;
        PFM_Vec2_Sub(&xz_target, &aux->target_prev_pos, &target_delta);

        if(PFM_Vec2_Len(&target_delta) > 5.0f) {
            out->flags |= UPDATE_SET_DEST | UPDATE_SET_TARGET_PREV;
            out->next_dest = xz_target;
            out->next_attack = false;
            out->next_target_prev = xz_target;
        }

        break;
    }
    case STATE_TURNING: {

        /* find the angle between the two quaternions */
        float angle_diff = PFM_Quat_PitchDiff(&ms->next_rot, &aux->target_dir);
        float degrees = RAD_TO_DEG(angle_diff);

        /* if it's within a tolerance, stop turning */
        if(fabs(degrees) <= 5.0f) {
            out->flags |= UPDATE_SET_STATE;
            out->next_state = STATE_ARRIVED;
            out->next_block = true;
            break;
        }

        /* If not, turn towards the target by at most the turn rate */
        quat_t final = turn_toward(ms->next_rot, aux->target_dir, SCALED_MAX_TURN_RATE);

        out->flags |= UPDATE_SET_ROTATION | UPDATE_SET_PREV_ROT | UPDATE_SET_NEXT_ROT;
        out->next_rot = final;
        out->next_prot = final;
        out->next_nrot = final;

        break;
    }
    case STATE_WAITING:
        /* The resume countdown runs on the main thread. */
        break;
    case STATE_ARRIVED:
        break;
    case STATE_FLEEING:
        /* No self-transitions; the combat system owns entry and exit. */
        break;
    case STATE_ARRIVING_TO_CELL: {
        if(in->fstate.fid == NULL_FID) {
            out->flags |= UPDATE_SET_STATE;
            out->next_state = STATE_MOVING;
            break;
        }
        if(!in->fstate.assignment_ready)
            break;
        if(!in->fstate.in_range_of_cell) {
            /* We got pushed off of the cell arrival field */
            out->flags |= UPDATE_SET_STATE;
            out->next_state = STATE_MOVING_IN_FORMATION;
            break;
        }
        if(in->fstate.arrived_at_cell && in->fstate.may_settle) {
            out->flags |= UPDATE_SET_STATE | UPDATE_SET_TARGET_DIR;
            out->next_target_dir = in->fstate.target_orientation;
            out->next_state = STATE_TURNING;
            break;
        }
        break;
    }
    default: 
        assert(0);
    }
}

static void ent_update_using_surround_field(uint32_t uid, struct movestate *ms)
{
    vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    vec2_t target_pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, ms->surround_target_uid);
    float dx = fabs(target_pos_xz.x - pos_xz.x);
    float dz = fabs(target_pos_xz.z - pos_xz.z);

    if(!ms->using_surround_field) {
        if(dx < SURROUND_LOW_WATER_X && dz < SURROUND_LOW_WATER_Z) {
            ms->using_surround_field = true;
        }
    }else{
        if(dx >= SURROUND_HIGH_WATER_X || dz >= SURROUND_HIGH_WATER_Z) {
            ms->using_surround_field = false;
        }
    }
}

/* Wall cones and parked bodies can leave a unit no permissible velocity but
 * zero, and it would stand there for good. The landing test keeps a relaxed
 * solve legal, so a window of one is the way out.
 */
/* A unit shuffling on the spot is as stuck as one standing still */
static bool ent_wobbling(struct movestate_aux *aux)
{
    vec2_t net = (vec2_t){0.0f, 0.0f};
    float walked = 0.0f;

    for(int i = 0; i < VEL_HIST_LEN; i++) {
        PFM_Vec2_Add(&net, &aux->vel_hist[i], &net);
        walked += PFM_Vec2_Len(&aux->vel_hist[i]);
    }
    float progress = PFM_Vec2_Len(&net);
    return walked > EPSILON && progress < CP_WOBBLE_NET_MAX
        && progress < CP_WOBBLE_NET_FRACTION * walked;
}

static void entity_apply_cp_stall(uint32_t uid, bool stalled)
{
    struct movestate_aux *aux = movestate_aux_get(uid);

    bool going_nowhere = stalled || ent_wobbling(aux);
    aux->cp_stall_ticks = going_nowhere ? MIN(aux->cp_stall_ticks + 1, UINT16_MAX) : 0;
    if(aux->cp_stall_ticks >= CP_STALL_RELAX_TICKS) {
        aux->cp_relax_ticks = CP_STALL_RELAX_HOLD;
    }else if(aux->cp_relax_ticks > 0) {
        aux->cp_relax_ticks--;
    }
}

static void entity_apply_cp_side(uint32_t uid, int side)
{
    struct movestate_aux *aux = movestate_aux_get(uid);
    if(!aux)
        return;
    if(side != 0) {
        aux->cp_side = side;
        aux->cp_side_ticks = 0;
    }else if(aux->cp_side != 0 && ++aux->cp_side_ticks > CP_SIDE_HOLD_TICKS) {
        aux->cp_side = 0;
    }
}

static void entity_apply_update(uint32_t uid, const struct movestate_patch *patch)
{
    ASSERT_IN_MAIN_THREAD();

    if(!G_EntityExists(uid) || G_EntityIsZombie(uid) || G_EntityIsGarrisoned(uid))
        return;

    struct movestate *ms = movestate_get(uid);
    struct movestate_aux *aux = movestate_aux_get(uid);
    if(!ms)
        return;

    if(patch->flags & UPDATE_SET_STATE) {

        if(patch->next_state == STATE_ARRIVED || (patch->next_state == STATE_WAITING)) {
            entity_finish_moving(uid, patch->next_state, patch->next_block);
        }else{
            ms->state = patch->next_state;
        }
    }

    if(patch->flags & UPDATE_SET_VELOCITY) {
        ms->velocity = patch->next_velocity;
        /* While pivoting in place, wipe the velocity history so the orientation
         * doesn't chase the stale pre-order heading once movement resumes. */
        if(patch->flags & UPDATE_TURNING_IN_PLACE) {
            memset(aux->vel_hist, 0, sizeof(aux->vel_hist));
        }else{
            if(vel_hist_empty(aux) && PFM_Vec2_Len(&ms->velocity) > EPSILON)
                seed_vel_hist_facing(ms, aux);
            update_vel_hist(aux, ms->velocity);
        }
    }

    if(patch->flags & UPDATE_SET_POSITION)
        G_Pos_Set(uid, patch->next_pos);

    if(patch->flags & UPDATE_SET_ROTATION)
        Entity_SetRot(uid, patch->next_rot);

    if(patch->flags & UPDATE_SET_PREV_POS)
        ms->prev_pos = patch->next_ppos;

    if(patch->flags & UPDATE_SET_NEXT_POS)
        ms->next_pos = patch->next_npos;

    if(patch->flags & UPDATE_SET_STEP)
        ms->step = patch->next_step;

    if(patch->flags & UPDATE_SET_LEFT)
        ms->left = patch->next_left;

    if(patch->flags & UPDATE_SET_PREV_ROT)
        ms->prev_rot = patch->next_prot;

    if(patch->flags & UPDATE_SET_NEXT_ROT)
        ms->next_rot = patch->next_nrot;

    if(patch->flags & UPDATE_SET_TARGET_PREV)
        aux->target_prev_pos = patch->next_target_prev;

    if(patch->flags & UPDATE_SET_TARGET_DIR)
        aux->target_dir = patch->next_target_dir;

    if(patch->flags & UPDATE_SET_MOVING) {
        if(ms->blocking)
            entity_unblock(uid);
        move_notify_motion_start(uid, ms);
        ms->state = patch->next_state;
    }

    if(ms->state == STATE_SURROUND_ENTITY) {
        ent_update_using_surround_field(uid, ms);
    }

    /* A held unit's soft block is managed by the flag alone; a unit that
     * went still is a real blocker now and must drop the soft one. */
    bool held = G_FlagsGet(uid) & ENTITY_FLAG_COMBAT_HELD;
    if(aux->soft_blocking
    && (ms->blocking || ent_still(ms)
        || (!held && ms->state != STATE_SEEK_ENEMIES && ms->state != STATE_MOVING
                  && ms->state != STATE_SURROUND_ENTITY))) {
        entity_soft_unblock(uid);
    }

    /* Re-register a held unit that shed its block through a still state. */
    if(held && !aux->soft_blocking && !ms->blocking && !ent_still(ms)) {
        entity_soft_block(uid);
    }

    aux->seek_pin_held = !!(patch->flags & UPDATE_SEEK_PINNED);
    if(patch->flags & UPDATE_FIELD_VOID) {
        if(aux->field_void_ticks < UINT16_MAX)
            aux->field_void_ticks++;
    }else{
        aux->field_void_ticks = 0;
    }

    if(!held && (ms->state == STATE_SEEK_ENEMIES || ms->state == STATE_MOVING
              || ms->state == STATE_SURROUND_ENTITY)) {
        bool stuck = patch->flags & UPDATE_SEEK_STUCK;
        bool clear = patch->flags & UPDATE_SEEK_CLEAR;
        if(PFM_Vec2_Len(&ms->velocity) > CLEARPATH_STILL_SPEED)
            aux->seek_progressed = true;
        aux->seek_stuck_ticks = stuck ? MIN(aux->seek_stuck_ticks + 1, UINT16_MAX) : 0;
        aux->seek_clear_ticks = clear ? MIN(aux->seek_clear_ticks + 1, UINT16_MAX) : 0;

        /* A unit that never got going is queueing, not jammed, so only one
         * that has moved may wall itself off. Pinned units hold their line by
         * intent, and a shuttle's re-issued order silently clears the flag.
         */
        bool may_wall = aux->seek_progressed || aux->seek_pin_held
                     || ms->state == STATE_SURROUND_ENTITY;
        /* A relaxed solve has had its window and the unit is still going
         * nowhere: too few neighbours to read as a jam, yet plainly wedged.
         */
        bool relax_spent = aux->cp_stall_ticks >= CP_STALL_RELAX_TICKS + CP_STALL_RELAX_HOLD;
        if(!aux->soft_blocking && may_wall && s_soft_block_budget > 0
        && (relax_spent || (stuck && !clear && aux->seek_stuck_ticks >= SEEK_STUCK_BLOCK_TICKS))) {
            s_soft_block_budget--;
            entity_soft_block(uid);
        }else if(aux->soft_blocking && aux->seek_clear_ticks >= SEEK_RELEASE_HOLD_TICKS) {
            entity_soft_unblock(uid);
        }
    }
}

struct near_ent_dist{
    uint32_t uid;
    float    dist2;
};

static int near_ent_dist_cmp(const void *a, const void *b)
{
    const struct near_ent_dist *na = a, *nb = b;
    return (na->dist2 > nb->dist2) - (na->dist2 < nb->dist2);
}

static void find_neighbours(uint32_t uid,
                            struct cp_ent *out_dyn, size_t *out_ndyn,
                            struct cp_ent *out_stat, size_t *out_nstat,
                            size_t *out_njam,
                            uint32_t *out_nn_uid, float *out_nn_dist)
{
    *out_ndyn = 0;
    *out_nstat = 0;
    *out_njam = 0;
    *out_nn_uid = NULL_UID;
    *out_nn_dist = INFINITY;

    /* For the ClearPath algorithm, we only consider entities with
     * ENTITY_FLAG_MOVABLE set, as they are the only ones that may need
     * to be avoided during moving. Here, 'static' entites refer
     * to those entites that are not currently in a 'moving' state,
     * meaning they will not perform collision avoidance maneuvers of
     * their own. */

    uint32_t ent_flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    vec2_t ent_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    float ent_radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
    int ent_faction = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
    bool phasing = movestate_aux_get(uid)->phasing;
    uint32_t near_ents[512];
    int num_near = G_Pos_EntsInCircleFrom(s_move_work.gamestate.postree, 
        s_move_work.gamestate.flags,
        ent_pos,
        clearpath_reach(ent_radius + s_move_work.max_radius), near_ents, ARR_SIZE(near_ents));

    /* Keep the NEAREST candidates in each class rather than the first
     * encountered: in dense crowds the query returns far more than the caps,
     * and an arbitrary subset both degrades avoidance and defeats the
     * nearest-first pairwise cap in the velocity solver.
     */
    struct near_ent_dist dists[512];
    for(int i = 0; i < num_near; i++) {
        vec2_t cpos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, near_ents[i]);
        vec2_t diff;
        PFM_Vec2_Sub(&cpos, &ent_pos, &diff);
        dists[i] = (struct near_ent_dist){near_ents[i], diff.x * diff.x + diff.y * diff.y};
    }
    if(num_near > 2 * MAX_NEIGHBOURS) {
        qsort(dists, num_near, sizeof(struct near_ent_dist), near_ent_dist_cmp);
    }

    for(int i = 0; i < num_near; i++) {

        if(*out_ndyn == MAX_NEIGHBOURS && *out_nstat == MAX_NEIGHBOURS)
            break;

        uint32_t curr = dists[i].uid;
        uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, curr);

        if(curr == uid)
            continue;

        if(!(flags & ENTITY_FLAG_MOVABLE))
            continue;

        float curr_radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, curr);
        if(curr_radius == 0.0f)
            continue;

        if((ent_flags & ENTITY_FLAG_AIR) != (flags & ENTITY_FLAG_AIR))
            continue;

        if(phasing
        && G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, curr) == ent_faction
        && ent_still(movestate_get(curr)))
            continue;

        float reach = clearpath_reach(ent_radius + curr_radius);
        if(dists[i].dist2 > reach * reach)
            continue;

        struct movestate *ms = movestate_get(curr);
        assert(ms);

        /* Parked bodies are workforce, not crowd. */
        if(!ent_still(ms))
            (*out_njam)++;

        if(dists[i].dist2 < (*out_nn_dist) * (*out_nn_dist)) {
            *out_nn_dist = sqrtf(dists[i].dist2);
            *out_nn_uid = curr;
        }

        vec2_t curr_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, curr);
        struct cp_ent newdesc = (struct cp_ent) {
            .xz_pos = curr_xz_pos,
            .xz_vel = ms->velocity,
            .radius = curr_radius
        };

        /* A neighbour right at its arrival slot is about to settle and will not yield,
         * so avoid it fully (static) even while it is still moving at speed. */
        struct movestate_aux *curr_aux = movestate_aux_get(curr);
        bool at_slot = G_Arrival_NeighbourSettling(&curr_aux->arrival,
            curr_xz_pos, newdesc.radius);

        if(ent_still(ms) || PFM_Vec2_Len(&ms->velocity) < CLEARPATH_STILL_SPEED || at_slot) {
            /* A static neighbour is a stationary obstacle; its velocity-obstacle apex
             * must sit on its body, not be offset by a stale/leftover velocity. */
            newdesc.xz_vel = (vec2_t){0.0f, 0.0f};
            if(*out_nstat < MAX_NEIGHBOURS)
                out_stat[(*out_nstat)++] = newdesc;
        }else {
            if(*out_ndyn < MAX_NEIGHBOURS)
                out_dyn[(*out_ndyn)++] = newdesc;
        }
    }
}

static void disband_empty_flocks(void)
{
    ASSERT_IN_MAIN_THREAD();
    PERF_ENTER();

    uint32_t curr;
    /* Iterate vector backwards so we can delete entries while iterating. */
    for(int i = vec_size(&s_flocks)-1; i >= 0; i--) {

        /* First, decide if we can disband this flock */
        bool disband = true;
        kh_foreach_key(vec_AT(&s_flocks, i).ents, curr, {

            struct movestate *ms = movestate_get(curr);
            assert(ms);

            if(ms->state != STATE_ARRIVED) {
                disband = false;
                break;
            }
        });

        if(disband) {

            struct flock *flock = &vec_AT(&s_flocks, i);
            uint32_t uid;
            kh_foreach_key(flock->ents, uid, {
                G_Formation_RemoveUnit(uid);
            });
            kh_destroy(entity, flock->ents);
            G_ArrivalGroup_Destroy(&flock->arrival);
            vec_flock_del(&s_flocks, i);
        }
    }
    PERF_RETURN_VOID();
}

static void do_add_entity(uint32_t uid, vec3_t pos, float selection_radius, int faction_id)
{
    ASSERT_IN_MAIN_THREAD();

    if(!G_EntityExists(uid))
        return;

    int ret;
    khiter_t k = kh_put(pos, s_move_work.gamestate.positions, uid, &ret);
    assert(ret != -1);
    kh_val(s_move_work.gamestate.positions, k) = pos;

    bg_ent_insert(s_move_work.gamestate.postree, pos.x, pos.z, uid);

    k = kh_put(range, s_move_work.gamestate.sel_radiuses, uid, &ret);
    assert(ret != -1);
    kh_value(s_move_work.gamestate.sel_radiuses, k) = selection_radius;
    s_max_sel_radius = MAX(s_max_sel_radius, selection_radius);

    k = kh_put(id, s_move_work.gamestate.faction_ids, uid, &ret);
    assert(ret != -1);
    kh_value(s_move_work.gamestate.faction_ids, k) = faction_id;

    k = kh_put(id, s_move_work.gamestate.flags, uid, &ret);
    assert(ret != -1);
    kh_value(s_move_work.gamestate.flags, k) = G_FlagsGet(uid);

    struct movestate new_ms = (struct movestate) {
        .velocity = {0.0f}, 
        .blocking = false,
        .state = STATE_ARRIVED,
        .max_speed = 0.0f,
        .base_speed = 0.0f,
        .speed_bonus = 0.0f,
        .left = 0,
        .prev_pos = pos,
        .next_pos = pos,
        .prev_rot = Entity_GetRot(uid),
        .next_rot = Entity_GetRot(uid),
    };

    struct movestate_aux new_aux = (struct movestate_aux) {
        .vel_hist_idx = 0,
        .combat_facing = Entity_GetRot(uid),
        .seek_pin_target = NULL_UID,
        .surround_target_prev = (vec2_t){0},
        .surround_nearest_prev = (vec2_t){0},
        .motion_stopped = true,
    };
    memset(new_aux.vel_hist, 0, sizeof(new_aux.vel_hist));

    k = kh_put(state, s_entity_state_table, uid, &ret);
    assert(ret != -1 && ret != 0);
    kh_value(s_entity_state_table, k) = new_ms;

    k = kh_put(auxstate, s_entity_aux_table, uid, &ret);
    assert(ret != -1 && ret != 0);
    kh_value(s_entity_aux_table, k) = new_aux;

    entity_block(uid);
}

static void do_remove_entity(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return;

    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);

    do_stop(uid);
    if(!(flags & ENTITY_FLAG_GARRISONED)) {
        entity_unblock(uid);
    }

    kh_del(state, s_entity_state_table, k);
    khiter_t l = kh_get(auxstate, s_entity_aux_table, uid);
    assert(l != kh_end(s_entity_aux_table));
    kh_del(auxstate, s_entity_aux_table, l);
}

static void do_stop(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return;

    entity_reset_seek_counters(uid);
    clear_seek_pin(uid);

    if(!ent_still(ms)) {
        entity_finish_moving(uid, STATE_ARRIVED, true);
    }

    remove_from_flocks(uid);
    ms->state = STATE_ARRIVED;
}

static void do_set_dest(uint32_t uid, vec2_t dest_xz, bool attack)
{
    ASSERT_IN_MAIN_THREAD();

    entity_reset_seek_counters(uid);
    clear_seek_pin(uid);

    /* An entity ordered in the frame it was created is not in the tick
     * snapshot yet; the live tables are authoritative here.
     */
    float radius = G_GetSelectionRadius(uid);
    uint32_t flags = G_FlagsGet(uid);
    enum nav_layer layer = Entity_NavLayerWithRadius(flags, radius);
    vec2_t pos = G_Pos_GetXZ(uid);
    dest_xz = M_NavClosestReachableDest(s_map, layer, pos, dest_xz);

    /* If a flock already exists for the entity's destination, 
     * simply add the entity to the flock. If necessary, the
     * right flow fields will be computed on-demand during the
     * next movement update. 
     */
    dest_id_t dest_id;
    if(attack) {
        int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
        dest_id = M_NavDestIDForPosAttacking(s_map, dest_xz, layer, faction_id);
    }else{
        dest_id = M_NavDestIDForPos(s_map, dest_xz, layer);
    }
    struct flock *fl = flock_for_dest(dest_id);

    if(fl && fl == flock_for_ent(uid)) { 
        struct movestate *ms = movestate_get(uid);
        assert(ms);
        if(ent_still(ms)) {
            entity_unblock(uid);
            move_notify_motion_start(uid, ms);
        }
        ms->state = STATE_MOVING;
        return;
    }

    if(fl) {

        assert(fl != flock_for_ent(uid));
        remove_from_flocks(uid);
        flock_add(fl, uid);

        struct movestate *ms = movestate_get(uid);
        assert(ms);
        if(ent_still(ms)) {
            entity_unblock(uid);
            move_notify_motion_start(uid, ms);
        }
        ms->state = STATE_MOVING;
        assert(flock_for_ent(uid));
        return;
    }

    /* Else, create a new flock and request a path for it.
     */
    vec_entity_t flock;
    vec_entity_init(&flock);
    vec_entity_push(&flock, uid);

    enum formation_type type = FORMATION_NONE;
    formation_id_t fid = G_Formation_GetForEnt(uid);
    if(fid != NULL_FID) {
        type = G_Formation_Type(fid);
    }

    make_flock(&flock, dest_xz, layer, attack, type);
    vec_entity_destroy(&flock);
}

static void do_set_change_direction(uint32_t uid, quat_t target)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return;

    entity_reset_seek_counters(uid);

    if(ent_still(ms)) {
        entity_unblock(uid);
        move_notify_motion_start(uid, ms);
    }

    ms->state = STATE_TURNING;
    movestate_aux_get(uid)->target_dir = target;
}

static void do_set_combat_facing(uint32_t uid, quat_t dir)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return;
    movestate_aux_get(uid)->combat_facing = dir;
}

static void do_set_combat_held(uint32_t uid, bool held)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return;

    uint32_t flags = G_FlagsGet(uid);
    if(held == (bool)(flags & ENTITY_FLAG_COMBAT_HELD))
        return;

    /* Release before clearing the flag: the unblock's motion start stays
     * suppressed and this function announces the single start. */
    if(!held)
        entity_reset_seek_counters(uid);

    G_FlagsSet(uid, held ? (flags | ENTITY_FLAG_COMBAT_HELD)
                         : (flags & ~ENTITY_FLAG_COMBAT_HELD));

    if(ent_still(ms))
        return;

    /* Block a held unit's tiles so the floods route followers around it;
     * still units already block. */
    if(held && !ms->blocking && !movestate_aux_get(uid)->soft_blocking)
        entity_soft_block(uid);

    if(held)
        move_notify_motion_end(uid);
    else
        move_notify_motion_start(uid, ms);
}

static void do_set_enter_range(uint32_t uid, uint32_t target, float range)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return;

    if(!G_EntityExists(target))
        return;

    vec2_t xz_src = G_Pos_GetXZ(uid);
    vec2_t xz_dst = G_Pos_GetXZ(target);
    float radius = G_GetSelectionRadius(uid);
    range = MAX(0.0f, range - radius);

    vec2_t delta;
    PFM_Vec2_Sub(&xz_src, &xz_dst, &delta);
    if(PFM_Vec2_Len(&delta) <= range) {
        do_stop(uid);
        return;
    }

    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    vec2_t xz_target = M_NavClosestReachableInRange(s_map, 
        Entity_NavLayerWithRadius(flags, radius), xz_src, xz_dst, range - radius);
    do_set_dest(uid, xz_target, false);

    ms->state = STATE_ENTER_ENTITY_RANGE;
    ms->surround_target_uid = target;
    struct movestate_aux *aux = movestate_aux_get(uid);
    aux->target_prev_pos = xz_dst;
    aux->target_range = range;
}

static bool using_surround_field(uint32_t uid, uint32_t target)
{
    vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    vec2_t target_pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, target);

    float dx = fabs(target_pos_xz.x - pos_xz.x);
    float dz = fabs(target_pos_xz.z - pos_xz.z);
    return (dx < SURROUND_LOW_WATER_X && dz < SURROUND_LOW_WATER_Z);
}

static void do_set_surround_entity(uint32_t uid, uint32_t target)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return;

    if(!G_EntityExists(target))
        return;

    do_stop(uid);
    do_set_dest(uid, G_Pos_GetXZ(target), false);

    assert(!ms->blocking);
    ms->state = STATE_SURROUND_ENTITY;
    ms->surround_target_uid = target;
    ms->using_surround_field = using_surround_field(uid, target);
}

static void do_set_seek_enemies(uint32_t uid)
{
    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return;

    entity_reset_seek_counters(uid);
    clear_seek_pin(uid);
    remove_from_flocks(uid);

    if(ent_still(ms)) {
        entity_unblock(uid);
        move_notify_motion_start(uid, ms);
    }

    ms->state = STATE_SEEK_ENEMIES;
}

static void clear_seek_pin(uint32_t uid)
{
    struct movestate_aux *aux = movestate_aux_get(uid);
    if(aux) {
        aux->seek_pin_target = NULL_UID;
        aux->seek_pin_held = false;
    }
}

/* A live-target swap keeps the unit's jam state so the wall it is part of
 * holds; a lost target or a cleared pin frees it to step into the gap.
 */
static void do_set_seek_pin(uint32_t uid, uint32_t target)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate_aux *aux = movestate_aux_get(uid);
    if(!aux || aux->seek_pin_target == target)
        return;

    uint32_t prev = aux->seek_pin_target;
    aux->seek_pin_target = target;
    if(target == NULL_UID)
        aux->seek_pin_held = false;

    bool prev_gone = (prev != NULL_UID)
        && (!entity_exists(prev)
         || (G_FlagsGetFrom(s_move_work.gamestate.flags, prev) & ENTITY_FLAG_ZOMBIE));
    if(target != NULL_UID && !prev_gone)
        return;

    bool progressed = aux->seek_progressed;
    entity_reset_seek_counters(uid);
    aux->seek_progressed = progressed;
}

static void do_set_flee(uint32_t uid, vec2_t threat_xz)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return;

    entity_reset_seek_counters(uid);

    struct movestate_aux *aux = movestate_aux_get(uid);
    aux->flee_threat_pos = threat_xz;

    if(ms->state == STATE_FLEEING)
        return;

    aux->flee_prev = ent_still(ms) ? STATE_MOVING : ms->state;

    if(ent_still(ms)) {
        entity_unblock(uid);
        move_notify_motion_start(uid, ms);
    }

    ms->state = STATE_FLEEING;
}

static void do_stop_flee(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    if(!ms || ms->state != STATE_FLEEING)
        return;

    /* The restored states steer relative to a flock; without one, stop. */
    if(!flock_for_ent(uid)) {
        do_stop(uid);
        return;
    }

    struct movestate_aux *aux = movestate_aux_get(uid);
    ms->state = aux->flee_prev;
}

static void do_update_pos(uint32_t uid, vec2_t pos)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return;

    vec3_t newpos = {
        pos.x,
        unit_height(uid, pos),
        pos.z
    };

    /* The positions table is read by the drain-time blocking paths and must be
     * patched; the snapshot postree has no reader before it is rebuilt from
     * live state at the tick's copy phase, so it is left stale.
     */
    khiter_t k = kh_get(pos, s_move_work.gamestate.positions, uid);
    assert(k != kh_end(s_move_work.gamestate.positions));
    vec3_t oldpos = kh_val(s_move_work.gamestate.positions, k);
    kh_val(s_move_work.gamestate.positions, k) = newpos;

    vec2_t jump;
    PFM_Vec2_Sub(&pos, &(vec2_t){oldpos.x, oldpos.z}, &jump);
    if(PFM_Vec2_Len(&jump) > MOVE_TELEPORT_RESET_DIST) {
        entity_reset_seek_counters(uid);
    }

    if(!ms->blocking)
        return;

    int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    M_NavBlockersDecref(ms->last_stop_pos, ms->last_stop_radius, faction_id, flags, s_map);
    M_NavBlockersIncref(pos, ms->last_stop_radius, faction_id, flags, s_map);
    ms->last_stop_pos = pos;
    ms->prev_pos = newpos;
    ms->next_pos = newpos;
}

static void do_update_faction_id(uint32_t uid, int oldfac, int newfac)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return;

    entity_reset_seek_counters(uid);

    khiter_t k = kh_get(id, s_move_work.gamestate.faction_ids, uid);
    assert(k != kh_end(s_move_work.gamestate.faction_ids));
    kh_val(s_move_work.gamestate.faction_ids, k) = newfac;

    if(!ms->blocking)
        return;

    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    M_NavBlockersDecref(ms->last_stop_pos, ms->last_stop_radius, oldfac, flags, s_map);
    M_NavBlockersIncref(ms->last_stop_pos, ms->last_stop_radius, newfac, flags, s_map);
}

static void do_update_selection_radius(uint32_t uid, float sel_radius)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return;

    entity_reset_seek_counters(uid);

    khiter_t k = kh_get(range, s_move_work.gamestate.sel_radiuses, uid);
    assert(k != kh_end(s_move_work.gamestate.sel_radiuses));
    kh_val(s_move_work.gamestate.sel_radiuses, k) = sel_radius;
    s_max_sel_radius = MAX(s_max_sel_radius, sel_radius);

    if(!ms->blocking)
        return;

    int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    M_NavBlockersDecref(ms->last_stop_pos, ms->last_stop_radius, faction_id, flags, s_map);
    M_NavBlockersIncref(ms->last_stop_pos, sel_radius, faction_id, flags, s_map);
    ms->last_stop_radius = sel_radius;
}

static void do_set_max_speed(uint32_t uid, float speed)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return;
    struct movestate *ms = &kh_value(s_entity_state_table, k);
    ms->base_speed = speed;
    ms->max_speed = MAX(0.0f, speed + ms->speed_bonus);
}

static void do_set_speed_bonus(uint32_t uid, float bonus)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return;
    struct movestate *ms = &kh_value(s_entity_state_table, k);
    ms->speed_bonus = bonus;
    ms->max_speed = MAX(0.0f, ms->base_speed + bonus);
}

static void do_block(uint32_t uid, vec3_t newpos)
{
    khiter_t k = kh_get(pos, s_move_work.gamestate.positions, uid);
    assert(k != kh_end(s_move_work.gamestate.positions));
    vec3_t oldpos = kh_val(s_move_work.gamestate.positions, k);
    bg_ent_delete(s_move_work.gamestate.postree, oldpos.x, oldpos.z, uid);
    bg_ent_insert(s_move_work.gamestate.postree, newpos.x, newpos.z, uid);
    kh_val(s_move_work.gamestate.positions, k) = newpos;

    entity_block(uid);
}

static void move_push_cmd(struct move_cmd cmd)
{
    queue_cmd_push(&s_move_commands, &cmd);
}

static void move_process_cmds(void)
{
    struct move_cmd cmd;
    while(queue_cmd_pop(&s_move_commands, &cmd)) {

        if(cmd.deleted)
            continue;

        /* The movestate outlives the entity by a tick, so the handlers' own
         * guards miss an order queued for one that has since died.
         */
        if(cmd.type != MOVE_CMD_REMOVE && cmd.type != MOVE_CMD_MAKE_FLOCKS
        && !G_EntityExists(cmd.uid))
            continue;

        switch(cmd.type) {
        case MOVE_CMD_ADD: {
            do_add_entity(cmd.uid, cmd.u.add.pos, cmd.u.add.radius,
                cmd.u.add.faction_id);
            break;
        }
        case MOVE_CMD_REMOVE: {
            do_remove_entity(cmd.uid);
            break;
        }
        case MOVE_CMD_STOP: {
            do_stop(cmd.uid);
            break;
        }
        case MOVE_CMD_SET_DEST: {
            do_set_dest(cmd.uid, cmd.u.set_dest.dest_xz, cmd.u.set_dest.attack);
            break;
        }
        case MOVE_CMD_CHANGE_DIRECTION: {
            do_set_change_direction(cmd.uid, cmd.u.change_direction.target);
            break;
        }
        case MOVE_CMD_SET_ENTER_RANGE: {
            do_set_enter_range(cmd.uid, cmd.u.enter_range.target,
                cmd.u.enter_range.range);
            break;
        }
        case MOVE_CMD_SET_SEEK_ENEMIES: {
            do_set_seek_enemies(cmd.uid);
            break;
        }
        case MOVE_CMD_SET_SEEK_PIN: {
            do_set_seek_pin(cmd.uid, cmd.u.seek_pin.target);
            break;
        }
        case MOVE_CMD_SET_SURROUND_ENTITY: {
            do_set_surround_entity(cmd.uid, cmd.u.surround.target);
            break;
        }
        case MOVE_CMD_UPDATE_POS: {
            do_update_pos(cmd.uid, cmd.u.update_pos.pos);
            break;
        }
        case MOVE_CMD_UPDATE_FACTION_ID: {
            do_update_faction_id(cmd.uid, cmd.u.update_faction.oldfac,
                cmd.u.update_faction.newfac);
            break;
        }
        case MOVE_CMD_UPDATE_SELECTION_RADIUS: {
            do_update_selection_radius(cmd.uid, cmd.u.update_radius.radius);
            break;
        }
        case MOVE_CMD_SET_MAX_SPEED: {
            do_set_max_speed(cmd.uid, cmd.u.max_speed.speed);
            break;
        }
        case MOVE_CMD_SET_SPEED_BONUS: {
            do_set_speed_bonus(cmd.uid, cmd.u.speed_bonus.bonus);
            break;
        }
        case MOVE_CMD_MAKE_FLOCKS: {
            vec_entity_t *sel = cmd.u.make_flocks.sel;
            make_flocks(sel, cmd.u.make_flocks.target_xz,
                cmd.u.make_flocks.target_orientation, cmd.u.make_flocks.type,
                cmd.u.make_flocks.attack);
            vec_entity_destroy(sel);
            PF_FREE(sel);
            break;
        }
        case MOVE_CMD_UNBLOCK: {
            struct movestate *ms = movestate_get(cmd.uid);
            if(ms && ms->blocking) {
                entity_unblock(cmd.uid);
            }
            break;
        }
        case MOVE_CMD_BLOCK: {
            struct movestate *ms = movestate_get(cmd.uid);
            if(ms && !ms->blocking) {
                do_block(cmd.uid, cmd.u.block.pos);
            }
            break;
        }
        case MOVE_CMD_SET_COMBAT_FACING: {
            do_set_combat_facing(cmd.uid, cmd.u.combat_facing.dir);
            break;
        }
        case MOVE_CMD_SET_COMBAT_HELD: {
            do_set_combat_held(cmd.uid, cmd.u.combat_held.held);
            break;
        }
        case MOVE_CMD_SET_FLEE: {
            do_set_flee(cmd.uid, cmd.u.flee.threat_xz);
            break;
        }
        case MOVE_CMD_STOP_FLEE: {
            do_stop_flee(cmd.uid);
            break;
        }
        default:
            assert(0);
        }
    }
}

/* The nearest movable neighbour regardless of the solver's query radius,
 * whether the solver saw it, and the separation force the steering summed.
 * Must run before the solve compacts the neighbour lists.
 */
static void move_trace_velocity(const struct move_work_in *in, struct move_trace *tr,
                                vec2_t vpref)
{
    uint32_t uid = in->ent_uid;
    uint32_t ent_flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    vec2_t pos = in->cp_ent.xz_pos;

    tr->vpref = vpref;
    tr->sep = separation_force(uid, SEPARATION_BUFFER_DIST);
    tr->nn_uid = NULL_UID;
    tr->nn_dist = INFINITY;

    uint32_t near_ents[512];
    int num_near = G_Pos_EntsInCircleFrom(s_move_work.gamestate.postree,
        s_move_work.gamestate.flags, pos, MV_TRACE_NN_RADIUS, near_ents, ARR_SIZE(near_ents));

    for(int i = 0; i < num_near; i++) {

        uint32_t curr = near_ents[i];
        uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, curr);
        if(curr == uid || !(flags & ENTITY_FLAG_MOVABLE))
            continue;
        if((ent_flags & ENTITY_FLAG_AIR) != (flags & ENTITY_FLAG_AIR))
            continue;

        vec2_t cpos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, curr);
        vec2_t diff;
        PFM_Vec2_Sub(&cpos, &pos, &diff);
        float dist = PFM_Vec2_Len(&diff);

        if(dist <= separation_reach(in->cp_ent.radius
                                  + G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, curr)))
            tr->sep_n++;
        if(dist < tr->nn_dist) {
            tr->nn_dist = dist;
            tr->nn_uid = curr;
            tr->nn_pos = cpos;
            tr->nn_radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, curr);
        }
    }

    for(size_t i = 0; i < in->ndyn + in->nstat; i++) {
        const struct cp_ent *n = (i < in->ndyn) ? &in->dyn_neighbs[i]
                                                : &in->stat_neighbs[i - in->ndyn];
        if(n->xz_pos.x == tr->nn_pos.x && n->xz_pos.z == tr->nn_pos.z) {
            tr->nn_seen = true;
            break;
        }
    }

    if(PFM_Vec2_Len(&vpref) < EPSILON && PFM_Vec2_Len(&in->ent_des_v) > EPSILON) {

        const struct map *map = s_move_work.gamestate.map;
        enum nav_layer layer = Entity_NavLayerWithRadius(ent_flags, in->cp_ent.radius);
        vec2_t nt = N_TileDims();
        vec2_t probes[4] = {
            (vec2_t){pos.x + nt.x, pos.z}, (vec2_t){pos.x - nt.x, pos.z},
            (vec2_t){pos.x, pos.z + nt.z}, (vec2_t){pos.x, pos.z - nt.z},
        };
        struct map_resolution res;
        M_NavGetResolution(map, &res);
        M_Tile_DescForPoint2D(res, M_GetPos(map), pos, &tr->td);
        tr->probed = true;
        tr->probe = M_NavPositionBlocked(map, layer, pos) ? 1 : 0;
        for(int i = 0; i < 4; i++) {
            tr->probe |= M_NavPositionPathable(map, layer, probes[i]) << (1 + 2 * i);
            tr->probe |= M_NavPositionBlocked(map, layer, probes[i]) << (2 + 2 * i);
        }
        struct target target = build_target(uid, in->flock);
        for(int dz = -1; dz <= 1; dz++) {
        for(int dx = -1; dx <= 1; dx++) {
            vec2_t p = (vec2_t){pos.x + dx * nt.x, pos.z + dz * nt.z};
            tr->dirs[(dz + 1) * 3 + (dx + 1)] = M_NavFlowFieldDirAt(map, target, p);
        }}
    }
}

static float wall_distance(const vec2_t *tiles, size_t ntiles, vec2_t pos)
{
    float ret = INFINITY;
    for(size_t i = 0; i < ntiles; i++) {
        vec2_t diff;
        PFM_Vec2_Sub((vec2_t*)&tiles[i], &pos, &diff);
        ret = MIN(ret, PFM_Vec2_Len(&diff));
    }
    return ret;
}

/* Beside a wall a wedged pair cannot both take the open lane. Both evaluate the
 * same test, so exactly one gives way: the one with less room to the wall, and
 * on a tie the lower uid. A pair that is moving needs no arbitration.
 */
static bool ent_hugs_wall(const struct move_work_in *in)
{
    if(in->ntiles == 0 || in->nn_uid == NULL_UID)
        return false;
    if(PFM_Vec2_Len((vec2_t*)&in->ent_des_v) < EPSILON)
        return false;
    if(movestate_aux_get(in->ent_uid)->cp_stall_ticks == 0
    || movestate_aux_get(in->nn_uid)->cp_stall_ticks == 0)
        return false;

    float nn_radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, in->nn_uid);
    if(in->nn_dist > in->cp_ent.radius + nn_radius + HUG_CONTACT_MARGIN)
        return false;

    const struct movestate *nn_ms = movestate_get(in->nn_uid);
    if(!nn_ms || ent_still(nn_ms))
        return false;

    vec2_t nn_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, in->nn_uid);
    vec2_t ahead, vhat;
    PFM_Vec2_Sub(&nn_pos, (vec2_t*)&in->cp_ent.xz_pos, &ahead);
    if(PFM_Vec2_Len(&ahead) < EPSILON)
        return false;
    PFM_Vec2_Normal(&ahead, &ahead);
    PFM_Vec2_Normal((vec2_t*)&in->ent_des_v, &vhat);
    if(PFM_Vec2_Dot(&ahead, &vhat) < HUG_AHEAD_DOT)
        return false;

    float mine = wall_distance(in->tile_obs, in->ntiles, in->cp_ent.xz_pos);
    float theirs = wall_distance(in->tile_obs, in->ntiles, nn_pos);
    if(fabsf(mine - theirs) > HUG_WALL_MARGIN)
        return mine < theirs;
    return in->ent_uid < in->nn_uid;
}

/* Keep what runs along the wall, drop the sideways detour. */
static vec2_t hug_wall_vpref(vec2_t vpref, vec2_t vdes, int side)
{
    vec2_t vhat;
    PFM_Vec2_Normal(&vdes, &vhat);

    vec2_t lat = (vec2_t){-vhat.z * side, vhat.x * side};
    float along = PFM_Vec2_Dot(&vpref, &lat);
    if(along <= 0.0f)
        return vpref;

    PFM_Vec2_Scale(&lat, along, &lat);
    PFM_Vec2_Sub(&vpref, &lat, &vpref);
    return vpref;
}

/* Standing is a real answer in a crowd and a wedge in a wall pocket, so the
 * two are told apart by how long it persists, not here.
 */
static bool solve_stalled(vec2_t new_vel, vec2_t vpref)
{
    return PFM_Vec2_Len(&new_vel) < CLEARPATH_STALL_SPEED
        && PFM_Vec2_Len(&vpref) >= CLEARPATH_STALL_SPEED;
}

static void move_velocity_work(int begin_idx, int end_idx)
{
    for(int i = begin_idx; i <= end_idx; i++) {
    
        struct move_work_in *in = &s_move_work.in[i];
        struct move_work_out *out = &s_move_work.out[i];
        struct move_trace *tr = s_move_work.trace ? &s_move_work.trace[i] : NULL;
        bool traced = tr && tr->traced;

        const struct movestate *ms = movestate_get(in->ent_uid);

        /* COMBAT_HELD, and a unit parked on its cell: keep the move state/cell
         * but zero velocity so the unit holds position. Steering toward a cell
         * it already stands on is an undamped spring, and holding a unit in
         * that state would have it orbit rather than stand.
         */
        if((G_FlagsGetFrom(s_move_work.gamestate.flags, in->ent_uid) & ENTITY_FLAG_COMBAT_HELD)
        || movestate_aux_get(in->ent_uid)->parked) {
            out->ent_uid = in->ent_uid;
            out->ent_vel = (vec2_t){0.0f, 0.0f};
            out->cp_flags = 0;
            out->cp_side = 0;
            continue;
        }

        /* Holds its ground; neighbours still gathered for the release test. */
        if(movestate_aux_get(in->ent_uid)->soft_blocking) {
            find_neighbours(in->ent_uid, in->dyn_neighbs, &in->ndyn,
                in->stat_neighbs, &in->nstat, &in->njam, &in->nn_uid, &in->nn_dist);
            out->ent_uid = in->ent_uid;
            out->ent_vel = (vec2_t){0.0f, 0.0f};
            out->cp_flags = 0;
            out->cp_side = 0;
            continue;
        }

        const struct flock *flock = in->flock;

        uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, in->ent_uid);
        bool relax = movestate_aux_get(in->ent_uid)->cp_relax_ticks > 0;
        struct cp_terrain terrain = (struct cp_terrain){
            .map = s_move_work.gamestate.map,
            .layer = Entity_NavLayerWithRadius(flags, in->cp_ent.radius),
            .max_step = ms->max_speed / hz_count(s_move_work.hz),
            .tile_horizon = relax ? CLEARPATH_TILE_HORIZON_SEC * hz_count(s_move_work.hz)
                                  : 0.0f,
        };
        terrain.on_blocked = movestate_aux_get(in->ent_uid)->phasing
            || M_NavPositionBlocked(terrain.map, terrain.layer, in->cp_ent.xz_pos);

        /* Walls in the solve: a unit standing on a blocked tile is escaping
         * and gets none. */
        if(!terrain.on_blocked && !(flags & ENTITY_FLAG_AIR)) {
            int nterrain = 0;
            in->ntiles = M_NavBlockedTilesAround(terrain.map, terrain.layer,
                in->cp_ent.xz_pos,
                CLEARPATH_TILE_RADIUS + terrain.max_step + CLEARPATH_TILE_MARGIN,
                in->tile_obs, CLEARPATH_MAX_TILE_OBS, &nterrain);
            in->nterrain = nterrain;
        }

        /* The passing side the unit is committed to, in cp_side_of's sense:
         * the remembered deflection, but never toward a nearby wall. */
        int side = in->cp_side ? in->cp_side : 1;
        if(in->ntiles > 0 && PFM_Vec2_Len(&in->ent_des_v) > EPSILON) {
            vec2_t centroid = (vec2_t){0.0f, 0.0f};
            for(size_t t = 0; t < in->ntiles; t++) {
                PFM_Vec2_Add(&centroid, &in->tile_obs[t], &centroid);
            }
            PFM_Vec2_Scale(&centroid, 1.0f / in->ntiles, &centroid);
            PFM_Vec2_Sub(&centroid, &in->cp_ent.xz_pos, &centroid);
            float cr = in->ent_des_v.x * centroid.z - in->ent_des_v.z * centroid.x;
            if(fabsf(cr) > EPSILON) {
                side = (cr > 0.0f) ? -1 : 1;
            }
        }

        /* Compute the preferred velocity */
        vec2_t vpref = (vec2_t){NAN, NAN};
        switch(ms->state) {
        case STATE_TURNING:
            vpref = (vec2_t){0.0f, 0.0f};
            break;
        case STATE_SEEK_ENEMIES: 
            assert(!flock);
            vpref = enemy_seek_vpref(in->ent_uid, in->speed, in->ent_des_v, in->seek_pinned);
            break;
        case STATE_FLEEING:
            vpref = flee_vpref(in->ent_uid, in->speed, in->ent_des_v);
            break;
        case STATE_ARRIVING_TO_CELL:
            assert(flock);
            if(!in->fstate.assignment_ready) {
                vpref = (vec2_t){0.0f, 0.0f};
                break;
            }
            vpref = cell_arrival_seek_vpref(in->ent_uid, in->cell_pos, in->speed,
                in->ent_des_v,
                in->fstate.normal_cohesion_force,
                in->fstate.normal_align_force,
                in->fstate.normal_drag_force);
            break;
        case STATE_MOVING_IN_FORMATION:
            assert(flock);
            if(!in->fstate.assignment_ready) {
                vpref = (vec2_t){0.0f, 0.0f};
                break;
            }
            vpref = formation_seek_vpref(in->ent_uid, flock, in->speed, 
                in->ent_des_v,
                in->fstate.normal_cohesion_force,
                in->fstate.normal_align_force,
                in->fstate.normal_drag_force,
                in->has_dest_los);
            break;
        default:
            assert(flock);
            vpref = point_seek_vpref(in->ent_uid, flock, in->fsnap,
                in->ent_des_v, in->has_dest_los, in->speed, side);
        }
        assert(vpref.x == vpref.x && vpref.z == vpref.z); /* a NaN vpref would corrupt the integration */

        /* Find the entity's neighbours */
        find_neighbours(in->ent_uid, in->dyn_neighbs, &in->ndyn,
            in->stat_neighbs, &in->nstat, &in->njam, &in->nn_uid, &in->nn_dist);

        in->hug = ent_hugs_wall(in);
        if(in->hug) {
            vpref = hug_wall_vpref(vpref, in->ent_des_v, side);
        }

        if(traced) {
            move_trace_velocity(in, tr, vpref);
        }

        /* Capture the inputs before the retry loop compacts the arrays. */
        int cap_slot = -1;
        if(s_log_cp_captures
        && (traced || (in->ndyn + in->nstat) >= CP_CAPTURE_MIN_NEIGHBS)) {
            int slot = SDL_AtomicAdd(&s_cp_ncaptures, 1);
            if(slot < CP_CAPTURE_MAX_PER_TICK) {
                cap_slot = slot;
                struct cp_capture *cap = &s_cp_captures[slot];
                cap->uid = in->ent_uid;
                cap->self = in->cp_ent;
                cap->vpref = vpref;
                cap->ndyn = in->ndyn;
                cap->nstat = in->nstat;
                memcpy(cap->dyn, in->dyn_neighbs, in->ndyn * sizeof(struct cp_ent));
                memcpy(cap->stat, in->stat_neighbs, in->nstat * sizeof(struct cp_ent));
            }
        }

        /* Compute the velocity constrainted by potential collisions */
        struct cp_solve_diag diag;
        vec2_t new_vel = G_ClearPath_NewVelocity(in->cp_ent, in->ent_uid,
            vpref, in->dyn_neighbs, in->ndyn, in->stat_neighbs, in->nstat,
            in->tile_obs, in->ntiles,
            terrain, side, relax, in->save_debug, &diag);

        out->ent_uid = in->ent_uid;
        out->ent_vel = new_vel;
        out->cp_side = diag.side;
        out->cp_flags = (diag.gave_up ? CP_OUT_GAVE_UP : 0)
                      | (!diag.gave_up && diag.retries > 0 ? CP_OUT_RETRY_OK : 0)
                      | (solve_stalled(new_vel, vpref) ? CP_OUT_STALLED : 0)
                      | (ms->state == STATE_SEEK_ENEMIES
                         && PFM_Vec2_Len(&in->ent_des_v) < EPSILON ? CP_OUT_SEEK_VDES0 : 0);
        if(cap_slot >= 0) {
            s_cp_captures[cap_slot].gave_up = diag.gave_up;
        }
        if(traced) {
            tr->retries = diag.retries;
        }
        vec2_truncate(&out->ent_vel, ms->max_speed / hz_count(s_move_work.hz));
    }
}

static void move_update_work(int begin_idx, int end_idx)
{
    for(int i = begin_idx; i <= end_idx; i++) {
    
        struct move_work_in *in = &s_move_work.in[i];
        struct move_work_out *out = &s_move_work.out[i];

        entity_compute_update(s_move_work.hz, out->ent_uid, out->ent_vel, 
            out->ent_des_v, in, &out->patch);
    }
}

static struct result move_dv_task(void *arg)
{
    uint64_t t0 = SDL_GetPerformanceCounter();
    struct move_task_arg *move_arg = arg;
    size_t ncomputed = 0;

    for(int i = move_arg->begin_idx; i <= move_arg->end_idx; i++) {

        struct move_work_in *in = &s_move_work.in[i];
        in->ent_des_v = ent_desired_velocity(in);
        s_move_work.out[i].ent_des_v = in->ent_des_v;
        ncomputed++;

        if(ncomputed % 16 == 0)
            Task_Yield();
    }
    Perf_NavParallelAddSince(t0);
    return NULL_RESULT;
}

static struct result move_velocity_task(void *arg)
{
    uint64_t t0 = SDL_GetPerformanceCounter();
    struct move_task_arg *move_arg = arg;
    size_t ncomputed = 0;

    for(int i = move_arg->begin_idx; i <= move_arg->end_idx; i++) {

        move_velocity_work(i, i);
        ncomputed++;

        if(ncomputed % 16 == 0)
            Task_Yield();
    }
    Perf_NavParallelAddSince(t0);
    return NULL_RESULT;
}

static struct result move_update_task(void *arg)
{
    uint64_t t0 = SDL_GetPerformanceCounter();
    struct move_task_arg *move_arg = arg;
    size_t ncomputed = 0;

    for(int i = move_arg->begin_idx; i <= move_arg->end_idx; i++) {

        move_update_work(i, i);
        ncomputed++;

        if(ncomputed % 16 == 0)
            Task_Yield();
    }
    Perf_NavParallelAddSince(t0);
    return NULL_RESULT;
}

static void move_complete_cpu_work(void)
{
    Sched_AwaitAll(s_move_work.tids, s_move_work.futures, s_move_work.ntasks);
    s_move_work.ntasks = 0;
}

static void move_complete_gpu_velocity_work(void)
{
    Task_RescheduleOnMain();
    ASSERT_IN_MAIN_THREAD();

    size_t nwork = s_move_work.nwork;
    size_t attr_buffsize = nwork * sizeof(vec2_t);

    R_PushCmd((struct rcmd){
        .func = R_GL_MoveReadNewVelocities,
        .nargs = 3,
        .args = {
            [0] = s_move_work.gpu_velocities,
            [1] = R_PushArg(&nwork, sizeof(size_t)),
            [2] = R_PushArg(&attr_buffsize, sizeof(size_t))
        }
    });

    R_PushCmd((struct rcmd){
        .func = R_GL_MoveInvalidateData,
        .nargs = 0
    });

    R_PushCmd((struct rcmd){
        .func = R_GL_PositionsInvalidateData,
        .nargs = 0
    });
}

static khash_t(aabb) *move_update_aabb_cache(void)
{
    PERF_ENTER();
    const khash_t(entity) *ents = G_GetAllEntsSet();

    if(kh_size(s_aabb_cache) > 2 * kh_size(ents) + 1024)
        kh_clear(aabb, s_aabb_cache);

    uint32_t uid;
    kh_foreach_key(ents, uid, {
        khiter_t k = kh_get(aabb, s_aabb_cache, uid);
        if(k == kh_end(s_aabb_cache)) {
            int ret;
            k = kh_put(aabb, s_aabb_cache, uid, &ret);
            assert(ret != -1);
            kh_value(s_aabb_cache, k) = AL_EntityGet(uid)->identity_aabb;
        }
    });
    PERF_RETURN(s_aabb_cache);
}

static void move_init_nav_unit_query_ctx(void)
{
    s_move_work.unit_query_ctx.flags = s_move_work.gamestate.flags;
    s_move_work.unit_query_ctx.positions = s_move_work.gamestate.positions;
    s_move_work.unit_query_ctx.postree = s_move_work.gamestate.postree;
    s_move_work.unit_query_ctx.faction_ids = s_move_work.gamestate.faction_ids;
    s_move_work.unit_query_ctx.aabbs = s_move_work.gamestate.aabbs;
    s_move_work.unit_query_ctx.transforms = s_move_work.gamestate.transforms;
    s_move_work.unit_query_ctx.sel_radiuses = s_move_work.gamestate.sel_radiuses;
    s_move_work.unit_query_ctx.fog_enabled = s_move_work.gamestate.fog_enabled;
    s_move_work.unit_query_ctx.fog_state = s_move_work.gamestate.fog_state;
    s_move_work.unit_query_ctx.dying_set = s_move_work.gamestate.dying_set;
    s_move_work.unit_query_ctx.diptable = (int(*)[MAX_FACTIONS])s_move_work.gamestate.diptable;
    s_move_work.unit_query_ctx.player_controllable = s_move_work.gamestate.player_controllable;
}

static void refcounted_map_destroy(void *owner)
{
    /* Runs on whichever thread drops the last reference; PF_FREE is thread-safe. */
    struct refcounted_map *rmap = owner;
    M_AL_FreeSnapshotShared(rmap->snapshot);
    PF_FREE(rmap);
}

struct refcounted_map *G_Move_NavSnapshotAcquire(void)
{
    ASSERT_IN_MAIN_THREAD();
    if(!s_nav_snapshot)
        return NULL;
    /* No resurrection race: main-thread-only, and s_nav_snapshot always holds
     * movement's reference, so refcount >= 1 here (workers only release).
     */
    sp_retain(s_nav_snapshot);
    return s_nav_snapshot;
}

static void move_copy_gamestate(void)
{
    PERF_ENTER();
    s_move_work.gamestate.flags = G_FlagsCopyTableInto(s_move_work.gamestate.flags);
    s_move_work.gamestate.positions = G_Pos_CopyTableInto(s_move_work.gamestate.positions);
    s_move_work.gamestate.postree = G_Pos_CopyBitmapGridInto(s_move_work.gamestate.postree);
    s_move_work.gamestate.sel_radiuses =
        G_SelectionRadiusCopyTableInto(s_move_work.gamestate.sel_radiuses);
    s_move_work.gamestate.faction_ids =
        G_FactionIDCopyTableInto(s_move_work.gamestate.faction_ids);
    s_move_work.gamestate.ent_gpu_id_map =
        G_CopyEntGPUIDMapInto(s_move_work.gamestate.ent_gpu_id_map);
    s_move_work.gamestate.gpu_id_ent_map =
        G_CopyGPUIDEntMapInto(s_move_work.gamestate.gpu_id_ent_map);
    struct refcounted_map *snap = PF_MALLOC(sizeof(struct refcounted_map));
    snap->snapshot = M_AL_SnapshotShared(s_map);
    sp_init(snap, refcounted_map_destroy);
    s_nav_snapshot = snap;
    s_move_work.gamestate.map = snap->snapshot;
    s_move_work.gamestate.transforms =
        Entity_CopyTransformsInto(s_move_work.gamestate.transforms);
    s_move_work.gamestate.aabbs = move_update_aabb_cache();
    s_move_work.gamestate.fog_enabled = G_Fog_Enabled();
    s_move_work.gamestate.fog_state =
        G_Fog_CopyStateInto(s_move_work.gamestate.fog_state, &s_fog_snap_ntiles);
    s_move_work.gamestate.dying_set = G_Combat_GetDyingSetCopy();
    s_move_work.gamestate.diptable = G_CopyDiplomacyTable();
    s_move_work.gamestate.player_controllable = G_GetPlayerControlledFactions();

    move_init_nav_unit_query_ctx();
    M_NavSetNavUnitQueryCtx(s_move_work.gamestate.map, &s_move_work.unit_query_ctx);

    PERF_RETURN_VOID();
}

static void move_destroy_gamestate(void)
{
    PERF_ENTER();
    if(s_move_work.gamestate.flags) {
        kh_destroy(id, s_move_work.gamestate.flags);
        s_move_work.gamestate.flags = NULL;
    }
    if(s_move_work.gamestate.positions) {
        kh_destroy(pos, s_move_work.gamestate.positions);
        s_move_work.gamestate.positions = NULL;
    }
    if(s_move_work.gamestate.postree) {
        G_Pos_DestroyBitmapGrid(s_move_work.gamestate.postree);
        s_move_work.gamestate.postree = NULL;
    }
    if(s_move_work.gamestate.sel_radiuses) {
        kh_destroy(range, s_move_work.gamestate.sel_radiuses);
        s_move_work.gamestate.sel_radiuses = NULL;
    }
    if(s_move_work.gamestate.faction_ids) {
        kh_destroy(id, s_move_work.gamestate.faction_ids);
        s_move_work.gamestate.faction_ids = NULL;
    }
    if(s_move_work.gamestate.ent_gpu_id_map) {
        kh_destroy(id, s_move_work.gamestate.ent_gpu_id_map);
        s_move_work.gamestate.ent_gpu_id_map = NULL;
    }
    if(s_move_work.gamestate.gpu_id_ent_map) {
        kh_destroy(id, s_move_work.gamestate.gpu_id_ent_map);
        s_move_work.gamestate.gpu_id_ent_map = NULL;
    }
    s_move_work.gamestate.map = NULL;
    /* Release before the next tick allocates so the freed block is recycled. */
    if(s_nav_snapshot) {
        sp_release(s_nav_snapshot);
        s_nav_snapshot = NULL;
    }
    if(s_move_work.gamestate.transforms) {
        kh_destroy(trans, s_move_work.gamestate.transforms);
        s_move_work.gamestate.transforms = NULL;
    }
    /* Aliases the persistent cache, which is destroyed separately */
    s_move_work.gamestate.aabbs = NULL;
    if(s_move_work.gamestate.fog_state) {
        PF_FREE(s_move_work.gamestate.fog_state);
        s_move_work.gamestate.fog_state = NULL;
    }
    if(s_move_work.gamestate.dying_set) {
        kh_destroy(id, s_move_work.gamestate.dying_set);
        s_move_work.gamestate.dying_set = NULL;
    }
    if(s_move_work.gamestate.diptable) {
        PF_FREE(s_move_work.gamestate.diptable);
        s_move_work.gamestate.diptable = NULL;
    }
    s_fog_snap_ntiles = 0;
    PERF_RETURN_VOID();
}

/* The copied tables are retained and refilled by the next tick's copy; only
 * the shared and per-tick resources are dropped here.
 */
static void move_release_gamestate(void)
{
    PERF_ENTER();
    s_move_work.gamestate.map = NULL;
    if(s_nav_snapshot) {
        sp_release(s_nav_snapshot);
        s_nav_snapshot = NULL;
    }
    if(s_move_work.gamestate.dying_set) {
        kh_destroy(id, s_move_work.gamestate.dying_set);
        s_move_work.gamestate.dying_set = NULL;
    }
    if(s_move_work.gamestate.diptable) {
        PF_FREE(s_move_work.gamestate.diptable);
        s_move_work.gamestate.diptable = NULL;
    }
    PERF_RETURN_VOID();
}

static void move_update_gamestate(void)
{
    move_release_gamestate();
    move_copy_gamestate();
}

static void move_consume_work_results(void)
{
    PERF_ENTER();

    if(s_move_work.nwork == 0)
        PERF_RETURN_VOID();

    PERF_PUSH("apply movement updates");

    s_soft_block_budget = SEEK_BLOCK_BUDGET_PER_TICK;
    for(int i = 0; i < s_move_work.nwork; i++) {
        struct move_work_out *out = &s_move_work.out[i];
        entity_apply_cp_stall(out->ent_uid, !!(out->cp_flags & CP_OUT_STALLED));
        entity_apply_update(out->ent_uid, &out->patch);
        entity_apply_cp_side(out->ent_uid, out->cp_side);
    }

    /* All this tick's position changes are enqueued; apply the batched fog
     * vision updates in one pipelined pass before any reader runs. */
    G_Fog_FlushUpdates();

    PERF_POP();

    stalloc_clear(&s_move_work.mem);
    s_move_work.in = NULL;
    s_move_work.out = NULL;
    s_move_work.nwork = 0;
    s_move_work.ntasks = 0;

    PERF_RETURN_VOID();
}

static void move_prepare_work(enum movement_hz hz)
{
    size_t ndynamic = kh_size(G_GetDynamicEntsSet());
    s_move_work.in = stalloc(&s_move_work.mem, ndynamic * sizeof(struct move_work_in));
    s_move_work.out = stalloc(&s_move_work.mem, ndynamic * sizeof(struct move_work_out));
    s_move_work.neighb_mem = stalloc(&s_move_work.mem,
        ndynamic * 2 * MAX_NEIGHBOURS * sizeof(struct cp_ent));
    s_move_work.tile_mem = stalloc(&s_move_work.mem,
        ndynamic * CLEARPATH_MAX_TILE_OBS * sizeof(vec2_t));
    s_move_work.trace = (s_move_trace_min_radius >= 0.0f)
                      ? stalloc(&s_move_work.mem, ndynamic * sizeof(struct move_trace))
                      : NULL;
    s_move_work.max_radius = s_max_sel_radius;
    s_move_work.hz = hz;
    s_move_work.type = (s_use_gpu ? WORK_TYPE_GPU : WORK_TYPE_CPU);
    SDL_AtomicSet(&s_move_work.gpu_velocities_ready, 0);
}

static void move_build_flock_snaps(void)
{
    ASSERT_IN_MAIN_THREAD();

    size_t nflocks = vec_size(&s_flocks);
    s_move_work.flock_snaps = stalloc(&s_move_work.mem,
        nflocks * sizeof(struct flock_snap));
    kh_clear(findex, s_flock_index);

    for(int i = 0; i < nflocks; i++) {

        struct flock *fl = &vec_AT(&s_flocks, i);
        size_t nents = kh_size(fl->ents);
        uint32_t *uids = stalloc(&s_move_work.mem, nents * sizeof(uint32_t));
        float *pos_x = stalloc(&s_move_work.mem, nents * sizeof(float));
        float *pos_z = stalloc(&s_move_work.mem, nents * sizeof(float));

        size_t idx = 0;
        uint32_t curr;
        kh_foreach_key(fl->ents, curr, {
            vec2_t pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, curr);
            uids[idx] = curr;
            pos_x[idx] = pos.x;
            pos_z[idx] = pos.z;
            idx++;
            int put_status;
            khiter_t k = kh_put(findex, s_flock_index, curr, &put_status);
            assert(put_status != -1);
            kh_val(s_flock_index, k) = i;
        });
        assert(idx == nents);

        s_move_work.flock_snaps[i] = (struct flock_snap){uids, pos_x, pos_z, nents};
    }
}

static bool ent_selected(uint32_t uid)
{
    for(int i = 0; s_move_trace_sel && i < vec_size(s_move_trace_sel); i++) {
        if(vec_AT(s_move_trace_sel, i) == uid)
            return true;
    }
    return false;
}

static void move_push_work(struct move_work_in in)
{
    size_t idx = s_move_work.nwork++;
    in.dyn_neighbs  = s_move_work.neighb_mem + (2 * idx + 0) * MAX_NEIGHBOURS;
    in.stat_neighbs = s_move_work.neighb_mem + (2 * idx + 1) * MAX_NEIGHBOURS;
    in.tile_obs = s_move_work.tile_mem + idx * CLEARPATH_MAX_TILE_OBS;
    in.ndyn = 0;
    in.nstat = 0;
    in.ntiles = 0;
    in.njam = 0;
    in.nterrain = 0;
    in.nn_uid = NULL_UID;
    in.nn_dist = INFINITY;
    in.hug = false;
    in.field_void = false;
    s_move_work.in[idx] = in;
    if(s_move_work.trace) {
        s_move_work.trace[idx] = (struct move_trace){
            .traced = (in.cp_ent.radius >= s_move_trace_min_radius)
                   || ent_selected(in.ent_uid)
        };
    }
}

static void move_submit_cpu_work(task_func_t code)
{
    if(s_move_work.nwork == 0)
        return;

    size_t ntasks = SDL_GetCPUCount();
    if(s_move_work.nwork < 64)
        ntasks = 1;
    ntasks = MIN(ntasks, MAX_MOVE_TASKS);

    for(int i = 0; i < ntasks; i++) {

        struct move_task_arg *arg = stalloc(&s_move_work.mem, sizeof(struct move_task_arg));
        size_t nitems = ceil((float)s_move_work.nwork / ntasks);

        arg->begin_idx = nitems * i;
        arg->end_idx = MIN(nitems * (i + 1) - 1, s_move_work.nwork-1);

        SDL_AtomicSet(&s_move_work.futures[s_move_work.ntasks].status, FUTURE_INCOMPLETE);
        s_move_work.tids[s_move_work.ntasks] = Sched_Create(4, code, arg,
            "move::work", &s_move_work.futures[s_move_work.ntasks], TASK_BIG_STACK);

        if(s_move_work.tids[s_move_work.ntasks] == NULL_TID) {
            code(arg);
        }else{
            s_move_work.ntasks++;
        }
    }
}

static struct move_work_in *work_input_for_uid(uint32_t uid)
{
    for(int i = 0; i < s_move_work.nwork; i++) {

        struct move_work_in *in = &s_move_work.in[i];
        if(in->ent_uid == uid)
            return in;
    }
    return NULL;
}

static void move_upload_input(size_t nents)
{
    ASSERT_IN_MAIN_THREAD();
    PERF_ENTER();

    /* Setup GPUID dispatch data.
     */
    struct render_workspace *ws = G_GetSimWS();
    const size_t gpuid_buffsize = s_move_work.nwork * sizeof(uint32_t);
    const size_t nactive = s_move_work.nwork;
    void *gpuid_buff = stalloc(&ws->args, gpuid_buffsize);
    unsigned char *cursor = gpuid_buff;

    for(int i = 0; i < s_move_work.nwork; i++) {

        struct move_work_in *in = &s_move_work.in[i];
        uint32_t gpuid = G_GPUIDForEntFrom(s_move_work.gamestate.ent_gpu_id_map, in->ent_uid);
        *((uint32_t*)cursor) = gpuid;
        cursor += sizeof(uint32_t);
    }
    assert(cursor == ((unsigned char*)gpuid_buff) + gpuid_buffsize);

    /* Setup moveattr data.
     */
    const size_t attr_buffsize = nents * sizeof(struct gpu_ent_desc);
    void *attrbuff = stalloc(&ws->args, attr_buffsize);
    cursor = attrbuff;

    for(int gpu_id = 1; gpu_id <= nents; gpu_id++) {

        uint32_t uid = G_EntForGPUIDFrom(s_move_work.gamestate.gpu_id_ent_map, gpu_id);
        const struct movestate *curr = movestate_get(uid);
        assert(curr);

        const struct flock *flock;
        uint32_t flock_id = flock_id_for_ent(uid, &flock);
        uint32_t movestate = curr->state;
        vec2_t pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
        vec2_t dest_xz = flock ? flock->target_xz : (vec2_t){0.0f, 0.0f};

        uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
        float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);

        struct move_work_in *work = NULL;
        if(!ent_still(curr)) {
            work = work_input_for_uid(uid);
        }

        *((struct gpu_ent_desc*)cursor) = (struct gpu_ent_desc){
            .dest = dest_xz,
            .vdes = work ? work->ent_des_v : (vec2_t){0},
            .cell_pos = work ? work->cell_pos : (vec2_t){0},
            .formation_cohesion_force  = work ? work->fstate.normal_cohesion_force : (vec2_t){0},
            .formation_align_force = work ? work->fstate.normal_align_force : (vec2_t){0},
            .formation_drag_force = work ? work->fstate.normal_drag_force : (vec2_t){0},
            .pos = pos,
            .velocity = curr->velocity,
            .movestate = curr->state,
            .flock_id = flock_id,
            .flags = flags,
            .speed = work ? work->speed : 0.0f,
            .max_speed = curr->max_speed,
            .radius = radius,
            .layer = Entity_NavLayerWithRadius(flags, radius),
            .has_dest_los = work ? work->has_dest_los : false,
            .formation_assignment_ready = work ? work->fstate.assignment_ready : 0,
        };
        cursor += sizeof(struct gpu_ent_desc);
    }
    assert(cursor == ((unsigned char*)attrbuff) + attr_buffsize);

    /* Setup flock data.
     */
    const size_t nflocks = vec_size(&s_flocks);
    const size_t flock_buffsize = nflocks * sizeof(struct gpu_flock_desc);
    void *flockbuff = stalloc(&ws->args, flock_buffsize);
    cursor = flockbuff;

    for(int i = 0; i < nflocks; i++) {

        struct flock *curr_flock = &vec_AT(&s_flocks, i);            
        uint32_t uid;
        size_t nents = 0;
        unsigned char *tmp = cursor;

        kh_foreach_key(curr_flock->ents, uid, {

            uint32_t gpuid = G_GPUIDForEntFrom(s_move_work.gamestate.ent_gpu_id_map, uid);
            *((uint32_t*)tmp) = gpuid; 
            tmp += sizeof(uint32_t);

            if(++nents == MAX_GPU_FLOCK_MEMBERS)
                break;
        });
        cursor += MAX_GPU_FLOCK_MEMBERS * sizeof(uint32_t);

        assert(nents == MIN(kh_size(curr_flock->ents), MAX_GPU_FLOCK_MEMBERS));
        *((uint32_t*)cursor) = (uint32_t)nents; 
        cursor += sizeof(uint32_t);

        *((vec2_t*)cursor) = curr_flock->target_xz;
        cursor += sizeof(vec2_t);
    }
    assert(cursor == ((unsigned char*)flockbuff) + flock_buffsize);

    /* Setup navigation data.
     */
    const size_t cost_base_buffsize = M_NavCostBaseBufferSize(s_move_work.gamestate.map);
    void *cost_base_buff = stalloc(&ws->args, cost_base_buffsize);
    M_NavCopyCostBasePacked(s_move_work.gamestate.map, cost_base_buff, cost_base_buffsize);

    const size_t blockers_buffsize = M_NavBlockersBufferSize(s_move_work.gamestate.map);
    void *blockers_buff = stalloc(&ws->args, blockers_buffsize);
    M_NavCopyBlockersPacked(s_move_work.gamestate.map, blockers_buff, blockers_buffsize);

    /* Upload everything.
     */
    R_PushCmd((struct rcmd){
        .func = R_GL_MoveUploadData,
        .nargs = 10,
        .args = {
            gpuid_buff,
            R_PushArg(&nactive, sizeof(nactive)),
            attrbuff,
            R_PushArg(&attr_buffsize, sizeof(attr_buffsize)),
            flockbuff,
            R_PushArg(&flock_buffsize, sizeof(flock_buffsize)),
            cost_base_buff,
            R_PushArg(&cost_base_buffsize, sizeof(cost_base_buffsize)),
            blockers_buff,
            R_PushArg(&blockers_buffsize, sizeof(blockers_buffsize)),
        },
    });

    PERF_RETURN_VOID();
}

static void move_update_uniforms(void)
{
    struct map_resolution res;
    M_GetResolution(s_move_work.gamestate.map, &res);
    vec3_t map_pos = M_GetPos(s_move_work.gamestate.map);
    vec2_t map_pos_xz = (vec2_t){map_pos.x, map_pos.z};
    int ticks = hz_count(s_move_work.hz);
    int nwork = s_move_work.nwork;

    R_PushCmd((struct rcmd){
        .func = R_GL_MoveUpdateUniforms,
        .nargs = 4,
        .args = {
            R_PushArg(&res, sizeof(res)),
            R_PushArg(&map_pos_xz, sizeof(map_pos_xz)),
            R_PushArg(&ticks, sizeof(ticks)),
            R_PushArg(&nwork, sizeof(nwork)),
        },
    });
}

static void move_submit_gpu_velocity_work(void)
{
    assert(Sched_ActiveTID() != NULL_TID);
    Task_RescheduleOnMain();

    size_t nents = G_Pos_UploadFrom(s_move_work.gamestate.positions,
        s_move_work.gamestate.ent_gpu_id_map,
        s_move_work.gamestate.map);
    assert(nents == kh_size(s_entity_state_table));

    move_upload_input(nents);
    move_update_uniforms();

    R_PushCmd((struct rcmd){
        .func = R_GL_MoveDispatchWork,
        .nargs = 1,
        .args = R_PushArg(&s_move_work.nwork, sizeof(s_move_work.nwork))
    });
    Task_Yield();
}

static void nav_tick_submit_work(void)
{
    ASSERT_IN_MAIN_THREAD();

    if(s_move_work.type == WORK_TYPE_GPU) {
        size_t nwork = s_move_work.nwork;
        size_t size = nwork * sizeof(vec2_t);
        s_move_work.gpu_velocities = stalloc(&s_move_work.mem, size);
    }

    SDL_AtomicSet(&s_tick_task_future.status, FUTURE_INCOMPLETE);
    s_tick_task_tid = Sched_Create(0, navigation_tick_task, NULL, 
            "navigation_tick_task", &s_tick_task_future, TASK_BIG_STACK);
    assert(s_tick_task_tid != NULL_TID);
    s_last_tick = g_frame_idx;
}

static unsigned move_trace_flags(const struct move_work_in *in, const struct move_work_out *out,
                                 const struct movestate *ms, const struct movestate_aux *aux)
{
    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, in->ent_uid);
    unsigned ret = 0;
    ret |= (out->patch.flags & UPDATE_HEADING_GATED)   ? MV_TRACE_HEADING_GATED   : 0;
    ret |= (out->cp_flags & CP_OUT_GAVE_UP)            ? MV_TRACE_CP_GAVE_UP      : 0;
    ret |= (out->cp_flags & CP_OUT_RETRY_OK)           ? MV_TRACE_CP_RETRY_OK     : 0;
    ret |= (out->cp_flags & CP_OUT_STALLED)            ? MV_TRACE_CP_STALLED      : 0;
    ret |= in->hug                                     ? MV_TRACE_HUGGING         : 0;
    ret |= (out->patch.flags & UPDATE_VETO_UNPATHABLE) ? MV_TRACE_VETO_UNPATHABLE : 0;
    ret |= (out->patch.flags & UPDATE_VETO_BLOCKED)    ? MV_TRACE_VETO_BLOCKED    : 0;
    ret |= aux->soft_blocking                          ? MV_TRACE_SOFT_BLOCKING   : 0;
    ret |= (out->patch.flags & UPDATE_SEEK_STUCK)      ? MV_TRACE_SEEK_STUCK      : 0;
    ret |= (out->patch.flags & UPDATE_SEEK_CLEAR)      ? MV_TRACE_SEEK_CLEAR      : 0;
    ret |= in->has_dest_los                            ? MV_TRACE_HAS_DEST_LOS    : 0;
    ret |= in->field_starved                           ? MV_TRACE_FIELD_STARVED   : 0;
    ret |= ms->using_surround_field                    ? MV_TRACE_SURROUND_FIELD  : 0;
    ret |= (flags & ENTITY_FLAG_COMBAT_HELD)           ? MV_TRACE_COMBAT_HELD     : 0;
    return ret;
}

/* Runs between the drain and the apply: in/out and the pre-apply movestates
 * describe the tick just computed.
 */
static void move_trace_emit(void)
{
    if(s_move_trace_tick % 20 == 0 && vec_size(s_move_trace_sel) > 0) {
        fprintf(stdout, "[mv-sel] %u", s_move_trace_tick);
        for(int i = 0; i < vec_size(s_move_trace_sel); i++) {
            fprintf(stdout, ",%u", vec_AT(s_move_trace_sel, i));
        }
        fputc('\n', stdout);
    }

    for(size_t i = 0; i < s_move_work.nwork; i++) {

        const struct move_trace *tr = &s_move_work.trace[i];
        if(!tr->traced)
            continue;

        const struct move_work_in *in = &s_move_work.in[i];
        const struct move_work_out *out = &s_move_work.out[i];
        uint32_t uid = in->ent_uid;
        const struct movestate *ms = movestate_get(uid);
        const struct movestate_aux *aux = movestate_aux_get(uid);
        if(!ms || !aux)
            continue;

        float radius = in->cp_ent.radius;
        uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
        int layer = Entity_NavLayerWithRadius(flags, radius);
        vec2_t app = (out->patch.flags & UPDATE_SET_VELOCITY) ? out->patch.next_velocity
                                                              : (vec2_t){0.0f, 0.0f};
        vec2_t face = facing_dir(ms->next_rot);

        fprintf(stdout, "[mv-trace] %u,%u,%.2f,%d,%s,%.3f,%.3f,%.2f,"
            "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
            "%u,%d,%u,%u,%u,%.3f,%.2f,%d,%.4f,%.4f,%d,%d,%u\n",
            s_move_trace_tick, uid, radius, layer, s_state_str[ms->state],
            in->cp_ent.xz_pos.x, in->cp_ent.xz_pos.z, in->speed,
            in->ent_des_v.x, in->ent_des_v.z, tr->vpref.x, tr->vpref.z,
            out->ent_vel.x, out->ent_vel.z, app.x, app.z, face.x, face.z,
            move_trace_flags(in, out, ms, aux), tr->retries,
            (unsigned)in->ndyn, (unsigned)in->nstat,
            tr->nn_uid, tr->nn_dist, tr->nn_radius, (int)tr->nn_seen,
            tr->sep.x, tr->sep.z, tr->sep_n, out->cp_side, (unsigned)in->ntiles);

        if(tr->probed) {
            fprintf(stdout, "[mv-probe] %u,%u,%d,%d,%d,%d,%u", s_move_trace_tick, uid,
                tr->td.chunk_r, tr->td.chunk_c, tr->td.tile_r, tr->td.tile_c, tr->probe);
            for(int i = 0; i < 9; i++) {
                fprintf(stdout, ",%d", tr->dirs[i]);
            }
            fputc('\n', stdout);
        }
    }
}

static enum move_work_status nav_tick_finish_work(void)
{
    if(s_tick_task_tid == NULL_TID) {
        return WORK_COMPLETE;
    }

    PERF_PUSH("nav tick drain");
    uint64_t drain_start = SDL_GetPerformanceCounter();
    while(!Sched_FutureIsReady(&s_tick_task_future)) {
        /* If the task is event-blocked waiting for GPU results,
         * we are not able to run it to completion at this point.
         */
        if(!Sched_RunSync(s_tick_task_tid)) {
            PERF_POP();
            return WORK_INCOMPLETE;
        }
    }
    PERF_POP();
    s_tick_task_tid = NULL_TID;

    /* s_move_work.{hz,nwork} still hold the just-completed task's values here. */
    s_last_nav_tick_stats.drain_us  = perf_ticks_to_us(SDL_GetPerformanceCounter() - drain_start);
    s_last_nav_tick_stats.nwork     = (uint32_t)s_move_work.nwork;
    s_last_nav_tick_stats.budget_us = 1000000u / hz_count(s_move_work.hz);

    struct nav_tick_diag diag;
    M_NavGetTickDiag(s_move_work.gamestate.map, &diag);
    s_last_nav_tick_stats.nenemy_built    = diag.enemy_built;
    s_last_nav_tick_stats.nzone_built     = diag.zone_built;
    s_last_nav_tick_stats.nsurround_built = diag.surround_built;
    s_last_nav_tick_stats.ninval_enemy    = diag.inval_enemy;
    s_last_nav_tick_stats.ninval_surround = diag.inval_surround;
    s_last_nav_tick_stats.nsvc_sync       = diag.svc_sync;
    s_last_nav_tick_stats.nsvc_patch      = diag.svc_patch;
    s_last_nav_tick_stats.nastar          = diag.nastar;
    s_last_nav_tick_stats.nastar_memo     = diag.nastar_memo;
    s_last_nav_tick_stats.npseek_built    = diag.pseek_built;

    /* The out array is consumed at the next tick's start; reducing the
     * per-solve diagnostics here is race-free. */
    s_last_nav_tick_stats.ncp_zero = 0;
    s_last_nav_tick_stats.ncp_retry_ok = 0;
    s_last_nav_tick_stats.ncp_stalled = 0;
    s_last_nav_tick_stats.nseek_vdes0 = 0;
    s_last_nav_tick_stats.nheading_gated = 0;
    for(size_t i = 0; i < s_move_work.nwork; i++) {
        const struct move_work_out *out = &s_move_work.out[i];
        s_last_nav_tick_stats.ncp_zero       += !!(out->cp_flags & CP_OUT_GAVE_UP);
        s_last_nav_tick_stats.ncp_retry_ok   += !!(out->cp_flags & CP_OUT_RETRY_OK);
        s_last_nav_tick_stats.ncp_stalled    += !!(out->cp_flags & CP_OUT_STALLED);
        s_last_nav_tick_stats.nseek_vdes0    += !!(out->cp_flags & CP_OUT_SEEK_VDES0);
        s_last_nav_tick_stats.nheading_gated += !!(out->patch.flags & UPDATE_HEADING_GATED);
    }

    int ncaps = MIN(SDL_AtomicGet(&s_cp_ncaptures), CP_CAPTURE_MAX_PER_TICK);
    for(int i = 0; i < ncaps; i++) {
        const struct cp_capture *cap = &s_cp_captures[i];
        fprintf(stdout, "[cp-cap] %u,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%u,%u",
            cap->uid, (int)cap->gave_up, cap->self.xz_pos.x, cap->self.xz_pos.z,
            cap->self.xz_vel.x, cap->self.xz_vel.z, cap->self.radius,
            cap->vpref.x, cap->vpref.z, cap->ndyn, cap->nstat);
        for(int j = 0; j < cap->ndyn + cap->nstat; j++) {
            const struct cp_ent *n = (j < cap->ndyn) ? &cap->dyn[j]
                                                     : &cap->stat[j - cap->ndyn];
            fprintf(stdout, ",%.4f,%.4f,%.4f,%.4f,%.4f",
                n->xz_pos.x, n->xz_pos.z, n->xz_vel.x, n->xz_vel.z, n->radius);
        }
        fputc('\n', stdout);
    }

    if(s_move_work.trace) {
        move_trace_emit();
    }
    s_move_trace_tick++;

    Perf_RecordNavTick(&s_last_nav_tick_stats);

    return WORK_COMPLETE;
}

static enum movement_hz event_to_hz(enum eventtype event)
{
    static const enum movement_hz mapping[] = {
        [EVENT_20HZ_TICK] = MOVE_HZ_20,
        [EVENT_10HZ_TICK] = MOVE_HZ_10,
        [EVENT_5HZ_TICK] = MOVE_HZ_5,
        [EVENT_1HZ_TICK] = MOVE_HZ_1,
    };
    return mapping[event];
}

static enum eventtype event_for_hz(enum movement_hz hz)
{
    assert(hz >= 0 && hz <= MOVE_HZ_1);
    static const enum eventtype mapping[] = {
        [MOVE_HZ_20] = EVENT_20HZ_TICK,
        [MOVE_HZ_10] = EVENT_10HZ_TICK,
        [MOVE_HZ_5 ] = EVENT_5HZ_TICK,
        [MOVE_HZ_1 ] = EVENT_1HZ_TICK,
    };
    return mapping[hz];
}

static void register_callback_for_hz(enum movement_hz hz)
{
    enum eventtype event = event_for_hz(hz);
    E_Global_Register(event, move_tick, (void*)(uintptr_t)event, G_RUNNING);
}

static void unregister_callback_for_hz(enum movement_hz hz)
{
    assert(hz >= 0 && hz <= MOVE_HZ_1);
    enum eventtype event = event_for_hz(hz);
    E_Global_Unregister(event, move_tick);
}

static void move_handle_hz_update(enum eventtype curr)
{
    if(!s_move_hz_dirty)
        return;

    s_move_hz_dirty = false;

    enum eventtype next = event_for_hz(s_move_hz);

    if(curr == next)
        return;

    enum movement_hz curr_hz = event_to_hz(curr);
    enum movement_hz next_hz = s_move_hz;

    unregister_callback_for_hz(curr_hz);
    register_callback_for_hz(next_hz);
}

static void entity_interpolation_step(uint32_t uid, int steps)
{
    ASSERT_IN_MAIN_THREAD();
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    /* Settled units reject on the already-fetched movestate alone */
    if(ms->left == 0)
        return;

    /* The movestate can outlive the entity by a tick */
    if(!G_EntityExists(uid))
        return;

    /* Garrisoned entities are off the map with their faction ref removed; a
     * G_Pos_Set here would double-remove it (see entity_apply_update). */
    if(G_EntityIsGarrisoned(uid))
        return;

    steps = MIN(steps, ms->left);
    ms->left -= steps;
    float fraction = 1.0 - (ms->step * ms->left);
    assert(fraction >= 0.0f && fraction <= 1.0f);

    vec3_t new_pos = interpolate_positions(ms->prev_pos, ms->next_pos, fraction);
    G_Pos_Set(uid, new_pos);

    quat_t new_rot = interpolate_rotations(ms->prev_rot, ms->next_rot, fraction);
    Entity_SetRot(uid, new_rot);
}

static void interpolate_tick(void *user, void *event)
{
    ASSERT_IN_MAIN_THREAD();

    /* Do not run the interpolation in the same tick as the move tick */
    if(g_frame_idx == s_last_tick)
        return;

    if(s_move_tick_queued)
        return;

    /* Perform a maximum of one interpolation per frame. */
    if(g_frame_idx == s_last_interpolate_tick)
        return;

    /* No need to perform the interpolation if we've got the next movement
     * tick coming right up.
     */
    enum eventtype type = event_for_hz(s_move_hz);
    if(E_QueuedThisFrame(type)) {
        s_last_interpolate_tick = g_frame_idx;
        return;
    }

    PERF_ENTER();
    bool coalese = E_QueuedThisFrame(EVENT_20HZ_TICK);

    /* Iterate over all the entities and advance the position forward
     * by one interpolated step */
    uint32_t key;
    kh_foreach_key(s_entity_state_table, key, {
        /* Coalese together queued updates when possible */
        int steps = coalese ? 2 : 1;
        entity_interpolation_step(key, steps);
    });

    s_last_interpolate_tick = g_frame_idx;
    PERF_RETURN_VOID();
}

static struct result move_los_peek_task(void *arg)
{
    uint64_t t0 = SDL_GetPerformanceCounter();
    struct move_task_arg *move_arg = arg;
    const struct map *map = s_move_work.gamestate.map;
    size_t ncomputed = 0;

    for(int i = move_arg->begin_idx; i <= move_arg->end_idx; i++) {

        struct move_work_in *in = &s_move_work.in[i];
        const struct movestate *ms = movestate_get(in->ent_uid);
        const struct flock *fl = in->flock;
        vec2_t pos = (vec2_t){ms->prev_pos.x, ms->prev_pos.z};

        in->has_dest_los = false;
        in->needs_los_build = false;
        in->los_queried = false;
        in->los_built = false;
        in->los_deferred = false;
        in->field_starved = false;
        if(fl && (ms->state != STATE_SURROUND_ENTITY || !ms->using_surround_field)) {
            bool present;
            bool vis = M_NavHasDestLOSCached(map, fl->dest_id, pos, &present);
            in->has_dest_los = present && vis;
            in->needs_los_build = !present;
            in->los_queried = true;
        }
        ncomputed++;

        if(ncomputed % 16 == 0)
            Task_Yield();
    }
    Perf_NavParallelAddSince(t0);
    return NULL_RESULT;
}

static void compute_los_state(void)
{
    PERF_ENTER();

    /* Parallel read: peek each unit's cached LOS read-only and flag the misses. */
    uint64_t phase_start = SDL_GetPerformanceCounter();
    move_submit_cpu_work(move_los_peek_task);
    move_complete_cpu_work();
    s_last_nav_tick_stats.los_peek_us =
        perf_ticks_to_us(SDL_GetPerformanceCounter() - phase_start);

    /* Serial record: route the flagged units' misses through the path request,
     * which defers the LOS floods to the worker pool as per-destination chains.
     */
    phase_start = SDL_GetPerformanceCounter();
    const struct map *map = s_move_work.gamestate.map;
    unsigned checked = 0, misses = 0;
    size_t nwork = s_move_work.nwork;
    for(size_t k = 0; k < nwork; k++) {

        size_t idx = (s_rebuild_start + k) % nwork;
        struct move_work_in *in = &s_move_work.in[idx];
        if(in->los_queried)
            checked++;
        if(!in->needs_los_build)
            continue;

        const struct movestate *ms = movestate_get(in->ent_uid);
        const struct flock *fl = in->flock;
        vec2_t pos = (vec2_t){ms->prev_pos.x, ms->prev_pos.z};

        /* The needs_los_build flag is a snapshot from the parallel peek: an
         * earlier build this tick may have already cached this (dest, chunk)
         * LOS field. A hit here costs no rebuild budget. */
        bool present;
        bool vis = M_NavHasDestLOSCached(map, fl->dest_id, pos, &present);
        if(present) {
            in->has_dest_los = vis;
            continue;
        }

        /* Neither does an already-recorded chain build */
        if(M_NavDestLOSPending(map, fl->dest_id, pos)) {
            in->los_deferred = true;
            continue;
        }
        misses++;

        if(s_rebuild_budget == 0) {
            if(s_first_starved == (size_t)-1)
                s_first_starved = idx;
            continue;
        }
        s_rebuild_budget--;

        switch(M_NavEnsureDestLOS(map, fl->dest_id, pos, fl->target_xz, &vis)) {
        case LOS_ENSURE_ANSWER:
            in->has_dest_los = vis;
            break;
        case LOS_ENSURE_DEFERRED:
            in->los_deferred = true;
            break;
        case LOS_ENSURE_FAIL:
            in->has_dest_los = false;
            break;
        }
        in->los_built = true;

        Sched_TryYield();
    }

    /* Flood the recorded chains in parallel, publish them at the join, and
     * resolve the answers that waited on them with a cache peek.
     */
    N_DispatchLOSChains();
    N_FinishLOSChains();

    for(size_t i = 0; i < nwork; i++) {

        struct move_work_in *in = &s_move_work.in[i];
        if(!in->los_deferred)
            continue;

        const struct movestate *ms = movestate_get(in->ent_uid);
        vec2_t pos = (vec2_t){ms->prev_pos.x, ms->prev_pos.z};

        bool present;
        bool vis = M_NavHasDestLOSCached(map, in->flock->dest_id, pos, &present);
        in->has_dest_los = present && vis;
    }

    s_last_nav_tick_stats.los_build_us =
        perf_ticks_to_us(SDL_GetPerformanceCounter() - phase_start);
    s_last_nav_tick_stats.nlos_builds = misses;

    M_NavAddLosSamples(map, checked, checked - misses);
    PERF_RETURN_VOID();
}

/* Dispatch the per-entity enemy/surround floods and the per-flock arrival
 * zone floods ahead of the serial LOS phase: they read only the immutable
 * snapshot, so they overlap the record loop and the LOS chain builds, and
 * the join in compute_path_requests finds them largely complete. Returns
 * the dispatch wall time in performance-counter ticks.
 */
static uint64_t dispatch_async_fields(void)
{
    uint64_t phase_start = SDL_GetPerformanceCounter();
    uint64_t last_dispatch_yield = SDL_GetPerformanceCounter();
    const uint64_t dispatch_yield_ticks = SDL_GetPerformanceFrequency() / 500; /* ~2ms */
    for(int i = 0; i < s_move_work.nwork; i++) {
        struct move_work_in *in = &s_move_work.in[i];
        request_async_field(in->ent_uid);
        if(SDL_GetPerformanceCounter() - last_dispatch_yield > dispatch_yield_ticks) {
            Sched_TryYield();
            last_dispatch_yield = SDL_GetPerformanceCounter();
        }
    }
    request_flock_arrival_fields();
    return SDL_GetPerformanceCounter() - phase_start;
}

static void compute_path_requests(uint64_t dispatch_ticks)
{
    PERF_ENTER();

    /* Join the enemy/surround/zone floods dispatched ahead of the LOS phase,
     * along with the flow floods that phase's path requests deferred.
     */
    uint64_t phase_start = SDL_GetPerformanceCounter();
    N_AwaitAsyncFields();

    /* The join clears the pool's arena; the serial loop below re-uses the
     * pool for its deferred flow floods, so it must be re-prepared (arrays
     * are carved from the arena before any per-task args). */
    N_PrepareAsyncWork();
    s_last_nav_tick_stats.cpr_async_us =
        perf_ticks_to_us(dispatch_ticks + (SDL_GetPerformanceCounter() - phase_start));

    /* Serial pre-build of point-seek paths and blocked-tile repairs, which mutate
     * the shared cache and so cannot run in the parallel desired-velocity phase.
     * Yield periodically so the scheduler can still meet its frame budget.
     */
    phase_start = SDL_GetPerformanceCounter();
    const struct map *map = s_move_work.gamestate.map;
    uint64_t last_yield = SDL_GetPerformanceCounter();
    const uint64_t yield_ticks = SDL_GetPerformanceFrequency() / 500; /* ~2ms */
    unsigned checked = 0, rebuilds = 0;
    size_t nwork = s_move_work.nwork;
    for(size_t k = 0; k < nwork; k++) {

        size_t idx = (s_rebuild_start + k) % nwork;
        struct move_work_in *in = &s_move_work.in[idx];
        uint32_t uid = in->ent_uid;
        const struct movestate *ms = movestate_get(uid);
        if(!ms || ms->state == STATE_TURNING || ms->state == STATE_ARRIVING_TO_CELL
        || ms->state == STATE_FLEEING)
            continue;

        vec2_t pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
        struct target t = build_target(uid, in->flock);
        checked++;
        /* Built this tick, by the LOS phase or a request here, counts as a miss. */
        bool rebuilt = in->los_built;
        if(M_NavRequiresPathRequest(map, t, pos)) {
            if(s_rebuild_budget > 0) {
                s_rebuild_budget--;
                M_NavServicePathRequest(map, t, pos);
                rebuilt = true;
            }else{
                in->field_starved = true;
                if(s_first_starved == (size_t)-1)
                    s_first_starved = idx;
            }
        }
        if(rebuilt)
            rebuilds++;

        /* Arrival units may fall back to the group centre-of-mass point-seek field. */
        struct flock *fl = in->flock;
        struct arrival_state *as = fl ? flock_arrival_for_ent(fl, uid) : NULL;
        if(as && as->phase == ARRIVAL_PHASE_FILLING) {
            struct target tc = {.kind = TARGET_KIND_POINT_SEEK,
                                .point_seek = {.dest_id = as->com_dest_id, .dest_xz = as->com}};
            if(M_NavRequiresPathRequest(map, tc, pos)) {
                if(s_rebuild_budget > 0) {
                    s_rebuild_budget--;
                    M_NavServicePathRequest(map, tc, pos);
                }else{
                    in->field_starved = true;
                    if(s_first_starved == (size_t)-1)
                        s_first_starved = idx;
                }
            }
        }

        if(SDL_GetPerformanceCounter() - last_yield > yield_ticks) {
            Task_Yield();
            last_yield = SDL_GetPerformanceCounter();
        }
    }

    if(s_first_starved != (size_t)-1) {
        s_rebuild_start = s_first_starved;
    }

    /* Join the flow floods and LOS chains that the serviced path requests
     * deferred to the async pool; the fields must be resident before the
     * desired-velocity phase peeks them. The LOS chains are dispatched first
     * so both flood sets share the window. */
    N_DispatchLOSChains();
    N_AwaitAsyncFields();
    N_FinishLOSChains();

    s_last_nav_tick_stats.cpr_serial_us =
        perf_ticks_to_us(SDL_GetPerformanceCounter() - phase_start);
    s_last_nav_tick_stats.nreq_rebuilds = rebuilds;

    M_NavAddFlowSamples(map, checked, checked - rebuilds);
    PERF_RETURN_VOID();
}

static void fork_join_desired_velocity(void)
{
    PERF_ENTER();
    move_submit_cpu_work(move_dv_task);
    move_complete_cpu_work();
    PERF_RETURN_VOID();
}

static void fork_join_velocity_computations(void)
{
    switch(s_move_work.type) {
    case WORK_TYPE_CPU:
        move_submit_cpu_work(move_velocity_task);
        move_complete_cpu_work();
        break;
    case WORK_TYPE_GPU:
        move_submit_gpu_velocity_work();
        break;
    default: assert(0);
    }
}

static void fork_join_state_updates(void)
{
    PERF_ENTER();
    PERF_PUSH("move::submit state updates");
    move_submit_cpu_work(move_update_task);
    PERF_POP();

    PERF_PUSH("move::complete state updates");
    move_complete_cpu_work();
    PERF_POP();

    PERF_RETURN_VOID();
}

static void await_gpu_completion(uint32_t timeout_ms)
{
    uint32_t begin = SDL_GetTicks();
    while(!SDL_AtomicGet(&s_move_work.gpu_velocities_ready)) {

        Task_RescheduleOnMain();
        R_PushCmd((struct rcmd){
            .func = R_GL_MovePollCompletion,
            .nargs = 1,
            .args = {
                [0] = &s_move_work.gpu_velocities_ready
            }
        });

        int source;
        Task_AwaitEvent(EVENT_UPDATE_START, &source);

        uint32_t now = SDL_GetTicks();
        if(SDL_TICKS_PASSED(now, begin + timeout_ms))
            break;
    }
}

static void await_gpu_download(void)
{
    /* We need to wait for 2 frames after the download command 
     * is queued. In one tick, it will be executed by the render
     * thread. In 2 ticks, it is guaranteed to have completed.
     */
    unsigned long start_frame = g_frame_idx;
    while((g_frame_idx - start_frame) < 2) {
        int source;
        Task_AwaitEvent(EVENT_UPDATE_START, &source);
    }
}

static void copy_gpu_results(void)
{
    PERF_ENTER();
    size_t nents = kh_size(s_entity_state_table);
    for(int i = 0; i < s_move_work.nwork; i++) {

        struct move_work_in *in = &s_move_work.in[i];
        struct move_work_out *out = &s_move_work.out[i];

        out->ent_uid = in->ent_uid;
        out->ent_vel = s_move_work.gpu_velocities[i];
        /* The shader reports no solve diagnostics; the counters read zero. */
        out->cp_flags = 0;

        /* The shader knows neither the COMBAT_HELD flag nor the parked one;
         * mirror the CPU path's short-circuits so neither creeps here. */
        if((G_FlagsGetFrom(s_move_work.gamestate.flags, in->ent_uid) & ENTITY_FLAG_COMBAT_HELD)
        || movestate_aux_get(in->ent_uid)->parked) {
            out->ent_vel = (vec2_t){0.0f, 0.0f};
        }
    }
    PERF_RETURN_VOID();
}

static struct result navigation_tick_task(void *arg)
{
    s_nav_task_active_tid = Sched_ActiveTID();
    uint64_t nav_start = SDL_GetPerformanceCounter();

    Perf_NavParallelReset();
    s_rebuild_budget = MAX_REBUILDS_PER_TICK;
    s_first_starved = (size_t)-1;
    N_ApplyDeferredInvalidations();

    /* Open the async field pool for the whole tick: the serial LOS-build loop
     * defers flow floods to it before compute_path_requests runs. */
    N_PrepareAsyncWork();

    s_last_nav_tick_stats.inval_us =
        perf_ticks_to_us(SDL_GetPerformanceCounter() - nav_start);

    uint64_t dispatch_ticks = dispatch_async_fields();

    compute_los_state();

    uint64_t serial_ticks = SDL_GetPerformanceCounter() - nav_start;
    uint64_t cpr_start = SDL_GetPerformanceCounter();

    compute_path_requests(dispatch_ticks);

    serial_ticks += SDL_GetPerformanceCounter() - cpr_start;
    uint64_t phase_start = SDL_GetPerformanceCounter();

    fork_join_desired_velocity();

    s_last_nav_tick_stats.dv_us =
        perf_ticks_to_us(SDL_GetPerformanceCounter() - phase_start);
    phase_start = SDL_GetPerformanceCounter();

    fork_join_velocity_computations();

    if(s_move_work.type == WORK_TYPE_GPU) {

        uint32_t period_ms = (1.0f / hz_count(s_move_work.hz)) * 1000;
        await_gpu_completion(period_ms);
        move_complete_gpu_velocity_work();

        await_gpu_download();
        copy_gpu_results();
    }

    s_last_nav_tick_stats.vel_us =
        perf_ticks_to_us(SDL_GetPerformanceCounter() - phase_start);
    phase_start = SDL_GetPerformanceCounter();

    fork_join_state_updates();

    s_last_nav_tick_stats.upd_us =
        perf_ticks_to_us(SDL_GetPerformanceCounter() - phase_start);

    s_nav_task_active_tid = NULL_TID;

    s_last_nav_tick_stats.dur_us    = perf_ticks_to_us(SDL_GetPerformanceCounter() - nav_start);
    s_last_nav_tick_stats.serial_us = perf_ticks_to_us(serial_ticks);
    s_last_nav_tick_stats.total_us  = s_last_nav_tick_stats.serial_us + Perf_NavParallelGet();
    return NULL_RESULT;
}

/* A unit that stopped short of its cell is skipped by the per-tick work for
 * good, so a formation that settles around it leaves it stranded wherever it
 * gave up. Once the formation has taken its tiles, put it back on the road with
 * a phase permit, until its budget runs out.
 */
static void resume_stranded_formation_units(void)
{
    ASSERT_IN_MAIN_THREAD();

    uint32_t uid;
    kh_foreach_key(s_entity_state_table, uid, {

        struct movestate *ms = movestate_get(uid);
        if(!ms || ms->state != STATE_ARRIVED)
            continue;
        if(!G_EntityExists(uid))
            continue;

        struct movestate_aux *aux = movestate_aux_get(uid);
        if(aux->phase_ticks > PHASE_MAX_TICKS)
            continue;

        struct formation_submit_state fss = {0};
        if(!G_Formation_SubmitState(uid, &fss))
            continue;
        if(!fss.may_settle || fss.at_cell || !fss.assigned_to_cell)
            continue;

        if(ms->blocking)
            entity_unblock(uid);
        move_notify_motion_start(uid, ms);
        ms->state = STATE_MOVING_IN_FORMATION;
    });
}

/* Waiting units aren't in the per-tick work; tick their resume countdown here. */
static void resume_waiting_units(void)
{
    ASSERT_IN_MAIN_THREAD();

    uint32_t uid;
    kh_foreach_key(s_entity_state_table, uid, {

        struct movestate *ms = movestate_get(uid);
        if(!ms || ms->state != STATE_WAITING)
            continue;
        if(!G_EntityExists(uid))
            continue;

        struct movestate_aux *aux = movestate_aux_get(uid);
        if(aux->wait_ticks_left > 0 && --aux->wait_ticks_left > 0)
            continue;

        assert(aux->wait_prev == STATE_MOVING
            || aux->wait_prev == STATE_MOVING_IN_FORMATION
            || aux->wait_prev == STATE_SEEK_ENEMIES
            || aux->wait_prev == STATE_SURROUND_ENTITY);

        if(ms->blocking)
            entity_unblock(uid);
        move_notify_motion_start(uid, ms);
        ms->state = aux->wait_prev;
    });
}

/* Stopped held units aren't in the per-tick work; pivot them toward their combat facing here. */
static void pivot_held_still_units(void)
{
    ASSERT_IN_MAIN_THREAD();

    uint32_t uid;
    kh_foreach_key(s_entity_state_table, uid, {

        struct movestate *ms = movestate_get(uid);
        if(!ms || !ent_still(ms))
            continue;
        if(!(G_FlagsGet(uid) & ENTITY_FLAG_COMBAT_HELD))
            continue;
        if(!G_EntityExists(uid))
            continue;

        quat_t next = turn_toward(Entity_GetRot(uid), movestate_aux_get(uid)->combat_facing,
            SCALED_MAX_TURN_RATE);
        Entity_SetRot(uid, next);
        ms->prev_rot = next;
        ms->next_rot = next;
    });
}

static void move_do_tick(enum eventtype curr_event, enum movement_hz hz)
{
    ASSERT_IN_MAIN_THREAD();
    PERF_PUSH("movement::tick");
    uint64_t tick_start = SDL_GetPerformanceCounter();

    struct sval cap_setting;
    s_log_cp_captures = (Settings_Get("pf.debug.log_cp_captures", &cap_setting) == SS_OKAY)
                     && cap_setting.as_bool;
    SDL_AtomicSet(&s_cp_ncaptures, 0);

    struct sval trace_setting;
    s_move_trace_min_radius =
        (Settings_Get("pf.debug.log_move_trace_min_radius", &trace_setting) == SS_OKAY)
        ? trace_setting.as_float : -1.0f;
    enum selection_type seltype;
    s_move_trace_sel = G_Sel_Get(&seltype);

    move_consume_work_results();

    s_last_nav_tick_stats.consume_us =
        perf_ticks_to_us(SDL_GetPerformanceCounter() - tick_start);

    uint64_t phase_start = SDL_GetPerformanceCounter();
    resume_waiting_units();
    resume_stranded_formation_units();
    pivot_held_still_units();
    s_last_nav_tick_stats.pivot_us =
        perf_ticks_to_us(SDL_GetPerformanceCounter() - phase_start);

    move_handle_hz_update(curr_event);

    phase_start = SDL_GetPerformanceCounter();
    move_process_cmds();
    s_last_nav_tick_stats.cmds_us =
        perf_ticks_to_us(SDL_GetPerformanceCounter() - phase_start);

    phase_start = SDL_GetPerformanceCounter();
    move_release_gamestate();
    uint64_t copy_ticks = SDL_GetPerformanceCounter() - phase_start;

    phase_start = SDL_GetPerformanceCounter();
    disband_empty_flocks();
    update_flock_arrival_fields();
    s_last_nav_tick_stats.flock_us =
        perf_ticks_to_us(SDL_GetPerformanceCounter() - phase_start);

    /* Run the navigation updates synchronous to the movement tick */
    phase_start = SDL_GetPerformanceCounter();
    G_UpdateMap();
    s_last_nav_tick_stats.map_update_us =
        perf_ticks_to_us(SDL_GetPerformanceCounter() - phase_start);

    phase_start = SDL_GetPerformanceCounter();
    move_prepare_work(hz);
    move_copy_gamestate();
    copy_ticks += SDL_GetPerformanceCounter() - phase_start;
    s_last_nav_tick_stats.copy_gs_us = perf_ticks_to_us(copy_ticks);

    phase_start = SDL_GetPerformanceCounter();
    move_build_flock_snaps();
    s_last_nav_tick_stats.snaps_us =
        perf_ticks_to_us(SDL_GetPerformanceCounter() - phase_start);

    PERF_PUSH("submit move work");
    phase_start = SDL_GetPerformanceCounter();
    uint32_t debug_uid = G_ClearPath_DebugUid();
    s_last_nav_tick_stats.nstate_moving = 0;
    s_last_nav_tick_stats.nstate_arrived = 0;
    s_last_nav_tick_stats.nstate_seek = 0;
    s_last_nav_tick_stats.nstate_waiting = 0;
    s_last_nav_tick_stats.nstate_turning = 0;
    uint32_t curr;
    kh_foreach_key(s_entity_state_table, curr, {

        struct movestate *ms = movestate_get(curr);
        assert(ms);

        switch(ms->state) {
        case STATE_ARRIVED:      s_last_nav_tick_stats.nstate_arrived++; break;
        case STATE_SEEK_ENEMIES: s_last_nav_tick_stats.nstate_seek++;    break;
        case STATE_WAITING:      s_last_nav_tick_stats.nstate_waiting++; break;
        case STATE_TURNING:      s_last_nav_tick_stats.nstate_turning++; break;
        default:                 s_last_nav_tick_stats.nstate_moving++;  break;
        }
        if(ent_still(ms)) {
            struct movestate_aux *saux = movestate_aux_get(curr);
            saux->parked = false;
            saux->phasing = false;
            continue;
        }

        struct flock *flock = NULL;
        const struct flock_snap *fsnap = NULL;
        khiter_t fit = kh_get(findex, s_flock_index, curr);
        if(fit != kh_end(s_flock_index)) {
            int fi = kh_val(s_flock_index, fit);
            flock = &vec_AT(&s_flocks, fi);
            fsnap = &s_move_work.flock_snaps[fi];
        }

        vec2_t pos = (vec2_t){ms->prev_pos.x, ms->prev_pos.z};
        float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, curr);
        s_max_sel_radius = MAX(s_max_sel_radius, radius);

        struct cp_ent curr_cp = (struct cp_ent) {
            .xz_pos = pos,
            .xz_vel = ms->velocity,
            .radius = radius
        };

        vec2_t cell_pos = (vec2_t){0.0f, 0.0f};
        vec2_t cell_arrival_vdes = {0};
        if(ms->state == STATE_ARRIVING_TO_CELL) {
            cell_pos = G_Formation_CellPosition(curr);
            if(!G_Formation_CanUseArrivalField(curr)) {
                cell_arrival_vdes = G_Formation_ApproximateDesiredArrivalVelocity(curr);
            }else{
                G_Formation_UpdateFieldIfNeeded(curr);
                cell_arrival_vdes = G_Formation_DesiredArrivalVelocity(curr);
            }
        }

        struct formation_submit_state fss = {0};
        bool in_formation = G_Formation_SubmitState(curr, &fss);

        struct movestate_aux *caux = movestate_aux_get(curr);
        /* A unit whose cell assignment has not landed yet has no cell to have
         * arrived at, and reads as arrived. Parking it there would flip the
         * whole formation to its idle clip for the width of that window.
         */
        bool parked = in_formation && fss.assigned_to_cell
                   && fss.arrived_at_cell && !fss.may_settle;
        caux->parked = parked;

        bool left_behind = in_formation && fss.may_settle && !fss.at_cell;
        if(!left_behind)
            caux->phase_ticks = 0;
        else if(caux->phase_ticks < UINT16_MAX)
            caux->phase_ticks++;
        caux->phasing = left_behind && (caux->phase_ticks <= PHASE_MAX_TICKS);

        /* Both notifications are level-triggered and idempotent through the
         * motion_stopped latch, so the pair is self-healing: whichever way the
         * unit leaves the parked condition, the very next tick puts the clip
         * back in step. A unit on its way to settling is left alone, since
         * entity_finish_moving announces its own end.
         */
        if(parked) {
            move_notify_motion_end(curr);
        }else if(caux->motion_stopped && !caux->soft_blocking
              && !(in_formation && fss.arrived_at_cell && fss.may_settle)) {
            move_notify_motion_start(curr, ms);
        }

        move_push_work((struct move_work_in){
            .ent_uid = curr,
            .flock = flock,
            .fsnap = fsnap,
            .speed = in_formation ? fss.speed : ms->max_speed,
            .cell_pos = cell_pos,
            .cp_ent = curr_cp,
            .cp_side = movestate_aux_get(curr)->cp_side,
            .save_debug = (curr == debug_uid),
            .fstate.fid = fss.fid,
            .fstate.assignment_ready = fss.assignment_ready,
            .fstate.assigned_to_cell = fss.assigned_to_cell,
            .fstate.in_range_of_cell = fss.in_range_of_cell,
            .fstate.arrived_at_cell = fss.arrived_at_cell,
            .fstate.may_settle = fss.may_settle,
            .fstate.at_cell = fss.at_cell,
            .fstate.normal_cohesion_force = fss.cohesion_force,
            .fstate.normal_align_force = fss.alignment_force,
            .fstate.normal_drag_force = fss.drag_force,
            .fstate.target_orientation = fss.target_orientation,
            .cell_arrival_vdes = cell_arrival_vdes
        });
    });
    s_last_nav_tick_stats.submit_us =
        perf_ticks_to_us(SDL_GetPerformanceCounter() - phase_start);
    PERF_POP();

    nav_tick_submit_work();

    s_last_nav_tick_stats.main_us =
        perf_ticks_to_us(SDL_GetPerformanceCounter() - tick_start);
    PERF_POP();
}

static void move_tick(void *user, void *event)
{
    /* If we are backed up, drop excess events */
    if(g_frame_idx == s_last_tick)
        return;

    enum eventtype curr_event = (uintptr_t)user;
    enum movement_hz hz = event_to_hz(curr_event);

    enum move_work_status status = nav_tick_finish_work();
    if(status == WORK_INCOMPLETE) {
        s_move_tick_queued = true;
        return;
    }

    s_move_tick_queued = false;
    move_do_tick(curr_event, hz);
}

static void handle_queued_tick(void)
{
    if(!s_move_tick_queued)
        return;

    enum move_work_status status = nav_tick_finish_work();
    if(status == WORK_INCOMPLETE)
        return;

    enum movement_hz hz = s_move_work.hz;
    enum eventtype curr_event = event_for_hz(hz);

    s_move_tick_queued = false;
    move_do_tick(curr_event, hz);
}

static void on_update(void *user, void *event)
{
    stalloc_clear(&s_eventargs);
    handle_queued_tick();
}

static void nav_cancel_gpu_work(void)
{
    /* Handle the case where the work task is blocked on an event. We 
     * cannot run it yet, as the new velocity data from the GPU won't 
     * be available until the next frame. Kill off the task.
     */
    assert(s_tick_task_tid != NULL_TID);
    Sched_TryCancel(s_tick_task_tid);
    s_tick_task_tid = NULL_TID;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

/* May only be called from within the navigation task context (it is invoked by the
 * fieldcache's nav-task assertion). Outside that context it reads as NULL_TID.
 */
uint32_t G_Move_GetNavTID(void)
{
    return s_nav_task_active_tid;
}

bool G_Move_NavQuiesce(void)
{
    return nav_tick_finish_work() == WORK_COMPLETE;
}

bool G_Move_Init(const struct map *map)
{
    assert(map);
    if(simd_avx2_supported()) {
        s_cohesion_force = cohesion_force_avx2;
    }
    if(NULL == (s_entity_state_table = kh_init(state))) {
        return false;
    }

    if(NULL == (s_entity_aux_table = kh_init(auxstate))) {
        kh_destroy(state, s_entity_state_table);
        return false;
    }

    if(NULL == (s_flock_index = kh_init(findex))) {
        kh_destroy(auxstate, s_entity_aux_table);
        kh_destroy(state, s_entity_state_table);
        return false;
    }

    if(NULL == (s_aabb_cache = kh_init(aabb))) {
        kh_destroy(findex, s_flock_index);
        kh_destroy(auxstate, s_entity_aux_table);
        kh_destroy(state, s_entity_state_table);
        return false;
    }

    memset(&s_move_work, 0, sizeof(s_move_work));
    if(!stalloc_init(&s_move_work.mem)) {
        kh_destroy(aabb, s_aabb_cache);
        kh_destroy(findex, s_flock_index);
        kh_destroy(auxstate, s_entity_aux_table);
        kh_destroy(state, s_entity_state_table);
        return NULL;
    }

    if(!queue_cmd_init(&s_move_commands, 256)) {
        stalloc_destroy(&s_move_work.mem);
        kh_destroy(aabb, s_aabb_cache);
        kh_destroy(findex, s_flock_index);
        kh_destroy(auxstate, s_entity_aux_table);
        kh_destroy(state, s_entity_state_table);
        return NULL;
    }

    if(!stalloc_init(&s_eventargs)) {
        stalloc_destroy(&s_move_work.mem);
        kh_destroy(aabb, s_aabb_cache);
        kh_destroy(findex, s_flock_index);
        kh_destroy(auxstate, s_entity_aux_table);
        kh_destroy(state, s_entity_state_table);
        queue_cmd_destroy(&s_move_commands);
        return NULL;
    }

    vec_entity_init(&s_move_markers);
    vec_flock_init(&s_flocks);

    N_FC_SetNavTaskTIDProvider(G_Move_GetNavTID);

    E_Global_Register(EVENT_UPDATE_START, on_update, NULL, G_RUNNING);
    E_Global_Register(SDL_MOUSEBUTTONDOWN, on_mousedown, NULL, G_RUNNING);
    E_Global_Register(SDL_MOUSEBUTTONUP, on_mouseup, NULL, G_RUNNING);
    E_Global_Register(SDL_MOUSEMOTION, on_mousemotion, NULL, G_RUNNING);
    E_Global_Register(EVENT_RENDER_3D_POST, on_render_3d, NULL, 
        G_RUNNING | G_PAUSED_FULL | G_PAUSED_UI_RUNNING);
    register_callback_for_hz(s_move_hz);
    E_Global_Register(EVENT_20HZ_TICK, interpolate_tick, NULL, G_RUNNING);

    s_map = map;
    s_attack_on_lclick = false;
    s_move_on_lclick = false;
    s_mouse_dragged = false;
    s_drag_attacking = false;
    s_nav_snapshot = NULL;
    move_copy_gamestate();
    return true;
}

void G_Move_Shutdown(void)
{
    if(nav_tick_finish_work() == WORK_INCOMPLETE) {
        nav_cancel_gpu_work();
    }
    s_move_tick_queued = false;
    s_map = NULL;

    unregister_callback_for_hz(s_move_hz);
    E_Global_Unregister(EVENT_20HZ_TICK, interpolate_tick);
    E_Global_Unregister(EVENT_RENDER_3D_POST, on_render_3d);
    E_Global_Unregister(SDL_MOUSEBUTTONDOWN, on_mousedown);
    E_Global_Unregister(SDL_MOUSEBUTTONUP, on_mouseup);
    E_Global_Unregister(SDL_MOUSEMOTION, on_mousemotion);
    E_Global_Unregister(EVENT_UPDATE_START, on_update);

    for(int i = 0; i < vec_size(&s_move_markers); i++) {
        E_Entity_Unregister(EVENT_ANIM_FINISHED, vec_AT(&s_move_markers, i), on_marker_anim_finish);
        G_RemoveEntity(vec_AT(&s_move_markers, i));
        G_FreeEntity(vec_AT(&s_move_markers, i));
    }

    move_destroy_gamestate();
    vec_flock_destroy(&s_flocks);
    vec_entity_destroy(&s_move_markers);
    stalloc_destroy(&s_eventargs);
    queue_cmd_destroy(&s_move_commands);
    stalloc_destroy(&s_move_work.mem);
    kh_destroy(aabb, s_aabb_cache);
    kh_destroy(findex, s_flock_index);
    kh_destroy(auxstate, s_entity_aux_table);
    kh_destroy(state, s_entity_state_table);
}

bool G_Move_HasWork(void)
{
    return (queue_size(s_move_commands) > 0);
}

void G_Move_FlushWork(void)
{
    if(!s_map)
        return;

    /* Discard the results of the last
     * movement tick.
     */
    if(nav_tick_finish_work() == WORK_INCOMPLETE) {
        nav_cancel_gpu_work();
    }

    stalloc_clear(&s_move_work.mem);
    s_move_work.in = NULL;
    s_move_work.out = NULL;
    s_move_work.nwork = 0;
    s_move_work.ntasks = 0;

    move_process_cmds();
}

void G_Move_AddEntity(uint32_t uid, vec3_t pos, float sel_radius, int faction_id)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_ADD,
        .uid = uid,
        .u.add = {
            .pos = pos,
            .radius = sel_radius,
            .faction_id = faction_id
        }
    });
}

void G_Move_RemoveEntity(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_REMOVE,
        .uid = uid
    });
}

void G_Move_Stop(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_STOP,
        .uid = uid
    });
}

bool G_Move_GetDest(uint32_t uid, vec2_t *out_xz, bool *out_attack)
{
    struct move_cmd *cmd = snoop_most_recent_command(MOVE_CMD_SET_DEST,
        (void*)(uintptr_t)uid, uids_match, false);

    if(cmd) {
        *out_xz = cmd->u.set_dest.dest_xz;
        *out_attack = cmd->u.set_dest.attack;
        return true;
    }

    struct flock *fl = flock_for_ent(uid);
    if(!fl)
        return false;
    *out_xz = fl->target_xz;
    *out_attack = N_DestIDIsAttacking(fl->dest_id);
    return true;
}

bool G_Move_GetSurrounding(uint32_t uid, uint32_t *out_uid)
{
    struct move_cmd *cmd = snoop_most_recent_command(MOVE_CMD_SET_SURROUND_ENTITY,
        (void*)(uintptr_t)uid, uids_match, false);

    if(cmd) {
        *out_uid = cmd->u.surround.target;
        return true;
    }

    struct movestate *ms = movestate_get(uid);
    assert(ms);
    if(ms->state != STATE_SURROUND_ENTITY)
        return false;
    *out_uid = ms->surround_target_uid;
    return true;
}

bool G_Move_Still(uint32_t uid)
{
    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return true;
    return snoop_still(uid);
}

/* Pause/resume the unit's move animation while it holds position to attack. */
void G_Move_SetCombatHeld(uint32_t uid, bool held)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_SET_COMBAT_HELD,
        .uid = uid,
        .u.combat_held.held = held
    });
}

void G_Move_SetCombatFacing(uint32_t uid, quat_t dir)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_SET_COMBAT_FACING,
        .uid = uid,
        .u.combat_facing.dir = dir
    });
}

void G_Move_SetDest(uint32_t uid, vec2_t dest_xz, bool attack)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_SET_DEST,
        .uid = uid,
        .u.set_dest = {
            .dest_xz = dest_xz,
            .attack = attack
        }
    });
}

void G_Move_SetChangeDirection(uint32_t uid, quat_t target)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_CHANGE_DIRECTION,
        .uid = uid,
        .u.change_direction.target = target
    });
}

void G_Move_SetEnterRange(uint32_t uid, uint32_t target, float range)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_SET_ENTER_RANGE,
        .uid = uid,
        .u.enter_range = {
            .target = target,
            .range = range
        }
    });
}

void G_Move_SetMoveOnLeftClick(void)
{
    s_attack_on_lclick = false;
    s_move_on_lclick = true;
}

void G_Move_SetAttackOnLeftClick(void)
{
    s_attack_on_lclick = true;
    s_move_on_lclick = false;
}

void G_Move_SetSeekEnemies(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_SET_SEEK_ENEMIES,
        .uid = uid
    });
}

void G_Move_SetSeekPin(uint32_t uid, uint32_t target)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_SET_SEEK_PIN,
        .uid = uid,
        .u.seek_pin.target = target
    });
}

void G_Move_SetSurroundEntity(uint32_t uid, uint32_t target)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_SET_SURROUND_ENTITY,
        .uid = uid,
        .u.surround.target = target
    });
}

void G_Move_SetFlee(uint32_t uid, vec2_t threat_xz)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_SET_FLEE,
        .uid = uid,
        .u.flee.threat_xz = threat_xz
    });
}

void G_Move_StopFlee(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_STOP_FLEE,
        .uid = uid
    });
}

bool G_Move_IsFleeing(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    struct movestate *ms = movestate_get(uid);
    return (ms && ms->state == STATE_FLEEING);
}

void G_Move_UpdatePos(uint32_t uid, vec2_t pos)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_UPDATE_POS,
        .uid = uid,
        .u.update_pos.pos = pos
    });
}

void G_Move_Unblock(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_UNBLOCK,
        .uid = uid
    });
}

void G_Move_BlockAt(uint32_t uid, vec3_t pos)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_BLOCK,
        .uid = uid,
        .u.block.pos = pos
    });
}

void G_Move_UpdateFactionID(uint32_t uid, int oldfac, int newfac)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_UPDATE_FACTION_ID,
        .uid = uid,
        .u.update_faction = {
            .oldfac = oldfac,
            .newfac = newfac
        }
    });
}

void G_Move_UpdateSelectionRadius(uint32_t uid, float sel_radius)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_UPDATE_SELECTION_RADIUS,
        .uid = uid,
        .u.update_radius.radius = sel_radius
    });
}

bool G_Move_InTargetMode(void)
{
    return (s_move_on_lclick || s_attack_on_lclick);
}

/* True while the user drags to orient a formation, before the order is issued on mouse-up. */
bool G_Move_InOrderDrag(void)
{
    return s_mouse_dragged;
}

void G_Move_SetClickEnabled(bool on)
{
    s_click_move_enabled = on;
}

bool G_Move_GetClickEnabled(void)
{
    return s_click_move_enabled;
}

bool G_Move_GetMaxSpeed(uint32_t uid, float *out)
{
    struct move_cmd *cmd = snoop_most_recent_command(MOVE_CMD_SET_MAX_SPEED,
        (void*)(uintptr_t)uid, uids_match, false);

    if(cmd) {
        *out = cmd->u.max_speed.speed;
        return true;
    }

    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return false;
    struct movestate *ms = &kh_value(s_entity_state_table, k);
    *out = ms->base_speed;
    return true;
}

bool G_Move_SetMaxSpeed(uint32_t uid, float speed)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_SET_MAX_SPEED,
        .uid = uid,
        .u.max_speed.speed = speed
    });
    return true;
}

bool G_Move_SetSpeedBonus(uint32_t uid, float bonus)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_SET_SPEED_BONUS,
        .uid = uid,
        .u.speed_bonus.bonus = bonus
    });
    return true;
}

bool G_Move_GetEffectiveSpeed(uint32_t uid, float *out)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return false;
    struct movestate *ms = &kh_value(s_entity_state_table, k);
    *out = ms->max_speed;
    return true;
}

void G_Move_ArrangeInFormation(vec_entity_t *ents, vec2_t target, 
                               vec2_t orientation, enum formation_type type)
{
    ASSERT_IN_MAIN_THREAD();
    vec_entity_t *copy = PF_MALLOC(sizeof(vec_entity_t));
    vec_entity_init(copy);
    vec_entity_copy(copy, ents);

    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_MAKE_FLOCKS,
        .u.make_flocks = {
            .sel = copy,
            .target_xz = target,
            .type = type,
            .attack = false,
            .target_orientation = orientation
        }
    });
}

void G_Move_AttackInFormation(vec_entity_t *ents, vec2_t target, 
                              vec2_t orientation, enum formation_type type)
{
    ASSERT_IN_MAIN_THREAD();
    vec_entity_t *copy = PF_MALLOC(sizeof(vec_entity_t));
    vec_entity_init(copy);
    vec_entity_copy(copy, ents);

    /* An attack-move engages on contact, as the mouse order does. */
    for(int i = 0; i < vec_size(ents); i++) {
        uint32_t curr = vec_AT(ents, i);
        if(G_FlagsGet(curr) & ENTITY_FLAG_COMBATABLE)
            G_Combat_SetStance(curr, COMBAT_STANCE_AGGRESSIVE);
    }

    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_MAKE_FLOCKS,
        .u.make_flocks = {
            .sel = copy,
            .target_xz = target,
            .type = type,
            .attack = true,
            .target_orientation = orientation
        }
    });
}

void G_Move_SetTickHz(enum movement_hz hz)
{
    s_move_hz_dirty = (s_move_hz != hz);
    s_move_hz = hz;
}

int G_Move_GetTickHz(void)
{
    return hz_count(s_move_hz);
}

void G_Move_SetUseGPU(bool use)
{
    s_use_gpu = use;
}

bool G_Move_SaveState(struct SDL_RWops *stream)
{
    struct attr click_move_enabled = (struct attr){
        .type = TYPE_BOOL,
        .val.as_bool = s_click_move_enabled
    };
    CHK_TRUE_RET(Attr_Write(stream, &click_move_enabled, "click_move_enabled"));

    /* save flock info */
    struct attr num_flocks = (struct attr){
        .type = TYPE_INT,
        .val.as_int = vec_size(&s_flocks)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_flocks, "num_flocks"));

    Sched_TryYield();

    for(int i = 0; i < vec_size(&s_flocks); i++) {

        const struct flock *curr_flock = &vec_AT(&s_flocks, i);

        struct attr num_flock_ents = (struct attr){
            .type = TYPE_INT,
            .val.as_int = kh_size(curr_flock->ents)
        };
        CHK_TRUE_RET(Attr_Write(stream, &num_flock_ents, "num_flock_ents"));

        uint32_t uid;
        kh_foreach_key(curr_flock->ents, uid, {
        
            struct attr flock_ent = (struct attr){
                .type = TYPE_INT,
                .val.as_int = uid
            };
            CHK_TRUE_RET(Attr_Write(stream, &flock_ent, "flock_ent"));
        });
        Sched_TryYield();

        struct attr flock_target = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = curr_flock->target_xz
        };
        CHK_TRUE_RET(Attr_Write(stream, &flock_target, "flock_target"));

        struct attr flock_dest = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr_flock->dest_id
        };
        CHK_TRUE_RET(Attr_Write(stream, &flock_dest, "flock_dest"));

        CHK_TRUE_RET(G_Arrival_SaveState(stream, &curr_flock->arrival));
        Sched_TryYield();
    }

    /* save the movement state */
    struct attr num_ents = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_entity_state_table)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_ents, "num_ents"));
    Sched_TryYield();

    uint32_t key;
    struct movestate curr;

    kh_foreach(s_entity_state_table, key, curr, {

        struct movestate_aux *aux = movestate_aux_get(key);
        assert(aux);

        struct attr uid = (struct attr){
            .type = TYPE_INT,
            .val.as_int = key
        };
        CHK_TRUE_RET(Attr_Write(stream, &uid, "uid"));

        struct attr state = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.state
        };
        CHK_TRUE_RET(Attr_Write(stream, &state, "state"));

        struct attr base_speed = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float = curr.base_speed
        };
        CHK_TRUE_RET(Attr_Write(stream, &base_speed, "base_speed"));

        struct attr speed_bonus = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float = curr.speed_bonus
        };
        CHK_TRUE_RET(Attr_Write(stream, &speed_bonus, "speed_bonus"));

        struct attr velocity = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = curr.velocity
        };
        CHK_TRUE_RET(Attr_Write(stream, &velocity, "velocity"));

        struct attr next_pos = (struct attr){
            .type = TYPE_VEC3,
            .val.as_vec3 = curr.next_pos
        };
        CHK_TRUE_RET(Attr_Write(stream, &next_pos, "next_pos"));

        struct attr prev_pos = (struct attr){
            .type = TYPE_VEC3,
            .val.as_vec3 = curr.prev_pos
        };
        CHK_TRUE_RET(Attr_Write(stream, &prev_pos, "prev_pos"));

        struct attr next_rot = (struct attr){
            .type = TYPE_QUAT,
            .val.as_quat = curr.next_rot
        };
        CHK_TRUE_RET(Attr_Write(stream, &next_rot, "next_rot"));

        struct attr prev_rot = (struct attr){
            .type = TYPE_QUAT,
            .val.as_quat = curr.prev_rot
        };
        CHK_TRUE_RET(Attr_Write(stream, &prev_rot, "prev_rot"));

        struct attr step = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float = curr.step
        };
        CHK_TRUE_RET(Attr_Write(stream, &step, "step"));

        struct attr left = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.left
        };
        CHK_TRUE_RET(Attr_Write(stream, &left, "left"));

        struct attr blocking = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = curr.blocking
        };
        CHK_TRUE_RET(Attr_Write(stream, &blocking, "blocking"));

        /* last_stop_pos and last_stop_radius are loaded in 
         * along with the entity's position. No need to overwrite
         * it and risk some inconsistency */

        struct attr wait_prev = (struct attr){
            .type = TYPE_INT,
            .val.as_int = aux->wait_prev
        };
        CHK_TRUE_RET(Attr_Write(stream, &wait_prev, "wait_prev"));

        struct attr wait_ticks_left = (struct attr){
            .type = TYPE_INT,
            .val.as_int = aux->wait_ticks_left
        };
        CHK_TRUE_RET(Attr_Write(stream, &wait_ticks_left, "wait_ticks_left"));

        for(int i = 0; i < VEL_HIST_LEN; i++) {
        
            struct attr hist_entry = (struct attr){
                .type = TYPE_VEC2,
                .val.as_vec2 = aux->vel_hist[i]
            };
            CHK_TRUE_RET(Attr_Write(stream, &hist_entry, "hist_entry"));
        }

        struct attr vel_hist_idx = (struct attr){
            .type = TYPE_INT,
            .val.as_int = aux->vel_hist_idx
        };
        CHK_TRUE_RET(Attr_Write(stream, &vel_hist_idx, "vel_hist_idx"));

        struct attr surround_target_uid = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.surround_target_uid
        };
        CHK_TRUE_RET(Attr_Write(stream, &surround_target_uid, "surround_target_uid"));

        struct attr surround_target_prev = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = aux->surround_target_prev
        };
        CHK_TRUE_RET(Attr_Write(stream, &surround_target_prev, "surround_target_prev"));

        struct attr surround_nearest_prev = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = aux->surround_nearest_prev
        };
        CHK_TRUE_RET(Attr_Write(stream, &surround_nearest_prev, "surround_nearest_prev"));

        struct attr using_surround_field = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = curr.using_surround_field
        };
        CHK_TRUE_RET(Attr_Write(stream, &using_surround_field, "using_surround_field"));

        struct attr target_prev_pos = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = aux->target_prev_pos
        };
        CHK_TRUE_RET(Attr_Write(stream, &target_prev_pos, "target_prev_pos"));

        struct attr target_range = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float = aux->target_range
        };
        CHK_TRUE_RET(Attr_Write(stream, &target_range, "target_range"));

        struct attr target_dir = (struct attr){
            .type = TYPE_QUAT,
            .val.as_quat = aux->target_dir
        };
        CHK_TRUE_RET(Attr_Write(stream, &target_dir, "target_dir"));

        struct attr combat_facing = (struct attr){
            .type = TYPE_QUAT,
            .val.as_quat = aux->combat_facing
        };
        CHK_TRUE_RET(Attr_Write(stream, &combat_facing, "combat_facing"));

        struct attr flee_threat_pos = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = aux->flee_threat_pos
        };
        CHK_TRUE_RET(Attr_Write(stream, &flee_threat_pos, "flee_threat_pos"));

        struct attr flee_prev = (struct attr){
            .type = TYPE_INT,
            .val.as_int = aux->flee_prev
        };
        CHK_TRUE_RET(Attr_Write(stream, &flee_prev, "flee_prev"));

        CHK_TRUE_RET(G_Arrival_SaveUnitState(stream, &aux->arrival));
        Sched_TryYield();
    });

    return true;
}

bool G_Move_LoadState(struct SDL_RWops *stream)
{
    /* Flush the commands submitted during loading */
    move_update_gamestate();
    move_process_cmds();

    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_BOOL);
    s_click_move_enabled = attr.val.as_bool;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const int num_flocks = attr.val.as_int;
    Sched_TryYield();

    assert(vec_size(&s_flocks) == 0);
    for(int i = 0; i < num_flocks; i++) {

        struct flock new_flock;
        new_flock.ents = kh_init(entity);
        CHK_TRUE_RET(new_flock.ents);
        G_ArrivalGroup_Init(&new_flock.arrival);

        CHK_TRUE_JMP(Attr_Parse(stream, &attr, true), fail_flock);
        CHK_TRUE_JMP(attr.type == TYPE_INT, fail_flock);
        const int num_flock_ents = attr.val.as_int;

        for(int j = 0; j < num_flock_ents; j++) {

            CHK_TRUE_JMP(Attr_Parse(stream, &attr, true), fail_flock);
            CHK_TRUE_JMP(attr.type == TYPE_INT, fail_flock);

            uint32_t flock_ent_uid = attr.val.as_int;
            flock_add(&new_flock, flock_ent_uid);
        }

        CHK_TRUE_JMP(Attr_Parse(stream, &attr, true), fail_flock);
        CHK_TRUE_JMP(attr.type == TYPE_VEC2, fail_flock);
        new_flock.target_xz = attr.val.as_vec2;

        CHK_TRUE_JMP(Attr_Parse(stream, &attr, true), fail_flock);
        CHK_TRUE_JMP(attr.type == TYPE_INT, fail_flock);
        new_flock.dest_id = attr.val.as_int;

        CHK_TRUE_JMP(G_Arrival_LoadState(stream, &new_flock.arrival), fail_flock);

        vec_flock_push(&s_flocks, new_flock);
        Sched_TryYield();
        continue;

    fail_flock:
        G_ArrivalGroup_Destroy(&new_flock.arrival);
        kh_destroy(entity, new_flock.ents);
        return false;
    }

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const int num_ents = attr.val.as_int;
    Sched_TryYield();

    for(int i = 0; i < num_ents; i++) {

        uint32_t uid;
        struct movestate *ms;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        uid = attr.val.as_int;

        /* The entity should have already been loaded by the scripting state */
        khiter_t k = kh_get(state, s_entity_state_table, uid);
        CHK_TRUE_RET(k != kh_end(s_entity_state_table));
        ms = &kh_value(s_entity_state_table, k);

        struct movestate_aux *aux = movestate_aux_get(uid);
        CHK_TRUE_RET(aux);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        ms->state = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_FLOAT);
        ms->base_speed = attr.val.as_float;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_FLOAT);
        ms->speed_bonus = attr.val.as_float;
        ms->max_speed = MAX(0.0f, ms->base_speed + ms->speed_bonus);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC2);
        ms->velocity = attr.val.as_vec2;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC3);
        ms->next_pos = attr.val.as_vec3;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC3);
        ms->prev_pos = attr.val.as_vec3;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_QUAT);
        ms->next_rot = attr.val.as_quat;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_QUAT);
        ms->prev_rot = attr.val.as_quat;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_FLOAT);
        ms->step = attr.val.as_float;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        ms->left = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);

        const bool blocking = attr.val.as_bool;
        assert(ms->blocking);
        if(!blocking) {
            entity_unblock(uid);
        }

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        aux->wait_prev = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        aux->wait_ticks_left = attr.val.as_int;

        for(int i = 0; i < VEL_HIST_LEN; i++) {
        
            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_VEC2);
            aux->vel_hist[i] = attr.val.as_vec2;
        }

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        aux->vel_hist_idx = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        ms->surround_target_uid = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC2);
        aux->surround_target_prev = attr.val.as_vec2;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC2);
        aux->surround_nearest_prev = attr.val.as_vec2;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        ms->using_surround_field = attr.val.as_bool;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC2);
        aux->target_prev_pos = attr.val.as_vec2;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_FLOAT);
        aux->target_range = attr.val.as_float;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_QUAT);
        aux->target_dir = attr.val.as_quat;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_QUAT);
        aux->combat_facing = attr.val.as_quat;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC2);
        aux->flee_threat_pos = attr.val.as_vec2;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        aux->flee_prev = attr.val.as_int;

        CHK_TRUE_RET(G_Arrival_LoadUnitState(stream, &aux->arrival));

        /* Re-emit the start for an entity reloaded mid-move so client scripts resume
         * the movement animation. */
        if(!ent_still(ms)) {
            move_notify_motion_start(uid, ms);
        }

        /* A held-at-save unit reloads with the flag set; the edge-triggered
         * hold path never re-registers its blocker. */
        if((G_FlagsGet(uid) & ENTITY_FLAG_COMBAT_HELD)
        && !ent_still(ms) && !ms->blocking && !aux->soft_blocking) {
            entity_soft_block(uid);
        }

        Sched_TryYield();
    }

    return true;
}

