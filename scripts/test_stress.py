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
import sys
import math

import common.views.perf_stats_window as psw

import rts.units.knight
import rts.units.berzerker
import rts.units.anim_combatable as am


ARMY_SIZE = 256

MAP_HEIGHT = 4 * pf.TILES_PER_CHUNK_HEIGHT * pf.Z_COORDS_PER_TILE
MAP_WIDTH = 4 * pf.TILES_PER_CHUNK_WIDTH * pf.X_COORDS_PER_TILE

SPACING = 12

DIR_RIGHT = (0.0, 1.0/math.sqrt(2.0), 0.0, 1.0/math.sqrt(2.0))
DIR_LEFT = (0.0, -1.0/math.sqrt(2.0), 0.0, 1.0/math.sqrt(2.0))

red_army_units = []
blue_army_units = []
war_on = False

def setup_scene():

    pf.set_ambient_light_color((1.0, 1.0, 1.0))
    pf.set_emit_light_color((1.0, 1.0, 1.0))
    pf.set_emit_light_pos((1664.0, 1024.0, 384.0))

    pf.new_game("assets/maps", "plain.pfmap")

    pf.add_faction("RED", (255, 0, 0, 255))
    pf.add_faction("BLUE", (0, 0, 255, 255))

    pf.set_diplomacy_state(0, 1, pf.DIPLOMACY_STATE_WAR)

    pf.set_faction_controllable(0, False)
    pf.set_faction_controllable(1, False)

def setup_armies():

    global red_army_units
    global blue_army_units

    NROWS = 4
    NCOLS = math.ceil(ARMY_SIZE / NROWS)

    assert math.ceil(NROWS/2) * SPACING < MAP_HEIGHT//2
    assert math.ceil(NCOLS/2) * SPACING < MAP_WIDTH//2

    # (0,0) is the center of the map

    for r in range(int(-NROWS//2), int(NROWS//2 + NROWS%2)):
        for c in range(int(-NCOLS//2), int(NCOLS//2 + NCOLS%2)):

            x = -(r * SPACING) + 35
            z = c * SPACING
            y = pf.map_height_at_point(x, z)

            knight = rts.units.knight.Knight("assets/models/knight", "knight.pfobj", "Knight")
            knight.pos = (float(x), float(y), float(z))
            knight.rotation = DIR_RIGHT
            knight.faction_id = 0
            knight.selection_radius = 3.25
            knight.activate()
            knight.hold_position()

            red_army_units += [knight]

            x = (r * SPACING) - 35.0
            z = c * SPACING
            y = pf.map_height_at_point(x, z)

            berz = rts.units.berzerker.Berzerker("assets/models/berzerker", "berzerker.pfobj", "Berzerker")
            berz.pos = (float(x), float(y), float(z))
            berz.rotation = DIR_LEFT
            berz.faction_id = 1
            berz.selection_radius = 3.00
            berz.activate()
            berz.hold_position()

            blue_army_units += [berz]

def fixup_anim_combatable():

    def __on_death(self, event):
        self.play_anim(self.death_anim(), mode=pf.ANIM_MODE_ONCE_HIDE_ON_FINISH)
        self.register(pf.EVENT_ANIM_CYCLE_FINISHED, 
            am.AnimCombatable._AnimCombatable__on_death_anim_finish, self)

    def __on_death_anim_finish(self, event):
        self.unregister(pf.EVENT_ANIM_CYCLE_FINISHED, 
            am.AnimCombatable._AnimCombatable__on_death_anim_finish)
        try: 
            red_army_units.remove(self)
        except: pass
        try:
            blue_army_units.remove(self)
        except: pass

    am.AnimCombatable._AnimCombatable__on_death = __on_death
    am.AnimCombatable._AnimCombatable__on_death_anim_finish = __on_death_anim_finish

def start_war(user, event):

    if event[0] != pf.SDL_SCANCODE_W:
        return

    global war_on, red_army_units, blue_army_units
    if war_on:
        return
    war_on = True
    chunk_height = pf.TILES_PER_CHUNK_HEIGHT * pf.Z_COORDS_PER_TILE

    for unit in red_army_units:
        atk_pos = (
            unit.pos[0] - 100,
            (unit.pos[1] // chunk_height + 0.5) * chunk_height
        )
        unit.attack(atk_pos)

    for unit in blue_army_units:
        atk_pos = (
            unit.pos[0] + 100,
            (unit.pos[1] // chunk_height + 0.5) * chunk_height
        )
        unit.attack(atk_pos)

setup_scene()
setup_armies()
fixup_anim_combatable()

perf_stats_win = psw.PerfStatsWindow()
perf_stats_win.show()

pf.register_event_handler(pf.SDL_KEYDOWN, start_war, None)

