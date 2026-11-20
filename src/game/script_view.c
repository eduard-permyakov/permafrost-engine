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

#include "public/game.h"
#include "game_private.h"
#include "position.h"
#include "../main.h"
#include "../lib/public/lockfree.h"
#include "../lib/public/pf_string.h"
#include "../entity.h"
#include "../lib/public/pf_math.h"

#include <string.h>
#include <assert.h>

/* The world as the script thread's view tasks read it: copies the main
 * thread makes once a frame, at a point where the world is still. There are
 * three, so that the main thread always has one to fill that is neither the
 * newest nor the one a view slice holds.
 */

#define NVIEWS            (3)
#define MAX_VIEW_CLUSTERS (1024)
#define MAX_VIEW_WORKSHOPS (4096)
#define PINNED_LEN        (32)

enum{
    VU_AUTO_WORK       = (1 << 0),
    VU_AUTO_TRANSPORT  = (1 << 1),
    VU_PLAYER_MANAGED  = (1 << 2),
    VU_INGREDIENT      = (1 << 3),
    VU_COMPLETED       = (1 << 4),
    VU_IDLE            = (1 << 5),
    VU_HAS_DUTY        = (1 << 6),
    VU_HAS_QUEUED      = (1 << 7),
    VU_HAS_JOB         = (1 << 8),
};

/* What a unit's or building's script attributes read, beyond its flags,
 * faction and position
 */
struct view_unit{
    int      home_district;
    uint32_t post_workshop;
    uint32_t bits;
    char     pinned[PINNED_LEN];
    char     queued[PINNED_LEN];
    struct duty duty;
    int      job_state;
    uint32_t job_src;
    uint32_t job_dest;
    char     job_rname[PINNED_LEN];
};

struct view_posts{
    int posts, locked, inside, coming;
};

KHASH_MAP_INIT_INT(vunit, struct view_unit)
KHASH_MAP_INIT_INT(vposts, struct view_posts)

struct view_cluster{
    int      id;
    int      faction;
    uint32_t anchor;
    char     type[32];
};

struct script_view{
    khash_t(id)              *flags;
    khash_t(id)              *factions;
    khash_t(pos)             *positions;
    khash_t(vunit)           *units;
    khash_t(vposts)          *posts;
    khash_t(id)              *base_dmg;
    khash_t(range)           *ranges;
    uint64_t                  base_dmg_gen;
    uint16_t                  faction_mask;
    char                      faction_names[MAX_FACTIONS][MAX_FAC_NAME_LEN];
    vec3_t                    faction_colors[MAX_FACTIONS];
    bool                      faction_ctrl[MAX_FACTIONS];
    size_t                    nclusters;
    struct view_cluster       clusters[MAX_VIEW_CLUSTERS];
    struct district_snapshot *districts;
};

static struct script_view  s_views[NVIEWS];
static int                 s_published = -1;   /* atomic */
static int                 s_pinned = -1;      /* atomic */
/* Whether the published view is of the frame before this one: an older one is
 * not read
 */
static int                 s_fresh;            /* atomic */
/* The frame the last view was published in */
static unsigned long       s_published_frame;  /* atomic */
static bool                s_init = false;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static struct view_unit unit_fill(uint32_t uid, int flags)
{
    struct view_unit ret = {.post_workshop = NULL_UID};
    if(flags & ENTITY_FLAG_HARVESTER) {
        ret.home_district = G_Automation_GetHomeDistrict(uid);
        ret.post_workshop = G_Posts_WorkshopOf(uid);
        pf_strlcpy(ret.pinned, G_Automation_GetPinnedWork(uid), sizeof(ret.pinned));
        if(G_Automation_GetAutomaticWork(uid))
            ret.bits |= VU_AUTO_WORK;
        if(!(flags & ENTITY_FLAG_ZOMBIE) && G_Automation_GetAutomaticTransport(uid))
            ret.bits |= VU_AUTO_TRANSPORT;
        if(G_Automation_PlayerManaged(uid))
            ret.bits |= VU_PLAYER_MANAGED;
        if(G_Automation_IsIdle(uid))
            ret.bits |= VU_IDLE;
        if(G_Duty_Get(uid, &ret.duty))
            ret.bits |= VU_HAS_DUTY;
        const char *queued = G_Harvester_QueuedGather(uid);
        if(queued) {
            pf_strlcpy(ret.queued, queued, sizeof(ret.queued));
            ret.bits |= VU_HAS_QUEUED;
        }
        const char *rname = NULL;
        if(!(flags & ENTITY_FLAG_ZOMBIE)
        && G_Harvester_JobInfo(uid, &ret.job_src, &ret.job_dest, &rname, &ret.job_state)) {
            pf_strlcpy(ret.job_rname, rname ? rname : "", sizeof(ret.job_rname));
            ret.bits |= VU_HAS_JOB;
        }
    }
    if((flags & ENTITY_FLAG_GARRISONABLE) && !(flags & ENTITY_FLAG_ZOMBIE)
    && G_Garrison_IsIngredient(uid))
        ret.bits |= VU_INGREDIENT;
    if((flags & ENTITY_FLAG_BUILDING) && !(flags & ENTITY_FLAG_ZOMBIE)
    && G_Building_IsCompleted(uid))
        ret.bits |= VU_COMPLETED;
    return ret;
}

static const struct view_unit *view_unit(const struct script_view *view, uint32_t uid)
{
    khiter_t k = kh_get(vunit, view->units, uid);
    if(k == kh_end(view->units))
        return NULL;
    return &kh_value(view->units, k);
}

static void view_fill(struct script_view *view)
{
    G_FlagsCopyTableInto(view->flags);
    G_FactionIDCopyTableInto(view->factions);
    G_Pos_CopyTableInto(view->positions);

    kh_clear(vunit, view->units);
    uint32_t uid;
    int flags;
    kh_foreach(view->flags, uid, flags, {
        if(!(flags & (ENTITY_FLAG_HARVESTER | ENTITY_FLAG_GARRISONABLE | ENTITY_FLAG_BUILDING)))
            continue;
        /* The flags table keeps the flags of removed entities */
        if(!G_EntityExists(uid))
            continue;
        int ret;
        khiter_t k = kh_put(vunit, view->units, uid, &ret);
        if(ret == -1)
            continue;
        kh_value(view->units, k) = unit_fill(uid, flags);
    });

    kh_clear(vposts, view->posts);
    uint32_t workshops[MAX_VIEW_WORKSHOPS];
    size_t nworkshops = G_Posts_GetAll(MAX_VIEW_WORKSHOPS, workshops);
    for(size_t i = 0; i < nworkshops; i++) {
        struct view_posts vp;
        if(!G_Posts_Get(workshops[i], &vp.posts, &vp.locked, &vp.inside, &vp.coming))
            continue;
        int ret;
        khiter_t k = kh_put(vposts, view->posts, workshops[i], &ret);
        if(ret != -1)
            kh_value(view->posts, k) = vp;
    }

    view->faction_mask = G_GetFactions(view->faction_names, view->faction_colors, view->faction_ctrl);

    int ids[MAX_VIEW_CLUSTERS];
    view->nclusters = G_Cluster_GetAll(MAX_VIEW_CLUSTERS, ids);
    for(size_t i = 0; i < view->nclusters; i++) {
        struct view_cluster *vc = &view->clusters[i];
        vc->id = ids[i];
        vc->faction = G_Cluster_Faction(ids[i]);
        vc->anchor = G_Cluster_Anchor(ids[i]);
        pf_strlcpy(vc->type, G_Cluster_Type(ids[i]), sizeof(vc->type));
    }

    if(view->base_dmg_gen != G_Combat_BaseDamageGen()) {
        G_Combat_CopyBaseDamage(view->base_dmg);
        G_Combat_CopyRanges(view->ranges);
        view->base_dmg_gen = G_Combat_BaseDamageGen();
    }

    G_District_SnapshotUpdate(view->districts);
}

static const struct view_cluster *view_cluster(const struct script_view *view, int id)
{
    for(size_t i = 0; i < view->nclusters; i++) {
        if(view->clusters[i].id == id)
            return &view->clusters[i];
    }
    return NULL;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

static bool view_init(void)
{
    for(int i = 0; i < NVIEWS; i++) {
        struct script_view *view = &s_views[i];
        view->flags = kh_init(id);
        view->factions = kh_init(id);
        view->positions = kh_init(pos);
        view->units = kh_init(vunit);
        view->posts = kh_init(vposts);
        view->base_dmg = kh_init(id);
        view->ranges = kh_init(range);
        view->base_dmg_gen = ~(uint64_t)0;
        view->districts = G_District_SnapshotNew();
        if(!view->flags || !view->factions || !view->positions
        || !view->units || !view->posts || !view->base_dmg || !view->ranges || !view->districts)
            return false;
    }
    s_init = true;
    return true;
}

void G_ScriptView_Shutdown(void)
{
    if(!s_init)
        return;
    for(int i = 0; i < NVIEWS; i++) {
        struct script_view *view = &s_views[i];
        if(view->flags)          kh_destroy(id, view->flags);
        if(view->factions)       kh_destroy(id, view->factions);
        if(view->positions)      kh_destroy(pos, view->positions);
        if(view->units)          kh_destroy(vunit, view->units);
        if(view->posts)          kh_destroy(vposts, view->posts);
        if(view->base_dmg)       kh_destroy(id, view->base_dmg);
        if(view->ranges)         kh_destroy(range, view->ranges);
        G_District_SnapshotFree(view->districts);
        memset(view, 0, sizeof(*view));
    }
    LF_STORE(&s_published, -1, LF_SC);
    LF_STORE(&s_fresh, 0, LF_SC);
    s_init = false;
}

void G_ScriptView_Publish(void)
{
    ASSERT_IN_MAIN_THREAD();
    if(!G_MapLoaded())
        return;
    if(!s_init && !view_init())
        return;

    int published = LF_LOAD(&s_published, LF_SC);
    int pinned = LF_LOAD(&s_pinned, LF_SC);
    int next = 0;
    while(next == published || next == pinned)
        next++;

    view_fill(&s_views[next]);
    LF_STORE(&s_published, next, LF_SC);
    LF_STORE(&s_fresh, 1, LF_SC);
    __atomic_store_n(&s_published_frame, g_frame_idx, __ATOMIC_RELEASE);
}

bool G_ScriptView_Current(void)
{
    if(LF_LOAD(&s_published, LF_SC) < 0)
        return false;
    unsigned long frame = __atomic_load_n(&s_published_frame, __ATOMIC_ACQUIRE);
    return (__atomic_load_n(&g_frame_idx, __ATOMIC_RELAXED) - frame) <= 1;
}

void G_ScriptView_Skip(void)
{
    ASSERT_IN_MAIN_THREAD();
    LF_STORE(&s_fresh, 0, LF_SC);
}

void G_ScriptView_MarkStale(void)
{
    LF_STORE(&s_fresh, 0, LF_SC);
}

bool G_ScriptView_Fresh(void)
{
    return LF_LOAD(&s_fresh, LF_SC);
}

void G_ScriptView_Invalidate(void)
{
    ASSERT_IN_MAIN_THREAD();
    LF_STORE(&s_published, -1, LF_SC);
    LF_STORE(&s_fresh, 0, LF_SC);
}

bool G_ScriptView_Published(void)
{
    return LF_LOAD(&s_published, LF_SC) >= 0;
}

const struct script_view *G_ScriptView_Pin(void)
{
    for(;;) {
        int published = LF_LOAD(&s_published, LF_SC);
        if(published < 0)
            return NULL;
        LF_STORE(&s_pinned, published, LF_SC);
        /* The main thread fills only what it saw neither published nor
         * pinned: a view still published after the pin is safe to read */
        if(LF_LOAD(&s_published, LF_SC) == published)
            return &s_views[published];
    }
}

void G_ScriptView_Unpin(void)
{
    LF_STORE(&s_pinned, -1, LF_SC);
}

uint32_t G_ScriptView_Flags(const struct script_view *view, uint32_t uid)
{
    return G_FlagsGetFrom(view->flags, uid);
}

int G_ScriptView_FactionID(const struct script_view *view, uint32_t uid)
{
    return G_GetFactionIDFrom(view->factions, uid);
}

vec3_t G_ScriptView_Pos(const struct script_view *view, uint32_t uid)
{
    return G_Pos_GetFrom(view->positions, uid);
}

int G_ScriptView_HomeDistrict(const struct script_view *view, uint32_t uid)
{
    const struct view_unit *vu = view_unit(view, uid);
    return vu ? vu->home_district : 0;
}

bool G_ScriptView_AutomaticWork(const struct script_view *view, uint32_t uid)
{
    const struct view_unit *vu = view_unit(view, uid);
    return vu && (vu->bits & VU_AUTO_WORK);
}

bool G_ScriptView_AutomaticTransport(const struct script_view *view, uint32_t uid)
{
    const struct view_unit *vu = view_unit(view, uid);
    return vu && (vu->bits & VU_AUTO_TRANSPORT);
}

bool G_ScriptView_PlayerManaged(const struct script_view *view, uint32_t uid)
{
    const struct view_unit *vu = view_unit(view, uid);
    return vu && (vu->bits & VU_PLAYER_MANAGED);
}

const char *G_ScriptView_PinnedWork(const struct script_view *view, uint32_t uid)
{
    const struct view_unit *vu = view_unit(view, uid);
    return vu ? vu->pinned : "";
}

uint32_t G_ScriptView_PostWorkshop(const struct script_view *view, uint32_t uid)
{
    const struct view_unit *vu = view_unit(view, uid);
    return vu ? vu->post_workshop : NULL_UID;
}

bool G_ScriptView_IsIngredient(const struct script_view *view, uint32_t uid)
{
    const struct view_unit *vu = view_unit(view, uid);
    return vu && (vu->bits & VU_INGREDIENT);
}

bool G_ScriptView_Completed(const struct script_view *view, uint32_t uid)
{
    const struct view_unit *vu = view_unit(view, uid);
    return vu && (vu->bits & VU_COMPLETED);
}

bool G_ScriptView_Duty(const struct script_view *view, uint32_t uid, struct duty *out)
{
    const struct view_unit *vu = view_unit(view, uid);
    if(!vu || !(vu->bits & VU_HAS_DUTY))
        return false;
    *out = vu->duty;
    return true;
}

bool G_ScriptView_Idle(const struct script_view *view, uint32_t uid)
{
    const struct view_unit *vu = view_unit(view, uid);
    return !vu || (vu->bits & VU_IDLE);
}

const char *G_ScriptView_QueuedGather(const struct script_view *view, uint32_t uid)
{
    const struct view_unit *vu = view_unit(view, uid);
    return (vu && (vu->bits & VU_HAS_QUEUED)) ? vu->queued : NULL;
}

bool G_ScriptView_JobInfo(const struct script_view *view, uint32_t uid, uint32_t *out_src, 
                          uint32_t *out_dest, const char **out_rname, int *out_state)
{
    const struct view_unit *vu = view_unit(view, uid);
    if(!vu || !(vu->bits & VU_HAS_JOB))
        return false;
    *out_src = vu->job_src;
    *out_dest = vu->job_dest;
    *out_rname = vu->job_rname;
    *out_state = vu->job_state;
    return true;
}

bool G_ScriptView_BaseDamage(const struct script_view *view, uint32_t uid, int *out)
{
    khiter_t k = kh_get(id, view->base_dmg, uid);
    if(k == kh_end(view->base_dmg))
        return false;
    *out = kh_value(view->base_dmg, k);
    return true;
}

bool G_ScriptView_AttackRange(const struct script_view *view, uint32_t uid, float *out)
{
    khiter_t k = kh_get(range, view->ranges, uid);
    if(k == kh_end(view->ranges))
        return false;
    *out = kh_value(view->ranges, k);
    return true;
}

bool G_ScriptView_Posts(const struct script_view *view, uint32_t uid, int *out_posts, 
                        int *out_locked, int *out_inside, int *out_coming)
{
    khiter_t k = kh_get(vposts, view->posts, uid);
    if(k == kh_end(view->posts))
        return false;
    const struct view_posts *vp = &kh_value(view->posts, k);
    *out_posts = vp->posts;
    *out_locked = vp->locked;
    *out_inside = vp->inside;
    *out_coming = vp->coming;
    return true;
}

uint16_t G_ScriptView_Factions(const struct script_view *view,
                               char out_names[][MAX_FAC_NAME_LEN], vec3_t *out_colors, bool *out_ctrl)
{
    memcpy(out_names, view->faction_names, sizeof(view->faction_names));
    memcpy(out_colors, view->faction_colors, sizeof(view->faction_colors));
    memcpy(out_ctrl, view->faction_ctrl, sizeof(view->faction_ctrl));
    return view->faction_mask;
}

size_t G_ScriptView_Clusters(const struct script_view *view, size_t maxout, int out[])
{
    size_t n = (maxout < view->nclusters) ? maxout : view->nclusters;
    for(size_t i = 0; i < n; i++)
        out[i] = view->clusters[i].id;
    return n;
}

const char *G_ScriptView_ClusterType(const struct script_view *view, int id)
{
    const struct view_cluster *vc = view_cluster(view, id);
    return vc ? vc->type : "";
}

int G_ScriptView_ClusterFaction(const struct script_view *view, int id)
{
    const struct view_cluster *vc = view_cluster(view, id);
    return vc ? vc->faction : -1;
}

uint32_t G_ScriptView_ClusterAnchor(const struct script_view *view, int id)
{
    const struct view_cluster *vc = view_cluster(view, id);
    return vc ? vc->anchor : NULL_UID;
}

int G_ScriptView_DistrictAt(const struct script_view *view, int faction, bool water,
                            vec2_t pos, float *out_cost)
{
    return G_District_AtFrom(view->districts, faction, water, pos, out_cost);
}
