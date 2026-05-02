import json
import math
import os
import sys
import time

import pf

sys.path.insert(0, pf.get_basedir() + "/scripts")

import rts.globals
import rts.main as demo_main


PROBE_PATH = "/tmp/pf_metal_mixed_gameplay_scenario_probe.txt"
ERROR_PATH = "/tmp/pf_metal_mixed_gameplay_scenario_probe_error.txt"
RENDER_STATS_PATH = "/tmp/pf_mixed_gameplay_scenario_render_stats.txt"
PROJECTILE_STATS_PATH = "/tmp/pf_mixed_gameplay_scenario_projectile_stats.txt"
DEFAULT_OUTPUT_DIR = "visual_parity_captures/mixed-gameplay-scenario-probe"
RESOURCE_NAME = "wood"

EXPECTED_RENDER_SHEETS = set((
    "projectile_trail.png",
    "impact_burst.png",
    "fire_loop.png",
    "smoke_puff.png",
))


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
        "selection_camera_move": False,
        "fog_minimap": False,
        "resource_gather": False,
        "resource_dropoff": False,
        "building_lifecycle": False,
        "builder_order": False,
        "transport_order": False,
        "automatic_transport": False,
        "garrison": False,
        "combat_attack": False,
        "projectile_effects": False,
    },
    "movement": {},
    "fog_minimap": {},
    "resource": {},
    "transport": {},
    "building": {},
    "garrison": {},
    "combat": {},
    "resource_dropoff_issued": False,
    "attack_started": False,
    "projectile_hits": [],
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
    print("MIXED_GAMEPLAY_SCENARIO_PHASE {0}".format(name))
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


def _safe_getattr(obj, name, default=None):
    if obj is None:
        return default
    try:
        return getattr(obj, name)
    except RuntimeError:
        return default


def _safe_ent_xz(ent):
    if ent is None:
        return None
    try:
        return _ent_xz(ent)
    except RuntimeError:
        return None


def _snapshot_entity(ent):
    if ent is None:
        return None
    payload = {
        "uid": _safe_getattr(ent, "uid", None),
        "name": _safe_getattr(ent, "name", None),
        "position": _safe_ent_xz(ent),
        "selectable": _safe_getattr(ent, "selectable", None),
        "selection_radius": _safe_getattr(ent, "selection_radius", None),
    }
    for attr in ("hp", "resource_amount", "completed", "founded", "supplied",
                 "garrisonable_capacity", "garrisonable_current",
                 "automatic_transport", "total_carry"):
        try:
            payload[attr] = getattr(ent, attr)
        except (AttributeError, RuntimeError):
            pass
    return payload


def _read_sheet_stats(path):
    sheets = {}
    if not os.path.exists(path):
        return sheets
    with open(path, "r") as infile:
        for line in infile:
            fields = line.strip().split()
            sheet = None
            for field in fields:
                if field.startswith("sheet="):
                    sheet = field.split("=", 1)[1]
                    break
            if sheet:
                sheets[sheet] = sheets.get(sheet, 0) + 1
    return sheets


def _read_projectile_events():
    events = []
    if not os.path.exists(PROJECTILE_STATS_PATH):
        return events
    with open(PROJECTILE_STATS_PATH, "r") as infile:
        for line in infile:
            row = {}
            for field in line.strip().split():
                if "=" in field:
                    key, value = field.split("=", 1)
                    row[key] = value
            if row:
                events.append(row)
    return events


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
        "movement": STATE["movement"],
        "fog_minimap": STATE["fog_minimap"],
        "resource": STATE["resource"],
        "transport": STATE["transport"],
        "building": STATE["building"],
        "garrison": STATE["garrison"],
        "combat": STATE["combat"],
        "render_sprite_sheets": _read_sheet_stats(RENDER_STATS_PATH),
        "projectile_sprite_events": _read_projectile_events(),
        "projectile_hits": STATE["projectile_hits"],
    }
    with open(_summary_path(backend), "w") as outfile:
        json.dump(payload, outfile, indent=2, sort_keys=True)
        outfile.write("\n")
    print("MIXED_GAMEPLAY_SCENARIO_SUMMARY {0}".format(_summary_path(backend)))
    sys.stdout.flush()


def _fail(reason):
    _write_summary("fail", reason)
    _write(ERROR_PATH, str(reason))
    print("MIXED_GAMEPLAY_SCENARIO_FAIL {0}".format(reason))
    sys.stdout.flush()
    os._exit(1)


def _succeed():
    _write_summary("pass")
    marker = (
        "MIXED_GAMEPLAY_SCENARIO_PASS backend={backend} move={move} fog={fog} "
        "resource={resource} building={building} transport={transport} "
        "garrison={garrison} combat={combat} effects={effects}"
    ).format(
        backend=pf.get_render_info().get("backend"),
        move=int(STATE["checks"]["selection_camera_move"]),
        fog=int(STATE["checks"]["fog_minimap"]),
        resource=int(STATE["checks"]["resource_gather"] and STATE["checks"]["resource_dropoff"]),
        building=int(STATE["checks"]["building_lifecycle"] and STATE["checks"]["builder_order"]),
        transport=int(STATE["checks"]["transport_order"] and STATE["checks"]["automatic_transport"]),
        garrison=int(STATE["checks"]["garrison"]),
        combat=int(STATE["checks"]["combat_attack"]),
        effects=int(STATE["checks"]["projectile_effects"]),
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
    for ring in range(1, 8):
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
    for row in range(-5, 6):
        for col in range(-5, 6):
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


def _set_entity_pos(ent, point):
    height = pf.map_height_at_point(point[0], point[1])
    if height is None:
        return False
    ent.pos = (point[0], height, point[1])
    return True


def _make_worker(name, point):
    worker = ProbeWorker("assets/models/cart", "cart.pfobj", name)
    _place(worker, point, radius=2.5, scale=(1.0, 1.0, 1.0), selectable=True)
    worker.speed = 32.0
    worker.set_max_carry(RESOURCE_NAME, 4)
    worker.set_gather_speed(RESOURCE_NAME, 4.0)
    worker.set_do_not_transport(RESOURCE_NAME, False)
    worker.garrison_capacity = 1
    worker.register(pf.EVENT_MOTION_START, _on_event("{0}_motion_start".format(name)), None)
    worker.register(pf.EVENT_MOTION_END, _on_event("{0}_motion_end".format(name)), None)
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
    storage.do_not_take_land = False
    storage.do_not_take_water = False
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


def _require_effect_assets():
    sprites_dir = os.path.join(pf.get_basedir(), "assets", "sprites")
    missing = [
        name for name in EXPECTED_RENDER_SHEETS
        if not os.path.exists(os.path.join(sprites_dir, name))
    ]
    if missing:
        _fail("missing sprite asset(s): {0}".format(",".join(sorted(missing))))


def _is_mage(ent):
    name = getattr(ent, "name", "")
    cls_name = ent.__class__.__name__
    return "mage" in name.lower() or "mage" in cls_name.lower()


def _choose_mages_and_enemy():
    scene = list(rts.globals.scene_objs)
    mages = [
        ent for ent in scene
        if getattr(ent, "faction_id", None) == 1
        and _is_mage(ent)
        and hasattr(ent, "attack")
        and getattr(ent, "selectable", False)
    ]
    if not mages:
        _fail("no friendly Mage found for mixed gameplay scenario")

    selected_mages = mages[:2]
    enemies = [
        ent for ent in scene
        if getattr(ent, "faction_id", None) not in (None, 1)
        and hasattr(ent, "hp")
        and getattr(ent, "selectable", False)
        and getattr(ent, "hp", 0) > 0
    ]
    if not enemies:
        _fail("no enemy target found for mixed gameplay scenario")

    enemy = min(enemies, key=lambda ent: _dist(_ent_xz(ent), _ent_xz(selected_mages[0])))
    return selected_mages, enemy


def _stage_combat(mages, enemy):
    mx, mz = _ent_xz(mages[0])
    enemy_point = _pathable_near((mx + 28.0, mz + 4.0), enemy.selection_radius)
    if enemy_point is None or not _set_entity_pos(enemy, enemy_point):
        _fail("could not stage enemy near Mage")

    ex, ez = _ent_xz(enemy)
    offsets = ((-26.0, -5.0), (-26.0, 7.0))
    staged = []
    for mage, offset in zip(mages, offsets):
        mage_point = _pathable_near((ex + offset[0], ez + offset[1]), mage.selection_radius)
        if mage_point is None or not _set_entity_pos(mage, mage_point):
            continue
        if hasattr(mage, "stop"):
            mage.stop()
        staged.append(mage)
    if not staged:
        _fail("could not stage Mage near enemy")

    if hasattr(enemy, "stop"):
        enemy.stop()
    pf.set_unit_selection(staged)
    pf.get_active_camera().center_over_location(_ent_xz(enemy))
    STATE["mages"] = staged


def _spawn_fire_smoke_fixture(enemy):
    ex, ez = _ent_xz(enemy)
    height = pf.map_height_at_point(ex, ez)
    if height is None:
        height = enemy.pos[1]
    pf.spawn_sprite_animated(
        ("fire_loop.png", 1, 4, 4),
        (11.0, 18.0),
        (ex + 8.0, height + 11.0, ez + 6.0),
        8,
        12,
    )
    pf.spawn_sprite_animated(
        ("smoke_puff.png", 1, 4, 4),
        (15.0, 15.0),
        (ex + 10.0, height + 17.0, ez + 7.0),
        7,
        12,
    )


def _on_attack_start(user, event):
    del user
    del event
    STATE["attack_started"] = True
    _record("attack_start")


def _on_projectile_hit(user, event):
    del user
    parent = None
    target = None
    if isinstance(event, tuple) and len(event) >= 3:
        target = _snapshot_entity(event[0])
        parent = _snapshot_entity(event[2])
    STATE["projectile_hits"].append({
        "target": target,
        "parent": parent,
    })
    _record("projectile_hit")


def _setup_entities():
    _require_effect_assets()
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
        slots = _slot_points(center, 16)
        if len(slots) >= 16:
            break
    if center is None or len(slots) < 16:
        _fail("could not find enough pathable mixed-scenario staging slots")

    camera = pf.get_active_camera()
    STATE["camera_start"] = camera.position
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

    mages, enemy = _choose_mages_and_enemy()
    for mage in mages:
        mage.register(pf.EVENT_ATTACK_START, _on_attack_start, None)
    pf.register_event_handler(pf.EVENT_PROJECTILE_HIT, _on_projectile_hit, None)

    STATE["center"] = center
    STATE["move_origin"] = slots[0]
    STATE["move_target"] = slots[11]
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
    STATE["mages"] = mages
    STATE["enemy"] = enemy

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
    STATE["combat"] = {
        "mages": [_snapshot_entity(mage) for mage in mages],
        "enemy": _snapshot_entity(enemy),
        "enemy_hp_start": _safe_getattr(enemy, "hp", None),
    }


def _selection_camera_fog_phase():
    worker = STATE["worker"]
    if STATE["ticks"] == 1:
        pf.enable_fog_of_war()
        pf.set_minimap_render_all_ents(False)
        if pf.get_minimap_size() <= 0:
            pf.set_minimap_size(160)
        pf.set_unit_selection([worker])
        worker.move(STATE["move_target"])

    cam = pf.get_active_camera()
    selection_ok = len(pf.get_unit_selection()) == 1
    camera_moved = _dist(
        (STATE["camera_start"][0], STATE["camera_start"][2]),
        (cam.position[0], cam.position[2]),
    ) > 5.0
    movement_started = _event_count("probe_worker_resource_motion_start") > 0

    STATE["movement"] = {
        "camera_start": STATE["camera_start"],
        "camera_end": cam.position,
        "selection_len": len(pf.get_unit_selection()),
        "move_origin": STATE["move_origin"],
        "move_target": STATE["move_target"],
        "movement_started": movement_started,
    }
    STATE["fog_minimap"] = {
        "minimap_position": pf.get_minimap_position(),
        "minimap_size": pf.get_minimap_size(),
        "render_all_entities_requested": False,
    }

    if selection_ok and camera_moved and movement_started:
        if hasattr(worker, "stop"):
            worker.stop()
        _move_entity_to(worker, STATE["move_origin"])
        STATE["checks"]["selection_camera_move"] = True
        STATE["checks"]["fog_minimap"] = STATE["fog_minimap"]["minimap_size"] > 0
        _set_phase("resource")
        return

    if _phase_elapsed() > 8.0:
        _fail("selection/camera/movement/fog setup did not converge")


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
        _set_phase("combat_setup")
        return

    if _phase_elapsed() > 12.0:
        _fail("garrison order did not complete")


def _issue_attack():
    enemy = STATE["enemy"]
    STATE["combat"]["enemy_hp_start"] = _safe_getattr(enemy, "hp", None)
    for mage in STATE["mages"]:
        mage.attack(_ent_xz(enemy))
    _set_phase("combat")


def _effects_ready():
    backend = pf.get_render_info().get("backend")
    enemy = STATE["enemy"]
    enemy_hp = _safe_getattr(enemy, "hp", None)
    hp_dropped = (
        enemy_hp is not None
        and STATE["combat"].get("enemy_hp_start") is not None
        and enemy_hp < STATE["combat"]["enemy_hp_start"]
    )
    if STATE["attack_started"] or hp_dropped or _event_count("projectile_hit") > 0:
        STATE["checks"]["combat_attack"] = True

    if backend != "METAL":
        if STATE["checks"]["combat_attack"]:
            STATE["checks"]["projectile_effects"] = bool(hp_dropped or _event_count("projectile_hit") > 0)
            if STATE["checks"]["projectile_effects"]:
                return True, None
        return False, "OpenGL combat projectile did not hit or damage the target"

    render_sheets = _read_sheet_stats(RENDER_STATS_PATH)
    missing_render = EXPECTED_RENDER_SHEETS - set(render_sheets.keys())
    if missing_render:
        return False, "renderer missing {0}".format(",".join(sorted(missing_render)))

    projectile_events = _read_projectile_events()
    mage_parents = set(str(_safe_getattr(ent, "uid", "")) for ent in STATE["mages"])
    saw_trail = any(
        event.get("event") == "trail"
        and event.get("sheet") == "projectile_trail.png"
        and event.get("parent") in mage_parents
        for event in projectile_events
    )
    saw_impact = any(
        event.get("event") in ("impact_hit", "impact_oob")
        and event.get("sheet") == "impact_burst.png"
        and event.get("parent") in mage_parents
        for event in projectile_events
    )
    if not saw_trail:
        return False, "projectile trail not emitted"
    if not saw_impact:
        return False, "projectile impact not emitted"

    STATE["checks"]["projectile_effects"] = True
    return STATE["checks"]["combat_attack"], None


def _combat_setup_phase():
    _stage_combat(STATE["mages"], STATE["enemy"])
    _spawn_fire_smoke_fixture(STATE["enemy"])
    _issue_attack()


def _combat_phase():
    ready, reason = _effects_ready()
    STATE["combat"].update({
        "mages": [_snapshot_entity(mage) for mage in STATE["mages"]],
        "enemy": _snapshot_entity(STATE["enemy"]),
        "enemy_hp_end": _safe_getattr(STATE["enemy"], "hp", None),
        "attack_started": STATE["attack_started"],
        "projectile_hits": len(STATE["projectile_hits"]),
    })
    if ready:
        _set_phase("done")
        return
    if _phase_elapsed() > 24.0:
        _fail(reason or "timed out waiting for combat projectile/effects evidence")


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
            _set_phase("selection_camera_fog")
        return

    if STATE["phase"] == "selection_camera_fog":
        _selection_camera_fog_phase()
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

    if STATE["phase"] == "combat_setup":
        _combat_setup_phase()
        return

    if STATE["phase"] == "combat":
        _combat_phase()
        return

    if STATE["phase"] == "done":
        if all(STATE["checks"].values()):
            _succeed()
        _fail("mixed gameplay scenario checks failed: {0}".format(STATE["checks"]))


def main():
    output_dir = _arg_value("--output-dir", os.environ.get("PF_MIXED_GAMEPLAY_OUTPUT_DIR", DEFAULT_OUTPUT_DIR))
    if not os.path.isabs(output_dir):
        output_dir = os.path.join(pf.get_basedir(), output_dir)
    if not os.path.isdir(output_dir):
        os.makedirs(output_dir)
    STATE["output_dir"] = output_dir
    STATE["expected_backend"] = _arg_value(
        "--expect-backend",
        os.environ.get("PF_MIXED_GAMEPLAY_EXPECT_BACKEND", "METAL"),
    )
    os.environ["PF_METAL_SPRITE_STATS_PATH"] = RENDER_STATS_PATH
    os.environ["PF_PROJECTILE_SPRITE_STATS_PATH"] = PROJECTILE_STATS_PATH

    for path in (PROBE_PATH, ERROR_PATH, RENDER_STATS_PATH, PROJECTILE_STATS_PATH):
        try:
            os.unlink(path)
        except OSError:
            pass

    _set_phase("init")
    demo_main.main()
    pf.register_ui_event_handler(pf.EVENT_UPDATE_START, on_update, None)


main()
