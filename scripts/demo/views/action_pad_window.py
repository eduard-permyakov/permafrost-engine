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
import action

class ActionPadWindow(pf.Window):

    BUTTON_WIDTH = 75
    BUTTON_PADDING = 6
    DISABLED_BG_COLOR = (40, 40, 40, 255)
    DISABLED_TEXT_COLOR = (60, 60, 60, 255)

    def __init__(self):
        # Each button also has a 1px border around it, hence the (ACTION_NUM_COLS*2)
        width = ActionPadWindow.BUTTON_WIDTH * ACTION_NUM_COLS \
            + (ACTION_NUM_COLS-1) * ActionPadWindow.BUTTON_PADDING +(ACTION_NUM_COLS*2) + 4
        height = ActionPadWindow.BUTTON_WIDTH * ACTION_NUM_ROWS \
            + (ACTION_NUM_ROWS-1) * ActionPadWindow.BUTTON_PADDING +(ACTION_NUM_ROWS*2) + 6
        resx, resy = pf.get_resolution()
        super(ActionPadWindow, self).__init__("ActionPad", (resx - width - 10, resy - height - 10, 
            width, height), pf.NK_WINDOW_BORDER | pf.NK_WINDOW_NO_SCROLLBAR)
        self.spacing = (float(ActionPadWindow.BUTTON_PADDING), float(ActionPadWindow.BUTTON_PADDING))
        self.padding = (2.0, 4.0)
        self.clear_actions()

    def __disabled_button_label(self, label, action):
        old_style = (pf.button_style.normal, pf.button_style.hover, pf.button_style.active)
        old_style_text = (pf.button_style.text_normal, pf.button_style.text_hover, pf.button_style.text_active)
        
        pf.button_style.normal = ActionPadWindow.DISABLED_BG_COLOR
        pf.button_style.hover = ActionPadWindow.DISABLED_BG_COLOR
        pf.button_style.active = ActionPadWindow.DISABLED_BG_COLOR
        pf.button_style.text_normal = ActionPadWindow.DISABLED_TEXT_COLOR
        pf.button_style.text_hover = ActionPadWindow.DISABLED_TEXT_COLOR
        pf.button_style.text_active = ActionPadWindow.DISABLED_TEXT_COLOR
        
        self.button_label(label, action)
        
        pf.button_style.normal, pf.button_style.hover, pf.button_style.active = old_style
        pf.button_style.text_normal, pf.button_style.text_hover, pf.button_style.text_active = old_style_text

    def __image_button(self, img_normal, img_hover, img_active, action):
        old = pf.button_style.normal, pf.button_style.hover, pf.button_style.active

        pf.button_style.normal = img_normal
        pf.button_style.hover = img_hover
        pf.button_style.active = img_active
        self.button_label("", action)

        pf.button_style.normal, pf.button_style.hover, pf.button_style.active = old

    def clear_actions(self):
        self.actions = [None] * (ACTION_NUM_ROWS * ACTION_NUM_COLS)

    def update(self):
        og_bpad, og_bround = pf.button_style.padding, pf.button_style.rounding

        pf.button_style.padding = (0.0, 0.0)
        pf.button_style.rounding = 0.0

        def on_button_pressed(idx):
            pf.global_event(EVENT_UNIT_ACTION, idx)

        for r in range(0, ACTION_NUM_ROWS):
            self.layout_row_static(ActionPadWindow.BUTTON_WIDTH, ActionPadWindow.BUTTON_WIDTH, ACTION_NUM_COLS)

            for c in range(0, ACTION_NUM_COLS):
                action = self.actions[r * ACTION_NUM_COLS + c]
                if action:
                    self.__image_button(
                        img_normal = action.icon_normal, 
                        img_hover = action.icon_hover, 
                        img_active = action.icon_active,
                        action = action.action)
                else:
                    self.__disabled_button_label("", lambda: None)

        pf.button_style.padding, pf.button_style.rounding = og_bpad, og_bround

