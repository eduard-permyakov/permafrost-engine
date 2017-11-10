#include "asset_load.h"
#include "entity.h"
#include "camera.h"
#include "render/public/render.h"
#include "anim/public/anim.h"

#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

#include <stdbool.h>


static SDL_Window    *s_window;
static SDL_GLContext  s_context;

static bool           s_quit = false; 

static struct camera *s_camera;

struct entity         *s_temp;


static void process_events(void)
{
    static bool s_w_down = false;
    static bool s_a_down = false;
    static bool s_s_down = false;
    static bool s_d_down = false;

    SDL_Event event;    
   
    while(SDL_PollEvent(&event)) {

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
            case SDL_SCANCODE_W: s_w_down = true; break;
            case SDL_SCANCODE_A: s_a_down = true; break;
            case SDL_SCANCODE_S: s_s_down = true; break;
            case SDL_SCANCODE_D: s_d_down = true; break;

            case SDL_SCANCODE_ESCAPE: s_quit = true; break;
            }

            break;

        case SDL_KEYUP:
            switch(event.key.keysym.scancode) {
            case SDL_SCANCODE_W: s_w_down = false; break;
            case SDL_SCANCODE_A: s_a_down = false; break;
            case SDL_SCANCODE_S: s_s_down = false; break;
            case SDL_SCANCODE_D: s_d_down = false; break;
            }

            break;

        case SDL_MOUSEMOTION: 
            camera_change_direction(s_camera, event.motion.xrel, event.motion.yrel);
            break;

        }
    }

    if(s_w_down) camera_move_front_tick(s_camera);
    if(s_a_down) camera_move_left_tick (s_camera);
    if(s_s_down) camera_move_back_tick (s_camera);
    if(s_d_down) camera_move_right_tick(s_camera);
}

static void render(void)
{
    SDL_GL_MakeCurrent(s_window, s_context); 

    glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT /* GL_DEPTH_BUFFER_BIT */);

    R_GL_Draw(s_temp);
    R_GL_DrawSkeleton(s_temp, A_GetSkeleton(s_temp));

    SDL_GL_SwapWindow(s_window);
}

int main(int argc, char **argv)
{
    if(SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "Failed to initialize SDL: %s\n", SDL_GetError());
        goto fail_sdl;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    s_window = SDL_CreateWindow(
        "Permafrost Engine",
        SDL_WINDOWPOS_UNDEFINED, 
        SDL_WINDOWPOS_UNDEFINED,
        1024, 
        576, 
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);

    s_context = SDL_GL_CreateContext(s_window); 

    glewExperimental = GL_TRUE;
    if(glewInit() != GLEW_OK) {
        fprintf(stderr, "Failed to initialize GLEW\n");
        goto fail_glew;
    }

    SDL_GL_SetSwapInterval(0); 
    SDL_SetRelativeMouseMode(true);
    glViewport(0, 0, 1024, 576);

    if(!R_Init())
        goto fail_render;

    s_camera = camera_new();
    if(!s_camera)
        goto fail_camera;

    camera_set_pos  (s_camera, (vec3_t){ 0.0f,  0.0f,  0.0f});
    camera_set_front(s_camera, (vec3_t){ 0.0f,  0.0f, -1.0f});
    camera_set_up   (s_camera, (vec3_t){ 0.0f,  1.0f,  0.0f});
    camera_set_speed(s_camera, 0.1f);
    camera_set_sens (s_camera, 0.05f);

    /* Temp */
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

    s_temp = AL_EntityFromPFObj("/home/eduard/engine/assets/models/mage/mage.pfobj", "mage", 4);
    //s_temp = AL_EntityFromPFObj("/home/eduard/engine/assets/models/archer/archer_version_3.pfobj", "mage", 4);
    PFM_mat4x4_make_trans(0.0f, 0.0f, -50.0f, &s_temp->model_matrix);

    R_AL_DumpPrivate(stdout, s_temp->render_private);
    A_AL_DumpPrivate(stdout, s_temp->anim_private);
    /* End Temp */

    while(!s_quit) {

        process_events();
        camera_tick_finish(s_camera);
        render();        

    }

    camera_free(s_camera);
    SDL_GL_DeleteContext(s_context);
    SDL_DestroyWindow(s_window);
    SDL_Quit();

    exit(EXIT_SUCCESS);

fail_camera:
    free(s_camera);
fail_render:
fail_glew:
    SDL_GL_DeleteContext(s_context);
    SDL_DestroyWindow(s_window);
    SDL_Quit();
fail_sdl:
    exit(EXIT_FAILURE);
}

