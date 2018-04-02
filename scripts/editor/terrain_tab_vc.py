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
from constants import *
from map import Material
from ui import ViewController
import globals

class TerrainTabVC(ViewController):

    MATERIALS_LIST = [
        Material("Grass",       "grass.png",        1.0, (0.3, 0.3, 0.3), (0.1, 0.1, 0.1)), 
        Material("Cliffs",      "cliffs.png",       1.0, (0.8, 0.8, 0.8), (0.3, 0.3, 0.3)),
        Material("Cobblestone", "cobblestone.jpg",  1.0, (0.5, 0.5, 0.5), (0.2, 0.2, 0.2)),
        Material("Dirty Grass", "dirty_grass.jpg",  1.0, (0.3, 0.3, 0.3), (0.1, 0.1, 0.1))
    ]

    def __init__(self, view):
        self.view = view
        self.selected_mat_idx = 0
        self.selected_tile = None
        self.painting = False

        self.view.materials_list = TerrainTabVC.MATERIALS_LIST
        self.view.selected_mat_idx = self.selected_mat_idx

    def __on_selected_tile_changed(self, event):
        self.selected_tile = event
        self.view.selected_tile = event

        if self.painting == True:
            globals.active_map.update_tile(self.selected_tile, TerrainTabVC.MATERIALS_LIST[self.selected_mat_idx])

    def __on_mat_selection_changed(self, event):
        self.selected_mat_idx = event

    def __on_mouse_pressed(self, event):
        if event[0] == SDL_BUTTON_LEFT:
            self.painting = True
        globals.active_map.update_tile(self.selected_tile, TerrainTabVC.MATERIALS_LIST[self.selected_mat_idx])

    def __on_mouse_released(self, event):
        if event[0] == SDL_BUTTON_LEFT:
            self.painting = False

    def activate(self):
        pf.register_event_handler(EVENT_SELECTED_TILE_CHANGED, TerrainTabVC.__on_selected_tile_changed, self)
        pf.register_event_handler(EVENT_TEXTURE_SELECTION_CHANGED, TerrainTabVC.__on_mat_selection_changed, self)
        pf.register_event_handler(EVENT_SDL_MOUSEBUTTONDOWN, TerrainTabVC.__on_mouse_pressed, self)
        pf.register_event_handler(EVENT_SDL_MOUSEBUTTONUP, TerrainTabVC.__on_mouse_released, self)

    def deactivate(self):
        pf.unregister_event_handler(EVENT_SELECTED_TILE_CHANGED, TerrainTabVC.__on_selected_tile_changed)
        pf.unregister_event_handler(EVENT_TEXTURE_SELECTION_CHANGED, TerrainTabVC.__on_mat_selection_changed)
        pf.unregister_event_handler(EVENT_SDL_MOUSEBUTTONDOWN, TerrainTabVC.__on_mouse_pressed)
        pf.unregister_event_handler(EVENT_SDL_MOUSEBUTTONUP, TerrainTabVC.__on_mouse_released)

