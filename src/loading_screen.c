/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2025 Eduard Permyakov 
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

#include "loading_screen.h"
#include "main.h"
#include "config.h"
#include "ui.h"
#include "render/public/render.h"
#include "render/public/render_ctrl.h"
#include "lib/public/vec.h"
#include "lib/public/mem.h"
#include "lib/public/pf_string.h"
#include "lib/public/stb_image.h"
#include "lib/public/nk_file_browser.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

VEC_TYPE(str, const char*)
VEC_IMPL(static inline, str, const char*)

#define LOADING_SCREEN_CHANGE_PERIOD_MS (5000)

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

/* Only used before the renderer is up */
static SDL_Surface *s_early_loading_screen;
static uint32_t     s_last_change_tick = 0;
static vec_str_t    s_loading_screens;
static int          s_curr_index = 0;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void index_loading_screens(void)
{
    char absdir[NK_MAX_PATH_LEN];
    pf_snprintf(absdir, sizeof(absdir), "%s/%s", g_basepath, "assets/loading_screens");

    size_t nfiles = 0;
    struct file *files = nk_file_list(absdir, &nfiles);

    for(int i = 0; i < nfiles; i++) {
        if(files[i].is_dir)
            continue;

        char path[NK_MAX_PATH_LEN];
        pf_snprintf(path, sizeof(path), "%s/%s", "assets/loading_screens", files[i].name);

        if(pf_endswith(path, CONFIG_LOADING_SCREEN))
            continue;

        if(!pf_endswith(path, ".png")
        && !pf_endswith(path, ".jpg")
        && !pf_endswith(path, ".jpeg"))
            continue;

        const char *loading_screen = pf_strdup(path);
        if(!loading_screen)
            continue;
        vec_str_push(&s_loading_screens, loading_screen);
    }
}

static const char *next_loading_screen(void)
{
    const char *ret = CONFIG_LOADING_SCREEN;
    if(vec_size(&s_loading_screens) > 0) {
        ret = vec_AT(&s_loading_screens, s_curr_index);
    }
    uint32_t now = SDL_GetTicks();
    if(s_last_change_tick == 0) {
        s_last_change_tick = now;
    }
    if(SDL_TICKS_PASSED(now, s_last_change_tick + LOADING_SCREEN_CHANGE_PERIOD_MS)) {
        /* Make sure we don't show the same screen twice in a row */
        int next;
        do{
            next = rand() % (vec_size(&s_loading_screens));
        }while(next == s_curr_index);
        assert(next < vec_size(&s_loading_screens));

        s_curr_index = next;
        s_last_change_tick = now;
    }
    return ret;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool LoadingScreen_Init(void)
{
    ASSERT_IN_MAIN_THREAD();
    SDL_Surface *surface = NULL;
    bool ret = false;

    vec_str_init(&s_loading_screens);
    index_loading_screens();
    s_curr_index = rand() % (vec_size(&s_loading_screens)+1);

    char fullpath[512];
    pf_snprintf(fullpath, sizeof(fullpath), "%s/%s", g_basepath, CONFIG_LOADING_SCREEN);

    int width, height, orig_format;
    unsigned char *image = stbi_load(fullpath, &width, &height, 
        &orig_format, STBI_rgb);

    if(!image) {
        fprintf(stderr, "Loading Screen: Failed to load image: %s\n", fullpath);
        goto fail_load_image;
    }

    surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 24, SDL_PIXELFORMAT_RGB24);

    if(!surface) {
        fprintf(stderr, "Loading Screen: Failed to create SDL surface: %s\n", SDL_GetError());    
        goto fail_surface;
    }

    memcpy(surface->pixels, image, width * height * 3);
    s_early_loading_screen = surface;
    ret = true;

fail_surface:
    stbi_image_free(image);
fail_load_image:
    return ret;
}

void LoadingScreen_DrawEarly(SDL_Window *window)
{
    ASSERT_IN_MAIN_THREAD();
    assert(window);

    if(!s_early_loading_screen)
        return;

    SDL_Surface *win_surface = SDL_GetWindowSurface(window);
    SDL_Renderer *sw_renderer = SDL_CreateSoftwareRenderer(win_surface);
    assert(sw_renderer);

    SDL_SetRenderDrawColor(sw_renderer, 0x00, 0x00, 0x00, 0xff);
    SDL_RenderClear(sw_renderer);

    SDL_Texture *tex;
    if(s_early_loading_screen && (tex = SDL_CreateTextureFromSurface(sw_renderer, s_early_loading_screen))) {
        SDL_RenderCopy(sw_renderer, tex, NULL, NULL);
        SDL_DestroyTexture(tex);
    }

    SDL_UpdateWindowSurface(window);
    SDL_DestroyRenderer(sw_renderer);
}

void LoadingScreen_Shutdown(void)
{
    for(int i = 0; i < vec_size(&s_loading_screens); i++) {
        const char *curr = vec_AT(&s_loading_screens, i);
        PF_FREE(curr);
    }
    vec_str_destroy(&s_loading_screens);
    if(s_early_loading_screen) {
        SDL_FreeSurface(s_early_loading_screen);
    }
}

/* As we are pusing to the _front_ of the render queue, push the rendering 
 * commands in reverse order. */
void LoadingScreen_Tick(void)
{
    char buff[256];
    pf_snprintf(buff, sizeof(buff), "FRAME: [%lu]", g_frame_idx);

    char old_font[256];
    pf_strlcpy(old_font, UI_GetActiveFont(), sizeof(old_font));
    UI_SetActiveFont("__default__");

    UI_DrawText(buff, (struct rect){50, 50, 200, 50}, (struct rgba){255, 0, 0, 255});
    UI_LoadingScreenTick();
    UI_SetActiveFont(old_font);

    const char *screen = next_loading_screen();
    R_PushCmdImmediateFront((struct rcmd){
        .func = R_GL_DrawLoadingScreen,
        .nargs = 1,
        .args = {
            R_PushArg(screen, strlen(screen) + 1)
        }
    });
    R_PushCmdImmediateFront((struct rcmd){ R_GL_BeginFrame, 0 });
}

