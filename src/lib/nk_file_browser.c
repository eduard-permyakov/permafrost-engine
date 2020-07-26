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

#include "public/nk_file_browser.h"
#include "public/pf_string.h"
#include "../main.h"

#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#if defined(_WIN32)
#error "TODO"
#else
#include <unistd.h>
#include <dirent.h>
#include <pwd.h>
#endif


#define DEFAULT_FOLDER_ICON     "assets/icons/folder-icon.png"
#define DEFAULT_FILE_ICON       "assets/icons/file-icon.png"
#define DEFAULT_HOME_ICON       "assets/icons/home-icon.png"
#define DEFAULT_DESKTOP_ICON    "assets/icons/desktop-icon.png"
#define DEFAULT_DISK_ICON       "assets/icons/hard-drive-icon.png"
#define SELECTOR_BAR_HEIGHT     (25)

struct file{
    bool is_dir;
    char name[NK_MAX_PATH_LEN];
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

#if defined(_WIN32)
#else

static struct file *fb_get_list(const char *dir, size_t *out_size)
{
    size_t capacity = 256;
    struct file *files = malloc(sizeof(struct file) * capacity);
    if(!files)
        return NULL;

    size_t nfiles = 0;
    struct dirent *entry;
    DIR *d = opendir(dir);
    if(d) {
        while((entry = readdir(d)) != NULL) {
            if(nfiles == capacity) {
                capacity *= 2;
                struct file *rfiles = realloc(files, sizeof(struct file) * capacity);
                if(!rfiles) {
                    free(files);
                    return NULL;
                }else{
                    files = rfiles;
                }
            }
            files[nfiles].is_dir = (entry->d_type == DT_DIR);
            pf_strlcpy(files[nfiles].name, entry->d_name, NK_MAX_PATH_LEN);
            nfiles++;
        }
        closedir(d);
    }else{
        free(files);
        return NULL;
    }

    *out_size = nfiles;
    return files;
}

static void fb_realpath(const char rel[static NK_MAX_PATH_LEN], char abs[static NK_MAX_PATH_LEN])
{
    char relf[PATH_MAX], absf[PATH_MAX];
    pf_strlcpy(relf, rel, NK_MAX_PATH_LEN);

    if(NULL == realpath(relf, absf)) {
        abs[0] = '\0';
    }else{
        pf_strlcpy(abs, absf, NK_MAX_PATH_LEN);
    }
}

static const char *fb_homedir(void)
{
    const char *home = getenv("HOME");
    if(!home) {
        home = getpwuid(getuid())->pw_dir;
    }
    return home;
}

#endif

static void fb_selector_bar(struct nk_context *ctx, struct nk_fb_state *state)
{
    float spacing_x = ctx->style.window.spacing.x;
    ctx->style.window.spacing.x = 0;
    const size_t buttons_per_row = 6;

    char *d = state->directory;
    char *begin = d + 1;

    nk_layout_row_dynamic(ctx, SELECTOR_BAR_HEIGHT, buttons_per_row);

    while(*d++) {
        if(*d == '/') {
            *d = '\0';
            if(nk_button_label(ctx, begin)) {
                *d++ = '/'; 
                *d = '\0';
                break;
            }
            *d = '/';
            begin = d + 1;
        }
    }

    if(strlen(begin) > 0) {
        nk_button_label(ctx, begin);
    }

    ctx->style.window.spacing.x = spacing_x;
}

static int files_compare(struct file *a, struct file *b)
{
    if(a->is_dir && !b->is_dir)
        return -1;

    if(!a->is_dir && b->is_dir)
        return 1;

    return strcmp(a->name, b->name);
}

/* Sort the files alphabetically, and putting directories first */
static void fb_sort_list(struct file *files, size_t nfiles)
{
    int i = 1;
    while(i < nfiles) {
        int j = i;
        while(j > 0 && files_compare(&files[j - 1], &files[j]) > 0) {

            struct file tmp = files[j - 1];
            files[j - 1] = files[j];
            files[j] = tmp;
            j--;
        }
        i++;
    }
}

static void fb_file_list(struct nk_context *ctx, struct nk_fb_state *state)
{
    struct nk_rect total_space = nk_window_get_content_region(ctx);
    float height = total_space.h - SELECTOR_BAR_HEIGHT - ctx->style.window.spacing.y * 2;

    size_t nfiles;
    struct file *files = fb_get_list(state->directory, &nfiles);
    if(!files) {
        return;
    }
    fb_sort_list(files, nfiles);

    nk_layout_row_dynamic(ctx, height, 1);

    char list_name[256];
    pf_snprintf(list_name, sizeof(list_name), "%s.List", state->name);

    int sel_idx = -1;
    nk_group_begin(ctx, list_name, NK_WINDOW_BORDER);
    {
        for(int i = 0; i < nfiles; i++) {

            nk_layout_row_dynamic(ctx, 25, 1);
            int sel = (0 == strcmp(state->selected, files[i].name));

            if(files[i].is_dir) {
                nk_selectable_texpath_label(ctx, DEFAULT_FOLDER_ICON, files[i].name, NK_TEXT_ALIGN_LEFT, &sel);
            }else{
                nk_selectable_texpath_label(ctx, DEFAULT_FILE_ICON, files[i].name, NK_TEXT_ALIGN_LEFT, &sel);
            }

            if(sel) {
                sel_idx = i;
                pf_strlcpy(state->selected, files[i].name, sizeof(state->selected));
            }
        }
    }
    nk_group_end(ctx);

    if(sel_idx >= 0 && files[sel_idx].is_dir) {
        char newpath_rel[NK_MAX_PATH_LEN], newpath_abs[NK_MAX_PATH_LEN];
        pf_snprintf(newpath_rel, sizeof(newpath_rel), "%s/%s", state->directory, files[sel_idx].name);
        fb_realpath(newpath_rel, newpath_abs);

        pf_strlcpy(state->directory, newpath_abs, sizeof(state->directory));
        state->selected[0] = '\0';
    }

    free(files);
}

static void fb_favorites_list(struct nk_context *ctx, struct nk_fb_state *state)
{
    const char *home = fb_homedir();

    nk_layout_row_dynamic(ctx, 25, 1);
    if(nk_button_texpath_label(ctx, DEFAULT_HOME_ICON, "Home", NK_TEXT_ALIGN_RIGHT)) {

        pf_strlcpy(state->directory, home, sizeof(state->directory));
        state->selected[0] = '\0';
    }

    nk_layout_row_dynamic(ctx, 25, 1);
    if(nk_button_texpath_label(ctx, DEFAULT_DESKTOP_ICON, "Desktop", NK_TEXT_ALIGN_RIGHT)) {
    
        pf_snprintf(state->directory, sizeof(state->directory), "%s/Desktop", home);
        state->selected[0] = '\0';
    }

    nk_layout_row_dynamic(ctx, 25, 1);
    if(nk_button_texpath_label(ctx, DEFAULT_DISK_ICON, "File System", NK_TEXT_ALIGN_RIGHT)) {

        pf_strlcpy(state->directory, "/", sizeof(state->directory));
        state->selected[0] = '\0';
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void nk_file_browser(struct nk_context *ctx, struct nk_fb_state *state)
{
    nk_group_begin(ctx, state->name, state->flags);
    {
        struct nk_rect total_space = nk_window_get_content_region(ctx);
        float ratio[] = {0.25f, NK_UNDEFINED};
        nk_layout_row(ctx, NK_DYNAMIC, total_space.h - ctx->style.window.group_padding.y * 2, 2, ratio);

        char left_name[256];
        pf_snprintf(left_name, sizeof(left_name), "%s.Left", state->name);

        nk_group_begin(ctx, left_name, NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BORDER);
        {
            fb_favorites_list(ctx, state);
        }
        nk_group_end(ctx);

        char right_name[256];
        pf_snprintf(right_name, sizeof(right_name), "%s.Right", state->name);

        nk_group_begin(ctx, right_name, NK_WINDOW_NO_SCROLLBAR);
        {
            fb_selector_bar(ctx, state);
            fb_file_list(ctx, state);
        }
        nk_group_end(ctx);
    }
    nk_group_end(ctx);
}

