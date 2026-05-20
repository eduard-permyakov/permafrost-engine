import json
import math
import os
import subprocess
import sys
import time

import pf

sys.path.insert(0, pf.get_basedir() + "/scripts")

import rts.globals
import rts.main as demo_main


ERROR_PATH = "/tmp/pf_metal_minimap_fog_probe_error.txt"
DEFAULT_OUTPUT_DIR = "visual_parity_captures/minimap-fog-probe"

STATE = {
    "phase": "init",
    "phase_started_at": None,
    "frames": 0,
    "output_dir": None,
    "expected_backend": None,
    "units": [],
    "waypoints": [],
    "stage_idx": -1,
    "records": [],
    "dynamic_water_enabled": False,
    "dynamic_water_done": False,
    "dynamic_water_record": None,
    "waypoint_count": 4,
}


def _arg_value(name, default=None):
    if name not in sys.argv:
        return default
    idx = sys.argv.index(name)
    if idx + 1 >= len(sys.argv):
        return default
    return sys.argv[idx + 1]


def _env_flag(name):
    return os.environ.get(name) == "1"


def _env_int(name, default):
    try:
        return int(os.environ.get(name, default))
    except (TypeError, ValueError):
        return default


def _write(path, payload):
    with open(path, "w") as outfile:
        outfile.write(payload + "\n")


def _fail(reason):
    _write(ERROR_PATH, str(reason))
    print("MINIMAP_FOG_PROBE_FAIL {0}".format(reason))
    sys.stdout.flush()
    os._exit(1)


def _set_phase(name):
    STATE["phase"] = name
    STATE["phase_started_at"] = time.monotonic()
    STATE["frames"] = 0
    print("MINIMAP_FOG_PROBE_PHASE {0}".format(name))
    sys.stdout.flush()


def _phase_elapsed():
    return time.monotonic() - STATE["phase_started_at"]


def _dist_xz(a, b):
    dx = a[0] - b[0]
    dz = a[1] - b[1]
    return math.sqrt(dx * dx + dz * dz)


def _ent_xz(ent):
    pos = ent.pos
    return (pos[0], pos[2])


def _choose_units():
    units = [
        ent for ent in list(rts.globals.scene_objs)
        if getattr(ent, "faction_id", None) == 1
        and hasattr(ent, "move")
        and getattr(ent, "selectable", False)
    ]
    if not units:
        _fail("no friendly movable units found")
    return units[:4]


def _pathable_near(point, radius=2.0):
    offsets = [
        (0.0, 0.0),
        (24.0, 0.0),
        (-24.0, 0.0),
        (0.0, 24.0),
        (0.0, -24.0),
        (48.0, 24.0),
        (-48.0, 24.0),
        (48.0, -24.0),
        (-48.0, -24.0),
        (72.0, 0.0),
        (-72.0, 0.0),
        (0.0, 72.0),
        (0.0, -72.0),
    ]
    for dx, dz in offsets:
        found = pf.map_nearest_pathable((point[0] + dx, point[1] + dz), radius=radius)
        if found is not None:
            return found
    return None


def _scene_pathable_points(radius):
    points = []
    for ent in list(rts.globals.scene_objs):
        try:
            point = _pathable_near(_ent_xz(ent), radius=radius)
        except Exception:
            continue
        if point is None:
            continue
        if all(_dist_xz(point, seen) > 24.0 for seen in points):
            points.append(point)
    return points


def _spread_waypoints(anchor, points, count):
    candidates = [point for point in points if _dist_xz(point, anchor) > 48.0]
    chosen = []
    while candidates and len(chosen) < count:
        refs = [anchor] + chosen
        best = max(candidates, key=lambda point: min(_dist_xz(point, ref) for ref in refs))
        chosen.append(best)
        candidates = [point for point in candidates if _dist_xz(point, best) > 48.0]
    return chosen


def _stage_units(point):
    units = STATE["units"]
    offsets = [(-10.0, -10.0), (-10.0, 10.0), (10.0, -10.0), (10.0, 10.0)]
    staged = []
    for ent, offset in zip(units, offsets):
        target = _pathable_near((point[0] + offset[0], point[1] + offset[1]),
                                radius=max(getattr(ent, "selection_radius", 2.0), 1.0))
        if target is None:
            continue
        height = pf.map_height_at_point(target[0], target[1])
        if height is None:
            continue
        ent.pos = (target[0], height, target[1])
        ent.move(point)
        staged.append({
            "name": getattr(ent, "name", "unit"),
            "position": target,
        })
    if not staged:
        _fail("could not stage units near {0}".format(point))
    pf.set_unit_selection(units)
    return staged


def _tile_attrs(tile):
    return {
        "pathable": int(tile.pathable),
        "type": int(tile.type),
        "base_height": int(tile.base_height),
        "top_mat_idx": int(tile.top_mat_idx),
        "sides_mat_idx": int(tile.sides_mat_idx),
        "ramp_height": int(tile.ramp_height),
        "blend_mode": int(tile.blend_mode),
        "blend_normals": int(tile.blend_normals),
    }


def _parse_pair_env(name, default):
    value = os.environ.get(name)
    if not value:
        return default
    parts = value.split(",")
    if len(parts) != 2:
        return default
    try:
        return (int(parts[0]), int(parts[1]))
    except ValueError:
        return default


def _find_dynamic_water_tile():
    chunk = _parse_pair_env("PF_MINIMAP_DYNAMIC_WATER_CHUNK", (3, 3))
    tile_pos = _parse_pair_env("PF_MINIMAP_DYNAMIC_WATER_TILE", (0, 0))
    tile = pf.Tile()
    return chunk, tile_pos, tile


def _start_dynamic_water_update():
    chunk, tile_pos, tile = _find_dynamic_water_tile()
    tile.type = pf.TILETYPE_FLAT
    tile.base_height = -3
    tile.ramp_height = 0
    tile.pathable = 1
    pf.update_tile(chunk, tile_pos, tile)
    STATE["dynamic_water_record"] = {
        "name": "dynamic_water_update",
        "chunk": chunk,
        "tile": tile_pos,
        "after": _tile_attrs(tile),
    }
    _set_phase("settle:dynamic_water_update")


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
        print("MINIMAP_FOG_PROBE_ACTIVATE_FALLBACK timeout")
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


def _capture(name):
    path = os.path.join(STATE["output_dir"], "metal_{0}.png".format(name))
    if _env_flag("PF_MINIMAP_FOG_PROBE_NO_CAPTURE"):
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
        _fail("screencapture failed for {0}".format(name))
    return path


def _write_summary():
    path = os.path.join(STATE["output_dir"], "summary_metal_minimap_fog.json")
    payload = {
        "backend": pf.get_render_info(),
        "records": STATE["records"],
        "waypoints": STATE["waypoints"],
        "dynamic_water_update": STATE["dynamic_water_record"],
    }
    with open(path, "w") as outfile:
        json.dump(payload, outfile, indent=2, sort_keys=True)
        outfile.write("\n")
    print("MINIMAP_FOG_PROBE_SUMMARY {0}".format(path))
    sys.stdout.flush()


def _start_next_stage():
    STATE["stage_idx"] += 1
    if STATE["stage_idx"] > len(STATE["waypoints"]):
        if STATE["dynamic_water_enabled"] and not STATE["dynamic_water_done"]:
            STATE["dynamic_water_done"] = True
            _start_dynamic_water_update()
            return
        _write_summary()
        print("MINIMAP_FOG_PROBE_PASS captures={0}".format(len(STATE["records"])))
        sys.stdout.flush()
        os._exit(0)

    if STATE["stage_idx"] == 0:
        point = _ent_xz(STATE["units"][0])
        pf.get_active_camera().center_over_location(point)
        pf.set_unit_selection(STATE["units"])
        _set_phase("settle:initial")
        return

    point = STATE["waypoints"][STATE["stage_idx"] - 1]
    staged = _stage_units(point)
    pf.get_active_camera().center_over_location(point)
    STATE["records"].append({
        "name": "stage_{0}".format(STATE["stage_idx"]),
        "target": point,
        "units": staged,
    })
    _set_phase("settle:stage_{0}".format(STATE["stage_idx"]))


def _finish_stage():
    name = "initial" if STATE["stage_idx"] == 0 else "stage_{0}".format(STATE["stage_idx"])
    capture = _capture(name)
    if STATE["stage_idx"] == 0:
        STATE["records"].append({
            "name": name,
            "target": _ent_xz(STATE["units"][0]),
            "capture": capture,
        })
    else:
        STATE["records"][-1]["capture"] = capture
    print("MINIMAP_FOG_PROBE_CAPTURE {0} {1}".format(name, capture))
    sys.stdout.flush()
    _start_next_stage()


def _finish_dynamic_water_update():
    capture = _capture("dynamic_water_update")
    STATE["dynamic_water_record"]["capture"] = capture
    STATE["records"].append(STATE["dynamic_water_record"])
    print("MINIMAP_FOG_PROBE_CAPTURE dynamic_water_update {0}".format(capture))
    sys.stdout.flush()
    _start_next_stage()


def on_update(user, event):
    del user
    del event

    if STATE["phase"] == "init":
        backend = pf.get_render_info().get("backend")
        if STATE["expected_backend"] and backend != STATE["expected_backend"]:
            _fail("expected {0} backend, got {1}".format(STATE["expected_backend"], backend))
        STATE["units"] = _choose_units()
        anchor = _ent_xz(STATE["units"][0])
        radius = max(getattr(STATE["units"][0], "selection_radius", 2.0), 1.0)
        STATE["waypoints"] = _spread_waypoints(anchor, _scene_pathable_points(radius), STATE["waypoint_count"])
        if STATE["waypoint_count"] > 0 and not STATE["waypoints"]:
            _fail("no spread waypoints found")
        _start_next_stage()
        return

    if STATE["phase"].startswith("settle:"):
        STATE["frames"] += 1
        if STATE["frames"] >= 90:
            if STATE["phase"] == "settle:dynamic_water_update":
                _finish_dynamic_water_update()
                return
            _finish_stage()
            return
        if _phase_elapsed() > 8.0:
            _fail("timed out settling {0}".format(STATE["phase"]))


def main():
    output_dir = _arg_value("--output-dir",
                            os.environ.get("PF_MINIMAP_FOG_PROBE_OUTPUT_DIR", DEFAULT_OUTPUT_DIR))
    if not os.path.isabs(output_dir):
        output_dir = os.path.join(pf.get_basedir(), output_dir)
    if not os.path.isdir(output_dir):
        os.makedirs(output_dir)
    STATE["output_dir"] = output_dir
    STATE["expected_backend"] = _arg_value("--expect-backend", "METAL")
    STATE["dynamic_water_enabled"] = _env_flag("PF_MINIMAP_FOG_PROBE_DYNAMIC_WATER")
    STATE["waypoint_count"] = max(0, _env_int("PF_MINIMAP_FOG_PROBE_WAYPOINTS", 4))
    _set_phase("init")
    demo_main.main()
    pf.register_ui_event_handler(pf.EVENT_UPDATE_START, on_update, None)


main()
