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
#include "public/game.h"
#include "../config.h"
#include "../camera.h"
#include "../asset_load.h"
#include "../event.h"
#include "../entity.h"
#include "../collision.h"
#include "../cursor.h"
#include "../script/public/script.h"
#include "../render/public/render.h"
#include "../map/public/map.h"
#include "../map/public/tile.h"
#include "../lib/public/kvec.h"
#include "../anim/public/anim.h"

#include <assert.h>
#include <SDL.h>


#define ATTACK_HOTKEY (SDL_SCANCODE_A)

/* For the purpose of movement simulation, all entities have the same mass,
 * meaning they are accelerate the same amount when applied equal forces. */
#define ENTITY_MASS (1.0f)
#define EPSILON     (1.0f/1024)
#define MAX_FORCE   (0.2f)
#define SIGNUM(x)   (((x) > 0) - ((x) < 0))

#define MIN(a, b)   ((a) < (b) ? (a) : (b))

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
    vec2_t             velocity;
    enum arrival_state state;
    /* After an obstacle is detected and a collision force is applied, 
     * it decays linearly over a fixed number of ticks.*/
    vec2_t             avoid_force;
    unsigned           avoid_ticks_left;
};

KHASH_MAP_INIT_INT(state, struct movestate)

struct flock{
    khash_t(entity) *ents;
    vec2_t           target_xz; 
    dest_id_t        dest_id;
};

/* Parameters controlling steering/flocking behaviours */
#define MOVE_SEPARATION_FORCE_SCALE     (1.6f)
#define MOVE_ARRIVE_FORCE_SCALE         (0.7f)
#define MOVE_COHESION_FORCE_SCALE       (0.1f)
#define MOVE_ALIGN_FORCE_SCALE          (0.1f)
#define MOVE_COL_AVOID_FORCE_SCALE      (0.7f)
#define SETTLE_SEPARATION_FORCE_SCALE   (3.2f)

#define ARRIVE_THRESHOLD_DIST           (5.0f)
#define MOVE_SEPARATION_BUFFER_DIST     (8.0f)
#define SETTLE_SEPARATION_BUFFER_DIST   (14.0f)
#define COHESION_NEIGHBOUR_RADIUS       (25.0f)
#define ALIGN_NEIGHBOUR_RADIUS          (10.0f)
#define ARRIVE_SLOWING_RADIUS           (10.0f)
#define ADJACENCY_SEP_DIST              (10.0f)

#define SETTLE_STOP_TOLERANCE           (0.05f)
#define COLLISION_MAX_SEE_AHEAD         (15.0f)
#define COLLISION_AVOID_MAX_TICKS       (25.0f)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static const struct map       *s_map;
static bool                    s_attack_on_lclick = false;

static kvec_t(struct entity*)  s_move_markers;
static kvec_t(struct flock)    s_flocks;
static khash_t(state)         *s_entity_state_table;

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

static void movestate_set(const struct entity *ent, const struct movestate *ms)
{
    int ret;
    khiter_t k = kh_put(state, s_entity_state_table, ent->uid, &ret);
    assert(ret != -1 && ret != 0);
    kh_value(s_entity_state_table, k) = *ms;
}

static void movestate_remove(const struct entity *ent)
{
    khiter_t k = kh_get(state, s_entity_state_table, ent->uid);
    if(k != kh_end(s_entity_state_table))
        kh_del(state, s_entity_state_table, k);
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
    for(int i = 0; i < kv_size(s_flocks); i++) {

        struct flock *curr_flock = &kv_A(s_flocks, i);            
        if(flock_contains(curr_flock, ent))
            return curr_flock;
    }
    return NULL;
}

static struct flock *flock_for_dest(dest_id_t id)
{
    for(int i = 0; i < kv_size(s_flocks); i++) {

        struct flock *curr_flock = &kv_A(s_flocks, i);            
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

    kv_indexof(struct entity*, s_move_markers, ent, entities_equal, idx);
    assert(idx != -1);
    kv_del(struct entity*, s_move_markers, idx);

    E_Entity_Unregister(EVENT_ANIM_FINISHED, ent->uid, on_marker_anim_finish);
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

static bool make_flock_from_selection(const pentity_kvec_t *sel, vec2_t target_xz, bool attack)
{
    /* First remove the entities in the selection from any active flocks */
    for(int i = 0; i < kv_size(*sel); i++) {

        const struct entity *curr_ent = kv_A(*sel, i);
        if(stationary(curr_ent))
            continue;
        /* Remove any flocks which may have become empty. Iterate vector in backwards order 
         * so that we can delete while iterating, since the last element in the vector takes
         * the place of the deleted one. */
        for(int j = kv_size(s_flocks)-1; j >= 0; j--) {

            khiter_t k;
            struct flock *curr_flock = &kv_A(s_flocks, j);
            flock_try_remove(curr_flock, curr_ent);

            if(kh_size(curr_flock->ents) == 0) {
                kh_destroy(entity, curr_flock->ents);
                kv_del(struct flock, s_flocks, j);
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
     * the FF for this 'island' will be computed on demand. */
    struct tile_desc pathed_ents_descs[kv_size(*sel)];
    size_t num_pathed_ents = 0;

    for(int i = 0; i < kv_size(*sel); i++) {

        const struct entity *curr_ent = kv_A(*sel, i);
        struct movestate *ms;

        if(stationary(curr_ent))
            continue;

        struct tile_desc curr_desc;
        M_DescForPoint2D(s_map, (vec2_t){curr_ent->pos.x, curr_ent->pos.z}, &curr_desc);

        if(same_chunk_as_any_in_set(curr_desc, pathed_ents_descs, num_pathed_ents)
        || M_NavRequestPath(s_map, (vec2_t){curr_ent->pos.x, curr_ent->pos.z}, target_xz, &new_flock.dest_id)) {

            pathed_ents_descs[num_pathed_ents++] = curr_desc;
            flock_add(&new_flock, curr_ent);

            /* When entities are moved from one flock to another, they keep their existing velocity. 
             * Otherwise, entities start out with a velocity of 0. */
            if((ms = movestate_get(curr_ent)) == NULL) {

                struct movestate new_ms = (struct movestate) {
                    .velocity = {0.0f}, 
                    .state = STATE_MOVING,
                    .avoid_ticks_left = 0,
                    .avoid_force = (vec2_t){0.0f},
                };
                movestate_set(curr_ent, &new_ms);
                E_Entity_Notify(EVENT_MOTION_START, curr_ent->uid, NULL, ES_ENGINE);

            }else{

                if(ms->state == STATE_ARRIVED) 
                    E_Entity_Notify(EVENT_MOTION_START, curr_ent->uid, NULL, ES_ENGINE);
                ms->state = STATE_MOVING;
            }

        }else if((ms = movestate_get(curr_ent)) != NULL){

            if(ms->state != STATE_ARRIVED) 
                entity_finish_moving(curr_ent);
            *ms = (struct movestate) {
                .state = STATE_ARRIVED,
                .velocity = (vec2_t){0.0f}
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
            kv_push(struct flock, s_flocks, new_flock);
        }

        return true;
    }else{
        kh_destroy(entity, new_flock.ents);
        return false;
    }
}

size_t adjacent_flock_members(const struct entity *ent, const struct flock *flock, 
                              struct entity *out[])
{
    vec2_t ent_xz_pos = (vec2_t){ent->pos.x, ent->pos.z};
    size_t ret = 0;

    uint32_t key;
    struct entity *curr;
    kh_foreach(flock->ents, key, curr, {

        if(curr == ent)
            continue;

        vec2_t curr_xz_pos = (vec2_t){curr->pos.x, curr->pos.z};
        vec2_t diff;
        PFM_Vec2_Sub(&ent_xz_pos, &curr_xz_pos, &diff);

        if(PFM_Vec2_Len(&diff) <= ent->selection_radius + curr->selection_radius + ADJACENCY_SEP_DIST)
            out[ret++] = curr;  
    });
    return ret;
}

static const struct entity *most_threatening_obstacle(const struct entity *ent, struct line_seg_2d ahead,
                                                      const struct flock *flock)
{
    const khash_t(entity) *dynamic = G_GetDynamicEntsSet();
    float min_t = INFINITY;
    const struct entity *ret = NULL;

    uint32_t key;
    struct entity *curr;
    kh_foreach(dynamic, key, curr, {

        if(flock_contains(flock, curr))
            continue;

        float t;
        vec2_t xz_center = (vec2_t){curr->pos.x, curr->pos.z};
        if(C_LineCircleIntersection(ahead, xz_center, curr->selection_radius + ent->selection_radius, &t)){

            if(t < min_t) {
                min_t = t;
                ret = curr;
            }
        }
    });

    assert(min_t < INFINITY ? (NULL != ret) : (NULL == ret));
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

    ent->pos = pos;
    ent->scale = (vec3_t){2.0f, 2.0f, 2.0f};
    E_Entity_Register(EVENT_ANIM_FINISHED, ent->uid, on_marker_anim_finish, ent);

    A_InitCtx(ent, "Converge", 48);
    A_SetActiveClip(ent, "Converge", ANIM_MODE_ONCE, 48);

    kv_push(struct entity*, s_move_markers, ent);
}

static void on_mousedown(void *user, void *event)
{
    SDL_MouseButtonEvent *mouse_event = &(((SDL_Event*)event)->button);

    bool attack = s_attack_on_lclick && (mouse_event->button == SDL_BUTTON_LEFT);
    bool move = (mouse_event->button == SDL_BUTTON_RIGHT);

    s_attack_on_lclick = false;
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
    const pentity_kvec_t *sel = G_Sel_Get(&sel_type);
    if(kv_size(*sel) > 0 && sel_type == SELECTION_TYPE_PLAYER) {

        for(int i = 0; i < kv_size(*sel); i++) {

            const struct entity *curr = kv_A(*sel, i);
            if(!(curr->flags & ENTITY_FLAG_COMBATABLE))
                continue;

            G_Combat_ClearSavedMoveCmd(curr);
            G_Combat_SetStance(curr, attack ? COMBAT_STANCE_AGGRESSIVE : COMBAT_STANCE_NO_ENGAGEMENT);
        }

        move_marker_add(mouse_coord, attack);
        make_flock_from_selection(sel, (vec2_t){mouse_coord.x, mouse_coord.z}, attack);
    }
}

static void on_keydown(void *user, void *event)
{
    s_attack_on_lclick = false;
    Cursor_SetRTSPointer(CURSOR_POINTER);

    SDL_KeyboardEvent *key_event = &(((SDL_Event*)event)->key);
    enum selection_type sel_type;
    const pentity_kvec_t *sel = G_Sel_Get(&sel_type);

    if(key_event->keysym.scancode == ATTACK_HOTKEY 
    && kv_size(*sel) > 0 && sel_type == SELECTION_TYPE_PLAYER) {

        s_attack_on_lclick = true;
        Cursor_SetRTSPointer(CURSOR_TARGET);
    }
}

static void on_render_3d(void *user, void *event)
{
    for(int i = 0; i < kv_size(s_move_markers); i++) {

        const struct entity *curr = kv_A(s_move_markers, i);
        if(curr->flags & ENTITY_FLAG_ANIMATED)
            A_Update(curr);

        mat4x4_t model;
        Entity_ModelMatrix(curr, &model);
        R_GL_Draw(curr->render_private, &model);
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
static vec2_t seek_force(const struct entity *ent, vec2_t target_xz, int tick_res)
{
    vec2_t ret, desired_velocity;
    vec2_t pos_xz = (vec2_t){ent->pos.x, ent->pos.z};

    PFM_Vec2_Sub(&target_xz, &pos_xz, &desired_velocity);
    PFM_Vec2_Normal(&desired_velocity, &desired_velocity);
    PFM_Vec2_Scale(&desired_velocity, ent->max_speed / tick_res, &desired_velocity);

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
static vec2_t arrive_force(const struct entity *ent, const struct flock *flock, int tick_res)
{
    vec2_t ret, desired_velocity;
    vec2_t pos_xz = (vec2_t){ent->pos.x, ent->pos.z};
    float distance;

    if(M_NavHasDestLOS(s_map, flock->dest_id, pos_xz)) {

        PFM_Vec2_Sub((vec2_t*)&flock->target_xz, &pos_xz, &desired_velocity);
        distance = PFM_Vec2_Len(&desired_velocity);
        PFM_Vec2_Normal(&desired_velocity, &desired_velocity);
        PFM_Vec2_Scale(&desired_velocity, ent->max_speed / tick_res, &desired_velocity);

        if(distance < ARRIVE_SLOWING_RADIUS) {
            PFM_Vec2_Scale(&desired_velocity, distance / ARRIVE_SLOWING_RADIUS, &desired_velocity);
        }
    }else{

        desired_velocity = M_NavDesiredVelocity(s_map, flock->dest_id, pos_xz, flock->target_xz);
        PFM_Vec2_Scale(&desired_velocity, ent->max_speed / tick_res, &desired_velocity);
    }

    struct movestate *ms = movestate_get(ent);
    assert(ms);

    PFM_Vec2_Sub(&desired_velocity, &ms->velocity, &ret);
    vec2_truncate(&ret, MAX_FORCE);
    return ret;
}

/* Alignment is a behaviour that causes a particular agent to line up with agents close by.
 */
static vec2_t alignment_force(const struct entity *ent, const struct flock *flock, int tick_res)
{
    vec2_t ret = (vec2_t){0.0f};
    size_t neighbour_count = 0;

    uint32_t key;
    struct entity *curr;
    kh_foreach(flock->ents, key, curr, {

        if(curr == ent)
            continue;

        vec2_t diff;
        vec2_t ent_xz_pos = (vec2_t){ent->pos.x, ent->pos.z};
        vec2_t curr_xz_pos = (vec2_t){curr->pos.x, curr->pos.z};

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
static vec2_t cohesion_force(const struct entity *ent, const struct flock *flock, int tick_res)
{
    vec2_t COM = (vec2_t){0.0f};
    size_t neighbour_count = 0;

    uint32_t key;
    struct entity *curr;
    kh_foreach(flock->ents, key, curr, {

        if(curr == ent)
            continue;

        vec2_t diff;
        vec2_t ent_xz_pos = (vec2_t){ent->pos.x, ent->pos.z};
        vec2_t curr_xz_pos = (vec2_t){curr->pos.x, curr->pos.z};

        PFM_Vec2_Sub(&curr_xz_pos, &ent_xz_pos, &diff);
        if(PFM_Vec2_Len(&diff) < COHESION_NEIGHBOUR_RADIUS) {

            vec2_t xz_pos = (vec2_t){curr->pos.x, curr->pos.z};
            PFM_Vec2_Add(&COM, &xz_pos, &COM);
            neighbour_count++;
        }
    });

    if(0 == neighbour_count)
        return (vec2_t){0.0f};

    vec2_t xz_pos = (vec2_t){ent->pos.x, ent->pos.z};
    PFM_Vec2_Scale(&COM, 1.0f / neighbour_count, &COM);

    vec2_t ret;
    PFM_Vec2_Sub(&COM, &xz_pos, &ret);
    vec2_truncate(&ret, MAX_FORCE);
    return ret;
}

/* Separation is a behaviour that causes agents to steer away from nearby agents.
 */
static vec2_t separation_force(const struct entity *ent, const struct flock *flock, int tick_res,
                               float buffer_dist)
{
    const float NEIGHBOUR_RADIUS = ent->selection_radius + buffer_dist;

    vec2_t ret = (vec2_t){0.0f};
    size_t neighbour_count = 0;
    const khash_t(entity) *dynamic = G_GetDynamicEntsSet();

    uint32_t key;
    struct entity *curr;
    kh_foreach(dynamic, key, curr, {

        if(curr == ent)
            continue;

        vec2_t diff;
        vec2_t ent_xz_pos = (vec2_t){ent->pos.x, ent->pos.z};
        vec2_t curr_xz_pos = (vec2_t){curr->pos.x, curr->pos.z};

        PFM_Vec2_Sub(&curr_xz_pos, &ent_xz_pos, &diff);
        if(PFM_Vec2_Len(&diff) < NEIGHBOUR_RADIUS) {

            float frac = 1.0f - (PFM_Vec2_Len(&diff) / NEIGHBOUR_RADIUS);
            PFM_Vec2_Scale(&diff, frac, &diff);
            PFM_Vec2_Add(&ret, &diff, &ret);
            neighbour_count++;
        }
    });

    if(0 == neighbour_count)
        return (vec2_t){0.0f};

    PFM_Vec2_Scale(&ret, -1.0f / neighbour_count, &ret);
    vec2_truncate(&ret, MAX_FORCE);
    return ret;
}

/* Collision avoidance is a behaviour that causes agents to steer around obstacles in front of them.
 */
static vec2_t collision_avoidance_force(const struct entity *ent, const struct flock *flock, int tick_res)
{
    struct movestate *ms = movestate_get(ent);
    assert(ms);

    if(PFM_Vec2_Len(&ms->velocity) < EPSILON)
        return (vec2_t){0.0f};

    vec2_t line;
    PFM_Vec2_Normal(&ms->velocity, &line);
    PFM_Vec2_Scale(&line, ent->selection_radius + COLLISION_MAX_SEE_AHEAD, &line);

    struct line_seg_2d ahead = {
        .ax = ent->pos.x,
        .az = ent->pos.z,
        .bx = ent->pos.x + line.raw[0],
        .bz = ent->pos.z + line.raw[1]
    };

    const struct entity *threat = most_threatening_obstacle(ent, ahead, flock);
    if(!threat)
        return (vec2_t){0.0f};

    vec2_t threat_center = (vec2_t){threat->pos.x, threat->pos.z};
    vec2_t ahead_tip = (vec2_t){ahead.bx, ahead.bz};

    vec2_t right_dir = (vec2_t){-(ahead.bz - ahead.az), ahead.bx - ahead.ax};
    PFM_Vec2_Normal(&right_dir, &right_dir);

    /* Return a rightward avoidance force which is scaled depending on how 
     * sharply the entity must turn. */
    vec2_t right_off = right_dir;
    assert(threat->selection_radius > 0.0f && ent->selection_radius > 0.0f);
    float collision_dist = ent->selection_radius + threat->selection_radius;
    PFM_Vec2_Scale(&right_off, collision_dist, &right_off);

    vec2_t diff;
    PFM_Vec2_Sub(&ahead_tip, &threat_center, &diff);
    vec2_t proj_tip_right = right_off;
    float coeff = PFM_Vec2_Dot(&diff, &right_off) / pow(PFM_Vec2_Len(&right_off), 2);
    PFM_Vec2_Scale(&proj_tip_right, coeff, &proj_tip_right);

    float frac = ((PFM_Vec2_Len(&proj_tip_right) / collision_dist * SIGNUM(coeff)) + 1.0f) / 2.0f;
    PFM_Vec2_Scale(&right_dir, MAX_FORCE * frac, &right_dir);
    vec2_truncate(&right_dir, MAX_FORCE);
    return right_dir;
}

static vec2_t total_steering_force(const struct entity *ent, const struct flock *flock, int tick_res,
                                   vec2_t *out_col_avoid_force)
{
    struct movestate *ms = movestate_get(ent);
    assert(ms);

    vec2_t arrive = arrive_force(ent, flock, tick_res);
    vec2_t cohesion = cohesion_force(ent, flock, tick_res);
    vec2_t alignment = alignment_force(ent, flock, tick_res);
    vec2_t collision_avoid = collision_avoidance_force(ent, flock, tick_res);
    *out_col_avoid_force = collision_avoid;

    unsigned ca_ticks_left = ms->avoid_ticks_left > 0 ? (ms->avoid_ticks_left - 1) : COLLISION_AVOID_MAX_TICKS;
    collision_avoid = ms->avoid_ticks_left > 0 ? ms->avoid_force : collision_avoid;

    /* When we get pushed onto an impassable tile, increase the proportion of the
     * 'arrive' force, which will steer us back towards the nearest passable tile.*/
    vec2_t xz_pos = (vec2_t){ent->pos.x, ent->pos.z};
    if(!M_NavPositionPathable(s_map, xz_pos)) {
        PFM_Vec2_Scale(&arrive, 3.0f, &arrive);
        PFM_Vec2_Scale(&alignment, 0.0f, &alignment);
    }

    vec2_t ret = (vec2_t){0.0f};
    switch(ms->state) {
    case STATE_MOVING: {
        vec2_t separation = separation_force(ent, flock, tick_res, MOVE_SEPARATION_BUFFER_DIST);

        PFM_Vec2_Scale(&collision_avoid, MOVE_COL_AVOID_FORCE_SCALE,  &collision_avoid);
        PFM_Vec2_Scale(&separation,      MOVE_SEPARATION_FORCE_SCALE, &separation);
        PFM_Vec2_Scale(&arrive,          MOVE_ARRIVE_FORCE_SCALE,     &arrive);
        PFM_Vec2_Scale(&cohesion,        MOVE_COHESION_FORCE_SCALE,   &cohesion);
        PFM_Vec2_Scale(&alignment,       MOVE_ALIGN_FORCE_SCALE,      &alignment);

        PFM_Vec2_Scale(&collision_avoid, ca_ticks_left / COLLISION_AVOID_MAX_TICKS, &collision_avoid);

        PFM_Vec2_Add(&ret, &collision_avoid, &ret);
        PFM_Vec2_Add(&ret, &separation, &ret);
        PFM_Vec2_Add(&ret, &arrive, &ret);
        PFM_Vec2_Add(&ret, &cohesion, &ret);
        PFM_Vec2_Add(&ret, &alignment, &ret);

        break;
    }
    case STATE_SETTLING: {
        vec2_t separation = separation_force(ent, flock, tick_res, SETTLE_SEPARATION_BUFFER_DIST);

        PFM_Vec2_Scale(&separation, SETTLE_SEPARATION_FORCE_SCALE, &separation);
        PFM_Vec2_Add(&ret, &separation, &ret);

        break;
    }
    case STATE_ARRIVED:
        break;
    default: assert(0);
    }

    vec2_truncate(&ret, MAX_FORCE);
    return ret;
}

static void on_30hz_tick(void *user, void *event)
{
    const int TICK_RES = 30;

    /* Iterate vector backwards so we can delete entries while iterating. */
    for(int i = kv_size(s_flocks)-1; i >= 0; i--) {

        uint32_t key;
        struct entity *curr;

        /* First, decide if we can disband this flock */
        bool disband = true;
        kh_foreach(kv_A(s_flocks, i).ents, key, curr, {

            struct movestate *ms = movestate_get(curr);
            assert(ms);
            if(ms->state != STATE_ARRIVED) {
                disband = false;
                break;
            }
        });

        if(disband) {

            kh_foreach(kv_A(s_flocks, i).ents, key, curr, { movestate_remove(curr); });
            kh_destroy(entity, kv_A(s_flocks, i).ents);
            kv_del(struct flock, s_flocks, i);
            continue;
        }

        kh_foreach(kv_A(s_flocks, i).ents, key, curr, {

            struct movestate *ms = movestate_get(curr);
            assert(ms);

            /* Compute acceleration */
            vec2_t col_avoid_force;
            vec2_t steer_accel, new_velocity; 

            vec2_t steer_force = total_steering_force(curr, &kv_A(s_flocks, i), TICK_RES, &col_avoid_force);
            PFM_Vec2_Scale(&steer_force, 1.0f / ENTITY_MASS, &steer_accel);

            /* Compute new velocity */
            PFM_Vec2_Add(&ms->velocity, &steer_accel, &new_velocity);
            vec2_truncate(&new_velocity, curr->max_speed / TICK_RES);

            /* Update position and rotation */
            vec2_t xz_pos = (vec2_t){curr->pos.x, curr->pos.z};
            vec2_t new_xz_pos;
            PFM_Vec2_Add(&xz_pos, &new_velocity, &new_xz_pos);
            new_xz_pos = M_ClampedMapCoordinate(s_map, new_xz_pos);
            curr->pos = (vec3_t){new_xz_pos.raw[0], M_HeightAtPoint(s_map, new_xz_pos), new_xz_pos.raw[1]};

            if(PFM_Vec2_Len(&new_velocity) > EPSILON) {
                curr->rotation = dir_quat_from_velocity(new_velocity);
            }

            /* Update state of entity */
            ms->velocity = new_velocity;

            if(ms->avoid_ticks_left > 0) {
                --ms->avoid_ticks_left;
            }

            if(PFM_Vec2_Len(&col_avoid_force) > 0.0f) {
                ms->avoid_ticks_left = COLLISION_AVOID_MAX_TICKS;
                ms->avoid_force = col_avoid_force;
            }

            switch(ms->state) {
            case STATE_MOVING: {

                vec2_t diff_to_target;
                vec2_t xz_pos = (vec2_t){curr->pos.x, curr->pos.z};
                PFM_Vec2_Sub(&kv_A(s_flocks, i).target_xz, &xz_pos, &diff_to_target);
                if(PFM_Vec2_Len(&diff_to_target) < ARRIVE_THRESHOLD_DIST){

                    *ms = (struct movestate) {
                        .state = STATE_ARRIVED,
                        .velocity = (vec2_t){0.0f}
                    };
                    entity_finish_moving(curr);
                }

                struct entity *adjacent[kh_size(kv_A(s_flocks, i).ents)]; 
                size_t num_adj = adjacent_flock_members(curr, &kv_A(s_flocks, i), adjacent);

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

                if(PFM_Vec2_Len(&new_velocity) < SETTLE_STOP_TOLERANCE * curr->max_speed)  {

                    *ms = (struct movestate) {
                        .state = STATE_ARRIVED,
                        .velocity = (vec2_t){0.0f}
                    };
                    entity_finish_moving(curr);
                }
                break;
            }
            case STATE_ARRIVED: 
                break;
            default: 
                assert(0);
            }

        });
    }
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
    kv_init(s_move_markers);
    kv_init(s_flocks);

    E_Global_Register(SDL_MOUSEBUTTONDOWN, on_mousedown, NULL);
    E_Global_Register(SDL_KEYDOWN, on_keydown, NULL);
    E_Global_Register(EVENT_RENDER_3D, on_render_3d, NULL);
    E_Global_Register(EVENT_30HZ_TICK, on_30hz_tick, NULL);

    s_map = map;
    return true;
}

void G_Move_Shutdown(void)
{
    s_map = NULL;

    E_Global_Unregister(EVENT_30HZ_TICK, on_30hz_tick);
    E_Global_Unregister(EVENT_RENDER_3D, on_render_3d);
    E_Global_Unregister(SDL_KEYDOWN, on_keydown);
    E_Global_Unregister(SDL_MOUSEBUTTONDOWN, on_mousedown);

    for(int i = 0; i < kv_size(s_move_markers); i++) {
        E_Entity_Unregister(EVENT_ANIM_FINISHED, kv_A(s_move_markers, i)->uid, on_marker_anim_finish);
        AL_EntityFree(kv_A(s_move_markers, i));
    }

    kv_destroy(s_flocks);
    kv_destroy(s_move_markers);
    kh_destroy(state, s_entity_state_table);
}

void G_Move_RemoveEntity(const struct entity *ent)
{
    uint32_t key;
    struct entity *curr;

    /* Remove this entity from any existing flocks */
    for(int i = kv_size(s_flocks)-1; i >= 0; i--) {

        struct flock *curr_flock = &kv_A(s_flocks, i);
        flock_try_remove(curr_flock, ent);

        if(kh_size(curr_flock->ents) == 0) {
            kh_destroy(entity, curr_flock->ents);
            kv_del(struct flock, s_flocks, i);
        }
    }

    struct movestate *ms = movestate_get(ent);
    if(ms && ms->state != STATE_ARRIVED) {
        entity_finish_moving(ent);
    }
    movestate_remove(ent);
}

bool G_Move_GetDest(const struct entity *ent, vec2_t *out_xz)
{
    struct movestate *ms = movestate_get(ent);
    if(!ms) 
        return false;

    struct flock *fl = flock_for_ent(ent);
    assert(fl);
    *out_xz = fl->target_xz;
    return true;
}

void G_Move_SetDest(const struct entity *ent, vec2_t dest_xz)
{
    pentity_kvec_t to_add;
    kv_init(to_add);
    kv_push(struct entity*, to_add, (struct entity*)ent);

    make_flock_from_selection(&to_add, dest_xz, false);
    kv_destroy(to_add);
}

