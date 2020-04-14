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

import pf
from constants import *
import common.view_controllers.view_controller as vc
import units.controllable as cont
import weakref

class ActionPadVC(vc.ViewController):

    def __init__(self, view):
        self.__view = view
        self.__hotkey_action_map = {}
        self.__active_controllable = None

    def __install_hotkeys(self, controllable):
        for i in range(0, ACTION_NUM_ROWS * ACTION_NUM_COLS):
            act = controllable.action(i)
            if act and act.hotkey:
                self.__hotkey_action_map[act.hotkey] = act.action
    
    def __uninstall_hotkeys(self, controllable):
        for i in range(0, ACTION_NUM_ROWS * ACTION_NUM_COLS):
            act = controllable.action(i)
            if act and act.hotkey:
                del self.__hotkey_action_map[act.hotkey]

    def __on_selection_changed(self, event):
        self.__view.clear_actions()
        if self.__active_controllable and self.__active_controllable():
            self.__uninstall_hotkeys(self.__active_controllable())
            self.__active_controllable = None

        sel = pf.get_unit_selection()
        controllable_sel = [ent for ent in sel if isinstance(ent, cont.Controllable)]

        if len(controllable_sel) > 0:
            first = controllable_sel[0]
            fac_list = pf.get_factions_list()
            if fac_list[first.faction_id]["controllable"]:
                self.__active_controllable = weakref.ref(first)
                self.__install_hotkeys(first)
                self.__view.actions = [first.action(i) for i in range(0, ACTION_NUM_ROWS * ACTION_NUM_COLS)]

    def __on_keydown(self, event):
        scancode = event[0]
        if scancode in self.__hotkey_action_map and not pf.ui_text_edit_has_focus():
            self.__hotkey_action_map[scancode]()

    def activate(self):
        # Don't use 'register_ui_event_handler' as we want the action pad to be disabled/frozen when paused
        pf.register_event_handler(pf.EVENT_UNIT_SELECTION_CHANGED, ActionPadVC.__on_selection_changed, self)
        pf.register_event_handler(pf.SDL_KEYDOWN, ActionPadVC.__on_keydown, self)
        self.__view.show()

    def deactivate(self):
        self.__view.hide()
        pf.unregister_event_handler(pf.SDL_KEYDOWN, ActionPadVC.__on_keydown)
        pf.unregister_event_handler(pf.EVENT_UNIT_SELECTION_CHANGED, ActionPadVC.__on_selection_changed)

