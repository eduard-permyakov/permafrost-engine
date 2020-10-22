/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2020 Eduard Permyakov 
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
    EVENT_RENDER_3D_PRE,
    EVENT_RENDER_3D_POST,
    EVENT_RENDER_UI,
    EVENT_SELECTED_TILE_CHANGED,
    EVENT_NEW_GAME,
    EVENT_UNIT_SELECTION_CHANGED,
    EVENT_60HZ_TICK,
    EVENT_30HZ_TICK,
    EVENT_20HZ_TICK,
    EVENT_15HZ_TICK,
    EVENT_10HZ_TICK,
    EVENT_1HZ_TICK,
    EVENT_ANIM_FINISHED,
    EVENT_ANIM_CYCLE_FINISHED,
    EVENT_MOVE_ISSUED,
    EVENT_MOTION_START,
    EVENT_MOTION_END,
    EVENT_ATTACK_START,
    EVENT_ENTITY_DEATH,
    EVENT_ATTACK_END,
    EVENT_GAME_SIMSTATE_CHANGED,
    EVENT_SESSION_LOADED,
    EVENT_SESSION_POPPED,
    EVENT_SESSION_FAIL_LOAD,
    EVENT_SCRIPT_TASK_EXCEPTION,
    EVENT_SCRIPT_TASK_FINISHED,
    EVENT_BUILD_BEGIN,
    EVENT_BUILD_END,
    EVENT_BUILD_FAIL_FOUND,
    EVENT_BUILD_TARGET_ACQUIRED,
    EVENT_BUILDING_COMPLETED,
    EVENT_ENTITY_DIED,
    EVENT_ENTITY_STOP,
    EVENT_HARVEST_BEGIN,
    EVENT_HARVEST_END,
    EVENT_HARVEST_TARGET_ACQUIRED,
    EVENT_STORAGE_TARGET_ACQUIRED,
    EVENT_RESOURCE_DROPPED_OFF,
    EVENT_RESOURCE_EXHAUSTED,

    EVENT_ENGINE_LAST = 0x1ffff,
};

enum event_source{
    ES_ENGINE,
    ES_SCRIPT,
};

typedef void (*handler_t)(void*, void*);

struct script_handler{
    enum eventtype  event;
    uint32_t        id;
    int             simmask;
    script_opaque_t handler;
    script_opaque_t arg;
};

/*###########################################################################*/
/* EVENT GENERAL                                                             */
/*###########################################################################*/

bool   E_Init(void);
void   E_ServiceQueue(void);
void   E_Shutdown(void);
void   E_DeleteScriptHandlers(void);
size_t E_GetScriptHandlers(size_t max_out, struct script_handler *out);
void   E_ClearPendingEvents(void);

/*###########################################################################*/
/* EVENT GLOBAL                                                              */
/*###########################################################################*/

void E_Global_Notify(enum eventtype event, void *event_arg, enum event_source);
void E_Global_NotifyImmediate(enum eventtype event, void *event_arg, enum event_source);

bool E_Global_Register(enum eventtype event, handler_t handler, void *user_arg,
                       int simmask);
bool E_Global_Unregister(enum eventtype event, handler_t handler);

bool E_Global_ScriptRegister(enum eventtype event, script_opaque_t handler, 
                             script_opaque_t user_arg, int simmask);
bool E_Global_ScriptUnregister(enum eventtype event, script_opaque_t handler);


/*###########################################################################*/
/* EVENT ENTITY                                                              */
/*###########################################################################*/

bool E_Entity_Register(enum eventtype event, uint32_t ent_uid, handler_t handler, 
                       void *user_arg, int simmask);
bool E_Entity_Unregister(enum eventtype event, uint32_t ent_uid, handler_t handler);

bool E_Entity_ScriptRegister(enum eventtype event, uint32_t ent_uid, 
                             script_opaque_t handler, script_opaque_t user_arg, int simmask);
bool E_Entity_ScriptUnregister(enum eventtype event, uint32_t ent_uid, 
                               script_opaque_t handler);
void E_Entity_Notify(enum eventtype, uint32_t ent_uid, void *event_arg, enum event_source);

#endif

