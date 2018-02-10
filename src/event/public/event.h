/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018 Eduard Permyakov 
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

#ifndef EVENT_H
#define EVENT_H

#include "../../script/public/script.h"

#include <SDL_events.h>
#include <stdbool.h>


enum eventtype{
    /*
     * +-----------------+-----------------------------------------------+
     * | Range           | Use                                           |
     * +-----------------+-----------------------------------------------+
     * | 0x0-0xffff      | SDL events                                    |
     * +-----------------+-----------------------------------------------+
     * | 0x10000-0x1ffff | Engine-generated events                       |
     * +-----------------+-----------------------------------------------+
     * | 0x20000-0x2ffff | Script-generated events                       |
     * +-----------------+-----------------------------------------------+
     */
    EVENT_UPDATE_START = SDL_LASTEVENT + 1,
    EVENT_UPDATE_END,
    EVENT_RENDER_START,
    EVENT_RENDER_END,

    EVENT_ENGINE_LAST = 0x1ffff,
};

typedef void (*handler_t)(void*, void*);

bool E_Global_Init(void);
void E_Global_Broadcast(enum eventtype event, void *event_arg);
void E_Global_Shutdown(void);

bool E_Global_Register(enum eventtype event, handler_t handler, void *user_arg);
bool E_Global_Unregister(enum eventtype event, handler_t handler);
/* If 'freefunc' is not NULL, it gets called with the 'handler' and 'user_arg' as arguments
 * at time of unregistering or shutdown. */
bool E_Global_ScriptRegister(enum eventtype event, script_opaque_t handler, script_opaque_t user_arg,
                             void (*freefunc)(script_opaque_t, script_opaque_t));
bool E_Global_ScriptUnregister(enum eventtype event, script_opaque_t handler);

#endif
