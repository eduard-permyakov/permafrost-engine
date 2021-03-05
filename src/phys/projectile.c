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
#include "../asset_load.h"
#include "../game/public/game.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../lib/public/vec.h"
#include "../lib/public/stalloc.h"

#include <math.h>
#include <assert.h>


#define GRAVITY         (9.81f/100.0f)
#define PHYS_HZ         (1.0f/30)
#define EPSILON         (1.0f/1024)
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define MAX_PROJ_TASKS  (64)

struct projectile{
    uint32_t uid;
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

static uint32_t      s_next_uid = 0;
static vec_proj_t    s_front; /* where new projectiles are added */
static vec_proj_t    s_back;  /* the last tick projectiles currently being processed */
struct proj_work     s_work;
static unsigned long s_last_tick = ULONG_MAX;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void phys_proj_update(struct projectile *proj)
{
    /* recall grade 11 physics here */
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

static void phys_proj_finish_work(void)
{
    for(int i = 0; i < s_work.ntasks; i++) {
        /* run to completion */
        while(!Sched_FutureIsReady(&s_work.futures[i])) {
            Sched_RunSync(s_work.tids[i]);
        }
    }
    stalloc_clear(&s_work.mem);
    s_work.ntasks = 0;

    /* swap front & back buffers */
    vec_proj_t tmp = s_back;
    s_back = s_front;
    s_front = tmp;
}

static void on_30hz_tick(void *user, void *event)
{
    Perf_Push("projectile::on_30hz_tick");

    vec_proj_reset(&s_back);
    vec_proj_copy(&s_back, &s_front);

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
        G_Pos_Set(ent, curr->pos);
        ent->scale = (vec3_t){12.0f, 12.0f, 12.0f};
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

        G_SafeFree(ent);
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

uint32_t P_Projectile_Add(vec3_t origin, vec3_t velocity)
{
    uint32_t ret = s_next_uid++;
    struct projectile proj = (struct projectile){
        .uid = ret,
        .pos = origin,
        .vel = velocity,
    };
    vec_proj_push(&s_front, proj);
    return ret;
}

void P_Projectile_Update(size_t nobjs, const struct obb *visible)
{
    /* We process the physics result exactly one frame after the work is 
     * enqueud */
    if((g_frame_idx - s_last_tick) != 1)
        return;

    phys_proj_finish_work();
    /* Here we do collision testing and pump out some events if intersection takes place */
    /* Discard 'unnecessary' projectiles from the simulation */
}

bool P_Projectile_Init(void)
{
    vec_proj_init(&s_front);
    if(!vec_proj_resize(&s_front, 1024))
        goto fail_front;
    vec_proj_init(&s_back);
    if(!vec_proj_resize(&s_back, 1024))
        goto fail_back;
    if(!stalloc_init(&s_work.mem))
        goto fail_mem;

    E_Global_Register(EVENT_30HZ_TICK, on_30hz_tick, NULL, G_RUNNING);
    E_Global_Register(EVENT_RENDER_3D_POST, on_render_3d, NULL, G_ALL);
    return true;

fail_mem:
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
    stalloc_destroy(&s_work.mem);
    vec_proj_destroy(&s_front);
    vec_proj_destroy(&s_back);
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
    const float v = init_speed;
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
    PFM_Vec3_Scale(&velocity, init_speed, &velocity);

    *out = velocity;
    return true;
}

