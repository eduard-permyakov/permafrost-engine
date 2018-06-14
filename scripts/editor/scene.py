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

import globals

def save_scene(filename):
    with open(filename, "w") as scenefile:
        scenefile.write("num_objects {0}\n".format(len(globals.active_objects_list)))
        for obj in globals.active_objects_list:
            num_atts = 5
            if "idle" in obj.meta_dict:
                num_atts += 1
            if "sel_radius" in obj.meta_dict:
                num_atts += 1
            if "class" in obj.meta_dict:
                num_atts += 1
            if "construct_args" in obj.meta_dict:
                num_atts += (1 + len(obj.meta_dict["construct_args"]))
            scenefile.write("entity {0} {1} {2}\n".format(obj.name, obj.meta_dict["path"], num_atts))
            scenefile.write("   position {0:.6f} {1:.6f} {2:.6f}\n".format(obj.pos[0], obj.pos[1], obj.pos[2]))
            scenefile.write("   scale {0:.6f} {1:.6f} {2:.6f}\n".format(obj.scale[0], obj.scale[1], obj.scale[2]))
            scenefile.write("   rotation {0:.6f} {1:.6f} {2:.6f} {3:.6f}\n".format(obj.rotation[0], obj.rotation[1], obj.rotation[2], obj.rotation[3]))
            scenefile.write("   animated {0}\n".format(int(obj.meta_dict["anim"])))
            scenefile.write("   selectable {0}\n".format(int(obj.selectable)))
            if "idle" in obj.meta_dict:
                scenefile.write("   idle_clip {0}\n".format(obj.meta_dict["idle"]))
            if "sel_radius" in obj.meta_dict:
                scenefile.write("   selection_radius {0}\n".format(obj.selection_radius))
            if "class" in obj.meta_dict:
                scenefile.write("   class {0}\n".format(obj.meta_dict["class"]))
            if "construct_args" in obj.meta_dict:
                scenefile.write("   constructor_arguments {0}\n".format(len(obj.meta_dict["construct_args"])))
                for arg in obj.meta_dict["construct_args"]:
                    if isinstance(arg, int):
                        type = "int" 
                    elif isinstance(arg, float):
                        type = "float"
                    elif isinstance(arg, basestring):
                        type = "string"
                    else:
                        raise ValueError("Constructor arguments must be of type int, float, or basestring")
                    scenefile.write("       {0} {1}\n".format(type, arg))

