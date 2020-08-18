/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019-2020 Eduard Permyakov 
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

#ifndef PY_PICKLE_H
#define PY_PICKLE_H

#include <Python.h> /* Must be included first */

#include "../lib/public/khash.h"
#include "../lib/public/vec.h"

#include <stdbool.h>

struct SDL_RWops;

VEC_TYPE(pobj, PyObject*)
VEC_PROTOTYPES(extern, pobj, PyObject*)

bool S_Pickle_Init(PyObject *module);
void S_Pickle_Shutdown(void);

bool S_PickleObjgraph(PyObject *obj, struct SDL_RWops *stream);
/* Returns a new reference */
PyObject *S_UnpickleObjgraph(struct SDL_RWops *stream);

/* Expose some methods of the pickling module for use types implementing 
 * their own pickling and unpickling routines and needing to deal with
 * self-referencing cases. 
 */
struct py_pickle_ctx{
    void             *private_ctx;
    struct SDL_RWops *stream;
    bool (*memo_contains)(void *ctx, PyObject *obj);
    void (*memoize)(void *ctx, PyObject *obj);
    bool (*emit_put)(void *ctx, PyObject *obj, struct SDL_RWops *stream);
    bool (*emit_get)(void *ctx, PyObject *obj, struct SDL_RWops *stream);
    bool (*pickle_obj)(void *ctx, PyObject *obj, struct SDL_RWops *stream);
    void (*deferred_free)(void *ctx, PyObject *obj);
};

struct py_unpickle_ctx{
    vec_pobj_t *stack;
};

#endif

