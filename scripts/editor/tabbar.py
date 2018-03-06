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

TAB_BAR_HEIGHT=40
TAB_BAR_NUM_COLS=3
TAB_BAR_COL_WIDTH=120

class TabBar(pf.Window):

    def __init__(self):
        resx, _ = pf.get_resolution()
        super(TabBar, self).__init__("", (0, 0, resx, TAB_BAR_HEIGHT), 
            NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)
        self.active_idx = 0
        self.labels = []
        self.child_windows = []

    def push_child(self, label, window):
        assert isinstance(label, basestring)
        assert isinstance(window, pf.Window)

        self.labels.append(label) 
        self.child_windows.append(window)

    def show_active(self):
        for idx, window in enumerate(self.child_windows):
            if idx == self.active_idx:
                window.show()
            else:
                window.hide() 

    def update(self):
        self.layout_row_begin(NK_STATIC, TAB_BAR_HEIGHT-10, TAB_BAR_NUM_COLS)
        for idx in range(0, len(self.child_windows)):

            def on_tab_click():
                self.active_idx = idx;
                self.show_active()

            self.layout_row_push(TAB_BAR_COL_WIDTH)
            self.button_label(self.labels[idx], on_tab_click)
        self.layout_row_end()

    def show(self):
        super(TabBar, self).show()
        self.show_active()

# The following is only temporary - for testing 

class DemoWindow(pf.Window):

    def __init__(self):
        super(DemoWindow, self).__init__("Permafrost Engine Demo", (25, 50, 230, 200), 
            NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_MINIMIZABLE | NK_WINDOW_TITLE)

    def update(self):

        def on_button_click():
            pf.global_event(EVENT_SDL_QUIT, None)

        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Demo Hotkeys:", (255, 255, 255));

        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("    C - Toggle Camera", (255, 255, 255));

        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("    V - Toggle Animation", (255, 255, 255));

        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("    ESC - Exit Demo", (255, 255, 255));

        self.layout_row_dynamic(30, 1)
        self.button_label("Exit Demo", on_button_click)

class PerfStatsWindow(pf.Window):

    def __init__(self):
        super(PerfStatsWindow, self).__init__("Performance", (280, 50, 600, 200), 
            NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_MINIMIZABLE | NK_WINDOW_TITLE | NK_WINDOW_NO_SCROLLBAR)
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
        self.simple_chart(NK_CHART_LINES, (0, 100), self.frame_times_ms)

        self.layout_row_dynamic(20, 1)
        avg_frame_latency_ms = float(self.ticksum_ms)/len(self.frame_times_ms)
        fps = 1000/avg_frame_latency_ms
        self.label_colored_wrap("FPS: {0}".format(int(fps)), (255, 255, 255));

        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Max frame latency: {0} ms".format(self.max_frame_latency), (255, 255, 255))

            

