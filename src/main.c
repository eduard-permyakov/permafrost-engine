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
#include "script/public/script.h"
#include "game/public/game.h"
#include "navigation/public/nav.h"
#include "event.h"
#include "ui.h"
#include "pf_math.h"
#include "settings.h"

#include <stdbool.h>
#include <assert.h>
#include <string.h>

#if defined(_WIN32)
    #include <windows.h>
#endif


#define PF_VER_MAJOR 0
#define PF_VER_MINOR 41
#define PF_VER_PATCH 0

VEC_TYPE(event, SDL_Event)
VEC_IMPL(static inline, event, SDL_Event)

/*****************************************************************************/
/* GLOBAL VARIABLES                                                          */
/*****************************************************************************/

const char                *g_basepath; /* write-once - path of the base directory */

unsigned                   g_last_frame_ms = 0;
unsigned long              g_frame_idx = 0;

SDL_threadID               g_main_thread_id;   /* write-once */
SDL_threadID               g_render_thread_id; /* write-once */

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static SDL_Window         *s_window;

/* Flag to perform a single step of the simulation while the game is paused. 
 * Cleared at after performing the step. 
 */
static bool                s_step_frame = false;
static bool                s_quit = false; 
static vec_event_t         s_prev_tick_events;

static struct nk_context  *s_nk_ctx;

static SDL_Thread         *s_render_thread;
static struct render_sync_state s_rstate;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void process_sdl_events(void)
{
    UI_InputBegin(s_nk_ctx);

    vec_event_reset(&s_prev_tick_events);
    SDL_Event event;    
   
    while(SDL_PollEvent(&event)) {

        UI_HandleEvent(&event);

        vec_event_push(&s_prev_tick_events, event);
        E_Global_Notify(event.type, &vec_AT(&s_prev_tick_events, vec_size(&s_prev_tick_events)-1),
            ES_ENGINE);

        switch(event.type) {

        case SDL_WINDOWEVENT:

            switch(event.window.event) {
            case SDL_WINDOWEVENT_RESIZED:
                glViewport(0, 0, event.window.data1, event.window.data2);
                break;
            }
            break;

        case SDL_KEYDOWN:
            switch(event.key.keysym.scancode) {

            case SDL_SCANCODE_ESCAPE: 
                s_quit = true; 
                break;
            }
            break;

        case SDL_USEREVENT:
            switch(event.user.code) {
            case 0: 
                E_Global_Notify(EVENT_60HZ_TICK, NULL, ES_ENGINE); 
                break;
            }
            break;
        }
    }

    UI_InputEnd(s_nk_ctx);
}

static void on_user_quit(void *user, void *event)
{
    (void)user;
    (void)event;

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

    rstate->done = false;

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
    SDL_LockMutex(s_rstate.sq_lock);
    s_rstate.start = true;
    SDL_CondSignal(s_rstate.sq_cond);
    SDL_UnlockMutex(s_rstate.sq_lock);
}

static void wait_render_work_done(void)
{
    SDL_LockMutex(s_rstate.done_lock);
    while(!s_rstate.done)
        SDL_CondWait(s_rstate.done_cond, s_rstate.done_lock);
    s_rstate.done = false;
    SDL_UnlockMutex(s_rstate.done_lock);
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

/* Fills the framebuffer with the loading screen using SDL's software renderer. 
 * Used to set a loading screen immediately, even before the rendering subsystem 
 * is initialized, */
static void early_loading_screen(void)
{
    assert(s_window);

    SDL_Surface *win_surface = SDL_GetWindowSurface(s_window);
    SDL_Renderer *sw_renderer = SDL_CreateSoftwareRenderer(win_surface);
    if(!sw_renderer) {
        fprintf(stderr, "Loading Screen: Failed to create SDL software renderer: %s\n", SDL_GetError()); 
        return;
    }

    SDL_SetRenderDrawColor(sw_renderer, 0xff, 0xff, 0xff, 0xff);
    SDL_RenderClear(sw_renderer);

    SDL_Rect draw_area;
    SDL_RenderGetViewport(sw_renderer, &draw_area);

    int width, height, orig_format;
    unsigned char *image = stbi_load(CONFIG_LOADING_SCREEN, &width, &height, 
        &orig_format, STBI_rgb);

    if(!image) {
        fprintf(stderr, "Loading Screen: Failed to load image: %s\n", CONFIG_LOADING_SCREEN);
        goto fail_load_image;
    }

    SDL_Surface *img_surface = SDL_CreateRGBSurfaceWithFormatFrom(image, width, height, 
        24, 3*width, SDL_PIXELFORMAT_RGB24);

    if(!img_surface) {
        fprintf(stderr, "Loading Screen: Failed to create SDL surface: %s\n", SDL_GetError());    
        goto fail_surface;
    }

    SDL_Texture *img_tex = SDL_CreateTextureFromSurface(sw_renderer, img_surface);
    if(!img_tex) {
        fprintf(stderr, "Loading Screen: Failed to create SDL texture: %s\n", SDL_GetError());
        goto fail_texture;
    }

    SDL_RenderCopy(sw_renderer, img_tex, NULL, NULL);
    SDL_UpdateWindowSurface(s_window);
    SDL_DestroyTexture(img_tex);

fail_texture:
    SDL_FreeSurface(img_surface);
fail_surface:
    stbi_image_free(image);
fail_load_image:
    SDL_DestroyRenderer(sw_renderer);
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

static bool engine_init(char **argv)
{
    g_main_thread_id = SDL_ThreadID();

    vec_event_init(&s_prev_tick_events);
    if(!vec_event_resize(&s_prev_tick_events, 256))
        return false;

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

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) {
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

    s_window = SDL_CreateWindow(
        "Permafrost Engine",
        SDL_WINDOWPOS_UNDEFINED, 
        SDL_WINDOWPOS_UNDEFINED,
        res[0], 
        res[1], 
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | wf | extra_flags);

    early_loading_screen();
    stbi_set_flip_vertically_on_load(true);

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
    g_render_thread_id = SDL_GetThreadID(s_render_thread);

    render_thread_start_work();
    wait_render_work_done();

    if(!rarg.out_success)
        goto fail_render_init;

    if(!AL_Init()) {
        fprintf(stderr, "Failed to initialize asset-loading module.\n");
        goto fail_al;
    }

    if(!Cursor_InitAll(argv[1])) {
        fprintf(stderr, "Failed to initialize cursor module\n");
        goto fail_cursor;
    }
    Cursor_SetActive(CURSOR_POINTER);

    if(!E_Init()) {
        fprintf(stderr, "Failed to initialize event subsystem\n");
        goto fail_event;
    }

    if(!G_Init()) {
        fprintf(stderr, "Failed to initialize game subsystem\n");
        goto fail_game;
    }

    if(!R_Init(argv[1])) {
        fprintf(stderr, "Failed to intiaialize rendering subsystem\n");
        goto fail_render;
    }

    Cursor_SetRTSMode(true);
    E_Global_Register(SDL_QUIT, on_user_quit, NULL, 
        G_RUNNING | G_PAUSED_UI_RUNNING | G_PAUSED_FULL);

    if( !(s_nk_ctx = UI_Init(argv[1], s_window)) ) {
        fprintf(stderr, "Failed to initialize nuklear\n");
        goto fail_nuklear;
    }

    if(!S_Init(argv[0], argv[1], s_nk_ctx)) {
        fprintf(stderr, "Failed to initialize scripting subsystem\n");
        goto fail_script;
    }

    if(!N_Init()) {
        fprintf(stderr, "Failed to intialize navigation subsystem\n");
        goto fail_nav;
    }

    engine_create_settings();
    s_rstate.swap_buffers = true;
    return true;

fail_nav:
    S_Shutdown();
fail_script:
    UI_Shutdown();
fail_nuklear:
    G_Shutdown();
fail_game:
    E_Shutdown();
fail_event:
fail_render:
    Cursor_FreeAll();
fail_cursor:
    AL_Shutdown();
fail_al:
fail_render_init:
    render_thread_quit();
fail_rthread:
    rstate_destroy(&s_rstate);
fail_rstate:
    SDL_DestroyWindow(s_window);
    SDL_Quit();
fail_sdl:
    Settings_Shutdown();
fail_settings:
    return false; 
}

static void engine_shutdown(void)
{
    S_Shutdown();
    UI_Shutdown();

    /* Execute the last batch of commands that may have been queued by the 
     * shutdown routines. 
     */
    render_thread_start_work();
    wait_render_work_done();
    render_thread_quit();

    /* 'Game' must shut down after 'Scripting'. There are still 
     * references to game entities in the Python interpreter that should get
     * their destructors called during 'S_Shutdown(), which will invoke the 
     * 'G_' API to remove them from the world.
     */
    G_Shutdown(); 
    N_Shutdown();

    Cursor_FreeAll();
    AL_Shutdown();
    E_Shutdown();

    vec_event_destroy(&s_prev_tick_events);
    rstate_destroy(&s_rstate);

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
    assert(g_frame_idx == 0);
    G_SwapBuffers();

    render_thread_start_work();
    wait_render_work_done();

    G_SwapBuffers();
}

#if defined(_WIN32)
int CALLBACK WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, 
                     LPSTR lpCmdLine, int nCmdShow)
#else
int main(int argc, char **argv)
#endif
{
#if defined(_WIN32)
    LPWSTR *argv_wide;
    char args[16][256];
    char *argv[16];
    int argc;
    argv_wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    if(!argv)
        goto fail_args;

    for(int i = 0; i < argc; i++) {
        argv[i] = args[i];
        snprintf(argv[i], sizeof(args[i]), "%S", argv_wide[i]);
    }
    LocalFree(argv_wide);
#endif

    int ret = EXIT_SUCCESS;

    if(argc != 3) {
        printf("Usage: %s [base directory path (containing 'assets', 'shaders' and 'scripts' folders)] [script path]\n", argv[0]);
        ret = EXIT_FAILURE;
        goto fail_args;
    }

    g_basepath = argv[1];

    if(!engine_init(argv)) {
        ret = EXIT_FAILURE; 
        goto fail_init;
    }

    S_RunFile(argv[2]);

    /* Run the first frame of the simulation, and prepare the buffers for rendering. */
    G_Update();
    G_Render();
    UI_Render(s_nk_ctx);
    G_SwapBuffers();

    uint32_t last_ts = SDL_GetTicks();
    while(!s_quit) {

        enum simstate curr_ss = G_GetSimState();
        bool prev_step_frame = s_step_frame;

        if(prev_step_frame) {
            assert(curr_ss != G_RUNNING); 
            G_SetSimState(G_RUNNING);
        }

        render_thread_start_work();

        process_sdl_events();
        E_ServiceQueue();
        G_Update();
        G_Render();
        UI_Render(s_nk_ctx);

        wait_render_work_done();

        G_SwapBuffers();

        if(prev_step_frame) {
            G_SetSimState(curr_ss);
            s_step_frame = false;
        }

        uint32_t curr_time = SDL_GetTicks();
        g_last_frame_ms = curr_time - last_ts;
        last_ts = curr_time;

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

