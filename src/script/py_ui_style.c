/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2020 Eduard Permyakov 
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

#include "py_ui_style.h"
#include "py_pickle.h"
#include "../ui.h"
#include "../lib/public/pf_nuklear.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/SDL_vec_rwops.h"
#include "../render/public/render.h"

#include <string.h>
#include <assert.h>


#define ARR_SIZE(a) (sizeof(a) / sizeof(a[0]))

#define CHK_TRUE(_pred, _label) do{ if(!(_pred)) goto _label; }while(0)
#define CHK_TRUE_RET(_pred)   \
    do{                       \
        if(!(_pred))          \
            return false;     \
    }while(0)

typedef struct {
    PyObject_HEAD
    struct nk_style_button *style;
}PyUIButtonStyleObject;

static PyObject *PyUIButtonStyle_get_normal(PyUIButtonStyleObject *self, void *);
static int       PyUIButtonStyle_set_normal(PyUIButtonStyleObject *self, PyObject *value, void *);
static PyObject *PyUIButtonStyle_get_hover(PyUIButtonStyleObject *self, void *);
static int       PyUIButtonStyle_set_hover(PyUIButtonStyleObject *self, PyObject *value, void *);
static PyObject *PyUIButtonStyle_get_active(PyUIButtonStyleObject *self, void *);
static int       PyUIButtonStyle_set_active(PyUIButtonStyleObject *self, PyObject *value, void *);
static PyObject *PyUIButtonStyle_get_border_color(PyUIButtonStyleObject *self, void *);
static int       PyUIButtonStyle_set_border_color(PyUIButtonStyleObject *self, PyObject *value, void *);
static PyObject *PyUIButtonStyle_get_text_background(PyUIButtonStyleObject *self, void *);
static int       PyUIButtonStyle_set_text_background(PyUIButtonStyleObject *self, PyObject *value, void *);
static PyObject *PyUIButtonStyle_get_text_normal(PyUIButtonStyleObject *self, void *);
static int       PyUIButtonStyle_set_text_normal(PyUIButtonStyleObject *self, PyObject *value, void *);
static PyObject *PyUIButtonStyle_get_text_hover(PyUIButtonStyleObject *self, void *);
static int       PyUIButtonStyle_set_text_hover(PyUIButtonStyleObject *self, PyObject *value, void *);
static PyObject *PyUIButtonStyle_get_text_active(PyUIButtonStyleObject *self, void *);
static int       PyUIButtonStyle_set_text_active(PyUIButtonStyleObject *self, PyObject *value, void *);
static PyObject *PyUIButtonStyle_get_text_alignment(PyUIButtonStyleObject *self, void *);
static int       PyUIButtonStyle_set_text_alignment(PyUIButtonStyleObject *self, PyObject *value, void *);
static PyObject *PyUIButtonStyle_get_border(PyUIButtonStyleObject *self, void *);
static int       PyUIButtonStyle_set_border(PyUIButtonStyleObject *self, PyObject *value, void *);
static PyObject *PyUIButtonStyle_get_rounding(PyUIButtonStyleObject *self, void *);
static int       PyUIButtonStyle_set_rounding(PyUIButtonStyleObject *self, PyObject *value, void *);
static PyObject *PyUIButtonStyle_get_padding(PyUIButtonStyleObject *self, void *);
static int       PyUIButtonStyle_set_padding(PyUIButtonStyleObject *self, PyObject *value, void *);
static PyObject *PyUIButtonStyle_get_image_padding(PyUIButtonStyleObject *self, void *);
static int       PyUIButtonStyle_set_image_padding(PyUIButtonStyleObject *self, PyObject *value, void *);
static PyObject *PyUIButtonStyle_get_touch_padding(PyUIButtonStyleObject *self, void *);
static int       PyUIButtonStyle_set_touch_padding(PyUIButtonStyleObject *self, PyObject *value, void *);

static PyObject *PyUIButtonStyle_pickle(PyUIButtonStyleObject *self);
static PyObject *PyUIButtonStyle_unpickle(PyObject *cls, PyObject *args);

typedef struct {
    PyObject_HEAD
    struct nk_style_window_header style;
    PyUIButtonStyleObject *close_button; 
    PyUIButtonStyleObject *minimize_button; 
}PyUIHeaderStyleObject;

static PyObject *PyUIHeaderStyle_new(PyTypeObject *type, PyObject *args, PyObject *kwds);
static void      PyUIHeaderStyle_dealloc(PyUIHeaderStyleObject *self);

static PyObject *PyUIHeaderStyle_get_close_button(PyUIHeaderStyleObject *self, void *);
static PyObject *PyUIHeaderStyle_get_minimize_button(PyUIHeaderStyleObject *self, void *);

static PyObject *PyUIHeaderStyle_get_normal(PyUIHeaderStyleObject *self, void *);
static int       PyUIHeaderStyle_set_normal(PyUIHeaderStyleObject *self, PyObject *value, void *);
static PyObject *PyUIHeaderStyle_get_hover(PyUIHeaderStyleObject *self, void *);
static int       PyUIHeaderStyle_set_hover(PyUIHeaderStyleObject *self, PyObject *value, void *);
static PyObject *PyUIHeaderStyle_get_active(PyUIHeaderStyleObject *self, void *);
static int       PyUIHeaderStyle_set_active(PyUIHeaderStyleObject *self, PyObject *value, void *);

static PyObject *PyUIHeaderStyle_get_label_normal(PyUIHeaderStyleObject *self, void *);
static int       PyUIHeaderStyle_set_label_normal(PyUIHeaderStyleObject *self, PyObject *value, void *);
static PyObject *PyUIHeaderStyle_get_label_hover(PyUIHeaderStyleObject *self, void *);
static int       PyUIHeaderStyle_set_label_hover(PyUIHeaderStyleObject *self, PyObject *value, void *);
static PyObject *PyUIHeaderStyle_get_label_active(PyUIHeaderStyleObject *self, void *);
static int       PyUIHeaderStyle_set_label_active(PyUIHeaderStyleObject *self, PyObject *value, void *);

static PyObject *PyUIHeaderStyle_pickle(PyUIHeaderStyleObject *self);
static PyObject *PyUIHeaderStyle_unpickle(PyObject *cls, PyObject *args);

typedef struct {
    PyObject_HEAD
    struct nk_style_selectable *style;
}PyUISelectableStyleObject;

/* background (inactive) */
static PyObject *PyUISelectableStyle_get_normal(PyUISelectableStyleObject *self, void *);
static int       PyUISelectableStyle_set_normal(PyUISelectableStyleObject *self, PyObject *value, void *);
static PyObject *PyUISelectableStyle_get_hover(PyUISelectableStyleObject *self, void *);
static int       PyUISelectableStyle_set_hover(PyUISelectableStyleObject *self, PyObject *value, void *);
static PyObject *PyUISelectableStyle_get_pressed(PyUISelectableStyleObject *self, void *);
static int       PyUISelectableStyle_set_pressed(PyUISelectableStyleObject *self, PyObject *value, void *);
/* background (active) */
static PyObject *PyUISelectableStyle_get_normal_active(PyUISelectableStyleObject *self, void *);
static int       PyUISelectableStyle_set_normal_active(PyUISelectableStyleObject *self, PyObject *value, void *);
static PyObject *PyUISelectableStyle_get_hover_active(PyUISelectableStyleObject *self, void *);
static int       PyUISelectableStyle_set_hover_active(PyUISelectableStyleObject *self, PyObject *value, void *);
static PyObject *PyUISelectableStyle_get_pressed_active(PyUISelectableStyleObject *self, void *);
static int       PyUISelectableStyle_set_pressed_active(PyUISelectableStyleObject *self, PyObject *value, void *);
/* text color (inactive) */
static PyObject *PyUISelectableStyle_get_text_normal(PyUISelectableStyleObject *self, void *);
static int       PyUISelectableStyle_set_text_normal(PyUISelectableStyleObject *self, PyObject *value, void *);
static PyObject *PyUISelectableStyle_get_text_hover(PyUISelectableStyleObject *self, void *);
static int       PyUISelectableStyle_set_text_hover(PyUISelectableStyleObject *self, PyObject *value, void *);
static PyObject *PyUISelectableStyle_get_text_pressed(PyUISelectableStyleObject *self, void *);
static int       PyUISelectableStyle_set_text_pressed(PyUISelectableStyleObject *self, PyObject *value, void *);
/* text color (active) */
static PyObject *PyUISelectableStyle_get_text_normal_active(PyUISelectableStyleObject *self, void *);
static int       PyUISelectableStyle_set_text_normal_active(PyUISelectableStyleObject *self, PyObject *value, void *);
static PyObject *PyUISelectableStyle_get_text_hover_active(PyUISelectableStyleObject *self, void *);
static int       PyUISelectableStyle_set_text_hover_active(PyUISelectableStyleObject *self, PyObject *value, void *);
static PyObject *PyUISelectableStyle_get_text_pressed_active(PyUISelectableStyleObject *self, void *);
static int       PyUISelectableStyle_set_text_pressed_active(PyUISelectableStyleObject *self, PyObject *value, void *);
/* properties */
static PyObject *PyUISelectableStyle_get_text_alignment(PyUISelectableStyleObject *self, void *);
static int       PyUISelectableStyle_set_text_alignment(PyUISelectableStyleObject *self, PyObject *value, void *);
static PyObject *PyUISelectableStyle_get_rounding(PyUISelectableStyleObject *self, void *);
static int       PyUISelectableStyle_set_rounding(PyUISelectableStyleObject *self, PyObject *value, void *);
static PyObject *PyUISelectableStyle_get_padding(PyUISelectableStyleObject *self, void *);
static int       PyUISelectableStyle_set_padding(PyUISelectableStyleObject *self, PyObject *value, void *);
static PyObject *PyUISelectableStyle_get_image_padding(PyUISelectableStyleObject *self, void *);
static int       PyUISelectableStyle_set_image_padding(PyUISelectableStyleObject *self, PyObject *value, void *);
static PyObject *PyUISelectableStyle_get_touch_padding(PyUISelectableStyleObject *self, void *);
static int       PyUISelectableStyle_set_touch_padding(PyUISelectableStyleObject *self, PyObject *value, void *);

static PyObject *PyUISelectableStyle_pickle(PyUISelectableStyleObject *self);
static PyObject *PyUISelectableStyle_unpickle(PyObject *cls, PyObject *args);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static PyMethodDef PyUIButtonStyle_methods[] = {
    {"__pickle__", 
    (PyCFunction)PyUIButtonStyle_pickle, METH_NOARGS,
    "Serialize a Permafrost Engine UIButtonStyle object to a string."},

    {"__unpickle__", 
    (PyCFunction)PyUIButtonStyle_unpickle, METH_VARARGS | METH_CLASS,
    "Create a new pf.UIButtonStyle instance from a string earlier returned from a __pickle__ method."
    "Returns a tuple of the new instance and the number of bytes consumed from the stream."},

    {NULL}  /* Sentinel */
};

static PyGetSetDef PyUIButtonStyle_getset[] = {
    {"normal",
    (getter)PyUIButtonStyle_get_normal, 
    (setter)PyUIButtonStyle_set_normal,
    "The look of the button in the normal state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"hover",
    (getter)PyUIButtonStyle_get_hover, 
    (setter)PyUIButtonStyle_set_hover,
    "The look of the button when the mouse is hovered over it - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"active",
    (getter)PyUIButtonStyle_get_active, 
    (setter)PyUIButtonStyle_set_active,
    "The look of the button in the active (pressed) state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"border_color",
    (getter)PyUIButtonStyle_get_border_color, 
    (setter)PyUIButtonStyle_set_border_color,
    "The (R, G, B, A) color of button borders.",
    NULL},

    {"text_background",
    (getter)PyUIButtonStyle_get_text_background, 
    (setter)PyUIButtonStyle_set_text_background,
    "The (R, G, B, A) background color of the text when an image is used for the button.",
    NULL},

    {"text_normal",
    (getter)PyUIButtonStyle_get_text_normal, 
    (setter)PyUIButtonStyle_set_text_normal,
    "The (R, G, B, A) color of button text when the button is in the default state.",
    NULL},

    {"text_hover",
    (getter)PyUIButtonStyle_get_text_hover, 
    (setter)PyUIButtonStyle_set_text_hover,
    "The (R, G, B, A) color of button text when the cursor is hovered over the button.",
    NULL},

    {"text_active",
    (getter)PyUIButtonStyle_get_text_active, 
    (setter)PyUIButtonStyle_set_text_active,
    "The (R, G, B, A) color of button text when the button is in the active state.", 
    NULL},

    {"text_alignment",
    (getter)PyUIButtonStyle_get_text_alignment, 
    (setter)PyUIButtonStyle_set_text_alignment,
    "A set of flags to control the text alignment of the button label.", 
    NULL},

    {"border",
    (getter)PyUIButtonStyle_get_border, 
    (setter)PyUIButtonStyle_set_border,
    "A floating-point value of the button border width, in pixels.", 
    NULL},

    {"rounding",
    (getter)PyUIButtonStyle_get_rounding, 
    (setter)PyUIButtonStyle_set_rounding,
    "A floating-point value to control how rounded the button corners are.", 
    NULL},

    {"padding",
    (getter)PyUIButtonStyle_get_padding, 
    (setter)PyUIButtonStyle_set_padding,
    "An (X, Y) tuple of floats to control the padding around buttons.", 
    NULL},

    {"image_padding",
    (getter)PyUIButtonStyle_get_image_padding, 
    (setter)PyUIButtonStyle_set_image_padding,
    "An (X, Y) tuple of floats to control the padding around images.", 
    NULL},

    {"touch_padding",
    (getter)PyUIButtonStyle_get_touch_padding, 
    (setter)PyUIButtonStyle_set_touch_padding,
    "An (X, Y) tuple of floats to control the clickable region of the button.", 
    NULL},

    {NULL}  /* Sentinel */
};

static PyTypeObject PyUIButtonStyle_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "pf.UIButtonStyle",        /* tp_name */
    sizeof(PyUIButtonStyleObject), /* tp_basicsize */
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
    Py_TPFLAGS_DEFAULT, /* tp_flags */
    "Style configuration for Permafrost Engine UI buttons.", /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    PyUIButtonStyle_methods, /* tp_methods */
    0,                         /* tp_members */
    PyUIButtonStyle_getset, /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    0,                         /* tp_init */
    0,                         /* tp_alloc */
    0,                         /* tp_new */
};

static PyMethodDef PyUIHeaderStyle_methods[] = {
    {"__pickle__", 
    (PyCFunction)PyUIHeaderStyle_pickle, METH_NOARGS,
    "Serialize a Permafrost Engine UIButtonStyle object to a string."},

    {"__unpickle__", 
    (PyCFunction)PyUIHeaderStyle_unpickle, METH_VARARGS | METH_CLASS,
    "Create a new pf.UIHeaderStyle instance from a string earlier returned from a __pickle__ method."
    "Returns a tuple of the new instance and the number of bytes consumed from the stream."},

    {NULL}  /* Sentinel */
};

static PyGetSetDef PyUIHeaderStyle_getset[] = {
    {"close_button",
    (getter)PyUIHeaderStyle_get_close_button, NULL,
    "A pf.UIButtonStyle object describing the style of the close button." ,
    NULL},

    {"minimize_button",
    (getter)PyUIHeaderStyle_get_minimize_button, NULL,
    "A pf.UIButtonStyle object describing the style of the minimize button." ,
    NULL},

    {"normal",
    (getter)PyUIHeaderStyle_get_normal, 
    (setter)PyUIHeaderStyle_set_normal,
    "The look of the button in the normal state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"hover",
    (getter)PyUIHeaderStyle_get_hover, 
    (setter)PyUIHeaderStyle_set_hover,
    "The look of the button when the mouse is hovered over it - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"active",
    (getter)PyUIHeaderStyle_get_active, 
    (setter)PyUIHeaderStyle_set_active,
    "The look of the button in the active (pressed) state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"label_normal",
    (getter)PyUIHeaderStyle_get_label_normal, 
    (setter)PyUIHeaderStyle_set_label_normal,
    "The (R, G, B, A) color of header label when the window is in the default state.",
    NULL},

    {"label_hover",
    (getter)PyUIHeaderStyle_get_label_hover, 
    (setter)PyUIHeaderStyle_set_label_hover,
    "The (R, G, B, A) color of header label when the cursor is hovered over the window.",
    NULL},

    {"label_active",
    (getter)PyUIHeaderStyle_get_label_active, 
    (setter)PyUIHeaderStyle_set_label_active,
    "The (R, G, B, A) color of header label when the window is in the active state.", 
    NULL},

    {NULL}  /* Sentinel */
};

static PyTypeObject PyUIHeaderStyle_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "pf.UIHeaderStyle",        /* tp_name */
    sizeof(PyUIHeaderStyleObject), /* tp_basicsize */
    0,                         /* tp_itemsize */
    (destructor)PyUIHeaderStyle_dealloc, /* tp_dealloc */
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
    Py_TPFLAGS_DEFAULT, /* tp_flags */
    "Style configuration for Permafrost Engine UI window headers.", /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    PyUIHeaderStyle_methods, /* tp_methods */
    0,                         /* tp_members */
    PyUIHeaderStyle_getset, /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    0,                         /* tp_init */
    0,                         /* tp_alloc */
    PyUIHeaderStyle_new,       /* tp_new */
};

static PyMethodDef PyUISelectableStyle_methods[] = {
    {"__pickle__", 
    (PyCFunction)PyUISelectableStyle_pickle, METH_NOARGS,
    "Serialize a Permafrost Engine UISelectableStyle object to a string."},

    {"__unpickle__", 
    (PyCFunction)PyUISelectableStyle_unpickle, METH_VARARGS | METH_CLASS,
    "Create a new pf.UISelectableStyle instance from a string earlier returned from a __pickle__ method."
    "Returns a tuple of the new instance and the number of bytes consumed from the stream."},

    {NULL}  /* Sentinel */
};

static PyGetSetDef PyUISelectableStyle_getset[] = {
    {"normal",
    (getter)PyUISelectableStyle_get_normal, 
    (setter)PyUISelectableStyle_set_normal,
    "The look of the selectable label in the normal (inactive) state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"hover",
    (getter)PyUISelectableStyle_get_hover, 
    (setter)PyUISelectableStyle_set_hover,
    "The look of the selectable label in the hovered (inactive) state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"pressed",
    (getter)PyUISelectableStyle_get_pressed, 
    (setter)PyUISelectableStyle_set_pressed,
    "The look of the selectable label in the pressed (inactive) state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"normal_active",
    (getter)PyUISelectableStyle_get_normal_active, 
    (setter)PyUISelectableStyle_set_normal_active,
    "The look of the selectable label in the normal (active) state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"hover_active",
    (getter)PyUISelectableStyle_get_hover_active, 
    (setter)PyUISelectableStyle_set_hover_active,
    "The look of the selectable label in the hovered (active) state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"pressed_active",
    (getter)PyUISelectableStyle_get_pressed_active,
    (setter)PyUISelectableStyle_set_pressed_active,
    "The look of the selectable label in the pressed (active) state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"text_normal",
    (getter)PyUISelectableStyle_get_text_normal, 
    (setter)PyUISelectableStyle_set_text_normal,
    "The color of the selectable label text in the normal (inactive) state - an (R, G, B, A) tuple.",
    NULL},

    {"text_hover",
    (getter)PyUISelectableStyle_get_text_hover, 
    (setter)PyUISelectableStyle_set_text_hover,
    "The color of the selectable label text in the hovered (inactive) state - an (R, G, B, A) tuple",
    NULL},

    {"text_pressed",
    (getter)PyUISelectableStyle_get_text_pressed, 
    (setter)PyUISelectableStyle_set_text_pressed,
    "The color of the selectable label text in the pressed (inactive) state - an (R, G, B, A) tuple",
    NULL},

    {"text_normal_active",
    (getter)PyUISelectableStyle_get_text_normal_active, 
    (setter)PyUISelectableStyle_set_text_normal_active,
    "The color of the selectable label text in the normal (active) state - an (R, G, B, A) tuple.",
    NULL},

    {"text_hover_active",
    (getter)PyUISelectableStyle_get_text_hover_active, 
    (setter)PyUISelectableStyle_set_text_hover_active,
    "The color of the selectable label text in the hovered (active) state - an (R, G, B, A) tuple",
    NULL},

    {"text_pressed_active",
    (getter)PyUISelectableStyle_get_text_pressed_active, 
    (setter)PyUISelectableStyle_set_text_pressed_active,
    "The color of the selectable label text in the pressed (active) state - an (R, G, B, A) tuple",
    NULL},

    {"text_alignment",
    (getter)PyUISelectableStyle_get_text_alignment, 
    (setter)PyUISelectableStyle_set_text_alignment,
    "The mode of text alignment (pf.NK_TEXT_CENTERED, etc.).",
    NULL},

    {"rounding",
    (getter)PyUISelectableStyle_get_rounding, 
    (setter)PyUISelectableStyle_set_rounding,
    "A floating-point value to control how rounded the selectable label corners are.", 
    NULL},

    {"padding",
    (getter)PyUISelectableStyle_get_padding, 
    (setter)PyUISelectableStyle_set_padding,
    "An (X, Y) tuple of floats to control the padding around selectable labels.", 
    NULL},

    {"image_padding",
    (getter)PyUISelectableStyle_get_image_padding, 
    (setter)PyUISelectableStyle_set_image_padding,
    "An (X, Y) tuple of floats to control the padding around images.", 
    NULL},

    {"touch_padding",
    (getter)PyUISelectableStyle_get_touch_padding, 
    (setter)PyUISelectableStyle_set_touch_padding,
    "An (X, Y) tuple of floats to control the clickable region of the selectable label.", 
    NULL},

    {NULL}  /* Sentinel */
};

static PyTypeObject PyUISelectableStyle_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "pf.UISelectableStyle",        /* tp_name */
    sizeof(PyUISelectableStyleObject), /* tp_basicsize */
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
    Py_TPFLAGS_DEFAULT, /* tp_flags */
    "Style configuration for Permafrost Engine selectable labels.", /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    PyUISelectableStyle_methods, /* tp_methods */
    0,                         /* tp_members */
    PyUISelectableStyle_getset, /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    0,                         /* tp_init */
    0,                         /* tp_alloc */
    0,                         /* tp_new */
};

static struct nk_style_window_header s_saved_header_style;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static int parse_float_pair(PyObject *tuple, float *out_a, float *out_b)
{
    if(!PyTuple_Check(tuple))
        return -1;

    PyObject *a = PyTuple_GetItem(tuple, 0);
    PyObject *b = PyTuple_GetItem(tuple, 1);

    if(!a || !b)
        return -1;

    if(!PyFloat_Check(a) || !PyFloat_Check(b))
        return -1;

    *out_a = PyFloat_AsDouble(a);
    *out_b = PyFloat_AsDouble(b);
    return 0;
}

static int parse_rgba(PyObject *tuple, float out[4])
{
    if(!PyTuple_Check(tuple))
        return -1;

    for(int i = 0; i < 4; i++) {
    
        PyObject *pynum = PyTuple_GetItem(tuple, i);
        if(!pynum)
            return -1;

        if(PyFloat_Check(pynum))
            out[i] = PyFloat_AsDouble(pynum);
        else if(PyInt_Check(pynum))
            out[i] = PyInt_AsLong(pynum);
        else 
            return -1;
    }

    return 0;
}

static PyObject *style_get_item(const struct nk_style_item *item)
{
    if(item->type == NK_STYLE_ITEM_COLOR) {
        return Py_BuildValue("(i,i,i,i)", 
            item->data.color.r,
            item->data.color.g,
            item->data.color.b,
            item->data.color.a);
    }else{

        return PyString_FromString(item->data.texpath);
    }
}

static int style_set_item(PyObject *value, struct nk_style_item *out)
{
    float rgba[4];

    if(parse_rgba(value, rgba) == 0) {

        out->type = NK_STYLE_ITEM_COLOR;
        out->data.color  = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
        return 0;

    }else if(PyString_Check(value)) {

        out->type = NK_STYLE_ITEM_TEXPATH;
        pf_strlcpy(out->data.texpath, PyString_AS_STRING(value),
            ARR_SIZE(out->data.texpath));
        return 0;

    }else{
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple or an image path.");
        return -1; 
    }
}

static PyObject *PyUIButtonStyle_get_normal(PyUIButtonStyleObject *self, void *closure)
{
    return style_get_item(&self->style->normal);
}

static int PyUIButtonStyle_set_normal(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->normal);
}

static PyObject *PyUIButtonStyle_get_hover(PyUIButtonStyleObject *self, void *closure)
{
    return style_get_item(&self->style->hover);
}

static int PyUIButtonStyle_set_hover(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->hover);
}

static PyObject *PyUIButtonStyle_get_active(PyUIButtonStyleObject *self, void *closure)
{
    return style_get_item(&self->style->active);
}

static int PyUIButtonStyle_set_active(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->active);
}

static PyObject *PyUIButtonStyle_get_border_color(PyUIButtonStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->border_color.r,
        self->style->border_color.g,
        self->style->border_color.b,
        self->style->border_color.a);
}

static int PyUIButtonStyle_set_border_color(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->border_color = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIButtonStyle_get_text_background(PyUIButtonStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->text_background.r,
        self->style->text_background.g,
        self->style->text_background.b,
        self->style->text_background.a);
}

static int PyUIButtonStyle_set_text_background(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->text_background = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIButtonStyle_get_text_normal(PyUIButtonStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->text_normal.r,
        self->style->text_normal.g,
        self->style->text_normal.b,
        self->style->text_normal.a);
}

static int PyUIButtonStyle_set_text_normal(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->text_normal = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIButtonStyle_get_text_hover(PyUIButtonStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->text_hover.r,
        self->style->text_hover.g,
        self->style->text_hover.b,
        self->style->text_hover.a);
}

static int PyUIButtonStyle_set_text_hover(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->text_hover = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIButtonStyle_get_text_active(PyUIButtonStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->text_active.r,
        self->style->text_active.g,
        self->style->text_active.b,
        self->style->text_active.a);
}

static int PyUIButtonStyle_set_text_active(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->text_active = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIButtonStyle_get_text_alignment(PyUIButtonStyleObject *self, void *closure)
{
    return Py_BuildValue("I", self->style->text_alignment);
}

static int PyUIButtonStyle_set_text_alignment(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    if(!PyInt_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be an integer.");
        return -1; 
    }

    self->style->text_alignment = PyInt_AsLong(value);
    return 0;
}

static PyObject *PyUIButtonStyle_get_border(PyUIButtonStyleObject *self, void *closure)
{
    return Py_BuildValue("f", self->style->border);
}

static int PyUIButtonStyle_set_border(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be a float.");
        return -1; 
    }

    self->style->border = PyFloat_AsDouble(value);
    return 0;
}

static PyObject *PyUIButtonStyle_get_rounding(PyUIButtonStyleObject *self, void *closure)
{
    return Py_BuildValue("f", self->style->rounding);
}

static int PyUIButtonStyle_set_rounding(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be a float.");
        return -1; 
    }

    self->style->rounding = PyFloat_AsDouble(value);
    return 0;
}

static PyObject *PyUIButtonStyle_get_padding(PyUIButtonStyleObject *self, void *closure)
{
    return Py_BuildValue("(f,f)", 
        self->style->padding.x,
        self->style->padding.y);
}

static int PyUIButtonStyle_set_padding(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style->padding = (struct nk_vec2){x, y};
    return 0;
}

static PyObject *PyUIButtonStyle_get_image_padding(PyUIButtonStyleObject *self, void *closure)
{
    return Py_BuildValue("(f,f)", 
        self->style->image_padding.x,
        self->style->image_padding.y);
}

static int PyUIButtonStyle_set_image_padding(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style->image_padding = (struct nk_vec2){x, y};
    return 0;
}

static PyObject *PyUIButtonStyle_get_touch_padding(PyUIButtonStyleObject *self, void *closure)
{
    return Py_BuildValue("(f,f)", 
        self->style->touch_padding.x,
        self->style->touch_padding.y);
}

static int PyUIButtonStyle_set_touch_padding(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style->touch_padding = (struct nk_vec2){x, y};
    return 0;
}

static bool save_color(struct SDL_RWops *stream, struct nk_color clr)
{
    PyObject *clrobj = Py_BuildValue("iiii", clr.r, clr.g, clr.g, clr.a);
    CHK_TRUE_RET(clrobj);
    bool ret = S_PickleObjgraph(clrobj, stream);
    Py_DECREF(clrobj);
    return ret;
}

static bool load_color(struct SDL_RWops *stream, struct nk_color *out)
{
    PyObject *obj = S_UnpickleObjgraph(stream);
    CHK_TRUE_RET(obj);
    if(!PyArg_ParseTuple(obj, "iiii", &out->r, &out->g, &out->b, &out->a)) {
        Py_DECREF(obj);
        return false;
    }
    Py_DECREF(obj);

    char tmp[1];
    SDL_RWread(stream, tmp, 1, 1);
    return true;
}

static bool save_float(struct SDL_RWops *stream, float flt)
{
    PyObject *fltobj = PyFloat_FromDouble(flt);
    CHK_TRUE_RET(fltobj);
    bool ret = S_PickleObjgraph(fltobj, stream);
    Py_DECREF(fltobj);
    return ret;
}

static bool load_float(struct SDL_RWops *stream, float *out)
{
    PyObject *obj = S_UnpickleObjgraph(stream);
    CHK_TRUE_RET(obj);
    if(!PyFloat_Check(obj)) {
        Py_DECREF(obj);
        return false;
    }
    *out = PyFloat_AsDouble(obj);
    Py_DECREF(obj);

    char tmp[1];
    SDL_RWread(stream, tmp, 1, 1);
    return true;
}

static bool save_int(struct SDL_RWops *stream, int integer)
{
    PyObject *integerobj = PyInt_FromLong(integer);
    CHK_TRUE_RET(integerobj);
    bool ret = S_PickleObjgraph(integerobj, stream);
    Py_DECREF(integerobj);
    return ret;
}

static bool load_int(struct SDL_RWops *stream, int *out)
{
    PyObject *obj = S_UnpickleObjgraph(stream);
    CHK_TRUE_RET(obj);
    if(!PyInt_Check(obj)) {
        Py_DECREF(obj);
        return false;
    }
    *out = PyInt_AS_LONG(obj);
    Py_DECREF(obj);

    char tmp[1];
    SDL_RWread(stream, tmp, 1, 1);
    return true;
}

static bool save_vec2(struct SDL_RWops *stream, struct nk_vec2 vec)
{
    PyObject *vecobj = Py_BuildValue("ff", vec.x, vec.y);
    CHK_TRUE_RET(vecobj);
    bool ret = S_PickleObjgraph(vecobj, stream);
    Py_DECREF(vecobj);
    return ret;
}

static bool load_vec2(struct SDL_RWops *stream, struct nk_vec2 *out)
{
    PyObject *obj = S_UnpickleObjgraph(stream);
    CHK_TRUE_RET(obj);
    if(!PyArg_ParseTuple(obj, "ff", &out->x, &out->y)) {
        Py_DECREF(obj);
        return false;
    }
    Py_DECREF(obj);

    char tmp[1];
    SDL_RWread(stream, tmp, 1, 1);
    return true;
}

bool save_item(struct SDL_RWops *stream, const struct nk_style_item *item)
{
    assert(item->type == NK_STYLE_ITEM_COLOR 
        || item->type == NK_STYLE_ITEM_TEXPATH);

    PyObject *val = item->type == NK_STYLE_ITEM_COLOR
        ? Py_BuildValue("iiii", item->data.color.r, item->data.color.g, item->data.color.b, item->data.color.a)
        : PyString_FromString(item->data.texpath);

    PyObject *pickle = Py_BuildValue("iO", item->type, val);
    Py_DECREF(val);

    bool ret = S_PickleObjgraph(pickle, stream);
    Py_DECREF(pickle);
    return ret;
}

bool load_item(struct SDL_RWops *stream, struct nk_style_item *out)
{
    PyObject *obj = S_UnpickleObjgraph(stream);
    if(!obj) {
        return false;
    }

    int type;
    PyObject *val; /* borrowed */

    if(!PyTuple_Check(obj)
    || !PyArg_ParseTuple(obj, "iO", &type, &val)) {
        Py_DECREF(obj);
        return false;
    }

    if(type != NK_STYLE_ITEM_COLOR
    && type != NK_STYLE_ITEM_TEXPATH) {
        Py_DECREF(obj);
        return false;
    }

    if(type == NK_STYLE_ITEM_COLOR
    && !PyArg_ParseTuple(val, "iiii", 
       &out->data.color.r, &out->data.color.g, &out->data.color.b, &out->data.color.a)) {
        Py_DECREF(obj);
        return false;
    }

    const char *str;
    if(type == NK_STYLE_ITEM_TEXPATH) {
        if(!PyString_Check(val)) {
            Py_DECREF(obj);
            return false;
        }
        pf_snprintf(out->data.texpath, sizeof(out->data.texpath), "%s", PyString_AS_STRING(val));
    }

    out->type = type;
    Py_DECREF(obj);

    char tmp[1];
    SDL_RWread(stream, tmp, 1, 1); /* consume NULL byte */
    return true;
}

bool save_button(struct SDL_RWops *stream, const struct nk_style_button *button)
{
    CHK_TRUE_RET(save_item(stream, &button->normal));
    CHK_TRUE_RET(save_item(stream, &button->hover));
    CHK_TRUE_RET(save_item(stream, &button->active));

    CHK_TRUE_RET(save_color(stream, button->border_color));
    CHK_TRUE_RET(save_color(stream, button->text_normal));
    CHK_TRUE_RET(save_color(stream, button->text_hover));
    CHK_TRUE_RET(save_color(stream, button->text_active));

    CHK_TRUE_RET(save_int(stream, button->text_alignment));
    CHK_TRUE_RET(save_float(stream, button->border));
    CHK_TRUE_RET(save_float(stream, button->rounding));
    CHK_TRUE_RET(save_vec2(stream, button->padding));
    CHK_TRUE_RET(save_vec2(stream, button->image_padding));
    CHK_TRUE_RET(save_vec2(stream, button->touch_padding));

    return true;
}

bool load_button(struct SDL_RWops *stream, struct nk_style_button *out)
{
    CHK_TRUE_RET(load_item(stream, &out->normal));
    CHK_TRUE_RET(load_item(stream, &out->hover));
    CHK_TRUE_RET(load_item(stream, &out->active));

    CHK_TRUE_RET(load_color(stream, &out->border_color));
    CHK_TRUE_RET(load_color(stream, &out->text_normal));
    CHK_TRUE_RET(load_color(stream, &out->text_hover));
    CHK_TRUE_RET(load_color(stream, &out->text_active));

    CHK_TRUE_RET(load_int(stream, (int*)&out->text_alignment));
    CHK_TRUE_RET(load_float(stream, &out->border));
    CHK_TRUE_RET(load_float(stream, &out->rounding));
    CHK_TRUE_RET(load_vec2(stream, &out->padding));
    CHK_TRUE_RET(load_vec2(stream, &out->image_padding));
    CHK_TRUE_RET(load_vec2(stream, &out->touch_padding));

    return true;
}

bool save_selectable(struct SDL_RWops *stream, const struct nk_style_selectable *selectable)
{
    CHK_TRUE_RET(save_item(stream, &selectable->normal));
    CHK_TRUE_RET(save_item(stream, &selectable->hover));
    CHK_TRUE_RET(save_item(stream, &selectable->pressed));

    CHK_TRUE_RET(save_item(stream, &selectable->normal_active));
    CHK_TRUE_RET(save_item(stream, &selectable->hover_active));
    CHK_TRUE_RET(save_item(stream, &selectable->pressed_active));

    CHK_TRUE_RET(save_color(stream, selectable->text_normal));
    CHK_TRUE_RET(save_color(stream, selectable->text_hover));
    CHK_TRUE_RET(save_color(stream, selectable->text_pressed));

    CHK_TRUE_RET(save_color(stream, selectable->text_normal_active));
    CHK_TRUE_RET(save_color(stream, selectable->text_hover_active));
    CHK_TRUE_RET(save_color(stream, selectable->text_pressed_active));

    CHK_TRUE_RET(save_int(stream, selectable->text_alignment));
    CHK_TRUE_RET(save_float(stream, selectable->rounding));
    CHK_TRUE_RET(save_vec2(stream, selectable->padding));
    CHK_TRUE_RET(save_vec2(stream, selectable->image_padding));
    CHK_TRUE_RET(save_vec2(stream, selectable->touch_padding));

    return true;
}

bool load_selectable(struct SDL_RWops *stream, struct nk_style_selectable *out)
{
    CHK_TRUE_RET(load_item(stream, &out->normal));
    CHK_TRUE_RET(load_item(stream, &out->hover));
    CHK_TRUE_RET(load_item(stream, &out->pressed));

    CHK_TRUE_RET(load_item(stream, &out->normal_active));
    CHK_TRUE_RET(load_item(stream, &out->hover_active));
    CHK_TRUE_RET(load_item(stream, &out->pressed_active));

    CHK_TRUE_RET(load_color(stream, &out->text_normal));
    CHK_TRUE_RET(load_color(stream, &out->text_hover));
    CHK_TRUE_RET(load_color(stream, &out->text_pressed));

    CHK_TRUE_RET(load_color(stream, &out->text_normal_active));
    CHK_TRUE_RET(load_color(stream, &out->text_hover_active));
    CHK_TRUE_RET(load_color(stream, &out->text_pressed_active));

    CHK_TRUE_RET(load_int(stream, (int*)&out->text_alignment));
    CHK_TRUE_RET(load_float(stream, &out->rounding));
    CHK_TRUE_RET(load_vec2(stream, &out->padding));
    CHK_TRUE_RET(load_vec2(stream, &out->image_padding));
    CHK_TRUE_RET(load_vec2(stream, &out->touch_padding));

    return true;
}

static bool save_header(struct SDL_RWops *stream, const struct nk_style_window_header *header)
{
    CHK_TRUE_RET(save_item(stream, &header->normal));
    CHK_TRUE_RET(save_item(stream, &header->hover));
    CHK_TRUE_RET(save_item(stream, &header->active));

    CHK_TRUE_RET(save_button(stream, &header->close_button));
    CHK_TRUE_RET(save_button(stream, &header->minimize_button));

    CHK_TRUE_RET(save_int(stream, header->close_symbol));
    CHK_TRUE_RET(save_int(stream, header->minimize_symbol));
    CHK_TRUE_RET(save_int(stream, header->maximize_symbol));

    CHK_TRUE_RET(save_color(stream, header->label_normal));
    CHK_TRUE_RET(save_color(stream, header->label_hover));
    CHK_TRUE_RET(save_color(stream, header->label_active));

    CHK_TRUE_RET(save_int(stream, header->align));

    CHK_TRUE_RET(save_vec2(stream, header->padding));
    CHK_TRUE_RET(save_vec2(stream, header->label_padding));
    CHK_TRUE_RET(save_vec2(stream, header->spacing));

    return true;
}

static bool load_header(struct SDL_RWops *stream, struct nk_style_window_header *out)
{
    CHK_TRUE_RET(load_item(stream, &out->normal));
    CHK_TRUE_RET(load_item(stream, &out->hover));
    CHK_TRUE_RET(load_item(stream, &out->active));

    CHK_TRUE_RET(load_button(stream, &out->close_button));
    CHK_TRUE_RET(load_button(stream, &out->minimize_button));

    CHK_TRUE_RET(load_int(stream, (int*)&out->close_symbol));
    CHK_TRUE_RET(load_int(stream, (int*)&out->minimize_symbol));
    CHK_TRUE_RET(load_int(stream, (int*)&out->maximize_symbol));

    CHK_TRUE_RET(load_color(stream, &out->label_normal));
    CHK_TRUE_RET(load_color(stream, &out->label_hover));
    CHK_TRUE_RET(load_color(stream, &out->label_active));

    CHK_TRUE_RET(load_int(stream, (int*)&out->align));

    CHK_TRUE_RET(load_vec2(stream, &out->padding));
    CHK_TRUE_RET(load_vec2(stream, &out->label_padding));
    CHK_TRUE_RET(load_vec2(stream, &out->spacing));

    return true;
}

static PyObject *PyUIButtonStyle_pickle(PyUIButtonStyleObject *self)
{
    PyObject *ret = NULL;

    SDL_RWops *stream = PFSDL_VectorRWOps();
    CHK_TRUE(stream, fail_alloc);
    CHK_TRUE(save_button(stream, self->style), fail_pickle);
    ret = PyString_FromStringAndSize(PFSDL_VectorRWOpsRaw(stream), SDL_RWsize(stream));

fail_pickle:
    SDL_RWclose(stream);
fail_alloc:
    if(!ret) {
        PyErr_SetString(PyExc_RuntimeError, "Error pickling pf.UIButtonStyle object");
    }
    return ret;
}

static PyObject *PyUIButtonStyle_unpickle(PyObject *cls, PyObject *args)
{
    PyObject *ret = NULL;
    const char *str;
    Py_ssize_t len;
    int status;
    char tmp;

    if(!PyArg_ParseTuple(args, "s#", &str, &len)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a single string.");
        goto fail_args;
    }

    SDL_RWops *stream = SDL_RWFromConstMem(str, len);
    CHK_TRUE(stream, fail_args);

    PyObject *styleobj = PyObject_New(PyObject, &PyUIButtonStyle_type);
    assert(styleobj || PyErr_Occurred());
    CHK_TRUE(styleobj, fail_unpickle);

    struct nk_context *ctx = UI_GetContext();
    ((PyUIButtonStyleObject*)styleobj)->style = &ctx->style.button;

    CHK_TRUE(load_button(stream, ((PyUIButtonStyleObject*)styleobj)->style), fail_unpickle);

    Py_ssize_t nread = SDL_RWseek(stream, 0, RW_SEEK_CUR);
    ret = Py_BuildValue("(Oi)", styleobj, (int)nread);
    Py_DECREF(styleobj);

fail_unpickle:
    SDL_RWclose(stream);
fail_args:
    if(!ret) {
        PyErr_SetString(PyExc_RuntimeError, "Error unpickling pf.UIButtonStyle object");
    }
    return ret;
}

static PyObject *PyUIHeaderStyle_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyUIHeaderStyleObject *self = (PyUIHeaderStyleObject*)type->tp_alloc(type, 0);
    struct nk_style_button *button;
    if(!self)
        return NULL;

    struct nk_context ctx;
    nk_style_default(&ctx);
    self->style = ctx.style.window.header;

    self->close_button = PyObject_New(PyUIButtonStyleObject, &PyUIButtonStyle_type);
    if(!self->close_button) {
        goto fail_close_button;
    }
    self->close_button->style = &self->style.close_button;

    self->minimize_button = PyObject_New(PyUIButtonStyleObject, &PyUIButtonStyle_type);
    if(!self->minimize_button) {
        goto fail_minimize_button;
    }
    self->minimize_button->style = &self->style.minimize_button;

    return (PyObject*)self;

fail_minimize_button:
    Py_DECREF(self->close_button);
fail_close_button:
    Py_DECREF(self);
    return NULL;
}

static void PyUIHeaderStyle_dealloc(PyUIHeaderStyleObject *self)
{
    Py_DECREF(self->close_button);
    Py_DECREF(self->minimize_button);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject *PyUIHeaderStyle_get_close_button(PyUIHeaderStyleObject *self, void *closure)
{
    Py_INCREF(self->close_button);
    return (PyObject*)self->close_button;
}

static PyObject *PyUIHeaderStyle_get_minimize_button(PyUIHeaderStyleObject *self, void *closure)
{
    Py_INCREF(self->minimize_button);
    return (PyObject*)self->minimize_button;
}

static PyObject *PyUIHeaderStyle_get_normal(PyUIHeaderStyleObject *self, void *closure)
{
    return style_get_item(&self->style.normal);
}

static int PyUIHeaderStyle_set_normal(PyUIHeaderStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style.normal);
}

static PyObject *PyUIHeaderStyle_get_hover(PyUIHeaderStyleObject *self, void *closure)
{
    return style_get_item(&self->style.hover);
}

static int PyUIHeaderStyle_set_hover(PyUIHeaderStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style.hover);
}

static PyObject *PyUIHeaderStyle_get_active(PyUIHeaderStyleObject *self, void *closure)
{
    return style_get_item(&self->style.active);
}

static int PyUIHeaderStyle_set_active(PyUIHeaderStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style.active);
}

static PyObject *PyUIHeaderStyle_get_label_normal(PyUIHeaderStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style.label_normal.r,
        self->style.label_normal.g,
        self->style.label_normal.b,
        self->style.label_normal.a);
}

static int PyUIHeaderStyle_set_label_normal(PyUIHeaderStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style.label_normal = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIHeaderStyle_get_label_hover(PyUIHeaderStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style.label_hover.r,
        self->style.label_hover.g,
        self->style.label_hover.b,
        self->style.label_hover.a);
}

static int PyUIHeaderStyle_set_label_hover(PyUIHeaderStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style.label_hover = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIHeaderStyle_get_label_active(PyUIHeaderStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style.label_active.r,
        self->style.label_active.g,
        self->style.label_active.b,
        self->style.label_active.a);
}

static int PyUIHeaderStyle_set_label_active(PyUIHeaderStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style.label_active = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIHeaderStyle_pickle(PyUIHeaderStyleObject *self)
{
    PyObject *ret = NULL;

    SDL_RWops *stream = PFSDL_VectorRWOps();
    CHK_TRUE(stream, fail_alloc);
    CHK_TRUE(save_header(stream, &self->style), fail_pickle);
    ret = PyString_FromStringAndSize(PFSDL_VectorRWOpsRaw(stream), SDL_RWsize(stream));

fail_pickle:
    SDL_RWclose(stream);
fail_alloc:
    if(!ret) {
        PyErr_SetString(PyExc_RuntimeError, "Error pickling pf.UIHeaderStyle object");
    }
    return ret;
}

static PyObject *PyUIHeaderStyle_unpickle(PyObject *cls, PyObject *args)
{
    PyObject *ret = NULL;
    const char *str;
    Py_ssize_t len;
    int status;
    char tmp;

    if(!PyArg_ParseTuple(args, "s#", &str, &len)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a single string.");
        goto fail_args;
    }

    SDL_RWops *stream = SDL_RWFromConstMem(str, len);
    CHK_TRUE(stream, fail_args);

    PyObject *styleobj = PyObject_CallFunctionObjArgs((PyObject*)&PyUIHeaderStyle_type, NULL);
    assert(styleobj || PyErr_Occurred());
    CHK_TRUE(styleobj, fail_unpickle);

    CHK_TRUE(load_header(stream, &((PyUIHeaderStyleObject*)styleobj)->style), fail_unpickle);

    Py_ssize_t nread = SDL_RWseek(stream, 0, RW_SEEK_CUR);
    ret = Py_BuildValue("(Oi)", styleobj, (int)nread);
    Py_DECREF(styleobj);

fail_unpickle:
    SDL_RWclose(stream);
fail_args:
    if(!ret) {
        PyErr_SetString(PyExc_RuntimeError, "Error unpickling pf.UIHeaderStyle object");
    }
    return ret;
}

static PyObject *PyUISelectableStyle_get_normal(PyUISelectableStyleObject *self, void *closure)
{
    return style_get_item(&self->style->normal);
}

static int PyUISelectableStyle_set_normal(PyUISelectableStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->normal);
}

static PyObject *PyUISelectableStyle_get_hover(PyUISelectableStyleObject *self, void *closure)
{
    return style_get_item(&self->style->hover);
}

static int PyUISelectableStyle_set_hover(PyUISelectableStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->hover);
}

static PyObject *PyUISelectableStyle_get_pressed(PyUISelectableStyleObject *self, void *closure)
{
    return style_get_item(&self->style->pressed);
}

static int PyUISelectableStyle_set_pressed(PyUISelectableStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->pressed);
}

static PyObject *PyUISelectableStyle_get_normal_active(PyUISelectableStyleObject *self, void *closure)
{
    return style_get_item(&self->style->normal_active);
}

static int PyUISelectableStyle_set_normal_active(PyUISelectableStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->normal_active);
}

static PyObject *PyUISelectableStyle_get_hover_active(PyUISelectableStyleObject *self, void *closure)
{
    return style_get_item(&self->style->hover_active);
}

static int PyUISelectableStyle_set_hover_active(PyUISelectableStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->hover_active);
}

static PyObject *PyUISelectableStyle_get_pressed_active(PyUISelectableStyleObject *self, void *closure)
{
    return style_get_item(&self->style->pressed_active);
}

static int PyUISelectableStyle_set_pressed_active(PyUISelectableStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->pressed_active);
}

static PyObject *PyUISelectableStyle_get_text_normal(PyUISelectableStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->text_normal.r,
        self->style->text_normal.g,
        self->style->text_normal.b,
        self->style->text_normal.a);
}

static int PyUISelectableStyle_set_text_normal(PyUISelectableStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->text_normal = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUISelectableStyle_get_text_hover(PyUISelectableStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->text_hover.r,
        self->style->text_hover.g,
        self->style->text_hover.b,
        self->style->text_hover.a);
}

static int PyUISelectableStyle_set_text_hover(PyUISelectableStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->text_hover = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUISelectableStyle_get_text_pressed(PyUISelectableStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->text_pressed.r,
        self->style->text_pressed.g,
        self->style->text_pressed.b,
        self->style->text_pressed.a);
}

static int PyUISelectableStyle_set_text_pressed(PyUISelectableStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->text_pressed = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUISelectableStyle_get_text_normal_active(PyUISelectableStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->text_normal_active.r,
        self->style->text_normal_active.g,
        self->style->text_normal_active.b,
        self->style->text_normal_active.a);
}

static int PyUISelectableStyle_set_text_normal_active(PyUISelectableStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->text_normal_active = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUISelectableStyle_get_text_hover_active(PyUISelectableStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->text_hover_active.r,
        self->style->text_hover_active.g,
        self->style->text_hover_active.b,
        self->style->text_hover_active.a);
}

static int PyUISelectableStyle_set_text_hover_active(PyUISelectableStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->text_hover_active = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUISelectableStyle_get_text_pressed_active(PyUISelectableStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->text_pressed_active.r,
        self->style->text_pressed_active.g,
        self->style->text_pressed_active.b,
        self->style->text_pressed_active.a);
}

static int PyUISelectableStyle_set_text_pressed_active(PyUISelectableStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->text_pressed_active = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUISelectableStyle_get_text_alignment(PyUISelectableStyleObject *self, void *closure)
{
    return PyInt_FromLong(self->style->text_alignment);
}

static int PyUISelectableStyle_set_text_alignment(PyUISelectableStyleObject *self, PyObject *value, void *closure)
{
    if(!PyInt_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be an integer.");
        return -1; 
    }

    self->style->text_alignment = PyInt_AsLong(value);
    return 0;
}

static PyObject *PyUISelectableStyle_get_rounding(PyUISelectableStyleObject *self, void *closure)
{
    return Py_BuildValue("f", self->style->rounding);
}

static int PyUISelectableStyle_set_rounding(PyUISelectableStyleObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be a float.");
        return -1; 
    }

    self->style->rounding = PyFloat_AsDouble(value);
    return 0;
}

static PyObject *PyUISelectableStyle_get_padding(PyUISelectableStyleObject *self, void *closure)
{
    return Py_BuildValue("(f,f)", 
        self->style->padding.x,
        self->style->padding.y);
}

static int PyUISelectableStyle_set_padding(PyUISelectableStyleObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style->padding = (struct nk_vec2){x, y};
    return 0;
}

static PyObject *PyUISelectableStyle_get_image_padding(PyUISelectableStyleObject *self, void *closure)\
{
    return Py_BuildValue("(f,f)", 
        self->style->touch_padding.x,
        self->style->touch_padding.y);
}

static int PyUISelectableStyle_set_image_padding(PyUISelectableStyleObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style->touch_padding = (struct nk_vec2){x, y};
    return 0;
}

static PyObject *PyUISelectableStyle_get_touch_padding(PyUISelectableStyleObject *self, void *closure)
{
    return Py_BuildValue("(f,f)", 
        self->style->image_padding.x,
        self->style->image_padding.y);
}

static int PyUISelectableStyle_set_touch_padding(PyUISelectableStyleObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style->image_padding = (struct nk_vec2){x, y};
    return 0;
}

static PyObject *PyUISelectableStyle_pickle(PyUISelectableStyleObject *self)
{
    PyObject *ret = NULL;

    SDL_RWops *stream = PFSDL_VectorRWOps();
    CHK_TRUE(stream, fail_alloc);
    CHK_TRUE(save_selectable(stream, self->style), fail_pickle);
    ret = PyString_FromStringAndSize(PFSDL_VectorRWOpsRaw(stream), SDL_RWsize(stream));

fail_pickle:
    SDL_RWclose(stream);
fail_alloc:
    if(!ret) {
        PyErr_SetString(PyExc_RuntimeError, "Error pickling pf.UISelectableStyle object");
    }
    return ret;
}

static PyObject *PyUISelectableStyle_unpickle(PyObject *cls, PyObject *args)
{
    PyObject *ret = NULL;
    const char *str;
    Py_ssize_t len;
    int status;
    char tmp;

    if(!PyArg_ParseTuple(args, "s#", &str, &len)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a single string.");
        goto fail_args;
    }

    SDL_RWops *stream = SDL_RWFromConstMem(str, len);
    CHK_TRUE(stream, fail_args);

    PyObject *styleobj = PyObject_New(PyObject, &PyUISelectableStyle_type);
    assert(styleobj || PyErr_Occurred());
    CHK_TRUE(styleobj, fail_unpickle);

    struct nk_context *ctx = UI_GetContext();
    ((PyUISelectableStyleObject*)styleobj)->style = &ctx->style.selectable;

    CHK_TRUE(load_selectable(stream, ((PyUISelectableStyleObject*)styleobj)->style), fail_unpickle);

    Py_ssize_t nread = SDL_RWseek(stream, 0, RW_SEEK_CUR);
    ret = Py_BuildValue("(Oi)", styleobj, (int)nread);
    Py_DECREF(styleobj);

fail_unpickle:
    SDL_RWclose(stream);
fail_args:
    if(!ret) {
        PyErr_SetString(PyExc_RuntimeError, "Error unpickling pf.UISelectableStyle object");
    }
    return ret;
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

void S_UI_Style_PyRegister(PyObject *module, struct nk_context *ctx)
{
    /* Button style */
    if(PyType_Ready(&PyUIButtonStyle_type) < 0)
        return;
    Py_INCREF(&PyUIButtonStyle_type);
    PyModule_AddObject(module, "UIButtonStyle", (PyObject*)&PyUIButtonStyle_type);

    /* Header style */
    if(PyType_Ready(&PyUIHeaderStyle_type) < 0)
        return;
    Py_INCREF(&PyUIHeaderStyle_type);
    PyModule_AddObject(module, "UIHeaderStyle", (PyObject*)&PyUIHeaderStyle_type);

    /* Selectable style */
    if(PyType_Ready(&PyUISelectableStyle_type) < 0)
        return;
    Py_INCREF(&PyUISelectableStyle_type);
    PyModule_AddObject(module, "UISelectableStyle", (PyObject*)&PyUISelectableStyle_type);

    /* Global style objects */
    PyUIButtonStyleObject *global_button_style = PyObject_New(PyUIButtonStyleObject, &PyUIButtonStyle_type);
    assert(global_button_style);
    global_button_style->style = &ctx->style.button;
    PyModule_AddObject(module, "button_style", (PyObject*)global_button_style);

    PyUISelectableStyleObject *global_sel_style = PyObject_New(PyUISelectableStyleObject, &PyUISelectableStyle_type);
    assert(global_sel_style);
    global_sel_style->style = &ctx->style.selectable;
    PyModule_AddObject(module, "selectable_style", (PyObject*)global_sel_style);
}

bool S_UI_Style_SaveWindow(struct SDL_RWops *stream, const struct nk_style_window *window)
{
    CHK_TRUE_RET(save_item(stream, &window->fixed_background));
    CHK_TRUE_RET(save_color(stream, window->background));

    CHK_TRUE_RET(save_color(stream, window->border_color));
    CHK_TRUE_RET(save_color(stream, window->popup_border_color));
    CHK_TRUE_RET(save_color(stream, window->combo_border_color));
    CHK_TRUE_RET(save_color(stream, window->contextual_border_color));
    CHK_TRUE_RET(save_color(stream, window->menu_border_color));
    CHK_TRUE_RET(save_color(stream, window->group_border_color));
    CHK_TRUE_RET(save_color(stream, window->tooltip_border_color));
    CHK_TRUE_RET(save_item(stream, &window->scaler));

    CHK_TRUE_RET(save_float(stream, window->border));
    CHK_TRUE_RET(save_float(stream, window->combo_border));
    CHK_TRUE_RET(save_float(stream, window->contextual_border));
    CHK_TRUE_RET(save_float(stream, window->menu_border));
    CHK_TRUE_RET(save_float(stream, window->group_border));
    CHK_TRUE_RET(save_float(stream, window->tooltip_border));
    CHK_TRUE_RET(save_float(stream, window->popup_border));
    CHK_TRUE_RET(save_float(stream, window->min_row_height_padding));

    CHK_TRUE_RET(save_float(stream, window->rounding));
    CHK_TRUE_RET(save_vec2(stream, window->spacing));
    CHK_TRUE_RET(save_vec2(stream, window->scrollbar_size));
    CHK_TRUE_RET(save_vec2(stream, window->min_size));

    CHK_TRUE_RET(save_vec2(stream, window->padding));
    CHK_TRUE_RET(save_vec2(stream, window->group_padding));
    CHK_TRUE_RET(save_vec2(stream, window->popup_padding));
    CHK_TRUE_RET(save_vec2(stream, window->combo_padding));
    CHK_TRUE_RET(save_vec2(stream, window->contextual_padding));
    CHK_TRUE_RET(save_vec2(stream, window->menu_padding));
    CHK_TRUE_RET(save_vec2(stream, window->tooltip_padding));

    return true;
}

bool S_UI_Style_LoadWindow(struct SDL_RWops *stream, struct nk_style_window *out)
{
    CHK_TRUE_RET(load_item(stream, &out->fixed_background));
    CHK_TRUE_RET(load_color(stream, &out->background));

    CHK_TRUE_RET(load_color(stream, &out->border_color));
    CHK_TRUE_RET(load_color(stream, &out->popup_border_color));
    CHK_TRUE_RET(load_color(stream, &out->combo_border_color));
    CHK_TRUE_RET(load_color(stream, &out->contextual_border_color));
    CHK_TRUE_RET(load_color(stream, &out->menu_border_color));
    CHK_TRUE_RET(load_color(stream, &out->group_border_color));
    CHK_TRUE_RET(load_color(stream, &out->tooltip_border_color));
    CHK_TRUE_RET(load_item(stream, &out->scaler));

    CHK_TRUE_RET(load_float(stream, &out->border));
    CHK_TRUE_RET(load_float(stream, &out->combo_border));
    CHK_TRUE_RET(load_float(stream, &out->contextual_border));
    CHK_TRUE_RET(load_float(stream, &out->menu_border));
    CHK_TRUE_RET(load_float(stream, &out->group_border));
    CHK_TRUE_RET(load_float(stream, &out->tooltip_border));
    CHK_TRUE_RET(load_float(stream, &out->popup_border));
    CHK_TRUE_RET(load_float(stream, &out->min_row_height_padding));

    CHK_TRUE_RET(load_float(stream, &out->rounding));
    CHK_TRUE_RET(load_vec2(stream, &out->spacing));
    CHK_TRUE_RET(load_vec2(stream, &out->scrollbar_size));
    CHK_TRUE_RET(load_vec2(stream, &out->min_size));

    CHK_TRUE_RET(load_vec2(stream, &out->padding));
    CHK_TRUE_RET(load_vec2(stream, &out->group_padding));
    CHK_TRUE_RET(load_vec2(stream, &out->popup_padding));
    CHK_TRUE_RET(load_vec2(stream, &out->combo_padding));
    CHK_TRUE_RET(load_vec2(stream, &out->contextual_padding));
    CHK_TRUE_RET(load_vec2(stream, &out->menu_padding));
    CHK_TRUE_RET(load_vec2(stream, &out->tooltip_padding));

    return true;
}

PyObject *S_UIHeaderStyleNew(void)
{
    return PyObject_CallFunctionObjArgs((PyObject*)&PyUIHeaderStyle_type, NULL);
}

size_t S_UIHeaderGetHeight(PyObject *obj, struct nk_context *ctx)
{
    assert(PyObject_IsInstance(obj, (PyObject*)&PyUIHeaderStyle_type));
    PyUIHeaderStyleObject *styleobj = (PyUIHeaderStyleObject*)obj;

    return ctx->style.font->height
         + 2 * styleobj->style.padding.y
         + 2 * styleobj->style.label_padding.y;
}

void S_UIHeaderStylePush(PyObject *obj, struct nk_context *ctx)
{
    assert(PyObject_IsInstance(obj, (PyObject*)&PyUIHeaderStyle_type));
    PyUIHeaderStyleObject *styleobj = (PyUIHeaderStyleObject*)obj;

    s_saved_header_style = ctx->style.window.header;
    ctx->style.window.header = styleobj->style;
}

void S_UIHeaderStylePop(PyObject *obj, struct nk_context *ctx)
{
    assert(PyObject_IsInstance(obj, (PyObject*)&PyUIHeaderStyle_type));
    PyUIHeaderStyleObject *styleobj = (PyUIHeaderStyleObject*)obj;

    styleobj->style = ctx->style.window.header;
    ctx->style.window.header = s_saved_header_style;
}

