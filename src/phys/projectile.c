/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2021 Eduard Permyakov 
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

#include "public/phys.h"
#include "../main.h"
#include "../event.h"
#include "../task.h"
#include "../perf.h"
#include "../sched.h"
#include "../entity.h"
#include "../collision.h"
#include "../asset_load.h"
#include "../map/public/tile.h"
#include "../game/public/game.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../lib/public/vec.h"
#include "../lib/public/stalloc.h"

#include <math.h>
#include <assert.h>


#define PHYS_HZ         (30)
#define UNITS_PER_METER (5.0f)
#define GRAVITY         (9.81f * UNITS_PER_METER / (PHYS_HZ * PHYS_HZ))
#define EPSILON         (1.0f/1024)
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define ARR_SIZE(a)     (sizeof(a)/sizeof(a[0]))
#define MAX_PROJ_TASKS  (64)
#define NEAR_TOLERANCE  (100.0f)

struct projectile{
    uint32_t uid;
    uint32_t ent_parent;
    uint32_t cookie;
    uint32_t flags;
    int      faction_id;
    vec3_t   pos;
    vec3_t   vel;
};

struct proj_task_arg{
    size_t begin_idx;
    size_t end_idx;
};

struct proj_work{
    struct memstack mem;
    size_t          ntasks;
    struct future   futures[MAX_PROJ_TASKS];
    uint32_t        tids[MAX_PROJ_TASKS];
};

VEC_TYPE(proj, struct projectile)
VEC_IMPL(static inline, proj, struct projectile)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static uint32_t         s_next_uid = 0;
static vec_proj_t       s_front; /* the processed projectiles currently being rendered */
static vec_proj_t       s_back;  /* the last tick projectiles currently being processed */
static vec_proj_t       s_added;
static vec_proj_t       s_deleted;
struct proj_work        s_work;
static struct memstack  s_eventargs;

static unsigned long    s_last_tick = ULONG_MAX;
static unsigned         s_simticks = 0;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static quat_t phys_velocity_dir(vec3_t vel)
{
    float yrot = atan2(vel.x, vel.z);
    float zrot = atan2(vel.y, sqrt(pow(vel.x, 2) + pow(vel.z, 2)));

    mat4x4_t yrotmat;
    PFM_Mat4x4_MakeRotY(yrot, &yrotmat);

    mat4x4_t zrotmat;
    PFM_Mat4x4_MakeRotY(zrot, &zrotmat);

    mat4x4_t rotmat;
    PFM_Mat4x4_Mult4x4(&zrotmat, &yrotmat, &rotmat);

    quat_t rot;
    PFM_Quat_FromRotMat(&rotmat, &rot);
    return rot;
}

static bool phys_proj_equal(const struct projectile *a, const struct projectile *b)
{
    return (a->uid == b->uid);
}

static void phys_proj_update(struct projectile *proj)
{
    vec3_t accel = (vec3_t){0.0f, -GRAVITY, 0.0f};
    PFM_Vec3_Add(&proj->vel, &accel, &proj->vel);
    PFM_Vec3_Add(&proj->pos, &proj->vel, &proj->pos);
}

static struct result phys_proj_task(void *arg)
{
    struct proj_task_arg *proj_arg = arg;
    size_t ncomputed = 0;

    for(int i = proj_arg->begin_idx; i <= proj_arg->end_idx; i++) {

        phys_proj_update(&vec_AT(&s_back, i));

        ncomputed++;
        if(ncomputed % 64 == 0)
            Task_Yield();
    }
    return NULL_RESULT;
}

static void phys_filter_out_of_bounds(void)
{
    for(int i = vec_size(&s_front)-1; i >= 0; i--) {

        const struct projectile *curr = &vec_AT(&s_front, i);
        if(curr->pos.y < -Z_COORDS_PER_TILE) {
            E_Global_Notify(EVENT_PROJECTILE_DISAPPEAR, (void*)((uintptr_t)curr->uid), ES_ENGINE);
            vec_proj_del(&s_front, i);
            vec_proj_push(&s_deleted, *curr);
        }
    }
}

static void phys_proj_finish_work(void)
{
    if(s_work.ntasks == 0)
        return;

    for(int i = 0; i < s_work.ntasks; i++) {
        /* run to completion */
        while(!Sched_FutureIsReady(&s_work.futures[i])) {
            Sched_RunSync(s_work.tids[i]);
        }
    }
    stalloc_clear(&s_work.mem);
    s_work.ntasks = 0;

    vec_proj_subtract(&s_back, &s_deleted, phys_proj_equal);
    vec_proj_reset(&s_deleted);

    /* swap front & back buffers */
    vec_proj_t tmp = s_back;
    s_back = s_front;
    s_front = tmp;
}

static bool phys_enemies(int faction_id, const struct entity *ent)
{
    if(faction_id == G_GetFactionID(ent->uid))
        return false;

    enum diplomacy_state ds;
    bool result = G_GetDiplomacyState(faction_id, G_GetFactionID(ent->uid), &ds);

    assert(result);
    return (ds == DIPLOMACY_STATE_WAR);
}

static void phys_sweep_test(int front_idx)
{
    const struct projectile *proj = &vec_AT(&s_front, front_idx);
    struct entity *near[256];
    size_t nents = G_Pos_EntsInCircle((vec2_t){proj->pos.x, proj->pos.z}, NEAR_TOLERANCE, near, ARR_SIZE(near));

    /* The collision test gets performed every frame (variable FPS) while, 
     * actual projectile motion is performed at fixed frequency of PHYS_HZ. 
     * Hence, when we perform the collision check, we must account for all
     * the motion since the last update - this is 's_simticks' worth of fixed
     * frequency physics ticks.
     *
     * Though the projectile travels in the shape of a parabola, we approximate 
     * its' motion with a straight line that is tangential to the motion parabola
     * at the projent moment. We perform the sweep test with a line segment
     * from the projent location of the projectile to the location it would
     * have been in 's_simticks' ago had its' velocity been constant.
     */
    vec3_t begin = proj->pos;
    vec3_t end, delta = proj->vel;
    PFM_Vec3_Scale(&delta, 1.0f * s_simticks, &end);

    for(int i = 0; i < nents; i++) {

        struct entity *ent = near[i];
        /* A projectile does not collide with its' 'parent' */
        if(proj->ent_parent == ent->uid)
            continue;
        if((proj->flags & PROJ_ONLY_HIT_COMBATABLE) && !(ent->flags & ENTITY_FLAG_COMBATABLE))
            continue;
        if((proj->flags & PROJ_ONLY_HIT_ENEMIES) && !phys_enemies(proj->faction_id, ent))
            continue;

        struct obb obb;
        Entity_CurrentOBB(ent, &obb, false);

        if(C_LineSegIntersectsOBB(begin, end, obb)) {

            struct proj_hit *hit = stalloc(&s_eventargs, sizeof(struct proj_hit));
            hit->ent_uid = ent->uid;
            hit->proj_uid = proj->uid;
            hit->parent_uid = proj->ent_parent;
            hit->cookie = proj->cookie;
            E_Global_Notify(EVENT_PROJECTILE_HIT, hit, ES_ENGINE);

            vec_proj_del(&s_front, front_idx);
            vec_proj_push(&s_deleted, *proj);
            return;
        }
    }
}

static void on_30hz_tick(void *user, void *event)
{
    Perf_Push("projectile::on_30hz_tick");

    phys_proj_finish_work();

    vec_proj_copy(&s_back, &s_front);
    vec_proj_concat(&s_back, &s_added);
    vec_proj_reset(&s_added);

    size_t nwork = vec_size(&s_back);
    if(nwork == 0)
        goto done;

    size_t ntasks = SDL_GetCPUCount();
    if(nwork < 64)
        ntasks = 1;
    ntasks = MIN(ntasks, MAX_PROJ_TASKS);

    for(int i = 0; i < ntasks; i++) {

        struct proj_task_arg *arg = stalloc(&s_work.mem, sizeof(struct proj_task_arg));
        size_t nitems = ceil((float)nwork / ntasks);

        arg->begin_idx = nitems * i;
        arg->end_idx = MIN(nitems * (i + 1) - 1, nwork-1);

        SDL_AtomicSet(&s_work.futures[s_work.ntasks].status, FUTURE_INCOMPLETE);
        s_work.tids[s_work.ntasks] = Sched_Create(4, phys_proj_task, arg, 
            &s_work.futures[s_work.ntasks], 0);

        if(s_work.tids[s_work.ntasks] == NULL_TID) {
            for(int j = arg->begin_idx; j <= arg->end_idx; j++) {
                phys_proj_update(&vec_AT(&s_back, j));
            }
        }else{
            s_work.ntasks++;
        }
    }

done:
    s_last_tick = g_frame_idx;
    s_simticks++;
    Perf_Pop();
}

static void on_render_3d(void *user, void *arg)
{
    //TODO: batch the rendered projectiles 
    //TODO: allow specifying the projectile model

    for(int i = 0; i < vec_size(&s_front); i++) {

        struct projectile *curr = &vec_AT(&s_front, i);
        const uint32_t uid = Entity_NewUID();
        struct entity *ent = AL_EntityFromPFObj("assets/models/bow_arrow", "arrow.pfobj", "__projectile__", uid);
        if(!ent)
            continue;

        bool translucent = false;
        mat4x4_t model;

        //TODO: likely the model matrices/rotations should also 
        // be calculated by the worker tasks
        //TODO: we also shouldn't add the position ot the sim - not 100% safe if the projectile 
        // goes out of map bounds. Alternatively, clamp the position to map bounds

        G_AddEntity(ent, curr->pos);
        ent->scale = (vec3_t){12.0f, 12.0f, 12.0f};
        ent->rotation = phys_velocity_dir(curr->vel);
        Entity_ModelMatrix(ent, &model);

        R_PushCmd((struct rcmd){
            .func = R_GL_Draw,
            .nargs = 3,
            .args = {
                ent->render_private,
                R_PushArg(&model, sizeof(model)),
                R_PushArg(&translucent, sizeof(translucent)),
            },
        });

        G_RemoveEntity(ent);
        G_SafeFree(ent);
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

uint32_t P_Projectile_Add(vec3_t origin, vec3_t velocity, uint32_t ent_parent, 
                          int faction_id, uint32_t cookie, int flags)
{
    uint32_t ret = s_next_uid++;
    struct projectile proj = (struct projectile){
        .uid = ret,
        .ent_parent = ent_parent,
        .cookie = cookie,
        .flags = flags,
        .faction_id = faction_id,
        .pos = origin,
        .vel = velocity,
    };
    vec_proj_push(&s_added, proj);
    return ret;
}

void P_Projectile_Update(void)
{
    PERF_ENTER();
    stalloc_clear(&s_eventargs);

    for(int i = vec_size(&s_front)-1; i >= 0; i--) {
        phys_sweep_test(i);
    }
    phys_filter_out_of_bounds();
    s_simticks = 0;

    PERF_RETURN_VOID();
}

bool P_Projectile_Init(void)
{
    vec_proj_init(&s_front);
    if(!vec_proj_resize(&s_front, 1024))
        goto fail_front;
    vec_proj_init(&s_back);
    if(!vec_proj_resize(&s_back, 1024))
        goto fail_back;
    vec_proj_init(&s_added);
    if(!vec_proj_resize(&s_added, 256))
        goto fail_added;
    vec_proj_init(&s_deleted);
    if(!vec_proj_resize(&s_deleted, 256))
        goto fail_deleted;
    if(!stalloc_init(&s_work.mem))
        goto fail_mem;
    if(!stalloc_init(&s_eventargs))
        goto fail_eventargs;

    E_Global_Register(EVENT_30HZ_TICK, on_30hz_tick, NULL, G_RUNNING);
    E_Global_Register(EVENT_RENDER_3D_POST, on_render_3d, NULL, G_ALL);
    return true;

fail_eventargs:
    stalloc_destroy(&s_work.mem);
fail_mem:
    vec_proj_destroy(&s_deleted);
fail_deleted:
    vec_proj_destroy(&s_added);
fail_added:
    vec_proj_destroy(&s_back);
fail_back:
    vec_proj_destroy(&s_front);
fail_front:
    return false;
}

void P_Projectile_Shutdown(void)
{
    E_Global_Unregister(EVENT_30HZ_TICK, on_30hz_tick);
    E_Global_Unregister(EVENT_RENDER_3D_POST, on_render_3d);
    stalloc_destroy(&s_eventargs);
    stalloc_destroy(&s_work.mem);
    vec_proj_destroy(&s_front);
    vec_proj_destroy(&s_back);
    vec_proj_destroy(&s_added);
    vec_proj_destroy(&s_deleted);
}

bool P_Projectile_VelocityForTarget(vec3_t src, vec3_t dst, float init_speed, vec3_t *out)
{
    vec3_t delta;
    PFM_Vec3_Sub(&dst, &src, &delta);

    /* Use a coordinate system such that the y-axis is up and 
     * the x-axis is along the direction of motion (src -> dst). 
     */
    const float x = sqrt(pow(delta.x, 2) + pow(delta.z, 2));
    const float y = delta.y;
    const float v = init_speed / PHYS_HZ;
    const float g = GRAVITY;

    /* To hit a target at range x and altitude y when fired from (0,0) 
     * and with initial speed v the required angle of launch THETA is: 
     *
     *              (v^2 +/- sqrt(v^4 - g(gx^2 + 2yv^2))
     * tan(THETA) = (----------------------------------)
     *              (              gx                  )
     *
     * The two roots of the equation correspond to the two possible 
     * launch angles, so long as they aren't imaginary, in which case 
     * the initial speed is not great enough to reach the point (x,y) 
     * selected.
     */

    float descriminant = pow(v, 4) - g * (g * pow(x, 2) + 2 * y * pow(v, 2));
    if(descriminant < -EPSILON) {
        /* No real solutions */
        return false;
    }

    size_t nsolutions = 1;
    if(fabs(descriminant) > EPSILON) {
        nsolutions = 2;
    }

    float t1, t2, tan_theta;
    t1 = pow(v, 2) + sqrt(descriminant);
    if(nsolutions > 1) {
        t2 = pow(v, 2) - sqrt(descriminant);
        tan_theta = MIN(t1, t2) / (g * x);
    }else{
        tan_theta = t1 / (g * x);
    }

    /* Theta is the angle of motion up from the ground along the angle of motion.
     * Convert this to a velocity vector. 
     */
    float xlen = sqrt(pow(delta.x, 2) + pow(delta.z, 2));
    float ylen = xlen * tan_theta;

    vec3_t velocity = (vec3_t){ delta.x, ylen, delta.z };
    assert(PFM_Vec3_Len(&velocity) > EPSILON); /* guaranteed by having real solutions */
    PFM_Vec3_Normal(&velocity, &velocity);
    PFM_Vec3_Scale(&velocity, v, &velocity);

    *out = velocity;
    return true;
}
