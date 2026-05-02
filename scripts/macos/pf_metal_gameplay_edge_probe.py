import json
import math
import os
import sys
import time

import pf

sys.path.insert(0, pf.get_basedir() + "/scripts")

import rts.globals
import rts.main as demo_main


PROBE_PATH = "/tmp/pf_metal_gameplay_edge_probe.txt"
ERROR_PATH = "/tmp/pf_metal_gameplay_edge_probe_error.txt"
DEFAULT_OUTPUT_DIR = "visual_parity_captures/gameplay-edge-probe"
RESOURCE_NAME = "edge_probe_wood"


class ProbeLandUnit(pf.MovableEntity):
    pass


class ProbeWaterUnit(pf.WaterEntity, pf.MovableEntity):
    pass


class ProbeAirUnit(pf.AirEntity, pf.MovableEntity):
    pass


class ProbeWaterHauler(pf.WaterEntity, pf.HarvesterEntity, pf.MovableEntity):
    pass


class ProbeWaterStorage(pf.WaterEntity, pf.StorageSiteEntity):
    pass


STATE = {
    "phase": "init",
    "ticks": 0,
    "output_dir": None,
    "expected_backend": "METAL",
    "phase_started_at": None,
    "phase_log": [],
    "events": {},
    "frame_ms": [],
    "entities": [],
    "checks": {
        "land_query": False,
        "water_query": False,
        "air_query": False,
        "water_move": False,
        "air_move": False,
        "water_storage_restriction": False,
        "water_transport": False,
    },
    "pathing": {},
    "transport": {},
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
    print("GAMEPLAY_EDGE_PHASE {0}".format(name))
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
        "checks": STATE["checks"],
        "events": STATE["events"],
        "phase_log": STATE["phase_log"],
        "frame_ms": _metrics(STATE["frame_ms"]),
        "pathing": STATE["pathing"],
        "transport": STATE["transport"],
    }
    with open(_summary_path(backend), "w") as outfile:
        json.dump(payload, outfile, indent=2, sort_keys=True)
        outfile.write("\n")
    print("GAMEPLAY_EDGE_SUMMARY {0}".format(_summary_path(backend)))
    sys.stdout.flush()


def _fail(reason):
    _write_summary("fail", reason)
    _write(ERROR_PATH, str(reason))
    print("GAMEPLAY_EDGE_FAIL {0}".format(reason))
    sys.stdout.flush()
    os._exit(1)


def _succeed():
    _write_summary("pass")
    marker = (
        "GAMEPLAY_EDGE_PASS backend={backend} pathing={pathing} "
        "movement={movement} water_transport={water_transport}"
    ).format(
        backend=pf.get_render_info().get("backend"),
        pathing=int(STATE["checks"]["land_query"] and STATE["checks"]["water_query"] and STATE["checks"]["air_query"]),
        movement=int(STATE["checks"]["water_move"] and STATE["checks"]["air_move"]),
        water_transport=int(STATE["checks"]["water_storage_restriction"] and STATE["checks"]["water_transport"]),
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


def _scene_anchors():
    anchors = []
    for ent in list(rts.globals.scene_objs):
        if getattr(ent, "faction_id", None) == 1 and hasattr(ent, "pos"):
            anchors.append(_ent_xz(ent))
    anchors.append((0.0, 0.0))
    return anchors


def _nearest_land(point, radius=128.0):
    for dx, dz in _offsets():
        candidate = (point[0] + dx, point[1] + dz)
        if not _inside_map(candidate):
            continue
        found = pf.map_nearest_pathable(candidate, radius=radius)
        if found is not None:
            return found
    return None


def _nearest_water(point, radius=512.0):
    for dx, dz in _offsets(step=48.0):
        candidate = (point[0] + dx, point[1] + dz)
        found = pf.map_nearest_pathable_water(candidate, radius=radius)
        if found is not None and pf.map_pos_over_water(found[0], found[1]):
            return found
    return None


def _nearest_air(point, radius=256.0):
    for dx, dz in _offsets(step=40.0):
        candidate = (point[0] + dx, point[1] + dz)
        if not _inside_map(candidate):
            continue
        found = pf.map_nearest_pathable_air(candidate, radius=radius)
        if found is not None:
            return found
    return None


def _offsets(step=24.0):
    offsets = [(0.0, 0.0)]
    for ring in range(1, 7):
        dist = step * ring
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
    return offsets


def _place(ent, point, radius=2.5, scale=None, selectable=False):
    height = pf.map_height_at_point(point[0], point[1])
    if height is None:
        height = 0.0
    ent.pos = (float(point[0]), float(height), float(point[1]))
    ent.faction_id = 1
    ent.selection_radius = float(radius)
    ent.selectable = selectable
    if scale is not None:
        ent.scale = scale
    STATE["entities"].append(ent)
    rts.globals.scene_objs.append(ent)
    return ent


def _make_storage(name, point, amount, desired, do_not_take_water=False):
    storage = ProbeWaterStorage("assets/models/crate", "crate_1.pfobj", name)
    _place(storage, point, radius=3.0, scale=(1.0, 1.0, 1.0), selectable=False)
    storage.set_capacity(RESOURCE_NAME, 64)
    storage.set_curr_amount(RESOURCE_NAME, amount)
    storage.set_desired(RESOURCE_NAME, desired)
    storage.do_not_take_water = do_not_take_water
    return storage


def _setup_entities():
    land = water = air = None
    for anchor in _scene_anchors():
        land = _nearest_land(anchor)
        water = _nearest_water(anchor)
        air = _nearest_air(anchor)
        if land is not None and water is not None and air is not None:
            break
    if land is None or water is None or air is None:
        _fail("could not find land/water/air pathing probes")

    water_target = None
    for dx, dz in ((96.0, 0.0), (-96.0, 0.0), (0.0, 96.0), (0.0, -96.0)):
        candidate = _nearest_water((water[0] + dx, water[1] + dz), radius=128.0)
        if candidate is not None and _dist(candidate, water) > 12.0:
            water_target = candidate
            break
    if water_target is None:
        _fail("could not find second water target")

    air_target = None
    for dx, dz in ((120.0, 80.0), (-120.0, 80.0), (120.0, -80.0), (-120.0, -80.0)):
        candidate = _nearest_air((air[0] + dx, air[1] + dz), radius=128.0)
        if candidate is not None and _dist(candidate, air) > 12.0:
            air_target = candidate
            break
    if air_target is None:
        _fail("could not find second air target")

    water_unit = ProbeWaterUnit("assets/models/cart", "cart.pfobj", "probe_water_unit")
    air_unit = ProbeAirUnit("assets/models/cart", "cart.pfobj", "probe_air_unit")
    hauler = ProbeWaterHauler("assets/models/cart", "cart.pfobj", "probe_water_hauler")
    _place(water_unit, water, radius=2.5, scale=(1.0, 1.0, 1.0), selectable=True)
    _place(air_unit, air, radius=2.5, scale=(1.0, 1.0, 1.0), selectable=True)
    _place(hauler, water, radius=2.5, scale=(1.0, 1.0, 1.0), selectable=True)
    water_unit.speed = 48.0
    air_unit.speed = 48.0
    hauler.speed = 64.0
    hauler.set_max_carry(RESOURCE_NAME, 4)
    hauler.set_do_not_transport(RESOURCE_NAME, False)

    source = _make_storage("probe_water_source", water_target, amount=12, desired=0, do_not_take_water=True)
    dest_target = _nearest_water((water_target[0] + 40.0, water_target[1] + 40.0), radius=128.0) or water
    dest = _make_storage("probe_water_dest", dest_target, amount=0, desired=12, do_not_take_water=False)

    water_unit.register(pf.EVENT_MOTION_START, _on_event("water_motion_start"), None)
    air_unit.register(pf.EVENT_MOTION_START, _on_event("air_motion_start"), None)
    hauler.register(pf.EVENT_TRANSPORT_TARGET_ACQUIRED, _on_event("water_transport_target"), None)
    hauler.register(pf.EVENT_RESOURCE_PICKED_UP, _on_event("water_transport_pickup"), None)
    hauler.register(pf.EVENT_RESOURCE_DROPPED_OFF, _on_event("water_transport_dropoff"), None)

    STATE["pathing"] = {
        "land": land,
        "water": water,
        "water_target": water_target,
        "air": air,
        "air_target": air_target,
    }
    STATE["transport"] = {
        "source_start": source.get_curr_amount(RESOURCE_NAME),
        "dest_start": dest.get_curr_amount(RESOURCE_NAME),
        "source_do_not_take_water_start": source.do_not_take_water,
    }
    STATE["checks"]["land_query"] = land is not None
    STATE["checks"]["water_query"] = water is not None and pf.map_pos_over_water(water[0], water[1])
    STATE["checks"]["air_query"] = air is not None
    STATE["water_unit"] = water_unit
    STATE["air_unit"] = air_unit
    STATE["hauler"] = hauler
    STATE["source"] = source
    STATE["dest"] = dest

    pf.get_active_camera().center_over_location(water)


def _movement_phase():
    if STATE["ticks"] == 1:
        STATE["water_unit"].move(STATE["pathing"]["water_target"])
        STATE["air_unit"].move(STATE["pathing"]["air_target"])

    STATE["checks"]["water_move"] = _event_count("water_motion_start") > 0
    STATE["checks"]["air_move"] = _event_count("air_motion_start") > 0
    if STATE["checks"]["water_move"] and STATE["checks"]["air_move"]:
        _set_phase("water_transport_blocked")
        return
    if _phase_elapsed() > 8.0:
        _fail("water/air move orders did not start")


def _water_transport_blocked_phase():
    hauler = STATE["hauler"]
    source = STATE["source"]
    dest = STATE["dest"]

    if STATE["ticks"] == 1:
        source.do_not_take_water = True
        try:
            hauler.transport(dest)
        except RuntimeError as exc:
            STATE["transport"]["blocked_call_error"] = str(exc)
            STATE["checks"]["water_storage_restriction"] = True

    hauler.notify(pf.EVENT_MOTION_END, None)
    STATE["transport"].update({
        "blocked_source": source.get_curr_amount(RESOURCE_NAME),
        "blocked_dest": dest.get_curr_amount(RESOURCE_NAME),
        "blocked_carry": hauler.get_curr_carry(RESOURCE_NAME),
    })

    if STATE["ticks"] >= 45:
        if dest.get_curr_amount(RESOURCE_NAME) == STATE["transport"]["dest_start"] and hauler.get_curr_carry(RESOURCE_NAME) == 0:
            STATE["checks"]["water_storage_restriction"] = True
            _set_phase("water_transport_allowed")
            return
        _fail("water transport ignored do_not_take_water restriction")


def _water_transport_allowed_phase():
    hauler = STATE["hauler"]
    source = STATE["source"]
    dest = STATE["dest"]

    if STATE["ticks"] == 1:
        source.do_not_take_water = False
        try:
            hauler.transport(dest)
        except RuntimeError as exc:
            _fail("water transport did not accept allowed source: {0}".format(exc))

    hauler.notify(pf.EVENT_MOTION_END, None)
    if hauler.get_curr_carry(RESOURCE_NAME) > 0:
        hauler.notify(pf.EVENT_MOTION_END, None)

    STATE["transport"].update({
        "source_end": source.get_curr_amount(RESOURCE_NAME),
        "dest_end": dest.get_curr_amount(RESOURCE_NAME),
        "hauler_carry": hauler.get_curr_carry(RESOURCE_NAME),
        "source_do_not_take_water_end": source.do_not_take_water,
    })

    if dest.get_curr_amount(RESOURCE_NAME) > STATE["transport"]["dest_start"]:
        STATE["checks"]["water_transport"] = True
        _set_phase("done")
        return
    if _phase_elapsed() > 12.0:
        _fail("allowed water transport did not move resources")


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
        _setup_entities()
        _set_phase("movement")
        return

    if STATE["phase"] == "movement":
        _movement_phase()
        return

    if STATE["phase"] == "water_transport_blocked":
        _water_transport_blocked_phase()
        return

    if STATE["phase"] == "water_transport_allowed":
        _water_transport_allowed_phase()
        return

    if STATE["phase"] == "done":
        _succeed()


def main():
    output_dir = _arg_value("--output-dir", os.environ.get("PF_GAMEPLAY_EDGE_OUTPUT_DIR", DEFAULT_OUTPUT_DIR))
    if not os.path.isabs(output_dir):
        output_dir = os.path.join(pf.get_basedir(), output_dir)
    if not os.path.isdir(output_dir):
        os.makedirs(output_dir)
    STATE["output_dir"] = output_dir
    STATE["expected_backend"] = _arg_value("--expect-backend", os.environ.get("PF_GAMEPLAY_EDGE_EXPECT_BACKEND", "METAL"))

    for path in (PROBE_PATH, ERROR_PATH):
        try:
            os.unlink(path)
        except OSError:
            pass

    _set_phase("init")
    demo_main.main()
    pf.register_ui_event_handler(pf.EVENT_UPDATE_START, on_update, None)


main()
