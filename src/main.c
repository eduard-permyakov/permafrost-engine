/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017-2023 Eduard Permyakov 
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

#include "main.h"
#include "asset_load.h"
#include "config.h"
#include "cursor.h"
#include "render/public/render.h"
#include "render/public/render_ctrl.h"
#include "lib/public/stb_image.h"
#include "lib/public/vec.h"
#include "lib/public/pf_string.h"
#include "lib/public/noise.h"
#include "script/public/script.h"
#include "game/public/game.h"
#include "navigation/public/nav.h"
#include "audio/public/audio.h"
#include "phys/public/phys.h"
#include "anim/public/anim.h"
#include "event.h"
#include "ui.h"
#include "pf_math.h"
#include "settings.h"
#include "session.h"
#include "perf.h"
#include "sched.h"
#include "loading_screen.h"

#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

#if defined(_WIN32)
#include "lib/public/windows.h"
#endif

#define EVENT_VEC_SIZE (32768)


/* In the WAITING state the engine only pumps events and re-draws the window,
 * giving all the remaining cycles to the scheduler. The purpose of this state 
 * is to allow the engine to remain responsive (i.e. the latency of handling 
 * window events or redrawing the window is bounded) while performing some 
 * long-running work within a task context.
 */
enum engine_state{
    ENGINE_STATE_RUNNING,
    ENGINE_STATE_WAITING,
    /* A transient state for making sure there is no more work to be done 
     * by the rendering thread. */
    ENGINE_STATE_RESUMING,
};

VEC_TYPE(event, SDL_Event)
VEC_IMPL(static inline, event, SDL_Event)

/*****************************************************************************/
/* GLOBAL VARIABLES                                                          */
/*****************************************************************************/

const char                      *g_basepath; /* write-once - path of the base directory */
unsigned long                    g_frame_idx = 0;

SDL_threadID                     g_main_thread_id;   /* write-once */
SDL_threadID                     g_render_thread_id; /* write-once */

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static SDL_Window               *s_window;

static enum engine_state         s_state = ENGINE_STATE_RUNNING;
static struct future             s_request_done;

/* Flag to perform a single step of the simulation while the game is paused. 
 * Cleared at after performing the step. 
 */
static bool                      s_step_frame = false;
static bool                      s_quit = false; 
static vec_event_t               s_prev_tick_events;

static SDL_Thread               *s_render_thread;
static struct render_sync_state  s_rstate;

static int                       s_argc;
static char                    **s_argv;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void process_sdl_events(void)
{
    PERF_ENTER();
    UI_InputBegin();
    vec_event_reset(&s_prev_tick_events);

    SDL_Event event;
    while(SDL_PollEvent(&event)) {

        if(vec_size(&s_prev_tick_events) == EVENT_VEC_SIZE)
            break;

        UI_HandleEvent(&event);
        vec_event_push(&s_prev_tick_events, event);

        switch(event.type) {

        case SDL_KEYDOWN:
            if(event.key.keysym.sym == SDLK_q && (event.key.keysym.mod & KMOD_LALT)) {
                s_quit = true; 
            }
            break;

        case SDL_USEREVENT:
            if(event.user.code == 0) {
                E_Global_Notify(EVENT_60HZ_TICK, NULL, ES_ENGINE); 
            }
            break;
        default: 
            break;
        }
    }

    if(s_state != ENGINE_STATE_WAITING) {
        for(int i = 0; i < vec_size(&s_prev_tick_events); i++) {
            const SDL_Event *event = &vec_AT(&s_prev_tick_events, i);
            E_Global_Notify(event->type, (void*)event, ES_ENGINE);
        }
    }

    UI_InputEnd();
    PERF_RETURN_VOID();
}

static void clear_sdl_events(void)
{
    SDL_PumpEvents();
    /* Always check for 'quit' events */
    int nevents;
    SDL_Event event;
    do{
        nevents = SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_KEYDOWN, SDL_KEYDOWN);
        if(nevents) {
            if(event.key.keysym.sym == SDLK_q && (event.key.keysym.mod & KMOD_LALT)) {
                s_quit = true; 
            }
        }
    }while(nevents > 0);
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
}

static void on_user_quit(void *user, void *event)
{
    s_quit = true;
}

static bool rstate_init(struct render_sync_state *rstate)
{
    rstate->start = false;

    rstate->sq_lock = SDL_CreateMutex();
    if(!rstate->sq_lock)
        goto fail_sq_lock;
        
    rstate->sq_cond = SDL_CreateCond();
    if(!rstate->sq_cond)
        goto fail_sq_cond;

    rstate->status = RSTAT_NONE;

    rstate->done_lock = SDL_CreateMutex();
    if(!rstate->done_lock)
        goto fail_done_lock;

    rstate->done_cond = SDL_CreateCond();
    if(!rstate->done_cond)
        goto fail_done_cond;

    rstate->swap_buffers = false;
    return true;

fail_done_cond:
    SDL_DestroyMutex(rstate->done_lock);
fail_done_lock:
    SDL_DestroyCond(rstate->sq_cond);
fail_sq_cond:
    SDL_DestroyMutex(rstate->sq_lock);
fail_sq_lock:
    return false;
}

static void rstate_destroy(struct render_sync_state *rstate)
{
    SDL_DestroyCond(rstate->done_cond);
    SDL_DestroyMutex(rstate->done_lock);
    SDL_DestroyCond(rstate->sq_cond);
    SDL_DestroyMutex(rstate->sq_lock);
}

static int render_thread_quit(void)
{
    SDL_LockMutex(s_rstate.sq_lock);
    s_rstate.quit = true;
    SDL_CondSignal(s_rstate.sq_cond);
    SDL_UnlockMutex(s_rstate.sq_lock);

    int ret;
    SDL_WaitThread(s_render_thread, &ret);
    return ret;
}

static void render_thread_start_work(void)
{
    SDL_LockMutex(s_rstate.done_lock);
    s_rstate.status = RSTAT_NONE;
    SDL_UnlockMutex(s_rstate.done_lock);

    SDL_LockMutex(s_rstate.sq_lock);
    s_rstate.start = true;
    SDL_CondSignal(s_rstate.sq_cond);
    SDL_UnlockMutex(s_rstate.sq_lock);
}

enum render_status render_thread_wait_done(void)
{
    PERF_ENTER();
    enum render_status ret;

    SDL_LockMutex(s_rstate.done_lock);
    while(s_rstate.status == RSTAT_NONE) {
        SDL_CondWait(s_rstate.done_cond, s_rstate.done_lock);
    }
    ret = s_rstate.status;
    s_rstate.status = RSTAT_NONE;
    SDL_UnlockMutex(s_rstate.done_lock);

    PERF_RETURN(ret);
}

enum render_status render_thread_poll(void)
{
    PERF_ENTER();
    enum render_status ret;

    SDL_LockMutex(s_rstate.done_lock);
    while(s_rstate.status == RSTAT_NONE) {
        SDL_CondWait(s_rstate.done_cond, s_rstate.done_lock);
    }
    ret = s_rstate.status;
    SDL_UnlockMutex(s_rstate.done_lock);

    PERF_RETURN(ret);
}

static void render_maybe_enable(void)
{
    /* Simulate a single frame after a session change without rendering 
     * it - this gives us a chance to handle this event without anyone 
     * noticing. */
    if(((uint64_t)g_frame_idx) - Session_ChangeTick() <= 1)
        return;
    s_rstate.swap_buffers = true;
}

static void fs_on_key_press(void *user, void *event)
{
    SDL_KeyboardEvent *key = &((SDL_Event*)event)->key;
    if(key->keysym.scancode != CONFIG_FRAME_STEP_HOTKEY)
        return;
    s_step_frame = true;
}

static bool frame_step_validate(const struct sval *new_val)
{
    return (new_val->type == ST_TYPE_BOOL);
}

static void frame_step_commit(const struct sval *new_val)
{
    if(new_val->as_bool) {
        E_Global_Register(SDL_KEYDOWN, fs_on_key_press, NULL, 
            G_PAUSED_FULL | G_PAUSED_UI_RUNNING);
    }else{
        E_Global_Unregister(SDL_KEYDOWN, fs_on_key_press); 
    }
}

static void engine_create_settings(void)
{
    ss_e status = Settings_Create((struct setting){
        .name = "pf.debug.paused_frame_step_enabled",
        .val = (struct sval) {
            .type = ST_TYPE_BOOL,
            .as_bool = false 
        },
        .prio = 0,
        .validate = frame_step_validate,
        .commit = frame_step_commit,
    });
    assert(status == SS_OKAY);
}

static void engine_set_icon(void)
{
    char iconpath[512];
    if(!Engine_GetArg("appicon", sizeof(iconpath), iconpath))
        return;

    char fullpath[512];
    pf_snprintf(fullpath, sizeof(fullpath), "%s/%s", g_basepath, iconpath);

    int width, height, orig_format;
    unsigned char *image = stbi_load(fullpath, &width, &height, 
        &orig_format, STBI_rgb_alpha);

    if(!image) {
        fprintf(stderr, "Failed to load client icon image: %s\n", fullpath);
        return;
    }

    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
    if(!surface) {
        fprintf(stderr, "Failed to create surface from client icon image: %s\n", fullpath);
        goto fail_surface;
    }

    memcpy(surface->pixels, image, width * height * 4);
    SDL_SetWindowIcon(s_window, surface);
    SDL_FreeSurface(surface);
fail_surface:
    free(image);
}

static bool engine_init(void)
{
    g_main_thread_id = SDL_ThreadID();
    Noise_Init();

    if(!Perf_Init()) {
        fprintf(stderr, "Failed to initialize performance module.\n");
        return false;;
    }

    vec_event_init(&s_prev_tick_events);
    if(!vec_event_resize(&s_prev_tick_events, EVENT_VEC_SIZE))
        goto fail_resize;

    /* Initialize 'Settings' before any subsystem to allow all of them 
     * to register settings. */
    if(Settings_Init() != SS_OKAY) {
        fprintf(stderr, "Failed to initialize settings module.\n");
        goto fail_settings;
    }

    ss_e status;
    if((status = Settings_LoadFromFile()) != SS_OKAY) {
        fprintf(stderr, "Could not load settings from file: %s [status: %d]\n", 
            Settings_GetFile(), status);
    }

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) {
        fprintf(stderr, "Failed to initialize SDL: %s\n", SDL_GetError());
        goto fail_sdl;
    }

    SDL_DisplayMode dm;
    SDL_GetDesktopDisplayMode(0, &dm);

    struct sval setting;
    int res[2] = {dm.w, dm.h};

    if(Settings_Get("pf.video.resolution", &setting) == SS_OKAY) {
        res[0] = (int)setting.as_vec2.x;
        res[1] = (int)setting.as_vec2.y;
    }

    enum pf_window_flags wf = PF_WF_BORDERLESS_WIN, extra_flags = 0;
    if(Settings_Get("pf.video.display_mode", &setting) == SS_OKAY) {
        wf = setting.as_int;
    }
    if(Settings_Get("pf.video.window_always_on_top", &setting) == SS_OKAY) {
        extra_flags = setting.as_bool ? SDL_WINDOW_ALWAYS_ON_TOP : 0;
    }

    R_InitAttributes();

    char appname[64] = "Permafrost Engine";
    Engine_GetArg("appname", sizeof(appname), appname);
    s_window = SDL_CreateWindow(
        appname,
        SDL_WINDOWPOS_UNDEFINED, 
        SDL_WINDOWPOS_UNDEFINED,
        res[0], 
        res[1], 
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | wf | extra_flags);

    LoadingScreen_Init();
    engine_set_icon();
    stbi_set_flip_vertically_on_load(true);

    LoadingScreen_DrawEarly(s_window);

    if(!rstate_init(&s_rstate)) {
        fprintf(stderr, "Failed to initialize the render sync state.\n");
        goto fail_rstate;
    }

    struct render_init_arg rarg = (struct render_init_arg) {
        .in_window = s_window,
        .in_width = res[0],
        .in_height = res[1],
    };

    s_rstate.arg = &rarg;
    s_render_thread = R_Run(&s_rstate);

    if(!s_render_thread) {
        fprintf(stderr, "Failed to start the render thread.\n");
        goto fail_rthread;
    }

    render_thread_start_work();
    render_thread_wait_done();
    render_maybe_enable();

    if(!rarg.out_success)
        goto fail_render_init;

    Perf_RegisterThread(g_main_thread_id, "main");
    Perf_RegisterThread(g_render_thread_id, "render");

    if(!Sched_Init()) {
        fprintf(stderr, "Failed to initialize scheduling module.\n");
        goto fail_sched;
    }

    if(!Session_Init()) {
        fprintf(stderr, "Failed to initialize session module.\n");
        goto fail_sesh;
    }

    if(!AL_Init()) {
        fprintf(stderr, "Failed to initialize asset-loading module.\n");
        goto fail_al;
    }

    if(!Cursor_InitDefault(g_basepath)) {
        fprintf(stderr, "Failed to initialize cursor module\n");
        goto fail_cursor;
    }
    Cursor_SetActive(CURSOR_POINTER);

    if(!E_Init()) {
        fprintf(stderr, "Failed to initialize event subsystem\n");
        goto fail_event;
    }

    if(!Entity_Init()) {
        fprintf(stderr, "Failed to initialize event subsystem\n");
        goto fail_entity;
    }

    if(!A_Init()) {
        fprintf(stderr, "Failed to initialize animation subsystem\n");
        goto fail_anim;
    }

    if(!G_Init()) {
        fprintf(stderr, "Failed to initialize game subsystem\n");
        goto fail_game;
    }

    if(!R_Init(g_basepath)) {
        fprintf(stderr, "Failed to intiaialize rendering subsystem\n");
        goto fail_render;
    }

    E_Global_Register(SDL_QUIT, on_user_quit, NULL, 
        G_RUNNING | G_PAUSED_UI_RUNNING | G_PAUSED_FULL);

    if(!UI_Init(g_basepath, s_window)) {
        fprintf(stderr, "Failed to initialize nuklear\n");
        goto fail_nuklear;
    }

    if(!S_Init(s_argv[0], g_basepath, UI_GetContext())) {
        fprintf(stderr, "Failed to initialize scripting subsystem\n");
        goto fail_script;
    }

    if(!N_Init()) {
        fprintf(stderr, "Failed to intialize navigation subsystem\n");
        goto fail_nav;
    }

    if(!Audio_Init()) {
        fprintf(stderr, "Failed to intialize audio subsystem\n");
        goto fail_audio;
    }

    if(!P_Projectile_Init()) {
        fprintf(stderr, "Failed to intialize physics subsystem\n");
        goto fail_phys;
    }

    engine_create_settings();
    return true;

fail_phys:
    Audio_Shutdown();
fail_audio:
    N_Shutdown();
fail_nav:
    S_Shutdown();
fail_script:
    UI_Shutdown();
fail_nuklear:
    G_Shutdown();
fail_game:
    A_Shutdown();
fail_anim:
    Entity_Shutdown();
fail_entity:
    E_Shutdown();
fail_event:
fail_render:
    Cursor_FreeAll();
fail_cursor:
    AL_Shutdown();
fail_al:
    Session_Shutdown();
fail_sesh:
    Sched_Shutdown();
fail_sched:
fail_render_init:
    render_thread_quit();
fail_rthread:
    rstate_destroy(&s_rstate);
fail_rstate:
    LoadingScreen_Shutdown();
    SDL_DestroyWindow(s_window);
    SDL_Quit();
fail_sdl:
    Settings_Shutdown();
fail_settings:
    Perf_Shutdown();
fail_resize:
    vec_event_destroy(&s_prev_tick_events);
    return false; 
}

static void engine_shutdown(void)
{
    P_Projectile_Shutdown();
    Audio_Shutdown();
    S_Shutdown();
    UI_Shutdown();

    /* Execute the last batch of commands that may have been queued by the 
     * shutdown routines. 
     */
    render_thread_start_work();
    render_thread_wait_done();
    render_thread_quit();

    /* 'Game' must shut down after 'Scripting'. There are still 
     * references to game entities in the Python interpreter that should get
     * their destructors called during 'S_Shutdown(), which will invoke the 
     * 'G_' API to remove them from the world.
     */
    G_Shutdown(); 
    A_Shutdown();
    Entity_Shutdown();
    N_Shutdown();

    Cursor_FreeAll();
    AL_Shutdown();
    E_Shutdown();
    Session_Shutdown();
    Sched_Shutdown();
    Perf_Shutdown();

    vec_event_destroy(&s_prev_tick_events);
    rstate_destroy(&s_rstate);

    LoadingScreen_Shutdown();
    SDL_DestroyWindow(s_window); 
    SDL_Quit();

    Settings_Shutdown();
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

int Engine_SetRes(int w, int h)
{
    SDL_DisplayMode dm = (SDL_DisplayMode) {
        .format = SDL_PIXELFORMAT_UNKNOWN,
        .w = w,
        .h = h,
        .refresh_rate = 0, /* Unspecified */
        .driverdata = NULL,
    };

    SDL_SetWindowSize(s_window, w, h);
    SDL_SetWindowPosition(s_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    return SDL_SetWindowDisplayMode(s_window, &dm);
}

void Engine_SetDispMode(enum pf_window_flags wf)
{
    SDL_SetWindowFullscreen(s_window, wf & SDL_WINDOW_FULLSCREEN);
    SDL_SetWindowBordered(s_window, !(wf & (SDL_WINDOW_BORDERLESS | SDL_WINDOW_FULLSCREEN)));
    SDL_SetWindowPosition(s_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

void Engine_WinDrawableSize(int *out_w, int *out_h)
{
    SDL_GL_GetDrawableSize(s_window, out_w, out_h);
}

void Engine_FlushRenderWorkQueue(void)
{
    ASSERT_IN_MAIN_THREAD();
    enum render_status status = RSTAT_NONE;

    /* Flush both queues. */
    if(Sched_ActiveTID() == NULL_TID) {

        render_thread_start_work();
        render_thread_wait_done();
        G_SwapBuffers();

        render_thread_start_work();
        render_thread_wait_done();
        G_SwapBuffers();
        return;
    }

    /* When called from task context, assume 
     * that the render thread is started. 
     */
    do{
        Sched_TryYield();
        status = render_thread_poll();
    }while(status != RSTAT_DONE);
}

void Engine_SetRenderThreadID(SDL_threadID id)
{
    g_render_thread_id = id;
}

void Engine_WaitRenderWorkDone(void)
{
    PERF_ENTER();
    if(s_quit) {
        PERF_RETURN_VOID();
    }

    /* Wait for the render thread to finish, but don't yet clear/ack the 'status' flag */
    SDL_LockMutex(s_rstate.done_lock);
    while(s_rstate.status == RSTAT_NONE) {
        SDL_CondWait(s_rstate.done_cond, s_rstate.done_lock);
    }
    SDL_UnlockMutex(s_rstate.done_lock);

    PERF_RETURN_VOID();
}

void Engine_SwapWindow(void)
{
    ASSERT_IN_RENDER_THREAD();
    SDL_GL_SwapWindow(s_window);
}

void Engine_ClearPendingEvents(void)
{
    clear_sdl_events();
    E_ClearPendingEvents();
}

bool Engine_GetArg(const char *name, size_t maxout, char out[])
{
    size_t namelen = strlen(name);
    for(int i = 2; i < s_argc; i++) {
        const char *curr = s_argv[i];
        if(strstr(curr, "--") != curr)
            continue;
        curr += 2;
        if(0 != strncmp(curr, name, namelen))
            continue;
        curr += namelen;
        if(*curr != '=')
            continue;
        pf_strlcpy(out, curr + 1, maxout);
        return true;
    }
    return false;
}

#if defined(_WIN32)
int CALLBACK WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, 
                     LPSTR lpCmdLine, int nCmdShow)
{
    int argc = __argc;
    char **argv = __argv;

#else
int main(int argc, char **argv)
{
#endif

    int ret = EXIT_SUCCESS;

    if(argc < 3) {
        printf("Usage: %s [base directory path (containing 'assets', 'shaders' and 'scripts' folders)] [script path]\n", argv[0]);
        ret = EXIT_FAILURE;
        goto fail_args;
    }

    g_basepath = argv[1];
    s_argc = argc;
    s_argv = argv;

    if(!engine_init()) {
        ret = EXIT_FAILURE; 
        goto fail_init;
    }

    Audio_PlayMusicFirst();
    S_RunFileAsync(argv[2], 0, NULL, &s_request_done);
    s_state = ENGINE_STATE_WAITING;
    enum render_status render_status = RSTAT_NONE;

    /* Run the first frame of the simulation, and prepare the buffers for rendering. */
    E_ServiceQueue();
    G_Update();
    G_Render();
    G_SwapBuffers();
    Perf_FinishTick();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);

    while(!s_quit) {

        Perf_BeginTick();
        enum simstate curr_ss = G_GetSimState();
        bool prev_step_frame = s_step_frame;

        if(prev_step_frame) {
            assert(curr_ss != G_RUNNING); 
            G_SetSimState(G_RUNNING);
        }

        if(s_state != ENGINE_STATE_RUNNING) {
            LoadingScreen_Tick();
        }

        render_maybe_enable();
        render_thread_start_work();
        Sched_StartBackgroundTasks();

        bool request = Session_ServiceRequests(&s_request_done);
        if(request) {
            s_state = ENGINE_STATE_WAITING;
        }

        switch(s_state) {
        case ENGINE_STATE_RUNNING:

            process_sdl_events();
            E_ServiceQueue();

            G_Update();
            G_Render();
            Sched_Tick();

            render_status = render_thread_wait_done();
            G_SwapBuffers();

            break;

        case ENGINE_STATE_WAITING:

            clear_sdl_events();
            Sched_Tick();
            render_status = render_thread_wait_done();
            if(Sched_FutureIsReady(&s_request_done)) {
                /* Kick off the rendering work */
                G_SwapBuffers();
                s_state = ENGINE_STATE_RESUMING;
            }
            break;

        case ENGINE_STATE_RESUMING:

            clear_sdl_events();
            Sched_Tick();
            render_status = render_thread_wait_done();
            if(render_status == RSTAT_DONE) {
                s_state = ENGINE_STATE_RUNNING;
            }
            break;

        default: assert(0); break;
        }

        Perf_FinishTick();

        if(prev_step_frame) {
            G_SetSimState(curr_ss);
            s_step_frame = false;
        }
        ++g_frame_idx;
    }

    ss_e status;
    if((status = Settings_SaveToFile()) != SS_OKAY) {
        fprintf(stderr, "Could not save settings to file: %s [status: %d]\n", 
            Settings_GetFile(), status);
    }

    engine_shutdown();
fail_init:
fail_args:
    exit(ret);
}

