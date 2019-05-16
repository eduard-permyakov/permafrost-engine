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

#include "ui_style_script.h"
#include "../lib/public/nuklear.h"
#include "../lib/public/khash.h"
#include "../lib/public/stb_image.h"
#include "../render/public/render.h"

#include <string.h>
#include <assert.h>


typedef struct {
    PyObject_HEAD
    struct nk_style_button *style;
}PyUIButtonStyleObject;

static PyObject *PyPf_UIButtonStyle_get_normal(PyUIButtonStyleObject *self, void *);
static int       PyPf_UIButtonStyle_set_normal(PyUIButtonStyleObject *self, PyObject *value, void *);
static PyObject *PyPf_UIButtonStyle_get_hover(PyUIButtonStyleObject *self, void *);
static int       PyPf_UIButtonStyle_set_hover(PyUIButtonStyleObject *self, PyObject *value, void *);
static PyObject *PyPf_UIButtonStyle_get_active(PyUIButtonStyleObject *self, void *);
static int       PyPf_UIButtonStyle_set_active(PyUIButtonStyleObject *self, PyObject *value, void *);
static PyObject *PyPf_UIButtonStyle_get_border_color(PyUIButtonStyleObject *self, void *);
static int       PyPf_UIButtonStyle_set_border_color(PyUIButtonStyleObject *self, PyObject *value, void *);
static PyObject *PyPf_UIButtonStyle_get_text_background(PyUIButtonStyleObject *self, void *);
static int       PyPf_UIButtonStyle_set_text_background(PyUIButtonStyleObject *self, PyObject *value, void *);
static PyObject *PyPf_UIButtonStyle_get_text_normal(PyUIButtonStyleObject *self, void *);
static int       PyPf_UIButtonStyle_set_text_normal(PyUIButtonStyleObject *self, PyObject *value, void *);
static PyObject *PyPf_UIButtonStyle_get_text_hover(PyUIButtonStyleObject *self, void *);
static int       PyPf_UIButtonStyle_set_text_hover(PyUIButtonStyleObject *self, PyObject *value, void *);
static PyObject *PyPf_UIButtonStyle_get_text_active(PyUIButtonStyleObject *self, void *);
static int       PyPf_UIButtonStyle_set_text_active(PyUIButtonStyleObject *self, PyObject *value, void *);
static PyObject *PyPf_UIButtonStyle_get_text_alignment(PyUIButtonStyleObject *self, void *);
static int       PyPf_UIButtonStyle_set_text_alignment(PyUIButtonStyleObject *self, PyObject *value, void *);
static PyObject *PyPf_UIButtonStyle_get_border(PyUIButtonStyleObject *self, void *);
static int       PyPf_UIButtonStyle_set_border(PyUIButtonStyleObject *self, PyObject *value, void *);
static PyObject *PyPf_UIButtonStyle_get_rounding(PyUIButtonStyleObject *self, void *);
static int       PyPf_UIButtonStyle_set_rounding(PyUIButtonStyleObject *self, PyObject *value, void *);
static PyObject *PyPf_UIButtonStyle_get_padding(PyUIButtonStyleObject *self, void *);
static int       PyPf_UIButtonStyle_set_padding(PyUIButtonStyleObject *self, PyObject *value, void *);
static PyObject *PyPf_UIButtonStyle_get_image_padding(PyUIButtonStyleObject *self, void *);
static int       PyPf_UIButtonStyle_set_image_padding(PyUIButtonStyleObject *self, PyObject *value, void *);
static PyObject *PyPf_UIButtonStyle_get_touch_padding(PyUIButtonStyleObject *self, void *);
static int       PyPf_UIButtonStyle_set_touch_padding(PyUIButtonStyleObject *self, PyObject *value, void *);

struct image_resource{
    char path[128];
};

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static PyGetSetDef PyPf_UIButtonStyle_getset[] = {
    {"normal",
    (getter)PyPf_UIButtonStyle_get_normal, 
    (setter)PyPf_UIButtonStyle_set_normal,
    "The look of the button in the normal state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"hover",
    (getter)PyPf_UIButtonStyle_get_hover, 
    (setter)PyPf_UIButtonStyle_set_hover,
    "The look of the button when the mouse is hovered over it - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"active",
    (getter)PyPf_UIButtonStyle_get_active, 
    (setter)PyPf_UIButtonStyle_set_active,
    "The look of the button in the active (pressed) state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"active",
    (getter)PyPf_UIButtonStyle_get_border_color, 
    (setter)PyPf_UIButtonStyle_set_border_color,
    "The (R, G, B, A) color of button borders.",
    NULL},

    {"text_background",
    (getter)PyPf_UIButtonStyle_get_text_background, 
    (setter)PyPf_UIButtonStyle_set_text_background,
    "The (R, G, B, A) background color of the text when an image is used for the button.",
    NULL},

    {"text_normal",
    (getter)PyPf_UIButtonStyle_get_text_normal, 
    (setter)PyPf_UIButtonStyle_set_text_normal,
    "The (R, G, B, A) color of button text when the button is in the default state.",
    NULL},

    {"text_hover",
    (getter)PyPf_UIButtonStyle_get_text_hover, 
    (setter)PyPf_UIButtonStyle_set_text_hover,
    "The (R, G, B, A) color of button text when the cursor is hovered over the button.",
    NULL},

    {"text_active",
    (getter)PyPf_UIButtonStyle_get_text_active, 
    (setter)PyPf_UIButtonStyle_set_text_active,
    "The (R, G, B, A) color of button text when the button is in the active state.", 
    NULL},

    {"text_alignment",
    (getter)PyPf_UIButtonStyle_get_text_alignment, 
    (setter)PyPf_UIButtonStyle_set_text_alignment,
    "A set of flags to control the text alignment of the button label.", 
    NULL},

    {"border",
    (getter)PyPf_UIButtonStyle_get_border, 
    (setter)PyPf_UIButtonStyle_set_border,
    "A floating-point value of the button border width, in pixels.", 
    NULL},

    {"rounding",
    (getter)PyPf_UIButtonStyle_get_rounding, 
    (setter)PyPf_UIButtonStyle_set_rounding,
    "A floating-point value to control how rounded the button corners are.", 
    NULL},

    {"padding",
    (getter)PyPf_UIButtonStyle_get_padding, 
    (setter)PyPf_UIButtonStyle_set_padding,
    "An (X, Y) tuple of floats to control the padding around buttons.", 
    NULL},

    {"image_padding",
    (getter)PyPf_UIButtonStyle_get_image_padding, 
    (setter)PyPf_UIButtonStyle_set_image_padding,
    "An (X, Y) tuple of floats to control the padding around images.", 
    NULL},

    {"touch_padding",
    (getter)PyPf_UIButtonStyle_get_touch_padding, 
    (setter)PyPf_UIButtonStyle_set_touch_padding,
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
    0,                         /* tp_methods */
    0,                         /* tp_members */
    PyPf_UIButtonStyle_getset, /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    0,                         /* tp_init */
    0,                         /* tp_alloc */
    0,                         /* tp_new */
};

KHASH_MAP_INIT_INT(image_res, struct image_resource)
khash_t(image_res) *s_id_path_table;

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

static bool image_load(const char *img_path, int *out_id)
{
    extern char *g_basepath;
    char path[512];
    char *name = NULL;

    if(strlen(img_path) + strlen(g_basepath) >= 128) {
        PyErr_SetString(PyExc_RuntimeError, "Image path too long.");
        return false;
    }
    strcpy(path, g_basepath);
    strcat(path, img_path);

    char *end = path + strlen(path) - 1;
    while(end > path && *end != '/')
        --end;
    if(end == path || end == path + strlen(path) - 1) {
        PyErr_SetString(PyExc_RuntimeError, "Malformed path for image.");
        return false;
    }
    name = end + 1;
    *end = '\0';

    stbi_set_flip_vertically_on_load(false);
    bool result = R_Texture_GetForName(name, out_id) || R_Texture_Load(path, name, out_id);
    stbi_set_flip_vertically_on_load(true);
    if(!result) {
        PyErr_SetString(PyExc_RuntimeError, "Not able to load image.");
        return false;
    }

    struct image_resource res;
    strcpy(res.path, img_path);

    int put_ret;
    khiter_t k = kh_put(image_res, s_id_path_table, *out_id, &put_ret);
    assert(put_ret != -1);
    kh_value(s_id_path_table, k) = res;

    return true;
}

static bool image_get_or_load(const char *img_path, int *out_id)
{
    int id;
    struct image_resource curr;
    kh_foreach(s_id_path_table, id, curr, {
    
        if(0 == strcmp(img_path, curr.path)) {
            *out_id = id;    
            return true;
        }
    });

    return image_load(img_path, out_id);
}

static PyObject *PyPf_UIButtonStyle_get_normal(PyUIButtonStyleObject *self, void *closure)
{
    if(self->style->normal.type == NK_STYLE_ITEM_COLOR) {
        return Py_BuildValue("(i,i,i,i)", 
            self->style->normal.data.color.r,
            self->style->normal.data.color.g,
            self->style->normal.data.color.b,
            self->style->normal.data.color.a);
    }else{

        khiter_t k = kh_get(image_res, s_id_path_table, self->style->normal.data.image.handle.id);
        assert(k != kh_end(s_id_path_table));
        return PyString_FromString(kh_value(s_id_path_table, k).path);
    }
}

static int PyPf_UIButtonStyle_set_normal(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) == 0) {

        self->style->normal.type = NK_STYLE_ITEM_COLOR;
        self->style->normal.data.color  = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
        return 0;

    }else if(PyString_Check(value)) {

        int id;
        if(!image_get_or_load(PyString_AS_STRING(value), &id))
            return -1; /* Error already set */

        self->style->normal.type = NK_STYLE_ITEM_IMAGE;
        self->style->normal.data.image = nk_image_id(id);
        return 0;

    }else{
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple or an image path.");
        return -1; 
    }
}

static PyObject *PyPf_UIButtonStyle_get_hover(PyUIButtonStyleObject *self, void *closure)
{
    if(self->style->hover.type == NK_STYLE_ITEM_COLOR) {
        return Py_BuildValue("(i,i,i,i)", 
            self->style->hover.data.color.r,
            self->style->hover.data.color.g,
            self->style->hover.data.color.b,
            self->style->hover.data.color.a);
    }else{

        khiter_t k = kh_get(image_res, s_id_path_table, self->style->hover.data.image.handle.id);
        assert(k != kh_end(s_id_path_table));
        return PyString_FromString(kh_value(s_id_path_table, k).path);
    }
}

static int PyPf_UIButtonStyle_set_hover(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) == 0) {

        self->style->hover.type = NK_STYLE_ITEM_COLOR;
        self->style->hover.data.color  = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
        return 0;

    }else if(PyString_Check(value)) {

        int id;
        if(!image_get_or_load(PyString_AS_STRING(value), &id))
            return -1; /* Error already set */

        self->style->hover.type = NK_STYLE_ITEM_IMAGE;
        self->style->hover.data.image = nk_image_id(id);
        return 0;

    }else{
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple or an image path.");
        return -1; 
    }
}

static PyObject *PyPf_UIButtonStyle_get_active(PyUIButtonStyleObject *self, void *closure)
{
    if(self->style->active.type == NK_STYLE_ITEM_COLOR) {
        return Py_BuildValue("(i,i,i,i)", 
            self->style->active.data.color.r,
            self->style->active.data.color.g,
            self->style->active.data.color.b,
            self->style->active.data.color.a);
    }else{

        khiter_t k = kh_get(image_res, s_id_path_table, self->style->active.data.image.handle.id);
        assert(k != kh_end(s_id_path_table));
        return PyString_FromString(kh_value(s_id_path_table, k).path);
    }
}

static int PyPf_UIButtonStyle_set_active(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) == 0) {

        self->style->active.type = NK_STYLE_ITEM_COLOR;
        self->style->active.data.color  = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
        return 0;

    }else if(PyString_Check(value)) {

        int id;
        if(!image_get_or_load(PyString_AS_STRING(value), &id))
            return -1; /* Error already set */

        self->style->active.type = NK_STYLE_ITEM_IMAGE;
        self->style->active.data.image = nk_image_id(id);
        return 0;

    }else{
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple or an image path.");
        return -1; 
    }
}

static PyObject *PyPf_UIButtonStyle_get_border_color(PyUIButtonStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->border_color.r,
        self->style->border_color.g,
        self->style->border_color.b,
        self->style->border_color.a);
}

static int PyPf_UIButtonStyle_set_border_color(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->border_color = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyPf_UIButtonStyle_get_text_background(PyUIButtonStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->text_background.r,
        self->style->text_background.g,
        self->style->text_background.b,
        self->style->text_background.a);
}

static int PyPf_UIButtonStyle_set_text_background(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->text_background = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyPf_UIButtonStyle_get_text_normal(PyUIButtonStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->text_normal.r,
        self->style->text_normal.g,
        self->style->text_normal.b,
        self->style->text_normal.a);
}

static int PyPf_UIButtonStyle_set_text_normal(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->text_normal = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyPf_UIButtonStyle_get_text_hover(PyUIButtonStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->text_hover.r,
        self->style->text_hover.g,
        self->style->text_hover.b,
        self->style->text_hover.a);
}

static int PyPf_UIButtonStyle_set_text_hover(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->text_hover = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyPf_UIButtonStyle_get_text_active(PyUIButtonStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->text_active.r,
        self->style->text_active.g,
        self->style->text_active.b,
        self->style->text_active.a);
}

static int PyPf_UIButtonStyle_set_text_active(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->text_active = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyPf_UIButtonStyle_get_text_alignment(PyUIButtonStyleObject *self, void *closure)
{
    return Py_BuildValue("I", self->style->text_alignment);
}

static int PyPf_UIButtonStyle_set_text_alignment(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    if(!PyInt_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be an unsigned integer.");
        return -1; 
    }

    self->style->text_alignment = PyInt_AsLong(value);
    return 0;
}

static PyObject *PyPf_UIButtonStyle_get_border(PyUIButtonStyleObject *self, void *closure)
{
    return Py_BuildValue("f", self->style->border);
}

static int PyPf_UIButtonStyle_set_border(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be a float.");
        return -1; 
    }

    self->style->border = PyFloat_AsDouble(value);
    return 0;
}

static PyObject *PyPf_UIButtonStyle_get_rounding(PyUIButtonStyleObject *self, void *closure)
{
    return Py_BuildValue("f", self->style->rounding);
}

static int PyPf_UIButtonStyle_set_rounding(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be a float.");
        return -1; 
    }

    self->style->rounding = PyFloat_AsDouble(value);
    return 0;
}

static PyObject *PyPf_UIButtonStyle_get_padding(PyUIButtonStyleObject *self, void *closure)
{
    return Py_BuildValue("(f,f)", 
        self->style->padding.x,
        self->style->padding.y);
}

static int PyPf_UIButtonStyle_set_padding(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style->padding = (struct nk_vec2){x, y};
    return 0;
}

static PyObject *PyPf_UIButtonStyle_get_image_padding(PyUIButtonStyleObject *self, void *closure)
{
    return Py_BuildValue("(f,f)", 
        self->style->image_padding.x,
        self->style->image_padding.y);
}

static int PyPf_UIButtonStyle_set_image_padding(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style->image_padding = (struct nk_vec2){x, y};
    return 0;
}

static PyObject *PyPf_UIButtonStyle_get_touch_padding(PyUIButtonStyleObject *self, void *closure)
{
    return Py_BuildValue("(f,f)", 
        self->style->touch_padding.x,
        self->style->touch_padding.y);
}

static int PyPf_UIButtonStyle_set_touch_padding(PyUIButtonStyleObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style->touch_padding = (struct nk_vec2){x, y};
    return 0;
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

    PyUIButtonStyleObject *global_button_style = PyObject_New(PyUIButtonStyleObject, &PyUIButtonStyle_type);
    assert(global_button_style);
    global_button_style->style = &ctx->style.button;

    int ret = PyObject_SetAttrString(module, "button_style", (PyObject*)global_button_style);
    assert(0 == ret);
}

bool S_UI_Style_Init(void)
{
    s_id_path_table = kh_init(image_res);
    return (s_id_path_table != NULL);
}

void S_UI_Style_Shutdown(void)
{
    kh_destroy(image_res, s_id_path_table);
}

