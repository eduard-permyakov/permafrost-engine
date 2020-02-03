#
#  This file is part of Permafrost Engine. 
#  Copyright (C) 2019-2020 Eduard Permyakov 
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

class VideoSettingsWindow(pf.Window):
    
    WIDTH = 300
    HEIGHT = 400

    def __init__(self):
        vresx, vresy = (1920, 1080)
        super(VideoSettingsWindow, self).__init__("VideoSettings", ((vresx - VideoSettingsWindow.WIDTH)/2, 
            (vresy - VideoSettingsWindow.HEIGHT)/2, VideoSettingsWindow.WIDTH, VideoSettingsWindow.HEIGHT), 0, (vresx, vresy))

        self.ar_idx = 0
        self.ar_opts = [
            (4, 3),
            (16, 9),
            (21, 9)
        ]
        self.__ar_opt_strings = ["{}:{}".format(*opt) for opt in self.ar_opts]

        nx, ny = pf.get_native_resolution()
        self.res_idx = 0
        self.res_opts = [ 
            (nx, ny), 
            (nx * 3/4, ny * 3/4),
            (nx / 2, ny / 2),
        ]
        self.res_opt_strings = ["{}:{}".format(int(opt[0]), int(opt[1])) for opt in self.res_opts]

        self.mode_idx = 0
        self.mode_opts = [
            pf.PF_WF_FULLSCREEN,
            pf.PF_WF_WINDOW,
            pf.PF_WF_BORDERLESS_WIN,
        ]
        self.mode_opt_strings = [
            "Fullscreen",
            "Window",
            "Borderless Window",
        ]

        self.win_on_top_idx = 0
        self.win_on_top_opts = [
            True,
            False
        ]
        self.__win_on_top_opt_strings = ["On", "Off"]

        self.vsync_idx = 0
        self.vsync_opts = [
            True,
            False
        ]
        self.__vsync_opt_strings = ["On", "Off"]

        self.shadows_idx = 0
        self.shadows_opts = [
            True,
            False
        ]
        self.__shadows_opt_strings = ["On", "Off"]

        self.water_reflect_idx = 0
        self.water_reflect_opts = [
            True,
            False
        ]
        self.__water_reflect_opt_strings = ["On", "Off"]

        self.dirty = False

    def update(self):

        # AR 
        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Aspect Ratio:", (255, 255, 255))

        self.layout_row_dynamic(25, 1)
        old_ar_idx = self.ar_idx
        self.ar_idx = self.combo_box(self.__ar_opt_strings, self.ar_idx, 25, (VideoSettingsWindow.WIDTH - 40, 200))
        if old_ar_idx != self.ar_idx:
            pf.global_event(EVENT_AR_SETTING_CHANGED, self.ar_opts[self.ar_idx])

        # Resolution 
        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Resolution:", (255, 255, 255))

        self.layout_row_dynamic(25, 1)
        old_res_idx = self.res_idx
        self.res_idx = self.combo_box(self.res_opt_strings, self.res_idx, 25, (VideoSettingsWindow.WIDTH - 40, 200))
        if old_res_idx != self.res_idx:
            pf.global_event(EVENT_RES_SETTING_CHANGED, self.res_opts[self.res_idx])

        # Window Mode
        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Window Mode:", (255, 255, 255))

        self.layout_row_dynamic(25, 1)
        old_mode_idx = self.mode_idx
        self.mode_idx = self.combo_box(self.mode_opt_strings, self.mode_idx, 25, (VideoSettingsWindow.WIDTH - 40, 200))
        if old_mode_idx != self.mode_idx:
            pf.global_event(EVENT_WINMODE_SETTING_CHANGED, self.mode_opts[self.mode_idx])

        # Window Always On Top
        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Window Always On Top (Requires Restart):", (255, 255, 255))

        self.layout_row_dynamic(20, 2)
        old_win_on_top_idx = self.win_on_top_idx
        if self.option_label(self.__win_on_top_opt_strings[0], self.win_on_top_idx == 0):
            self.win_on_top_idx = 0
        if self.option_label(self.__win_on_top_opt_strings[1], self.win_on_top_idx == 1):
            self.win_on_top_idx = 1
        
        if self.win_on_top_idx != old_win_on_top_idx:
            pf.global_event(EVENT_WIN_TOP_SETTING_CHANGED, self.win_on_top_opts[self.win_on_top_idx])

        # Vsync
        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Vsync:", (255, 255, 255))

        self.layout_row_dynamic(20, 2)
        old_vsync_idx = self.vsync_idx
        if self.option_label(self.__vsync_opt_strings[0], self.vsync_idx == 0):
            self.vsync_idx = 0
        if self.option_label(self.__vsync_opt_strings[1], self.vsync_idx == 1):
            self.vsync_idx = 1
        
        if self.vsync_idx != old_vsync_idx:
            pf.global_event(EVENT_VSYNC_SETTING_CHANGED, self.vsync_opts[self.vsync_idx])

        # Shadows
        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Shadows:", (255, 255, 255))

        self.layout_row_dynamic(20, 2)
        old_shadows_idx = self.shadows_idx
        if self.option_label(self.__shadows_opt_strings[0], self.shadows_idx == 0):
            self.shadows_idx = 0
        if self.option_label(self.__shadows_opt_strings[1], self.shadows_idx == 1):
            self.shadows_idx = 1
        
        if self.shadows_idx != old_shadows_idx:
            pf.global_event(EVENT_SHADOWS_SETTING_CHANGED, self.shadows_opts[self.shadows_idx])

        # Water Reflections
        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Water Reflections:", (255, 255, 255))

        self.layout_row_dynamic(20, 2)
        old_water_reflect_idx = self.water_reflect_idx
        if self.option_label(self.__water_reflect_opt_strings[0], self.water_reflect_idx == 0):
            self.water_reflect_idx = 0
        if self.option_label(self.__water_reflect_opt_strings[1], self.water_reflect_idx == 1):
            self.water_reflect_idx = 1
        
        if self.water_reflect_idx != old_water_reflect_idx:
            pf.global_event(EVENT_WATER_REF_SETTING_CHANGED, self.water_reflect_opts[self.water_reflect_idx])

