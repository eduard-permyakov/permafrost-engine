#
#  This file is part of Permafrost Engine. 
#  Copyright (C) 2019 Eduard Permyakov 
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
import view_controller as vc

class VideoSettingsVC(vc.ViewController):

    def __init__(self, view):
        self.view = view
        self.view.dirty = False
        self.__og_res_idx = self.view.res_idx
        self.__og_mode_idx = self.view.mode_idx
        self.__load_selection()

    def __load_selection(self):
        try:
            res_saved = pf.settings_get("pf.video.resolution")
            for i, cand in enumerate(self.view.res_opts):
                if cand == res_saved:
                    self.view.res_idx = i
                    self.__og_res_idx = i
                    break

            dm_saved = pf.settings_get("pf.video.display_mode")
            for i, cand in enumerate(self.view.mode_opts):
                if cand == dm_saved:
                    self.view.mode_idx = i
                    self.__og_mode_idx = i
                    break

        except Exception as e:
            print("Could not load settings:" + str(e))
            raise

    def __update_dirty_flag(self):
        if self.view.res_idx != self.__og_res_idx \
        or self.view.mode_idx != self.__og_mode_idx:
            self.view.dirty = True
        else:
            self.view.dirty = False

    def __on_settings_apply(self, event):
        if self.view.res_idx != self.__og_res_idx:
            try:
                pf.settings_set("pf.video.resolution", self.view.res_opts[self.view.res_idx])
                self.__og_res_idx = self.view.res_idx
            except Exception as e:
                print("Could not set pf.video.resolution:" + str(e))

        if self.view.mode_idx != self.__og_mode_idx:
            try:
                pf.settings_set("pf.video.display_mode", self.view.mode_opts[self.view.mode_idx])
                self.__og_mode_idx = self.view.mode_idx
            except Exception as e:
                print("Could not set pf.video.display_mode:" + str(e))

        self.__update_dirty_flag()

    def __on_resolution_changed(self, event):
        self.__update_dirty_flag() 

    def __on_winmode_changed(self, event):
        self.__update_dirty_flag()

    def activate(self):
        pf.register_event_handler(EVENT_SETTINGS_APPLY, VideoSettingsVC.__on_settings_apply, self)
        pf.register_event_handler(EVENT_RES_SETTING_CHANGED, VideoSettingsVC.__on_resolution_changed, self)
        pf.register_event_handler(EVENT_WINMODE_SETTING_CHANGED, VideoSettingsVC.__on_winmode_changed, self)

    def deactivate(self):
        pf.unregister_event_handler(EVENT_WINMODE_SETTING_CHANGED, VideoSettingsVC.__on_winmode_changed)
        pf.unregister_event_handler(EVENT_RES_SETTING_CHANGED, VideoSettingsVC.__on_resolution_changed)
        pf.unregister_event_handler(EVENT_SETTINGS_APPLY, VideoSettingsVC.__on_settings_apply)

