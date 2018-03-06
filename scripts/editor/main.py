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
import tabbar

############################################################
# Global configs                                           #
############################################################

pf.set_ambient_light_color([1.0, 1.0, 1.0])
pf.set_emit_light_color([1.0, 1.0, 1.0])
pf.set_emit_light_pos([1024.0, 512.0, 256.0])

pf.new_game("assets/maps/grass-cliffs-1", "grass-cliffs.pfmap")

############################################################
# Setup UI                                                 #
############################################################

one = tabbar.PerfStatsWindow()
two = tabbar.DemoWindow();

top_tb = tabbar.TabBar()
top_tb.push_child("Demo", two)
top_tb.push_child("Performance", one)
top_tb.show()

