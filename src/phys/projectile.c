/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2021-2023 Eduard Permyakov 
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
#include "public/collision.h"
#include "../main.h"
#include "../event.h"
#include "../task.h"
#include "../perf.h"
#include "../sched.h"
#include "../entity.h"
#include "../asset_load.h"
#include "../map/public/tile.h"
#include "../game/public/game.h"
#include "../render/public/render.h"
#include "../render/public/render_ctrl.h"
#include "../lib/public/vec.h"
#include "../lib/public/stalloc.h"
#include "../lib/public/attr.h"
#include "../lib/public/pf_string.h"

#include <math.h>
#include <assert.h>
#include <SDL.h>


#define PHYS_HZ         (30)
#define UNITS_PER_METER (7.5f)
/* Everyone knows moon physics are just more fun ;) */
#define GRAVITY         (1.62f * UNITS_PER_METER / (PHYS_HZ * PHYS_HZ))
#define EPSILON         (1.0f/1024)
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define ARR_SIZE(a)     (sizeof(a)/sizeof(a[0]))
#define MAX_PROJ_TASKS  (64)
#define NEAR_TOLERANCE  (100.0f)

#define CHK_TRUE_RET(_pred)             \
    do{                                 \
        if(!(_pred))                    \
            return false;               \
    }while(0)

struct projectile{
    uint32_t uid;
    uint32_t ent_parent;
    uint32_t cookie;
    uint32_t flags;
    int      faction_id;
    void    *render_private;
    vec3_t   pos;
    vec3_t   vel;
    vec3_t   scale;
    mat4x4_t model;
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
    PFM_Mat4x4_MakeRotZ(zrot, &zrotmat);

    mat4x4_t rotmat;
    PFM_Mat4x4_Mult4x4(&yrotmat, &zrotmat, &rotmat);

    quat_t rot;
    PFM_Quat_FromRotMat(&rotmat, &rot);
    return rot;
}

static bool phys_proj_equal(const struct projectile *a, const struct projectile *b)
{
    return (a->uid == b->uid);
}

static void assert_no_zero(vec_proj_t *vec)
{
    struct projectile proj = (struct projectile){ .uid = 0 };
    int idx = vec_proj_indexof(vec, proj, phys_proj_equal);
    assert(idx == -1);
}

static void phys_proj_update(struct projectile *proj)
{
    vec3_t accel = (vec3_t){0.0f, -GRAVITY, 0.0f};
    PFM_Vec3_Add(&proj->vel, &accel, &proj->vel);
    PFM_Vec3_Add(&proj->pos, &proj->vel, &proj->pos);

    mat4x4_t trans, scale, rot, tmp;
    quat_t qrot = phys_velocity_dir(proj->vel);

    PFM_Mat4x4_MakeTrans(proj->pos.x, proj->pos.y, proj->pos.z, &trans);
    PFM_Mat4x4_MakeScale(proj->scale.x, proj->scale.y, proj->scale.z, &scale);
    PFM_Mat4x4_RotFromQuat(&qrot, &rot);

    PFM_Mat4x4_Mult4x4(&scale, &rot, &tmp);
    PFM_Mat4x4_Mult4x4(&trans, &tmp, &proj->model);
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
            vec_proj_push(&s_deleted, *curr);
            vec_proj_del(&s_front, i);
        }
    }
}

static void phys_proj_join_work(void)
{
    for(int i = 0; i < s_work.ntasks; i++) {
        while(!Sched_FutureIsReady(&s_work.futures[i])) {
            Sched_RunSync(s_work.tids[i]);
        }
    }
}

static void phys_proj_finish_work(void)
{
    phys_proj_join_work();
    stalloc_clear(&s_work.mem);
    s_work.ntasks = 0;

    vec_proj_subtract(&s_back, &s_deleted, phys_proj_equal);
    vec_proj_reset(&s_deleted);

    /* swap front & back buffers */
    vec_proj_t tmp = s_back;
    s_back = s_front;
    s_front = tmp;
}

static bool phys_enemies(int faction_id, uint32_t ent)
{
    if(faction_id == G_GetFactionID(ent))
        return false;

    enum diplomacy_state ds;
    bool result = G_GetDiplomacyState(faction_id, G_GetFactionID(ent), &ds);

    assert(result);
    return (ds == DIPLOMACY_STATE_WAR);
}

static void phys_sweep_test(int front_idx)
{
    const struct projectile *proj = &vec_AT(&s_front, front_idx);
    uint32_t nearp[256];
    size_t nents = G_Pos_EntsInCircle((vec2_t){proj->pos.x, proj->pos.z}, NEAR_TOLERANCE, nearp, ARR_SIZE(nearp));

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
    PFM_Vec3_Scale(&delta, -1.0f * s_simticks, &delta);
    PFM_Vec3_Add(&begin, &delta, &end);

    float min_dist = INFINITY;
    uint32_t hit_ent = NULL_UID;

    for(int i = 0; i < nents; i++) {

        uint32_t ent = nearp[i];
        /* A projectile does not collide with its' 'parent' */
        if(proj->ent_parent == ent)
            continue;
        if(G_FlagsGet(ent) & ENTITY_FLAG_ZOMBIE)
            continue;
        if((proj->flags & PROJ_ONLY_HIT_COMBATABLE) && !(G_FlagsGet(ent) & ENTITY_FLAG_COMBATABLE))
            continue;
        if((proj->flags & PROJ_ONLY_HIT_ENEMIES) && !phys_enemies(proj->faction_id, ent))
            continue;

        struct obb obb;
        Entity_CurrentOBB(ent, &obb, false);

        if(C_LineSegIntersectsOBB(begin, end, obb)) {

            vec3_t diff;
            vec3_t ent_pos = G_Pos_Get(ent);
            PFM_Vec3_Sub((vec3_t*)&proj->pos, &ent_pos, &diff);

            if(PFM_Vec3_Len(&diff) < min_dist) {
                min_dist = PFM_Vec3_Len(&diff);
                hit_ent = ent;
            }
        }
    }

    if(hit_ent != NULL_UID) {

        struct proj_hit *hit = stalloc(&s_eventargs, sizeof(struct proj_hit));
        hit->ent_uid = hit_ent;
        hit->proj_uid = proj->uid;
        hit->parent_uid = proj->ent_parent;
        hit->cookie = proj->cookie;
        E_Global_Notify(EVENT_PROJECTILE_HIT, hit, ES_ENGINE);

        vec_proj_push(&s_deleted, *proj);
        vec_proj_del(&s_front, front_idx);
    }
}

static void on_30hz_tick(void *user, void *event)
{
    PERF_PUSH("projectile::on_30hz_tick");

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
    PERF_POP();
}

static void phys_create_render_input(struct render_input *out)
{
    out->cam = G_GetActiveCamera();
    out->map = G_GetPrevTickMap();
    out->shadows = false;
    out->light_pos = G_GetLightPos();

    vec_rstat_init(&out->cam_vis_stat);
    vec_ranim_init(&out->cam_vis_anim);

    vec_rstat_init(&out->light_vis_stat);
    vec_ranim_init(&out->light_vis_anim);

    for(int i = 0; i < vec_size(&s_front); i++) {

        const struct projectile *curr = &vec_AT(&s_front, i);
        if(!curr->render_private)
            continue;
        struct ent_stat_rstate rstate = (struct ent_stat_rstate){
            .render_private = curr->render_private,
            .model = curr->model,
            .translucent = false,
            .td = {0},
        };
        vec_rstat_push(&out->cam_vis_stat, rstate);
    }
}

static void phys_destroy_render_input(struct render_input *in)
{
    vec_rstat_destroy(&in->cam_vis_stat);
    vec_ranim_destroy(&in->cam_vis_anim);

    vec_rstat_destroy(&in->light_vis_stat);
    vec_ranim_destroy(&in->light_vis_anim);
}

static void *phys_push_render_input(struct render_input *in)
{
    struct render_input *ret = R_PushArg(in, sizeof(*in));

    if(in->cam_vis_stat.size) {
        ret->cam_vis_stat.array = R_PushArg(
            in->cam_vis_stat.array, 
            in->cam_vis_stat.size * sizeof(struct ent_stat_rstate)
        );
    }
    if(in->cam_vis_anim.size) {
        ret->cam_vis_anim.array = R_PushArg(
            in->cam_vis_anim.array, 
            in->cam_vis_anim.size * sizeof(struct ent_anim_rstate)
        );
    }
    if(in->light_vis_stat.size) {
        ret->light_vis_stat.array = R_PushArg(
            in->light_vis_stat.array, 
            in->light_vis_stat.size * sizeof(struct ent_stat_rstate)
        );
    }
    if(in->light_vis_anim.size) {
        ret->light_vis_anim.array = R_PushArg(
            in->light_vis_anim.array, 
            in->light_vis_anim.size * sizeof(struct ent_anim_rstate)
        );
    }
    return ret;
}

static void on_render_3d(void *user, void *arg)
{
    struct render_input rinput;
    phys_create_render_input(&rinput);

    enum batch_id id = BATCH_ID_PROJECTILE;
    struct render_input *pushed = phys_push_render_input(&rinput);

    R_PushCmd((struct rcmd){
        .func = R_GL_Batch_DrawWithID,
        .nargs = 2,
        .args = {
            pushed,
            R_PushArg(&id, sizeof(id)),
        },
    });

    phys_destroy_render_input(&rinput);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

uint32_t P_Projectile_Add(vec3_t origin, vec3_t velocity, uint32_t ent_parent, int faction_id, 
                          uint32_t cookie, int flags, struct proj_desc pd)
{
    uint32_t ret = s_next_uid++;
    struct projectile proj = (struct projectile){
        .uid = ret,
        .ent_parent = ent_parent,
        .cookie = cookie,
        .flags = flags,
        .faction_id = faction_id,
        .render_private = AL_RenderPrivateForName(pd.basedir, pd.pfobj),
        .pos = origin,
        .vel = velocity,
        .scale = pd.scale,
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
    phys_proj_join_work();
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
    if(PFM_Vec3_Len(&velocity) <= EPSILON) {
        return false;
    }
    assert(PFM_Vec3_Len(&velocity) > EPSILON);
    PFM_Vec3_Normal(&velocity, &velocity);
    PFM_Vec3_Scale(&velocity, v, &velocity);

    *out = velocity;
    return true;
}

bool P_Projectile_SaveState(struct SDL_RWops *stream)
{
    phys_proj_finish_work();
    vec_proj_concat(&s_front, &s_added);
    vec_proj_reset(&s_added);
    /* 's_front' now has the most up-to-date projectile state */

    struct attr num_proj = (struct attr){
        .type = TYPE_INT,
        .val.as_int = vec_size(&s_front)
    };
    CHK_TRUE_RET(Attr_Write(stream, &num_proj, "num_proj"));
    Sched_TryYield();

    for(int i = 0; i < vec_size(&s_front); i++) {
   
        const struct projectile *curr = &vec_AT(&s_front, i);

        struct attr uid = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr->uid,
        };
        CHK_TRUE_RET(Attr_Write(stream, &uid, "uid"));

        struct attr ent_parent = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr->ent_parent,
        };
        CHK_TRUE_RET(Attr_Write(stream, &ent_parent, "ent_parent"));

        struct attr cookie = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr->cookie,
        };
        CHK_TRUE_RET(Attr_Write(stream, &cookie, "cookie"));

        struct attr flags = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr->flags,
        };
        CHK_TRUE_RET(Attr_Write(stream, &flags, "flags"));

        struct attr faction_id = (struct attr){
            .type = TYPE_INT,
            .val.as_int = curr->faction_id,
        };
        CHK_TRUE_RET(Attr_Write(stream, &faction_id, "faction_id"));

        char dir[512] = "", name[512] = "";
        AL_NameForRenderPrivate(curr->render_private, dir, name);

        struct attr basedir = (struct attr){
            .type = TYPE_STRING,
        };
        pf_strlcpy(basedir.val.as_string, dir, sizeof(basedir.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &basedir, "basedir"));

        struct attr filename = (struct attr){
            .type = TYPE_STRING,
        };
        pf_strlcpy(filename.val.as_string, name, sizeof(filename.val.as_string));
        CHK_TRUE_RET(Attr_Write(stream, &filename, "filename"));

        struct attr pos = (struct attr){
            .type = TYPE_VEC3,
            .val.as_vec3 = curr->pos,
        };
        CHK_TRUE_RET(Attr_Write(stream, &pos, "pos"));

        struct attr vel = (struct attr){
            .type = TYPE_VEC3,
            .val.as_vec3 = curr->vel,
        };
        CHK_TRUE_RET(Attr_Write(stream, &vel, "vel"));

        struct attr scale = (struct attr){
            .type = TYPE_VEC3,
            .val.as_vec3 = curr->scale,
        };
        CHK_TRUE_RET(Attr_Write(stream, &scale, "scale"));
        Sched_TryYield();

        /* No need to save the matrix - it is fully derived */
    }

    struct attr next_uid = (struct attr){
        .type = TYPE_INT,
        .val.as_int = s_next_uid,
    };
    CHK_TRUE_RET(Attr_Write(stream, &next_uid, "next_uid"));

    return true;
}

bool P_Projectile_LoadState(struct SDL_RWops *stream)
{
    struct attr attr;

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    const int num_proj = attr.val.as_int;
    Sched_TryYield();

    for(int i = 0; i < num_proj; i++) {

        struct projectile proj;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        proj.uid = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        proj.ent_parent = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        proj.cookie = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        proj.flags = attr.val.as_int;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_INT);
        proj.faction_id = attr.val.as_int;

        char dir[512], name[512];
        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);
        pf_strlcpy(dir, attr.val.as_string, ARR_SIZE(dir));

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_STRING);
        pf_strlcpy(name, attr.val.as_string, ARR_SIZE(name));

        AL_PreloadPFObj(dir, name);
        proj.render_private = AL_RenderPrivateForName(dir, name);

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC3);
        proj.pos = attr.val.as_vec3;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC3);
        proj.vel = attr.val.as_vec3;

        CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
        CHK_TRUE_RET(attr.type == TYPE_VEC3);
        proj.scale = attr.val.as_vec3;

        /* Lastly, derive the most up-to-date model matrix */
        mat4x4_t trans, scale, rot, tmp;
        quat_t qrot = phys_velocity_dir(proj.vel);

        PFM_Mat4x4_MakeTrans(proj.pos.x, proj.pos.y, proj.pos.z, &trans);
        PFM_Mat4x4_MakeScale(proj.scale.x, proj.scale.y, proj.scale.z, &scale);
        PFM_Mat4x4_RotFromQuat(&qrot, &rot);

        PFM_Mat4x4_Mult4x4(&scale, &rot, &tmp);
        PFM_Mat4x4_Mult4x4(&trans, &tmp, &proj.model);

        /* Add it to the list of projectiles */
        vec_proj_push(&s_front, proj);
        vec_proj_push(&s_back, proj);
        Sched_TryYield();
    }

    CHK_TRUE_RET(Attr_Parse(stream, &attr, true));
    CHK_TRUE_RET(attr.type == TYPE_INT);
    s_next_uid = attr.val.as_int;

    return true;
}

void P_Projectile_ClearState(void)
{
    s_work.ntasks = 0;
    stalloc_clear(&s_eventargs);
    stalloc_clear(&s_work.mem);
    vec_proj_reset(&s_front);
    vec_proj_reset(&s_back);
    vec_proj_reset(&s_added);
    vec_proj_reset(&s_deleted);
}

