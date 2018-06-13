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
import map
import globals
import view_controller as vc
from math import cos, pi

import views.file_chooser_window as fc

class MenuVC(vc.ViewController):

    def __init__(self, view):
        self.view = view
        self.fc = None

    ### NEW ###

    def __on_new(self, event):
        del globals.active_objects_list[:]
        globals.active_map = map.Map(4, 4)
        pf.new_game_string(globals.active_map.pfmap_str())
        pf.set_minimap_position(UI_LEFT_PANE_WIDTH + MINIMAP_PX_WIDTH/cos(pi/4)/2 + 10, 
            pf.get_resolution()[1] - MINIMAP_PX_WIDTH/cos(pi/4)/2 - 10)
        self.view.hide()

    ### LOAD ###

    def __on_load(self, event):
        assert self.fc is None
        self.fc = fc.FileChooser()
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
        if os.path.isfile(event):
            new_map = map.Map.from_filepath(event)
            if new_map is not None:
                del globals.active_objects_list[:]
                globals.active_map = new_map
                pf.new_game_string(globals.active_map.pfmap_str())
                pf.set_minimap_position(UI_LEFT_PANE_WIDTH + MINIMAP_PX_WIDTH/cos(pi/4)/2 + 10, 
                    pf.get_resolution()[1] - MINIMAP_PX_WIDTH/cos(pi/4)/2 - 10)
                self.view.hide()

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
        self.fc = fc.FileChooser()
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

        globals.active_map.filename = event
        try: globals.active_map.write_to_file()
        except: pass
        else: self.view.hide()

    def __on_save_as_cancel(self, event):
        pf.unregister_event_handler(EVENT_FILE_CHOOSER_OKAY, MenuVC.__on_save_as_confirm)
        pf.unregister_event_handler(EVENT_FILE_CHOOSER_CANCEL, MenuVC.__on_save_as_cancel)

        assert self.fc is not None
        self.fc.hide()
        self.fc = None
        self.activate()
        
    ### SAVE ###

    def __on_save(self, event):
        if globals.active_map.filename is not None:
            try: globals.active_map.write_to_file()
            except: pass
            else: self.view.hide()
        else:
            self.__on_save_as(None)

    ### OTHER ###

    def __on_exit(self, event):
        pf.global_event(EVENT_SDL_QUIT, None)

    def __on_cancel(self, event):
        self.view.hide() 

    def activate(self):
        pf.register_event_handler(EVENT_MENU_NEW, MenuVC.__on_new, self)
        pf.register_event_handler(EVENT_MENU_LOAD, MenuVC.__on_load, self)
        pf.register_event_handler(EVENT_MENU_SAVE, MenuVC.__on_save, self)
        pf.register_event_handler(EVENT_MENU_SAVE_AS, MenuVC.__on_save_as, self)
        pf.register_event_handler(EVENT_MENU_EXIT, MenuVC.__on_exit, self)
        pf.register_event_handler(EVENT_MENU_CANCEL, MenuVC.__on_cancel, self)

    def deactivate(self):
        pf.unregister_event_handler(EVENT_MENU_NEW, MenuVC.__on_new)
        pf.unregister_event_handler(EVENT_MENU_LOAD, MenuVC.__on_load)
        pf.unregister_event_handler(EVENT_MENU_SAVE, MenuVC.__on_save)
        pf.unregister_event_handler(EVENT_MENU_SAVE_AS, MenuVC.__on_save_as)
        pf.unregister_event_handler(EVENT_MENU_EXIT, MenuVC.__on_exit)
        pf.unregister_event_handler(EVENT_MENU_CANCEL, MenuVC.__on_cancel)

