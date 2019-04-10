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
#  Linking this software statically or dynamically with other modules is making 
#  a combined work based on this software. Thus, the terms and conditions of 
#  the GNU General Public License cover the whole combination. 
#  
#  As a special exception, the copyright holders of Permafrost Engine give 
#  you permission to link Permafrost Engine with independent modules to produce 
#  an executable, regardless of the license terms of these independent 
#  modules, and to copy and distribute the resulting executable under 
#  terms of your choice, provided that you also meet, for each linked 
#  independent module, the terms and conditions of the license of that 
#  module. An independent module is a module which is not derived from 
#  or based on Permafrost Engine. If you modify Permafrost Engine, you may 
#  extend this exception to your version of Permafrost Engine, but you are not 
#  obliged to do so. If you do not wish to do so, delete this exception 
#  statement from your version.
#

import pf
from constants import *

class MenuButtonWindow(pf.Window):
    
    def __init__(self, menu_window):
        vresx, vresy = (1920, 1080)
        super(MenuButtonWindow, self).__init__("MenuButton", 
            (vresx - UI_TAB_BAR_COL_WIDTH, 0, UI_TAB_BAR_COL_WIDTH, UI_TAB_BAR_HEIGHT, (vresx, vresy)),
            pf.NK_WINDOW_NO_SCROLLBAR)
        self.menu = menu_window

    def update(self):

        def on_menu_click():
            self.menu.show() 

        self.layout_row_dynamic(UI_TAB_BAR_HEIGHT-10, 1)
        self.button_label("Menu", on_menu_click)


class Menu(pf.Window):
    WINDOW_WIDTH = 300
    WINDOW_HEIGHT = 405
    menu_shown = False

    def __init__(self):
        resx, resy = pf.get_resolution()
        super(Menu, self).__init__("Menu", 
            (resx / 2 - Menu.WINDOW_WIDTH/ 2, resy / 2 - Menu.WINDOW_HEIGHT / 2, Menu.WINDOW_WIDTH, Menu.WINDOW_HEIGHT), 
            pf.NK_WINDOW_BORDER | pf.NK_WINDOW_NO_SCROLLBAR)

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

        def on_settings():
            pf.global_event(EVENT_MENU_SETTINGS_SHOW, None) 

        def on_performance():
            pf.global_event(EVENT_MENU_PERF_SHOW, None)

        def on_exit():
            pf.global_event(EVENT_MENU_EXIT, None)

        self.layout_row_static(10, Menu.WINDOW_WIDTH, 1)
        self.layout_row_dynamic(UI_TAB_BAR_HEIGHT-10, 1)
        self.button_label("Cancel", on_cancel)

        self.layout_row_static(10, Menu.WINDOW_WIDTH, 1)
        self.layout_row_dynamic(UI_TAB_BAR_HEIGHT-10, 1)
        self.button_label("New", on_new)

        self.layout_row_static(10, Menu.WINDOW_WIDTH, 1)
        self.layout_row_dynamic(UI_TAB_BAR_HEIGHT-10, 1)
        self.button_label("Load", on_load)

        self.layout_row_static(10, Menu.WINDOW_WIDTH, 1)
        self.layout_row_dynamic(UI_TAB_BAR_HEIGHT-10, 1)
        self.button_label("Save", on_save)

        self.layout_row_static(10, Menu.WINDOW_WIDTH, 1)
        self.layout_row_dynamic(UI_TAB_BAR_HEIGHT-10, 1)
        self.button_label("Save As", on_save_as)

        self.layout_row_static(10, Menu.WINDOW_WIDTH, 1)
        self.layout_row_dynamic(UI_TAB_BAR_HEIGHT-10, 1)
        self.button_label("Settings", on_settings)

        self.layout_row_static(10, Menu.WINDOW_WIDTH, 1)
        self.layout_row_dynamic(UI_TAB_BAR_HEIGHT-10, 1)
        self.button_label("Performance", on_performance)

        self.layout_row_static(10, Menu.WINDOW_WIDTH, 1)
        self.layout_row_dynamic(UI_TAB_BAR_HEIGHT-10, 1)
        self.button_label("Exit", on_exit)

