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
import globals

class DiplomacyTabWindow(pf.Window):

    DISABLED_BG_COLOR = (40, 40, 40, 255)
    DISABLED_TEXT_COLOR = (60, 60, 60, 255)

    def __init__(self):
        vresx, vresy = (1920, 1080)
        super(DiplomacyTabWindow, self).__init__("DiplomacyTab", 
            (0, UI_TAB_BAR_HEIGHT + 1, UI_LEFT_PANE_WIDTH, vresy - UI_TAB_BAR_HEIGHT - 1), pf.NK_WINDOW_BORDER, (vresx, vresy),
            resize_mask = pf.ANCHOR_X_LEFT | pf.ANCHOR_Y_TOP | pf.ANCHOR_Y_BOT)
        self.selected_fac_idx = 0

        factions_list = pf.get_factions_list()
        assert len(factions_list) > 0
        self.fac_name = factions_list[self.selected_fac_idx]["name"]
        self.fac_color = factions_list[self.selected_fac_idx]["color"]

    def update(self):

        factions_list = pf.get_factions_list()

        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Factions:", (255, 255, 255))

        def factions_group():
            self.layout_row_static(25, UI_LEFT_PANE_WIDTH-60, 1)
            for i in range(0, len(factions_list)):
                old = self.selected_fac_idx
                on = self.selectable_label(factions_list[i]["name"], 
                    pf.NK_TEXT_ALIGN_LEFT, i == self.selected_fac_idx)
                if on: 
                    self.selected_fac_idx = i
                if self.selected_fac_idx != old:
                    pf.global_event(EVENT_DIPLO_FAC_SELECTION_CHANGED, i)

        self.layout_row_static(400, UI_LEFT_PANE_WIDTH-30, 1)
        self.group("Factions", pf.NK_WINDOW_BORDER, factions_group)

        def on_delete_selected():
            pf.global_event(EVENT_DIPLO_FAC_REMOVED, self.selected_fac_idx)

        self.layout_row_dynamic(30, 1)
        if len(factions_list) > 1:
            self.button_label("Delete Selected", on_delete_selected)
        else: #disabled - We cannot delete the very last faction
            old_style = (pf.button_style.normal, pf.button_style.hover, pf.button_style.active)
            old_style_text = (pf.button_style.text_normal, pf.button_style.text_hover, pf.button_style.text_active)

            pf.button_style.normal = DiplomacyTabWindow.DISABLED_BG_COLOR
            pf.button_style.hover = DiplomacyTabWindow.DISABLED_BG_COLOR
            pf.button_style.active = DiplomacyTabWindow.DISABLED_BG_COLOR
            pf.button_style.text_normal = DiplomacyTabWindow.DISABLED_TEXT_COLOR
            pf.button_style.text_hover = DiplomacyTabWindow.DISABLED_TEXT_COLOR
            pf.button_style.text_active = DiplomacyTabWindow.DISABLED_TEXT_COLOR

            self.button_label("Delete Selected", lambda : None)

            pf.button_style.normal, pf.button_style.hover, pf.button_style.active = old_style
            pf.button_style.text_normal, pf.button_style.text_hover, pf.button_style.text_active = old_style_text

        self.layout_row_dynamic(10, 1)

        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Faction Name:", (255, 255, 255))
        self.layout_row_dynamic(30, 1)
        self.fac_name = self.edit_string(pf.NK_EDIT_SIMPLE, self.fac_name)

        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Faction Color:", (255, 255, 255))
        self.layout_row_dynamic(30, 1)
        self.fac_color = self.color_picker(self.fac_color, (UI_LEFT_PANE_WIDTH-30, 120))

        self.layout_row_dynamic(10, 1)

        def on_update_fac():
            pf.global_event(EVENT_DIPLO_FAC_CHANGED, (self.selected_fac_idx, self.fac_name, self.fac_color))

        def on_new_fac():
            pf.global_event(EVENT_DIPLO_FAC_NEW, (self.fac_name, self.fac_color))

        if len(self.fac_name) > 0:
            self.layout_row_dynamic(30, 1)
            self.button_label("Update Selected", on_update_fac)
            self.layout_row_dynamic(30, 1)
            self.button_label("Add New Faction", on_new_fac)
        else: #disabled - Can't update name to empty string
            old_style = (pf.button_style.normal, pf.button_style.hover, pf.button_style.active)
            old_style_text = (pf.button_style.text_normal, pf.button_style.text_hover, pf.button_style.text_active)

            pf.button_style.normal = DiplomacyTabWindow.DISABLED_BG_COLOR
            pf.button_style.hover = DiplomacyTabWindow.DISABLED_BG_COLOR
            pf.button_style.active = DiplomacyTabWindow.DISABLED_BG_COLOR
            pf.button_style.text_normal = DiplomacyTabWindow.DISABLED_TEXT_COLOR
            pf.button_style.text_hover = DiplomacyTabWindow.DISABLED_TEXT_COLOR
            pf.button_style.text_active = DiplomacyTabWindow.DISABLED_TEXT_COLOR

            self.layout_row_dynamic(30, 1)
            self.button_label("Update Selected", lambda : None)
            self.layout_row_dynamic(30, 1)
            self.button_label("Add New Faction", lambda : None)

            pf.button_style.normal, pf.button_style.hover, pf.button_style.active = old_style
            pf.button_style.text_normal, pf.button_style.text_hover, pf.button_style.text_active = old_style_text

