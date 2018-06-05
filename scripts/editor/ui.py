#
#  This file is part of Permafrost Engine. 
#  Copyright (C) 2018 Eduard Permyakov 
#
#  Permafrost Engine is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, either version 3 of the License, or
#  (at your option) any later version.
#
#  Permafrost Engine is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program.  If not, see <http://www.gnu.org/licenses/>.
#

import pf
from constants import *
from map import Material

TAB_BAR_HEIGHT = 40
TAB_BAR_NUM_COLS = 5
TAB_BAR_COL_WIDTH = 120
LEFT_PANE_WIDTH = 250

def format_str_for_numlist(numlist):
    ret = "["
    for i, num in enumerate(numlist):
        ret += "{0:.1f}".format(num)
        if i != len(numlist)-1:
            ret += ", "
    ret += "]"
    return ret


class TabBarWindow(pf.Window):
    SELECTED_COLOR = (90, 90, 90, 255)
    SELECTED_HOVER_COLOR = (75, 75, 75, 255)

    def __init__(self):
        resx, _ = pf.get_resolution()

        dims = (0, 0, resx - TAB_BAR_COL_WIDTH, TAB_BAR_HEIGHT)
        super(TabBarWindow, self).__init__("TabBar", dims, NK_WINDOW_NO_SCROLLBAR)
        self.active_idx = 0
        self.labels = []
        self.child_windows = []

    def push_child(self, label, window):
        assert isinstance(label, basestring)
        assert isinstance(window, pf.Window)

        self.labels.append(label) 
        self.child_windows.append(window)

    def update(self):
        orig_rounding = pf.button_style.rounding
        pf.button_style.rounding = 0.0
        self.layout_row_begin(NK_STATIC, TAB_BAR_HEIGHT-10, TAB_BAR_NUM_COLS)
        for idx in range(0, len(self.child_windows)):

            def on_tab_click():
                self.active_idx = idx;
                self.__show_active()
                pf.global_event(EVENT_TOP_TAB_SELECTION_CHANGED, self.active_idx)

            self.layout_row_push(TAB_BAR_COL_WIDTH)

            if idx == self.active_idx:
                normal_style = pf.button_style.normal
                hover_style = pf.button_style.hover
                pf.button_style.normal = TabBarWindow.SELECTED_COLOR
                pf.button_style.hover = TabBarWindow.SELECTED_HOVER_COLOR
                self.button_label(self.labels[idx], on_tab_click)
                pf.button_style.normal = normal_style
                pf.button_style.hover = hover_style
            else:
                self.button_label(self.labels[idx], on_tab_click)

        self.layout_row_end()
        pf.button_style.rounding = orig_rounding

    def show(self):
        super(TabBarWindow, self).show()
        self.__show_active()

    def __show_active(self):
        for idx, window in enumerate(self.child_windows):
            if idx == self.active_idx:
                window.show()
            else:
                window.hide() 


class TerrainTabWindow(pf.Window):

    def __init__(self):
        _, resy = pf.get_resolution()
        super(TerrainTabWindow, self).__init__("TerrainTab", 
            (0, TAB_BAR_HEIGHT + 1, LEFT_PANE_WIDTH, resy - TAB_BAR_HEIGHT - 1), NK_WINDOW_BORDER)

        self.selected_mat_idx = 0
        self.brush_size_idx = 0
        self.brush_type_idx = 0
        self.edges_type_idx = 0
        self.selected_tile = None
        self.materials_list = []
        self.heights = [h for h in range(0, 10)]
        self.selected_height_idx = 0

    def update(self):
        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Selected Tile:", (255, 255, 255))

        self.layout_row_dynamic(30, 1)
        label = str(self.selected_tile) if self.selected_tile is None \
            else "Chunk: {0} Tile: {1}".format(self.selected_tile[0], self.selected_tile[1])
        self.label_colored_wrap(label, (200, 200, 0))

        # Brush type
        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Brush Type:", (255, 255, 255))

        old_brush_type_idx = self.brush_type_idx
        self.layout_row_dynamic(20, 2)
        if self.option_label("Texture", self.brush_type_idx == 0):
            self.brush_type_idx = 0
        if self.option_label("Elevation", self.brush_type_idx == 1):
            self.brush_type_idx = 1
        self.layout_row_dynamic(10, 1)

        if self.brush_type_idx != old_brush_type_idx:
            pf.global_event(EVENT_TERRAIN_BRUSH_TYPE_CHANGED, self.brush_type_idx)

        # Brush size
        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Brush Size:", (255, 255, 255))

        old_brush_size_idx = self.brush_size_idx
        self.layout_row_dynamic(20, 2)
        if self.option_label("Small", self.brush_size_idx == 0):
            self.brush_size_idx = 0
        if self.option_label("Large", self.brush_size_idx == 1):
            self.brush_size_idx = 1
        self.layout_row_dynamic(10, 1)

        if self.brush_size_idx != old_brush_size_idx:
            pf.global_event(EVENT_TERRAIN_BRUSH_SIZE_CHANGED, self.brush_size_idx)

        if self.brush_type_idx == 0:
            # Texture
            self.layout_row_dynamic(20, 1)
            self.label_colored_wrap("Texture:", (255, 255, 255))

            def textures_group():
                self.layout_row_static(25, LEFT_PANE_WIDTH-60, 1)
                for i in range(0, len(self.materials_list)):
                    old = self.selected_mat_idx
                    on = self.selectable_label(self.materials_list[i].name, 
                        NK_TEXT_ALIGN_LEFT, i == self.selected_mat_idx)
                    if on: 
                        self.selected_mat_idx = i
                    if self.selected_mat_idx != old:
                        pf.global_event(EVENT_TEXTURE_SELECTION_CHANGED, i)

            self.layout_row_static(400, LEFT_PANE_WIDTH-30, 1)
            self.group("Texture:", NK_WINDOW_BORDER, textures_group)
        else:
            # Elevation
            self.layout_row_dynamic(20, 1)
            self.label_colored_wrap("Edges:", (255, 255, 255))

            old_edges_type_idx = self.edges_type_idx
            self.layout_row_dynamic(20, 2)
            if self.option_label("Hard", self.edges_type_idx == 0):
                self.edges_type_idx = 0
            if self.option_label("Smooth", self.edges_type_idx == 1):
                self.edges_type_idx = 1
            self.layout_row_dynamic(10, 1)

            if old_edges_type_idx != self.edges_type_idx:
                pf.global_event(EVENT_TERRAIN_EDGE_TYPE_CHANGED, self.edges_type_idx)

            self.layout_row_dynamic(20, 1)
            self.label_colored_wrap("Height:", (255, 255, 255))

            self.layout_row_static(25, LEFT_PANE_WIDTH - 30, 1)
            old_height_idx = self.selected_height_idx
            self.selected_height_idx = self.combo_box([str(h) for h in self.heights], self.selected_height_idx, 25, (LEFT_PANE_WIDTH - 30, 200))
            if old_height_idx != self.selected_height_idx:
                pf.global_event(EVENT_HEIGHT_SELECTION_CHANGED, self.heights[self.selected_height_idx])


class ObjectsTabWindow(pf.Window):

    OBJECTS_MODE_PLACE  = 0
    OBJECTS_MODE_SELECT = 1

    def __init__(self):
        _, resy = pf.get_resolution()
        super(ObjectsTabWindow, self).__init__("ObjectsTab", 
            (0, TAB_BAR_HEIGHT + 1, LEFT_PANE_WIDTH, resy - TAB_BAR_HEIGHT - 1), NK_WINDOW_BORDER)
        self.mode = self.OBJECTS_MODE_PLACE
        self.objects_list = []
        self.selected_object_idx = 0

    def update(self):

        # Mode
        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Mode:", (255, 255, 255))

        old_mode = self.mode
        self.layout_row_dynamic(20, 2)
        if self.option_label("Place", self.mode == self.OBJECTS_MODE_PLACE):
            self.mode = self.OBJECTS_MODE_PLACE
        if self.option_label("Select", self.mode == self.OBJECTS_MODE_SELECT):
            self.mode = self.OBJECTS_MODE_SELECT
        self.layout_row_dynamic(10, 1)

        if self.mode != old_mode:
            pf.global_event(EVENT_OBJECTS_TAB_MODE_CHANGED, self.mode)

        if self.mode == self.OBJECTS_MODE_PLACE:
            # Objects
            self.layout_row_dynamic(20, 1)
            self.label_colored_wrap("Objects:", (255, 255, 255))

            def objects_group():
                self.layout_row_static(25, LEFT_PANE_WIDTH-60, 1)
                for i in range(0, len(self.objects_list)):
                    old = self.selected_object_idx
                    on = self.selectable_label(self.objects_list[i], 
                        NK_TEXT_ALIGN_LEFT, i == self.selected_object_idx)
                    if on: 
                        self.selected_object_idx = i
                    if self.selected_object_idx != old:
                        pf.global_event(EVENT_OBJECT_SELECTION_CHANGED, i)

            self.layout_row_static(400, LEFT_PANE_WIDTH-30, 1)
            self.group("Objects", NK_WINDOW_BORDER, objects_group)
        elif self.mode == self.OBJECTS_MODE_SELECT:
            # Selection
            sel_obj_list = pf.get_unit_selection()

            if len(sel_obj_list) == 0:
                return

            self.layout_row_dynamic(20, 1)
            self.label_colored_wrap("Selection:", (255, 255, 255))

            if len(sel_obj_list) > 1:
                def selection_group():
                    self.layout_row_static(25, LEFT_PANE_WIDTH-60, 1)
                    for i in range(0, len(sel_obj_list)):
                        name = "{0} {1}".format(sel_obj_list[i].name, format_str_for_numlist(sel_obj_list[i].pos))
                        on = self.selectable_label(name, NK_TEXT_ALIGN_LEFT, False)
                        if on:
                            pf.global_event(EVENT_OBJECT_SELECTED_UNIT_PICKED, sel_obj_list[i])
                self.layout_row_static(400, LEFT_PANE_WIDTH-30, 1)
                self.group("Selection", NK_WINDOW_BORDER, selection_group)
            else:
                assert(len(sel_obj_list) == 1)
                self.layout_row_dynamic(10, 1)
                self.layout_row_dynamic(20, 1)
                self.label_colored_wrap(sel_obj_list[0].name, (200, 200, 0))

                pos_str = "Position: {0}".format(format_str_for_numlist(sel_obj_list[0].pos))
                self.layout_row_dynamic(20, 1)
                self.label_colored_wrap(pos_str, (255, 255, 255))

                rot_str = "Rotation: {0}".format(format_str_for_numlist(sel_obj_list[0].rotation))
                self.layout_row_dynamic(20, 1)
                self.label_colored_wrap(rot_str, (255, 255, 255))

                scale_str = "Scale: {0}".format(format_str_for_numlist(sel_obj_list[0].scale))
                self.layout_row_dynamic(20, 1)
                self.label_colored_wrap(scale_str, (255, 255, 255))

                select_str = "Selectable: {0}".format("True" if sel_obj_list[0].selectable else "False")
                self.layout_row_dynamic(20, 1)
                self.label_colored_wrap(select_str, (255, 255, 255))

            def on_delete():
                pf.global_event(EVENT_OBJECT_DELETE_SELECTION, None)

            self.layout_row_dynamic(30, 1)
            self.button_label("Delete", on_delete)


class MenuButtonWindow(pf.Window):
    
    def __init__(self, menu_window):
        resx, _ = pf.get_resolution()
        super(MenuButtonWindow, self).__init__("MenuButton", 
            (resx - TAB_BAR_COL_WIDTH, 0, TAB_BAR_COL_WIDTH, TAB_BAR_HEIGHT),
            NK_WINDOW_NO_SCROLLBAR)
        self.menu = menu_window

    def update(self):

        def on_menu_click():
            self.menu.show() 

        self.layout_row_dynamic(TAB_BAR_HEIGHT-10, 1)
        self.button_label("Menu", on_menu_click)


class Menu(pf.Window):
    WINDOW_WIDTH = 300
    WINDOW_HEIGHT = 400
    menu_shown = False

    def __init__(self):
        resx, resy = pf.get_resolution()
        super(Menu, self).__init__("Menu", 
            (resx / 2 - Menu.WINDOW_WIDTH/ 2, resy / 2 - Menu.WINDOW_HEIGHT / 2, Menu.WINDOW_WIDTH, Menu.WINDOW_HEIGHT), 
            NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)

    def show(self):
        super(Menu, self).show()
        Menu.menu_shown = True

    def hide(self):
        super(Menu, self).hide()
        Menu.menu_shown = False

    def update(self):

        def on_cancel():
            pf.global_event(EVENT_MENU_CANCEL, None)

        def on_new():
            pf.global_event(EVENT_MENU_NEW, None)

        def on_load():
            pf.global_event(EVENT_MENU_LOAD, None) 

        def on_save():
            pf.global_event(EVENT_MENU_SAVE, None) 

        def on_save_as():
            pf.global_event(EVENT_MENU_SAVE_AS, None) 

        def on_exit():
            pf.global_event(EVENT_MENU_EXIT, None)

        self.layout_row_static(10, Menu.WINDOW_WIDTH, 1)
        self.layout_row_dynamic(TAB_BAR_HEIGHT-10, 1)
        self.button_label("Cancel", on_cancel)

        self.layout_row_static(10, Menu.WINDOW_WIDTH, 1)
        self.layout_row_dynamic(TAB_BAR_HEIGHT-10, 1)
        self.button_label("New", on_new)

        self.layout_row_static(10, Menu.WINDOW_WIDTH, 1)
        self.layout_row_dynamic(TAB_BAR_HEIGHT-10, 1)
        self.button_label("Load", on_load)

        self.layout_row_static(10, Menu.WINDOW_WIDTH, 1)
        self.layout_row_dynamic(TAB_BAR_HEIGHT-10, 1)
        self.button_label("Save", on_save)

        self.layout_row_static(10, Menu.WINDOW_WIDTH, 1)
        self.layout_row_dynamic(TAB_BAR_HEIGHT-10, 1)
        self.button_label("Save As", on_save_as)

        self.layout_row_static(10, Menu.WINDOW_WIDTH, 1)
        self.layout_row_dynamic(TAB_BAR_HEIGHT-10, 1)
        self.button_label("Exit", on_exit)


class FileChooser(pf.Window):
    WINDOW_WIDTH = 400 
    WINDOW_HEIGHT = 150

    def __init__(self):
        resx, resy = pf.get_resolution()
        super(FileChooser, self).__init__("FileChooser", 
            (resx / 2 - FileChooser.WINDOW_WIDTH/ 2, resy / 2 - FileChooser.WINDOW_HEIGHT / 2, 
            FileChooser.WINDOW_WIDTH, FileChooser.WINDOW_HEIGHT), NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)
        import os 
        self.filestring = os.path.realpath(pf.get_basedir()) + "/assets/maps/"

    def update(self):

        self.layout_row_static(15, FileChooser.WINDOW_WIDTH, 1)
        self.layout_row_dynamic(30, 1)
        self.filestring = self.edit_string(NK_EDIT_SIMPLE, self.filestring)
        self.layout_row_static(15, FileChooser.WINDOW_WIDTH, 1)

        def on_okay():
            pf.global_event(EVENT_FILE_CHOOSER_OKAY, self.filestring)

        def on_cancel():
            pf.global_event(EVENT_FILE_CHOOSER_CANCEL, None)

        self.layout_row_dynamic(30, 2)
        self.button_label("OK", on_okay)
        self.button_label("Cancel", on_cancel)


class ViewController(object):

    def activate(self): pass 
    def deactivate(self): pass 

