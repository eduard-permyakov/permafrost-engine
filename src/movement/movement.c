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

#define MEM_FILE_SYS MEM_SYS_MOVEMENT
#define MEM_FILE_SUB MEM_SUB_MOVEMENT_MOVEMENT

#include "public/movement.h"
#include "move_private.h"
#include "arrival.h"
#include "formation.h"
#include "clearpath.h"
#include "flocking.h"
#include "../game/public/game.h"
#include "../config.h"
#include "../camera.h"
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
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"
#include "../lib/public/vec.h"
#include "../lib/public/attr.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/stalloc.h"
#include "../lib/public/stalloc.h"

#include <assert.h>
#include <SDL.h>

#include "../mem.h"

#undef PF_MALLOC
#undef PF_CALLOC
#undef PF_REALLOC
#define PF_MALLOC(_n)       PF_MALLOC_TAGGED((_n), MEM_SYS_MOVEMENT, MEM_SUB_MOVEMENT_MOVEMENT)
#define PF_CALLOC(_c, _n)   PF_CALLOC_TAGGED((_c), (_n), MEM_SYS_MOVEMENT, MEM_SUB_MOVEMENT_MOVEMENT)
#define PF_REALLOC(_p, _n)  PF_REALLOC_TAGGED((_p), (_n), MEM_SYS_MOVEMENT, MEM_SUB_MOVEMENT_MOVEMENT)

static int hz_count(enum movement_hz hz);

/* For the purpose of movement simulation, all entities have the same mass,
 * meaning they are accelerate the same amount when applied equal forces. */
#define ENTITY_MASS           (1.0f)
#define SCALED_MAX_FORCE      MOVE_FLOCK_MAX_FORCE(hz_count(s_move_work.hz))
#define VEL_HIST_LEN          (14)
#define MAX_MOVE_TASKS        (64)
#define MAX_REBUILDS_PER_TICK (64)
/* Stale-served LOS entries whose rebuild came due refresh at this rate, so a
 * melee's mark conveyor cannot fire dozens of chain floods on one tick. */
#define MAX_LOS_STALE_REBUILDS_PER_TICK (16)

#define MAX(a, b)    ((a) > (b) ? (a) : (b))
#define MIN(a, b)    ((a) < (b) ? (a) : (b))
#define CLAMP(a, min, max) (MIN(MAX((a), (min)), (max)))
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
/* Contacts in a unit's way recorded per tick for the settle judgement */
#define MAX_WAY_LINKS                   (8)

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
    /* Tick from which a unit held up short of its target may look for a
     * nearer free side again; 0 asks at once. A cooldown, not saved. */
    unsigned           surround_requery_tick;
    /* Waits it went on from on its way round the target since it was sent */
    int                surround_resumes;
    /* How far from its target the unit took up the surround field, and the
     * tick before which it keeps off it after the field led it away. Not saved. */
    float              surround_on_dist;
    unsigned           surround_shun_tick;
    /* Additional state for entities in 'ENTER_ENTITY_RANGE' state 
     */
    vec2_t             target_prev_pos;
    float              target_range;
    /* Reach the unit had when its attack-move was ordered, which picks the enemy
     * field that guides it to a firing position rather than onto the enemy. Read
     * off the main thread, so it is cached rather than asked of combat. Transient.
     */
    float              attack_reach;
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
    /* Side of the last ClearPath deflection, kept CP_SIDE_HOLD_S */
    int8_t             cp_side;
    uint8_t            cp_side_ticks;
    /* A jammed unit registers as a temporary nav blocker so the floods route
     * the units behind it around the clump. Transient. */
    uint16_t           seek_stuck_ticks;
    uint16_t           seek_clear_ticks;
    /* Consecutive ticks coasting through a stale hole in the flow field */
    uint16_t           field_void_ticks;
    /* Whether the reach field is steering this unit, and how many consecutive
     * ticks it has been missing from the cache. Transient.
     */
    bool               range_latched;
    uint16_t           range_miss_ticks;
    /* Consecutive stand-still solves, and the relaxed solves they buy. Transient. */
    uint16_t           cp_stall_ticks;
    uint16_t           cp_relax_ticks;
    /* Ticks left in which the unit follows the field though its goal is in
     * sight: the straight way is shut by bodies. Transient. */
    uint16_t           los_off_ticks;
    /* Set once the unit has been flowing since its last order; only then may
     * it wall itself as jam-stuck (a queued launch rear is not a jam). */
    bool               seek_progressed;
    /* Standing on its formation cell while the cells in front of it are not
     * yet taken: visually settled, but neither blocking nor immovable.
     */
    bool               parked;
    /* The goal the ratchet below measures against, the nearest the unit has
     * come to it, and the ticks since it last came nearer. Transient.
     */
    vec2_t             goal_ref;
    float              best_goal_dist;
    uint16_t           noapproach_ticks;
    /* The current travel segment: where it began, the ground covered since,
     * where the unit was last tick, and the ticks in it. Transient.
     */
    vec2_t             seg_start;
    vec2_t             seg_last;
    float              seg_path;
    uint16_t           seg_ticks;
    /* What stood in the unit's way on the tick last consumed: the contacts
     * ahead, whether the field had nothing to say, and the direction it
     * wanted. Transient.
     */
    uint8_t            way_links;
    bool               way_void;
    /* Its formation asked it to stand: the cell assignment was still being
     * computed, it was parked on its cell, or the drag was pacing it. */
    bool               way_paced;
    /* The heading gate held its step while it turned to its way. */
    bool               way_gated;
    /* Stepping into its formation cell facing the way the rank faces; once
     * begun, kept for the rest of the approach. Transient.
     */
    bool               dressing;
    /* Stopped to fight, short of the end of its order, which it goes back to
     * when the fight is over. Transient.
     */
    bool               order_paused;
    /* Diagnostic: the tick's flags, for the [root] and [hold] lines */
    uint16_t           way_bits;
    uint32_t           way[MAX_WAY_LINKS];
    vec2_t             way_dir;
    /* What a held unit waits on: the body in its way that may yet move, its
     * goal, and how many tiles of its layer's cells here and ahead had been
     * freed when it stopped. Transient.
     */
    bool               hold_way;
    uint32_t           hold_link;
    vec2_t             hold_goal;
    uint32_t           hold_gen_here;
    uint32_t           hold_gen_ahead;
    vec2_t             hold_ahead;
    /* Whether the facing is following the travel direction. Transient. */
    bool               facing_follows;
    /* Walled out of its cell by its own side: walks through its own side's
     * bodies and stamps to reach it, for a bounded number of ticks.
     */
    bool               phasing;
    uint16_t           phase_ticks;
    /* Steps vetoed in a row for landing on a body, and the pass left to walk through */
    uint16_t           veto_ticks;
    uint16_t           wedge_ticks;
    bool               soft_blocking;
    vec2_t             soft_block_pos;
    float              soft_block_radius;
    /* Keeps EVENT_MOTION_START/END strictly alternating; clients assert the
     * pairing. Transient. */
    bool               motion_stopped;
    /* Ticks in a row a unit on its way took no step and turned on no spot:
     * held up, it shows its idle clip rather than walking in place. Transient.
     */
    uint16_t           standing_ticks;
    /* Ticks in a row the solve's only step lay behind the band of the facing,
     * and the ticks left of turning to take it instead of the wish. Transient.
     */
    uint16_t           band_held_ticks;
    uint16_t           band_yield_ticks;
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
    /* The landing ground is chosen and reserved on the tick after the order
     * and after every change of membership.
     */
    bool             plan_pending;
};

struct formation_state{
    formation_id_t fid;
    bool           assignment_ready;
    bool           assigned_to_cell;
    bool           in_range_of_cell;
    bool           arrived_at_cell;
    bool           may_park;
    bool           straggler;
    vec2_t         normal_cohesion_force;
    vec2_t         normal_align_force;
    vec2_t         normal_drag_force;
    quat_t         target_orientation;
};

enum movestate_flags{
    UPDATE_SET_STATE        = (1 << 0),
    UPDATE_SET_VELOCITY     = (1 << 1),
    UPDATE_SET_NEXT_POS     = (1 << 4),
    UPDATE_SET_PREV_POS     = (1 << 5),
    UPDATE_SET_NEXT_ROT     = (1 << 8),
    UPDATE_SET_PREV_ROT     = (1 << 9),
    UPDATE_SET_DEST         = (1 << 10),
    UPDATE_SET_TARGET_PREV  = (1 << 11),
    UPDATE_SET_TARGET_DIR   = (1 << 13),
    UPDATE_TURNING_IN_PLACE = (1 << 14),
    /* Diagnostic only: translation was zeroed by the heading gate this tick */
    UPDATE_HEADING_GATED    = (1 << 15),
    /* A seeking unit made no progress this tick / has a thin local crowd */
    UPDATE_SEEK_STUCK       = (1 << 16),
    UPDATE_SEEK_CLEAR       = (1 << 17),
    /* The seek pin held this tick */
    UPDATE_SEEK_PINNED      = (1 << 18),
    /* Diagnostic only: the step was refused by the landing tile */
    UPDATE_VETO_UNPATHABLE  = (1 << 19),
    UPDATE_VETO_BLOCKED     = (1 << 20),
    UPDATE_FIELD_VOID       = (1 << 21),
    /* The facing followed the travel direction this tick */
    UPDATE_FACING_FOLLOWS   = (1 << 22),
    /* The solve found a step that lay behind the band of the facing */
    UPDATE_BAND_HELD        = (1 << 23)
};

struct movestate_patch{
    enum movestate_flags flags;
    enum move_state   next_state;
    vec2_t               next_velocity;
    bool                 next_block;
    vec3_t               next_ppos;
    vec3_t               next_npos;
    quat_t               next_nrot;
    quat_t               next_prot;
    vec2_t               next_dest;
    bool                 next_attack;
    vec2_t               next_target_prev;
    quat_t               next_target_dir;
};

struct move_work_in{
    uint32_t       ent_uid;
    /* The unit's flock (and its snapshot), resolved once at submit time;
     * flock membership is stable for the duration of the tick. */
    struct flock            *flock;
    const struct flock_snap *fsnap;
    vec2_t         ent_des_v;
    /* Whether ent_des_v came from the reach-seeded enemy field rather than the
     * flock's point field, which decides what the unit is arriving at.
     */
    bool           range_field;
    float          speed;
    /* The unit's top speed this tick, roads included */
    float          max_speed;
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
    /* Contacts in the way, stopped or not, capacity MAX_WAY_LINKS */
    uint32_t      *links;
    size_t         nlinks;
    /* Nearest neighbour in reach, and whether this unit gives it the lane */
    uint32_t       nn_uid;
    float          nn_dist;
    bool           hug;
    /* Terrain-impassable subset of the tile obstacles */
    size_t         nterrain;
    /* The field sample was void and the unit is coasting toward the goal */
    bool           field_void;
    /* Diagnostic: the arrival ball, not the field, gave the desired velocity */
    bool           arrival_vdes;
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
    /* The velocity solve's inputs, written by the gather part of the velocity
     * phase for a solve on the CPU or the GPU */
    vec2_t            vpref;
    struct cp_terrain terrain;
    int               solve_side;
    bool              relax;
    int               cap_slot;
    /* The unit's slot in the GPU solve, or a GPU_SLOT_* value */
    int               gpu_slot;
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

enum move_work_type{
    WORK_TYPE_CPU,
    WORK_TYPE_GPU
};

enum move_work_status{
    WORK_COMPLETE,
    WORK_INCOMPLETE
};

/* A snapshot of one wide movable, matched against ordinary units by exact pair
 * reach so the giants never enter the ordinary crowd query. */
struct large_movable{
    vec2_t   xz_pos;
    float    radius;
    uint32_t flags;
    uint32_t uid;
};

/* A unit simulated from the end of its step while drawn short of it. */
struct step_end{
    uint32_t uid;
    vec2_t   drawn;
    vec2_t   end;
};

struct move_work{
    struct memstack           mem;
    struct move_world         gamestate;
    enum move_work_type       type;
    struct nav_unit_query_ctx unit_query_ctx;
    enum movement_hz          hz;
    struct move_work_in      *in;
    struct move_work_out     *out;
    struct cp_ent            *neighb_mem;
    vec2_t                   *tile_mem;
    uint32_t                 *link_mem;
    struct step_end          *step_ends;
    size_t                    nstep_ends;
    struct flock_snap        *flock_snaps;
    /* Parallel to in/out, NULL unless tracing */
    struct move_trace        *trace;
    /* Largest movable radius seen, for the centre-based neighbour queries */
    float                     max_radius;
    /* Widest ordinary (<= LARGE_MOVABLE_RADIUS) movable per class, indexed by
     * air-ness, sizing the crowd query; giants go in the registry below. */
    float                     small_max_radius[2];
    float                     small_max_step[2];
    struct large_movable     *large_movables;
    size_t                    nlarge;
    size_t                    nwork;
    size_t                    ntasks;
    uint32_t                  tids[MAX_MOVE_TASKS];
    /* The GPU solve's sizes and the per-slot offsets into its buffers */
    size_t                    gpu_nunits;
    size_t                    gpu_nnbs;
    size_t                    gpu_nwords;
    uint32_t                 *gpu_first;
    uint32_t                 *gpu_pfirst;
    struct gpu_parity_ref    *gpu_parity;
    struct future             futures[MAX_MOVE_TASKS];
};

/* One unit's solve for the GPU, as shaders/compute/movement.glsl reads it:
 * all 4-byte scalars, so the std430 layout is this one.
 */
struct gpu_cp_unit{
    float    px, pz;
    float    vx, vz;
    float    dx, dz;
    float    radius;
    float    max_step;
    float    horizon;
    uint32_t flags;
    int32_t  side;
    uint32_t first;
    uint32_t ndyn, nstat, ntiles;
    int32_t  prow, pcol;
    uint32_t pdim;
    uint32_t pfirst;
    float    travel;
};

/* A dynamic or static neighbour, or a wall tile's centre, of a GPU solve */
struct gpu_cp_nb{
    float px, pz;
    float vx, vz;
    float radius;
};

struct gpu_cp_result{
    float    vx, vz;
    int32_t  side;
    uint32_t diag;
};

/* The CPU's answer to a solve the GPU takes, on the same inputs */
struct gpu_parity_ref{
    vec2_t               vel;
    struct cp_solve_diag diag;
};

#define GPU_UNIT_RELAX       (1u << 0)
#define GPU_UNIT_ON_BLOCKED  (1u << 1)
#define GPU_DIAG_RETRIES     (0xffu)
#define GPU_DIAG_GAVE_UP     (1u << 8)
#define GPU_DIAG_PATCH_MISS  (1u << 9)
/* The widest tile patch a GPU solve takes: a unit whose step could land
 * further out is solved on the CPU. */
#define GPU_PATCH_MAX_DIM    (15)

enum{
    /* No solve: the gather wrote the output */
    GPU_SLOT_NONE = -1,
    /* Solved on the CPU */
    GPU_SLOT_CPU  = -2,
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
    MOVE_CMD_SET_SEEK_PIN,
    MOVE_CMD_RESUME_ORDER
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
            bool   for_combat;
        }stop;
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

/* The exact tuple a block's nav refcounts were taken with; the unblock must
 * mirror it even if the entity's flags or faction changed while blocked.
 */
struct block_ref{
    int      faction_id;
    uint32_t flags;
};

KHASH_MAP_INIT_INT(state, struct movestate)
KHASH_MAP_INIT_INT(auxstate, struct movestate_aux)
KHASH_MAP_INIT_INT64(blockref, struct block_ref)
KHASH_MAP_INIT_INT(findex, int)

QUEUE_TYPE(cmd, struct move_cmd)
QUEUE_IMPL(static, cmd, struct move_cmd)

VEC_TYPE(flock, struct flock)
VEC_IMPL(static inline, flock, struct flock)

VEC_TYPE(step, struct move_step)
VEC_IMPL(static inline, step, struct move_step)

VEC_TYPE(facing, struct move_facing)
VEC_IMPL(static inline, facing, struct move_facing)

VEC_TYPE(reset, struct move_reset)
VEC_IMPL(static inline, reset, struct move_reset)

VEC_TYPE(msptr, struct movestate*)
VEC_IMPL(static inline, msptr, struct movestate*)

SHARED_PTR_ASSERT_LAYOUT(struct refcounted_map, sp);

static void move_push_cmd(struct move_cmd cmd);
static void do_set_dest(uint32_t uid, vec2_t dest_xz, bool attack);
static float attack_move_reach(uint32_t uid, const struct flock *fl);
static float attack_move_seed_reach(uint32_t uid, uint32_t flags);
static bool using_surround_field(uint32_t uid, uint32_t target);
static void do_stop(uint32_t uid);
static void do_set_seek_pin(uint32_t uid, uint32_t target);
static void clear_seek_pin(uint32_t uid);
static bool ent_still(const struct movestate *ms);
static bool yields_behind_band(enum move_state state);
static void move_notify_motion_start(uint32_t uid, struct movestate *ms);
static void move_notify_motion_end(uint32_t uid);
static void do_update_pos(uint32_t uid, vec2_t pos);
static void entity_follow_own_move(uint32_t uid, struct movestate *ms);
static void settle_own_moves(void);
static void settle_own_move(uint32_t uid, struct movestate *ms);
static void snapshot_step_ends(void);
static void snapshot_step_ends_grid(void);
static void resume_waiting_units(void);
static void settle_reset(struct movestate_aux *aux);
static float cell_closing_radius(float radius);
static bool body_rooted(uint32_t uid);
static uint32_t way_first_open(uint32_t uid, const struct movestate_aux *aux);
static bool way_body_final(uint32_t uid, uint32_t body);
static vec2_t keep_right_vpref(const struct move_work_in *in, const struct movestate *ms,
                               const struct flock *flock, vec2_t vpref, int *inout_side);
static void hold_begin(uint32_t uid, const struct movestate *ms, struct movestate_aux *aux);
static struct result navigation_tick_task(void *arg);

/* Parameters controlling steering/flocking behaviours */
#define SEPARATION_FORCE_SCALE          (0.6f)
#define MOVE_ARRIVE_FORCE_SCALE         (0.5f)
#define MOVE_COHESION_FORCE_SCALE       (0.15f)
#define ALIGNMENT_FORCE_SCALE           (0.15f)

#define SEPARATION_BUFFER_DIST          (0.0f)
#define ADJACENCY_SEP_DIST              (5.0f)
/* A unit in contact whose wish has collapsed under the forces cancelling
 * wishes a step aside instead: this far off the way, this much of its pace
 */
#define MOVE_ESCAPE_GAP                 (0.5f)
#define CELL_ARRIVAL_RADIUS             (30.0f)

#define COLLISION_MAX_SEE_AHEAD         (10.0f)
#define WAIT_S                          (3.0f)
/* Waits on its way round a target before a unit stands where it is as arrived */
#define SURROUND_MAX_RESUMES            (3)
#define MAX_TURN_RATE                   (15.0f) /* degree/tick */
#define SCALED_MAX_TURN_RATE            (MAX_TURN_RATE / hz_count(s_move_work.hz) * 20.0)
#define MOVE_HEADING_HALT               (90.0f) /* degrees; halt a moving unit to re-aim past this */
#define MOVE_HEADING_RESUME             (10.0f) /* degrees; resume/start a halted unit within this */
#define MOVE_BAND_YIELD_S               (0.5f)  /* held behind the band this long, a unit turns to the step */
#define MOVE_BAND_YIELD_HOLD_S          (1.0f)  /* and keeps to the step's heading this long */
#define CELL_DRESS_TIME_S               (1.5f)  /* a cell this many seconds of shuffling away is side-stepped into */
/* On a road, a moving unit keeps to the middle of the right half of it, 
 * steering at most this share of its pace sideways to get there */
#define KEEP_RIGHT_MAX_LATERAL          (0.35f)
/* Per unit of distance off the lane, the share of its pace it steers back */
#define KEEP_RIGHT_GAIN                 (0.08f)
/* Roads narrower or wider than this across the way (junctions, squares) 
 * have no lanes; a road two tiles wide has */
#define KEEP_RIGHT_MIN_WIDTH            (12.0f)
#define KEEP_RIGHT_MAX_WIDTH            (80.0f)
/* This close to its goal a unit lines up for arriving instead */
#define KEEP_RIGHT_GOAL_DIST            (48.0f)
/* The passing side, in cp_side_of's sense, that keeps a unit to its right as
 * seen on screen: the view mirrors the world */
#define KEEP_RIGHT_SIDE                 (1)
#define MOTION_IDLE_S                   (0.5f)  /* held up this long, a unit shows its idle clip */
#define MAX_NEIGHBOURS                  (32)
#define CLEARPATH_REACT_S               (0.5f)  /* An approaching pair comes into each other's solve this long before contact */
#define CLEARPATH_STILL_SPEED           (0.3f)  /* A neighbour slower than this is treated as static (full, non-reciprocal avoidance) so a settling unit is not passed through */
/* Movables wider than this (the rare siege engines and skyships) are matched
 * against a small registry so they do not widen every soldier's crowd query. */
#define LARGE_MOVABLE_RADIUS            (8.0f)
#define MAX_LARGE_MOVABLES              (1024)

#define SURROUND_LOW_WATER_X            (CHUNK_WIDTH/3.0f)
#define SURROUND_HIGH_WATER_X           (CHUNK_WIDTH/2.0f)
#define SURROUND_LOW_WATER_Z            (CHUNK_HEIGHT/3.0f)
#define SURROUND_HIGH_WATER_Z           (CHUNK_HEIGHT/2.0f)
/* A surround field leading a unit this much further from its target than where
 * it took the field up goes the long way round a crowd sealing the near side;
 * the unit keeps to the path field for a while instead of pacing between the two */
#define SURROUND_DETOUR_SLACK           (32.0f)
#define SURROUND_SHUN_TICKS             (200)

/* How long a unit heads for its goal across a stale hole in the field before
 * treating the destination as genuinely unreachable.
 */
#define FIELD_VOID_COAST_S              (2.0f)

/* A straggler gives up phasing after this long, so that one which can never
 * reach its cell does not walk through its own side for good.
 */
#define PHASE_MAX_TICKS                 (400)
/* Every step vetoed this long, the unit is wedged, as between two walls with
 * another body in the gap, and walks through bodies for a pass this long */
#define WEDGE_VETO_S                    (3.0f)
#define WEDGE_PASS_TICKS                (60)
/* A group none of whose members has come nearer its goal for this long has
 * stopped for good, and each member takes the ground it stands on. This is
 * the one terminal timer; every other settle rule is a distance to the
 * unit's own goal.
 */
#define SETTLE_WINDOW_S                 (5.0f)
#define SETTLE_WALK_WINDOW_S            (20.0f) /* walking without coming nearer the goal this long = stuck */
#define SETTLE_TRAVEL_RADII             (2.0f)  /* a segment covering this x radius ... */
#define SETTLE_TRAVEL_STRAIGHTNESS      (0.5f)  /* ... and ending this x its path away is travel */
#define SETTLE_TRAVEL_PACE              (0.25f) /* travel covers at least this x the ground it could */
/* Standing this far from where it last moved on from is progress, whichever
 * way: a detour round a wall gains no straight-line distance for a while, and
 * a unit shuffling on the spot gains nothing.
 */
#define GROUP_PROGRESS_MIN              (3.0f)
/* How long the reach field has to be absent before a unit it was steering gives
 * up on it and goes back to the ordered point. */
#define RANGE_FIELD_MISS_TICKS          (30)
/* A unit standing still this long inside a crowd becomes a soft blocker,
 * released once the local crowd thins for the hold duration. */
#define SEEK_STUCK_BLOCK_S              (1.0f)
/* A unit stuck pressing straight at a goal it can see follows the field round
 * the bodies in the way for this long instead */
#define LOS_STUCK_FIELD_S               (3.0f)
/* Neighbours in ClearPath reach that are themselves trying to move: a jam is
 * mutual obstruction, so parked bodies are not evidence of one.
 */
#define SEEK_RELEASE_NEIGHBS            (6)
#define SEEK_RELEASE_HOLD_S             (2.0f)
/* Stagger registrations so a mass jam doesn't spike the invalidations */
#define SEEK_BLOCK_BUDGET_PER_TICK      (16)
/* Ticks without a deflection before the passing side is forgotten */
#define CP_SIDE_HOLD_S                  (1.0f)
/* Stand-still answers before the solve is relaxed, and the relaxation's hold */
#define CP_STALL_RELAX_S                (1.0f)
#define CP_STALL_RELAX_HOLD_S           (1.0f)
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
/* A unit held up short of its target looks for a nearer free side about once
 * a second, staggered, and only moves to one clearly nearer or when its own
 * is taken; asking every tick both costs a flood and turns it back and forth. */
#define SURROUND_REQUERY_TICKS          (15)
#define SURROUND_REQUERY_SPREAD         (8)
#define SURROUND_SWITCH_MARGIN          (8.0f)
/* Within this of its spot, a surrounding unit counts as having got there */
#define SURROUND_AT_SPOT                (8.0f)
/* Terrain walls take contact-ring slots a unit could never fill; credit them
 * toward the crowd test so jams against walls can arm the drain. Blocker
 * stamps get none: a stamp is the output of walling, not evidence for it.
 */
#define SEEK_WALL_NEIGHB_CREDIT         (3)
#define ATTACK_MOVE_REACH_FRAC          (0.9f)
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
    MV_TRACE_BAND_HELD       = (1 << 14),
    MV_TRACE_BAND_YIELD      = (1 << 15),
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
/* pf.game.road_keep_right, read once per tick for the velocity workers */
static bool                    s_keep_right = true;

/* Units the last apply moved, for the stamp follow-up after the drain. */
static vec_entity_t            s_own_moves;
static vec_flock_t             s_flocks;
/* Per-tick uid -> flock index, rebuilt alongside the flock snapshots */
static khash_t(findex)        *s_flock_index;
static struct nav_unit_query_ctx s_unit_query_ctx_ring[2];
static int                     s_gs_slot;
static struct refcounted_map  *s_parked_snapshot;
static khash_t(state)         *s_entity_state_table;
static khash_t(auxstate)      *s_entity_aux_table;
/* Keyed by (uid << 1) | soft-ness; holds the incref-time tuple. */
static khash_t(blockref)      *s_block_refs;

/* Store the most recently issued move command location for debug rendering */
static bool                    s_last_cmd_dest_valid = false;
static dest_id_t               s_last_cmd_dest;

static struct move_work        s_move_work;
static queue_cmd_t             s_move_commands;
static struct memstack         s_eventargs;

/* pf.debug.log_cp_captures, hoisted once per tick for the worker phase */
static bool                    s_log_cp_captures;
static bool                    s_log_perf_csv;
static bool                    s_move_parallel_executor;
static struct cp_capture       s_cp_captures[CP_CAPTURE_MAX_PER_TICK];
static SDL_atomic_t            s_cp_ncaptures;

/* pf.debug.log_move_trace_min_radius, hoisted once per tick; negative = off */
static float                   s_move_trace_min_radius = -1.0f;
/* pf.debug.log_move_trace_uid, hoisted once per tick; negative = none */
static int                     s_move_trace_uid = -1;
/* Traced whatever their radius, so a session can be driven by the selection */
static const vec_entity_t     *s_move_trace_sel;
static unsigned                s_move_trace_tick;

static int                     s_soft_block_budget;
/* Monotone: a departed unit leaves the queries slightly wide, never narrow */
static float                   s_max_sel_radius;

/* Every tick run so far, by which the game clock is paced to the frames */
static unsigned long           s_ticks_done;
static enum movement_hz        s_move_hz = MOVE_HZ_20;
static struct refcounted_map  *s_nav_snapshot;
static uint32_t                s_nav_snapshot_gen;
static bool                    s_use_gpu = true;
/* The GPU solve's persistently mapped buffers, the flag the render thread
 * sets once it has (re)created them, and the number of the last solve whose
 * results are visible */
static struct gpu_move_bufs    s_gpu_bufs;
static SDL_atomic_t            s_gpu_reserved;
static SDL_atomic_t            s_gpu_solved;
static int                     s_gpu_seq;
/* pf.debug.gpu_move_parity: every GPU solve is also run on the CPU and the
 * disagreements are counted */
static bool                    s_gpu_parity;
static SDL_atomic_t            s_gpu_nparity;
static SDL_atomic_t            s_gpu_nvel_diff;
static SDL_atomic_t            s_gpu_ndiag_diff;
/* The times of the last solve, when logging perf */
static struct gpu_move_times   s_gpu_times;
static uint64_t                s_prepare_copy_ticks;
static bool                    s_submit_gather_active;

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
 * main-thread fields by the tick's passes; read by the main thread at the join
 * (valid once s_tick_task_future is ready).
 */
static struct nav_tick_sample  s_last_nav_tick_stats;
static uint64_t                s_submit_pc;
static unsigned long           s_submit_frame;

/* What the last pass left for the game to change */
static vec_step_t              s_out_steps;
static vec_msptr_t             s_out_step_states;
static vec_entity_t            s_out_stopped;
static vec_entity_t            s_out_held_still;
static vec_facing_t            s_out_facings;
static vec_reset_t             s_out_resets;

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

static bool flock_try_remove(struct flock *flock, uint32_t uid)
{
    khiter_t k;
    if((k = kh_get(entity, flock->ents, uid)) != kh_end(flock->ents)) {
        kh_del(entity, flock->ents, k);
        Move_Formation_RemoveUnit(uid);
        return true;
    }
    return false;
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
    return Move_ArrivalGroup_ForLayer(&flock->arrival, Entity_NavLayerWithRadius(flags, radius));
}

static void block_ref_save(uint32_t uid, int soft, int faction_id, uint32_t flags)
{
    int ret;
    khiter_t k = kh_put(blockref, s_block_refs, ((uint64_t)uid << 1) | soft, &ret);
    assert(ret != -1);
    kh_val(s_block_refs, k) = (struct block_ref){faction_id, flags};
}

static struct block_ref block_ref_take(uint32_t uid, int soft, int faction_id, uint32_t flags)
{
    khiter_t k = kh_get(blockref, s_block_refs, ((uint64_t)uid << 1) | soft);
    if(k == kh_end(s_block_refs))
        return (struct block_ref){faction_id, flags};

    struct block_ref ret = kh_val(s_block_refs, k);
    kh_del(blockref, s_block_refs, k);
    return ret;
}

static void entity_block(uint32_t uid)
{
    float sel_radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
    vec2_t pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
    M_NavBlockersIncref(pos, sel_radius, faction_id, flags, s_map);
    M_NavInvalidateZoneFieldsAt(s_move_work.gamestate.map, pos,
        Entity_NavLayerWithRadius(flags, sel_radius));
    block_ref_save(uid, 0, faction_id, flags);

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

    struct block_ref ref = block_ref_take(uid, 0,
        G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid),
        G_FlagsGetFrom(s_move_work.gamestate.flags, uid));
    M_NavBlockersDecref(ms->last_stop_pos, ms->last_stop_radius,
        ref.faction_id, ref.flags, s_map);
    M_NavInvalidateZoneFieldsAt(s_move_work.gamestate.map, ms->last_stop_pos,
        Entity_NavLayerWithRadius(ref.flags, ms->last_stop_radius));
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
    int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
    M_NavBlockersIncref(pos, sel_radius, faction_id, flags, s_map);
    block_ref_save(uid, 1, faction_id, flags);

    if(s_log_perf_csv) {
        const struct movestate *dms = movestate_get(uid);
        fprintf(stdout, "[soft] %u,%u,on,%s,%u,%u,%u,%.2f,%.2f\n", s_move_trace_tick, uid,
            dms ? s_state_str[dms->state] : "?", (unsigned)aux->cp_stall_ticks,
            (unsigned)aux->seek_stuck_ticks, (unsigned)aux->seek_clear_ticks, pos.x, pos.z);
    }
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

    struct block_ref ref = block_ref_take(uid, 1,
        G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid),
        G_FlagsGetFrom(s_move_work.gamestate.flags, uid));
    M_NavBlockersDecref(aux->soft_block_pos, aux->soft_block_radius,
        ref.faction_id, ref.flags, s_map);

    if(s_log_perf_csv) {
        const struct movestate *dms = movestate_get(uid);
        fprintf(stdout, "[soft] %u,%u,off,%s,%u,%u,%u,%.2f,%.2f\n", s_move_trace_tick, uid,
            dms ? s_state_str[dms->state] : "?", (unsigned)aux->cp_stall_ticks,
            (unsigned)aux->seek_stuck_ticks, (unsigned)aux->seek_clear_ticks,
            aux->soft_block_pos.x, aux->soft_block_pos.z);
    }
    aux->soft_blocking = false;
    aux->seek_stuck_ticks = 0;
    aux->seek_clear_ticks = 0;
    aux->seek_progressed = false;

    /* Motion resumes unless settling or still combat-held. */
    struct movestate *ms = movestate_get(uid);
    if(ms && !ent_still(ms))
        move_notify_motion_start(uid, ms);
}

/* A unit whose only step lies behind it does not stand facing its wish for
 * good: held there a while, it turns to take the step for a while instead.
 */
static void entity_apply_band_held(struct movestate_aux *aux, bool band_held)
{
    aux->band_held_ticks = band_held ? MIN(aux->band_held_ticks + 1, UINT16_MAX) : 0;
    if(aux->band_held_ticks >= MOVE_BAND_YIELD_S * hz_count(s_move_work.hz)) {
        aux->band_held_ticks = 0;
        aux->band_yield_ticks = MOVE_BAND_YIELD_HOLD_S * hz_count(s_move_work.hz);
    }else if(aux->band_yield_ticks > 0) {
        aux->band_yield_ticks--;
    }
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
    aux->los_off_ticks = 0;
    aux->band_held_ticks = 0;
    aux->band_yield_ticks = 0;
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

static bool ent_still(const struct movestate *ms)
{
    return (ms->state == STATE_ARRIVED || ms->state == STATE_WAITING);
}

static float entity_speed(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    if(Move_Formation_GetForEnt(uid) != NULL_FID) {
        return Move_Formation_Speed(uid);
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

    struct movestate_aux *aux = movestate_aux_get(uid);
    aux->standing_ticks = 0;
    if(G_FlagsGet(uid) & ENTITY_FLAG_COMBAT_HELD)
        return;
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
    if(newstate != STATE_TURNING) {
        vec_entity_push(&s_out_stopped, uid);
    }

    if(newstate == STATE_WAITING) {
        struct movestate_aux *aux = movestate_aux_get(uid);
        aux->wait_prev = ms->state;
        aux->wait_ticks_left = WAIT_S * hz_count(s_move_work.hz);
        hold_begin(uid, ms, aux);
    }

    ms->state = newstate;
    ms->velocity = (vec2_t){0.0f, 0.0f};

    if(block) {
        entity_block(uid);
    }
    assert(ent_still(ms));
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
        bool removed = flock_try_remove(curr_flock, uid);

        if(kh_size(curr_flock->ents) == 0) {
            kh_destroy(entity, curr_flock->ents);
            Move_ArrivalGroup_Destroy(&curr_flock->arrival);
            vec_flock_del(&s_flocks, i);
        }else if(removed) {
            curr_flock->plan_pending = true;
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
                       enum nav_layer layer, bool attack, bool roads, enum formation_type type)
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
        struct movestate_aux *caux = movestate_aux_get(curr_ent);
        caux->attack_reach = attack ? attack_move_seed_reach(curr_ent,
            G_FlagsGetFrom(s_move_work.gamestate.flags, curr_ent)) : 0.0f;
        caux->range_latched = false;
        caux->range_miss_ticks = 0;
        Move_Arrival_InitUnit(&caux->arrival,
            G_Pos_GetXZFrom(s_move_work.gamestate.positions, curr_ent));
        settle_reset(caux);
    }

    /* The flow fields will be computed on-demand during the next movement update tick */
    new_flock.target_xz = target_xz;
    if(attack) {
        int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, first);
        new_flock.dest_id = M_NavDestIDForPosAttacking(s_map, target_xz, layer, faction_id);
    }else{
        new_flock.dest_id = M_NavDestIDForPos(s_map, target_xz, layer);
    }
    if(roads) {
        new_flock.dest_id = N_DestIDWithRoads(new_flock.dest_id);
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
        merge_flock->target_xz = target_xz;
        Move_ArrivalGroup_Reset(&merge_flock->arrival);
        merge_flock->plan_pending = true;

    }else{
        new_flock.plan_pending = true;
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
        if(first == NULL_UID || Move_Formation_GetForEnt(first) != NULL_FID) {
            Move_ArrivalGroup_Deactivate(&flock->arrival);
            flock->plan_pending = false;
            continue;
        }

        STALLOC(struct arrival_member, members, kh_size(flock->ents));
        int n = build_arrival_members(flock, members, kh_size(flock->ents));
        /* The landing ground is chosen and reserved as soon as the order is in,
         * with every member the order brought, so that later orders fit around
         * it. A formation's members land on its cells instead.
         */
        if(flock->plan_pending) {
            flock->plan_pending = false;
            Move_ArrivalGroup_Reset(&flock->arrival);
            Move_ArrivalGroup_Plan(&flock->arrival, s_map, flock->target_xz, members, n,
                N_DestUsesRoads(flock->dest_id));
        }
        Move_ArrivalGroup_Update(&flock->arrival, s_map, flock->target_xz, members, n,
            N_DestUsesRoads(flock->dest_id));
        STFREE(members);
    }
}

/* The point a member's order has it heading for right now */
static vec2_t member_goal(const struct flock *flock, uint32_t uid)
{
    if(Move_Formation_GetForEnt(uid) != NULL_FID && Move_Formation_AssignedToCell(uid))
        return Move_Formation_CellPosition(uid);

    const struct arrival_unit_state *us = &movestate_aux_get(uid)->arrival;
    if(us->sink_valid)
        return us->sink;
    return flock->target_xz;
}

/* A member on its way to a point of its own; the work and combat states
 * answer to their targets, not to the group's ground.
 */
static bool member_on_the_way(uint32_t uid, const struct movestate *ms)
{
    switch(ms->state) {
    case STATE_MOVING:
    case STATE_MOVING_IN_FORMATION:
    case STATE_ARRIVING_TO_CELL:
    case STATE_TURNING:
        return true;
    case STATE_WAITING: {
        const struct movestate_aux *aux = movestate_aux_get(uid);
        return aux->wait_prev == STATE_MOVING
            || aux->wait_prev == STATE_MOVING_IN_FORMATION
            || aux->wait_prev == STATE_ARRIVING_TO_CELL;
    }
    default:
        return false;
    }
}

/* A member stops for good where it stands: at its goal, or against bodies
 * that have. A formation member gives up its cell so that it holds nobody up.
 */
static void root_member_in_place(struct flock *flock, uint32_t uid, struct movestate *ms,
                                 const char *why)
{
    if(Move_Formation_GetForEnt(uid) != NULL_FID)
        Move_Formation_ConcedeCell(uid);
    if(s_log_perf_csv) {
        vec2_t goal = member_goal(flock, uid);
        vec2_t pos = G_Pos_GetXZ(uid);
        vec2_t to_goal;
        PFM_Vec2_Sub(&goal, &pos, &to_goal);
        const struct movestate_aux *taux = movestate_aux_get(uid);
        fprintf(stdout, "[root] %u,%u,%s,%s,%.1f,%d,%d,%u,%d,%x,%u,%u\n", s_move_trace_tick,
            uid, why, s_state_str[ms->state], PFM_Vec2_Len(&to_goal), (int)taux->parked,
            (int)taux->way_links, (unsigned)taux->cp_stall_ticks, (int)taux->soft_blocking,
            (unsigned)taux->way_bits, (unsigned)taux->standing_ticks,
            (unsigned)taux->noapproach_ticks);
    }

    /* A waiting member already blocks, but its watchers ignored the stop it
     * announced while it was still to move on, so it announces the arrival
     */
    if(ent_still(ms)) {
        ms->state = STATE_ARRIVED;
        if(!(G_FlagsGet(uid) & ENTITY_FLAG_COMBAT_HELD))
            move_notify_motion_finished(uid);
        return;
    }
    entity_reset_seek_counters(uid);
    entity_finish_moving(uid, STATE_ARRIVED, true);
}

/* A member that cannot come nearer while something in its way may yet move
 * is held up, not done: it waits, standing and blocking like a settled unit,
 * and is sent on again when what held it changes.
 */
static void hold_member_for_way(uint32_t uid, struct movestate *ms, struct movestate_aux *aux,
                                const char *why, uint32_t open)
{
    aux->noapproach_ticks = 0;
    if(ent_still(ms))
        return;
    if(s_log_perf_csv) {
        fprintf(stdout, "[hold] %u,%u,%s,%s,%u,%d,%x\n", s_move_trace_tick, uid, why,
            s_state_str[ms->state], open, (int)aux->soft_blocking, (unsigned)aux->way_bits);
    }
    aux->hold_link = open;
    entity_reset_seek_counters(uid);
    entity_finish_moving(uid, STATE_WAITING, true);
}

/* Start the approach ratchet afresh: a new goal has no best distance yet. */
static void settle_reset(struct movestate_aux *aux)
{
    aux->goal_ref = (vec2_t){0.0f, 0.0f};
    aux->best_goal_dist = INFINITY;
    aux->seg_ticks = 0;
    aux->seg_path = 0.0f;
    aux->noapproach_ticks = 0;
    aux->way_links = 0;
    aux->way_void = false;
    aux->way_paced = false;
    aux->way_gated = false;
    aux->order_paused = false;
    aux->way_dir = (vec2_t){0.0f, 0.0f};
}

/* Whether a body in a unit's way has stopped for good: settled, and not
 * merely standing to fight.
 */
static bool body_rooted(uint32_t uid)
{
    const struct movestate *ms = movestate_get(uid);
    if(!ms)
        return false;
    /* Parked on its own formation cell, it waits only to turn and settle. */
    if(movestate_aux_get(uid)->parked)
        return true;
    if(ms->state != STATE_ARRIVED)
        return false;
    return !(G_FlagsGet(uid) & ENTITY_FLAG_COMBAT_HELD);
}

/* What the tick just consumed found in the unit's way, kept for the settle
 * pass, which runs after the work arrays are released.
 */
static void record_way(struct movestate_aux *aux, const struct move_work_in *in,
                       const struct move_work_out *out)
{
    aux->way_bits = ((out->patch.flags & UPDATE_HEADING_GATED)   ? 0x001 : 0)
                  | ((out->patch.flags & UPDATE_VETO_BLOCKED)    ? 0x002 : 0)
                  | ((out->patch.flags & UPDATE_VETO_UNPATHABLE) ? 0x004 : 0)
                  | ((out->patch.flags & UPDATE_FIELD_VOID)      ? 0x008 : 0)
                  | (in->has_dest_los                            ? 0x010 : 0)
                  | (in->field_starved                           ? 0x020 : 0)
                  | (in->fstate.assignment_ready                 ? 0x040 : 0)
                  | (in->fstate.assigned_to_cell                 ? 0x080 : 0)
                  | (in->fstate.in_range_of_cell                 ? 0x100 : 0)
                  | ((out->cp_flags & CP_OUT_STALLED)            ? 0x200 : 0)
                  | ((PFM_Vec2_Len(&out->ent_vel) > EPSILON)     ? 0x400 : 0)
                  | ((PFM_Vec2_Len(&out->patch.next_velocity) > EPSILON) ? 0x800 : 0)
                  | (in->arrival_vdes                            ? 0x1000 : 0);
    aux->way_links = MIN(in->nlinks, MAX_WAY_LINKS);
    aux->veto_ticks = (out->patch.flags & UPDATE_VETO_BLOCKED) ? MIN(aux->veto_ticks + 1, UINT16_MAX) : 0;
    aux->way_void = PFM_Vec2_Len(&in->ent_des_v) < EPSILON;
    aux->way_paced = (in->fstate.fid != NULL_FID)
                  && (!in->fstate.assignment_ready || aux->parked
                      || PFM_Vec2_Len((vec2_t*)&in->fstate.normal_drag_force) > EPSILON);
    aux->way_gated = !!(out->patch.flags & UPDATE_HEADING_GATED);
    aux->way_dir = (vec2_t){0.0f, 0.0f};
    if(!aux->way_void)
        PFM_Vec2_Normal((vec2_t*)&in->ent_des_v, &aux->way_dir);
    for(size_t l = 0; l < aux->way_links; l++) {
        aux->way[l] = in->links[l];
    }
}

/* Whether a body in the way will never move on its own: it has stopped for
 * good, or it is held up on a chain of held bodies that leads back to this
 * unit, ends at one held with nothing in its way, or ends at one that has
 * stopped for good. A chain that reaches a body still moving, or standing to
 * fight, may open yet.
 */
static bool way_body_final(uint32_t uid, uint32_t body)
{
    uint32_t seen[MAX_WAY_LINKS];
    for(int hop = 0; hop < MAX_WAY_LINKS; hop++) {

        if(body == NULL_UID || body == uid || body_rooted(body))
            return true;
        for(int i = 0; i < hop; i++) {
            if(seen[i] == body)
                return true;
        }
        seen[hop] = body;

        const struct movestate *ms = movestate_get(body);
        const struct movestate_aux *aux = movestate_aux_get(body);
        if(!ms || !aux || !G_EntityExists(body))
            return true;
        if(ms->state != STATE_WAITING || !aux->hold_way)
            return false;
        body = aux->hold_link;
    }
    return false;
}

/* The first body in the unit's way that may yet move, or NULL_UID. */
static uint32_t way_first_open(uint32_t uid, const struct movestate_aux *aux)
{
    for(int l = 0; l < aux->way_links; l++) {
        if(!way_body_final(uid, aux->way[l]))
            return aux->way[l];
    }
    return NULL_UID;
}

/* A member makes progress when it comes nearer its goal than it has been.
 * One that has stood without for a window, or walked without for a longer
 * one (the way may lead away from the goal for a while; walking out and
 * back does not), is judged by what stands in its way: if every body there
 * has stopped for good, this is as near as it gets and it stops for good
 * too; if one of them may yet move, or the field has gone quiet with
 * nothing in contact, it is held until that changes. Members in someone
 * else's hands (a work mover, a combat hold, a latched shooter, a formation
 * standing its ranks, the heading gate) answer to those and are left alone.
 */
static void settle_members(enum movement_hz hz)
{
    ASSERT_IN_MAIN_THREAD();

    int window = SETTLE_WINDOW_S * hz_count(hz);
    int walk_window = SETTLE_WALK_WINDOW_S * hz_count(hz);
    for(int i = 0; i < vec_size(&s_flocks); i++) {

        struct flock *flock = &vec_AT(&s_flocks, i);
        uint32_t uid;
        kh_foreach_key(flock->ents, uid, {

            struct movestate *ms = movestate_get(uid);
            if(!ms || !G_EntityExists(uid))
                continue;
            if(ms->state == STATE_ARRIVED || ms->state == STATE_TURNING
            || !member_on_the_way(uid, ms) || (G_FlagsGet(uid) & ENTITY_FLAG_COMBAT_HELD))
                continue;

            struct movestate_aux *aux = movestate_aux_get(uid);
            if(aux->range_latched)
                continue;

            /* A member held up for a window by bodies that have all stopped
             * for good waits on nothing that will ever change: this is as
             * near as it gets.
             */
            if(ms->state == STATE_WAITING) {
                if(!aux->hold_way)
                    continue;
                if(aux->noapproach_ticks < window) {
                    aux->noapproach_ticks++;
                    continue;
                }
                if(aux->hold_link == NULL_UID || way_body_final(uid, aux->hold_link)) {
                    if(way_first_open(uid, aux) == NULL_UID)
                        root_member_in_place(flock, uid, ms, "held");
                }
                continue;
            }

            vec2_t goal = member_goal(flock, uid);
            vec2_t pos = G_Pos_GetXZ(uid);
            vec2_t to_goal, goal_moved;
            PFM_Vec2_Sub(&goal, &pos, &to_goal);
            PFM_Vec2_Sub(&goal, &aux->goal_ref, &goal_moved);
            float dist = PFM_Vec2_Len(&to_goal);

            /* Travel on a detour is progress too: over a segment the unit
             * covers ground and ends well away from where it began. Walking
             * out and back, or being jostled about, covers ground and ends
             * where it began.
             */
            bool travelling = false;
            if(aux->seg_ticks == 0) {
                aux->seg_start = aux->seg_last = pos;
                aux->seg_path = 0.0f;
            }else{
                vec2_t step;
                PFM_Vec2_Sub(&pos, &aux->seg_last, &step);
                aux->seg_path += PFM_Vec2_Len(&step);
                aux->seg_last = pos;
            }
            if(++aux->seg_ticks >= window) {
                vec2_t net;
                PFM_Vec2_Sub(&pos, &aux->seg_start, &net);
                float radius = G_GetSelectionRadius(uid);
                float free_path = entity_speed(uid) * SETTLE_WINDOW_S;
                travelling = aux->seg_path >= MAX(SETTLE_TRAVEL_RADII * radius,
                                                  SETTLE_TRAVEL_PACE * free_path)
                          && PFM_Vec2_Len(&net) >= SETTLE_TRAVEL_STRAIGHTNESS * aux->seg_path;
                aux->seg_ticks = 0;
            }

            /* A goal that moved is measured afresh, but the wait for an
             * approach goes on: hopping goals are no approach either.
             */
            if(aux->best_goal_dist == INFINITY || PFM_Vec2_Len(&goal_moved) > EPSILON) {
                aux->goal_ref = goal;
                aux->best_goal_dist = dist;
            }else if(dist < aux->best_goal_dist - GROUP_PROGRESS_MIN || travelling) {
                aux->best_goal_dist = MIN(dist, aux->best_goal_dist);
                aux->noapproach_ticks = 0;
                continue;
            }
            /* A member its formation holds back is keeping station, not
             * stuck: a formation assigning cells or pacing its ranks stands
             * its members for seconds at a time. One turning to its way
             * under the heading gate has not been let go yet.
             */
            if(aux->way_paced || aux->way_gated)
                continue;
            if(aux->noapproach_ticks < walk_window)
                aux->noapproach_ticks++;
            bool standing = aux->standing_ticks >= window;
            if(aux->noapproach_ticks < window
            || (!standing && aux->noapproach_ticks < walk_window))
                continue;

            uint32_t open = way_first_open(uid, aux);
            if(open != NULL_UID) {
                hold_member_for_way(uid, ms, aux, "way", open);
                continue;
            }
            if(aux->way_links == 0 && aux->way_void) {
                hold_member_for_way(uid, ms, aux, "void", NULL_UID);
                continue;
            }
            root_member_in_place(flock, uid, ms, aux->way_links > 0 ? "way" : "ground");
        });
    }
}

/* Every unit's movement state, every 30 seconds, for offline analysis of who
 * stands where and why (pf.debug.log_perf_csv).
 */
static void move_log_census(enum movement_hz hz)
{
    if(!s_log_perf_csv || (s_move_trace_tick % (30 * hz_count(hz))) != 0)
        return;

    for(khiter_t k = kh_begin(s_entity_state_table); k != kh_end(s_entity_state_table); k++) {

        if(!kh_exist(s_entity_state_table, k))
            continue;
        uint32_t uid = kh_key(s_entity_state_table, k);
        if(!G_EntityExists(uid))
            continue;
        const struct movestate *ms = &kh_value(s_entity_state_table, k);
        const struct movestate_aux *aux = movestate_aux_get(uid);
        const struct flock *flock = flock_for_ent(uid);
        vec2_t pos = G_Pos_GetXZ(uid);
        float goal_dist = -1.0f;
        if(flock) {
            vec2_t to_goal;
            PFM_Vec2_Sub((vec2_t*)&flock->target_xz, &pos, &to_goal);
            goal_dist = PFM_Vec2_Len(&to_goal);
        }
        bool in_form = Move_Formation_GetForEnt(uid) != NULL_FID;
        int fbits = 0;
        float cell_dist = -1.0f;
        if(in_form) {
            fbits = (Move_Formation_AssignmentReady(uid) ? 1 : 0)
                  | ((Move_Formation_AssignmentReady(uid) && Move_Formation_AssignedToCell(uid)) ? 2 : 0)
                  | (Move_Formation_InRangeOfCell(uid) ? 4 : 0);
            if(fbits & 2) {
                vec2_t cell = Move_Formation_CellPosition(uid);
                vec2_t to_cell;
                PFM_Vec2_Sub(&cell, &pos, &to_cell);
                cell_dist = PFM_Vec2_Len(&to_cell);
            }
        }
        fprintf(stdout, "[census] %u,%u,%d,%s,%d,%d,%d,%d,%.1f,%.1f,%.1f,%.2f,%.2f,%d,%.1f\n",
            s_move_trace_tick, uid, G_GetFactionID(uid), s_state_str[ms->state],
            !!(G_FlagsGet(uid) & ENTITY_FLAG_COMBAT_HELD), (int)aux->soft_blocking,
            (int)aux->parked, in_form, pos.x, pos.z,
            goal_dist, G_GetSelectionRadius(uid), PFM_Vec2_Len(&ms->velocity), fbits, cell_dist);
    }
}

static void request_flock_arrival_fields(void)
{
    for(int i = 0; i < vec_size(&s_flocks); i++) {
        const struct flock *flock = &vec_AT(&s_flocks, i);
        Move_ArrivalGroup_RequestFields(&flock->arrival, s_move_work.gamestate.map);
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

    /* One ground for the whole order: the ordered point if the widest ground
     * layer present can reach it, else the nearest point that layer can. The
     * per-layer flocks and the formation's cells then agree on where the
     * order ends, and a box of guns and guards stays whole where the guns can
     * go.
     */
    for(int i = NAV_LAYER_GROUND_7X7; i >= NAV_LAYER_GROUND_1X1; i--) {
        if(vec_size(&layer_flocks[i]) == 0)
            continue;
        vec2_t first_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions,
            vec_AT(&layer_flocks[i], 0));
        target_xz = M_NavClosestReachableDest(s_map, i, first_pos, target_xz);
        break;
    }

    /* Roads are for units going about their business on their own: a group,
     * and anyone on the attack, keep their shape off them */
    bool roads = (vec_size(&fsel) == 1) && !attack;
    for(int i = 0; i < NAV_LAYER_MAX; i++) {
        make_flock(layer_flocks + i, target_xz, i, attack, roads, type);
        vec_entity_destroy(layer_flocks + i);
    }

    Move_Formation_Create(target_xz, target_orientation, &fsel, type);
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
    /* The field-visualisation overlays below read the task-owned field cache on
     * the main thread; drain the nav task so it is quiescent. Skip those
     * overlays if it cannot be drained this frame (GPU work in flight).
     */
    bool cache_ok = !move_nav_field_overlay_enabled() || Move_NavQuiesce();

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
            case STATE_ENTER_ENTITY_RANGE: {
                assert(flock);
                if(G_EntityExists(ms->surround_target_uid)
                && movestate_aux_get(ent)->range_latched) {
                    float radius = G_GetSelectionRadiusFrom(
                        s_move_work.gamestate.sel_radiuses, ent);
                    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, ent);
                    int layer = Entity_NavLayerWithRadius(flags, radius);
                    M_NavRenderVisibleSurroundField(s_map, cam, layer, ms->surround_target_uid,
                        movestate_aux_get(ent)->target_range);
                    UI_DrawText("(Ranged Surround Field)", (struct rect){5,75,600,50}, text_color);
                }else{
                    M_NavRenderVisiblePathFlowField(s_map, cam, flock->dest_id);
                    UI_DrawText("(Path Field)", (struct rect){5,75,600,50}, text_color);
                }
                break;
            }
            case STATE_SURROUND_ENTITY: {

                if(!G_EntityExists(ms->surround_target_uid))
                    break;

                if(ms->using_surround_field) {
                    float radius = G_GetSelectionRadiusFrom(
                        s_move_work.gamestate.sel_radiuses, ent);
                    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, ent);
                    int layer = Entity_NavLayerWithRadius(flags, radius); 
                    M_NavRenderVisibleSurroundField(s_map, cam, layer, ms->surround_target_uid,
                        0.0f);
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
            if(!Move_ArrivalGroup_IsActive(&flock->arrival))
                continue;
            STALLOC(struct arrival_member, members, kh_size(flock->ents));
            int n = build_arrival_members(flock, members, kh_size(flock->ents));
            Move_ArrivalGroup_RenderDebug(&flock->arrival, cam, s_map, flock->dest_id,
                dbg_flock_pal[i % ARR_SIZE(dbg_flock_pal)], members, n);
            STFREE(members);
        }
    }
}

static bool entity_exists(uint32_t uid)
{
    khiter_t k = kh_get(pos, s_move_work.gamestate.positions, uid);
    return (k != kh_end(s_move_work.gamestate.positions));
}

/* A unit sent to a building of its own side makes for the building's whole
 * cluster as one footprint; an attacker wants the one building */
static bool surround_footprint(uint32_t uid, const struct movestate *ms)
{
    uint32_t target = ms->surround_target_uid;
    return (G_FlagsGetFrom(s_move_work.gamestate.flags, target) & ENTITY_FLAG_BUILDING)
        && G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid)
        == G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, target);
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
            layer, pos_xz, faction_id, 0.0f);
    }
    case STATE_MOVING:
    case STATE_MOVING_IN_FORMATION:
    case STATE_ARRIVING_TO_CELL: {
        float reach = attack_move_reach(uid, flock_for_ent(uid));
        if(reach == 0.0f)
            break;
        float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
        uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
        int layer = Entity_NavLayerWithRadius(flags, radius);
        int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
        return M_NavRequestAsyncEnemySeekField(s_move_work.gamestate.map,
            layer, pos_xz, faction_id, reach);
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
                ms->surround_target_uid, faction_id, 0.0f, surround_footprint(uid, ms));
        }
        break;
    }
    case STATE_ENTER_ENTITY_RANGE: {

        if(!entity_exists(ms->surround_target_uid))
            return;

        vec2_t target_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions,
            ms->surround_target_uid);
        if(fabs(target_pos.x - pos_xz.x) < CHUNK_WIDTH
        && fabs(target_pos.z - pos_xz.z) < CHUNK_HEIGHT) {
            float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
            uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
            int layer = Entity_NavLayerWithRadius(flags, radius);
            int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
            return M_NavRequestAsyncSurroundField(s_move_work.gamestate.map, layer, pos_xz,
                ms->surround_target_uid, faction_id, movestate_aux_get(uid)->target_range,
                surround_footprint(uid, ms));
        }
        break;
    }
    default:;
        /* No-op */
    }
}

/* The reach an attack-move should be guided by, or zero for an order which is
 * not one. A unit walking to the ordered point is following a field which knows
 * nothing of the enemy, so it will happily march past a firing position and take
 * the long way round its own front rank to reach the point it was sent to.
 */
/* A formation member on the reach field is steered by the field alone and the
 * scaffold treats it as away, so the cell's pull and the field's never compete.
 */
static float attack_move_reach(uint32_t uid, const struct flock *fl)
{
    const struct movestate *ms = movestate_get(uid);
    if(ms->state != STATE_MOVING
    && ms->state != STATE_MOVING_IN_FORMATION
    && ms->state != STATE_ARRIVING_TO_CELL)
        return 0.0f;
    if(!fl || !N_DestIDIsAttacking(fl->dest_id))
        return 0.0f;

    const struct movestate_aux *aux = movestate_aux_get(uid);
    return aux ? aux->attack_reach : 0.0f;
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
                                            .uid = ms->surround_target_uid,
                                            .footprint = surround_footprint(uid, ms)}};
    }
    if(ms->state == STATE_ENTER_ENTITY_RANGE
    && entity_exists(ms->surround_target_uid)) {

        /* Follow the field from as far out as it has flow for us, and keep
         * following it across rebuild gaps. The entity fields are age-evicted
         * every few passes, so an unlatched presence test flips the unit's
         * heading between the field's slot and the shared greedy point once
         * per cycle, and it walks back and forth between the two.
         */
        struct movestate_aux *eaux = movestate_aux_get(uid);
        vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
        enum range_field_state state = M_NavEntityRangeFieldState(s_move_work.gamestate.map,
            layer, ms->surround_target_uid, eaux->target_range, pos_xz);
        bool present = (state == RANGE_FIELD_FLOW) || (state == RANGE_FIELD_AT_GOAL);

        if(present) {
            eaux->range_latched = true;
            eaux->range_miss_ticks = 0;
        }else if(eaux->range_latched
             && ++eaux->range_miss_ticks >= RANGE_FIELD_MISS_TICKS) {
            eaux->range_latched = false;
        }

        if(eaux->range_latched) {
            return (struct target){.kind = TARGET_KIND_SURROUND,
                                   .surround = {.layer = layer, .faction_id = faction_id,
                                                .uid = ms->surround_target_uid,
                                                .range = eaux->target_range,
                                                .footprint = surround_footprint(uid, ms)}};
        }
    }

    /* Only once the field is built and reaches this tile: until then, and wherever
     * there is no enemy to shoot at, the ordered point is still the destination.
     *
     * The choice is latched rather than made afresh each tick. The field is per
     * chunk and a battle evicts and rebuilds it constantly, so a unit near the
     * enemy would otherwise be handed a firing position on one tick and the
     * ordered point beyond the enemy on the next, and swing between the two
     * headings on the spot.
     */
    float reach = attack_move_reach(uid, fl);
    struct movestate_aux *raux = movestate_aux_get(uid);
    if(reach > 0.0f && raux) {

        vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
        enum range_field_state state = M_NavEnemyRangeFieldState(s_move_work.gamestate.map,
            layer, faction_id, reach, pos_xz);
        bool present = (state == RANGE_FIELD_FLOW) || (state == RANGE_FIELD_AT_GOAL);

        if(present) {
            raux->range_latched = true;
            raux->range_miss_ticks = 0;
        }else if(raux->range_latched
             && ++raux->range_miss_ticks >= RANGE_FIELD_MISS_TICKS) {
            raux->range_latched = false;
        }

        if(raux->range_latched) {
            return (struct target){.kind = TARGET_KIND_ENEMY_SEEK,
                                   .enemy_seek = {.layer = layer, .faction_id = faction_id,
                                                  .range = reach}};
        }
    }else if(raux) {
        raux->range_latched = false;
        raux->range_miss_ticks = 0;
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
    in->range_field = false;
    in->arrival_vdes = false;

    switch(ms->state) {
    case STATE_TURNING:
        return (vec2_t){0.0f, 0.0f};

    case STATE_ARRIVING_TO_CELL: {
        if(attack_move_reach(uid, fl) > 0.0f) {
            struct target target = build_target(uid, fl);
            in->range_field = (target.kind == TARGET_KIND_ENEMY_SEEK)
                           && (target.enemy_seek.range > 0.0f);
            if(in->range_field)
                return M_NavDesiredVelocityForTargetCached(map, target, pos_xz);
        }
        return cell_arrival_vdes;
    }
    case STATE_SEEK_ENEMIES: {
        vec2_t pinned;
        if(seek_pin_vdes(uid, pos_xz, &pinned, out_seek_pinned))
            return pinned;
        return M_NavDesiredVelocityForTargetCached(map, build_target(uid, fl), pos_xz);
    }
    case STATE_SURROUND_ENTITY:
    case STATE_ENTER_ENTITY_RANGE:
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
        struct target target = build_target(uid, fl);
        in->range_field = (target.kind == TARGET_KIND_ENEMY_SEEK)
                       && (target.enemy_seek.range > 0.0f);

        struct movestate *mms = movestate_get(uid);
        struct arrival_state *as = flock_arrival_for_ent(fl, uid);
        vec2_t arrival_vel;
        if(!in->range_field && as
        && Move_Arrival_DesiredVelocity(as, &movestate_aux_get(uid)->arrival, s_map,
            map, pos_xz, mms->velocity, has_dest_los, &arrival_vel)) {
            in->arrival_vdes = true;
            return arrival_vel;
        }

        /* In sight of the goal the unit heads straight for it, and so must the
         * way it wants to go: a field in a chunk it strayed into may lead the
         * other way, and the two cancel out on the border.
         */
        if(has_dest_los && target.kind == TARGET_KIND_POINT_SEEK) {
            vec2_t seek = as ? Move_Arrival_SeekTarget(as, &movestate_aux_get(uid)->arrival,
                fl->target_xz) : fl->target_xz;
            vec2_t to_seek;
            PFM_Vec2_Sub(&seek, &pos_xz, &to_seek);
            if(PFM_Vec2_Len(&to_seek) > EPSILON) {
                PFM_Vec2_Normal(&to_seek, &to_seek);
                return to_seek;
            }
        }

        /* A zero on the reach field's passable ground is the firing position. */
        vec2_t field_vdes = M_NavDesiredVelocityForTargetCached(map, target, pos_xz);
        if(in->range_field || PFM_Vec2_Len(&field_vdes) > EPSILON)
            return field_vdes;

        /* A void sample on free, pathable ground is a stale hole rather than
         * an unreachable destination; hold a heading at the goal to cross it.
         */
        float vradius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
        uint32_t vflags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
        enum nav_layer vlayer = Entity_NavLayerWithRadius(vflags, vradius);
        struct movestate_aux *vaux = movestate_aux_get(uid);
        if(vaux->field_void_ticks >= FIELD_VOID_COAST_S * hz_count(s_move_work.hz)
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

static vec2_t seek_force(uint32_t uid, vec2_t target_xz)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    return Move_Flock_Seek(pos_xz, ms->velocity, ms->max_speed, hz_count(s_move_work.hz), target_xz);
}

static vec2_t arrive_force_point(uint32_t uid, vec2_t target_xz, vec2_t vdes, bool has_dest_los,
                                 float speed)
{
    vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);

    struct movestate *ms = movestate_get(uid);
    assert(ms);

    return Move_Flock_ArrivePoint(pos_xz, ms->velocity, hz_count(s_move_work.hz), target_xz,
        vdes, has_dest_los, speed);
}

static vec2_t arrive_force_cell(uint32_t uid, vec2_t cell_xz, vec2_t vdes)
{
    struct movestate *ms = movestate_get(uid);
    vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    return Move_Flock_ArriveCell(pos_xz, ms->max_speed, hz_count(s_move_work.hz), cell_xz, vdes);
}

static vec2_t arrive_force_enemies(uint32_t uid, vec2_t vdes)
{
    const struct movestate *ms = movestate_get(uid);
    assert(ms);

    return Move_Flock_ArriveEnemies(ms->velocity, ms->max_speed, hz_count(s_move_work.hz), vdes);
}

static float clearpath_reach(float radius_sum)
{
    return MAX(CLEARPATH_NEIGHBOUR_RADIUS, CLEARPATH_NEIGHBOUR_SCALE * radius_sum);
}

static float small_max_for_class(uint32_t flags)
{
    return s_move_work.small_max_radius[(flags & ENTITY_FLAG_AIR) ? 1 : 0];
}

static float small_max_step_for_class(uint32_t flags)
{
    return s_move_work.small_max_step[(flags & ENTITY_FLAG_AIR) ? 1 : 0];
}

/* How many ticks ahead a moving unit looks for the bodies it is closing on,
 * so that a pass is steered at full pace rather than negotiated at contact.
 */
static float react_ticks(vec2_t velocity)
{
    if(PFM_Vec2_Len(&velocity) < CLEARPATH_STILL_SPEED)
        return 0.0f;
    return CLEARPATH_REACT_S * hz_count(s_move_work.hz);
}

/* Crowd query at the caller's class reach plus a registry pass for the
 * giants; the two sets partition by LARGE_MOVABLE_RADIUS, so the result
 * matches a single query at the old global reach exactly.
 */
static int gather_movable_neighbours(uint32_t uid, vec2_t ent_pos, float ent_radius,
                                     uint32_t ent_flags, float (*reach_fn)(float),
                                     float lookahead, uint32_t *out, int maxout)
{
    float grid_reach = reach_fn(ent_radius + small_max_for_class(ent_flags)) + lookahead;
    int ngrid = G_Pos_EntsInCircleFrom(s_move_work.gamestate.postree,
        s_move_work.gamestate.flags, ent_pos, grid_reach, out, maxout);

    int n = 0;
    for(int i = 0; i < ngrid; i++) {
        float cr = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, out[i]);
        if(cr > LARGE_MOVABLE_RADIUS)
            continue;
        out[n++] = out[i];
    }

    for(size_t i = 0; i < s_move_work.nlarge && n < maxout; i++) {
        const struct large_movable *lm = &s_move_work.large_movables[i];
        if((ent_flags ^ lm->flags) & ENTITY_FLAG_AIR)
            continue;
        if(lm->uid == uid)
            continue;
        float dx = lm->xz_pos.x - ent_pos.x;
        float dz = lm->xz_pos.z - ent_pos.z;
        float reach = reach_fn(ent_radius + lm->radius) + lookahead;
        if(dx * dx + dz * dz <= reach * reach)
            out[n++] = lm->uid;
    }
    return n;
}

static vec2_t separation_force_gap(uint32_t uid, float buffer_dist, float *out_min_gap)
{
    vec2_t ret = (vec2_t){0.0f};
    uint32_t ent_flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    float ent_radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
    int ent_faction = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
    bool phasing = movestate_aux_get(uid)->phasing;

    vec2_t ent_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    uint32_t near_ents[256];
    int num_near = gather_movable_neighbours(uid, ent_xz_pos, ent_radius, ent_flags,
        Move_Flock_SeparationReach, 0.0f, near_ents, ARR_SIZE(near_ents));

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
        vec2_t curr_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, curr);

        float radius_sum = ent_radius
                         + G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, curr);
        PFM_Vec2_Sub(&curr_xz_pos, &ent_xz_pos, &diff);

        Move_Flock_Separate(diff, radius_sum, buffer_dist, &ret, out_min_gap);
    }

    if(0 == num_near)
        return (vec2_t){0.0f};

    PFM_Vec2_Scale(&ret, -1.0f, &ret);
    Move_Flock_Truncate(&ret, SCALED_MAX_FORCE);
    return ret;
}

static vec2_t separation_force(uint32_t uid, float buffer_dist)
{
    float min_gap = INFINITY;
    return separation_force_gap(uid, buffer_dist, &min_gap);
}

static vec2_t point_seek_total_force(uint32_t uid, const struct flock *flock,
                                     const struct flock_snap *fsnap,
                                     vec2_t vdes, bool has_dest_los, int side, float speed)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    struct arrival_state *as = flock_arrival_for_ent(flock, uid);
    vec2_t seek = as ? Move_Arrival_SeekTarget(as, &movestate_aux_get(uid)->arrival,
        flock->target_xz) : flock->target_xz;
    vec2_t arrive = arrive_force_point(uid, seek, vdes, has_dest_los, speed);
    vec2_t cohesion = Move_Flock_Cohesion(uid,
        G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid), hz_count(s_move_work.hz), fsnap);
    float min_gap = INFINITY;
    vec2_t separation = separation_force_gap(uid, SEPARATION_BUFFER_DIST, &min_gap);
    separation = Move_Flock_RedirectBackward(separation, vdes, side, min_gap);

    PFM_Vec2_Scale(&arrive,     MOVE_ARRIVE_FORCE_SCALE,   &arrive);
    PFM_Vec2_Scale(&cohesion,   MOVE_COHESION_FORCE_SCALE, &cohesion);
    PFM_Vec2_Scale(&separation, SEPARATION_FORCE_SCALE,    &separation);

    vec2_t ret = (vec2_t){0.0f};
    PFM_Vec2_Add(&ret, &arrive, &ret);
    PFM_Vec2_Add(&ret, &separation, &ret);
    PFM_Vec2_Add(&ret, &cohesion, &ret);

    Move_Flock_Truncate(&ret, SCALED_MAX_FORCE);
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

    Move_Flock_Truncate(&ret, SCALED_MAX_FORCE);
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

    Move_Flock_Truncate(&ret, SCALED_MAX_FORCE);
    return ret;
}

static vec2_t new_pos_for_vel(uint32_t uid, vec2_t velocity)
{
    vec2_t xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    vec2_t new_pos;

    PFM_Vec2_Add(&xz_pos, &velocity, &new_pos);
    return new_pos;
}

/* Whether a step may end at a position: never on impassable ground, and on a
 * body's or building's stamp only for a unit already standing on one, which
 * must be free to step off it, or phasing through its own side.
 */
static bool surround_switch_ok(enum nav_layer layer, vec2_t pos, vec2_t curr, vec2_t found)
{
    if(M_NavPositionBlocked(s_move_work.gamestate.map, layer, curr))
        return true;
    vec2_t to_curr, to_found;
    PFM_Vec2_Sub(&curr, &pos, &to_curr);
    PFM_Vec2_Sub(&found, &pos, &to_found);
    /* At its spot and still not beside the target, as when the spot was
     * picked past a crowd since gone, it takes the next one however far */
    if(PFM_Vec2_Len(&to_curr) < SURROUND_AT_SPOT)
        return true;
    return PFM_Vec2_Len(&to_found) + SURROUND_SWITCH_MARGIN < PFM_Vec2_Len(&to_curr);
}

static bool landing_ok(enum nav_layer layer, bool on_blocked, vec2_t pos)
{
    return Move_Flock_LandingOk(s_move_work.gamestate.map, layer, on_blocked, pos);
}

static vec2_t slide_along_obstacle(uint32_t uid, enum nav_layer layer, bool on_blocked,
                                   vec2_t vel)
{
    return Move_Flock_Slide(s_move_work.gamestate.map,
        G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid), layer, on_blocked, vel);
}

static void nullify_impass_components(uint32_t uid, vec2_t *inout_force)
{
    float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    enum nav_layer layer = Entity_NavLayerWithRadius(flags, radius);

    vec2_t pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);

    /* A unit already sitting on a blocked tile must be allowed to steer toward
     * blocked neighbours so it can slide off the blocked region, and one phasing
     * back into its formation must be allowed to steer into it; only impassable
     * terrain is forbidden in those cases.
     */
    bool on_blocked = movestate_aux_get(uid)->phasing
                   || M_NavPositionBlocked(s_move_work.gamestate.map, layer, pos);

    Move_Flock_NullifyImpass(s_move_work.gamestate.map, layer, pos, on_blocked, inout_force);
}

/* Where a unit is arriving, for the deceleration fallback. A unit steered by the
 * reach field is heading for a firing position the field only expresses as a
 * gradient, and it is not going to the ordered point at all: brake in place
 * instead of being drawn on past the enemy when the seek force goes slack.
 */
static vec2_t arrive_force_seek(uint32_t uid, vec2_t target_xz, vec2_t vdes,
                                bool has_dest_los, bool range_field, float speed)
{
    if(range_field) {
        vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
        return arrive_force_point(uid, pos_xz, vdes, true, speed);
    }
    return arrive_force_point(uid, target_xz, vdes, has_dest_los, speed);
}

/* Two bodies in contact push each other straight back while their wishes
 * pull them straight on, and the sum is next to nothing: a wish that small
 * leaves the solve nothing to work with. A traveller's collapsed wish in contact
 * becomes a step forward and aside on the committed side, at a fraction of
 * the pace, and the solve keeps what is admissible of it.
 */
static vec2_t escape_wish(uint32_t uid, vec2_t vpref, vec2_t vdes, int side, float max_step)
{
    enum move_state state = movestate_get(uid)->state;
    if(state != STATE_SURROUND_ENTITY && state != STATE_ENTER_ENTITY_RANGE)
        return vpref;
    if(PFM_Vec2_Len(&vdes) < EPSILON || PFM_Vec2_Len(&vpref) >= MOVE_ESCAPE_FLOOR * max_step)
        return vpref;

    float min_gap = INFINITY;
    separation_force_gap(uid, SEPARATION_BUFFER_DIST, &min_gap);
    if(min_gap >= MOVE_ESCAPE_GAP)
        return vpref;

    return Move_Flock_EscapeStep(vdes, side, max_step);
}

static vec2_t point_seek_vpref(uint32_t uid, const struct flock *flock,
                               const struct flock_snap *fsnap,
                               vec2_t vdes, bool has_dest_los, float speed, float max_speed,
                               int side, bool range_field)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    vec2_t steer_force;
    for(int prio = 0; prio < 3; prio++) {

        switch(prio) {
        case 0:
            steer_force = point_seek_total_force(uid, flock, fsnap, vdes, has_dest_los, side, max_speed);
            break;
        case 1: {
            float min_gap = INFINITY;
            steer_force = separation_force_gap(uid, SEPARATION_BUFFER_DIST, &min_gap);
            steer_force = Move_Flock_RedirectBackward(steer_force, vdes, side, min_gap);
            break;
        }
        case 2: {
            struct arrival_state *as = flock_arrival_for_ent(flock, uid);
            vec2_t seek = as ? Move_Arrival_SeekTarget(as, &movestate_aux_get(uid)->arrival,
                flock->target_xz) : flock->target_xz;
            steer_force = arrive_force_seek(uid, seek, vdes, has_dest_los, range_field, max_speed);
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
    Move_Flock_Truncate(&new_vel, speed / hz_count(s_move_work.hz));

    return escape_wish(uid, new_vel, vdes, side, speed / hz_count(s_move_work.hz));
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
    Move_Flock_Truncate(&new_vel, speed / hz_count(s_move_work.hz));
    if(PFM_Vec2_Len(&drag) > EPSILON) {
        Move_Flock_Truncate(&new_vel, (speed * 0.75) / hz_count(s_move_work.hz));
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
    Move_Flock_Truncate(&new_vel, speed / hz_count(s_move_work.hz));

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

    Move_Flock_Truncate(&ret, SCALED_MAX_FORCE);
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
    Move_Flock_Truncate(&new_vel, speed / hz_count(s_move_work.hz));

    return new_vel;
}

static vec2_t formation_point_seek_total_force(uint32_t uid, const struct flock *flock, vec2_t vdes,
                                               vec2_t cohesion, vec2_t alignment, bool has_dest_los)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    vec2_t arrive = arrive_force_point(uid, flock->target_xz, vdes, has_dest_los, ms->max_speed);
    vec2_t separation = separation_force(uid, SEPARATION_BUFFER_DIST);

    PFM_Vec2_Scale(&arrive,     MOVE_ARRIVE_FORCE_SCALE,   &arrive);
    PFM_Vec2_Scale(&cohesion,   MOVE_COHESION_FORCE_SCALE, &cohesion);
    PFM_Vec2_Scale(&separation, SEPARATION_FORCE_SCALE,    &separation);
    PFM_Vec2_Scale(&alignment,  ALIGNMENT_FORCE_SCALE,     &alignment);

    vec2_t ret = (vec2_t){0.0f};
    PFM_Vec2_Add(&ret, &arrive, &ret);
    PFM_Vec2_Add(&ret, &separation, &ret);
    PFM_Vec2_Add(&ret, &cohesion, &ret);

    Move_Flock_Truncate(&ret, SCALED_MAX_FORCE);
    return ret;
}

static vec2_t formation_seek_vpref(uint32_t uid, const struct flock *flock, float speed,
                                   vec2_t vdes, vec2_t cohesion, vec2_t alignment, vec2_t drag,
                                   bool has_dest_los, bool range_field)
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
            steer_force = arrive_force_seek(uid, flock->target_xz, vdes, has_dest_los,
                range_field, ms->max_speed); 
            break;
        }

        nullify_impass_components(uid, &steer_force);
        if(PFM_Vec2_Len(&steer_force) > SCALED_MAX_FORCE * 0.01)
            break;
    }

    vec2_t accel, new_vel; 
    PFM_Vec2_Scale(&steer_force, 1.0f / ENTITY_MASS, &accel);

    PFM_Vec2_Add(&ms->velocity, &accel, &new_vel);
    Move_Flock_Truncate(&new_vel, speed / hz_count(s_move_work.hz));
    if(PFM_Vec2_Len(&drag) > EPSILON) {
        Move_Flock_Truncate(&new_vel, (speed * 0.75) / hz_count(s_move_work.hz));
    }

    return new_vel;
}

/* On the reach field the member is steered by the field alone: no cell, no
 * cohesion, alignment or drag, no line of sight, and the brake at its own
 * position once the field reads zero.
 */
static vec2_t range_seek_vpref(uint32_t uid, const struct flock *flock, float speed, vec2_t vdes)
{
    vec2_t zero = (vec2_t){0.0f, 0.0f};
    return formation_seek_vpref(uid, flock, speed, vdes, zero, zero, zero, false, true);
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

static void seed_vel_hist_facing(struct movestate *ms, struct movestate_aux *aux)
{
    vec2_t dir = Move_Flock_FacingDir(ms->next_rot);
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
        PFM_Vec2_Scale(&term, i+1, &term);
        PFM_Vec2_Add(&ret, &term, &ret);
        denom += (i+1);
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

static bool move_gated_by_heading(enum move_state state)
{
    switch(state) {
    case STATE_MOVING:
    case STATE_MOVING_IN_FORMATION:
    case STATE_ARRIVING_TO_CELL:
    case STATE_SEEK_ENEMIES:
    case STATE_SURROUND_ENTITY:
    case STATE_ENTER_ENTITY_RANGE:
    case STATE_FLEEING:
        return true;
    default:
        return false;
    }
}

/* A traveller whose only step lies behind it turns to take it in the end;
 * a fighter keeps facing its fight and a formation member its rank.
 */
static bool yields_behind_band(enum move_state state)
{
    switch(state) {
    case STATE_MOVING:
    case STATE_SURROUND_ENTITY:
    case STATE_ENTER_ENTITY_RANGE:
        return true;
    default:
        return false;
    }
}

static vec2_t intended_heading(vec2_t vdes, vec2_t new_vel)
{
    return (PFM_Vec2_Len(&vdes) > EPSILON) ? vdes : new_vel;
}

/* The facing follows the travel direction only while the unit is translating
 * in earnest, with hysteresis on the speed so that the follow does not
 * flicker. A unit inching through a crowd fronts the way it means to go
 * instead of sweeping after every displacement.
 */
static quat_t orient_to_velocity_history(const struct movestate *ms,
                                         const struct movestate_aux *aux, vec2_t vdes,
                                         float step, bool *out_follows)
{
    vec2_t wma = vel_wma(aux);
    float band = aux->facing_follows ? FACING_FOLLOW_RELEASE : FACING_FOLLOW_ENGAGE;
    *out_follows = PFM_Vec2_Len(&wma) > EPSILON && PFM_Vec2_Len(&wma) >= band * step;
    if(!*out_follows) {
        if(PFM_Vec2_Len(&vdes) > EPSILON)
            return Move_Flock_TurnToward(ms->next_rot, Move_Flock_DirQuat(vdes), SCALED_MAX_TURN_RATE);
        return ms->next_rot;
    }

    /* History opposing the intent is displacement by the crowd, not a
     * heading: chasing it commits the facing to a full pirouette by the time
     * the stale samples wash out. Hold the facing instead; a unit giving
     * ground keeps fronting the way it means to go.
     */
    if(PFM_Vec2_Len(&vdes) > EPSILON && PFM_Vec2_Dot(&wma, &vdes) < 0.0f)
        return ms->next_rot;

    return Move_Flock_TurnToward(ms->next_rot, Move_Flock_DirQuat(wma), SCALED_MAX_TURN_RATE);
}

enum settle_verdict{
    SETTLE_NONE,
    /* Within reach of the ordered point */
    SETTLE_AT_GOAL,
    /* The arrival ball took the unit */
    SETTLE_AT_SLOT,
};

/* The one answer to whether a plain mover may stop where it is. A formation
 * member stops at its cell (STATE_ARRIVING_TO_CELL) and a member that cannot
 * come nearer its goal is settled or held by the tick (settle_members).
 */
static enum settle_verdict mover_settle_verdict(uint32_t uid, vec2_t new_pos_xz, float radius,
                                                enum nav_layer layer, struct movestate *ms,
                                                struct movestate_aux *aux)
{
    struct flock *flock = flock_for_ent(uid);
    assert(flock);

    struct arrival_state *as = Move_ArrivalGroup_ForLayer(&flock->arrival, layer);
    if(as && Move_Arrival_IsActive(as)) {
        int n_settled = adjacent_settled_count(uid);
        bool at_slot = Move_Arrival_ShouldSettle(as, &aux->arrival, s_map,
            s_move_work.gamestate.map, new_pos_xz, ms->velocity, radius, n_settled);
        return at_slot ? SETTLE_AT_SLOT : SETTLE_NONE;
    }

    if(arrived(uid, new_pos_xz))
        return SETTLE_AT_GOAL;
    return SETTLE_NONE;
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

    assert(hz_count(hz) <= 20);
    assert(20 % hz_count(hz) == 0);

    float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, uid);
    enum nav_layer layer = Entity_NavLayerWithRadius(flags, radius);
    vec2_t curr_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    bool on_blocked = aux->phasing
                   || M_NavPositionBlocked(s_move_work.gamestate.map, layer, curr_xz);

    if(PFM_Vec2_Len(&new_vel) > EPSILON
    && !landing_ok(layer, on_blocked, new_pos_for_vel(uid, new_vel))) {
        vec2_t slide = slide_along_obstacle(uid, layer, on_blocked, new_vel);
        if(PFM_Vec2_Len(&slide) > EPSILON)
            new_vel = slide;
    }

    /* Dressing the rank: the unit steps to its place facing the way the
     * rank faces, rather than turning to each small correction and back. It
     * dresses a correction short enough to shuffle that does not take it
     * back more than a step, or the last stretch of a cell straight ahead of
     * the rank; any other approach it walks facing its way, and turns to the
     * rank on the cell.
     */
    if(ms->state == STATE_ARRIVING_TO_CELL && in->fstate.assigned_to_cell && !in->range_field) {
        vec2_t to_cell;
        PFM_Vec2_Sub((vec2_t*)&in->cell_pos, &curr_xz, &to_cell);
        float dist = PFM_Vec2_Len(&to_cell);
        float shuffle_reach = FACING_FOLLOW_ENGAGE * in->speed * CELL_DRESS_TIME_S;
        float bearing = Move_Flock_CellBearing(to_cell, in->fstate.target_orientation);
        if((dist < shuffle_reach && (bearing <= 90.0f || dist <= radius))
        || (dist < cell_closing_radius(radius) && bearing <= MOVE_STEP_BAND))
            aux->dressing = true;
    }else{
        aux->dressing = false;
    }
    bool dressing = aux->dressing;

    /* A member in range of its cell switches to it this tick: neither turn
     * nor step toward the ordered point on the way.
     */
    if(ms->state == STATE_MOVING_IN_FORMATION && in->fstate.fid != NULL_FID
    && in->fstate.assignment_ready && in->fstate.assigned_to_cell
    && in->fstate.in_range_of_cell)
        new_vel = (vec2_t){0.0f, 0.0f};

    /* Gate translation on heading so a unit never slides sideways out of a stop.
     */
    bool turn_to_move = false;
    quat_t travel_dir = ms->next_rot;
    bool yielding = aux->band_yield_ticks > 0 && yields_behind_band(ms->state);
    if(PFM_Vec2_Len(&new_vel) > EPSILON && move_gated_by_heading(ms->state) && !dressing) {
        travel_dir = Move_Flock_DirQuat(yielding ? new_vel : intended_heading(vdes, new_vel));
        float heading_err = fabs(RAD_TO_DEG(PFM_Quat_PitchDiff(&ms->next_rot, &travel_dir)));
        float tolerance = (PFM_Vec2_Len(&ms->velocity) > EPSILON) ? MOVE_HEADING_HALT
                                                                  : MOVE_HEADING_RESUME;
        if(heading_err > tolerance) {
            turn_to_move = true;
            new_vel = (vec2_t){0.0f, 0.0f};
            out->flags |= UPDATE_HEADING_GATED;
        }
    }

    vec2_t solved = new_vel;
    new_vel = dressing ? Move_Flock_DressStep(new_vel, Move_Flock_FacingDir(ms->next_rot), in->speed / hz_count(hz))
                       : Move_Flock_StepWithinHeading(new_vel, Move_Flock_FacingDir(ms->next_rot), in->speed / hz_count(hz));
    if(!dressing && yields_behind_band(ms->state)
    && !(out->flags & UPDATE_HEADING_GATED)
    && PFM_Vec2_Len(&solved) >= CLEARPATH_STALL_SPEED
    && PFM_Vec2_Len(&new_vel) < EPSILON)
        out->flags |= UPDATE_BAND_HELD;
    vec2_t new_pos_xz = new_pos_for_vel(uid, new_vel);

    if(flags & ENTITY_FLAG_GARRISONED) {
        if(!ent_still(ms)) {
            out->flags |= UPDATE_SET_STATE;
            out->next_state = STATE_ARRIVED;
            out->next_block = false;
        }
        return;
    }

    if(PFM_Vec2_Len(&new_vel) > 0 && landing_ok(layer, on_blocked, new_pos_xz)) {

        vec3_t new_pos = (vec3_t){new_pos_xz.x, unit_height(uid, new_pos_xz), new_pos_xz.z};

        out->flags |= UPDATE_SET_PREV_POS | UPDATE_SET_NEXT_POS;
        out->next_ppos = ms->next_pos;
        out->next_npos = new_pos;

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
        bool follows = false;
        out->next_nrot = (aux->parked || dressing)
            ? Move_Flock_TurnToward(ms->next_rot, in->fstate.target_orientation, SCALED_MAX_TURN_RATE)
            : orient_to_velocity_history(ms, aux, vdes, in->speed / hz_count(hz), &follows);
        out->flags |= follows ? UPDATE_FACING_FOLLOWS : 0;

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
            out->flags |= UPDATE_SET_PREV_ROT | UPDATE_SET_NEXT_ROT;
            out->flags |= UPDATE_TURNING_IN_PLACE;
            out->next_prot = ms->next_rot;
            out->next_nrot = Move_Flock_TurnToward(ms->next_rot, facing, SCALED_MAX_TURN_RATE);
        }else if(turn_to_move) {
            out->flags |= UPDATE_SET_PREV_ROT | UPDATE_SET_NEXT_ROT;
            out->flags |= UPDATE_TURNING_IN_PLACE;
            out->next_prot = ms->next_rot;
            out->next_nrot = Move_Flock_TurnToward(ms->next_rot, travel_dir, SCALED_MAX_TURN_RATE);
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

        enum settle_verdict verdict = in->range_field ? SETTLE_NONE
            : mover_settle_verdict(uid, new_pos_xz, radius, layer, ms, aux);
        if(verdict != SETTLE_NONE) {
            if(s_log_perf_csv) {
                const struct flock *vflock = flock_for_ent(uid);
                vec2_t to_target;
                PFM_Vec2_Sub((vec2_t*)&vflock->target_xz, &new_pos_xz, &to_target);
                fprintf(stdout, "[settle-verdict] %u,%u,%d,%.2f,%.1f\n", s_move_trace_tick,
                    uid, (int)verdict, radius, PFM_Vec2_Len(&to_target));
            }
            out->flags |= UPDATE_SET_STATE;
            out->next_state = STATE_ARRIVED;
            out->next_block = true;
            break;
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

            /* A zero on the reach field is the firing position: hold it. */
            if(in->range_field)
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
            if(s_log_perf_csv)
                fprintf(stdout, "[surr-arrive] %u,%u,%u,adjacent\n", s_move_trace_tick,
                    uid, ms->surround_target_uid);
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
        bool target_moved = PFM_Vec2_Len(&delta) > EPSILON;
        bool held_up = PFM_Vec2_Len(&ms->velocity) < EPSILON;
        bool first_pick = aux->surround_requery_tick == 0;
        bool requery_due = first_pick || s_move_trace_tick >= aux->surround_requery_tick;

        if(target_moved || (held_up && requery_due)) {

            vec2_t found;
            bool hasdest = M_NavClosestReachableAdjacentPosFrom(s_move_work.gamestate.map, layer,
                new_pos_xz, ms->surround_target_uid, surround_footprint(uid, ms),
                &s_move_work.unit_query_ctx, &found);
            aux->surround_requery_tick = s_move_trace_tick + SURROUND_REQUERY_TICKS
                                       + uid % SURROUND_REQUERY_SPREAD;

            if(hasdest && (target_moved || first_pick
            || surround_switch_ok(layer, new_pos_xz, dest, found)))
                dest = found;

            if(!hasdest) {
                if(s_log_perf_csv)
                    fprintf(stdout, "[surr-arrive] %u,%u,%u,nodest\n", s_move_trace_tick,
                        uid, ms->surround_target_uid);
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
            out->flags |= UPDATE_SET_DEST | UPDATE_SET_TARGET_PREV | UPDATE_SET_STATE;
            out->next_state = STATE_ENTER_ENTITY_RANGE;
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
        quat_t final = Move_Flock_TurnToward(ms->next_rot, aux->target_dir, SCALED_MAX_TURN_RATE);

        out->flags |= UPDATE_SET_PREV_ROT | UPDATE_SET_NEXT_ROT;
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
        if(in->fstate.arrived_at_cell && in->fstate.may_park) {
            out->flags |= UPDATE_SET_STATE | UPDATE_SET_TARGET_DIR;
            out->next_target_dir = in->fstate.target_orientation;
            out->next_state = STATE_TURNING;
            break;
        }

        /* The cell field has no way to the cell from here: hold until the
         * ground around changes, rather than stand to be jostled. Near the
         * cell the field reads none on the cell's own tiles and the unit
         * steers straight at the cell instead.
         */
        vec2_t to_cell;
        PFM_Vec2_Sub((vec2_t*)&in->cell_pos, &curr_xz, &to_cell);
        if(PFM_Vec2_Len(&vdes) < EPSILON && !in->fstate.arrived_at_cell
        && PFM_Vec2_Len(&to_cell) > cell_closing_radius(radius)
        && !in->range_field && !aux->soft_blocking) {
            out->flags |= UPDATE_SET_STATE;
            out->next_state = STATE_WAITING;
            out->next_block = true;
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

    struct movestate_aux *aux = movestate_aux_get(uid);
    float dist = sqrtf(dx * dx + dz * dz);

    if(!ms->using_surround_field) {
        if(dx < SURROUND_LOW_WATER_X && dz < SURROUND_LOW_WATER_Z
        && (!aux || s_move_trace_tick >= aux->surround_shun_tick)) {
            ms->using_surround_field = true;
            if(aux) {
                aux->surround_on_dist = dist;
            }
        }
    }else{
        if(dx >= SURROUND_HIGH_WATER_X || dz >= SURROUND_HIGH_WATER_Z) {
            ms->using_surround_field = false;
        }else if(aux && dist > aux->surround_on_dist + SURROUND_DETOUR_SLACK) {
            ms->using_surround_field = false;
            aux->surround_shun_tick = s_move_trace_tick + SURROUND_SHUN_TICKS;
        }else if(aux) {
            aux->surround_on_dist = MIN(aux->surround_on_dist, dist);
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

static void entity_apply_cp_stall(struct movestate_aux *aux, bool stalled)
{
    bool going_nowhere = stalled || ent_wobbling(aux);
    aux->cp_stall_ticks = going_nowhere ? MIN(aux->cp_stall_ticks + 1, UINT16_MAX) : 0;
    if(aux->cp_stall_ticks >= CP_STALL_RELAX_S * hz_count(s_move_work.hz)) {
        aux->cp_relax_ticks = CP_STALL_RELAX_HOLD_S * hz_count(s_move_work.hz);
    }else if(aux->cp_relax_ticks > 0) {
        aux->cp_relax_ticks--;
    }
}

static void entity_apply_cp_side(struct movestate_aux *aux, int side)
{
    if(side != 0) {
        aux->cp_side = side;
        aux->cp_side_ticks = 0;
    }else if(aux->cp_side != 0 && ++aux->cp_side_ticks > CP_SIDE_HOLD_S * hz_count(s_move_work.hz)) {
        aux->cp_side = 0;
    }
}

static void entity_apply_update(uint32_t uid, struct movestate *ms,
                                struct movestate_aux *aux,
                                const struct movestate_patch *patch)
{
    ASSERT_IN_MAIN_THREAD();

    if(!G_EntityExists(uid) || G_EntityIsZombie(uid) || G_EntityIsGarrisoned(uid))
        return;

    if(!ms)
        return;

    /* The next leg of a trip round a target: the state set below stands
     * over the plain move the new destination starts as.
     */
    if(patch->flags & UPDATE_SET_DEST)
        do_set_dest(uid, patch->next_dest, patch->next_attack);

    if(patch->flags & UPDATE_SET_STATE) {

        if(patch->next_state == STATE_ARRIVED || (patch->next_state == STATE_WAITING)) {
            entity_finish_moving(uid, patch->next_state, patch->next_block);
        }else{
            ms->state = patch->next_state;
        }
    }

    if(patch->flags & UPDATE_SET_VELOCITY) {
        /* A unit that stopped this tick stands with no velocity, whatever
         * step the solve had found for it.
         */
        ms->velocity = ent_still(ms) ? (vec2_t){0.0f, 0.0f} : patch->next_velocity;
        bool pivoting = (patch->flags & UPDATE_TURNING_IN_PLACE) || ms->state == STATE_TURNING;
        if(pivoting || PFM_Vec2_Len(&ms->velocity) >= CLEARPATH_STILL_SPEED)
            aux->standing_ticks = 0;
        else if(aux->standing_ticks < UINT16_MAX)
            aux->standing_ticks++;
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

    if(patch->flags & (UPDATE_SET_NEXT_POS | UPDATE_SET_NEXT_ROT)) {
        vec_step_push(&s_out_steps, (struct move_step){
            .uid = uid,
            .has_pos = !!(patch->flags & UPDATE_SET_NEXT_POS),
            .has_rot = !!(patch->flags & UPDATE_SET_NEXT_ROT),
            .pos = patch->next_npos,
            .rot = patch->next_nrot,
        });
        vec_msptr_push(&s_out_step_states, ms);
    }

    if(patch->flags & UPDATE_SET_PREV_POS)
        ms->prev_pos = patch->next_ppos;

    if(patch->flags & UPDATE_SET_NEXT_POS)
        ms->next_pos = patch->next_npos;

    if(patch->flags & UPDATE_SET_PREV_ROT)
        ms->prev_rot = patch->next_prot;

    if(patch->flags & UPDATE_SET_NEXT_ROT)
        ms->next_rot = patch->next_nrot;

    if(patch->flags & UPDATE_SET_TARGET_PREV)
        aux->target_prev_pos = patch->next_target_prev;

    if(patch->flags & UPDATE_SET_TARGET_DIR)
        aux->target_dir = patch->next_target_dir;

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
    aux->facing_follows = !!(patch->flags & UPDATE_FACING_FOLLOWS);
    entity_apply_band_held(aux, !held && (patch->flags & UPDATE_BAND_HELD));
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
        if(stuck) {
            aux->los_off_ticks = LOS_STUCK_FIELD_S * hz_count(s_move_work.hz);
        }else if(aux->los_off_ticks > 0) {
            aux->los_off_ticks--;
        }

        /* A unit that never got going is queueing, not jammed, so only one
         * that has moved may wall itself off. Pinned units hold their line by
         * intent, and a shuttle's re-issued order silently clears the flag.
         */
        bool may_wall = aux->seek_progressed || aux->seek_pin_held
                     || ms->state == STATE_SURROUND_ENTITY;
        /* A relaxed solve has had its window and the unit is still going
         * nowhere: too few neighbours to read as a jam, yet plainly wedged.
         */
        bool relax_spent = aux->cp_stall_ticks
                        >= (CP_STALL_RELAX_S + CP_STALL_RELAX_HOLD_S) * hz_count(s_move_work.hz);
        if(!aux->soft_blocking && may_wall && s_soft_block_budget > 0
        && (relax_spent || (stuck && !clear
                            && aux->seek_stuck_ticks >= SEEK_STUCK_BLOCK_S * hz_count(s_move_work.hz)))) {
            s_soft_block_budget--;
            if(s_log_perf_csv)
                fprintf(stdout, "[soft-why] %u,%u,%s\n", s_move_trace_tick, uid,
                    relax_spent ? "relax" : "stuck");
            entity_soft_block(uid);
        }else if(aux->soft_blocking
              && aux->seek_clear_ticks >= SEEK_RELEASE_HOLD_S * hz_count(s_move_work.hz)) {
            entity_soft_unblock(uid);
        }
    }

    if(patch->flags & UPDATE_SET_NEXT_POS)
        entity_follow_own_move(uid, ms);
}

/* The unit's own moves are not queued back to it, so the snapshot's position
 * follows the tick's end position here, and the stamp of a unit that stops
 * this tick, here or in the command drain, follows it in settle_own_moves.
 */
static void entity_follow_own_move(uint32_t uid, struct movestate *ms)
{
    khiter_t k = kh_get(pos, s_move_work.gamestate.positions, uid);
    if(k != kh_end(s_move_work.gamestate.positions))
        kh_val(s_move_work.gamestate.positions, k) = ms->next_pos;
    vec_entity_push(&s_own_moves, uid);
}

/* A unit that stopped mid-step is stamped where the step ends and keeps
 * sliding the rest of the way there.
 */
static void settle_own_moves(void)
{
    for(int i = 0; i < vec_size(&s_own_moves); i++) {

        uint32_t uid = vec_AT(&s_own_moves, i);
        struct movestate *ms = movestate_get(uid);
        if(!ms || !ms->blocking || !G_EntityExists(uid))
            continue;
        settle_own_move(uid, ms);
    }
    vec_entity_reset(&s_own_moves);
}

static void settle_own_move(uint32_t uid, struct movestate *ms)
{
    vec2_t end = (vec2_t){ms->next_pos.x, ms->next_pos.z};
    if(end.x != ms->last_stop_pos.x || end.z != ms->last_stop_pos.z) {
        struct block_ref ref = block_ref_take(uid, 0,
            G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid),
            G_FlagsGetFrom(s_move_work.gamestate.flags, uid));
        M_NavBlockersDecref(ms->last_stop_pos, ms->last_stop_radius, ref.faction_id, ref.flags, s_map);
        M_NavBlockersIncref(end, ms->last_stop_radius, ref.faction_id, ref.flags, s_map);
        block_ref_save(uid, 0, ref.faction_id, ref.flags);
        ms->last_stop_pos = end;
    }
    ms->prev_pos = ms->next_pos;
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

static void find_neighbours(uint32_t uid, vec2_t ahead,
                            struct cp_ent *out_dyn, size_t *out_ndyn,
                            struct cp_ent *out_stat, size_t *out_nstat,
                            size_t *out_njam, uint32_t *out_links, size_t *out_nlinks,
                            uint32_t *out_nn_uid, float *out_nn_dist)
{
    *out_ndyn = 0;
    *out_nstat = 0;
    *out_njam = 0;
    *out_nlinks = 0;
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
    vec2_t ent_vel = movestate_get(uid)->velocity;
    float react = react_ticks(ent_vel);
    float lookahead = (PFM_Vec2_Len(&ent_vel) + small_max_step_for_class(ent_flags)) * react;
    uint32_t near_ents[512];
    int num_near = gather_movable_neighbours(uid, ent_pos, ent_radius, ent_flags,
        clearpath_reach, lookahead, near_ents, ARR_SIZE(near_ents));

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
    if(num_near > 1) {
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

        /* One made since the snapshot was taken is in it, but has no movestate 
         * until the next tick: a ghost nobody avoids for one tick
         */
        struct movestate *ms = movestate_get(curr);
        if(!ms)
            continue;

        if(phasing
        && G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, curr) == ent_faction
        && ent_still(ms))
            continue;

        vec2_t curr_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, curr);
        vec2_t to_curr;
        PFM_Vec2_Sub(&curr_xz_pos, &ent_pos, &to_curr);

        /* A body this unit is closing on comes into reach as many ticks out
         * as the closing speed makes it.
         */
        float reach = clearpath_reach(ent_radius + curr_radius);
        if(react > 0.0f && dists[i].dist2 > EPSILON) {
            vec2_t rel, dir;
            PFM_Vec2_Sub(&ent_vel, &ms->velocity, &rel);
            PFM_Vec2_Scale(&to_curr, 1.0f / sqrtf(dists[i].dist2), &dir);
            float closing = PFM_Vec2_Dot(&rel, &dir);
            if(closing > 0.0f)
                reach = MAX(reach, ent_radius + curr_radius + closing * react);
        }
        if(dists[i].dist2 > reach * reach)
            continue;

        /* Parked and soft-blocked bodies are workforce and walls, not crowd:
         * counted as crowd, a few walled-off neighbours keep each other's
         * release test failing for good.
         */
        if(!ent_still(ms) && !movestate_aux_get(curr)->soft_blocking)
            (*out_njam)++;

        if(dists[i].dist2 < (*out_nn_dist) * (*out_nn_dist)) {
            *out_nn_dist = sqrtf(dists[i].dist2);
            *out_nn_uid = curr;
        }

        struct cp_ent newdesc = (struct cp_ent) {
            .xz_pos = curr_xz_pos,
            .xz_vel = ms->velocity,
            .radius = curr_radius
        };

        /* A neighbour right at its arrival slot is about to settle and will not yield,
         * so avoid it fully (static) even while it is still moving at speed. */
        struct movestate_aux *curr_aux = movestate_aux_get(curr);
        bool at_slot = Move_Arrival_NeighbourSettling(&curr_aux->arrival,
            curr_xz_pos, newdesc.radius);

        /* A contact in this unit's way, stopped or not, is what the settle
         * judgement weighs (settle_members); with no wish to move, every
         * contact is in the way.
         */
        bool in_the_way = (PFM_Vec2_Len(&ahead) < EPSILON) || (PFM_Vec2_Dot(&to_curr, &ahead) > 0.0f);
        if(in_the_way && *out_nlinks < MAX_WAY_LINKS)
            out_links[(*out_nlinks)++] = curr;

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

            if(ms->state != STATE_ARRIVED || movestate_aux_get(curr)->order_paused) {
                disband = false;
                break;
            }
        });

        if(disband) {

            struct flock *flock = &vec_AT(&s_flocks, i);
            uint32_t uid;
            kh_foreach_key(flock->ents, uid, {
                Move_Formation_RemoveUnit(uid);
            });
            kh_destroy(entity, flock->ents);
            Move_ArrivalGroup_Destroy(&flock->arrival);
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
        .surround_requery_tick = 0,
        .surround_resumes = 0,
        .surround_on_dist = INFINITY,
        .motion_stopped = true,
    };
    memset(new_aux.vel_hist, 0, sizeof(new_aux.vel_hist));

    vec_reset_push(&s_out_resets, (struct move_reset){uid, false});
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

    /* The snapshot may no longer hold a unit removed since it was taken, and
     * a garrisoned one has given up its tiles already
     */
    do_stop(uid);
    if(kh_val(s_entity_state_table, k).blocking) {
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

/* The radius the attack-move field is seeded at. It sits inside the real reach:
 * the field is seeded a whole tile at a time while combat measures the exact
 * distance, so a unit stopped on the outermost seeded tile could be fractionally
 * out of range, hold no flow to walk it in, and stand there without firing. The
 * margin also keeps a unit from dropping its target on its first step backwards.
 */
static float attack_move_seed_reach(uint32_t uid, uint32_t flags)
{
    if(!(flags & ENTITY_FLAG_COMBATABLE))
        return 0.0f;
    return G_Combat_GetEffectiveRange(uid) * ATTACK_MOVE_REACH_FRAC;
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
    struct movestate_aux *daux = movestate_aux_get(uid);
    if(attack) {
        int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
        dest_id = M_NavDestIDForPosAttacking(s_map, dest_xz, layer, faction_id);
        if(daux) {
            daux->attack_reach = attack_move_seed_reach(uid, flags);
            daux->range_latched = false;
            daux->range_miss_ticks = 0;
        }
    }else{
        dest_id = M_NavDestIDForPos(s_map, dest_xz, layer);
        if(daux) {
            daux->attack_reach = 0.0f;
        }
    }
    if(!attack) {
        dest_id = N_DestIDWithRoads(dest_id);
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
        fl->plan_pending = true;
        struct movestate_aux *jaux = movestate_aux_get(uid);
        settle_reset(jaux);

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
    formation_id_t fid = Move_Formation_GetForEnt(uid);
    if(fid != NULL_FID) {
        type = Move_Formation_Type(fid);
    }

    make_flock(&flock, dest_xz, layer, attack, !attack, type);
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
        Entity_NavLayerWithRadius(flags, radius), xz_src, xz_dst, range);
    do_set_dest(uid, xz_target, false);

    ms->state = STATE_ENTER_ENTITY_RANGE;
    ms->surround_target_uid = target;
    struct movestate_aux *aux = movestate_aux_get(uid);
    aux->target_prev_pos = xz_dst;
    aux->target_range = range;
    aux->range_latched = false;
    aux->range_miss_ticks = 0;
}

static bool using_surround_field(uint32_t uid, uint32_t target)
{
    /* Either may be newer than the tick's snapshot */
    vec2_t pos_xz = G_Pos_GetXZ(uid);
    vec2_t target_pos_xz = G_Pos_GetXZ(target);

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

    /* One field serves a whole cluster of the unit's own side */
    if((G_FlagsGet(target) & ENTITY_FLAG_BUILDING) && G_GetFactionID(target) == G_GetFactionID(uid)) {
        target = G_Cluster_FootprintAnchor(target);
    }

    do_stop(uid);
    do_set_dest(uid, G_Pos_GetXZ(target), false);

    assert(!ms->blocking);
    ms->state = STATE_SURROUND_ENTITY;
    ms->surround_target_uid = target;
    ms->using_surround_field = using_surround_field(uid, target);

    struct movestate_aux *aux = movestate_aux_get(uid);
    if(aux) {
        aux->surround_requery_tick = 0;
        aux->surround_resumes = 0;
        aux->surround_shun_tick = 0;
        aux->surround_on_dist = INFINITY;
    }
}

static void do_set_seek_enemies(uint32_t uid)
{
    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return;

    entity_reset_seek_counters(uid);
    clear_seek_pin(uid);

    /* The unit keeps its flock and its formation cell while it fights, so
     * it can go back to them once the fight is over (Move_ResumeOrder).
     */
    if(ent_still(ms)) {
        entity_unblock(uid);
        move_notify_motion_start(uid, ms);
    }

    ms->state = STATE_SEEK_ENEMIES;
}

/* A unit stopped to fight has not finished its order: its flock and its
 * formation cell wait for it (disband_empty_flocks).
 */
static void do_pause_order(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return;
    if(!flock_for_ent(uid)) {
        do_stop(uid);
        return;
    }

    entity_reset_seek_counters(uid);
    clear_seek_pin(uid);
    if(!ent_still(ms))
        entity_finish_moving(uid, STATE_ARRIVED, true);
    ms->state = STATE_ARRIVED;
    movestate_aux_get(uid)->order_paused = true;
}

/* Back to the order the unit was following before a fight took it away:
 * its formation cell, its arrival slot or the ordered point.
 */
static void do_resume_order(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    if(!ms || !flock_for_ent(uid) || ms->state == STATE_FLEEING)
        return;

    entity_reset_seek_counters(uid);
    clear_seek_pin(uid);
    if(ent_still(ms)) {
        entity_unblock(uid);
        move_notify_motion_start(uid, ms);
    }
    settle_reset(movestate_aux_get(uid));
    ms->state = (Move_Formation_GetForEnt(uid) != NULL_FID) ? STATE_MOVING_IN_FORMATION
                                                         : STATE_MOVING;
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
    ms->left = 0;
    vec_reset_push(&s_out_resets, (struct move_reset){uid, true});
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
            if(cmd.u.stop.for_combat)
                do_pause_order(cmd.uid);
            else
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
        case MOVE_CMD_RESUME_ORDER: {
            do_resume_order(cmd.uid);
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

        if(dist <= Move_Flock_SeparationReach(in->cp_ent.radius
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
        /* The flow sample is of the flock's own field; a flock-less unit
         * (fleeing, seeking) has none to sample.
         */
        memset(tr->dirs, 0, sizeof(tr->dirs));
        if(in->flock) {
            struct target target = build_target(uid, in->flock);
            for(int dz = -1; dz <= 1; dz++) {
            for(int dx = -1; dx <= 1; dx++) {
                vec2_t p = (vec2_t){pos.x + dx * nt.x, pos.z + dz * nt.z};
                tr->dirs[(dz + 1) * 3 + (dx + 1)] = M_NavFlowFieldDirAt(map, target, p);
            }}
        }
    }
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

    /* Roles are taken when the pair is already wedged, or as soon as the
     * two are closing head-on within the reaction window, so that the pass
     * is steered rather than negotiated at contact.
     */
    float nn_radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, in->nn_uid);
    float rsum = in->cp_ent.radius + nn_radius;
    bool wedged = movestate_aux_get(in->ent_uid)->cp_stall_ticks > 0
               && movestate_aux_get(in->nn_uid)->cp_stall_ticks > 0
               && in->nn_dist <= rsum + HUG_CONTACT_MARGIN;
    if(!wedged)
        return false;

    float mine = Move_Flock_WallDistance(in->tile_obs, in->ntiles, in->cp_ent.xz_pos);
    float theirs = Move_Flock_WallDistance(in->tile_obs, in->ntiles, nn_pos);
    if(fabsf(mine - theirs) > HUG_WALL_MARGIN)
        return mine < theirs;
    return in->ent_uid < in->nn_uid;
}

/* Standing is a real answer in a crowd and a wedge in a wall pocket, so the
 * two are told apart by how long it persists, not here.
 */
static bool solve_stalled(vec2_t new_vel, vec2_t vpref)
{
    return PFM_Vec2_Len(&new_vel) < CLEARPATH_STALL_SPEED
        && PFM_Vec2_Len(&vpref) >= CLEARPATH_STALL_SPEED;
}

/* The last stretch onto a cell, about one cell pitch: the bodies parked on
 * the cells around stand within it, and the pitch grows with the body.
 */
static float cell_closing_radius(float radius)
{
    return MAX(ARRIVE_SLOWING_RADIUS, 2.0f * radius);
}

/* What is left of the way to the point the unit stops at: its cell, its
 * arrival slot or the ordered point. Units without one get INFINITY.
 */
static float travel_to_stop(const struct move_work_in *in, const struct movestate *ms)
{
    switch(ms->state) {
    case STATE_MOVING:
    case STATE_MOVING_IN_FORMATION:
    case STATE_ARRIVING_TO_CELL:
        break;
    default:
        return INFINITY;
    }
    if(!in->flock || in->range_field)
        return INFINITY;

    vec2_t goal;
    const struct arrival_unit_state *us = &movestate_aux_get(in->ent_uid)->arrival;
    if(in->fstate.fid != NULL_FID && in->fstate.assigned_to_cell)
        goal = in->cell_pos;
    else if(us->sink_valid)
        goal = us->sink;
    else
        goal = in->flock->target_xz;

    vec2_t to_goal;
    PFM_Vec2_Sub(&goal, (vec2_t*)&in->cp_ent.xz_pos, &to_goal);
    return PFM_Vec2_Len(&to_goal);
}

/* The velocity phase's first part: everything the solve takes, exactly as it
 * takes it, into the unit's work item. False when the unit needs no solve and
 * its output is written already.
 */
/* On a road, a unit on its way keeps to the right half of it and passes the
 * units it meets on the right, so that two streams along one road go by each 
 * other instead of meeting head on.
 */
static vec2_t keep_right_vpref(const struct move_work_in *in, const struct movestate *ms,
                               const struct flock *flock, vec2_t vpref, int *inout_side)
{
    if(!s_keep_right)
        return vpref;
    if(ms->state != STATE_MOVING && ms->state != STATE_SURROUND_ENTITY)
        return vpref;

    float speed = PFM_Vec2_Len(&vpref);
    if(speed < EPSILON)
        return vpref;

    vec2_t pos = in->cp_ent.xz_pos;
    vec2_t to_goal;
    PFM_Vec2_Sub((vec2_t*)&flock->target_xz, &pos, &to_goal);
    if(PFM_Vec2_Len(&to_goal) < KEEP_RIGHT_GOAL_DIST)
        return vpref;

    /* Along the way, not the unit's own heading: steering off a heading that
     * the last tick's drift already turned builds into a weave at speed
     */
    vec2_t dir = (vec2_t){vpref.x / speed, vpref.z / speed};
    float des_len = PFM_Vec2_Len((vec2_t*)&in->ent_des_v);
    if(des_len > EPSILON) {
        dir = (vec2_t){in->ent_des_v.x / des_len, in->ent_des_v.z / des_len};
    }
    /* Right as seen on screen, which mirrors the world */
    vec2_t right = (vec2_t){dir.z, -dir.x};
    float to_left, to_right;
    if(!M_NavRoadSpan(s_move_work.gamestate.map, pos, right, KEEP_RIGHT_MAX_WIDTH,
                      &to_left, &to_right))
        return vpref;

    float width = to_left + to_right;
    if(width < KEEP_RIGHT_MIN_WIDTH || width > KEEP_RIGHT_MAX_WIDTH)
        return vpref;

    /* How far right of the road's middle the unit is, and of its lane */
    float offset = (to_left - to_right) / 2.0f;
    float lane = width / 4.0f;
    float lateral = CLAMP((lane - offset) * KEEP_RIGHT_GAIN, 
        -KEEP_RIGHT_MAX_LATERAL, KEEP_RIGHT_MAX_LATERAL);

    vec2_t steer = (vec2_t){dir.x + right.x * lateral, dir.z + right.z * lateral};
    float len = PFM_Vec2_Len(&steer);
    if(in->ntiles == 0) {
        *inout_side = KEEP_RIGHT_SIDE;
    }
    return (vec2_t){steer.x / len * speed, steer.z / len * speed};
}

static bool velocity_gather(int i)
{
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
        return false;
    }

    /* Holds its ground; neighbours still gathered for the release test. */
    if(movestate_aux_get(in->ent_uid)->soft_blocking) {
        find_neighbours(in->ent_uid, in->ent_des_v, in->dyn_neighbs, &in->ndyn,
            in->stat_neighbs, &in->nstat, &in->njam, in->links, &in->nlinks,
            &in->nn_uid, &in->nn_dist);
        out->ent_uid = in->ent_uid;
        out->ent_vel = (vec2_t){0.0f, 0.0f};
        out->cp_flags = 0;
        out->cp_side = 0;
        return false;
    }

    const struct flock *flock = in->flock;

    uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, in->ent_uid);
    bool relax = movestate_aux_get(in->ent_uid)->cp_relax_ticks > 0;
    struct cp_terrain terrain = (struct cp_terrain){
        .map = s_move_work.gamestate.map,
        .layer = Entity_NavLayerWithRadius(flags, in->cp_ent.radius),
        .max_step = in->max_speed / hz_count(s_move_work.hz),
        .tile_horizon = relax ? CLEARPATH_TILE_HORIZON_SEC * hz_count(s_move_work.hz)
                              : 0.0f,
        .travel = travel_to_stop(in, ms),
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
     * the remembered deflection, unless a wall beside the unit leaves no
     * room for the body on that side. Two units bound opposite ways along
     * one lane then each keep to their own wall instead of both taking
     * the lane's middle.
     */
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
        if(in->range_field) {
            vpref = range_seek_vpref(in->ent_uid, flock, in->speed, in->ent_des_v);
            break;
        }
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
        if(in->range_field) {
            vpref = range_seek_vpref(in->ent_uid, flock, in->speed, in->ent_des_v);
            break;
        }
        if(!in->fstate.assignment_ready) {
            vpref = (vec2_t){0.0f, 0.0f};
            break;
        }
        vpref = formation_seek_vpref(in->ent_uid, flock, in->speed, 
            in->ent_des_v,
            in->fstate.normal_cohesion_force,
            in->fstate.normal_align_force,
            in->fstate.normal_drag_force,
            in->has_dest_los, in->range_field);
        break;
    default:
        assert(flock);
        vpref = point_seek_vpref(in->ent_uid, flock, in->fsnap, in->ent_des_v,
            in->has_dest_los && !in->range_field, in->speed, in->max_speed, side, in->range_field);
        vpref = keep_right_vpref(in, ms, flock, vpref, &side);
    }
    assert(vpref.x == vpref.x && vpref.z == vpref.z); /* a NaN vpref would corrupt the integration */

    /* Find the entity's neighbours */
    find_neighbours(in->ent_uid, in->ent_des_v, in->dyn_neighbs, &in->ndyn,
        in->stat_neighbs, &in->nstat, &in->njam, in->links, &in->nlinks,
        &in->nn_uid, &in->nn_dist);

    /* A unit with no wish to move takes no avoidance step either: the others
     * treat it as a still body and keep clear of it, so a step here would
     * only shove it sideways (and a pivot would take a heading from it).
     */
    if(PFM_Vec2_Len(&vpref) < EPSILON) {
        if(traced) {
            move_trace_velocity(in, tr, vpref);
        }
        out->ent_uid = in->ent_uid;
        out->ent_vel = (vec2_t){0.0f, 0.0f};
        out->cp_flags = 0;
        out->cp_side = 0;
        return false;
    }

    /* The hugger keeps its line: its wish loses the detour, and the solve
     * is told the detour is the dearer side, so it brakes rather than
     * sidesteps while the other unit goes round.
     */
    in->hug = ent_hugs_wall(in);
    if(in->hug) {
        vpref = Move_Flock_HugWall(vpref, in->ent_des_v, side);
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

    in->vpref = vpref;
    in->terrain = terrain;
    in->solve_side = side;
    in->relax = relax;
    in->cap_slot = cap_slot;
    return true;
}

/* The velocity phase's last part: the solve's answer into the unit's output */
static void velocity_finish(int i, vec2_t new_vel, struct cp_solve_diag diag)
{
    struct move_work_in *in = &s_move_work.in[i];
    struct move_work_out *out = &s_move_work.out[i];
    struct move_trace *tr = s_move_work.trace ? &s_move_work.trace[i] : NULL;
    bool traced = tr && tr->traced;
    const struct movestate *ms = movestate_get(in->ent_uid);
    vec2_t vpref = in->vpref;
    int cap_slot = in->cap_slot;

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
    Move_Flock_Truncate(&out->ent_vel, in->max_speed / hz_count(s_move_work.hz));
}

static vec2_t velocity_solve(int i, struct cp_solve_diag *diag)
{
    struct move_work_in *in = &s_move_work.in[i];

    /* Compute the velocity constrainted by potential collisions */
    return Move_ClearPath_NewVelocity(in->cp_ent, in->ent_uid,
        in->vpref, in->dyn_neighbs, in->ndyn, in->stat_neighbs, in->nstat,
        in->tile_obs, in->ntiles,
        in->terrain, in->solve_side, in->relax, in->save_debug, diag);
}

static void velocity_solve_cpu(int i)
{
    struct cp_solve_diag diag;
    vec2_t new_vel = velocity_solve(i, &diag);
    velocity_finish(i, new_vel, diag);
}

static void move_velocity_work(int begin_idx, int end_idx)
{
    for(int i = begin_idx; i <= end_idx; i++) {
        if(velocity_gather(i))
            velocity_solve_cpu(i);
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

static void move_dv_work(int begin_idx, int end_idx)
{
    for(int i = begin_idx; i <= end_idx; i++) {

        struct move_work_in *in = &s_move_work.in[i];
        in->ent_des_v = ent_desired_velocity(in);
        s_move_work.out[i].ent_des_v = in->ent_des_v;
    }
}

static struct result move_dv_task(void *arg)
{
    uint64_t t0 = SDL_GetPerformanceCounter();
    struct move_task_arg *move_arg = arg;
    size_t ncomputed = 0;

    for(int i = move_arg->begin_idx; i <= move_arg->end_idx; i++) {

        move_dv_work(i, i);
        ncomputed++;

        if(ncomputed % 128 == 0)
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

        if(ncomputed % 128 == 0)
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

        if(ncomputed % 128 == 0)
            Task_Yield();
    }
    Perf_NavParallelAddSince(t0);
    return NULL_RESULT;
}

static void move_dv_range(int begin, int end, void *arg)
{
    (void)arg;
    uint64_t t0 = SDL_GetPerformanceCounter();
    move_dv_work(begin, end);
    Perf_NavParallelAddSince(t0);
}

static void move_velocity_range(int begin, int end, void *arg)
{
    (void)arg;
    uint64_t t0 = SDL_GetPerformanceCounter();
    move_velocity_work(begin, end);
    Perf_NavParallelAddSince(t0);
}

static void move_update_range(int begin, int end, void *arg)
{
    (void)arg;
    uint64_t t0 = SDL_GetPerformanceCounter();
    move_update_work(begin, end);
    Perf_NavParallelAddSince(t0);
}

static void move_complete_cpu_work(void)
{
    Sched_AwaitAll(s_move_work.tids, s_move_work.futures, s_move_work.ntasks);
    s_move_work.ntasks = 0;
}

static void move_init_nav_unit_query_ctx(void)
{
    s_move_work.unit_query_ctx.flags = s_move_work.gamestate.flags;
    s_move_work.unit_query_ctx.positions = s_move_work.gamestate.positions;
    s_move_work.unit_query_ctx.postree = s_move_work.gamestate.postree;
    s_move_work.unit_query_ctx.faction_ids = s_move_work.gamestate.faction_ids;
    s_move_work.unit_query_ctx.aabbs = s_move_work.gamestate.aabbs;
    s_move_work.unit_query_ctx.footprints = s_move_work.gamestate.footprints;
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

uint32_t Move_NavSnapshotGeneration(void)
{
    ASSERT_IN_MAIN_THREAD();
    return s_nav_snapshot_gen;
}

struct refcounted_map *Move_NavSnapshotAcquire(void)
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

/* The world is the game's to fill; the navigation state is snapshotted here. */
static void move_take_world(const struct move_world *world)
{
    PERF_ENTER();
    s_gs_slot ^= 1;
    s_move_work.gamestate = *world;
    struct refcounted_map *snap = PF_MALLOC(sizeof(struct refcounted_map));
    snap->snapshot = M_AL_SnapshotShared(s_map);
    sp_init(snap, refcounted_map_destroy);
    s_nav_snapshot = snap;
    s_nav_snapshot_gen++;
    s_move_work.gamestate.map = snap->snapshot;

    move_init_nav_unit_query_ctx();
    s_unit_query_ctx_ring[s_gs_slot] = s_move_work.unit_query_ctx;
    M_NavSetNavUnitQueryCtx(s_move_work.gamestate.map, &s_unit_query_ctx_ring[s_gs_slot]);

    PERF_RETURN_VOID();
}

/* A unit part way along its interpolated step is simulated from where the
 * step ends, not from where it is drawn: integrating from the drawn point
 * would cut every step short by the part still to be drawn. The positions
 * are patched here and the grid copy, which only the tick task reads, at the
 * task's start.
 */
static void snapshot_step_ends(void)
{
    size_t nmax = kh_size(s_entity_state_table);
    s_move_work.step_ends = stalloc(&s_move_work.mem, nmax * sizeof(struct step_end));
    s_move_work.nstep_ends = 0;

    for(khiter_t it = kh_begin(s_entity_state_table); it != kh_end(s_entity_state_table); it++) {

        if(!kh_exist(s_entity_state_table, it))
            continue;
        const struct movestate *ms = &kh_value(s_entity_state_table, it);
        if(ms->left == 0)
            continue;
        uint32_t uid = kh_key(s_entity_state_table, it);
        khiter_t k = kh_get(pos, s_move_work.gamestate.positions, uid);
        if(k == kh_end(s_move_work.gamestate.positions))
            continue;
        if(G_FlagsGetFrom(s_move_work.gamestate.flags, uid) & ENTITY_FLAG_GARRISONED)
            continue;

        vec3_t drawn = kh_val(s_move_work.gamestate.positions, k);
        kh_val(s_move_work.gamestate.positions, k) = ms->next_pos;
        s_move_work.step_ends[s_move_work.nstep_ends++] = (struct step_end){
            uid, (vec2_t){drawn.x, drawn.z}, (vec2_t){ms->next_pos.x, ms->next_pos.z}
        };
    }
}

static void snapshot_step_ends_grid(void)
{
    for(size_t i = 0; i < s_move_work.nstep_ends; i++) {
        const struct step_end *se = &s_move_work.step_ends[i];
        bg_ent_update(s_move_work.gamestate.postree, se->drawn.x, se->drawn.z,
            se->end.x, se->end.z, se->uid);
    }
    /* Keep the copy packed, as the live grid was when it was copied. */
    if(s_move_work.nstep_ends > 0)
        bg_ent_cleanup(s_move_work.gamestate.postree);
}

static void move_destroy_gamestate(void)
{
    PERF_ENTER();
    N_JoinCarriedAsyncFields();
    s_move_work.gamestate = (struct move_world){0};
    /* Release before the next tick allocates so the freed block is recycled. */
    if(s_nav_snapshot) {
        sp_release(s_nav_snapshot);
        s_nav_snapshot = NULL;
    }
    if(s_parked_snapshot) {
        sp_release(s_parked_snapshot);
        s_parked_snapshot = NULL;
    }
    PERF_RETURN_VOID();
}

/* The copied tables are retained and refilled by the next tick's copy; only
 * the shared and per-tick resources are dropped here.
 */
static void move_release_gamestate(void)
{
    PERF_ENTER();
    s_move_work.gamestate.map = NULL;
    if(s_parked_snapshot) {
        sp_release(s_parked_snapshot);
    }
    s_parked_snapshot = s_nav_snapshot;
    s_nav_snapshot = NULL;
    PERF_RETURN_VOID();
}

static bool move_consume_work_results(void)
{
    PERF_ENTER();

    if(s_move_work.nwork == 0)
        PERF_RETURN(false);

    PERF_PUSH("apply movement updates");

    s_soft_block_budget = SEEK_BLOCK_BUDGET_PER_TICK;
    for(int i = 0; i < s_move_work.nwork; i++) {
        struct move_work_out *out = &s_move_work.out[i];
        struct movestate *ms = movestate_get(out->ent_uid);
        struct movestate_aux *aux = movestate_aux_get(out->ent_uid);
        if(!aux)
            continue;
        /* A stop applied below records what stood in the way this tick. */
        record_way(aux, &s_move_work.in[i], out);
        entity_apply_cp_stall(aux, !!(out->cp_flags & CP_OUT_STALLED));
        entity_apply_update(out->ent_uid, ms, aux, &out->patch);
        entity_apply_cp_side(aux, out->cp_side);
    }

    PERF_POP();

    stalloc_clear(&s_move_work.mem);
    s_move_work.in = NULL;
    s_move_work.out = NULL;
    s_move_work.nwork = 0;
    s_move_work.ntasks = 0;

    PERF_RETURN(true);
}

static void move_prepare_work(enum movement_hz hz)
{
    /* The work set is drawn from the movement state table, which a removed
     * entity leaves a tick after the dynamic entity set (its removal is a
     * queued command), so the table is the bound, not the set.
     */
    size_t nmax = kh_size(s_entity_state_table);
    s_move_work.in = stalloc(&s_move_work.mem, nmax * sizeof(struct move_work_in));
    s_move_work.out = stalloc(&s_move_work.mem, nmax * sizeof(struct move_work_out));
    s_move_work.neighb_mem = stalloc(&s_move_work.mem,
        nmax * 2 * MAX_NEIGHBOURS * sizeof(struct cp_ent));
    s_move_work.tile_mem = stalloc(&s_move_work.mem,
        nmax * CLEARPATH_MAX_TILE_OBS * sizeof(vec2_t));
    s_move_work.link_mem = stalloc(&s_move_work.mem,
        nmax * MAX_WAY_LINKS * sizeof(uint32_t));
    s_move_work.trace = (s_move_trace_min_radius >= 0.0f)
                      ? stalloc(&s_move_work.mem, nmax * sizeof(struct move_trace))
                      : NULL;
    s_move_work.large_movables = stalloc(&s_move_work.mem,
        MAX_LARGE_MOVABLES * sizeof(struct large_movable));
    s_move_work.nlarge = 0;
    s_move_work.small_max_radius[0] = 0.0f;
    s_move_work.small_max_radius[1] = 0.0f;
    s_move_work.small_max_step[0] = 0.0f;
    s_move_work.small_max_step[1] = 0.0f;
    s_move_work.max_radius = s_max_sel_radius;
    s_move_work.hz = hz;
    s_move_work.type = (s_use_gpu ? WORK_TYPE_GPU : WORK_TYPE_CPU);
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
    in.links = s_move_work.link_mem + idx * MAX_WAY_LINKS;
    in.ndyn = 0;
    in.nstat = 0;
    in.ntiles = 0;
    in.njam = 0;
    in.nlinks = 0;
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
                   || (int)in.ent_uid == s_move_trace_uid
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

static void nav_tick_submit_work(void)
{
    ASSERT_IN_MAIN_THREAD();

    SDL_AtomicSet(&s_tick_task_future.status, FUTURE_INCOMPLETE);
    s_tick_task_tid = Sched_Create(0, navigation_tick_task, NULL, 
            "navigation_tick_task", &s_tick_task_future, TASK_BIG_STACK);
    assert(s_tick_task_tid != NULL_TID);

    uint64_t now = SDL_GetPerformanceCounter();
    s_last_nav_tick_stats.period_us = s_submit_pc ? perf_ticks_to_us(now - s_submit_pc) : 0;
    s_submit_pc = now;
    s_submit_frame = g_frame_idx;
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
    ret |= (out->patch.flags & UPDATE_BAND_HELD)       ? MV_TRACE_BAND_HELD       : 0;
    ret |= (aux->band_yield_ticks > 0)                 ? MV_TRACE_BAND_YIELD      : 0;
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
        vec2_t face = Move_Flock_FacingDir(ms->next_rot);
        float goal_dist = NAN;
        if(in->flock) {
            vec2_t goal = member_goal(in->flock, uid);
            vec2_t to_goal;
            PFM_Vec2_Sub(&goal, (vec2_t*)&in->cp_ent.xz_pos, &to_goal);
            goal_dist = PFM_Vec2_Len(&to_goal);
        }

        fprintf(stdout, "[mv-trace] %u,%u,%.2f,%d,%s,%.3f,%.3f,%.2f,"
            "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
            "%u,%d,%u,%u,%u,%.3f,%.2f,%d,%.4f,%.4f,%d,%d,%u,%u,%u,%.2f,%d,%d,%d,"
            "%d,%u,%u,%u,%u\n",
            s_move_trace_tick, uid, radius, layer, s_state_str[ms->state],
            in->cp_ent.xz_pos.x, in->cp_ent.xz_pos.z, in->speed,
            in->ent_des_v.x, in->ent_des_v.z, tr->vpref.x, tr->vpref.z,
            out->ent_vel.x, out->ent_vel.z, app.x, app.z, face.x, face.z,
            move_trace_flags(in, out, ms, aux), tr->retries,
            (unsigned)in->ndyn, (unsigned)in->nstat,
            tr->nn_uid, tr->nn_dist, tr->nn_radius, (int)tr->nn_seen,
            tr->sep.x, tr->sep.z, tr->sep_n, out->cp_side, (unsigned)in->ntiles,
            (unsigned)in->nlinks, (unsigned)aux->cp_stall_ticks, goal_dist,
            (int)in->fstate.in_range_of_cell, (int)in->fstate.arrived_at_cell,
            (int)in->fstate.may_park,
            (int)in->njam, (unsigned)in->nterrain, ms->surround_target_uid,
            (unsigned)aux->seek_stuck_ticks, (unsigned)aux->seek_clear_ticks);

        bool zeroed = PFM_Vec2_Len(&out->ent_vel) > CLEARPATH_STILL_SPEED
                   && PFM_Vec2_Len(&app) < EPSILON
                   && !(out->patch.flags & UPDATE_HEADING_GATED);
        bool voided = PFM_Vec2_Len(&in->ent_des_v) < EPSILON && !ent_still(ms)
                   && (ms->state == STATE_SURROUND_ENTITY || ms->state == STATE_MOVING);
        bool slow = PFM_Vec2_Len(&app) < CLEARPATH_STILL_SPEED && !ent_still(ms)
                 && (PFM_Vec2_Len(&in->ent_des_v) > EPSILON || voided);
        if((zeroed || voided || slow || (out->cp_flags & CP_OUT_STALLED))
        && in->flock && (s_move_trace_tick + uid) % 47 == 0) {
            const struct map *map = s_move_work.gamestate.map;
            vec2_t nt = N_TileDims();
            vec2_t pos = in->cp_ent.xz_pos;
            struct target target = build_target(uid, in->flock);
            struct map_resolution gres;
            struct tile_desc gtd;
            M_NavGetResolution(map, &gres);
            M_Tile_DescForPoint2D(gres, M_GetPos(map), pos, &gtd);
            fprintf(stdout, "[mv-grid] %u,%u,%.2f,%.2f,%d,%d,%d,%d,", s_move_trace_tick, uid,
                pos.x, pos.z, gtd.chunk_r, gtd.chunk_c, gtd.tile_r, gtd.tile_c);
            for(int dz = 4; dz >= -4; dz--) {
                for(int dx = -4; dx <= 4; dx++) {
                    vec2_t p = (vec2_t){pos.x + dx * nt.x, pos.z + dz * nt.z};
                    char c = !M_NavPositionPathable(map, layer, p) ? '#'
                           : M_NavPositionBlocked(map, layer, p)   ? 'B' : '.';
                    int dir = M_NavFlowFieldDirAt(map, target, p);
                    fprintf(stdout, "%c%d", c, dir);
                }
                fputc(dz > -4 ? '|' : '\n', stdout);
            }
            fprintf(stdout, "[mv-isl] %u,%u,", s_move_trace_tick, uid);
            for(int dz = 4; dz >= -4; dz--) {
                for(int dx = -4; dx <= 4; dx++) {
                    vec2_t p = (vec2_t){pos.x + dx * nt.x, pos.z + dz * nt.z};
                    fprintf(stdout, "%d%c ", M_NavPositionLocalIsland(map, layer, p),
                        M_NavPositionIsPortal(map, layer, p) ? 'P' : ' ');
                }
                fputc(dz > -4 ? '|' : '\n', stdout);
            }
        }

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

/* With 'wait', a task that a worker runs, or that waits for the ranges it handed
 * out, is waited for: the caller is about to drop it and clear what it reads.
 */
static enum move_work_status nav_tick_finish_work(bool wait)
{
    if(s_tick_task_tid == NULL_TID) {
        return WORK_COMPLETE;
    }

    PERF_PUSH("nav tick drain");
    uint64_t drain_start = SDL_GetPerformanceCounter();
    uint64_t wait_end = 0;
    while(!Sched_FutureIsReady(&s_tick_task_future)) {
        if(Sched_RunSync(s_tick_task_tid)) {
            wait_end = 0;
            continue;
        }
        /* If the task is event-blocked waiting for GPU results,
         * we are not able to run it to completion at this point.
         */
        if(Sched_IsEventBlocked(s_tick_task_tid)) {
            PERF_POP();
            return WORK_INCOMPLETE;
        }
        if(!wait) {
            PERF_POP();
            return WORK_INCOMPLETE;
        }
        uint64_t now = SDL_GetPerformanceCounter();
        if(!wait_end) {
            wait_end = now + SDL_GetPerformanceFrequency() / 2;
        }else if(now > wait_end) {
            fprintf(stderr, "[movement] the navigation tick task could not be finished\n");
            PERF_POP();
            return WORK_INCOMPLETE;
        }
        SDL_Delay(0);
    }
    PERF_POP();
    s_tick_task_tid = NULL_TID;
    s_last_nav_tick_stats.fiber_frames = g_frame_idx - s_submit_frame;

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
    s_last_nav_tick_stats.nfield_tasks    = diag.nfield_tasks;
    s_last_nav_tick_stats.field_us_sum    = diag.field_us_sum;
    s_last_nav_tick_stats.field_us_max    = diag.field_us_max;
    s_last_nav_tick_stats.field_wall_us   = diag.field_wall_us;
    s_last_nav_tick_stats.nchain_tasks    = diag.nchain_tasks;
    s_last_nav_tick_stats.chain_us_sum    = diag.chain_us_sum;
    s_last_nav_tick_stats.chain_us_max    = diag.chain_us_max;
    s_last_nav_tick_stats.chain_wall_us   = diag.chain_wall_us;
    s_last_nav_tick_stats.ncarried        = diag.ncarried;
    s_last_nav_tick_stats.astar_us        = diag.astar_us;
    s_last_nav_tick_stats.req_prep_us     = diag.req_prep_us;
    s_last_nav_tick_stats.inline_flood_us = diag.inline_flood_us;
    s_last_nav_tick_stats.ninline_flood   = diag.ninline_flood;
    s_last_nav_tick_stats.req_total_us    = diag.req_total_us;
    s_last_nav_tick_stats.nreq            = diag.nreq;
    s_last_nav_tick_stats.req_walk_us     = diag.req_walk_us;

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
    s_last_nav_tick_stats.nlate = 0;

    return WORK_COMPLETE;
}

static bool los_held_off(uint32_t uid)
{
    return movestate_aux_get(uid)->los_off_ticks > 0;
}

static void move_los_peek_work(int begin_idx, int end_idx)
{
    const struct map *map = s_move_work.gamestate.map;

    for(int i = begin_idx; i <= end_idx; i++) {

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
        bool field_led = (ms->state == STATE_SURROUND_ENTITY && ms->using_surround_field)
                      || (ms->state == STATE_ENTER_ENTITY_RANGE
                          && movestate_aux_get(in->ent_uid)->range_latched);
        if(fl && !field_led) {
            bool present;
            bool vis = M_NavHasDestLOSCached(map, fl->dest_id, pos, &present);
            in->has_dest_los = present && vis && !los_held_off(in->ent_uid);
            in->needs_los_build = !present
                               || M_NavDestLOSRebuildDue(map, fl->dest_id, pos);
            in->los_queried = true;
        }
    }
}

static struct result move_los_peek_task(void *arg)
{
    uint64_t t0 = SDL_GetPerformanceCounter();
    struct move_task_arg *move_arg = arg;
    size_t ncomputed = 0;

    for(int i = move_arg->begin_idx; i <= move_arg->end_idx; i++) {

        move_los_peek_work(i, i);
        ncomputed++;

        if(ncomputed % 128 == 0)
            Task_Yield();
    }
    Perf_NavParallelAddSince(t0);
    return NULL_RESULT;
}

static void move_los_peek_range(int begin, int end, void *arg)
{
    (void)arg;
    uint64_t t0 = SDL_GetPerformanceCounter();
    move_los_peek_work(begin, end);
    Perf_NavParallelAddSince(t0);
}

static void compute_los_state(void)
{
    PERF_ENTER();

    /* Parallel read: peek each unit's cached LOS read-only and flag the misses. */
    uint64_t phase_start = SDL_GetPerformanceCounter();
    if(s_move_parallel_executor) {
        Sched_ParallelFor(move_los_peek_range, NULL, s_move_work.nwork, 256);
    }else{
        move_submit_cpu_work(move_los_peek_task);
        move_complete_cpu_work();
    }
    s_last_nav_tick_stats.los_peek_us =
        perf_ticks_to_us(SDL_GetPerformanceCounter() - phase_start);

    /* Serial record: route the flagged units' misses through the path request,
     * which defers the LOS floods to the worker pool as per-destination chains.
     */
    phase_start = SDL_GetPerformanceCounter();
    const struct map *map = s_move_work.gamestate.map;
    unsigned checked = 0, misses = 0, stale_rebuilds = 0, flagged = 0;
    uint64_t ensure_ticks = 0;
    size_t nwork = s_move_work.nwork;
    for(size_t k = 0; k < nwork; k++) {

        size_t idx = (s_rebuild_start + k) % nwork;
        struct move_work_in *in = &s_move_work.in[idx];
        if(in->los_queried)
            checked++;
        if(!in->needs_los_build)
            continue;
        flagged++;

        const struct movestate *ms = movestate_get(in->ent_uid);
        const struct flock *fl = in->flock;
        vec2_t pos = (vec2_t){ms->prev_pos.x, ms->prev_pos.z};

        /* The needs_los_build flag is a snapshot from the parallel peek: an
         * earlier build this tick may have already cached this (dest, chunk)
         * LOS field. A hit here costs no rebuild budget. A stale-served entry
         * whose rebuild is due keeps its answer but falls through to the
         * budgeted rebuild below. */
        bool present;
        bool vis = M_NavHasDestLOSCached(map, fl->dest_id, pos, &present);
        if(present) {
            in->has_dest_los = vis && !los_held_off(in->ent_uid);
            if(!M_NavDestLOSRebuildDue(map, fl->dest_id, pos))
                continue;
        }

        /* Neither does an already-recorded chain build */
        if(M_NavDestLOSPending(map, fl->dest_id, pos)) {
            in->los_deferred = true;
            continue;
        }
        if(present) {
            if(stale_rebuilds == MAX_LOS_STALE_REBUILDS_PER_TICK)
                continue;
            stale_rebuilds++;
        }
        misses++;

        if(s_rebuild_budget == 0) {
            if(s_first_starved == (size_t)-1)
                s_first_starved = idx;
            continue;
        }
        s_rebuild_budget--;

        uint64_t ensure_t0 = SDL_GetPerformanceCounter();
        enum los_ensure_result ensured =
            M_NavEnsureDestLOS(map, fl->dest_id, pos, fl->target_xz, &vis);
        ensure_ticks += SDL_GetPerformanceCounter() - ensure_t0;
        switch(ensured) {
        case LOS_ENSURE_ANSWER:
            in->has_dest_los = vis && !los_held_off(in->ent_uid);
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

    s_last_nav_tick_stats.los_record_us =
        perf_ticks_to_us(SDL_GetPerformanceCounter() - phase_start);
    s_last_nav_tick_stats.los_ensure_us = perf_ticks_to_us(ensure_ticks);
    s_last_nav_tick_stats.nlos_flagged = flagged;
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
        if(!ms || ms->state == STATE_TURNING || ms->state == STATE_FLEEING)
            continue;
        if(ms->state == STATE_ARRIVING_TO_CELL && !movestate_aux_get(uid)->range_latched)
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
    if(s_move_parallel_executor) {
        Sched_ParallelFor(move_dv_range, NULL, s_move_work.nwork, 256);
    }else{
        move_submit_cpu_work(move_dv_task);
        move_complete_cpu_work();
    }
    PERF_RETURN_VOID();
}

/* The side of the tile patch a unit's solve reads on the GPU: every tile a
 * step clamped to max_step can end on.
 */
static int gpu_patch_dim(const struct map_resolution *res, float max_step)
{
    float tile = (float)res->field_w / res->tile_w;
    int reach = (int)ceilf(max_step / tile) + 1;
    return 2 * reach + 1;
}

static size_t gpu_patch_words(int dim)
{
    return (size_t)(dim * dim * 2 + 31) / 32;
}

static void move_velocity_gather_range(int begin, int end, void *arg)
{
    (void)arg;
    uint64_t t0 = SDL_GetPerformanceCounter();
    for(int i = begin; i <= end; i++) {
        s_move_work.in[i].gpu_slot = velocity_gather(i) ? GPU_SLOT_CPU : GPU_SLOT_NONE;
    }
    Perf_NavParallelAddSince(t0);
}

/* Slots and buffer offsets for the solves the GPU takes. The debug overlay's
 * unit, whose solve the CPU records, and a unit whose step could land beyond
 * the widest patch stay on the CPU.
 */
static void gpu_assign_slots(void)
{
    const struct map *map = s_move_work.gamestate.map;
    struct map_resolution res;
    M_NavGetResolution(map, &res);
    vec3_t map_pos = M_GetPos(map);

    s_move_work.gpu_first = stalloc(&s_move_work.mem, s_move_work.nwork * sizeof(uint32_t));
    s_move_work.gpu_pfirst = stalloc(&s_move_work.mem, s_move_work.nwork * sizeof(uint32_t));
    s_move_work.gpu_parity = s_gpu_parity
        ? stalloc(&s_move_work.mem, s_move_work.nwork * sizeof(struct gpu_parity_ref))
        : NULL;
    size_t nunits = 0, nnbs = 0, nwords = 0;

    for(int i = 0; i < s_move_work.nwork; i++) {

        struct move_work_in *in = &s_move_work.in[i];
        if(in->gpu_slot != GPU_SLOT_CPU)
            continue;

        struct tile_desc td;
        int dim = gpu_patch_dim(&res, in->terrain.max_step);
        if(in->save_debug || dim > GPU_PATCH_MAX_DIM
        || !M_Tile_DescForPoint2D(res, map_pos, in->cp_ent.xz_pos, &td))
            continue;

        in->gpu_slot = nunits++;
        s_move_work.gpu_first[i] = nnbs;
        s_move_work.gpu_pfirst[i] = nwords;
        nnbs += in->ndyn + in->nstat + in->ntiles;
        nwords += gpu_patch_words(dim);
    }
    s_move_work.gpu_nunits = nunits;
    s_move_work.gpu_nnbs = nnbs;
    s_move_work.gpu_nwords = nwords;
}

static void gpu_await(SDL_atomic_t *flag, int value)
{
    /* The render thread signals from inside its frame, which spans both */
    static const int events[] = {EVENT_UPDATE_START, EVENT_UPDATE_END};
    for(int i = 0; SDL_AtomicGet(flag) != value; i++) {
        int source;
        Task_AwaitEvent(events[i % 2], &source);
    }
}

/* Grows the mapped buffers to hold this tick's solves, false if they could
 * not be had.
 */
static bool gpu_reserve(void)
{
    size_t need[4] = {
        s_move_work.gpu_nunits * sizeof(struct gpu_cp_unit),
        s_move_work.gpu_nnbs * sizeof(struct gpu_cp_nb),
        s_move_work.gpu_nwords * sizeof(uint32_t),
        s_move_work.gpu_nunits * sizeof(struct gpu_cp_result),
    };
    size_t have[4] = {
        s_gpu_bufs.units ? s_gpu_bufs.units_cap : 0,
        s_gpu_bufs.nbs ? s_gpu_bufs.nbs_cap : 0,
        s_gpu_bufs.patch ? s_gpu_bufs.patch_cap : 0,
        s_gpu_bufs.results ? s_gpu_bufs.results_cap : 0,
    };
    if(need[0] <= have[0] && need[1] <= have[1] && need[2] <= have[2] && need[3] <= have[3])
        return true;

    /* With room to grow, so that the reserve is rare */
    size_t caps[4];
    for(int i = 0; i < 4; i++) {
        caps[i] = MAX(need[i] + need[i] / 2, (size_t)4096);
    }

    SDL_AtomicSet(&s_gpu_reserved, 0);
    Task_RescheduleOnMain();
    R_PushCmd((struct rcmd){
        .func = R_GL_MoveReserve,
        .nargs = 3,
        .args = {
            R_PushArg(caps, sizeof(caps)),
            &s_gpu_bufs,
            &s_gpu_reserved,
        },
    });
    gpu_await(&s_gpu_reserved, 1);
    return (s_gpu_bufs.units != NULL);
}

/* The retries compact the neighbour arrays, so the CPU solves copies */
static void gpu_parity_solve(int i)
{
    const struct move_work_in *in = &s_move_work.in[i];
    struct cp_ent dyn[MAX_NEIGHBOURS], stat[MAX_NEIGHBOURS];
    vec2_t tiles[CLEARPATH_MAX_TILE_OBS];
    memcpy(dyn, in->dyn_neighbs, in->ndyn * sizeof(struct cp_ent));
    memcpy(stat, in->stat_neighbs, in->nstat * sizeof(struct cp_ent));
    memcpy(tiles, in->tile_obs, in->ntiles * sizeof(vec2_t));

    struct gpu_parity_ref *ref = &s_move_work.gpu_parity[i];
    ref->vel = Move_ClearPath_NewVelocity(in->cp_ent, in->ent_uid, in->vpref,
        dyn, in->ndyn, stat, in->nstat, tiles, in->ntiles,
        in->terrain, in->solve_side, in->relax, false, &ref->diag);
}

static void move_velocity_pack_range(int begin, int end, void *arg)
{
    (void)arg;
    uint64_t t0 = SDL_GetPerformanceCounter();

    const struct map *map = s_move_work.gamestate.map;
    struct map_resolution res;
    M_NavGetResolution(map, &res);
    vec3_t map_pos = M_GetPos(map);

    struct gpu_cp_unit *units = s_gpu_bufs.units;
    struct gpu_cp_nb *nbs = s_gpu_bufs.nbs;
    uint32_t *patch = s_gpu_bufs.patch;

    for(int i = begin; i <= end; i++) {

        const struct move_work_in *in = &s_move_work.in[i];
        if(in->gpu_slot < 0)
            continue;

        struct tile_desc td;
        M_Tile_DescForPoint2D(res, map_pos, in->cp_ent.xz_pos, &td);
        int dim = gpu_patch_dim(&res, in->terrain.max_step);
        int row0 = td.chunk_r * res.tile_h + td.tile_r - dim / 2;
        int col0 = td.chunk_c * res.tile_w + td.tile_c - dim / 2;

        /* Built in cached memory: the mapping is write-combined */
        uint32_t words[(GPU_PATCH_MAX_DIM * GPU_PATCH_MAX_DIM * 2 + 31) / 32];
        M_NavTilePatch(map, in->terrain.layer, row0, col0, dim, words);
        uint32_t pfirst = s_move_work.gpu_pfirst[i];
        memcpy(patch + pfirst, words, gpu_patch_words(dim) * sizeof(uint32_t));

        uint32_t first = s_move_work.gpu_first[i];
        units[in->gpu_slot] = (struct gpu_cp_unit){
            .px = in->cp_ent.xz_pos.x,
            .pz = in->cp_ent.xz_pos.z,
            .vx = in->cp_ent.xz_vel.x,
            .vz = in->cp_ent.xz_vel.z,
            .dx = in->vpref.x,
            .dz = in->vpref.z,
            .radius = in->cp_ent.radius,
            .max_step = in->terrain.max_step,
            .horizon = in->terrain.tile_horizon,
            .travel = in->terrain.travel,
            .flags = (in->relax ? GPU_UNIT_RELAX : 0)
                   | (in->terrain.on_blocked ? GPU_UNIT_ON_BLOCKED : 0),
            .side = in->solve_side,
            .first = first,
            .ndyn = in->ndyn,
            .nstat = in->nstat,
            .ntiles = in->ntiles,
            .prow = row0,
            .pcol = col0,
            .pdim = dim,
            .pfirst = pfirst,
        };

        for(size_t k = 0; k < in->ndyn; k++) {
            const struct cp_ent *n = &in->dyn_neighbs[k];
            nbs[first++] = (struct gpu_cp_nb){
                n->xz_pos.x, n->xz_pos.z, n->xz_vel.x, n->xz_vel.z, n->radius
            };
        }
        for(size_t k = 0; k < in->nstat; k++) {
            const struct cp_ent *n = &in->stat_neighbs[k];
            nbs[first++] = (struct gpu_cp_nb){
                n->xz_pos.x, n->xz_pos.z, n->xz_vel.x, n->xz_vel.z, n->radius
            };
        }
        for(size_t k = 0; k < in->ntiles; k++) {
            nbs[first++] = (struct gpu_cp_nb){
                in->tile_obs[k].x, in->tile_obs[k].z, 0.0f, 0.0f, 0.0f
            };
        }
        if(s_gpu_parity) {
            gpu_parity_solve(i);
        }
    }
    Perf_NavParallelAddSince(t0);
}

static void move_velocity_finish_range(int begin, int end, void *arg)
{
    (void)arg;
    uint64_t t0 = SDL_GetPerformanceCounter();
    const struct gpu_cp_result *results = s_gpu_bufs.results;

    for(int i = begin; i <= end; i++) {

        const struct move_work_in *in = &s_move_work.in[i];
        if(in->gpu_slot == GPU_SLOT_CPU) {
            velocity_solve_cpu(i);
            continue;
        }
        if(in->gpu_slot < 0)
            continue;

        struct gpu_cp_result res = results[in->gpu_slot];
        /* A landing outside the patch cannot be answered on the GPU */
        if(res.diag & GPU_DIAG_PATCH_MISS) {
            velocity_solve_cpu(i);
            continue;
        }
        struct cp_solve_diag diag = {
            .retries = res.diag & GPU_DIAG_RETRIES,
            .gave_up = !!(res.diag & GPU_DIAG_GAVE_UP),
            .side = res.side,
        };
        if(s_gpu_parity) {
            const struct gpu_parity_ref *ref = &s_move_work.gpu_parity[i];
            SDL_AtomicAdd(&s_gpu_nparity, 1);
            if(ref->vel.x != res.vx || ref->vel.z != res.vz)
                SDL_AtomicAdd(&s_gpu_nvel_diff, 1);
            if(ref->diag.retries != diag.retries || ref->diag.gave_up != diag.gave_up
            || ref->diag.side != diag.side)
                SDL_AtomicAdd(&s_gpu_ndiag_diff, 1);
        }
        velocity_finish(i, (vec2_t){res.vx, res.vz}, diag);
    }
    Perf_NavParallelAddSince(t0);
}

/* The velocity phase with the solve on the GPU: the workers gather each unit's
 * inputs as for the CPU solve and write them straight into the mapped
 * buffers, the render thread dispatches the solve, and the workers finish the
 * units from its results.
 */
static void gpu_velocity_computations(void)
{
    SDL_AtomicSet(&s_gpu_nparity, 0);
    SDL_AtomicSet(&s_gpu_nvel_diff, 0);
    SDL_AtomicSet(&s_gpu_ndiag_diff, 0);
    uint64_t start = SDL_GetPerformanceCounter();
    uint64_t t_post = 0, t_wake = 0;
    unsigned long start_frame = g_frame_idx;

    Sched_ParallelFor(move_velocity_gather_range, NULL, s_move_work.nwork, 64);
    gpu_assign_slots();

    bool on_gpu = (s_move_work.gpu_nunits > 0) && gpu_reserve();
    if(!on_gpu) {
        for(int i = 0; i < s_move_work.nwork; i++) {
            if(s_move_work.in[i].gpu_slot >= 0)
                s_move_work.in[i].gpu_slot = GPU_SLOT_CPU;
        }
    }else{
        Sched_ParallelFor(move_velocity_pack_range, NULL, s_move_work.nwork, 256);

        const struct map *map = s_move_work.gamestate.map;
        struct map_resolution res;
        M_NavGetResolution(map, &res);
        vec3_t map_pos = M_GetPos(map);
        vec2_t map_xz = (vec2_t){map_pos.x, map_pos.z};
        int nunits = s_move_work.gpu_nunits;

        /* A cancelled tick's solve may yet signal, with an older number */
        int seq = ++s_gpu_seq;
        s_gpu_times = (struct gpu_move_times){0};
        t_post = SDL_GetPerformanceCounter();
        R_GL_MovePost(&res, map_xz, nunits, &s_gpu_solved, seq,
            s_log_perf_csv ? &s_gpu_times : NULL);
        gpu_await(&s_gpu_solved, seq);
        t_wake = SDL_GetPerformanceCounter();
    }
    Sched_ParallelFor(move_velocity_finish_range, NULL, s_move_work.nwork, 64);

    if(s_log_perf_csv) {
        uint64_t t_end = SDL_GetPerformanceCounter();
        fprintf(stdout, "[gpu-move] %u,%zu,%zu,%lu,%u,%d,%d,%d,%u,%u,%u,%u,%u\n", s_move_trace_tick,
            s_move_work.gpu_nunits, s_move_work.gpu_nnbs, g_frame_idx - start_frame,
            (unsigned)perf_ticks_to_us(t_end - start),
            SDL_AtomicGet(&s_gpu_nparity), SDL_AtomicGet(&s_gpu_nvel_diff),
            SDL_AtomicGet(&s_gpu_ndiag_diff), s_gpu_times.gpu_us,
            t_post ? (unsigned)perf_ticks_to_us(t_post - start) : 0,
            s_gpu_times.dispatched ? (unsigned)perf_ticks_to_us(s_gpu_times.dispatched - start) : 0,
            s_gpu_times.signalled ? (unsigned)perf_ticks_to_us(s_gpu_times.signalled - start) : 0,
            t_wake ? (unsigned)perf_ticks_to_us(t_wake - start) : 0);
    }
}

static void fork_join_velocity_computations(void)
{
    switch(s_move_work.type) {
    case WORK_TYPE_CPU:
        if(s_move_parallel_executor) {
            Sched_ParallelFor(move_velocity_range, NULL, s_move_work.nwork, 64);
        }else{
            move_submit_cpu_work(move_velocity_task);
            move_complete_cpu_work();
        }
        break;
    case WORK_TYPE_GPU:
        gpu_velocity_computations();
        break;
    default: assert(0);
    }
}

static void fork_join_state_updates(void)
{
    PERF_ENTER();
    if(s_move_parallel_executor) {
        PERF_PUSH("move::state updates");
        Sched_ParallelFor(move_update_range, NULL, s_move_work.nwork, 256);
        PERF_POP();
        PERF_RETURN_VOID();
    }

    PERF_PUSH("move::submit state updates");
    move_submit_cpu_work(move_update_task);
    PERF_POP();

    PERF_PUSH("move::complete state updates");
    move_complete_cpu_work();
    PERF_POP();

    PERF_RETURN_VOID();
}

static struct result navigation_tick_task(void *arg)
{
    s_nav_task_active_tid = Sched_ActiveTID();
    uint64_t nav_start = SDL_GetPerformanceCounter();

    Perf_NavParallelReset();
    snapshot_step_ends_grid();
    s_rebuild_budget = MAX_REBUILDS_PER_TICK;
    s_first_starved = (size_t)-1;
    N_ApplyDeferredInvalidations();

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

/* A unit held on its way waits on the things that stopped it, not on a clock:
 * the body in its way, its goal, and the tiles of its layer's chunks here and
 * one step ahead. The cell counts are read before its own stop lands on the
 * map, so its own stamp never wakes it: a stop frees nothing.
 */
static void hold_begin(uint32_t uid, const struct movestate *ms, struct movestate_aux *aux)
{
    aux->hold_way = (ms->state == STATE_MOVING)
                 || (ms->state == STATE_MOVING_IN_FORMATION)
                 || (ms->state == STATE_ARRIVING_TO_CELL);
    if(!aux->hold_way)
        return;
    /* Held up is standing, however it was moving the tick before. */
    aux->standing_ticks = UINT16_MAX;
    aux->noapproach_ticks = 0;

    float radius = G_GetSelectionRadius(uid);
    enum nav_layer layer = Entity_NavLayerWithRadius(G_FlagsGet(uid), radius);
    vec2_t pos = G_Pos_GetXZ(uid);
    vec2_t tile = N_TileDims();
    vec2_t ahead = aux->way_dir;
    PFM_Vec2_Scale(&ahead, radius + 2.0f * MAX(tile.x, tile.z), &ahead);
    PFM_Vec2_Add(&pos, &ahead, &ahead);

    const struct flock *flock = flock_for_ent(uid);
    if(ms->state != STATE_WAITING)
        aux->hold_link = way_first_open(uid, aux);
    aux->hold_goal = flock ? member_goal(flock, uid) : pos;
    aux->hold_ahead = M_ClampedMapCoordinate(s_map, ahead);
    aux->hold_gen_here = M_NavCellFreedGeneration(s_map, layer, pos);
    aux->hold_gen_ahead = M_NavCellFreedGeneration(s_map, layer, aux->hold_ahead);
}

/* The reason a held unit goes on again, or NULL while nothing it waits on has
 * changed.
 */
static const char *hold_wake_reason(uint32_t uid, struct movestate_aux *aux)
{
    /* The body it waited on is gone, has stopped for good, or is out of its
     * way: beyond contact, or no longer ahead. A body jostled about in
     * contact ahead of it still bars the way.
     */
    uint32_t link = aux->hold_link;
    if(link != NULL_UID) {
        if(!G_EntityExists(link) || body_rooted(link) || !movestate_get(link))
            return "link";
        vec2_t pos = G_Pos_GetXZ(uid);
        vec2_t to_link;
        vec2_t lpos = G_Pos_GetXZ(link);
        PFM_Vec2_Sub(&lpos, &pos, &to_link);
        float reach = clearpath_reach(G_GetSelectionRadius(uid) + G_GetSelectionRadius(link));
        if(PFM_Vec2_Len(&to_link) > reach)
            return "link";
        if(PFM_Vec2_Len(&aux->way_dir) > EPSILON && PFM_Vec2_Dot(&to_link, &aux->way_dir) <= 0.0f)
            return "link";
    }

    const struct flock *flock = flock_for_ent(uid);
    if(flock) {
        vec2_t goal = member_goal(flock, uid);
        vec2_t goal_moved;
        PFM_Vec2_Sub(&goal, &aux->hold_goal, &goal_moved);
        if(PFM_Vec2_Len(&goal_moved) > EPSILON)
            return "goal";
    }

    float radius = G_GetSelectionRadius(uid);
    enum nav_layer layer = Entity_NavLayerWithRadius(G_FlagsGet(uid), radius);
    if(M_NavCellFreedGeneration(s_map, layer, G_Pos_GetXZ(uid)) != aux->hold_gen_here
    || M_NavCellFreedGeneration(s_map, layer, aux->hold_ahead) != aux->hold_gen_ahead)
        return "freed";
    return NULL;
}

/* Waiting units aren't in the per-tick work; wake them here: a unit held on
 * its way when what held it changes, the rest on their countdown.
 */
static void resume_waiting_units(void)
{
    ASSERT_IN_MAIN_THREAD();

    for(khiter_t k = kh_begin(s_entity_state_table); k != kh_end(s_entity_state_table); k++) {

        if(!kh_exist(s_entity_state_table, k))
            continue;
        uint32_t uid = kh_key(s_entity_state_table, k);
        struct movestate *ms = &kh_value(s_entity_state_table, k);
        if(ms->state != STATE_WAITING)
            continue;
        if(!G_EntityExists(uid))
            continue;

        struct movestate_aux *aux = movestate_aux_get(uid);
        if(aux->hold_way) {
            const char *why = hold_wake_reason(uid, aux);
            if(!why)
                continue;
            if(s_log_perf_csv)
                fprintf(stdout, "[wake] %u,%u,%s\n", s_move_trace_tick, uid, why);
        }else if(aux->wait_ticks_left > 0 && --aux->wait_ticks_left > 0) {
            continue;
        }

        assert(aux->wait_prev == STATE_MOVING
            || aux->wait_prev == STATE_MOVING_IN_FORMATION
            || aux->wait_prev == STATE_ARRIVING_TO_CELL
            || aux->wait_prev == STATE_SEEK_ENEMIES
            || aux->wait_prev == STATE_SURROUND_ENTITY
            || aux->wait_prev == STATE_ENTER_ENTITY_RANGE);

        if(ms->blocking)
            entity_unblock(uid);
        /* A unit trying its way again is standing until it finds a step; the
         * submit's level-triggered clip logic announces the motion when it
         * does. Pivots and scripts rotate a still unit behind the shadow's
         * back; motion resumes from the rotation the unit is showing.
         */
        /* Held up again and again on its way round the target, the ground it
         * was bound for has closed under it: it picks its spot afresh, and
         * after a few tries stands where it is as arrived, which lets its
         * errand settle from there
         */
        if(aux->wait_prev == STATE_SURROUND_ENTITY) {
            aux->surround_requery_tick = 0;
            if(++aux->surround_resumes >= SURROUND_MAX_RESUMES) {
                aux->surround_resumes = 0;
                entity_finish_moving(uid, STATE_ARRIVED, true);
                continue;
            }
        }
        if(aux->hold_way) {
            ms->prev_rot = ms->next_rot = Entity_GetRot(uid);
        }else{
            move_notify_motion_start(uid, ms);
        }
        ms->state = aux->wait_prev;
    }
}

static void output_reset(void)
{
    vec_step_reset(&s_out_steps);
    vec_msptr_reset(&s_out_step_states);
    vec_entity_reset(&s_out_stopped);
    vec_entity_reset(&s_out_held_still);
    vec_facing_reset(&s_out_facings);
    vec_reset_reset(&s_out_resets);
}

/* Stopped held units aren't in the per-tick work; they are turned toward their combat facing. */
static void collect_held_still_units(void)
{
    ASSERT_IN_MAIN_THREAD();

    for(khiter_t k = kh_begin(s_entity_state_table); k != kh_end(s_entity_state_table); k++) {

        if(!kh_exist(s_entity_state_table, k))
            continue;
        uint32_t uid = kh_key(s_entity_state_table, k);
        struct movestate *ms = &kh_value(s_entity_state_table, k);
        if(!ent_still(ms))
            continue;
        if(!(G_FlagsGet(uid) & ENTITY_FLAG_COMBAT_HELD))
            continue;
        if(!G_EntityExists(uid))
            continue;
        vec_entity_push(&s_out_held_still, uid);
    }
}

static void pivot_held_still_units(void)
{
    ASSERT_IN_MAIN_THREAD();

    for(int i = 0; i < vec_size(&s_out_held_still); i++) {

        uint32_t uid = vec_AT(&s_out_held_still, i);
        struct movestate *ms = movestate_get(uid);
        quat_t next = Move_Flock_TurnToward(Entity_GetRot(uid), movestate_aux_get(uid)->combat_facing,
            SCALED_MAX_TURN_RATE);
        vec_facing_push(&s_out_facings, (struct move_facing){uid, next});
        ms->prev_rot = next;
        ms->next_rot = next;
    }
}

/* The first part of the submit: the flocks and the navigation map are brought
 * up to date. The snapshot taken by the second part reads the map as this
 * part published it.
 */
void Move_TickPrepare(void)
{
    ASSERT_IN_MAIN_THREAD();
    output_reset();
    struct sval keep_right;
    s_keep_right = (Settings_Get("pf.game.road_keep_right", &keep_right) == SS_OKAY)
                && keep_right.as_bool;
    PERF_PUSH("movement::tick prepare");
    uint64_t tick_start = SDL_GetPerformanceCounter();

    uint64_t phase_start = SDL_GetPerformanceCounter();
    N_JoinStaleCarriedAsyncFields();
    s_last_nav_tick_stats.carry_join_us =
        perf_ticks_to_us(SDL_GetPerformanceCounter() - phase_start);

    phase_start = SDL_GetPerformanceCounter();
    move_release_gamestate();
    s_prepare_copy_ticks = SDL_GetPerformanceCounter() - phase_start;

    phase_start = SDL_GetPerformanceCounter();
    disband_empty_flocks();
    update_flock_arrival_fields();
    s_last_nav_tick_stats.flock_us =
        perf_ticks_to_us(SDL_GetPerformanceCounter() - phase_start);

    /* Run the navigation updates synchronous to the movement tick */
    phase_start = SDL_GetPerformanceCounter();
    M_Update(s_map);
    s_last_nav_tick_stats.map_update_us =
        perf_ticks_to_us(SDL_GetPerformanceCounter() - phase_start);

    s_last_nav_tick_stats.main_us +=
        perf_ticks_to_us(SDL_GetPerformanceCounter() - tick_start);
    PERF_POP();
}

/* The read side of the submit, one slot per unit in table order, gathered in
 * parallel; the side effects follow in the same order on the main thread. */
struct submit_gather{
    uint32_t                      uid;
    float                         radius;
    uint32_t                      flags;
    bool                          still;
    bool                          in_formation;
    int                           flock_idx;
    vec2_t                        cell_pos;
    vec2_t                        cell_arrival_vdes;
    struct formation_submit_state fss;
};

static void submit_gather_range(int begin, int end, void *arg)
{
    struct submit_gather *gather = arg;
    for(int i = begin; i <= end; i++) {

        struct submit_gather *sg = &gather[i];
        uint32_t curr = sg->uid;
        sg->radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, curr);
        sg->flags = G_FlagsGetFrom(s_move_work.gamestate.flags, curr);
        sg->flock_idx = -1;
        sg->in_formation = false;

        const struct movestate *ms = movestate_get(curr);
        sg->still = ent_still(ms);
        if(sg->still)
            continue;

        /* A seeker answers to the fight; its order waits until it is over. */
        if(ms->state == STATE_SEEK_ENEMIES)
            continue;

        khiter_t fit = kh_get(findex, s_flock_index, curr);
        if(fit != kh_end(s_flock_index))
            sg->flock_idx = kh_val(s_flock_index, fit);
        sg->in_formation = Move_Formation_SubmitStateGather(curr, &sg->fss);
    }
}

void Move_TickSubmit(const struct move_world *world, enum movement_hz hz)
{
    ASSERT_IN_MAIN_THREAD();
    PERF_PUSH("movement::tick submit");
    output_reset();

    uint64_t tick_start = SDL_GetPerformanceCounter();
    uint64_t copy_ticks = s_prepare_copy_ticks;

    uint64_t phase_start = SDL_GetPerformanceCounter();
    move_prepare_work(hz);
    move_take_world(world);
    snapshot_step_ends();
    copy_ticks += SDL_GetPerformanceCounter() - phase_start;
    s_last_nav_tick_stats.copy_gs_us = perf_ticks_to_us(copy_ticks);

    phase_start = SDL_GetPerformanceCounter();
    move_build_flock_snaps();
    s_last_nav_tick_stats.snaps_us =
        perf_ticks_to_us(SDL_GetPerformanceCounter() - phase_start);

    /* Settling stamps through the gamestate copies, so this waits for them. */
    settle_members(hz);
    move_log_census(hz);

    PERF_PUSH("submit move work");
    phase_start = SDL_GetPerformanceCounter();
    uint32_t debug_uid = Move_ClearPath_DebugUid();
    s_last_nav_tick_stats.nstate_moving = 0;
    s_last_nav_tick_stats.nstate_arrived = 0;
    s_last_nav_tick_stats.nstate_seek = 0;
    s_last_nav_tick_stats.nstate_waiting = 0;
    s_last_nav_tick_stats.nstate_turning = 0;
    size_t nunits = 0;
    struct submit_gather *gather = stalloc(&s_move_work.mem,
        kh_size(s_entity_state_table) * sizeof(struct submit_gather));
    for(khiter_t it = kh_begin(s_entity_state_table); it != kh_end(s_entity_state_table); it++) {
        if(!kh_exist(s_entity_state_table, it))
            continue;
        gather[nunits++] = (struct submit_gather){ .uid = kh_key(s_entity_state_table, it) };
    }

    /* Arrival fields are built on the main thread before the gather reads them. */
    for(size_t i = 0; i < nunits; i++) {
        struct submit_gather *sg = &gather[i];
        const struct movestate *ms = movestate_get(sg->uid);
        if(ms->state != STATE_ARRIVING_TO_CELL || ent_still(ms))
            continue;
        sg->cell_pos = Move_Formation_CellPosition(sg->uid);
        if(!Move_Formation_CanUseArrivalField(sg->uid)) {
            sg->cell_arrival_vdes = Move_Formation_ApproximateDesiredArrivalVelocity(sg->uid);
        }else{
            Move_Formation_UpdateFieldIfNeeded(sg->uid);
            sg->cell_arrival_vdes = Move_Formation_DesiredArrivalVelocity(sg->uid);
        }
    }

    s_submit_gather_active = true;
    Move_Formation_SetGatherActive(true);
    Sched_ParallelFor(submit_gather_range, gather, nunits, 128);
    Move_Formation_SetGatherActive(false);
    s_submit_gather_active = false;

    for(size_t i = 0; i < nunits; i++) {

        const struct submit_gather *sg = &gather[i];
        uint32_t curr = sg->uid;
        struct movestate *ms = movestate_get(curr);

        switch(ms->state) {
        case STATE_ARRIVED:      s_last_nav_tick_stats.nstate_arrived++; break;
        case STATE_SEEK_ENEMIES: s_last_nav_tick_stats.nstate_seek++;    break;
        case STATE_WAITING:      s_last_nav_tick_stats.nstate_waiting++; break;
        case STATE_TURNING:      s_last_nav_tick_stats.nstate_turning++; break;
        default:                 s_last_nav_tick_stats.nstate_moving++;  break;
        }

        /* Classify every movable, still ones included: a still giant is a
         * static neighbour the crowd must still avoid. */
        float radius = sg->radius;
        uint32_t curr_flags = sg->flags;
        int curr_class = (curr_flags & ENTITY_FLAG_AIR) ? 1 : 0;
        if(radius > LARGE_MOVABLE_RADIUS && s_move_work.nlarge < MAX_LARGE_MOVABLES) {
            s_move_work.large_movables[s_move_work.nlarge++] = (struct large_movable){
                .xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, curr),
                .radius = radius,
                .flags = curr_flags,
                .uid = curr
            };
        }else{
            s_move_work.small_max_radius[curr_class] =
                MAX(s_move_work.small_max_radius[curr_class], radius);
            s_move_work.small_max_step[curr_class] = MAX(s_move_work.small_max_step[curr_class],
                movestate_get(curr)->max_speed / hz_count(s_move_work.hz));
        }

        if(sg->still) {
            struct movestate_aux *saux = movestate_aux_get(curr);
            saux->parked = false;
            saux->phasing = false;
            continue;
        }

        struct flock *flock = NULL;
        const struct flock_snap *fsnap = NULL;
        if(sg->flock_idx >= 0) {
            flock = &vec_AT(&s_flocks, sg->flock_idx);
            fsnap = &s_move_work.flock_snaps[sg->flock_idx];
        }

        vec2_t pos = (vec2_t){ms->prev_pos.x, ms->prev_pos.z};

        struct cp_ent curr_cp = (struct cp_ent) {
            .xz_pos = pos,
            .xz_vel = ms->velocity,
            .radius = radius
        };

        vec2_t cell_pos = sg->cell_pos;
        vec2_t cell_arrival_vdes = sg->cell_arrival_vdes;

        const struct formation_submit_state fss = sg->fss;
        bool in_formation = sg->in_formation;

        struct movestate_aux *caux = movestate_aux_get(curr);
        /* A unit whose cell assignment has not landed yet has no cell to have
         * arrived at, and reads as arrived. Parking it there would flip the
         * whole formation to its idle clip for the width of that window.
         */
        bool parked = in_formation && fss.assigned_to_cell
                   && fss.arrived_at_cell && !fss.may_park;
        caux->parked = parked;

        bool straggler = in_formation && fss.straggler;
        if(!straggler)
            caux->phase_ticks = 0;
        else if(caux->phase_ticks < UINT16_MAX)
            caux->phase_ticks++;
        /* The pass is bounded; a straggler that has spent it is judged like
         * every other member (settle_members).
         */
        bool pass = straggler && (caux->phase_ticks <= PHASE_MAX_TICKS);
        if(caux->wedge_ticks > 0) {
            caux->wedge_ticks--;
        }else if(caux->veto_ticks >= WEDGE_VETO_S * hz_count(s_move_work.hz)) {
            caux->wedge_ticks = WEDGE_PASS_TICKS;
            caux->veto_ticks = 0;
        }
        pass = pass || caux->wedge_ticks > 0;
        /* The last few units onto a cell cross the clearance rings of the
         * parked neighbours; the lattice keeps the cell itself body-clear, so
         * the rings are transit margin here, not bodies.
         */
        vec2_t to_cell;
        PFM_Vec2_Sub(&cell_pos, &curr_cp.xz_pos, &to_cell);
        bool closing = (ms->state == STATE_ARRIVING_TO_CELL) && fss.assigned_to_cell
                    && !parked && (PFM_Vec2_Len(&to_cell) < cell_closing_radius(radius));
        caux->phasing = pass || closing;

        /* Both notifications are level-triggered and idempotent through the
         * motion_stopped latch, so the pair is self-healing: whichever way the
         * unit leaves the parked condition, the very next tick puts the clip
         * back in step. A unit on its way to settling is left alone, since
         * entity_finish_moving announces its own end.
         */
        bool standing = caux->standing_ticks >= MOTION_IDLE_S * hz_count(hz);
        if(parked || standing) {
            move_notify_motion_end(curr);
        }else if(caux->motion_stopped && !caux->soft_blocking
              && !(in_formation && fss.arrived_at_cell && fss.may_park)) {
            move_notify_motion_start(curr, ms);
        }

        float max_speed = ms->max_speed;
        if(!in_formation && flock && N_DestUsesRoads(flock->dest_id)) {
            max_speed *= G_Roads_SpeedFactor(curr_cp.xz_pos);
        }

        move_push_work((struct move_work_in){
            .ent_uid = curr,
            .flock = flock,
            .fsnap = fsnap,
            .speed = in_formation ? fss.speed : max_speed,
            .max_speed = max_speed,
            .cell_pos = cell_pos,
            .cp_ent = curr_cp,
            .cp_side = movestate_aux_get(curr)->cp_side,
            .save_debug = (curr == debug_uid),
            .fstate.fid = fss.fid,
            .fstate.assignment_ready = fss.assignment_ready,
            .fstate.assigned_to_cell = fss.assigned_to_cell,
            .fstate.in_range_of_cell = fss.in_range_of_cell,
            .fstate.arrived_at_cell = fss.arrived_at_cell,
            .fstate.may_park = fss.may_park,
            .fstate.straggler = fss.straggler,
            .fstate.normal_cohesion_force = fss.cohesion_force,
            .fstate.normal_align_force = fss.alignment_force,
            .fstate.normal_drag_force = fss.drag_force,
            .fstate.target_orientation = fss.target_orientation,
            .cell_arrival_vdes = cell_arrival_vdes
        });
    }
    s_last_nav_tick_stats.submit_us =
        perf_ticks_to_us(SDL_GetPerformanceCounter() - phase_start);
    PERF_POP();

    nav_tick_submit_work();

    s_last_nav_tick_stats.main_us +=
        perf_ticks_to_us(SDL_GetPerformanceCounter() - tick_start);
    PERF_POP();
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
uint32_t Move_GetNavTID(void)
{
    return s_nav_task_active_tid;
}

bool Move_NavQuiesce(void)
{
    return nav_tick_finish_work(false) == WORK_COMPLETE;
}

bool Move_TickFinished(void)
{
    return nav_tick_finish_work(false) == WORK_COMPLETE;
}

bool Move_TickConsume(void)
{
    ASSERT_IN_MAIN_THREAD();
    s_ticks_done++;

    struct sval cap_setting;
    s_log_cp_captures = (Settings_Get("pf.debug.log_cp_captures", &cap_setting) == SS_OKAY)
                     && cap_setting.as_bool;
    struct sval csv_setting;
    s_log_perf_csv = (Settings_Get("pf.debug.log_perf_csv", &csv_setting) == SS_OKAY)
                  && csv_setting.as_bool;
    SDL_AtomicSet(&s_cp_ncaptures, 0);

    struct sval trace_setting;
    s_move_trace_min_radius =
        (Settings_Get("pf.debug.log_move_trace_min_radius", &trace_setting) == SS_OKAY)
        ? trace_setting.as_float : -1.0f;
    s_move_trace_uid =
        (Settings_Get("pf.debug.log_move_trace_uid", &trace_setting) == SS_OKAY)
        ? trace_setting.as_int : -1;

    struct sval exec_setting;
    s_move_parallel_executor =
        (Settings_Get("pf.debug.move_parallel_executor", &exec_setting) == SS_OKAY)
        && exec_setting.as_bool;

    struct sval parity_setting;
    s_gpu_parity = (Settings_Get("pf.debug.gpu_move_parity", &parity_setting) == SS_OKAY)
                && parity_setting.as_bool;

    struct sval carry_setting;
    N_SetAsyncFieldCarry((Settings_Get("pf.debug.nav_field_carry", &carry_setting) == SS_OKAY)
                      && carry_setting.as_bool);
    enum selection_type seltype;
    s_move_trace_sel = G_Sel_Get(&seltype);

    output_reset();
    return move_consume_work_results();
}

void Move_TickResume(void)
{
    ASSERT_IN_MAIN_THREAD();
    output_reset();
    resume_waiting_units();
    collect_held_still_units();
}

void Move_TickPivot(void)
{
    ASSERT_IN_MAIN_THREAD();
    vec_entity_reset(&s_out_stopped);
    pivot_held_still_units();
}

void Move_TickCommands(void)
{
    ASSERT_IN_MAIN_THREAD();
    output_reset();
    move_process_cmds();
    settle_own_moves();
}

void Move_Output(struct move_output *out)
{
    ASSERT_IN_MAIN_THREAD();
    *out = (struct move_output){
        .steps = s_out_steps.array,
        .nsteps = vec_size(&s_out_steps),
        .stopped = s_out_stopped.array,
        .nstopped = vec_size(&s_out_stopped),
        .held_still = s_out_held_still.array,
        .nheld_still = vec_size(&s_out_held_still),
        .facings = s_out_facings.array,
        .nfacings = vec_size(&s_out_facings),
        .resets = s_out_resets.array,
        .nresets = vec_size(&s_out_resets),
    };
}

void Move_StepsBegun(const struct move_step *steps, size_t nsteps)
{
    ASSERT_IN_MAIN_THREAD();
    assert(steps == s_out_steps.array && nsteps == vec_size(&s_out_steps));

    for(size_t i = 0; i < nsteps; i++) {
        vec_AT(&s_out_step_states, i)->left = steps[i].owed ? 1 : 0;
    }
}

bool Move_StepDone(uint32_t uid)
{
    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return false;
    ms->left = 0;
    return true;
}

bool Move_Tracks(uint32_t uid)
{
    return (movestate_get(uid) != NULL);
}

void Move_FrameStart(void)
{
    stalloc_clear(&s_eventargs);
}

struct nav_tick_sample *Move_TickStats(void)
{
    return &s_last_nav_tick_stats;
}

int Move_HzCount(enum movement_hz hz)
{
    return hz_count(hz);
}

bool Move_Init(const struct map *map, const struct move_world *world)
{
    assert(map);
    Move_Flock_Init();
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

    memset(&s_move_work, 0, sizeof(s_move_work));
    if(!stalloc_init(&s_move_work.mem)) {
        kh_destroy(findex, s_flock_index);
        kh_destroy(auxstate, s_entity_aux_table);
        kh_destroy(state, s_entity_state_table);
        return NULL;
    }

    if(!queue_cmd_init(&s_move_commands, 256)) {
        stalloc_destroy(&s_move_work.mem);
        kh_destroy(findex, s_flock_index);
        kh_destroy(auxstate, s_entity_aux_table);
        kh_destroy(state, s_entity_state_table);
        return NULL;
    }

    if(!stalloc_init(&s_eventargs)) {
        stalloc_destroy(&s_move_work.mem);
        kh_destroy(findex, s_flock_index);
        kh_destroy(auxstate, s_entity_aux_table);
        kh_destroy(state, s_entity_state_table);
        queue_cmd_destroy(&s_move_commands);
        return NULL;
    }

    if(NULL == (s_block_refs = kh_init(blockref))) {
        stalloc_destroy(&s_eventargs);
        stalloc_destroy(&s_move_work.mem);
        kh_destroy(findex, s_flock_index);
        kh_destroy(auxstate, s_entity_aux_table);
        kh_destroy(state, s_entity_state_table);
        queue_cmd_destroy(&s_move_commands);
        return NULL;
    }

    vec_entity_init(&s_own_moves);
    vec_flock_init(&s_flocks);
    vec_step_init(&s_out_steps);
    vec_msptr_init(&s_out_step_states);
    vec_entity_init(&s_out_stopped);
    vec_entity_init(&s_out_held_still);
    vec_facing_init(&s_out_facings);
    vec_reset_init(&s_out_resets);

    N_FC_SetNavTaskTIDProvider(Move_GetNavTID);

    E_Global_Register(EVENT_RENDER_3D_POST, on_render_3d, NULL, 
        G_RUNNING | G_PAUSED_FULL | G_PAUSED_UI_RUNNING);

    s_map = map;
    s_nav_snapshot = NULL;
    move_take_world(world);
    return true;
}

unsigned Move_TraceTick(void)
{
    return s_move_trace_tick;
}

void Move_FinishTick(void)
{
    if(nav_tick_finish_work(true) == WORK_INCOMPLETE) {
        nav_cancel_gpu_work();
    }
}

void Move_Shutdown(void)
{
    Move_FinishTick();
    /* The render thread frees the GPU solve's buffers with the map */
    s_gpu_bufs = (struct gpu_move_bufs){0};
    s_map = NULL;

    E_Global_Unregister(EVENT_RENDER_3D_POST, on_render_3d);

    move_destroy_gamestate();
    vec_flock_destroy(&s_flocks);
    vec_step_destroy(&s_out_steps);
    vec_msptr_destroy(&s_out_step_states);
    vec_entity_destroy(&s_out_stopped);
    vec_entity_destroy(&s_out_held_still);
    vec_facing_destroy(&s_out_facings);
    vec_reset_destroy(&s_out_resets);
    vec_entity_destroy(&s_own_moves);
    stalloc_destroy(&s_eventargs);
    queue_cmd_destroy(&s_move_commands);
    stalloc_destroy(&s_move_work.mem);
    kh_destroy(findex, s_flock_index);
    kh_destroy(auxstate, s_entity_aux_table);
    kh_destroy(state, s_entity_state_table);
    kh_destroy(blockref, s_block_refs);
}

bool Move_HasWork(void)
{
    return (queue_size(s_move_commands) > 0);
}

void Move_FlushWork(const struct move_world *world)
{
    /* Discard the results of the last
     * movement tick.
     */
    if(nav_tick_finish_work(true) == WORK_INCOMPLETE) {
        nav_cancel_gpu_work();
    }
    /* The first part of a split submit released the snapshot the drain below
     * reads.
     */
    if(world) {
        move_take_world(world);
    }

    stalloc_clear(&s_move_work.mem);
    s_move_work.in = NULL;
    s_move_work.out = NULL;
    s_move_work.nwork = 0;
    s_move_work.ntasks = 0;

    output_reset();
    move_process_cmds();
}

void Move_AddEntity(uint32_t uid, vec3_t pos, float sel_radius, int faction_id)
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

void Move_RemoveEntity(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_REMOVE,
        .uid = uid
    });
}

void Move_Stop(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_STOP,
        .uid = uid
    });
}

void Move_StopForCombat(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_STOP,
        .uid = uid,
        .u.stop.for_combat = true
    });
}

bool Move_GetDest(uint32_t uid, vec2_t *out_xz, bool *out_attack)
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

bool Move_SeekingFiringPosition(uint32_t uid)
{
    /* Also read by the submit gather's workers while the main thread is
     * blocked in that parallel-for. */
    assert(Engine_OwnsWorld() || s_submit_gather_active);

    const struct movestate *ms = movestate_get(uid);
    const struct movestate_aux *aux = movestate_aux_get(uid);
    if(!ms || !aux || !aux->range_latched || aux->attack_reach == 0.0f)
        return false;
    enum move_state state = (ms->state == STATE_WAITING) ? aux->wait_prev : ms->state;
    return state == STATE_MOVING
        || state == STATE_MOVING_IN_FORMATION
        || state == STATE_ARRIVING_TO_CELL;
}

bool Move_Still(uint32_t uid)
{
    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return true;
    return snoop_still(uid);
}

size_t Move_DebugDesc(uint32_t uid, char *out, size_t len)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return pf_snprintf(out, len, "none");

    size_t ret = pf_snprintf(out, len, "state=%s still=%d vel=(%.2f,%.2f) surround=%u surround_field=%d blocking=%d",
        s_state_str[ms->state], (int)snoop_still(uid), ms->velocity.x, ms->velocity.z,
        ms->surround_target_uid, (int)ms->using_surround_field, (int)ms->blocking);

    const struct flock *flock = flock_for_ent(uid);
    if(flock && ret < len) {
        enum nav_layer layer = Entity_NavLayerWithRadius(G_FlagsGet(uid), G_GetSelectionRadius(uid));
        vec2_t pos = G_Pos_GetXZ(uid);
        ret += pf_snprintf(out + ret, len - ret, " dest=(%.1f,%.1f) dest_pathable=%d dest_blocked=%d here_pathable=%d here_blocked=%d",
            flock->target_xz.x, flock->target_xz.z,
            (int)M_NavPositionPathable(s_map, layer, flock->target_xz),
            (int)M_NavPositionBlocked(s_map, layer, flock->target_xz),
            (int)M_NavPositionPathable(s_map, layer, pos), (int)M_NavPositionBlocked(s_map, layer, pos));
    }
    return ret;
}

/* Pause/resume the unit's move animation while it holds position to attack. */
void Move_SetCombatHeld(uint32_t uid, bool held)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_SET_COMBAT_HELD,
        .uid = uid,
        .u.combat_held.held = held
    });
}

void Move_SetCombatFacing(uint32_t uid, quat_t dir)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_SET_COMBAT_FACING,
        .uid = uid,
        .u.combat_facing.dir = dir
    });
}

void Move_SetDest(uint32_t uid, vec2_t dest_xz, bool attack)
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

void Move_SetChangeDirection(uint32_t uid, quat_t target)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_CHANGE_DIRECTION,
        .uid = uid,
        .u.change_direction.target = target
    });
}

void Move_SetEnterRange(uint32_t uid, uint32_t target, float range)
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

void Move_SetSeekEnemies(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_SET_SEEK_ENEMIES,
        .uid = uid
    });
}

bool Move_Arrived(uint32_t uid)
{
    assert(Engine_OwnsWorld() || s_submit_gather_active);
    const struct movestate *ms = movestate_get(uid);
    return ms && ms->state == STATE_ARRIVED;
}

bool Move_ResumeOrder(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    if(!flock_for_ent(uid))
        return false;
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_RESUME_ORDER,
        .uid = uid
    });
    return true;
}

bool Move_Seeking(uint32_t uid)
{
    assert(Engine_OwnsWorld() || s_submit_gather_active);

    const struct movestate *ms = movestate_get(uid);
    if(!ms)
        return false;
    if(ms->state == STATE_SEEK_ENEMIES)
        return true;
    return ms->state == STATE_WAITING
        && movestate_aux_get(uid)->wait_prev == STATE_SEEK_ENEMIES;
}

void Move_SetSeekPin(uint32_t uid, uint32_t target)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_SET_SEEK_PIN,
        .uid = uid,
        .u.seek_pin.target = target
    });
}

void Move_SetSurroundEntity(uint32_t uid, uint32_t target)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_SET_SURROUND_ENTITY,
        .uid = uid,
        .u.surround.target = target
    });
}

void Move_SetFlee(uint32_t uid, vec2_t threat_xz)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_SET_FLEE,
        .uid = uid,
        .u.flee.threat_xz = threat_xz
    });
}

void Move_StopFlee(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_STOP_FLEE,
        .uid = uid
    });
}

bool Move_IsFleeing(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    struct movestate *ms = movestate_get(uid);
    return (ms && ms->state == STATE_FLEEING);
}

void Move_UpdatePos(uint32_t uid, vec2_t pos)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_UPDATE_POS,
        .uid = uid,
        .u.update_pos.pos = pos
    });
}

void Move_Unblock(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_UNBLOCK,
        .uid = uid
    });
}

void Move_BlockAt(uint32_t uid, vec3_t pos)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_BLOCK,
        .uid = uid,
        .u.block.pos = pos
    });
}

void Move_UpdateFactionID(uint32_t uid, int oldfac, int newfac)
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

void Move_UpdateSelectionRadius(uint32_t uid, float sel_radius)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_UPDATE_SELECTION_RADIUS,
        .uid = uid,
        .u.update_radius.radius = sel_radius
    });
}

bool Move_GetMaxSpeed(uint32_t uid, float *out)
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

bool Move_SetMaxSpeed(uint32_t uid, float speed)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_SET_MAX_SPEED,
        .uid = uid,
        .u.max_speed.speed = speed
    });
    return true;
}

bool Move_SetSpeedBonus(uint32_t uid, float bonus)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_SET_SPEED_BONUS,
        .uid = uid,
        .u.speed_bonus.bonus = bonus
    });
    return true;
}

bool Move_GetEffectiveSpeed(uint32_t uid, float *out)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return false;
    struct movestate *ms = &kh_value(s_entity_state_table, k);
    *out = ms->max_speed;
    return true;
}

void Move_ArrangeInFormation(vec_entity_t *ents, vec2_t target, 
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

void Move_AttackInFormation(vec_entity_t *ents, vec2_t target, 
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
            .attack = true,
            .target_orientation = orientation
        }
    });
}

void Move_FilterPathable(const vec_entity_t *ents, vec_entity_t *out)
{
    filter_selection_pathable(ents, out);
}

unsigned long Move_TicksDone(void)
{
    return s_ticks_done;
}

void Move_SetTickHz(enum movement_hz hz)
{
    s_move_hz = hz;
}

int Move_GetTickHz(void)
{
    return hz_count(s_move_hz);
}

void Move_SetUseGPU(bool use)
{
    s_use_gpu = use;
}

bool Move_SaveState(struct SDL_RWops *stream)
{
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

        CHK_TRUE_RET(Move_Arrival_SaveState(stream, &curr_flock->arrival));
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

        CHK_TRUE_RET(Move_Arrival_SaveUnitState(stream, &aux->arrival));
        Sched_TryYield();
    });

    return true;
}

bool Move_LoadState(struct SDL_RWops *stream, const struct move_world *world)
{
    /* Flush the commands submitted during loading */
    move_release_gamestate();
    move_take_world(world);
    output_reset();
    move_process_cmds();

    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const int num_flocks = attr.val.as_int;
    Sched_TryYield();

    assert(vec_size(&s_flocks) == 0);
    for(int i = 0; i < num_flocks; i++) {

        struct flock new_flock = {0};
        new_flock.ents = kh_init(entity);
        CHK_TRUE_RET(new_flock.ents);
        Move_ArrivalGroup_Init(&new_flock.arrival);

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

        CHK_TRUE_JMP(Move_Arrival_LoadState(stream, &new_flock.arrival, s_map), fail_flock);

        vec_flock_push(&s_flocks, new_flock);
        Sched_TryYield();
        continue;

    fail_flock:
        Move_ArrivalGroup_Destroy(&new_flock.arrival);
        kh_destroy(entity, new_flock.ents);
        return false;
    }

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const int num_ents = attr.val.as_int;
    Sched_TryYield();
    for(khiter_t k = kh_begin(s_entity_state_table); k != kh_end(s_entity_state_table); k++) {
        if(!kh_exist(s_entity_state_table, k))
            continue;
        kh_value(s_entity_state_table, k).left = 0;
    }

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

        CHK_TRUE_RET(Move_Arrival_LoadUnitState(stream, &aux->arrival));

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

