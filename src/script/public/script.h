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

#include <stdio.h>
#include <stdbool.h>

/* 'Handle' type to let the rest of the engine hold on to scripting objects 
 * without needing to include Python.h */
typedef void *script_opaque_t;

enum eventtype;

/*###########################################################################*/
/* SCRIPT GENERAL                                                            */
/*###########################################################################*/

bool            S_Init(char *progname, const char *base_path);
void            S_Shutdown(void);
bool            S_RunFile(const char *path);

void            S_RunEventHandler(script_opaque_t callable, script_opaque_t user_arg, 
                                  void *event_arg);

/* Decrement reference count for Python objects. 
 * No-op in the case of a NULL-pointer passed in */
void            S_Release(script_opaque_t obj);

script_opaque_t S_WrapEngineEventArg(enum eventtype e, void *arg);


#endif

