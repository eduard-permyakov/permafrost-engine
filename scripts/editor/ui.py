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

TAB_BAR_HEIGHT = 40
TAB_BAR_NUM_COLS = 3
TAB_BAR_COL_WIDTH = 120
TAB_SELECTED_COLOR = (90, 90, 90, 255)
TAB_SELECTED_HOVER_COLOR = (75, 75, 75, 255)

LEFT_PANE_WIDTH = 250

class TabBarWindow(pf.Window):

    def __init__(self):
        resx, _ = pf.get_resolution()

        dims = (0, 0, resx - TAB_BAR_COL_WIDTH, TAB_BAR_HEIGHT)
        super(TabBarWindow, self).__init__("TabBar", dims, NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)
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

            self.layout_row_push(TAB_BAR_COL_WIDTH)

            if idx == self.active_idx:
                normal_style = pf.button_style.normal
                hover_style = pf.button_style.hover
                pf.button_style.normal = TAB_SELECTED_COLOR
                pf.button_style.hover = TAB_SELECTED_HOVER_COLOR
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

    def update(self):
        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Terrain", (255, 255, 255));


class ObjectsTabWindow(pf.Window):

    def __init__(self):
        _, resy = pf.get_resolution()
        super(ObjectsTabWindow, self).__init__("ObjectsTab", 
            (0, TAB_BAR_HEIGHT + 1, LEFT_PANE_WIDTH, resy - TAB_BAR_HEIGHT - 1), NK_WINDOW_BORDER)

    def update(self):
        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Objects", (255, 255, 255));


class MenuButtonWindow(pf.Window):
    
    def __init__(self, menu_window):
        resx, _ = pf.get_resolution()
        super(MenuButtonWindow, self).__init__("MenuButton", 
            (resx - TAB_BAR_COL_WIDTH + 1, 0, TAB_BAR_COL_WIDTH - 1, TAB_BAR_HEIGHT), 
            NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)
        self.menu = menu_window

    def update(self):

        def on_menu_click():
            self.menu.show() 

        self.layout_row_dynamic(TAB_BAR_HEIGHT-10, 1)
        self.button_label("Menu", on_menu_click)


MENU_WINDOW_WIDTH = 300
MENU_WINDOW_HEIGHT = 400

class Menu(pf.Window):

    def __init__(self):
        resx, resy = pf.get_resolution()
        super(Menu, self).__init__("Menu", 
            (resx / 2 - MENU_WINDOW_WIDTH / 2, resy / 2 - MENU_WINDOW_HEIGHT / 2, MENU_WINDOW_WIDTH, MENU_WINDOW_HEIGHT), 
            NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)

    def update(self):

        def on_exit():
            pf.global_event(EVENT_SDL_QUIT, None)

        def on_cancel():
            self.hide()

        self.layout_row_static(10, MENU_WINDOW_WIDTH, 1)
        self.layout_row_dynamic(TAB_BAR_HEIGHT-10, 1)
        self.button_label("Exit", on_exit)

        self.layout_row_static(10, MENU_WINDOW_WIDTH, 1)
        self.layout_row_dynamic(TAB_BAR_HEIGHT-10, 1)
        self.button_label("Cancel", on_cancel)

