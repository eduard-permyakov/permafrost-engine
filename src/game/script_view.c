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
#include "../pf_math.h"

#include <string.h>
#include <assert.h>

/* The world as the script thread's view tasks read it: copies the main
 * thread makes once a frame, at a point where the world is still. There are
 * three, so that the main thread always has one to fill that is neither the
 * newest nor the one a view slice holds.
 */

#define NVIEWS          (3)
#define MAX_VIEW_CLUSTERS (1024)

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
    khash_t(id)              *home_districts;
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
static bool                s_init = false;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void view_fill(struct script_view *view)
{
    G_FlagsCopyTableInto(view->flags);
    G_FactionIDCopyTableInto(view->factions);
    G_Pos_CopyTableInto(view->positions);

    kh_clear(id, view->home_districts);
    uint32_t uid;
    int flags;
    kh_foreach(view->flags, uid, flags, {
        if(!(flags & ENTITY_FLAG_HARVESTER))
            continue;
        int ret;
        khiter_t k = kh_put(id, view->home_districts, uid, &ret);
        if(ret != -1)
            kh_value(view->home_districts, k) = G_Automation_GetHomeDistrict(uid);
    });

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
        view->home_districts = kh_init(id);
        view->districts = G_District_SnapshotNew();
        if(!view->flags || !view->factions || !view->positions
        || !view->home_districts || !view->districts)
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
        if(view->home_districts) kh_destroy(id, view->home_districts);
        G_District_SnapshotFree(view->districts);
        memset(view, 0, sizeof(*view));
    }
    LF_STORE(&s_published, -1, LF_SC);
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
}

void G_ScriptView_Invalidate(void)
{
    ASSERT_IN_MAIN_THREAD();
    LF_STORE(&s_published, -1, LF_SC);
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
    khiter_t k = kh_get(id, view->home_districts, uid);
    if(k == kh_end(view->home_districts))
        return 0;
    return kh_value(view->home_districts, k);
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
