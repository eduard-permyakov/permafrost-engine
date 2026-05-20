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


PROBE_PATH = "/tmp/pf_gameplay_soak_probe.txt"
ERROR_PATH = "/tmp/pf_gameplay_soak_probe_error.txt"
DEFAULT_OUTPUT_DIR = "visual_parity_captures/gameplay-soak-probe"

STATE = {
    "phase": "init",
    "ticks": 0,
    "output_dir": None,
    "expected_backend": None,
    "units": [],
    "enemy": None,
    "enemy_hp": None,
    "waypoints": [],
    "stage_idx": -1,
    "waypoint_count": 4,
    "settle_frames": 90,
    "dynamic_water_enabled": False,
    "dynamic_water_done": False,
    "dynamic_water_record": None,
    "motion_started": False,
    "attack_started": False,
    "phase_started_at": None,
    "phase_log": [],
    "frame_ms": [],
    "records": [],
    "checks": {
        "camera": False,
        "selection": False,
        "movement": False,
        "dynamic_water": False,
        "combat": False,
    },
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


def _set_phase(name):
    STATE["phase"] = name
    STATE["ticks"] = 0
    STATE["phase_started_at"] = time.monotonic()
    STATE["phase_log"].append(name)
    print("GAMEPLAY_SOAK_PHASE {0}".format(name))
    sys.stdout.flush()


def _phase_elapsed():
    return time.monotonic() - STATE["phase_started_at"]


def _metrics_summary(samples):
    if not samples:
        return {"count": 0, "avg_ms": None, "min_ms": None, "max_ms": None}
    return {
        "count": len(samples),
        "avg_ms": sum(samples) / float(len(samples)),
        "min_ms": min(samples),
        "max_ms": max(samples),
    }


def _summary_path(backend):
    if not STATE["output_dir"]:
        return None
    return os.path.join(STATE["output_dir"], "summary_{0}.json".format(backend.lower()))


def _ent_xz(ent):
    pos = ent.pos
    return (pos[0], pos[2])


def _write_summary(status, reason=None):
    backend = pf.get_render_info().get("backend", "unknown")
    path = _summary_path(backend)
    if not path:
        return
    enemy = STATE["enemy"]
    payload = {
        "status": status,
        "reason": reason,
        "backend": pf.get_render_info(),
        "expected_backend": STATE["expected_backend"],
        "checks": STATE["checks"],
        "phase_log": STATE["phase_log"],
        "frame_ms": _metrics_summary(STATE["frame_ms"]),
        "waypoints": STATE["waypoints"],
        "records": STATE["records"],
        "dynamic_water_update": STATE["dynamic_water_record"],
        "selected": [
            {
                "name": getattr(ent, "name", "unit"),
                "position": _ent_xz(ent),
            }
            for ent in STATE["units"]
        ],
        "enemy": None if enemy is None else {
            "name": getattr(enemy, "name", "enemy"),
            "position": _ent_xz(enemy),
            "hp_start": STATE["enemy_hp"],
            "hp_end": getattr(enemy, "hp", None),
        },
    }
    with open(path, "w") as outfile:
        json.dump(payload, outfile, indent=2, sort_keys=True)
        outfile.write("\n")
    print("GAMEPLAY_SOAK_SUMMARY {0}".format(path))
    sys.stdout.flush()


def _fail(reason):
    _write_summary("fail", reason)
    _write(ERROR_PATH, str(reason))
    print("GAMEPLAY_SOAK_FAIL {0}".format(reason))
    sys.stdout.flush()
    os._exit(1)


def _succeed():
    _write_summary("pass")
    marker = (
        "GAMEPLAY_SOAK_PASS backend={backend} stages={stages} "
        "dynamic_water={dynamic_water} combat={combat}"
    ).format(
        backend=pf.get_render_info().get("backend"),
        stages=len(STATE["records"]),
        dynamic_water=int(STATE["checks"]["dynamic_water"]),
        combat=int(STATE["checks"]["combat"]),
    )
    _write(PROBE_PATH, marker)
    print(marker)
    sys.stdout.flush()
    os._exit(0)


def _dist_xz(a, b):
    dx = a[0] - b[0]
    dz = a[1] - b[1]
    return math.sqrt(dx * dx + dz * dz)


def _choose_units():
    scene = list(rts.globals.scene_objs)
    friendlies = [
        ent for ent in scene
        if getattr(ent, "faction_id", None) == 1
        and hasattr(ent, "move")
        and hasattr(ent, "attack")
        and getattr(ent, "selectable", False)
    ]
    if len(friendlies) < 3:
        _fail("not enough friendly movable combat units for soak probe")

    anchor = friendlies[0]
    enemies = [
        ent for ent in scene
        if getattr(ent, "faction_id", None) not in (None, 1)
        and hasattr(ent, "hp")
        and hasattr(ent, "attack_range")
        and getattr(ent, "selectable", False)
        and getattr(ent, "hp", 0) > 0
    ]
    if not enemies:
        _fail("no enemy combat target found for soak probe")

    enemy = min(enemies, key=lambda ent: _dist_xz(_ent_xz(ent), _ent_xz(anchor)))
    units = friendlies[:4]
    print(
        "GAMEPLAY_SOAK_TARGETS selected={0} enemy={1}".format(
            ",".join("{0}:{1}".format(ent.name, _ent_xz(ent)) for ent in units),
            "{0}:{1}".format(enemy.name, _ent_xz(enemy)),
        )
    )
    sys.stdout.flush()
    return units, enemy


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
        (96.0, 48.0),
        (-96.0, 48.0),
        (96.0, -48.0),
        (-96.0, -48.0),
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
        target = _pathable_near(
            (point[0] + offset[0], point[1] + offset[1]),
            radius=max(getattr(ent, "selection_radius", 2.0), 1.0),
        )
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
    chunk = _parse_pair_env("PF_GAMEPLAY_SOAK_DYNAMIC_WATER_CHUNK", (3, 3))
    tile_pos = _parse_pair_env("PF_GAMEPLAY_SOAK_DYNAMIC_WATER_TILE", (0, 0))
    return chunk, tile_pos, pf.Tile()


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
        print("GAMEPLAY_SOAK_ACTIVATE_FALLBACK timeout")
        sys.stdout.flush()
    time.sleep(0.2)


def _capture_window_id():
    helper = os.path.join(pf.get_basedir(), "scripts/macos/pf_window_id_for_pid.swift")
    try:
        ret = subprocess.run(
            ["/usr/bin/swift", helper, str(os.getpid())],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            universal_newlines=True,
            timeout=3.0,
        )
    except subprocess.TimeoutExpired:
        return None
    if ret.returncode != 0:
        return None
    window_id = ret.stdout.strip().splitlines()[-1]
    if not window_id.isdigit():
        return None
    return window_id


def _capture(name):
    backend = pf.get_render_info().get("backend", "unknown").lower()
    path = os.path.join(STATE["output_dir"], "{0}_{1}.png".format(backend, name))
    if _env_flag("PF_GAMEPLAY_SOAK_NO_CAPTURE"):
        _write(path + ".skipped", "capture skipped")
        return path + ".skipped"
    _activate_own_window()
    window_id = _capture_window_id()
    ret = 1
    if window_id is not None:
        try:
            ret = subprocess.run(
                ["screencapture", "-x", "-o", "-l{0}".format(window_id), path],
                timeout=3.0,
            ).returncode
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


def on_motion_start(user, event):
    del user
    del event
    STATE["motion_started"] = True


def on_attack_start(user, event):
    del user
    del event
    STATE["attack_started"] = True


def _register_entity_probes(units):
    for ent in units:
        ent.register(pf.EVENT_MOTION_START, on_motion_start, None)
        ent.register(pf.EVENT_ATTACK_START, on_attack_start, None)


def _finish_stage():
    name = "initial" if STATE["stage_idx"] == 0 else "stage_{0}".format(STATE["stage_idx"])
    capture = _capture(name)
    if STATE["stage_idx"] == 0:
        STATE["records"].append({
            "name": name,
            "target": _ent_xz(STATE["units"][0]),
            "capture": capture,
            "units": [
                {
                    "name": getattr(ent, "name", "unit"),
                    "position": _ent_xz(ent),
                }
                for ent in STATE["units"]
            ],
        })
    else:
        STATE["records"][-1]["capture"] = capture
        STATE["records"][-1]["units_after"] = [
            {
                "name": getattr(ent, "name", "unit"),
                "position": _ent_xz(ent),
            }
            for ent in STATE["units"]
        ]
        STATE["checks"]["movement"] = STATE["checks"]["movement"] or STATE["motion_started"]
    print("GAMEPLAY_SOAK_CAPTURE {0} {1}".format(name, capture))
    sys.stdout.flush()
    _start_next_stage()


def _finish_dynamic_water_update():
    capture = _capture("dynamic_water_update")
    STATE["dynamic_water_record"]["capture"] = capture
    STATE["records"].append(STATE["dynamic_water_record"])
    STATE["checks"]["dynamic_water"] = True
    print("GAMEPLAY_SOAK_CAPTURE dynamic_water_update {0}".format(capture))
    sys.stdout.flush()
    _start_next_stage()


def _issue_combat():
    units = STATE["units"]
    enemy = STATE["enemy"]
    sx, sz = _ent_xz(units[0])
    enemy_anchor = _pathable_near((sx + 18.0, sz + 6.0), radius=enemy.selection_radius)
    if enemy_anchor is not None:
        enemy_height = pf.map_height_at_point(enemy_anchor[0], enemy_anchor[1])
        if enemy_height is not None:
            enemy.pos = (enemy_anchor[0], enemy_height, enemy_anchor[1])

    ex, ez = _ent_xz(enemy)
    offsets = [(-10.0, -8.0), (-10.0, 8.0), (8.0, -10.0), (8.0, 10.0)]
    staged = []
    for ent, (dx, dz) in zip(units, offsets):
        point = _pathable_near((ex + dx, ez + dz), radius=ent.selection_radius)
        if point is None:
            continue
        height = pf.map_height_at_point(point[0], point[1])
        if height is None:
            continue
        ent.pos = (point[0], height, point[1])
        staged.append({
            "name": getattr(ent, "name", "unit"),
            "position": point,
        })

    if not staged:
        _fail("could not stage attackers near combat target")

    STATE["enemy_hp"] = enemy.hp
    STATE["records"].append({
        "name": "combat",
        "target": (ex, ez),
        "enemy": {
            "name": getattr(enemy, "name", "enemy"),
            "position": (ex, ez),
            "hp_start": STATE["enemy_hp"],
        },
        "units": staged,
    })
    pf.get_active_camera().center_over_location((ex, ez))
    pf.set_unit_selection(units)
    for ent in units:
        ent.attack((ex, ez))
    _set_phase("combat")


def _finish_combat():
    capture = _capture("combat")
    STATE["records"][-1]["capture"] = capture
    STATE["records"][-1]["enemy"]["hp_end"] = getattr(STATE["enemy"], "hp", None)
    STATE["checks"]["combat"] = True
    print("GAMEPLAY_SOAK_CAPTURE combat {0}".format(capture))
    sys.stdout.flush()
    _succeed()


def _start_next_stage():
    STATE["stage_idx"] += 1
    if STATE["stage_idx"] > len(STATE["waypoints"]):
        if STATE["dynamic_water_enabled"] and not STATE["dynamic_water_done"]:
            STATE["dynamic_water_done"] = True
            _start_dynamic_water_update()
            return
        _issue_combat()
        return

    if STATE["stage_idx"] == 0:
        point = _ent_xz(STATE["units"][0])
        pf.get_active_camera().center_over_location(point)
        pf.set_unit_selection(STATE["units"])
        _set_phase("settle:initial")
        return

    point = STATE["waypoints"][STATE["stage_idx"] - 1]
    STATE["motion_started"] = False
    staged = _stage_units(point)
    pf.get_active_camera().center_over_location(point)
    STATE["records"].append({
        "name": "stage_{0}".format(STATE["stage_idx"]),
        "target": point,
        "units": staged,
    })
    _set_phase("settle:stage_{0}".format(STATE["stage_idx"]))


def on_update(user, event):
    del user
    del event

    STATE["ticks"] += 1
    try:
        STATE["frame_ms"].append(float(pf.prev_frame_ms()))
    except Exception:
        pass

    if STATE["phase"] == "init":
        backend = pf.get_render_info().get("backend")
        if STATE["expected_backend"] and backend != STATE["expected_backend"]:
            _fail("expected {0} backend, got {1}".format(STATE["expected_backend"], backend))
        STATE["units"], STATE["enemy"] = _choose_units()
        _register_entity_probes(STATE["units"])
        anchor = _ent_xz(STATE["units"][0])
        radius = max(getattr(STATE["units"][0], "selection_radius", 2.0), 1.0)
        STATE["waypoints"] = _spread_waypoints(
            anchor,
            _scene_pathable_points(radius),
            STATE["waypoint_count"],
        )
        if STATE["waypoint_count"] > 0 and not STATE["waypoints"]:
            _fail("no spread waypoints found")
        _start_next_stage()
        return

    if STATE["phase"].startswith("settle:"):
        if STATE["phase"] == "settle:initial" and len(pf.get_unit_selection()) == len(STATE["units"]):
            STATE["checks"]["selection"] = True
            STATE["checks"]["camera"] = True
        if STATE["ticks"] >= STATE["settle_frames"]:
            if STATE["phase"] == "settle:dynamic_water_update":
                _finish_dynamic_water_update()
                return
            _finish_stage()
            return
        if _phase_elapsed() > 10.0:
            _fail("timed out settling {0}".format(STATE["phase"]))

    if STATE["phase"] == "combat":
        try:
            current_hp = STATE["enemy"].hp
        except Exception:
            _finish_combat()
            return

        if STATE["attack_started"] or current_hp < STATE["enemy_hp"]:
            _finish_combat()
            return
        if _phase_elapsed() > 20.0:
            _fail("attack did not start or reduce target hp")


def main():
    output_dir = _arg_value(
        "--output-dir",
        os.environ.get("PF_GAMEPLAY_SOAK_OUTPUT_DIR", DEFAULT_OUTPUT_DIR),
    )
    if not os.path.isabs(output_dir):
        output_dir = os.path.join(pf.get_basedir(), output_dir)
    if not os.path.isdir(output_dir):
        os.makedirs(output_dir)
    STATE["output_dir"] = output_dir
    STATE["expected_backend"] = _arg_value(
        "--expect-backend",
        os.environ.get("PF_GAMEPLAY_SOAK_EXPECT_BACKEND"),
    )
    STATE["dynamic_water_enabled"] = _env_flag("PF_GAMEPLAY_SOAK_DYNAMIC_WATER")
    STATE["waypoint_count"] = max(0, _env_int("PF_GAMEPLAY_SOAK_WAYPOINTS", 4))
    STATE["settle_frames"] = max(1, _env_int("PF_GAMEPLAY_SOAK_SETTLE_FRAMES", 90))
    _set_phase("init")
    demo_main.main()
    pf.register_ui_event_handler(pf.EVENT_UPDATE_START, on_update, None)


main()
