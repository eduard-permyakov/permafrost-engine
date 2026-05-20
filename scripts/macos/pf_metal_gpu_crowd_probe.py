import json
import math
import os
import sys
import time

import pf

sys.path.insert(0, pf.get_basedir() + "/scripts")

import rts.globals
import rts.main as demo_main
from rts.units import knight


PROBE_PATH = "/tmp/pf_metal_gpu_crowd_probe.txt"
ERROR_PATH = "/tmp/pf_metal_gpu_crowd_probe_error.txt"
DEFAULT_OUTPUT_DIR = "visual_parity_captures/gpu-crowd-probe"

STATE = {
    "phase": "init",
    "ticks": 0,
    "output_dir": None,
    "expected_backend": "METAL",
    "movement_mode": "gpu",
    "unit_count": 64,
    "settle_frames": 240,
    "units": [],
    "start_slots": [],
    "start_center": None,
    "target": None,
    "motion_start_count": 0,
    "motion_end_count": 0,
    "frame_ms": [],
    "phase_started_at": None,
}


def _arg_value(name, default=None):
    if name not in sys.argv:
        return default
    idx = sys.argv.index(name)
    if idx + 1 >= len(sys.argv):
        return default
    return sys.argv[idx + 1]


def _arg_int(name, default):
    value = _arg_value(name)
    if value is None:
        value = os.environ.get(name.strip("-").upper().replace("-", "_"))
    try:
        return int(value) if value is not None else default
    except ValueError:
        return default


def _write(path, payload):
    with open(path, "w") as outfile:
        outfile.write(payload + "\n")


def _summary_path(backend):
    return os.path.join(STATE["output_dir"], "summary_{0}.json".format(backend.lower()))


def _set_phase(name):
    STATE["phase"] = name
    STATE["ticks"] = 0
    STATE["phase_started_at"] = time.monotonic()
    print("GPU_CROWD_PHASE {0}".format(name))
    sys.stdout.flush()


def _phase_elapsed():
    return time.monotonic() - STATE["phase_started_at"]


def _ent_xz(ent):
    pos = ent.pos
    return (pos[0], pos[2])


def _dist(a, b):
    dx = a[0] - b[0]
    dz = a[1] - b[1]
    return math.sqrt(dx * dx + dz * dz)


def _metrics(samples):
    if not samples:
        return {"count": 0, "avg_ms": None, "min_ms": None, "max_ms": None}
    return {
        "count": len(samples),
        "avg_ms": sum(samples) / float(len(samples)),
        "min_ms": min(samples),
        "max_ms": max(samples),
    }


def _movement_stats():
    units = STATE["units"]
    starts = STATE["start_slots"]
    target = STATE["target"]
    if not units or not starts or target is None:
        return {
            "moved_count": 0,
            "avg_displacement": 0.0,
            "avg_distance_to_target": None,
            "avg_progress": 0.0,
        }

    total_disp = 0.0
    total_target = 0.0
    total_progress = 0.0
    moved_count = 0

    sx = sum(point[0] for point in starts) / float(len(starts))
    sz = sum(point[1] for point in starts) / float(len(starts))
    tx, tz = target
    vx = tx - sx
    vz = tz - sz
    vlen2 = max(vx * vx + vz * vz, 1.0)

    for ent, start in zip(units, starts):
        pos = _ent_xz(ent)
        disp = _dist(pos, start)
        total_disp += disp
        total_target += _dist(pos, target)
        total_progress += ((pos[0] - start[0]) * vx + (pos[1] - start[1]) * vz) / math.sqrt(vlen2)
        if disp > 4.0:
            moved_count += 1

    n = float(len(units))
    return {
        "moved_count": moved_count,
        "avg_displacement": total_disp / n,
        "avg_distance_to_target": total_target / n,
        "avg_progress": total_progress / n,
    }


def _write_summary(status, reason=None):
    backend = pf.get_render_info().get("backend", "unknown")
    payload = {
        "status": status,
        "reason": reason,
        "backend": pf.get_render_info(),
        "expected_backend": STATE["expected_backend"],
        "movement_mode": STATE["movement_mode"],
        "gpu_movement_enabled": bool(pf.settings_get("pf.game.movement_use_gpu")),
        "unit_count": len(STATE["units"]),
        "start_center": STATE["start_center"],
        "target": STATE["target"],
        "motion_start_count": STATE["motion_start_count"],
        "motion_end_count": STATE["motion_end_count"],
        "frame_ms": _metrics(STATE["frame_ms"]),
        "movement": _movement_stats(),
        "ticks": STATE["ticks"],
    }
    with open(_summary_path(backend), "w") as outfile:
        json.dump(payload, outfile, indent=2, sort_keys=True)
        outfile.write("\n")
    print("GPU_CROWD_SUMMARY {0}".format(_summary_path(backend)))
    sys.stdout.flush()


def _fail(reason):
    _write_summary("fail", reason)
    _write(ERROR_PATH, str(reason))
    print("GPU_CROWD_FAIL {0}".format(reason))
    sys.stdout.flush()
    os._exit(1)


def _succeed():
    stats = _movement_stats()
    _write_summary("pass")
    marker = (
        "GPU_CROWD_PASS backend={backend} movement_mode={mode} gpu_movement={gpu} "
        "units={units} moved={moved} avg_progress={progress:.2f} avg_frame_ms={frame:.2f}"
    ).format(
        backend=pf.get_render_info().get("backend"),
        mode=STATE["movement_mode"],
        gpu=int(bool(pf.settings_get("pf.game.movement_use_gpu"))),
        units=len(STATE["units"]),
        moved=stats["moved_count"],
        progress=stats["avg_progress"],
        frame=_metrics(STATE["frame_ms"])["avg_ms"] or 0.0,
    )
    _write(PROBE_PATH, marker)
    print(marker)
    sys.stdout.flush()
    os._exit(0)


def _pathable_near(point, radius=3.25):
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
        candidate = (point[0] + dx, point[1] + dz)
        if pf.map_height_at_point(candidate[0], candidate[1]) is None:
            continue
        found = pf.map_nearest_pathable(candidate, radius=radius)
        if found is not None:
            return found
    return None


def _scene_pathable_points(radius):
    points = []
    for ent in list(rts.globals.scene_objs):
        try:
            point = _pathable_near(_ent_xz(ent), radius)
        except Exception:
            continue
        if point is None:
            continue
        if all(_dist(point, seen) > 32.0 for seen in points):
            points.append(point)
    return points


def _grid_slots(center, count, radius=3.25):
    slots = []
    spacing = 7.5
    extent = int(math.ceil(math.sqrt(count))) + 4
    offsets = []
    for row in range(-extent, extent + 1):
        for col in range(-extent, extent + 1):
            offsets.append((col * spacing, row * spacing))
    offsets.sort(key=lambda item: item[0] * item[0] + item[1] * item[1])

    for dx, dz in offsets:
        candidate = (center[0] + dx, center[1] + dz)
        if pf.map_height_at_point(candidate[0], candidate[1]) is None:
            continue
        found = pf.map_nearest_pathable(candidate, radius=radius)
        if found is None:
            continue
        if all(_dist(found, seen) > radius * 1.5 for seen in slots):
            slots.append(found)
            if len(slots) >= count:
                break
    return slots


def _choose_start_and_target():
    points = _scene_pathable_points(3.25)
    if len(points) < 2:
        _fail("not enough pathable scene points for crowd probe")

    preferred_pairs = []
    fallback_pairs = []
    for start in points:
        slots = _grid_slots(start, STATE["unit_count"])
        if len(slots) < STATE["unit_count"]:
            continue
        for target in points:
            distance = _dist(start, target)
            if distance < 96.0:
                continue
            item = (distance, start, target, slots)
            fallback_pairs.append(item)
            if distance <= 260.0:
                preferred_pairs.append(item)
    pairs = preferred_pairs or fallback_pairs
    if not pairs:
        _fail("could not find dense start grid plus distant target")

    pairs.sort(key=lambda item: abs(item[0] - 180.0))
    return pairs[0][1], pairs[0][2], pairs[0][3]


def _make_units(slots):
    created = []
    for idx, point in enumerate(slots):
        ent = knight.Knight("assets/models/knight", "Knight.pfobj", "gpu_crowd_knight_{0}".format(idx))
        ent.faction_id = 1
        ent.selectable = True
        ent.selection_radius = 3.25
        ent.speed = 20.0
        height = pf.map_height_at_point(point[0], point[1])
        if height is None:
            height = 0.0
        ent.pos = (point[0], height, point[1])
        rts.globals.scene_objs.append(ent)
        created.append(ent)
    return created


def on_motion_start(user, event):
    del user
    del event
    STATE["motion_start_count"] += 1


def on_motion_end(user, event):
    del user
    del event
    STATE["motion_end_count"] += 1


def _register_motion_events(units):
    for ent in units:
        ent.register(pf.EVENT_MOTION_START, on_motion_start, None)
        ent.register(pf.EVENT_MOTION_END, on_motion_end, None)


def _configure_movement_mode():
    mode = STATE["movement_mode"]
    if mode == "gpu":
        pf.settings_set("pf.game.movement_use_gpu", True, persist=False)
        if not bool(pf.settings_get("pf.game.movement_use_gpu")):
            _fail("GPU movement request was rejected by the active backend")
    elif mode == "cpu":
        pf.settings_set("pf.game.movement_use_gpu", False, persist=False)
        if bool(pf.settings_get("pf.game.movement_use_gpu")):
            _fail("CPU movement request did not disable GPU movement")
    else:
        _fail("unknown movement mode {0}".format(mode))


def _issue_formation_move():
    start, target, slots = _choose_start_and_target()
    STATE["start_center"] = start
    STATE["target"] = target
    STATE["start_slots"] = slots
    STATE["units"] = _make_units(slots)
    _register_motion_events(STATE["units"])
    pf.set_unit_selection(STATE["units"])
    pf.get_active_camera().center_over_location(start)

    orientation = (target[0] - start[0], target[1] - start[1])
    length = math.sqrt(orientation[0] * orientation[0] + orientation[1] * orientation[1])
    if length > 0.0:
        orientation = (orientation[0] / length, orientation[1] / length)
    else:
        orientation = (1.0, 0.0)

    pf.move_in_formation(STATE["units"], pf.FORMATION_RANK, target, orientation)
    _set_phase("moving")


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
        _configure_movement_mode()
        _issue_formation_move()
        return

    if STATE["phase"] == "moving":
        if STATE["ticks"] < STATE["settle_frames"]:
            if _phase_elapsed() > 20.0:
                _fail("timed out waiting for crowd movement")
            return

        stats = _movement_stats()
        required = max(8, int(len(STATE["units"]) * 0.75))
        if stats["moved_count"] < required:
            _fail("insufficient moved units required={0} actual={1}".format(required, stats["moved_count"]))
        if stats["avg_displacement"] < 12.0:
            _fail("average formation displacement too small: {0:.2f}".format(stats["avg_displacement"]))
        if stats["avg_progress"] < 3.0:
            _fail("average formation progress too small: {0:.2f}".format(stats["avg_progress"]))
        _succeed()


def main():
    output_dir = _arg_value("--output-dir", os.environ.get("PF_GPU_CROWD_OUTPUT_DIR", DEFAULT_OUTPUT_DIR))
    if not os.path.isabs(output_dir):
        output_dir = os.path.join(pf.get_basedir(), output_dir)
    if not os.path.isdir(output_dir):
        os.makedirs(output_dir)
    STATE["output_dir"] = output_dir
    STATE["expected_backend"] = _arg_value("--expect-backend", os.environ.get("PF_GPU_CROWD_EXPECT_BACKEND", "METAL"))
    STATE["movement_mode"] = _arg_value("--movement-mode", os.environ.get("PF_GPU_CROWD_MOVEMENT_MODE", "gpu"))
    STATE["unit_count"] = max(8, _arg_int("--unit-count", STATE["unit_count"]))
    STATE["settle_frames"] = max(30, _arg_int("--settle-frames", STATE["settle_frames"]))

    for path in (PROBE_PATH, ERROR_PATH):
        try:
            os.unlink(path)
        except OSError:
            pass

    _set_phase("init")
    demo_main.main()
    pf.register_ui_event_handler(pf.EVENT_UPDATE_START, on_update, None)


main()
