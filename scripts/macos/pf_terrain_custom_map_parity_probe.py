import json
import math
import os
import subprocess
import sys
import time

import pf


ERROR_PATH = "/tmp/pf_terrain_custom_map_parity_probe_error.txt"
DEFAULT_OUTPUT_DIR = "visual_parity_captures/terrain-custom-map-probe"
TILES_PER_CHUNK = 32
X_COORDS_PER_TILE = 8
Z_COORDS_PER_TILE = 8

MATERIALS = [
    ("Grass", "grass.png"),
    ("Cliffs", "cliffs.png"),
    ("Grass2", "grass2.jpg"),
    ("Cobblestone", "cobblestone.jpg"),
    ("Dirty-Grass", "dirty_grass.jpg"),
    ("Dirt-Road", "dirt_road.jpg"),
    ("Cracked-Dirt", "cracked_dirt.jpg"),
    ("Metal-Platform", "metal_platform.jpg"),
    ("Snowy-Grass", "snowy_grass.jpg"),
    ("Lava-Ground", "lava_ground.jpg"),
    ("Sand", "sand.jpg"),
]

SPLAT_PAIRS = [(0, 2), (2, 4), (4, 6), (10, 4)]

STATE = {
    "phase": "init",
    "phase_started_at": None,
    "frames": 0,
    "output_dir": None,
    "expected_backend": None,
    "camera": None,
    "scene_idx": 0,
    "scenes": [],
    "records": [],
    "updated_tile": None,
    "map_rows": 10,
    "map_cols": 10,
    "minimap_mode": "default",
}


def _arg_value(name, default=None):
    if name not in sys.argv:
        return default
    idx = sys.argv.index(name)
    if idx + 1 >= len(sys.argv):
        return default
    return sys.argv[idx + 1]


def _env_int(name, default):
    try:
        return int(os.environ.get(name, default))
    except (TypeError, ValueError):
        return default


def _env_flag(name):
    return os.environ.get(name) == "1"


def _env_choice(name, default, choices):
    value = os.environ.get(name, default).strip().lower()
    if value not in choices:
        _fail("{0} must be one of {1}".format(name, ",".join(choices)))
    return value


def _write(path, payload):
    with open(path, "w") as outfile:
        outfile.write(payload + "\n")


def _fail(reason):
    _write(ERROR_PATH, str(reason))
    print("TERRAIN_CUSTOM_MAP_FAIL {0}".format(reason))
    sys.stdout.flush()
    os._exit(1)


def _set_phase(name):
    STATE["phase"] = name
    STATE["phase_started_at"] = time.monotonic()
    STATE["frames"] = 0
    print("TERRAIN_CUSTOM_MAP_PHASE {0}".format(name))
    sys.stdout.flush()


def _phase_elapsed():
    return time.monotonic() - STATE["phase_started_at"]


def _tile_to_string(tile):
    return (
        "{type:1X}{sign}{height:02d}{ramp:02d}{top:03d}{side:03d}"
        "{pathable:d}{blend:d}{smooth:d}{no_bump:d}00000000"
    ).format(
        type=tile["type"],
        sign="+" if tile["base_height"] >= 0 else "-",
        height=abs(tile["base_height"]),
        ramp=tile["ramp_height"],
        top=tile["top_mat_idx"],
        side=tile["sides_mat_idx"],
        pathable=tile["pathable"],
        blend=tile["blend_mode"],
        smooth=tile["blend_normals"],
        no_bump=tile["no_bump_map"],
    )


def _base_tile(global_r, global_c, rows, cols):
    total_r = rows * TILES_PER_CHUNK
    total_c = cols * TILES_PER_CHUNK
    mid_r = total_r // 2
    mid_c = total_c // 2

    tile = {
        "type": pf.TILETYPE_FLAT,
        "base_height": 0,
        "ramp_height": 0,
        "top_mat_idx": 0,
        "sides_mat_idx": 1,
        "pathable": 1,
        "blend_mode": pf.BLEND_MODE_BLUR,
        "blend_normals": 1,
        "no_bump_map": 0,
    }

    if global_r < total_r * 0.33:
        tile["top_mat_idx"] = 10
    elif global_r > total_r * 0.70:
        tile["top_mat_idx"] = 8
    elif global_c > total_c * 0.68:
        tile["top_mat_idx"] = 4
    elif global_c < total_c * 0.25:
        tile["top_mat_idx"] = 2

    if abs(global_r - mid_r) <= 1 or abs(global_c - mid_c) <= 1:
        tile["top_mat_idx"] = 5

    plaza_r0 = mid_r - 10
    plaza_r1 = mid_r + 10
    plaza_c0 = mid_c - 10
    plaza_c1 = mid_c + 10
    if plaza_r0 <= global_r <= plaza_r1 and plaza_c0 <= global_c <= plaza_c1:
        tile["top_mat_idx"] = 3

    basin_specs = (
        (int(total_r * 0.28), int(total_c * 0.72), 15.0, 22.0),
        (int(total_r * 0.40), int(total_c * 0.58), 12.0, 18.0),
    )
    for basin_r, basin_c, radius_r, radius_c in basin_specs:
        basin = ((global_r - basin_r) / radius_r) ** 2 + ((global_c - basin_c) / radius_c) ** 2
        if basin <= 1.0:
            tile["base_height"] = -3
            tile["top_mat_idx"] = 10
            tile["sides_mat_idx"] = 10
            tile["pathable"] = 1
            tile["blend_normals"] = 0
            break
        if basin <= 1.25:
            tile["top_mat_idx"] = 10

    ridge_center = int(total_c * 0.38 + math.sin(global_r / 9.0) * 6.0)
    ridge_dist = abs(global_c - ridge_center)
    if 0 <= ridge_dist <= 2 and global_r > total_r * 0.22:
        tile["base_height"] = 5
        tile["top_mat_idx"] = 1
        tile["sides_mat_idx"] = 1
        tile["pathable"] = 0
        tile["blend_normals"] = 1
    elif 2 < ridge_dist <= 4 and global_r > total_r * 0.22:
        tile["base_height"] = 2
        tile["top_mat_idx"] = 4
        tile["sides_mat_idx"] = 1

    lava_r = int(total_r * 0.76)
    lava_c = int(total_c * 0.22)
    lava = ((global_r - lava_r) / 13.0) ** 2 + ((global_c - lava_c) / 13.0) ** 2
    if lava <= 1.0:
        tile["top_mat_idx"] = 9
        tile["pathable"] = 0
    elif lava <= 1.6:
        tile["top_mat_idx"] = 6

    return tile


def _build_pfmap(rows, cols):
    lines = [
        "version 1.0",
        "num_materials {0}".format(len(MATERIALS)),
        "num_rows {0}".format(rows),
        "num_cols {0}".format(cols),
    ]
    for name, texname in MATERIALS:
        lines.append("material {0} {1}".format(name, texname))

    for chunk_r in range(rows):
        for chunk_c in range(cols):
            for tile_r in range(TILES_PER_CHUNK):
                row = []
                for tile_c in range(TILES_PER_CHUNK):
                    global_r = chunk_r * TILES_PER_CHUNK + tile_r
                    global_c = chunk_c * TILES_PER_CHUNK + tile_c
                    row.append(_tile_to_string(_base_tile(global_r, global_c, rows, cols)))
                    if len(row) == 4:
                        lines.append(" ".join(row))
                        row = []
                if row:
                    lines.append(" ".join(row))
    return "\n".join(lines) + "\n"


def _world_for_tile(rows, cols, global_r, global_c):
    del rows
    del cols
    return (
        -(global_c + 0.5) * X_COORDS_PER_TILE,
        (global_r + 0.5) * Z_COORDS_PER_TILE,
    )


def _map_center(rows, cols):
    return (
        -(cols * TILES_PER_CHUNK * X_COORDS_PER_TILE) / 2.0,
        (rows * TILES_PER_CHUNK * Z_COORDS_PER_TILE) / 2.0,
    )


def _setup_camera(rows, cols):
    center = _map_center(rows, cols)
    cam = pf.Camera(
        mode=pf.CAM_MODE_FREE,
        position=(center[0], 620.0, center[1]),
        pitch=-65.0,
        yaw=135.0,
    )
    pf.set_active_camera(cam)
    STATE["camera"] = cam


def _setup_world():
    if _env_flag("PF_TERRAIN_CUSTOM_MAP_DISABLE_VSYNC"):
        pf.settings_set("pf.video.vsync", False, persist=False)

    rows = max(2, _env_int("PF_TERRAIN_CUSTOM_MAP_ROWS", STATE["map_rows"]))
    cols = max(2, _env_int("PF_TERRAIN_CUSTOM_MAP_COLS", STATE["map_cols"]))
    STATE["map_rows"] = rows
    STATE["map_cols"] = cols

    pf.load_map_string(_build_pfmap(rows, cols), update_navgrid=False)
    pf.disable_fog_of_war()
    pf.disable_unit_selection()
    pf.set_ambient_light_color((1.0, 1.0, 1.0))
    pf.set_emit_light_color((1.0, 1.0, 1.0))
    pf.set_emit_light_pos((1664.0, 1024.0, 384.0))
    minimap_mode = _env_choice("PF_TERRAIN_CUSTOM_MAP_MINIMAP_MODE", "default",
                               ("default", "hidden", "offscreen"))
    STATE["minimap_mode"] = minimap_mode
    if minimap_mode == "hidden":
        pf.set_minimap_size(0)
    else:
        pf.set_minimap_size(320)
    if minimap_mode == "offscreen":
        pf.set_minimap_position(-10000.0, -10000.0)
    else:
        pf.set_minimap_position(250.0, 830.0)
    pf.set_minimap_resize_mask(pf.ANCHOR_X_LEFT | pf.ANCHOR_Y_BOT)

    if not _env_flag("PF_TERRAIN_CUSTOM_MAP_DISABLE_SPLATS"):
        for base, accent in SPLAT_PAIRS:
            pf.map_add_splat(base, accent)

    _setup_camera(rows, cols)

    total_r = rows * TILES_PER_CHUNK
    total_c = cols * TILES_PER_CHUNK
    center = _map_center(rows, cols)
    ridge = _world_for_tile(rows, cols, int(total_r * 0.58), int(total_c * 0.38))
    water = _world_for_tile(rows, cols, int(total_r * 0.40), int(total_c * 0.58))
    update = _world_for_tile(rows, cols, (rows // 2) * TILES_PER_CHUNK + 10,
                             (cols // 2) * TILES_PER_CHUNK + 10)

    STATE["scenes"] = [
        {"name": "overview", "target": center, "height": 720.0},
        {"name": "ridge_splat_detail", "target": ridge, "height": 380.0},
        {"name": "water_edge", "target": water, "height": 720.0},
        {"name": "post_tile_update", "target": update, "height": 360.0, "update": True},
    ]
    only_scene = os.environ.get("PF_TERRAIN_CUSTOM_MAP_ONLY_SCENE")
    if only_scene:
        STATE["scenes"] = [scene for scene in STATE["scenes"] if scene["name"] == only_scene]
        if not STATE["scenes"]:
            _fail("unknown scene {0}".format(only_scene))


def _apply_tile_update():
    rows = STATE["map_rows"]
    cols = STATE["map_cols"]
    chunk = (rows // 2, cols // 2)
    tile_pos = (10, 10)
    tile = pf.Tile()
    tile.type = pf.TILETYPE_FLAT
    tile.base_height = 4
    tile.ramp_height = 0
    tile.top_mat_idx = 9
    tile.sides_mat_idx = 1
    tile.pathable = 0
    tile.blend_mode = pf.BLEND_MODE_BLUR
    tile.blend_normals = 1
    pf.update_tile(chunk, tile_pos, tile)
    STATE["updated_tile"] = {
        "chunk": chunk,
        "tile": tile_pos,
        "after": {
            "type": int(tile.type),
            "base_height": int(tile.base_height),
            "top_mat_idx": int(tile.top_mat_idx),
            "sides_mat_idx": int(tile.sides_mat_idx),
            "pathable": int(tile.pathable),
            "blend_mode": int(tile.blend_mode),
            "blend_normals": int(tile.blend_normals),
        },
    }


def _place_camera(scene):
    cam = STATE["camera"]
    target = scene["target"]
    cam.position = (target[0], scene["height"], target[1])
    cam.pitch = -65.0
    cam.yaw = 135.0
    cam.center_over_location(target)


def _activate_own_window():
    pid = str(os.getpid())
    script = (
        'tell application "System Events"\n'
        "    set capture_process to first process whose unix id is {0}\n"
        "    set frontmost of capture_process to true\n"
        "    if (count of windows of capture_process) is greater than 0 then\n"
        '        perform action "AXRaise" of window 1 of capture_process\n'
        "    end if\n"
        "end tell"
    ).format(pid)
    try:
        subprocess.run(["osascript", "-e", script], timeout=2.0)
    except subprocess.TimeoutExpired:
        print("TERRAIN_CUSTOM_MAP_ACTIVATE_FALLBACK timeout")
        sys.stdout.flush()
    time.sleep(0.2)


def _capture_window_id():
    helper = os.path.join(pf.get_basedir(), "scripts/macos/pf_window_id_for_pid.swift")
    try:
        ret = subprocess.run(["/usr/bin/swift", helper, str(os.getpid())],
                             stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE,
                             universal_newlines=True,
                             timeout=3.0)
    except subprocess.TimeoutExpired:
        return None
    if ret.returncode != 0:
        return None
    window_id = ret.stdout.strip().splitlines()[-1]
    if not window_id.isdigit():
        return None
    return window_id


def _capture(scene):
    backend = pf.get_render_info().get("backend", "unknown").lower()
    path = os.path.join(STATE["output_dir"], "{0}_{1}.png".format(backend, scene["name"]))
    if _env_flag("PF_TERRAIN_CUSTOM_MAP_NO_CAPTURE"):
        _write(path + ".skipped", "capture skipped")
        return path + ".skipped"

    _activate_own_window()
    window_id = _capture_window_id()
    ret = 1
    if window_id is not None:
        try:
            ret = subprocess.run(["screencapture", "-x", "-o", "-l{0}".format(window_id), path],
                                 timeout=3.0).returncode
        except subprocess.TimeoutExpired:
            ret = 1
    if ret != 0:
        try:
            ret = subprocess.run(["screencapture", "-x", "-o", path], timeout=3.0).returncode
        except subprocess.TimeoutExpired:
            ret = 1
    if ret != 0:
        _fail("screencapture failed for {0}".format(scene["name"]))
    return path


def _write_summary():
    path = os.path.join(STATE["output_dir"], "summary_{0}.json".format(
        pf.get_render_info().get("backend", "unknown").lower()
    ))
    payload = {
        "backend": pf.get_render_info(),
        "map": {
            "rows": STATE["map_rows"],
            "cols": STATE["map_cols"],
            "materials": MATERIALS,
            "splat_pairs": SPLAT_PAIRS,
            "minimap_mode": STATE["minimap_mode"],
        },
        "records": STATE["records"],
        "updated_tile": STATE["updated_tile"],
    }
    with open(path, "w") as outfile:
        json.dump(payload, outfile, indent=2, sort_keys=True)
        outfile.write("\n")
    print("TERRAIN_CUSTOM_MAP_SUMMARY {0}".format(path))
    sys.stdout.flush()


def _start_scene():
    if STATE["scene_idx"] >= len(STATE["scenes"]):
        _write_summary()
        print("TERRAIN_CUSTOM_MAP_PASS backend={0} scenes={1} output_dir={2}".format(
            pf.get_render_info().get("backend"),
            len(STATE["records"]),
            STATE["output_dir"],
        ))
        sys.stdout.flush()
        if os.environ.get("PF_METAL_CAPTURE_PATH"):
            delay = float(os.environ.get("PF_METAL_CAPTURE_EXIT_DELAY", "8.0"))
            time.sleep(delay)
        os._exit(0)

    scene = STATE["scenes"][STATE["scene_idx"]]
    if scene.get("update") and STATE["updated_tile"] is None:
        _apply_tile_update()
    _place_camera(scene)
    _set_phase("settle:{0}".format(scene["name"]))


def _finish_scene():
    scene = STATE["scenes"][STATE["scene_idx"]]
    cam = pf.get_active_camera()
    capture_path = _capture(scene)
    STATE["records"].append({
        "name": scene["name"],
        "target": scene["target"],
        "camera_position": cam.position,
        "camera_direction": cam.direction,
        "capture": capture_path,
    })
    print("TERRAIN_CUSTOM_MAP_CAPTURE {0} {1}".format(scene["name"], capture_path))
    sys.stdout.flush()
    STATE["scene_idx"] += 1
    _start_scene()


def on_update(user, event):
    del user
    del event

    if STATE["phase"] == "init":
        backend = pf.get_render_info().get("backend")
        if STATE["expected_backend"] and backend != STATE["expected_backend"]:
            _fail("expected {0} backend, got {1}".format(STATE["expected_backend"], backend))
        _setup_world()
        _start_scene()
        return

    if STATE["phase"].startswith("settle:"):
        STATE["frames"] += 1
        if STATE["frames"] >= 90:
            _finish_scene()
            return
        if _phase_elapsed() > 8.0:
            _fail("timed out settling {0}".format(STATE["phase"]))


def main():
    output_dir = _arg_value("--output-dir",
                            os.environ.get("PF_TERRAIN_CUSTOM_MAP_OUTPUT_DIR", DEFAULT_OUTPUT_DIR))
    if not os.path.isabs(output_dir):
        output_dir = os.path.join(pf.get_basedir(), output_dir)
    if not os.path.isdir(output_dir):
        os.makedirs(output_dir)
    STATE["output_dir"] = output_dir
    STATE["expected_backend"] = _arg_value("--expect-backend", os.environ.get("PF_TERRAIN_CUSTOM_MAP_EXPECT_BACKEND"))
    _set_phase("init")
    pf.register_ui_event_handler(pf.EVENT_UPDATE_START, on_update, None)


main()
