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

class PerfStatsWindow(pf.Window):

    def __init__(self):
        super(PerfStatsWindow, self).__init__("Performance", (300, 25, 600, 195), 
            pf.NK_WINDOW_BORDER | pf.NK_WINDOW_MOVABLE | pf.NK_WINDOW_MINIMIZABLE | pf.NK_WINDOW_TITLE | pf.NK_WINDOW_NO_SCROLLBAR)
        self.tickindex = 0
        self.ticksum_ms = 0
        self.max_frame_latency = 0
        self.frame_times_ms = [0]*100

    def update(self):

        newtick = pf.prev_frame_ms()
        if newtick > self.max_frame_latency:
            self.max_frame_latency = newtick

        self.ticksum_ms -= self.frame_times_ms[self.tickindex]
        self.ticksum_ms += newtick
        self.frame_times_ms[self.tickindex] = newtick
        self.tickindex = (self.tickindex + 1) % len(self.frame_times_ms)

        self.layout_row_dynamic(100, 1)
        self.simple_chart(pf.NK_CHART_LINES, (0, 100), self.frame_times_ms)

        self.layout_row_dynamic(20, 1)
        avg_frame_latency_ms = float(self.ticksum_ms)/len(self.frame_times_ms)
        if avg_frame_latency_ms == 0.0:
            return
        
        fps = 1000/avg_frame_latency_ms
        self.label_colored_wrap("FPS: {0}".format(int(fps)), (255, 255, 255));

        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Max frame latency: {0} ms".format(self.max_frame_latency), (255, 255, 255))

