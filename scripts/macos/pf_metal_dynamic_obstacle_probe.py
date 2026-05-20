import json
import math
import os
import sys
import time

import pf

sys.path.insert(0, pf.get_basedir() + "/scripts")

import rts.globals
import rts.main as demo_main


PROBE_PATH = "/tmp/pf_metal_dynamic_obstacle_probe.txt"
ERROR_PATH = "/tmp/pf_metal_dynamic_obstacle_probe_error.txt"
DEFAULT_OUTPUT_DIR = "visual_parity_captures/dynamic-obstacle-probe"


class ProbeGround1x1(pf.MovableEntity):
    formation_priority = 0


class ProbeGround3x3(pf.MovableEntity):
    formation_priority = 1


class ProbeGround5x5(pf.MovableEntity):
    formation_priority = 2


class ProbeGround7x7(pf.MovableEntity):
    formation_priority = 3


class ProbeBlocker(pf.BuildableEntity):
    pass


UNIT_SPECS = [
    (ProbeGround1x1, 3.25, 3),
    (ProbeGround3x3, 5.25, 3),
    (ProbeGround5x5, 10.25, 3),
    (ProbeGround7x7, 15.25, 3),
]


STATE = {
    "phase": "init",
    "ticks": 0,
    "output_dir": None,
    "expected_backend": "METAL",
    "movement_mode": "gpu",
    "phase_started_at": None,
    "phase_log": [],
    "frame_ms": [],
    "events": {},
    "units": [],
    "unit_specs": [],
    "start_slots": [],
    "post_blocker_start_slots": [],
    "target": None,
    "blocker": None,
    "blocker_position": None,
    "checks": {
        "move_started": False,
        "pre_blocker_progress": False,
        "blocker_inserted": False,
        "blocker_affects_pathing": False,
        "continued_progress": False,
        "clearance_preserved": False,
    },
    "pathing": {},
    "movement": {},
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


def _summary_path(backend):
    return os.path.join(STATE["output_dir"], "summary_{0}.json".format(backend.lower()))


def _set_phase(name):
    STATE["phase"] = name
    STATE["ticks"] = 0
    STATE["phase_started_at"] = time.monotonic()
    STATE["phase_log"].append(name)
    print("DYNAMIC_OBSTACLE_PHASE {0}".format(name))
    sys.stdout.flush()


def _phase_elapsed():
    return time.monotonic() - STATE["phase_started_at"]


def _dist(a, b):
    dx = a[0] - b[0]
    dz = a[1] - b[1]
    return math.sqrt(dx * dx + dz * dz)


def _ent_xz(ent):
    pos = ent.pos
    return (pos[0], pos[2])


def _metrics(samples):
    if not samples:
        return {"count": 0, "avg_ms": None, "min_ms": None, "max_ms": None}
    return {
        "count": len(samples),
        "avg_ms": sum(samples) / float(len(samples)),
        "min_ms": min(samples),
        "max_ms": max(samples),
    }


def _event_count(name):
    return STATE["events"].get(name, 0)


def _record(name):
    STATE["events"][name] = _event_count(name) + 1


def _on_event(name):
    def handler(user, event):
        del user
        del event
        _record(name)
    return handler


def _write_summary(status, reason=None):
    backend = pf.get_render_info().get("backend", "unknown")
    payload = {
        "status": status,
        "reason": reason,
        "backend": pf.get_render_info(),
        "expected_backend": STATE["expected_backend"],
        "movement_mode": STATE["movement_mode"],
        "gpu_movement_enabled": bool(pf.settings_get("pf.game.movement_use_gpu")),
        "checks": STATE["checks"],
        "events": STATE["events"],
        "phase_log": STATE["phase_log"],
        "frame_ms": _metrics(STATE["frame_ms"]),
        "pathing": STATE["pathing"],
        "movement": STATE["movement"],
    }
    with open(_summary_path(backend), "w") as outfile:
        json.dump(payload, outfile, indent=2, sort_keys=True)
        outfile.write("\n")
    print("DYNAMIC_OBSTACLE_SUMMARY {0}".format(_summary_path(backend)))
    sys.stdout.flush()


def _fail(reason):
    _write_summary("fail", reason)
    _write(ERROR_PATH, str(reason))
    print("DYNAMIC_OBSTACLE_FAIL {0}".format(reason))
    sys.stdout.flush()
    os._exit(1)


def _succeed():
    _write_summary("pass")
    marker = (
        "DYNAMIC_OBSTACLE_PASS backend={backend} mode={mode} started={started} "
        "blocker={blocker} pathing={pathing} progress={progress} clearance={clearance}"
    ).format(
        backend=pf.get_render_info().get("backend"),
        mode=STATE["movement_mode"],
        started=int(STATE["checks"]["move_started"]),
        blocker=int(STATE["checks"]["blocker_inserted"]),
        pathing=int(STATE["checks"]["blocker_affects_pathing"]),
        progress=int(STATE["checks"]["continued_progress"]),
        clearance=int(STATE["checks"]["clearance_preserved"]),
    )
    _write(PROBE_PATH, marker)
    print(marker)
    sys.stdout.flush()
    os._exit(0)


def _inside_map(point):
    try:
        return pf.map_height_at_point(point[0], point[1]) is not None
    except Exception:
        return False


def _pathable_near(point, radius=3.25):
    offsets = [(0.0, 0.0)]
    for ring in range(1, 9):
        dist = 24.0 * ring
        offsets.extend([
            (dist, 0.0),
            (-dist, 0.0),
            (0.0, dist),
            (0.0, -dist),
            (dist, dist),
            (-dist, dist),
            (dist, -dist),
            (-dist, -dist),
        ])
    for dx, dz in offsets:
        candidate = (point[0] + dx, point[1] + dz)
        if not _inside_map(candidate):
            continue
        found = pf.map_nearest_pathable(candidate, radius=radius)
        if found is not None:
            return found
    return None


def _scene_pathable_points(radius):
    points = []
    for ent in list(rts.globals.scene_objs):
        if not hasattr(ent, "pos"):
            continue
        try:
            point = _pathable_near(_ent_xz(ent), radius)
        except Exception:
            continue
        if point is None:
            continue
        if all(_dist(point, seen) > 48.0 for seen in points):
            points.append(point)
    points.append(_pathable_near((0.0, 0.0), radius))
    return [point for point in points if point is not None]


def _orientation(src, dst):
    vec = (dst[0] - src[0], dst[1] - src[1])
    length = math.sqrt(vec[0] * vec[0] + vec[1] * vec[1])
    if length <= 0.01:
        return (1.0, 0.0)
    return (vec[0] / length, vec[1] / length)


def _pathable_for_radius(point, radius):
    if not _inside_map(point):
        return None
    return pf.map_nearest_pathable(point, radius=radius)


def _line_point(start, target, fraction):
    return (
        start[0] + (target[0] - start[0]) * fraction,
        start[1] + (target[1] - start[1]) * fraction,
    )


def _find_slots(center, specs):
    max_radius = max(radius for _, radius in specs)
    spacing = (max_radius * 2.0) + 8.0
    slots = []
    offsets = []
    extent = 4
    for row in range(-extent, extent + 1):
        for col in range(-extent, extent + 1):
            offsets.append((col * spacing, row * spacing))
    offsets.sort(key=lambda item: item[0] * item[0] + item[1] * item[1])

    for _, radius in specs:
        chosen = None
        for dx, dz in offsets:
            candidate = (center[0] + dx, center[1] + dz)
            found = _pathable_for_radius(candidate, radius)
            if found is None:
                continue
            if all(_dist(found, item["point"]) > radius + item["radius"] + 4.0 for item in slots):
                chosen = found
                break
        if chosen is None:
            return None
        slots.append({"point": chosen, "radius": radius})
    return slots


def _local_targets(start, radius):
    offsets = []
    for distance in (96.0, 128.0, 160.0, 192.0):
        offsets.extend([
            (distance, 0.0),
            (-distance, 0.0),
            (0.0, distance),
            (0.0, -distance),
            (distance, distance * 0.5),
            (-distance, distance * 0.5),
            (distance, -distance * 0.5),
            (-distance, -distance * 0.5),
        ])

    targets = []
    for dx, dz in offsets:
        found = _pathable_near((start[0] + dx, start[1] + dz), radius)
        if found is None:
            continue
        distance = _dist(start, found)
        if distance < 128.0 or distance > 280.0:
            continue
        if all(_dist(found, seen) > 48.0 for seen in targets):
            targets.append(found)
    return targets


def _unit_specs():
    specs = []
    for cls, radius, count in UNIT_SPECS:
        for _ in range(count):
            specs.append((cls, radius))
    return specs


def _find_scenario():
    specs = _unit_specs()
    candidates = _scene_pathable_points(15.25)
    for start in candidates:
        slots = _find_slots(start, specs)
        if slots is None:
            continue
        targets = []
        for target in _local_targets(start, 15.25):
            blocker = _pathable_near(_line_point(start, target, 0.58), 15.25)
            if blocker is None:
                continue
            if _dist(start, blocker) < 80.0 or _dist(target, blocker) < 64.0:
                continue
            targets.append((target, blocker))
        if targets:
            targets.sort(key=lambda item: _dist(start, item[0]), reverse=True)
            return start, targets[0][0], targets[0][1], specs, slots
    _fail("could not find dynamic obstacle scenario")


def _place(ent, point, radius=3.25, scale=None, selectable=False):
    height = pf.map_height_at_point(point[0], point[1])
    if height is None:
        height = 0.0
    ent.pos = (float(point[0]), float(height), float(point[1]))
    ent.faction_id = 1
    ent.selection_radius = float(radius)
    try:
        ent.selectable = selectable
    except AttributeError:
        pass
    if scale is not None:
        ent.scale = scale
    rts.globals.scene_objs.append(ent)
    return ent


def _make_units(specs, slots):
    units = []
    for idx, ((cls, radius), slot) in enumerate(zip(specs, slots)):
        ent = cls("assets/models/cart", "cart.pfobj", "dynamic_probe_unit_{0}".format(idx))
        _place(ent, slot["point"], radius=radius, scale=(1.0, 1.0, 1.0), selectable=True)
        ent.speed = 36.0
        ent.preferred_formation = pf.FORMATION_RANK
        ent.register(pf.EVENT_MOTION_START, _on_event("motion_start"), None)
        ent.register(pf.EVENT_MOTION_END, _on_event("motion_end"), None)
        units.append(ent)
    return units


def _make_blocker(point):
    blocker = ProbeBlocker(
        "assets/models/cart",
        "cart.pfobj",
        "dynamic_probe_blocker",
        required_resources={},
        pathable=False,
    )
    _place(blocker, point, radius=16.0, scale=(6.0, 2.0, 6.0), selectable=True)
    blocker.register(pf.EVENT_BUILDING_FOUNDED, _on_event("blocker_founded"), None)
    return blocker


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


def _setup_probe():
    start, target, blocker_pos, specs, slots = _find_scenario()
    STATE["unit_specs"] = [{"class": cls.__name__, "radius": radius} for cls, radius in specs]
    STATE["start_slots"] = [slot["point"] for slot in slots]
    STATE["target"] = target
    STATE["blocker_position"] = blocker_pos
    STATE["units"] = _make_units(specs, slots)
    STATE["blocker"] = _make_blocker(blocker_pos)
    STATE["pathing"] = {
        "start": start,
        "target": target,
        "blocker": blocker_pos,
        "pre_blocker_pathable": pf.map_nearest_pathable(blocker_pos, radius=15.25),
    }
    pf.set_unit_selection(STATE["units"])
    pf.get_active_camera().center_over_location(start)


def _movement_stats(start_slots, target):
    if not STATE["units"] or not start_slots or target is None:
        return {
            "avg_displacement": 0.0,
            "avg_progress": 0.0,
            "min_blocker_distance": None,
            "moved_count": 0,
        }

    sx = sum(point[0] for point in start_slots) / float(len(start_slots))
    sz = sum(point[1] for point in start_slots) / float(len(start_slots))
    tx, tz = target
    vx = tx - sx
    vz = tz - sz
    vlen = max(math.sqrt(vx * vx + vz * vz), 1.0)

    blocker = STATE["blocker_position"]
    total_disp = 0.0
    total_progress = 0.0
    moved_count = 0
    min_blocker = None
    for ent, start in zip(STATE["units"], start_slots):
        pos = _ent_xz(ent)
        disp = _dist(pos, start)
        total_disp += disp
        total_progress += ((pos[0] - start[0]) * vx + (pos[1] - start[1]) * vz) / vlen
        if disp > 3.0:
            moved_count += 1
        if blocker is not None:
            bdist = _dist(pos, blocker)
            if min_blocker is None or bdist < min_blocker:
                min_blocker = bdist

    count = float(len(STATE["units"]))
    return {
        "avg_displacement": total_disp / count,
        "avg_progress": total_progress / count,
        "min_blocker_distance": min_blocker,
        "moved_count": moved_count,
    }


def _issue_move():
    start = STATE["pathing"]["start"]
    target = STATE["target"]
    pf.move_in_formation(STATE["units"], pf.FORMATION_RANK, target, _orientation(start, target))


def _insert_blocker():
    blocker = STATE["blocker"]
    try:
        blocker.mark()
        blocker.found(blocking=True, force=True)
    except Exception as exc:
        _fail("failed to insert blocking building: {0}: {1}".format(exc.__class__.__name__, exc))
    STATE["checks"]["blocker_inserted"] = True
    STATE["post_blocker_start_slots"] = [_ent_xz(ent) for ent in STATE["units"]]

    post = pf.map_nearest_pathable(STATE["blocker_position"], radius=15.25)
    STATE["pathing"]["post_blocker_pathable"] = post
    if post is not None and _dist(post, STATE["blocker_position"]) > 8.0:
        STATE["checks"]["blocker_affects_pathing"] = True


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
        _setup_probe()
        _set_phase("moving_before_blocker")
        return

    if STATE["phase"] == "moving_before_blocker":
        if STATE["ticks"] == 1:
            _issue_move()
        if _event_count("motion_start") >= len(STATE["units"]):
            STATE["checks"]["move_started"] = True
        stats = _movement_stats(STATE["start_slots"], STATE["target"])
        STATE["movement"]["before_blocker"] = stats
        if stats["avg_progress"] > 12.0 and stats["avg_displacement"] > 12.0:
            STATE["checks"]["pre_blocker_progress"] = True
            _insert_blocker()
            _set_phase("moving_after_blocker")
            return
        if _phase_elapsed() > 18.0:
            _fail("movement did not progress before blocker insertion: {0}".format(stats))
        return

    if STATE["phase"] == "moving_after_blocker":
        if STATE["ticks"] < 150:
            return
        stats = _movement_stats(STATE["post_blocker_start_slots"], STATE["target"])
        STATE["movement"]["after_blocker"] = stats
        STATE["checks"]["continued_progress"] = stats["avg_progress"] > 8.0 and stats["avg_displacement"] > 8.0
        STATE["checks"]["clearance_preserved"] = (
            stats["min_blocker_distance"] is not None and stats["min_blocker_distance"] > 14.0
        )
        if all(STATE["checks"].values()):
            _set_phase("done")
            return
        if _phase_elapsed() > 16.0:
            _fail("dynamic obstacle checks failed: {0}".format(STATE["checks"]))
        return

    if STATE["phase"] == "done":
        _succeed()


def main():
    output_dir = _arg_value("--output-dir", os.environ.get("PF_DYNAMIC_OBSTACLE_OUTPUT_DIR", DEFAULT_OUTPUT_DIR))
    if not os.path.isabs(output_dir):
        output_dir = os.path.join(pf.get_basedir(), output_dir)
    if not os.path.isdir(output_dir):
        os.makedirs(output_dir)
    STATE["output_dir"] = output_dir
    STATE["expected_backend"] = _arg_value("--expect-backend", os.environ.get("PF_DYNAMIC_OBSTACLE_EXPECT_BACKEND", "METAL"))
    STATE["movement_mode"] = _arg_value("--movement-mode", os.environ.get("PF_DYNAMIC_OBSTACLE_MOVEMENT_MODE", "gpu"))

    for path in (PROBE_PATH, ERROR_PATH):
        try:
            os.unlink(path)
        except OSError:
            pass

    _set_phase("init")
    demo_main.main()
    pf.register_event_handler(pf.EVENT_BUILDING_PLACED, _on_event("building_placed"), None)
    pf.register_event_handler(pf.EVENT_MOVABLE_ENTITY_BLOCK, _on_event("movable_block"), None)
    pf.register_ui_event_handler(pf.EVENT_UPDATE_START, on_update, None)


main()
