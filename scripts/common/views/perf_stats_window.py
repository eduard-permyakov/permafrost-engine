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

    WIDTH = 600
    HEIGHT = 440

    def __init__(self):
        vresx, vresy = (1920, 1080)
        super(PerfStatsWindow, self).__init__("Performance", 
            (vresx/2 - PerfStatsWindow.WIDTH/2, vresy/2 - PerfStatsWindow.HEIGHT/2, PerfStatsWindow.WIDTH, PerfStatsWindow.HEIGHT), 
            pf.NK_WINDOW_BORDER | pf.NK_WINDOW_MOVABLE | pf.NK_WINDOW_MINIMIZABLE | pf.NK_WINDOW_TITLE | 
            pf.NK_WINDOW_CLOSABLE | pf.NK_WINDOW_NO_SCROLLBAR, (vresx, vresy))
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
        self.label_colored_wrap("FPS: {0}".format(int(fps)), (255, 255, 0));

        self.layout_row_dynamic(20, 2)
        self.label_colored_wrap("Avg. Frame Latency: {0:.1f} ms".format(avg_frame_latency_ms), (255, 255, 0));

        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Max Frame Latency: {0} ms".format(self.max_frame_latency), (255, 255, 0))

        self.layout_row_dynamic(10, 1)
        render_info = pf.get_render_info()

        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Renderer: %s" % render_info["renderer"], (255, 255, 255))
        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Version: %s" % render_info["version"], (255, 255, 255))
        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Shading Language Version: %s" % render_info["shading_language_version"], (255, 255, 255))
        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Vendor: %s" % render_info["vendor"], (255, 255, 255))

        self.layout_row_dynamic(10, 1)
        nav_stats = pf.get_nav_perfstats()

        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("[LOS Field Cache]   Used: {used:04d}/{cap:04d}  Hit Rate: {hr:02.03f} Invalidated: {inv:04d}" \
            .format(used=nav_stats["los_used"], cap=nav_stats["los_max"], 
            hr=nav_stats["los_hit_rate"], inv=nav_stats["los_invalidated"]), \
            (0, 255, 0))

        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("[Flow Field Cache]   Used: {used:04d}/{cap:04d}   Hit Rate: {hr:02.03f} Invalidated: {inv:04d}" \
            .format(used=nav_stats["flow_used"], cap=nav_stats["flow_max"], 
            hr=nav_stats["flow_hit_rate"], inv=nav_stats["flow_invalidated"]), \
            (0, 255, 0))

        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("[Dest:Field Mapping Cache] Used: {used:04d}/{cap:04d}   Hit Rate: {hr:02.03f}" \
            .format(used=nav_stats["ffid_used"], cap=nav_stats["ffid_max"], hr=nav_stats["ffid_hit_rate"]), \
            (0, 255, 0))

        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("[Grid Path Cache] Used: {used:04d}/{cap:04d}   Hit Rate: {hr:02.03f}" \
            .format(used=nav_stats["grid_path_used"], cap=nav_stats["grid_path_max"], hr=nav_stats["grid_path_hit_rate"]), \
            (0, 255, 0))

