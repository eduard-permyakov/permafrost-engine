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
from constants import *
import map
import globals
import scene
import view_controller as vc
from math import cos, pi
import traceback
import faction

import views.file_chooser_window as fc

class MenuVC(vc.ViewController):

    def __init__(self, view):
        self.view = view
        self.fc = None

    ### NEW ###

    # We boradcast a 'TEARDOWN_BEGIN' event before loading the new game to 
    # give a chance for cleanup handlers to run
    def __on_old_game_teardown_begin(self, event):
        pf.global_event(EVENT_OLD_GAME_TEARDOWN_END, event)

    def __on_old_game_teardown_end(self, event):
        assert isinstance(event[0], map.Map)
        del globals.active_objects_list[:]
        del globals.factions_list[:]

        globals.active_map = event[0]
        pf.new_game_string(globals.active_map.pfmap_str())
        pf.set_minimap_position(UI_LEFT_PANE_WIDTH + MINIMAP_PX_WIDTH/cos(pi/4)/2 + 10, 
            pf.get_resolution()[1] - MINIMAP_PX_WIDTH/cos(pi/4)/2 - 10)
        self.view.hide()

        import os
        if event[1] is not None and os.path.isfile(event[1]):
            assert len(globals.active_objects_list) == 0 
            try:
                globals.active_objects_list = pf.load_scene(event[1])
                globals.scene_filename = event[1]
                for obj in globals.active_objects_list:
                    obj.selectable = True
                faction_descs = pf.get_factions_list() 
                globals.factions_list = [faction.Faction(desc["name"], desc["color"]) for desc in faction_descs]
            except:
                print("Failed to load scene! [{0}]".format(event[1]))

        if len(pf.get_factions_list()) == 0:
            pf.add_faction(DEFAULT_FACTION_NAME, DEFAULT_FACTION_COLOR)
            globals.factions_list = [faction.Faction(desc["name"], desc["color"]) for desc in pf.get_factions_list()]


    def __on_new(self, event):
        pf.global_event(EVENT_OLD_GAME_TEARDOWN_BEGIN, (map.Map(4, 4), None))

    ### LOAD ###

    def __on_load(self, event):
        assert self.fc is None
        self.fc = fc.FileChooser("Load")
        self.fc.show()
        self.deactivate()

        pf.register_event_handler(EVENT_FILE_CHOOSER_OKAY, MenuVC.__on_load_confirm, self)
        pf.register_event_handler(EVENT_FILE_CHOOSER_CANCEL, MenuVC.__on_load_cancel, self)

    def __on_load_confirm(self, event):
        pf.unregister_event_handler(EVENT_FILE_CHOOSER_OKAY, MenuVC.__on_load_confirm)
        pf.unregister_event_handler(EVENT_FILE_CHOOSER_CANCEL, MenuVC.__on_load_cancel)

        assert self.fc is not None
        self.fc.hide()
        self.fc = None
        self.activate()

        import os.path 
        if os.path.isfile(event[0]):
            try:
                new_map = map.Map.from_filepath(event[0])
            except:
                print("Failed to load map! [{0}]".format(event[0]))
            else:
                pf.global_event(EVENT_OLD_GAME_TEARDOWN_BEGIN, (new_map, event[1]))

    def __on_load_cancel(self, event):
        pf.unregister_event_handler(EVENT_FILE_CHOOSER_OKAY, MenuVC.__on_load_confirm)
        pf.unregister_event_handler(EVENT_FILE_CHOOSER_CANCEL, MenuVC.__on_load_cancel)

        assert self.fc is not None
        self.fc.hide()
        self.fc = None
        self.activate()

    ### SAVE AS ###

    def __on_save_as(self, event):
        assert self.fc is None
        self.fc = fc.FileChooser("Save")
        self.fc.show()
        self.deactivate()

        pf.register_event_handler(EVENT_FILE_CHOOSER_OKAY, MenuVC.__on_save_as_confirm, self)
        pf.register_event_handler(EVENT_FILE_CHOOSER_CANCEL, MenuVC.__on_save_as_cancel, self)

    def __on_save_as_confirm(self, event):
        pf.unregister_event_handler(EVENT_FILE_CHOOSER_OKAY, MenuVC.__on_save_as_confirm)
        pf.unregister_event_handler(EVENT_FILE_CHOOSER_CANCEL, MenuVC.__on_save_as_cancel)

        assert self.fc is not None
        self.fc.hide()
        self.fc = None
        self.activate()

        old_filename = globals.active_map.filename
        globals.active_map.filename = event[0]
        try: 
            globals.active_map.write_to_file()
            if event[1] is not None:
                scene.save_scene(event[1])
                globals.scene_filename = event[1]
        except:
            globals.active_map.filename = old_filename
            print("Failed to save map/scene!")
            traceback.print_exc() 
        else: 
            self.view.hide()

    def __on_save_as_cancel(self, event):
        pf.unregister_event_handler(EVENT_FILE_CHOOSER_OKAY, MenuVC.__on_save_as_confirm)
        pf.unregister_event_handler(EVENT_FILE_CHOOSER_CANCEL, MenuVC.__on_save_as_cancel)

        assert self.fc is not None
        self.fc.hide()
        self.fc = None
        self.activate()
        
    ### SAVE ###

    def __on_save(self, event):
        success = True
        if globals.active_map.filename is not None:
            try: 
                globals.active_map.write_to_file()
            except: 
                success = False
                print("Failed to save map!")
        else:
            self.__on_save_as(None)

        if globals.scene_filename is not None:
            try: 
                scene.save_scene(globals.scene_filename)
            except: 
                success = False
                print("Failed to save scene!")

        if success:
            self.view.hide()

    ### OTHER ###

    def __on_exit(self, event):
        pf.global_event(pf.SDL_QUIT, None)

    def __on_cancel(self, event):
        self.view.hide() 

    def activate(self):
        pf.register_event_handler(EVENT_MENU_NEW, MenuVC.__on_new, self)
        pf.register_event_handler(EVENT_MENU_LOAD, MenuVC.__on_load, self)
        pf.register_event_handler(EVENT_MENU_SAVE, MenuVC.__on_save, self)
        pf.register_event_handler(EVENT_MENU_SAVE_AS, MenuVC.__on_save_as, self)
        pf.register_event_handler(EVENT_MENU_EXIT, MenuVC.__on_exit, self)
        pf.register_event_handler(EVENT_MENU_CANCEL, MenuVC.__on_cancel, self)
        pf.register_event_handler(EVENT_OLD_GAME_TEARDOWN_BEGIN, MenuVC.__on_old_game_teardown_begin, self)
        pf.register_event_handler(EVENT_OLD_GAME_TEARDOWN_END, MenuVC.__on_old_game_teardown_end, self)

    def deactivate(self):
        pf.unregister_event_handler(EVENT_MENU_NEW, MenuVC.__on_new)
        pf.unregister_event_handler(EVENT_MENU_LOAD, MenuVC.__on_load)
        pf.unregister_event_handler(EVENT_MENU_SAVE, MenuVC.__on_save)
        pf.unregister_event_handler(EVENT_MENU_SAVE_AS, MenuVC.__on_save_as)
        pf.unregister_event_handler(EVENT_MENU_EXIT, MenuVC.__on_exit)
        pf.unregister_event_handler(EVENT_MENU_CANCEL, MenuVC.__on_cancel)
        pf.unregister_event_handler(EVENT_OLD_GAME_TEARDOWN_BEGIN, MenuVC.__on_old_game_teardown_begin)
        pf.unregister_event_handler(EVENT_OLD_GAME_TEARDOWN_END, MenuVC.__on_old_game_teardown_end)

