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

#ifndef PY_PROF_H
#define PY_PROF_H

#include <Python.h>
#include <stdbool.h>

/* Records every stretch of Python the engine runs (a 'slice') and, at level 2,
 * every binding it calls, into the file named by pf.debug.python_profile_file. 
 * Level 1 of pf.debug.python_profile records slices only and keeps the timings 
 * honest; level 2 adds the bindings for classifying the slices offline.
 */

enum pyprof_kind{
    PYPROF_HANDLER,
    PYPROF_HANDLER_IMMEDIATE,
    PYPROF_TASK,
    PYPROF_WINDOW,
    PYPROF_REGION,
    PYPROF_ENGINE,
    PYPROF_VIEW_TASK,
};

enum pyprof_stage{
    PYPROF_STAGE_PRE,
    PYPROF_STAGE_EVENTS,
    PYPROF_STAGE_UPDATE,
    PYPROF_STAGE_RENDER,
    PYPROF_STAGE_SCHED,
    PYPROF_STAGE_POST,
};

extern int g_pyprof_level;

void PyProf_Init(PyObject *module);
void PyProf_Shutdown(void);
void PyProf_SetStage(enum pyprof_stage stage);
/* 'obj' names the slice: a callable, a task or window object; 'tag' tells
 * apart the several methods called on one object.
 */
void PyProf_SliceBegin(enum pyprof_kind kind, PyObject *obj, const char *tag);
void PyProf_SliceEnd(void);
void PyProf_WatchThreadState(PyThreadState *ts);
/* Records a binding the profile hook cannot see, e.g. a type's constructor */
void PyProf_PseudoEnter(const char *name);
void PyProf_PseudoExit(void);

#define PYPROF_ON() (g_pyprof_level > 0)

#endif
