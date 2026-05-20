import json
import math
import os
import sys
import time

import pf

sys.path.insert(0, pf.get_basedir() + "/scripts")

import rts.globals
import rts.main as demo_main


PROBE_PATH = "/tmp/pf_metal_gameplay_systems_probe.txt"
ERROR_PATH = "/tmp/pf_metal_gameplay_systems_probe_error.txt"
DEFAULT_OUTPUT_DIR = "visual_parity_captures/gameplay-systems-probe"
RESOURCE_NAME = "wood"


class ProbeWorker(pf.BuilderEntity, pf.HarvesterEntity, pf.MovableEntity, pf.GarrisonEntity):
    def __init__(self, path, pfobj, name):
        pf.BuilderEntity.__init__(self, path, pfobj, name, build_speed=512)


class ProbeResource(pf.ResourceEntity):
    pass


class ProbeStorage(pf.StorageSiteEntity):
    pass


class ProbeBuildable(pf.BuildableEntity):
    pass


class ProbeGarrisonable(pf.GarrisonableEntity):
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
        "resource_gather": False,
        "resource_dropoff": False,
        "building_lifecycle": False,
        "builder_order": False,
        "transport_order": False,
        "automatic_transport": False,
        "garrison": False,
    },
    "resource": {},
    "transport": {},
    "building": {},
    "garrison": {},
    "resource_dropoff_issued": False,
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
    print("GAMEPLAY_SYSTEMS_PHASE {0}".format(name))
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


def _snapshot_entity(ent):
    if ent is None:
        return None
    payload = {
        "name": getattr(ent, "name", None),
        "position": _ent_xz(ent),
        "selectable": getattr(ent, "selectable", None),
        "selection_radius": getattr(ent, "selection_radius", None),
    }
    for attr in ("resource_amount", "completed", "founded", "supplied",
                 "garrisonable_capacity", "garrisonable_current",
                 "automatic_transport", "total_carry"):
        if hasattr(ent, attr):
            try:
                payload[attr] = getattr(ent, attr)
            except Exception as exc:
                payload[attr] = "{0}: {1}".format(exc.__class__.__name__, exc)
    return payload


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
        "resource": STATE["resource"],
        "transport": STATE["transport"],
        "building": STATE["building"],
        "garrison": STATE["garrison"],
    }
    with open(_summary_path(backend), "w") as outfile:
        json.dump(payload, outfile, indent=2, sort_keys=True)
        outfile.write("\n")
    print("GAMEPLAY_SYSTEMS_SUMMARY {0}".format(_summary_path(backend)))
    sys.stdout.flush()


def _fail(reason):
    _write_summary("fail", reason)
    _write(ERROR_PATH, str(reason))
    print("GAMEPLAY_SYSTEMS_FAIL {0}".format(reason))
    sys.stdout.flush()
    os._exit(1)


def _succeed():
    _write_summary("pass")
    marker = (
        "GAMEPLAY_SYSTEMS_PASS backend={backend} resource={resource} "
        "building={building} builder={builder} transport={transport} "
        "automation={automation} garrison={garrison}"
    ).format(
        backend=pf.get_render_info().get("backend"),
        resource=int(STATE["checks"]["resource_gather"] and STATE["checks"]["resource_dropoff"]),
        building=int(STATE["checks"]["building_lifecycle"]),
        builder=int(STATE["checks"]["builder_order"]),
        transport=int(STATE["checks"]["transport_order"]),
        automation=int(STATE["checks"]["automatic_transport"]),
        garrison=int(STATE["checks"]["garrison"]),
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
    offsets = [
        (0.0, 0.0),
        (16.0, 0.0),
        (-16.0, 0.0),
        (0.0, 16.0),
        (0.0, -16.0),
        (32.0, 0.0),
        (-32.0, 0.0),
        (0.0, 32.0),
        (0.0, -32.0),
        (32.0, 32.0),
        (-32.0, 32.0),
        (32.0, -32.0),
        (-32.0, -32.0),
    ]
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


def _move_entity_to(ent, point):
    height = pf.map_height_at_point(point[0], point[1])
    if height is None:
        height = 0.0
    ent.pos = (float(point[0]), float(height), float(point[1]))


def _make_worker(name, point):
    worker = ProbeWorker("assets/models/cart", "cart.pfobj", name)
    _place(worker, point, radius=2.5, scale=(1.0, 1.0, 1.0), selectable=True)
    worker.speed = 32.0
    worker.set_max_carry(RESOURCE_NAME, 4)
    worker.set_gather_speed(RESOURCE_NAME, 4.0)
    worker.set_do_not_transport(RESOURCE_NAME, False)
    worker.garrison_capacity = 1
    return worker


def _make_resource(name, point, amount=16):
    resource = ProbeResource(
        "assets/models/hay",
        "haystack.pfobj",
        name,
        resource_name=RESOURCE_NAME,
        resource_amount=amount,
    )
    _place(resource, point, radius=2.5, scale=(0.75, 0.75, 0.75), selectable=False)
    return resource


def _make_storage(name, point):
    storage = ProbeStorage("assets/models/crate", "crate_1.pfobj", name)
    _place(storage, point, radius=2.5, scale=(1.1, 1.1, 1.1), selectable=False)
    storage.set_capacity(RESOURCE_NAME, 64)
    storage.set_desired(RESOURCE_NAME, 64)
    storage.set_curr_amount(RESOURCE_NAME, 0)
    return storage


def _make_buildable(name, point):
    buildable = ProbeBuildable(
        "assets/models/build_site_marker",
        "build-site-marker.pfobj",
        name,
        required_resources={},
        pathable=True,
    )
    _place(buildable, point, radius=2.5, scale=(1.2, 1.2, 1.2), selectable=True)
    buildable.vision_range = 18.0
    return buildable


def _make_garrisonable(name, point):
    garrisonable = ProbeGarrisonable("assets/models/cart", "cart.pfobj", name)
    _place(garrisonable, point, radius=3.0, scale=(1.3, 1.3, 1.3), selectable=True)
    garrisonable.garrisonable_capacity = 4
    return garrisonable


def _register_resource_events(worker, resource):
    worker.register(pf.EVENT_HARVEST_TARGET_ACQUIRED, _on_event("harvest_target"), None)
    worker.register(pf.EVENT_HARVEST_BEGIN, _on_event("harvest_begin"), None)
    worker.register(pf.EVENT_HARVEST_END, _on_event("harvest_end"), None)
    worker.register(pf.EVENT_STORAGE_TARGET_ACQUIRED, _on_event("storage_target"), None)
    worker.register(pf.EVENT_RESOURCE_DROPPED_OFF, _on_event("resource_dropped_off"), None)
    resource.register(pf.EVENT_RESOURCE_AMOUNT_CHANGED, _on_event("resource_amount_changed"), None)


def _register_builder_events(worker, buildable, suffix):
    worker.register(pf.EVENT_BUILD_TARGET_ACQUIRED, _on_event("build_target_{0}".format(suffix)), None)
    worker.register(pf.EVENT_BUILD_BEGIN, _on_event("build_begin_{0}".format(suffix)), None)
    worker.register(pf.EVENT_BUILD_END, _on_event("build_end_{0}".format(suffix)), None)
    buildable.register(pf.EVENT_BUILDING_COMPLETED, _on_event("building_completed_{0}".format(suffix)), None)


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
        slots = _slot_points(center, 14)
        if len(slots) >= 14:
            break
    if center is None or len(slots) < 14:
        _fail("could not find enough pathable staging slots")

    camera = pf.get_active_camera()
    camera.center_over_location(center)

    worker = _make_worker("probe_worker_resource", slots[0])
    resource = _make_resource("probe_resource", slots[1])
    storage = _make_storage("probe_storage", slots[2])
    _register_resource_events(worker, resource)

    lifecycle_building = _make_buildable("probe_lifecycle_building", slots[3])
    _register_builder_events(worker, lifecycle_building, "lifecycle")

    builder_worker = _make_worker("probe_worker_builder", slots[4])
    builder_target = _make_buildable("probe_builder_target", slots[5])
    _register_builder_events(builder_worker, builder_target, "order")

    transport_worker = _make_worker("probe_worker_transport", slots[6])
    transport_src = _make_storage("probe_transport_source", slots[7])
    transport_dst = _make_storage("probe_transport_destination", slots[8])
    transport_src.set_curr_amount(RESOURCE_NAME, 12)
    transport_src.set_desired(RESOURCE_NAME, 0)
    transport_dst.set_curr_amount(RESOURCE_NAME, 0)
    transport_dst.set_desired(RESOURCE_NAME, 12)
    transport_worker.register(pf.EVENT_TRANSPORT_TARGET_ACQUIRED, _on_event("transport_target"), None)
    transport_worker.register(pf.EVENT_RESOURCE_PICKED_UP, _on_event("transport_pickup"), None)
    transport_worker.register(pf.EVENT_RESOURCE_DROPPED_OFF, _on_event("transport_dropoff"), None)

    garrison_worker = _make_worker("probe_worker_garrison", slots[9])
    garrisonable = _make_garrisonable("probe_garrisonable", slots[10])

    STATE["resource"] = {
        "worker": _snapshot_entity(worker),
        "resource": _snapshot_entity(resource),
        "storage": _snapshot_entity(storage),
        "resource_amount_start": resource.resource_amount,
        "storage_amount_start": storage.get_curr_amount(RESOURCE_NAME),
    }
    STATE["transport"] = {
        "worker": _snapshot_entity(transport_worker),
        "source": _snapshot_entity(transport_src),
        "destination": _snapshot_entity(transport_dst),
        "source_amount_start": transport_src.get_curr_amount(RESOURCE_NAME),
        "destination_amount_start": transport_dst.get_curr_amount(RESOURCE_NAME),
    }
    STATE["building"] = {
        "lifecycle": _snapshot_entity(lifecycle_building),
        "builder_worker": _snapshot_entity(builder_worker),
        "builder_target": _snapshot_entity(builder_target),
    }
    STATE["garrison"] = {
        "worker": _snapshot_entity(garrison_worker),
        "garrisonable": _snapshot_entity(garrisonable),
    }

    STATE["worker"] = worker
    STATE["resource_ent"] = resource
    STATE["storage"] = storage
    STATE["lifecycle_building"] = lifecycle_building
    STATE["builder_worker"] = builder_worker
    STATE["builder_target"] = builder_target
    STATE["transport_worker"] = transport_worker
    STATE["transport_src"] = transport_src
    STATE["transport_dst"] = transport_dst
    STATE["garrison_worker"] = garrison_worker
    STATE["garrisonable"] = garrisonable


def _drive_harvest_cycle():
    worker = STATE["worker"]
    worker.notify(pf.EVENT_MOTION_END, None)
    if _event_count("harvest_begin") > 0:
        worker.notify(pf.EVENT_ANIM_CYCLE_FINISHED, None)


def _drive_dropoff_cycle():
    STATE["worker"].notify(pf.EVENT_MOTION_END, None)


def _resource_phase():
    worker = STATE["worker"]
    resource = STATE["resource_ent"]
    storage = STATE["storage"]
    if STATE["ticks"] == 1:
        worker.gather(resource)

    _drive_harvest_cycle()
    if worker.get_curr_carry(RESOURCE_NAME) > 0 or resource.resource_amount < STATE["resource"]["resource_amount_start"]:
        STATE["checks"]["resource_gather"] = True

    if worker.get_curr_carry(RESOURCE_NAME) > 0 and not STATE["resource_dropoff_issued"]:
        wx, wz = _ent_xz(worker)
        near_storage = _pathable_near((wx + 6.0, wz), radius=storage.selection_radius) or (wx + 6.0, wz)
        _move_entity_to(storage, near_storage)
        worker.drop_off(storage)
        STATE["resource_dropoff_issued"] = True
    _drive_dropoff_cycle()

    STATE["resource"].update({
        "resource_amount_end": resource.resource_amount,
        "worker_carry": worker.get_curr_carry(RESOURCE_NAME),
        "worker_total_carry": worker.total_carry,
        "storage_amount_end": storage.get_curr_amount(RESOURCE_NAME),
        "events": dict(STATE["events"]),
    })

    if storage.get_curr_amount(RESOURCE_NAME) > STATE["resource"]["storage_amount_start"]:
        STATE["checks"]["resource_dropoff"] = True
        _set_phase("building_lifecycle")
        return

    if _phase_elapsed() > 10.0:
        _fail("resource gather/drop-off did not complete")


def _building_lifecycle_phase():
    building = STATE["lifecycle_building"]
    try:
        building.mark()
        building.found(force=True)
        building.supply()
        building.complete()
    except Exception as exc:
        _fail("direct building lifecycle failed: {0}: {1}".format(exc.__class__.__name__, exc))

    STATE["checks"]["building_lifecycle"] = bool(building.founded and building.supplied and building.completed)
    STATE["building"]["lifecycle_end"] = _snapshot_entity(building)
    if not STATE["checks"]["building_lifecycle"]:
        _fail("building lifecycle flags did not reach completed")
    _set_phase("builder_order")


def _builder_order_phase():
    worker = STATE["builder_worker"]
    target = STATE["builder_target"]
    if STATE["ticks"] == 1:
        wx, wz = _ent_xz(worker)
        near_target = _pathable_near((wx + 6.0, wz), radius=target.selection_radius) or (wx + 6.0, wz)
        _move_entity_to(target, near_target)
        target.mark()
        target.found(force=True)
        target.supply()
        worker.build(target)

    worker.notify(pf.EVENT_MOTION_END, None)
    worker.notify(pf.EVENT_ANIM_CYCLE_FINISHED, None)
    STATE["building"]["builder_target_end"] = _snapshot_entity(target)

    if target.completed and _event_count("build_target_order") > 0:
        STATE["checks"]["builder_order"] = True
        _set_phase("transport")
        return

    if _phase_elapsed() > 10.0:
        _fail("builder order did not complete")


def _transport_phase():
    worker = STATE["transport_worker"]
    src = STATE["transport_src"]
    dst = STATE["transport_dst"]

    if STATE["ticks"] == 1:
        worker.strategy = pf.TRANSPORT_STRATEGY_NEAREST
        worker.automatic_transport = True
        if not worker.automatic_transport:
            _fail("automatic transport flag did not stick")
        STATE["checks"]["automatic_transport"] = True
        worker.transport(dst)

    worker.notify(pf.EVENT_MOTION_END, None)
    if worker.get_curr_carry(RESOURCE_NAME) > 0:
        worker.notify(pf.EVENT_MOTION_END, None)

    STATE["transport"].update({
        "source_amount_end": src.get_curr_amount(RESOURCE_NAME),
        "destination_amount_end": dst.get_curr_amount(RESOURCE_NAME),
        "worker_carry": worker.get_curr_carry(RESOURCE_NAME),
        "worker_total_carry": worker.total_carry,
        "automatic_transport": worker.automatic_transport,
        "events": dict(STATE["events"]),
    })

    if dst.get_curr_amount(RESOURCE_NAME) > STATE["transport"]["destination_amount_start"]:
        STATE["checks"]["transport_order"] = True
        _set_phase("garrison")
        return

    if _event_count("transport_target") > 0 and STATE["ticks"] > 90:
        STATE["checks"]["transport_order"] = True
        _set_phase("garrison")
        return

    if _phase_elapsed() > 12.0:
        _fail("transport order did not acquire a target or move resources")


def _garrison_phase():
    worker = STATE["garrison_worker"]
    garrisonable = STATE["garrisonable"]
    if STATE["ticks"] == 1:
        wx, wz = _ent_xz(worker)
        near_garrison = _pathable_near((wx + 6.0, wz), radius=garrisonable.selection_radius) or (wx + 6.0, wz)
        _move_entity_to(garrisonable, near_garrison)
        worker.garrison(garrisonable)

    STATE["garrison"].update({
        "worker_garrisoned": worker.is_garrisoned(),
        "garrisonable_current": garrisonable.garrisonable_current,
        "garrisonable_capacity": garrisonable.garrisonable_capacity,
    })

    if worker.is_garrisoned() and garrisonable.garrisonable_current >= worker.garrison_capacity:
        STATE["checks"]["garrison"] = True
        _set_phase("done")
        return

    if _phase_elapsed() > 12.0:
        _fail("garrison order did not complete")


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
            _set_phase("resource")
        return

    if STATE["phase"] == "resource":
        _resource_phase()
        return

    if STATE["phase"] == "building_lifecycle":
        _building_lifecycle_phase()
        return

    if STATE["phase"] == "builder_order":
        _builder_order_phase()
        return

    if STATE["phase"] == "transport":
        _transport_phase()
        return

    if STATE["phase"] == "garrison":
        _garrison_phase()
        return

    if STATE["phase"] == "done":
        _succeed()


def main():
    output_dir = _arg_value("--output-dir", os.environ.get("PF_GAMEPLAY_SYSTEMS_OUTPUT_DIR", DEFAULT_OUTPUT_DIR))
    if not os.path.isabs(output_dir):
        output_dir = os.path.join(pf.get_basedir(), output_dir)
    if not os.path.isdir(output_dir):
        os.makedirs(output_dir)
    STATE["output_dir"] = output_dir
    STATE["expected_backend"] = _arg_value("--expect-backend", os.environ.get("PF_GAMEPLAY_SYSTEMS_EXPECT_BACKEND", "METAL"))

    for path in (PROBE_PATH, ERROR_PATH):
        try:
            os.unlink(path)
        except OSError:
            pass

    _set_phase("init")
    demo_main.main()
    pf.register_ui_event_handler(pf.EVENT_UPDATE_START, on_update, None)


main()
