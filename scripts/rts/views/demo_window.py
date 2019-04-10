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

class DemoWindow(pf.Window):

    WIDTH = 250
    HEIGHT = 325

    def __init__(self):
        super(DemoWindow, self).__init__("Permafrost Engine Demo", (25, 25, DemoWindow.WIDTH, DemoWindow.HEIGHT), 
            pf.NK_WINDOW_BORDER | pf.NK_WINDOW_MOVABLE | pf.NK_WINDOW_MINIMIZABLE | pf.NK_WINDOW_TITLE |  pf.NK_WINDOW_NO_SCROLLBAR, (1920, 1080))
        self.fac_names = []
        self.active_fac_idx = 0

    def update(self):

        def factions_group():
            self.layout_row_dynamic(25, 1)
            for i in range(0, len(self.fac_names)):
                old = self.active_fac_idx
                on = self.selectable_label(self.fac_names[i], 
                pf.NK_TEXT_ALIGN_LEFT, i == self.active_fac_idx)
                if on: 
                    self.active_fac_idx = i
                if self.active_fac_idx != old:
                    pf.global_event(EVENT_CONTROLLED_FACTION_CHANGED, i)

        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Controlled Faction:", (255, 255, 255))

        self.layout_row_dynamic(140, 1)
        self.group("Controlled Faction", pf.NK_WINDOW_BORDER, factions_group)

        self.layout_row_dynamic(5, 1)

        def on_exit():
            pf.global_event(pf.SDL_QUIT, None)

        def on_settings():
            pf.global_event(EVENT_SETTINGS_SHOW, None)

        def on_performance():
            pf.global_event(EVENT_PERF_SHOW, None)

        self.layout_row_dynamic(30, 1)
        self.button_label("Settings", on_settings)

        self.layout_row_dynamic(30, 1)
        self.button_label("Performance", on_performance)

        self.layout_row_dynamic(30, 1)
        self.button_label("Exit Demo", on_exit)

