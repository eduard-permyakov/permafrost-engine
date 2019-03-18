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
from constants import *

class VideoSettingsWindow(pf.Window):
    
    WIDTH = 300
    HEIGHT = 400

    def __init__(self):
        resx, resy = pf.get_resolution()
        super(VideoSettingsWindow, self).__init__("VideoSettings", ((resx - VideoSettingsWindow.WIDTH)/2, 
            (resy - VideoSettingsWindow.HEIGHT)/2, VideoSettingsWindow.WIDTH, VideoSettingsWindow.HEIGHT), 0)

        self.res_idx = 0
        self.res_opts = [ 
            (1920.0, 1080.0), 
            (1280.0, 720.0),
            (960.0, 540.0),
            (640.0, 360.0) 
        ]
        self.res_opt_strings = ["{0}:{1}".format(int(opt[0]), int(opt[1])) for opt in self.res_opts]

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

        self.dirty = False

    def update(self):

        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Resolution:", (255, 255, 255))

        self.layout_row_dynamic(25, 1)
        old_res_idx = self.res_idx
        self.res_idx = self.combo_box(self.res_opt_strings, self.res_idx, 25, (VideoSettingsWindow.WIDTH - 40, 200))
        if old_res_idx != self.res_idx:
            pf.global_event(EVENT_RES_SETTING_CHANGED, self.res_opts[self.res_idx])

        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Window Mode:", (255, 255, 255))

        self.layout_row_dynamic(25, 1)
        old_mode_idx = self.mode_idx
        self.mode_idx = self.combo_box(self.mode_opt_strings, self.mode_idx, 25, (VideoSettingsWindow.WIDTH - 40, 200))
        if old_mode_idx != self.mode_idx:
            pf.global_event(EVENT_WINMODE_SETTING_CHANGED, self.mode_opts[self.mode_idx])

