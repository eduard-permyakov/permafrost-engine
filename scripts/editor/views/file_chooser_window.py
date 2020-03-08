#
#  This file is part of Permafrost Engine. 
#  Copyright (C) 2018-2020 Eduard Permyakov 
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

class FileChooser(pf.Window):
    WINDOW_WIDTH = 500 
    WINDOW_HEIGHT = 260

    def __init__(self, title):
        vresx, vresy = (1920, 1080)
        super(FileChooser, self).__init__(title, 
            (vresx / 2 - FileChooser.WINDOW_WIDTH/ 2, vresy / 2 - FileChooser.WINDOW_HEIGHT / 2, 
            FileChooser.WINDOW_WIDTH, FileChooser.WINDOW_HEIGHT), pf.NK_WINDOW_BORDER | pf.NK_WINDOW_NO_SCROLLBAR | pf.NK_WINDOW_TITLE, (vresx, vresy))
        self.mapstring = pf.get_basedir() + "assets/maps/"
        self.scenestring = pf.get_basedir() + "assets/maps/"
        self.scene_flag = False
        self.title = title

    def update(self):

        self.layout_row_static(15, FileChooser.WINDOW_WIDTH, 1)
        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap(self.title + " Map:", (175, 175, 175))
        self.layout_row_dynamic(30, 1)
        self.mapstring = self.edit_string(pf.NK_EDIT_SIMPLE, self.mapstring)

        def on_okay():
            scenepath = self.scenestring if self.scene_flag else None
            pf.global_event(EVENT_FILE_CHOOSER_OKAY, (self.mapstring, scenepath))

        def on_cancel():
            pf.global_event(EVENT_FILE_CHOOSER_CANCEL, None)

        self.layout_row_dynamic(30, 1)
        self.scene_flag = self.checkbox(self.title + " Scene:", self.scene_flag)
        self.layout_row_dynamic(30, 1)
        if self.scene_flag:
            self.scenestring = self.edit_string(pf.NK_EDIT_SIMPLE, self.scenestring)
        self.layout_row_static(15, FileChooser.WINDOW_WIDTH, 1)

        self.layout_row_dynamic(30, 2)
        self.button_label("OK", on_okay)
        self.button_label("Cancel", on_cancel)

