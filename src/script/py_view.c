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
};

struct view_getset{
    getter get;
    setter set;
    void  *closure;
    int    get_class;
    int    set_class;
};

KHASH_MAP_INIT_INT64(vclass, int)
VEC_TYPE(vuid, uint32_t)
VEC_IMPL(static inline, vuid, uint32_t)

/* What a view task may reach without the world: the task API, whose requests
 * go to the scheduler, the frame clock, and what the script view holds. An
 * attribute's reading and writing are named apart. */
static const char *s_allowed[] = {
    "pf.Task.",
    "pf.get_ticks",
    "pf.prev_frame_ms",
    "pf.Entity.zombie.get",
    "pf.Entity.pos.get",
    "pf.Entity.faction_id.get",
    "pf.HarvesterEntity.home_district.get",
    "pf.get_factions_list",
    "pf.clusters",
    "pf.cluster_anchor",
    "pf.district_at",
    NULL
};

static khash_t(vclass) *s_method_class;
/* The view the running view slice reads, if one was published */
static const struct script_view *s_current;
static vec_vuid_t       s_deferred_removes;
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
    return VIEW_PARK;
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
        return S_View_RequireWorld();
    return 0;
}

static PyObject *view_get(PyObject *self, void *closure)
{
    struct view_getset *vg = closure;
    if(vg->get_class == VIEW_PARK && S_View_RequireWorld() < 0)
        return NULL;
    return vg->get(self, vg->closure);
}

static int view_set(PyObject *self, PyObject *value, void *closure)
{
    struct view_getset *vg = closure;
    if(vg->set_class == VIEW_PARK && S_View_RequireWorld() < 0)
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
        *vg = (struct view_getset){def->get, def->set, def->closure, VIEW_PARK, VIEW_PARK};
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

/* Gives the thread up between two instructions, for a call the main thread
 * made: the slice goes on as a view slice once the call is done */
static int view_preempt(void)
{
    if(!S_View_Active()) {
        *S_Thread_PreemptFlag() = 0;
        return 0;
    }
    S_Task_ViewYield(false);
    return PyErr_Occurred() ? -1 : 0;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void S_View_Init(struct _object *module)
{
    if(!s_init) {
        s_method_class = kh_init(vclass);
        vec_vuid_init(&s_deferred_removes);
        s_init = true;
    }
    note_module(module, 0);
    PyEval_SetPreemptHook(view_preempt, S_Thread_PreemptFlag());
}

bool S_View_Active(void)
{
    return S_Thread_OnScript() && Sched_InViewSlice();
}

int S_View_RequireWorld(void)
{
    if(!S_View_Active())
        return 0;
    S_Task_ViewYield(true);
    return PyErr_Occurred() ? -1 : 0;
}

void S_View_EnterTask(bool view)
{
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
    s_current = G_ScriptView_Pin();
    bool ret = Sched_RunViewSlice();
    G_ScriptView_Unpin();
    s_current = NULL;
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
