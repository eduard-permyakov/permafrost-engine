#include <GL/glew.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

#include "asset_load.h"
#include "entity.h"
#include "render/public/render.h"
#include "anim/public/anim.h"

#include "stdbool.h"


static SDL_Window    *s_window;
static SDL_GLContext  s_context;

static bool           s_quit = false; 

struct entity *s_temp;


static void process_events(void)
{
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
        }
    }
}

static void render(void)
{
    SDL_GL_MakeCurrent(s_window, s_context); 

    glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT /* GL_DEPTH_BUFFER_BIT */);

    R_GL_Draw(s_temp);

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
    glViewport(0, 0, 1024, 576);

    if(!R_Init())
        goto fail_render;

    /* Temp */
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

    s_temp = AL_EntityFromPFObj("/home/eduard/engine/assets/models/mage/mage.pfobj", "mage", 4);
    PFM_mat4x4_identity(&s_temp->model_matrix);
    PFM_mat4x4_make_trans(0.0f, 0.0f, -50.0f, &s_temp->view_matrix);

    R_AL_DumpPrivate(stdout, s_temp->render_private);
    A_AL_DumpPrivate(stdout, s_temp->anim_private);
    /* End Temp */

    while(!s_quit) {

        process_events();
        render();        

    }

    SDL_GL_DeleteContext(s_context);
    SDL_DestroyWindow(s_window);
    SDL_Quit();

    exit(EXIT_SUCCESS);

fail_render:
fail_glew:
    SDL_GL_DeleteContext(s_context);
    SDL_DestroyWindow(s_window);
    SDL_Quit();
fail_sdl:
    exit(EXIT_FAILURE);
}

