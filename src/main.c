/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2017 Eduard Permyakov 
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
#include "camera.h"
#include "cam_control.h"
#include "config.h"
#include "cursor.h"
#include "render/public/render.h"
#include "anim/public/anim.h"
#include "lib/public/stb_image.h"
#include "map/public/map.h"

#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

#include <stdbool.h>
#include <assert.h>


#define PF_VER_MAJOR 0
#define PF_VER_MINOR 2
#define PF_VER_PATCH 0

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static SDL_Window            *s_window;
static SDL_GLContext          s_context;

static bool                   s_quit = false; 

static struct camera         *s_camera;
static struct camcontrol_ctx *s_cam_ctx;

struct entity                *s_demo_entity;
struct map                   *s_demo_map;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void process_events(void)
{
    SDL_Event event;    
   
    while(SDL_PollEvent(&event)) {

        CamControl_FPS_HandleEvent(s_cam_ctx, s_camera, event);

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
        }
    }
}

static void render(void)
{
    SDL_GL_MakeCurrent(s_window, s_context); 

    glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    R_GL_Draw(s_demo_entity->render_private, &s_demo_entity->model_matrix);

    M_RenderEntireMap(s_demo_map);

    SDL_GL_SwapWindow(s_window);
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

    if(SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "Failed to initialize SDL: %s\n", SDL_GetError());
        ret = EXIT_FAILURE;
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

    glewExperimental = GL_TRUE;
    if(glewInit() != GLEW_OK) {
        fprintf(stderr, "Failed to initialize GLEW\n");
        ret = EXIT_FAILURE;
        goto fail_glew;
    }

    SDL_GL_SetSwapInterval(0); 
    //SDL_SetRelativeMouseMode(true);
    glViewport(0, 0, CONFIG_RES_X, CONFIG_RES_Y);
    glEnable(GL_DEPTH_TEST);

    stbi_set_flip_vertically_on_load(true);

    if(!Cursor_InitAll(argv[1])) {
        ret = EXIT_FAILURE;
        goto fail_cursor;
    }
    Cursor_SetActive(CURSOR_POINTER);

    if(!R_Init(argv[1])) {
        ret = EXIT_FAILURE;
        goto fail_render;
    }

    s_camera = Camera_New();
    if(!s_camera) {
        ret = EXIT_FAILURE;
        goto fail_camera;
    }
    s_cam_ctx = CamControl_CtxNew();
    if(!s_cam_ctx) {
        ret = EXIT_FAILURE;
        goto fail_camera_ctx;
    }

    Camera_SetPos  (s_camera, (vec3_t){ 0.0f,  15.0f,  0.0f});
    Camera_SetFrontAndUp(s_camera, (vec3_t){ 0.0f,  0.0f, -1.0f}, (vec3_t){ 0.0f, 1.0f, 0.0f});
    Camera_SetSpeed(s_camera, 0.05f);
    Camera_SetSens (s_camera, 0.05f);

    char entity_path[512];
    strcpy(entity_path, argv[1]);
    strcat(entity_path, "assets/models/sinbad");

    s_demo_entity = AL_EntityFromPFObj(entity_path, "Sinbad.pfobj", "Sinbad");
    if(!s_demo_entity){
        ret = EXIT_FAILURE; 
        goto fail_entity;
    }

    A_InitCtx(s_demo_entity, "Dance", 24);

    mat4x4_t scale, trans;
    PFM_Mat4x4_MakeTrans(0.0f, 5.0f, -50.0f, &trans);
    PFM_Mat4x4_MakeScale(1.0f, 1.0f, 1.0f, &scale);
    PFM_Mat4x4_Mult4x4(&scale, &trans, &s_demo_entity->model_matrix);

    R_GL_SetAmbientLightColor((vec3_t){1.0f, 1.0f, 1.0f});
    R_GL_SetLightEmitColor((vec3_t){1.0f, 1.0f, 1.0f});
    R_GL_SetLightPos((vec3_t){0.0f, 100.0f, 0.0f});

    char map_path[512];
    strcpy(map_path, argv[1]);
    strcat(map_path, "assets/maps/grass-cliffs-1");

    s_demo_map = AL_MapFromPFMap(map_path, "grass-cliffs.pfmap", "grass-cliffs.pfmat");
    if(!s_demo_map){
        ret = EXIT_FAILURE; 
        goto fail_map;
    }
    M_CenterAtOrigin(s_demo_map);

    while(!s_quit) {

        process_events();
        CamControl_TickFinish(s_cam_ctx, s_camera);
        A_Update(s_demo_entity);
        render();        

    }

    AL_MapFree(s_demo_map);
fail_map:
    AL_EntityFree(s_demo_entity);
fail_entity:
    CamControl_CtxFree(s_cam_ctx);
fail_camera_ctx:
    Camera_Free(s_camera);
fail_camera:
fail_render:
    Cursor_FreeAll();
fail_cursor:
fail_glew:
    SDL_GL_DeleteContext(s_context);
    SDL_DestroyWindow(s_window);
    SDL_Quit();
fail_sdl:
fail_args:
    exit(ret);
}

