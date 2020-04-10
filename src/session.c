/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020 Eduard Permyakov 
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

#include "session.h"
#include "event.h"
#include "main.h"
#include "lib/public/attr.h"
#include "game/public/game.h"
#include "script/public/script.h"


#define PFSAVE_VERSION  (1.0f)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static bool        s_load_requested = false;
static const char *s_load_path = NULL;

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool Session_Save(SDL_RWops *stream)
{
    //TODO: write some comment about the flow
    if(!G_SaveGlobalState(stream)) {
        printf("error writing map\n");
        return false;
    }

    if(!S_SaveState(stream)) {
        printf("error saving script state\n");
        return false;
    }

    if(!G_SaveEntityState(stream)) {
        printf("error saving game entity state\n");
        return false;
    }

    return true;
}

void Session_RequestLoad(const char *path)
{
    s_load_requested = true;
    s_load_path = path;
}

void Session_ServiceRequests(void)
{
    if(!s_load_requested)
        return;
    s_load_requested = false;

    E_DeleteScriptHandlers();
    S_ClearState();
    Engine_ClearPendingEvents();

    SDL_RWops *stream = SDL_RWFromFile(s_load_path, "r"); /* file will be closed when stream is */
    if(!stream)
        goto fail_file;

    if(!G_LoadGlobalState(stream)) {
        printf("fail map\n");
        goto fail_load;
    }

    if(!S_LoadState(stream)) {
        printf("fail script\n");
        goto fail_load;
    }

    if(!G_LoadEntityState(stream)) {
        printf("fail ent state\n");
        goto fail_load;
    }

    SDL_RWclose(stream);
    return;

fail_load:
    SDL_RWclose(stream);
fail_file:
    printf("we failed!!!\n");
    abort();
}

