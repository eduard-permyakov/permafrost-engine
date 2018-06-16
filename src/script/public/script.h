/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2018 Eduard Permyakov 
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
 */

#ifndef SCRIPT_H
#define SCRIPT_H

#include "../../scene.h"

#include <stdio.h>
#include <stdbool.h>

/* 'Handle' type to let the rest of the engine hold on to scripting objects 
 * without needing to include Python.h */
typedef void *script_opaque_t;

enum eventtype;
struct nk_context;

/*###########################################################################*/
/* SCRIPT GENERAL                                                            */
/*###########################################################################*/

bool            S_Init(char *progname, const char *base_path, struct nk_context *ctx);
void            S_Shutdown(void);
bool            S_RunFile(const char *path);

void            S_RunEventHandler(script_opaque_t callable, script_opaque_t user_arg, 
                                  void *event_arg);

/* Decrement reference count for Python objects. 
 * No-op in the case of a NULL-pointer passed in */
void            S_Release(script_opaque_t obj);
script_opaque_t S_WrapEngineEventArg(enum eventtype e, void *arg);
bool            S_ObjectsEqual(script_opaque_t a, script_opaque_t b);

/*###########################################################################*/
/* SCRIPT UI                                                                 */
/*###########################################################################*/

bool S_UI_MouseOverWindow(int mouse_x, int mouse_y);

/*###########################################################################*/
/* SCRIPT ENTITY                                                             */
/*###########################################################################*/

script_opaque_t S_Entity_ObjFromAtts(const char *path, const char *name,
                                     const khash_t(attr) *attr_table, 
                                     const kvec_attr_t *construct_args);

#endif

