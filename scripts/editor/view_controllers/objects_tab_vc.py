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
import os
import mouse_events
import globals
import view_controller as vc


def listdir_fullpath(dir):
    return [os.path.join(dir, f) for f in os.listdir(dir)]

class ObjectsVC(vc.ViewController):

    OBJECTS_LIST = [
        { "path" : "sinbad/Sinbad.pfobj",            "anim" : True,      "idle" : "IdleBase",    "scale" : [1.2,  1.2,  1.2], "sel_radius" : 3.25 },
        { "path" : "knight/knight.pfobj",            "anim" : True,      "idle" : "Idle",        "scale" : [0.8,  0.8,  0.8], "sel_radius" : 3.25 },
        { "path" : "mage/mage.pfobj",                "anim" : True,      "idle" : "Idle",        "scale" : [0.7,  0.7,  0.7], "sel_radius" : 4.25 },
        { "path" : "oak_tree/oak_tree.pfobj",        "anim" : False,                             "scale" : [1.6,  1.6,  1.6], "sel_radius" : 5.25 },
        { "path" : "oak_tree/oak_leafless.pfobj",    "anim" : False,                             "scale" : [1.6,  1.6,  1.6], "sel_radius" : 5.25 },
    ]

    def __init__(self, view):
        self.view = view
        self.view.objects_list = [os.path.split(o["path"])[-1] for o in self.OBJECTS_LIST]
        assert(len(self.OBJECTS_LIST) > 0)
        self.view.selected_height_idx = 0
        self.current_object = self.__object_at_index(self.view.selected_height_idx)
        self.view.mode = self.view.OBJECTS_MODE_PLACE
        self.right_mousebutton_state = SDL_RELEASED

    def __object_at_index(self, index):
        split_path = os.path.split("assets/models/" + self.OBJECTS_LIST[index]["path"])
        if self.OBJECTS_LIST[index]["anim"]:
            ret = pf.AnimEntity(os.path.join(split_path[:-1])[0], split_path[-1], split_path[-1].split(".")[0], self.OBJECTS_LIST[index]["idle"])
        else:
            ret = pf.Entity(os.path.join(split_path[:-1])[0], split_path[-1], split_path[-1].split(".")[0])
        ret.scale = self.OBJECTS_LIST[index]["scale"]
        ret.selection_radius = self.OBJECTS_LIST[index]["sel_radius"]
        ret.selectable = True
        return ret

    def __set_selection_for_mode(self):
        if self.view.mode == self.view.OBJECTS_MODE_SELECT:
            pf.enable_unit_selection()
        else:
            pf.disable_unit_selection()

    def __on_selected_object_changed(self, event):
        self.object_index = event
        if self.view.mode == self.view.OBJECTS_MODE_PLACE:
            if self.current_object is not None:
                del self.current_object
            self.current_object = self.__object_at_index(event)
            if mouse_events.mouse_over_map:
                self.current_object.activate()

    def __on_mousemove(self, event):
        if self.current_object and pf.map_pos_under_cursor():
            self.current_object.pos = pf.map_pos_under_cursor()
        if self.right_mousebutton_state == SDL_PRESSED:
            sel = pf.get_unit_selection()
            if len(sel) == 1:
                sel[0].pos = pf.map_pos_under_cursor()

    def __on_mouse_enter_map(self, event):
        if self.current_object: 
            self.current_object.activate()

    def __on_mouse_exit_map(self, event):
        if self.current_object: 
            self.current_object.deactivate()

    def __on_click(self, event):
        if not mouse_events.mouse_over_map:
            return
        if event[0] == SDL_BUTTON_LEFT:
            if self.current_object:
                globals.active_objects_list.append(self.current_object)
                self.current_object = self.__object_at_index(self.view.selected_object_idx)
                if mouse_events.mouse_over_map:
                    self.current_object.pos = pf.map_pos_under_cursor()
                    self.current_object.activate()
        elif event[0] == SDL_BUTTON_RIGHT:
            self.right_mousebutton_state = event[1]
            sel = pf.get_unit_selection()
            if len(sel) == 1:
                sel[0].pos = pf.map_pos_under_cursor()

    def __on_release(self, event):
        if event[0] == SDL_BUTTON_RIGHT:
            self.right_mousebutton_state = event[1]

    def __on_mode_changed(self, event):
        self.__set_selection_for_mode()
        if self.view.mode == self.view.OBJECTS_MODE_PLACE:
            pf.clear_unit_selection()
            self.current_object = self.__object_at_index(self.view.selected_object_idx)
            if mouse_events.mouse_over_map:
                self.current_object.pos = pf.map_pos_under_cursor()
                self.current_object.activate()
        elif self.view.mode == self.view.OBJECTS_MODE_SELECT:
            del self.current_object
            self.current_object = None

    def __on_new_game(self, event):
        if self.view.mode == self.view.OBJECTS_MODE_PLACE:
            self.current_object = self.__object_at_index(self.view.selected_object_idx)
            self.current_object.pos = pf.map_pos_under_cursor()
            self.current_object.activate()

    def __on_selected_unit_picked(self, event):
        assert isinstance(event, pf.Entity)
        pf.clear_unit_selection()
        event.select()

    def __on_delete_selection(self, event):
        sel_obj_list = pf.get_unit_selection()
        for obj in sel_obj_list:
            globals.active_objects_list.remove(obj)
        del sel_obj_list[:]

    def __on_mousewheel(self, event):
        CCW_ROT_5DEG = [0.0,  0.0436194, 0.0, 0.9990482]
        CW_ROT_5DEG  = [0.0, -0.0436194, 0.0, 0.9990482]
        if self.view.mode == self.view.OBJECTS_MODE_SELECT:
            sel_obj_list = pf.get_unit_selection()
            if len(sel_obj_list) != 1:
                return
            obj = sel_obj_list[0]
        else:
            obj = self.current_object
        if event[1] > 0:
            obj.rotation = pf.multiply_quaternions(obj.rotation, CCW_ROT_5DEG)
        else:
            obj.rotation = pf.multiply_quaternions(obj.rotation, CW_ROT_5DEG)

    def activate(self):
        pf.register_event_handler(EVENT_OBJECT_SELECTION_CHANGED, ObjectsVC.__on_selected_object_changed, self)
        pf.register_event_handler(EVENT_MOUSE_ENTERED_MAP, ObjectsVC.__on_mouse_enter_map, self)
        pf.register_event_handler(EVENT_MOUSE_EXITED_MAP, ObjectsVC.__on_mouse_exit_map, self)
        pf.register_event_handler(EVENT_SDL_MOUSEMOTION, ObjectsVC.__on_mousemove, self)
        pf.register_event_handler(EVENT_SDL_MOUSEBUTTONDOWN, ObjectsVC.__on_click, self)
        pf.register_event_handler(EVENT_SDL_MOUSEBUTTONUP, ObjectsVC.__on_release, self)
        pf.register_event_handler(EVENT_OBJECTS_TAB_MODE_CHANGED, ObjectsVC.__on_mode_changed, self)
        pf.register_event_handler(EVENT_NEW_GAME, ObjectsVC.__on_new_game, self)
        pf.register_event_handler(EVENT_OBJECT_SELECTED_UNIT_PICKED, ObjectsVC.__on_selected_unit_picked, self)
        pf.register_event_handler(EVENT_OBJECT_DELETE_SELECTION, ObjectsVC.__on_delete_selection, self)
        pf.register_event_handler(EVENT_SDL_MOUSEWHEEL, ObjectsVC.__on_mousewheel, self)
        self.__set_selection_for_mode()

    def deactivate(self):
        pf.unregister_event_handler(EVENT_OBJECT_SELECTION_CHANGED, ObjectsVC.__on_selected_object_changed)
        pf.unregister_event_handler(EVENT_MOUSE_ENTERED_MAP, ObjectsVC.__on_mouse_enter_map)
        pf.unregister_event_handler(EVENT_MOUSE_EXITED_MAP, ObjectsVC.__on_mouse_exit_map)
        pf.unregister_event_handler(EVENT_SDL_MOUSEMOTION, ObjectsVC.__on_mousemove)
        pf.unregister_event_handler(EVENT_SDL_MOUSEBUTTONDOWN, ObjectsVC.__on_click)
        pf.unregister_event_handler(EVENT_SDL_MOUSEBUTTONUP, ObjectsVC.__on_release)
        pf.unregister_event_handler(EVENT_OBJECTS_TAB_MODE_CHANGED, ObjectsVC.__on_mode_changed)
        pf.unregister_event_handler(EVENT_NEW_GAME, ObjectsVC.__on_new_game)
        pf.unregister_event_handler(EVENT_OBJECT_SELECTED_UNIT_PICKED, ObjectsVC.__on_selected_unit_picked)
        pf.unregister_event_handler(EVENT_OBJECT_DELETE_SELECTION, ObjectsVC.__on_delete_selection)
        pf.unregister_event_handler(EVENT_SDL_MOUSEWHEEL, ObjectsVC.__on_mousewheel)
        pf.clear_unit_selection()
        pf.disable_unit_selection()

