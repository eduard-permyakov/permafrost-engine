#
#  This file is part of Permafrost Engine.
#  Copyright (C) 2018-2023 Eduard Permyakov
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

    WIDTH = 850
    HEIGHT = 700

    def __init__(self):
        vresx, vresy = (1920, 1080)
        super(PerfStatsWindow, self).__init__("Performance",
            (vresx/2 - PerfStatsWindow.WIDTH/2, vresy/2 - PerfStatsWindow.HEIGHT/2, PerfStatsWindow.WIDTH, PerfStatsWindow.HEIGHT),
            pf.NK_WINDOW_BORDER | pf.NK_WINDOW_MOVABLE | pf.NK_WINDOW_MINIMIZABLE | pf.NK_WINDOW_TITLE |
            pf.NK_WINDOW_CLOSABLE, (vresx, vresy))
        self.tickindex = 0
        self.ticksum_ms = 0
        self.max_frame_latency = 0
        self.frame_times_ms = [0]*100
        self.frame_perfstats = [()]*100
        self.frame_allocd_bytes = [0]*100
        self.frame_memstats = [{}]*100
        self.frame_mem_usage = [0]*100
        self.selected_perfstats = ()
        self.expanded_paths = set()
        self.paused = False
        self.trace_python = pf.settings_get("pf.debug.trace_python")
        self.trace_gpu = pf.settings_get("pf.debug.trace_gpu")

    def frame_perf_tab(self):

        ROW_H    = 18
        LEFT_W   = 380
        TRI_W    = 20
        INDENT_W = 16
        MS_W     = 80
        IPC_W    = 60
        BR_W     = 80
        L1D_W    = 80
        LLC_W    = 80

        WHITE   = (255, 255, 255)
        YELLOW  = (255, 220, 0)
        GREY    = (180, 180, 180)
        BLUE    = (100, 180, 255)
        GREEN   = (0, 200, 100)
        RED     = (220, 60, 60)

        def miss_color(val, green_th, red_th):
            if val < green_th:
                return GREEN
            if val < red_th:
                return YELLOW
            return RED

        def header():
            self.layout_row_begin(pf.NK_STATIC, ROW_H, 6)
            self.layout_row_push(LEFT_W)
            self.label_colored("Function", pf.NK_TEXT_LEFT, BLUE)
            self.layout_row_push(MS_W)
            self.label_colored("ms", pf.NK_TEXT_RIGHT, BLUE)
            self.layout_row_push(IPC_W)
            self.label_colored("IPC", pf.NK_TEXT_RIGHT, BLUE)
            self.layout_row_push(BR_W)
            self.label_colored("br miss", pf.NK_TEXT_RIGHT, BLUE)
            self.layout_row_push(L1D_W)
            self.label_colored("L1D miss", pf.NK_TEXT_RIGHT, BLUE)
            self.layout_row_push(LLC_W)
            self.label_colored("LLC miss", pf.NK_TEXT_RIGHT, BLUE)
            self.layout_row_end()

        def render_row(depth, name, ms, ipc, br, l1d, llc, has_children, path, is_thread):
            expanded = path in self.expanded_paths
            self.layout_row_begin(pf.NK_STATIC, ROW_H, 8)

            indent_px = max(depth * INDENT_W, 1)
            self.layout_row_push(indent_px)
            self.label_colored("", pf.NK_TEXT_LEFT, WHITE)

            self.layout_row_push(TRI_W)
            if has_children:
                sym = pf.NK_SYMBOL_TRIANGLE_DOWN if expanded else pf.NK_SYMBOL_TRIANGLE_RIGHT
                new_state = self.selectable_symbol_label(sym, " ", pf.NK_TEXT_LEFT, expanded)
                if new_state != expanded:
                    if new_state:
                        self.expanded_paths.add(path)
                    else:
                        self.expanded_paths.discard(path)
            else:
                self.label_colored("", pf.NK_TEXT_LEFT, WHITE)

            name_w = max(LEFT_W - indent_px - TRI_W, 1)
            self.layout_row_push(name_w)
            self.label_colored(name, pf.NK_TEXT_LEFT, YELLOW if is_thread else WHITE)

            metric_color = GREY if is_thread else WHITE
            self.layout_row_push(MS_W)
            self.label_colored("" if is_thread else "{:.2f}".format(ms), pf.NK_TEXT_RIGHT, metric_color)
            self.layout_row_push(IPC_W)
            self.label_colored("" if is_thread else "{:.2f}".format(ipc), pf.NK_TEXT_RIGHT, metric_color)
            self.layout_row_push(BR_W)
            self.label_colored("" if is_thread else "{:.2f}%".format(br),
                pf.NK_TEXT_RIGHT, metric_color if is_thread else miss_color(br, 1.0, 5.0))
            self.layout_row_push(L1D_W)
            self.label_colored("" if is_thread else "{:.2f}%".format(l1d),
                pf.NK_TEXT_RIGHT, metric_color if is_thread else miss_color(l1d, 5.0, 25.0))
            self.layout_row_push(LLC_W)
            self.label_colored("" if is_thread else "{:.2f}%".format(llc),
                pf.NK_TEXT_RIGHT, metric_color if is_thread else miss_color(llc, 20.0, 60.0))

            self.layout_row_end()

        def render_entry(perfinfo, idx, depth, path, children_of):
            kids = children_of[idx]
            render_row(depth, perfinfo.funcname(idx), perfinfo.ms_delta(idx),
                perfinfo.hw_ipc(idx), perfinfo.hw_br_miss(idx),
                perfinfo.hw_l1d_miss(idx), perfinfo.hw_llc_miss(idx),
                len(kids) > 0, path, is_thread=False)

            if path in self.expanded_paths and kids:
                sorted_kids = sorted(kids, key=lambda c: perfinfo.ms_delta(c), reverse=True)
                for k in sorted_kids:
                    render_entry(perfinfo, k, depth + 1, path + (k,), children_of)

        header()
        for perfinfo in sorted(self.selected_perfstats, key=lambda pi: pi.threadname):
            thread_name = perfinfo.threadname
            thread_path = (thread_name,)
            has_entries = perfinfo.nentries > 0

            render_row(0, thread_name, 0, 0, 0, 0, 0,
                has_entries, thread_path, is_thread=True)

            if thread_path not in self.expanded_paths or not has_entries:
                continue

            n = perfinfo.nentries
            children_of = [[] for _ in range(n)]
            roots = []
            for i in range(n):
                pi_idx = perfinfo.parent_idx(i)
                if pi_idx < 0:
                    roots.append(i)
                else:
                    children_of[pi_idx].append(i)

            sorted_roots = sorted(roots, key=lambda j: perfinfo.ms_delta(j), reverse=True)
            for j in sorted_roots:
                render_entry(perfinfo, j, 1, (thread_name, j), children_of)

    def render_info_tab(self):
        render_info = pf.get_render_info()
        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Renderer: %s" % render_info["renderer"], (255, 255, 255))
        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Version: %s" % render_info["version"], (255, 255, 255))
        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Shading Language Version: %s" % render_info["shading_language_version"], (255, 255, 255))
        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Vendor: %s" % render_info["vendor"], (255, 255, 255))

    def nav_stats_tab(self):
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

    def threads_tab(self):
        current = self.frame_perfstats[self.tickindex]
        if not current:
            return

        def thread_root_sum(stats_tuple, thread_name):
            for pi in stats_tuple:
                if pi.threadname != thread_name:
                    continue
                total = 0.0
                for j in range(pi.nentries):
                    if pi.parent_idx(j) < 0:
                        total += pi.ms_delta(j)
                return total
            return 0.0

        for pi_curr in current:
            thread_name = pi_curr.threadname
            t_frame_times = [int(thread_root_sum(self.frame_perfstats[i], thread_name))
                for i in range(100)]

            self.layout_row_dynamic(20, 1)
            self.label_colored_wrap(thread_name, (255, 255, 0))
            self.layout_row_dynamic(40, 1)
            self.simple_chart(pf.NK_CHART_LINES, (0, 200), t_frame_times, lambda idx: None)

    def memory_tab(self):
        alloc_values = [v // (1024 * 1024) for v in self.frame_allocd_bytes]
        alloc_max = max(max(alloc_values), 1)

        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Allocations per frame (MB)", (255, 255, 0))

        self.layout_row_dynamic(100, 1)
        self.simple_chart(pf.NK_CHART_COLUMN, (0, alloc_max), alloc_values, self.on_chart_click)

        rss_values = [v // 1024 for v in self.frame_mem_usage]
        rss_nonzero = [v for v in rss_values if v > 0]
        if rss_nonzero:
            rss_min = min(rss_nonzero)
            rss_max = max(max(rss_nonzero), rss_min + 1)
        else:
            rss_min, rss_max = 0, 1

        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Resident memory (RSS) over time (MB)", (255, 255, 0))

        self.layout_row_dynamic(100, 1)
        self.simple_chart(pf.NK_CHART_LINES, (rss_min, rss_max), rss_values, self.on_chart_click)

        memstats = self.frame_memstats[self.tickindex]
        if memstats == {}: # We may not have filled up the memstats array yet
            return

        def fmt_bytes(b):
            if b >= 1024 ** 3:
                return "{:.2f} GB".format(b / float(1024 ** 3))
            return "{:.2f} MB".format(b / float(1024 ** 2))

        LABEL_W      = 300
        VALUE_W      = 180
        ROW_H        = 20
        LABEL_COLOR  = (180, 220, 255)
        VALUE_COLOR  = (255, 255, 0)

        def row(label, value):
            self.layout_row_begin(pf.NK_STATIC, ROW_H, 2)
            self.layout_row_push(LABEL_W)
            self.label_colored(label, pf.NK_TEXT_LEFT, LABEL_COLOR)
            self.layout_row_push(VALUE_W)
            self.label_colored(value, pf.NK_TEXT_RIGHT, VALUE_COLOR)
            self.layout_row_end()

        row("Resident memory (RSS)",     fmt_bytes(memstats["vm_rss_kb"] * 1024))
        row("Virtual memory (VSZ)",      fmt_bytes(memstats["vm_size_kb"] * 1024))
        row("Live small allocations",    fmt_bytes(memstats["mi_malloc_normal_current"]))
        row("Mimalloc pages",            "{:d}".format(memstats["mi_pages_current"]))
        row("Threads",                   "{:d}".format(memstats["mi_threads_current"]))

    def on_chart_click(self, index):
        self.selected_perfstats = self.frame_perfstats[index]

    def update(self):

        active_font = pf.get_active_font()
        pf.set_active_font("__default__")

        if not self.paused:
            newtick = pf.prev_frame_ms()
            newstats = pf.prev_frame_perfstats()
            new_allocd = pf.prev_frame_allocd_bytes()
            new_memstats = pf.prev_frame_memstats()
            curr_total_mem = new_memstats["vm_rss_kb"]

            if newtick > self.max_frame_latency:
                self.max_frame_latency = newtick

            self.ticksum_ms -= self.frame_times_ms[self.tickindex]
            self.ticksum_ms += newtick
            self.frame_times_ms[self.tickindex] = newtick
            self.frame_perfstats[self.tickindex] = newstats
            self.frame_allocd_bytes[self.tickindex] = new_allocd
            self.frame_memstats[self.tickindex] = new_memstats
            self.frame_mem_usage[self.tickindex] = curr_total_mem
            self.selected_perfstats = newstats
            self.tickindex = (self.tickindex + 1) % len(self.frame_times_ms)

        self.layout_row_dynamic(100, 1)
        self.simple_chart(pf.NK_CHART_LINES, (0, 400), self.frame_times_ms, self.on_chart_click)

        self.layout_row_dynamic(20, 1)
        avg_frame_latency_ms = float(self.ticksum_ms)/len(self.frame_times_ms)

        try:
            fps = 1000/avg_frame_latency_ms
            self.label_colored_wrap("FPS: {0}".format(int(fps)), (255, 255, 0))
        except:
            pass

        self.layout_row_dynamic(20, 2)
        self.label_colored_wrap("Avg. Frame Latency: {0:.1f} ms".format(avg_frame_latency_ms), (255, 255, 0))

        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Max Frame Latency: {0} ms".format(self.max_frame_latency), (255, 255, 0))

        self.layout_row_dynamic(20, 1)
        old_trace_python = self.trace_python
        self.trace_python = True if self.checkbox("Trace Python", self.trace_python) else False
        if old_trace_python != self.trace_python:
            pf.settings_set("pf.debug.trace_python", self.trace_python, persist=False)

        self.layout_row_dynamic(20, 1)
        old_trace_gpu = self.trace_gpu
        self.trace_gpu = True if self.checkbox("Trace GPU", self.trace_gpu) else False
        if old_trace_gpu != self.trace_gpu:
            pf.settings_set("pf.debug.trace_gpu", self.trace_gpu, persist=False)

        def on_pause_resume():
            self.paused = not self.paused

        text = lambda p: "Resume " if p else "Pause"
        self.layout_row_dynamic(30, 1)
        self.button_label(text(self.paused), on_pause_resume)

        self.tree(pf.NK_TREE_TAB, "Memory", pf.NK_MINIMIZED, self.memory_tab)
        self.tree(pf.NK_TREE_TAB, "Frame Performance", pf.NK_MINIMIZED, self.frame_perf_tab)
        self.tree(pf.NK_TREE_TAB, "Threads", pf.NK_MINIMIZED, self.threads_tab)
        self.tree(pf.NK_TREE_TAB, "Renderer Info", pf.NK_MINIMIZED, self.render_info_tab)
        self.tree(pf.NK_TREE_TAB, "Navigation Stats", pf.NK_MINIMIZED, self.nav_stats_tab)

        pf.set_active_font(active_font)

