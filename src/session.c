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
#include "ui.h"
#include "lib/public/attr.h"
#include "lib/public/pf_string.h"
#include "game/public/game.h"
#include "script/public/script.h"


#define PFSAVE_VERSION  (1.0f)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static bool s_load_requested = false;
static char s_load_path[512];
static char s_errstr[512];

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void session_print_message(void *user, void *event)
{
    UI_DrawText(s_errstr, (struct rect){5,5,600,50}, (struct rgba){255,255,255,255});
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool Session_Save(SDL_RWops *stream)
{
    struct attr version = (struct attr){
        .type = TYPE_FLOAT,
        .val.as_float = PFSAVE_VERSION
    };
    if(!Attr_Write(stream, &version, "version"))
        return false;

    /* First save the state of the map, lighting, camera, etc. (everything that 
     * isn't entities). Loading this state initalizes the session. */
    if(!G_SaveGlobalState(stream))
        return false;

    /* All live entities have a scripting object associated with them. Loading the
     * scripting state will re-create all the entities. */
    if(!S_SaveState(stream))
        return false;

    /* After the entities are loaded, populate all the auxiliary entity state that
     * isn't visible via the scripting API. (animation context, pricise movement 
     * state, etc) */
    if(!G_SaveEntityState(stream))
        return false;

    /* Roll forward the 'next_uid' so there's no collision with already loaded 
     * entities (which preserve their UIDs from the old session) */
    struct attr next_uid = (struct attr){
        .type = TYPE_INT,
        .val.as_int = Entity_NewUID()
    };
    if(!Attr_Write(stream, &next_uid, "next_uid"))
        return false;

    return true;
}

void Session_RequestLoad(const char *path)
{
    s_load_requested = true;
    pf_snprintf(s_load_path, sizeof(s_load_path), "%s", path);
}

void Session_ServiceRequests(void)
{
    struct attr attr;

    if(!s_load_requested)
        return;
    s_load_requested = false;
    E_Global_Unregister(EVENT_UPDATE_START, session_print_message);

    E_DeleteScriptHandlers();
    S_ClearState();
    Engine_ClearPendingEvents();

    SDL_RWops *stream = SDL_RWFromFile(s_load_path, "r"); /* file will be closed when stream is */
    if(!stream) {
        pf_snprintf(s_errstr, sizeof(s_errstr), "Could not open session file: %s", s_load_path);
        goto fail_file;
    }

    if(!Attr_Parse(stream, &attr, true) || attr.type != TYPE_FLOAT) {
        pf_snprintf(s_errstr, sizeof(s_errstr), "Could not read version from file: %s", s_load_path);
        goto fail_load;
    }

    if(attr.val.as_float > PFSAVE_VERSION) {
        pf_snprintf(s_errstr, sizeof(s_errstr), 
            "Incompatible save version: %.01f [Expecting %.01f or less]", attr.val.as_float, PFSAVE_VERSION);
        goto fail_load;
    }

    if(!G_LoadGlobalState(stream)) {
        pf_snprintf(s_errstr, sizeof(s_errstr), 
            "Could not de-serialize map and globals state from session file: %s", s_load_path);
        goto fail_load;
    }

    if(!S_LoadState(stream)) {
        pf_snprintf(s_errstr, sizeof(s_errstr), 
            "Could not de-serialize script-defined state from session file: %s", s_load_path);
        goto fail_load;
    }

    if(!G_LoadEntityState(stream)) {
        pf_snprintf(s_errstr, sizeof(s_errstr), 
            "Could not de-serialize addional entity state from session file: %s", s_load_path);
        goto fail_load;
    }

    if(!Attr_Parse(stream, &attr, true) || attr.type != TYPE_INT) {
        pf_snprintf(s_errstr, sizeof(s_errstr), 
            "Could not read version next_uid attribute from session file: %s", s_load_path);
        goto fail_load;
    }
    Entity_SetNextUID(attr.val.as_int);

    SDL_RWclose(stream);
    return;

fail_load:
    SDL_RWclose(stream);
fail_file:
    /* We've torn down the old session, but screwed up somewhere along the way
     * when creating the new session. Unfortunately we don't currently have a 
     * better recovery mechanism than dumping an error message and nuking the 
     * entire session. Perhaps we can save the old session prior to loading and 
     * roll back to it we can't load the new session... */
    S_ClearState();
    Engine_ClearPendingEvents();
    G_ClearState();
    E_Global_Register(EVENT_UPDATE_START, session_print_message, NULL, G_RUNNING | G_PAUSED_UI_RUNNING);

    fprintf(stderr, "%s\n", s_errstr);
    fflush(stderr);
}

