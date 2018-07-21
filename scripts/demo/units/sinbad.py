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

import anim_moveable as am
from constants import *

class Sinbad(am.AnimMoveable):

    def __init__(self, path, pfobj, name):
        self.idle_idx = 0
        self.idle_map = ["Dance", "JumpLoop"]
        super(Sinbad, self).__init__(path, pfobj, name, self.idle_anim())
        self.register(EVENT_SINBAD_TOGGLE_ANIM, Sinbad.on_anim_toggle, self)
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

