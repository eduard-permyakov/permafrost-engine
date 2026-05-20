import os
import sys

import pf

sys.path.insert(0, pf.get_basedir() + "/scripts")

import rts.globals
import rts.main as demo_main


PROBE_PATH = "/tmp/pf_metal_sprite_probe.txt"
ERROR_PATH = "/tmp/pf_metal_sprite_probe_error.txt"
STATS_PATH = "/tmp/pf_metal_sprite_probe_stats.txt"

SPRITES = (
    {
        "name": "projectile_trail",
        "sheet": ("projectile_trail.png", 1, 4, 4),
        "size": (18.0, 6.0),
        "offset": (-18.0, 11.0, -10.0),
        "fps": 12,
        "repeat": 8,
    },
    {
        "name": "impact_burst",
        "sheet": ("impact_burst.png", 1, 4, 4),
        "size": (13.0, 13.0),
        "offset": (0.0, 11.0, -10.0),
        "fps": 10,
        "repeat": 8,
    },
    {
        "name": "fire_loop",
        "sheet": ("fire_loop.png", 1, 4, 4),
        "size": (11.0, 18.0),
        "offset": (18.0, 13.0, -10.0),
        "fps": 8,
        "repeat": 8,
    },
    {
        "name": "smoke_puff",
        "sheet": ("smoke_puff.png", 1, 4, 4),
        "size": (15.0, 15.0),
        "offset": (0.0, 16.0, 10.0),
        "fps": 7,
        "repeat": 8,
    },
)

STATE = {
    "updates": 0,
    "render_frames": 0,
    "spawned": False,
}


def _write(path, payload):
    with open(path, "w") as outfile:
        outfile.write(payload + "\n")


def _fail(reason):
    _write(ERROR_PATH, str(reason))
    print("METAL_SPRITE_PROBE_FAIL {0}".format(reason))
    sys.stdout.flush()
    os._exit(1)


def _succeed():
    _require_sprite_draw_stats()
    marker = "METAL_SPRITE_PROBE_PASS backend={0} render_frames={1}".format(
        pf.get_render_info().get("backend"),
        STATE["render_frames"],
    )
    _write(PROBE_PATH, marker)
    print(marker)
    sys.stdout.flush()
    os._exit(0)


def _require_sprite_assets():
    sprites_dir = os.path.join(pf.get_basedir(), "assets", "sprites")
    missing = []
    for effect in SPRITES:
        filename = effect["sheet"][0]
        path = os.path.join(sprites_dir, filename)
        if not os.path.exists(path):
            missing.append(filename)
    if missing:
        _fail("missing sprite asset(s): {0}".format(",".join(missing)))


def _require_sprite_draw_stats():
    if not os.path.exists(STATS_PATH):
        _fail("Metal sprite stats file was not written")

    expected = set(effect["sheet"][0] for effect in SPRITES)
    seen = set()
    with open(STATS_PATH, "r") as infile:
        for line in infile:
            fields = line.strip().split()
            for field in fields:
                if field.startswith("sheet="):
                    seen.add(field.split("=", 1)[1])
                    break

    missing = sorted(expected - seen)
    if missing:
        _fail("Metal did not draw sprite sheet(s): {0}".format(",".join(missing)))


def on_render(user, event):
    del user
    del event
    STATE["render_frames"] += 1


def _spawn_probe_sprite():
    _require_sprite_assets()

    scene = list(rts.globals.scene_objs)
    anchors = [
        ent for ent in scene
        if getattr(ent, "faction_id", None) == 1
        and getattr(ent, "selectable", False)
        and hasattr(ent, "pos")
    ]
    if not anchors:
        _fail("no friendly anchor for sprite probe")

    pos = anchors[0].pos
    pf.get_active_camera().center_over_location((pos[0], pos[2]))
    for effect in SPRITES:
        offset = effect["offset"]
        pf.spawn_sprite_animated(
            effect["sheet"],
            effect["size"],
            (pos[0] + offset[0], pos[1] + offset[1], pos[2] + offset[2]),
            effect["fps"],
            effect["repeat"],
        )
    STATE["spawned"] = True


def on_update(user, event):
    del user
    del event
    STATE["updates"] += 1

    backend = pf.get_render_info().get("backend")
    if backend != "METAL":
        _fail("expected METAL backend, got {0}".format(backend))

    if not STATE["spawned"]:
        _spawn_probe_sprite()
        return

    if STATE["render_frames"] >= 24:
        _succeed()
    if STATE["updates"] > 240:
        _fail("sprite probe did not render enough frames")


os.environ["PF_METAL_SPRITE_STATS_PATH"] = STATS_PATH
try:
    os.unlink(STATS_PATH)
except OSError:
    pass

demo_main.main()
pf.register_ui_event_handler(pf.EVENT_RENDER_3D_POST, on_render, None)
pf.register_ui_event_handler(pf.EVENT_UPDATE_START, on_update, None)
