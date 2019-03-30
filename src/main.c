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
#include "lib/public/stb_image.h"
#include "lib/public/kvec.h"
#include "script/public/script.h"
#include "game/public/game.h"
#include "navigation/public/nav.h"
#include "event.h"
#include "ui.h"
#include "pf_math.h"
#include "settings.h"

#include <GL/glew.h>
#include <SDL_opengl.h>

#include <stdbool.h>
#include <assert.h>
#include <string.h>

#if defined(_WIN32)
    #include <windows.h>
#endif


#define PF_VER_MAJOR 0
#define PF_VER_MINOR 31
#define PF_VER_PATCH 0

/*****************************************************************************/
/* GLOBAL VARIABLES                                                          */
/*****************************************************************************/

/* Write-once global - path of the base directory */
const char                *g_basepath;

unsigned                   g_last_frame_ms = 0;

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static SDL_Window         *s_window;
static SDL_GLContext       s_context;

static bool                s_quit = false; 
static kvec_t(SDL_Event)   s_prev_tick_events;

static struct nk_context  *s_nk_ctx;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void process_sdl_events(void)
{
    UI_InputBegin(s_nk_ctx);

    kv_reset(s_prev_tick_events);
    SDL_Event event;    
   
    while(SDL_PollEvent(&event)) {

        UI_HandleEvent(&event);

        kv_push(SDL_Event, s_prev_tick_events, event);
        E_Global_Notify(event.type, &kv_A(s_prev_tick_events, kv_size(s_prev_tick_events)-1), 
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

            case SDL_SCANCODE_ESCAPE: s_quit = true; break;
            }
            break;

        case SDL_USEREVENT:
            switch(event.user.code) {
            case 0: E_Global_Notify(EVENT_60HZ_TICK, NULL, ES_ENGINE); break;
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

static void gl_set_globals(void)
{
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
}

static void render(void)
{
    SDL_GL_MakeCurrent(s_window, s_context); 

    glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    /* Restore OpenGL global state after it's been clobbered by nuklear */
    gl_set_globals(); 

    G_Render();
    UI_Render();

    SDL_GL_SwapWindow(s_window);
}

/* Fills the framebuffer with the loading screen using SDL's software renderer. 
 * Used to set a loading screen immediately, even before the rendering subsystem 
 * is initialized, The loading screen will be overwriten by the first 'render' call. */
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
    unsigned char *image = stbi_load(CONFIG_LOADING_SCREEN, &width, &height, &orig_format, STBI_rgb);
    if(!image) {
        fprintf(stderr, "Loading Screen: Failed to load image: %s\n", CONFIG_LOADING_SCREEN);
        goto fail_load_image;
    }

    SDL_Surface *img_surface = SDL_CreateRGBSurfaceWithFormatFrom(image, width, height, 24, 3*width, SDL_PIXELFORMAT_RGB24);
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

static bool engine_init(char **argv)
{
    kv_init(s_prev_tick_events);
    if(!kv_resize(SDL_Event, s_prev_tick_events, 256))
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

    enum pf_window_flags wf = PF_WF_BORDERLESS_WIN;
    if(Settings_Get("pf.video.display_mode", &setting) == SS_OKAY) {
        wf = setting.as_int;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);

    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    s_window = SDL_CreateWindow(
        "Permafrost Engine",
        SDL_WINDOWPOS_UNDEFINED, 
        SDL_WINDOWPOS_UNDEFINED,
        res[0], 
        res[1], 
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN 
#ifdef __linux__
            | SDL_WINDOW_ALWAYS_ON_TOP 
#endif
            | wf);

    early_loading_screen();

    s_context = SDL_GL_CreateContext(s_window); 
    SDL_GL_SetSwapInterval(CONFIG_VSYNC ? 1 : 0); 

    glewExperimental = GL_TRUE;
    if(glewInit() != GLEW_OK) {
        fprintf(stderr, "Failed to initialize GLEW\n");
        goto fail_glew;
    }

    if(!GLEW_VERSION_3_3) {
        fprintf(stderr, "Required OpenGL version not supported in GLEW\n");
        goto fail_glew;
    }

    glViewport(0, 0, res[0], res[1]);
    glProvokingVertex(GL_FIRST_VERTEX_CONVENTION); 
    glFrontFace(GL_CW);
    glCullFace(GL_BACK);

    stbi_set_flip_vertically_on_load(true);

    if(!AL_Init()) {
        fprintf(stderr, "Failed to initialize asset-loading module.\n");
        goto fail_al;
    }

    if(!Cursor_InitAll(argv[1])) {
        fprintf(stderr, "Failed to initialize cursor module\n");
        goto fail_cursor;
    }
    Cursor_SetActive(CURSOR_POINTER);

    if(!R_Init(argv[1])) {
        fprintf(stderr, "Failed to iniaialize rendering subsystem\n");
        goto fail_render;
    }

    if(!E_Init()) {
        fprintf(stderr, "Failed to initialize event subsystem\n");
        goto fail_event;
    }
    Cursor_SetRTSMode(true);
    E_Global_Register(SDL_QUIT, on_user_quit, NULL);

    if( !(s_nk_ctx = UI_Init(argv[1], s_window)) ) {
        fprintf(stderr, "Failed to initialize nuklear\n");
        goto fail_nuklear;
    }

    if(!S_Init(argv[0], argv[1], s_nk_ctx)) {
        fprintf(stderr, "Failed to initialize scripting subsystem\n");
        goto fail_script;
    }

    /* depends on Event subsystem */
    if(!G_Init()) {
        fprintf(stderr, "Failed to initialize game subsystem\n");
        goto fail_game;
    }

    if(!N_Init()) {
        fprintf(stderr, "Failed to intialize navigation subsystem\n");
        goto fail_nav;
    }

    return true;

fail_nav:
    G_Shutdown();
fail_game:
fail_script:
    UI_Shutdown();
fail_nuklear:
fail_event:
fail_render:
    Cursor_FreeAll();
fail_cursor:
fail_al:
fail_settings:
fail_glew:
    SDL_GL_DeleteContext(s_context);
    SDL_DestroyWindow(s_window);
    SDL_Quit();
fail_sdl:
    return false; 
}

static void engine_shutdown(void)
{
    N_Shutdown();
    S_Shutdown();

    /* 'Game' must shut down after 'Scripting'. There are still 
     * references to game entities in the Python interpreter that should get
     * their destructors called during 'S_Shutdown(), which will invoke the 
     * 'G_' API to remove them from the world.
     */
    G_Shutdown(); 
    Cursor_FreeAll();
    AL_Shutdown();
    UI_Shutdown();
    E_Shutdown();

    kv_destroy(s_prev_tick_events);

    SDL_GL_DeleteContext(s_context);
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
        printf("Usage: %s [base directory path (which contains 'assets' and 'shaders' folders)] [script path]\n", argv[0]);
        ret = EXIT_FAILURE;
        goto fail_args;
    }

    g_basepath = argv[1];

    if(!engine_init(argv)) {
        ret = EXIT_FAILURE; 
        goto fail_init;
    }

    S_RunFile(argv[2]);

    uint32_t last_ts = SDL_GetTicks();
    while(!s_quit) {

        process_sdl_events();
        E_ServiceQueue();
        G_Update();
        render();

        uint32_t curr_time = SDL_GetTicks();
        g_last_frame_ms = curr_time - last_ts;
        last_ts = curr_time;
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

