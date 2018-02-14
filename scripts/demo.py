import pf


EVENT_SDL_KEYDOWN = 0x300
EVENT_CUSTOM      = 0x20000
SDL_SCANCODE_C    = 6

pf.set_ambient_light_color([1.0, 1.0, 1.0])
pf.set_emit_light_color([1.0, 1.0, 1.0])
pf.set_emit_light_pos([1024.0, 512.0, 256.0])

pf.new_game("assets/maps/grass-cliffs-1", "grass-cliffs.pfmap")

sinbad = pf.AnimEntity("assets/models/sinbad", "Sinbad.pfobj", "Sinbad", "Dance")
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


active_cam_idx = 0
def toggle_camera(user, event):
    mode_for_idx = [1, 0]
    if event[0] == SDL_SCANCODE_C:
        global active_cam_idx
        active_cam_idx = (active_cam_idx + 1) % 2
        pf.activate_camera(active_cam_idx, mode_for_idx[active_cam_idx])

def on_custom_event(user, event):
    print("Custom Event! [{0},{1}]".format(user, event))

pf.register_event_handler(EVENT_SDL_KEYDOWN, toggle_camera, None)
pf.register_event_handler(EVENT_CUSTOM, on_custom_event, "UserArg")

pf.broadcast_event(EVENT_CUSTOM, "EventArg")

