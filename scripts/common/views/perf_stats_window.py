#
#  This file is part of EVERGLORY
#  Copyright (C) 2020 Eduard Permyakov 
#  All rights reserved.
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
        self.frame_mem_accounting = [{}]*100
        self.frame_thread_roots = [None]*100
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

        nframes = len(self.frame_perfstats)
        roots_per_frame = self.frame_thread_roots
        frame_ms        = self.frame_times_ms

        def avg_active(thread_name):
            s = 0.0
            n = 0
            for i in range(nframes):
                rs = roots_per_frame[i]
                if rs is None:
                    continue
                fm = frame_ms[i]
                if fm <= 0:
                    continue
                s += (rs.get(thread_name, 0.0) / fm) * 100.0
                n += 1
            return s / n if n else 0.0

        worker_names = [pi.threadname for pi in current
            if pi.threadname.startswith("worker-")]
        worker_avgs = [avg_active(n) for n in worker_names]
        worker_util = sum(worker_avgs) / len(worker_avgs) if worker_avgs else 0.0

        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Worker Utilization: {:.2f}%".format(worker_util),
            (255, 220, 0))

        cur_roots = roots_per_frame[self.tickindex] or {}
        cur_frame = frame_ms[self.tickindex]

        for pi_curr in current:
            thread_name = pi_curr.threadname
            is_worker = thread_name.startswith("worker-")
            t_frame_times = [int((roots_per_frame[i] or {}).get(thread_name, 0))
                for i in range(nframes)]

            self.layout_row_dynamic(20, 1)
            self.label_colored_wrap(thread_name, (255, 255, 0))
            if is_worker:
                curr_pct = (cur_roots.get(thread_name, 0.0) / cur_frame * 100.0) if cur_frame > 0 else 0.0
                avg_pct  = avg_active(thread_name)
                self.layout_row_dynamic(20, 1)
                self.label_colored_wrap("Active: ({:.2f}% / {:.2f}%)".format(curr_pct, avg_pct),
                    (200, 200, 200))
            self.layout_row_dynamic(40, 1)
            self.simple_chart(pf.NK_CHART_LINES, (0, 200), t_frame_times, lambda idx: None)

    def mem_accounting_section(self):
        acc = self.frame_mem_accounting[self.tickindex]
        if not acc:
            return

        LABEL_W    = 240
        TRI_W      = 20
        INDENT_W   = 16
        BAR_W      = 260
        VAL_W      = 100
        COUNT_W    = 70
        ROW_H      = 20

        HEADER_COLOR = (100, 180, 255)
        SYS_COLOR    = (255, 255, 255)
        SUB_COLOR    = (200, 200, 200)
        VAL_COLOR    = (255, 220, 0)
        TOTAL_COLOR  = (180, 220, 255)

        bar_max = 0
        total_bytes = 0
        total_count = 0
        for entry in acc.values():
            if entry["bytes"] > bar_max:
                bar_max = entry["bytes"]
            total_bytes += entry["bytes"]
            total_count += entry["count"]
        if bar_max <= 0:
            bar_max = 1

        def fmt_bytes(b):
            if b >= 1024 ** 3:
                return "{:.2f} GB".format(b / float(1024 ** 3))
            if b >= 1024 ** 2:
                return "{:.2f} MB".format(b / float(1024 ** 2))
            if b >= 1024:
                return "{:.2f} kB".format(b / float(1024))
            return "{:d} B".format(int(b))

        def render_row(depth, name, bytes_, count_, has_children, path, color):
            expanded = path in self.expanded_paths
            self.layout_row_begin(pf.NK_STATIC, ROW_H, 6)

            indent_px = max(depth * INDENT_W, 1)
            self.layout_row_push(indent_px)
            self.label_colored("", pf.NK_TEXT_LEFT, color)

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
                self.label_colored("", pf.NK_TEXT_LEFT, color)

            name_w = max(LABEL_W - indent_px - TRI_W, 1)
            self.layout_row_push(name_w)
            self.label_colored(name, pf.NK_TEXT_LEFT, color)

            self.layout_row_push(BAR_W)
            frac = 0 if bar_max <= 0 else min(1000, int((bytes_ * 1000) // bar_max))
            self.progress(frac, 1000, False)

            self.layout_row_push(VAL_W)
            self.label_colored(fmt_bytes(bytes_), pf.NK_TEXT_RIGHT, VAL_COLOR)

            self.layout_row_push(COUNT_W)
            self.label_colored("{:d}".format(int(count_)), pf.NK_TEXT_RIGHT, color)

            self.layout_row_end()

        def render_header():
            self.layout_row_begin(pf.NK_STATIC, ROW_H, 5)
            self.layout_row_push(1 + TRI_W)
            self.label_colored("", pf.NK_TEXT_LEFT, HEADER_COLOR)
            self.layout_row_push(LABEL_W - 1 - TRI_W)
            self.label_colored("system / subsystem", pf.NK_TEXT_LEFT, HEADER_COLOR)
            self.layout_row_push(BAR_W)
            self.label_colored("", pf.NK_TEXT_LEFT, HEADER_COLOR)
            self.layout_row_push(VAL_W)
            self.label_colored("bytes", pf.NK_TEXT_RIGHT, HEADER_COLOR)
            self.layout_row_push(COUNT_W)
            self.label_colored("count", pf.NK_TEXT_RIGHT, HEADER_COLOR)
            self.layout_row_end()

        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("Per-system memory accounting", (255, 255, 0))

        render_header()

        for sys_name in sorted(acc.keys(), key=lambda k: -acc[k]["bytes"]):
            entry = acc[sys_name]
            subs = entry.get("subsystems", {})
            named_subs = any(isinstance(k, str) for k in subs.keys())
            path = ("acc", sys_name)
            render_row(0, sys_name, entry["bytes"], entry["count"],
                named_subs, path, SYS_COLOR)

            if path in self.expanded_paths and subs:
                named = [k for k in subs.keys() if isinstance(k, str)]
                for sub_id in sorted(named, key=lambda k: -subs[k]["bytes"]):
                    sub_entry = subs[sub_id]
                    sub_path = ("acc", sys_name, sub_id)
                    render_row(1, sub_id, sub_entry["bytes"], sub_entry["count"],
                        False, sub_path, SUB_COLOR)

        render_row(0, "TOTAL", total_bytes, total_count, False, ("acc", "__total"), TOTAL_COLOR)

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

        self.mem_accounting_section()

    def video_memory_tab(self):
        vram = pf.prev_frame_vramstats()

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

        row("Dedicated video memory",     fmt_bytes(vram["dedicated_kb"] * 1024))
        row("Total available memory",     fmt_bytes(vram["total_available_kb"] * 1024))
        row("Used video memory",          fmt_bytes((vram["total_available_kb"] - vram["current_available_kb"]) * 1024))
        row("Evicted memory",             fmt_bytes(vram["evicted_kb"] * 1024))
        row("Eviction count",             "{:d}".format(vram["eviction_count"]))

        self.gpu_accounting_section()

    def gpu_accounting_section(self):
        acc = pf.prev_frame_gpu_mem_accounting()
        if not acc:
            return

        LABEL_W    = 240
        TRI_W      = 20
        INDENT_W   = 16
        BAR_W      = 260
        VAL_W      = 100
        COUNT_W    = 70
        ROW_H      = 20

        HEADER_COLOR = (100, 180, 255)
        SYS_COLOR    = (255, 255, 255)
        SUB_COLOR    = (200, 200, 200)
        VAL_COLOR    = (255, 220, 0)
        TOTAL_COLOR  = (180, 220, 255)

        bar_max = 0
        total_bytes = 0
        total_count = 0
        for entry in acc.values():
            if entry["bytes"] > bar_max:
                bar_max = entry["bytes"]
            total_bytes += entry["bytes"]
            total_count += entry["count"]
        if bar_max <= 0:
            bar_max = 1

        def fmt_bytes(b):
            if b >= 1024 ** 3:
                return "{:.2f} GB".format(b / float(1024 ** 3))
            if b >= 1024 ** 2:
                return "{:.2f} MB".format(b / float(1024 ** 2))
            if b >= 1024:
                return "{:.2f} kB".format(b / float(1024))
            return "{:d} B".format(int(b))

        def render_row(depth, name, bytes_, count_, has_children, path, color):
            expanded = path in self.expanded_paths
            self.layout_row_begin(pf.NK_STATIC, ROW_H, 6)

            indent_px = max(depth * INDENT_W, 1)
            self.layout_row_push(indent_px)
            self.label_colored("", pf.NK_TEXT_LEFT, color)

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
                self.label_colored("", pf.NK_TEXT_LEFT, color)

            name_w = max(LABEL_W - indent_px - TRI_W, 1)
            self.layout_row_push(name_w)
            self.label_colored(name, pf.NK_TEXT_LEFT, color)

            self.layout_row_push(BAR_W)
            frac = 0 if bar_max <= 0 else min(1000, int((bytes_ * 1000) // bar_max))
            self.progress(frac, 1000, False)

            self.layout_row_push(VAL_W)
            self.label_colored(fmt_bytes(bytes_), pf.NK_TEXT_RIGHT, VAL_COLOR)

            self.layout_row_push(COUNT_W)
            self.label_colored("{:d}".format(int(count_)), pf.NK_TEXT_RIGHT, color)

            self.layout_row_end()

        def render_header():
            self.layout_row_begin(pf.NK_STATIC, ROW_H, 5)
            self.layout_row_push(1 + TRI_W)
            self.label_colored("", pf.NK_TEXT_LEFT, HEADER_COLOR)
            self.layout_row_push(LABEL_W - 1 - TRI_W)
            self.label_colored("system / subsystem", pf.NK_TEXT_LEFT, HEADER_COLOR)
            self.layout_row_push(BAR_W)
            self.label_colored("", pf.NK_TEXT_LEFT, HEADER_COLOR)
            self.layout_row_push(VAL_W)
            self.label_colored("bytes", pf.NK_TEXT_RIGHT, HEADER_COLOR)
            self.layout_row_push(COUNT_W)
            self.label_colored("count", pf.NK_TEXT_RIGHT, HEADER_COLOR)
            self.layout_row_end()

        self.layout_row_dynamic(20, 1)
        self.label_colored_wrap("GPU allocations (per render file)", (255, 255, 0))

        render_header()

        for sys_name in sorted(acc.keys(), key=lambda k: -acc[k]["bytes"]):
            entry = acc[sys_name]
            subs = entry.get("subsystems", {})
            path = ("gpuacc", sys_name)
            render_row(0, sys_name, entry["bytes"], entry["count"],
                bool(subs), path, SYS_COLOR)
            if path in self.expanded_paths and subs:
                for sub_id in sorted(subs.keys(), key=lambda k: -subs[k]["bytes"]):
                    sub_entry = subs[sub_id]
                    render_row(1, sub_id, sub_entry["bytes"], sub_entry["count"],
                        False, ("gpuacc", sys_name, sub_id), SUB_COLOR)

        render_row(0, "TOTAL", total_bytes, total_count, False,
            ("gpuacc", "__total"), TOTAL_COLOR)

    def on_chart_click(self, index):
        self.selected_perfstats = self.frame_perfstats[index]

    def __pickle__(self, **kwargs):
        self.frame_times_ms = [0]*100
        self.frame_perfstats = [()]*100
        self.frame_allocd_bytes = [0]*100
        self.frame_memstats = [{}]*100
        self.frame_mem_usage = [0]*100
        self.frame_mem_accounting = [{}]*100
        self.frame_thread_roots = [None]*100
        self.selected_perfstats = ()
        self.expanded_paths = set()
        self.tickindex = 0
        self.ticksum_ms = 0
        self.max_frame_latency = 0
        return pf.Window.__pickle__(self, **kwargs)

    def update(self):

        active_font = pf.get_active_font()
        pf.set_active_font("__default__")

        if not self.paused:
            newtick = pf.prev_frame_ms()
            newstats = pf.prev_frame_perfstats()
            new_allocd = pf.prev_frame_allocd_bytes()
            new_memstats = pf.prev_frame_memstats()
            new_mem_accounting = pf.prev_frame_mem_accounting()
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
            self.frame_mem_accounting[self.tickindex] = new_mem_accounting
            self.selected_perfstats = newstats

            roots = {}
            for pi in newstats:
                total = 0.0
                for j in range(pi.nentries):
                    if pi.parent_idx(j) >= 0:
                        continue
                    total += pi.ms_delta(j)
                roots[pi.threadname] = total
            self.frame_thread_roots[self.tickindex] = roots

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
        self.tree(pf.NK_TREE_TAB, "Video Memory", pf.NK_MINIMIZED, self.video_memory_tab)
        self.tree(pf.NK_TREE_TAB, "Frame Performance", pf.NK_MINIMIZED, self.frame_perf_tab)
        self.tree(pf.NK_TREE_TAB, "Threads", pf.NK_MINIMIZED, self.threads_tab)
        self.tree(pf.NK_TREE_TAB, "Renderer Info", pf.NK_MINIMIZED, self.render_info_tab)
        self.tree(pf.NK_TREE_TAB, "Navigation Stats", pf.NK_MINIMIZED, self.nav_stats_tab)

        pf.set_active_font(active_font)

