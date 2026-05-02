import json
import math
import os
import sys
import time

import pf

sys.path.insert(0, pf.get_basedir() + "/scripts")

import rts.globals
import rts.main as demo_main


PROBE_PATH = "/tmp/pf_metal_production_automation_probe.txt"
ERROR_PATH = "/tmp/pf_metal_production_automation_probe_error.txt"
DEFAULT_OUTPUT_DIR = "visual_parity_captures/production-automation-probe"
RESOURCE_NAME = "wood"


class ProbeWorker(pf.HarvesterEntity, pf.MovableEntity):
    pass


class ProbeStorage(pf.StorageSiteEntity):
    pass


STATE = {
    "phase": "init",
    "ticks": 0,
    "output_dir": None,
    "expected_backend": "METAL",
    "phase_started_at": None,
    "phase_log": [],
    "frame_ms": [],
    "entities": [],
    "events": {},
    "checks": {
        "manual_toggle": False,
        "auto_off_idle": False,
        "do_not_transport_blocks": False,
        "auto_transport_resumes": False,
        "pickup": False,
        "dropoff": False,
        "idle_after_delivery": False,
    },
    "automation": {},
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
    print("PRODUCTION_AUTOMATION_PHASE {0}".format(name))
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
        "automation": STATE["automation"],
    }
    with open(_summary_path(backend), "w") as outfile:
        json.dump(payload, outfile, indent=2, sort_keys=True)
        outfile.write("\n")
    print("PRODUCTION_AUTOMATION_SUMMARY {0}".format(_summary_path(backend)))
    sys.stdout.flush()


def _fail(reason):
    _write_summary("fail", reason)
    _write(ERROR_PATH, str(reason))
    print("PRODUCTION_AUTOMATION_FAIL {0}".format(reason))
    sys.stdout.flush()
    os._exit(1)


def _succeed():
    _write_summary("pass")
    marker = (
        "PRODUCTION_AUTOMATION_PASS backend={backend} toggle={toggle} idle={idle} "
        "blocked={blocked} resumed={resumed} pickup={pickup} dropoff={dropoff}"
    ).format(
        backend=pf.get_render_info().get("backend"),
        toggle=int(STATE["checks"]["manual_toggle"]),
        idle=int(STATE["checks"]["auto_off_idle"]),
        blocked=int(STATE["checks"]["do_not_transport_blocks"]),
        resumed=int(STATE["checks"]["auto_transport_resumes"]),
        pickup=int(STATE["checks"]["pickup"]),
        dropoff=int(STATE["checks"]["dropoff"]),
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


def _pathable_near(point, radius=2.5):
    offsets = [(0.0, 0.0)]
    for ring in range(1, 7):
        dist = 14.0 * ring
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


def _slot_points(center, count, radius=2.5):
    slots = []
    offsets = []
    for row in range(-4, 5):
        for col in range(-4, 5):
            offsets.append((col * 14.0, row * 14.0))
    offsets.sort(key=lambda item: item[0] * item[0] + item[1] * item[1])

    for dx, dz in offsets:
        point = (center[0] + dx, center[1] + dz)
        if not _inside_map(point):
            continue
        found = pf.map_nearest_pathable(point, radius=radius)
        if found is None:
            continue
        if all(_dist(found, seen) > 8.0 for seen in slots):
            slots.append(found)
            if len(slots) >= count:
                return slots
    return slots


def _place(ent, point, radius=2.5, scale=None, selectable=False):
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
    STATE["entities"].append(ent)
    rts.globals.scene_objs.append(ent)
    return ent


def _make_worker(point):
    worker = ProbeWorker("assets/models/cart", "cart.pfobj", "probe_automation_worker")
    _place(worker, point, radius=2.5, scale=(1.0, 1.0, 1.0), selectable=True)
    worker.speed = 32.0
    worker.set_max_carry(RESOURCE_NAME, 4)
    worker.set_gather_speed(RESOURCE_NAME, 0.0)
    worker.set_do_not_transport(RESOURCE_NAME, False)
    worker.strategy = pf.TRANSPORT_STRATEGY_NEAREST
    worker.automatic_transport = False
    worker.register(pf.EVENT_TRANSPORT_TARGET_ACQUIRED, _on_event("transport_target"), None)
    worker.register(pf.EVENT_RESOURCE_PICKED_UP, _on_event("pickup"), None)
    worker.register(pf.EVENT_RESOURCE_DROPPED_OFF, _on_event("dropoff"), None)
    worker.register(pf.EVENT_MOTION_START, _on_event("motion_start"), None)
    worker.register(pf.EVENT_MOTION_END, _on_event("motion_end"), None)
    return worker


def _make_storage(name, point, amount, desired, capacity=32):
    storage = ProbeStorage("assets/models/crate", "crate_1.pfobj", name)
    _place(storage, point, radius=2.5, scale=(1.1, 1.1, 1.1), selectable=False)
    storage.set_capacity(RESOURCE_NAME, capacity)
    storage.set_curr_amount(RESOURCE_NAME, amount)
    storage.set_desired(RESOURCE_NAME, desired)
    storage.do_not_take_land = False
    storage.do_not_take_water = False
    return storage


def _setup_entities():
    anchors = []
    for ent in list(rts.globals.scene_objs):
        if getattr(ent, "faction_id", None) == 1 and hasattr(ent, "pos"):
            anchors.append(_ent_xz(ent))
    anchors.append((0.0, 0.0))

    center = None
    slots = []
    for anchor in anchors:
        center = _pathable_near(anchor)
        if center is None:
            continue
        slots = _slot_points(center, 4)
        if len(slots) >= 4:
            break
    if center is None or len(slots) < 4:
        _fail("could not find enough pathable automation slots")

    worker = _make_worker(slots[0])
    source = _make_storage("probe_automation_source", slots[1], amount=12, desired=0)
    dest = _make_storage("probe_automation_destination", slots[2], amount=0, desired=8)

    pf.get_active_camera().center_over_location(center)
    pf.set_unit_selection([worker])

    STATE["worker"] = worker
    STATE["source"] = source
    STATE["dest"] = dest
    STATE["automation"] = {
        "start": {
            "worker": _ent_xz(worker),
            "source": _ent_xz(source),
            "dest": _ent_xz(dest),
            "source_amount": source.get_curr_amount(RESOURCE_NAME),
            "source_desired": source.get_desired(RESOURCE_NAME),
            "dest_amount": dest.get_curr_amount(RESOURCE_NAME),
            "dest_desired": dest.get_desired(RESOURCE_NAME),
            "auto": worker.automatic_transport,
            "do_not_transport": worker.get_do_not_transport(RESOURCE_NAME),
        }
    }


def _snapshot(label):
    worker = STATE["worker"]
    source = STATE["source"]
    dest = STATE["dest"]
    STATE["automation"][label] = {
        "source_amount": source.get_curr_amount(RESOURCE_NAME),
        "dest_amount": dest.get_curr_amount(RESOURCE_NAME),
        "worker_carry": worker.get_curr_carry(RESOURCE_NAME),
        "worker_total_carry": worker.total_carry,
        "auto": worker.automatic_transport,
        "idle": worker.idle,
        "do_not_transport": worker.get_do_not_transport(RESOURCE_NAME),
        "transport_target_events": _event_count("transport_target"),
        "pickup_events": _event_count("pickup"),
        "dropoff_events": _event_count("dropoff"),
    }


def _drive_transport_cycle():
    worker = STATE["worker"]
    worker.notify(pf.EVENT_MOTION_END, None)
    if worker.get_curr_carry(RESOURCE_NAME) > 0:
        worker.notify(pf.EVENT_MOTION_END, None)


def _toggle_phase():
    worker = STATE["worker"]
    if STATE["ticks"] == 1:
        worker.automatic_transport = True
        enabled = worker.automatic_transport
        worker.automatic_transport = False
        disabled = not worker.automatic_transport
        STATE["checks"]["manual_toggle"] = bool(enabled and disabled)
        _snapshot("manual_toggle")
        _set_phase("auto_off_idle")


def _auto_off_idle_phase():
    worker = STATE["worker"]
    if STATE["ticks"] == 1:
        worker.set_do_not_transport(RESOURCE_NAME, False)
        worker.automatic_transport = False
    if STATE["ticks"] < 50:
        return
    _snapshot("auto_off_idle")
    unchanged = (
        STATE["source"].get_curr_amount(RESOURCE_NAME) == STATE["automation"]["start"]["source_amount"]
        and STATE["dest"].get_curr_amount(RESOURCE_NAME) == STATE["automation"]["start"]["dest_amount"]
        and worker.get_curr_carry(RESOURCE_NAME) == 0
        and _event_count("transport_target") == 0
    )
    STATE["checks"]["auto_off_idle"] = unchanged
    if not unchanged:
        _fail("automation moved resources while automatic transport was disabled")
    _set_phase("blocked_by_do_not_transport")


def _blocked_phase():
    worker = STATE["worker"]
    if STATE["ticks"] == 1:
        worker.set_do_not_transport(RESOURCE_NAME, True)
        worker.automatic_transport = True
    _drive_transport_cycle()
    if STATE["ticks"] < 70:
        return
    _snapshot("blocked_by_do_not_transport")
    unchanged = (
        STATE["source"].get_curr_amount(RESOURCE_NAME) == STATE["automation"]["start"]["source_amount"]
        and STATE["dest"].get_curr_amount(RESOURCE_NAME) == STATE["automation"]["start"]["dest_amount"]
        and worker.get_curr_carry(RESOURCE_NAME) == 0
        and _event_count("transport_target") == 0
        and worker.get_do_not_transport(RESOURCE_NAME)
    )
    STATE["checks"]["do_not_transport_blocks"] = unchanged
    if not unchanged:
        _fail("automatic transport ignored worker do_not_transport gate")
    _set_phase("resume_transport")


def _resume_phase():
    worker = STATE["worker"]
    source = STATE["source"]
    dest = STATE["dest"]
    if STATE["ticks"] == 1:
        worker.set_do_not_transport(RESOURCE_NAME, False)
        worker.automatic_transport = True
    _drive_transport_cycle()
    _snapshot("resume_transport")

    if _event_count("transport_target") > 0:
        STATE["checks"]["auto_transport_resumes"] = True
    if _event_count("pickup") > 0 and source.get_curr_amount(RESOURCE_NAME) < STATE["automation"]["start"]["source_amount"]:
        STATE["checks"]["pickup"] = True
    if _event_count("dropoff") > 0 and dest.get_curr_amount(RESOURCE_NAME) > STATE["automation"]["start"]["dest_amount"]:
        STATE["checks"]["dropoff"] = True
    delivered = (
        dest.get_curr_amount(RESOURCE_NAME) >= dest.get_desired(RESOURCE_NAME)
        and worker.total_carry == 0
    )
    if STATE["checks"]["auto_transport_resumes"] and STATE["checks"]["pickup"] and STATE["checks"]["dropoff"] and delivered:
        worker.automatic_transport = False
        _snapshot("delivery_complete")
        _set_phase("post_delivery_idle")
        return
    if _phase_elapsed() > 12.0:
        _fail("automatic transport did not resume after do_not_transport cleared")


def _post_delivery_idle_phase():
    if STATE["ticks"] < 50:
        return
    _snapshot("post_delivery_idle")
    worker = STATE["worker"]
    dest = STATE["dest"]
    STATE["checks"]["idle_after_delivery"] = (
        worker.idle
        and worker.total_carry == 0
        and dest.get_curr_amount(RESOURCE_NAME) >= dest.get_desired(RESOURCE_NAME)
    )
    if STATE["checks"]["idle_after_delivery"]:
        _set_phase("done")
        return
    if _phase_elapsed() > 8.0:
        _fail("automation worker did not settle idle after delivery")


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
        _set_phase("settle")
        return

    if STATE["phase"] == "settle":
        if STATE["ticks"] >= 8:
            _set_phase("manual_toggle")
        return

    if STATE["phase"] == "manual_toggle":
        _toggle_phase()
        return

    if STATE["phase"] == "auto_off_idle":
        _auto_off_idle_phase()
        return

    if STATE["phase"] == "blocked_by_do_not_transport":
        _blocked_phase()
        return

    if STATE["phase"] == "resume_transport":
        _resume_phase()
        return

    if STATE["phase"] == "post_delivery_idle":
        _post_delivery_idle_phase()
        return

    if STATE["phase"] == "done":
        if all(STATE["checks"].values()):
            _succeed()
        _fail("production automation checks failed: {0}".format(STATE["checks"]))


def main():
    output_dir = _arg_value("--output-dir", os.environ.get("PF_PRODUCTION_AUTOMATION_OUTPUT_DIR", DEFAULT_OUTPUT_DIR))
    if not os.path.isabs(output_dir):
        output_dir = os.path.join(pf.get_basedir(), output_dir)
    if not os.path.isdir(output_dir):
        os.makedirs(output_dir)
    STATE["output_dir"] = output_dir
    STATE["expected_backend"] = _arg_value(
        "--expect-backend",
        os.environ.get("PF_PRODUCTION_AUTOMATION_EXPECT_BACKEND", "METAL"),
    )

    for path in (PROBE_PATH, ERROR_PATH):
        try:
            os.unlink(path)
        except OSError:
            pass

    _set_phase("init")
    demo_main.main()
    pf.register_ui_event_handler(pf.EVENT_UPDATE_START, on_update, None)


main()
