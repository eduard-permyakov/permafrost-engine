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

class FileChooser(pf.Window):
    WINDOW_WIDTH = 500 
    WINDOW_HEIGHT = 260

    def __init__(self, title):
        resx, resy = pf.get_resolution()
        super(FileChooser, self).__init__(title, 
            (resx / 2 - FileChooser.WINDOW_WIDTH/ 2, resy / 2 - FileChooser.WINDOW_HEIGHT / 2, 
            FileChooser.WINDOW_WIDTH, FileChooser.WINDOW_HEIGHT), pf.NK_WINDOW_BORDER | pf.NK_WINDOW_NO_SCROLLBAR | pf.NK_WINDOW_TITLE)
        import os 
        self.mapstring = os.path.realpath(pf.get_basedir()) + "/assets/maps/"
        self.scenestring = os.path.realpath(pf.get_basedir()) + "/assets/maps/"
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

