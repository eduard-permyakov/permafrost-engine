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
import globals 
import weakref
import action
import controllable as cont

class AnimCombatable(pf.AnimEntity, pf.CombatableEntity, cont.Controllable):
    """ 
    Mixin base that extends animated and combatable entities with behaviours for 
    playing specific animations on attack and death, as well as adds 'hold positon'
    and 'attack' actions.
        
    """
    __metaclass__ = ABCMeta

    def __init__(self, path, pfobj, name, **kwargs):
        super(AnimCombatable, self).__init__(path, pfobj, name, **kwargs)
        self.attacking = False
        self.register(pf.EVENT_ATTACK_START, AnimCombatable.on_attack_begin, weakref.ref(self))
        self.register(pf.EVENT_ATTACK_END, AnimCombatable.on_attack_end, weakref.ref(self))
        self.register(pf.EVENT_ENTITY_DEATH, AnimCombatable.on_death, weakref.ref(self))

    def __del__(self):
        self.unregister(pf.EVENT_ENTITY_DEATH, AnimCombatable.on_death)
        self.unregister(pf.EVENT_ATTACK_END, AnimCombatable.on_attack_end)
        self.unregister(pf.EVENT_ATTACK_START, AnimCombatable.on_attack_begin)
        super(AnimCombatable, self).__del__()

    @abstractproperty
    def attack_anim(self): 
        """ Name of animation clip that should be played when attacking """
        pass

    @abstractproperty
    def death_anim(self): 
        """ Name of animation clip that should be played on death """
        pass

    def on_attack_begin(self, event):
        assert not self.attacking
        self.attacking = True
        self.play_anim(self.attack_anim())

    def on_attack_end(self, event):
        assert self.attacking
        self.attacking = False
        self.play_anim(self.idle_anim())

    def on_death(self, event):
        self.play_anim(self.death_anim(), mode=pf.ANIM_MODE_ONCE)
        # retain this entity until the death event 
        self.register(pf.EVENT_ANIM_CYCLE_FINISHED, AnimCombatable.on_death_anim_finish, self)

    def on_death_anim_finish(self, event):
        self.unregister(pf.EVENT_ANIM_CYCLE_FINISHED, AnimCombatable.on_death_anim_finish)
        globals.scene_objs.remove(self)

    def action(self, idx):
        if idx == 2:
            return action.ActionDesc(
                icon_normal="assets/icons/glest/magic-actions/magic_hold_normal.bmp",
                icon_hover="assets/icons/glest/magic-actions/magic_hold_hover.bmp",
                icon_active="assets/icons/glest/magic-actions/magic_hold_active.bmp",
                action = AnimCombatable.__hold_position_action,
                hotkey = pf.SDL_SCANCODE_H)
        if idx == 3:
            return action.ActionDesc(
                icon_normal="assets/icons/glest/magic-actions/magic_armor_attack_normal.bmp",
                icon_hover="assets/icons/glest/magic-actions/magic_armor_attack_hover.bmp",
                icon_active="assets/icons/glest/magic-actions/magic_armor_attack_active.bmp",
                action = AnimCombatable.__attack_action,
                hotkey = pf.SDL_SCANCODE_A)
        return super(AnimCombatable, self).action(idx)

    @classmethod
    def __attack_action(cls):
        pf.set_attack_on_left_click()

    @classmethod
    def __hold_position_action(cls):
        for ent in pf.get_unit_selection():
            if isinstance(ent, pf.CombatableEntity):
                ent.hold_position()

