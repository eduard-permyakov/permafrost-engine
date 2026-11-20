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

#include "py_prof.h"
#include "../main.h"
#include "../settings.h"
#include "public/script.h"
#include "../lib/public/khash.h"
#include "../lib/public/pf_malloc.h"
#include "../lib/public/pf_string.h"

#include <frameobject.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_DEPTH  (32)
#define MAX_CALLS  (4096)
#define MAX_BDEPTH (64)

/* The file holds three kinds of records, all little-endian:
 *   'N' u8 space, u32 id, u16 len, name   (space 0 = slice names, 1 = bindings)
 *   'S' u32 frame, u8 stage, u8 kind, u32 name id, u32 total ns, u32 self ns,
 *       u16 ncalls, u8 overflow, ncalls x (u16 binding id, u32 ns)
 */

KHASH_MAP_INIT_INT64(id, uint32_t)

struct bcall{
    uint16_t id;
    uint32_t ns;
};

struct bopen{
    uint16_t id;
    uint64_t t0;
};

struct slice{
    uint8_t      kind;
    uint32_t     name;
    uint64_t     t0;
    uint64_t     child_ns;
    int          ncalls;
    bool         overflow;
    struct bcall calls[MAX_CALLS];
    int          bdepth;
    struct bopen bstack[MAX_BDEPTH];
};

struct getset_wrap{
    getter    get;
    setter    set;
    void     *closure;
    uint16_t  get_id;
    uint16_t  set_id;
};

int                  g_pyprof_level = 0;

static FILE         *s_file;
static enum pyprof_stage s_stage;
static khash_t(id)  *s_slice_ids;
static khash_t(id)  *s_binding_ids;
static uint32_t      s_next_slice_id = 1;
static uint32_t      s_next_binding_id = 1;
static int           s_depth;
static bool          s_settings_created;
static struct slice  s_slices[MAX_DEPTH];

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void put(const void *data, size_t size)
{
    fwrite(data, size, 1, s_file);
}

static void put_name(uint8_t space, uint32_t id, const char *name)
{
    uint16_t len = (uint16_t)strlen(name);
    put("N", 1);
    put(&space, 1);
    put(&id, 4);
    put(&len, 2);
    put(name, len);
}

static uint32_t id_for(khash_t(id) *table, uint32_t *next, uint8_t space,
                       uint64_t key, const char *name)
{
    khiter_t k = kh_get(id, table, key);
    if(k != kh_end(table))
        return kh_value(table, k);
    int status;
    k = kh_put(id, table, key, &status);
    uint32_t ret = name ? (*next)++ : 0;
    kh_value(table, k) = ret;
    if(ret)
        put_name(space, ret, name);
    return ret;
}

static const char *file_base(PyCodeObject *code)
{
    const char *path = PyString_AS_STRING(code->co_filename);
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static const char *class_name(PyObject *cls)
{
    if(PyClass_Check(cls))
        return PyString_AS_STRING(((PyClassObject*)cls)->cl_name);
    if(PyType_Check(cls))
        return ((PyTypeObject*)cls)->tp_name;
    return "?";
}

static uint32_t slice_name_id(enum pyprof_kind kind, PyObject *obj, const char *tag)
{
    char name[256];
    uint64_t key;

    if(kind == PYPROF_HANDLER || kind == PYPROF_HANDLER_IMMEDIATE) {

        PyObject *func = obj;
        const char *cls = NULL;
        if(PyMethod_Check(obj)) {
            func = PyMethod_GET_FUNCTION(obj);
            PyObject *self = PyMethod_GET_SELF(obj);
            cls = self ? Py_TYPE(self)->tp_name : class_name(PyMethod_GET_CLASS(obj));
            if(self && PyInstance_Check(self))
                cls = class_name((PyObject*)((PyInstanceObject*)self)->in_class);
        }
        if(PyFunction_Check(func)) {
            PyCodeObject *code = (PyCodeObject*)PyFunction_GET_CODE(func);
            key = (uint64_t)(uintptr_t)code;
            khiter_t k = kh_get(id, s_slice_ids, key);
            if(k != kh_end(s_slice_ids))
                return kh_value(s_slice_ids, k);
            pf_snprintf(name, sizeof(name), "%s%s%s (%s:%d)", cls ? cls : "", cls ? "." : "",
                PyString_AS_STRING(code->co_name), file_base(code), code->co_firstlineno);
        }else{
            key = (uint64_t)(uintptr_t)Py_TYPE(obj);
            pf_snprintf(name, sizeof(name), "<%s>", Py_TYPE(obj)->tp_name);
        }
    }else{
        PyTypeObject *type = obj ? Py_TYPE(obj) : NULL;
        key = (uint64_t)(uintptr_t)type * 31 + (uint64_t)(uintptr_t)tag;
        khiter_t k = kh_get(id, s_slice_ids, key);
        if(k != kh_end(s_slice_ids))
            return kh_value(s_slice_ids, k);
        static const char *prefix[] = {"", "", "task:", "win:", "region:", "engine:", "view:"};
        pf_snprintf(name, sizeof(name), "%s%s%s%s", prefix[kind],
            type ? type->tp_name : "", (type && tag) ? "." : "", tag ? tag : "");
    }
    return id_for(s_slice_ids, &s_next_slice_id, 0, key, name);
}

static bool is_ours(PyTypeObject *type)
{
    return !(type->tp_flags & Py_TPFLAGS_HEAPTYPE)
        && 0 == strncmp(type->tp_name, "pf.", 3);
}

static uint16_t binding_id(PyCFunctionObject *func)
{
    uint64_t key = (uint64_t)(uintptr_t)func->m_ml;
    khiter_t k = kh_get(id, s_binding_ids, key);
    if(k != kh_end(s_binding_ids))
        return (uint16_t)kh_value(s_binding_ids, k);

    char name[256];
    const char *owner = NULL;
    PyObject *self = func->m_self;

    if(self && PyModule_Check(self)) {
        const char *modname = PyModule_GetName(self);
        if(modname && 0 == strncmp(modname, "pf", 2))
            owner = modname;
    }else if(self) {
        PyTypeObject *type = PyType_Check(self) ? (PyTypeObject*)self : Py_TYPE(self);
        PyObject *mro = type->tp_mro;
        for(Py_ssize_t i = 0; mro && i < PyTuple_GET_SIZE(mro) && !owner; i++) {
            PyObject *base = PyTuple_GET_ITEM(mro, i);
            if(!PyType_Check(base) || !is_ours((PyTypeObject*)base))
                continue;
            PyMethodDef *def = ((PyTypeObject*)base)->tp_methods;
            for(; def && def->ml_name; def++) {
                if(def == func->m_ml) {
                    owner = ((PyTypeObject*)base)->tp_name;
                    break;
                }
            }
        }
    }
    if(owner)
        pf_snprintf(name, sizeof(name), "%s.%s", owner, func->m_ml->ml_name);
    return (uint16_t)id_for(s_binding_ids, &s_next_binding_id, 1, key, owner ? name : NULL);
}

static void binding_enter(uint16_t id)
{
    if(s_depth == 0)
        return;
    struct slice *curr = &s_slices[s_depth - 1];
    if(curr->bdepth == MAX_BDEPTH)
        return;
    curr->bstack[curr->bdepth++] = (struct bopen){id, now_ns()};
}

static void binding_record(struct slice *curr, uint16_t id, uint64_t ns)
{
    if(!id)
        return;
    if(curr->ncalls == MAX_CALLS) {
        curr->overflow = true;
        return;
    }
    curr->calls[curr->ncalls++] = (struct bcall){id, (uint32_t)(ns > UINT32_MAX ? UINT32_MAX : ns)};
}

static void binding_exit(void)
{
    if(s_depth == 0)
        return;
    struct slice *curr = &s_slices[s_depth - 1];
    /* A task resumed after blocking in a binding returns from a call
     * that its previous slice already closed.
     */
    if(curr->bdepth == 0)
        return;
    struct bopen top = curr->bstack[--curr->bdepth];
    binding_record(curr, top.id, now_ns() - top.t0);
}

static int profile_hook(PyObject *obj, PyFrameObject *frame, int what, PyObject *arg)
{
    switch(what) {
    case PyTrace_C_CALL:
        binding_enter(PyCFunction_Check(arg) ? binding_id((PyCFunctionObject*)arg) : 0);
        break;
    case PyTrace_C_RETURN:
    case PyTrace_C_EXCEPTION:
        binding_exit();
        break;
    default:
        break;
    }
    return 0;
}

static PyObject *getset_get(PyObject *self, void *closure)
{
    struct getset_wrap *wrap = closure;
    binding_enter(wrap->get_id);
    PyObject *ret = wrap->get(self, wrap->closure);
    binding_exit();
    return ret;
}

static int getset_set(PyObject *self, PyObject *value, void *closure)
{
    struct getset_wrap *wrap = closure;
    binding_enter(wrap->set_id);
    int ret = wrap->set(self, value, wrap->closure);
    binding_exit();
    return ret;
}

static void wrap_getsets(PyTypeObject *type)
{
    char name[256];
    for(PyGetSetDef *def = type->tp_getset; def && def->name; def++) {
        if(def->get == getset_get)
            continue;
        struct getset_wrap *wrap = PF_MALLOC(sizeof(struct getset_wrap));
        *wrap = (struct getset_wrap){def->get, def->set, def->closure, 0, 0};
        if(def->get) {
            pf_snprintf(name, sizeof(name), "%s.%s.get", type->tp_name, def->name);
            wrap->get_id = (uint16_t)id_for(s_binding_ids, &s_next_binding_id, 1,
                (uint64_t)(uintptr_t)wrap * 2, name);
        }
        if(def->set) {
            pf_snprintf(name, sizeof(name), "%s.%s.set", type->tp_name, def->name);
            wrap->set_id = (uint16_t)id_for(s_binding_ids, &s_next_binding_id, 1,
                (uint64_t)(uintptr_t)wrap * 2 + 1, name);
        }
        def->get = def->get ? getset_get : NULL;
        def->set = def->set ? getset_set : NULL;
        def->closure = wrap;
    }
}

static void wrap_module_types(PyObject *module, int depth)
{
    PyObject *dict = PyModule_GetDict(module);
    Py_ssize_t pos = 0;
    PyObject *key, *value;
    char name[256];
    while(PyDict_Next(dict, &pos, &key, &value)) {
        /* Module functions are created without a 'self' to name their owner */
        if(PyCFunction_Check(value)) {
            PyMethodDef *def = ((PyCFunctionObject*)value)->m_ml;
            pf_snprintf(name, sizeof(name), "%s.%s", PyModule_GetName(module), def->ml_name);
            id_for(s_binding_ids, &s_next_binding_id, 1, (uint64_t)(uintptr_t)def, name);
        }
        if(PyType_Check(value) && is_ours((PyTypeObject*)value))
            wrap_getsets((PyTypeObject*)value);
        else if(depth == 0 && PyModule_Check(value) && value != module)
            wrap_module_types(value, depth + 1);
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

static bool level_validate(const struct sval *new_val)
{
    return (new_val->type == ST_TYPE_INT)
        && (new_val->as_int >= 0)
        && (new_val->as_int <= 2);
}

static bool path_validate(const struct sval *new_val)
{
    return (new_val->type == ST_TYPE_STRING);
}

/* Both are read once, when the interpreter starts */
static void create_settings(void)
{
    if(s_settings_created)
        return;
    s_settings_created = true;

    ss_e status = Settings_Create((struct setting){
        .name = "pf.debug.python_profile",
        .val = (struct sval) {
            .type = ST_TYPE_INT,
            .as_int = 0
        },
        .prio = 0,
        .validate = level_validate,
        .commit = NULL,
    });
    assert(status == SS_OKAY);

    status = Settings_Create((struct setting){
        .name = "pf.debug.python_profile_file",
        .val = (struct sval) {
            .type = ST_TYPE_STRING,
            .as_string = "pyprof.bin"
        },
        .prio = 0,
        .validate = path_validate,
        .commit = NULL,
    });
    assert(status == SS_OKAY);
    (void)status;
}

void PyProf_Init(PyObject *module)
{
    create_settings();

    struct sval level, path;
    if(Settings_Get("pf.debug.python_profile", &level) != SS_OKAY || level.as_int == 0
    || Settings_Get("pf.debug.python_profile_file", &path) != SS_OKAY)
        return;

    if(!s_file) {
        s_file = fopen(path.as_string, "wb");
        if(!s_file) {
            fprintf(stderr, "[py-prof] cannot open %s\n", path.as_string);
            return;
        }
        setvbuf(s_file, NULL, _IOFBF, 1 << 20);
        put("PYPROF1\n", 8);
        s_slice_ids = kh_init(id);
        s_binding_ids = kh_init(id);
        g_pyprof_level = level.as_int;
    }else{
        /* A new interpreter can reuse the addresses of the old code objects */
        kh_clear(id, s_slice_ids);
    }

    if(g_pyprof_level >= 2) {
        wrap_module_types(module, 0);
        PyEval_SetProfile(profile_hook, NULL);
    }
}

void PyProf_Shutdown(void)
{
    if(s_file)
        fflush(s_file);
}

void PyProf_SetStage(enum pyprof_stage stage)
{
    s_stage = stage;
    if(s_file && stage == PYPROF_STAGE_PRE && (g_frame_idx % 60) == 0)
        fflush(s_file);
}

void S_ProfSetStage(int stage)
{
    PyProf_SetStage((enum pyprof_stage)stage);
}

void PyProf_PseudoEnter(const char *name)
{
    if(g_pyprof_level < 2)
        return;
    binding_enter((uint16_t)id_for(s_binding_ids, &s_next_binding_id, 1, 
        (uint64_t)(uintptr_t)name, name));
}

void PyProf_PseudoExit(void)
{
    if(g_pyprof_level < 2)
        return;
    binding_exit();
}

void PyProf_WatchThreadState(PyThreadState *ts)
{
    if(g_pyprof_level >= 2 && ts->c_profilefunc != profile_hook)
        PyEval_SetProfile(profile_hook, NULL);
}

void PyProf_SliceBegin(enum pyprof_kind kind, PyObject *obj, const char *tag)
{
    if(!s_file)
        return;
    if(s_depth == MAX_DEPTH) {
        s_depth++;
        return;
    }
    /* 'pf.debug.trace_python' resets the main thread state's profile function */
    if(g_pyprof_level >= 2 && PyThreadState_GET()->c_profilefunc != profile_hook)
        PyEval_SetProfile(profile_hook, NULL);

    struct slice *curr = &s_slices[s_depth++];
    curr->kind = kind;
    curr->name = slice_name_id(kind, obj, tag);
    curr->child_ns = 0;
    curr->ncalls = 0;
    curr->overflow = false;
    curr->bdepth = 0;
    curr->t0 = now_ns();
}

void PyProf_SliceEnd(void)
{
    if(!s_file || s_depth == 0)
        return;
    if(s_depth > MAX_DEPTH) {
        s_depth--;
        return;
    }
    uint64_t now = now_ns();
    struct slice *curr = &s_slices[--s_depth];
    while(curr->bdepth > 0) {
        struct bopen top = curr->bstack[--curr->bdepth];
        binding_record(curr, top.id, now - top.t0);
    }

    uint64_t total = now - curr->t0;
    uint64_t self = total > curr->child_ns ? total - curr->child_ns : 0;
    if(s_depth > 0)
        s_slices[s_depth - 1].child_ns += total;

    uint32_t frame = (uint32_t)g_frame_idx;
    uint8_t stage = (uint8_t)s_stage;
    uint32_t total32 = total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
    uint32_t self32 = self > UINT32_MAX ? UINT32_MAX : (uint32_t)self;
    uint16_t ncalls = (uint16_t)curr->ncalls;
    uint8_t overflow = curr->overflow;

    put("S", 1);
    put(&frame, 4);
    put(&stage, 1);
    put(&curr->kind, 1);
    put(&curr->name, 4);
    put(&total32, 4);
    put(&self32, 4);
    put(&ncalls, 2);
    put(&overflow, 1);
    for(int i = 0; i < curr->ncalls; i++) {
        put(&curr->calls[i].id, 2);
        put(&curr->calls[i].ns, 4);
    }
}
