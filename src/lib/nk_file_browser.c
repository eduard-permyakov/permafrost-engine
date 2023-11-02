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

#include "public/nk_file_browser.h"
#include "public/pf_string.h"
#include "public/mem.h"
#include "../main.h"

#include <string.h>
#include <stdio.h>
#include <math.h>

#if defined(_WIN32)
#define WINVER 0x0600

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#define NTDDI_VERSION 0x06000000
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#include <windows.h>
#include <shlobj.h>
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
#define BUTTONS_PER_ROW         (6)

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

#if defined(_WIN32)

static void fb_path_fix_separator(char *path)
{
    while(*path) {
        if(*path == '\\')
            *path = '/';
        path++;
    }
}

static struct file *fb_get_list(const char *dir, size_t *out_size)
{
    size_t capacity = 256;
    struct file *files = malloc(sizeof(struct file) * capacity);
    if(!files)
        return NULL;

    size_t wlen;
    wchar_t wdir[NK_MAX_PATH_LEN + 1];
    mbstowcs_s(&wlen, wdir, NK_MAX_PATH_LEN, dir, strlen(dir));
    wdir[wlen - 1] = '/';
    wdir[wlen + 0] = '*';
    wdir[wlen + 1] = '\0';

    size_t nfiles = 0;
    WIN32_FIND_DATA entry = {0};
    HANDLE handle;

    handle = FindFirstFile(wdir, &entry);
    if(handle == INVALID_HANDLE_VALUE) {
        free(files);
        return NULL;
    }

    do{
        if(nfiles == capacity) {
            capacity *= 2;
            struct file *rfiles = realloc(files, sizeof(struct file) * capacity);
            if(!rfiles) {
                FindClose(handle);
                free(files);
                return NULL;
            }else{
                files = rfiles;
            }
        }

        char cname[MAX_PATH];
        size_t len = WideCharToMultiByte(CP_UTF8, WC_NO_BEST_FIT_CHARS | WC_COMPOSITECHECK | WC_DEFAULTCHAR,
            entry.cFileName, -1, cname, sizeof(cname), NULL, NULL);

        files[nfiles].is_dir = (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        pf_strlcpy(files[nfiles].name, cname, NK_MAX_PATH_LEN);
        nfiles++;

    }while(FindNextFile(handle, &entry));

    FindClose(handle);

    *out_size = nfiles;
    return files;
}

static void fb_realpath(const char rel[], char abs[])
{
    if(NULL == _fullpath(abs, rel, NK_MAX_PATH_LEN)) {
        abs[0] = '\0';
    }
    fb_path_fix_separator(abs);
}

static bool fb_homedir(char out[])
{
    PWSTR path;
    KNOWNFOLDERID id = FOLDERID_Profile;
    HRESULT result = SHGetKnownFolderPath(&id, 0, NULL, &path);
    if(result != S_OK)
        return false;

    char cpath[NK_MAX_PATH_LEN];
    size_t len = WideCharToMultiByte(CP_UTF8, WC_NO_BEST_FIT_CHARS | WC_COMPOSITECHECK | WC_DEFAULTCHAR,
        path, -1, cpath, sizeof(cpath), NULL, NULL);

    pf_strlcpy(out, cpath, NK_MAX_PATH_LEN);
    fb_path_fix_separator(out);
    return true;
}

static size_t fb_get_places(size_t maxout, char out[][NK_MAX_PATH_LEN], 
                            char *icons[], char names[][32])
{
    size_t ret = 0;
    if(ret == maxout)
        return ret;

    bool found = fb_homedir(out[0]);
    if(found) {
        icons[ret] = DEFAULT_HOME_ICON;
        pf_strlcpy(names[ret], "Home", 32);
        ret++;
    }
    if(ret == maxout)
        return ret;

    if(found) {
        pf_snprintf(out[ret], NK_MAX_PATH_LEN, "%s/Desktop", out[ret-1]);
        icons[ret] = DEFAULT_DESKTOP_ICON;
        pf_strlcpy(names[ret], "Desktop", 32);
        ret++;
    }

    wchar_t drives[MAX_PATH];
    size_t len = GetLogicalDriveStrings(MAX_PATH, drives);
    size_t cursor = 0;

    while(cursor < len) {

        if(ret == maxout)
            return ret;

        char cpath[MAX_PATH];
        size_t len = WideCharToMultiByte(CP_UTF8, WC_NO_BEST_FIT_CHARS | WC_COMPOSITECHECK | WC_DEFAULTCHAR,
            drives + cursor, -1, cpath, sizeof(cpath), NULL, NULL);

        icons[ret] = DEFAULT_DISK_ICON;
        pf_strlcpy(out[ret], cpath, NK_MAX_PATH_LEN);
        fb_path_fix_separator(out[ret]);
        pf_strlcpy(names[ret], cpath, len-1);
        ret++;

        cursor += len;
    }

    return ret;
}

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
                    closedir(d);
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
    pf_strlcpy(relf, rel, sizeof(relf));

    if(NULL == realpath(relf, absf)) {
        abs[0] = '\0';
    }else{
        pf_strlcpy(abs, absf, NK_MAX_PATH_LEN);
    }
}

static bool fb_homedir(char out[static NK_MAX_PATH_LEN])
{
    const char *home = getenv("HOME");
    if(!home) {
        home = getpwuid(getuid())->pw_dir;
    }
    if(!home) {
        return false;
    }
    pf_strlcpy(out, home, NK_MAX_PATH_LEN);
    return true;
}

static size_t fb_get_places(size_t maxout, char out[][NK_MAX_PATH_LEN], 
                            char *icons[], char names[][32])
{
    size_t ret = 0;
    if(ret == maxout)
        return ret;

    bool found = fb_homedir(out[0]);
    if(found) {
        icons[ret] = DEFAULT_HOME_ICON;
        pf_strlcpy(names[ret], "Home", 32);
        ret++;
    }
    if(ret == maxout)
        return ret;

    if(found) {
        pf_snprintf(out[ret], NK_MAX_PATH_LEN, "%s/Desktop", out[ret-1]);
        icons[ret] = DEFAULT_DESKTOP_ICON;
        pf_strlcpy(names[ret], "Desktop", 32);
        ret++;
    }
    if(ret == maxout)
        return ret;

    icons[ret] = DEFAULT_DISK_ICON;
    pf_strlcpy(out[ret], "/", NK_MAX_PATH_LEN);
    pf_strlcpy(names[ret], "File System", 32);
    ret++;

    return ret;
}

#endif

static void fb_selector_bar(struct nk_context *ctx, struct nk_fb_state *state)
{
    float spacing_x = ctx->style.window.spacing.x;
    ctx->style.window.spacing.x = 0;

    char *d = state->directory;
    char *begin = d;
    if(*begin == '/')
        begin++;

    nk_layout_row_dynamic(ctx, SELECTOR_BAR_HEIGHT, BUTTONS_PER_ROW);

    while(*d++) {
        if(*d == '/') {
            *d = '\0';
            if(nk_button_label(ctx, begin)) {
                *d++ = '/'; 
                *d = '\0';
                begin = d + 1;
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

static size_t fb_selector_rows(const char *path)
{
    size_t ret = 0;
    char *ch = strchr(path,'/');
    while(ch) {
        ret++;
        ch = strchr(ch + 1, '/');
    }
    return ceil(ret / (float)BUTTONS_PER_ROW);
}

static void fb_file_list(struct nk_context *ctx, struct nk_fb_state *state)
{
    struct nk_rect total_space = nk_window_get_content_region(ctx);
    float height = total_space.h - ctx->style.window.spacing.y 
        - fb_selector_rows(state->directory) * (SELECTOR_BAR_HEIGHT + ctx->style.window.spacing.y);

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

static void fb_places_list(struct nk_context *ctx, struct nk_fb_state *state)
{
    const size_t max_places = 16;
    STALLOC(char, paths, max_places * NK_MAX_PATH_LEN);
    STALLOC(char*, icons, max_places);
    STALLOC(char, names, max_places * 32);

    size_t nplaces = fb_get_places(max_places, 
        (char(*)[NK_MAX_PATH_LEN])paths, icons, 
        (char(*)[32])names);

    for(int i = 0; i < nplaces; i++) {

        nk_layout_row_dynamic(ctx, 25, 1);
        if(nk_button_texpath_label(ctx, icons[i], names + (i * 32), NK_TEXT_ALIGN_RIGHT)) {

            pf_strlcpy(state->directory, paths + (i * NK_MAX_PATH_LEN), sizeof(state->directory));
            state->selected[0] = '\0';
        }
    }

    STFREE(paths);
    STFREE(icons);
    STFREE(names);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void nk_file_browser(struct nk_context *ctx, struct nk_fb_state *state)
{
    char abs[NK_MAX_PATH_LEN];
    fb_realpath(state->directory, abs);
    pf_strlcpy(state->directory, abs, sizeof(state->directory));

    nk_group_begin(ctx, state->name, state->flags);
    {
        struct nk_rect total_space = nk_window_get_content_region(ctx);
        float ratio[] = {0.25f, NK_UNDEFINED};
        nk_layout_row(ctx, NK_DYNAMIC, total_space.h - ctx->style.window.group_padding.y * 2, 2, ratio);

        char left_name[256];
        pf_snprintf(left_name, sizeof(left_name), "%s.Left", state->name);

        nk_group_begin(ctx, left_name, NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BORDER);
        {
            fb_places_list(ctx, state);
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

struct file *nk_file_list(const char *dir, size_t *out_size)
{
    return fb_get_list(dir, out_size);
}

