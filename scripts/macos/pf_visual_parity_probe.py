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
import rts.time_of_day as time_of_day


ERROR_PATH = "/tmp/pf_visual_parity_probe_error.txt"
DEFAULT_OUTPUT_DIR = "visual_parity_captures"

STATE = {
    "phase": "init",
    "phase_started_at": None,
    "scene_idx": 0,
    "scenes": [],
    "samples": [],
    "records": [],
    "output_dir": None,
    "expected_backend": None,
    "water_stage": None,
    "capture_camera": None,
    "skybox_camera": None,
    "frozen_anim_count": 0,
    "splat_pairs": [],
}


def _arg_value(name, default=None):
    if name not in sys.argv:
        return default
    idx = sys.argv.index(name)
    if idx + 1 >= len(sys.argv):
        return default
    return sys.argv[idx + 1]


def _write(path, payload):
    with open(path, "w") as outfile:
        outfile.write(payload + "\n")


def _fail(reason):
    _write(ERROR_PATH, str(reason))
    print("VISUAL_PARITY_FAIL {0}".format(reason))
    sys.stdout.flush()
    os._exit(1)


def _set_phase(name):
    STATE["phase"] = name
    STATE["phase_started_at"] = time.monotonic()
    print("VISUAL_PARITY_PHASE {0}".format(name))
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


def _choose_friendlies():
    return [
        ent for ent in list(rts.globals.scene_objs)
        if getattr(ent, "faction_id", None) == 1
        and getattr(ent, "selectable", False)
    ]


def _choose_movable_friendlies():
    return [
        ent for ent in _choose_friendlies()
        if hasattr(ent, "move")
    ]


def _choose_enemy(anchor):
    enemies = [
        ent for ent in list(rts.globals.scene_objs)
        if getattr(ent, "faction_id", None) not in (None, 1)
        and getattr(ent, "selectable", False)
        and getattr(ent, "hp", 1) > 0
    ]
    if not enemies:
        return None
    return min(enemies, key=lambda ent: _dist_xz(_ent_xz(ent), _ent_xz(anchor)))


def _choose_rock(anchor):
    rocks = []
    for ent in list(rts.globals.scene_objs):
        name = getattr(ent, "name", "")
        if "rock" not in name.lower():
            continue
        try:
            pos = _ent_xz(ent)
        except Exception:
            continue
        rocks.append((ent, pos))
    if not rocks:
        return None
    return min(rocks, key=lambda item: _dist_xz(item[1], anchor))[0]


def _scene_anchor_points():
    points = []
    for ent in list(rts.globals.scene_objs):
        try:
            pos = ent.pos
        except Exception:
            continue
        points.append((pos[0], pos[2]))
        if len(points) >= 64:
            break
    return points


def _find_water_point():
    for anchor in _scene_anchor_points():
        water = pf.map_nearest_pathable_water(anchor)
        if water is None:
            continue
        if pf.map_pos_over_water(water[0], water[1]):
            return water
    return None


def _pathable_near(point, radius=2.0):
    offsets = [
        (0.0, 0.0),
        (8.0, 0.0),
        (-8.0, 0.0),
        (0.0, 8.0),
        (0.0, -8.0),
        (14.0, 8.0),
        (-14.0, 8.0),
        (14.0, -8.0),
        (-14.0, -8.0),
        (24.0, 0.0),
        (-24.0, 0.0),
        (0.0, 24.0),
        (0.0, -24.0),
    ]
    for dx, dz in offsets:
        found = pf.map_nearest_pathable((point[0] + dx, point[1] + dz), radius=radius)
        if found is not None:
            return found
    return None


def _stage_water_units(scene):
    selected = scene.get("selection", [])
    if not selected:
        return None

    radius = max(getattr(selected[0], "selection_radius", 2.0), 1.0)
    move_target = _pathable_near(scene["target"], radius=radius)
    if move_target is None:
        return None

    offsets = [(-28.0, -12.0), (-28.0, 12.0), (-18.0, -24.0), (-18.0, 24.0)]
    staged = []
    for ent, (dx, dz) in zip(selected, offsets):
        pos = _pathable_near((move_target[0] + dx, move_target[1] + dz),
                            radius=max(getattr(ent, "selection_radius", 2.0), 1.0))
        if pos is None:
            continue
        height = pf.map_height_at_point(pos[0], pos[1])
        if height is None:
            continue
        ent.pos = (pos[0], height, pos[1])
        ent.move(move_target)
        staged.append({
            "name": getattr(ent, "name", "unit"),
            "start": pos,
        })

    if not staged:
        return None
    return {
        "move_target": move_target,
        "units": staged,
    }


def _build_scenes():
    friendlies = _choose_friendlies()
    if not friendlies:
        _fail("no friendly selectable units found")

    anchor = friendlies[0]
    enemy = _choose_enemy(anchor)
    water = _find_water_point()
    water_units = _choose_movable_friendlies()[:4]
    rock = _choose_rock(water if water is not None else _ent_xz(anchor))
    scenes = [
        {
            "name": "overview",
            "target": _ent_xz(anchor),
            "selection": friendlies[:4],
        }
    ]
    if water is not None:
        scenes.append({
            "name": "water",
            "target": water,
            "selection": water_units,
            "stage_water_units": True,
        })
    if rock is not None:
        scenes.append({
            "name": "rocks",
            "target": _ent_xz(rock),
            "selection": [],
        })
    if enemy is not None:
        scenes.append({
            "name": "combat",
            "target": _ent_xz(enemy),
            "selection": [enemy],
        })
    if os.environ.get("PF_VISUAL_PARITY_INCLUDE_SKYBOX") == "1":
        scenes.append({
            "name": "skybox",
            "target": _ent_xz(anchor),
            "selection": [],
            "camera": "skybox",
        })
    scene_filter = os.environ.get("PF_VISUAL_PARITY_SCENES")
    if scene_filter:
        wanted = set(name.strip() for name in scene_filter.split(",") if name.strip())
        scenes = [scene for scene in scenes if scene["name"] in wanted]
        if not scenes:
            _fail("scene filter matched no scenes: {0}".format(scene_filter))
    return scenes


def _freeze_anim_poses():
    count = 0
    for ent in list(rts.globals.scene_objs):
        get_anim = getattr(ent, "get_anim", None)
        play_anim = getattr(ent, "play_anim", None)
        if get_anim is None or play_anim is None:
            continue
        try:
            play_anim(get_anim())
        except Exception:
            continue
        count += 1
    STATE["frozen_anim_count"] = count
    print("VISUAL_PARITY_ANIM_FREEZE count={0}".format(count))
    sys.stdout.flush()


def _apply_diagnostic_settings():
    if os.environ.get("PF_VISUAL_PARITY_DISABLE_VSYNC") == "1":
        pf.settings_set("pf.video.vsync", False, persist=False)
    if os.environ.get("PF_VISUAL_PARITY_DISABLE_SHADOWS") == "1":
        pf.settings_set("pf.video.shadows_enabled", False, persist=False)


def _apply_splat_settings():
    spec = os.environ.get("PF_VISUAL_PARITY_SPLAT_PAIRS", "").strip()
    if not spec:
        return
    pairs = []
    for token in spec.split(";"):
        token = token.strip()
        if not token:
            continue
        parts = token.split(":")
        if len(parts) != 2:
            _fail("invalid PF_VISUAL_PARITY_SPLAT_PAIRS token: {0}".format(token))
        try:
            base = int(parts[0])
            accent = int(parts[1])
        except ValueError:
            _fail("invalid splat material index: {0}".format(token))
        pf.map_add_splat(base, accent)
        pairs.append({"base": base, "accent": accent})
    STATE["splat_pairs"] = pairs
    print("VISUAL_PARITY_SPLATS {0}".format(pairs))
    sys.stdout.flush()


def _metrics_summary(samples):
    if not samples:
        return {"count": 0, "avg_ms": None, "min_ms": None, "max_ms": None}
    return {
        "count": len(samples),
        "avg_ms": sum(samples) / float(len(samples)),
        "min_ms": min(samples),
        "max_ms": max(samples),
    }


def _ensure_capture_camera():
    if STATE["capture_camera"] is not None:
        return STATE["capture_camera"]

    STATE["capture_camera"] = pf.get_active_camera()
    return STATE["capture_camera"]


def _place_camera_at_target(target):
    cam = _ensure_capture_camera()
    cam.center_over_location(target)


def _place_skybox_camera(target):
    if STATE["skybox_camera"] is None:
        STATE["skybox_camera"] = pf.Camera(
            mode=pf.CAM_MODE_FPS,
            position=(target[0], 130.0, target[1]),
            pitch=5.0,
            yaw=135.0,
        )
    pf.set_active_camera(STATE["skybox_camera"])


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
        print("VISUAL_PARITY_ACTIVATE_FALLBACK timeout")
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
        print("VISUAL_PARITY_WINDOW_ID_FALLBACK timeout")
        sys.stdout.flush()
        return None
    if ret.returncode != 0:
        print("VISUAL_PARITY_WINDOW_ID_FALLBACK {0}".format(ret.stderr.strip()))
        sys.stdout.flush()
        return None
    window_id = ret.stdout.strip().splitlines()[-1]
    if not window_id.isdigit():
        print("VISUAL_PARITY_WINDOW_ID_FALLBACK invalid:{0}".format(window_id))
        sys.stdout.flush()
        return None
    return window_id


def _capture(scene):
    backend = pf.get_render_info().get("backend")
    filename = "{0}_{1}.png".format(backend.lower(), scene["name"])
    path = os.path.join(STATE["output_dir"], filename)
    ret = 1
    window_id = None
    last_error = ""
    for _ in range(5):
        if os.environ.get("PF_VISUAL_PARITY_ACTIVATE_WINDOW") == "1":
            _activate_own_window()
        window_id = _capture_window_id()
        if window_id is None:
            last_error = "no window id"
            time.sleep(0.15)
            continue
        try:
            capture = subprocess.run(
                ["screencapture", "-x", "-o", "-l{0}".format(window_id), path],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                universal_newlines=True,
                timeout=3.0,
            )
            ret = capture.returncode
            last_error = capture.stderr.strip()
            if ret == 0:
                break
        except subprocess.TimeoutExpired:
            ret = 1
            last_error = "timeout"
        time.sleep(0.15)
    if ret != 0 and os.environ.get("PF_VISUAL_PARITY_FULLSCREEN_FALLBACK") == "1":
        print("VISUAL_PARITY_CAPTURE_FALLBACK fullscreen {0}".format(scene["name"]))
        sys.stdout.flush()
        try:
            ret = subprocess.run(
                ["screencapture", "-x", "-o", path],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=3.0,
            ).returncode
        except subprocess.TimeoutExpired:
            ret = 1
    if ret != 0:
        _fail("screencapture failed for {0} window_id={1} stderr={2}".format(
            scene["name"],
            window_id,
            last_error,
        ))
    return path


def _write_summary():
    path = os.path.join(STATE["output_dir"], "summary_{0}.json".format(
        pf.get_render_info().get("backend", "unknown").lower()
    ))
    payload = {
        "backend": pf.get_render_info(),
        "records": STATE["records"],
        "frozen_anim_count": STATE["frozen_anim_count"],
        "splat_pairs": STATE["splat_pairs"],
        "lighting": time_of_day.current_state(),
    }
    with open(path, "w") as outfile:
        json.dump(payload, outfile, indent=2, sort_keys=True)
        outfile.write("\n")
    print("VISUAL_PARITY_SUMMARY {0}".format(path))
    sys.stdout.flush()


def _resume_sim_if_paused():
    try:
        if pf.get_simstate() == pf.G_PAUSED_UI_RUNNING:
            pf.set_simstate(pf.G_RUNNING)
    except Exception:
        pass


def _start_scene():
    if STATE["scene_idx"] >= len(STATE["scenes"]):
        _write_summary()
        print("VISUAL_PARITY_PASS backend={0} scenes={1} output_dir={2}".format(
            pf.get_render_info().get("backend"),
            len(STATE["records"]),
            STATE["output_dir"],
        ))
        sys.stdout.flush()
        if os.environ.get("PF_METAL_CAPTURE_PATH"):
            delay = float(os.environ.get("PF_METAL_CAPTURE_EXIT_DELAY", "8.0"))
            time.sleep(delay)
        _resume_sim_if_paused()
        os._exit(0)

    scene = STATE["scenes"][STATE["scene_idx"]]
    pf.set_unit_selection(scene["selection"])
    if scene.get("camera") == "skybox":
        _place_skybox_camera(scene["target"])
    else:
        _place_camera_at_target(scene["target"])
    STATE["samples"] = []
    STATE["water_stage"] = None
    if scene.get("stage_water_units"):
        STATE["water_stage"] = _stage_water_units(scene)
        pf.set_unit_selection(scene["selection"])
        _set_phase("stage_water_units")
        return
    _set_phase("settle:{0}".format(scene["name"]))


def _finish_scene():
    scene = STATE["scenes"][STATE["scene_idx"]]
    capture_path = _capture(scene)
    cam = pf.get_active_camera()
    record = {
        "name": scene["name"],
        "target": scene["target"],
        "camera_position": cam.position,
        "camera_direction": cam.direction,
        "frame_ms": _metrics_summary(STATE["samples"]),
        "capture": capture_path,
    }
    if STATE["water_stage"] is not None:
        record["water_stage"] = STATE["water_stage"]
    STATE["records"].append(record)
    print("VISUAL_PARITY_CAPTURE {0} {1}".format(scene["name"], capture_path))
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
        _apply_diagnostic_settings()
        _apply_splat_settings()
        pf.set_simstate(pf.G_PAUSED_UI_RUNNING)
        _freeze_anim_poses()
        STATE["scenes"] = _build_scenes()
        _start_scene()
        return

    if STATE["phase"].startswith("settle:"):
        STATE["samples"].append(float(pf.prev_frame_ms()))
        if len(STATE["samples"]) >= 90:
            _finish_scene()
            return
        if _phase_elapsed() > 8.0:
            _fail("timed out settling {0}".format(STATE["phase"]))

    if STATE["phase"] == "stage_water_units":
        if _phase_elapsed() >= 1.2:
            scene = STATE["scenes"][STATE["scene_idx"]]
            _place_camera_at_target(scene["target"])
            STATE["samples"] = []
            _set_phase("settle:{0}".format(scene["name"]))
            return
        if _phase_elapsed() > 4.0:
            _fail("timed out staging water units")


def main():
    output_dir = _arg_value("--output-dir", os.environ.get("PF_VISUAL_PARITY_OUTPUT_DIR", DEFAULT_OUTPUT_DIR))
    if not os.path.isabs(output_dir):
        output_dir = os.path.join(pf.get_basedir(), output_dir)
    if not os.path.isdir(output_dir):
        os.makedirs(output_dir)
    STATE["output_dir"] = output_dir
    STATE["expected_backend"] = _arg_value("--expect-backend", os.environ.get("PF_VISUAL_PARITY_EXPECT_BACKEND"))
    _set_phase("init")
    demo_main.main()
    pf.register_ui_event_handler(pf.EVENT_UPDATE_START, on_update, None)


main()
