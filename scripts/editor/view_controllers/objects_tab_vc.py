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

import pf
from constants import *
import mouse_events
import globals
import common.view_controllers.view_controller as vc
import scene


class ObjectsVC(vc.ViewController):

    def __init__(self, view):
        self.view = view
        self.view.objects_list = [(o["path"]).split("/")[-1] for o in scene.OBJECTS_LIST]
        assert(len(scene.OBJECTS_LIST) > 0)
        self.current_object = None
        self.view.mode = self.view.OBJECTS_MODE_PLACE
        self.right_mousebutton_state = pf.SDL_RELEASED

    def __object_at_index(self, index):
        split_path = (MODELS_PREFIX_DIR + scene.OBJECTS_LIST[index]["path"]).split("/")
        pfobj_dir = '/'.join(split_path[:-1])
        pfobj_filename = split_path[-1]
        if scene.OBJECTS_LIST[index]["anim"]:
            ret = pf.AnimEntity(pfobj_dir, pfobj_filename, pfobj_filename.split(".")[0], idle_clip=scene.OBJECTS_LIST[index]["idle"])
        else:
            ret = pf.Entity(pfobj_dir, pfobj_filename, pfobj_filename.split(".")[0])
        ret.scale = scene.OBJECTS_LIST[index]["scale"]
        ret.selection_radius = scene.OBJECTS_LIST[index]["sel_radius"]
        ret.selectable = True
        return ret

    def __set_selection_for_mode(self):
        if self.view.mode == self.view.OBJECTS_MODE_SELECT:
            pf.enable_unit_selection()
        else:
            pf.disable_unit_selection()

    def __on_selected_object_changed(self, event):
        self.object_index = event
        pos = self.current_object.pos
        if self.view.mode == self.view.OBJECTS_MODE_PLACE:
            self.current_object = self.__object_at_index(event)
            self.current_object.pos = pos

    def __on_mousemove(self, event):
        if self.current_object and pf.map_pos_under_cursor():
            self.current_object.pos = pf.map_pos_under_cursor()
        if self.right_mousebutton_state == pf.SDL_PRESSED:
            sel = pf.get_unit_selection()
            if len(sel) == 1:
                sel[0].pos = pf.map_pos_under_cursor()

    def __on_click(self, event):
        if not mouse_events.mouse_over_map:
            return
        if pf.map_pos_under_cursor() is None:
            return
        if event[0] == pf.SDL_BUTTON_LEFT:
            if self.current_object:
                self.current_object.faction_id = pf.get_factions_list()[self.view.selected_faction_idx]["id"]
                globals.active_objects_list.append(self.current_object)
                self.current_object = self.__object_at_index(self.view.selected_object_idx)
                self.current_object.pos = pf.map_pos_under_cursor()
        elif event[0] == pf.SDL_BUTTON_RIGHT:
            self.right_mousebutton_state = event[1]
            sel = pf.get_unit_selection()
            if len(sel) == 1:
                sel[0].pos = pf.map_pos_under_cursor()

    def __on_release(self, event):
        if event[0] == pf.SDL_BUTTON_RIGHT:
            self.right_mousebutton_state = event[1]

    def __on_mode_changed(self, event):
        self.__set_selection_for_mode()
        if self.view.mode == self.view.OBJECTS_MODE_PLACE:
            pf.clear_unit_selection()
            self.current_object = self.__object_at_index(self.view.selected_object_idx)
            if pf.map_pos_under_cursor():
                self.current_object.pos = pf.map_pos_under_cursor()
        elif self.view.mode == self.view.OBJECTS_MODE_SELECT:
            self.current_object = None

    def __on_selected_unit_picked(self, event):
        assert isinstance(event, pf.Entity)
        pf.clear_unit_selection()
        event.select()

    def __on_delete_selection(self, event):
        sel_obj_list = pf.get_unit_selection()
        for obj in sel_obj_list:
            globals.active_objects_list.remove(obj)

    def __on_mousewheel(self, event):
        CCW_ROT_5DEG = (0.0,  0.0436194, 0.0, 0.9990482)
        CW_ROT_5DEG  = (0.0, -0.0436194, 0.0, 0.9990482)
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
        pf.register_event_handler(pf.SDL_MOUSEMOTION, ObjectsVC.__on_mousemove, self)
        pf.register_event_handler(pf.SDL_MOUSEBUTTONDOWN, ObjectsVC.__on_click, self)
        pf.register_event_handler(pf.SDL_MOUSEBUTTONUP, ObjectsVC.__on_release, self)
        pf.register_event_handler(pf.SDL_MOUSEWHEEL, ObjectsVC.__on_mousewheel, self)
        pf.register_event_handler(EVENT_OBJECT_SELECTION_CHANGED, ObjectsVC.__on_selected_object_changed, self)
        pf.register_event_handler(EVENT_OBJECTS_TAB_MODE_CHANGED, ObjectsVC.__on_mode_changed, self)
        pf.register_event_handler(EVENT_OBJECT_SELECTED_UNIT_PICKED, ObjectsVC.__on_selected_unit_picked, self)
        pf.register_event_handler(EVENT_OBJECT_DELETE_SELECTION, ObjectsVC.__on_delete_selection, self)
        self.__set_selection_for_mode()
        if self.view.mode == self.view.OBJECTS_MODE_PLACE:
            self.current_object = self.__object_at_index(self.view.selected_object_idx)
        else:
            self.current_object = None
        # Handle the case where we deleted some factions so our view's index is now stale
        factions_list = pf.get_factions_list()
        if self.view.selected_faction_idx >= len(factions_list):
            self.view.selected_faction_idx = len(factions_list)-1

    def deactivate(self):
        pf.unregister_event_handler(pf.SDL_MOUSEMOTION, ObjectsVC.__on_mousemove)
        pf.unregister_event_handler(pf.SDL_MOUSEBUTTONDOWN, ObjectsVC.__on_click)
        pf.unregister_event_handler(pf.SDL_MOUSEBUTTONUP, ObjectsVC.__on_release)
        pf.unregister_event_handler(pf.SDL_MOUSEWHEEL, ObjectsVC.__on_mousewheel)
        pf.unregister_event_handler(EVENT_OBJECT_SELECTION_CHANGED, ObjectsVC.__on_selected_object_changed)
        pf.unregister_event_handler(EVENT_OBJECTS_TAB_MODE_CHANGED, ObjectsVC.__on_mode_changed)
        pf.unregister_event_handler(EVENT_OBJECT_SELECTED_UNIT_PICKED, ObjectsVC.__on_selected_unit_picked)
        pf.unregister_event_handler(EVENT_OBJECT_DELETE_SELECTION, ObjectsVC.__on_delete_selection)
        pf.clear_unit_selection()
        pf.disable_unit_selection()
        self.current_object = None

