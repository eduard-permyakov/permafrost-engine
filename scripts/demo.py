import pf

############################################################
# Constants                                                #
############################################################

EVENT_SDL_QUIT           = 0x100
EVENT_SDL_KEYDOWN        = 0x300
EVENT_CUSTOM             = 0x20000
EVENT_SINBAD_TOGGLE_ANIM = 0x20001

SDL_SCANCODE_C  = 6
SDL_SCANCODE_V  = 25

NK_WINDOW_BORDER            = 1 << 0
NK_WINDOW_MOVABLE           = 1 << 1
NK_WINDOW_SCALABLE          = 1 << 2
NK_WINDOW_CLOSABLE          = 1 << 3
NK_WINDOW_MINIMIZABLE       = 1 << 4
NK_WINDOW_NO_SCROLLBAR      = 1 << 5
NK_WINDOW_TITLE             = 1 << 6
NK_WINDOW_SCROLL_AUTO_HIDE  = 1 << 7
NK_WINDOW_BACKGROUND        = 1 << 8
NK_WINDOW_SCALE_LEFT        = 1 << 9
NK_WINDOW_NO_INPUT          = 1 << 10

NK_TEXT_ALIGN_LEFT          = 0x01
NK_TEXT_ALIGN_CENTERED      = 0x02
NK_TEXT_ALIGN_RIGHT         = 0x04
NK_TEXT_ALIGN_TOP           = 0x08
NK_TEXT_ALIGN_MIDDLE        = 0x10
NK_TEXT_ALIGN_BOTTOM        = 0x20

NK_TEXT_LEFT        = NK_TEXT_ALIGN_MIDDLE | NK_TEXT_ALIGN_LEFT
NK_TEXT_CENTERED    = NK_TEXT_ALIGN_MIDDLE | NK_TEXT_ALIGN_CENTERED
NK_TEXT_RIGHT       = NK_TEXT_ALIGN_MIDDLE | NK_TEXT_ALIGN_RIGHT


NK_CHART_LINES      = 0
NK_CHART_COLUMN     = 1
NK_CHART_MAX        = 2

############################################################
# Global configs                                           #
############################################################

pf.set_ambient_light_color([1.0, 1.0, 1.0])
pf.set_emit_light_color([1.0, 1.0, 1.0])
pf.set_emit_light_pos([1024.0, 512.0, 256.0])

pf.new_game("assets/maps/grass-cliffs-1", "grass-cliffs.pfmap")

############################################################
# Entities example                                         #
############################################################

class Sinbad(pf.AnimEntity):

    def __init__(self, path, pfobj, name):
        self.anim_idx = 0
        self.anim_map = ["Dance", "RunBase"]
        super(Sinbad, self).__init__(path, pfobj, name, self.anim_map[self.anim_idx])
        self.register(EVENT_SINBAD_TOGGLE_ANIM, Sinbad.on_anim_toggle, self)
    
    def __del__(self):
        self.unregister(EVENT_SINBAD_TOGGLE_ANIM, Sinbad.on_anim_toggle)
    
    def on_anim_toggle(self, event):
        self.anim_idx = (self.anim_idx + 1) % 2
        self.play_anim(self.anim_map[self.anim_idx])
        

sinbad = Sinbad("assets/models/sinbad", "Sinbad.pfobj", "Sinbad")
sinbad.pos = [0.0, 6.0, -50.0]
sinbad.scale = [1.0, 1.0, 1.0]
sinbad.activate()

oak_tree = pf.Entity("assets/models/oak_tree", "oak_tree.pfobj", "OakTree")
oak_tree.pos = [30.0, 4.0, -50.0]
oak_tree.scale = [2.0, 2.0, 2.0]
oak_tree.activate()

oak_leafless = pf.Entity("assets/models/oak_tree", "oak_leafless.pfobj", "OakLeafless")
oak_leafless.pos = [0.0, 0.0, -10.0]
oak_leafless.scale = [1.5, 1.5, 1.5]
oak_leafless.activate()

############################################################
# Events example                                           #
############################################################

active_cam_idx = 0
def toggle_camera(user, event):
    mode_for_idx = [1, 0]

    if event[0] == SDL_SCANCODE_C:
        global active_cam_idx
        active_cam_idx = (active_cam_idx + 1) % 2
        pf.activate_camera(active_cam_idx, mode_for_idx[active_cam_idx])
    elif event[0] == SDL_SCANCODE_V:
        sinbad.notify(EVENT_SINBAD_TOGGLE_ANIM, None)

def on_custom_event(user, event):
    print("Custom Event! [{0},{1}]".format(user, event))

pf.register_event_handler(EVENT_SDL_KEYDOWN, toggle_camera, None)
pf.register_event_handler(EVENT_CUSTOM, on_custom_event, "UserArg")

pf.global_event(EVENT_CUSTOM, "EventArg")

############################################################
# UI example                                               #
############################################################

class DemoWindow(pf.Window):

    def __init__(self):
        super(DemoWindow, self).__init__("Permafrost Engine Demo", (25, 25, 230, 200), 
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

demo_win = DemoWindow()
demo_win.show()

class PerfStatsWindow(pf.Window):

    def __init__(self):
        super(PerfStatsWindow, self).__init__("Performance", (280, 25, 600, 200), 
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

perf_win = PerfStatsWindow()
perf_win.show()

