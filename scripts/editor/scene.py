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

import globals
from constants import *


OBJECTS_LIST = [

    ########################################################################
    # UNITS                                                                #
    ########################################################################

    { 
        "path"           : "sinbad/Sinbad.pfobj",
        "anim"           : True,
        "idle"           : "IdleBase",
        "scale"          : [1.2,  1.2,  1.2], 
        "selectable"     : True,
        "sel_radius"     : 3.25,
        "class"          : "Sinbad",
        "construct_args" : ["assets/models/sinbad", "Sinbad.pfobj", "Sinbad"],
        "static"         : False,
        "collision"      : True
    },
    { 
        "path"           : "knight/knight.pfobj",
        "anim"           : True,
        "idle"           : "Idle",
        "scale"          : [0.8,  0.8,  0.8], 
        "selectable"     : True,
        "sel_radius"     : 3.25,
        "class"          : "Knight",
        "construct_args" : ["assets/models/knight", "knight.pfobj", "Knight"],
        "static"         : False,
        "collision"      : True
    },
    { 
        "path"           : "mage/mage.pfobj",
        "anim"           : True,
        "idle"           : "Idle",
        "scale"          : [0.6,  0.6,  0.6], 
        "selectable"     : True,
        "sel_radius"     : 4.25,
        "class"          : "Mage",
        "construct_args" : ["assets/models/mage", "mage.pfobj", "Mage"],
        "static"         : False,
        "collision"      : True
    },
    { 
        "path"           : "berzerker/berzerker.pfobj",
        "anim"           : True,
        "idle"           : "Idle",
        "scale"          : [0.7,  0.7,  0.7], 
        "selectable"     : True,
        "sel_radius"     : 3.00,
        "class"          : "Berzerker",
        "construct_args" : ["assets/models/berzerker", "berzerker.pfobj", "Berzerker"],
        "static"         : False,
        "collision"      : True
    },
    {
        "path"           : "goblin/goblin.pfobj",
        "anim"           : True,
        "idle"           : "Idle",
        "scale"          : [0.9,  0.9,  0.9],
        "selectable"     : True,
        "sel_radius"     : 3.00,
        "class"          : "Goblin",
        "construct_args" : ["assets/models/goblin", "goblin.pfobj", "Goblin"],
        "static"         : False,
        "collision"      : True
    },
    {
        "path"           : "deer/doe.pfobj",
        "anim"           : True,
        "idle"           : "Idle",
        "scale"          : [2.0,  2.0,  2.0],
        "selectable"     : True,
        "sel_radius"     : 3.25,
        "class"          : "Doe",
        "construct_args" : ["assets/models/deer", "doe.pfobj", "Doe"],
        "static"         : False,
        "collision"      : True
    },
    {
        "path"           : "deer/deer.pfobj",
        "anim"           : True,
        "idle"           : "Idle",
        "scale"          : [2.0,  2.0,  2.0],
        "selectable"     : True,
        "sel_radius"     : 3.25,
        "class"          : "Deer",
        "construct_args" : ["assets/models/deer", "deer.pfobj", "Deer"],
        "static"         : False,
        "collision"      : True
    },
    {
        "path"           : "chicken/chicken.pfobj",
        "anim"           : True,
        "idle"           : "Idle",
        "scale"          : [0.4,  0.4,  0.4],
        "selectable"     : True,
        "sel_radius"     : 1.50,
        "class"          : "Chicken",
        "construct_args" : ["assets/models/chicken", "chicken.pfobj", "Chicken"],
        "static"         : False,
        "collision"      : True
    },

    ########################################################################
    # TREES                                                                #
    ########################################################################

    { 
        "path"           : "oak_tree/oak_tree.pfobj",
        "anim"           : False,
        "scale"          : [1.6,  1.6,  1.6],
        "selectable"     : False,
        "sel_radius"     : 5.25,
        "static"         : True,
        "collision"      : True
    },
    { 
        "path"           : "oak_tree/oak_leafless.pfobj",
        "anim"           : False,
        "scale"          : [1.6,  1.6,  1.6],
        "selectable"     : False,
        "sel_radius"     : 5.25,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "tree_basic/tree_basic.pfobj",
        "anim"           : False,
        "scale"          : [10.0,  10.0,  10.0],
        "selectable"     : False,
        "sel_radius"     : 3.00,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "pine_tree/pine_tree.pfobj",
        "anim"           : False,
        "scale"          : [15.0,  15.0,  15.0],
        "selectable"     : False,
        "sel_radius"     : 2.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "tree_leafy/tree_leafy.pfobj",
        "anim"           : False,
        "scale"          : [10.0,  10.0,  10.0],
        "selectable"     : False,
        "sel_radius"     : 3.00,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "tree_dry/tree_dry.pfobj",
        "anim"           : False,
        "scale"          : [10.0,  10.0,  10.0],
        "selectable"     : False,
        "sel_radius"     : 2.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "large_pine_tree/large_pine_tree.pfobj",
        "anim"           : False,
        "scale"          : [22.0,  22.0,  22.0],
        "selectable"     : False,
        "sel_radius"     : 6.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "large_tree/large_tree.pfobj",
        "anim"           : False,
        "scale"          : [15.0,  15.0,  15.0],
        "selectable"     : False,
        "sel_radius"     : 5.25,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "palm_tree/palm.pfobj",
        "anim"           : False,
        "scale"          : [5.5,  5.5,  5.5],
        "selectable"     : False,
        "sel_radius"     : 3.25,
        "static"         : True,
        "collision"      : True
    },

    ########################################################################
    # DOODADS                                                              #
    ########################################################################

    {
        "path"           : "ceramic_jar/ceramic_jar.pfobj",
        "anim"           : False,
        "scale"          : [35.0,  35.0,  35.0],
        "selectable"     : False,
        "sel_radius"     : 2.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "rock/rock.pfobj",
        "anim"           : False,
        "scale"          : [2.0,  2.0,  2.0],
        "selectable"     : False,
        "sel_radius"     : 3.25,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "varied_rocks/rock_1.pfobj",
        "anim"           : False,
        "scale"          : [4.0,  4.0,  4.0],
        "selectable"     : False,
        "sel_radius"     : 3.25,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "varied_rocks/rock_2.pfobj",
        "anim"           : False,
        "scale"          : [4.0,  4.0,  4.0],
        "selectable"     : False,
        "sel_radius"     : 3.00,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "varied_rocks/rock_3.pfobj",
        "anim"           : False,
        "scale"          : [4.0,  4.0,  4.0],
        "selectable"     : False,
        "sel_radius"     : 3.25,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "varied_rocks/rock_4.pfobj",
        "anim"           : False,
        "scale"          : [4.0,  4.0,  4.0],
        "selectable"     : False,
        "sel_radius"     : 3.75,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "varied_rocks/rock_5.pfobj",
        "anim"           : False,
        "scale"          : [4.0,  4.0,  4.0],
        "selectable"     : False,
        "sel_radius"     : 3.25,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "shrub/shrub.pfobj",
        "anim"           : False,
        "scale"          : [6.5,  6.5,  6.5],
        "selectable"     : False,
        "sel_radius"     : 3.50,
        "static"         : True,
        "collision"      : False 
    },
    {
        "path"           : "bushes/bush_1.pfobj",
        "anim"           : False,
        "scale"          : [9.5,  9.5,  9.5],
        "selectable"     : False,
        "sel_radius"     : 3.50,
        "static"         : True,
        "collision"      : False
    },
    {
        "path"           : "bushes/bush_2.pfobj",
        "anim"           : False,
        "scale"          : [2.5,  2.5,  2.5],
        "selectable"     : False,
        "sel_radius"     : 3.75,
        "static"         : True,
        "collision"      : False
    },
    {
        "path"           : "grass/grass_1.pfobj",
        "anim"           : False,
        "scale"          : [1.5,  1.5,  1.5],
        "selectable"     : False,
        "sel_radius"     : 2.00,
        "static"         : True,
        "collision"      : False
    },
    {
        "path"           : "grass/grass_2.pfobj",
        "anim"           : False,
        "scale"          : [1.5,  1.5,  1.5],
        "selectable"     : False,
        "sel_radius"     : 2.00,
        "static"         : True,
        "collision"      : False
    },
    {
        "path"           : "grass/grass_3.pfobj",
        "anim"           : False,
        "scale"          : [1.5,  1.5,  1.5],
        "selectable"     : False,
        "sel_radius"     : 2.00,
        "static"         : True,
        "collision"      : False
    },
    {
        "path"           : "fern/fern.pfobj",
        "anim"           : False,
        "scale"          : [1.5,  1.5,  1.5],
        "selectable"     : False,
        "sel_radius"     : 2.50,
        "static"         : True,
        "collision"      : False
    },
    {
        "path"           : "well/well.pfobj",
        "anim"           : False,
        "scale"          : [3.0,  3.0,  3.0],
        "selectable"     : False,
        "sel_radius"     : 3.25,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/wood_fence_1.pfobj",
        "anim"           : False,
        "scale"          : [3.0,  3.0,  3.0],
        "selectable"     : False,
        "sel_radius"     : 3.00,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/wood_fence_2.pfobj",
        "anim"           : False,
        "scale"          : [3.0,  3.0,  3.0],
        "selectable"     : False,
        "sel_radius"     : 3.00,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/wood_trough.pfobj",
        "anim"           : False,
        "scale"          : [1.5,  1.5,  1.5],
        "selectable"     : False,
        "sel_radius"     : 3.00,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/wood_road_sign.pfobj",
        "anim"           : False,
        "scale"          : [0.7,  0.7,  0.7],
        "selectable"     : False,
        "sel_radius"     : 1.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/wood_lamp_post.pfobj",
        "anim"           : False,
        "scale"          : [0.7,  0.7,  0.7],
        "selectable"     : False,
        "sel_radius"     : 1.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/tombstone_1.pfobj",
        "anim"           : False,
        "scale"          : [3.0,  3.0,  3.0],
        "selectable"     : False,
        "sel_radius"     : 2.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/tombstone_2.pfobj",
        "anim"           : False,
        "scale"          : [3.0,  3.0,  3.0],
        "selectable"     : False,
        "sel_radius"     : 2.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/tombstone_3.pfobj",
        "anim"           : False,
        "scale"          : [3.0,  3.0,  3.0],
        "selectable"     : False,
        "sel_radius"     : 2.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/cross_1.pfobj",
        "anim"           : False,
        "scale"          : [3.0,  3.0,  3.0],
        "selectable"     : False,
        "sel_radius"     : 2.00,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/cross_2.pfobj",
        "anim"           : False,
        "scale"          : [3.0,  3.0,  3.0],
        "selectable"     : False,
        "sel_radius"     : 2.25,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/cross_3.pfobj",
        "anim"           : False,
        "scale"          : [3.0,  3.0,  3.0],
        "selectable"     : False,
        "sel_radius"     : 3.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/cross_4.pfobj",
        "anim"           : False,
        "scale"          : [3.0,  3.0,  3.0],
        "selectable"     : False,
        "sel_radius"     : 3.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/obelisk_1.pfobj",
        "anim"           : False,
        "scale"          : [3.0,  3.0,  3.0],
        "selectable"     : False,
        "sel_radius"     : 2.00,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/obelisk_2.pfobj",
        "anim"           : False,
        "scale"          : [3.0,  3.0,  3.0],
        "selectable"     : False,
        "sel_radius"     : 3.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/obelisk_3.pfobj",
        "anim"           : False,
        "scale"          : [3.0,  3.0,  3.0],
        "selectable"     : False,
        "sel_radius"     : 4.00,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/broken_pillar_1.pfobj",
        "anim"           : False,
        "scale"          : [3.0,  3.0,  3.0],
        "selectable"     : False,
        "sel_radius"     : 4.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/broken_pillar_2.pfobj",
        "anim"           : False,
        "scale"          : [3.0,  3.0,  3.0],
        "selectable"     : False,
        "sel_radius"     : 4.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "barrel/barrel.pfobj",
        "anim"           : False,
        "scale"          : [8.0,  8.0,  8.0],
        "selectable"     : False,
        "sel_radius"     : 3.25,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "hay/hay.pfobj",
        "anim"           : False,
        "scale"          : [6.0,  6.0,  6.0],
        "selectable"     : False,
        "sel_radius"     : 4.00,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "hay/hay_2.pfobj",
        "anim"           : False,
        "scale"          : [4.0,  4.0,  4.0],
        "selectable"     : False,
        "sel_radius"     : 4.00,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "hay/haystack.pfobj",
        "anim"           : False,
        "scale"          : [4.0,  4.0,  4.0],
        "selectable"     : False,
        "sel_radius"     : 6.75,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "crate/crate_1.pfobj",
        "anim"           : False,
        "scale"          : [4.0,  4.0,  4.0],
        "selectable"     : False,
        "sel_radius"     : 3.25,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "crate/crate_2.pfobj",
        "anim"           : False,
        "scale"          : [4.0,  4.0,  4.0],
        "selectable"     : False,
        "sel_radius"     : 3.25,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "war_banner/war_banner.pfobj",
        "anim"           : True,
        "idle"           : "ArmatureAction",
        "scale"          : [2.5,  2.5,  2.5],
        "selectable"     : False,
        "sel_radius"     : 1.75,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "cart/cart.pfobj",
        "anim"           : False,
        "scale"          : [2.5,  2.5,  2.5],
        "selectable"     : False,
        "sel_radius"     : 3.75,
        "static"         : True,
        "collision"      : True
    },

    ########################################################################
    # BUILDINGS                                                            #
    ########################################################################

    {
        "path"           : "mage_tower/mage_tower.pfobj",
        "anim"           : False,
        "scale"          : [6.0,  6.0,  6.0],
        "selectable"     : False,
        "sel_radius"     : 7.00,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "tower/tower.pfobj",
        "anim"           : False,
        "scale"          : [5.0,  5.0,  5.0],
        "selectable"     : False,
        "sel_radius"     : 8.00,
        "static"         : True,
        "collision"      : True
    },
]


def __meta_dict_for_path(path):
    import os
    for dict in OBJECTS_LIST:
        dict_path = dict["path"]
        if dict_path == path:
            return dict
    return None

def save_scene(filename):
    with open(filename, "w") as scenefile:

        scenefile.write("num_factions {0}\n".format(len(globals.factions_list)))
        for fac in globals.factions_list:
            scenefile.write("faction \"{0}\"\n".format(fac.name))
            scenefile.write("    color vec3 {0:.6f} {1:.6f} {2:.6f}\n".format(fac.color[0], fac.color[1], fac.color[2]))

        scenefile.write("num_entities {0}\n".format(len(globals.active_objects_list)))
        for obj in globals.active_objects_list:
            num_atts = 8
            meta_dict = __meta_dict_for_path(obj.pfobj_path[len(MODELS_PREFIX_DIR) + 2:])
            assert(meta_dict is not None)
            if "idle" in meta_dict:
                num_atts += 1
            if "sel_radius" in meta_dict:
                num_atts += 1
            if "class" in meta_dict:
                num_atts += 1
            if "construct_args" in meta_dict:
                num_atts += 1
            scenefile.write("entity {0} {1} {2}\n".format(obj.name, MODELS_PREFIX_DIR + meta_dict["path"], num_atts))
            scenefile.write("   position vec3 {0:.6f} {1:.6f} {2:.6f}\n".format(obj.pos[0], obj.pos[1], obj.pos[2]))
            scenefile.write("   scale vec3 {0:.6f} {1:.6f} {2:.6f}\n".format(obj.scale[0], obj.scale[1], obj.scale[2]))
            scenefile.write("   rotation quat {0:.6f} {1:.6f} {2:.6f} {3:.6f}\n".format(obj.rotation[0], obj.rotation[1], obj.rotation[2], obj.rotation[3]))
            scenefile.write("   animated bool {0}\n".format(int(meta_dict["anim"])))
            scenefile.write("   selectable bool {0}\n".format(int(meta_dict["selectable"])))
            scenefile.write("   static bool {0}\n".format(int(meta_dict["static"])))
            scenefile.write("   collision bool {0}\n".format(int(meta_dict["collision"])))
            scenefile.write("   faction_id int {0}\n".format(obj.faction_id))
            if "idle" in meta_dict:
                scenefile.write("   idle_clip string {0}\n".format(meta_dict["idle"]))
            if "sel_radius" in meta_dict:
                scenefile.write("   selection_radius float {0}\n".format(obj.selection_radius))
            if "class" in meta_dict:
                scenefile.write("   class string {0}\n".format(meta_dict["class"]))
            if "construct_args" in meta_dict:
                scenefile.write("   constructor_arguments int {0}\n".format(len(meta_dict["construct_args"])))
                for arg in meta_dict["construct_args"]:
                    if isinstance(arg, int):
                        type = "int" 
                    elif isinstance(arg, float):
                        type = "float"
                    elif isinstance(arg, basestring):
                        type = "string"
                    else:
                        raise ValueError("Constructor arguments must be of type int, float, or basestring")
                    scenefile.write("       {0} {1}\n".format(type, arg))

