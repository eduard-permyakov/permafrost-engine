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
import view_controller as vc
import globals
import faction

class DiplomacyVC(vc.ViewController):

    def __init__(self, view):
        self.view = view

    def __on_fac_sel_changed(self, event):
        self.view.fac_name = globals.factions_list[event].name
        self.view.fac_color = globals.factions_list[event].color

    def __on_fac_removed(self, event):
        # TODO: delete all entities belonging to this faction
        del globals.factions_list[event]
        if event == len(globals.factions_list): # we removed the last element
            self.view.selected_fac_idx = len(globals.factions_list)-1
        self.view.fac_name = globals.factions_list[self.view.selected_fac_idx].name
        self.view.fac_color = globals.factions_list[self.view.selected_fac_idx].color

    def __on_fac_changed(self, event):
        idx = event[0]
        new_name = event[1]
        new_clr = event[2]
        globals.factions_list[idx].name = new_name
        globals.factions_list[idx].color = new_clr

    def __on_fac_new(self, event):
        new_name = event[0]
        new_clr = event[1]
        globals.factions_list.append(faction.Faction(new_name, new_clr))
        self.view.selected_fac_idx = len(globals.factions_list)-1

    def activate(self):
        pf.register_event_handler(EVENT_DIPLO_FAC_SELECTION_CHANGED, DiplomacyVC.__on_fac_sel_changed, self)
        pf.register_event_handler(EVENT_DIPLO_FAC_REMOVED, DiplomacyVC.__on_fac_removed, self)
        pf.register_event_handler(EVENT_DIPLO_FAC_CHANGED, DiplomacyVC.__on_fac_changed, self)
        pf.register_event_handler(EVENT_DIPLO_FAC_NEW, DiplomacyVC.__on_fac_new, self)

    def deactivate(self):
        pf.unregister_event_handler(EVENT_DIPLO_FAC_SELECTION_CHANGED, DiplomacyVC.__on_fac_sel_changed)
        pf.unregister_event_handler(EVENT_DIPLO_FAC_REMOVED, DiplomacyVC.__on_fac_removed)
        pf.unregister_event_handler(EVENT_DIPLO_FAC_CHANGED, DiplomacyVC.__on_fac_changed)
        pf.unregister_event_handler(EVENT_DIPLO_FAC_NEW, DiplomacyVC.__on_fac_new)

