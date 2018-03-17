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
import terrain_tab_vc as tt
import tab_bar_vc as tb
import menu_vc
import globals

############################################################
# Global globalss                                           #
############################################################

pf.set_ambient_light_color([1.0, 1.0, 1.0])
pf.set_emit_light_color([1.0, 1.0, 1.0])
pf.set_emit_light_pos([1024.0, 512.0, 256.0])

globals.active_map = map.Map.from_filepath(pf.get_basedir() + "assets/maps/grass-cliffs.pfmap")
pf.new_game_string(globals.active_map.pfmap_str())

############################################################
# Setup UI                                                 #
############################################################

terrain_tab_vc = tt.TerrainTabVC(ui.TerrainTabWindow())
objects_tab_vc = ui.ObjectsVC(ui.ObjectsTabWindow())

tab_bar_vc = tb.TabBarVC(ui.TabBarWindow())
tab_bar_vc.push_child("Terrain", terrain_tab_vc)
tab_bar_vc.push_child("Objects", objects_tab_vc)
tab_bar_vc.activate()
tab_bar_vc.view.show()

menu = ui.Menu()
menuvc = menu_vc.MenuVC(menu)
menuvc.activate()

mb = ui.MenuButtonWindow(menu)
mb.show()


