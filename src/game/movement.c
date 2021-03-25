/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2020 Eduard Permyakov 
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
#define MAX_CP_TASKS (64)

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
    /* The base movement speed in units of OpenGL coords / second */
    float              max_speed;
    /* The desired velocity returned by the navigation system */
    vec2_t             vdes;
    /* The newly computed velocity (the desired velocity constrained by flocking forces) */
    vec2_t             vnew;
    /* The current velocity */
    vec2_t             velocity;
    /* Flag to track whether the entiy is currently acting as a 
     * navigation blocker, and the last position where it became a blocker. */
    bool               blocking;
    vec2_t             last_stop_pos;
    float              last_stop_radius;
    /* Information for waking up from the 'WAITING' state */
    enum arrival_state wait_prev;
    int                wait_ticks_left;
    /* History of the previous ticks' velocities. Used for velocity smoothing. */
    vec2_t             vel_hist[VEL_HIST_LEN];
    int                vel_hist_idx;
    /* Entity that we're moving towards when in the 'SURROUND_STATIC_ENTITY' state */
    uint32_t           surround_target_uid;
    /* Additional state for entities in 'ENTER_ENTITY_RANGE' state */
    vec2_t             target_prev_pos;
    float              target_range;
    /* The target direction for 'turning' entities */
    quat_t             target_dir;
};

struct flock{
    khash_t(entity) *ents;
    vec2_t           target_xz; 
    dest_id_t        dest_id;
};

struct cp_work_in{
    struct       cp_ent ent;
    uint32_t     ent_uid;
    vec2_t       ent_des_v;
    vec_cp_ent_t dyn_neighbs;
    vec_cp_ent_t stat_neighbs;
    bool         save_debug;
};

struct cp_work_out{
    uint32_t ent_uid;
    vec2_t   ent_vel;
};

struct cp_task_arg{
    size_t begin_idx;
    size_t end_idx;
};

struct cp_work{
    struct memstack     mem;
    struct cp_work_in  *in;
    struct cp_work_out *out;
    size_t              nwork;
    size_t              ntasks;
    uint32_t            tids[MAX_CP_TASKS];
    struct future       futures[MAX_CP_TASKS];
};

KHASH_MAP_INIT_INT(state, struct movestate)

VEC_TYPE(flock, struct flock)
VEC_IMPL(static inline, flock, struct flock)

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

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map       *s_map;
static bool                    s_attack_on_lclick = false;
static bool                    s_move_on_lclick = false;
static bool                    s_click_move_enabled = true;

static vec_pentity_t           s_move_markers;
static vec_flock_t             s_flocks;
static khash_t(state)         *s_entity_state_table;

/* Store the most recently issued move command location for debug rendering */
static bool                    s_last_cmd_dest_valid = false;
static dest_id_t               s_last_cmd_dest;

static struct cp_work          s_cp_work;

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
static struct movestate *movestate_get(const struct entity *ent)
{
    khiter_t k = kh_get(state, s_entity_state_table, ent->uid);
    if(k == kh_end(s_entity_state_table))
        return NULL;
    return &kh_value(s_entity_state_table, k);
}

static void flock_try_remove(struct flock *flock, const struct entity *ent)
{
    khiter_t k;
    if((k = kh_get(entity, flock->ents, ent->uid)) != kh_end(flock->ents))
        kh_del(entity, flock->ents, k);
}

static void flock_add(struct flock *flock, const struct entity *ent)
{
    int ret;
    khiter_t k = kh_put(entity, flock->ents, ent->uid, &ret);
    assert(ret != -1 && ret != 0);
    kh_value(flock->ents, k) = (struct entity*)ent;
}

static bool flock_contains(const struct flock *flock, const struct entity *ent)
{
    khiter_t k = kh_get(entity, flock->ents, ent->uid);
    if(k != kh_end(flock->ents))
        return true;
    return false;
}

static struct flock *flock_for_ent(const struct entity *ent)
{
    for(int i = 0; i < vec_size(&s_flocks); i++) {

        struct flock *curr_flock = &vec_AT(&s_flocks, i);            
        if(flock_contains(curr_flock, ent))
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

static void entity_block(const struct entity *ent)
{
    M_NavBlockersIncref(G_Pos_GetXZ(ent->uid), G_GetSelectionRadius(ent->uid), G_GetFactionID(ent->uid), s_map);

    struct movestate *ms = movestate_get(ent);
    assert(!ms->blocking);

    ms->blocking = true;
    ms->last_stop_pos = G_Pos_GetXZ(ent->uid);
    ms->last_stop_radius = G_GetSelectionRadius(ent->uid);
}

static void entity_unblock(const struct entity *ent)
{
    struct movestate *ms = movestate_get(ent);
    assert(ms->blocking);

    M_NavBlockersDecref(ms->last_stop_pos, ms->last_stop_radius, G_GetFactionID(ent->uid), s_map);
    ms->blocking = false;
}

static bool stationary(const struct entity *ent)
{
    if(!(ent->flags & ENTITY_FLAG_MOVABLE))
        return true;

    struct movestate *ms = movestate_get(ent);
    if(ms->max_speed == 0.0f)
        return true;

    return false;
}

static bool entities_equal(struct entity **a, struct entity **b)
{
    return ((*a)->uid == (*b)->uid);
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

static void entity_finish_moving(const struct entity *ent, enum arrival_state newstate)
{
    struct movestate *ms = movestate_get(ent);
    assert(!ent_still(ms));

    E_Entity_Notify(EVENT_MOTION_END, ent->uid, NULL, ES_ENGINE);
    if(ent->flags & ENTITY_FLAG_COMBATABLE
    && (ms->state != STATE_TURNING)) {
        G_Combat_SetStance(ent, COMBAT_STANCE_AGGRESSIVE);
    }

    if(newstate == STATE_WAITING) {
        ms->wait_prev = ms->state;
        ms->wait_ticks_left = WAIT_TICKS;
    }

    ms->state = newstate;
    ms->velocity = (vec2_t){0.0f, 0.0f};
    ms->vnew = (vec2_t){0.0f, 0.0f};

    entity_block(ent);
    assert(ent_still(ms));
}

static void on_marker_anim_finish(void *user, void *event)
{
    struct entity *ent = user;
    assert(ent);

    int idx = vec_pentity_indexof(&s_move_markers, ent, entities_equal);
    assert(idx != -1);
    vec_pentity_del(&s_move_markers, idx);

    E_Entity_Unregister(EVENT_ANIM_FINISHED, ent->uid, on_marker_anim_finish);
    G_RemoveEntity(ent);
    G_SafeFree(ent);
}

static bool same_chunk_as_any_in_set(struct tile_desc desc, const struct tile_desc *set,
                                     size_t set_size)
{
    for(int i = 0; i < set_size; i++) {

        const struct tile_desc *curr = &set[i];
        if(desc.chunk_r == curr->chunk_r && desc.chunk_c == curr->chunk_c) 
            return true;
    }
    return false;
}

static void remove_from_flocks(const struct entity *ent)
{
    /* Remove any flocks which may have become empty. Iterate vector in backwards order 
     * so that we can delete while iterating, since the last element in the vector takes
     * the place of the deleted one. 
     */
    for(int i = vec_size(&s_flocks)-1; i >= 0; i--) {

        struct flock *curr_flock = &vec_AT(&s_flocks, i);
        flock_try_remove(curr_flock, ent);

        if(kh_size(curr_flock->ents) == 0) {
            kh_destroy(entity, curr_flock->ents);
            vec_flock_del(&s_flocks, i);
        }
    }
    assert(NULL == flock_for_ent(ent));
}

static void filter_selection_pathable(const vec_pentity_t *in_sel, vec_pentity_t *out_sel)
{
    vec_pentity_init(out_sel);
    for(int i = 0; i < vec_size(in_sel); i++) {

        struct entity *curr = vec_AT(in_sel, i);
        vec2_t xz_pos = G_Pos_GetXZ(curr->uid);

        if(!M_NavPositionPathable(s_map, Entity_NavLayer(curr), xz_pos))
            continue;
        vec_pentity_push(out_sel, curr);
    }
}

static void split_into_layers(const vec_pentity_t *sel, vec_pentity_t layer_flocks[static NAV_LAYER_MAX])
{
    for(int i = 0; i < NAV_LAYER_MAX; i++) {
        vec_pentity_init(layer_flocks + i);
    }

    for(int i = 0; i < vec_size(sel); i++) {

        struct entity *curr = vec_AT(sel, i);
        enum nav_layer layer = Entity_NavLayer(curr);
        vec_pentity_push(&layer_flocks[layer], curr);
    }
}

static bool make_flock(const vec_pentity_t *units, vec2_t target_xz, enum nav_layer layer, bool attack)
{
    if(vec_size(units) == 0)
        return true;

    bool ret;
    const struct entity *first = vec_AT(units, 0);

    /* The following won't be optimal when the entities in the unitsection are on different 
     * 'islands'. Handling that case is not a top priority. 
     */
    vec2_t first_ent_pos_xz = G_Pos_GetXZ(first->uid);
    target_xz = M_NavClosestReachableDest(s_map, layer, first_ent_pos_xz, target_xz);

    /* First remove the entities in the unitsection from any active flocks */
    for(int i = 0; i < vec_size(units); i++) {

        const struct entity *curr_ent = vec_AT(units, i);
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

        const struct entity *curr_ent = vec_AT(units, i);
        if(stationary(curr_ent))
            continue;

        struct movestate *ms = movestate_get(curr_ent);
        assert(ms);

        if(ent_still(ms)) {
            entity_unblock(curr_ent); 
            E_Entity_Notify(EVENT_MOTION_START, curr_ent->uid, NULL, ES_ENGINE);
        }

        flock_add(&new_flock, curr_ent);
        ms->state = STATE_MOVING;
    }

    /* The flow fields will be computed on-demand during the next movement update tick */
    new_flock.target_xz = target_xz;
    if(attack) {
        new_flock.dest_id = M_NavDestIDForPosAttacking(s_map, target_xz, layer, G_GetFactionID(first->uid));
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

        struct entity *curr;
        kh_foreach(new_flock.ents, (uint32_t){0}, curr, { flock_add(merge_flock, curr); });
        kh_destroy(entity, new_flock.ents);
    
    }else{
        vec_flock_push(&s_flocks, new_flock);
    }

    s_last_cmd_dest_valid = true;
    s_last_cmd_dest = new_flock.dest_id;
    return true;
}

static void make_flocks(const vec_pentity_t *sel, vec2_t target_xz, bool attack)
{
    vec_pentity_t fsel;
    filter_selection_pathable(sel, &fsel);

    if(vec_size(&fsel) == 0)
        return;

    vec_pentity_t layer_flocks[NAV_LAYER_MAX];
    split_into_layers(&fsel, layer_flocks);
    vec_pentity_destroy(&fsel);

    for(int i = 0; i < NAV_LAYER_MAX; i++) {
        make_flock(layer_flocks + i, target_xz, i, attack);
        vec_pentity_destroy(layer_flocks + i);
    }
}

size_t adjacent_flock_members(const struct entity *ent, const struct flock *flock, 
                              struct entity *out[])
{
    vec2_t ent_xz_pos = G_Pos_GetXZ(ent->uid);
    size_t ret = 0;

    uint32_t key;
    struct entity *curr;
    (void)key;

    kh_foreach(flock->ents, key, curr, {

        if(curr == ent)
            continue;

        vec2_t diff;

        vec2_t curr_xz_pos = G_Pos_GetXZ(curr->uid);
        PFM_Vec2_Sub(&ent_xz_pos, &curr_xz_pos, &diff);

        if(PFM_Vec2_Len(&diff) <= G_GetSelectionRadius(ent->uid) + G_GetSelectionRadius(curr->uid) + ADJACENCY_SEP_DIST)
            out[ret++] = curr;  
    });
    return ret;
}

static void move_marker_add(vec3_t pos, bool attack)
{
    const uint32_t uid = Entity_NewUID();
    struct entity *ent = attack ? AL_EntityFromPFObj("assets/models/arrow", "arrow-red.pfobj", "__move_marker__", uid) 
                                : AL_EntityFromPFObj("assets/models/arrow", "arrow-green.pfobj", "__move_marker__", uid);
    if(!ent)
        return;

    ent->flags |= ENTITY_FLAG_MARKER;
    G_AddEntity(ent, pos);

    Entity_SetScale(ent->uid, (vec3_t){2.0f, 2.0f, 2.0f});
    E_Entity_Register(EVENT_ANIM_FINISHED, ent->uid, on_marker_anim_finish, ent, G_RUNNING);
    A_SetActiveClip(ent->uid, "Converge", ANIM_MODE_ONCE, 48);

    vec_pentity_push(&s_move_markers, ent);
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
    const vec_pentity_t *sel = G_Sel_Get(&sel_type);
    size_t nmoved = 0;

    if(vec_size(sel) == 0 || sel_type != SELECTION_TYPE_PLAYER)
        return;

    for(int i = 0; i < vec_size(sel); i++) {

        const struct entity *curr = vec_AT(sel, i);
        if(!(curr->flags & ENTITY_FLAG_MOVABLE))
            continue;

        G_StopEntity(curr, false);
        E_Entity_Notify(EVENT_MOVE_ISSUED, curr->uid, NULL, ES_ENGINE);
        nmoved++;

        if(curr->flags & ENTITY_FLAG_COMBATABLE) {
            G_Combat_SetStance(curr, attack ? COMBAT_STANCE_AGGRESSIVE : COMBAT_STANCE_NO_ENGAGEMENT);
        }
    }

    if(nmoved) {
        move_marker_add(mouse_coord, attack);
        make_flocks(sel, (vec2_t){mouse_coord.x, mouse_coord.z}, attack);
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
    const vec_pentity_t *sel = G_Sel_Get(&seltype);

    if(setting.as_bool && vec_size(sel) > 0) {
    
        const struct entity *ent = vec_AT(sel, 0);
        struct movestate *ms = movestate_get(ent);
        if(ms) {

            char strbuff[256];
            pf_snprintf(strbuff, ARR_SIZE(strbuff), "Arrival State: %s Velocity: (%f, %f)", 
                s_state_str[ms->state], ms->velocity.x, ms->velocity.z);
            struct rgba text_color = (struct rgba){255, 0, 0, 255};
            UI_DrawText(strbuff, (struct rect){5,50,450,50}, text_color);

            const struct camera *cam = G_GetActiveCamera();
            struct flock *flock = flock_for_ent(ent);

            switch(ms->state) {
            case STATE_MOVING:
            case STATE_ENTER_ENTITY_RANGE:
                assert(flock);
                M_NavRenderVisiblePathFlowField(s_map, cam, flock->dest_id);
                break;
            case STATE_SURROUND_ENTITY: {

                struct entity *target = G_EntityForUID(ms->surround_target_uid);
                if(!target)
                    break;

                vec2_t pos_xz = G_Pos_GetXZ(ent->uid);
                vec2_t target_pos_xz = G_Pos_GetXZ(target->uid);
                float dx = target_pos_xz.x - pos_xz.x;
                float dz = target_pos_xz.z - pos_xz.z;
                if(fabs(dx) < CHUNK_WIDTH/4.0f && fabs(dz) < CHUNK_HEIGHT/4.0f) {
                    M_NavRenderVisibleSurroundField(s_map, cam, Entity_NavLayer(ent), target);
                }else{
                    M_NavRenderVisiblePathFlowField(s_map, cam, flock->dest_id);
                }
                break;
            }
            case STATE_ARRIVED:
            case STATE_WAITING:
            case STATE_TURNING:
                break;
            case STATE_SEEK_ENEMIES:
                M_NavRenderVisibleEnemySeekField(s_map, cam, Entity_NavLayer(ent), G_GetFactionID(ent->uid));
                break;
            default: assert(0);
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

static vec2_t ent_desired_velocity(const struct entity *ent)
{
    struct movestate *ms = movestate_get(ent);
    vec2_t pos_xz = G_Pos_GetXZ(ent->uid);
    struct flock *fl = flock_for_ent(ent);

    switch(ms->state) {
    case STATE_TURNING:
        return (vec2_t){0.0f, 0.0f};

    case STATE_SEEK_ENEMIES: 
        return M_NavDesiredEnemySeekVelocity(s_map, Entity_NavLayer(ent), pos_xz, G_GetFactionID(ent->uid));

    case STATE_SURROUND_ENTITY: {

        const struct entity *target = G_EntityForUID(ms->surround_target_uid);
        if(target) {
            vec2_t target_pos_xz = G_Pos_GetXZ(ms->surround_target_uid);
            float dx = target_pos_xz.x - pos_xz.x;
            float dz = target_pos_xz.z - pos_xz.z;
            if(fabs(dx) < CHUNK_WIDTH/4.0f && fabs(dz) < CHUNK_HEIGHT/4.0f) {
                return M_NavDesiredSurroundVelocity(s_map, Entity_NavLayer(ent), 
                    pos_xz, target, G_GetFactionID(ent->uid));
            }
        }
        return M_NavDesiredPointSeekVelocity(s_map, fl->dest_id, pos_xz, fl->target_xz);
    }
    default:
        assert(fl);
        return M_NavDesiredPointSeekVelocity(s_map, fl->dest_id, pos_xz, fl->target_xz);
    }
}

/* Seek behaviour makes the entity target and approach a particular destination point.
 */
static vec2_t seek_force(const struct entity *ent, vec2_t target_xz)
{
    struct movestate *ms = movestate_get(ent);
    assert(ms);

    vec2_t ret, desired_velocity;
    vec2_t pos_xz = G_Pos_GetXZ(ent->uid);

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
static vec2_t arrive_force_point(const struct entity *ent, dest_id_t dest_id, vec2_t target_xz)
{
    vec2_t ret, desired_velocity;
    vec2_t pos_xz = G_Pos_GetXZ(ent->uid);
    float distance;

    struct movestate *ms = movestate_get(ent);
    assert(ms);

    if(M_NavHasDestLOS(s_map, dest_id, pos_xz)) {

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

static vec2_t arrive_force_enemies(const struct entity *ent)
{
    vec2_t ret, desired_velocity;
    vec2_t pos_xz = G_Pos_GetXZ(ent->uid);
    float distance;

    struct movestate *ms = movestate_get(ent);
    assert(ms);

    PFM_Vec2_Scale(&ms->vdes, ms->max_speed / MOVE_TICK_RES, &desired_velocity);
    PFM_Vec2_Sub(&desired_velocity, &ms->velocity, &ret);
    vec2_truncate(&ret, MAX_FORCE);
    return ret;
}

/* Alignment is a behaviour that causes a particular agent to line up with agents close by.
 */
static vec2_t alignment_force(const struct entity *ent, const struct flock *flock)
{
    vec2_t ret = (vec2_t){0.0f};
    size_t neighbour_count = 0;

    uint32_t key;
    struct entity *curr;
    (void)key;

    kh_foreach(flock->ents, key, curr, {

        if(curr == ent)
            continue;

        vec2_t diff;
        vec2_t ent_xz_pos = G_Pos_GetXZ(ent->uid);
        vec2_t curr_xz_pos = G_Pos_GetXZ(curr->uid);

        PFM_Vec2_Sub(&curr_xz_pos, &ent_xz_pos, &diff);
        if(PFM_Vec2_Len(&diff) < ALIGN_NEIGHBOUR_RADIUS) {

            struct movestate *ms = movestate_get(ent);
            assert(ms);

            if(PFM_Vec2_Len(&ms->velocity) < EPSILON)
                continue; 

            PFM_Vec2_Add(&ret, &ms->velocity, &ret);
            neighbour_count++;
        }
    });

    if(0 == neighbour_count)
        return (vec2_t){0.0f};

    struct movestate *ms = movestate_get(ent);
    assert(ms);

    PFM_Vec2_Scale(&ret, 1.0f / neighbour_count, &ret);
    PFM_Vec2_Sub(&ret, &ms->velocity, &ret);
    vec2_truncate(&ret, MAX_FORCE);
    return ret;
}

/* Cohesion is a behaviour that causes agents to steer towards the center of mass of nearby agents.
 */
static vec2_t cohesion_force(const struct entity *ent, const struct flock *flock)
{
    vec2_t COM = (vec2_t){0.0f};
    size_t neighbour_count = 0;
    vec2_t ent_xz_pos = G_Pos_GetXZ(ent->uid);

    uint32_t key;
    struct entity *curr;
    (void)key;

    kh_foreach(flock->ents, key, curr, {

        if(curr == ent)
            continue;

        vec2_t diff;
        vec2_t curr_xz_pos = G_Pos_GetXZ(curr->uid);
        PFM_Vec2_Sub(&curr_xz_pos, &ent_xz_pos, &diff);

        float t = (PFM_Vec2_Len(&diff) - COHESION_NEIGHBOUR_RADIUS*0.75) / COHESION_NEIGHBOUR_RADIUS;
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
static vec2_t separation_force(const struct entity *ent, float buffer_dist)
{
    vec2_t ret = (vec2_t){0.0f};
    struct entity *near_ents[128];
    int num_near = G_Pos_EntsInCircle(G_Pos_GetXZ(ent->uid), 
        SEPARATION_NEIGHB_RADIUS, near_ents, ARR_SIZE(near_ents));

    for(int i = 0; i < num_near; i++) {

        struct entity *curr = near_ents[i];
        if(curr == ent)
            continue;
        if(!(curr->flags & ENTITY_FLAG_MOVABLE))
            continue;

        vec2_t diff;
        vec2_t ent_xz_pos = G_Pos_GetXZ(ent->uid);
        vec2_t curr_xz_pos = G_Pos_GetXZ(curr->uid);

        float radius = G_GetSelectionRadius(ent->uid) + G_GetSelectionRadius(curr->uid) + buffer_dist;
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

static vec2_t point_seek_total_force(const struct entity *ent, const struct flock *flock)
{
    struct movestate *ms = movestate_get(ent);
    assert(ms);

    vec2_t arrive = arrive_force_point(ent, flock->dest_id, flock->target_xz);
    vec2_t cohesion = cohesion_force(ent, flock);
    vec2_t separation = separation_force(ent, SEPARATION_BUFFER_DIST);

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

static vec2_t enemy_seek_total_force(const struct entity *ent)
{
    struct movestate *ms = movestate_get(ent);
    assert(ms);

    vec2_t arrive = arrive_force_enemies(ent);
    vec2_t separation = separation_force(ent, SEPARATION_BUFFER_DIST);

    PFM_Vec2_Scale(&arrive,     MOVE_ARRIVE_FORCE_SCALE,   &arrive);
    PFM_Vec2_Scale(&separation, SEPARATION_FORCE_SCALE,    &separation);

    vec2_t ret = (vec2_t){0.0f, 0.0f};
    PFM_Vec2_Add(&ret, &arrive, &ret);
    PFM_Vec2_Add(&ret, &separation, &ret);

    vec2_truncate(&ret, MAX_FORCE);
    return ret;
}

static vec2_t new_pos_for_vel(const struct entity *ent, vec2_t velocity)
{
    vec2_t xz_pos = G_Pos_GetXZ(ent->uid);
    vec2_t new_pos;

    PFM_Vec2_Add(&xz_pos, &velocity, &new_pos);
    return new_pos;
}

/* Nullify the components of the force which would guide
 * the entity towards an impassable tile. */
static void nullify_impass_components(const struct entity *ent, vec2_t *inout_force)
{
    vec2_t nt_dims = N_TileDims();
    enum nav_layer layer = Entity_NavLayer(ent);

    vec2_t left =  (vec2_t){G_Pos_Get(ent->uid).x + nt_dims.x, G_Pos_Get(ent->uid).z};
    vec2_t right = (vec2_t){G_Pos_Get(ent->uid).x - nt_dims.x, G_Pos_Get(ent->uid).z};
    vec2_t top =   (vec2_t){G_Pos_Get(ent->uid).x, G_Pos_Get(ent->uid).z + nt_dims.z};
    vec2_t bot =   (vec2_t){G_Pos_Get(ent->uid).x, G_Pos_Get(ent->uid).z - nt_dims.z};

    if((inout_force->x > 0 && !M_NavPositionPathable(s_map, layer, left))
    || (inout_force->x < 0 && !M_NavPositionPathable(s_map, layer, right)))
        inout_force->x = 0.0f;

    if((inout_force->z > 0 && !M_NavPositionPathable(s_map, layer, top))
    || (inout_force->z < 0 && !M_NavPositionPathable(s_map, layer, bot)))
        inout_force->z = 0.0f;
}

static vec2_t point_seek_vpref(const struct entity *ent, const struct flock *flock)
{
    struct movestate *ms = movestate_get(ent);
    assert(ms);

    vec2_t steer_force;
    for(int prio = 0; prio < 3; prio++) {

        switch(prio) {
        case 0: steer_force = point_seek_total_force(ent, flock); break;
        case 1: steer_force = separation_force(ent, SEPARATION_BUFFER_DIST); break;
        case 2: steer_force = arrive_force_point(ent, flock->dest_id, flock->target_xz); break;
        }

        nullify_impass_components(ent, &steer_force);
        if(PFM_Vec2_Len(&steer_force) > MAX_FORCE * 0.01)
            break;
    }

    vec2_t accel, new_vel; 
    PFM_Vec2_Scale(&steer_force, 1.0f / ENTITY_MASS, &accel);

    PFM_Vec2_Add(&ms->velocity, &accel, &new_vel);
    vec2_truncate(&new_vel, ms->max_speed / MOVE_TICK_RES);

    return new_vel;
}

static vec2_t enemy_seek_vpref(const struct entity *ent)
{
    struct movestate *ms = movestate_get(ent);
    assert(ms);

    vec2_t steer_force = enemy_seek_total_force(ent);

    vec2_t accel, new_vel; 
    PFM_Vec2_Scale(&steer_force, 1.0f / ENTITY_MASS, &accel);

    PFM_Vec2_Add(&ms->velocity, &accel, &new_vel);
    vec2_truncate(&new_vel, ms->max_speed / MOVE_TICK_RES);

    return new_vel;
}

static void update_vel_hist(struct movestate *ms, vec2_t vnew)
{
    assert(ms->vel_hist >= 0 && ms->vel_hist_idx < VEL_HIST_LEN);
    ms->vel_hist[ms->vel_hist_idx] = vnew;
    ms->vel_hist_idx = ((ms->vel_hist_idx+1) % VEL_HIST_LEN);
}

/* Simple Moving Average */
static vec2_t vel_sma(const struct movestate *ms)
{
    vec2_t ret = {0};
    for(int i = 0; i < VEL_HIST_LEN; i++)
        PFM_Vec2_Add(&ret, (vec2_t*)&ms->vel_hist[i], &ret); 
    PFM_Vec2_Scale(&ret, 1.0f/VEL_HIST_LEN, &ret);
    return ret;
}

/* Weighted Moving Average */
static vec2_t vel_wma(const struct movestate *ms)
{
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

static void entity_update(struct entity *ent, vec2_t new_vel)
{
    struct movestate *ms = movestate_get(ent);
    assert(ms);

    vec2_t new_pos_xz = new_pos_for_vel(ent, new_vel);
    enum nav_layer layer = Entity_NavLayer(ent);

    if(PFM_Vec2_Len(&new_vel) > 0
    && M_NavPositionPathable(s_map, layer, new_pos_xz)) {
    
        vec3_t new_pos = (vec3_t){new_pos_xz.x, M_HeightAtPoint(s_map, new_pos_xz), new_pos_xz.z};
        G_Pos_Set(ent, new_pos);
        ms->velocity = new_vel;

        /* Use a weighted average of past velocities ot set the entity's orientation. This means that 
         * the entity's visible orientation lags behind its' true orientation slightly. However, this 
         * greatly smooths the turning of the entity, giving a more natural look to the movemment. 
         */
        vec2_t wma = vel_wma(ms);
        if(PFM_Vec2_Len(&wma) > EPSILON) {
            Entity_SetRot(ent->uid, dir_quat_from_velocity(wma));
        }
    }else{
        ms->velocity = (vec2_t){0.0f, 0.0f}; 
    }

    /* If the entity's current position isn't pathable, simply keep it 'stuck' there in
     * the same state it was in before. Under normal conditions, no entity can move from 
     * pathable terrain to non-pathable terrain, but an this violation is possible by 
     * forcefully setting the entity's position from a scripting call. 
     */
    if(!M_NavPositionPathable(s_map, layer, G_Pos_GetXZ(ent->uid)))
        return;

    switch(ms->state) {
    case STATE_MOVING: {

        vec2_t diff_to_target;
        vec2_t xz_pos = G_Pos_GetXZ(ent->uid);
        struct flock *flock = flock_for_ent(ent);
        assert(flock);

        PFM_Vec2_Sub((vec2_t*)&flock->target_xz, &xz_pos, &diff_to_target);
        float arrive_thresh = G_GetSelectionRadius(ent->uid) * 1.5f;

        if(PFM_Vec2_Len(&diff_to_target) < arrive_thresh
        || M_NavIsMaximallyClose(s_map, layer, xz_pos, flock->target_xz, arrive_thresh)) {

            entity_finish_moving(ent, STATE_ARRIVED);
            break;
        }

        struct entity *adjacent[kh_size(flock->ents)];
        size_t num_adj = adjacent_flock_members(ent, flock, adjacent);

        bool done = false;
        for(int j = 0; j < num_adj; j++) {

            struct movestate *adj_ms = movestate_get(adjacent[j]);
            assert(adj_ms);

            if(adj_ms->state == STATE_ARRIVED) {

                entity_finish_moving(ent, STATE_ARRIVED);
                done = true;
                break;
            }
        }

        if(done)
            break;

        /* If we've not hit a condition to stop or give up but our desired velocity 
         * is zero, that means the navigation system is currently not able to guide
         * the entity any closer to its' goal. Stop and wait, re-requesting the  path 
         * after some time. 
         */
        if(PFM_Vec2_Len(&ms->vdes) < EPSILON) {

            assert(flock_for_ent(ent));
            entity_finish_moving(ent, STATE_WAITING);
            break;
        }
        break;
    }
    case STATE_SEEK_ENEMIES: {

        if(PFM_Vec2_Len(&ms->vdes) < EPSILON) {

            entity_finish_moving(ent, STATE_WAITING);
        }
        break;
    }
    case STATE_SURROUND_ENTITY: {

        struct entity *target = G_EntityForUID(ms->surround_target_uid);
        if(!target) {
            entity_finish_moving(ent, STATE_ARRIVED);
            break;
        }

        if(Entity_MaybeAdjacentFast(ent, target, 10.0f) 
        && M_NavObjAdjacent(s_map, ent, target)) {
            entity_finish_moving(ent, STATE_ARRIVED);
            break;
        }

        vec2_t dest;
        vec2_t target_pos = G_Pos_GetXZ(ms->surround_target_uid);
        bool hasdest = M_NavClosestReachableAdjacentPos(s_map, layer, 
            G_Pos_GetXZ(ent->uid), target, &dest);

        if(!hasdest) {
            entity_finish_moving(ent, STATE_ARRIVED);
            break;
        }

        struct flock *flock = flock_for_ent(ent);
        assert(flock);

        vec2_t diff;
        PFM_Vec2_Sub(&flock->target_xz, &dest, &diff);

        if(PFM_Vec2_Len(&diff) > EPSILON) {
            G_Move_SetDest(ent, dest, false);
            ms->state = STATE_SURROUND_ENTITY;
            break;
        }
        break;
    }
    case STATE_ENTER_ENTITY_RANGE: {

        struct entity *target = G_EntityForUID(ms->surround_target_uid);
        if(!target) {
            entity_finish_moving(ent, STATE_ARRIVED);
            break;
        }

        vec2_t xz_pos = G_Pos_GetXZ(ent->uid);
        vec2_t xz_target = G_Pos_GetXZ(ms->surround_target_uid);

        vec2_t delta;
        PFM_Vec2_Sub(&xz_pos, &xz_target, &delta);

        if(PFM_Vec2_Len(&delta) <= ms->target_range
        || M_NavIsMaximallyClose(s_map, layer, xz_pos, xz_target, 0.0f)) {
        
            entity_finish_moving(ent, STATE_ARRIVED);
            break;
        }

        vec2_t target_delta;
        PFM_Vec2_Sub(&xz_target, &ms->target_prev_pos, &target_delta);

        if(PFM_Vec2_Len(&target_delta) > 5.0f) {
            G_Move_SetDest(ent, xz_target, false);
            ms->state = STATE_ENTER_ENTITY_RANGE;
            ms->target_prev_pos = xz_target;
        }

        break;
    }
    case STATE_TURNING: {

        /* find the angle between the two quaternions */
        quat_t ent_rot = Entity_GetRot(ent->uid);
        float angle_diff = PFM_Quat_PitchDiff(&ent_rot, &ms->target_dir);
        float degrees = RAD_TO_DEG(angle_diff);

        /* if it's within a tolerance, stop turning */
        if(fabs(degrees) <= 5.0f) {
            entity_finish_moving(ent, STATE_ARRIVED);
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
        Entity_SetRot(ent->uid, final);

        break;
    }
    case STATE_WAITING: {

        assert(ms->wait_ticks_left > 0);
        ms->wait_ticks_left--;
        if(ms->wait_ticks_left == 0) {

            assert(ms->wait_prev == STATE_MOVING 
                || ms->wait_prev == STATE_SEEK_ENEMIES);

            entity_unblock(ent);
            E_Entity_Notify(EVENT_MOTION_START, ent->uid, NULL, ES_ENGINE);
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

static void find_neighbours(const struct entity *ent,
                            vec_cp_ent_t *out_dyn,
                            vec_cp_ent_t *out_stat)
{
    /* For the ClearPath algorithm, we only consider entities with
     * ENTITY_FLAG_MOVABLE set, as they are the only ones that may need
     * to be avoided during moving. Here, 'static' entites refer
     * to those entites that are not currently in a 'moving' state,
     * meaning they will not perform collision avoidance maneuvers of
     * their own. */

    struct entity *near_ents[512];
    int num_near = G_Pos_EntsInCircle(G_Pos_GetXZ(ent->uid), 
        CLEARPATH_NEIGHBOUR_RADIUS, near_ents, ARR_SIZE(near_ents));

    for(int i = 0; i < num_near; i++) {
        struct entity *curr = near_ents[i];

        if(curr->uid == ent->uid)
            continue;

        if(!(curr->flags & ENTITY_FLAG_MOVABLE))
            continue;

        if(G_GetSelectionRadius(curr->uid) == 0.0f)
            continue;

        struct movestate *ms = movestate_get(curr);
        assert(ms);

        vec2_t curr_xz_pos = G_Pos_GetXZ(curr->uid);
        struct cp_ent newdesc = (struct cp_ent) {
            .xz_pos = curr_xz_pos,
            .xz_vel = ms->velocity,
            .radius = G_GetSelectionRadius(curr->uid)
        };

        if(ent_still(ms))
            vec_cp_ent_push(out_stat, newdesc);
        else
            vec_cp_ent_push(out_dyn, newdesc);
    }
}

static void disband_empty_flocks(void)
{
    uint32_t key;
    struct entity *curr;
    (void)key;

    /* Iterate vector backwards so we can delete entries while iterating. */
    for(int i = vec_size(&s_flocks)-1; i >= 0; i--) {

        /* First, decide if we can disband this flock */
        bool disband = true;
        kh_foreach(vec_AT(&s_flocks, i).ents, key, curr, {

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
}

static void *cp_vec_realloc(void *ptr, size_t size)
{
    if(!ptr)
        return stalloc(&s_cp_work.mem, size);

    void *ret = stalloc(&s_cp_work.mem, size);
    if(!ret)
        return NULL;

    assert(size % 2 == 0);
    memcpy(ret, ptr, size / 2);
    return ret;
}

static void cp_vec_free(void *ptr)
{
    /* no-op */
}

static void clearpath_work(int begin_idx, int end_idx)
{
    for(int i = begin_idx; i <= end_idx; i++) {
    
        struct cp_work_in *in = &s_cp_work.in[i];
        struct cp_work_out *out = &s_cp_work.out[i];

        vec2_t new_vel = G_ClearPath_NewVelocity(in->ent, in->ent_uid, 
            in->ent_des_v, in->dyn_neighbs, in->stat_neighbs, in->save_debug);
        out->ent_uid = in->ent_uid;
        out->ent_vel = new_vel;
    }
}

static struct result clearpath_task(void *arg)
{
    struct cp_task_arg *cp_arg = arg;
    size_t ncomputed = 0;

    for(int i = cp_arg->begin_idx; i <= cp_arg->end_idx; i++) {

        struct cp_work_in *in = &s_cp_work.in[i];
        struct cp_work_out *out = &s_cp_work.out[i];

        vec2_t new_vel = G_ClearPath_NewVelocity(in->ent, in->ent_uid, 
            in->ent_des_v, in->dyn_neighbs, in->stat_neighbs, in->save_debug);
        out->ent_uid = in->ent_uid;
        out->ent_vel = new_vel;

        ncomputed++;
        if(ncomputed % 64 == 0)
            Task_Yield();
    }
    return NULL_RESULT;
}

static void clearpath_run_to_completion(int idx)
{
    while(!Sched_FutureIsReady(&s_cp_work.futures[idx])) {
        Sched_RunSync(s_cp_work.tids[idx]);
    }
}

static void clearpath_finish_work(void)
{
    PERF_ENTER();

    if(s_cp_work.nwork == 0)
        PERF_RETURN_VOID();

    for(int i = 0; i < s_cp_work.ntasks; i++) {
        clearpath_run_to_completion(i);    
    }

    Perf_Push("velocity updates");
    for(int i = 0; i < s_cp_work.nwork; i++) {

        struct entity *ent = G_EntityForUID(s_cp_work.out[i].ent_uid);
        assert(ent);

        struct movestate *ms = movestate_get(ent);
        assert(ms);

        ms->vnew = s_cp_work.out[i].ent_vel;
        update_vel_hist(ms, ms->vnew);

        vec2_t vel_diff;
        PFM_Vec2_Sub(&ms->vnew, &ms->velocity, &vel_diff);

        PFM_Vec2_Add(&ms->velocity, &vel_diff, &ms->vnew);
        vec2_truncate(&ms->vnew, ms->max_speed / MOVE_TICK_RES);
    }
    Perf_Pop();

    uint32_t key;
    struct entity *curr;
    (void)key;

    Perf_Push("position updates");
    kh_foreach(G_GetDynamicEntsSet(), key, curr, {

        struct movestate *ms = movestate_get(curr);
        assert(ms);
        entity_update(curr, ms->vnew);
    });
    Perf_Pop();

    stalloc_clear(&s_cp_work.mem);
    s_cp_work.in = NULL;
    s_cp_work.out = NULL;
    s_cp_work.nwork = 0;
    s_cp_work.ntasks = 0;

    PERF_RETURN_VOID();
}

static void clearpath_prepare_work(void)
{
    size_t ndynamic = kh_size(G_GetDynamicEntsSet());
    s_cp_work.in = stalloc(&s_cp_work.mem, ndynamic * sizeof(struct cp_work_in));
    s_cp_work.out = stalloc(&s_cp_work.mem, ndynamic * sizeof(struct cp_work_out));
}

static void clearpath_push_work(struct cp_work_in in)
{
    s_cp_work.in[s_cp_work.nwork++] = in;
}

static void clearpath_submit_work(void)
{
    if(s_cp_work.nwork == 0)
        return;

    size_t ntasks = SDL_GetCPUCount();
    if(s_cp_work.nwork < 64)
        ntasks = 1;
    ntasks = MIN(ntasks, MAX_CP_TASKS);

    for(int i = 0; i < ntasks; i++) {

        struct cp_task_arg *arg = stalloc(&s_cp_work.mem, sizeof(struct cp_task_arg));
        size_t nitems = ceil((float)s_cp_work.nwork / ntasks);

        arg->begin_idx = nitems * i;
        arg->end_idx = MIN(nitems * (i + 1) - 1, s_cp_work.nwork-1);

        SDL_AtomicSet(&s_cp_work.futures[s_cp_work.ntasks].status, FUTURE_INCOMPLETE);
        s_cp_work.tids[s_cp_work.ntasks] = Sched_Create(4, clearpath_task, arg, 
            &s_cp_work.futures[s_cp_work.ntasks], TASK_BIG_STACK);

        if(s_cp_work.tids[s_cp_work.ntasks] == NULL_TID) {
            clearpath_work(arg->begin_idx, arg->end_idx);
        }else{
            s_cp_work.ntasks++;
        }
    }
}

static void on_20hz_tick(void *user, void *event)
{
    Perf_Push("movement::on_20hz_tick");

    uint32_t key;
    struct entity *curr;

    disband_empty_flocks();
    clearpath_prepare_work();

    kh_foreach(G_GetDynamicEntsSet(), key, curr, {

        struct movestate *ms = movestate_get(curr);
        assert(ms);

        if(ent_still(ms))
            continue;

        struct flock *flock = flock_for_ent(curr);
        vec2_t vpref = (vec2_t){-1,-1};
        ms->vdes = ent_desired_velocity(curr);

        switch(ms->state) {
        case STATE_TURNING:
            vpref = (vec2_t){0.0f, 0.0f};
            break;
        case STATE_SEEK_ENEMIES: 
            assert(!flock);
            vpref = enemy_seek_vpref(curr);
            break;
        default:
            assert(flock);
            vpref = point_seek_vpref(curr, flock);
        }
        assert(vpref.x != -1 || vpref.z != -1);

        struct cp_ent curr_cp = (struct cp_ent) {
            .xz_pos = G_Pos_GetXZ(curr->uid),
            .xz_vel = ms->velocity,
            .radius = G_GetSelectionRadius(curr->uid)
        };

        vec_cp_ent_t *dyn, *stat;
        dyn = stalloc(&s_cp_work.mem, sizeof(vec_cp_ent_t));
        stat = stalloc(&s_cp_work.mem, sizeof(vec_cp_ent_t));

        vec_cp_ent_init_alloc(dyn, cp_vec_realloc, cp_vec_free);
        vec_cp_ent_init_alloc(stat, cp_vec_realloc, cp_vec_free);

        find_neighbours(curr, dyn, stat);

        assert(vpref.x != NAN && vpref.z != NAN);
        clearpath_push_work((struct cp_work_in){
            .ent = curr_cp,
            .ent_uid = key,
            .ent_des_v = vpref,
            .dyn_neighbs = *dyn,
            .stat_neighbs = *stat,
            .save_debug = G_ClearPath_ShouldSaveDebug(key)
        });
    });

    clearpath_submit_work();
    clearpath_finish_work();

    Perf_Pop();
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

    if(!stalloc_init(&s_cp_work.mem)) {
        kh_destroy(state, s_entity_state_table);
        return NULL;
    }

    vec_pentity_init(&s_move_markers);
    vec_flock_init(&s_flocks);

    E_Global_Register(SDL_MOUSEBUTTONDOWN, on_mousedown, NULL, G_RUNNING);
    E_Global_Register(EVENT_RENDER_3D_POST, on_render_3d, NULL, G_RUNNING | G_PAUSED_FULL | G_PAUSED_UI_RUNNING);
    E_Global_Register(EVENT_20HZ_TICK, on_20hz_tick, NULL, G_RUNNING);

    s_map = map;
    s_attack_on_lclick = false;
    s_move_on_lclick = false;
    return true;
}

void G_Move_Shutdown(void)
{
    s_map = NULL;

    E_Global_Unregister(EVENT_20HZ_TICK, on_20hz_tick);
    E_Global_Unregister(EVENT_RENDER_3D_POST, on_render_3d);
    E_Global_Unregister(SDL_MOUSEBUTTONDOWN, on_mousedown);

    for(int i = 0; i < vec_size(&s_move_markers); i++) {
        E_Entity_Unregister(EVENT_ANIM_FINISHED, vec_AT(&s_move_markers, i)->uid, on_marker_anim_finish);
        G_RemoveEntity(vec_AT(&s_move_markers, i));
        G_SafeFree(vec_AT(&s_move_markers, i));
    }

    vec_flock_destroy(&s_flocks);
    vec_pentity_destroy(&s_move_markers);
    stalloc_destroy(&s_cp_work.mem);
    kh_destroy(state, s_entity_state_table);
}

void G_Move_AddEntity(const struct entity *ent)
{
    struct movestate new_ms = (struct movestate) {
        .velocity = {0.0f}, 
        .blocking = false,
        .state = STATE_ARRIVED,
        .vel_hist_idx = 0,
        .vnew = (vec2_t){0.0f, 0.0f},
        .max_speed = 0.0f,
    };
    memset(new_ms.vel_hist, 0, sizeof(new_ms.vel_hist));

    int ret;
    khiter_t k = kh_put(state, s_entity_state_table, ent->uid, &ret);
    assert(ret != -1 && ret != 0);
    kh_value(s_entity_state_table, k) = new_ms;

    entity_block(ent);
}

void G_Move_RemoveEntity(const struct entity *ent)
{
    khiter_t k = kh_get(state, s_entity_state_table, ent->uid);
    if(k == kh_end(s_entity_state_table))
        return;

    G_Move_Stop(ent);
    entity_unblock(ent);

    kh_del(state, s_entity_state_table, k);
}

void G_Move_Stop(const struct entity *ent)
{
    struct movestate *ms = movestate_get(ent);
    if(!ms)
        return;

    if(!ent_still(ms)) {
        entity_finish_moving(ent, STATE_ARRIVED);
    }

    remove_from_flocks(ent);
    ms->state = STATE_ARRIVED;
}

bool G_Move_GetDest(const struct entity *ent, vec2_t *out_xz, bool *out_attack)
{
    struct flock *fl = flock_for_ent(ent);
    if(!fl)
        return false;
    *out_xz = fl->target_xz;
    *out_attack = N_DestIDIsAttacking(fl->dest_id);
    return true;
}

bool G_Move_GetSurrounding(const struct entity *ent, uint32_t *out_uid)
{
    struct movestate *ms = movestate_get(ent);
    assert(ms);
    if(ms->state != STATE_SURROUND_ENTITY)
        return false;
    *out_uid = ms->surround_target_uid;
    return true;
}

bool G_Move_Still(const struct entity *ent)
{
    struct movestate *ms = movestate_get(ent);
    if(!ms)
        return true;
    return (ms->state == STATE_ARRIVED);
}

void G_Move_SetDest(const struct entity *ent, vec2_t dest_xz, bool attack)
{
    enum nav_layer layer = Entity_NavLayer(ent);
    dest_xz = M_NavClosestReachableDest(s_map, layer, G_Pos_GetXZ(ent->uid), dest_xz);

    /* If a flock already exists for the entity's destination, 
     * simply add the entity to the flock. If necessary, the
     * right flow fields will be computed on-demand during the
     * next movement update. 
     */
    dest_id_t dest_id;
    if(attack) {
        dest_id = M_NavDestIDForPosAttacking(s_map, dest_xz, layer, G_GetFactionID(ent->uid));
    }else{
        dest_id = M_NavDestIDForPos(s_map, dest_xz, layer);
    }
    struct flock *fl = flock_for_dest(dest_id);

    if(fl && fl == flock_for_ent(ent))
        return;

    if(fl) {

        assert(fl != flock_for_ent(ent));
        remove_from_flocks(ent);
        flock_add(fl, ent);

        struct movestate *ms = movestate_get(ent);
        assert(ms);
        if(ent_still(ms)) {
            entity_unblock(ent);
            E_Entity_Notify(EVENT_MOTION_START, ent->uid, NULL, ES_ENGINE);
        }
        ms->state = STATE_MOVING;
        assert(flock_for_ent(ent));
        return;
    }

    /* Else, create a new flock and request a path for it.
     */
    vec_pentity_t flock;
    vec_pentity_init(&flock);
    vec_pentity_push(&flock, (struct entity*)ent);

    make_flock(&flock, dest_xz, Entity_NavLayer(ent), attack);
    vec_pentity_destroy(&flock);
}

void G_Move_SetChangeDirection(const struct entity *ent, quat_t target)
{
    struct movestate *ms = movestate_get(ent);
    assert(ms);

    if(ent_still(ms)) {
        entity_unblock(ent);
        E_Entity_Notify(EVENT_MOTION_START, ent->uid, NULL, ES_ENGINE);
    }

    ms->state = STATE_TURNING;
    ms->target_dir = target;
}

void G_Move_SetEnterRange(const struct entity *ent, const struct entity *target, float range)
{
    struct movestate *ms = movestate_get(ent);
    assert(ms);

    vec2_t xz_src = G_Pos_GetXZ(ent->uid);
    vec2_t xz_dst = G_Pos_GetXZ(target->uid);
    range = MAX(0.0f, range - G_GetSelectionRadius(ent->uid));

    vec2_t delta;
    PFM_Vec2_Sub(&xz_src, &xz_dst, &delta);
    if(PFM_Vec2_Len(&delta) <= range) {
        G_Move_Stop(ent);
        return;
    }

    vec2_t xz_target = M_NavClosestReachableInRange(s_map, Entity_NavLayer(ent), xz_src, xz_dst, range);
    G_Move_SetDest(ent, xz_target, false);

    ms->state = STATE_ENTER_ENTITY_RANGE;
    ms->surround_target_uid = target->uid;
    ms->target_prev_pos = xz_dst;
    ms->target_range = range;
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

void G_Move_SetSeekEnemies(const struct entity *ent)
{
    struct movestate *ms = movestate_get(ent);
    assert(ms);

    remove_from_flocks(ent);

    if(ent_still(ms)) {
        entity_unblock(ent);
        E_Entity_Notify(EVENT_MOTION_START, ent->uid, NULL, ES_ENGINE);
    }

    ms->state = STATE_SEEK_ENEMIES;
}

void G_Move_SetSurroundEntity(const struct entity *ent, const struct entity *target)
{
    struct movestate *ms = movestate_get(ent);
    assert(ms);

    G_Move_Stop(ent);

    vec2_t pos = G_Pos_GetXZ(target->uid);
    G_Move_SetDest(ent, pos, false);

    assert(!ms->blocking);
    ms->state = STATE_SURROUND_ENTITY;
    ms->surround_target_uid = target->uid;
}

void G_Move_UpdatePos(const struct entity *ent, vec2_t pos)
{
    struct movestate *ms = movestate_get(ent);
    if(!ms)
        return;

    if(!ms->blocking)
        return;

    M_NavBlockersDecref(ms->last_stop_pos, ms->last_stop_radius, G_GetFactionID(ent->uid), s_map);
    M_NavBlockersIncref(pos, ms->last_stop_radius, G_GetFactionID(ent->uid), s_map);
    ms->last_stop_pos = pos;
}

void G_Move_UpdateFactionID(const struct entity *ent, int oldfac, int newfac)
{
    struct movestate *ms = movestate_get(ent);
    if(!ms)
        return;

    if(!ms->blocking)
        return;

    M_NavBlockersDecref(ms->last_stop_pos, ms->last_stop_radius, oldfac, s_map);
    M_NavBlockersIncref(ms->last_stop_pos, ms->last_stop_radius, newfac, s_map);
}

void G_Move_UpdateSelectionRadius(const struct entity *ent, float sel_radius)
{
    struct movestate *ms = movestate_get(ent);
    if(!ms)
        return;

    if(!ms->blocking)
        return;

    M_NavBlockersDecref(ms->last_stop_pos, ms->last_stop_radius, G_GetFactionID(ent->uid), s_map);
    M_NavBlockersIncref(ms->last_stop_pos, sel_radius, G_GetFactionID(ent->uid), s_map);
    ms->last_stop_radius = sel_radius;
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
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return false;
    struct movestate *ms = &kh_value(s_entity_state_table, k);
    *out = ms->max_speed;
    return true;
}

bool G_Move_SetMaxSpeed(uint32_t uid, float speed)
{
    khiter_t k = kh_get(state, s_entity_state_table, uid);
    if(k == kh_end(s_entity_state_table))
        return false;
    struct movestate *ms = &kh_value(s_entity_state_table, k);
    ms->max_speed = speed;
    return true;
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

    for(int i = 0; i < vec_size(&s_flocks); i++) {

        const struct flock *curr_flock = &vec_AT(&s_flocks, i);

        struct attr num_flock_ents = (struct attr){
            .type = TYPE_INT,
            .val.as_int = kh_size(curr_flock->ents)
        };
        CHK_TRUE_RET(Attr_Write(stream, &num_flock_ents, "num_flock_ents"));

        uint32_t uid;
        struct entity *curr_ent;
        (void)curr_ent;

        kh_foreach(curr_flock->ents, uid, curr_ent, {
        
            struct attr flock_ent = (struct attr){
                .type = TYPE_INT,
                .val.as_int = uid
            };
            CHK_TRUE_RET(Attr_Write(stream, &flock_ent, "flock_ent"));
        });

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
    }

    /* save the movement state */
    struct attr num_ents = (struct attr){
        .type = TYPE_INT,
        .val.as_int = kh_size(s_entity_state_table)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_ents, "num_ents"));

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
    });

    return true;
}

bool G_Move_LoadState(struct SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_BOOL);
    s_click_move_enabled = attr.val.as_bool;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const int num_flocks = attr.val.as_int;

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

            uint32_t flock_end_uid = attr.val.as_int;
            const struct entity *ent = G_EntityForUID(flock_end_uid);

            CHK_TRUE_JMP(ent, fail_flock);
            flock_add(&new_flock, ent);
        }

        CHK_TRUE_JMP(Attr_Parse(stream, &attr, true), fail_flock);
        CHK_TRUE_JMP(attr.type == TYPE_VEC2, fail_flock);
        new_flock.target_xz = attr.val.as_vec2;

        CHK_TRUE_JMP(Attr_Parse(stream, &attr, true), fail_flock);
        CHK_TRUE_JMP(attr.type == TYPE_INT, fail_flock);
        new_flock.dest_id = attr.val.as_int;

        vec_flock_push(&s_flocks, new_flock);
        continue;

    fail_flock:
        kh_destroy(entity, new_flock.ents);
        return false;
    }

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const int num_ents = attr.val.as_int;

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
            const struct entity *ent = G_EntityForUID(uid);
            assert(ent);
            entity_unblock(ent);
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
        ms->target_prev_pos = attr.val.as_vec2;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_FLOAT);
        ms->target_range = attr.val.as_float;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_QUAT);
        ms->target_dir = attr.val.as_quat;
    }

    return true;
}

