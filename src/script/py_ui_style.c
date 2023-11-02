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
    enum button_type{
        BUTTON_REGULAR,
        BUTTON_CONTEXTUAL,
        BUTTON_MENU,
    }type;
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

static PyObject *PyUIButtonStyle_pickle(PyUIButtonStyleObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyUIButtonStyle_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs);

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

static PyObject *PyUIHeaderStyle_pickle(PyUIHeaderStyleObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyUIHeaderStyle_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs);

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

static PyObject *PyUISelectableStyle_pickle(PyUISelectableStyleObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyUISelectableStyle_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs);

typedef struct {
    PyObject_HEAD
    struct nk_style_combo *style;
    PyUIButtonStyleObject *button; 
}PyUIComboStyleObject;

static PyObject *PyUIComboStyle_new(PyTypeObject *type, PyObject *args, PyObject *kwds);
static void      PyUIComboStyle_dealloc(PyUIComboStyleObject *self);

/* background */
static PyObject *PyUIComboStyle_get_normal(PyUIComboStyleObject *self, void *);
static int       PyUIComboStyle_set_normal(PyUIComboStyleObject *self, PyObject *value, void *);
static PyObject *PyUIComboStyle_get_hover(PyUIComboStyleObject *self, void *);
static int       PyUIComboStyle_set_hover(PyUIComboStyleObject *self, PyObject *value, void *);
static PyObject *PyUIComboStyle_get_active(PyUIComboStyleObject *self, void *);
static int       PyUIComboStyle_set_active(PyUIComboStyleObject *self, PyObject *value, void *);
static PyObject *PyUIComboStyle_get_border_color(PyUIComboStyleObject *self, void *);
static int       PyUIComboStyle_set_border_color(PyUIComboStyleObject *self, PyObject *value, void *);

/* label */
static PyObject *PyUIComboStyle_get_label_normal(PyUIComboStyleObject *self, void *);
static int       PyUIComboStyle_set_label_normal(PyUIComboStyleObject *self, PyObject *value, void *);
static PyObject *PyUIComboStyle_get_label_hover(PyUIComboStyleObject *self, void *);
static int       PyUIComboStyle_set_label_hover(PyUIComboStyleObject *self, PyObject *value, void *);
static PyObject *PyUIComboStyle_get_label_active(PyUIComboStyleObject *self, void *);
static int       PyUIComboStyle_set_label_active(PyUIComboStyleObject *self, PyObject *value, void *);

/* symbol */
static PyObject *PyUIComboStyle_get_symbol_normal(PyUIComboStyleObject *self, void *);
static int       PyUIComboStyle_set_symbol_normal(PyUIComboStyleObject *self, PyObject *value, void *);
static PyObject *PyUIComboStyle_get_symbol_hover(PyUIComboStyleObject *self, void *);
static int       PyUIComboStyle_set_symbol_hover(PyUIComboStyleObject *self, PyObject *value, void *);
static PyObject *PyUIComboStyle_get_symbol_active(PyUIComboStyleObject *self, void *);
static int       PyUIComboStyle_set_symbol_active(PyUIComboStyleObject *self, PyObject *value, void *);

/* button */
static PyObject *PyUIComboStyle_get_button(PyUIComboStyleObject *self, void *);
static PyObject *PyUIComboStyle_get_sym_normal(PyUIComboStyleObject *self, void *);
static int       PyUIComboStyle_set_sym_normal(PyUIComboStyleObject *self, PyObject *value, void *);
static PyObject *PyUIComboStyle_get_sym_hover(PyUIComboStyleObject *self, void *);
static int       PyUIComboStyle_set_sym_hover(PyUIComboStyleObject *self, PyObject *value, void *);
static PyObject *PyUIComboStyle_get_sym_active(PyUIComboStyleObject *self, void *);
static int       PyUIComboStyle_set_sym_active(PyUIComboStyleObject *self, PyObject *value, void *);

/* properties */
static PyObject *PyUIComboStyle_get_border(PyUIComboStyleObject *self, void *);
static int       PyUIComboStyle_set_border(PyUIComboStyleObject *self, PyObject *value, void *);
static PyObject *PyUIComboStyle_get_rounding(PyUIComboStyleObject *self, void *);
static int       PyUIComboStyle_set_rounding(PyUIComboStyleObject *self, PyObject *value, void *);
static PyObject *PyUIComboStyle_get_content_padding(PyUIComboStyleObject *self, void *);
static int       PyUIComboStyle_set_content_padding(PyUIComboStyleObject *self, PyObject *value, void *);
static PyObject *PyUIComboStyle_get_button_padding(PyUIComboStyleObject *self, void *);
static int       PyUIComboStyle_set_button_padding(PyUIComboStyleObject *self, PyObject *value, void *);
static PyObject *PyUIComboStyle_get_spacing(PyUIComboStyleObject *self, void *);
static int       PyUIComboStyle_set_spacing(PyUIComboStyleObject *self, PyObject *value, void *);

static PyObject *PyUIComboStyle_pickle(PyUIComboStyleObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyUIComboStyle_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs);

typedef struct {
    PyObject_HEAD
    enum toggle_type{
        TOGGLE_OPTION,
        TOGGLE_CHECKBOX,
    }type;
    struct nk_style_toggle *style;
}PyUIToggleStyleObject;

/* background */
static PyObject *PyUIToggleStyle_get_normal(PyUIToggleStyleObject *self, void *);
static int       PyUIToggleStyle_set_normal(PyUIToggleStyleObject *self, PyObject *value, void *);
static PyObject *PyUIToggleStyle_get_hover(PyUIToggleStyleObject *self, void *);
static int       PyUIToggleStyle_set_hover(PyUIToggleStyleObject *self, PyObject *value, void *);
static PyObject *PyUIToggleStyle_get_active(PyUIToggleStyleObject *self, void *);
static int       PyUIToggleStyle_set_active(PyUIToggleStyleObject *self, PyObject *value, void *);
static PyObject *PyUIToggleStyle_get_border_color(PyUIToggleStyleObject *self, void *);
static int       PyUIToggleStyle_set_border_color(PyUIToggleStyleObject *self, PyObject *value, void *);

/* cursor */
static PyObject *PyUIToggleStyle_get_cursor_normal(PyUIToggleStyleObject *self, void *);
static int       PyUIToggleStyle_set_cursor_normal(PyUIToggleStyleObject *self, PyObject *value, void *);
static PyObject *PyUIToggleStyle_get_cursor_hover(PyUIToggleStyleObject *self, void *);
static int       PyUIToggleStyle_set_cursor_hover(PyUIToggleStyleObject *self, PyObject *value, void *);

/* text */
static PyObject *PyUIToggleStyle_get_text_normal(PyUIToggleStyleObject *self, void *);
static int       PyUIToggleStyle_set_text_normal(PyUIToggleStyleObject *self, PyObject *value, void *);
static PyObject *PyUIToggleStyle_get_text_hover(PyUIToggleStyleObject *self, void *);
static int       PyUIToggleStyle_set_text_hover(PyUIToggleStyleObject *self, PyObject *value, void *);
static PyObject *PyUIToggleStyle_get_text_active(PyUIToggleStyleObject *self, void *);
static int       PyUIToggleStyle_set_text_active(PyUIToggleStyleObject *self, PyObject *value, void *);
static PyObject *PyUIToggleStyle_get_text_background(PyUIToggleStyleObject *self, void *);
static int       PyUIToggleStyle_set_text_background(PyUIToggleStyleObject *self, PyObject *value, void *);
static PyObject *PyUIToggleStyle_get_text_alignment(PyUIToggleStyleObject *self, void *);
static int       PyUIToggleStyle_set_text_alignment(PyUIToggleStyleObject *self, PyObject *value, void *);

/* properties */
static PyObject *PyUIToggleStyle_get_padding(PyUIToggleStyleObject *self, void *);
static int       PyUIToggleStyle_set_padding(PyUIToggleStyleObject *self, PyObject *value, void *);
static PyObject *PyUIToggleStyle_get_touch_padding(PyUIToggleStyleObject *self, void *);
static int       PyUIToggleStyle_set_touch_padding(PyUIToggleStyleObject *self, PyObject *value, void *);
static PyObject *PyUIToggleStyle_get_spacing(PyUIToggleStyleObject *self, void *);
static int       PyUIToggleStyle_set_spacing(PyUIToggleStyleObject *self, PyObject *value, void *);
static PyObject *PyUIToggleStyle_get_border(PyUIToggleStyleObject *self, void *);
static int       PyUIToggleStyle_set_border(PyUIToggleStyleObject *self, PyObject *value, void *);

static PyObject *PyUIToggleStyle_pickle(PyUIToggleStyleObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyUIToggleStyle_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs);

typedef struct {
    PyObject_HEAD
    enum scrollbar_type{
        SCROLLBAR_HORIZONTAL,
        SCROLLBAR_VERTICAL,
        SCROLLBAR_EDIT,
        SCROLLBAR_PROPERTY
    }type;
    struct nk_style_scrollbar *style;
}PyUIScrollbarStyleObject;

/* background */
static PyObject *PyUIScrollbarStyle_get_normal(PyUIScrollbarStyleObject *self, void *);
static int       PyUIScrollbarStyle_set_normal(PyUIScrollbarStyleObject *self, PyObject *value, void *);
static PyObject *PyUIScrollbarStyle_get_hover(PyUIScrollbarStyleObject *self, void *);
static int       PyUIScrollbarStyle_set_hover(PyUIScrollbarStyleObject *self, PyObject *value, void *);
static PyObject *PyUIScrollbarStyle_get_active(PyUIScrollbarStyleObject *self, void *);
static int       PyUIScrollbarStyle_set_active(PyUIScrollbarStyleObject *self, PyObject *value, void *);
static PyObject *PyUIScrollbarStyle_get_border_color(PyUIScrollbarStyleObject *self, void *);
static int       PyUIScrollbarStyle_set_border_color(PyUIScrollbarStyleObject *self, PyObject *value, void *);

/* cursor */
static PyObject *PyUIScrollbarStyle_get_cursor_normal(PyUIScrollbarStyleObject *self, void *);
static int       PyUIScrollbarStyle_set_cursor_normal(PyUIScrollbarStyleObject *self, PyObject *value, void *);
static PyObject *PyUIScrollbarStyle_get_cursor_hover(PyUIScrollbarStyleObject *self, void *);
static int       PyUIScrollbarStyle_set_cursor_hover(PyUIScrollbarStyleObject *self, PyObject *value, void *);
static PyObject *PyUIScrollbarStyle_get_cursor_active(PyUIScrollbarStyleObject *self, void *);
static int       PyUIScrollbarStyle_set_cursor_active(PyUIScrollbarStyleObject *self, PyObject *value, void *);
static PyObject *PyUIScrollbarStyle_get_cursor_border_color(PyUIScrollbarStyleObject *self, void *);
static int       PyUIScrollbarStyle_set_cursor_border_color(PyUIScrollbarStyleObject *self, PyObject *value, void *);

/* properties */
static PyObject *PyUIScrollbarStyle_get_border(PyUIScrollbarStyleObject *self, void *);
static int       PyUIScrollbarStyle_set_border(PyUIScrollbarStyleObject *self, PyObject *value, void *);
static PyObject *PyUIScrollbarStyle_get_rounding(PyUIScrollbarStyleObject *self, void *);
static int       PyUIScrollbarStyle_set_rounding(PyUIScrollbarStyleObject *self, PyObject *value, void *);
static PyObject *PyUIScrollbarStyle_get_border_cursor(PyUIScrollbarStyleObject *self, void *);
static int       PyUIScrollbarStyle_set_border_cursor(PyUIScrollbarStyleObject *self, PyObject *value, void *);
static PyObject *PyUIScrollbarStyle_get_rounding_cursor(PyUIScrollbarStyleObject *self, void *);
static int       PyUIScrollbarStyle_set_rounding_cursor(PyUIScrollbarStyleObject *self, PyObject *value, void *);
static PyObject *PyUIScrollbarStyle_get_padding(PyUIScrollbarStyleObject *self, void *);
static int       PyUIScrollbarStyle_set_padding(PyUIScrollbarStyleObject *self, PyObject *value, void *);

static PyObject *PyUIScrollbarStyle_pickle(PyUIScrollbarStyleObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyUIScrollbarStyle_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs);

typedef struct {
    PyObject_HEAD
    enum edit_type{
        EDIT_REGULAR,
        EDIT_PROPERTY,
    }type;
    struct nk_style_edit *style;
    PyUIScrollbarStyleObject *scrollbar;
}PyUIEditStyleObject;

static PyObject *PyUIEditStyle_new(PyTypeObject *type, PyObject *args, PyObject *kwds);
static void      PyUIEditStyle_dealloc(PyUIEditStyleObject *self);

/* background */
static PyObject *PyUIEditStyle_get_normal(PyUIEditStyleObject *self, void *);
static int       PyUIEditStyle_set_normal(PyUIEditStyleObject *self, PyObject *value, void *);
static PyObject *PyUIEditStyle_get_hover(PyUIEditStyleObject *self, void *);
static int       PyUIEditStyle_set_hover(PyUIEditStyleObject *self, PyObject *value, void *);
static PyObject *PyUIEditStyle_get_active(PyUIEditStyleObject *self, void *);
static int       PyUIEditStyle_set_active(PyUIEditStyleObject *self, PyObject *value, void *);
static PyObject *PyUIEditStyle_get_border_color(PyUIEditStyleObject *self, void *);
static int       PyUIEditStyle_set_border_color(PyUIEditStyleObject *self, PyObject *value, void *);
static PyObject *PyUIEditStyle_get_scrollbar(PyUIEditStyleObject *self, void *);

/* cursor */
static PyObject *PyUIEditStyle_get_cursor_normal(PyUIEditStyleObject *self, void *);
static int       PyUIEditStyle_set_cursor_normal(PyUIEditStyleObject *self, PyObject *value, void *);
static PyObject *PyUIEditStyle_get_cursor_hover(PyUIEditStyleObject *self, void *);
static int       PyUIEditStyle_set_cursor_hover(PyUIEditStyleObject *self, PyObject *value, void *);
static PyObject *PyUIEditStyle_get_cursor_text_normal(PyUIEditStyleObject *self, void *);
static int       PyUIEditStyle_set_cursor_text_normal(PyUIEditStyleObject *self, PyObject *value, void *);
static PyObject *PyUIEditStyle_get_cursor_text_hover(PyUIEditStyleObject *self, void *);
static int       PyUIEditStyle_set_cursor_text_hover(PyUIEditStyleObject *self, PyObject *value, void *);

/* text (unselected) */
static PyObject *PyUIEditStyle_get_text_normal(PyUIEditStyleObject *self, void *);
static int       PyUIEditStyle_set_text_normal(PyUIEditStyleObject *self, PyObject *value, void *);
static PyObject *PyUIEditStyle_get_text_hover(PyUIEditStyleObject *self, void *);
static int       PyUIEditStyle_set_text_hover(PyUIEditStyleObject *self, PyObject *value, void *);
static PyObject *PyUIEditStyle_get_text_active(PyUIEditStyleObject *self, void *);
static int       PyUIEditStyle_set_text_active(PyUIEditStyleObject *self, PyObject *value, void *);

/* text (selected) */
static PyObject *PyUIEditStyle_get_selected_normal(PyUIEditStyleObject *self, void *);
static int       PyUIEditStyle_set_selected_normal(PyUIEditStyleObject *self, PyObject *value, void *);
static PyObject *PyUIEditStyle_get_selected_hover(PyUIEditStyleObject *self, void *);
static int       PyUIEditStyle_set_selected_hover(PyUIEditStyleObject *self, PyObject *value, void *);
static PyObject *PyUIEditStyle_get_selected_text_normal(PyUIEditStyleObject *self, void *);
static int       PyUIEditStyle_set_selected_text_normal(PyUIEditStyleObject *self, PyObject *value, void *);
static PyObject *PyUIEditStyle_get_selected_text_hover(PyUIEditStyleObject *self, void *);
static int       PyUIEditStyle_set_selected_text_hover(PyUIEditStyleObject *self, PyObject *value, void *);

/* properties */
static PyObject *PyUIEditStyle_get_border(PyUIEditStyleObject *self, void *);
static int       PyUIEditStyle_set_border(PyUIEditStyleObject *self, PyObject *value, void *);
static PyObject *PyUIEditStyle_get_rounding(PyUIEditStyleObject *self, void *);
static int       PyUIEditStyle_set_rounding(PyUIEditStyleObject *self, PyObject *value, void *);
static PyObject *PyUIEditStyle_get_cursor_size(PyUIEditStyleObject *self, void *);
static int       PyUIEditStyle_set_cursor_size(PyUIEditStyleObject *self, PyObject *value, void *);
static PyObject *PyUIEditStyle_get_scrollbar_size(PyUIEditStyleObject *self, void *);
static int       PyUIEditStyle_set_scrollbar_size(PyUIEditStyleObject *self, PyObject *value, void *);
static PyObject *PyUIEditStyle_get_padding(PyUIEditStyleObject *self, void *);
static int       PyUIEditStyle_set_padding(PyUIEditStyleObject *self, PyObject *value, void *);
static PyObject *PyUIEditStyle_get_row_padding(PyUIEditStyleObject *self, void *);
static int       PyUIEditStyle_set_row_padding(PyUIEditStyleObject *self, PyObject *value, void *);

static PyObject *PyUIEditStyle_pickle(PyUIEditStyleObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyUIEditStyle_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs);

typedef struct {
    PyObject_HEAD
    struct nk_style_property *style;
    PyUIEditStyleObject *edit;
    PyUIButtonStyleObject *inc_button;
    PyUIButtonStyleObject *dec_button;
}PyUIPropertyStyleObject;

static PyObject *PyUIPropertyStyle_new(PyTypeObject *type, PyObject *args, PyObject *kwds);
static void      PyUIPropertyStyle_dealloc(PyUIPropertyStyleObject *self);

/* background */
static PyObject *PyUIPropertyStyle_get_normal(PyUIPropertyStyleObject *self, void *);
static int       PyUIPropertyStyle_set_normal(PyUIPropertyStyleObject *self, PyObject *value, void *);
static PyObject *PyUIPropertyStyle_get_hover(PyUIPropertyStyleObject *self, void *);
static int       PyUIPropertyStyle_set_hover(PyUIPropertyStyleObject *self, PyObject *value, void *);
static PyObject *PyUIPropertyStyle_get_active(PyUIPropertyStyleObject *self, void *);
static int       PyUIPropertyStyle_set_active(PyUIPropertyStyleObject *self, PyObject *value, void *);
static PyObject *PyUIPropertyStyle_get_border_color(PyUIPropertyStyleObject *self, void *);
static int       PyUIPropertyStyle_set_border_color(PyUIPropertyStyleObject *self, PyObject *value, void *);

/* text */
static PyObject *PyUIPropertyStyle_get_label_normal(PyUIPropertyStyleObject *self, void *);
static int       PyUIPropertyStyle_set_label_normal(PyUIPropertyStyleObject *self, PyObject *value, void *);
static PyObject *PyUIPropertyStyle_get_label_hover(PyUIPropertyStyleObject *self, void *);
static int       PyUIPropertyStyle_set_label_hover(PyUIPropertyStyleObject *self, PyObject *value, void *);
static PyObject *PyUIPropertyStyle_get_label_active(PyUIPropertyStyleObject *self, void *);
static int       PyUIPropertyStyle_set_label_active(PyUIPropertyStyleObject *self, PyObject *value, void *);

/* symbols */
static PyObject *PyUIPropertyStyle_get_sym_left(PyUIPropertyStyleObject *self, void *);
static int       PyUIPropertyStyle_set_sym_left(PyUIPropertyStyleObject *self, PyObject *value, void *);
static PyObject *PyUIPropertyStyle_get_sym_right(PyUIPropertyStyleObject *self, void *);
static int       PyUIPropertyStyle_set_sym_right(PyUIPropertyStyleObject *self, PyObject *value, void *);

/* properties */
static PyObject *PyUIPropertyStyle_get_border(PyUIPropertyStyleObject *self, void *);
static int       PyUIPropertyStyle_set_border(PyUIPropertyStyleObject *self, PyObject *value, void *);
static PyObject *PyUIPropertyStyle_get_rounding(PyUIPropertyStyleObject *self, void *);
static int       PyUIPropertyStyle_set_rounding(PyUIPropertyStyleObject *self, PyObject *value, void *);
static PyObject *PyUIPropertyStyle_get_padding(PyUIPropertyStyleObject *self, void *);
static int       PyUIPropertyStyle_set_padding(PyUIPropertyStyleObject *self, PyObject *value, void *);
static PyObject *PyUIPropertyStyle_get_edit(PyUIPropertyStyleObject *self, void *);
static PyObject *PyUIPropertyStyle_get_inc_button(PyUIPropertyStyleObject *self, void *);
static PyObject *PyUIPropertyStyle_get_dec_button(PyUIPropertyStyleObject *self, void *);

static PyObject *PyUIPropertyStyle_pickle(PyUIPropertyStyleObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyUIPropertyStyle_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs);

typedef struct {
    PyObject_HEAD
    struct nk_style_slider *style;
    PyUIButtonStyleObject *inc_button;
    PyUIButtonStyleObject *dec_button;
}PyUISliderStyleObject;

static PyObject *PyUISliderStyle_new(PyTypeObject *type, PyObject *args, PyObject *kwds);
static void      PyUISliderStyle_dealloc(PyUISliderStyleObject *self);

/* background */
static PyObject *PyUISliderStyle_get_normal(PyUISliderStyleObject *self, void *);
static int       PyUISliderStyle_set_normal(PyUISliderStyleObject *self, PyObject *value, void *);
static PyObject *PyUISliderStyle_get_hover(PyUISliderStyleObject *self, void *);
static int       PyUISliderStyle_set_hover(PyUISliderStyleObject *self, PyObject *value, void *);
static PyObject *PyUISliderStyle_get_active(PyUISliderStyleObject *self, void *);
static int       PyUISliderStyle_set_active(PyUISliderStyleObject *self, PyObject *value, void *);
static PyObject *PyUISliderStyle_get_border_color(PyUISliderStyleObject *self, void *);
static int       PyUISliderStyle_set_border_color(PyUISliderStyleObject *self, PyObject *value, void *);

/* background bar */
static PyObject *PyUISliderStyle_get_bar_normal(PyUISliderStyleObject *self, void *);
static int       PyUISliderStyle_set_bar_normal(PyUISliderStyleObject *self, PyObject *value, void *);
static PyObject *PyUISliderStyle_get_bar_hover(PyUISliderStyleObject *self, void *);
static int       PyUISliderStyle_set_bar_hover(PyUISliderStyleObject *self, PyObject *value, void *);
static PyObject *PyUISliderStyle_get_bar_active(PyUISliderStyleObject *self, void *);
static int       PyUISliderStyle_set_bar_active(PyUISliderStyleObject *self, PyObject *value, void *);
static PyObject *PyUISliderStyle_get_bar_filled(PyUISliderStyleObject *self, void *);
static int       PyUISliderStyle_set_bar_filled(PyUISliderStyleObject *self, PyObject *value, void *);

/* cursor */
static PyObject *PyUISliderStyle_get_cursor_normal(PyUISliderStyleObject *self, void *);
static int       PyUISliderStyle_set_cursor_normal(PyUISliderStyleObject *self, PyObject *value, void *);
static PyObject *PyUISliderStyle_get_cursor_hover(PyUISliderStyleObject *self, void *);
static int       PyUISliderStyle_set_cursor_hover(PyUISliderStyleObject *self, PyObject *value, void *);
static PyObject *PyUISliderStyle_get_cursor_active(PyUISliderStyleObject *self, void *);
static int       PyUISliderStyle_set_cursor_active(PyUISliderStyleObject *self, PyObject *value, void *);

/* properties */
static PyObject *PyUISliderStyle_get_border(PyUISliderStyleObject *self, void *);
static int       PyUISliderStyle_set_border(PyUISliderStyleObject *self, PyObject *value, void *);
static PyObject *PyUISliderStyle_get_rounding(PyUISliderStyleObject *self, void *);
static int       PyUISliderStyle_set_rounding(PyUISliderStyleObject *self, PyObject *value, void *);
static PyObject *PyUISliderStyle_get_bar_height(PyUISliderStyleObject *self, void *);
static int       PyUISliderStyle_set_bar_height(PyUISliderStyleObject *self, PyObject *value, void *);
static PyObject *PyUISliderStyle_get_padding(PyUISliderStyleObject *self, void *);
static int       PyUISliderStyle_set_padding(PyUISliderStyleObject *self, PyObject *value, void *);
static PyObject *PyUISliderStyle_get_spacing(PyUISliderStyleObject *self, void *);
static int       PyUISliderStyle_set_spacing(PyUISliderStyleObject *self, PyObject *value, void *);
static PyObject *PyUISliderStyle_get_cursor_size(PyUISliderStyleObject *self, void *);
static int       PyUISliderStyle_set_cursor_size(PyUISliderStyleObject *self, PyObject *value, void *);

/* optional buttons */
static PyObject *PyUISliderStyle_get_show_buttons(PyUISliderStyleObject *self, void *);
static int       PyUISliderStyle_set_show_buttons(PyUISliderStyleObject *self, PyObject *value, void *);
static PyObject *PyUISliderStyle_get_inc_button(PyUISliderStyleObject *self, void *);
static PyObject *PyUISliderStyle_get_dec_button(PyUISliderStyleObject *self, void *);
static PyObject *PyUISliderStyle_get_inc_symbol(PyUISliderStyleObject *self, void *);
static int       PyUISliderStyle_set_inc_symbol(PyUISliderStyleObject *self, PyObject *value, void *);
static PyObject *PyUISliderStyle_get_dec_symbol(PyUISliderStyleObject *self, void *);
static int       PyUISliderStyle_set_dec_symbol(PyUISliderStyleObject *self, PyObject *value, void *);

static PyObject *PyUISliderStyle_pickle(PyUISliderStyleObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyUISliderStyle_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs);

typedef struct {
    PyObject_HEAD
    struct nk_style_progress *style;
}PyUIProgressStyleObject;

/* background */
static PyObject *PyUIProgressStyle_get_normal(PyUIProgressStyleObject *self, void *);
static int       PyUIProgressStyle_set_normal(PyUIProgressStyleObject *self, PyObject *value, void *);
static PyObject *PyUIProgressStyle_get_hover(PyUIProgressStyleObject *self, void *);
static int       PyUIProgressStyle_set_hover(PyUIProgressStyleObject *self, PyObject *value, void *);
static PyObject *PyUIProgressStyle_get_active(PyUIProgressStyleObject *self, void *);
static int       PyUIProgressStyle_set_active(PyUIProgressStyleObject *self, PyObject *value, void *);
static PyObject *PyUIProgressStyle_get_border_color(PyUIProgressStyleObject *self, void *);
static int       PyUIProgressStyle_set_border_color(PyUIProgressStyleObject *self, PyObject *value, void *);

/* cursor */
static PyObject *PyUIProgressStyle_get_cursor_normal(PyUIProgressStyleObject *self, void *);
static int       PyUIProgressStyle_set_cursor_normal(PyUIProgressStyleObject *self, PyObject *value, void *);
static PyObject *PyUIProgressStyle_get_cursor_hover(PyUIProgressStyleObject *self, void *);
static int       PyUIProgressStyle_set_cursor_hover(PyUIProgressStyleObject *self, PyObject *value, void *);
static PyObject *PyUIProgressStyle_get_cursor_active(PyUIProgressStyleObject *self, void *);
static int       PyUIProgressStyle_set_cursor_active(PyUIProgressStyleObject *self, PyObject *value, void *);
static PyObject *PyUIProgressStyle_get_cursor_border_color(PyUIProgressStyleObject *self, void *);
static int       PyUIProgressStyle_set_cursor_border_color(PyUIProgressStyleObject *self, PyObject *value, void *);

/* properties */
static PyObject *PyUIProgressStyle_get_rounding(PyUIProgressStyleObject *self, void *);
static int       PyUIProgressStyle_set_rounding(PyUIProgressStyleObject *self, PyObject *value, void *);
static PyObject *PyUIProgressStyle_get_border(PyUIProgressStyleObject *self, void *);
static int       PyUIProgressStyle_set_border(PyUIProgressStyleObject *self, PyObject *value, void *);
static PyObject *PyUIProgressStyle_get_cursor_border(PyUIProgressStyleObject *self, void *);
static int       PyUIProgressStyle_set_cursor_border(PyUIProgressStyleObject *self, PyObject *value, void *);
static PyObject *PyUIProgressStyle_get_cursor_rounding(PyUIProgressStyleObject *self, void *);
static int       PyUIProgressStyle_set_cursor_rounding(PyUIProgressStyleObject *self, PyObject *value, void *);
static PyObject *PyUIProgressStyle_get_padding(PyUIProgressStyleObject *self, void *);
static int       PyUIProgressStyle_set_padding(PyUIProgressStyleObject *self, PyObject *value, void *);

static PyObject *PyUIProgressStyle_pickle(PyUIProgressStyleObject *self, PyObject *args, PyObject *kwargs);
static PyObject *PyUIProgressStyle_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs);

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static PyMethodDef PyUIButtonStyle_methods[] = {
    {"__pickle__", 
    (PyCFunction)PyUIButtonStyle_pickle, METH_KEYWORDS,
    "Serialize a Permafrost Engine UIButtonStyle object to a string."},

    {"__unpickle__", 
    (PyCFunction)PyUIButtonStyle_unpickle, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
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
    (PyCFunction)PyUIHeaderStyle_pickle, METH_KEYWORDS,
    "Serialize a Permafrost Engine UIButtonStyle object to a string."},

    {"__unpickle__", 
    (PyCFunction)PyUIHeaderStyle_unpickle, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
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
    (PyCFunction)PyUISelectableStyle_pickle, METH_KEYWORDS,
    "Serialize a Permafrost Engine UISelectableStyle object to a string."},

    {"__unpickle__", 
    (PyCFunction)PyUISelectableStyle_unpickle, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
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

static PyMethodDef PyUIComboStyle_methods[] = {
    {"__pickle__", 
    (PyCFunction)PyUIComboStyle_pickle, METH_KEYWORDS,
    "Serialize a Permafrost Engine UIComboStyle object to a string."},

    {"__unpickle__", 
    (PyCFunction)PyUIComboStyle_unpickle, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
    "Create a new pf.UIComboStyle instance from a string earlier returned from a __pickle__ method."
    "Returns a tuple of the new instance and the number of bytes consumed from the stream."},

    {NULL}  /* Sentinel */
};

static PyGetSetDef PyUIComboStyle_getset[] = {
    {"normal",
    (getter)PyUIComboStyle_get_normal, 
    (setter)PyUIComboStyle_set_normal,
    "The look of the combo element in the normal state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"hover",
    (getter)PyUIComboStyle_get_hover, 
    (setter)PyUIComboStyle_set_hover,
    "The look of the combo element in the hovered state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"active",
    (getter)PyUIComboStyle_get_active, 
    (setter)PyUIComboStyle_set_active,
    "The look of the combo element in the active state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"border_color",
    (getter)PyUIComboStyle_get_border_color, 
    (setter)PyUIComboStyle_set_border_color,
    "The color of the combo box border - an (R, G, B, A) tuple.",
    NULL},

    {"label_normal",
    (getter)PyUIComboStyle_get_label_normal, 
    (setter)PyUIComboStyle_set_label_normal,
    "The color of the combo item label in the normal state - an (R, G, B, A) tuple.",
    NULL},

    {"label_hover",
    (getter)PyUIComboStyle_get_label_hover, 
    (setter)PyUIComboStyle_set_label_hover,
    "The color of the combo item label in the hovered state - an (R, G, B, A) tuple",
    NULL},

    {"label_active",
    (getter)PyUIComboStyle_get_label_active, 
    (setter)PyUIComboStyle_set_label_active,
    "The color of the combo item label in the active state - an (R, G, B, A) tuple",
    NULL},

    {"symbol_normal",
    (getter)PyUIComboStyle_get_symbol_normal, 
    (setter)PyUIComboStyle_set_symbol_normal,
    "The color of the combo symbol in the normal state - an (R, G, B, A) tuple.",
    NULL},

    {"symbol_hover",
    (getter)PyUIComboStyle_get_symbol_hover, 
    (setter)PyUIComboStyle_set_symbol_hover,
    "The color of the combo symbol in the hovered state - an (R, G, B, A) tuple",
    NULL},

    {"symbol_active",
    (getter)PyUIComboStyle_get_symbol_active, 
    (setter)PyUIComboStyle_set_symbol_active,
    "The color of the combo symbol in the active state - an (R, G, B, A) tuple",
    NULL},

    {"button",
    (getter)PyUIComboStyle_get_button, NULL,
    "A pf.UIButtonStyle object describing the style of the combo box drop-down button.",
    NULL},

    {"sym_normal",
    (getter)PyUIComboStyle_get_sym_normal, 
    (setter)PyUIComboStyle_set_sym_normal,
    "The type of the combo box drop-down glyph in the normal state - an integer (pf.NK_SYMBOL_X, etc.)",
    NULL},

    {"sym_hover",
    (getter)PyUIComboStyle_get_sym_hover, 
    (setter)PyUIComboStyle_set_sym_hover,
    "The type of the combo box drop-down glyph in the hovered state - an integer (pf.NK_SYMBOL_X, etc.)",
    NULL},

    {"sym_active",
    (getter)PyUIComboStyle_get_sym_active, 
    (setter)PyUIComboStyle_set_sym_active,
    "The type of the combo box drop-down glyph in the active state - an integer (pf.NK_SYMBOL_X, etc.)",
    NULL},

    {"border",
    (getter)PyUIComboStyle_get_rounding, 
    (setter)PyUIComboStyle_set_rounding,
    "A floating-point value to control width of the combo box border.", 
    NULL},

    {"rounding",
    (getter)PyUIComboStyle_get_rounding, 
    (setter)PyUIComboStyle_set_rounding,
    "A floating-point value to control how rounded the selectable label corners are.", 
    NULL},

    {"content_padding",
    (getter)PyUIComboStyle_get_content_padding, 
    (setter)PyUIComboStyle_set_content_padding,
    "An (X, Y) tuple of floats to control the padding around combo box contents.", 
    NULL},

    {"button_padding",
    (getter)PyUIComboStyle_get_button_padding, 
    (setter)PyUIComboStyle_set_button_padding,
    "An (X, Y) tuple of floats to control the padding around combo box drop-down buttons.", 
    NULL},

    {"spacing",
    (getter)PyUIComboStyle_get_spacing, 
    (setter)PyUIComboStyle_set_spacing,
    "An (X, Y) tuple of floats to control the spacing in between combo box elements.", 
    NULL},

    {NULL}  /* Sentinel */
};

static PyTypeObject PyUIComboStyle_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "pf.UIComboStyle",        /* tp_name */
    sizeof(PyUIComboStyleObject), /* tp_basicsize */
    0,                         /* tp_itemsize */
    (destructor)PyUIComboStyle_dealloc, /* tp_dealloc */
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
    "Style configuration for Permafrost Engine combo box UI elements.", /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    PyUIComboStyle_methods, /* tp_methods */
    0,                         /* tp_members */
    PyUIComboStyle_getset, /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    0,                         /* tp_init */
    0,                         /* tp_alloc */
    PyUIComboStyle_new,        /* tp_new */
};

static PyMethodDef PyUIToggleStyle_methods[] = {
    {"__pickle__", 
    (PyCFunction)PyUIToggleStyle_pickle, METH_KEYWORDS,
    "Serialize a Permafrost Engine UIToggleStyle object to a string."},

    {"__unpickle__", 
    (PyCFunction)PyUIToggleStyle_unpickle, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
    "Create a new pf.UIToggleStyle instance from a string earlier returned from a __pickle__ method."
    "Returns a tuple of the new instance and the number of bytes consumed from the stream."},

    {NULL}  /* Sentinel */
};

static PyGetSetDef PyUIToggleStyle_getset[] = {
    {"normal",
    (getter)PyUIToggleStyle_get_normal, 
    (setter)PyUIToggleStyle_set_normal,
    "The look of the toggle button in the normal state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"hover",
    (getter)PyUIToggleStyle_get_hover, 
    (setter)PyUIToggleStyle_set_hover,
    "The look of the toggle button in the hovered state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"active",
    (getter)PyUIToggleStyle_get_active, 
    (setter)PyUIToggleStyle_set_active,
    "The look of the toggle button in the active state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"border_color",
    (getter)PyUIToggleStyle_get_border_color, 
    (setter)PyUIToggleStyle_set_border_color,
    "The color of the toggle button border - an (R, G, B, A) tuple.",
    NULL},

    {"cursor_normal",
    (getter)PyUIToggleStyle_get_cursor_normal, 
    (setter)PyUIToggleStyle_set_cursor_normal,
    "The look of the toggle button cursor (selection indicator) in the normal state - "
    "either an (R, G, B, A) tuple or a string representing a path to an image.",
    NULL},

    {"cursor_hover",
    (getter)PyUIToggleStyle_get_cursor_hover, 
    (setter)PyUIToggleStyle_set_cursor_hover,
    "The look of the toggle button cursor (selection indicator) in the hover state - "
    "either an (R, G, B, A) tuple or a string representing a path to an image.",
    NULL},

    {"text_normal",
    (getter)PyUIToggleStyle_get_text_normal, 
    (setter)PyUIToggleStyle_set_text_normal,
    "The color of the option text in the normal state - an (R, G, B, A) tuple.",
    NULL},

    {"text_hover",
    (getter)PyUIToggleStyle_get_text_hover, 
    (setter)PyUIToggleStyle_set_text_hover,
    "The color of the option text in the hovered state - an (R, G, B, A) tuple",
    NULL},

    {"text_active",
    (getter)PyUIToggleStyle_get_text_active, 
    (setter)PyUIToggleStyle_set_text_active,
    "The color of the option text in the active state - an (R, G, B, A) tuple",
    NULL},

    {"text_background",
    (getter)PyUIToggleStyle_get_text_background, 
    (setter)PyUIToggleStyle_set_text_background,
    "The color of the option text background - an (R, G, B, A) tuple",
    NULL},

    {"text_alignment",
    (getter)PyUIToggleStyle_get_text_alignment, 
    (setter)PyUIToggleStyle_set_text_alignment,
    "A set of flags to control the text alignment of the option label.", 
    NULL},

    {"padding",
    (getter)PyUIToggleStyle_get_padding, 
    (setter)PyUIToggleStyle_set_padding,
    "An (X, Y) tuple of floats to control the padding around toggle buttons.", 
    NULL},

    {"touch_padding",
    (getter)PyUIToggleStyle_get_touch_padding, 
    (setter)PyUIToggleStyle_set_touch_padding,
    "An (X, Y) tuple of floats to control the clickable region of the toggle button.", 
    NULL},

    {"spacing",
    (getter)PyUIToggleStyle_get_spacing, 
    (setter)PyUIToggleStyle_set_spacing,
    "A float to control the spacing within a toggle button widget.", 
    NULL},

    {"border",
    (getter)PyUIToggleStyle_get_border, 
    (setter)PyUIToggleStyle_set_border,
    "A floating-point value of the toggle button border width, in pixels.", 
    NULL},

    {NULL}  /* Sentinel */
};

static PyTypeObject PyUIToggleStyle_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "pf.UIToggleStyle",        /* tp_name */
    sizeof(PyUIToggleStyleObject), /* tp_basicsize */
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
    "Style configuration for Permafrost Engine UI toggle-able options.", /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    PyUIToggleStyle_methods,   /* tp_methods */
    0,                         /* tp_members */
    PyUIToggleStyle_getset,    /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    0,                         /* tp_init */
    0,                         /* tp_alloc */
    0,                         /* tp_new */
};

static PyMethodDef PyUIScrollbarStyle_methods[] = {
    {"__pickle__", 
    (PyCFunction)PyUIScrollbarStyle_pickle, METH_KEYWORDS,
    "Serialize a Permafrost Engine UIScrollbarStyle object to a string."},

    {"__unpickle__", 
    (PyCFunction)PyUIScrollbarStyle_unpickle, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
    "Create a new pf.UIScrollbarStyle instance from a string earlier returned from a __pickle__ method."
    "Returns a tuple of the new instance and the number of bytes consumed from the stream."},

    {NULL}  /* Sentinel */
};

static PyGetSetDef PyUIScrollbarStyle_getset[] = {
    {"normal",
    (getter)PyUIScrollbarStyle_get_normal, 
    (setter)PyUIScrollbarStyle_set_normal,
    "The look of the scrollbar in the normal state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"hover",
    (getter)PyUIScrollbarStyle_get_hover, 
    (setter)PyUIScrollbarStyle_set_hover,
    "The look of the scrollbar in the hovered state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"active",
    (getter)PyUIScrollbarStyle_get_active, 
    (setter)PyUIScrollbarStyle_set_active,
    "The look of the scrollbar in the active state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"border_color",
    (getter)PyUIScrollbarStyle_get_border_color, 
    (setter)PyUIScrollbarStyle_set_border_color,
    "The color of the scrollbar border - an (R, G, B, A) tuple.",
    NULL},

    {"cursor_normal",
    (getter)PyUIScrollbarStyle_get_cursor_normal, 
    (setter)PyUIScrollbarStyle_set_cursor_normal,
    "The look of the scrollbar cursor (selection indicator) in the normal state - "
    "either an (R, G, B, A) tuple or a string representing a path to an image.",
    NULL},

    {"cursor_hover",
    (getter)PyUIScrollbarStyle_get_cursor_hover, 
    (setter)PyUIScrollbarStyle_set_cursor_hover,
    "The look of the scrollbar cursor (selection indicator) in the hover state - "
    "either an (R, G, B, A) tuple or a string representing a path to an image.",
    NULL},

    {"cursor_active",
    (getter)PyUIScrollbarStyle_get_cursor_active, 
    (setter)PyUIScrollbarStyle_set_cursor_active,
    "The look of the scrollbar cursor (selection indicator) in the active state - "
    "either an (R, G, B, A) tuple or a string representing a path to an image.",
    NULL},

    {"cursor_border_color",
    (getter)PyUIScrollbarStyle_get_cursor_border_color, 
    (setter)PyUIScrollbarStyle_set_cursor_border_color,
    "The look of the scrollbar cursor (selection indicator) in the active state - "
    "an (R, G, B, A) tuple.",
    NULL},

    {"border",
    (getter)PyUIScrollbarStyle_get_border, 
    (setter)PyUIScrollbarStyle_set_border,
    "The width of the scrollbar borders.", 
    NULL},

    {"rounding",
    (getter)PyUIScrollbarStyle_get_rounding, 
    (setter)PyUIScrollbarStyle_set_rounding,
    "An (X, Y) tuple of floats to control the rounding of the scrollbars.", 
    NULL},

    {"border_cursor",
    (getter)PyUIScrollbarStyle_get_border_cursor, 
    (setter)PyUIScrollbarStyle_set_border_cursor,
    "A float to control the border of the cursor.", 
    NULL},

    {"rounding_cursor",
    (getter)PyUIScrollbarStyle_get_rounding_cursor, 
    (setter)PyUIScrollbarStyle_set_rounding_cursor,
    "A float to control the rounding of the cursor.", 
    NULL},

    {"padding",
    (getter)PyUIScrollbarStyle_get_padding, 
    (setter)PyUIScrollbarStyle_set_padding,
    "A float to control the padding within a scrollbar.", 
    NULL},

    {NULL}  /* Sentinel */
};

static PyTypeObject PyUIScrollbarStyle_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "pf.UIScrollbarStyle",        /* tp_name */
    sizeof(PyUIScrollbarStyleObject), /* tp_basicsize */
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
    "Style configuration for Permafrost Engine UI toggle-able options.", /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    PyUIScrollbarStyle_methods,   /* tp_methods */
    0,                         /* tp_members */
    PyUIScrollbarStyle_getset,    /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    0,                         /* tp_init */
    0,                         /* tp_alloc */
    0,                         /* tp_new */
};

static PyMethodDef PyUIEditStyle_methods[] = {
    {"__pickle__", 
    (PyCFunction)PyUIEditStyle_pickle, METH_KEYWORDS,
    "Serialize a Permafrost Engine UIEditStyle object to a string."},

    {"__unpickle__", 
    (PyCFunction)PyUIEditStyle_unpickle, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
    "Create a new pf.UIEditStyle instance from a string earlier returned from a __pickle__ method."
    "Returns a tuple of the new instance and the number of bytes consumed from the stream."},

    {NULL}  /* Sentinel */
};

static PyGetSetDef PyUIEditStyle_getset[] = {
    {"normal",
    (getter)PyUIEditStyle_get_normal, 
    (setter)PyUIEditStyle_set_normal,
    "The look of the editable field in the normal state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"hover",
    (getter)PyUIEditStyle_get_hover, 
    (setter)PyUIEditStyle_set_hover,
    "The look of the editable field in the hovered state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"active",
    (getter)PyUIEditStyle_get_active, 
    (setter)PyUIEditStyle_set_active,
    "The look of the editable field in the active state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"border_color",
    (getter)PyUIEditStyle_get_border_color, 
    (setter)PyUIEditStyle_set_border_color,
    "The color of the editable field border - an (R, G, B, A) tuple.",
    NULL},

    {"scrollbar",
    (getter)PyUIEditStyle_get_scrollbar, 
    NULL,
    "The style of the scrollbar of editable fields - a UIStyleObject instance",
    NULL},

    {"cursor_normal",
    (getter)PyUIEditStyle_get_cursor_normal, 
    (setter)PyUIEditStyle_set_cursor_normal,
    "The color of the cursor in the normal state - an (R, G, B, A) tuple.",
    NULL},

    {"cursor_hover",
    (getter)PyUIEditStyle_get_cursor_hover, 
    (setter)PyUIEditStyle_set_cursor_hover,
    "The color of the cursor in the hover state - an (R, G, B, A) tuple.",
    NULL},

    {"cursor_text_normal",
    (getter)PyUIEditStyle_get_cursor_text_normal, 
    (setter)PyUIEditStyle_set_cursor_text_normal,
    "The color of the text cursor in normal mode - an (R, G, B, A) tuple.",
    NULL},

    {"cursor_text_hover",
    (getter)PyUIEditStyle_get_cursor_text_hover, 
    (setter)PyUIEditStyle_set_cursor_text_hover,
    "The color of the text cursor in hover mode - an (R, G, B, A) tuple.",
    NULL},

    {"text_normal",
    (getter)PyUIEditStyle_get_text_normal, 
    (setter)PyUIEditStyle_set_text_normal,
    "The color of the (unselected) text in normal mode - an (R, G, B, A) tuple.",
    NULL},

    {"text_hover",
    (getter)PyUIEditStyle_get_text_hover, 
    (setter)PyUIEditStyle_set_text_hover,
    "The color of the (unselected) text in hover mode - an (R, G, B, A) tuple.",
    NULL},

    {"text_active",
    (getter)PyUIEditStyle_get_text_active, 
    (setter)PyUIEditStyle_set_text_active,
    "The color of the (unselected) text in active mode - an (R, G, B, A) tuple.",
    NULL},

    {"selected_normal",
    (getter)PyUIEditStyle_get_selected_normal, 
    (setter)PyUIEditStyle_set_selected_normal,
    "The color of the selection box in normal mode - an (R, G, B, A) tuple.",
    NULL},

    {"selected_hover",
    (getter)PyUIEditStyle_get_selected_hover, 
    (setter)PyUIEditStyle_set_selected_hover,
    "The color of the selection box in hover mode - an (R, G, B, A) tuple.",
    NULL},

    {"selected_text_normal",
    (getter)PyUIEditStyle_get_selected_text_normal, 
    (setter)PyUIEditStyle_set_selected_text_normal,
    "The color of the (selected) text in normal mode - an (R, G, B, A) tuple.",
    NULL},

    {"selected_text_hover",
    (getter)PyUIEditStyle_get_selected_text_hover, 
    (setter)PyUIEditStyle_set_selected_text_hover,
    "The color of the (selected) text in hover mode - an (R, G, B, A) tuple.",
    NULL},

    {"border",
    (getter)PyUIEditStyle_get_border, 
    (setter)PyUIEditStyle_set_border,
    "The width of the editable field borders.", 
    NULL},

    {"rounding",
    (getter)PyUIEditStyle_get_rounding, 
    (setter)PyUIEditStyle_set_rounding,
    "An (X, Y) tuple of floats to control the rounding of the editable fields.", 
    NULL},

    {"cursor_size",
    (getter)PyUIEditStyle_get_cursor_size, 
    (setter)PyUIEditStyle_set_cursor_size,
    "A float to control the size of the cursor.", 
    NULL},

    {"scrollbar_size",
    (getter)PyUIEditStyle_get_scrollbar_size, 
    (setter)PyUIEditStyle_set_scrollbar_size,
    "An (X, Y) tuple to control the size of the editable field scrollbar.", 
    NULL},

    {"padding",
    (getter)PyUIEditStyle_get_padding, 
    (setter)PyUIEditStyle_set_padding,
    "An (X, Y) tuple to control the padding within an editable field.", 
    NULL},

    {"row_padding",
    (getter)PyUIEditStyle_get_row_padding, 
    (setter)PyUIEditStyle_set_row_padding,
    "An float to control the row padding within an editable field.", 
    NULL},

    {NULL}  /* Sentinel */
};

static PyTypeObject PyUIEditStyle_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "pf.UIEditStyle",        /* tp_name */
    sizeof(PyUIEditStyleObject), /* tp_basicsize */
    0,                         /* tp_itemsize */
    (destructor)PyUIEditStyle_dealloc, /* tp_dealloc */
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
    Py_TPFLAGS_DEFAULT,        /* tp_flags */
    "Style configuration for Permafrost Engine UI toggle-able options.", /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    PyUIEditStyle_methods,     /* tp_methods */
    0,                         /* tp_members */
    PyUIEditStyle_getset,      /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    0,                         /* tp_init */
    0,                         /* tp_alloc */
    PyUIEditStyle_new,         /* tp_new */
};

static PyMethodDef PyUIPropertyStyle_methods[] = {
    {"__pickle__", 
    (PyCFunction)PyUIPropertyStyle_pickle, METH_KEYWORDS,
    "Serialize a Permafrost Engine UIPropertyStyle object to a string."},

    {"__unpickle__", 
    (PyCFunction)PyUIPropertyStyle_unpickle, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
    "Create a new pf.UIPropertyStyle instance from a string earlier returned from a __pickle__ method."
    "Returns a tuple of the new instance and the number of bytes consumed from the stream."},

    {NULL}  /* Sentinel */
};

static PyGetSetDef PyUIPropertyStyle_getset[] = {
    {"normal",
    (getter)PyUIPropertyStyle_get_normal, 
    (setter)PyUIPropertyStyle_set_normal,
    "The look of the property field in the normal state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"hover",
    (getter)PyUIPropertyStyle_get_hover, 
    (setter)PyUIPropertyStyle_set_hover,
    "The look of the property field in the hovered state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"active",
    (getter)PyUIPropertyStyle_get_active, 
    (setter)PyUIPropertyStyle_set_active,
    "The look of the property field in the active state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"border_color",
    (getter)PyUIPropertyStyle_get_border_color, 
    (setter)PyUIPropertyStyle_set_border_color,
    "The color of the property field border - an (R, G, B, A) tuple.",
    NULL},

    {"label_normal",
    (getter)PyUIPropertyStyle_get_label_normal, 
    (setter)PyUIPropertyStyle_set_label_normal,
    "The color of the label in the normal state - an (R, G, B, A) tuple.",
    NULL},

    {"label_hover",
    (getter)PyUIPropertyStyle_get_label_hover, 
    (setter)PyUIPropertyStyle_set_label_hover,
    "The color of the label in the hover state - an (R, G, B, A) tuple.",
    NULL},

    {"label_active",
    (getter)PyUIPropertyStyle_get_label_active, 
    (setter)PyUIPropertyStyle_set_label_active,
    "The color of the label in the active state - an (R, G, B, A) tuple.",
    NULL},

    {"sym_left",
    (getter)PyUIPropertyStyle_get_sym_left, 
    (setter)PyUIPropertyStyle_set_sym_left,
    "The style of the left button symbol - an NK_SYMBOL enum value.",
    NULL},

    {"sym_right",
    (getter)PyUIPropertyStyle_get_sym_right, 
    (setter)PyUIPropertyStyle_set_sym_right,
    "The style of the left button symbol - an NK_SYMBOL enum value.",
    NULL},

    {"border",
    (getter)PyUIPropertyStyle_get_border, 
    (setter)PyUIPropertyStyle_set_border,
    "The width of the property field borders.", 
    NULL},

    {"rounding",
    (getter)PyUIPropertyStyle_get_rounding, 
    (setter)PyUIPropertyStyle_set_rounding,
    "An (X, Y) tuple of floats to control the rounding of the property fields.", 
    NULL},

    {"padding",
    (getter)PyUIPropertyStyle_get_padding, 
    (setter)PyUIPropertyStyle_set_padding,
    "An (X, Y) tuple to control the padding within a property field.", 
    NULL},

    {"edit",
    (getter)PyUIPropertyStyle_get_edit, 
    NULL,
    "The style of the property editable region a pf.UIEditStyleObject instance.", 
    NULL},

    {"inc_button",
    (getter)PyUIPropertyStyle_get_inc_button, 
    NULL,
    "The style of the property increment value button - a pf.UIButtonStyleObject instance.", 
    NULL},

    {"dec_button",
    (getter)PyUIPropertyStyle_get_dec_button, 
    NULL,
    "The style of the property decrement value button - a pf.UIButtonStyleObject instance.", 
    NULL},

    {NULL}  /* Sentinel */
};

static PyTypeObject PyUIPropertyStyle_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "pf.UIPropertyStyle",        /* tp_name */
    sizeof(PyUIPropertyStyleObject), /* tp_basicsize */
    0,                         /* tp_itemsize */
    (destructor)PyUIPropertyStyle_dealloc, /* tp_dealloc */
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
    Py_TPFLAGS_DEFAULT,        /* tp_flags */
    "Style configuration for Permafrost Engine UI property fields.", /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    PyUIPropertyStyle_methods,     /* tp_methods */
    0,                         /* tp_members */
    PyUIPropertyStyle_getset,      /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    0,                         /* tp_init */
    0,                         /* tp_alloc */
    PyUIPropertyStyle_new,         /* tp_new */
};

static PyMethodDef PyUISliderStyle_methods[] = {
    {"__pickle__", 
    (PyCFunction)PyUISliderStyle_pickle, METH_KEYWORDS,
    "Serialize a Permafrost Engine UISliderStyle object to a string."},

    {"__unpickle__", 
    (PyCFunction)PyUISliderStyle_unpickle, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
    "Create a new pf.UISliderStyle instance from a string earlier returned from a __pickle__ method."
    "Returns a tuple of the new instance and the number of bytes consumed from the stream."},

    {NULL}  /* Sentinel */
};

static PyGetSetDef PyUISliderStyle_getset[] = {
    {"normal",
    (getter)PyUISliderStyle_get_normal, 
    (setter)PyUISliderStyle_set_normal,
    "The look of the slider background in the normal state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"hover",
    (getter)PyUISliderStyle_get_hover, 
    (setter)PyUISliderStyle_set_hover,
    "The look of the slider background in the hovered state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"active",
    (getter)PyUISliderStyle_get_active, 
    (setter)PyUISliderStyle_set_active,
    "The look of the slider background in the active state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"border_color",
    (getter)PyUISliderStyle_get_border_color, 
    (setter)PyUISliderStyle_set_border_color,
    "The color of the slider bar border - an (R, G, B, A) tuple.",
    NULL},

    {"bar_normal",
    (getter)PyUISliderStyle_get_bar_normal, 
    (setter)PyUISliderStyle_set_bar_normal,
    "The color of the slider bar in the normal state - an (R, G, B, A) tuple.",
    NULL},

    {"bar_hover",
    (getter)PyUISliderStyle_get_bar_hover, 
    (setter)PyUISliderStyle_set_bar_hover,
    "The color of the slider bar in the hover state - an (R, G, B, A) tuple.",
    NULL},

    {"bar_active",
    (getter)PyUISliderStyle_get_bar_active, 
    (setter)PyUISliderStyle_set_bar_active,
    "The color of the slider bar in the active state - an (R, G, B, A) tuple.",
    NULL},

    {"bar_filled",
    (getter)PyUISliderStyle_get_bar_filled, 
    (setter)PyUISliderStyle_set_bar_filled,
    "The color of the slider bar in the filled state - an (R, G, B, A) tuple.",
    NULL},

    {"cursor_normal",
    (getter)PyUISliderStyle_get_cursor_normal, 
    (setter)PyUISliderStyle_set_cursor_normal,
    "The look of the slider cursor in the normal state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"cursor_hover",
    (getter)PyUISliderStyle_get_cursor_hover, 
    (setter)PyUISliderStyle_set_cursor_hover,
    "The look of the slider cursor in the hovered state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"cursor_active",
    (getter)PyUISliderStyle_get_cursor_active, 
    (setter)PyUISliderStyle_set_cursor_active,
    "The look of the slider cursor in the active state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"border",
    (getter)PyUISliderStyle_get_border, 
    (setter)PyUISliderStyle_set_border,
    "The width of the slider widget borders.", 
    NULL},

    {"rounding",
    (getter)PyUISliderStyle_get_rounding, 
    (setter)PyUISliderStyle_set_rounding,
    "An (X, Y) tuple of floats to control the rounding of the slider widget.", 
    NULL},

    {"bar_height",
    (getter)PyUISliderStyle_get_bar_height, 
    (setter)PyUISliderStyle_set_bar_height,
    "The height of the slider bar.", 
    NULL},

    {"padding",
    (getter)PyUISliderStyle_get_padding, 
    (setter)PyUISliderStyle_set_padding,
    "An (X, Y) tuple to control the padding within a slider widget.", 
    NULL},

    {"spacing",
    (getter)PyUISliderStyle_get_spacing, 
    (setter)PyUISliderStyle_set_spacing,
    "An (X, Y) tuple to control the spacing within a slider widget.", 
    NULL},

    {"cursor_size",
    (getter)PyUISliderStyle_get_cursor_size, 
    (setter)PyUISliderStyle_set_cursor_size,
    "An (X, Y) tuple to control the size of the slider cursor.", 
    NULL},

    {"show_buttons",
    (getter)PyUISliderStyle_get_show_buttons, 
    (setter)PyUISliderStyle_set_show_buttons,
    "A boolean to control whether to show the increment/decrement buttons at the edges of the slider widget.", 
    NULL},

    {"inc_button",
    (getter)PyUISliderStyle_get_inc_button, 
    NULL,
    "Returns a UIButtonStyleObject to control the look of the increment button.", 
    NULL},

    {"dec_button",
    (getter)PyUISliderStyle_get_dec_button, 
    NULL,
    "Returns a UIButtonStyleObject to control the look of the decrement button.", 
    NULL},

    {"inc_symbol",
    (getter)PyUISliderStyle_get_inc_symbol, 
    (setter)PyUISliderStyle_set_inc_symbol,
    "The style of the increment button symbol - an NK_SYMBOL enum value.",
    NULL},

    {"dec_symbol",
    (getter)PyUISliderStyle_get_dec_symbol, 
    (setter)PyUISliderStyle_set_dec_symbol,
    "The style of the decrement button symbol - an NK_SYMBOL enum value.",
    NULL},

    {NULL}  /* Sentinel */
};

static PyTypeObject PyUISliderStyle_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "pf.UISliderStyle",        /* tp_name */
    sizeof(PyUISliderStyleObject), /* tp_basicsize */
    0,                         /* tp_itemsize */
    (destructor)PyUISliderStyle_dealloc, /* tp_dealloc */
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
    Py_TPFLAGS_DEFAULT,        /* tp_flags */
    "Style configuration for Permafrost Engine UI slider options.", /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    PyUISliderStyle_methods,     /* tp_methods */
    0,                         /* tp_members */
    PyUISliderStyle_getset,      /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    0,                         /* tp_init */
    0,                         /* tp_alloc */
    PyUISliderStyle_new,         /* tp_new */
};

static PyMethodDef PyUIProgressStyle_methods[] = {
    {"__pickle__", 
    (PyCFunction)PyUIProgressStyle_pickle, METH_KEYWORDS,
    "Serialize a Permafrost Engine UIProgressStyle object to a string."},

    {"__unpickle__", 
    (PyCFunction)PyUIProgressStyle_unpickle, METH_VARARGS | METH_KEYWORDS | METH_CLASS,
    "Create a new pf.UIProgressStyle instance from a string earlier returned from a __pickle__ method."
    "Returns a tuple of the new instance and the number of bytes consumed from the stream."},

    {NULL}  /* Sentinel */
};

static PyGetSetDef PyUIProgressStyle_getset[] = {
    {"normal",
    (getter)PyUIProgressStyle_get_normal, 
    (setter)PyUIProgressStyle_set_normal,
    "The look of the progress bar background in the normal state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"hover",
    (getter)PyUIProgressStyle_get_hover, 
    (setter)PyUIProgressStyle_set_hover,
    "The look of the progress bar background in the hovered state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"active",
    (getter)PyUIProgressStyle_get_active, 
    (setter)PyUIProgressStyle_set_active,
    "The look of the progress bar background in the active state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"border_color",
    (getter)PyUIProgressStyle_get_border_color, 
    (setter)PyUIProgressStyle_set_border_color,
    "The color of the progress bar border - an (R, G, B, A) tuple.",
    NULL},

    {"cursor_normal",
    (getter)PyUIProgressStyle_get_cursor_normal, 
    (setter)PyUIProgressStyle_set_cursor_normal,
    "The look of the progress bar cursor in the normal state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"cursor_hover",
    (getter)PyUIProgressStyle_get_cursor_hover, 
    (setter)PyUIProgressStyle_set_cursor_hover,
    "The look of the progress bar cursor in the hovered state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"cursor_active",
    (getter)PyUIProgressStyle_get_cursor_active, 
    (setter)PyUIProgressStyle_set_cursor_active,
    "The look of the progress bar cursor in the active state - either an (R, G, B, A) tuple or a "
    "string representing a path to an image.",
    NULL},

    {"cursor_border_color",
    (getter)PyUIProgressStyle_get_cursor_border_color, 
    (setter)PyUIProgressStyle_set_cursor_border_color,
    "The color of the progress bar cursor border - an (R, G, B, A) tuple.",
    NULL},

    {"border",
    (getter)PyUIProgressStyle_get_border, 
    (setter)PyUIProgressStyle_set_border,
    "The width of the progress bar borders.", 
    NULL},

    {"rounding",
    (getter)PyUIProgressStyle_get_rounding, 
    (setter)PyUIProgressStyle_set_rounding,
    "An (X, Y) tuple of floats to control the rounding of the progress bar widget.", 
    NULL},

    {"cursor_border",
    (getter)PyUIProgressStyle_get_cursor_border, 
    (setter)PyUIProgressStyle_set_cursor_border,
    "The width of the progress bar cursor borders.", 
    NULL},

    {"cursor_rounding",
    (getter)PyUIProgressStyle_get_cursor_rounding, 
    (setter)PyUIProgressStyle_set_cursor_rounding,
    "An (X, Y) tuple of floats to control the rounding of the progress bar cursor.", 
    NULL},

    {"padding",
    (getter)PyUIProgressStyle_get_padding, 
    (setter)PyUIProgressStyle_set_padding,
    "An (X, Y) tuple to control the padding within a progress bar widget.", 
    NULL},

    {NULL}  /* Sentinel */
};

static PyTypeObject PyUIProgressStyle_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "pf.UIProgressStyle",        /* tp_name */
    sizeof(PyUIProgressStyleObject), /* tp_basicsize */
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
    Py_TPFLAGS_DEFAULT,        /* tp_flags */
    "Style configuration for Permafrost Engine UI progress bar options.", /* tp_doc */
    0,                         /* tp_traverse */
    0,                         /* tp_clear */
    0,                         /* tp_richcompare */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter */
    0,                         /* tp_iternext */
    PyUIProgressStyle_methods, /* tp_methods */
    0,                         /* tp_members */
    PyUIProgressStyle_getset,  /* tp_getset */
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

bool save_combo(struct SDL_RWops *stream, const struct nk_style_combo *combo)
{
    CHK_TRUE_RET(save_item(stream, &combo->normal));
    CHK_TRUE_RET(save_item(stream, &combo->hover));
    CHK_TRUE_RET(save_item(stream, &combo->active));
    CHK_TRUE_RET(save_color(stream, combo->border_color));

    CHK_TRUE_RET(save_color(stream, combo->label_normal));
    CHK_TRUE_RET(save_color(stream, combo->label_hover));
    CHK_TRUE_RET(save_color(stream, combo->label_active));

    CHK_TRUE_RET(save_color(stream, combo->symbol_normal));
    CHK_TRUE_RET(save_color(stream, combo->symbol_hover));
    CHK_TRUE_RET(save_color(stream, combo->symbol_active));

    CHK_TRUE_RET(save_button(stream, &combo->button));
    CHK_TRUE_RET(save_int(stream, combo->sym_normal));
    CHK_TRUE_RET(save_int(stream, combo->sym_hover));
    CHK_TRUE_RET(save_int(stream, combo->sym_active));

    CHK_TRUE_RET(save_float(stream, combo->border));
    CHK_TRUE_RET(save_float(stream, combo->rounding));
    CHK_TRUE_RET(save_vec2(stream, combo->content_padding));
    CHK_TRUE_RET(save_vec2(stream, combo->button_padding));
    CHK_TRUE_RET(save_vec2(stream, combo->spacing));

    return true;
}

bool load_combo(struct SDL_RWops *stream, struct nk_style_combo *out)
{
    CHK_TRUE_RET(load_item(stream, &out->normal));
    CHK_TRUE_RET(load_item(stream, &out->hover));
    CHK_TRUE_RET(load_item(stream, &out->active));
    CHK_TRUE_RET(load_color(stream, &out->border_color));

    CHK_TRUE_RET(load_color(stream, &out->label_normal));
    CHK_TRUE_RET(load_color(stream, &out->label_hover));
    CHK_TRUE_RET(load_color(stream, &out->label_active));

    CHK_TRUE_RET(load_color(stream, &out->symbol_normal));
    CHK_TRUE_RET(load_color(stream, &out->symbol_hover));
    CHK_TRUE_RET(load_color(stream, &out->symbol_active));

    CHK_TRUE_RET(load_button(stream, &out->button));
    CHK_TRUE_RET(load_int(stream, (int*)&out->sym_normal));
    CHK_TRUE_RET(load_int(stream, (int*)&out->sym_hover));
    CHK_TRUE_RET(load_int(stream, (int*)&out->sym_active));

    CHK_TRUE_RET(load_float(stream, &out->border));
    CHK_TRUE_RET(load_float(stream, &out->rounding));
    CHK_TRUE_RET(load_vec2(stream, &out->content_padding));
    CHK_TRUE_RET(load_vec2(stream, &out->button_padding));
    CHK_TRUE_RET(load_vec2(stream, &out->spacing));

    return true;
}

static bool save_toggle(struct SDL_RWops *stream, const struct nk_style_toggle *toggle)
{
    CHK_TRUE_RET(save_item(stream, &toggle->normal));
    CHK_TRUE_RET(save_item(stream, &toggle->hover));
    CHK_TRUE_RET(save_item(stream, &toggle->active));
    CHK_TRUE_RET(save_color(stream, toggle->border_color));

    CHK_TRUE_RET(save_item(stream, &toggle->cursor_normal));
    CHK_TRUE_RET(save_item(stream, &toggle->cursor_hover));

    CHK_TRUE_RET(save_color(stream, toggle->text_normal));
    CHK_TRUE_RET(save_color(stream, toggle->text_hover));
    CHK_TRUE_RET(save_color(stream, toggle->text_active));
    CHK_TRUE_RET(save_color(stream, toggle->text_background));
    CHK_TRUE_RET(save_int(stream, toggle->text_alignment));

    CHK_TRUE_RET(save_vec2(stream, toggle->padding));
    CHK_TRUE_RET(save_vec2(stream, toggle->touch_padding));
    CHK_TRUE_RET(save_float(stream, toggle->spacing));
    CHK_TRUE_RET(save_float(stream, toggle->border));

    return true;
}

static bool load_toggle(struct SDL_RWops *stream, struct nk_style_toggle *out)
{
    CHK_TRUE_RET(load_item(stream, &out->normal));
    CHK_TRUE_RET(load_item(stream, &out->hover));
    CHK_TRUE_RET(load_item(stream, &out->active));
    CHK_TRUE_RET(load_color(stream, &out->border_color));

    CHK_TRUE_RET(load_item(stream, &out->cursor_normal));
    CHK_TRUE_RET(load_item(stream, &out->cursor_hover));

    CHK_TRUE_RET(load_color(stream, &out->text_normal));
    CHK_TRUE_RET(load_color(stream, &out->text_hover));
    CHK_TRUE_RET(load_color(stream, &out->text_active));
    CHK_TRUE_RET(load_color(stream, &out->text_background));
    CHK_TRUE_RET(load_int(stream, (int*)&out->text_alignment));

    CHK_TRUE_RET(load_vec2(stream, &out->padding));
    CHK_TRUE_RET(load_vec2(stream, &out->touch_padding));
    CHK_TRUE_RET(load_float(stream, &out->spacing));
    CHK_TRUE_RET(load_float(stream, &out->border));

    return true;
}

static bool save_scrollbar(struct SDL_RWops *stream, const struct nk_style_scrollbar *scroll)
{
    CHK_TRUE_RET(save_item(stream, &scroll->normal));
    CHK_TRUE_RET(save_item(stream, &scroll->hover));
    CHK_TRUE_RET(save_item(stream, &scroll->active));
    CHK_TRUE_RET(save_color(stream, scroll->border_color));

    CHK_TRUE_RET(save_item(stream, &scroll->cursor_normal));
    CHK_TRUE_RET(save_item(stream, &scroll->cursor_hover));
    CHK_TRUE_RET(save_item(stream, &scroll->cursor_active));
    CHK_TRUE_RET(save_color(stream, scroll->cursor_border_color));

    CHK_TRUE_RET(save_float(stream, scroll->border));
    CHK_TRUE_RET(save_float(stream, scroll->rounding));
    CHK_TRUE_RET(save_float(stream, scroll->border_cursor));
    CHK_TRUE_RET(save_float(stream, scroll->rounding_cursor));
    CHK_TRUE_RET(save_vec2(stream, scroll->padding));

    return true;
}

static bool load_scrollbar(struct SDL_RWops *stream, struct nk_style_scrollbar *out)
{
    CHK_TRUE_RET(load_item(stream, &out->normal));
    CHK_TRUE_RET(load_item(stream, &out->hover));
    CHK_TRUE_RET(load_item(stream, &out->active));
    CHK_TRUE_RET(load_color(stream, &out->border_color));

    CHK_TRUE_RET(load_item(stream, &out->cursor_normal));
    CHK_TRUE_RET(load_item(stream, &out->cursor_hover));
    CHK_TRUE_RET(load_item(stream, &out->cursor_active));
    CHK_TRUE_RET(load_color(stream, &out->cursor_border_color));

    CHK_TRUE_RET(load_float(stream, &out->border));
    CHK_TRUE_RET(load_float(stream, &out->rounding));
    CHK_TRUE_RET(load_float(stream, &out->border_cursor));
    CHK_TRUE_RET(load_float(stream, &out->rounding_cursor));
    CHK_TRUE_RET(load_vec2(stream, &out->padding));

    return true;
}

bool save_edit(struct SDL_RWops *stream, const struct nk_style_edit *edit)
{
    CHK_TRUE_RET(save_item(stream, &edit->normal));
    CHK_TRUE_RET(save_item(stream, &edit->hover));
    CHK_TRUE_RET(save_item(stream, &edit->active));
    CHK_TRUE_RET(save_scrollbar(stream, &edit->scrollbar));

    CHK_TRUE_RET(save_color(stream, edit->cursor_normal));
    CHK_TRUE_RET(save_color(stream, edit->cursor_hover));
    CHK_TRUE_RET(save_color(stream, edit->cursor_text_normal));
    CHK_TRUE_RET(save_color(stream, edit->cursor_text_hover));

    CHK_TRUE_RET(save_color(stream, edit->text_normal));
    CHK_TRUE_RET(save_color(stream, edit->text_hover));
    CHK_TRUE_RET(save_color(stream, edit->text_active));

    CHK_TRUE_RET(save_color(stream, edit->selected_normal));
    CHK_TRUE_RET(save_color(stream, edit->selected_hover));
    CHK_TRUE_RET(save_color(stream, edit->selected_text_normal));
    CHK_TRUE_RET(save_color(stream, edit->selected_text_hover));

    CHK_TRUE_RET(save_float(stream, edit->border));
    CHK_TRUE_RET(save_float(stream, edit->rounding));
    CHK_TRUE_RET(save_float(stream, edit->cursor_size));
    CHK_TRUE_RET(save_vec2(stream, edit->scrollbar_size));
    CHK_TRUE_RET(save_vec2(stream, edit->padding));
    CHK_TRUE_RET(save_float(stream, edit->row_padding));

    return true;
}

static bool load_edit(struct SDL_RWops *stream, struct nk_style_edit *out)
{
    CHK_TRUE_RET(load_item(stream, &out->normal));
    CHK_TRUE_RET(load_item(stream, &out->hover));
    CHK_TRUE_RET(load_item(stream, &out->active));
    CHK_TRUE_RET(load_scrollbar(stream, &out->scrollbar));

    CHK_TRUE_RET(load_color(stream, &out->cursor_normal));
    CHK_TRUE_RET(load_color(stream, &out->cursor_hover));
    CHK_TRUE_RET(load_color(stream, &out->cursor_text_normal));
    CHK_TRUE_RET(load_color(stream, &out->cursor_text_hover));

    CHK_TRUE_RET(load_color(stream, &out->text_normal));
    CHK_TRUE_RET(load_color(stream, &out->text_hover));
    CHK_TRUE_RET(load_color(stream, &out->text_active));

    CHK_TRUE_RET(load_color(stream, &out->selected_normal));
    CHK_TRUE_RET(load_color(stream, &out->selected_hover));
    CHK_TRUE_RET(load_color(stream, &out->selected_text_normal));
    CHK_TRUE_RET(load_color(stream, &out->selected_text_hover));

    CHK_TRUE_RET(load_float(stream, &out->border));
    CHK_TRUE_RET(load_float(stream, &out->rounding));
    CHK_TRUE_RET(load_float(stream, &out->cursor_size));
    CHK_TRUE_RET(load_vec2(stream, &out->scrollbar_size));
    CHK_TRUE_RET(load_vec2(stream, &out->padding));
    CHK_TRUE_RET(load_float(stream, &out->row_padding));

    return true;
}

bool save_property(struct SDL_RWops *stream, const struct nk_style_property *property)
{
    CHK_TRUE_RET(save_item(stream, &property->normal));
    CHK_TRUE_RET(save_item(stream, &property->hover));
    CHK_TRUE_RET(save_item(stream, &property->active));
    CHK_TRUE_RET(save_color(stream, property->border_color));

    CHK_TRUE_RET(save_color(stream, property->label_normal));
    CHK_TRUE_RET(save_color(stream, property->label_hover));
    CHK_TRUE_RET(save_color(stream, property->label_active));

    CHK_TRUE_RET(save_int(stream, property->sym_left));
    CHK_TRUE_RET(save_int(stream, property->sym_right));

    CHK_TRUE_RET(save_float(stream, property->border));
    CHK_TRUE_RET(save_float(stream, property->rounding));
    CHK_TRUE_RET(save_vec2(stream, property->padding));

    CHK_TRUE_RET(save_edit(stream, &property->edit));
    CHK_TRUE_RET(save_button(stream, &property->inc_button));
    CHK_TRUE_RET(save_button(stream, &property->dec_button));

    return true;
}

static bool load_property(struct SDL_RWops *stream, struct nk_style_property *out)
{
    CHK_TRUE_RET(load_item(stream, &out->normal));
    CHK_TRUE_RET(load_item(stream, &out->hover));
    CHK_TRUE_RET(load_item(stream, &out->active));
    CHK_TRUE_RET(load_color(stream, &out->border_color));

    CHK_TRUE_RET(load_color(stream, &out->label_normal));
    CHK_TRUE_RET(load_color(stream, &out->label_hover));
    CHK_TRUE_RET(load_color(stream, &out->label_active));

    CHK_TRUE_RET(load_int(stream, (int*)&out->sym_left));
    CHK_TRUE_RET(load_int(stream, (int*)&out->sym_right));

    CHK_TRUE_RET(load_float(stream, &out->border));
    CHK_TRUE_RET(load_float(stream, &out->rounding));
    CHK_TRUE_RET(load_vec2(stream, &out->padding));

    CHK_TRUE_RET(load_edit(stream, &out->edit));
    CHK_TRUE_RET(load_button(stream, &out->inc_button));
    CHK_TRUE_RET(load_button(stream, &out->dec_button));

    return true;
}

bool save_slider(struct SDL_RWops *stream, const struct nk_style_slider *slider)
{
    CHK_TRUE_RET(save_item(stream, &slider->normal));
    CHK_TRUE_RET(save_item(stream, &slider->hover));
    CHK_TRUE_RET(save_item(stream, &slider->active));
    CHK_TRUE_RET(save_color(stream, slider->border_color));

    CHK_TRUE_RET(save_color(stream, slider->bar_normal));
    CHK_TRUE_RET(save_color(stream, slider->bar_hover));
    CHK_TRUE_RET(save_color(stream, slider->bar_active));
    CHK_TRUE_RET(save_color(stream, slider->bar_filled));

    CHK_TRUE_RET(save_item(stream, &slider->cursor_normal));
    CHK_TRUE_RET(save_item(stream, &slider->cursor_hover));
    CHK_TRUE_RET(save_item(stream, &slider->cursor_active));

    CHK_TRUE_RET(save_float(stream, slider->border));
    CHK_TRUE_RET(save_float(stream, slider->rounding));
    CHK_TRUE_RET(save_float(stream, slider->bar_height));
    CHK_TRUE_RET(save_vec2(stream, slider->padding));
    CHK_TRUE_RET(save_vec2(stream, slider->spacing));
    CHK_TRUE_RET(save_vec2(stream, slider->cursor_size));

    CHK_TRUE_RET(save_int(stream, slider->show_buttons));
    CHK_TRUE_RET(save_button(stream, &slider->inc_button));
    CHK_TRUE_RET(save_button(stream, &slider->dec_button));
    CHK_TRUE_RET(save_int(stream, slider->inc_symbol));
    CHK_TRUE_RET(save_int(stream, slider->dec_symbol));

    return true;
}

static bool load_slider(struct SDL_RWops *stream, struct nk_style_slider *out)
{
    CHK_TRUE_RET(load_item(stream, &out->normal));
    CHK_TRUE_RET(load_item(stream, &out->hover));
    CHK_TRUE_RET(load_item(stream, &out->active));
    CHK_TRUE_RET(load_color(stream, &out->border_color));

    CHK_TRUE_RET(load_color(stream, &out->bar_normal));
    CHK_TRUE_RET(load_color(stream, &out->bar_hover));
    CHK_TRUE_RET(load_color(stream, &out->bar_active));
    CHK_TRUE_RET(load_color(stream, &out->bar_filled));

    CHK_TRUE_RET(load_item(stream, &out->cursor_normal));
    CHK_TRUE_RET(load_item(stream, &out->cursor_hover));
    CHK_TRUE_RET(load_item(stream, &out->cursor_active));

    CHK_TRUE_RET(load_float(stream, &out->border));
    CHK_TRUE_RET(load_float(stream, &out->rounding));
    CHK_TRUE_RET(load_float(stream, &out->bar_height));
    CHK_TRUE_RET(load_vec2(stream, &out->padding));
    CHK_TRUE_RET(load_vec2(stream, &out->spacing));
    CHK_TRUE_RET(load_vec2(stream, &out->cursor_size));

    CHK_TRUE_RET(load_int(stream, (int*)&out->show_buttons));
    CHK_TRUE_RET(load_button(stream, &out->inc_button));
    CHK_TRUE_RET(load_button(stream, &out->dec_button));
    CHK_TRUE_RET(load_int(stream, (int*)&out->inc_symbol));
    CHK_TRUE_RET(load_int(stream, (int*)&out->dec_symbol));

    return true;
}

bool save_progress(struct SDL_RWops *stream, const struct nk_style_progress *progress)
{
    CHK_TRUE_RET(save_item(stream, &progress->normal));
    CHK_TRUE_RET(save_item(stream, &progress->hover));
    CHK_TRUE_RET(save_item(stream, &progress->active));
    CHK_TRUE_RET(save_color(stream, progress->border_color));

    CHK_TRUE_RET(save_item(stream, &progress->cursor_normal));
    CHK_TRUE_RET(save_item(stream, &progress->cursor_hover));
    CHK_TRUE_RET(save_item(stream, &progress->cursor_active));
    CHK_TRUE_RET(save_color(stream, progress->cursor_border_color));

    CHK_TRUE_RET(save_float(stream, progress->border));
    CHK_TRUE_RET(save_float(stream, progress->rounding));
    CHK_TRUE_RET(save_float(stream, progress->cursor_border));
    CHK_TRUE_RET(save_float(stream, progress->cursor_rounding));
    CHK_TRUE_RET(save_vec2(stream, progress->padding));

    return true;
}

static bool load_progress(struct SDL_RWops *stream, struct nk_style_progress *out)
{
    CHK_TRUE_RET(load_item(stream, &out->normal));
    CHK_TRUE_RET(load_item(stream, &out->hover));
    CHK_TRUE_RET(load_item(stream, &out->active));
    CHK_TRUE_RET(load_color(stream, &out->border_color));

    CHK_TRUE_RET(load_item(stream, &out->cursor_normal));
    CHK_TRUE_RET(load_item(stream, &out->cursor_hover));
    CHK_TRUE_RET(load_item(stream, &out->cursor_active));
    CHK_TRUE_RET(load_color(stream, &out->cursor_border_color));

    CHK_TRUE_RET(load_float(stream, &out->border));
    CHK_TRUE_RET(load_float(stream, &out->rounding));
    CHK_TRUE_RET(load_float(stream, &out->cursor_border));
    CHK_TRUE_RET(load_float(stream, &out->cursor_rounding));
    CHK_TRUE_RET(load_vec2(stream, &out->padding));

    return true;
}

static PyObject *PyUIButtonStyle_pickle(PyUIButtonStyleObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *ret = NULL;

    SDL_RWops *stream = PFSDL_VectorRWOps();
    CHK_TRUE(stream, fail_alloc);

    CHK_TRUE(save_int(stream, self->type), fail_pickle);
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

static PyObject *PyUIButtonStyle_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs)
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

    CHK_TRUE(load_int(stream, (int*)&((PyUIButtonStyleObject*)styleobj)->type), fail_unpickle);
    struct nk_context *ctx = UI_GetContext();

    switch(((PyUIButtonStyleObject*)styleobj)->type) {
        case BUTTON_REGULAR:
            ((PyUIButtonStyleObject*)styleobj)->style = &ctx->style.button;
            break;
        case BUTTON_CONTEXTUAL:
            ((PyUIButtonStyleObject*)styleobj)->style = &ctx->style.contextual_button;
            break;
        case BUTTON_MENU:
            ((PyUIButtonStyleObject*)styleobj)->style = &ctx->style.menu_button;
            break;
        default:
            goto fail_unpickle;
    }

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

static PyObject *PyUIHeaderStyle_pickle(PyUIHeaderStyleObject *self, PyObject *args, PyObject *kwargs)
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

static PyObject *PyUIHeaderStyle_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs)
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

static PyObject *PyUISelectableStyle_pickle(PyUISelectableStyleObject *self, PyObject *args, PyObject *kwargs)
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

static PyObject *PyUISelectableStyle_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs)
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

static PyObject *PyUIComboStyle_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyUIComboStyleObject *self = (PyUIComboStyleObject*)type->tp_alloc(type, 0);
    if(!self)
        return NULL;

    self->button = PyObject_New(PyUIButtonStyleObject, &PyUIButtonStyle_type);
    if(!self->button) {
        goto fail_button;
    }

    struct nk_context *ctx = UI_GetContext();
    self->style = &ctx->style.combo;
    self->button->style = &ctx->style.combo.button;

    return (PyObject*)self;

fail_button:
    Py_DECREF(self);
    return NULL;
}

static void PyUIComboStyle_dealloc(PyUIComboStyleObject *self)
{
    Py_DECREF(self->button);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject *PyUIComboStyle_get_normal(PyUIComboStyleObject *self, void *closure)
{
    return style_get_item(&self->style->normal);
}

static int PyUIComboStyle_set_normal(PyUIComboStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->normal);
}

static PyObject *PyUIComboStyle_get_hover(PyUIComboStyleObject *self, void *closure)
{
    return style_get_item(&self->style->hover);
}

static int PyUIComboStyle_set_hover(PyUIComboStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->hover);
}

static PyObject *PyUIComboStyle_get_active(PyUIComboStyleObject *self, void *closure)
{
    return style_get_item(&self->style->active);
}

static int PyUIComboStyle_set_active(PyUIComboStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->active);
}

static PyObject *PyUIComboStyle_get_border_color(PyUIComboStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->border_color.r,
        self->style->border_color.g,
        self->style->border_color.b,
        self->style->border_color.a);
}

static int PyUIComboStyle_set_border_color(PyUIComboStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->border_color = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIComboStyle_get_label_normal(PyUIComboStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->label_normal.r,
        self->style->label_normal.g,
        self->style->label_normal.b,
        self->style->label_normal.a);
}

static int PyUIComboStyle_set_label_normal(PyUIComboStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->label_normal = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIComboStyle_get_label_hover(PyUIComboStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->label_hover.r,
        self->style->label_hover.g,
        self->style->label_hover.b,
        self->style->label_hover.a);
}

static int PyUIComboStyle_set_label_hover(PyUIComboStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->label_hover = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIComboStyle_get_label_active(PyUIComboStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->label_active.r,
        self->style->label_active.g,
        self->style->label_active.b,
        self->style->label_active.a);
}

static int PyUIComboStyle_set_label_active(PyUIComboStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->label_active = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIComboStyle_get_symbol_normal(PyUIComboStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->symbol_normal.r,
        self->style->symbol_normal.g,
        self->style->symbol_normal.b,
        self->style->symbol_normal.a);
}

static int PyUIComboStyle_set_symbol_normal(PyUIComboStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->symbol_normal = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIComboStyle_get_symbol_hover(PyUIComboStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->symbol_hover.r,
        self->style->symbol_hover.g,
        self->style->symbol_hover.b,
        self->style->symbol_hover.a);
}

static int PyUIComboStyle_set_symbol_hover(PyUIComboStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->symbol_hover = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIComboStyle_get_symbol_active(PyUIComboStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->symbol_active.r,
        self->style->symbol_active.g,
        self->style->symbol_active.b,
        self->style->symbol_active.a);
}

static int PyUIComboStyle_set_symbol_active(PyUIComboStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->symbol_active = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIComboStyle_get_button(PyUIComboStyleObject *self, void *closure)
{
    Py_INCREF(self->button);
    return (PyObject*)self->button;
}

static PyObject *PyUIComboStyle_get_sym_normal(PyUIComboStyleObject *self, void *closure)
{
    return PyInt_FromLong(self->style->sym_normal);
}

static int PyUIComboStyle_set_sym_normal(PyUIComboStyleObject *self, PyObject *value, void *closure)
{
    if(!PyInt_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be an integer.");
        return -1; 
    }

    self->style->sym_normal = PyInt_AsLong(value);
    return 0;
}

static PyObject *PyUIComboStyle_get_sym_hover(PyUIComboStyleObject *self, void *closure)
{
    return PyInt_FromLong(self->style->sym_hover);
}

static int PyUIComboStyle_set_sym_hover(PyUIComboStyleObject *self, PyObject *value, void *closure)
{
    if(!PyInt_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be an integer.");
        return -1; 
    }

    self->style->sym_hover = PyInt_AsLong(value);
    return 0;
}

static PyObject *PyUIComboStyle_get_sym_active(PyUIComboStyleObject *self, void *closure)
{
    return PyInt_FromLong(self->style->sym_active);
}

static int PyUIComboStyle_set_sym_active(PyUIComboStyleObject *self, PyObject *value, void *closure)
{
    if(!PyInt_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be an integer.");
        return -1; 
    }

    self->style->sym_active = PyInt_AsLong(value);
    return 0;
}

static PyObject *PyUIComboStyle_get_border(PyUIComboStyleObject *self, void *closure)
{
    return PyFloat_FromDouble(self->style->border);
}

static int PyUIComboStyle_set_border(PyUIComboStyleObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be a float.");
        return -1; 
    }

    self->style->border = PyInt_AsLong(value);
    return 0;
}

static PyObject *PyUIComboStyle_get_rounding(PyUIComboStyleObject *self, void *closure)
{
    return PyFloat_FromDouble(self->style->rounding);
}

static int PyUIComboStyle_set_rounding(PyUIComboStyleObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be a float.");
        return -1; 
    }

    self->style->rounding = PyInt_AsLong(value);
    return 0;
}

static PyObject *PyUIComboStyle_get_content_padding(PyUIComboStyleObject *self, void *closure)
{
    return Py_BuildValue("(f,f)", 
        self->style->content_padding.x,
        self->style->content_padding.y);
}

static int PyUIComboStyle_set_content_padding(PyUIComboStyleObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style->content_padding = (struct nk_vec2){x, y};
    return 0;
}

static PyObject *PyUIComboStyle_get_button_padding(PyUIComboStyleObject *self, void *closure)
{
    return Py_BuildValue("(f,f)", 
        self->style->button_padding.x,
        self->style->button_padding.y);
}

static int PyUIComboStyle_set_button_padding(PyUIComboStyleObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style->button_padding = (struct nk_vec2){x, y};
    return 0;
}

static PyObject *PyUIComboStyle_get_spacing(PyUIComboStyleObject *self, void *closure)
{
    return Py_BuildValue("(f,f)", 
        self->style->spacing.x,
        self->style->spacing.y);
}

static int PyUIComboStyle_set_spacing(PyUIComboStyleObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style->spacing = (struct nk_vec2){x, y};
    return 0;
}

static PyObject *PyUIComboStyle_pickle(PyUIComboStyleObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *ret = NULL;

    SDL_RWops *stream = PFSDL_VectorRWOps();
    CHK_TRUE(stream, fail_alloc);
    CHK_TRUE(save_combo(stream, self->style), fail_pickle);
    ret = PyString_FromStringAndSize(PFSDL_VectorRWOpsRaw(stream), SDL_RWsize(stream));

fail_pickle:
    SDL_RWclose(stream);
fail_alloc:
    if(!ret) {
        PyErr_SetString(PyExc_RuntimeError, "Error pickling pf.UIComboStyle object");
    }
    return ret;
}

static PyObject *PyUIComboStyle_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs)
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

    PyUIComboStyleObject *styleobj = 
        (PyUIComboStyleObject*)PyObject_CallFunctionObjArgs((PyObject*)&PyUIComboStyle_type, NULL);
    assert(styleobj || PyErr_Occurred());
    CHK_TRUE(styleobj, fail_unpickle);

    CHK_TRUE(load_combo(stream, ((PyUIComboStyleObject*)styleobj)->style), fail_unpickle);

    Py_ssize_t nread = SDL_RWseek(stream, 0, RW_SEEK_CUR);
    ret = Py_BuildValue("(Oi)", styleobj, (int)nread);
    Py_DECREF(styleobj);

fail_unpickle:
    SDL_RWclose(stream);
fail_args:
    if(!ret) {
        PyErr_SetString(PyExc_RuntimeError, "Error unpickling pf.UIComboStyle object");
    }
    return ret;
}

static PyObject *PyUIToggleStyle_get_normal(PyUIToggleStyleObject *self, void *closure)
{
    return style_get_item(&self->style->normal);
}

static int PyUIToggleStyle_set_normal(PyUIToggleStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->normal);
}

static PyObject *PyUIToggleStyle_get_hover(PyUIToggleStyleObject *self, void *closure)
{
    return style_get_item(&self->style->hover);
}

static int PyUIToggleStyle_set_hover(PyUIToggleStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->hover);
}

static PyObject *PyUIToggleStyle_get_active(PyUIToggleStyleObject *self, void *closure)
{
    return style_get_item(&self->style->active);
}

static int PyUIToggleStyle_set_active(PyUIToggleStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->active);
}

static PyObject *PyUIToggleStyle_get_border_color(PyUIToggleStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->border_color.r,
        self->style->border_color.g,
        self->style->border_color.b,
        self->style->border_color.a);
}

static int PyUIToggleStyle_set_border_color(PyUIToggleStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->border_color = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIToggleStyle_get_cursor_normal(PyUIToggleStyleObject *self, void *closure)
{
    return style_get_item(&self->style->cursor_normal);
}

static int PyUIToggleStyle_set_cursor_normal(PyUIToggleStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->cursor_normal);
}

static PyObject *PyUIToggleStyle_get_cursor_hover(PyUIToggleStyleObject *self, void *closure)
{
    return style_get_item(&self->style->cursor_hover);
}

static int PyUIToggleStyle_set_cursor_hover(PyUIToggleStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->cursor_hover);
}

static PyObject *PyUIToggleStyle_get_text_normal(PyUIToggleStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->text_normal.r,
        self->style->text_normal.g,
        self->style->text_normal.b,
        self->style->text_normal.a);
}

static int PyUIToggleStyle_set_text_normal(PyUIToggleStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->text_normal = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIToggleStyle_get_text_hover(PyUIToggleStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->text_hover.r,
        self->style->text_hover.g,
        self->style->text_hover.b,
        self->style->text_hover.a);
}

static int PyUIToggleStyle_set_text_hover(PyUIToggleStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->text_hover = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIToggleStyle_get_text_active(PyUIToggleStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->text_active.r,
        self->style->text_active.g,
        self->style->text_active.b,
        self->style->text_active.a);
}

static int PyUIToggleStyle_set_text_active(PyUIToggleStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->text_active = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIToggleStyle_get_text_background(PyUIToggleStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->text_background.r,
        self->style->text_background.g,
        self->style->text_background.b,
        self->style->text_background.a);
}

static int PyUIToggleStyle_set_text_background(PyUIToggleStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->text_background = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIToggleStyle_get_text_alignment(PyUIToggleStyleObject *self, void *closure)
{
    return Py_BuildValue("I", self->style->text_alignment);
}

static int PyUIToggleStyle_set_text_alignment(PyUIToggleStyleObject *self, PyObject *value, void *closure)
{
    if(!PyInt_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be an integer.");
        return -1; 
    }

    self->style->text_alignment = PyInt_AsLong(value);
    return 0;
}

static PyObject *PyUIToggleStyle_get_padding(PyUIToggleStyleObject *self, void *closure)
{
    return Py_BuildValue("(f,f)", 
        self->style->padding.x,
        self->style->padding.y);
}

static int PyUIToggleStyle_set_padding(PyUIToggleStyleObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style->padding = (struct nk_vec2){x, y};
    return 0;
}

static PyObject *PyUIToggleStyle_get_touch_padding(PyUIToggleStyleObject *self, void *closure)
{
    return Py_BuildValue("(f,f)", 
        self->style->touch_padding.x,
        self->style->touch_padding.y);
}

static int PyUIToggleStyle_set_touch_padding(PyUIToggleStyleObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style->touch_padding = (struct nk_vec2){x, y};
    return 0;
}

static PyObject *PyUIToggleStyle_get_spacing(PyUIToggleStyleObject *self, void *closure)
{
    return Py_BuildValue("f", self->style->spacing);
}

static int PyUIToggleStyle_set_spacing(PyUIToggleStyleObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be a float.");
        return -1; 
    }

    self->style->spacing = PyFloat_AsDouble(value);
    return 0;
}

static PyObject *PyUIToggleStyle_get_border(PyUIToggleStyleObject *self, void *closure)
{
    return Py_BuildValue("f", self->style->border);
}

static int PyUIToggleStyle_set_border(PyUIToggleStyleObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be a float.");
        return -1; 
    }

    self->style->border = PyFloat_AsDouble(value);
    return 0;
}

static PyObject *PyUIToggleStyle_pickle(PyUIToggleStyleObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *ret = NULL;

    SDL_RWops *stream = PFSDL_VectorRWOps();
    CHK_TRUE(stream, fail_alloc);
    CHK_TRUE(save_int(stream, self->type), fail_pickle);
    CHK_TRUE(save_toggle(stream, self->style), fail_pickle);
    ret = PyString_FromStringAndSize(PFSDL_VectorRWOpsRaw(stream), SDL_RWsize(stream));

fail_pickle:
    SDL_RWclose(stream);
fail_alloc:
    if(!ret) {
        PyErr_SetString(PyExc_RuntimeError, "Error pickling pf.UIToggleStyle object");
    }
    return ret;
}

static PyObject *PyUIToggleStyle_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs)
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

    PyObject *styleobj = PyObject_New(PyObject, &PyUIToggleStyle_type);
    assert(styleobj || PyErr_Occurred());
    CHK_TRUE(styleobj, fail_unpickle);

    CHK_TRUE(load_int(stream, (int*)&((PyUIToggleStyleObject*)styleobj)->type), fail_unpickle);
    struct nk_context *ctx = UI_GetContext();

    switch(((PyUIToggleStyleObject*)styleobj)->type) {
        case TOGGLE_OPTION:
            ((PyUIToggleStyleObject*)styleobj)->style = &ctx->style.option;
            break;
        case TOGGLE_CHECKBOX:
            ((PyUIToggleStyleObject*)styleobj)->style = &ctx->style.checkbox;
            break;
        default:
            goto fail_unpickle;
    }

    CHK_TRUE(load_toggle(stream, ((PyUIToggleStyleObject*)styleobj)->style), fail_unpickle);

    Py_ssize_t nread = SDL_RWseek(stream, 0, RW_SEEK_CUR);
    ret = Py_BuildValue("(Oi)", styleobj, (int)nread);
    Py_DECREF(styleobj);

fail_unpickle:
    SDL_RWclose(stream);
fail_args:
    if(!ret) {
        PyErr_SetString(PyExc_RuntimeError, "Error unpickling pf.UIToggleStyle object");
    }
    return ret;
}

static PyObject *PyUIScrollbarStyle_get_normal(PyUIScrollbarStyleObject *self, void *closure)
{
    return style_get_item(&self->style->normal);
}

static int PyUIScrollbarStyle_set_normal(PyUIScrollbarStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->normal);
}

static PyObject *PyUIScrollbarStyle_get_hover(PyUIScrollbarStyleObject *self, void *closure)
{
    return style_get_item(&self->style->hover);
}

static int PyUIScrollbarStyle_set_hover(PyUIScrollbarStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->hover);
}

static PyObject *PyUIScrollbarStyle_get_active(PyUIScrollbarStyleObject *self, void *closure)
{
    return style_get_item(&self->style->active);
}

static int PyUIScrollbarStyle_set_active(PyUIScrollbarStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->active);
}

static PyObject *PyUIScrollbarStyle_get_border_color(PyUIScrollbarStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->border_color.r,
        self->style->border_color.g,
        self->style->border_color.b,
        self->style->border_color.a);
}

static int PyUIScrollbarStyle_set_border_color(PyUIScrollbarStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->border_color = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIScrollbarStyle_get_cursor_normal(PyUIScrollbarStyleObject *self, void *closure)
{
    return style_get_item(&self->style->cursor_normal);
}

static int PyUIScrollbarStyle_set_cursor_normal(PyUIScrollbarStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->cursor_normal);
}

static PyObject *PyUIScrollbarStyle_get_cursor_hover(PyUIScrollbarStyleObject *self, void *closure)
{
    return style_get_item(&self->style->cursor_hover);
}

static int PyUIScrollbarStyle_set_cursor_hover(PyUIScrollbarStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->cursor_hover);
}

static PyObject *PyUIScrollbarStyle_get_cursor_active(PyUIScrollbarStyleObject *self, void *closure)
{
    return style_get_item(&self->style->cursor_active);
}

static int PyUIScrollbarStyle_set_cursor_active(PyUIScrollbarStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->cursor_active);
}

static PyObject *PyUIScrollbarStyle_get_cursor_border_color(PyUIScrollbarStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->cursor_border_color.r,
        self->style->cursor_border_color.g,
        self->style->cursor_border_color.b,
        self->style->cursor_border_color.a);
}

static int PyUIScrollbarStyle_set_cursor_border_color(PyUIScrollbarStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->cursor_border_color = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}


static PyObject *PyUIScrollbarStyle_get_border(PyUIScrollbarStyleObject *self, void *closure)
{
    return Py_BuildValue("f", self->style->border);
}

static int PyUIScrollbarStyle_set_border(PyUIScrollbarStyleObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be a float.");
        return -1; 
    }

    self->style->border = PyFloat_AsDouble(value);
    return 0;
}

static PyObject *PyUIScrollbarStyle_get_rounding(PyUIScrollbarStyleObject *self, void *closure)
{
    return Py_BuildValue("f", self->style->rounding);
}

static int PyUIScrollbarStyle_set_rounding(PyUIScrollbarStyleObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be a float.");
        return -1; 
    }

    self->style->rounding = PyFloat_AsDouble(value);
    return 0;
}

static PyObject *PyUIScrollbarStyle_get_border_cursor(PyUIScrollbarStyleObject *self, void *closure)
{
    return Py_BuildValue("f", self->style->border_cursor);
}

static int PyUIScrollbarStyle_set_border_cursor(PyUIScrollbarStyleObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be a float.");
        return -1; 
    }

    self->style->border_cursor = PyFloat_AsDouble(value);
    return 0;
}

static PyObject *PyUIScrollbarStyle_get_rounding_cursor(PyUIScrollbarStyleObject *self, void *closure)
{
    return Py_BuildValue("f", self->style->rounding_cursor);
}

static int PyUIScrollbarStyle_set_rounding_cursor(PyUIScrollbarStyleObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be a float.");
        return -1; 
    }

    self->style->rounding_cursor = PyFloat_AsDouble(value);
    return 0;
}

static PyObject *PyUIScrollbarStyle_get_padding(PyUIScrollbarStyleObject *self, void *closure)
{
    return Py_BuildValue("(f,f)", 
        self->style->padding.x,
        self->style->padding.y);
}

static int PyUIScrollbarStyle_set_padding(PyUIScrollbarStyleObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style->padding = (struct nk_vec2){x, y};
    return 0;
}

static PyObject *PyUIScrollbarStyle_pickle(PyUIScrollbarStyleObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *ret = NULL;

    SDL_RWops *stream = PFSDL_VectorRWOps();
    CHK_TRUE(stream, fail_alloc);
    CHK_TRUE(save_int(stream, self->type), fail_pickle);
    CHK_TRUE(save_scrollbar(stream, self->style), fail_pickle);
    ret = PyString_FromStringAndSize(PFSDL_VectorRWOpsRaw(stream), SDL_RWsize(stream));

fail_pickle:
    SDL_RWclose(stream);
fail_alloc:
    if(!ret) {
        PyErr_SetString(PyExc_RuntimeError, "Error pickling pf.UIScrollbarStyle object");
    }
    return ret;
}

static PyObject *PyUIScrollbarStyle_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs)
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

    PyObject *styleobj = PyObject_New(PyObject, &PyUIScrollbarStyle_type);
    assert(styleobj || PyErr_Occurred());
    CHK_TRUE(styleobj, fail_unpickle);

    CHK_TRUE(load_int(stream, (int*)&((PyUIScrollbarStyleObject*)styleobj)->type), fail_unpickle);
    struct nk_context *ctx = UI_GetContext();

    switch(((PyUIScrollbarStyleObject*)styleobj)->type) {
        case SCROLLBAR_HORIZONTAL:
            ((PyUIScrollbarStyleObject*)styleobj)->style = &ctx->style.scrollh;
            break;
        case SCROLLBAR_VERTICAL:
            ((PyUIScrollbarStyleObject*)styleobj)->style = &ctx->style.scrollv;
            break;
        case SCROLLBAR_EDIT:
            ((PyUIScrollbarStyleObject*)styleobj)->style = &ctx->style.edit.scrollbar;
            break;
        case SCROLLBAR_PROPERTY:
            ((PyUIScrollbarStyleObject*)styleobj)->style = &ctx->style.property.edit.scrollbar;
            break;
        default:
            goto fail_unpickle;
    }

    CHK_TRUE(load_scrollbar(stream, ((PyUIScrollbarStyleObject*)styleobj)->style), fail_unpickle);

    Py_ssize_t nread = SDL_RWseek(stream, 0, RW_SEEK_CUR);
    ret = Py_BuildValue("(Oi)", styleobj, (int)nread);

fail_unpickle:
    Py_XDECREF(styleobj);
    SDL_RWclose(stream);
fail_args:
    if(!ret) {
        PyErr_SetString(PyExc_RuntimeError, "Error unpickling pf.UIScrollbarStyle object");
    }
    return ret;
}

static PyObject *PyUIEditStyle_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    int kind;
    if(!PyArg_ParseTuple(args, "i", &kind)
    || (kind < 0 || kind > EDIT_PROPERTY)) {
        PyErr_SetString(PyExc_TypeError, "Argument must be an integer (type).");
        return NULL;
    }

    PyUIEditStyleObject *self = (PyUIEditStyleObject*)type->tp_alloc(type, 0);
    if(!self)
        return NULL;

    self->scrollbar = PyObject_New(PyUIScrollbarStyleObject, &PyUIScrollbarStyle_type);
    if(!self->scrollbar) {
        goto fail_scrollbar;
    }

    struct nk_context *ctx = UI_GetContext();
    self->type = kind;

    switch(self->type) {
    case EDIT_REGULAR:
        self->style = &ctx->style.edit;
        self->scrollbar->style = &ctx->style.edit.scrollbar;
        self->scrollbar->type = SCROLLBAR_EDIT;
        break;
    case EDIT_PROPERTY:
        self->style = &ctx->style.property.edit;
        self->scrollbar->style = &ctx->style.property.edit.scrollbar;
        self->scrollbar->type = SCROLLBAR_PROPERTY;
        break;
    default: assert(0);
    }

    return (PyObject*)self;

fail_scrollbar:
    Py_DECREF(self);
    return NULL;
}

static void PyUIEditStyle_dealloc(PyUIEditStyleObject *self)
{
    Py_DECREF(self->scrollbar);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject *PyUIEditStyle_get_normal(PyUIEditStyleObject *self, void *closure)
{
    return style_get_item(&self->style->normal);
}

static int PyUIEditStyle_set_normal(PyUIEditStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->normal);
}

static PyObject *PyUIEditStyle_get_hover(PyUIEditStyleObject *self, void *closure)
{
    return style_get_item(&self->style->hover);
}

static int PyUIEditStyle_set_hover(PyUIEditStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->hover);
}

static PyObject *PyUIEditStyle_get_active(PyUIEditStyleObject *self, void *closure)
{
    return style_get_item(&self->style->active);
}

static int PyUIEditStyle_set_active(PyUIEditStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->active);
}

static PyObject *PyUIEditStyle_get_border_color(PyUIEditStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->border_color.r,
        self->style->border_color.g,
        self->style->border_color.b,
        self->style->border_color.a);
}

static int PyUIEditStyle_set_border_color(PyUIEditStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->border_color = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIEditStyle_get_scrollbar(PyUIEditStyleObject *self, void *closure)
{
    Py_INCREF(self->scrollbar);
    return (PyObject*)self->scrollbar;
}

static PyObject *PyUIEditStyle_get_cursor_normal(PyUIEditStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->cursor_normal.r,
        self->style->cursor_normal.g,
        self->style->cursor_normal.b,
        self->style->cursor_normal.a);
}

static int PyUIEditStyle_set_cursor_normal(PyUIEditStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->cursor_normal = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIEditStyle_get_cursor_hover(PyUIEditStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->cursor_hover.r,
        self->style->cursor_hover.g,
        self->style->cursor_hover.b,
        self->style->cursor_hover.a);
}

static int PyUIEditStyle_set_cursor_hover(PyUIEditStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->cursor_hover = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIEditStyle_get_cursor_text_normal(PyUIEditStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->cursor_text_normal.r,
        self->style->cursor_text_normal.g,
        self->style->cursor_text_normal.b,
        self->style->cursor_text_normal.a);
}

static int PyUIEditStyle_set_cursor_text_normal(PyUIEditStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->cursor_text_normal = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIEditStyle_get_cursor_text_hover(PyUIEditStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->cursor_text_hover.r,
        self->style->cursor_text_hover.g,
        self->style->cursor_text_hover.b,
        self->style->cursor_text_hover.a);
}

static int PyUIEditStyle_set_cursor_text_hover(PyUIEditStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->cursor_text_hover = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIEditStyle_get_text_normal(PyUIEditStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->text_normal.r,
        self->style->text_normal.g,
        self->style->text_normal.b,
        self->style->text_normal.a);
}

static int PyUIEditStyle_set_text_normal(PyUIEditStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->text_normal = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIEditStyle_get_text_hover(PyUIEditStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->text_hover.r,
        self->style->text_hover.g,
        self->style->text_hover.b,
        self->style->text_hover.a);
}

static int PyUIEditStyle_set_text_hover(PyUIEditStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->text_hover = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIEditStyle_get_text_active(PyUIEditStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->text_active.r,
        self->style->text_active.g,
        self->style->text_active.b,
        self->style->text_active.a);
}

static int PyUIEditStyle_set_text_active(PyUIEditStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->text_active = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIEditStyle_get_selected_normal(PyUIEditStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->selected_normal.r,
        self->style->selected_normal.g,
        self->style->selected_normal.b,
        self->style->selected_normal.a);
}

static int PyUIEditStyle_set_selected_normal(PyUIEditStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->selected_normal = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIEditStyle_get_selected_hover(PyUIEditStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->selected_hover.r,
        self->style->selected_hover.g,
        self->style->selected_hover.b,
        self->style->selected_hover.a);
}

static int PyUIEditStyle_set_selected_hover(PyUIEditStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->selected_hover = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIEditStyle_get_selected_text_normal(PyUIEditStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->selected_text_normal.r,
        self->style->selected_text_normal.g,
        self->style->selected_text_normal.b,
        self->style->selected_text_normal.a);
}

static int PyUIEditStyle_set_selected_text_normal(PyUIEditStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->selected_text_normal = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIEditStyle_get_selected_text_hover(PyUIEditStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->selected_text_hover.r,
        self->style->selected_text_hover.g,
        self->style->selected_text_hover.b,
        self->style->selected_text_hover.a);
}

static int PyUIEditStyle_set_selected_text_hover(PyUIEditStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->selected_text_hover = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIEditStyle_get_border(PyUIEditStyleObject *self, void *closure)
{
    return Py_BuildValue("f", self->style->border);
}

static int PyUIEditStyle_set_border(PyUIEditStyleObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be a float.");
        return -1; 
    }

    self->style->border = PyFloat_AsDouble(value);
    return 0;
}

static PyObject *PyUIEditStyle_get_rounding(PyUIEditStyleObject *self, void *closure)
{
    return Py_BuildValue("f", self->style->rounding);
}

static int PyUIEditStyle_set_rounding(PyUIEditStyleObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be a float.");
        return -1; 
    }

    self->style->rounding = PyFloat_AsDouble(value);
    return 0;
}

static PyObject *PyUIEditStyle_get_cursor_size(PyUIEditStyleObject *self, void *closure)
{
    return Py_BuildValue("f", self->style->cursor_size);
}

static int PyUIEditStyle_set_cursor_size(PyUIEditStyleObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be a float.");
        return -1; 
    }

    self->style->cursor_size = PyFloat_AsDouble(value);
    return 0;
}

static PyObject *PyUIEditStyle_get_scrollbar_size(PyUIEditStyleObject *self, void *closure)
{
    return Py_BuildValue("(f,f)", 
        self->style->scrollbar_size.x,
        self->style->scrollbar_size.y);
}

static int PyUIEditStyle_set_scrollbar_size(PyUIEditStyleObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style->scrollbar_size = (struct nk_vec2){x, y};
    return 0;
}

static PyObject *PyUIEditStyle_get_padding(PyUIEditStyleObject *self, void *closure)
{
    return Py_BuildValue("(f,f)", 
        self->style->padding.x,
        self->style->padding.y);
}

static int PyUIEditStyle_set_padding(PyUIEditStyleObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style->padding = (struct nk_vec2){x, y};
    return 0;
}

static PyObject *PyUIEditStyle_get_row_padding(PyUIEditStyleObject *self, void *closure)
{
    return Py_BuildValue("f", self->style->row_padding);
}

static int PyUIEditStyle_set_row_padding(PyUIEditStyleObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be a float.");
        return -1; 
    }

    self->style->row_padding = PyFloat_AsDouble(value);
    return 0;
}

static PyObject *PyUIEditStyle_pickle(PyUIEditStyleObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *ret = NULL;

    SDL_RWops *stream = PFSDL_VectorRWOps();
    CHK_TRUE(stream, fail_alloc);
    CHK_TRUE(save_int(stream, self->type), fail_pickle);
    CHK_TRUE(save_edit(stream, self->style), fail_pickle);
    ret = PyString_FromStringAndSize(PFSDL_VectorRWOpsRaw(stream), SDL_RWsize(stream));

fail_pickle:
    SDL_RWclose(stream);
fail_alloc:
    if(!ret) {
        PyErr_SetString(PyExc_RuntimeError, "Error pickling pf.UIEditStyle object");
    }
    return ret;
}

static PyObject *PyUIEditStyle_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs)
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

    int kind;
    CHK_TRUE(load_int(stream, &kind), fail_unpickle);

    PyUIEditStyleObject *styleobj = 
        (PyUIEditStyleObject*)PyObject_CallFunction((PyObject*)&PyUIEditStyle_type, "i", kind);
    assert(styleobj || PyErr_Occurred());
    CHK_TRUE(styleobj, fail_unpickle);

    CHK_TRUE(load_edit(stream, ((PyUIEditStyleObject*)styleobj)->style), fail_unpickle);

    Py_ssize_t nread = SDL_RWseek(stream, 0, RW_SEEK_CUR);
    ret = Py_BuildValue("(Oi)", styleobj, (int)nread);
    Py_DECREF(styleobj);

fail_unpickle:
    SDL_RWclose(stream);
fail_args:
    if(!ret) {
        PyErr_SetString(PyExc_RuntimeError, "Error unpickling pf.UIEditStyle object");
    }
    return ret;
}

static PyObject *PyUIPropertyStyle_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyUIPropertyStyleObject *self = (PyUIPropertyStyleObject*)type->tp_alloc(type, 0);
    if(!self)
        return NULL;

    self->edit = (PyUIEditStyleObject*)PyObject_CallFunction(
        (PyObject*)&PyUIEditStyle_type, 
        "i", EDIT_PROPERTY
    );
    if(!self->edit)
        goto fail_edit;

    struct nk_context *ctx = UI_GetContext();
    self->style = &ctx->style.property;

    self->inc_button = PyObject_New(PyUIButtonStyleObject, &PyUIButtonStyle_type);
    if(!self->inc_button)
        goto fail_inc_button;
    self->inc_button->style = &ctx->style.property.inc_button;

    self->dec_button = PyObject_New(PyUIButtonStyleObject, &PyUIButtonStyle_type);
    if(!self->dec_button)
        goto fail_dec_button;
    self->dec_button->style = &ctx->style.property.dec_button;

    return (PyObject*)self;

fail_dec_button:
    Py_DECREF(self->inc_button);
fail_inc_button:
    Py_DECREF(self->edit);
fail_edit:
    Py_DECREF(self);
    return NULL;
}

static void PyUIPropertyStyle_dealloc(PyUIPropertyStyleObject *self)
{
    Py_DECREF(self->dec_button);
    Py_DECREF(self->inc_button);
    Py_DECREF(self->edit);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject *PyUIPropertyStyle_get_normal(PyUIPropertyStyleObject *self, void *closure)
{
    return style_get_item(&self->style->normal);
}

static int PyUIPropertyStyle_set_normal(PyUIPropertyStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->normal);
}

static PyObject *PyUIPropertyStyle_get_hover(PyUIPropertyStyleObject *self, void *closure)
{
    return style_get_item(&self->style->hover);
}

static int PyUIPropertyStyle_set_hover(PyUIPropertyStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->hover);
}

static PyObject *PyUIPropertyStyle_get_active(PyUIPropertyStyleObject *self, void *closure)
{
    return style_get_item(&self->style->active);
}

static int PyUIPropertyStyle_set_active(PyUIPropertyStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->active);
}

static PyObject *PyUIPropertyStyle_get_border_color(PyUIPropertyStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->border_color.r,
        self->style->border_color.g,
        self->style->border_color.b,
        self->style->border_color.a);
}

static int PyUIPropertyStyle_set_border_color(PyUIPropertyStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->border_color = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIPropertyStyle_get_label_normal(PyUIPropertyStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->label_normal.r,
        self->style->label_normal.g,
        self->style->label_normal.b,
        self->style->label_normal.a);
}

static int PyUIPropertyStyle_set_label_normal(PyUIPropertyStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->label_normal = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIPropertyStyle_get_label_hover(PyUIPropertyStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->label_hover.r,
        self->style->label_hover.g,
        self->style->label_hover.b,
        self->style->label_hover.a);
}

static int PyUIPropertyStyle_set_label_hover(PyUIPropertyStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->label_hover = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIPropertyStyle_get_label_active(PyUIPropertyStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->label_active.r,
        self->style->label_active.g,
        self->style->label_active.b,
        self->style->label_active.a);
}

static int PyUIPropertyStyle_set_label_active(PyUIPropertyStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->label_active = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIPropertyStyle_get_sym_left(PyUIPropertyStyleObject *self, void *closure)
{
    return Py_BuildValue("i", self->style->sym_left);
}

static int PyUIPropertyStyle_set_sym_left(PyUIPropertyStyleObject *self, PyObject *value, void *closure)
{
    if(!PyInt_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be an int.");
        return -1; 
    }

    self->style->sym_left = PyInt_AS_LONG(value);
    return 0;
}

static PyObject *PyUIPropertyStyle_get_sym_right(PyUIPropertyStyleObject *self, void *closure)
{
    return Py_BuildValue("i", self->style->sym_right);
}

static int PyUIPropertyStyle_set_sym_right(PyUIPropertyStyleObject *self, PyObject *value, void *closure)
{
    if(!PyInt_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be an int.");
        return -1; 
    }

    self->style->sym_right = PyInt_AS_LONG(value);
    return 0;
}

static PyObject *PyUIPropertyStyle_get_border(PyUIPropertyStyleObject *self, void *closure)
{
    return Py_BuildValue("f", self->style->border);
}

static int PyUIPropertyStyle_set_border(PyUIPropertyStyleObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be a float.");
        return -1; 
    }

    self->style->border = PyFloat_AsDouble(value);
    return 0;
}

static PyObject *PyUIPropertyStyle_get_rounding(PyUIPropertyStyleObject *self, void *closure)
{
    return Py_BuildValue("f", self->style->rounding);
}

static int PyUIPropertyStyle_set_rounding(PyUIPropertyStyleObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be a float.");
        return -1; 
    }

    self->style->rounding = PyFloat_AsDouble(value);
    return 0;
}

static PyObject *PyUIPropertyStyle_get_padding(PyUIPropertyStyleObject *self, void *closure)
{
    return Py_BuildValue("(f,f)", 
        self->style->padding.x,
        self->style->padding.y);
}

static int PyUIPropertyStyle_set_padding(PyUIPropertyStyleObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style->padding = (struct nk_vec2){x, y};
    return 0;
}

static PyObject *PyUIPropertyStyle_get_edit(PyUIPropertyStyleObject *self, void *closure)
{
    Py_INCREF(self->edit);
    return (PyObject*)self->edit;
}

static PyObject *PyUIPropertyStyle_get_inc_button(PyUIPropertyStyleObject *self, void *closure)
{
    Py_INCREF(self->inc_button);
    return (PyObject*)self->inc_button;
}

static PyObject *PyUIPropertyStyle_get_dec_button(PyUIPropertyStyleObject *self, void *closure)
{
    Py_INCREF(self->dec_button);
    return (PyObject*)self->dec_button;
}

static PyObject *PyUIPropertyStyle_pickle(PyUIPropertyStyleObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *ret = NULL;

    SDL_RWops *stream = PFSDL_VectorRWOps();
    CHK_TRUE(stream, fail_alloc);
    CHK_TRUE(save_property(stream, self->style), fail_pickle);
    ret = PyString_FromStringAndSize(PFSDL_VectorRWOpsRaw(stream), SDL_RWsize(stream));

fail_pickle:
    SDL_RWclose(stream);
fail_alloc:
    if(!ret) {
        PyErr_SetString(PyExc_RuntimeError, "Error pickling pf.UIPropertyStyle object");
    }
    return ret;
}

static PyObject *PyUIPropertyStyle_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs)
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

    PyUIPropertyStyleObject *styleobj = 
        (PyUIPropertyStyleObject*)PyObject_CallFunctionObjArgs((PyObject*)&PyUIPropertyStyle_type, NULL);
    assert(styleobj || PyErr_Occurred());
    CHK_TRUE(styleobj, fail_unpickle);

    CHK_TRUE(load_property(stream, ((PyUIPropertyStyleObject*)styleobj)->style), fail_unpickle);

    Py_ssize_t nread = SDL_RWseek(stream, 0, RW_SEEK_CUR);
    ret = Py_BuildValue("(Oi)", styleobj, (int)nread);
    Py_DECREF(styleobj);

fail_unpickle:
    SDL_RWclose(stream);
fail_args:
    if(!ret) {
        PyErr_SetString(PyExc_RuntimeError, "Error unpickling pf.UIPropertyStyle object");
    }
    return ret;
}


static PyObject *PyUISliderStyle_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PyUISliderStyleObject *self = (PyUISliderStyleObject*)type->tp_alloc(type, 0);
    if(!self)
        return NULL;

    self->inc_button = PyObject_New(PyUIButtonStyleObject, &PyUIButtonStyle_type);
    if(!self->inc_button)
        goto fail_inc_button;

    self->dec_button = PyObject_New(PyUIButtonStyleObject, &PyUIButtonStyle_type);
    if(!self->dec_button)
        goto fail_dec_button;

    struct nk_context *ctx = UI_GetContext();
    self->style = &ctx->style.slider;
    self->inc_button->style = &ctx->style.slider.inc_button;
    self->dec_button->style = &ctx->style.slider.dec_button;

    return (PyObject*)self;

fail_dec_button:
    Py_DECREF(self->inc_button);
fail_inc_button:
    Py_DECREF(self);
    return NULL;
}

static void PyUISliderStyle_dealloc(PyUISliderStyleObject *self)
{
    Py_DECREF(self->inc_button);
    Py_DECREF(self->dec_button);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject *PyUISliderStyle_get_normal(PyUISliderStyleObject *self, void *closure)
{
    return style_get_item(&self->style->normal);
}

static int PyUISliderStyle_set_normal(PyUISliderStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->normal);
}

static PyObject *PyUISliderStyle_get_hover(PyUISliderStyleObject *self, void *closure)
{
    return style_get_item(&self->style->hover);
}

static int PyUISliderStyle_set_hover(PyUISliderStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->hover);
}

static PyObject *PyUISliderStyle_get_active(PyUISliderStyleObject *self, void *closure)
{
    return style_get_item(&self->style->active);
}

static int PyUISliderStyle_set_active(PyUISliderStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->active);
}

static PyObject *PyUISliderStyle_get_border_color(PyUISliderStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->border_color.r,
        self->style->border_color.g,
        self->style->border_color.b,
        self->style->border_color.a);
}

static int PyUISliderStyle_set_border_color(PyUISliderStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->border_color = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUISliderStyle_get_bar_normal(PyUISliderStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->bar_normal.r,
        self->style->bar_normal.g,
        self->style->bar_normal.b,
        self->style->bar_normal.a);
}

static int PyUISliderStyle_set_bar_normal(PyUISliderStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->bar_normal = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUISliderStyle_get_bar_hover(PyUISliderStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->bar_hover.r,
        self->style->bar_hover.g,
        self->style->bar_hover.b,
        self->style->bar_hover.a);
}

static int PyUISliderStyle_set_bar_hover(PyUISliderStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->bar_hover = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUISliderStyle_get_bar_active(PyUISliderStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->bar_active.r,
        self->style->bar_active.g,
        self->style->bar_active.b,
        self->style->bar_active.a);
}

static int PyUISliderStyle_set_bar_active(PyUISliderStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->bar_active = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUISliderStyle_get_bar_filled(PyUISliderStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->bar_filled.r,
        self->style->bar_filled.g,
        self->style->bar_filled.b,
        self->style->bar_filled.a);
}

static int PyUISliderStyle_set_bar_filled(PyUISliderStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->bar_filled = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUISliderStyle_get_cursor_normal(PyUISliderStyleObject *self, void *closure)
{
    return style_get_item(&self->style->cursor_normal);
}

static int PyUISliderStyle_set_cursor_normal(PyUISliderStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->cursor_normal);
}

static PyObject *PyUISliderStyle_get_cursor_hover(PyUISliderStyleObject *self, void *closure)
{
    return style_get_item(&self->style->cursor_hover);
}

static int PyUISliderStyle_set_cursor_hover(PyUISliderStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->cursor_hover);
}

static PyObject *PyUISliderStyle_get_cursor_active(PyUISliderStyleObject *self, void *closure)
{
    return style_get_item(&self->style->cursor_active);
}

static int PyUISliderStyle_set_cursor_active(PyUISliderStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->cursor_active);
}

static PyObject *PyUISliderStyle_get_border(PyUISliderStyleObject *self, void *closure)
{
    return Py_BuildValue("f", self->style->border);
}

static int PyUISliderStyle_set_border(PyUISliderStyleObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be a float.");
        return -1; 
    }

    self->style->border = PyFloat_AsDouble(value);
    return 0;
}

static PyObject *PyUISliderStyle_get_rounding(PyUISliderStyleObject *self, void *closure)
{
    return Py_BuildValue("f", self->style->rounding);
}

static int PyUISliderStyle_set_rounding(PyUISliderStyleObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be a float.");
        return -1; 
    }

    self->style->rounding = PyFloat_AsDouble(value);
    return 0;
}

static PyObject *PyUISliderStyle_get_bar_height(PyUISliderStyleObject *self, void *closure)
{
    return Py_BuildValue("f", self->style->bar_height);
}

static int PyUISliderStyle_set_bar_height(PyUISliderStyleObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be a float.");
        return -1; 
    }

    self->style->bar_height = PyFloat_AsDouble(value);
    return 0;
}

static PyObject *PyUISliderStyle_get_padding(PyUISliderStyleObject *self, void *closure)
{
    return Py_BuildValue("(f,f)", 
        self->style->padding.x,
        self->style->padding.y);
}

static int PyUISliderStyle_set_padding(PyUISliderStyleObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style->padding = (struct nk_vec2){x, y};
    return 0;
}

static PyObject *PyUISliderStyle_get_spacing(PyUISliderStyleObject *self, void *closure)
{
    return Py_BuildValue("(f,f)", 
        self->style->spacing.x,
        self->style->spacing.y);
}

static int PyUISliderStyle_set_spacing(PyUISliderStyleObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style->spacing = (struct nk_vec2){x, y};
    return 0;
}

static PyObject *PyUISliderStyle_get_cursor_size(PyUISliderStyleObject *self, void *closure)
{
    return Py_BuildValue("(f,f)", 
        self->style->cursor_size.x,
        self->style->cursor_size.y);
}

static int PyUISliderStyle_set_cursor_size(PyUISliderStyleObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style->cursor_size = (struct nk_vec2){x, y};
    return 0;
}

static PyObject *PyUISliderStyle_get_show_buttons(PyUISliderStyleObject *self, void *closure)
{
    if(self->style->show_buttons) {
        Py_RETURN_TRUE;
    }else{
        Py_RETURN_FALSE;
    }
}

static int PyUISliderStyle_set_show_buttons(PyUISliderStyleObject *self, PyObject *value, void *closure)
{
    self->style->show_buttons = PyObject_IsTrue(value);
    return 0;
}

static PyObject *PyUISliderStyle_get_inc_button(PyUISliderStyleObject *self, void *closure)
{
    Py_INCREF(self->inc_button);
    return (PyObject*)self->inc_button;
}

static PyObject *PyUISliderStyle_get_dec_button(PyUISliderStyleObject *self, void *closure)
{
    Py_INCREF(self->dec_button);
    return (PyObject*)self->dec_button;
}

static PyObject *PyUISliderStyle_get_inc_symbol(PyUISliderStyleObject *self, void *closure)
{
    return PyInt_FromLong(self->style->inc_symbol);
}

static int PyUISliderStyle_set_inc_symbol(PyUISliderStyleObject *self, PyObject *value, void *closure)
{
    if(!PyInt_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be an integer.");
        return -1; 
    }

    self->style->inc_symbol = PyInt_AsLong(value);
    return 0;
}

static PyObject *PyUISliderStyle_get_dec_symbol(PyUISliderStyleObject *self, void *closure)
{
    return PyInt_FromLong(self->style->dec_symbol);
}

static int PyUISliderStyle_set_dec_symbol(PyUISliderStyleObject *self, PyObject *value, void *closure)
{
    if(!PyInt_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be an integer.");
        return -1; 
    }

    self->style->dec_symbol = PyInt_AsLong(value);
    return 0;
}

static PyObject *PyUISliderStyle_pickle(PyUISliderStyleObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *ret = NULL;

    SDL_RWops *stream = PFSDL_VectorRWOps();
    CHK_TRUE(stream, fail_alloc);
    CHK_TRUE(save_slider(stream, self->style), fail_pickle);
    ret = PyString_FromStringAndSize(PFSDL_VectorRWOpsRaw(stream), SDL_RWsize(stream));

fail_pickle:
    SDL_RWclose(stream);
fail_alloc:
    if(!ret) {
        PyErr_SetString(PyExc_RuntimeError, "Error pickling pf.UISliderStyle object");
    }
    return ret;
}

static PyObject *PyUISliderStyle_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs)
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

    PyUISliderStyleObject *styleobj = 
        (PyUISliderStyleObject*)PyObject_CallFunctionObjArgs((PyObject*)&PyUISliderStyle_type, NULL);
    assert(styleobj || PyErr_Occurred());
    CHK_TRUE(styleobj, fail_unpickle);

    CHK_TRUE(load_slider(stream, ((PyUISliderStyleObject*)styleobj)->style), fail_unpickle);

    Py_ssize_t nread = SDL_RWseek(stream, 0, RW_SEEK_CUR);
    ret = Py_BuildValue("(Oi)", styleobj, (int)nread);
    Py_DECREF(styleobj);

fail_unpickle:
    SDL_RWclose(stream);
fail_args:
    if(!ret) {
        PyErr_SetString(PyExc_RuntimeError, "Error unpickling pf.UISliderStyle object");
    }
    return ret;
}

static PyObject *PyUIProgressStyle_get_normal(PyUIProgressStyleObject *self, void *closure)
{
    return style_get_item(&self->style->normal);
}

static int PyUIProgressStyle_set_normal(PyUIProgressStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->normal);
}

static PyObject *PyUIProgressStyle_get_hover(PyUIProgressStyleObject *self, void *closure)
{
    return style_get_item(&self->style->hover);
}

static int PyUIProgressStyle_set_hover(PyUIProgressStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->hover);
}

static PyObject *PyUIProgressStyle_get_active(PyUIProgressStyleObject *self, void *closure)
{
    return style_get_item(&self->style->active);
}

static int PyUIProgressStyle_set_active(PyUIProgressStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->active);
}

static PyObject *PyUIProgressStyle_get_border_color(PyUIProgressStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->border_color.r,
        self->style->border_color.g,
        self->style->border_color.b,
        self->style->border_color.a);
}

static int PyUIProgressStyle_set_border_color(PyUIProgressStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->border_color = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIProgressStyle_get_cursor_normal(PyUIProgressStyleObject *self, void *closure)
{
    return style_get_item(&self->style->cursor_normal);
}

static int PyUIProgressStyle_set_cursor_normal(PyUIProgressStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->cursor_normal);
}

static PyObject *PyUIProgressStyle_get_cursor_hover(PyUIProgressStyleObject *self, void *closure)
{
    return style_get_item(&self->style->cursor_hover);
}

static int PyUIProgressStyle_set_cursor_hover(PyUIProgressStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->cursor_hover);
}

static PyObject *PyUIProgressStyle_get_cursor_active(PyUIProgressStyleObject *self, void *closure)
{
    return style_get_item(&self->style->cursor_active);
}

static int PyUIProgressStyle_set_cursor_active(PyUIProgressStyleObject *self, PyObject *value, void *closure)
{
    return style_set_item(value, &self->style->cursor_active);
}

static PyObject *PyUIProgressStyle_get_cursor_border_color(PyUIProgressStyleObject *self, void *closure)
{
    return Py_BuildValue("(i,i,i,i)", 
        self->style->cursor_border_color.r,
        self->style->cursor_border_color.g,
        self->style->cursor_border_color.b,
        self->style->cursor_border_color.a);
}

static int PyUIProgressStyle_set_cursor_border_color(PyUIProgressStyleObject *self, PyObject *value, void *closure)
{
    float rgba[4];

    if(parse_rgba(value, rgba) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be an (R, G, B, A) tuple.");
        return -1; 
    }

    self->style->cursor_border_color = (struct nk_color){rgba[0], rgba[1], rgba[2], rgba[3]};
    return 0;
}

static PyObject *PyUIProgressStyle_get_rounding(PyUIProgressStyleObject *self, void *closure)
{
    return Py_BuildValue("f", self->style->rounding);
}

static int PyUIProgressStyle_set_rounding(PyUIProgressStyleObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be a float.");
        return -1; 
    }

    self->style->rounding = PyFloat_AsDouble(value);
    return 0;
}

static PyObject *PyUIProgressStyle_get_border(PyUIProgressStyleObject *self, void *closure)
{
    return Py_BuildValue("f", self->style->border);
}

static int PyUIProgressStyle_set_border(PyUIProgressStyleObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be a float.");
        return -1; 
    }

    self->style->border = PyFloat_AsDouble(value);
    return 0;
}

static PyObject *PyUIProgressStyle_get_cursor_border(PyUIProgressStyleObject *self, void *closure)
{
    return Py_BuildValue("f", self->style->cursor_border);
}

static int PyUIProgressStyle_set_cursor_border(PyUIProgressStyleObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be a float.");
        return -1; 
    }

    self->style->cursor_border = PyFloat_AsDouble(value);
    return 0;
}

static PyObject *PyUIProgressStyle_get_cursor_rounding(PyUIProgressStyleObject *self, void *closure)
{
    return Py_BuildValue("f", self->style->cursor_rounding);
}

static int PyUIProgressStyle_set_cursor_rounding(PyUIProgressStyleObject *self, PyObject *value, void *closure)
{
    if(!PyFloat_Check(value)) {
        PyErr_SetString(PyExc_TypeError, "Type must be a float.");
        return -1; 
    }

    self->style->cursor_rounding = PyFloat_AsDouble(value);
    return 0;
}

static PyObject *PyUIProgressStyle_get_padding(PyUIProgressStyleObject *self, void *closure)
{
    return Py_BuildValue("(f,f)", 
        self->style->padding.x,
        self->style->padding.y);
}

static int PyUIProgressStyle_set_padding(PyUIProgressStyleObject *self, PyObject *value, void *closure)
{
    float x, y;

    if(parse_float_pair(value, &x, &y) != 0) {
        PyErr_SetString(PyExc_TypeError, "Type must be a tuple of 2 floats.");
        return -1; 
    }

    self->style->padding = (struct nk_vec2){x, y};
    return 0;
}

static PyObject *PyUIProgressStyle_pickle(PyUIProgressStyleObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *ret = NULL;

    SDL_RWops *stream = PFSDL_VectorRWOps();
    CHK_TRUE(stream, fail_alloc);
    CHK_TRUE(save_progress(stream, self->style), fail_pickle);
    ret = PyString_FromStringAndSize(PFSDL_VectorRWOpsRaw(stream), SDL_RWsize(stream));

fail_pickle:
    SDL_RWclose(stream);
fail_alloc:
    if(!ret) {
        PyErr_SetString(PyExc_RuntimeError, "Error pickling pf.UIProgressStyle object");
    }
    return ret;
}

static PyObject *PyUIProgressStyle_unpickle(PyObject *cls, PyObject *args, PyObject *kwargs)
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

    PyObject *styleobj = PyObject_New(PyObject, &PyUIProgressStyle_type);
    assert(styleobj || PyErr_Occurred());
    CHK_TRUE(styleobj, fail_unpickle);

    struct nk_context *ctx = UI_GetContext();
    ((PyUIProgressStyleObject*)styleobj)->style = &ctx->style.progress;

    CHK_TRUE(load_progress(stream, ((PyUIProgressStyleObject*)styleobj)->style), fail_unpickle);

    Py_ssize_t nread = SDL_RWseek(stream, 0, RW_SEEK_CUR);
    ret = Py_BuildValue("(Oi)", styleobj, (int)nread);
    Py_DECREF(styleobj);

fail_unpickle:
    SDL_RWclose(stream);
fail_args:
    if(!ret) {
        PyErr_SetString(PyExc_RuntimeError, "Error unpickling pf.UIProgressStyle object");
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

    /* Combo style */
    if(PyType_Ready(&PyUIComboStyle_type) < 0)
        return;
    Py_INCREF(&PyUIComboStyle_type);
    PyModule_AddObject(module, "UIComboStyle", (PyObject*)&PyUIComboStyle_type);

    /* Toggle style */
    if(PyType_Ready(&PyUIToggleStyle_type) < 0)
        return;
    Py_INCREF(&PyUIToggleStyle_type);
    PyModule_AddObject(module, "UIToggleStyle", (PyObject*)&PyUIToggleStyle_type);

    /* Scrollbar style */
    if(PyType_Ready(&PyUIScrollbarStyle_type) < 0)
        return;
    Py_INCREF(&PyUIScrollbarStyle_type);
    PyModule_AddObject(module, "UIScrollbarStyle", (PyObject*)&PyUIScrollbarStyle_type);

    /* Edit style */
    if(PyType_Ready(&PyUIEditStyle_type) < 0)
        return;
    Py_INCREF(&PyUIEditStyle_type);
    PyModule_AddObject(module, "UIEditStyle", (PyObject*)&PyUIEditStyle_type);

    /* Property style */
    if(PyType_Ready(&PyUIPropertyStyle_type) < 0)
        return;
    Py_INCREF(&PyUIPropertyStyle_type);
    PyModule_AddObject(module, "UIPropertyStyle", (PyObject*)&PyUIPropertyStyle_type);

    /* Slider style */
    if(PyType_Ready(&PyUISliderStyle_type) < 0)
        return;
    Py_INCREF(&PyUISliderStyle_type);
    PyModule_AddObject(module, "UISliderStyle", (PyObject*)&PyUISliderStyle_type);

    /* Progress style */
    if(PyType_Ready(&PyUIProgressStyle_type) < 0)
        return;
    Py_INCREF(&PyUIProgressStyle_type);
    PyModule_AddObject(module, "UIProgressStyle", (PyObject*)&PyUIProgressStyle_type);

    /* Global style objects */
    PyUIButtonStyleObject *button_style = PyObject_New(PyUIButtonStyleObject, &PyUIButtonStyle_type);
    assert(button_style);
    button_style->style = &ctx->style.button;
    button_style->type = BUTTON_REGULAR;
    PyModule_AddObject(module, "button_style", (PyObject*)button_style);

    PyUIButtonStyleObject *ctx_button_style = PyObject_New(PyUIButtonStyleObject, &PyUIButtonStyle_type);
    assert(ctx_button_style);
    ctx_button_style->style = &ctx->style.contextual_button;
    ctx_button_style->type = BUTTON_CONTEXTUAL;
    PyModule_AddObject(module, "contextual_button_style", (PyObject*)ctx_button_style);

    PyUIButtonStyleObject *menu_button_style = PyObject_New(PyUIButtonStyleObject, &PyUIButtonStyle_type);
    assert(menu_button_style);
    menu_button_style->style = &ctx->style.menu_button;
    menu_button_style->type = BUTTON_MENU;
    PyModule_AddObject(module, "menu_button_style", (PyObject*)menu_button_style);

    PyUISelectableStyleObject *sel_style 
        = PyObject_New(PyUISelectableStyleObject, &PyUISelectableStyle_type);
    assert(sel_style);
    sel_style->style = &ctx->style.selectable;
    PyModule_AddObject(module, "selectable_style", (PyObject*)sel_style);

    PyUIComboStyleObject *combo_style = 
        (PyUIComboStyleObject*)PyObject_CallFunctionObjArgs((PyObject*)&PyUIComboStyle_type, NULL);
    assert(combo_style);
    PyModule_AddObject(module, "combo_style", (PyObject*)combo_style);

    PyUIToggleStyleObject *option_style = PyObject_New(PyUIToggleStyleObject, &PyUIToggleStyle_type);
    assert(option_style);
    option_style->style = &ctx->style.option;
    option_style->type = TOGGLE_OPTION;
    PyModule_AddObject(module, "option_style", (PyObject*)option_style);

    PyUIToggleStyleObject *checkbox_style = PyObject_New(PyUIToggleStyleObject, &PyUIToggleStyle_type);
    assert(checkbox_style);
    checkbox_style->style = &ctx->style.checkbox;
    checkbox_style->type = TOGGLE_CHECKBOX;
    PyModule_AddObject(module, "checkbox_style", (PyObject*)checkbox_style);

    PyUIScrollbarStyleObject *scrollbar_hori_style = PyObject_New(PyUIScrollbarStyleObject, &PyUIScrollbarStyle_type);
    assert(scrollbar_hori_style);
    scrollbar_hori_style->style = &ctx->style.scrollh;
    scrollbar_hori_style->type = SCROLLBAR_HORIZONTAL;
    PyModule_AddObject(module, "scrollbar_horizontal_style", (PyObject*)scrollbar_hori_style);

    PyUIScrollbarStyleObject *scrollbar_vert_style = PyObject_New(PyUIScrollbarStyleObject, &PyUIScrollbarStyle_type);
    assert(scrollbar_vert_style);
    scrollbar_vert_style->style = &ctx->style.scrollv;
    scrollbar_vert_style->type = SCROLLBAR_VERTICAL;
    PyModule_AddObject(module, "scrollbar_vertical_style", (PyObject*)scrollbar_vert_style);

    PyUIEditStyleObject *edit_style = (PyUIEditStyleObject*)PyObject_CallFunction(
        (PyObject*)&PyUIEditStyle_type, 
        "i", EDIT_REGULAR
    );
    assert(edit_style);
    PyModule_AddObject(module, "edit_style", (PyObject*)edit_style);

    PyUIPropertyStyleObject *property_style = (PyUIPropertyStyleObject*)PyObject_CallFunctionObjArgs(
        (PyObject*)&PyUIPropertyStyle_type, NULL);
    assert(property_style);
    PyModule_AddObject(module, "property_style", (PyObject*)property_style);

    PyUISliderStyleObject *slider_style = (PyUISliderStyleObject*)PyObject_CallFunctionObjArgs(
        (PyObject*)&PyUISliderStyle_type, NULL);
    assert(slider_style);
    PyModule_AddObject(module, "slider_style", (PyObject*)slider_style);

    PyUIProgressStyleObject *progress_style = PyObject_New(PyUIProgressStyleObject, &PyUIProgressStyle_type);
    assert(progress_style);
    progress_style->style = &ctx->style.progress;
    PyModule_AddObject(module, "progress_style", (PyObject*)progress_style);
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

