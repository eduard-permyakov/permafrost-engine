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
#define MEM_FILE_SUB MEM_SUB_GAME_MOVEMENT

#include "move_harness.h"
#include "game_private.h"
#include "combat.h"
#include "position.h"
#include "fog_of_war.h"
#include "public/game.h"
#include "../move/public/movement.h"
#include "../config.h"
#include "../asset_load.h"
#include "../event.h"
#include "../entity.h"
#include "../main.h"
#include "../perf.h"
#include "../script/public/script.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../map/public/map.h"
#include "../anim/public/anim.h"
#include "../lib/public/attr.h"
#include "../lib/public/khash.h"
#include "../lib/public/vec.h"

#include <assert.h>
#include <SDL.h>

#include "../mem.h"

#define EPSILON               (1.0f/1024)

#define MAX(a, b)    ((a) > (b) ? (a) : (b))
#define MIN(a, b)    ((a) < (b) ? (a) : (b))

#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

KHASH_MAP_INIT_INT(aabb, struct aabb)
KHASH_MAP_INIT_INT(footprint, struct cluster_footprint)

/* A display segment: the unit is drawn sliding from where it stood at the
 * last consume to the step the tick computed, over the measured tick period.
 */
struct interp_seg{
    uint32_t uid;
    bool     done;
    vec3_t   from_pos;
    vec3_t   to_pos;
    quat_t   from_rot;
    quat_t   to_rot;
};

VEC_TYPE(seg, struct interp_seg)
VEC_IMPL(static inline, seg, struct interp_seg)

static void move_tick(void *user, void *event);
static void handle_hz_update(enum eventtype curr);
static void handle_split_submit(void);

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

/* The copy of the world the tick in flight works from, and the one before it,
 * kept whole while its carried field floods may still read it; each fill
 * takes the older.
 */
static struct move_world       s_worlds[2];
static int                     s_world_slot;

static unsigned long           s_last_tick = 0;

static enum movement_hz        s_move_hz = MOVE_HZ_20;
static bool                    s_move_hz_dirty = false;
/* The rate of the tick last submitted */
static enum movement_hz        s_work_hz = MOVE_HZ_20;
static bool                    s_move_tick_queued = false;
/* A tick's main-thread work is split across two frames: the consume half runs
 * on the tick event's frame, the snapshot + submit half on the next frame, so
 * no single frame carries the whole stall. */
static bool                    s_move_split_pending = false;
/* The first part of a split submit ran and released the snapshot */
static bool                    s_submit_prepared = false;
/* Smoothed cost of the two submit halves, for fitting them into the frame
 * that consumed the last tick. */
static double                  s_prepare_ema_us = 8000.0;
static double                  s_submit_ema_us = 8000.0;
static unsigned long           s_split_frame;
static enum movement_hz        s_split_hz;

static uint32_t                s_interp_nunits;
static uint32_t                s_interp_us;
static vec_seg_t               s_interp;
static vec_seg_t               s_interp_next;
static uint64_t                s_interp_t0;
static double                  s_interp_period_us = 50000.0;
static float                   s_interp_alpha;
static uint64_t                s_last_consume_pc;
/* By uid: one past the unit's pending display segment, 0 for none */
static int                    *s_seg_slot;
static size_t                  s_seg_slot_cap;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static bool entities_equal(uint32_t *a, uint32_t *b)
{
    return (*a == *b);
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
    enum formation_type type = Move_Formation_PreferredForSet(sel);

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

        G_Duty_End(curr, NULL);
        G_StopEntity(curr, false, true);
        E_Entity_Notify(EVENT_MOVE_ISSUED, curr, NULL, ES_ENGINE);
        G_NotifyOrderIssued(curr, true, true);
        nmoved++;

        if(flags & ENTITY_FLAG_COMBATABLE) {
            G_Combat_SetStance(curr, 
                attack ? COMBAT_STANCE_AGGRESSIVE : COMBAT_STANCE_NO_ENGAGEMENT);
        }
    }

    if(nmoved) {
        move_marker_add(mouse_coord, attack);
        vec2_t target = (vec2_t){mouse_coord.x, mouse_coord.z};
        if(attack)
            Move_AttackInFormation(&capped, target, orientation, type);
        else
            Move_ArrangeInFormation(&capped, target, orientation, type);
    }
    vec_entity_destroy(&capped);
}

/* The units that may not target a unit attack-move to the clicked enemy's
 * position instead; the combat click handler orders the rest.
 */
static void ground_attack_order(const vec_entity_t *sel, uint32_t target)
{
    vec_entity_t ground;
    vec_entity_init(&ground);

    for(int i = 0; i < vec_size(sel); i++) {
        uint32_t curr = vec_AT(sel, i);
        if(!(G_FlagsGet(curr) & ENTITY_FLAG_COMBATABLE))
            continue;
        if(!G_Combat_GroundAttackOnly(curr))
            continue;
        vec_entity_push(&ground, curr);
    }

    if(vec_size(&ground) > 0) {
        vec3_t pos = G_Pos_Get(target);
        vec2_t orientation = (vec2_t){0.0f, 0.0f};
        if(Move_Formation_PreferredForSet(&ground) != FORMATION_NONE) {
            orientation = Move_Formation_AutoOrientation((vec2_t){pos.x, pos.z}, &ground);
        }
        move_order(&ground, true, pos, orientation);
    }
    vec_entity_destroy(&ground);
}

static void on_mousedown(void *user, void *event)
{
    SDL_MouseButtonEvent *mouse_event = &(((SDL_Event*)event)->button);

    bool targeting = G_Move_InTargetMode();
    bool attack = s_attack_on_lclick && (mouse_event->button == SDL_BUTTON_LEFT);
    bool move = s_move_on_lclick ? mouse_event->button == SDL_BUTTON_LEFT
                                 : mouse_event->button == SDL_BUTTON_RIGHT;

    if(move && !s_move_on_lclick && G_GetAttackOnRightClick()) {
        attack = true;
        move = false;
    }

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

    int ctx_action = G_CurrContextualAction();
    if(ctx_action != CTX_ACTION_NONE && ctx_action != CTX_ACTION_ATTACK)
        return;

    if(G_MouseInTargetMode() && !targeting)
        return;

    if(ctx_action == CTX_ACTION_ATTACK) {

        enum selection_type sel_type;
        const vec_entity_t *sel = G_Sel_Get(&sel_type);

        vec_entity_t fsel;
        Move_FilterPathable(sel, &fsel);

        if(vec_size(&fsel) > 0 && sel_type == SELECTION_TYPE_PLAYER) {
            ground_attack_order(&fsel, G_Sel_GetHovered());
        }
        vec_entity_destroy(&fsel);
        return;
    }

    vec3_t mouse_coord;
    if(!M_MinimapMouseMapCoords(s_map, &mouse_coord)
    && !M_Raycast_MouseIntersecCoord(&mouse_coord))
        return;

    enum selection_type sel_type;
    const vec_entity_t *sel = G_Sel_Get(&sel_type);

    vec_entity_t fsel;
    Move_FilterPathable(sel, &fsel);

    if(vec_size(&fsel) == 0 || sel_type != SELECTION_TYPE_PLAYER)
        return;

    /* Allow dragging the mouse to orient the formation around 
     * the clicked location. The move orders will be issued when
     * the mouse is released. 
     */
    if(Move_Formation_PreferredForSet(&fsel) != FORMATION_NONE) {
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
        orientation = Move_Formation_AutoOrientation(endpoints[0], sel);
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
    
    Move_Formation_RenderPlacement(sel, endpoints[0], delta);
}

static void on_render_3d(void *user, void *event)
{
    if(s_mouse_dragged) {
        render_formation_orientation();
    }
}

/* uid -> identity aabb, persistent across ticks (the aabb never changes for a
 * model). Entries for removed entities linger harmlessly until the periodic
 * clear.
 */
static khash_t(aabb) *world_update_aabbs(khash_t(aabb) *cache)
{
    PERF_ENTER();
    if(!cache) {
        cache = kh_init(aabb);
    }
    const khash_t(entity) *ents = G_GetAllEntsSet();

    if(kh_size(cache) > 2 * kh_size(ents) + 1024)
        kh_clear(aabb, cache);

    uint32_t uid;
    kh_foreach_key(ents, uid, {
        khiter_t k = kh_get(aabb, cache, uid);
        if(k == kh_end(cache)) {
            int ret;
            k = kh_put(aabb, cache, uid, &ret);
            assert(ret != -1);
            kh_value(cache, k) = AL_EntityGet(uid)->identity_aabb;
        }
    });
    PERF_RETURN(cache);
}

static const struct move_world *world_fill(void)
{
    PERF_ENTER();
    s_world_slot ^= 1;
    struct move_world *world = &s_worlds[s_world_slot];

    if(world->dying_set) {
        kh_destroy(id, world->dying_set);
        world->dying_set = NULL;
    }
    if(world->diptable) {
        PF_FREE(world->diptable);
        world->diptable = NULL;
    }
    world->flags = G_FlagsCopyTableInto(world->flags);
    world->positions = G_Pos_CopyTableInto(world->positions);
    world->postree = G_Pos_CopyBitmapGridInto(world->postree);
    world->sel_radiuses =
        G_SelectionRadiusCopyTableInto(world->sel_radiuses);
    world->faction_ids =
        G_FactionIDCopyTableInto(world->faction_ids);
    world->transforms =
        Entity_CopyTransformsInto(world->transforms);
    world->aabbs = world_update_aabbs(world->aabbs);
    world->footprints =
        G_Cluster_CopyFootprintsInto(world->footprints);
    world->fog_enabled = G_Fog_Enabled();
    world->fog_state =
        G_Fog_CopyStateInto(world->fog_state, &world->fog_ntiles);
    world->dying_set = G_Combat_GetDyingSetCopy();
    world->diptable = G_CopyDiplomacyTable();
    world->player_controllable = G_GetPlayerControlledFactions();

    PERF_RETURN(world);
}

static void world_destroy(struct move_world *world)
{
    if(world->flags) {
        kh_destroy(id, world->flags);
        world->flags = NULL;
    }
    if(world->positions) {
        kh_destroy(pos, world->positions);
        world->positions = NULL;
    }
    if(world->postree) {
        G_Pos_DestroyBitmapGrid(world->postree);
        world->postree = NULL;
    }
    if(world->sel_radiuses) {
        kh_destroy(range, world->sel_radiuses);
        world->sel_radiuses = NULL;
    }
    if(world->faction_ids) {
        kh_destroy(id, world->faction_ids);
        world->faction_ids = NULL;
    }
    if(world->transforms) {
        kh_destroy(trans, world->transforms);
        world->transforms = NULL;
    }
    if(world->aabbs) {
        kh_destroy(aabb, world->aabbs);
        world->aabbs = NULL;
    }
    if(world->footprints) {
        kh_destroy(footprint, world->footprints);
        world->footprints = NULL;
    }
    if(world->fog_state) {
        PF_FREE(world->fog_state);
        world->fog_state = NULL;
    }
    world->fog_ntiles = 0;
    if(world->dying_set) {
        kh_destroy(id, world->dying_set);
        world->dying_set = NULL;
    }
    if(world->diptable) {
        PF_FREE(world->diptable);
        world->diptable = NULL;
    }
}

static int seg_idx_get(uint32_t uid)
{
    return (uid < s_seg_slot_cap) ? s_seg_slot[uid] - 1 : -1;
}

static void seg_idx_set(uint32_t uid, int idx)
{
    if(uid >= s_seg_slot_cap) {
        if(idx == -1)
            return;
        size_t cap = MAX((size_t)uid + 1, s_seg_slot_cap * 2);
        cap = MAX(cap, (size_t)4096);
        int *slots = PF_REALLOC(s_seg_slot, cap * sizeof(int));
        if(!slots)
            return;
        memset(slots + s_seg_slot_cap, 0, (cap - s_seg_slot_cap) * sizeof(int));
        s_seg_slot = slots;
        s_seg_slot_cap = cap;
    }
    s_seg_slot[uid] = idx + 1;
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

static void interp_begin_segment(struct move_step *step)
{
    ASSERT_IN_MAIN_THREAD();

    uint32_t uid = step->uid;
    const vec3_t *to_pos = step->has_pos ? &step->pos : NULL;
    const quat_t *to_rot = step->has_rot ? &step->rot : NULL;

    /* The refs catch up to where the unit is drawn once per tick. */
    G_Pos_SyncRefs(uid);

    bool pending = false;
    vec3_t old_to = {0};
    quat_t old_rot = {0};
    int idx = seg_idx_get(uid);
    if(idx >= 0 && idx < vec_size(&s_interp)) {
        struct interp_seg *old = &vec_AT(&s_interp, idx);
        if(old->uid == uid) {
            pending = !old->done;
            old_to = old->to_pos;
            old_rot = old->to_rot;
            old->done = true;
        }
    }

    /* A unit not drawn may not have had its turn to snap before the tick
     * ended; it takes the step it is owed now, else it falls behind where
     * it is simulated a tick at a time
     */
    if(pending && !G_Pos_IsInterpCandidate(uid)
    && G_Pos_SetInterpolated(uid, old_to, old_rot, false)) {
        G_Pos_SyncRefs(uid);
        pending = false;
    }

    vec3_t pos = G_Pos_Get(uid);
    quat_t rot = Entity_GetRot(uid);
    struct interp_seg seg = (struct interp_seg){
        .uid = uid,
        .done = false,
        .from_pos = pos,
        .to_pos = to_pos ? *to_pos : (pending ? old_to : pos),
        .from_rot = rot,
        .to_rot = to_rot ? *to_rot : rot,
    };
    seg_idx_set(uid, vec_size(&s_interp_next));
    step->owed = (to_pos || pending);
    vec_seg_push(&s_interp_next, seg);
}

/* Segments the tick did not replace keep sliding from where they are drawn;
 * then the new set becomes current and the clock restarts. */
static void interp_consume_end(void)
{
    ASSERT_IN_MAIN_THREAD();

    for(int i = 0; i < vec_size(&s_interp); i++) {
        struct interp_seg *seg = &vec_AT(&s_interp, i);
        if(seg->done)
            continue;
        if(!Move_Tracks(seg->uid) || seg_idx_get(seg->uid) != i)
            continue;
        if(!G_EntityExists(seg->uid)) {
            seg_idx_set(seg->uid, -1);
            Move_StepDone(seg->uid);
            continue;
        }
        seg->from_pos = G_Pos_Get(seg->uid);
        seg->from_rot = Entity_GetRot(seg->uid);
        seg_idx_set(seg->uid, vec_size(&s_interp_next));
        vec_seg_push(&s_interp_next, *seg);
    }
    vec_seg_t tmp = s_interp;
    s_interp = s_interp_next;
    s_interp_next = tmp;

    uint64_t now = SDL_GetPerformanceCounter();
    double nominal = 1e6 / Move_HzCount(s_work_hz);
    if(s_last_consume_pc) {
        double interval = (now - s_last_consume_pc) * 1e6 / SDL_GetPerformanceFrequency();
        interval = MIN(MAX(interval, nominal), 3.0 * nominal);
        s_interp_period_us = 0.75 * s_interp_period_us + 0.25 * interval;
    }else{
        s_interp_period_us = nominal;
    }
    s_last_consume_pc = now;
    s_interp_t0 = now;
    s_interp_alpha = 0.0f;
}

static void interp_drop_segment(uint32_t uid)
{
    int idx = seg_idx_get(uid);
    if(idx >= 0 && idx < vec_size(&s_interp)) {
        struct interp_seg *seg = &vec_AT(&s_interp, idx);
        if(seg->uid == uid)
            seg->done = true;
    }
    seg_idx_set(uid, -1);
}

static void interp_finish_segment(uint32_t uid)
{
    int idx = seg_idx_get(uid);
    if(idx < 0 || idx >= vec_size(&s_interp))
        return;
    struct interp_seg *seg = &vec_AT(&s_interp, idx);
    if(!seg->done && seg->uid == uid && G_Pos_SetInterpolated(uid, seg->to_pos, seg->to_rot, false))
        G_Pos_SyncRefs(uid);
    interp_drop_segment(uid);
    Move_StepDone(uid);
}

static void interp_reset(void)
{
    vec_seg_reset(&s_interp);
    vec_seg_reset(&s_interp_next);
    s_last_consume_pc = 0;
    s_interp_alpha = 1.0f;
    if(s_seg_slot) {
        memset(s_seg_slot, 0, s_seg_slot_cap * sizeof(int));
    }
}

static void move_interpolate_frame(void)
{
    ASSERT_IN_MAIN_THREAD();

    size_t nsegs = vec_size(&s_interp);
    if(nsegs == 0 || s_interp_alpha >= 1.0f)
        return;

    uint64_t t0 = SDL_GetPerformanceCounter();
    double elapsed_us = (t0 - s_interp_t0) * 1e6 / SDL_GetPerformanceFrequency();
    float alpha = MIN(1.0f, (float)(elapsed_us / s_interp_period_us));
    if(alpha <= s_interp_alpha)
        return;
    bool final = (alpha >= 1.0f);
    /* Units not drawn lately take their step in one write; those writes are
     * spread over the period in segment order so no frame carries them all. */
    size_t snap_upto = final ? nsegs : (size_t)(nsegs * alpha);

    PERF_ENTER();
    uint32_t nwritten = 0;
    for(size_t i = 0; i < nsegs; i++) {

        struct interp_seg *seg = &vec_AT(&s_interp, i);
        if(seg->done)
            continue;
        bool drawn = G_Pos_IsInterpCandidate(seg->uid);
        bool snap = !drawn && (i < snap_upto);
        if(!final && !drawn && !snap)
            continue;

        bool at_end = final || snap;
        vec3_t pos = at_end ? seg->to_pos
                            : interpolate_positions(seg->from_pos, seg->to_pos, alpha);
        quat_t rot = at_end ? seg->to_rot
                            : interpolate_rotations(seg->from_rot, seg->to_rot, alpha);
        if(!G_Pos_SetInterpolated(seg->uid, pos, rot, drawn)) {
            seg->done = true;
            continue;
        }
        nwritten++;

        if(at_end) {
            G_Pos_SyncRefs(seg->uid);
            seg->done = true;
            if(Move_StepDone(seg->uid)) {
                seg_idx_set(seg->uid, -1);
            }
        }
    }
    s_interp_alpha = alpha;
    s_interp_nunits += nwritten;
    s_interp_us += (SDL_GetPerformanceCounter() - t0) * 1000000 / SDL_GetPerformanceFrequency();
    PERF_RETURN_VOID();
}

static uint32_t ticks_to_us(uint64_t delta_ticks)
{
    uint64_t freq = SDL_GetPerformanceFrequency();
    return freq ? (uint32_t)(delta_ticks * 1000000ull / freq) : 0;
}

static void steps_begin(struct move_output *out)
{
    vec_seg_reset(&s_interp_next);
    for(size_t i = 0; i < out->nsteps; i++) {
        interp_begin_segment(&out->steps[i]);
    }
    Move_StepsBegun(out->steps, out->nsteps);
    interp_consume_end();

    /* All this tick's position changes are enqueued; apply the batched fog
     * vision updates in one pipelined pass before any reader runs. */
    G_Fog_FlushUpdates();
}

static void output_apply(const struct move_output *out)
{
    for(size_t i = 0; i < out->nstopped; i++) {
        uint32_t uid = out->stopped[i];
        if(G_FlagsGet(uid) & ENTITY_FLAG_COMBATABLE)
            G_Combat_SetStance(uid, COMBAT_STANCE_AGGRESSIVE);
    }

    for(size_t i = 0; i < out->nresets; i++) {
        if(out->resets[i].drop)
            interp_drop_segment(out->resets[i].uid);
        else
            seg_idx_set(out->resets[i].uid, -1);
    }
}

/* The results of the tick that ran enter the game, and the orders given since
 * are taken up.
 */
static void tick_consume(enum eventtype curr_event, enum movement_hz hz)
{
    ASSERT_IN_MAIN_THREAD();
    PERF_PUSH("movement::tick");
    uint64_t tick_start = SDL_GetPerformanceCounter();
    struct nav_tick_sample *stats = Move_TickStats();
    struct move_output out;

    if(Move_TickConsume()) {
        Move_Output(&out);
        steps_begin(&out);
        output_apply(&out);
    }
    stats->consume_us = ticks_to_us(SDL_GetPerformanceCounter() - tick_start);

    uint64_t phase_start = SDL_GetPerformanceCounter();
    Move_TickResume();
    Move_Output(&out);
    output_apply(&out);
    for(size_t i = 0; i < out.nheld_still; i++) {
        interp_finish_segment(out.held_still[i]);
    }
    Move_TickPivot();
    Move_Output(&out);
    for(size_t i = 0; i < out.nfacings; i++) {
        Entity_SetRot(out.facings[i].uid, out.facings[i].rot);
    }
    stats->pivot_us = ticks_to_us(SDL_GetPerformanceCounter() - phase_start);

    handle_hz_update(curr_event);

    phase_start = SDL_GetPerformanceCounter();
    Move_TickCommands();
    Move_Output(&out);
    output_apply(&out);
    stats->cmds_us = ticks_to_us(SDL_GetPerformanceCounter() - phase_start);

    stats->main_us = ticks_to_us(SDL_GetPerformanceCounter() - tick_start);

    s_move_split_pending = true;
    s_split_frame = g_frame_idx;
    s_split_hz = hz;
    PERF_POP();
}

static void tick_prepare(void)
{
    struct move_output out;
    Move_TickPrepare();
    Move_Output(&out);
    output_apply(&out);
    s_submit_prepared = true;
}

static void tick_submit(enum movement_hz hz)
{
    if(!s_submit_prepared)
        tick_prepare();
    s_submit_prepared = false;

    uint64_t fill_start = SDL_GetPerformanceCounter();
    const struct move_world *world = world_fill();
    uint32_t fill_us = ticks_to_us(SDL_GetPerformanceCounter() - fill_start);

    struct move_output out;
    Move_TickSubmit(world, hz);
    Move_Output(&out);
    output_apply(&out);

    struct nav_tick_sample *stats = Move_TickStats();
    stats->copy_gs_us += fill_us;
    stats->main_us += fill_us;
    s_work_hz = hz;
    s_last_tick = g_frame_idx;
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

static void handle_hz_update(enum eventtype curr)
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
    s_interp_period_us = 1e6 / Move_HzCount(next_hz);
    s_last_consume_pc = 0;
}

static void move_tick(void *user, void *event)
{
    /* A tick that cannot start this frame runs on the first frame that
     * allows it; at most one is kept, so a backlog never builds. */
    if(g_frame_idx == s_last_tick || s_move_split_pending) {
        s_move_tick_queued = true;
        return;
    }

    enum eventtype curr_event = (uintptr_t)user;
    enum movement_hz hz = event_to_hz(curr_event);

    if(!Move_TickFinished()) {
        s_move_tick_queued = true;
        Move_TickStats()->nlate++;
        return;
    }

    s_move_tick_queued = false;
    tick_consume(curr_event, hz);
    handle_split_submit();
}

static void handle_queued_tick(void)
{
    if(!s_move_tick_queued)
        return;
    if(g_frame_idx == s_last_tick || s_move_split_pending)
        return;

    if(!Move_TickFinished()) {
        Move_TickStats()->nlate++;
        return;
    }

    enum movement_hz hz = s_work_hz;
    enum eventtype curr_event = event_for_hz(hz);

    s_move_tick_queued = false;
    tick_consume(curr_event, hz);
    handle_split_submit();
}

/* Below 20 Hz the tick's period leaves room to put the map update and the
 * snapshot with the submit on frames of their own, so no one frame carries
 * both.
 */
#define SPLIT_FRAME_BUDGET_US   (0.85 * 1e6 / CONFIG_SCHED_TARGET_FPS)

static bool frame_has_room_for(double cost_us)
{
    double elapsed_us = (SDL_GetPerformanceCounter() - Perf_FrameStartTicks())
                      * 1e6 / SDL_GetPerformanceFrequency();
    return elapsed_us + cost_us < SPLIT_FRAME_BUDGET_US;
}

static void ema_note(double *ema, uint64_t t0)
{
    double us = (SDL_GetPerformanceCounter() - t0) * 1e6 / SDL_GetPerformanceFrequency();
    *ema = 0.8 * *ema + 0.2 * us;
}

/* Each submit half runs on the frame that consumed the tick when it fits
 * the frame's budget, and on the next frame regardless. */
static void handle_split_submit(void)
{
    if(!s_move_split_pending)
        return;
    bool same_frame = (g_frame_idx == s_split_frame);

    if(!s_submit_prepared) {
        if(same_frame && !frame_has_room_for(s_prepare_ema_us))
            return;
        uint64_t t0 = SDL_GetPerformanceCounter();
        tick_prepare();
        ema_note(&s_prepare_ema_us, t0);
        s_split_frame = g_frame_idx;
        if(!frame_has_room_for(s_submit_ema_us))
            return;
    }else if(same_frame && !frame_has_room_for(s_submit_ema_us)) {
        return;
    }

    s_move_split_pending = false;
    uint64_t t0 = SDL_GetPerformanceCounter();
    tick_submit(s_split_hz);
    ema_note(&s_submit_ema_us, t0);
}

static void on_update(void *user, void *event)
{
    Move_FrameStart();
    move_interpolate_frame();
    handle_queued_tick();
    handle_split_submit();
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool G_Move_Init(const struct map *map)
{
    vec_entity_init(&s_move_markers);
    vec_seg_init(&s_interp);
    vec_seg_init(&s_interp_next);

    E_Global_Register(EVENT_UPDATE_START, on_update, NULL, G_RUNNING);
    E_Global_Register(SDL_MOUSEBUTTONDOWN, on_mousedown, NULL, G_RUNNING);
    E_Global_Register(SDL_MOUSEBUTTONUP, on_mouseup, NULL, G_RUNNING);
    E_Global_Register(SDL_MOUSEMOTION, on_mousemotion, NULL, G_RUNNING);
    E_Global_Register(EVENT_RENDER_3D_POST, on_render_3d, NULL, 
        G_RUNNING | G_PAUSED_FULL | G_PAUSED_UI_RUNNING);
    register_callback_for_hz(s_move_hz);

    s_map = map;
    s_attack_on_lclick = false;
    s_move_on_lclick = false;
    s_mouse_dragged = false;
    s_drag_attacking = false;
    s_work_hz = MOVE_HZ_20;

    if(!Move_Init(map, world_fill())) {
        unregister_callback_for_hz(s_move_hz);
        E_Global_Unregister(EVENT_UPDATE_START, on_update);
        E_Global_Unregister(EVENT_RENDER_3D_POST, on_render_3d);
        E_Global_Unregister(SDL_MOUSEBUTTONDOWN, on_mousedown);
        E_Global_Unregister(SDL_MOUSEBUTTONUP, on_mouseup);
        E_Global_Unregister(SDL_MOUSEMOTION, on_mousemotion);
        world_destroy(&s_worlds[0]);
        world_destroy(&s_worlds[1]);
        vec_seg_destroy(&s_interp);
        vec_seg_destroy(&s_interp_next);
        vec_entity_destroy(&s_move_markers);
        return false;
    }
    return true;
}

void G_Move_Shutdown(void)
{
    Move_FinishTick();
    s_move_tick_queued = false;
    s_move_split_pending = false;
    s_submit_prepared = false;

    unregister_callback_for_hz(s_move_hz);
    interp_reset();
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
    vec_entity_destroy(&s_move_markers);

    Move_Shutdown();
    world_destroy(&s_worlds[0]);
    world_destroy(&s_worlds[1]);
    vec_seg_destroy(&s_interp);
    vec_seg_destroy(&s_interp_next);
    PF_FREE(s_seg_slot);
    s_seg_slot = NULL;
    s_seg_slot_cap = 0;
    s_map = NULL;
}

void G_Move_FlushWork(void)
{
    if(!s_map)
        return;

    Move_FinishTick();
    s_move_split_pending = false;

    const struct move_world *world = NULL;
    if(s_submit_prepared) {
        world = world_fill();
        s_submit_prepared = false;
    }
    Move_FlushWork(world);

    struct move_output out;
    Move_Output(&out);
    output_apply(&out);
}

void G_Move_SetTickHz(enum movement_hz hz)
{
    s_move_hz_dirty = (s_move_hz != hz);
    s_move_hz = hz;
    Move_SetTickHz(hz);
}

int G_Move_TickRate(void)
{
    return Move_HzCount(s_move_hz);
}

void G_Move_InterpStats(uint32_t *out_nunits, uint32_t *out_us)
{
    *out_nunits = s_interp_nunits;
    *out_us = s_interp_us;
    s_interp_nunits = 0;
    s_interp_us = 0;
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

void G_Move_AttackInFormation(vec_entity_t *ents, vec2_t target, 
                              vec2_t orientation, enum formation_type type)
{
    ASSERT_IN_MAIN_THREAD();

    /* An attack-move engages on contact, as the mouse order does. */
    for(int i = 0; i < vec_size(ents); i++) {
        uint32_t curr = vec_AT(ents, i);
        if(G_FlagsGet(curr) & ENTITY_FLAG_COMBATABLE)
            G_Combat_SetStance(curr, COMBAT_STANCE_AGGRESSIVE);
    }

    Move_AttackInFormation(ents, target, orientation, type);
}

bool G_Move_SaveState(struct SDL_RWops *stream)
{
    struct attr click_move_enabled = (struct attr){
        .type = TYPE_BOOL,
        .val.as_bool = s_click_move_enabled
    };
    CHK_TRUE_RET(Attr_Write(stream, &click_move_enabled, "click_move_enabled"));

    return Move_SaveState(stream);
}

bool G_Move_LoadState(struct SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_BOOL);
    s_click_move_enabled = attr.val.as_bool;

    interp_reset();
    bool ret = Move_LoadState(stream, world_fill());

    struct move_output out;
    Move_Output(&out);
    output_apply(&out);
    return ret;
}
