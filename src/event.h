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

#include "script/public/script.h"

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
     *
     * The very first event serviced during a tick is a single EVENT_UPDATE_START one.
     * The very last event serviced during a tick is a single EVENT_UPDATE_END one. 
     */
    EVENT_UPDATE_START = SDL_LASTEVENT + 1,
    EVENT_UPDATE_END,
    EVENT_UPDATE_UI,
    EVENT_RENDER_3D,
    EVENT_RENDER_UI,
    EVENT_SELECTED_TILE_CHANGED,
    EVENT_NEW_GAME,
    EVENT_UNIT_SELECTION_CHANGED,

    EVENT_ENGINE_LAST = 0x1ffff,
};

enum event_source{
    ES_ENGINE,
    ES_SCRIPT,
};

typedef void (*handler_t)(void*, void*);

/*###########################################################################*/
/* EVENT GENERAL                                                             */
/*###########################################################################*/

bool E_Init(void);
void E_ServiceQueue(void);
void E_Shutdown(void);

/*###########################################################################*/
/* EVENT GLOBAL                                                              */
/*###########################################################################*/

void E_Global_Notify(enum eventtype event, void *event_arg, enum event_source);
void E_Global_NotifyImmediate(enum eventtype event, void *event_arg, enum event_source);

bool E_Global_Register(enum eventtype event, handler_t handler, void *user_arg);
bool E_Global_Unregister(enum eventtype event, handler_t handler);

bool E_Global_ScriptRegister(enum eventtype event, script_opaque_t handler, 
                             script_opaque_t user_arg);
bool E_Global_ScriptUnregister(enum eventtype event, script_opaque_t handler);


/*###########################################################################*/
/* EVENT ENTITY                                                              */
/*###########################################################################*/

bool E_Entity_ScriptRegister(enum eventtype event, uint32_t ent_uid, 
                             script_opaque_t handler, script_opaque_t user_arg);
bool E_Entity_ScriptUnregister(enum eventtype event, uint32_t ent_uid, 
                               script_opaque_t handler);
void E_Entity_Notify(enum eventtype, uint32_t ent_uid, void *event_arg, enum event_source);

#endif

