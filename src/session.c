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
#include "lib/public/attr.h"
#include "game/public/game.h"
#include "script/public/script.h"

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static bool do_it = false;

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool Session_Save(SDL_RWops *stream)
{
    /* step 1: save map in PFMAP format */
    if(!G_WriteMap(stream)) {
        printf("error 1\n");
        return false;
    }

    /* step 2: save sys.modules['__main__'] */
    if(!S_WriteMainModule(stream)) {
        printf("error 2\n");
        return false;
    }

    /* step 3: save the event handlers and args (by qualname or by pickling) */
    return true;
}

void Session_RequestLoad(const char *path)
{
    do_it = true;
}

void Session_ServiceRequests(void)
{
    if(!do_it)
        return;

    /* step 1: load new map with specified PFMAP */

    /* step 2: clear current interpreter state */
    E_DeleteScriptHandlers();
    S_ClearState();

    /* step 3: unpickle the __main__ module */
    /* step 4: load scripting event handlers and args */

    do_it = false;
}

