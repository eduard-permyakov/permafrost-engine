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
import ui
import sinbad as sb

from constants import *

############################################################
# Global configs                                           #
############################################################

pf.set_ambient_light_color([1.0, 1.0, 1.0])
pf.set_emit_light_color([1.0, 1.0, 1.0])
pf.set_emit_light_pos([1024.0, 768.0, 768.0])

pf.new_game("assets/maps", "demo-arena.pfmap")
pf.set_map_render_mode(CHUNK_RENDER_MODE_PREBAKED);

############################################################
# Setup entities                                           #
############################################################

sinbad = sb.Sinbad("assets/models/sinbad", "Sinbad.pfobj", "Sinbad")
sinbad.pos = [-80.0, 18.0, -95.0]
sinbad.scale = [1.0, 1.0, 1.0]
sinbad.activate()

oak_tree = pf.Entity("assets/models/oak_tree", "oak_tree.pfobj", "OakTree")
oak_tree.pos = [-40.0, 14.0, -115.0]
oak_tree.scale = [2.0, 2.0, 2.0]
oak_tree.activate()

oak_leafless = pf.Entity("assets/models/oak_tree", "oak_leafless.pfobj", "OakLeafless")
oak_leafless.pos = [-95.0, 18.0, -35.0]
oak_leafless.scale = [1.5, 1.5, 1.5]
oak_leafless.activate()

############################################################
# Setup global events                                      #
############################################################

active_cam_idx = 0
def toggle_camera(user, event):
    mode_for_idx = [1, 0]

    if event[0] == SDL_SCANCODE_C:
        global active_cam_idx
        active_cam_idx = (active_cam_idx + 1) % 2
        pf.activate_camera(active_cam_idx, mode_for_idx[active_cam_idx])

def toggle_sinbad(user, event):
    if event[0] == SDL_SCANCODE_V:
        sinbad.notify(EVENT_SINBAD_TOGGLE_ANIM, None)


pf.register_event_handler(EVENT_SDL_KEYDOWN, toggle_camera, None)
pf.register_event_handler(EVENT_SDL_KEYDOWN, toggle_sinbad, None)

############################################################
# Setup UI                                                 #
############################################################

demo_win = ui.DemoWindow()
demo_win.show()

perf_win = ui.PerfStatsWindow()
perf_win.show()

