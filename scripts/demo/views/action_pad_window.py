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

NUM_ROWS = 3
NUM_COLS = 4
BUTTON_WIDTH = 75
BUTTON_PADDING = 6

class ActionPadWindow(pf.Window):

    def __init__(self):
        # Each button also has a 1px border around it, hence the (NUM_COLS*2)
        width = BUTTON_WIDTH * NUM_COLS + (NUM_COLS-1) * BUTTON_PADDING +(NUM_COLS*2) + 4
        height = BUTTON_WIDTH * NUM_ROWS + (NUM_ROWS-1) * BUTTON_PADDING +(NUM_ROWS*2) + 6
        resx, resy = pf.get_resolution()
        super(ActionPadWindow, self).__init__("ActionPad", (resx - width - 10, resy - height - 10, 
            width, height), pf.NK_WINDOW_BORDER | pf.NK_WINDOW_NO_SCROLLBAR)
        self.spacing = (float(BUTTON_PADDING), float(BUTTON_PADDING))
        self.padding = (2.0, 4.0)

    def update(self):
        og_bpad, og_bround = pf.button_style.padding, pf.button_style.rounding

        pf.button_style.padding = (0.0, 0.0)
        pf.button_style.rounding = 0.0

        for r in range(0, NUM_ROWS):
            self.layout_row_static(BUTTON_WIDTH, BUTTON_WIDTH, NUM_COLS)

            for c in range(0, NUM_COLS):
                self.button_label("", lambda: None)

        pf.button_style.padding, pf.button_style.rounding = og_bpad, og_bround

