/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2020-2023 Eduard Permyakov 
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
#include "sched.h"
#include "cursor.h"
#include "asset_load.h"
#include "lib/public/attr.h"
#include "lib/public/pf_string.h"
#include "lib/public/vec.h"
#include "lib/public/mem.h"
#include "lib/public/SDL_vec_rwops.h"
#include "navigation/public/nav.h"
#include "game/public/game.h"
#include "script/public/script.h"
#include "audio/public/audio.h"
#include "phys/public/phys.h"
#include "anim/public/anim.h"

#include <SDL.h> /* for SDL_RWops */
#include <assert.h>


#define PFSAVE_VERSION  (1.0f)
#define MIN(a, b)       ((a) < (b) ? (a) : (b))

VEC_TYPE(stream, SDL_RWops*)
VEC_IMPL(static, stream, SDL_RWops*)

enum srequest{
    SESH_REQ_NONE,
    SESH_REQ_SAVE,
    SESH_REQ_LOAD,
    SESH_REQ_PUSH,
    SESH_REQ_POP,
    SESH_REQ_POP_TO_ROOT,
    SESH_REQ_EXEC,
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

/* A subsession describes the current state of the engine. 
 * A session is a stack of subsessions.
 */

static vec_stream_t    s_subsession_stack;
static enum srequest   s_request = SESH_REQ_NONE;
static enum srequest   s_current = SESH_REQ_NONE;
static int             s_argc;
static char            s_argv[MAX_ARGC][128];
static char            s_req_path[512];
static char            s_errbuff[512] = {0};
static bool            s_pushing = false;
static uint64_t        s_change_tick = UINT64_MAX;

static struct arg_desc s_saved_args;
static char            s_saved_argv[MAX_ARGC + 1][128];

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void subsession_clear(void)
{
    Sched_ClearState();
    E_DeleteScriptHandlers();
    Cursor_ClearState();
    N_ClearState();
    S_ClearState();
    G_ClearRenderWork();
    G_ClearState();
    A_ClearState();
    Entity_ClearState();
    Audio_ClearState();
    P_Projectile_ClearState();
    UI_ClearState();
    AL_ClearState();
    E_ClearPendingEvents();
}

static void subsession_save_args(void)
{
    s_saved_argv[0][0] = '\0';
    S_GetFilePath(s_saved_argv[0], sizeof(s_saved_argv[0]));

    memcpy(s_saved_argv + 1, s_argv, sizeof(s_argv));
    s_saved_args.argc = s_argc + 1;

    for(int i = 0; i < s_argc + 1; i++) {
        s_saved_args.argv[i] = s_saved_argv[i];
    }
}

static void subsession_flush(void)
{
    /* Drain the event queue to make sure we don't lose any events 
     * when moving from session to session. A 'lost' event can cause
     * some event-driven state machines to enter a bad state. 
     * 
     * In addition, some tasks may have been put in the ready queue
     * but not yet run. To ensure that the reactivation of the tasks 
     * also does not become 'lost', we drain the ready queues such that
     * all tasks have either completed or are blocked. This is a nice
     * boundary to capture the state on.
     *
     * As task execution can generate events and event handling can 
     * unblock tasks, we keep doing it until the entire event chain is 
     * completed.
     */
    do{
        G_FlushWork();
        E_FlushEventQueue();
        Sched_Flush();
    }while(G_HasWork() || E_EventsQueued() || Sched_HasBlocked());
}

static bool subsession_save(SDL_RWops *stream)
{
    subsession_flush();

    if(!Cursor_SaveState(stream))
        return false;

    /* First save the state of the map, lighting, camera, etc. (everything that 
     * isn't entities). Loading this state initalizes the session. */
    if(!G_SaveGlobalState(stream))
        return false;

    /* All live entities have a scripting object associated with them. Loading the
     * scripting state will re-create all the entities. */
    if(!S_SaveState(stream))
        return false;

    /* Roll forward the 'next_uid' so there's no collision with already loaded 
     * entities (which preserve their UIDs from the old session) */
    struct attr next_uid = (struct attr){
        .type = TYPE_INT,
        .val.as_int = Entity_NewUID()
    };
    if(!Attr_Write(stream, &next_uid, "next_uid"))
        return false;

    /* After the entities are loaded, populate all the auxiliary entity state that
     * isn't visible via the scripting API. (animation context, pricise movement 
     * state, etc) */
    if(!G_SaveEntityState(stream))
        return false;

    if(!Audio_SaveState(stream))
        return false;

    if(!P_Projectile_SaveState(stream))
        return false;

    return true;
}

static bool subsession_load(SDL_RWops *stream, char *errstr, size_t errlen)
{
    struct attr attr;
    subsession_clear();

    if(!Cursor_LoadState(stream)) {
        pf_snprintf(errstr, errlen, 
            "Could not de-serialize cursor state from session file");
        goto fail;
    }

    if(!G_LoadGlobalState(stream)) {
        pf_snprintf(errstr, errlen, 
            "Could not de-serialize map and globals state from session file");
        goto fail;
    }

    if(!S_LoadState(stream)) {
        pf_snprintf(errstr, errlen, 
            "Could not de-serialize script-defined state from session file");
        goto fail;
    }

    if(!Attr_Parse(stream, &attr, true) || attr.type != TYPE_INT) {
        pf_snprintf(errstr, errlen, 
            "Could not read 'next_uid' attribute from session file");
        goto fail;
    }
    Entity_SetNextUID(attr.val.as_int);
    Sched_TryYield();

    if(!G_LoadEntityState(stream)) {
        pf_snprintf(errstr, errlen, 
            "Could not de-serialize additional entity state from session file");
        goto fail;
    }

    if(!Audio_LoadState(stream)) {
        pf_snprintf(errstr, errlen, 
            "Could not de-serialize audio state from session file");
        goto fail;
    }

    if(!P_Projectile_LoadState(stream)) {
        pf_snprintf(errstr, errlen, 
            "Could not de-serialize physics state from session file");
        goto fail;
    }

    /* We may have loaded some assets during the session loading 
     * process - make sure the appropriate initialization is performed 
     * by the render thread */
    E_ClearPendingEvents();
    Engine_FlushRenderWorkQueue();

    G_UpdateSimStateChangeTick();
    return true;

fail:
    subsession_clear();
    return false;
}

static bool session_load(const char *file, char *errstr, size_t errlen)
{
    bool ret = false;
    struct attr attr;

    vec_stream_t loaded;
    vec_stream_init(&loaded);

    /* First save the current subsession to memory. If things go sour, we will roll back to it */
    SDL_RWops *current = PFSDL_VectorRWOps();
    PFSDL_VectorRWOpsReserve(current, 64 * 1024 * 1024);

    bool result = subsession_save(current);
    assert(result);
    SDL_RWseek(current, 0, RW_SEEK_SET);

    SDL_RWops *stream = SDL_RWFromFile(file, "r");
    if(!stream) {
        pf_snprintf(errstr, errlen, "Could not open session file: %s", file);
        goto fail_stream;
    }

    if(!Attr_Parse(stream, &attr, true) || attr.type != TYPE_FLOAT) {
        pf_snprintf(errstr, errlen, "Could not read PFSAVE version");
        goto fail_parse;
    }

    if(attr.val.as_float > PFSAVE_VERSION) {
        pf_snprintf(errstr, errlen, 
            "Incompatible save version: %.01f [Expecting %.01f or less]", 
                attr.val.as_float, PFSAVE_VERSION);
        goto fail_parse;
    }

    if(!Attr_Parse(stream, &attr, true) || attr.type != TYPE_INT) {
        pf_snprintf(errstr, errlen, "Could not read number of subsessions");
        goto fail_parse;
    }

    for(int i = 0; i < attr.val.as_int; i++) {
    
        if(!subsession_load(stream, errstr, errlen)) {

            bool result = subsession_load(current, errstr, errlen);
            assert(result);
            goto fail_parse;
        }

        SDL_RWops *sub = PFSDL_VectorRWOps();
        PFSDL_VectorRWOpsReserve(sub, 64 * 1024 * 1024);

        bool result = subsession_save(sub);
        assert(result);
        SDL_RWseek(sub, 0, RW_SEEK_SET);

        vec_stream_push(&loaded, sub);
    }

    while(vec_size(&s_subsession_stack) > 0) {
        SDL_RWops *stream = vec_stream_pop(&s_subsession_stack);
        SDL_RWclose(stream);
    }

    SDL_RWclose(vec_stream_pop(&loaded));
    vec_stream_copy(&s_subsession_stack, &loaded);
    ret = true;

fail_parse:
    SDL_RWclose(stream);
fail_stream:
    SDL_RWclose(current);
    vec_stream_destroy(&loaded);
    return ret;
}

static bool session_pop_subsession(char *errstr, size_t errlen)
{
    if(vec_size(&s_subsession_stack) == 0) {
        pf_snprintf(errstr, errlen, "Cannot pop subsession: stack is empty");
        return false;
    }

    subsession_save_args();
    subsession_clear();

    SDL_RWops *stream = vec_stream_pop(&s_subsession_stack);
    bool result = subsession_load(stream, errstr, errlen);
    assert(result);

    E_Global_Notify(EVENT_SESSION_POPPED, &s_saved_args, ES_ENGINE);

    SDL_RWclose(stream);
    return true;
}

static bool session_pop_subsession_to_root(char *errstr, size_t errlen)
{
    if(vec_size(&s_subsession_stack) == 0) {
        pf_snprintf(errstr, errlen, "Cannot pop subsession: stack is empty");
        return false;
    }

    subsession_save_args();

    SDL_RWops *stream = vec_AT(&s_subsession_stack, 0);
    bool result = subsession_load(stream, errstr, errlen);
    assert(result);

    while(vec_size(&s_subsession_stack) > 0) {
        stream = vec_stream_pop(&s_subsession_stack);
        SDL_RWclose(stream);
    }

    E_Global_Notify(EVENT_SESSION_POPPED, &s_saved_args, ES_ENGINE);
    return true;
}

static bool session_push_subsession(const char *script, char *errstr, size_t errlen)
{
    SDL_RWops *stream = PFSDL_VectorRWOps();
    PFSDL_VectorRWOpsReserve(stream, 64 * 1024 * 1024);

    bool result, ret = false;
    (void)result;

    if(!subsession_save(stream)) {
        SDL_RWclose(stream);
        return false;
    }
    SDL_RWseek(stream, 0, RW_SEEK_SET);

    subsession_clear();

    STALLOC(char*, argv, s_argc);
    for(int i = 0; i < s_argc; i++)
        argv[i] = s_argv[i];

    if(!S_RunFile(script, s_argc, argv)) {
        result = subsession_load(stream, errstr, errlen);
        assert(result);
        SDL_RWclose(stream);
        goto out;
    }

    vec_stream_push(&s_subsession_stack, stream);
    ret = true;

out:
    STFREE(argv);
    return ret;
}

static bool session_exec_subsession(const char *script, char *errstr, size_t errlen)
{
    if(!session_push_subsession(script, errstr, errlen))
        return false;

    assert(vec_size(&s_subsession_stack) > 0);
    SDL_RWops *stream = vec_stream_pop(&s_subsession_stack);
    SDL_RWclose(stream);
    return true;
}

static bool session_save(const char *file, char* errstr, size_t errlen)
{
    SDL_RWops *stream = SDL_RWFromFile(file, "w");
    if(!stream) {
        pf_snprintf(errstr, errlen, "Could not open session file: %s", file);
        goto fail_stream;
    }

    struct attr version = (struct attr){
        .type = TYPE_FLOAT,
        .val.as_float = PFSAVE_VERSION
    };
    if(!Attr_Write(stream, &version, "version"))
        return false;

    struct attr num_subsessions = (struct attr){
        .type = TYPE_INT,
        .val.as_int = 1 + vec_size(&s_subsession_stack)
    };
    if(!Attr_Write(stream, &num_subsessions, "num_subsessions"))
        return false;

    for(int i = 0; i < vec_size(&s_subsession_stack); i++) {

        SDL_RWops *sub = vec_AT(&s_subsession_stack, i);
        const char *data = PFSDL_VectorRWOpsRaw(sub);
        size_t size = SDL_RWsize(sub);
        SDL_RWwrite(stream, data, size, 1);
    }

    Sched_TryYield();

    if(!subsession_save(stream))
        goto fail_save;

    SDL_RWclose(stream);
    return true;

fail_save:
    SDL_RWclose(stream);
fail_stream:
    return false;
}

static struct result session_task(void* arg)
{
    ASSERT_IN_MAIN_THREAD();
    bool result = false;

    switch(s_current) {
    case SESH_REQ_SAVE:
        result = session_save(s_req_path, s_errbuff, sizeof(s_errbuff));
        break;
    case SESH_REQ_LOAD:
        result = session_load(s_req_path, s_errbuff, sizeof(s_errbuff));
        break;
    case SESH_REQ_PUSH:
        s_pushing = true;
        result = session_push_subsession(s_req_path, s_errbuff, sizeof(s_errbuff));
        s_pushing = false;
        break;
    case SESH_REQ_POP:
        result = session_pop_subsession(s_errbuff, sizeof(s_errbuff));
        break;
    case SESH_REQ_POP_TO_ROOT:
        result = session_pop_subsession_to_root(s_errbuff, sizeof(s_errbuff));
        break;
    case SESH_REQ_EXEC:
        result = session_exec_subsession(s_req_path, s_errbuff, sizeof(s_errbuff));
        break;
    default: assert(0);
    }

    int success_event = (s_current == SESH_REQ_SAVE) ? EVENT_SESSION_SAVED     
                                                     : EVENT_SESSION_LOADED;
    int failure_event = (s_current == SESH_REQ_SAVE) ? EVENT_SESSION_FAIL_SAVE 
                                                     : EVENT_SESSION_FAIL_LOAD;

    if(result) {
        E_Global_Notify(success_event, NULL, ES_ENGINE);
    }else {
        E_Global_Notify(failure_event, s_errbuff, ES_ENGINE);
    }

    s_argc = 0;
    s_request = SESH_REQ_NONE;
    s_change_tick = g_frame_idx;

    Engine_EnableRendering(false);

    return (struct result) {
        .type = RESULT_BOOL,
        .val.as_bool = result
    };
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void Session_RequestSave(const char *path)
{
    s_request = SESH_REQ_SAVE;
    pf_snprintf(s_req_path, sizeof(s_req_path), "%s", path);
}

void Session_RequestLoad(const char *path)
{
    s_request = SESH_REQ_LOAD;
    pf_snprintf(s_req_path, sizeof(s_req_path), "%s", path);
}

void Session_RequestPush(const char *script, int argc, char **argv)
{
    s_argc = MIN(argc, MAX_ARGC);
    for(int i = 0; i < s_argc; i++) {
        pf_strlcpy(s_argv[i], argv[i], sizeof(s_argv[i]));
    }
    s_request = SESH_REQ_PUSH;
    pf_snprintf(s_req_path, sizeof(s_req_path), "%s", script);
}

void Session_RequestExec(const char *script, int argc, char **argv)
{
    s_argc = MIN(argc, MAX_ARGC);
    for(int i = 0; i < s_argc; i++) {
        pf_strlcpy(s_argv[i], argv[i], sizeof(s_argv[i]));
    }
    s_request = SESH_REQ_EXEC;
    pf_snprintf(s_req_path, sizeof(s_req_path), "%s", script);
}

void Session_RequestPop(int argc, char **argv)
{
    s_argc = MIN(argc, MAX_ARGC);
    for(int i = 0; i < s_argc; i++) {
        pf_strlcpy(s_argv[i], argv[i], sizeof(s_argv[i]));
    }
    s_request = SESH_REQ_POP;
}

void Session_RequestPopToRoot(int argc, char **argv)
{
    s_argc = MIN(argc, MAX_ARGC);
    for(int i = 0; i < s_argc; i++) {
        pf_strlcpy(s_argv[i], argv[i], sizeof(s_argv[i]));
    }
    s_request = SESH_REQ_POP_TO_ROOT;
}

bool Session_ServiceRequests(struct future *result)
{
    if(s_request == SESH_REQ_NONE)
        return false;

    s_current = s_request;
    s_request = SESH_REQ_NONE;

    /* Put all the session saving or loading logic into an async 
     * task, so that the main thread can handle window events and 
     * re-draw things when the session task yields.
     */
    uint32_t tid = Sched_Create(1, session_task, NULL, result, 
        TASK_MAIN_THREAD_PINNED | TASK_BIG_STACK | TASK_RUN_DURING_PAUSE);
    return true;
}

int Session_StackDepth(void)
{
    return vec_size(&s_subsession_stack) + 1 + !!s_pushing;
}

uint64_t Session_ChangeTick(void)
{
    return s_change_tick;
}

bool Session_Init(void)
{
    vec_stream_init(&s_subsession_stack);
    if(!vec_stream_resize(&s_subsession_stack, 64))
        return false;
    return true;
}

void Session_Shutdown(void)
{
    while(vec_size(&s_subsession_stack) > 0) {
        SDL_RWops *stream = vec_stream_pop(&s_subsession_stack);
        SDL_RWclose(stream);
    }
    vec_stream_destroy(&s_subsession_stack);
}

