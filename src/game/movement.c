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

#include "movement.h"
#include "game_private.h"
#include "combat.h"
#include "clearpath.h"
#include "position.h"
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

#include <assert.h>
#include <SDL.h>


/* For the purpose of movement simulation, all entities have the same mass,
 * meaning they are accelerate the same amount when applied equal forces. */
#define ENTITY_MASS  (1.0f)
#define EPSILON      (1.0f/1024)
#define MAX_FORCE    (0.75f)

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

#define VEL_HIST_LEN (14)
#define MAX_MOVE_TASKS (64)

enum arrival_state{
    /* Entity is moving towards the flock's destination point */
    STATE_MOVING,
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
};

struct movestate{
    enum arrival_state state;
    /* The base movement speed in units of OpenGL coords / second 
     */
    float              max_speed;
    /* The desired velocity returned by the navigation system 
     */
    vec2_t             vdes;
    /* The newly computed velocity (the desired velocity constrained by flocking forces) 
     */
    vec2_t             vnew;
    /* The current velocity 
     */
    vec2_t             velocity;
    /* Flag to track whether the entiy is currently acting as a 
     * navigation blocker, and the last position where it became a blocker. 
     */
    bool               blocking;
    vec2_t             last_stop_pos;
    float              last_stop_radius;
    /* Information for waking up from the 'WAITING' state 
     */
    enum arrival_state wait_prev;
    int                wait_ticks_left;
    /* History of the previous ticks' velocities. Used for velocity smoothing. 
     */
    vec2_t             vel_hist[VEL_HIST_LEN];
    int                vel_hist_idx;
    /* Entity that we're moving towards when in the 'SURROUND_STATIC_ENTITY' state 
     */
    uint32_t           surround_target_uid;
    vec2_t             surround_target_prev;
    vec2_t             surround_nearest_prev;
    /* Flag indicating that we are now using the 'surround' field rather than the 
     * 'target seek' field to get to path to our target. This kicks in once we pass
     * the distance 'low water' threshold and is turned off if we pass the 'high water' 
     * threshold again - this is to prevent 'toggling' at a boundary where we switch 
     * from one field to another. 
     */
    bool               using_surround_field;
    /* Additional state for entities in 'ENTER_ENTITY_RANGE' state 
     */
    vec2_t             target_prev_pos;
    float              target_range;
    /* The target direction for 'turning' entities 
     */
    quat_t             target_dir;
};

struct flock{
    khash_t(entity) *ents;
    vec2_t           target_xz; 
    dest_id_t        dest_id;
};

struct move_work_in{
    uint32_t      ent_uid;
    vec2_t        ent_des_v;
    struct cp_ent cp_ent;
    bool          save_debug;
    vec_cp_ent_t *stat_neighbs;
    vec_cp_ent_t *dyn_neighbs;
    bool          has_dest_los;
};

struct move_work_out{
    uint32_t ent_uid;
    vec2_t   ent_vel;
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
    khash_t(id)      *flags;
    khash_t(pos)     *positions;
    qt_ent_t         *postree;
    khash_t(range)   *sel_radiuses;
    khash_t(id)      *faction_ids;
    const struct map *map;
};

struct move_work{
    struct memstack       mem;
    struct move_gamestate gamestate;
    struct move_work_in  *in;
    struct move_work_out *out;
    size_t                nwork;
    size_t                ntasks;
    uint32_t              tids[MAX_MOVE_TASKS];
    struct future         futures[MAX_MOVE_TASKS];
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
    MOVE_CMD_MAKE_FLOCKS
};

struct move_cmd{
    bool               deleted;
    enum move_cmd_type type;
    struct attr        args[4];
};

KHASH_MAP_INIT_INT(state, struct movestate)

QUEUE_TYPE(cmd, struct move_cmd)
QUEUE_IMPL(static, cmd, struct move_cmd)

VEC_TYPE(flock, struct flock)
VEC_IMPL(static inline, flock, struct flock)

static void move_push_cmd(struct move_cmd cmd);
static void do_set_dest(uint32_t uid, vec2_t dest_xz, bool attack);
static void do_stop(uint32_t uid);
static void do_update_pos(uint32_t uid, vec2_t pos);

/* Parameters controlling steering/flocking behaviours */
#define SEPARATION_FORCE_SCALE          (0.6f)
#define MOVE_ARRIVE_FORCE_SCALE         (0.5f)
#define MOVE_COHESION_FORCE_SCALE       (0.15f)

#define SEPARATION_BUFFER_DIST          (0.0f)
#define COHESION_NEIGHBOUR_RADIUS       (50.0f)
#define ARRIVE_SLOWING_RADIUS           (10.0f)
#define ADJACENCY_SEP_DIST              (5.0f)
#define ALIGN_NEIGHBOUR_RADIUS          (10.0f)
#define SEPARATION_NEIGHB_RADIUS        (30.0f)

#define COLLISION_MAX_SEE_AHEAD         (10.0f)
#define WAIT_TICKS                      (60)
#define MAX_TURN_RATE                   (15.0f) /* degree/tick */

#define SURROUND_LOW_WATER_X            (CHUNK_WIDTH/4.0f)
#define SURROUND_HIGH_WATER_X           (CHUNK_WIDTH/2.0f)
#define SURROUND_LOW_WATER_Z            (CHUNK_HEIGHT/4.0f)
#define SURROUND_HIGH_WATER_Z           (CHUNK_HEIGHT/2.0f)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map       *s_map;
static bool                    s_attack_on_lclick = false;
static bool                    s_move_on_lclick = false;
static bool                    s_click_move_enabled = true;

static vec_entity_t            s_move_markers;
static vec_flock_t             s_flocks;
static khash_t(state)         *s_entity_state_table;

/* Store the most recently issued move command location for debug rendering */
static bool                    s_last_cmd_dest_valid = false;
static dest_id_t               s_last_cmd_dest;

static struct move_work        s_move_work;
static queue_cmd_t             s_move_commands;

static const char *s_state_str[] = {
    [STATE_MOVING]              = STR(STATE_MOVING),
    [STATE_ARRIVED]             = STR(STATE_ARRIVED),
    [STATE_SEEK_ENEMIES]        = STR(STATE_SEEK_ENEMIES),
    [STATE_WAITING]             = STR(STATE_WAITING),
    [STATE_SURROUND_ENTITY]     = STR(STATE_SURROUND_ENTITY),
    [STATE_ENTER_ENTITY_RANGE]  = STR(STATE_ENTER_ENTITY_RANGE),
    [STATE_TURNING]             = STR(STATE_TURNING)
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

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

static void flock_try_remove(struct flock *flock, uint32_t uid)
{
    khiter_t k;
    if((k = kh_get(entity, flock->ents, uid)) != kh_end(flock->ents))
        kh_del(entity, flock->ents, k);
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

static void entity_block(uint32_t uid)
{
    float sel_radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
    vec2_t pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    M_NavBlockersIncref(pos, sel_radius, 
        G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid), s_map);

    struct movestate *ms = movestate_get(uid);
    assert(!ms->blocking);

    ms->blocking = true;
    ms->last_stop_pos = pos;
    ms->last_stop_radius = sel_radius;
}

static void entity_unblock(uint32_t uid)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms->blocking);

    int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
    M_NavBlockersDecref(ms->last_stop_pos, ms->last_stop_radius, faction_id, s_map);
    ms->blocking = false;
}

static bool stationary(uint32_t uid)
{
    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return false;

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

static void entity_finish_moving(uint32_t uid, enum arrival_state newstate)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    assert(!ent_still(ms));
    uint32_t flags = G_FlagsGet(uid);

    E_Entity_Notify(EVENT_MOTION_END, uid, NULL, ES_ENGINE);
    if(flags & ENTITY_FLAG_COMBATABLE
    && (ms->state != STATE_TURNING)
    && G_Combat_GetStance(uid) != COMBAT_STANCE_HOLD_POSITION) {
        G_Combat_SetStance(uid, COMBAT_STANCE_AGGRESSIVE);
    }

    if(newstate == STATE_WAITING) {
        ms->wait_prev = ms->state;
        ms->wait_ticks_left = WAIT_TICKS;
    }

    ms->state = newstate;
    ms->velocity = (vec2_t){0.0f, 0.0f};
    ms->vnew = (vec2_t){0.0f, 0.0f};

    entity_block(uid);
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
        vec2_t xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, curr);

        float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, curr);
        if(!M_NavPositionPathable(s_map, Entity_NavLayerWithRadius(radius), xz_pos))
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
        enum nav_layer layer = Entity_NavLayerWithRadius(radius);
        vec_entity_push(&layer_flocks[layer], curr);
    }
}

static bool make_flock(const vec_entity_t *units, vec2_t target_xz, 
                       enum nav_layer layer, bool attack)
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
        if(stationary(curr_ent))
            continue;
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

        if(ent_still(ms)) {
            entity_unblock(curr_ent); 
            E_Entity_Notify(EVENT_MOTION_START, curr_ent, NULL, ES_ENGINE);
        }

        flock_add(&new_flock, curr_ent);
        ms->state = STATE_MOVING;
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
    
    }else{
        vec_flock_push(&s_flocks, new_flock);
    }

    s_last_cmd_dest_valid = true;
    s_last_cmd_dest = new_flock.dest_id;
    return true;
}

static void make_flocks(const vec_entity_t *sel, vec2_t target_xz, bool attack)
{
    ASSERT_IN_MAIN_THREAD();

    vec_entity_t fsel;
    filter_selection_pathable(sel, &fsel);

    if(vec_size(&fsel) == 0)
        return;

    vec_entity_t layer_flocks[NAV_LAYER_MAX];
    split_into_layers(&fsel, layer_flocks);
    vec_entity_destroy(&fsel);

    for(int i = 0; i < NAV_LAYER_MAX; i++) {
        make_flock(layer_flocks + i, target_xz, i, attack);
        vec_entity_destroy(layer_flocks + i);
    }
}

static size_t adjacent_flock_members(uint32_t uid, const struct flock *flock, 
                                     uint32_t out[])
{
    vec2_t ent_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    size_t ret = 0;
    uint32_t curr;

    kh_foreach_key(flock->ents, curr, {

        if(curr == uid)
            continue;

        vec2_t diff;
        vec2_t curr_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, curr);
        PFM_Vec2_Sub(&ent_xz_pos, &curr_xz_pos, &diff);

        float radius_uid = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
        float radius_curr = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, curr);

        if(PFM_Vec2_Len(&diff) <= radius_uid + radius_curr + ADJACENCY_SEP_DIST) {
            out[ret++] = curr;  
        }
    });
    return ret;
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
    size_t nmoved = 0;

    if(vec_size(sel) == 0 || sel_type != SELECTION_TYPE_PLAYER)
        return;

    for(int i = 0; i < vec_size(sel); i++) {

        uint32_t curr = vec_AT(sel, i);
        uint32_t flags = G_FlagsGet(curr);
        if(!(flags & ENTITY_FLAG_MOVABLE))
            continue;

        G_StopEntity(curr, false);
        E_Entity_Notify(EVENT_MOVE_ISSUED, curr, NULL, ES_ENGINE);
        G_NotifyOrderIssued(curr);
        nmoved++;

        if(flags & ENTITY_FLAG_COMBATABLE) {
            G_Combat_SetStance(curr, 
                attack ? COMBAT_STANCE_AGGRESSIVE : COMBAT_STANCE_NO_ENGAGEMENT);
        }
    }

    if(nmoved) {
        move_marker_add(mouse_coord, attack);
        vec_entity_t *copy = malloc(sizeof(vec_entity_t));
        vec_entity_init(copy);
        vec_entity_copy(copy, (vec_entity_t*)sel);
        move_push_cmd((struct move_cmd){
            .type = MOVE_CMD_MAKE_FLOCKS,
            .args[0] = {
                .type = TYPE_POINTER,
                .val.as_pointer = copy
            },
            .args[1] = {
                .type = TYPE_VEC2,
                .val.as_vec2 = (vec2_t){mouse_coord.x, mouse_coord.z}
            },
            .args[2] = {
                .type = TYPE_BOOL,
                .val.as_bool = attack
            }
        });
    }
}

static void on_render_3d(void *user, void *event)
{
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
    if(setting.as_bool && s_last_cmd_dest_valid) {
        M_NavRenderVisiblePathFlowField(s_map, cam, s_last_cmd_dest);
    }

    status = Settings_Get("pf.debug.show_first_sel_movestate", &setting);
    assert(status == SS_OKAY);

    enum selection_type seltype;
    const vec_entity_t *sel = G_Sel_Get(&seltype);

    if(setting.as_bool && vec_size(sel) > 0) {
    
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
                    int layer = Entity_NavLayerWithRadius(radius); 
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
                break;
            case STATE_SEEK_ENEMIES: {
                float radius = G_GetSelectionRadiusFrom(
                    s_move_work.gamestate.sel_radiuses, ent);
                int layer = Entity_NavLayerWithRadius(radius); 
                int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, ent);
                M_NavRenderVisibleEnemySeekField(s_map, cam, layer, faction_id);
                break;
            }
            default: 
                assert(0);
            }
        }
    }

    status = Settings_Get("pf.debug.show_enemy_seek_fields", &setting);
    assert(status == SS_OKAY);
    if(setting.as_bool) {

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

static void request_async_field(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    if(!ms || ent_still(ms))
        return;

    vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    struct flock *fl = flock_for_ent(uid);

    switch(ms->state) {
    case STATE_SEEK_ENEMIES:  {
        float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
        int layer = Entity_NavLayerWithRadius(radius);
        int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
        return M_NavRequestAsyncEnemySeekField(s_map, layer, pos_xz, faction_id);
    }
    case STATE_SURROUND_ENTITY: {

        if(!G_EntityExists(ms->surround_target_uid))
            return;

        if(ms->using_surround_field) {
            float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
            int layer = Entity_NavLayerWithRadius(radius);
            int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
            return M_NavRequestAsyncSurroundField(s_map, layer, pos_xz, 
                ms->surround_target_uid, faction_id);
        }
        break;
    }
    default:;
        /* No-op */
    }
}

static vec2_t ent_desired_velocity(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    struct flock *fl = flock_for_ent(uid);

    switch(ms->state) {
    case STATE_TURNING:
        return (vec2_t){0.0f, 0.0f};

    case STATE_SEEK_ENEMIES:  {
        float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
        int layer = Entity_NavLayerWithRadius(radius);
        int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
        return M_NavDesiredEnemySeekVelocity(s_map, layer, pos_xz, faction_id);
    }
    case STATE_SURROUND_ENTITY: {

        if(!G_EntityExists(ms->surround_target_uid)) {
            return M_NavDesiredPointSeekVelocity(s_map, fl->dest_id, pos_xz, fl->target_xz);
        }

        vec2_t target_pos_xz = G_Pos_GetXZ(ms->surround_target_uid);
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

        if(ms->using_surround_field) {
            float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
            int layer = Entity_NavLayerWithRadius(radius);
            int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
            return M_NavDesiredSurroundVelocity(s_map, layer, pos_xz, 
                ms->surround_target_uid, faction_id);
        }else{
            return M_NavDesiredPointSeekVelocity(s_map, fl->dest_id, pos_xz, fl->target_xz);
        }
        break;
    }
    default:
        assert(fl);
        return M_NavDesiredPointSeekVelocity(s_map, fl->dest_id, pos_xz, fl->target_xz);
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
    PFM_Vec2_Normal(&desired_velocity, &desired_velocity);
    PFM_Vec2_Scale(&desired_velocity, ms->max_speed / MOVE_TICK_RES, &desired_velocity);

    PFM_Vec2_Sub(&desired_velocity, &ms->velocity, &ret);
    return ret;
}

/* Arrival behaviour is like 'seek' but the entity decelerates and comes to a halt when it is 
 * within a threshold radius of the destination point.
 * 
 * When not within line of sight of the destination, this will steer the entity along the 
 * flow field.
 */
static vec2_t arrive_force_point(uint32_t uid, vec2_t target_xz, bool has_dest_los)
{
    vec2_t ret, desired_velocity;
    vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    float distance;

    struct movestate *ms = movestate_get(uid);
    assert(ms);

    if(has_dest_los) {

        PFM_Vec2_Sub(&target_xz, &pos_xz, &desired_velocity);
        distance = PFM_Vec2_Len(&desired_velocity);
        PFM_Vec2_Normal(&desired_velocity, &desired_velocity);
        PFM_Vec2_Scale(&desired_velocity, ms->max_speed / MOVE_TICK_RES, &desired_velocity);

        if(distance < ARRIVE_SLOWING_RADIUS) {
            PFM_Vec2_Scale(&desired_velocity, distance / ARRIVE_SLOWING_RADIUS, &desired_velocity);
        }

    }else{

        PFM_Vec2_Scale(&ms->vdes, ms->max_speed / MOVE_TICK_RES, &desired_velocity);
    }

    PFM_Vec2_Sub(&desired_velocity, &ms->velocity, &ret);
    vec2_truncate(&ret, MAX_FORCE);
    return ret;
}

static vec2_t arrive_force_enemies(uint32_t uid)
{
    vec2_t ret, desired_velocity;
    vec2_t pos_xz = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    float distance;

    const struct movestate *ms = movestate_get(uid);
    assert(ms);

    PFM_Vec2_Scale((vec2_t*)&ms->vdes, ms->max_speed / MOVE_TICK_RES, &desired_velocity);
    PFM_Vec2_Sub(&desired_velocity, (vec2_t*)&ms->velocity, &ret);
    vec2_truncate(&ret, MAX_FORCE);
    return ret;
}

/* Alignment is a behaviour that causes a particular agent to line up with agents close by.
 */
static vec2_t alignment_force(uint32_t uid, const struct flock *flock)
{
    vec2_t ret = (vec2_t){0.0f};
    size_t neighbour_count = 0;

    uint32_t curr;
    kh_foreach_key(flock->ents, curr, {

        if(curr == uid)
            continue;

        vec2_t diff;
        vec2_t ent_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
        vec2_t curr_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, curr);

        PFM_Vec2_Sub(&curr_xz_pos, &ent_xz_pos, &diff);
        if(PFM_Vec2_Len(&diff) < ALIGN_NEIGHBOUR_RADIUS) {

            struct movestate *ms = movestate_get(uid);
            assert(ms);

            if(PFM_Vec2_Len(&ms->velocity) < EPSILON)
                continue; 

            PFM_Vec2_Add(&ret, &ms->velocity, &ret);
            neighbour_count++;
        }
    });

    if(0 == neighbour_count)
        return (vec2_t){0.0f};

    struct movestate *ms = movestate_get(uid);
    assert(ms);

    PFM_Vec2_Scale(&ret, 1.0f / neighbour_count, &ret);
    PFM_Vec2_Sub(&ret, &ms->velocity, &ret);
    vec2_truncate(&ret, MAX_FORCE);
    return ret;
}

/* Cohesion is a behaviour that causes agents to steer towards the center of mass of nearby agents.
 */
static vec2_t cohesion_force(uint32_t uid, const struct flock *flock)
{
    vec2_t COM = (vec2_t){0.0f};
    size_t neighbour_count = 0;
    vec2_t ent_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);

    uint32_t curr;
    kh_foreach_key(flock->ents, curr, {

        if(curr == uid)
            continue;

        vec2_t diff;
        vec2_t curr_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, curr);
        PFM_Vec2_Sub(&curr_xz_pos, &ent_xz_pos, &diff);

        float t = (PFM_Vec2_Len(&diff) 
                - COHESION_NEIGHBOUR_RADIUS*0.75) / COHESION_NEIGHBOUR_RADIUS;
        float scale = exp(-6.0f * t);

        PFM_Vec2_Scale(&curr_xz_pos, scale, &curr_xz_pos);
        PFM_Vec2_Add(&COM, &curr_xz_pos, &COM);
        neighbour_count++;
    });

    if(0 == neighbour_count)
        return (vec2_t){0.0f};

    vec2_t ret;
    PFM_Vec2_Scale(&COM, 1.0f / neighbour_count, &COM);
    PFM_Vec2_Sub(&COM, &ent_xz_pos, &ret);
    vec2_truncate(&ret, MAX_FORCE);
    return ret;
}

/* Separation is a behaviour that causes agents to steer away from nearby agents.
 */
static vec2_t separation_force(uint32_t uid, float buffer_dist)
{
    vec2_t ret = (vec2_t){0.0f};
    uint32_t near_ents[128];
    int num_near = G_Pos_EntsInCircleFrom(s_move_work.gamestate.postree,
        G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid), 
        SEPARATION_NEIGHB_RADIUS, near_ents, ARR_SIZE(near_ents));

    for(int i = 0; i < num_near; i++) {

        uint32_t curr = near_ents[i];
        uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, curr);
        if(curr == uid)
            continue;
        if(!(flags & ENTITY_FLAG_MOVABLE))
            continue;

        vec2_t diff;
        vec2_t ent_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
        vec2_t curr_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, curr);

        float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid) 
                     + G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, curr) 
                     + buffer_dist;
        PFM_Vec2_Sub(&curr_xz_pos, &ent_xz_pos, &diff);

        /* Exponential decay with y=1 when diff = radius*0.85 
         * Use smooth decay curves in order to curb the 'toggling' or oscillating 
         * behaviour that may arise when there are discontinuities in the forces. 
         */
        float t = (PFM_Vec2_Len(&diff) - radius*0.85) / PFM_Vec2_Len(&diff);
        float scale = exp(-20.0f * t);
        PFM_Vec2_Scale(&diff, scale, &diff);

        PFM_Vec2_Add(&ret, &diff, &ret);
    }

    if(0 == num_near)
        return (vec2_t){0.0f};

    PFM_Vec2_Scale(&ret, -1.0f, &ret);
    vec2_truncate(&ret, MAX_FORCE);
    return ret;
}

static vec2_t point_seek_total_force(uint32_t uid, const struct flock *flock, 
                                     bool has_dest_los)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    vec2_t arrive = arrive_force_point(uid, flock->target_xz, has_dest_los);
    vec2_t cohesion = cohesion_force(uid, flock);
    vec2_t separation = separation_force(uid, SEPARATION_BUFFER_DIST);

    PFM_Vec2_Scale(&arrive,     MOVE_ARRIVE_FORCE_SCALE,   &arrive);
    PFM_Vec2_Scale(&cohesion,   MOVE_COHESION_FORCE_SCALE, &cohesion);
    PFM_Vec2_Scale(&separation, SEPARATION_FORCE_SCALE,    &separation);

    vec2_t ret = (vec2_t){0.0f};
    assert(!ent_still(ms));

    PFM_Vec2_Add(&ret, &arrive, &ret);
    PFM_Vec2_Add(&ret, &separation, &ret);
    PFM_Vec2_Add(&ret, &cohesion, &ret);

    vec2_truncate(&ret, MAX_FORCE);
    return ret;
}

static vec2_t enemy_seek_total_force(uint32_t uid)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    vec2_t arrive = arrive_force_enemies(uid);
    vec2_t separation = separation_force(uid, SEPARATION_BUFFER_DIST);

    PFM_Vec2_Scale(&arrive,     MOVE_ARRIVE_FORCE_SCALE,   &arrive);
    PFM_Vec2_Scale(&separation, SEPARATION_FORCE_SCALE,    &separation);

    vec2_t ret = (vec2_t){0.0f, 0.0f};
    PFM_Vec2_Add(&ret, &arrive, &ret);
    PFM_Vec2_Add(&ret, &separation, &ret);

    vec2_truncate(&ret, MAX_FORCE);
    return ret;
}

static vec2_t new_pos_for_vel(uint32_t uid, vec2_t velocity)
{
    ASSERT_IN_MAIN_THREAD();

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
    enum nav_layer layer = Entity_NavLayerWithRadius(radius);

    vec2_t pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    vec2_t left =  (vec2_t){pos.x + nt_dims.x, pos.z};
    vec2_t right = (vec2_t){pos.x - nt_dims.x, pos.z};
    vec2_t top =   (vec2_t){pos.x, pos.z + nt_dims.z};
    vec2_t bot =   (vec2_t){pos.x, pos.z - nt_dims.z};

    if(inout_force->x > 0 
    && (!M_NavPositionPathable(s_move_work.gamestate.map, layer, left)  
      || M_NavPositionBlocked(s_move_work.gamestate.map, layer, left)))
        inout_force->x = 0.0f;

    if(inout_force->x < 0 
    && (!M_NavPositionPathable(s_move_work.gamestate.map, layer, right) 
     || M_NavPositionBlocked(s_move_work.gamestate.map, layer, right)))
        inout_force->x = 0.0f;

    if(inout_force->z > 0 
    && (!M_NavPositionPathable(s_move_work.gamestate.map, layer, top) 
      || M_NavPositionBlocked(s_move_work.gamestate.map, layer, top)))
        inout_force->z = 0.0f;

    if(inout_force->z < 0 
    && (!M_NavPositionPathable(s_move_work.gamestate.map, layer, bot) 
      || M_NavPositionBlocked(s_move_work.gamestate.map, layer, bot)))
        inout_force->z = 0.0f;
}

static vec2_t point_seek_vpref(uint32_t uid, const struct flock *flock, bool has_dest_los)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    vec2_t steer_force;
    for(int prio = 0; prio < 3; prio++) {

        switch(prio) {
        case 0: 
            steer_force = point_seek_total_force(uid, flock, has_dest_los); 
            break;
        case 1: 
            steer_force = separation_force(uid, SEPARATION_BUFFER_DIST); 
            break;
        case 2: 
            steer_force = arrive_force_point(uid, flock->target_xz, has_dest_los); 
            break;
        }

        nullify_impass_components(uid, &steer_force);
        if(PFM_Vec2_Len(&steer_force) > MAX_FORCE * 0.01)
            break;
    }

    vec2_t accel, new_vel; 
    PFM_Vec2_Scale(&steer_force, 1.0f / ENTITY_MASS, &accel);

    PFM_Vec2_Add(&ms->velocity, &accel, &new_vel);
    vec2_truncate(&new_vel, ms->max_speed / MOVE_TICK_RES);

    return new_vel;
}

static vec2_t enemy_seek_vpref(uint32_t uid)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    vec2_t steer_force = enemy_seek_total_force(uid);

    vec2_t accel, new_vel; 
    PFM_Vec2_Scale(&steer_force, 1.0f / ENTITY_MASS, &accel);

    PFM_Vec2_Add(&ms->velocity, &accel, &new_vel);
    vec2_truncate(&new_vel, ms->max_speed / MOVE_TICK_RES);

    return new_vel;
}

static void update_vel_hist(struct movestate *ms, vec2_t vnew)
{
    ASSERT_IN_MAIN_THREAD();

    assert(ms->vel_hist >= 0 && ms->vel_hist_idx < VEL_HIST_LEN);
    ms->vel_hist[ms->vel_hist_idx] = vnew;
    ms->vel_hist_idx = ((ms->vel_hist_idx+1) % VEL_HIST_LEN);
}

/* Simple Moving Average */
static vec2_t vel_sma(const struct movestate *ms)
{
    ASSERT_IN_MAIN_THREAD();

    vec2_t ret = {0};
    for(int i = 0; i < VEL_HIST_LEN; i++)
        PFM_Vec2_Add(&ret, (vec2_t*)&ms->vel_hist[i], &ret); 
    PFM_Vec2_Scale(&ret, 1.0f/VEL_HIST_LEN, &ret);
    return ret;
}

/* Weighted Moving Average */
static vec2_t vel_wma(const struct movestate *ms)
{
    ASSERT_IN_MAIN_THREAD();

    vec2_t ret = {0};
    float denom = 0.0f;

    for(int i = 0; i < VEL_HIST_LEN; i++) {

        vec2_t term = ms->vel_hist[i];
        PFM_Vec2_Scale(&term, VEL_HIST_LEN-i, &term);
        PFM_Vec2_Add(&ret, &term, &ret);
        denom += (VEL_HIST_LEN-i);
    }

    PFM_Vec2_Scale(&ret, 1.0f/denom, &ret);
    return ret;
}

static bool uids_match(void *arg, struct move_cmd *cmd)
{
    uint32_t desired_uid = (uintptr_t)arg;
    uint32_t actual_uid = cmd->args[0].val.as_int;
    return (desired_uid == actual_uid);
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
                curr->deleted = true;
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

static void flush_update_pos_commands(uint32_t uid)
{
    struct move_cmd *cmd;
    while((cmd = snoop_most_recent_command(MOVE_CMD_UPDATE_POS, 
        (void*)(uintptr_t)uid, uids_match, true))) {

        uint32_t uid = cmd->args[0].val.as_int;
        vec2_t pos = cmd->args[1].val.as_vec2;
        do_update_pos(uid, pos);
    }
}

static void entity_update(uint32_t uid, vec2_t new_vel)
{
    ASSERT_IN_MAIN_THREAD();
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    vec2_t new_pos_xz = new_pos_for_vel(uid, new_vel);
    float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
    enum nav_layer layer = Entity_NavLayerWithRadius(radius);

    if(PFM_Vec2_Len(&new_vel) > 0
    && M_NavPositionPathable(s_map, layer, new_pos_xz)) {
    
        vec3_t new_pos = (vec3_t){new_pos_xz.x, M_HeightAtPoint(s_map, new_pos_xz), new_pos_xz.z};
        G_Pos_Set(uid, new_pos);
        flush_update_pos_commands(uid);
        ms->velocity = new_vel;

        /* Use a weighted average of past velocities ot set the entity's orientation. 
         * This means that the entity's visible orientation lags behind its' true orientation 
         * slightly. However, this greatly smooths the turning of the entity, giving a more 
         * natural look to the movemment. 
         */
        vec2_t wma = vel_wma(ms);
        if(PFM_Vec2_Len(&wma) > EPSILON) {
            Entity_SetRot(uid, dir_quat_from_velocity(wma));
        }
    }else{
        ms->velocity = (vec2_t){0.0f, 0.0f}; 
    }

    /* If the entity's current position isn't pathable, simply keep it 'stuck' there in
     * the same state it was in before. Under normal conditions, no entity can move from 
     * pathable terrain to non-pathable terrain, but an this violation is possible by 
     * forcefully setting the entity's position from a scripting call. 
     */
    if(!M_NavPositionPathable(s_map, layer, G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid)))
        return;

    switch(ms->state) {
    case STATE_MOVING: {

        vec2_t diff_to_target;
        vec2_t xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
        struct flock *flock = flock_for_ent(uid);
        assert(flock);

        PFM_Vec2_Sub((vec2_t*)&flock->target_xz, &xz_pos, &diff_to_target);
        float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
        float arrive_thresh = radius * 1.5f;

        if(PFM_Vec2_Len(&diff_to_target) < arrive_thresh
        || (M_NavIsAdjacentToImpassable(s_map, layer, xz_pos) 
            && M_NavIsMaximallyClose(s_map, layer, xz_pos, flock->target_xz, arrive_thresh))) {

            entity_finish_moving(uid, STATE_ARRIVED);
            break;
        }

        STALLOC(uint32_t, adjacent, kh_size(flock->ents));
        size_t num_adj = adjacent_flock_members(uid, flock, adjacent);

        bool done = false;
        for(int j = 0; j < num_adj; j++) {

            struct movestate *adj_ms = movestate_get(adjacent[j]);
            assert(adj_ms);

            if(adj_ms->state == STATE_ARRIVED) {

                entity_finish_moving(uid, STATE_ARRIVED);
                done = true;
                break;
            }
        }

        STFREE(adjacent);

        if(done) {
            break;
        }

        /* If we've not hit a condition to stop or give up but our desired velocity 
         * is zero, that means the navigation system is currently not able to guide
         * the entity any closer to its' goal. Stop and wait, re-requesting the  path 
         * after some time. 
         */
        if(PFM_Vec2_Len(&ms->vdes) < EPSILON) {

            assert(flock_for_ent(uid));
            entity_finish_moving(uid, STATE_WAITING);
            break;
        }
        break;
    }
    case STATE_SEEK_ENEMIES: {

        if(PFM_Vec2_Len(&ms->vdes) < EPSILON) {

            entity_finish_moving(uid, STATE_WAITING);
        }
        break;
    }
    case STATE_SURROUND_ENTITY: {

        if(ms->surround_target_uid == NULL_UID) {
            entity_finish_moving(uid, STATE_ARRIVED);
            break;
        }

        if(Entity_MaybeAdjacentFast(uid, ms->surround_target_uid, 10.0f) 
        && M_NavObjAdjacent(s_map, uid, ms->surround_target_uid)) {
            entity_finish_moving(uid, STATE_ARRIVED);
            break;
        }

        vec2_t target_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, 
            ms->surround_target_uid);
        vec2_t dest = ms->surround_nearest_prev;

        vec2_t delta;
        PFM_Vec2_Sub(&target_pos, &ms->surround_target_prev, &delta);
        if(PFM_Vec2_Len(&delta) > EPSILON || PFM_Vec2_Len(&ms->velocity) < EPSILON) {

            bool hasdest = M_NavClosestReachableAdjacentPos(s_map, layer, 
                G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid), 
                ms->surround_target_uid, &dest);

            if(!hasdest) {
                entity_finish_moving(uid, STATE_ARRIVED);
                break;
            }
        }

        struct flock *flock = flock_for_ent(uid);
        assert(flock);

        vec2_t diff;
        PFM_Vec2_Sub(&flock->target_xz, &dest, &diff);
        ms->surround_target_prev = target_pos;
        ms->surround_nearest_prev = dest;

        if(PFM_Vec2_Len(&diff) > EPSILON) {
            do_set_dest(uid, dest, false);
            ms->state = STATE_SURROUND_ENTITY;
            break;
        }

        if(PFM_Vec2_Len(&ms->vdes) < EPSILON) {
            entity_finish_moving(uid, STATE_WAITING);
        }
        break;
    }
    case STATE_ENTER_ENTITY_RANGE: {

        if(ms->surround_target_uid == NULL_UID) {
            entity_finish_moving(uid, STATE_ARRIVED);
            break;
        }

        vec2_t xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
        vec2_t xz_target = G_Pos_GetXZFrom(s_move_work.gamestate.positions, 
            ms->surround_target_uid);

        vec2_t delta;
        PFM_Vec2_Sub(&xz_pos, &xz_target, &delta);

        if(PFM_Vec2_Len(&delta) <= ms->target_range
        || (M_NavIsAdjacentToImpassable(s_map, layer, xz_pos) 
            && M_NavIsMaximallyClose(s_map, layer, xz_pos, xz_target, 0.0f))) {
        
            entity_finish_moving(uid, STATE_ARRIVED);
            break;
        }

        vec2_t target_delta;
        PFM_Vec2_Sub(&xz_target, &ms->target_prev_pos, &target_delta);

        if(PFM_Vec2_Len(&target_delta) > 5.0f) {
            do_set_dest(uid, xz_target, false);
            ms->state = STATE_ENTER_ENTITY_RANGE;
            ms->target_prev_pos = xz_target;
        }

        break;
    }
    case STATE_TURNING: {

        /* find the angle between the two quaternions */
        quat_t ent_rot = Entity_GetRot(uid);
        float angle_diff = PFM_Quat_PitchDiff(&ent_rot, &ms->target_dir);
        float degrees = RAD_TO_DEG(angle_diff);

        /* if it's within a tolerance, stop turning */
        if(fabs(degrees) <= 5.0f) {
            entity_finish_moving(uid, STATE_ARRIVED);
            break;
        }

        /* If not, find the amount we should turn by around the Y axis */
        float turn_deg = MIN(MAX_TURN_RATE, fabs(degrees)) * -SIGNUM(degrees);
        float turn_rad = DEG_TO_RAD(turn_deg);
        mat4x4_t rotmat;
        PFM_Mat4x4_MakeRotY(turn_rad, &rotmat);
        quat_t rot;
        PFM_Quat_FromRotMat(&rotmat, &rot);

        /* Turn */
        quat_t final;
        PFM_Quat_MultQuat(&rot, &ent_rot, &final);
        PFM_Quat_Normal(&final, &final);
        Entity_SetRot(uid, final);

        break;
    }
    case STATE_WAITING: {

        assert(ms->wait_ticks_left > 0);
        ms->wait_ticks_left--;
        if(ms->wait_ticks_left == 0) {

            assert(ms->wait_prev == STATE_MOVING 
                || ms->wait_prev == STATE_SEEK_ENEMIES
                || ms->wait_prev == STATE_SURROUND_ENTITY);

            entity_unblock(uid);
            E_Entity_Notify(EVENT_MOTION_START, uid, NULL, ES_ENGINE);
            ms->state = ms->wait_prev;
        }
        break;
    }
    case STATE_ARRIVED:
        break;
    default: 
        assert(0);
    }
}

static void find_neighbours(uint32_t uid,
                            vec_cp_ent_t *out_dyn,
                            vec_cp_ent_t *out_stat)
{
    /* For the ClearPath algorithm, we only consider entities with
     * ENTITY_FLAG_MOVABLE set, as they are the only ones that may need
     * to be avoided during moving. Here, 'static' entites refer
     * to those entites that are not currently in a 'moving' state,
     * meaning they will not perform collision avoidance maneuvers of
     * their own. */

    uint32_t near_ents[512];
    int num_near = G_Pos_EntsInCircleFrom(s_move_work.gamestate.postree, 
        G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid), 
        CLEARPATH_NEIGHBOUR_RADIUS, near_ents, ARR_SIZE(near_ents));

    for(int i = 0; i < num_near; i++) {

        uint32_t curr = near_ents[i];
        uint32_t flags = G_FlagsGetFrom(s_move_work.gamestate.flags, curr);

        if(curr == uid)
            continue;

        if(!(flags & ENTITY_FLAG_MOVABLE))
            continue;

        if(G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, curr) == 0.0f)
            continue;

        struct movestate *ms = movestate_get(curr);
        assert(ms);

        vec2_t curr_xz_pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, curr);
        struct cp_ent newdesc = (struct cp_ent) {
            .xz_pos = curr_xz_pos,
            .xz_vel = ms->velocity,
            .radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, curr)
        };

        if(ent_still(ms))
            vec_cp_ent_push(out_stat, newdesc);
        else
            vec_cp_ent_push(out_dyn, newdesc);
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

            kh_destroy(entity, vec_AT(&s_flocks, i).ents);
            vec_flock_del(&s_flocks, i);
        }
    }
    PERF_RETURN_VOID();
}

static void do_add_entity(uint32_t uid, vec3_t pos, float selection_radius, int faction_id)
{
    ASSERT_IN_MAIN_THREAD();

    int ret;
    khiter_t k = kh_put(pos, s_move_work.gamestate.positions, uid, &ret);
    assert(ret != -1);
    kh_val(s_move_work.gamestate.positions, k) = pos;

    qt_ent_insert(s_move_work.gamestate.postree, pos.x, pos.z, uid);

    k = kh_put(range, s_move_work.gamestate.sel_radiuses, uid, &ret);
    assert(ret != -1);
    kh_value(s_move_work.gamestate.sel_radiuses, k) = selection_radius;

    k = kh_put(id, s_move_work.gamestate.faction_ids, uid, &ret);
    assert(ret != -1);
    kh_value(s_move_work.gamestate.faction_ids, k) = faction_id;

    struct movestate new_ms = (struct movestate) {
        .velocity = {0.0f}, 
        .blocking = false,
        .state = STATE_ARRIVED,
        .vel_hist_idx = 0,
        .vnew = (vec2_t){0.0f, 0.0f},
        .max_speed = 0.0f,
        .surround_target_prev = (vec2_t){0},
        .surround_nearest_prev = (vec2_t){0},
    };
    memset(new_ms.vel_hist, 0, sizeof(new_ms.vel_hist));

    k = kh_put(state, s_entity_state_table, uid, &ret);
    assert(ret != -1 && ret != 0);
    kh_value(s_entity_state_table, k) = new_ms;

    entity_block(uid);
}

static void do_remove_entity(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return;

    do_stop(uid);
    entity_unblock(uid);

    kh_del(state, s_entity_state_table, k);
}

static void do_stop(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return;

    if(!ent_still(ms)) {
        entity_finish_moving(uid, STATE_ARRIVED);
    }

    remove_from_flocks(uid);
    ms->state = STATE_ARRIVED;
}

static void do_set_dest(uint32_t uid, vec2_t dest_xz, bool attack)
{
    ASSERT_IN_MAIN_THREAD();

    float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
    enum nav_layer layer = Entity_NavLayerWithRadius(radius);
    vec2_t pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
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

    if(fl && fl == flock_for_ent(uid))
        return;

    if(fl) {

        assert(fl != flock_for_ent(uid));
        remove_from_flocks(uid);
        flock_add(fl, uid);

        struct movestate *ms = movestate_get(uid);
        assert(ms);
        if(ent_still(ms)) {
            entity_unblock(uid);
            E_Entity_Notify(EVENT_MOTION_START, uid, NULL, ES_ENGINE);
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

    make_flock(&flock, dest_xz, layer, attack);
    vec_entity_destroy(&flock);
}

static void do_set_change_direction(uint32_t uid, quat_t target)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    assert(ms);

    if(ent_still(ms)) {
        entity_unblock(uid);
        E_Entity_Notify(EVENT_MOTION_START, uid, NULL, ES_ENGINE);
    }

    ms->state = STATE_TURNING;
    ms->target_dir = target;
}

static void do_set_enter_range(uint32_t uid, uint32_t target, float range)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    assert(ms);

    vec2_t xz_src = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
    vec2_t xz_dst = G_Pos_GetXZFrom(s_move_work.gamestate.positions, target);
    float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, uid);
    range = MAX(0.0f, range - radius);

    vec2_t delta;
    PFM_Vec2_Sub(&xz_src, &xz_dst, &delta);
    if(PFM_Vec2_Len(&delta) <= range) {
        do_stop(uid);
        return;
    }

    vec2_t xz_target = M_NavClosestReachableInRange(s_map, 
        Entity_NavLayerWithRadius(radius), xz_src, xz_dst, range);
    do_set_dest(uid, xz_target, false);

    ms->state = STATE_ENTER_ENTITY_RANGE;
    ms->surround_target_uid = target;
    ms->target_prev_pos = xz_dst;
    ms->target_range = range;
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
    assert(ms);

    do_stop(uid);

    vec2_t pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, target);
    do_set_dest(uid, pos, false);

    assert(!ms->blocking);
    ms->state = STATE_SURROUND_ENTITY;
    ms->surround_target_uid = target;
    ms->using_surround_field = using_surround_field(uid, target);
}

static void do_set_seek_enemies(uint32_t uid)
{
    struct movestate *ms = movestate_get(uid);
    assert(ms);

    remove_from_flocks(uid);

    if(ent_still(ms)) {
        entity_unblock(uid);
        E_Entity_Notify(EVENT_MOTION_START, uid, NULL, ES_ENGINE);
    }

    ms->state = STATE_SEEK_ENEMIES;
}

static void do_update_pos(uint32_t uid, vec2_t pos)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return;

    vec3_t newpos = {
        pos.x,
        M_HeightAtPoint(s_map, pos),
        pos.z
    };

    khiter_t k = kh_get(pos, s_move_work.gamestate.positions, uid);
    assert(k != kh_end(s_move_work.gamestate.positions));
    vec3_t oldpos = kh_val(s_move_work.gamestate.positions, k);
    qt_ent_delete(s_move_work.gamestate.postree, oldpos.x, oldpos.z, uid);
    qt_ent_insert(s_move_work.gamestate.postree, newpos.x, newpos.z, uid);
    kh_val(s_move_work.gamestate.positions, k) = newpos;

    if(!ms->blocking)
        return;

    int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
    M_NavBlockersDecref(ms->last_stop_pos, ms->last_stop_radius, faction_id, s_map);
    M_NavBlockersIncref(pos, ms->last_stop_radius, faction_id, s_map);
    ms->last_stop_pos = pos;
}

static void do_update_faction_id(uint32_t uid, int oldfac, int newfac)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return;

    khiter_t k = kh_get(id, s_move_work.gamestate.faction_ids, uid);
    assert(k != kh_end(s_move_work.gamestate.faction_ids));
    kh_val(s_move_work.gamestate.faction_ids, k) = newfac;

    if(!ms->blocking)
        return;

    M_NavBlockersDecref(ms->last_stop_pos, ms->last_stop_radius, oldfac, s_map);
    M_NavBlockersIncref(ms->last_stop_pos, ms->last_stop_radius, newfac, s_map);
}

static void do_update_selection_radius(uint32_t uid, float sel_radius)
{
    ASSERT_IN_MAIN_THREAD();

    struct movestate *ms = movestate_get(uid);
    if(!ms)
        return;

    khiter_t k = kh_get(range, s_move_work.gamestate.sel_radiuses, uid);
    assert(k != kh_end(s_move_work.gamestate.sel_radiuses));
    kh_val(s_move_work.gamestate.sel_radiuses, k) = sel_radius;

    if(!ms->blocking)
        return;

    int faction_id = G_GetFactionIDFrom(s_move_work.gamestate.faction_ids, uid);
    M_NavBlockersDecref(ms->last_stop_pos, ms->last_stop_radius, faction_id, s_map);
    M_NavBlockersIncref(ms->last_stop_pos, sel_radius, faction_id, s_map);
    ms->last_stop_radius = sel_radius;
}

static void do_set_max_speed(uint32_t uid, float speed)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return;
    struct movestate *ms = &kh_value(s_entity_state_table, k);
    ms->max_speed = speed;
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

        switch(cmd.type) {
        case MOVE_CMD_ADD: {
            uint32_t uid = cmd.args[0].val.as_int;
            vec3_t pos = cmd.args[1].val.as_vec3;
            float radius = cmd.args[2].val.as_float;
            int faction_id = cmd.args[3].val.as_int;
            do_add_entity(uid, pos, radius, faction_id);
            break;
        }
        case MOVE_CMD_REMOVE: {
            uint32_t uid = cmd.args[0].val.as_int;
            do_remove_entity(uid);
            break;
        }
        case MOVE_CMD_STOP: {
            uint32_t uid = cmd.args[0].val.as_int;
            do_stop(uid);
            break;
        }
        case MOVE_CMD_SET_DEST: {
            uint32_t uid = cmd.args[0].val.as_int;
            vec2_t dest_xz = cmd.args[1].val.as_vec2;
            bool attack = cmd.args[2].val.as_bool;
            do_set_dest(uid, dest_xz, attack);
            break;
        }
        case MOVE_CMD_CHANGE_DIRECTION: {
            uint32_t uid = cmd.args[0].val.as_int;
            quat_t target = cmd.args[1].val.as_quat;
            do_set_change_direction(uid, target);
            break;
        }
        case MOVE_CMD_SET_ENTER_RANGE: {
            uint32_t uid = cmd.args[0].val.as_int;
            uint32_t target = cmd.args[1].val.as_int;
            float range = cmd.args[2].val.as_float;
            do_set_enter_range(uid, target, range);
            break;
        }
        case MOVE_CMD_SET_SEEK_ENEMIES: {
            uint32_t uid = cmd.args[0].val.as_int;
            do_set_seek_enemies(uid);
            break;
        }
        case MOVE_CMD_SET_SURROUND_ENTITY: {
            uint32_t uid = cmd.args[0].val.as_int;
            uint32_t target = cmd.args[1].val.as_int;
            do_set_surround_entity(uid, target);
            break;
        }
        case MOVE_CMD_UPDATE_POS: {
            uint32_t uid = cmd.args[0].val.as_int;
            vec2_t pos = cmd.args[1].val.as_vec2;
            do_update_pos(uid, pos);
            break;
        }
        case MOVE_CMD_UPDATE_FACTION_ID: {
            uint32_t uid = cmd.args[0].val.as_int;
            int oldfac = cmd.args[1].val.as_int;
            int newfac = cmd.args[2].val.as_int;
            do_update_faction_id(uid, oldfac, newfac);
            break;
        }
        case MOVE_CMD_UPDATE_SELECTION_RADIUS: {
            uint32_t uid = cmd.args[0].val.as_int;
            float radius = cmd.args[1].val.as_float;
            do_update_selection_radius(uid, radius);
            break;
        }
        case MOVE_CMD_SET_MAX_SPEED: {
            uint32_t uid = cmd.args[0].val.as_int;
            float speed = cmd.args[1].val.as_float;
            do_set_max_speed(uid, speed);
            break;
        }
        case MOVE_CMD_MAKE_FLOCKS: {
            vec_entity_t *sel = (vec_entity_t*)cmd.args[0].val.as_pointer;
            vec2_t target_xz = cmd.args[1].val.as_vec2;
            bool attack = cmd.args[2].val.as_bool;
            make_flocks(sel, target_xz, attack);
            vec_entity_destroy(sel);
            PF_FREE(sel);
            break;
        }
        default:
            assert(0);
        }
    }
}

static void *cp_vec_realloc(void *ptr, size_t size)
{
    if(!ptr)
        return stalloc(&s_move_work.mem, size);

    void *ret = stalloc(&s_move_work.mem, size);
    if(!ret)
        return NULL;

    assert(size % 2 == 0);
    memcpy(ret, ptr, size / 2);
    return ret;
}

static void cp_vec_PF_FREE(void *ptr)
{
    /* no-op */
}

static void move_work(int begin_idx, int end_idx)
{
    for(int i = begin_idx; i <= end_idx; i++) {
    
        struct move_work_in *in = &s_move_work.in[i];
        struct move_work_out *out = &s_move_work.out[i];

        const struct movestate *ms = movestate_get(in->ent_uid);
        const struct flock *flock = flock_for_ent(in->ent_uid);

        /* Compute the preferred velocity */
        vec2_t vpref = (vec2_t){NAN, NAN};
        enum arrival_state old_state = ms->state;
        switch(ms->state) {
        case STATE_TURNING:
            vpref = (vec2_t){0.0f, 0.0f};
            break;
        case STATE_SEEK_ENEMIES: 
            assert(!flock);
            vpref = enemy_seek_vpref(in->ent_uid);
            break;
        default:
            assert(flock);
            vpref = point_seek_vpref(in->ent_uid, flock, in->has_dest_los);
        }
        assert(vpref.x != NAN && vpref.z != NAN);

        /* Find the entity's neighbours */
        find_neighbours(in->ent_uid, in->dyn_neighbs, in->stat_neighbs);

        /* Compute the velocity constrainted by potential collisions */
        vec2_t new_vel = G_ClearPath_NewVelocity(in->cp_ent, in->ent_uid, 
            vpref, *in->dyn_neighbs, *in->stat_neighbs, in->save_debug);

        out->ent_uid = in->ent_uid;
        out->ent_vel = new_vel;
    }
}

static struct result move_task(void *arg)
{
    struct move_task_arg *move_arg = arg;
    size_t ncomputed = 0;

    for(int i = move_arg->begin_idx; i <= move_arg->end_idx; i++) {

        move_work(i, i);
        ncomputed++;

        if(ncomputed % 64 == 0)
            Task_Yield();
    }
    return NULL_RESULT;
}

static void move_complete_work(void)
{
    for(int i = 0; i < s_move_work.ntasks; i++) {
        while(!Sched_FutureIsReady(&s_move_work.futures[i])) {
            Sched_RunSync(s_move_work.tids[i]);
        }
    }
}

static void move_copy_gamestate(void)
{
    PERF_ENTER();
    s_move_work.gamestate.flags = G_FlagsCopyTable();
    s_move_work.gamestate.positions = G_Pos_CopyTable();
    s_move_work.gamestate.postree = G_Pos_CopyQuadTree();
    s_move_work.gamestate.sel_radiuses = G_SelectionRadiusCopyTable();
    s_move_work.gamestate.faction_ids = G_FactionIDCopyTable();
    s_move_work.gamestate.map = M_AL_CopyWithCostsAndBlockers(s_map);
    PERF_RETURN_VOID();
}

static void move_release_gamestate(void)
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
        G_Pos_DestroyQuadTree(s_move_work.gamestate.postree);
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
    if(s_move_work.gamestate.map) {
        PF_FREE(s_move_work.gamestate.map);
        s_move_work.gamestate.map = NULL;
    }
    PERF_RETURN_VOID();
}

static void move_update_gamestate(void)
{
    move_release_gamestate();
    move_copy_gamestate();
}

static void move_finish_work(void)
{
    PERF_ENTER();

    if(s_move_work.nwork == 0)
        PERF_RETURN_VOID();

    move_complete_work();

    PERF_PUSH("velocity updates");
    for(int i = 0; i < s_move_work.nwork; i++) {

        struct move_work_out *out = &s_move_work.out[i];
        struct movestate *ms = movestate_get(out->ent_uid);
        assert(ms);

        ms->vnew = out->ent_vel;
        update_vel_hist(ms, ms->vnew);

        vec2_t vel_diff;
        PFM_Vec2_Sub(&ms->vnew, &ms->velocity, &vel_diff);

        PFM_Vec2_Add(&ms->velocity, &vel_diff, &ms->vnew);
        vec2_truncate(&ms->vnew, ms->max_speed / MOVE_TICK_RES);
    }
    PERF_POP();

    uint32_t key;
    struct movestate curr;

    PERF_PUSH("position updates");
    kh_foreach(s_entity_state_table, key, curr, {
        /* The entity has been removed already */
        if(!G_EntityExists(key))
            continue;
        entity_update(key, curr.vnew);
    });
    PERF_POP();

    stalloc_clear(&s_move_work.mem);
    s_move_work.in = NULL;
    s_move_work.out = NULL;
    s_move_work.nwork = 0;
    s_move_work.ntasks = 0;

    PERF_RETURN_VOID();
}

static void move_prepare_work(void)
{
    size_t ndynamic = kh_size(G_GetDynamicEntsSet());
    s_move_work.in = stalloc(&s_move_work.mem, ndynamic * sizeof(struct move_work_in));
    s_move_work.out = stalloc(&s_move_work.mem, ndynamic * sizeof(struct move_work_out));
}

static void move_push_work(struct move_work_in in)
{
    s_move_work.in[s_move_work.nwork++] = in;
}

static void move_submit_work(void)
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
        s_move_work.tids[s_move_work.ntasks] = Sched_Create(4, move_task, arg, 
            &s_move_work.futures[s_move_work.ntasks], TASK_BIG_STACK);

        if(s_move_work.tids[s_move_work.ntasks] == NULL_TID) {
            move_work(arg->begin_idx, arg->end_idx);
        }else{
            s_move_work.ntasks++;
        }
    }
}

static void on_20hz_tick(void *user, void *event)
{
    PERF_PUSH("movement::on_20hz_tick");

    move_finish_work();
    move_process_cmds();
    move_release_gamestate();
    disband_empty_flocks();

    move_prepare_work();
    move_copy_gamestate();

    uint32_t curr;

    PERF_PUSH("compute volatile fields");
    N_PrepareAsyncWork();
    kh_foreach_key(G_GetDynamicEntsSet(), curr, {
        request_async_field(curr);
    });
    N_AwaitAsyncFields();
    PERF_POP();

    PERF_PUSH("desired velocity computations");
    kh_foreach_key(G_GetDynamicEntsSet(), curr, {

        struct movestate *ms = movestate_get(curr);
        assert(ms);

        if(ent_still(ms))
            continue;

        struct flock *flock = flock_for_ent(curr);
        ms->vdes = ent_desired_velocity(curr);

        vec_cp_ent_t *dyn, *stat;
        dyn = stalloc(&s_move_work.mem, sizeof(vec_cp_ent_t));
        stat = stalloc(&s_move_work.mem, sizeof(vec_cp_ent_t));

        vec_cp_ent_init_alloc(dyn, cp_vec_realloc, cp_vec_PF_FREE);
        vec_cp_ent_init_alloc(stat, cp_vec_realloc, cp_vec_PF_FREE);

        vec_cp_ent_resize(dyn, 16);
        vec_cp_ent_resize(stat, 16);

        vec2_t pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, curr);
        float radius = G_GetSelectionRadiusFrom(s_move_work.gamestate.sel_radiuses, curr);

        struct cp_ent curr_cp = (struct cp_ent) {
            .xz_pos = pos,
            .xz_vel = ms->velocity,
            .radius = radius
        };

        move_push_work((struct move_work_in){
            .ent_uid = curr,
            .ent_des_v = ms->vdes,
            .cp_ent = curr_cp,
            .save_debug = G_ClearPath_ShouldSaveDebug(curr),
            .stat_neighbs = stat,
            .dyn_neighbs = dyn,
            .has_dest_los = flock ? M_NavHasDestLOS(s_map, flock->dest_id, pos) : false
        });
    });
    PERF_POP();

    move_submit_work();

    PERF_POP();
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Move_Init(const struct map *map)
{
    assert(map);
    if(NULL == (s_entity_state_table = kh_init(state))) {
        return false;
    }

    memset(&s_move_work, 0, sizeof(s_move_work));
    if(!stalloc_init(&s_move_work.mem)) {
        kh_destroy(state, s_entity_state_table);
        return NULL;
    }

    if(!queue_cmd_init(&s_move_commands, 256)) {
        stalloc_destroy(&s_move_work.mem);
        kh_destroy(state, s_entity_state_table);
        return NULL;
    }

    vec_entity_init(&s_move_markers);
    vec_flock_init(&s_flocks);

    E_Global_Register(SDL_MOUSEBUTTONDOWN, on_mousedown, NULL, G_RUNNING);
    E_Global_Register(EVENT_RENDER_3D_POST, on_render_3d, NULL, 
        G_RUNNING | G_PAUSED_FULL | G_PAUSED_UI_RUNNING);
    E_Global_Register(EVENT_20HZ_TICK, on_20hz_tick, NULL, G_RUNNING);

    s_map = map;
    s_attack_on_lclick = false;
    s_move_on_lclick = false;
    move_copy_gamestate();
    return true;
}

void G_Move_Shutdown(void)
{
    move_complete_work();
    s_map = NULL;

    E_Global_Unregister(EVENT_20HZ_TICK, on_20hz_tick);
    E_Global_Unregister(EVENT_RENDER_3D_POST, on_render_3d);
    E_Global_Unregister(SDL_MOUSEBUTTONDOWN, on_mousedown);

    for(int i = 0; i < vec_size(&s_move_markers); i++) {
        E_Entity_Unregister(EVENT_ANIM_FINISHED, vec_AT(&s_move_markers, i), on_marker_anim_finish);
        G_RemoveEntity(vec_AT(&s_move_markers, i));
        G_FreeEntity(vec_AT(&s_move_markers, i));
    }

    move_release_gamestate();
    vec_flock_destroy(&s_flocks);
    vec_entity_destroy(&s_move_markers);
    queue_cmd_destroy(&s_move_commands);
    stalloc_destroy(&s_move_work.mem);
    kh_destroy(state, s_entity_state_table);
}

bool G_Move_HasWork(void)
{
    return (queue_size(s_move_commands) > 0);
}

void G_Move_FlushWork(void)
{
    move_finish_work();
    move_process_cmds();
}

void G_Move_AddEntity(uint32_t uid, vec3_t pos, float sel_radius, int faction_id)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_ADD,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = {
            .type = TYPE_VEC3,
            .val.as_vec3 = pos
        },
        .args[2] = {
            .type = TYPE_FLOAT,
            .val.as_float = sel_radius
        },
        .args[3] = {
            .type = TYPE_INT,
            .val.as_float = faction_id
        }
    });
}

void G_Move_RemoveEntity(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_REMOVE,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = uid
        }
    });
}

void G_Move_Stop(uint32_t uid)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_STOP,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = uid
        }
    });
}

bool G_Move_GetDest(uint32_t uid, vec2_t *out_xz, bool *out_attack)
{
    struct move_cmd *cmd = snoop_most_recent_command(MOVE_CMD_SET_DEST,
        (void*)(uintptr_t)uid, uids_match, false);

    if(cmd) {
        *out_xz = cmd->args[1].val.as_vec2;
        *out_attack = cmd->args[2].val.as_bool;
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
        *out_uid = cmd->args[1].val.as_int;
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
    return (ms->state == STATE_ARRIVED);
}

void G_Move_SetDest(uint32_t uid, vec2_t dest_xz, bool attack)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_SET_DEST,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = {
            .type = TYPE_VEC2,
            .val.as_vec2 = dest_xz
        },
        .args[2] = {
            .type = TYPE_BOOL,
            .val.as_bool = attack
        }
    });
}

void G_Move_SetChangeDirection(uint32_t uid, quat_t target)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_CHANGE_DIRECTION,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = {
            .type = TYPE_QUAT,
            .val.as_quat = target
        }
    });
}

void G_Move_SetEnterRange(uint32_t uid, uint32_t target, float range)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_SET_ENTER_RANGE,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = {
            .type = TYPE_INT,
            .val.as_int = target
        },
        .args[2] = {
            .type = TYPE_FLOAT,
            .val.as_float = range
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
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = uid
        }
    });
}

void G_Move_SetSurroundEntity(uint32_t uid, uint32_t target)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_SET_SURROUND_ENTITY,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = {
            .type = TYPE_INT,
            .val.as_int = target
        }
    });
}

void G_Move_UpdatePos(uint32_t uid, vec2_t pos)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_UPDATE_POS,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = {
            .type = TYPE_VEC2,
            .val.as_vec2 = pos
        }
    });
}

void G_Move_UpdateFactionID(uint32_t uid, int oldfac, int newfac)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_UPDATE_FACTION_ID,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = {
            .type = TYPE_INT,
            .val.as_int = oldfac
        },
        .args[2] = {
            .type = TYPE_INT,
            .val.as_int = newfac
        }
    });
}

void G_Move_UpdateSelectionRadius(uint32_t uid, float sel_radius)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_UPDATE_SELECTION_RADIUS,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = {
            .type = TYPE_FLOAT,
            .val.as_float  = sel_radius
        }
    });
}

bool G_Move_InTargetMode(void)
{
    return (s_move_on_lclick || s_attack_on_lclick);
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
        *out = cmd->args[1].val.as_float;
        return true;
    }

    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return false;
    struct movestate *ms = &kh_value(s_entity_state_table, k);
    *out = ms->max_speed;
    return true;
}

bool G_Move_SetMaxSpeed(uint32_t uid, float speed)
{
    ASSERT_IN_MAIN_THREAD();
    move_push_cmd((struct move_cmd){
        .type = MOVE_CMD_SET_MAX_SPEED,
        .args[0] = {
            .type = TYPE_INT,
            .val.as_int = uid
        },
        .args[1] = {
            .type = TYPE_FLOAT,
            .val.as_float = speed
        }
    });
    return true;
}

void G_Move_Upload(void)
{
    ASSERT_IN_MAIN_THREAD();
    PERF_ENTER();

    const size_t nents = kh_size(G_GetDynamicEntsSet());
    const size_t buffsize = nents * (sizeof(uint32_t) * 3 + sizeof(vec2_t));
    struct render_workspace *ws = G_GetSimWS();
    void *buff = stalloc(&ws->args, buffsize);
    unsigned char *cursor = buff;

    for(int gpu_id = 1; gpu_id <= nents; gpu_id++) {

        uint32_t uid = G_EntForGPUID(gpu_id);
        khiter_t k = kh_get(state, s_entity_state_table, uid);
        assert(k != kh_end(s_entity_state_table));
        const struct movestate *curr = &kh_value(s_entity_state_table, k);

        const struct flock *flock;
        uint32_t flock_id = flock_id_for_ent(uid, &flock);
        uint32_t movestate = curr->state;
        vec2_t pos = G_Pos_GetXZFrom(s_move_work.gamestate.positions, uid);
        uint32_t has_dest_los = flock ? M_NavHasDestLOS(s_map, flock->dest_id, pos) : false;
        vec2_t dest_xz = flock ? flock->target_xz : (vec2_t){0.0f, 0.0f};

        *((uint32_t*)cursor) = flock_id;        cursor += sizeof(uint32_t);
        *((uint32_t*)cursor) = movestate;       cursor += sizeof(uint32_t);
        *((uint32_t*)cursor) = has_dest_los;    cursor += sizeof(uint32_t);
        *((vec2_t*)cursor) = dest_xz;           cursor += sizeof(vec2_t);
    }
    assert(cursor == ((unsigned char*)buff) + buffsize);

    R_PushCmd((struct rcmd){
        .func = R_GL_MoveUploadData,
        .nargs = 3,
        .args = {
            buff,
            R_PushArg(&nents, sizeof(nents)),
            R_PushArg(&buffsize, sizeof(buffsize)),
        },
    });

    PERF_RETURN_VOID();
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

        struct attr max_speed = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float = curr.max_speed
        };
        CHK_TRUE_RET(Attr_Write(stream, &max_speed, "max_speed"));

        struct attr vdes = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = curr.vdes
        };
        CHK_TRUE_RET(Attr_Write(stream, &vdes, "vdes"));

        struct attr velocity = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = curr.velocity
        };
        CHK_TRUE_RET(Attr_Write(stream, &velocity, "velocity"));

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
            .val.as_int = curr.wait_prev
        };
        CHK_TRUE_RET(Attr_Write(stream, &wait_prev, "wait_prev"));

        struct attr wait_ticks_left = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.wait_ticks_left
        };
        CHK_TRUE_RET(Attr_Write(stream, &wait_ticks_left, "wait_ticks_left"));

        for(int i = 0; i < VEL_HIST_LEN; i++) {
        
            struct attr hist_entry = (struct attr){
                .type = TYPE_VEC2,
                .val.as_vec2 = curr.vel_hist[i]
            };
            CHK_TRUE_RET(Attr_Write(stream, &hist_entry, "hist_entry"));
        }

        struct attr vel_hist_idx = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.vel_hist_idx
        };
        CHK_TRUE_RET(Attr_Write(stream, &vel_hist_idx, "vel_hist_idx"));

        struct attr surround_target_uid = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr.surround_target_uid
        };
        CHK_TRUE_RET(Attr_Write(stream, &surround_target_uid, "surround_target_uid"));

        struct attr surround_target_prev = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = curr.surround_target_prev
        };
        CHK_TRUE_RET(Attr_Write(stream, &surround_target_prev, "surround_target_prev"));

        struct attr surround_nearest_prev = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = curr.surround_nearest_prev
        };
        CHK_TRUE_RET(Attr_Write(stream, &surround_nearest_prev, "surround_nearest_prev"));

        struct attr using_surround_field = (struct attr){
            .type = TYPE_BOOL,
            .val.as_bool = curr.using_surround_field
        };
        CHK_TRUE_RET(Attr_Write(stream, &using_surround_field, "using_surround_field"));

        struct attr target_prev_pos = (struct attr){
            .type = TYPE_VEC2,
            .val.as_vec2 = curr.target_prev_pos
        };
        CHK_TRUE_RET(Attr_Write(stream, &target_prev_pos, "target_prev_pos"));

        struct attr target_range = (struct attr){
            .type = TYPE_FLOAT,
            .val.as_float = curr.target_range
        };
        CHK_TRUE_RET(Attr_Write(stream, &target_range, "target_range"));

        struct attr target_dir = (struct attr){
            .type = TYPE_QUAT,
            .val.as_quat = curr.target_dir
        };
        CHK_TRUE_RET(Attr_Write(stream, &target_dir, "target_dir"));
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

        vec_flock_push(&s_flocks, new_flock);
        Sched_TryYield();
        continue;

    fail_flock:
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

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        ms->state = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_FLOAT);
        ms->max_speed = attr.val.as_float;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC2);
        ms->vdes = attr.val.as_vec2;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC2);
        ms->velocity = attr.val.as_vec2;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);

        const bool blocking = attr.val.as_bool;
        assert(ms->blocking);
        if(!blocking) {
            entity_unblock(uid);
        }

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        ms->wait_prev = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        ms->wait_ticks_left = attr.val.as_int;

        for(int i = 0; i < VEL_HIST_LEN; i++) {
        
            CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
            CHK_TRUE_RET(attr.type == TYPE_VEC2);
            ms->vel_hist[i] = attr.val.as_vec2;
        }

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        ms->vel_hist_idx = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        ms->surround_target_uid = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC2);
        ms->surround_target_prev = attr.val.as_vec2;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC2);
        ms->surround_nearest_prev = attr.val.as_vec2;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_BOOL);
        ms->using_surround_field = attr.val.as_bool;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC2);
        ms->target_prev_pos = attr.val.as_vec2;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_FLOAT);
        ms->target_range = attr.val.as_float;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_QUAT);
        ms->target_dir = attr.val.as_quat;

        Sched_TryYield();
    }

    return true;
}

