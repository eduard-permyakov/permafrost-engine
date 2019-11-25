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
#include "../collision.h"
#include "../cursor.h"
#include "../settings.h"
#include "../script/public/script.h"
#include "../render/public/render.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"
#include "../lib/public/vec.h"
#include "../anim/public/anim.h"

#include <assert.h>
#include <SDL.h>


/* For the purpose of movement simulation, all entities have the same mass,
 * meaning they are accelerate the same amount when applied equal forces. */
#define ENTITY_MASS  (1.0f)
#define EPSILON      (1.0f/1024)
#define MAX_FORCE    (0.5f)

#define SIGNUM(x)    (((x) > 0) - ((x) < 0))
#define MIN(a, b)    ((a) < (b) ? (a) : (b))
#define ARR_SIZE(a)  (sizeof(a)/sizeof(a[0]))

#define VEL_HIST_LEN (16)

enum arrival_state{
    /* Entity is moving towards the flock's destination point */
    STATE_MOVING,
    /* Entity is in proximity of the flock's destination point, 
     * it is looking for a good point to stop. */
    STATE_SETTLING,
    /* Entity is considered to have arrived and no longer moving. */
    STATE_ARRIVED,
};

struct movestate{
    vec2_t             vnew;
    vec2_t             velocity;
    enum arrival_state state;
    vec2_t             vel_hist[VEL_HIST_LEN];
    int                vel_hist_idx;
};

KHASH_MAP_INIT_INT(state, struct movestate)

struct flock{
    khash_t(entity) *ents;
    vec2_t           target_xz; 
    dest_id_t        dest_id;
};

VEC_TYPE(flock, struct flock)
VEC_IMPL(static inline, flock, struct flock)

/* Parameters controlling steering/flocking behaviours */
#define SEPARATION_FORCE_SCALE          (0.5f)
#define MOVE_ARRIVE_FORCE_SCALE         (0.5f)
#define MOVE_COHESION_FORCE_SCALE       (0.15f)

#define ARRIVE_THRESHOLD_DIST           (5.0f)
#define SEPARATION_BUFFER_DIST          (5.0f)
#define COHESION_NEIGHBOUR_RADIUS       (50.0f)
#define ARRIVE_SLOWING_RADIUS           (10.0f)
#define ADJACENCY_SEP_DIST              (5.0f)
#define ALIGN_NEIGHBOUR_RADIUS          (10.0f)

#define SETTLE_STOP_TOLERANCE           (0.1f)
#define COLLISION_MAX_SEE_AHEAD         (10.0f)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map       *s_map;
static bool                    s_attack_on_lclick = false;
static bool                    s_move_on_lclick = false;

static vec_pentity_t          s_move_markers;
static vec_flock_t            s_flocks;
static khash_t(state)         *s_entity_state_table;

/* Store the most recently issued move command location for debug rendering */
static bool                    s_last_cmd_dest_valid = false;
static dest_id_t               s_last_cmd_dest;

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
    if(kh_exist(flock->ents, k))
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

static bool stationary(const struct entity *ent)
{
    return (ent->flags & ENTITY_FLAG_STATIC) || (ent->max_speed == 0.0f);
}

static bool entities_equal(struct entity **a, struct entity **b)
{
    return (0 == memcmp(*a, *b, sizeof(struct entity)));
}

static void vec2_truncate(vec2_t *inout, float max_len)
{
    if(PFM_Vec2_Len(inout) > max_len) {

        PFM_Vec2_Normal(inout, inout);
        PFM_Vec2_Scale(inout, max_len, inout);
    }
}

static void entity_finish_moving(const struct entity *ent)
{
    E_Entity_Notify(EVENT_MOTION_END, ent->uid, NULL, ES_ENGINE);
    if(ent->flags & ENTITY_FLAG_COMBATABLE)
        G_Combat_SetStance(ent, COMBAT_STANCE_AGGRESSIVE);
}

static void on_marker_anim_finish(void *user, void *event)
{
    int idx;
    struct entity *ent = user;
    assert(ent);

    vec_pentity_indexof(&s_move_markers, ent, entities_equal, &idx);
    assert(idx != -1);
    vec_pentity_del(&s_move_markers, idx);

    E_Entity_Unregister(EVENT_ANIM_FINISHED, ent->uid, on_marker_anim_finish);
    G_RemoveEntity(ent);
    AL_EntityFree(ent);
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

static bool make_flock_from_selection(const vec_pentity_t *sel, vec2_t target_xz, bool attack)
{
    if(vec_size(sel) == 0)
        return false;

    /* The following won't be optimal when the entities in the selection are on different 
     * 'islands'. Handling that case is not a top priority. 
     */
    vec2_t first_ent_pos_xz = G_Pos_GetXZ(vec_AT(sel, 0)->uid);
    target_xz = M_NavClosestReachableDest(s_map, first_ent_pos_xz, target_xz);

    /* First remove the entities in the selection from any active flocks */
    for(int i = 0; i < vec_size(sel); i++) {

        const struct entity *curr_ent = vec_AT(sel, i);
        if(stationary(curr_ent))
            continue;
        /* Remove any flocks which may have become empty. Iterate vector in backwards order 
         * so that we can delete while iterating, since the last element in the vector takes
         * the place of the deleted one. 
         */
        for(int j = vec_size(&s_flocks)-1; j >= 0; j--) {

            khiter_t k;
            struct flock *curr_flock = &vec_AT(&s_flocks, j);
            flock_try_remove(curr_flock, curr_ent);

            if(kh_size(curr_flock->ents) == 0) {
                kh_destroy(entity, curr_flock->ents);
                vec_flock_del(&s_flocks, j);
            }
        }
    }

    struct flock new_flock = (struct flock) {
        .ents = kh_init(entity),
        .target_xz = target_xz,
    };

    if(!new_flock.ents)
        return false;

    /* Don't request a new path (flow field) for an entity that is on the same
     * chunk as another entity for which a path has already been requested. This 
     * allows saving pathfinding cycles. In the case that an entity is on a different
     * 'island' of the chunk than the one for which the flow field has been computed,
     * the FF for this 'island' will be computed on demand. 
     */
    struct tile_desc pathed_ents_descs[vec_size(sel)];
    size_t num_pathed_ents = 0;

    for(int i = 0; i < vec_size(sel); i++) {

        const struct entity *curr_ent = vec_AT(sel, i);
        struct movestate *ms = movestate_get(curr_ent);
        assert(ms);

        if(stationary(curr_ent))
            continue;

        struct tile_desc curr_desc;
        M_DescForPoint2D(s_map, G_Pos_GetXZ(curr_ent->uid), &curr_desc);

        if(same_chunk_as_any_in_set(curr_desc, pathed_ents_descs, num_pathed_ents)
        || M_NavRequestPath(s_map, G_Pos_GetXZ(curr_ent->uid), target_xz, &new_flock.dest_id)) {

            pathed_ents_descs[num_pathed_ents++] = curr_desc;
            flock_add(&new_flock, curr_ent);

            /* When entities are moved from one flock to another, they keep their existing velocity.*/
            if(ms->state == STATE_ARRIVED) {
                E_Entity_Notify(EVENT_MOTION_START, curr_ent->uid, NULL, ES_ENGINE);
            }
            ms->state = STATE_MOVING;

        }else {

            if(ms->state != STATE_ARRIVED) {
                entity_finish_moving(curr_ent);
            }
            *ms = (struct movestate) {
                .state = STATE_ARRIVED,
                .velocity = (vec2_t){0.0f},
            };
        }
    }

    if(kh_size(new_flock.ents) > 0) {

        /* If there is another flock with the same dest_id, then we merge the two flocks. */
        struct flock *merge_flock = flock_for_dest(new_flock.dest_id);
        if(merge_flock) {

            uint32_t key;
            struct entity *curr;
            kh_foreach(new_flock.ents, key, curr, { flock_add(merge_flock, curr); });
            kh_destroy(entity, new_flock.ents);
        
        }else{
            vec_flock_push(&s_flocks, new_flock);
        }

        s_last_cmd_dest_valid = true;
        s_last_cmd_dest = new_flock.dest_id;

        return true;
    }else{
        kh_destroy(entity, new_flock.ents);
        return false;
    }
}

size_t adjacent_flock_members(const struct entity *ent, const struct flock *flock, 
                              struct entity *out[])
{
    vec2_t ent_xz_pos = G_Pos_GetXZ(ent->uid);
    size_t ret = 0;

    uint32_t key;
    struct entity *curr;
    kh_foreach(flock->ents, key, curr, {

        if(curr == ent)
            continue;

        vec2_t curr_xz_pos = G_Pos_GetXZ(curr->uid);
        vec2_t diff;
        PFM_Vec2_Sub(&ent_xz_pos, &curr_xz_pos, &diff);

        if(PFM_Vec2_Len(&diff) <= ent->selection_radius + curr->selection_radius + ADJACENCY_SEP_DIST)
            out[ret++] = curr;  
    });
    return ret;
}

static void move_marker_add(vec3_t pos, bool attack)
{
    extern const char *g_basepath;
    char path[256];
    strcpy(path, g_basepath);
    strcat(path, "assets/models/arrow");

    struct entity *ent = attack ? AL_EntityFromPFObj(path, "arrow-red.pfobj", "__move_marker__") 
                                : AL_EntityFromPFObj(path, "arrow-green.pfobj", "__move_marker__");
    assert(ent);
    ent->flags |= ENTITY_FLAG_STATIC;
    G_AddEntity(ent, pos);

    ent->scale = (vec3_t){2.0f, 2.0f, 2.0f};
    E_Entity_Register(EVENT_ANIM_FINISHED, ent->uid, on_marker_anim_finish, ent, G_RUNNING);

    A_InitCtx(ent, "Converge", 48);
    A_SetActiveClip(ent, "Converge", ANIM_MODE_ONCE_HIDE_ON_FINISH, 48);

    vec_pentity_push(&s_move_markers, ent);
}

static void on_mousedown(void *user, void *event)
{
    SDL_MouseButtonEvent *mouse_event = &(((SDL_Event*)event)->button);

    assert(!s_move_on_lclick || !s_attack_on_lclick);
    bool attack = s_attack_on_lclick && (mouse_event->button == SDL_BUTTON_LEFT);
    bool move = s_move_on_lclick ? mouse_event->button == SDL_BUTTON_LEFT
                                 : mouse_event->button == SDL_BUTTON_RIGHT;
    assert(!attack || !move);

    s_attack_on_lclick = false;
    s_move_on_lclick = false;
    Cursor_SetRTSPointer(CURSOR_POINTER);

    if(G_MouseOverMinimap())
        return;

    if(S_UI_MouseOverWindow(mouse_event->x, mouse_event->y))
        return;

    if(!attack && !move)
        return;

    vec3_t mouse_coord;
    if(!M_Raycast_IntersecCoordinate(&mouse_coord))
        return;

    enum selection_type sel_type;
    const vec_pentity_t *sel = G_Sel_Get(&sel_type);
    if(vec_size(sel) > 0 && sel_type == SELECTION_TYPE_PLAYER) {

        for(int i = 0; i < vec_size(sel); i++) {

            const struct entity *curr = vec_AT(sel, i);
            if(!(curr->flags & ENTITY_FLAG_COMBATABLE))
                continue;

            G_Combat_ClearSavedMoveCmd(curr);
            G_Combat_SetStance(curr, attack ? COMBAT_STANCE_AGGRESSIVE : COMBAT_STANCE_NO_ENGAGEMENT);
        }

        move_marker_add(mouse_coord, attack);
        make_flock_from_selection(sel, (vec2_t){mouse_coord.x, mouse_coord.z}, attack);
    }
}

static void on_render_3d(void *user, void *event)
{
    struct sval setting;
    ss_e status = Settings_Get("pf.debug.show_last_cmd_flow_field", &setting);
    assert(status == SS_OKAY);

    if(setting.as_bool && s_last_cmd_dest_valid) {

        const struct camera *cam = G_GetActiveCamera();
        M_NavRenderVisiblePathFlowField(s_map, cam, s_last_cmd_dest);
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

/* Seek behaviour makes the entity target and approach a particular destination point.
 */
static vec2_t seek_force(const struct entity *ent, vec2_t target_xz)
{
    vec2_t ret, desired_velocity;
    vec2_t pos_xz = G_Pos_GetXZ(ent->uid);

    PFM_Vec2_Sub(&target_xz, &pos_xz, &desired_velocity);
    PFM_Vec2_Normal(&desired_velocity, &desired_velocity);
    PFM_Vec2_Scale(&desired_velocity, ent->max_speed / MOVE_TICK_RES, &desired_velocity);

    struct movestate *ms = movestate_get(ent);
    assert(ms);

    PFM_Vec2_Sub(&desired_velocity, &ms->velocity, &ret);
    return ret;
}

/* Arrival behaviour is like 'seek' but the entity decelerates and comes to a halt when it is 
 * within a threshold radius of the destination point.
 * 
 * When not within line of sight of the destination, this will steer the entity along the 
 * flow field.
 */
static vec2_t arrive_force(const struct entity *ent, const struct flock *flock)
{
    assert(0 == (ent->flags & ENTITY_FLAG_STATIC));
    vec2_t ret, desired_velocity;
    vec2_t pos_xz = G_Pos_GetXZ(ent->uid);
    float distance;

    struct movestate *ms = movestate_get(ent);
    assert(ms);

    if(M_NavHasDestLOS(s_map, flock->dest_id, pos_xz)) {

        PFM_Vec2_Sub((vec2_t*)&flock->target_xz, &pos_xz, &desired_velocity);
        distance = PFM_Vec2_Len(&desired_velocity);
        PFM_Vec2_Normal(&desired_velocity, &desired_velocity);
        PFM_Vec2_Scale(&desired_velocity, ent->max_speed / MOVE_TICK_RES, &desired_velocity);

        if(distance < ARRIVE_SLOWING_RADIUS) {
            PFM_Vec2_Scale(&desired_velocity, distance / ARRIVE_SLOWING_RADIUS, &desired_velocity);
        }

    }else{

        desired_velocity = M_NavDesiredVelocity(s_map, flock->dest_id, pos_xz, flock->target_xz);
        PFM_Vec2_Scale(&desired_velocity, ent->max_speed / MOVE_TICK_RES, &desired_velocity);
    }

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
        ent->selection_radius + buffer_dist, near_ents, ARR_SIZE(near_ents));

    for(int i = 0; i < num_near; i++) {

        struct entity *curr = near_ents[i];
        if(curr == ent)
            continue;
        if(curr->flags & ENTITY_FLAG_STATIC)
            continue;

        vec2_t diff;
        vec2_t ent_xz_pos = G_Pos_GetXZ(ent->uid);
        vec2_t curr_xz_pos = G_Pos_GetXZ(curr->uid);

        float radius = ent->selection_radius + curr->selection_radius + buffer_dist;
        PFM_Vec2_Sub(&curr_xz_pos, &ent_xz_pos, &diff);

        /* Exponential decay with y=1 when diff = radius*0.95 
         * Use smooth decay curves in order to curb the 'toggling' or oscillating 
         * behaviour that may arise when there are discontinuities in the forces. 
         */
        float t = (PFM_Vec2_Len(&diff) - radius*0.95) / radius;
        float scale = exp(-5.0f * t);
        PFM_Vec2_Scale(&diff, scale, &diff);

        PFM_Vec2_Add(&ret, &diff, &ret);
    }

    if(0 == num_near)
        return (vec2_t){0.0f};

    PFM_Vec2_Scale(&ret, -1.0f / num_near, &ret);
    vec2_truncate(&ret, MAX_FORCE);
    return ret;
}

static vec2_t total_steering_force(const struct entity *ent, const struct flock *flock)
{
    struct movestate *ms = movestate_get(ent);
    assert(ms);

    vec2_t arrive = arrive_force(ent, flock);
    vec2_t cohesion = cohesion_force(ent, flock);
    vec2_t separation = separation_force(ent, SEPARATION_BUFFER_DIST);

    PFM_Vec2_Scale(&arrive,     MOVE_ARRIVE_FORCE_SCALE,   &arrive);
    PFM_Vec2_Scale(&cohesion,   MOVE_COHESION_FORCE_SCALE, &cohesion);
    PFM_Vec2_Scale(&separation, SEPARATION_FORCE_SCALE,    &separation);

    vec2_t ret = (vec2_t){0.0f};
    switch(ms->state) {
    case STATE_MOVING: {

        PFM_Vec2_Add(&ret, &arrive, &ret);
        PFM_Vec2_Add(&ret, &separation, &ret);
        PFM_Vec2_Add(&ret, &cohesion, &ret);

        break;
    }
    case STATE_SETTLING: {

        PFM_Vec2_Add(&ret, &separation, &ret);

        break;
    }
    case STATE_ARRIVED:
        break;
    default: assert(0);
    }

    vec2_truncate(&ret, MAX_FORCE);

    /* Some forces guide us to impassable terrain. Nullify the components of the force 
     * vector that do this so that the entity is never guided towards impassable terrain. 
     */

    float old_mag = PFM_Vec2_Len(&ret);
    vec2_t nt_dims = N_TileDims();

    vec2_t left =  (vec2_t){G_Pos_Get(ent->uid).x + nt_dims.x, G_Pos_Get(ent->uid).z};
    vec2_t right = (vec2_t){G_Pos_Get(ent->uid).x - nt_dims.x, G_Pos_Get(ent->uid).z};
    vec2_t top =   (vec2_t){G_Pos_Get(ent->uid).x, G_Pos_Get(ent->uid).z + nt_dims.z};
    vec2_t bot =   (vec2_t){G_Pos_Get(ent->uid).x, G_Pos_Get(ent->uid).z - nt_dims.z};

    if((ret.raw[0] > 0 && !M_NavPositionPathable(s_map, left))
    || (ret.raw[0] < 0 && !M_NavPositionPathable(s_map, right)))
        ret.raw[0] = 0.0f;

    if((ret.raw[1] > 0 && !M_NavPositionPathable(s_map, top))
    || (ret.raw[1] < 0 && !M_NavPositionPathable(s_map, bot)))
        ret.raw[1] = 0.0f;

    float new_mag = PFM_Vec2_Len(&ret);
    if(new_mag < EPSILON) {

        /* When both components of the force are truncated due to steering the entity
         * off the pathable terrain, return a very slight flow field following force.
         * This force is guaranteed not to guide the entity off pathable terrain. If
         * we simply return a zero force, the entity can get stuck. The following 
         * guarantees we make eventual progress in those cases.
         */
        vec2_truncate(&arrive, MAX_FORCE * 0.02f);
        return arrive;
    }

    PFM_Vec2_Scale(&ret, old_mag / new_mag, &ret);
    assert(fabs(PFM_Vec2_Len(&ret) - old_mag) < EPSILON);
    return ret;
}

static vec2_t new_pos_for_vel(const struct entity *ent, vec2_t velocity)
{
    vec2_t xz_pos = G_Pos_GetXZ(ent->uid);
    vec2_t new_pos;

    PFM_Vec2_Add(&xz_pos, &velocity, &new_pos);
    return new_pos;
}

static vec2_t calculate_vpref(const struct entity *ent, const struct flock *flock)
{
    vec2_t xz_pos = G_Pos_GetXZ(ent->uid);
    struct movestate *ms = movestate_get(ent);
    assert(ms);

    vec2_t steer_force = total_steering_force(ent, flock);

    vec2_t accel, new_vel, new_pos; 
    PFM_Vec2_Scale(&steer_force, 1.0f / ENTITY_MASS, &accel);

    PFM_Vec2_Add(&ms->velocity, &accel, &new_vel);
    vec2_truncate(&new_vel, ent->max_speed / MOVE_TICK_RES);

    return new_vel;
}

static void update_vel_hist(struct movestate *ms, vec2_t vnew)
{
    assert(ms->vel_hist >= 0 && ms->vel_hist_idx < VEL_HIST_LEN);
    ms->vel_hist[ms->vel_hist_idx] = vnew;
    ms->vel_hist_idx = (++ms->vel_hist_idx % VEL_HIST_LEN);
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

static void entity_update(struct entity *ent, const struct flock *flock, vec2_t new_vel)
{
    struct movestate *ms = movestate_get(ent);
    assert(ms);

    if(ms->state == STATE_ARRIVED)
        return;

    vec2_t new_pos_xz = new_pos_for_vel(ent, new_vel);
    if(M_NavPositionPathable(s_map, new_pos_xz)) {
    
        vec3_t new_pos = (vec3_t){new_pos_xz.x, M_HeightAtPoint(s_map, new_pos_xz), new_pos_xz.z};
        G_Pos_Set(ent->uid, new_pos);
        ms->velocity = new_vel;

        /* Use a weighted average of past velocities ot set the entity's orientation. This means that 
         * the entity's visible orientation lags behind its' true orientation slightly. However, this 
         * greatly smooths the turning of the entity, giving a more natural look to the movemment. 
         */
        vec2_t wma = vel_wma(ms);
        if(PFM_Vec2_Len(&wma) > EPSILON) {
            ent->rotation = dir_quat_from_velocity(wma);
        }
    }else{
        ms->velocity = (vec2_t){0.0f, 0.0f}; 
    }

    assert(M_NavPositionPathable(s_map, G_Pos_GetXZ(ent->uid)));
    switch(ms->state) {
    case STATE_MOVING: {

        vec2_t diff_to_target;
        vec2_t xz_pos = G_Pos_GetXZ(ent->uid);
        PFM_Vec2_Sub((vec2_t*)&flock->target_xz, &xz_pos, &diff_to_target);
        if(PFM_Vec2_Len(&diff_to_target) < ARRIVE_THRESHOLD_DIST){

            *ms = (struct movestate) {
                .state = STATE_ARRIVED,
                .velocity = (vec2_t){0.0f},
            };
            entity_finish_moving(ent);
            break;
        }

        struct entity *adjacent[kh_size(flock->ents)];
        size_t num_adj = adjacent_flock_members(ent, flock, adjacent);

        for(int j = 0; j < num_adj; j++) {

            struct movestate *adj_ms = movestate_get(adjacent[j]);
            assert(adj_ms);

            if(adj_ms->state == STATE_ARRIVED || adj_ms->state == STATE_SETTLING) {

                ms->state = STATE_SETTLING;
                break;
            }
        }
        break;
    }
    case STATE_SETTLING: {

        if(PFM_Vec2_Len(&new_vel) < SETTLE_STOP_TOLERANCE * ent->max_speed) {

            *ms = (struct movestate) {
                .state = STATE_ARRIVED,
                .velocity = (vec2_t){0.0f},
            };
            entity_finish_moving(ent);
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
    /* For the ClearPath algorithm, we only consider entities without
     * ENTITY_FLAG_STATIC set, as they are the only ones that may need
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

        if(curr->flags & ENTITY_FLAG_STATIC)
            continue;

        if(curr->selection_radius == 0.0f)
            continue;

        struct movestate *ms = movestate_get(curr);
        assert(ms);

        vec2_t curr_xz_pos = G_Pos_GetXZ(curr->uid);
        struct cp_ent newdesc = (struct cp_ent) {
            .xz_pos = curr_xz_pos,
            .xz_vel = ms->velocity,
            .radius = curr->selection_radius
        };

        if(ms->state == STATE_ARRIVED)
            vec_cp_ent_push(out_stat, newdesc);
        else
            vec_cp_ent_push(out_dyn, newdesc);
    }
}

static void on_30hz_tick(void *user, void *event)
{
    vec_cp_ent_t dyn, stat;
    vec_cp_ent_init(&dyn);
    vec_cp_ent_init(&stat);

    uint32_t key;
    struct entity *curr;

    /* Iterate vector backwards so we can delete entries while iterating. */
    for(int i = vec_size(&s_flocks)-1; i >= 0; i--) {

        struct flock *flock = &vec_AT(&s_flocks, i);

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
            continue;
        }

        kh_foreach(vec_AT(&s_flocks, i).ents, key, curr, {

            struct movestate *ms = movestate_get(curr);
            assert(ms);
            vec2_t vpref = calculate_vpref(curr, flock);

            struct cp_ent curr_cp = (struct cp_ent) {
                .xz_pos = G_Pos_GetXZ(curr->uid),
                .xz_vel = ms->velocity,
                .radius = curr->selection_radius,
            };

            vec_cp_ent_reset(&dyn);
            vec_cp_ent_reset(&stat);
            find_neighbours(curr, &dyn, &stat);

            ms->vnew = G_ClearPath_NewVelocity(curr_cp, key, vpref, dyn, stat);
            update_vel_hist(ms, ms->vnew);

            vec2_t vel_diff;
            PFM_Vec2_Sub(&ms->vnew, &ms->velocity, &vel_diff);

            PFM_Vec2_Add(&ms->velocity, &vel_diff, &ms->vnew);
            vec2_truncate(&ms->vnew, curr->max_speed / MOVE_TICK_RES);
        });
    }

    for(int i = vec_size(&s_flocks)-1; i >= 0; i--) {

        uint32_t key;
        struct entity *curr;
        struct flock *flock = &vec_AT(&s_flocks, i);
    
        kh_foreach(vec_AT(&s_flocks, i).ents, key, curr, {

            struct movestate *ms = movestate_get(curr);
            assert(ms);
            entity_update(curr, flock, ms->vnew);
        });
    }

    vec_cp_ent_destroy(&dyn);
    vec_cp_ent_destroy(&stat);
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
    vec_pentity_init(&s_move_markers);
    vec_flock_init(&s_flocks);

    E_Global_Register(SDL_MOUSEBUTTONDOWN, on_mousedown, NULL, G_RUNNING);
    E_Global_Register(EVENT_RENDER_3D, on_render_3d, NULL, G_RUNNING | G_PAUSED_FULL | G_PAUSED_UI_RUNNING);
    E_Global_Register(EVENT_30HZ_TICK, on_30hz_tick, NULL, G_RUNNING);

    s_map = map;
    return true;
}

void G_Move_Shutdown(void)
{
    s_map = NULL;

    E_Global_Unregister(EVENT_30HZ_TICK, on_30hz_tick);
    E_Global_Unregister(EVENT_RENDER_3D, on_render_3d);
    E_Global_Unregister(SDL_MOUSEBUTTONDOWN, on_mousedown);

    for(int i = 0; i < vec_size(&s_move_markers); i++) {
        E_Entity_Unregister(EVENT_ANIM_FINISHED, vec_AT(&s_move_markers, i)->uid, on_marker_anim_finish);
        G_RemoveEntity(vec_AT(&s_move_markers, i));
        AL_EntityFree(vec_AT(&s_move_markers, i));
    }

    vec_flock_destroy(&s_flocks);
    vec_pentity_destroy(&s_move_markers);
    kh_destroy(state, s_entity_state_table);
}

void G_Move_AddEntity(const struct entity *ent)
{
    struct movestate new_ms = (struct movestate) {
        .velocity = {0.0f}, 
        .state = STATE_ARRIVED,
        .vel_hist_idx = 0,
    };
    memset(new_ms.vel_hist, 0, sizeof(new_ms.vel_hist));

    int ret;
    khiter_t k = kh_put(state, s_entity_state_table, ent->uid, &ret);
    assert(ret != -1 && ret != 0);
    kh_value(s_entity_state_table, k) = new_ms;
}

void G_Move_RemoveEntity(const struct entity *ent)
{
    G_Move_Stop(ent);

    khiter_t k = kh_get(state, s_entity_state_table, ent->uid);
    if(k != kh_end(s_entity_state_table))
        kh_del(state, s_entity_state_table, k);
}

void G_Move_Stop(const struct entity *ent)
{
    uint32_t key;
    struct entity *curr;

    /* Remove this entity from any existing flocks */
    for(int i = vec_size(&s_flocks)-1; i >= 0; i--) {

        struct flock *curr_flock = &vec_AT(&s_flocks, i);
        flock_try_remove(curr_flock, ent);

        if(kh_size(curr_flock->ents) == 0) {
            kh_destroy(entity, curr_flock->ents);
            vec_flock_del(&s_flocks, i);
        }
    }

    struct movestate *ms = movestate_get(ent);
    if(ms && ms->state != STATE_ARRIVED) {

        *ms = (struct movestate) {
            .state = STATE_ARRIVED,
            .velocity = (vec2_t){0.0f}
        };
        entity_finish_moving(ent);
    }
}

bool G_Move_GetDest(const struct entity *ent, vec2_t *out_xz)
{
    struct flock *fl = flock_for_ent(ent);
    if(!fl)
        return false;
    *out_xz = fl->target_xz;
    return true;
}

void G_Move_SetDest(const struct entity *ent, vec2_t dest_xz)
{
    vec_pentity_t to_add;
    vec_pentity_init(&to_add);
    vec_pentity_push(&to_add, (struct entity*)ent);

    make_flock_from_selection(&to_add, dest_xz, false);
    vec_pentity_destroy(&to_add);
}

void G_Move_SetMoveOnLeftClick(void)
{
    s_attack_on_lclick = false;
    s_move_on_lclick = true;
    Cursor_SetRTSPointer(CURSOR_TARGET);
}

void G_Move_SetAttackOnLeftClick(void)
{
    s_attack_on_lclick = true;
    s_move_on_lclick = false;
    Cursor_SetRTSPointer(CURSOR_TARGET);
}

