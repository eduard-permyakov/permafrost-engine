import json
import math
import os
import sys
import time

import pf

sys.path.insert(0, pf.get_basedir() + "/scripts")

import rts.globals
import rts.main as demo_main


PROBE_PATH = "/tmp/pf_metal_nav_formation_probe.txt"
ERROR_PATH = "/tmp/pf_metal_nav_formation_probe_error.txt"
DEFAULT_OUTPUT_DIR = "visual_parity_captures/nav-formation-probe"


class ProbeGround1x1(pf.MovableEntity):
    formation_priority = 0


class ProbeGround3x3(pf.MovableEntity):
    formation_priority = 1


class ProbeGround5x5(pf.MovableEntity):
    formation_priority = 2


class ProbeGround7x7(pf.MovableEntity):
    formation_priority = 3


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
    "first_target": None,
    "second_target": None,
    "reshuffle_start_slots": [],
    "checks": {
        "ground_layers_pathable": False,
        "preferred_rank": False,
        "preferred_mixed": False,
        "rank_move": False,
        "column_reshuffle": False,
    },
    "pathing": {},
    "formation": {},
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
    print("NAV_FORMATION_PHASE {0}".format(name))
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
        "formation": STATE["formation"],
    }
    with open(_summary_path(backend), "w") as outfile:
        json.dump(payload, outfile, indent=2, sort_keys=True)
        outfile.write("\n")
    print("NAV_FORMATION_SUMMARY {0}".format(_summary_path(backend)))
    sys.stdout.flush()


def _fail(reason):
    _write_summary("fail", reason)
    _write(ERROR_PATH, str(reason))
    print("NAV_FORMATION_FAIL {0}".format(reason))
    sys.stdout.flush()
    os._exit(1)


def _succeed():
    _write_summary("pass")
    marker = (
        "NAV_FORMATION_PASS backend={backend} mode={mode} layers={layers} "
        "rank={rank} reshuffle={reshuffle}"
    ).format(
        backend=pf.get_render_info().get("backend"),
        mode=STATE["movement_mode"],
        layers=int(STATE["checks"]["ground_layers_pathable"]),
        rank=int(STATE["checks"]["rank_move"]),
        reshuffle=int(STATE["checks"]["column_reshuffle"]),
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


def _find_layer_points(anchor):
    radii = [3.25, 5.25, 10.25, 15.25]
    result = {}
    for radius in radii:
        found = _pathable_near(anchor, radius)
        if found is None:
            return None
        result[str(radius)] = found
    return result


def _find_slots(center, specs):
    max_radius = max(radius for _, radius in specs)
    spacing = (max_radius * 2.0) + 8.0
    offsets = []
    extent = 4
    for row in range(-extent, extent + 1):
        for col in range(-extent, extent + 1):
            offsets.append((col * spacing, row * spacing))
    offsets.sort(key=lambda item: item[0] * item[0] + item[1] * item[1])

    slots = []
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
        if distance < 72.0 or distance > 240.0:
            continue
        if all(_dist(found, seen) > 48.0 for seen in targets):
            targets.append(found)
    return targets


def _choose_start_and_targets():
    specs = []
    for cls, radius, count in UNIT_SPECS:
        for _ in range(count):
            specs.append((cls, radius))

    candidates = _scene_pathable_points(15.25)
    for start in candidates:
        layer_points = _find_layer_points(start)
        if layer_points is None:
            continue
        slots = _find_slots(start, specs)
        if slots is None:
            continue
        targets = _local_targets(start, 15.25)
        if len(targets) < 2:
            targets = []
            for target in candidates:
                if target == start:
                    continue
                if _dist(start, target) < 128.0:
                    continue
                if _pathable_for_radius(target, 15.25) is not None:
                    targets.append(target)
        if len(targets) < 2:
            continue
        targets.sort(key=lambda point: _dist(start, point), reverse=True)
        first = targets[0]
        second = None
        for target in targets[1:]:
            if _dist(first, target) > 96.0:
                second = target
                break
        if second is None:
            second = start
        return start, first, second, specs, slots, layer_points
    _fail("could not find suitable multi-layer formation start and targets")


def _make_units(specs, slots):
    units = []
    for idx, ((cls, radius), slot) in enumerate(zip(specs, slots)):
        ent = cls("assets/models/cart", "cart.pfobj", "nav_form_unit_{0}".format(idx))
        ent.faction_id = 1
        ent.selectable = True
        ent.selection_radius = float(radius)
        ent.speed = 36.0
        ent.preferred_formation = pf.FORMATION_RANK
        point = slot["point"]
        height = pf.map_height_at_point(point[0], point[1])
        if height is None:
            height = 0.0
        ent.pos = (float(point[0]), float(height), float(point[1]))
        ent.register(pf.EVENT_MOTION_START, _on_event("motion_start"), None)
        ent.register(pf.EVENT_MOTION_END, _on_event("motion_end"), None)
        rts.globals.scene_objs.append(ent)
        units.append(ent)
    return units


def _movement_stats(start_slots, target):
    if not STATE["units"] or not start_slots or target is None:
        return {"moved_count": 0, "avg_displacement": 0.0, "avg_progress": 0.0, "min_pair_distance": None}

    sx = sum(point[0] for point in start_slots) / float(len(start_slots))
    sz = sum(point[1] for point in start_slots) / float(len(start_slots))
    tx, tz = target
    vx = tx - sx
    vz = tz - sz
    vlen = max(math.sqrt(vx * vx + vz * vz), 1.0)

    total_disp = 0.0
    total_progress = 0.0
    moved_count = 0
    min_pair = None
    positions = []
    for ent, start in zip(STATE["units"], start_slots):
        pos = _ent_xz(ent)
        positions.append(pos)
        disp = _dist(pos, start)
        total_disp += disp
        total_progress += ((pos[0] - start[0]) * vx + (pos[1] - start[1]) * vz) / vlen
        if disp > 3.0:
            moved_count += 1

    for i in range(len(positions)):
        for j in range(i + 1, len(positions)):
            distance = _dist(positions[i], positions[j])
            if min_pair is None or distance < min_pair:
                min_pair = distance

    count = float(len(STATE["units"]))
    return {
        "moved_count": moved_count,
        "avg_displacement": total_disp / count,
        "avg_progress": total_progress / count,
        "min_pair_distance": min_pair,
    }


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
    start, first, second, specs, slots, layer_points = _choose_start_and_targets()
    STATE["unit_specs"] = [{"class": cls.__name__, "radius": radius} for cls, radius in specs]
    STATE["start_slots"] = [slot["point"] for slot in slots]
    STATE["first_target"] = first
    STATE["second_target"] = second
    STATE["pathing"] = {
        "start": start,
        "first_target": first,
        "second_target": second,
        "layer_points": layer_points,
    }
    STATE["checks"]["ground_layers_pathable"] = True
    STATE["units"] = _make_units(specs, slots)
    pf.set_unit_selection(STATE["units"])
    pf.get_active_camera().center_over_location(start)

    if pf.formation_preferred_for_set(STATE["units"]) == pf.FORMATION_RANK:
        STATE["checks"]["preferred_rank"] = True
    STATE["units"][0].preferred_formation = pf.FORMATION_COLUMN
    if pf.formation_preferred_for_set(STATE["units"]) == pf.FORMATION_NONE:
        STATE["checks"]["preferred_mixed"] = True
    STATE["units"][0].preferred_formation = pf.FORMATION_RANK


def _start_rank_move():
    target = STATE["first_target"]
    start = STATE["pathing"]["start"]
    pf.move_in_formation(STATE["units"], pf.FORMATION_RANK, target, _orientation(start, target))


def _start_column_reshuffle():
    target = STATE["second_target"]
    center = _formation_center()
    STATE["reshuffle_start_slots"] = [_ent_xz(ent) for ent in STATE["units"]]
    for ent in STATE["units"]:
        ent.preferred_formation = pf.FORMATION_COLUMN
    pf.formation_arrange(STATE["units"], pf.FORMATION_COLUMN)
    pf.move_in_formation(STATE["units"], pf.FORMATION_COLUMN, target, _orientation(center, target))


def _formation_center():
    positions = [_ent_xz(ent) for ent in STATE["units"]]
    return (
        sum(point[0] for point in positions) / float(len(positions)),
        sum(point[1] for point in positions) / float(len(positions)),
    )


def _check_rank_move():
    stats = _movement_stats(STATE["start_slots"], STATE["first_target"])
    STATE["formation"]["rank_stats"] = stats
    if _event_count("motion_start") >= len(STATE["units"]) and stats["avg_progress"] > 12.0:
        STATE["checks"]["rank_move"] = True
        _set_phase("column_reshuffle")
        return
    if _phase_elapsed() > 18.0:
        _fail("rank formation move did not make enough progress: {0}".format(stats))


def _check_column_reshuffle():
    stats = _movement_stats(STATE["reshuffle_start_slots"], STATE["second_target"])
    STATE["formation"]["column_stats"] = stats
    if stats["avg_progress"] > 10.0 and stats["avg_displacement"] > 10.0:
        STATE["checks"]["column_reshuffle"] = True
        _set_phase("done")
        return
    if _phase_elapsed() > 18.0:
        _fail("column formation reshuffle did not make enough progress: {0}".format(stats))


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
        _set_phase("rank_move")
        return

    if STATE["phase"] == "rank_move":
        if STATE["ticks"] == 1:
            _start_rank_move()
        if STATE["ticks"] > 90:
            _check_rank_move()
        return

    if STATE["phase"] == "column_reshuffle":
        if STATE["ticks"] == 1:
            _start_column_reshuffle()
        if STATE["ticks"] > 90:
            _check_column_reshuffle()
        return

    if STATE["phase"] == "done":
        _succeed()


def main():
    output_dir = _arg_value("--output-dir", os.environ.get("PF_NAV_FORMATION_OUTPUT_DIR", DEFAULT_OUTPUT_DIR))
    if not os.path.isabs(output_dir):
        output_dir = os.path.join(pf.get_basedir(), output_dir)
    if not os.path.isdir(output_dir):
        os.makedirs(output_dir)
    STATE["output_dir"] = output_dir
    STATE["expected_backend"] = _arg_value("--expect-backend", os.environ.get("PF_NAV_FORMATION_EXPECT_BACKEND", "METAL"))
    STATE["movement_mode"] = _arg_value("--movement-mode", os.environ.get("PF_NAV_FORMATION_MOVEMENT_MODE", "gpu"))

    for path in (PROBE_PATH, ERROR_PATH):
        try:
            os.unlink(path)
        except OSError:
            pass

    _set_phase("init")
    demo_main.main()
    pf.register_ui_event_handler(pf.EVENT_UPDATE_START, on_update, None)


main()
