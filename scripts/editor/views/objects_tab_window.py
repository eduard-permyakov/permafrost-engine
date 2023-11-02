#
#  This file is part of Permafrost Engine. 
#  Copyright (C) 2018-2023 Eduard Permyakov 
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
import globals

def format_str_for_numlist(numlist):
    ret = "["
    for i, num in enumerate(numlist):
        ret += "{0:.1f}".format(num)
        if i != len(numlist)-1:
            ret += ", "
    ret += "]"
    return ret

class ObjectsTabWindow(pf.Window):

    OBJECTS_MODE_PLACE  = 0
    OBJECTS_MODE_SELECT = 1

    def __init__(self):
        vresx, vresy = (1920, 1080)
        super(ObjectsTabWindow, self).__init__("ObjectsTab", 
            (0, UI_TAB_BAR_HEIGHT + 1, UI_LEFT_PANE_WIDTH, vresy - UI_TAB_BAR_HEIGHT - 1), pf.NK_WINDOW_BORDER, (vresx, vresy),
            resize_mask = pf.ANCHOR_X_LEFT | pf.ANCHOR_Y_TOP | pf.ANCHOR_Y_BOT)
        self.mode = self.OBJECTS_MODE_PLACE
        self.objects_list = []
        self.selected_object_idx = 0
        self.selected_faction_idx = 0

    def update(self):

        # Mode
        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Mode:", (255, 255, 255))

        old_mode = self.mode
        self.layout_row_dynamic(20, 2)
        if self.option_label("Place", self.mode == self.OBJECTS_MODE_PLACE):
            self.mode = self.OBJECTS_MODE_PLACE
        if self.option_label("Select", self.mode == self.OBJECTS_MODE_SELECT):
            self.mode = self.OBJECTS_MODE_SELECT
        self.layout_row_dynamic(10, 1)

        if self.mode != old_mode:
            pf.global_event(EVENT_OBJECTS_TAB_MODE_CHANGED, self.mode)

        if self.mode == self.OBJECTS_MODE_PLACE:
            # Faction
            self.layout_row_dynamic(20, 1)
            self.label_colored_wrap("Faction:", (255, 255, 255))
            self.layout_row_dynamic(25, 1)
            fac_names = [fac["name"] for fac in pf.get_factions_list()]
            self.selected_faction_idx = self.combo_box(fac_names, self.selected_faction_idx, 25, (UI_LEFT_PANE_WIDTH-30, 200))
            self.layout_row_dynamic(10, 1)
            
            # Objects
            self.layout_row_dynamic(20, 1)
            self.label_colored_wrap("Objects:", (255, 255, 255))

            def objects_group():
                self.layout_row_static(25, UI_LEFT_PANE_WIDTH-60, 1)
                for i in range(0, len(self.objects_list)):
                    old = self.selected_object_idx
                    on = self.selectable_label(self.objects_list[i], 
                        pf.NK_TEXT_ALIGN_LEFT, i == self.selected_object_idx)
                    if on: 
                        self.selected_object_idx = i
                    if self.selected_object_idx != old:
                        pf.global_event(EVENT_OBJECT_SELECTION_CHANGED, i)

            self.layout_row_static(400, UI_LEFT_PANE_WIDTH-30, 1)
            self.group("Objects", pf.NK_WINDOW_BORDER, objects_group)
        elif self.mode == self.OBJECTS_MODE_SELECT:
            # Selection
            sel_obj_list = pf.get_unit_selection()

            if len(sel_obj_list) == 0:
                return

            self.layout_row_dynamic(20, 1)
            self.label_colored_wrap("Selection:", (255, 255, 255))

            if len(sel_obj_list) > 1:
                def selection_group():
                    self.layout_row_static(25, UI_LEFT_PANE_WIDTH-60, 1)
                    for i in range(0, len(sel_obj_list)):
                        name = "{0} {1}".format(sel_obj_list[i].name, format_str_for_numlist(sel_obj_list[i].pos))
                        on = self.selectable_label(name, pf.NK_TEXT_ALIGN_LEFT, False)
                        if on:
                            pf.global_event(EVENT_OBJECT_SELECTED_UNIT_PICKED, sel_obj_list[i])
                self.layout_row_static(400, UI_LEFT_PANE_WIDTH-30, 1)
                self.group("Selection", pf.NK_WINDOW_BORDER, selection_group)
            else:
                assert(len(sel_obj_list) == 1)
                self.layout_row_dynamic(10, 1)
                self.layout_row_dynamic(20, 1)
                self.label_colored_wrap(sel_obj_list[0].name, (200, 200, 0))

                pos_str = "Position: {0}".format(format_str_for_numlist(sel_obj_list[0].pos))
                self.layout_row_dynamic(20, 1)
                self.label_colored_wrap(pos_str, (255, 255, 255))

                rot_str = "Rotation: {0}".format(format_str_for_numlist(sel_obj_list[0].rotation))
                self.layout_row_dynamic(20, 1)
                self.label_colored_wrap(rot_str, (255, 255, 255))

                scale_str = "Scale: {0}".format(format_str_for_numlist(sel_obj_list[0].scale))
                self.layout_row_dynamic(20, 1)
                self.label_colored_wrap(scale_str, (255, 255, 255))

                select_str = "Selectable: {0}".format("True" if sel_obj_list[0].selectable else "False")
                self.layout_row_dynamic(20, 1)
                self.label_colored_wrap(select_str, (255, 255, 255))

                fac = next(f for f in pf.get_factions_list() if f["id"] == sel_obj_list[0].faction_id)
                select_str = "Faction: {0}".format(fac["name"])
                self.layout_row_dynamic(20, 1)
                self.label_colored_wrap(select_str, (255, 255, 255))

            def on_delete():
                pf.global_event(EVENT_OBJECT_DELETE_SELECTION, None)

            self.layout_row_dynamic(30, 1)
            self.button_label("Delete", on_delete)

