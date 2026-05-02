#
#  This file is part of Permafrost Engine. 
#  Copyright (C) 2018-2023 Eduard Permyakov 
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

from io import StringIO
import os
import sys

try:
    from editor.constants import MODELS_PREFIX_DIR, pf
except ImportError:
    from constants import MODELS_PREFIX_DIR, pf

try:
    STRING_TYPES = (basestring,)
except NameError:
    STRING_TYPES = (str,)


PFSCENE_VERSION = 1.0
OBJECTS_LIST = [

    ########################################################################
    # UNITS                                                                #
    ########################################################################

    { 
        "path"           : "sinbad/Sinbad.pfobj",
        "anim"           : True,
        "idle"           : "IdleBase",
        "scale"          : (1.2,  1.2,  1.2), 
        "selectable"     : True,
        "sel_radius"     : 3.25,
        "class"          : "Sinbad",
        "construct_args" : ("assets/models/sinbad", "Sinbad.pfobj", "Sinbad"),
        "static"         : False,
        "collision"      : True,
        "vision_range"   : 35.0
    },
    { 
        "path"           : "knight/knight.pfobj",
        "anim"           : True,
        "idle"           : "Idle",
        "scale"          : (0.8,  0.8,  0.8), 
        "selectable"     : True,
        "sel_radius"     : 3.25,
        "class"          : "Knight",
        "construct_args" : ("assets/models/knight", "knight.pfobj", "Knight"),
        "static"         : False,
        "collision"      : True,
        "vision_range"   : 35.0
    },
    { 
        "path"           : "mage/mage.pfobj",
        "anim"           : True,
        "idle"           : "Idle",
        "scale"          : (0.6,  0.6,  0.6), 
        "selectable"     : True,
        "sel_radius"     : 4.25,
        "class"          : "Mage",
        "construct_args" : ("assets/models/mage", "mage.pfobj", "Mage"),
        "static"         : False,
        "collision"      : True,
        "vision_range"   : 35.0
    },
    { 
        "path"           : "berzerker/berzerker.pfobj",
        "anim"           : True,
        "idle"           : "Idle",
        "scale"          : (0.7,  0.7,  0.7), 
        "selectable"     : True,
        "sel_radius"     : 3.00,
        "class"          : "Berzerker",
        "construct_args" : ("assets/models/berzerker", "berzerker.pfobj", "Berzerker"),
        "static"         : False,
        "collision"      : True,
        "vision_range"   : 35.0
    },
    {
        "path"           : "goblin/goblin.pfobj",
        "anim"           : True,
        "idle"           : "Idle",
        "scale"          : (0.9,  0.9,  0.9),
        "selectable"     : True,
        "sel_radius"     : 3.00,
        "class"          : "Goblin",
        "construct_args" : ("assets/models/goblin", "goblin.pfobj", "Goblin"),
        "static"         : False,
        "collision"      : True,
        "vision_range"   : 35.0
    },
    {
        "path"           : "deer/doe.pfobj",
        "anim"           : True,
        "idle"           : "Idle",
        "scale"          : (2.0,  2.0,  2.0),
        "selectable"     : True,
        "sel_radius"     : 3.25,
        "class"          : "Doe",
        "construct_args" : ("assets/models/deer", "doe.pfobj", "Doe"),
        "static"         : False,
        "collision"      : True,
        "vision_range"   : 35.0
    },
    {
        "path"           : "deer/deer.pfobj",
        "anim"           : True,
        "idle"           : "Idle",
        "scale"          : (2.0,  2.0,  2.0),
        "selectable"     : True,
        "sel_radius"     : 3.25,
        "class"          : "Deer",
        "construct_args" : ("assets/models/deer", "deer.pfobj", "Deer"),
        "static"         : False,
        "collision"      : True,
        "vision_range"   : 35.0
    },
    {
        "path"           : "chicken/chicken.pfobj",
        "anim"           : True,
        "idle"           : "Idle",
        "scale"          : (0.4,  0.4,  0.4),
        "selectable"     : True,
        "sel_radius"     : 1.50,
        "class"          : "Chicken",
        "construct_args" : ("assets/models/chicken", "chicken.pfobj", "Chicken"),
        "static"         : False,
        "collision"      : True,
        "vision_range"   : 15.0
    },

    ########################################################################
    # TREES                                                                #
    ########################################################################

    { 
        "path"           : "oak_tree/oak_tree.pfobj",
        "anim"           : False,
        "scale"          : (1.6,  1.6,  1.6),
        "selectable"     : False,
        "sel_radius"     : 5.25,
        "static"         : True,
        "collision"      : True
    },
    { 
        "path"           : "oak_tree/oak_leafless.pfobj",
        "anim"           : False,
        "scale"          : (1.6,  1.6,  1.6),
        "selectable"     : False,
        "sel_radius"     : 5.25,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "tree_basic/tree_basic.pfobj",
        "anim"           : False,
        "scale"          : (10.0,  10.0,  10.0),
        "selectable"     : False,
        "sel_radius"     : 3.00,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "pine_tree/pine_tree.pfobj",
        "anim"           : False,
        "scale"          : (15.0,  15.0,  15.0),
        "selectable"     : False,
        "sel_radius"     : 2.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "tree_leafy/tree_leafy.pfobj",
        "anim"           : False,
        "scale"          : (10.0,  10.0,  10.0),
        "selectable"     : False,
        "sel_radius"     : 3.00,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "tree_dry/tree_dry.pfobj",
        "anim"           : False,
        "scale"          : (10.0,  10.0,  10.0),
        "selectable"     : False,
        "sel_radius"     : 2.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "large_pine_tree/large_pine_tree.pfobj",
        "anim"           : False,
        "scale"          : (22.0,  22.0,  22.0),
        "selectable"     : False,
        "sel_radius"     : 6.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "large_tree/large_tree.pfobj",
        "anim"           : False,
        "scale"          : (15.0,  15.0,  15.0),
        "selectable"     : False,
        "sel_radius"     : 5.25,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "palm_tree/palm.pfobj",
        "anim"           : False,
        "scale"          : (5.5,  5.5,  5.5),
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
        "scale"          : (35.0,  35.0,  35.0),
        "selectable"     : False,
        "sel_radius"     : 2.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "rock/rock.pfobj",
        "anim"           : False,
        "scale"          : (2.0,  2.0,  2.0),
        "selectable"     : False,
        "sel_radius"     : 3.25,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "varied_rocks/rock_1.pfobj",
        "anim"           : False,
        "scale"          : (4.0,  4.0,  4.0),
        "selectable"     : False,
        "sel_radius"     : 3.25,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "varied_rocks/rock_2.pfobj",
        "anim"           : False,
        "scale"          : (4.0,  4.0,  4.0),
        "selectable"     : False,
        "sel_radius"     : 3.00,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "varied_rocks/rock_3.pfobj",
        "anim"           : False,
        "scale"          : (4.0,  4.0,  4.0),
        "selectable"     : False,
        "sel_radius"     : 3.25,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "varied_rocks/rock_4.pfobj",
        "anim"           : False,
        "scale"          : (4.0,  4.0,  4.0),
        "selectable"     : False,
        "sel_radius"     : 3.75,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "varied_rocks/rock_5.pfobj",
        "anim"           : False,
        "scale"          : (4.0,  4.0,  4.0),
        "selectable"     : False,
        "sel_radius"     : 3.25,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "shrub/shrub.pfobj",
        "anim"           : False,
        "scale"          : (6.5,  6.5,  6.5),
        "selectable"     : False,
        "sel_radius"     : 3.50,
        "static"         : True,
        "collision"      : False 
    },
    {
        "path"           : "bushes/bush_1.pfobj",
        "anim"           : False,
        "scale"          : (9.5,  9.5,  9.5),
        "selectable"     : False,
        "sel_radius"     : 3.50,
        "static"         : True,
        "collision"      : False
    },
    {
        "path"           : "bushes/bush_2.pfobj",
        "anim"           : False,
        "scale"          : (2.5,  2.5,  2.5),
        "selectable"     : False,
        "sel_radius"     : 3.75,
        "static"         : True,
        "collision"      : False
    },
    {
        "path"           : "grass/grass_1.pfobj",
        "anim"           : False,
        "scale"          : (1.5,  1.5,  1.5),
        "selectable"     : False,
        "sel_radius"     : 2.00,
        "static"         : True,
        "collision"      : False
    },
    {
        "path"           : "grass/grass_2.pfobj",
        "anim"           : False,
        "scale"          : (1.5,  1.5,  1.5),
        "selectable"     : False,
        "sel_radius"     : 2.00,
        "static"         : True,
        "collision"      : False
    },
    {
        "path"           : "grass/grass_3.pfobj",
        "anim"           : False,
        "scale"          : (1.5,  1.5,  1.5),
        "selectable"     : False,
        "sel_radius"     : 2.00,
        "static"         : True,
        "collision"      : False
    },
    {
        "path"           : "fern/fern.pfobj",
        "anim"           : False,
        "scale"          : (1.5,  1.5,  1.5),
        "selectable"     : False,
        "sel_radius"     : 2.50,
        "static"         : True,
        "collision"      : False
    },
    {
        "path"           : "well/well.pfobj",
        "anim"           : False,
        "scale"          : (3.0,  3.0,  3.0),
        "selectable"     : False,
        "sel_radius"     : 3.25,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/wood_fence_1.pfobj",
        "anim"           : False,
        "scale"          : (3.0,  3.0,  3.0),
        "selectable"     : False,
        "sel_radius"     : 3.00,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/wood_fence_2.pfobj",
        "anim"           : False,
        "scale"          : (3.0,  3.0,  3.0),
        "selectable"     : False,
        "sel_radius"     : 3.00,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/wood_trough.pfobj",
        "anim"           : False,
        "scale"          : (1.5,  1.5,  1.5),
        "selectable"     : False,
        "sel_radius"     : 3.00,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/wood_road_sign.pfobj",
        "anim"           : False,
        "scale"          : (0.7,  0.7,  0.7),
        "selectable"     : False,
        "sel_radius"     : 1.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/wood_lamp_post.pfobj",
        "anim"           : False,
        "scale"          : (0.7,  0.7,  0.7),
        "selectable"     : False,
        "sel_radius"     : 1.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/tombstone_1.pfobj",
        "anim"           : False,
        "scale"          : (3.0,  3.0,  3.0),
        "selectable"     : False,
        "sel_radius"     : 2.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/tombstone_2.pfobj",
        "anim"           : False,
        "scale"          : (3.0,  3.0,  3.0),
        "selectable"     : False,
        "sel_radius"     : 2.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/tombstone_3.pfobj",
        "anim"           : False,
        "scale"          : (3.0,  3.0,  3.0),
        "selectable"     : False,
        "sel_radius"     : 2.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/cross_1.pfobj",
        "anim"           : False,
        "scale"          : (3.0,  3.0,  3.0),
        "selectable"     : False,
        "sel_radius"     : 2.00,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/cross_2.pfobj",
        "anim"           : False,
        "scale"          : (3.0,  3.0,  3.0),
        "selectable"     : False,
        "sel_radius"     : 2.25,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/cross_3.pfobj",
        "anim"           : False,
        "scale"          : (3.0,  3.0,  3.0),
        "selectable"     : False,
        "sel_radius"     : 3.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/cross_4.pfobj",
        "anim"           : False,
        "scale"          : (3.0,  3.0,  3.0),
        "selectable"     : False,
        "sel_radius"     : 3.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/obelisk_1.pfobj",
        "anim"           : False,
        "scale"          : (3.0,  3.0,  3.0),
        "selectable"     : False,
        "sel_radius"     : 2.00,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/obelisk_2.pfobj",
        "anim"           : False,
        "scale"          : (3.0,  3.0,  3.0),
        "selectable"     : False,
        "sel_radius"     : 3.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/obelisk_3.pfobj",
        "anim"           : False,
        "scale"          : (3.0,  3.0,  3.0),
        "selectable"     : False,
        "sel_radius"     : 4.00,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/broken_pillar_1.pfobj",
        "anim"           : False,
        "scale"          : (3.0,  3.0,  3.0),
        "selectable"     : False,
        "sel_radius"     : 4.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "props/broken_pillar_2.pfobj",
        "anim"           : False,
        "scale"          : (3.0,  3.0,  3.0),
        "selectable"     : False,
        "sel_radius"     : 4.50,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "barrel/barrel.pfobj",
        "anim"           : False,
        "scale"          : (8.0,  8.0,  8.0),
        "selectable"     : False,
        "sel_radius"     : 3.25,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "hay/hay.pfobj",
        "anim"           : False,
        "scale"          : (6.0,  6.0,  6.0),
        "selectable"     : False,
        "sel_radius"     : 4.00,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "hay/hay_2.pfobj",
        "anim"           : False,
        "scale"          : (4.0,  4.0,  4.0),
        "selectable"     : False,
        "sel_radius"     : 4.00,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "hay/haystack.pfobj",
        "anim"           : False,
        "scale"          : (4.0,  4.0,  4.0),
        "selectable"     : False,
        "sel_radius"     : 6.75,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "crate/crate_1.pfobj",
        "anim"           : False,
        "scale"          : (4.0,  4.0,  4.0),
        "selectable"     : False,
        "sel_radius"     : 3.25,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "crate/crate_2.pfobj",
        "anim"           : False,
        "scale"          : (4.0,  4.0,  4.0),
        "selectable"     : False,
        "sel_radius"     : 3.25,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "war_banner/war_banner.pfobj",
        "anim"           : True,
        "idle"           : "ArmatureAction",
        "scale"          : (2.5,  2.5,  2.5),
        "selectable"     : False,
        "sel_radius"     : 1.75,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "cart/cart.pfobj",
        "anim"           : False,
        "scale"          : (2.5,  2.5,  2.5),
        "selectable"     : False,
        "sel_radius"     : 3.75,
        "static"         : True,
        "collision"      : True
    },
    {
        "path"           : "build_site_marker/build-site-marker.pfobj",
        "anim"           : False,
        "scale"          : (2.5,  2.5,  2.5),
        "selectable"     : False,
        "sel_radius"     : 3.50,
        "static"         : True,
        "collision"      : True
    },

    ########################################################################
    # BUILDINGS                                                            #
    ########################################################################

    {
        "path"           : "mage_tower/mage_tower.pfobj",
        "anim"           : False,
        "scale"          : (6.0,  6.0,  6.0),
        "selectable"     : False,
        "sel_radius"     : 7.00,
        "static"         : True,
        "collision"      : True,
        "vision_range"   : 50.0
    },
    {
        "path"           : "tower/tower.pfobj",
        "anim"           : False,
        "scale"          : (5.0,  5.0,  5.0),
        "selectable"     : False,
        "sel_radius"     : 8.00,
        "static"         : True,
        "collision"      : True,
        "vision_range"   : 50.0
    },
]


def _meta_dict_for_path(path):
    for dict in OBJECTS_LIST:
        dict_path = dict["path"]
        if dict_path == path:
            return dict
    return None

def _meta_dict_for_object(obj, path):
    meta = _safe_getattr(obj, "session_scene_meta")
    if callable(meta):
        meta = meta()

    base = _meta_dict_for_path(path)
    if meta is None:
        return base

    ret = {}
    if base is not None:
        ret.update(base)
    ret.update(meta)
    if "path" not in ret:
        ret["path"] = path
    return ret

def _write_constructor_args(scenefile, construct_args):
    scenefile.write("   constructor_arguments int {0}\n".format(len(construct_args)))
    for arg in construct_args:
        if isinstance(arg, bool):
            arg_type = "int"
            arg = int(arg)
        elif isinstance(arg, int):
            arg_type = "int"
        elif isinstance(arg, float):
            arg_type = "float"
        elif isinstance(arg, STRING_TYPES):
            arg_type = "string"
        else:
            raise ValueError("Constructor arguments must be of type int, float, or string")
        scenefile.write("       {0} {1}\n".format(arg_type, arg))

def _safe_getattr(obj, attr):
    try:
        return getattr(obj, attr)
    except AttributeError:
        return None

def _scene_output_path(filename):
    if os.path.isabs(filename):
        out_path = filename
    else:
        out_path = os.path.join(pf.get_basedir(), filename)

    out_dir = os.path.dirname(out_path)
    if out_dir and not os.path.isdir(out_dir):
        os.makedirs(out_dir)
    return out_path

def _write_region_section(scenefile, regions_list):
    scenefile.write("section \"regions\"\n")
    scenefile.write("num_regions {0}\n".format(len(regions_list)))

    for region in regions_list:
        params = region.parameters
        if region.type == pf.REGION_CIRCLE:
            scenefile.write(" region {0} {1} 2\n".format(region.name, region.type))
            scenefile.write("   pos vec2 {0:.6f} {1:.6f}\n".format(region.position[0], region.position[1]))
            scenefile.write("   radius float {0:.6f}\n".format(params["radius"]))
        elif region.type == pf.REGION_RECTANGLE:
            scenefile.write(" region {0} {1} 2\n".format(region.name, region.type))
            scenefile.write("   pos vec2 {0:.6f} {1:.6f}\n".format(region.position[0], region.position[1]))
            scenefile.write("   dimensions vec2 {0:.6f} {1:.6f}\n".format(params["dimensions"][0], params["dimensions"][1]))
        else:
            raise ValueError("Unsupported region type '{0}'".format(region.type))

def _write_camera_section(scenefile, cameras_list):
    scenefile.write("section \"cameras\"\n")
    scenefile.write("num_cameras {0}\n".format(len(cameras_list)))

    for idx, camera in enumerate(cameras_list):
        name = _safe_getattr(camera, "name")
        if not name:
            name = "camera_{0}".format(idx)
        scenefile.write(" camera {0}\n".format(name))
        scenefile.write("   position vec3 {0:.6f} {1:.6f} {2:.6f}\n".format(camera.position[0], camera.position[1], camera.position[2]))
        scenefile.write("   pitch float {0:.6f}\n".format(camera.pitch))
        scenefile.write("   yaw float {0:.6f}\n".format(camera.yaw))

def _write_scene_to_stream(scenefile, objects_list, factions_list=None, regions_list=None, cameras_list=None, include_factions=True):
    scenefile.write("version %.01f\n" % (PFSCENE_VERSION,))
    if regions_list is None:
        regions_list = []
    if cameras_list is None:
        cameras_list = []

    num_sections = 1 + int(include_factions) + int(bool(regions_list)) + int(bool(cameras_list))
    scenefile.write("num_sections {0}\n".format(num_sections))

    if factions_list is None:
        factions_list = pf.get_factions_list()

    if include_factions:
        scenefile.write("section \"factions\"\n")
        scenefile.write("num_factions {0}\n".format(len(factions_list)))
        for fac in factions_list:
            scenefile.write("faction \"{0}\"\n".format(fac["name"]))
            scenefile.write("    color vec3 {0:.6f} {1:.6f} {2:.6f}\n".format(fac["color"][0], fac["color"][1], fac["color"][2]))

    scenefile.write("section \"entities\"\n")
    scenefile.write("num_entities {0}\n".format(len(objects_list)))
    for obj in objects_list:
        pfobj_path = obj.pfobj_path
        if pfobj_path.startswith(MODELS_PREFIX_DIR):
            pfobj_path = pfobj_path[len(MODELS_PREFIX_DIR):]

        meta_dict = _meta_dict_for_object(obj, pfobj_path)
        if meta_dict is None:
            raise ValueError("Unable to find scene metadata for '{0}'".format(obj.pfobj_path))

        entries = [
            ("line", "   position vec3 {0:.6f} {1:.6f} {2:.6f}\n".format(obj.pos[0], obj.pos[1], obj.pos[2])),
            ("line", "   scale vec3 {0:.6f} {1:.6f} {2:.6f}\n".format(obj.scale[0], obj.scale[1], obj.scale[2])),
            ("line", "   rotation quat {0:.6f} {1:.6f} {2:.6f} {3:.6f}\n".format(obj.rotation[0], obj.rotation[1], obj.rotation[2], obj.rotation[3])),
            ("line", "   animated bool {0}\n".format(int(meta_dict["anim"]))),
            ("line", "   selectable bool {0}\n".format(int(obj.selectable))),
            ("line", "   static bool {0}\n".format(int(meta_dict["static"]))),
            ("line", "   collision bool {0}\n".format(int(meta_dict["collision"]))),
            ("line", "   faction_id int {0}\n".format(obj.faction_id)),
            ("line", "   selection_radius float {0}\n".format(obj.selection_radius)),
            ("line", "   vision_range float {0}\n".format(obj.vision_range)),
        ]

        uid = _safe_getattr(obj, "uid")
        if uid is not None:
            entries.append(("line", "   uid int {0}\n".format(int(uid))))

        if meta_dict.get("anim") and "idle" in meta_dict:
            entries.append(("line", "   idle_clip string {0}\n".format(meta_dict["idle"])))

        if "class" in meta_dict:
            entries.append(("line", "   class string {0}\n".format(meta_dict["class"])))

        if "construct_args" in meta_dict:
            entries.append(("constructor_arguments", meta_dict["construct_args"]))

        hp = _safe_getattr(obj, "hp")
        if hp is not None:
            entries.append(("line", "   hp int {0}\n".format(int(hp))))

        tags = tuple(_safe_getattr(obj, "tags") or ())
        if tags:
            entries.append(("tags", tags))

        num_atts = len(entries)
        scenefile.write("entity {0} {1} {2}\n".format(obj.name, MODELS_PREFIX_DIR + meta_dict["path"], num_atts))
        for entry_type, entry_value in entries:
            if entry_type == "line":
                scenefile.write(entry_value)
            elif entry_type == "constructor_arguments":
                _write_constructor_args(scenefile, entry_value)
            else:
                scenefile.write("   tags int {0}\n".format(len(entry_value)))
                for tag in entry_value:
                    scenefile.write("       tag \"{0}\"\n".format(tag))

    if regions_list:
        _write_region_section(scenefile, regions_list)

    if cameras_list:
        _write_camera_section(scenefile, cameras_list)

def dumps_scene_from_objects(objects_list, factions_list=None):
    buf = StringIO()
    _write_scene_to_stream(buf, objects_list, factions_list)
    return buf.getvalue()

def dumps_session_scene_from_state(objects_list, regions_list=None, cameras_list=None):
    buf = StringIO()
    _write_scene_to_stream(
        buf,
        objects_list,
        factions_list=[],
        regions_list=regions_list,
        cameras_list=cameras_list,
        include_factions=False,
    )
    return buf.getvalue()

def save_scene_from_objects(filename, objects_list, factions_list=None):
    with open(_scene_output_path(filename), "w") as scenefile:
        scenefile.write(dumps_scene_from_objects(objects_list, factions_list))

def save_scene(filename):
    editor_globals = sys.modules.get("globals") or sys.modules.get("editor.globals")
    if editor_globals is None:
        import globals as editor_globals
    save_scene_from_objects(filename, editor_globals.active_objects_list)
