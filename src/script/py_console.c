/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2024 Eduard Permyakov 
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

#include <Python.h> /* Must be included first */
#include <node.h>
#include <grammar.h>
#include <parsetok.h>
#include <errcode.h>

#include "py_console.h"
#include "../ui.h"
#include "../event.h"
#include "../lib/public/lru_cache.h"
#include "../lib/public/mem.h"
#include "../lib/public/vec.h"
#include "../lib/public/pf_string.h"
#include "../game/public/game.h"
#include "../lib/public/pf_nuklear.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

PyAPI_DATA(grammar) _PyParser_Grammar;

#define CONSOLE_HIST_SIZE (1024)
#define MAX_LINES         (256)
#define MIN(a, b)         ((a) < (b) ? (a) : (b))
#define CONSOLE_HOTKEY    (SDL_SCANCODE_F12)

enum line_type{
    LINE_STDOUT,
    LINE_STDERR,
    LINE_INPUT_NEW,
    LINE_INPUT_CONTINUE
};

struct strbuff{
    enum line_type type;
    char line[256];
};

enum code_status{
    COMPLETE,
    NEEDS_MORE,
    INVALID
};

VEC_TYPE(str, struct strbuff)
VEC_IMPL(static inline, str, struct strbuff)

LRU_CACHE_TYPE(hist, struct strbuff)
LRU_CACHE_PROTOTYPES(static, hist, struct strbuff)
LRU_CACHE_IMPL(static, hist, struct strbuff)

static PyObject *PyPf_write_stdout(PyObject *self, PyObject *args);
static PyObject *PyPf_write_stderr(PyObject *self, PyObject *args);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static bool      s_shown = false;
static lru(hist) s_history;
static uint64_t  s_next_lineid;
static char      s_inputbuff[256];
static vec_str_t s_multilines;

static PyMethodDef stdout_catcher_methods[] = {
    {"write", PyPf_write_stdout, METH_VARARGS, "Write something to stdout."},
    {NULL, NULL, 0, NULL}
};

static PyMethodDef stderr_catcher_methods[] = {
    {"write", PyPf_write_stderr, METH_VARARGS, "Write something to stderr."},
    {NULL, NULL, 0, NULL}
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void add_history(const char *str, enum line_type type)
{
    struct strbuff next;
    next.type = type;
    pf_strlcpy(next.line, str, sizeof(next.line));
    lru_hist_put(&s_history, s_next_lineid++, &next);
}

static void write_str(const char *str, size_t idx)
{
    assert(idx < 2);
    static size_t nextline_idx[2] = {0};
    static char nextline[2][80];

    /* Tokenize by lines, and add to the history */
    const char *curr = str;
    while(*curr) {
        if(*curr == '\r') {
            curr++;
            continue;
        }
        if(*curr == '\n' || nextline_idx[idx] == sizeof(nextline[0])-1) {
            struct strbuff next;
            next.type = (idx == 0 ? LINE_STDOUT : LINE_STDERR);
            memcpy(next.line, nextline[idx], nextline_idx[idx]);
            next.line[nextline_idx[idx]] = '\0';
            lru_hist_put(&s_history, s_next_lineid++, &next);

            nextline_idx[idx] = 0;
            if(*curr == '\n') {
                curr++;
            }
            continue;
        }
        nextline[idx][nextline_idx[idx]++] = *curr;
        curr++;
    }
}

static PyObject *PyPf_write_stdout(PyObject *self, PyObject *args)
{
    const char *what;
    if(!PyArg_ParseTuple(args, "s", &what))
        return NULL;
    if(*what == '\0') {
        Py_RETURN_NONE; 
    }
    write_str(what, 0);
    Py_RETURN_NONE;
}

static PyObject *PyPf_write_stderr(PyObject *self, PyObject *args)
{
    const char *what;
    if(!PyArg_ParseTuple(args, "s", &what))
        return NULL;
    if(*what == '\0') {
        Py_RETURN_NONE; 
    }
    write_str(what, 1);
    Py_RETURN_NONE;
}

static bool try_run_multiline(const char *next)
{
    size_t tot_size = 0;
    for(int i = 0; i < vec_size(&s_multilines); i++) {
        const char *line = vec_AT(&s_multilines, i).line;
        tot_size += strlen(line) + 1;
    }
    tot_size += 1;

    char *buff = malloc(tot_size);
    if(!buff)
        return false;

    buff[0] = '\0';
    for(int i = 0; i < vec_size(&s_multilines); i++) {
        const char *line = vec_AT(&s_multilines, i).line;
        pf_strlcat(buff, line, tot_size);
        pf_strlcat(buff, "\n", tot_size);
    }

    int result = PyRun_SimpleString(buff);
    PF_FREE(buff);
    return (result == 0);
}

static bool errors_equal(perrdetail err1, perrdetail err2)
{
    if(err1.error != err2.error)
        return false;
    if(err1.lineno != err2.lineno)
        return false;
    if(err1.offset != err2.offset)
        return false;
    if(err1.token != err2.token)
        return false;
    if(err1.expected != err2.expected)
        return false;
    return true;
}

/* To support evaluating multi-line statements, we need to be able to 
 * determine when code is 'incomplete', and expects more lines to finish
 * off a proper multi-line statement. Due to the limited parser API, the
 * logic for this is not intuitive, but it is the same approach used in
 * CPython's own REPL loop:
 * 
 * Compile three times: as is, with \n, and with \n\n appended.  If it
 * compiles as is, it's complete. If it compiles with one \n appended,
 * we expect more. If it doesn't compile either way, we compare the
 * error we get when compiling with \n or \n\n appended. If the errors
 * are the same, the code is broken. But if the errors are different, we
 * expect more.
*/
static enum code_status try_compile(const char *next, bool append)
{
    enum code_status ret = INVALID;
    size_t tot_size = 0;
    if(append) {
        for(int i = 0; i < vec_size(&s_multilines); i++) {
            const char *line = vec_AT(&s_multilines, i).line;
            tot_size += strlen(line) + 1;
        }
    }
    tot_size += strlen(next);
    tot_size += 3;

    char *buff = malloc(tot_size);
    if(!buff)
        return INVALID;

    buff[0] = '\0';
    if(append) {
        for(int i = 0; i < vec_size(&s_multilines); i++) {
            const char *line = vec_AT(&s_multilines, i).line;
            pf_strlcat(buff, line, tot_size);
            pf_strlcat(buff, "\n", tot_size);
        }
    }
    pf_strlcat(buff, next, tot_size);

    perrdetail orig_err;
    struct _node *orig = PyParser_ParseStringFlagsFilename(buff, "<console>",
        &_PyParser_Grammar, Py_single_input, &orig_err, 0);
    bool compiled = (orig != NULL);
    PyNode_Free(orig);

    if(compiled) {
        ret = COMPLETE;
        goto out;
    }

    if(orig_err.error == E_EOF) {
        ret = NEEDS_MORE;
        goto out;
    }

    pf_strlcat(buff, "\n", tot_size);
    perrdetail with_newline_err;
    struct _node *with_newline = PyParser_ParseStringFlagsFilename(buff, "<console>",
        &_PyParser_Grammar, Py_single_input, &with_newline_err, 0);
    compiled = (with_newline != NULL);
    PyNode_Free(with_newline);

    if(compiled) {
        ret = NEEDS_MORE;
        goto out;
    }

    pf_strlcat(buff, "\n", tot_size);
    perrdetail with_two_newlines_err;
    struct _node *with_two_newlines = PyParser_ParseStringFlagsFilename(buff, "<console>",
        &_PyParser_Grammar, Py_single_input, &with_two_newlines_err, 0);
    compiled = (with_two_newlines != NULL);
    PyNode_Free(with_two_newlines);

    if(compiled || !errors_equal(with_newline_err, with_two_newlines_err)) {
        ret = NEEDS_MORE;
        goto out;
    }

out:
    PF_FREE(buff);
    if(ret == INVALID) {
        PyParser_SetError(&orig_err);
    }
    return ret;
}

static bool handle_compilation_result(enum code_status status, const char *str)
{
    switch(status) {
    case COMPLETE:
        return true;
    case NEEDS_MORE: {
        /* We are waiting for the next line */
        struct strbuff nextline;
        pf_strlcpy(nextline.line, str, sizeof(nextline.line));
        vec_str_push(&s_multilines, nextline);
        break;
    }
    case INVALID:
        /* The line is malformed */
        vec_str_reset(&s_multilines);
        PyErr_Print();
        break;
    }
    return false;
}

static void do_interactive_one(const char *str)
{
    size_t len = strlen(str);

    if(len == 0) {
        if(vec_size(&s_multilines)) {
            /* We terminated a multi-line statement 
             */
            bool success = try_run_multiline(str);
            vec_str_reset(&s_multilines);
            if(!success) {
                PyErr_Print();
            }
        }
        return;
    }

    if(vec_size(&s_multilines)) {
        /* In any case, the line must be syntactically valid. Check this. 
         */
        enum code_status status = try_compile(str, true);
        bool valid = handle_compilation_result(status, str);
        if(valid) {
            /* Continue adding lines to the current multi-line string, but do 
             * not evaluate the multi-line string until it is terminated by an 
             * explicit newline.
             */
            struct strbuff nextline;
            pf_strlcpy(nextline.line, str, sizeof(nextline.line));
            vec_str_push(&s_multilines, nextline);
        }
        return;
    }

    /* It must be a single-line statement, or the start of a new multi-line statement */
    enum code_status status = try_compile(str, false);
    bool valid = handle_compilation_result(status, str);
    if(valid) {
        PyRun_SimpleString(str);
    }
}

static void on_update(void *user, void *event)
{
    if(!s_shown)
        return;

    static nk_uint x_offset = 0;
    static nk_uint y_offset = 0;
    const char *font = UI_GetActiveFont();
    UI_SetActiveFont("__default__");

    struct nk_context *ctx = UI_GetContext();
    const vec2_t vres = (vec2_t){1920, 1080};
    const vec2_t adj_vres = UI_ArAdjustedVRes(vres);
    const struct rect bounds = (struct rect){
        vres.x / 2.0f - 400,
        vres.y / 2.0f - 300,
        800,
        600,
    };
    const struct rect adj_bounds = UI_BoundsForAspectRatio(bounds, 
        vres, adj_vres, ANCHOR_X_CENTER | ANCHOR_Y_CENTER);

    if(nk_begin_with_vres(ctx, "Console", 
        nk_rect(adj_bounds.x, adj_bounds.y, adj_bounds.w, adj_bounds.h), 
        NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_MOVABLE 
      | NK_WINDOW_CLOSABLE | NK_WINDOW_NO_SCROLLBAR, 
        (struct nk_vec2i){adj_vres.x, adj_vres.y})) {

        nk_layout_row_dynamic(ctx, 500, 1);
        if(nk_group_scrolled_offset_begin(ctx, &x_offset, &y_offset, "__history__", NK_WINDOW_BORDER)) {

            uint64_t key;
            (void)key;
            struct strbuff line;
            LRU_FOREACH_REVERSE_SAFE_REMOVE(hist, &s_history, key, line, {

                const char *prompt = (line.type == LINE_INPUT_NEW)      ? ">>>"
                                   : (line.type == LINE_INPUT_CONTINUE) ? "..."
                                                                        : "";
                struct nk_color color = (line.type == LINE_STDOUT) ? nk_rgb(0xaa, 0xaa, 0x80)
                                      : (line.type == LINE_STDERR) ? nk_rgb(0xed, 0x48, 0x48)
                                                                   : nk_rgb(0xff, 0xff, 0xff);

                nk_layout_row_begin(ctx, NK_DYNAMIC, 12, 2);
                nk_layout_row_push(ctx, 0.05);
                nk_label_colored(ctx, prompt, NK_TEXT_ALIGN_RIGHT | NK_TEXT_ALIGN_MIDDLE, 
                    nk_rgb(255, 255, 0));
                nk_layout_row_push(ctx, 0.95);
                nk_label_colored(ctx, line.line, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, color);
                nk_layout_row_end(ctx);
            });
            nk_group_end(ctx);
        }
        nk_layout_row_begin(ctx, NK_DYNAMIC, 40, 3);
        nk_layout_row_push(ctx, 0.05);
        const char *prompt;
        if(vec_size(&s_multilines) > 0) {
            prompt = "...";
        }else{
            prompt = ">>>";
        }
        nk_label_colored(ctx, prompt, NK_TEXT_ALIGN_RIGHT | NK_TEXT_ALIGN_MIDDLE, 
            nk_rgb(0, 255, 0));

        int len = strlen(s_inputbuff);
        nk_layout_row_push(ctx, 0.8);
        nk_edit_string(ctx, NK_EDIT_SIMPLE | NK_EDIT_ALWAYS_INSERT_MODE | NK_EDIT_ALLOW_TAB, 
            s_inputbuff, &len, sizeof(s_inputbuff), nk_filter_default);
        len = MIN(len, sizeof(s_inputbuff)-1);
        s_inputbuff[len] = '\0';

        bool enter_pressed = ctx->current->edit.active 
                          && nk_input_is_key_pressed(&ctx->input, NK_KEY_ENTER);

        nk_layout_row_push(ctx, 0.15);
        if((nk_button_label(ctx, "ENTER") || enter_pressed)) {
            enum line_type type = (vec_size(&s_multilines) > 0 ? LINE_INPUT_CONTINUE : LINE_INPUT_NEW);
            add_history(s_inputbuff, type);
            do_interactive_one(s_inputbuff);
            s_inputbuff[0] = '\0';
            /* Scroll to the bottom */
            y_offset = INT_MAX;
        }
        nk_layout_row_end(ctx);
    }
    nk_end(ctx);
    UI_SetActiveFont(font);

    struct nk_window *win = nk_window_find(ctx, "Console");
    if(win->flags & (NK_WINDOW_CLOSED | NK_WINDOW_HIDDEN)) {
        s_shown = false;
    }
}

static void on_keydown(void *user, void *event)
{
    SDL_Event *e = (SDL_Event*)event;
    if(e->key.keysym.scancode == CONSOLE_HOTKEY) {
        S_Console_ToggleShown();
    }
}

static void print_welcome(void)
{
    const char *version = Py_GetVersion();
    const char *platform = Py_GetPlatform();

    char infoline[256];
    pf_snprintf(infoline, sizeof(infoline), "%s on %s", version, platform);

    char *firstline = infoline;
    char *secondline = infoline;
    while(*secondline && *secondline++ != '\n');
    *(secondline - 1) = '\0';

    add_history(firstline, LINE_STDOUT);
    if(*secondline) {
        add_history(secondline, LINE_STDOUT);
    }

    pf_snprintf(infoline, sizeof(infoline), "Welcome to the Permafrost Engine Python console.");
    add_history(infoline, LINE_STDOUT);
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool S_Console_Init(void)
{
    s_next_lineid = 0;
    s_shown = false;
    memset(s_inputbuff, 0, sizeof(s_inputbuff));
    E_Global_Register(EVENT_UPDATE_START, on_update, NULL, G_ALL);
    E_Global_Register(SDL_KEYDOWN, on_keydown, NULL, G_ALL);

    vec_str_init(&s_multilines);

    if(!lru_hist_init(&s_history, CONSOLE_HIST_SIZE, NULL))
        return false;

    PyObject *stdout_catcher = Py_InitModule("stdout_catcher", stdout_catcher_methods);
    if(!stdout_catcher) {
        lru_hist_destroy(&s_history);
        return false;
    }

    PyObject *stderr_catcher = Py_InitModule("stderr_catcher", stderr_catcher_methods);
    if(!stderr_catcher) {
        Py_DECREF(stdout_catcher);
        lru_hist_destroy(&s_history);
        return false;
    }

    PySys_SetObject("stdout", stdout_catcher);
    PySys_SetObject("stderr", stderr_catcher);

    print_welcome();
    return true;
}

void S_Console_Shutdown(void)
{
    vec_str_destroy(&s_multilines);
    lru_hist_destroy(&s_history);
    E_Global_Unregister(EVENT_UPDATE_START, on_update);
    E_Global_Unregister(SDL_KEYDOWN, on_keydown);
}

void S_Console_Show(void)
{
    s_shown = true;
}

void S_Console_ToggleShown(void)
{
    s_shown = !s_shown;
}

