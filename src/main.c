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
 */

#include "asset_load.h"
#include "entity.h"
#include "config.h"
#include "cursor.h"
#include "render/public/render.h"
#include "anim/public/anim.h"
#include "lib/public/stb_image.h"
#include "map/public/map.h"
#include "script/public/script.h"
#include "game/public/game.h"

#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

#include <stdbool.h>
#include <assert.h>


#define PF_VER_MAJOR 0
#define PF_VER_MINOR 3
#define PF_VER_PATCH 0

/*****************************************************************************/
/* GLOBAL VARIABLES                                                          */
/*****************************************************************************/

/* Write-once global - path of the base directory */
const char *g_basepath;

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static SDL_Window         *s_window;
static SDL_GLContext       s_context;

static bool                s_quit = false; 

struct entity             *s_demo_entity;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void process_events(void)
{
    SDL_Event event;    
   
    while(SDL_PollEvent(&event)) {

        G_HandleEvent(&event);

        switch(event.type) {

        case SDL_QUIT: 
            s_quit = true;
            break;

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

        case SDL_MOUSEMOTION:  {

            int mouse_x, mouse_y;
            SDL_GetMouseState(&mouse_x, &mouse_y);

            Cursor_RTS_SetActive(mouse_x, mouse_y); 
            break;
            }
        }
    }
}

static void render(void)
{
    SDL_GL_MakeCurrent(s_window, s_context); 

    glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    G_Render();

    SDL_GL_SwapWindow(s_window);
}

static bool engine_init(char **argv)
{
    bool result = true;

    /* ---------------------------------- */
    /* SDL Initialization                 */
    /* ---------------------------------- */
    if(SDL_Init(SDL_INIT_VIDEO) < 0) {
        result = false;
        goto fail_sdl;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    s_window = SDL_CreateWindow(
        "Permafrost Engine",
        SDL_WINDOWPOS_UNDEFINED, 
        SDL_WINDOWPOS_UNDEFINED,
        CONFIG_RES_X, 
        CONFIG_RES_Y, 
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_FULLSCREEN);

    s_context = SDL_GL_CreateContext(s_window); 

    SDL_GL_SetSwapInterval(0); 
    glViewport(0, 0, CONFIG_RES_X, CONFIG_RES_Y);
    glEnable(GL_DEPTH_TEST);

    /* ---------------------------------- */
    /* GLEW initialization                */
    /* ---------------------------------- */
    glewExperimental = GL_TRUE;
    if(glewInit() != GLEW_OK) {
        result = false;
        goto fail_glew;
    }

    /* ---------------------------------- */
    /* stb_image initialization           */
    /* ---------------------------------- */
    stbi_set_flip_vertically_on_load(true);

    /* ---------------------------------- */
    /* Cursor initialization              */
    /* ---------------------------------- */
    if(!Cursor_InitAll(argv[1])) {
        result = false;
        goto fail_cursor;
    }
    Cursor_SetActive(CURSOR_POINTER);

    /* ---------------------------------- */
    /* Rendering subsystem initialization */
    /* ---------------------------------- */
    if(!R_Init(argv[1])) {
        result = false;
        goto fail_render;
    }

    /* ---------------------------------- */
    /* Scripting subsystem initialization */
    /* ---------------------------------- */
    if(!S_Init(argv[0], argv[1])){
        result = false; 
        goto fail_script;
    }

    /* ---------------------------------- */
    /* Game state initialization          */
    /* ---------------------------------- */
    if(!G_Init()) {
        result = false; 
        goto fail_game;
    }

    return result;

fail_game:
fail_script:
fail_render:
    Cursor_FreeAll();
fail_cursor:
fail_glew:
    SDL_GL_DeleteContext(s_context);
    SDL_DestroyWindow(s_window);
    SDL_Quit();
fail_sdl:
    return result; 
}

void engine_shutdown(void)
{
    G_Shutdown(); 

    S_Shutdown();

    Cursor_FreeAll();

    SDL_GL_DeleteContext(s_context);
    SDL_DestroyWindow(s_window); 
    SDL_Quit();
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

int main(int argc, char **argv)
{
    int ret = EXIT_SUCCESS;

    if(argc != 2) {
        printf("Usage: %s [base directory path (which contains 'assets' and 'shaders' folders)]\n", argv[0]);
        ret = EXIT_FAILURE;
        goto fail_args;
    }

    g_basepath = argv[1];

    if(!engine_init(argv)) {
        ret = EXIT_FAILURE; 
        goto fail_init;
    }

    char map_path[512];
    strcpy(map_path, argv[1]);
    strcat(map_path, "assets/maps/grass-cliffs-1");
    G_NewGameWithMap(map_path, "grass-cliffs.pfmap", "grass-cliffs.pfmat");

    char script_path[512];
    strcpy(script_path, argv[1]);
    strcat(script_path, "scripts/demo.py");
    S_RunFile(script_path);

    /* -----> TODO: Loading of the entity - move this into scripting */
    char entity_path[512];
    strcpy(entity_path, argv[1]);
    strcat(entity_path, "assets/models/sinbad");

    s_demo_entity = AL_EntityFromPFObj(entity_path, "Sinbad.pfobj", "Sinbad");
    if(!s_demo_entity){
        ret = EXIT_FAILURE; 
        goto fail_entity;
    }
    G_AddEntity(s_demo_entity);

    A_InitCtx(s_demo_entity, "Dance", 24);

    s_demo_entity->pos = (vec3_t){0.0f, 5.0f, -50.0f};
    s_demo_entity->scale = (vec3_t){1.0f, 1.0f, 1.0f};
    /* <-----                                                        */

    /* -----> TODO: Setting one-time lighting configs - move into scripting */
    R_GL_SetAmbientLightColor((vec3_t){1.0f, 1.0f, 1.0f});
    R_GL_SetLightEmitColor((vec3_t){1.0f, 1.0f, 1.0f});
    R_GL_SetLightPos((vec3_t){0.0f, 300.0f, 0.0f});
    /* <-----                                                               */

    while(!s_quit) {

        process_events();
        G_Update();
        render();        

    }

fail_entity:
    engine_shutdown();
fail_init:
fail_args:
    exit(ret);
}

