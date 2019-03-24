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
import map
import globals
import common.view_controllers.view_controller as vc


class TerrainTabVC(vc.ViewController):

    def __init__(self, view):
        self.view = view
        self.selected_tile = None
        self.painting = False

    def __update_objects_for_height_change(self):
        for obj in globals.active_objects_list:
            obj.pos = [obj.pos[0], pf.map_height_at_point(obj.pos[0], obj.pos[2]), obj.pos[2]]

    def __paint_selection(self):

        if pf.mouse_over_ui():
            return

        if pf.mouse_over_minimap():
            return

        global_r = self.selected_tile[0][0] * pf.TILES_PER_CHUNK_HEIGHT + self.selected_tile[1][0]
        global_c = self.selected_tile[0][1] * pf.TILES_PER_CHUNK_WIDTH  + self.selected_tile[1][1]

        for r in range(-((self.view.brush_size_idx + 1) // 2), ((self.view.brush_size_idx + 1) // 2) + 1):
            for c in range(-((self.view.brush_size_idx + 1) // 2), ((self.view.brush_size_idx + 1) // 2) + 1):

                tile_coords = globals.active_map.relative_tile_coords(global_r, global_c, r, c)
                if tile_coords is not None:

                    if self.view.brush_type_idx == 0:
                        globals.active_map.update_tile_mat(tile_coords, globals.active_map.materials[self.view.selected_mat_idx])
                    elif self.view.brush_type_idx == 1:
                        center_height = self.view.heights[self.view.selected_height_idx]
                        globals.active_map.update_tile(tile_coords, center_height, newtype=pf.TILETYPE_FLAT)

        if self.view.edges_type_idx == 1:
            self.__paint_smooth_border(self.view.brush_size_idx + 1, 'down')
            self.__paint_smooth_border(self.view.brush_size_idx + 1, 'up')
            self.__update_objects_for_height_change()

    def __tile_make_smooth(self, tile_coords, dir):
        """
        First, we pick a height for each of the 4 corners. The height is taken
        to be the the height of that corner in the 3 adjacent tiles.
        (Ex. when 'X' is the current corner of the '?' tile, pick the maximum/minimum height of 
        that corner in the surrounding tiles 1, 2, 3) Whether we use the max or min depends on if
        we have just lowered or raised the center tile.

            +------+------+------+
            |      |      |      |
            |   2 /-\ 1   |      |
            +-----|X|-----+------+
            |   3 \-/?????|      |
            |      |??????|      |
            +------+------+------+
            |      |      |      |
            |      |      |      |
            +------+------+------+

        Then we set the height to be one of 2 values: the max of the corner heights
        if it is that already, otherwise the min of the corner heights. This way, the
        corners will be at one of 2 heights.

        Lastly, we use the 'marching squares' algorithm to pick out the right tile. The
        corners that are 'high' are taken to be inside the contour and the corners that are
        'low' are taken to be outside the contour.
        """

        global_r = tile_coords[0][0] * pf.TILES_PER_CHUNK_HEIGHT + tile_coords[1][0]
        global_c = tile_coords[0][1] * pf.TILES_PER_CHUNK_WIDTH  + tile_coords[1][1]

        tile_for_case = [
            pf.TILETYPE_FLAT,
            pf.TILETYPE_CORNER_CONCAVE_NE,
            pf.TILETYPE_CORNER_CONCAVE_NW,
            pf.TILETYPE_RAMP_NS,
            pf.TILETYPE_CORNER_CONCAVE_SW,
            pf.TILETYPE_FLAT, #ambiguous case
            pf.TILETYPE_RAMP_WE,
            pf.TILETYPE_CORNER_CONVEX_NW,
            pf.TILETYPE_CORNER_CONCAVE_SE,
            pf.TILETYPE_RAMP_EW,
            pf.TILETYPE_FLAT, #ambiguous case
            pf.TILETYPE_CORNER_CONVEX_NE,
            pf.TILETYPE_RAMP_SN,
            pf.TILETYPE_CORNER_CONVEX_SE,
            pf.TILETYPE_CORNER_CONVEX_SW,
            pf.TILETYPE_FLAT
        ]

        tile = globals.active_map.tile_at_coords(*tile_coords)
        func = max if dir == 'up' else min

        nw_height = func([h for h in [
            getattr(globals.active_map.relative_tile(global_r, global_c,  0, -1), "top_right_height", None),
            getattr(globals.active_map.relative_tile(global_r, global_c, -1, -1), "bot_right_height", None),
            getattr(globals.active_map.relative_tile(global_r, global_c, -1,  0), "bot_left_height", None),
            getattr(tile, "top_left_height", None)
        ] if h is not None])

        ne_height = func([h for h in [
            getattr(globals.active_map.relative_tile(global_r, global_c, -1,  0), "bot_right_height", None),
            getattr(globals.active_map.relative_tile(global_r, global_c, -1,  1), "bot_left_height", None),
            getattr(globals.active_map.relative_tile(global_r, global_c,  0,  1), "top_left_height", None),
            getattr(tile, "top_right_height", None)
        ] if h is not None])

        se_height = func([h for h in [
            getattr(globals.active_map.relative_tile(global_r, global_c,  0,  1), "bot_left_height", None),
            getattr(globals.active_map.relative_tile(global_r, global_c,  1,  1), "top_left_height", None),
            getattr(globals.active_map.relative_tile(global_r, global_c,  1,  0), "top_right_height", None),
            getattr(tile, "bot_right_height", None)
        ] if h is not None])

        sw_height = func([h for h in [
            getattr(globals.active_map.relative_tile(global_r, global_c,  1,  0), "top_left_height", None),
            getattr(globals.active_map.relative_tile(global_r, global_c,  1, -1), "top_right_height", None),
            getattr(globals.active_map.relative_tile(global_r, global_c,  0, -1), "bot_right_height", None),
            getattr(tile, "bot_left_height", None)
        ] if h is not None]) 

        og_heights = [nw_height, ne_height, se_height, sw_height]
        heights = [h if h == max(og_heights) else min(og_heights) for h in og_heights]
        index = ((1 << 3) if heights[0] == max(heights) else 0) \
              | ((1 << 2) if heights[1] == max(heights) else 0) \
              | ((1 << 1) if heights[2] == max(heights) else 0) \
              | ((1 << 0) if heights[3] == max(heights) else 0)
        assert index >= 0 and index < 16
        
        base_height = min(heights)
        ramp_height = max(heights) - base_height
        new_type = tile_for_case[index]

        return base_height, new_type, ramp_height

    def __paint_smooth_border(self, radius, dir='up'):
        assert self.selected_tile is not None
        global_r = self.selected_tile[0][0] * pf.TILES_PER_CHUNK_HEIGHT + self.selected_tile[1][0]
        global_c = self.selected_tile[0][1] * pf.TILES_PER_CHUNK_WIDTH  + self.selected_tile[1][1]

        results = []
        for r in range(-radius, radius + 1):
            for c in range(-radius, radius + 1):

                tile_coords = globals.active_map.relative_tile_coords(global_r, global_c, r, c)
                if tile_coords is None:
                    continue

                left_edge = (c == -radius)
                right_edge = (c == radius)
                top_edge = (r == -radius)
                bot_edge = (r == radius)

                if left_edge and not (top_edge or bot_edge) \
                or right_edge and not (top_edge or bot_edge) \
                or top_edge and not (left_edge or right_edge) \
                or bot_edge and not (left_edge or right_edge):
                    results.append((tile_coords) + self.__tile_make_smooth(tile_coords, dir))

        for r in results:
            globals.active_map.update_tile( (r[0], r[1]), *r[2:] )

        corner_tiles_coords = [
            globals.active_map.relative_tile_coords(global_r, global_c, -radius, -radius),
            globals.active_map.relative_tile_coords(global_r, global_c, -radius,  radius),
            globals.active_map.relative_tile_coords(global_r, global_c,  radius,  radius),
            globals.active_map.relative_tile_coords(global_r, global_c,  radius, -radius)
        ]
        for coords in [c for c in corner_tiles_coords if c is not None]:
            r = self.__tile_make_smooth(coords, dir)
            globals.active_map.update_tile(coords, *r)

    def __on_selected_tile_changed(self, event):
        self.selected_tile = event
        self.view.selected_tile = event

        if self.painting == True and self.selected_tile is not None:
            self.__paint_selection() 

    def __on_mouse_pressed(self, event):
        if event[0] == pf.SDL_BUTTON_LEFT:
            self.painting = True
        if self.selected_tile is not None:
            self.__paint_selection() 

    def __on_mouse_released(self, event):
        if event[0] == pf.SDL_BUTTON_LEFT:
            self.painting = False

    def __on_brush_size_changed(self, event):
        pf.set_map_highlight_size(self.view.brush_size_idx + 1)

    def activate(self):
        pf.set_map_highlight_size(self.view.brush_size_idx + 1)
        pf.register_event_handler(pf.SDL_MOUSEBUTTONDOWN, TerrainTabVC.__on_mouse_pressed, self)
        pf.register_event_handler(pf.SDL_MOUSEBUTTONUP, TerrainTabVC.__on_mouse_released, self)
        pf.register_event_handler(pf.EVENT_SELECTED_TILE_CHANGED, TerrainTabVC.__on_selected_tile_changed, self)
        pf.register_event_handler(EVENT_TERRAIN_BRUSH_SIZE_CHANGED, TerrainTabVC.__on_brush_size_changed, self)

    def deactivate(self):
        pf.set_map_highlight_size(0)
        pf.unregister_event_handler(pf.SDL_MOUSEBUTTONDOWN, TerrainTabVC.__on_mouse_pressed)
        pf.unregister_event_handler(pf.SDL_MOUSEBUTTONUP, TerrainTabVC.__on_mouse_released)
        pf.unregister_event_handler(pf.EVENT_SELECTED_TILE_CHANGED, TerrainTabVC.__on_selected_tile_changed)
        pf.unregister_event_handler(EVENT_TERRAIN_BRUSH_SIZE_CHANGED, TerrainTabVC.__on_brush_size_changed)

