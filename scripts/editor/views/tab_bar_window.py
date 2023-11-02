#
#  This file is part of Permafrost Engine. 
#  Copyright (C) 2018-2023 Eduard Permyakov 
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

import common.button_style_ctx as btc

class TabBarWindow(pf.Window):
    SELECTED_COLOR = (90, 90, 90, 255)
    SELECTED_HOVER_COLOR = (75, 75, 75, 255)

    def __init__(self):
        vresx, vresy = (1920, 1080)

        dims = (0, 0, vresx - UI_TAB_BAR_COL_WIDTH, UI_TAB_BAR_HEIGHT)
        super(TabBarWindow, self).__init__("TabBar", dims, pf.NK_WINDOW_NO_SCROLLBAR, (vresx, vresy),
            resize_mask = pf.ANCHOR_X_LEFT | pf.ANCHOR_X_RIGHT | pf.ANCHOR_Y_TOP)
        self.active_idx = 0
        self.labels = []
        self.child_windows = []

    def push_child(self, label, window):
        assert isinstance(label, basestring)
        assert isinstance(window, pf.Window)

        self.labels.append(label) 
        self.child_windows.append(window)

    def update(self):

        with btc.ButtonStyle(rounding=0.0):
            self.layout_row_begin(pf.NK_STATIC, UI_TAB_BAR_HEIGHT-10, UI_TAB_BAR_NUM_COLS)

            for idx in range(0, len(self.child_windows)):

                def on_tab_click():
                    self.active_idx = idx;
                    self.__show_active()
                    pf.global_event(EVENT_TOP_TAB_SELECTION_CHANGED, self.active_idx)

                self.layout_row_push(UI_TAB_BAR_COL_WIDTH)

                if idx == self.active_idx:
                    with btc.ButtonStyle(normal=TabBarWindow.SELECTED_COLOR, hover=TabBarWindow.SELECTED_HOVER_COLOR):
                        self.button_label(self.labels[idx], on_tab_click)
                else:
                    self.button_label(self.labels[idx], on_tab_click)

            self.layout_row_end()

    def show(self):
        super(TabBarWindow, self).show()
        self.__show_active()

    def __show_active(self):
        for idx, window in enumerate(self.child_windows):
            if idx == self.active_idx:
                window.show()
            else:
                window.hide()

