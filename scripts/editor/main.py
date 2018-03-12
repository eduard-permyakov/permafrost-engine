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
import view_controller as vc
import terrain_tab_controller as ter
import top_tab_controller as top

############################################################
# Global configs                                           #
############################################################

pf.set_ambient_light_color([1.0, 1.0, 1.0])
pf.set_emit_light_color([1.0, 1.0, 1.0])
pf.set_emit_light_pos([1024.0, 512.0, 256.0])

default_map = map.Map(4, 4)

with open(pf.get_basedir() + "assets/maps/grass-cliffs.pfmap", "r") as mapfile:
    mapdata = mapfile.read()
pf.new_game_string(default_map.pfmap_str())

############################################################
# Setup UI                                                 #
############################################################

terrain_tab_ctrl = ter.TerrainViewController(ui.TerrainTabWindow())
objects_tab_ctrl = vc.ObjectsViewController(ui.ObjectsTabWindow())

top_tab_ctrl = top.TabController(ui.TabBarWindow())
top_tab_ctrl.push_child("Terrain", terrain_tab_ctrl)
top_tab_ctrl.push_child("Objects", objects_tab_ctrl)

top_tab_ctrl.activate()

mb = ui.MenuButtonWindow(ui.Menu())
mb.show()

