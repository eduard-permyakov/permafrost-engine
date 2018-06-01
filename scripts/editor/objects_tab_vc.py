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
from constants import *
import os
import mouse_events


def listdir_fullpath(dir):
    return [os.path.join(dir, f) for f in os.listdir(dir)]

class ObjectsVC(ui.ViewController):

    OBJECTS_LIST = [
        {"path" : "sinbad/Sinbad.pfobj",            "anim" : True,      "idle" : "IdleBase"     },
        {"path" : "knight/knight.pfobj",            "anim" : True,      "idle" : "Idle"         },
        {"path" : "mage/mage.pfobj",                "anim" : True,      "idle" : "Idle"         },
        {"path" : "oak_tree/oak_tree.pfobj",        "anim" : False                              },
        {"path" : "oak_tree/oak_leafless.pfobj",    "anim" : False                              },
    ]

    def __init__(self, view):
        self.view = view
        self.view.objects_list = [os.path.split(o["path"])[-1] for o in self.OBJECTS_LIST]
        if len(self.OBJECTS_LIST) > 0:
            self.current_object = self.__object_at_index(0)
        else:
            self.current_object = None

    def __object_at_index(self, index):
        split_path = os.path.split("assets/models/" + self.OBJECTS_LIST[index]["path"])
        if self.OBJECTS_LIST[index]["anim"]:
            return pf.AnimEntity(os.path.join(split_path[:-1])[0], split_path[-1], split_path[-1].split(".")[0], self.OBJECTS_LIST[index]["idle"])
        else:
            return pf.Entity(os.path.join(split_path[:-1])[0], split_path[-1], split_path[-1].split(".")[0])

    def __on_selected_object_changed(self, event):
        if self.current_object is not None:
            self.current_object.deactivate()
            del self.current_object
        self.current_object = self.__object_at_index(event)
        if mouse_events.mouse_over_map:
            self.current_object.activate()

    def __on_mousemove(self, event):
        if self.current_object and pf.map_pos_under_cursor():
            self.current_object.pos = pf.map_pos_under_cursor()

    def __on_mouse_enter_map(self, event):
        if self.current_object: 
            self.current_object.activate()

    def __on_mouse_exit_map(self, event):
        if self.current_object: 
            self.current_object.deactivate()
        
    def activate(self):
        pf.register_event_handler(EVENT_OBJECT_SELECTION_CHANGED, ObjectsVC.__on_selected_object_changed, self)
        pf.register_event_handler(EVENT_MOUSE_ENTERED_MAP, ObjectsVC.__on_mouse_enter_map, self)
        pf.register_event_handler(EVENT_MOUSE_EXITED_MAP, ObjectsVC.__on_mouse_exit_map, self)
        pf.register_event_handler(EVENT_SDL_MOUSEMOTION, ObjectsVC.__on_mousemove, self)

    def deactivate(self):
        pf.unregister_event_handler(EVENT_OBJECT_SELECTION_CHANGED, ObjectsVC.__on_selected_object_changed)
        pf.unregister_event_handler(EVENT_MOUSE_ENTERED_MAP, ObjectsVC.__on_mouse_enter_map)
        pf.unregister_event_handler(EVENT_MOUSE_EXITED_MAP, ObjectsVC.__on_mouse_exit_map)
        pf.unregister_event_handler(EVENT_SDL_MOUSEMOTION, ObjectsVC.__on_mousemove)

