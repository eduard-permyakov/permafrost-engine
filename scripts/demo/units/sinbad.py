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

import anim_moveable as am
from constants import *
import weakref

class Sinbad(am.AnimMoveable):

    def __init__(self, path, pfobj, name):
        self.idle_idx = 0
        self.idle_map = ["Dance", "JumpLoop"]
        super(Sinbad, self).__init__(path, pfobj, name, idle_clip=self.idle_anim())
        self.register(EVENT_SINBAD_TOGGLE_ANIM, Sinbad.on_anim_toggle, weakref.ref(self))
        self.speed = 20.0
    
    def __del__(self):
        self.unregister(EVENT_SINBAD_TOGGLE_ANIM, Sinbad.on_anim_toggle)
        super(Sinbad, self).__del__()
    
    def on_anim_toggle(self, event):
        self.idle_idx = (self.idle_idx + 1) % 2
        if not self.moving:
            self.play_anim(self.idle_map[self.idle_idx])

    def idle_anim(self):
        return self.idle_map[self.idle_idx]

    def move_anim(self):
        return "RunBase"

