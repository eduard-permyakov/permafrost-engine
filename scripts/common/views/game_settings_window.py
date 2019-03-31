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

class GameSettingsWindow(pf.Window):
    
    WIDTH = 300
    HEIGHT = 400

    def __init__(self):
        resx, resy = pf.get_resolution()
        super(GameSettingsWindow, self).__init__("GameSettings", ((resx - GameSettingsWindow.WIDTH)/2, 
            (resy - GameSettingsWindow.HEIGHT)/2, GameSettingsWindow.WIDTH, GameSettingsWindow.HEIGHT), 0)
        self.hb_idx = 0
        self.dirty = False

    def update(self):
        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Health Bars:", (255, 255, 255))
        
        self.layout_row_dynamic(20, 2)
        old_hb_idx = self.hb_idx
        if self.option_label("On", self.hb_idx == 0):
            self.hb_idx = 0
        if self.option_label("Off", self.hb_idx == 1):
            self.hb_idx = 1
        
        if self.hb_idx != old_hb_idx:
            pf.global_event(EVENT_SETTINGS_HB_MODE_CHANGED, self.hb_idx)
         
