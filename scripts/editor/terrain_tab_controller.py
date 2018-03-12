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

import pf
import view_controller as vc
from constants import *
from map import Material

class TerrainViewController(vc.ViewController):

    MATERIALS_LIST = [
        Material("Grass",       "grass.png"), 
        Material("Cliffs",      "cliffs.png"),
        Material("Cobblestone", "cobblestone.png"),
    ]

    def __init__(self, view):
        self.view = view
        self.selected_mat_idx = 0
        self.selected_tile = None

        self.view.materials_list = TerrainViewController.MATERIALS_LIST
        self.view.selected_mat_idx = self.selected_mat_idx

    def __on_selected_tile_changed(self, event):
        self.selected_tile = event
        self.view.selected_tile = event

    def __on_mat_selection_changed(self, event):
        self.view.selected_mat_idx = event

    def activate(self):
        pf.register_event_handler(EVENT_SELECTED_TILE_CHANGED, TerrainViewController.__on_selected_tile_changed, self)
        pf.register_event_handler(EVENT_TEXTURE_SELECTION_CHANGED, TerrainViewController.__on_mat_selection_changed, self)

    def deactivate(self):
        pf.register_event_handler(EVENT_SELECTED_TILE_CHANGED, TerrainViewController.__on_selected_tile_changed, self)
        pf.register_event_handler(EVENT_TEXTURE_SELECTION_CHANGED, TerrainViewController.__on_mat_selection_changed, self)

