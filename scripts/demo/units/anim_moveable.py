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

from abc import ABCMeta, abstractproperty
import pf
from constants import *

class AnimMoveable(pf.AnimEntity):
    __metaclass__ = ABCMeta

    def __init__(self, path, pfobj, name, idle_anim):
        super(AnimMoveable, self).__init__(path, pfobj, name, idle_anim)
        self.moving = False
        self.register(EVENT_MOTION_START, AnimMoveable.__on_motion_begin, self)
        self.register(EVENT_MOTION_END, AnimMoveable.__on_motion_end, self)

    def __del__(self):
        self.unregister(EVENT_MOTION_START, AnimMoveable.__on_motion_begin)
        self.unregister(EVENT_MOTION_END, AnimMoveable.__on_motion_end)
        super(AnimMoveable, self).__del__()
    
    @abstractproperty
    def idle_anim(self): 
        """ Name of animation clip that should be played when not moving """
        pass

    @abstractproperty
    def move_anim(self): 
        """ Name of animation clip that should be played when moving """
        pass

    def __on_motion_begin(self, event):
        self.moving = True
        self.play_anim(self.move_anim())

    def __on_motion_end(self, event):
        self.moving = False
        self.play_anim(self.idle_anim())

