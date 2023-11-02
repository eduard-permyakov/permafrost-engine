/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2021-2023 Eduard Permyakov 
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

#include <Python.h> /* must be first */
#include <osdefs.h>
#include <frameobject.h>

#include "py_error.h"
#include "../ui.h"
#include "../lib/public/pf_nuklear.h"
#include "../lib/public/pf_string.h"

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static int s_parse_syntax_error(PyObject *err, PyObject **message, const char **filename, 
                                 int *lineno, int *offset, const char **text)
{
    long hold;
    PyObject *v;

    /* old style errors */
    if (PyTuple_Check(err))
        return PyArg_ParseTuple(err, "O(ziiz)", message, filename,
                                lineno, offset, text);

    *message = NULL;

    /* new style errors.  `err' is an instance */
    *message = PyObject_GetAttrString(err, "msg");
    if (!*message)
        goto finally;

    v = PyObject_GetAttrString(err, "filename");
    if (!v)
        goto finally;
    if (v == Py_None) {
        Py_DECREF(v);
        *filename = NULL;
    }
    else {
        *filename = PyString_AsString(v);
        Py_DECREF(v);
        if (!*filename)
            goto finally;
    }

    v = PyObject_GetAttrString(err, "lineno");
    if (!v)
        goto finally;
    hold = PyInt_AsLong(v);
    Py_DECREF(v);
    if (hold < 0 && PyErr_Occurred())
        goto finally;
    *lineno = (int)hold;

    v = PyObject_GetAttrString(err, "offset");
    if (!v)
        goto finally;
    if (v == Py_None) {
        *offset = -1;
        Py_DECREF(v);
    } else {
        hold = PyInt_AsLong(v);
        Py_DECREF(v);
        if (hold < 0 && PyErr_Occurred())
            goto finally;
        *offset = (int)hold;
    }

    v = PyObject_GetAttrString(err, "text");
    if (!v)
        goto finally;
    if (v == Py_None) {
        Py_DECREF(v);
        *text = NULL;
    }
    else {
        *text = PyString_AsString(v);
        Py_DECREF(v);
        if (!*text)
            goto finally;
    }
    return 1;

finally:
    Py_XDECREF(*message);
    return 0;
}

static void s_print_err_text(int offset, const char *text, size_t maxout, char out[])
{
    if(!maxout)
        return;
    out[0] = '\0';

    char *nl;
    if (offset >= 0) {
        if (offset > 0 && offset == strlen(text) && text[offset - 1] == '\n')
            offset--;
        for (;;) {
            nl = strchr(text, '\n');
            if (nl == NULL || nl-text >= offset)
                break;
            offset -= (int)(nl+1-text);
            text = nl+1;
        }
        while (*text == ' ' || *text == '\t') {
            text++;
            offset--;
        }
    }
    pf_strlcat(out, "    ", maxout);
    pf_strlcat(out, text, maxout);
    if (*text == '\0' || text[strlen(text)-1] != '\n')
        pf_strlcat(out, "\n", maxout);
    if (offset == -1)
        return;
    pf_strlcat(out, "    ", maxout);
    offset--;
    while (offset > 0) {
        pf_strlcat(out, " ", maxout);
        offset--;
    }
    pf_strlcat(out, "^\n", maxout);
}

bool s_print_source_line(const char *filename, int lineno, int indent,
                        size_t maxout, char out[])
{
    if(!maxout)
        return true;
    out[0] = '\0';

    FILE *xfp = NULL;
    char linebuf[2000];
    int i;
    char namebuf[MAXPATHLEN+1];

    if (filename == NULL)
        return false;
    xfp = fopen(filename, "r" PY_STDIOTEXTMODE);
    if (xfp == NULL) {
        /* Search tail of filename in sys.path before giving up */
        PyObject *path;
        const char *tail = strrchr(filename, SEP);
        if (tail == NULL)
            tail = filename;
        else
            tail++;
        path = PySys_GetObject("path");
        if (path != NULL && PyList_Check(path)) {
            Py_ssize_t _npath = PyList_Size(path);
            int npath = Py_SAFE_DOWNCAST(_npath, Py_ssize_t, int);
            size_t taillen = strlen(tail);
            for (i = 0; i < npath; i++) {
                PyObject *v = PyList_GetItem(path, i);
                if (v == NULL) {
                    PyErr_Clear();
                    break;
                }
                if (PyString_Check(v)) {
                    size_t len;
                    len = PyString_GET_SIZE(v);
                    if (len + 1 + taillen >= MAXPATHLEN)
                        continue; /* Too long */
                    strcpy(namebuf, PyString_AsString(v));
                    if (strlen(namebuf) != len)
                        continue; /* v contains '\0' */
                    if (len > 0 && namebuf[len-1] != SEP)
                        namebuf[len++] = SEP;
                    strcpy(namebuf+len, tail);
                    xfp = fopen(namebuf, "r" PY_STDIOTEXTMODE);
                    if (xfp != NULL) {
                        break;
                    }
                }
            }
        }
    }

    if (xfp == NULL)
        return false;

    for (i = 0; i < lineno; i++) {
        char* pLastChar = &linebuf[sizeof(linebuf)-2];
        do {
            *pLastChar = '\0';
            if (Py_UniversalNewlineFgets(linebuf, sizeof linebuf, xfp, NULL) == NULL)
                break;
            /* fgets read *something*; if it didn't get as
               far as pLastChar, it must have found a newline
               or hit the end of the file;              if pLastChar is \n,
               it obviously found a newline; else we haven't
               yet seen a newline, so must continue */
        } while (*pLastChar != '\0' && *pLastChar != '\n');
    }
    if (i == lineno) {
        char buf[11];
        char *p = linebuf;
        while (*p == ' ' || *p == '\t' || *p == '\014')
            p++;

        /* Write some spaces before the line */
        strcpy(buf, "          ");
        assert (strlen(buf) == 10);
        while (indent > 0) {
            if(indent < 10)
                buf[indent] = '\0';
            pf_strlcat(out, buf, maxout);
            indent -= 10;
        }

        pf_strlcat(out, p, maxout);
        if (strchr(p, '\n') == NULL)
            pf_strlcat(out, "\n", maxout);
    }

    fclose(xfp);
    return true;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void S_Error_Update(struct py_err_ctx *err_ctx)
{
    if(!err_ctx->occurred)
        return;

    const char *font = UI_GetActiveFont();
    UI_SetActiveFont("__default__");

    struct nk_context *ctx = UI_GetContext();
    const vec2_t vres = (vec2_t){1920, 1080};
    const vec2_t adj_vres = UI_ArAdjustedVRes(vres);
    const struct rect bounds = (struct rect){
        vres.x / 2.0f - 400,
        vres.y / 2.0f - 200,
        800,
        400,
    };
    const struct rect adj_bounds = UI_BoundsForAspectRatio(bounds, 
        vres, adj_vres, ANCHOR_X_CENTER | ANCHOR_Y_CENTER);

    if(nk_begin_with_vres(ctx, "Unhandled Python Exception", 
        nk_rect(adj_bounds.x, adj_bounds.y, adj_bounds.w, adj_bounds.h), 
        NK_WINDOW_TITLE | NK_WINDOW_BORDER, (struct nk_vec2i){adj_vres.x, adj_vres.y})) {

        nk_layout_row_dynamic(ctx, 72, 1);
        nk_label_colored_wrap(ctx, "The application has encountered an unhandled Python exception. "
            "This indicates an error in the scripting logic but, depending on the error, it is quite "
            "possible that the game can continue operating without further problems if the error is "
            "simply ignored. Report the issue to the script authors and proceed at your own risk.", 
            nk_rgb(255, 255, 255));

        char buff[256] = "";
        PyObject *repr;

        assert(err_ctx->type);
        if(PyExceptionClass_Check(err_ctx->type) && PyExceptionClass_Name(err_ctx->type)) {
            const char *clsname = PyExceptionClass_Name(err_ctx->type);
            char *dot = strrchr(clsname, '.');
            if(dot)
                clsname = dot+1;
            pf_strlcpy(buff, clsname, sizeof(buff));
        }else{
            repr = PyObject_Str(err_ctx->type);
            if(repr) {
                pf_strlcpy(buff, PyString_AS_STRING(repr), sizeof(buff));
            }
            Py_XDECREF(repr);
        }

        PyObject *message = NULL;
        const char *filename, *text;
        int lineno, offset;

        if(err_ctx->value) {

            if(s_parse_syntax_error(err_ctx->value, &message, &filename, &lineno, &offset, &text)) {
                repr = PyObject_Str(message);
                Py_DECREF(message);
            }else{
                PyErr_Clear();
                repr = PyObject_Str(err_ctx->value);
            }

            if(repr && strlen(PyString_AS_STRING(repr))) {
                pf_strlcat(buff, ": ", sizeof(buff));
                pf_strlcat(buff, PyString_AS_STRING(repr), sizeof(buff));
            }
            Py_XDECREF(repr);
        }

        nk_layout_row_dynamic(ctx, 18, 1);
        nk_label_colored(ctx, buff, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, nk_rgb(255, 0, 0));

        if(message) {

            nk_layout_row_dynamic(ctx, 8, 1);

            pf_snprintf(buff, sizeof(buff), "    File: \"%s\"", filename ? filename : "<string>", lineno);
            nk_layout_row_dynamic(ctx, 18, 1);
            nk_label_colored(ctx, buff, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, nk_rgb(255, 255, 0));

            pf_snprintf(buff, sizeof(buff), "    Line: %d", lineno);
            nk_layout_row_dynamic(ctx, 18, 1);
            nk_label_colored(ctx, buff, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, nk_rgb(255, 255, 0));

            if(text) {
            
                nk_layout_row_dynamic(ctx, 8, 1);
                s_print_err_text(offset, text, sizeof(buff), buff);

                int idx = 0;
                char *saveptr;
                char *curr = pf_strtok_r(buff, "\n", &saveptr);
                while(curr) {
                    struct nk_color clr = (idx == 0) ? nk_rgb(255, 255, 255) : nk_rgb(255, 0, 0);
                    nk_layout_row_dynamic(ctx, 18, 1);
                    nk_label_colored(ctx, curr, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, clr);
                    curr = pf_strtok_r(NULL, "\n", &saveptr);
                    idx++;
                }
            }
        }
        Py_XDECREF(message);

        if(err_ctx->traceback) {
        
            nk_layout_row_dynamic(ctx, 8, 1);
            nk_layout_row_dynamic(ctx, 18, 1);
            nk_label_colored(ctx, "Traceback:", NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, nk_rgb(255, 0, 0));

            long depth = 0;
            PyTracebackObject *tb = (PyTracebackObject*)err_ctx->traceback;

            while (tb != NULL) {
                depth++;
                tb = tb->tb_next;
            }
            tb = (PyTracebackObject*)err_ctx->traceback;

            while (tb != NULL) {
                if (depth <= 128) {

                    char filebuff[512] = "";
                    pf_snprintf(filebuff, sizeof(filebuff), "  [%02ld] %s: %d", depth,
                        PyString_AsString(tb->tb_frame->f_code->co_filename), tb->tb_lineno);
                    nk_layout_row_dynamic(ctx, 18, 1);
                    nk_label_colored(ctx, filebuff, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, 
                        nk_rgb(255, 255, 0));

                    char linebuff[512] = "";
                    s_print_source_line(PyString_AsString(tb->tb_frame->f_code->co_filename), tb->tb_lineno, 4, 
                        sizeof(linebuff), linebuff);
                    size_t len = strlen(linebuff);
                    linebuff[len > 0 ? len-1 : 0] = '\0'; /* trim newline */

                    nk_layout_row_dynamic(ctx, 18, 1);
                    nk_label_colored(ctx, linebuff, NK_TEXT_ALIGN_LEFT | NK_TEXT_ALIGN_MIDDLE, 
                        nk_rgb(255, 255, 255));
                }
                depth--;
                tb = tb->tb_next;
            }
        }

        nk_layout_row_dynamic(ctx, 8, 1);
        nk_layout_row_begin(ctx, NK_DYNAMIC, 40, 3);

        nk_layout_row_push(ctx, 0.3);
        nk_spacing(ctx, 1);

        nk_layout_row_push(ctx, 0.4);
        if(nk_button_label(ctx, "Continue")) {

            err_ctx->occurred = false;
            Py_CLEAR(err_ctx->type);
            Py_CLEAR(err_ctx->value);
            Py_CLEAR(err_ctx->traceback);
            G_SetSimState(err_ctx->prev_state);
        }

        nk_layout_row_push(ctx, 0.3);
        nk_spacing(ctx, 1);

        nk_layout_row_end(ctx);
    }
    nk_end(ctx);
    UI_SetActiveFont(font);
}

