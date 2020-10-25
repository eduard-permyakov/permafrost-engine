#
#  This file is part of Permafrost Engine. 
#  Copyright (C) 2018-2020 Eduard Permyakov 
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

from abc import ABCMeta, abstractproperty
import pf
from constants import *
import weakref
import controllable as cont
import action

class AnimMoveable(pf.AnimEntity, pf.MovableEntity, cont.Controllable):
    """ 
    Mixin base that extends animated entities with behaviours for playing specific
    animations on movement, as well as adds 'move' and 'stop' actions.
    """
    __metaclass__ = ABCMeta

    def __init__(self, path, pfobj, name, **kwargs):
        super(AnimMoveable, self).__init__(path, pfobj, name, **kwargs)
        self.moving = False
        self.register(pf.EVENT_MOTION_START, AnimMoveable.__on_motion_begin, weakref.ref(self))
        self.register(pf.EVENT_MOTION_END, AnimMoveable.__on_motion_end, weakref.ref(self))

    def __del__(self):
        self.unregister(pf.EVENT_MOTION_START, AnimMoveable.__on_motion_begin)
        self.unregister(pf.EVENT_MOTION_END, AnimMoveable.__on_motion_end)
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
        assert not self.moving
        self.moving = True
        self.play_anim(self.move_anim())

    def __on_motion_end(self, event):
        assert self.moving
        self.moving = False
        self.play_anim(self.idle_anim())

    def action(self, idx):
        if idx == 0:
            return action.ActionDesc(
                icon_normal="assets/icons/glest/magic-actions/magic_move_normal.bmp",
                icon_hover="assets/icons/glest/magic-actions/magic_move_hover.bmp",
                icon_active="assets/icons/glest/magic-actions/magic_move_active.bmp",
                action = AnimMoveable.__move_action,
                hotkey = pf.SDL_SCANCODE_M)
        if idx == 1 and super(AnimMoveable, self).action(1) is None:
            return action.ActionDesc(
                icon_normal="assets/icons/glest/magic-actions/magic_stop_normal.bmp",
                icon_hover="assets/icons/glest/magic-actions/magic_stop_hover.bmp",
                icon_active="assets/icons/glest/magic-actions/magic_stop_active.bmp",
                action = AnimMoveable.__stop_action,
                hotkey = pf.SDL_SCANCODE_S)
        return super(AnimMoveable, self).action(idx)

    @classmethod
    def __move_action(cls):
        pf.set_move_on_left_click()

    @classmethod
    def __stop_action(cls):
        for ent in pf.get_unit_selection():
            ent.stop()

