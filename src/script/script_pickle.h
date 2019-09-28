/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019 Eduard Permyakov 
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

#ifndef SCRIPT_PICKLE_H
#define SCRIPT_PICKLE_H

#include <Python.h> /* Must be included first */

#include "../lib/public/khash.h"
#include <SDL.h> /* for SDL_RWops */

#include <stdbool.h>

bool S_Pickle_Init(PyObject *module);
void S_Pickle_Shutdown(void);

bool S_PickleObjgraph(PyObject *obj, SDL_RWops *stream);
bool S_PickleObjgraphByName(const char *module, const char *name, SDL_RWops *stream);

/* Returns a new reference */
PyObject *S_UnpickleObjgraph(SDL_RWops *stream);

/* Sets the object as the value of the attribute in the specified module, potentially
 * overwriting an existing value. */
bool S_UnpickleObjgraphByName(const char *module, const char *name, SDL_RWops *stream);

#endif

