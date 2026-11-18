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

#include <Python.h> /* must be first */
#include "script_thread.h"
#include "py_task.h"
#include "py_entity.h"
#include "../sched.h"
#include "../lib/public/khash.h"
#include "../lib/public/vec.h"
#include "../lib/public/pf_string.h"
#include "../game/public/game.h"

#include <frameobject.h>
#include <string.h>

/* A view task runs on the script thread while the main thread changes the
 * world, so it may only reach what does not touch the world. Every binding a
 * view slice calls, and every attribute it reads, is checked against what is
 * allowed; anything else makes the rest of the slice run in the main thread's
 * next world window, against the live world.
 */

enum view_class{
    VIEW_PARK,
    VIEW_ALLOW,
    VIEW_POLL,
};

/* What a view slice read that the view does not hold: worked out once with
 * the world, kept, and worked out again each frame in a world window for as
 * long as it goes on being asked for. A window that shows a count every frame
 * then shows the last frame's, without waiting for the world.
 */
struct poll_entry{
    int       kind;
    void     *fn;
    PyObject *self;
    PyObject *args;
    PyObject *key;
    PyObject *value;
    uint32_t  asked;      /* the frame it was last asked for */
    bool      renewed;    /* asked for by a view slice: worth working out again */
    bool      used;
};

VEC_TYPE(poll, struct poll_entry)
VEC_IMPL(static inline, poll, struct poll_entry)

/* Entries a slice may add while it has the world: a task that reads a
 * thousand things once is not a window that reads the same few every frame
 */
#define POLL_LEARN_MAX  (1024)
/* Frames an entry is kept for after it was last asked for. The windows' frames
 * may come many of the main thread's frames apart when the script thread is busy.
 */
#define POLL_KEEP       (30)

struct view_getset{
    getter      get;
    setter      set;
    void       *closure;
    int         get_class;
    int         set_class;
    const char *name;
};

KHASH_MAP_INIT_INT64(vclass, int)
KHASH_MAP_INIT_INT64(vsite, int)

/* A place in the scripts a view slice waited for the world from */
struct park_site{
    char     file[96];
    int      line;
    char     what[48];
    uint64_t count;
};

VEC_TYPE(site, struct park_site)
VEC_IMPL(static inline, site, struct park_site)
VEC_TYPE(vuid, uint32_t)
VEC_IMPL(static inline, vuid, uint32_t)

VEC_TYPE(vcmd, struct view_cmd)
VEC_IMPL(static inline, vcmd, struct view_cmd)

/* What a view task may reach without the world: the task API, whose requests
 * go to the scheduler, the frame clock, and what the script view holds. An
 * attribute's reading and writing are named apart. */
static const char *s_allowed[] = {
    "pf.Task.",
    "pf.Window.",
    "pf.UI",
    "pf._ui_frame",
    "pf._dispatch_events",
    "pf.rand",
    "pf.Entity.register",
    "pf.Entity.unregister",
    "pf.register_event_handler",
    "pf.register_ui_event_handler",
    "pf.unregister_event_handler",
    "pf.draw_text",
    "pf.set_active_font",
    "pf.get_active_font",
    "pf.get_ticks",
    "pf.prev_frame_ms",
    "pf.Entity.uid.get",
    "pf.Entity.zombie.get",
    "pf.BuildableEntity.pos.get",
    "pf.Entity.pos.get",
    "pf.Entity.faction_id.get",
    "pf.HarvesterEntity.home_district.get",
    "pf.get_factions_list",
    "pf.clusters",
    "pf.cluster_anchor",
    "pf.district_at",
    "pf.Entity.is_garrisoned",
    "pf.GarrisonEntity.is_ingredient.get",
    "pf.BuildableEntity.completed.get",
    "pf.HarvesterEntity.automatic_work.get",
    "pf.HarvesterEntity.automatic_transport.get",
    "pf.HarvesterEntity.pinned_work.get",
    "pf.HarvesterEntity.player_managed.get",
    "pf.Entity.duty.get",
    "pf.Entity.idle.get",
    "pf.HarvesterEntity.queued_gather.get",
    "pf.HarvesterEntity.current_job",
    "pf.CombatableEntity.base_dmg.get",
    "pf.CombatableEntity.attack_range.get",
    "pf.workshop_posts",
    "pf.post_workshop",
    NULL
};

/* Functions and methods that keep their write for the next world window: each
 * hands it to S_View_Defer
 */
static const char *s_deferred[] = {
    "pf.AnimEntity.play_anim",
    "pf.play_effect",
    "pf.play_global_effect",
    NULL
};

/* Functions and methods that answer from what was polled: each begins with
 * S_VIEW_POLLED. Getters need no listing: one the view does not hold is polled.
 */
static const char *s_polled[] = {
    "pf.settings_get",
    "pf.get_resource_stored",
    "pf.get_resource_capacity",
    "pf.get_player_population",
    "pf.get_player_population_limit",
    "pf.get_population",
    "pf.get_population_limit",
    "pf.get_simstate",
    "pf.get_key_name",
    "pf.get_resolution",
    "pf.districts_shown",
    "pf.lens_shown",
    "pf.CombatableEntity.get_bonus",
    "pf.GarrisonableEntity.ingredients",
    NULL
};

static khash_t(vclass) *s_method_class;
/* The view the running view slice reads, if one was published */
static const struct script_view *s_current;
static vec_vuid_t       s_deferred_removes;
static vec_vcmd_t       s_cmds;
static PyObject        *s_poll_index;      /* key -> index into s_polls */
static vec_poll_t       s_polls;
static vec_vuid_t       s_poll_free;
static uint32_t         s_poll_frame;
/* Set from a view slice's park to its end: what it reads next is noted */
static bool             s_poll_learning;
/* Whether the running task's reads may be answered from what was polled. The
 * answers are as of the last refresh, before scripts wrote what they wrote
 * since, so only a task that shows what it reads may take them.
 */
static bool             s_task_polls;
static int              s_poll_learned;
/* Set while an entry is worked out, so that the call it makes is not itself looked up */
static bool             s_poll_working;
/* Counted for PF_VIEW_STATS */
static uint64_t         s_nparks, s_npoll_hits, s_npoll_learned, s_npoll_renewed, s_ncmds;
/* The places a view slice has parked from, each logged once */
static khash_t(vsite)  *s_park_sites;
static vec_site_t       s_sites;
static bool             s_init = false;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static int classify(const char *qualname)
{
    for(const char **curr = s_allowed; *curr; curr++) {
        if(0 == strncmp(qualname, *curr, strlen(*curr)))
            return VIEW_ALLOW;
    }
    for(const char **curr = s_polled; *curr; curr++) {
        if(0 == strcmp(qualname, *curr))
            return VIEW_POLL;
    }
    for(const char **curr = s_deferred; *curr; curr++) {
        if(0 == strcmp(qualname, *curr))
            return VIEW_ALLOW;
    }
    return VIEW_PARK;
}

/* Leaves the view slice for the next world window, saying the first time where
 * from, so that a read the view could serve is seen
 */
static int park(const char *what)
{
    PyFrameObject *frame = PyEval_GetFrame();
    if(frame) {
        int line = PyFrame_GetLineNumber(frame);
        uint64_t site = ((uint64_t)(uintptr_t)frame->f_code << 20) ^ (uint64_t)line;
        int ret;
        khiter_t k = kh_put(vsite, s_park_sites, site, &ret);
        if(ret > 0) {
            fprintf(stderr, "[view] a view task waits for the world at %s:%d (%s)\n",
                PyString_AsString(frame->f_code->co_filename), line, what);
            struct park_site ps = {.line = line, .count = 0};
            const char *file = PyString_AsString(frame->f_code->co_filename);
            size_t len = strlen(file);
            pf_strlcpy(ps.file, len >= sizeof(ps.file) ? file + len - sizeof(ps.file) + 1 : file, sizeof(ps.file));
            pf_strlcpy(ps.what, what, sizeof(ps.what));
            kh_value(s_park_sites, k) = vec_size(&s_sites);
            vec_site_push(&s_sites, ps);
        }
        if(ret != -1)
            vec_AT(&s_sites, kh_value(s_park_sites, k)).count++;
    }
    s_nparks++;
    S_Task_ViewPark();
    s_poll_learning = s_task_polls;
    s_poll_learned = 0;
    return PyErr_Occurred() ? -1 : 0;
}

static PyObject *poll_call(int kind, void *fn, PyObject *self, PyObject *args)
{
    switch(kind) {
    case VIEW_POLL_GETSET: {
        struct view_getset *vg = fn;
        return vg->get(self, vg->closure);
    }
    case VIEW_POLL_NOARGS:
        return ((PyCFunction)fn)(self, NULL);
    case VIEW_POLL_VARARGS:
        return ((PyCFunction)fn)(self, args);
    default: assert(0); return NULL;
    }
}

/* A caller that changed a list it was handed must not change what is kept */
static PyObject *poll_result(PyObject *value)
{
    if(PyList_CheckExact(value))
        return PyList_GetSlice(value, 0, PyList_GET_SIZE(value));
    if(PyDict_CheckExact(value))
        return PyDict_Copy(value);
    Py_INCREF(value);
    return value;
}

static void poll_drop(int idx)
{
    struct poll_entry *pe = &vec_AT(&s_polls, idx);
    assert(pe->used);
    PyDict_DelItem(s_poll_index, pe->key);
    PyErr_Clear();
    Py_CLEAR(pe->key);
    Py_CLEAR(pe->self);
    Py_CLEAR(pe->args);
    Py_CLEAR(pe->value);
    pe->used = false;
    vec_vuid_push(&s_poll_free, idx);
}

static PyObject *poll_key(void *fn, PyObject *self, PyObject *args)
{
    /* The object by where it is, as its class may have its own notion of
     * equality: the entry holds it, so the place is not reused meanwhile
     */
    return Py_BuildValue("(KKO)", (unsigned PY_LONG_LONG)(uintptr_t)fn,
        (unsigned PY_LONG_LONG)(uintptr_t)self, args ? args : Py_None);
}

static bool is_ours(PyTypeObject *type)
{
    return !(type->tp_flags & Py_TPFLAGS_HEAPTYPE)
        && 0 == strncmp(type->tp_name, "pf.", 3);
}

/* A non-zero return fails the call with the exception the task was given
 * while it waited, such as the one that ends it at teardown */
static int view_profile(PyObject *obj, PyFrameObject *frame, int what, PyObject *arg)
{
    if(what != PyTrace_C_CALL || !PyCFunction_Check(arg) || !Sched_InViewSlice())
        return 0;
    PyMethodDef *def = ((PyCFunctionObject*)arg)->m_ml;
    khiter_t k = kh_get(vclass, s_method_class, (uint64_t)(uintptr_t)def);
    if(k != kh_end(s_method_class) && kh_value(s_method_class, k) == VIEW_PARK)
        return park(def->ml_name);
    return 0;
}

static PyObject *view_get(PyObject *self, void *closure)
{
    struct view_getset *vg = closure;
    if(vg->get_class == VIEW_PARK) {
        PyObject *ret;
        if(S_View_Poll(VIEW_POLL_GETSET, vg, self, NULL, vg->name, &ret))
            return ret;
    }
    return vg->get(self, vg->closure);
}

static int view_set(PyObject *self, PyObject *value, void *closure)
{
    struct view_getset *vg = closure;
    if(vg->set_class == VIEW_PARK && Sched_InViewSlice() && park(vg->name) < 0)
        return -1;
    return vg->set(self, value, vg->closure);
}

static void note_methods(const char *owner, PyMethodDef *defs)
{
    char name[256];
    for(PyMethodDef *def = defs; def && def->ml_name; def++) {
        pf_snprintf(name, sizeof(name), "%s.%s", owner, def->ml_name);
        int ret;
        khiter_t k = kh_put(vclass, s_method_class, (uint64_t)(uintptr_t)def, &ret);
        if(ret != -1)
            kh_value(s_method_class, k) = classify(name);
    }
}

static void wrap_getsets(PyTypeObject *type)
{
    char name[256];
    for(PyGetSetDef *def = type->tp_getset; def && def->name; def++) {
        if(def->get == view_get)
            continue;
        struct view_getset *vg = PF_MALLOC(sizeof(struct view_getset));
        *vg = (struct view_getset){def->get, def->set, def->closure, VIEW_PARK, VIEW_PARK, def->name};
        pf_snprintf(name, sizeof(name), "%s.%s.get", type->tp_name, def->name);
        vg->get_class = classify(name);
        pf_snprintf(name, sizeof(name), "%s.%s.set", type->tp_name, def->name);
        vg->set_class = classify(name);
        def->get = def->get ? view_get : NULL;
        def->set = def->set ? view_set : NULL;
        def->closure = vg;
    }
}

static void note_module(PyObject *module, int depth)
{
    PyObject *dict = PyModule_GetDict(module);
    Py_ssize_t pos = 0;
    PyObject *key, *value;
    char name[256];

    while(PyDict_Next(dict, &pos, &key, &value)) {

        if(PyCFunction_Check(value)) {
            PyMethodDef *def = ((PyCFunctionObject*)value)->m_ml;
            pf_snprintf(name, sizeof(name), "%s.%s", PyModule_GetName(module), def->ml_name);
            int ret;
            khiter_t k = kh_put(vclass, s_method_class, (uint64_t)(uintptr_t)def, &ret);
            if(ret != -1)
                kh_value(s_method_class, k) = classify(name);
        }else if(PyType_Check(value) && is_ours((PyTypeObject*)value)) {
            PyTypeObject *type = (PyTypeObject*)value;
            note_methods(type->tp_name, type->tp_methods);
            wrap_getsets(type);
        }else if(depth == 0 && PyModule_Check(value) && value != module) {
            note_module(value, depth + 1);
        }
    }
}

static void view_unpin(void)
{
    if(s_current)
        G_ScriptView_Unpin();
    s_current = NULL;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void S_View_Init(struct _object *module)
{
    if(!s_init) {
        s_method_class = kh_init(vclass);
        s_park_sites = kh_init(vsite);
        vec_site_init(&s_sites);
        vec_vuid_init(&s_deferred_removes);
        vec_vcmd_init(&s_cmds);
        vec_poll_init(&s_polls);
        vec_vuid_init(&s_poll_free);
        s_init = true;
    }
    s_poll_index = PyDict_New();
    note_module(module, 0);
}

bool S_View_Active(void)
{
    return S_Thread_OnScript() && Sched_InViewSlice();
}

int S_View_RequireWorld(void)
{
    if(!S_View_Active())
        return 0;
    return park("a constructor or the view not yet published");
}

void S_View_EnterTask(bool view, bool poll)
{
    s_task_polls = poll;
    /* A view task with the world notes what it reads, to answer from later */
    s_poll_learning = view && poll && !Sched_InViewSlice();
    s_poll_learned = 0;
    PyThreadState *ts = PyThreadState_GET();
    if(view && Sched_InViewSlice()) {
        if(ts->c_profilefunc != view_profile)
            PyEval_SetProfile(view_profile, NULL);
    }else if(ts->c_profilefunc == view_profile) {
        PyEval_SetProfile(NULL, NULL);
    }
}

bool S_View_RunSlice(void)
{
    /* With no view published lately, the view tasks run in the world windows */
    if(!G_ScriptView_Current())
        return false;
    s_current = G_ScriptView_Pin();
    if(!s_current)
        return false;
    /* One that script code has written past since it was published is read
     * only by the tasks that just show what they read
     */
    bool ret = Sched_RunViewSlice(G_ScriptView_Fresh());
    view_unpin();
    return ret;
}

int S_View_Read(const struct script_view **out)
{
    *out = NULL;
    if(!S_View_Active())
        return 0;
    if(s_current) {
        *out = s_current;
        return 0;
    }
    /* No view was published: the read waits for the live world */
    return S_View_RequireWorld();
}

bool S_View_Poll(int kind, void *fn, struct _object *self, struct _object *args, 
                 const char *what, struct _object **out)
{
    bool view = Sched_InViewSlice();
    if(s_poll_working || !s_init)
        return false;
    if(view && !s_task_polls) {
        /* Read with the world, after the wait */
        if(park(what) < 0) {
            *out = NULL;
            return true;
        }
        return false;
    }
    if(!view && !s_poll_learning)
        return false;

    *out = NULL;
    PyObject *key = poll_key(fn, self, args);
    if(!key || PyObject_Hash(key) == -1) {
        /* Not to be looked up, such as a list among the arguments */
        PyErr_Clear();
        Py_XDECREF(key);
        if(view && park(what) < 0)
            return true;
        return false;
    }

    PyObject *slot = PyDict_GetItem(s_poll_index, key);
    if(slot) {
        struct poll_entry *pe = &vec_AT(&s_polls, PyInt_AS_LONG(slot));
        pe->asked = s_poll_frame;
        if(view) {
            pe->renewed = true;
            s_npoll_hits++;
            Py_DECREF(key);
            *out = poll_result(pe->value);
            return true;
        }
        /* With the world, the answer is the live one, and it is kept */
        Py_DECREF(key);
        s_poll_working = true;
        PyObject *value = poll_call(kind, fn, self, args);
        s_poll_working = false;
        if(!value)
            return true;
        Py_DECREF(pe->value);
        pe->value = value;
        *out = poll_result(value);
        return true;
    }

    if(view && park(what) < 0) {
        Py_DECREF(key);
        return true;
    }
    if(s_poll_learned >= POLL_LEARN_MAX) {
        Py_DECREF(key);
        return false;
    }

    s_poll_working = true;
    PyObject *value = poll_call(kind, fn, self, args);
    s_poll_working = false;
    if(!value) {
        Py_DECREF(key);
        return true;
    }

    int idx;
    if(vec_size(&s_poll_free) > 0) {
        idx = vec_vuid_pop(&s_poll_free);
    }else{
        idx = vec_size(&s_polls);
        vec_poll_push(&s_polls, (struct poll_entry){0});
    }
    slot = PyInt_FromLong(idx);
    if(!slot || 0 != PyDict_SetItem(s_poll_index, key, slot)) {
        PyErr_Clear();
        Py_XDECREF(slot);
        Py_DECREF(key);
        vec_vuid_push(&s_poll_free, idx);
        *out = value;
        return true;
    }
    Py_DECREF(slot);
    Py_XINCREF(self);
    Py_XINCREF(args);
    vec_AT(&s_polls, idx) = (struct poll_entry){
        .kind = kind,
        .fn = fn,
        .self = self,
        .args = args,
        .key = key,
        .value = value,
        .asked = s_poll_frame,
        .renewed = false,
        .used = true,
    };
    s_poll_learned++;
    s_npoll_learned++;
    *out = poll_result(value);
    return true;
}

void S_View_PollRefresh(void)
{
    if(!s_init || !s_poll_index)
        return;
    assert(!Sched_InViewSlice());

    s_poll_working = true;
    for(int i = 0; i < vec_size(&s_polls); i++) {
        struct poll_entry *pe = &vec_AT(&s_polls, i);
        if(!pe->used)
            continue;
        if(s_poll_frame - pe->asked >= POLL_KEEP) {
            poll_drop(i);
            continue;
        }
        if(!pe->renewed)
            continue;
        PyObject *value = poll_call(pe->kind, pe->fn, pe->self, pe->args);
        if(!value) {
            /* Such as a unit that died: whoever asks again finds out with the world */
            PyErr_Clear();
            poll_drop(i);
            continue;
        }
        Py_DECREF(pe->value);
        pe->value = value;
        s_npoll_renewed++;
    }
    s_poll_working = false;
    s_poll_frame++;
}

void S_View_LeaveTask(void)
{
    s_poll_learning = false;
    s_task_polls = false;
}

void S_View_Shutdown(void)
{
    if(!s_init || !s_poll_index)
        return;
    if(getenv("PF_VIEW_STATS")) {
        fprintf(stderr, "[view-stats] parks %llu, polled: answered %llu, learned %llu, renewed %llu over %u frames; "
            "writes kept %llu\n",
            (unsigned long long)s_nparks, (unsigned long long)s_npoll_hits,
            (unsigned long long)s_npoll_learned, (unsigned long long)s_npoll_renewed, s_poll_frame,
            (unsigned long long)s_ncmds);
        for(int i = 0; i < vec_size(&s_sites); i++) {
            const struct park_site *ps = &vec_AT(&s_sites, i);
            if(ps->count > 1) {
                fprintf(stderr, "[view-stats]   %llu waits at %s:%d (%s)\n",
                    (unsigned long long)ps->count, ps->file, ps->line, ps->what);
            }
        }
    }
    for(int i = 0; i < vec_size(&s_polls); i++) {
        if(vec_AT(&s_polls, i).used)
            poll_drop(i);
    }
    vec_poll_reset(&s_polls);
    vec_vuid_reset(&s_poll_free);
    vec_vcmd_reset(&s_cmds);
    Py_CLEAR(s_poll_index);
    s_poll_learning = false;
}

bool S_View_Defer(const struct view_cmd *cmd)
{
    if(!S_View_Active())
        return false;
    vec_vcmd_push(&s_cmds, *cmd);
    s_ncmds++;
    return true;
}

void S_View_ApplyCommands(void)
{
    if(!s_init)
        return;
    assert(!Sched_InViewSlice());
    /* One may queue no other: it has the world */
    for(int i = 0; i < vec_size(&s_cmds); i++) {
        const struct view_cmd *cmd = &vec_AT(&s_cmds, i);
        cmd->apply(cmd);
    }
    vec_vcmd_reset(&s_cmds);
}

void S_View_DeferRemove(uint32_t uid)
{
    vec_vuid_push(&s_deferred_removes, uid);
}

void S_View_ApplyDeferred(void)
{
    if(!s_init)
        return;
    for(int i = 0; i < vec_size(&s_deferred_removes); i++) {
        S_Entity_ReleaseUID(vec_AT(&s_deferred_removes, i));
    }
    vec_vuid_reset(&s_deferred_removes);
}
