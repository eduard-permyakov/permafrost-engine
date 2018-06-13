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
    WINDOW_WIDTH = 400 
    WINDOW_HEIGHT = 150

    def __init__(self):
        resx, resy = pf.get_resolution()
        super(FileChooser, self).__init__("FileChooser", 
            (resx / 2 - FileChooser.WINDOW_WIDTH/ 2, resy / 2 - FileChooser.WINDOW_HEIGHT / 2, 
            FileChooser.WINDOW_WIDTH, FileChooser.WINDOW_HEIGHT), NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)
        import os 
        self.filestring = os.path.realpath(pf.get_basedir()) + "/assets/maps/"

    def update(self):

        self.layout_row_static(15, FileChooser.WINDOW_WIDTH, 1)
        self.layout_row_dynamic(30, 1)
        self.filestring = self.edit_string(NK_EDIT_SIMPLE, self.filestring)
        self.layout_row_static(15, FileChooser.WINDOW_WIDTH, 1)

        def on_okay():
            pf.global_event(EVENT_FILE_CHOOSER_OKAY, self.filestring)

        def on_cancel():
            pf.global_event(EVENT_FILE_CHOOSER_CANCEL, None)

        self.layout_row_dynamic(30, 2)
        self.button_label("OK", on_okay)
        self.button_label("Cancel", on_cancel)

