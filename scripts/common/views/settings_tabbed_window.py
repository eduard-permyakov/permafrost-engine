#
#  This file is part of Permafrost Engine. 
#  Copyright (C) 2019 Eduard Permyakov 
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
from common.constants import *

class SettingsTabbedWindow(pf.Window):

    WIDTH = 500
    TAB_COL_WIDTH = 150
    HEIGHT = 600

    SELECTED_COLOR = (90, 90, 90, 255)
    SELECTED_HOVER_COLOR = (75, 75, 75, 255)

    def __init__(self):
        resx, resy = pf.get_resolution()

        dims = ((resx - SettingsTabbedWindow.WIDTH)/2, (resy - SettingsTabbedWindow.HEIGHT)/2, 
            SettingsTabbedWindow.WIDTH, SettingsTabbedWindow.HEIGHT)
        super(SettingsTabbedWindow, self).__init__("Settings", dims, 
            pf.NK_WINDOW_NO_SCROLLBAR | pf.NK_WINDOW_MOVABLE | pf.NK_WINDOW_BORDER | pf.NK_WINDOW_TITLE | pf.NK_WINDOW_CLOSABLE)
        self.padding = (0.0, 0.0)

        self.active_idx = 0
        self.labels = []
        self.child_windows = []

    def push_child(self, label, window):
        assert isinstance(label, basestring)
        assert isinstance(window, pf.Window)

        self.labels.append(label) 
        self.child_windows.append(window)

    def on_hide(self):
        pf.global_event(EVENT_SETTINGS_HIDE, None)

    def update(self):

        def settings_tab_group():
            orig_rounding = pf.button_style.rounding
            pf.button_style.rounding = 0.0

            for idx in range(0, len(self.child_windows)):

                def on_tab_click():
                    self.active_idx = idx;
                    pf.global_event(EVENT_SETTINGS_TAB_SEL_CHANGED, self.active_idx)

                self.layout_row_dynamic(40, 1)
                if idx == self.active_idx:
                    normal_style = pf.button_style.normal
                    hover_style = pf.button_style.hover

                    pf.button_style.normal = SettingsTabbedWindow.SELECTED_COLOR
                    pf.button_style.hover = SettingsTabbedWindow.SELECTED_HOVER_COLOR

                    self.button_label(self.labels[idx], on_tab_click)

                    pf.button_style.normal = normal_style
                    pf.button_style.hover = hover_style
                else:
                    self.button_label(self.labels[idx], on_tab_click)

            pf.button_style.rounding = orig_rounding

        BOT_PAD = 10
        self.layout_row_begin(pf.NK_STATIC, SettingsTabbedWindow.HEIGHT - self.header_height - \
            int(50 + 2 * self.group_padding[1]) - int(2 * self.spacing[1]) - BOT_PAD, 2)

        self.layout_row_push(SettingsTabbedWindow.TAB_COL_WIDTH)
        self.group("SettingsTabCol", pf.NK_WINDOW_NO_SCROLLBAR, settings_tab_group)

        self.layout_row_push(SettingsTabbedWindow.WIDTH - SettingsTabbedWindow.TAB_COL_WIDTH - int(2 * self.border + self.spacing[0]))
        self.group("SettingsSubWindow", pf.NK_WINDOW_NO_SCROLLBAR, self.child_windows[self.active_idx].update)
        self.layout_row_end()

        def on_exit():
            pf.global_event(EVENT_SETTINGS_HIDE, None)

        def on_apply():
            pf.global_event(EVENT_SETTINGS_APPLY, None)

        self.layout_row_dynamic(50 + int(2 * self.group_padding[1]), 1)

        def apply_done_group():
            self.layout_row_dynamic(20, 1)
            if self.child_windows[self.active_idx].dirty:
                self.label_colored_wrap("You have unsaved changes on this page.", (255, 0, 0))
            else:
                self.label_colored_wrap("No unsaved changes.", (0, 255, 0))
            self.layout_row_dynamic(30, 2)
            self.button_label("Apply", on_apply)
            self.button_label("Done", on_exit)

        self.group("SettingsApplyDone", pf.NK_WINDOW_NO_SCROLLBAR, apply_done_group)

