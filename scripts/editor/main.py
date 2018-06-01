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
import ui
import map
import terrain_tab_vc as ttvc
import objects_tab_vc as otvc
import tab_bar_vc as tbvc
import menu_vc
import globals
import mouse_events
from math import cos, pi

############################################################
# Global settings                                           #
############################################################

pf.set_ambient_light_color([1.0, 1.0, 1.0])
pf.set_emit_light_color([1.0, 1.0, 1.0])
pf.set_emit_light_pos([1024.0, 512.0, 256.0])

pf.new_game_string(globals.active_map.pfmap_str())
pf.set_minimap_position(ui.LEFT_PANE_WIDTH + MINIMAP_PX_WIDTH/cos(pi/4)/2 + 10, 
    pf.get_resolution()[1] - MINIMAP_PX_WIDTH/cos(pi/4)/2 - 10)
pf.disable_unit_selection()

mouse_events.install()

############################################################
# Setup UI                                                 #
############################################################

terrain_tab_vc = ttvc.TerrainTabVC(ui.TerrainTabWindow())
objects_tab_vc = otvc.ObjectsVC(ui.ObjectsTabWindow())

tab_bar_vc = tbvc.TabBarVC(ui.TabBarWindow())
tab_bar_vc.push_child("Terrain", terrain_tab_vc)
tab_bar_vc.push_child("Objects", objects_tab_vc)
tab_bar_vc.activate()
tab_bar_vc.view.show()

menu = ui.Menu()
menuvc = menu_vc.MenuVC(menu)
menuvc.activate()

mb = ui.MenuButtonWindow(menu)
mb.show()


