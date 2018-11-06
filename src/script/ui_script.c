/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018 Eduard Permyakov 
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

#include "public/script.h"
#include "ui_style_script.h"
#include "../lib/public/pf_nuklear.h"
#include "../lib/public/kvec.h"
#include "../event.h"
#include "../collision.h"
#include "../config.h"

#include <assert.h>

struct rect{
    int x, y, width, height;
};

typedef struct {
    PyObject_HEAD
    const char  *name;
    struct rect  rect;
    int          flags;
    bool         shown;
}PyWindowObject;

static int       PyWindow_init(PyWindowObject *self, PyObject *args);

static PyObject *PyWindow_layout_row_static(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_layout_row_dynamic(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_layout_row_begin(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_layout_row_end(PyWindowObject *self);
static PyObject *PyWindow_layout_row_push(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_label_colored(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_label_colored_wrap(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_button_label(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_simple_chart(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_selectable_label(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_option_label(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_edit_string(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_group(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_combo_box(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_checkbox(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_color_picker(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_show(PyWindowObject *self);
static PyObject *PyWindow_hide(PyWindowObject *self);
static PyObject *PyWindow_update(PyWindowObject *self);
static PyObject *PyWindow_new(PyTypeObject *type, PyObject *args, PyObject *kwds);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct nk_context       *s_nk_ctx;
static kvec_t(PyWindowObject*)  s_active_windows;

static PyMethodDef PyWindow_methods[] = {
    {"layout_row_static", 
    (PyCFunction)PyWindow_layout_row_static, METH_VARARGS,
    "Add a row with a static layout."},

    {"layout_row_dynamic", 
    (PyCFunction)PyWindow_layout_row_dynamic, METH_VARARGS,
    "Add a row with a dynamic layout."},

    {"layout_row_begin", 
    (PyCFunction)PyWindow_layout_row_begin, METH_VARARGS,
    "Begin a new row to which widgets can be pushed."},

    {"layout_row_end", 
    (PyCFunction)PyWindow_layout_row_end, METH_NOARGS,
    "End a row previously started with 'layout_row_begin'."},

    {"layout_row_push", 
    (PyCFunction)PyWindow_layout_row_push, METH_VARARGS,
    "Add a widget to the currently active row. Note that this must be preceded by "
    "a call to 'layout_row_begin'."},

    {"label_colored", 
    (PyCFunction)PyWindow_label_colored, METH_VARARGS,
    "Add a colored label layout with the specified alignment."},

    {"label_colored_wrap", 
    (PyCFunction)PyWindow_label_colored_wrap, METH_VARARGS,
    "Add a colored label layout."},

    {"button_label", 
    (PyCFunction)PyWindow_button_label, METH_VARARGS,
    "Add a button with a label and action."},

    {"simple_chart", 
    (PyCFunction)PyWindow_simple_chart, METH_VARARGS,
    "Add a chart with a single slot."},

    {"selectable_label", 
    (PyCFunction)PyWindow_selectable_label, METH_VARARGS,
    "Adds a label that can be toggled to be selected with a mouse click. "
    "Returns the new state of the selectable label."},

    {"option_label", 
    (PyCFunction)PyWindow_option_label, METH_VARARGS,
    "Radio button with the specified text. Returns if the radio button is selected."},

    {"edit_string", 
    (PyCFunction)PyWindow_edit_string, METH_VARARGS,
    "Text field for getting string input from the user. Returns the current text."},

    {"group", 
    (PyCFunction)PyWindow_group, METH_VARARGS,
    "The window UI statements within the argument callable will be put in a group."},

    {"combo_box", 
    (PyCFunction)PyWindow_combo_box, METH_VARARGS,
    "Present a combo box with a list of selectable options."},

    {"checkbox", 
    (PyCFunction)PyWindow_checkbox, METH_VARARGS,
    "Checkbox which can be toggled. Returns True if checked."},

    {"color_picker", 
    (PyCFunction)PyWindow_color_picker, METH_VARARGS,
    "Graphical color picker widget. Returns the selected color as an RGBA tuple."},

    {"show", 
    (PyCFunction)PyWindow_show, METH_NOARGS,
    "Make the window visible."},

    {"hide", 
    (PyCFunction)PyWindow_hide, METH_NOARGS,
    "Make the window invisible."},

    {"update", 
    (PyCFunction)PyWindow_update, METH_NOARGS,
    "Handles layout and state changes of the window. Default implementation is empty. "
    "This method should be overridden by subclasses to customize the window look and behavior."},

    {NULL}  /* Sentinel */
};

static PyTypeObject PyWindow_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "pf.Window",               /* tp_name */
    sizeof(PyWindowObject),    /* tp_basicsize */
    0,                         /* tp_itemsize */
    0,                         /* tp_dealloc */
    0,                         /* tp_print */
    0,                         /* tp_getattr */
    0,                         /* tp_setattr */
    0,                         /* tp_reserved */
    0,                         /* tp_repr */
    0,                         /* tp_as_number */
    0,                         /* tp_as_sequence */
    0,                         /* tp_as_mapping */
    0,                         /* tp_hash  */
    0,                         /* tp_call */
    0,                         /* tp_str */
    0,                         /* tp_getattro */
    0,                         /* tp_setattro */
    0,                         /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    "Permafrost Engine UI window.", /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    PyWindow_methods,          /* tp_methods */
    0,                         /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)PyWindow_init,   /* tp_init */
    0,                         /* tp_alloc */
    PyWindow_new,              /* tp_new */
};

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static int PyWindow_init(PyWindowObject *self, PyObject *args)
{
    const char *name;
    struct rect rect;
    int flags;

    if(!PyArg_ParseTuple(args, "s(iiii)i", &name, &rect.x, &rect.y, &rect.width, &rect.height, &flags)) {
        PyErr_SetString(PyExc_TypeError, "3 arguments expected: integer, tuple of 4 integers, and integer.");
        return -1;
    }

    self->name = name;
    self->rect = rect;
    self->flags = flags;
    self->shown = false;
    return 0;
}

static PyObject *PyWindow_layout_row_static(PyWindowObject *self, PyObject *args)
{
    int height, width, cols;

    if(!PyArg_ParseTuple(args, "iii", &height, &width, &cols)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be three integers.");
        return NULL;
    }
    nk_layout_row_static(s_nk_ctx, height, width, cols);
    Py_RETURN_NONE;
}

static PyObject *PyWindow_layout_row_dynamic(PyWindowObject *self, PyObject *args)
{
    int height, cols;

    if(!PyArg_ParseTuple(args, "ii", &height, &cols)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be three integers.");
        return NULL;
    }
    nk_layout_row_dynamic(s_nk_ctx, height, cols);
    Py_RETURN_NONE;
}

static PyObject *PyWindow_layout_row_begin(PyWindowObject *self, PyObject *args)
{
    int layout_fmt;
    int height, cols;

    if(!PyArg_ParseTuple(args, "iii", &layout_fmt, &height, &cols)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be three integers.");
        return NULL;
    }

    if(layout_fmt != NK_STATIC && layout_fmt != NK_DYNAMIC) {
        PyErr_SetString(PyExc_TypeError, "First argument must be 0 or 1.");
        return NULL;
    }

    nk_layout_row_begin(s_nk_ctx, layout_fmt, height, cols);
    Py_RETURN_NONE;
}

static PyObject *PyWindow_layout_row_end(PyWindowObject *self)
{
    nk_layout_row_end(s_nk_ctx);
    Py_RETURN_NONE;
}

static PyObject *PyWindow_layout_row_push(PyWindowObject *self, PyObject *args)
{
    int width;

    if(!PyArg_ParseTuple(args, "i", &width)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a single integer.");
        return NULL;
    }

    nk_layout_row_push(s_nk_ctx, width);
    Py_RETURN_NONE; 
}

static PyObject *PyWindow_label_colored(PyWindowObject *self, PyObject *args)
{
    const char *text;
    int alignment;
    int r, g, b;

    if(!PyArg_ParseTuple(args, "si(iii)", &text, &alignment, &r, &g, &b)) {
        PyErr_SetString(PyExc_TypeError, "3 arguments expected: a string, an integer and a tuple of 3 integers.");
        return NULL;
    }

    nk_label_colored(s_nk_ctx, text, alignment, nk_rgb(r, g, b));
    Py_RETURN_NONE;  
}

static PyObject *PyWindow_label_colored_wrap(PyWindowObject *self, PyObject *args)
{
    const char *text;
    int r, g, b;

    if(!PyArg_ParseTuple(args, "s(iii)", &text, &r, &g, &b)) {
        PyErr_SetString(PyExc_TypeError, "2 arguments expected: a string and a tuple of 3 integers.");
        return NULL;
    }

    nk_label_colored_wrap(s_nk_ctx, text, nk_rgb(r, g, b));
    Py_RETURN_NONE;  
}

static PyObject *PyWindow_button_label(PyWindowObject *self, PyObject *args)
{
    const char *str;
    PyObject *callable;

    if(!PyArg_ParseTuple(args, "sO", &str, &callable)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a string and an object.");
        return NULL;
    }

    if(!PyCallable_Check(callable)) {
        PyErr_SetString(PyExc_TypeError, "Second argument must be callable.");
        return NULL;
    }

    if(nk_button_label(s_nk_ctx, str)) {
        PyObject *ret = PyObject_CallObject(callable, NULL);
        Py_XDECREF(ret);
    }

    Py_RETURN_NONE;
}

static PyObject *PyWindow_simple_chart(PyWindowObject *self, PyObject *args)
{
    int type;
    int min, max;
    PyObject *list;

    int hovered_index = -1;
    long hovered_val;

    if(!PyArg_ParseTuple(args, "i(ii)O", &type, &min, &max, &list)) {
        PyErr_SetString(PyExc_TypeError, "3 arguments expected: an integer, a tuple of two integers, and an object.");
        return NULL;
    }

    if(!PyList_Check(list)) {
        PyErr_SetString(PyExc_TypeError, "Last argument must be a list.");
        return NULL;
    }

    unsigned num_datapoints = PyList_Size(list);
    if(nk_chart_begin(s_nk_ctx, type, num_datapoints, min, max)) {
    
        for(int i = 0; i < num_datapoints; i++) {
        
            PyObject *elem = PyList_GetItem(list, i);
            if(!PyInt_Check(elem)) {
                PyErr_SetString(PyExc_TypeError, "List elements must be integers.");
                return NULL;
            }

            long val = PyInt_AsLong(elem);
            nk_flags res = nk_chart_push(s_nk_ctx, val);

            if(res & NK_CHART_HOVERING) {
                hovered_index = i;
                hovered_val = val;
            }
        }
        nk_chart_end(s_nk_ctx);

        if(hovered_index != -1)
            nk_tooltipf(s_nk_ctx, "Value: %lu", hovered_val);
    }

    Py_RETURN_NONE;
}

static PyObject *PyWindow_selectable_label(PyWindowObject *self, PyObject *args)
{
    const char *text; 
    int align_flags;
    int on;

    if(!PyArg_ParseTuple(args, "sii", &text, &align_flags, &on)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a string and two integers.");
        return NULL;
    }

    nk_selectable_label(s_nk_ctx, text, align_flags, &on);
    if(0 == on)
        Py_RETURN_FALSE;
    else
        Py_RETURN_TRUE;
}

static PyObject *PyWindow_option_label(PyWindowObject *self, PyObject *args)
{
    const char *text; 
    int set;

    if(!PyArg_ParseTuple(args, "si", &text, &set)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a string and an integer.");
        return NULL;
    }

    set = nk_option_label(s_nk_ctx, text, set);
    if(0 == set)
        Py_RETURN_FALSE;
    else
        Py_RETURN_TRUE;
}

static PyObject *PyWindow_edit_string(PyWindowObject *self, PyObject *args)
{
    int flags;
    const char *str;

    if(!PyArg_ParseTuple(args, "is", &flags, &str)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be an integer and a string.");
        return NULL;
    }

    char textbuff[128];
    int len = strlen(str);

    assert(len < sizeof(textbuff));
    strcpy(textbuff, str);

    nk_edit_string(s_nk_ctx, flags, textbuff, &len, sizeof(textbuff), nk_filter_default);
    textbuff[len] = '\0';
    return Py_BuildValue("s", textbuff);
}

static PyObject *PyWindow_group(PyWindowObject *self, PyObject *args)
{
    const char *name;
    int group_flags;
    PyObject *callable;

    if(!PyArg_ParseTuple(args, "siO", &name, &group_flags, &callable)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a string, an integer and an object object.");
        return NULL;
    }

    if(!PyCallable_Check(callable)) {
        PyErr_SetString(PyExc_TypeError, "Second argument must be callable.");
        return NULL;
    }

    if(nk_group_begin(s_nk_ctx, name, group_flags)) {
        PyObject *ret = PyObject_CallObject(callable, NULL);
        Py_XDECREF(ret);
    }
    nk_group_end(s_nk_ctx);
    Py_RETURN_NONE;
}

static PyObject *PyWindow_combo_box(PyWindowObject *self, PyObject *args)
{
    PyObject *items_list;
    int selected_idx;
    int item_height;
    struct nk_vec2 size;

    if(!PyArg_ParseTuple(args, "Oii(ff)", &items_list, &selected_idx, &item_height, &size.x, &size.y)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be an object, two integers, and a tuple of two floats.");
        return NULL;
    }

    if(!PyList_Check(items_list)) {
        PyErr_SetString(PyExc_TypeError, "First argument must be a list.");
        return NULL;
    }

    size_t num_items = PyList_Size(items_list);
    const char *labels[num_items];

    for(int i = 0; i < num_items; i++) {

        PyObject *str = PyList_GetItem(items_list, i);
        if(!PyString_Check(str)) {
            PyErr_SetString(PyExc_TypeError, "First argument list must only contain strings.");
            return NULL;
        }
        labels[i] = PyString_AsString(str);
    }

    int ret = nk_combo(s_nk_ctx, labels, num_items, selected_idx, item_height, size);
    return Py_BuildValue("i", ret);
}

static PyObject *PyWindow_checkbox(PyWindowObject *self, PyObject *args)
{
    const char *label;
    int selected;

    if(!PyArg_ParseTuple(args, "si", &label, &selected)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a string and an integer.");
        return NULL;
    }

    nk_checkbox_label(s_nk_ctx, label, &selected);
    return Py_BuildValue("i", selected);
}

static PyObject *PyWindow_color_picker(PyWindowObject *self, PyObject *args)
{
    struct nk_color color;
    struct nk_colorf colorf;
    struct nk_vec2 size;

    if(!PyArg_ParseTuple(args, "(iiii)(ff)", &color.r, &color.g, &color.b, &color.a, &size.x, &size.y)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a tuple of 4 ints and a tuple of 2 floats.");
        return NULL;
    }

    if(nk_combo_begin_color(s_nk_ctx, color, nk_vec2(size.x, size.y+10))) {
    
        nk_layout_row_dynamic(s_nk_ctx, size.y, 1);
        colorf = nk_color_picker(s_nk_ctx, nk_color_cf(color), NK_RGB);
        color = nk_rgba_cf(colorf);
        nk_combo_end(s_nk_ctx);
    }

    return Py_BuildValue("(i,i,i,i)", color.r, color.g, color.b, color.a);
}

static PyObject *PyWindow_show(PyWindowObject *self)
{
    if(self->shown)
        Py_RETURN_NONE;

    kv_push(PyWindowObject*, s_active_windows, self);
    self->shown = true;
    nk_window_show(s_nk_ctx, self->name, NK_SHOWN);
    Py_RETURN_NONE;
}

static bool equal(PyWindowObject *const *a, PyWindowObject *const *b)
{
    return *a == *b;
}

static PyObject *PyWindow_hide(PyWindowObject *self)
{
    if(!self->shown)
        Py_RETURN_NONE;

    int idx;
    kv_indexof(PyWindowObject*, s_active_windows, self, equal, idx);
    kv_del(PyWindowObject*, s_active_windows, idx);
    self->shown = false;
    nk_window_show(s_nk_ctx, self->name, NK_HIDDEN);
    Py_RETURN_NONE;
}

static PyObject *PyWindow_update(PyWindowObject *self)
{
    Py_RETURN_NONE;
}

static PyObject *PyWindow_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *self = type->tp_alloc(type, 0);
    return self;
}

static void active_windows_update(void *user, void *event)
{
    (void)user;
    (void)event;

    for(int i = 0; i < kv_size(s_active_windows); i++) {
    
        PyWindowObject *win = kv_A(s_active_windows, i);

        if(nk_begin(s_nk_ctx, win->name, 
            nk_rect(win->rect.x, win->rect.y, win->rect.width, win->rect.height), win->flags)) {

            PyObject *ret = PyObject_CallMethod((PyObject*)win, "update", NULL); 
            Py_XDECREF(ret);
            if(!ret) {
                PyErr_Print();
                exit(EXIT_FAILURE);
            }

            struct nk_vec2 pos = nk_window_get_position(s_nk_ctx);
            struct nk_vec2 size = nk_window_get_size(s_nk_ctx);
            win->rect = (struct rect){pos.x, pos.y, size.x, size.y};
        }
        nk_end(s_nk_ctx);
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool S_UI_Init(struct nk_context *ctx)
{
    assert(ctx);
    s_nk_ctx = ctx;

    kv_init(s_active_windows);
    return E_Global_Register(EVENT_UPDATE_UI, active_windows_update, NULL);
}

void S_UI_Shutdown(void)
{
    E_Global_Unregister(EVENT_UPDATE_UI, active_windows_update);
    kv_destroy(s_active_windows);
}

void S_UI_PyRegister(PyObject *module)
{
    if(PyType_Ready(&PyWindow_type) < 0)
        return;
    Py_INCREF(&PyWindow_type);
    PyModule_AddObject(module, "Window", (PyObject*)&PyWindow_type);

    assert(s_nk_ctx);
    S_UI_Style_PyRegister(module, s_nk_ctx);
}

bool S_UI_MouseOverWindow(int mouse_x, int mouse_y)
{
    for(int i = 0; i < kv_size(s_active_windows); i++) {

        PyWindowObject *win = kv_A(s_active_windows, i);
        if(C_PointInsideRect2D(
            (vec2_t){mouse_x,                       mouse_y                       },
            (vec2_t){win->rect.x,                   win->rect.y                   },
            (vec2_t){win->rect.x + win->rect.width, win->rect.y                   },
            (vec2_t){win->rect.x + win->rect.width, win->rect.y + win->rect.height},
            (vec2_t){win->rect.x,                   win->rect.y + win->rect.height}))
            return true;
    }

    return false;
}

