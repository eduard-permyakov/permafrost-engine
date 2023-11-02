/*
 *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2018-2023 Eduard Permyakov 
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
#include "py_ui_style.h"
#include "py_pickle.h"
#include "../lib/public/pf_nuklear.h"
#include "../lib/public/vec.h"
#include "../lib/public/mem.h"
#include "../lib/public/SDL_vec_rwops.h"
#include "../lib/public/pf_string.h"
#include "../lib/public/nk_file_browser.h"
#include "../game/public/game.h"
#include "../phys/public/collision.h"
#include "../event.h"
#include "../config.h"
#include "../main.h"
#include "../ui.h"

#include <assert.h>

#define TO_VEC2T(_nk_vec2i) ((vec2_t){_nk_vec2i.x, _nk_vec2i.y})
#define TO_VEC2I(_pf_vec2t) ((struct nk_vec2i){_pf_vec2t.x, _pf_vec2t.y})

#define CHK_TRUE(_pred, _label) do{ if(!(_pred)) goto _label; }while(0)
#define CHK_TRUE_RET(_pred)   \
    do{                       \
        if(!(_pred))          \
            return false;     \
    }while(0)

typedef struct {
    PyObject_HEAD
    char                    name[128];
    struct rect             rect; /* In virtual window coordinates */
    int                     flags;
    struct nk_style_window  style;
    PyObject               *header_style;
    int                     resize_mask;
    bool                    suspend_on_pause;
    /* The resolution for which the position and size of the window are 
     * defined. When the physical screen resolution changes to one that is
     * not equal to this window's virtual resolution, the window bounds
     * will be transformed according to the resize mask. */
    struct nk_vec2i         virt_res;
    bool                    hide;
}PyWindowObject;

VEC_TYPE(win, PyWindowObject*)
VEC_IMPL(static inline, win, PyWindowObject*)

static int       PyWindow_init(PyWindowObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyWindow_layout_row_static(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_layout_row_dynamic(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_layout_row(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_layout_row_begin(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_layout_row_end(PyWindowObject *self);
static PyObject *PyWindow_layout_row_push(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_label_colored(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_label_colored_wrap(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_button_label(PyWindowObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyWindow_simple_chart(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_selectable_label(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_option_label(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_edit_string(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_edit_focus(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_group(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_popup(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_popup_close(PyWindowObject *self);
static PyObject *PyWindow_tree(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_tree_element(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_selectable_symbol_label(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_combo_box(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_checkbox(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_color_picker(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_image(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_spacer(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_property_float(PyObject *self, PyObject *args);
static PyObject *PyWindow_property_int(PyObject *self, PyObject *args);
static PyObject *PyWindow_file_browser(PyWindowObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyWindow_slider_float(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_slider_int(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_progress(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_progress_text(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_text_lines(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_text_lines_width(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_show(PyWindowObject *self);
static PyObject *PyWindow_hide(PyWindowObject *self);
static PyObject *PyWindow_update(PyWindowObject *self);
static PyObject *PyWindow_on_hide(PyWindowObject *self, PyObject *args);
static PyObject *PyWindow_on_minimize(PyWindowObject *self);
static PyObject *PyWindow_on_maximize(PyWindowObject *self);
static PyObject *PyWindow_pickle(PyWindowObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyWindow_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs);
static PyObject *PyWindow_new(PyTypeObject *type, PyObject *args, PyObject *kwds);
static void      PyWindow_dealloc(PyWindowObject *self);

static PyObject *PyWindow_get_header(PyWindowObject *self, void *closure);
static PyObject *PyWindow_get_pos(PyWindowObject *self, void *closure);
static int       PyWindow_set_pos(PyWindowObject *self, PyObject *value, void *closure);
static PyObject *PyWindow_get_size(PyWindowObject *self, void *closure);
static PyObject *PyWindow_get_header_height(PyWindowObject *self, void *closure);

static PyObject *PyWindow_get_border_color(PyWindowObject *self, void *closure);
static int       PyWindow_set_border_color(PyWindowObject *self, PyObject *value, void *closure);
static PyObject *PyWindow_get_popup_border_color(PyWindowObject *self, void *closure);
static int       PyWindow_set_popup_border_color(PyWindowObject *self, PyObject *value, void *closure);
static PyObject *PyWindow_get_combo_border_color(PyWindowObject *self, void *closure);
static int       PyWindow_set_combo_border_color(PyWindowObject *self, PyObject *value, void *closure);
static PyObject *PyWindow_get_contextual_border_color(PyWindowObject *self, void *closure);
static int       PyWindow_set_contextual_border_color(PyWindowObject *self, PyObject *value, void *closure);
static PyObject *PyWindow_get_menu_border_color(PyWindowObject *self, void *closure);
static int       PyWindow_set_menu_border_color(PyWindowObject *self, PyObject *value, void *closure);
static PyObject *PyWindow_get_group_border_color(PyWindowObject *self, void *closure);
static int       PyWindow_set_group_border_color(PyWindowObject *self, PyObject *value, void *closure);
static PyObject *PyWindow_get_tooltip_border_color(PyWindowObject *self, void *closure);
static int       PyWindow_set_tooltip_border_color(PyWindowObject *self, PyObject *value, void *closure);

static PyObject *PyWindow_get_border(PyWindowObject *self, void *closure);
static int       PyWindow_set_border(PyWindowObject *self, PyObject *value, void *closure);
static PyObject *PyWindow_get_combo_border(PyWindowObject *self, void *closure);
static int       PyWindow_set_combo_border(PyWindowObject *self, PyObject *value, void *closure);
static PyObject *PyWindow_get_contextual_border(PyWindowObject *self, void *closure);
static int       PyWindow_set_contextual_border(PyWindowObject *self, PyObject *value, void *closure);
static PyObject *PyWindow_get_menu_border(PyWindowObject *self, void *closure);
static int       PyWindow_set_menu_border(PyWindowObject *self, PyObject *value, void *closure);
static PyObject *PyWindow_get_group_border(PyWindowObject *self, void *closure);
static int       PyWindow_set_group_border(PyWindowObject *self, PyObject *value, void *closure);
static PyObject *PyWindow_get_tooltip_border(PyWindowObject *self, void *closure);
static int       PyWindow_set_tooltip_border(PyWindowObject *self, PyObject *value, void *closure);
static PyObject *PyWindow_get_popup_border(PyWindowObject *self, void *closure);
static int       PyWindow_set_popup_border(PyWindowObject *self, PyObject *value, void *closure);
static PyObject *PyWindow_get_min_row_height_padding(PyWindowObject *self, void *closure);
static int       PyWindow_set_min_row_height_padding(PyWindowObject *self, PyObject *value, void *closure);

static PyObject *PyWindow_get_rounding(PyWindowObject *self, void *closure);
static int       PyWindow_set_rounding(PyWindowObject *self, PyObject *value, void *closure);
static PyObject *PyWindow_get_spacing(PyWindowObject *self, void *closure);
static int       PyWindow_set_spacing(PyWindowObject *self, PyObject *value, void *closure);
static PyObject *PyWindow_get_scrollbar_size(PyWindowObject *self, void *closure);
static int       PyWindow_set_scrollbar_size(PyWindowObject *self, PyObject *value, void *closure);
static PyObject *PyWindow_get_min_size(PyWindowObject *self, void *closure);
static int       PyWindow_set_min_size(PyWindowObject *self, PyObject *value, void *closure);

static PyObject *PyWindow_get_padding(PyWindowObject *self, void *closure);
static int       PyWindow_set_padding(PyWindowObject *self, PyObject *value, void *closure);
static PyObject *PyWindow_get_group_padding(PyWindowObject *self, void *closure);
static int       PyWindow_set_group_padding(PyWindowObject *self, PyObject *value, void *closure);
static PyObject *PyWindow_get_popup_padding(PyWindowObject *self, void *closure);
static int       PyWindow_set_popup_padding(PyWindowObject *self, PyObject *value, void *closure);
static PyObject *PyWindow_get_combo_padding(PyWindowObject *self, void *closure);
static int       PyWindow_set_combo_padding(PyWindowObject *self, PyObject *value, void *closure);
static PyObject *PyWindow_get_contextual_padding(PyWindowObject *self, void *closure);
static int       PyWindow_set_contextual_padding(PyWindowObject *self, PyObject *value, void *closure);
static PyObject *PyWindow_get_menu_padding(PyWindowObject *self, void *closure);
static int       PyWindow_set_menu_padding(PyWindowObject *self, PyObject *value, void *closure);
static PyObject *PyWindow_get_tooltip_padding(PyWindowObject *self, void *closure);
static int       PyWindow_set_tooltip_padding(PyWindowObject *self, PyObject *value, void *closure);

static PyObject *PyWindow_get_closed(PyWindowObject *self, void *closure);
static PyObject *PyWindow_get_hidden(PyWindowObject *self, void *closure);
static PyObject *PyWindow_get_interactive(PyWindowObject *self, void *closure);
static int       PyWindow_set_interactive(PyWindowObject *self, PyObject *value, void *closure);
static PyObject *PyWindow_get_background(PyWindowObject *self, void *closure);
static int       PyWindow_set_background(PyWindowObject *self, PyObject *value, void *closure);
static PyObject *PyWindow_get_fixed_background(PyWindowObject *self, void *closure);
static int       PyWindow_set_fixed_background(PyWindowObject *self, PyObject *value, void *closure);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct nk_context *s_nk_ctx;
static vec_win_t          s_active_windows;

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
    (PyCFunction)PyWindow_button_label, METH_VARARGS | METH_KEYWORDS,
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

    {"edit_focus", 
    (PyCFunction)PyWindow_edit_focus, METH_VARARGS,
    "Give focus to the next active text edit widget."},

    {"group", 
    (PyCFunction)PyWindow_group, METH_VARARGS,
    "The window UI components pushed in the callable argument will be nested under a group."},

    {"popup", 
    (PyCFunction)PyWindow_popup, METH_VARARGS,
    "The window UI components pushed in the callable argument will be presented in a popup."},

    {"popup_close", 
    (PyCFunction)PyWindow_popup_close, METH_NOARGS,
    "Close the currently active popup window. Must only be called from poup context."},

    {"tree", 
    (PyCFunction)PyWindow_tree, METH_VARARGS,
    "The window UI components pushed in the callable argument will be nested under a "
    "collapsable tree section."},

    {"tree_element", 
    (PyCFunction)PyWindow_tree_element, METH_VARARGS,
    "The window UI components pushed in the callable argument will be nested under a "
    "collapsable non-root tree section."},

    {"selectable_symbol_label", 
    (PyCFunction)PyWindow_selectable_symbol_label, METH_VARARGS,
    "Text label preceded by one of the pf.NK_SYMBOL_ symbols."},

    {"combo_box", 
    (PyCFunction)PyWindow_combo_box, METH_VARARGS,
    "Present a combo box with a list of selectable options."},

    {"checkbox", 
    (PyCFunction)PyWindow_checkbox, METH_VARARGS,
    "Checkbox which can be toggled. Returns True if checked."},

    {"color_picker", 
    (PyCFunction)PyWindow_color_picker, METH_VARARGS,
    "Graphical color picker widget. Returns the selected color as an RGBA tuple."},

    {"image", 
    (PyCFunction)PyWindow_image, METH_VARARGS,
    "Present an image at the specified path."},

    {"spacer", 
    (PyCFunction)PyWindow_spacer, METH_VARARGS,
    "Empty widget to consume slots in a row."},

    {"property_float", 
    (PyCFunction)PyWindow_property_float, METH_VARARGS,
    "Editable input field for floating-point properties."},

    {"property_int", 
    (PyCFunction)PyWindow_property_int, METH_VARARGS,
    "Editable input field for integer properties."},

    {"file_browser", 
    (PyCFunction)PyWindow_file_browser, METH_VARARGS | METH_KEYWORDS,
    "Present a file browser widget."},

    {"slider_float", 
    (PyCFunction)PyWindow_slider_float, METH_VARARGS,
    "Present a slider widget with floating-point precision."},

    {"slider_int", 
    (PyCFunction)PyWindow_slider_int, METH_VARARGS,
    "Present a slider widget with integer precision."},

    {"progress", 
    (PyCFunction)PyWindow_progress, METH_VARARGS,
    "Present a progress bar widget with the current value, the maximum value and a 'modifiable' flag."},

    {"progress_text", 
    (PyCFunction)PyWindow_progress_text, METH_VARARGS,
    "Like 'progress', but also taking a string and (RGBA) parameters to draw a label over the progress bar."},

    {"text_lines", 
    (PyCFunction)PyWindow_text_lines, METH_VARARGS,
    "Returns the number of lines taken up by the specified text."},

    {"text_lines_width", 
    (PyCFunction)PyWindow_text_lines_width, METH_VARARGS,
    "Returns the number of lines taken up by the specified text in a widget of the specified width."},

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

    {"on_hide", 
    (PyCFunction)PyWindow_on_hide, METH_VARARGS,
    "Callback that gets invoked when the user hides the window with the close button (or via an API call)."},

    {"on_minimize", 
    (PyCFunction)PyWindow_on_minimize, METH_NOARGS,
    "Callback that gets invoked when the user minimizes the window with the minimize button."},

    {"on_maximize", 
    (PyCFunction)PyWindow_on_maximize, METH_NOARGS,
    "Callback that gets invoked when the user maximizes the window with the maximize button."},

    {"__pickle__", 
    (PyCFunction)PyWindow_pickle, METH_KEYWORDS,
    "Serialize a Permafrost Engine window to a string."},

    {"__unpickle__", 
    (PyCFunction)PyWindow_unpickle, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
    "Create a new pf.Window instance from a string earlier returned from a __pickle__ method."
    "Returns a tuple of the new instance and the number of bytes consumed from the stream."},

    {NULL}  /* Sentinel */
};

static PyGetSetDef PyWindow_getset[] = {
    {"header",
    (getter)PyWindow_get_header, NULL,
    "An pf.UIHeaderStyle type for controlling the style parameters of the window header.",
    NULL},
    {"position",
    (getter)PyWindow_get_pos, (setter)PyWindow_set_pos,
    "A tuple of two integers specifying the X and Y position of the window.",
    NULL},
    {"size",
    (getter)PyWindow_get_size, NULL,
    "A tuple of two integers specifying the width and height dimentions of the window.",
    NULL},
    {"header_height",
    (getter)PyWindow_get_header_height, NULL,
    "A float specifying the height of the window header in pixels.",
    NULL},
    {"spacing",
    (getter)PyWindow_get_spacing, 
    (setter)PyWindow_set_spacing,
    "An (X, Y) tuple of floats to control the spacing (between components) within a window.", 
    NULL},
    {"padding",
    (getter)PyWindow_get_padding, 
    (setter)PyWindow_set_padding,
    "An (X, Y) tuple of floats to control the padding (between border and content) of a window.", 
    NULL},
    {"group_padding",
    (getter)PyWindow_get_group_padding, 
    (setter)PyWindow_set_group_padding,
    "An (X, Y) tuple of floats to control the padding around a group in a window.", 
    NULL},
    {"popup_padding",
    (getter)PyWindow_get_popup_padding, 
    (setter)PyWindow_set_popup_padding,
    "An (X, Y) tuple of floats to control the padding in a popup window.", 
    NULL},
    {"combo_padding",
    (getter)PyWindow_get_combo_padding, 
    (setter)PyWindow_set_combo_padding,
    "An (X, Y) tuple of floats to control the padding around a combo section in a window.", 
    NULL},
    {"contextual_padding",
    (getter)PyWindow_get_contextual_padding, 
    (setter)PyWindow_set_contextual_padding,
    "An (X, Y) tuple of floats to control the padding around a contextual button.", 
    NULL},
    {"menu_padding",
    (getter)PyWindow_get_menu_padding, 
    (setter)PyWindow_set_menu_padding,
    "An (X, Y) tuple of floats to control the padding around a menu button in a window.", 
    NULL},
    {"tooltip_padding",
    (getter)PyWindow_get_tooltip_padding, 
    (setter)PyWindow_set_tooltip_padding,
    "An (X, Y) tuple of floats to control the padding in a tooltip window.", 
    NULL},
    {"border_color",
    (getter)PyWindow_get_border_color, 
    (setter)PyWindow_set_border_color,
    "An (R,G,B,A) tuple to control the border color of a window.", 
    NULL},
    {"popup_border_color",
    (getter)PyWindow_get_popup_border_color, 
    (setter)PyWindow_set_popup_border_color,
    "An (R,G,B,A) tuple to control the border color of window popups.", 
    NULL},
    {"combo_border_color",
    (getter)PyWindow_get_combo_border_color, 
    (setter)PyWindow_set_combo_border_color,
    "An (R,G,B,A) tuple to control the border color of window combo boxes.", 
    NULL},
    {"contextual_border_color",
    (getter)PyWindow_get_contextual_border_color, 
    (setter)PyWindow_set_contextual_border_color,
    "An (R,G,B,A) tuple to control the border color of window contextual panels.", 
    NULL},
    {"menu_border_color",
    (getter)PyWindow_get_menu_border_color, 
    (setter)PyWindow_set_menu_border_color,
    "An (R,G,B,A) tuple to control the border color of window menus.", 
    NULL},
    {"group_border_color",
    (getter)PyWindow_get_group_border_color, 
    (setter)PyWindow_set_group_border_color,
    "An (R,G,B,A) tuple to control the border color of window group panels.", 
    NULL},
    {"tooltip_border_color",
    (getter)PyWindow_get_tooltip_border_color, 
    (setter)PyWindow_set_tooltip_border_color,
    "An (R,G,B,A) tuple to control the border color of tooltip panels.", 
    NULL},
    {"border",
    (getter)PyWindow_get_border, 
    (setter)PyWindow_set_border,
    "A float to control the border width of a window.", 
    NULL},
    {"combo_border",
    (getter)PyWindow_get_combo_border, 
    (setter)PyWindow_set_combo_border,
    "A float to control the border width around a combo section.",
    NULL},
    {"contextual_border",
    (getter)PyWindow_get_contextual_border, 
    (setter)PyWindow_set_contextual_border,
    "A float to control the border width around a contextual button.",
    NULL},
    {"menu_border",
    (getter)PyWindow_get_menu_border, 
    (setter)PyWindow_set_menu_border,
    "A float to control the border width around a menu button.",
    NULL},
    {"group_border",
    (getter)PyWindow_get_group_border, 
    (setter)PyWindow_set_group_border,
    "A float to control the border width around a group.", 
    NULL},
    {"tooltip_border",
    (getter)PyWindow_get_tooltip_border, 
    (setter)PyWindow_set_tooltip_border,
    "A float to control the border width around a tooltip window.",
    NULL},
    {"popup_border",
    (getter)PyWindow_get_popup_border, 
    (setter)PyWindow_set_popup_border,
    "A float to control the border width around a popup window.",
    NULL},
    {"min_row_height_padding",
    (getter)PyWindow_get_min_row_height_padding, 
    (setter)PyWindow_set_min_row_height_padding,
    "A float to control the minimum number of pixels of padding at the header and footer of a row..", 
    NULL},
    {"scrollbar_size",
    (getter)PyWindow_get_scrollbar_size, 
    (setter)PyWindow_set_scrollbar_size,
    "An (X, Y) tuple of floats to control the size of the scrollbar.", 
    NULL},
    {"min_size",
    (getter)PyWindow_get_min_size, 
    (setter)PyWindow_set_min_size,
    "An (X, Y) tuple of floats to control the minimum size of the window.", 
    NULL},
    {"closed",
    (getter)PyWindow_get_closed, NULL,
    "A readonly bool indicating if this window is 'closed'.",
    NULL},
    {"hidden",
    (getter)PyWindow_get_hidden, NULL,
    "A readonly bool indicating if this window is 'hidden'.",
    NULL},
    {"interactive",
    (getter)PyWindow_get_interactive, 
    (setter)PyWindow_set_interactive,
    "A read-write bool to enable or disable user interactivity for this window.",
    NULL},
    {"background",
    (getter)PyWindow_get_background, 
    (setter)PyWindow_set_background,
    "An (R, G, B, A) tuple of floats specifying the background color for some panels "
    "such as the combo box popup.", 
    NULL},
    {"fixed_background",
    (getter)PyWindow_get_fixed_background, 
    (setter)PyWindow_set_fixed_background,
    "An image path or an (R, G, B, A) tuple of floats specifying the background style of the window.", 
    NULL},
    {NULL}  /* Sentinel */
};

static PyTypeObject PyWindow_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "pf.Window",               /* tp_name */
    sizeof(PyWindowObject),    /* tp_basicsize */
    0,                         /* tp_itemsize */
    (destructor)PyWindow_dealloc, /* tp_dealloc */
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
    PyWindow_getset,           /* tp_getset */
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

static bool equal(PyWindowObject *const *a, PyWindowObject *const *b)
{
    return *a == *b;
}

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

static int PyWindow_init(PyWindowObject *self, PyObject *args, PyObject *kwargs)
{
    const char *name;
    struct rect rect;
    int flags;
    int vres[2];
    int resize_mask = ANCHOR_DEFAULT;
    int suspend_on_pause = false;
    static char *kwlist[] = { "name", "bounds", "flags", "virtual_resolution", "resize_mask", "suspend_on_pause", NULL };

    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "s(iiii)i(ii)|ii", kwlist, &name, &rect.x, &rect.y, 
                                    &rect.w, &rect.h, &flags, &vres[0], &vres[1], &resize_mask, &suspend_on_pause)) {
        PyErr_SetString(PyExc_TypeError, "4 arguments expected: integer, tuple of 4 integers, integer, and a tuple of 2 integers.");
        return -1;
    }

    if((resize_mask & ANCHOR_X_MASK) == 0
    || (resize_mask & ANCHOR_Y_MASK) == 0) {

        PyErr_SetString(PyExc_RuntimeError, "Invalid reisize mask: the window must have at least one anchor in each dimension.");
        return -1;
    }

    self->header_style = S_UIHeaderStyleNew();
    if(!self->header_style) {
        assert(PyErr_Occurred());
        return -1;
    }

    pf_strlcpy(self->name, name, sizeof(self->name));
    self->rect = rect;
    self->flags = flags;
    self->style = s_nk_ctx->style.window;
    self->resize_mask = resize_mask;
    self->suspend_on_pause = !!suspend_on_pause;
    self->virt_res.x = vres[0];
    self->virt_res.y = vres[1];

    self->flags |= (NK_WINDOW_CLOSED | NK_WINDOW_HIDDEN); /* closed by default */
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
    float width;

    if(!PyArg_ParseTuple(args, "f", &width)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a single float.");
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

static PyObject *PyWindow_button_label(PyWindowObject *self, PyObject *args, PyObject *kwargs)
{
    const char *str, *tooltip = NULL;
    PyObject *callable, *cargs = NULL;
    static char *kwlist[] = { "string", "callable", "args", "tooltip", NULL };
    struct nk_rect bounds = nk_widget_bounds(s_nk_ctx);
    const struct nk_input *in = &s_nk_ctx->input;

    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "sO|Os", kwlist, &str, &callable, &cargs, &tooltip)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a string and an object. "
            "Optionally, an argument to the callable can be provided, as well as tooltip text.");
        return NULL;
    }

    if(!PyCallable_Check(callable)) {
        PyErr_SetString(PyExc_TypeError, "Second argument must be callable.");
        return NULL;
    }

    if(nk_button_label(s_nk_ctx, str)) {
        PyObject *ret = PyObject_CallObject(callable, cargs);
        Py_XDECREF(ret);
    }

    bool hovering = nk_input_is_mouse_hovering_rect(in, bounds);
    if(tooltip && hovering) {
        nk_tooltip(s_nk_ctx, tooltip);
    }

    if(hovering) {
        Py_RETURN_TRUE;
    }else{
        Py_RETURN_FALSE;
    }
}

static PyObject *PyWindow_simple_chart(PyWindowObject *self, PyObject *args)
{
    int type;
    int min, max;
    PyObject *list, *on_click_handler = NULL;

    int clicked_index = -1;
    int hovered_index = -1;
    long hovered_val;

    if(!PyArg_ParseTuple(args, "i(ii)O|O", &type, &min, &max, &list, &on_click_handler)) {
        PyErr_SetString(PyExc_TypeError, "3 arguments expected: an integer, a tuple of two integers, and a list object. "
            "Optionally, a callable taking exactly one integer index argument (click handler) can additionally be supplied.");
        return NULL;
    }

    if(!PyList_Check(list)) {
        PyErr_SetString(PyExc_TypeError, "Third argument must be a list.");
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

            if((res & NK_CHART_CLICKED) && on_click_handler) {
                clicked_index = i;
            }
        }
        nk_chart_end(s_nk_ctx);

        if(hovered_index != -1)
            nk_tooltipf(s_nk_ctx, "Value: %lu", hovered_val);

        if(clicked_index != -1 && on_click_handler) {
            PyObject *args = Py_BuildValue("(i)", clicked_index);
            if(args) {
                PyObject *ret = PyObject_CallObject(on_click_handler, args);
                Py_DECREF(args);
                Py_XDECREF(ret);
            }
        }
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

static PyObject *PyWindow_edit_focus(PyWindowObject *self, PyObject *args)
{
    int flags;
    if(!PyArg_ParseTuple(args, "i", &flags)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an integer (flags).");
        return NULL;
    }

    nk_edit_focus(s_nk_ctx, flags);
    Py_RETURN_NONE;
}

static PyObject *PyWindow_group(PyWindowObject *self, PyObject *args)
{
    const char *name;
    int group_flags;
    PyObject *callable, *cargs = NULL;

    if(!PyArg_ParseTuple(args, "siO|O", &name, &group_flags, &callable, &cargs)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a string, an integer and an object. "
            "Optionally, args to the callable can be supplied.");
        return NULL;
    }

    if(!PyCallable_Check(callable)) {
        PyErr_SetString(PyExc_TypeError, "Third argument must be callable.");
        return NULL;
    }

    if(nk_group_begin(s_nk_ctx, name, group_flags)) {
        PyObject *ret = PyObject_CallObject(callable, cargs);
        Py_XDECREF(ret);

        nk_group_end(s_nk_ctx);
        if(!ret)
            return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyWindow_popup(PyWindowObject *self, PyObject *args)
{
    const char *name;
    enum nk_popup_type type;
    int popup_flags;
    struct nk_rect rect;
    PyObject *callable, *cargs = NULL;

    if(!PyArg_ParseTuple(args, "sii(ffff)O|O", &name, &type, &popup_flags, &rect.x, &rect.y, &rect.w, &rect.h, &callable, &cargs)) {
        PyErr_SetString(PyExc_TypeError, "Arguments must be a string, an integer (type), an integer (flags), "
            "a tuple of 4 floats (bounds) and a callable object. "
            "Optionally, args to the callable can be supplied.");
        return NULL;
    }

    if(type != NK_POPUP_STATIC && type != NK_POPUP_DYNAMIC) {
        PyErr_SetString(PyExc_TypeError, "The type argument must be one of pf.NK_POPUP_STATIC or pf.NK_POPUP_DYNAMIC.");
        return NULL;
    }

    if(!PyCallable_Check(callable)) {
        PyErr_SetString(PyExc_TypeError, "Fifth argument must be callable.");
        return NULL;
    }

    if(nk_popup_begin(s_nk_ctx, type, name, popup_flags, rect)) {
        PyObject *ret = PyObject_CallObject(callable, cargs);
        Py_XDECREF(ret);
        nk_popup_end(s_nk_ctx);

        if(!ret)
            return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *PyWindow_popup_close(PyWindowObject *self)
{
    struct nk_window *popup;

    struct nk_window *win = s_nk_ctx->current;
    struct nk_panel *panel = win->layout;
    if(!(panel->type & NK_PANEL_SET_POPUP)) {
    
        PyErr_SetString(PyExc_RuntimeError, "The 'popup_close' method must only be called from poup context.");
        return NULL;
    }

    nk_popup_close(s_nk_ctx);
    Py_RETURN_NONE;
}

static PyObject *PyWindow_tree(PyWindowObject *self, PyObject *args)
{
    int type, state;
    const char *name;
    PyObject *callable, *cargs = NULL;

    if(!PyArg_ParseTuple(args, "isiO|O", &type, &name, &state, &callable, &cargs)) {
        PyErr_SetString(PyExc_TypeError, "Invalid arguments. Expecting: (type, name, state, callable, [args])");
        return NULL;
    }

    if(type != NK_TREE_TAB && type != NK_TREE_NODE) {
        PyErr_SetString(PyExc_TypeError, "First argument must be one of pf.NK_TREE_TAB or pf.NK_TREE_NODE.");
        return NULL;
    }

    if(state != NK_MINIMIZED && state != NK_MAXIMIZED) {
        PyErr_SetString(PyExc_TypeError, "Third argument must be one of pf.NK_MINIMIZED or pf.NK_MAXIMIZED.");
        return NULL;
    }

    if(!PyCallable_Check(callable)) {
        PyErr_SetString(PyExc_TypeError, "Fourth argument must be callable.");
        return NULL;
    }

    if(cargs && !PyTuple_Check(cargs)) {
        PyErr_SetString(PyExc_TypeError, "(Optional) fifth argument must be a tuple.");
        return NULL;
    }

    bool shown;
    if((shown = nk_tree_push_hashed(s_nk_ctx, type, name, state, name, strlen(name), (uintptr_t)self))) {
        PyObject *ret = PyObject_CallObject(callable, cargs);
        Py_XDECREF(ret);
        nk_tree_pop(s_nk_ctx);
    }

    if(shown)
        Py_RETURN_TRUE;
    else
        Py_RETURN_FALSE;
}

static PyObject *PyWindow_tree_element(PyWindowObject *self, PyObject *args)
{
    int type, state;
    const char *name;
    PyObject *callable, *selected, *cargs = NULL;

    if(!PyArg_ParseTuple(args, "isiOO|O", &type, &name, &state, &selected, &callable, &cargs)) {
        PyErr_SetString(PyExc_TypeError, "Invalida arguments. Expecting: (type, name, state, selected, callable, [args])");
        return NULL;
    }

    if(type != NK_TREE_TAB && type != NK_TREE_NODE) {
        PyErr_SetString(PyExc_TypeError, "First argument must be one of pf.NK_TREE_TAB or pf.NK_TREE_NODE.");
        return NULL;
    }

    if(state != NK_MINIMIZED && state != NK_MAXIMIZED) {
        PyErr_SetString(PyExc_TypeError, "Third argument must be one of pf.NK_MINIMIZED or pf.NK_MAXIMIZED.");
        return NULL;
    }

    if(!PyCallable_Check(callable)) {
        PyErr_SetString(PyExc_TypeError, "Fifth argument must be callable.");
        return NULL;
    }

    if(cargs && !PyTuple_Check(cargs)) {
        PyErr_SetString(PyExc_TypeError, "(Optional) sixth argument must be a tuple.");
        return NULL;
    }

    int sel = PyObject_IsTrue(selected);
    if(nk_tree_element_push_hashed(s_nk_ctx, type, name, state, &sel, name, strlen(name), (uintptr_t)self)) {
        PyObject *ret = PyObject_CallObject(callable, cargs);
        Py_XDECREF(ret);
        nk_tree_pop(s_nk_ctx);
    }

    if(sel)
        Py_RETURN_TRUE;
    else
        Py_RETURN_FALSE;
}

static PyObject *PyWindow_selectable_symbol_label(PyWindowObject *self, PyObject *args)
{
    int symbol, align;
    const char *title;
    PyObject *selected;

    if(!PyArg_ParseTuple(args, "isiO", &symbol, &title, &align, &selected)) {
        PyErr_SetString(PyExc_TypeError, "Invalida arguments. Expecting: (symbol, title, alignment, selected)");
        return NULL;
    }

    if(symbol < 0 || symbol >= NK_SYMBOL_MAX) {
        PyErr_SetString(PyExc_TypeError, "First argument must be one of the pf.NK_SYMBOL_ constants.");
        return NULL;
    }

    if(align != NK_TEXT_LEFT && align != NK_TEXT_RIGHT && align != NK_TEXT_CENTERED) {
        PyErr_SetString(PyExc_TypeError, "Third argument must be one of: pf.NK_TEXT_LEFT, pf.NK_TEXT_CENTERED, pf.NK_TEXT_RIGHT.");
        return NULL;
    }

    int sel = PyObject_IsTrue(selected);
    nk_selectable_symbol_label(s_nk_ctx, symbol, title, align, &sel);

    if(sel)
        Py_RETURN_TRUE;
    else
        Py_RETURN_FALSE;
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
    STALLOC(const char*, labels, num_items);

    for(int i = 0; i < num_items; i++) {

        PyObject *str = PyList_GetItem(items_list, i);
        if(!PyString_Check(str)) {
            PyErr_SetString(PyExc_TypeError, "First argument list must only contain strings.");
            return NULL;
        }
        labels[i] = PyString_AsString(str);
    }

    int ret = nk_combo(s_nk_ctx, labels, num_items, selected_idx, item_height, size);
    STFREE(labels);
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

static PyObject *PyWindow_image(PyWindowObject *self, PyObject *args)
{
    const char *path;

    if(!PyArg_ParseTuple(args, "s", &path)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a string.");
        return NULL;
    }

    nk_image_texpath(s_nk_ctx, path);
    Py_RETURN_NONE;
}

static PyObject *PyWindow_spacer(PyWindowObject *self, PyObject *args)
{
    int ncols;

    if(!PyArg_ParseTuple(args, "i", &ncols)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an int.");
        return NULL;
    }

    nk_spacing(s_nk_ctx, ncols);
    Py_RETURN_NONE;
}

static PyObject *PyWindow_property_float(PyObject *self, PyObject *args)
{
    const char *name;
    float min, max, val, step, drag_step;

    if(!PyArg_ParseTuple(args, "sfffff", &name, &min, &max, &val, &step, &drag_step)) {
        PyErr_SetString(PyExc_TypeError, "Expecting 6 arguments: name (string), min (float), max (float), "
            "val (float), step (float), drag_step (float).");
        return NULL;
    }

    nk_property_float(s_nk_ctx, name, min, &val, max, step, drag_step);
    return PyFloat_FromDouble(val);
}

static PyObject *PyWindow_property_int(PyObject *self, PyObject *args)
{
    const char *name;
    int min, max, val, step;
    float drag_step;

    if(!PyArg_ParseTuple(args, "siiiif", &name, &min, &max, &val, &step, &drag_step)) {
        PyErr_SetString(PyExc_TypeError, "Expecting 6 arguments: name (string), min (int), max (int), "
            "val (int), step (int), drag_step (float).");
        return NULL;
    }

    nk_property_int(s_nk_ctx, name, min, &val, max, step, drag_step);
    return PyInt_FromLong(val);
}

static PyObject *PyWindow_file_browser(PyWindowObject *self, PyObject *args, PyObject *kwargs)
{
    const char *name, *directory;
    PyObject *selected;
    int flags;

    static char *kwlist[] = { "name", "directory", "selected", "flags", NULL };

    if(!PyArg_ParseTupleAndKeywords(args, kwargs, "ssOi", kwlist, &name, &directory, &selected, &flags)) {
        goto fail_args;
    }

    if(selected != Py_None && !PyString_Check(selected)) {
        goto fail_args;
    }

    struct nk_fb_state state;
    pf_strlcpy(state.name, name, sizeof(state.name));
    pf_strlcpy(state.directory, directory, sizeof(state.directory));
    state.flags = flags;

    if(selected == Py_None) {
        state.selected[0] = '\0';
    }else{
        pf_strlcpy(state.selected, PyString_AS_STRING(selected), sizeof(state.selected));
    }

    nk_file_browser(s_nk_ctx, &state);

    if(strlen(state.selected) == 0) {
        Py_INCREF(Py_None);    
        selected = Py_None;
    }else{
        selected = PyString_FromString(state.selected);
    }

    return Py_BuildValue("{s:s,s:s,s:O,s:i}", 
        "name", name,
        "directory", state.directory, 
        "selected", selected,
        "flags", flags);

fail_args:
    PyErr_SetString(PyExc_TypeError, "4 arguments expected: name (string), directory (string), selected (string or None), flags (int).");
    return NULL;
}

static PyObject *PyWindow_slider_float(PyWindowObject *self, PyObject *args)
{
    float min, max, curr, step;
    if(!PyArg_ParseTuple(args, "ffff", &min, &max, &curr, &step)) {
        PyErr_SetString(PyExc_TypeError, "Expecting 4 float arguments: min, max, curr, step");
        return NULL;
    }

    nk_slider_float(s_nk_ctx, min, &curr, max, step);
    return PyFloat_FromDouble(curr);
}

static PyObject *PyWindow_slider_int(PyWindowObject *self, PyObject *args)
{
    int min, max, curr, step;
    if(!PyArg_ParseTuple(args, "iiii", &min, &max, &curr, &step)) {
        PyErr_SetString(PyExc_TypeError, "Expecting 4 int arguments: min, max, curr, step");
        return NULL;
    }

    nk_slider_int(s_nk_ctx, min, &curr, max, step);
    return PyInt_FromLong(curr);
}

static PyObject *PyWindow_progress(PyWindowObject *self, PyObject *args)
{
    int curr, max;
    PyObject *mod;

    if(!PyArg_ParseTuple(args, "iiO", &curr, &max, &mod)) {
        PyErr_SetString(PyExc_TypeError, "Expecting 3 arguments: curr (int), max (int), "
            "and modifiable (bool expression)");
        return NULL;
    }

    bool modifiable = PyObject_IsTrue(mod);
    curr = nk_prog(s_nk_ctx, curr, max, modifiable);
    return PyInt_FromLong(curr);
}

static PyObject *PyWindow_progress_text(PyWindowObject *self, PyObject *args)
{
    int curr, max;
    PyObject *mod;
    const char *str;
    int r, g, b, a;

    if(!PyArg_ParseTuple(args, "iiOs(iiii)", &curr, &max, &mod, &str, &r, &g, &b, &a)) {
        PyErr_SetString(PyExc_TypeError, "Expecting 5 arguments: curr (int), max (int), "
            "modifiable (bool expression), text (string), color (RGBA) integer tuple.");
        return NULL;
    }

    struct nk_color clr = (struct nk_color){r, g, b, a};
    bool modifiable = PyObject_IsTrue(mod);

    curr = nk_prog_text(s_nk_ctx, curr, max, modifiable, str, clr);
    return PyInt_FromLong(curr);
}

static PyObject *PyWindow_text_lines(PyWindowObject *self, PyObject *args)
{
    const char *str;
    if(!PyArg_ParseTuple(args, "s", &str)) {
        PyErr_SetString(PyExc_TypeError, "Expecting one (string) argument.");
        return NULL;
    }
    int ret = nk_text_lines(s_nk_ctx, str);
    return PyInt_FromLong(ret);
}

static PyObject *PyWindow_text_lines_width(PyWindowObject *self, PyObject *args)
{
    const char *str;
    int width;
    if(!PyArg_ParseTuple(args, "si", &str, &width)) {
        PyErr_SetString(PyExc_TypeError, "Expecting a string argument (text) and an integer argument (width).");
        return NULL;
    }
    int ret = nk_text_lines_width(s_nk_ctx, str, width);
    return PyInt_FromLong(ret);
}

static PyObject *PyWindow_show(PyWindowObject *self)
{
    self->flags &= ~(NK_WINDOW_HIDDEN | NK_WINDOW_CLOSED);
    nk_window_show(s_nk_ctx, self->name, NK_SHOWN);
    self->hide = false;
    Py_RETURN_NONE;
}

static PyObject *PyWindow_hide(PyWindowObject *self)
{
    self->hide = true;
    Py_RETURN_NONE;
}

static PyObject *PyWindow_update(PyWindowObject *self)
{
    Py_RETURN_NONE;
}

static PyObject *PyWindow_on_hide(PyWindowObject *self, PyObject *args)
{
    Py_RETURN_NONE;
}

static PyObject *PyWindow_on_minimize(PyWindowObject *self)
{
    Py_RETURN_NONE;
}

static PyObject *PyWindow_on_maximize(PyWindowObject *self)
{
    Py_RETURN_NONE;
}

static PyObject *PyWindow_pickle(PyWindowObject *self, PyObject *args, PyObject *kwargs)
{
    bool status;
    PyObject *ret = NULL;

    SDL_RWops *stream = PFSDL_VectorRWOps();
    CHK_TRUE(stream, fail_alloc);

    PyObject *name = PyString_FromString(self->name);
    CHK_TRUE(name, fail_pickle);
    status = S_PickleObjgraph(name, stream);
    Py_DECREF(name);
    CHK_TRUE(status, fail_pickle);

    PyObject *rect = Py_BuildValue("(iiii)", 
        self->rect.x, self->rect.y, self->rect.w, self->rect.h);
    CHK_TRUE(rect, fail_pickle);
    status = S_PickleObjgraph(rect, stream);
    Py_DECREF(rect);
    CHK_TRUE(status, fail_pickle);

    PyObject *flags = PyInt_FromLong(self->flags);
    CHK_TRUE(flags, fail_pickle);
    status = S_PickleObjgraph(flags, stream);
    Py_DECREF(flags);
    CHK_TRUE(status, fail_pickle);

    PyObject *virt_res = Py_BuildValue("(ii)", self->virt_res.x, self->virt_res.y);
    CHK_TRUE(virt_res, fail_pickle);
    status = S_PickleObjgraph(virt_res, stream);
    Py_DECREF(virt_res);
    CHK_TRUE(status, fail_pickle);

    PyObject *resize_mask = PyInt_FromLong(self->resize_mask);
    CHK_TRUE(resize_mask, fail_pickle);
    status = S_PickleObjgraph(resize_mask, stream);
    Py_DECREF(resize_mask);
    CHK_TRUE(status, fail_pickle);

    PyObject *sop = PyInt_FromLong(self->suspend_on_pause);
    CHK_TRUE(sop, fail_pickle);
    status = S_PickleObjgraph(sop, stream);
    Py_DECREF(sop);
    CHK_TRUE(status, fail_pickle);

    PyObject *hide = PyInt_FromLong(self->hide);
    CHK_TRUE(hide, fail_pickle);
    status = S_PickleObjgraph(hide, stream);
    Py_DECREF(hide);
    CHK_TRUE(status, fail_pickle);

    status = S_PickleObjgraph(self->header_style, stream);
    CHK_TRUE(status, fail_pickle);

    CHK_TRUE_RET(S_UI_Style_SaveWindow(stream, &self->style));
    ret = PyString_FromStringAndSize(PFSDL_VectorRWOpsRaw(stream), SDL_RWsize(stream));

fail_pickle:
    SDL_RWclose(stream);
fail_alloc:
    return ret;
}

static PyObject *PyWindow_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs)
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

    PyObject *name = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    PyObject *rect = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    PyObject *flags = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    PyObject *virt_res = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    PyObject *resize_mask = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    PyObject *sop = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    PyObject *hide = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    PyObject *header_style = S_UnpickleObjgraph(stream);
    SDL_RWread(stream, &tmp, 1, 1); /* consume NULL byte */

    if(!name
    || !rect
    || !flags
    || !resize_mask
    || !sop
    || !hide
    || !virt_res) {
        PyErr_SetString(PyExc_RuntimeError, "Could not unpickle internal state of pf.Window instance");
        goto fail_unpickle;
    }

    /* Use the 'plain' (i.e. direct successor with no extra overrides) as the arugment
     * to avoid calling any magic that might be risiding in the user-implemented __new__ 
     */
    PyTypeObject *heap_subtype = (PyTypeObject*)S_Pickle_PlainHeapSubtype((PyTypeObject*)cls);
    assert(heap_subtype->tp_new);

    PyObject *win_args = Py_BuildValue("(OOOOOO)", name, rect, flags, virt_res, resize_mask, sop);
    PyWindowObject *winobj = (PyWindowObject*)heap_subtype->tp_new((struct _typeobject*)cls, win_args, NULL);
    assert(winobj || PyErr_Occurred());
    CHK_TRUE(winobj, fail_window);

    const char *namestr;
    int isop;

    if(!PyArg_ParseTuple(win_args, "s(iiii)i(ii)|ii", &namestr,
        &winobj->rect.x, &winobj->rect.y, 
        &winobj->rect.w, &winobj->rect.h, 
        &winobj->flags, 
        &winobj->virt_res.x, &winobj->virt_res.y, 
        &winobj->resize_mask,
        &isop)) {
        goto fail_window;
    }
    pf_strlcpy(winobj->name, namestr, sizeof(winobj->name));
    winobj->suspend_on_pause = !!isop;

    CHK_TRUE(PyInt_Check(hide), fail_window);
    winobj->hide = !!PyInt_AS_LONG(hide);

    if(!S_UI_Style_LoadWindow(stream, &winobj->style)) {
        PyErr_SetString(PyExc_RuntimeError, "Could not unpickle style state of pf.Window instance");
        goto fail_window;
    }

    Py_INCREF(header_style);
    winobj->header_style = header_style;

    Py_ssize_t nread = SDL_RWseek(stream, 0, RW_SEEK_CUR);
    ret = Py_BuildValue("(Oi)", winobj, (int)nread);

fail_window:
    Py_XDECREF(winobj);
    Py_XDECREF(win_args);
fail_unpickle:
    Py_XDECREF(name);
    Py_XDECREF(rect);
    Py_XDECREF(flags);
    Py_XDECREF(virt_res);
    Py_XDECREF(resize_mask);
    Py_XDECREF(sop);
    Py_XDECREF(hide);
    Py_XDECREF(header_style);
    SDL_RWclose(stream);
fail_args:
    return ret;
}

static PyObject *PyWindow_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyObject *self = type->tp_alloc(type, 0);
    ((PyWindowObject*)self)->header_style = NULL;
    ((PyWindowObject*)self)->resize_mask = ANCHOR_DEFAULT;
    ((PyWindowObject*)self)->suspend_on_pause = false;
    ((PyWindowObject*)self)->hide = false;
    vec_win_push(&s_active_windows, (PyWindowObject*)self);
    return self;
}

static void PyWindow_dealloc(PyWindowObject *self)
{
    Py_XDECREF(self->header_style);

    int idx = vec_win_indexof(&s_active_windows, self, equal);
    vec_win_del(&s_active_windows, idx);

    nk_window_close(s_nk_ctx, self->name);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject *PyWindow_get_header(PyWindowObject *self, void *closure)
{
    Py_INCREF(self->header_style);
    return self->header_style;
}

static PyObject *PyWindow_get_pos(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("(ii)", self->rect.x, self->rect.y);
}

static int PyWindow_set_pos(PyWindowObject *self, PyObject *value, void *closure)
{
    int x, y;
    if(!PyArg_ParseTuple(value, "ii", &x, &y)) {
        PyErr_SetString(PyExc_TypeError, "Value must be a tuple of 2 integers.");
        return -1;
    }
    self->rect.x = x;
    self->rect.y = y;
    return 0;
}

static PyObject *PyWindow_get_size(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("(ii)", self->rect.w, self->rect.h);
}

static PyObject *PyWindow_get_header_height(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("i", (int)S_UIHeaderGetHeight(self->header_style, s_nk_ctx));
}

static PyObject *PyWindow_get_spacing(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("(f, f)",
        self->style.spacing.x,
        self->style.spacing.y); 
}

static int PyWindow_set_spacing(PyWindowObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style.spacing = (struct nk_vec2){x, y};
    return 0;
}

static PyObject *PyWindow_get_padding(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("(f, f)",
        self->style.padding.x,
        self->style.padding.y);
}

static int PyWindow_set_padding(PyWindowObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style.padding = nk_vec2(x,y);
    return 0;
}

static PyObject *PyWindow_get_group_padding(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("(f, f)",
        self->style.group_padding.x,
        self->style.group_padding.y);
}

static int PyWindow_set_group_padding(PyWindowObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style.group_padding = nk_vec2(x,y);
    return 0;
}

static PyObject *PyWindow_get_combo_padding(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("(f, f)",
        self->style.combo_padding.x,
        self->style.combo_padding.y);
}

static PyObject *PyWindow_get_popup_padding(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("(f, f)",
        self->style.popup_padding.x,
        self->style.popup_padding.y);
}

static int PyWindow_set_popup_padding(PyWindowObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style.popup_padding = nk_vec2(x,y);
    return 0;
}

static int PyWindow_set_combo_padding(PyWindowObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style.combo_padding = nk_vec2(x,y);
    return 0;
}

static PyObject *PyWindow_get_contextual_padding(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("(f, f)",
        self->style.contextual_padding.x,
        self->style.contextual_padding.y);
}

static int PyWindow_set_contextual_padding(PyWindowObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style.contextual_padding = nk_vec2(x,y);
    return 0;
}

static PyObject *PyWindow_get_menu_padding(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("(f, f)",
        self->style.menu_padding.x,
        self->style.menu_padding.y);
}

static int PyWindow_set_menu_padding(PyWindowObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style.menu_padding = nk_vec2(x,y);
    return 0;
}

static PyObject *PyWindow_get_tooltip_padding(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("(f, f)",
        self->style.tooltip_padding.x,
        self->style.tooltip_padding.y);
}

static int PyWindow_set_tooltip_padding(PyWindowObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style.tooltip_padding = nk_vec2(x,y);
    return 0;
}

static PyObject *PyWindow_get_border(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("f", self->style.border);
}

static int PyWindow_set_border(PyWindowObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a float.");
        return -1;
    }

    self->style.border = PyFloat_AS_DOUBLE(value);
    return 0;
}

static PyObject *PyWindow_get_border_color(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("iiii", 
        self->style.border_color.r,
        self->style.border_color.g,
        self->style.border_color.b,
        self->style.border_color.a
    );
}

static int PyWindow_set_border_color(PyWindowObject *self, PyObject *value, void *closure)
{
    int r, g, b, a;
    if(!PyArg_ParseTuple(value, "iiii", &r, &g, &b, &a)) {
        PyErr_SetString(PyExc_TypeError, "Value must be a tuple of 4 integers (0-255).");
        return -1;
    }

    self->style.border_color = (struct nk_color){r, g, b, a};
    return 0;
}

static PyObject *PyWindow_get_popup_border_color(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("iiii", 
        self->style.popup_border_color.r,
        self->style.popup_border_color.g,
        self->style.popup_border_color.b,
        self->style.popup_border_color.a
    );
}

static int PyWindow_set_popup_border_color(PyWindowObject *self, PyObject *value, void *closure)
{
    int r, g, b, a;
    if(!PyArg_ParseTuple(value, "iiii", &r, &g, &b, &a)) {
        PyErr_SetString(PyExc_TypeError, "Value must be a tuple of 4 integers (0-255).");
        return -1;
    }

    self->style.popup_border_color = (struct nk_color){r, g, b, a};
    return 0;
}

static PyObject *PyWindow_get_combo_border_color(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("iiii", 
        self->style.combo_border_color.r,
        self->style.combo_border_color.g,
        self->style.combo_border_color.b,
        self->style.combo_border_color.a
    );
}

static int PyWindow_set_combo_border_color(PyWindowObject *self, PyObject *value, void *closure)
{
    int r, g, b, a;
    if(!PyArg_ParseTuple(value, "iiii", &r, &g, &b, &a)) {
        PyErr_SetString(PyExc_TypeError, "Value must be a tuple of 4 integers (0-255).");
        return -1;
    }

    self->style.combo_border_color = (struct nk_color){r, g, b, a};
    return 0;
}

static PyObject *PyWindow_get_contextual_border_color(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("iiii", 
        self->style.contextual_border_color.r,
        self->style.contextual_border_color.g,
        self->style.contextual_border_color.b,
        self->style.contextual_border_color.a
    );
}

static int PyWindow_set_contextual_border_color(PyWindowObject *self, PyObject *value, void *closure)
{
    int r, g, b, a;
    if(!PyArg_ParseTuple(value, "iiii", &r, &g, &b, &a)) {
        PyErr_SetString(PyExc_TypeError, "Value must be a tuple of 4 integers (0-255).");
        return -1;
    }

    self->style.contextual_border_color = (struct nk_color){r, g, b, a};
    return 0;
}

static PyObject *PyWindow_get_menu_border_color(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("iiii", 
        self->style.menu_border_color.r,
        self->style.menu_border_color.g,
        self->style.menu_border_color.b,
        self->style.menu_border_color.a
    );
}

static int PyWindow_set_menu_border_color(PyWindowObject *self, PyObject *value, void *closure)
{
    int r, g, b, a;
    if(!PyArg_ParseTuple(value, "iiii", &r, &g, &b, &a)) {
        PyErr_SetString(PyExc_TypeError, "Value must be a tuple of 4 integers (0-255).");
        return -1;
    }

    self->style.menu_border_color = (struct nk_color){r, g, b, a};
    return 0;
}

static PyObject *PyWindow_get_group_border_color(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("iiii", 
        self->style.group_border_color.r,
        self->style.group_border_color.g,
        self->style.group_border_color.b,
        self->style.group_border_color.a
    );
}

static int PyWindow_set_group_border_color(PyWindowObject *self, PyObject *value, void *closure)
{
    int r, g, b, a;
    if(!PyArg_ParseTuple(value, "iiii", &r, &g, &b, &a)) {
        PyErr_SetString(PyExc_TypeError, "Value must be a tuple of 4 integers (0-255).");
        return -1;
    }

    self->style.group_border_color = (struct nk_color){r, g, b, a};
    return 0;
}

static PyObject *PyWindow_get_tooltip_border_color(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("iiii", 
        self->style.tooltip_border_color.r,
        self->style.tooltip_border_color.g,
        self->style.tooltip_border_color.b,
        self->style.tooltip_border_color.a
    );
}

static int PyWindow_set_tooltip_border_color(PyWindowObject *self, PyObject *value, void *closure)
{
    int r, g, b, a;
    if(!PyArg_ParseTuple(value, "iiii", &r, &g, &b, &a)) {
        PyErr_SetString(PyExc_TypeError, "Value must be a tuple of 4 integers (0-255).");
        return -1;
    }

    self->style.tooltip_border_color = (struct nk_color){r, g, b, a};
    return 0;
}

static PyObject *PyWindow_get_group_border(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("f", self->style.group_border);
}

static int PyWindow_set_group_border(PyWindowObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a float.");
        return -1;
    }

    self->style.group_border = PyFloat_AS_DOUBLE(value);
    return 0;
}

static PyObject *PyWindow_get_combo_border(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("f", self->style.combo_border);
}

static PyObject *PyWindow_get_contextual_border(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("f", self->style.contextual_border);
}

static int PyWindow_set_contextual_border(PyWindowObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a float.");
        return -1;
    }

    self->style.contextual_border = PyFloat_AS_DOUBLE(value);
    return 0;
}

static PyObject *PyWindow_get_menu_border(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("f", self->style.menu_border);
}

static int PyWindow_set_menu_border(PyWindowObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a float.");
        return -1;
    }

    self->style.menu_border = PyFloat_AS_DOUBLE(value);
    return 0;
}

static PyObject *PyWindow_get_tooltip_border(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("f", self->style.tooltip_border);
}

static int PyWindow_set_tooltip_border(PyWindowObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a float.");
        return -1;
    }

    self->style.group_border = PyFloat_AS_DOUBLE(value);
    return 0;
}

static PyObject *PyWindow_get_popup_border(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("f", self->style.popup_border);
}

static int PyWindow_set_popup_border(PyWindowObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a float.");
        return -1;
    }

    self->style.group_border = PyFloat_AS_DOUBLE(value);
    return 0;
}

static int PyWindow_set_combo_border(PyWindowObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a float.");
        return -1;
    }

    self->style.combo_border = PyFloat_AS_DOUBLE(value);
    return 0;
}

static PyObject *PyWindow_get_min_row_height_padding(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("f", self->style.min_row_height_padding);
}

static int PyWindow_set_min_row_height_padding(PyWindowObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be a float.");
        return -1;
    }

    self->style.min_row_height_padding = PyFloat_AS_DOUBLE(value);
    return 0;
}

static PyObject *PyWindow_get_scrollbar_size(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("(f, f)", 
        self->style.scrollbar_size.x, 
        self->style.scrollbar_size.y);
}

static int PyWindow_set_scrollbar_size(PyWindowObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style.scrollbar_size = nk_vec2(x,y);
    return 0;
}

static PyObject *PyWindow_get_min_size(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("(f, f)", 
        self->style.min_size.x, 
        self->style.min_size.y);
}

static int PyWindow_set_min_size(PyWindowObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style.min_size = nk_vec2(x,y);
    return 0;
}

static PyObject *PyWindow_get_closed(PyWindowObject *self, void *closure)
{
    if(self->flags & NK_WINDOW_CLOSED) 
        Py_RETURN_TRUE;
    else
        Py_RETURN_FALSE;
}

static PyObject *PyWindow_get_hidden(PyWindowObject *self, void *closure)
{
    if(self->flags & NK_WINDOW_HIDDEN) 
        Py_RETURN_TRUE;
    else
        Py_RETURN_FALSE;
}

static PyObject *PyWindow_get_interactive(PyWindowObject *self, void *closure)
{
    if(self->flags & NK_WINDOW_NOT_INTERACTIVE)
        Py_RETURN_FALSE;
    else
        Py_RETURN_TRUE;
}

static int PyWindow_set_interactive(PyWindowObject *self, PyObject *value, void *closure)
{
    if(PyObject_IsTrue(value))
        self->flags &= ~NK_WINDOW_NOT_INTERACTIVE;
    else
        self->flags |= NK_WINDOW_NOT_INTERACTIVE;
    return 0;
}

static PyObject *PyWindow_get_background(PyWindowObject *self, void *closure)
{
    return Py_BuildValue("iiii", 
        self->style.background.r,
        self->style.background.g,
        self->style.background.b,
        self->style.background.a
    );
}

static int PyWindow_set_background(PyWindowObject *self, PyObject *value, void *closure)
{
    int r, g, b, a;
    if(!PyArg_ParseTuple(value, "iiii", &r, &g, &b, &a)) {
        PyErr_SetString(PyExc_TypeError, "Value must be a tuple of 4 integers (0-255).");
        return -1;
    }

    self->style.background = (struct nk_color){r, g, b, a};
    return 0;
}

static PyObject *PyWindow_get_fixed_background(PyWindowObject *self, void *closure)
{
    if(self->style.fixed_background.type == NK_STYLE_ITEM_TEXPATH) {
        return PyString_FromString(self->style.fixed_background.data.texpath);
    }else{
        assert(self->style.fixed_background.type == NK_STYLE_ITEM_COLOR);
        struct nk_color clr = self->style.fixed_background.data.color;
        return Py_BuildValue("iiii", clr.r, clr.g, clr.b, clr.a);
    }
}

static int PyWindow_set_fixed_background(PyWindowObject *self, PyObject *value, void *closure)
{
    int r, g, b, a;

    if(PyTuple_Check(value)) {
    
        if(!PyArg_ParseTuple(value, "iiii", &r, &g, &b, &a)) {
            PyErr_SetString(PyExc_TypeError, "Value must be a tuple of 4 integers (0-255) or a path string.");
            return -1;
        }

        struct nk_color bg = (struct nk_color){r, g, b, a};
        self->style.fixed_background = (struct nk_style_item){
            .type = NK_STYLE_ITEM_COLOR, 
            .data.color = bg
        };

    }else if (PyString_Check(value)) {

        self->style.fixed_background = (struct nk_style_item){ .type = NK_STYLE_ITEM_TEXPATH };
        pf_strlcpy(self->style.fixed_background.data.texpath, PyString_AS_STRING(value), 
            sizeof(self->style.fixed_background.data.texpath));
    
    }else{
        PyErr_SetString(PyExc_TypeError, "Value must be a tuple of 4 integers (0-255) or a path string.");
        return -1;
    }

    return 0;
}

static void call_registered(PyObject *obj, char *method_name)
{
    PyObject *ret = PyObject_CallMethod(obj, method_name, NULL);
    if(!ret) {
        S_ShowLastError();
    }
    Py_XDECREF(ret);
}

static void call_registered_arg(PyObject *obj, char *method_name, PyObject *arg)
{
    PyObject *ret = PyObject_CallMethod(obj, method_name, "O", arg, NULL);
    if(!ret) {
        S_ShowLastError();
    }
    Py_XDECREF(ret);
}

static void active_windows_update(void *user, void *event)
{
    (void)user;
    (void)event;

    for(int i = 0; i < vec_size(&s_active_windows); i++) {
    
        PyWindowObject *win = vec_AT(&s_active_windows, i);
        if(win->flags & (NK_WINDOW_HIDDEN | NK_WINDOW_CLOSED))
            continue;

        bool interactive = !(win->flags & NK_WINDOW_NOT_INTERACTIVE);
        if(win->suspend_on_pause && G_GetSimState() != G_RUNNING)
            win->flags |= NK_WINDOW_NOT_INTERACTIVE;

        struct nk_style_window saved_style = s_nk_ctx->style.window;
        s_nk_ctx->style.window = win->style;
        if(win->header_style) {
            S_UIHeaderStylePush(win->header_style, s_nk_ctx);
        }

        struct nk_vec2i adj_vres = TO_VEC2I(UI_ArAdjustedVRes(TO_VEC2T(win->virt_res)));
        struct rect adj_bounds = UI_BoundsForAspectRatio(win->rect, 
            TO_VEC2T(win->virt_res), TO_VEC2T(adj_vres), win->resize_mask);

        if(nk_begin_with_vres(s_nk_ctx, win->name, 
            nk_rect(adj_bounds.x, adj_bounds.y, adj_bounds.w, adj_bounds.h), win->flags, adj_vres)) {

            call_registered((PyObject*)win, "update");
        }

        if(win->hide || (s_nk_ctx->current->flags & NK_WINDOW_HIDDEN && !(win->flags & NK_WINDOW_HIDDEN))) {

            PyObject *manual = win->hide ? Py_False : Py_True;
            Py_INCREF(manual);
            call_registered_arg((PyObject*)win, "on_hide", manual);
            Py_DECREF(manual);
        }

        if(s_nk_ctx->current->minimized) {
            call_registered((PyObject*)win, "on_minimize");
        }

        if(s_nk_ctx->current->maximized) {
            call_registered((PyObject*)win, "on_maximize");
        }

        struct nk_vec2 pos = nk_window_get_position(s_nk_ctx);
        struct nk_vec2 size = nk_window_get_size(s_nk_ctx);
        adj_bounds = (struct rect){pos.x, pos.y, size.x, size.y};
        win->rect = UI_BoundsForAspectRatio(adj_bounds, TO_VEC2T(adj_vres), TO_VEC2T(win->virt_res), win->resize_mask);

        int sample_mask = NK_WINDOW_HIDDEN | NK_WINDOW_CLOSED;
        win->flags &= ~sample_mask; 
        win->flags |= (s_nk_ctx->current->flags & sample_mask);

        if(interactive) {
            win->flags &= ~NK_WINDOW_NOT_INTERACTIVE;
        }

        nk_end(s_nk_ctx);
        if(win->header_style) {
            S_UIHeaderStylePop(win->header_style, s_nk_ctx);
        }
        s_nk_ctx->style.window = saved_style;

        if(win->hide) {
            nk_window_close(s_nk_ctx, win->name);
            win->flags |= (NK_WINDOW_HIDDEN | NK_WINDOW_CLOSED);
            win->hide = false;
        }
    }
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool S_UI_Init(struct nk_context *ctx)
{
    assert(ctx);
    s_nk_ctx = ctx;
    vec_win_init(&s_active_windows);

    if(!E_Global_Register(EVENT_UPDATE_UI, active_windows_update, NULL, G_RUNNING | G_PAUSED_UI_RUNNING))
        return false;
    return true;
}

void S_UI_Shutdown(void)
{
    E_Global_Unregister(EVENT_UPDATE_UI, active_windows_update);
    vec_win_destroy(&s_active_windows);
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
    int w, h;    
    Engine_WinDrawableSize(&w, &h);

    for(int i = 0; i < vec_size(&s_active_windows); i++) {

        PyWindowObject *win = vec_AT(&s_active_windows, i);
        struct rect adj_bounds = UI_BoundsForAspectRatio(win->rect, TO_VEC2T(win->virt_res),
            UI_ArAdjustedVRes(TO_VEC2T(win->virt_res)), win->resize_mask);
        struct nk_vec2 visible_size = {adj_bounds.w, adj_bounds.h};

        if(win->flags & NK_WINDOW_HIDDEN
        || win->flags & NK_WINDOW_CLOSED) {
            continue; 
        }

        int vmouse_x = (float)mouse_x / w * UI_ArAdjustedVRes(TO_VEC2T(win->virt_res)).x;
        int vmouse_y = (float)mouse_y / h * UI_ArAdjustedVRes(TO_VEC2T(win->virt_res)).y;

        /* For minimized windows, only the header is visible */
        struct nk_window *nkwin = nk_window_find(s_nk_ctx, win->name);
        if(nkwin && nkwin->flags & NK_WINDOW_MINIMIZED) {
        
            float header_height = S_UIHeaderGetHeight(win->header_style, s_nk_ctx);
            visible_size.y = header_height;
        }

        if(C_PointInsideRect2D(
            (vec2_t){vmouse_x,                      vmouse_y},
            (vec2_t){adj_bounds.x,                  adj_bounds.y},
            (vec2_t){adj_bounds.x + visible_size.x, adj_bounds.y},
            (vec2_t){adj_bounds.x + visible_size.x, adj_bounds.y + visible_size.y},
            (vec2_t){adj_bounds.x,                  adj_bounds.y + visible_size.y}))
            return true;
    }

    return false;
}

bool S_UI_TextEditHasFocus(void)
{
    for(int i = 0; i < vec_size(&s_active_windows); i++) {

        PyWindowObject *win = vec_AT(&s_active_windows, i);

        if(win->flags & NK_WINDOW_HIDDEN
        || win->flags & NK_WINDOW_CLOSED) {
            continue; 
        }

        struct nk_window *nkwin = nk_window_find(s_nk_ctx, win->name);
        if(!nkwin)
            continue;

        if(nkwin->edit.active == nk_true)
            return true;
    }

    return false;
}

PyObject *S_UI_ActiveWindow(void)
{
    for(int i = 0; i < vec_size(&s_active_windows); i++) {

        PyWindowObject *win = vec_AT(&s_active_windows, i);

        if(win->flags & NK_WINDOW_HIDDEN
        || win->flags & NK_WINDOW_CLOSED) {
            continue; 
        }

        struct nk_window *nkwin = nk_window_find(s_nk_ctx, win->name);
        if(!nkwin)
            continue;

        if(nkwin->edit.active == nk_true) {
            Py_INCREF(win);
            return (PyObject*)win;
        }
    }
    return NULL;
}

