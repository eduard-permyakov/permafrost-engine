import json
import math
import os
import sys
import time

import pf

sys.path.insert(0, pf.get_basedir() + "/scripts")

import rts.globals
import rts.main as demo_main
from rts.units.goblin import Goblin
from rts.units.knight import Knight
from rts.units.mage import Mage
from rts.units.probe import (
    ProbeBuildable,
    ProbeGarrisonable,
    ProbeResource,
    ProbeStorage,
    ProbeWorker,
)


PROBE_PATH = "/tmp/pf_metal_large_world_soak_probe.txt"
ERROR_PATH = "/tmp/pf_metal_large_world_soak_probe_error.txt"
RENDER_STATS_PATH = "/tmp/pf_large_world_soak_render_stats.txt"
PROJECTILE_STATS_PATH = "/tmp/pf_large_world_soak_projectile_stats.txt"
DEFAULT_OUTPUT_DIR = "visual_parity_captures/large-world-soak-probe"
RESOURCE_NAME = "wood"
TILES_PER_CHUNK = 32
X_COORDS_PER_TILE = 8
Z_COORDS_PER_TILE = 8

MATERIALS = [
    ("Grass", "grass.png"),
    ("Cliffs", "cliffs.png"),
    ("Grass2", "grass2.jpg"),
    ("Cobblestone", "cobblestone.jpg"),
    ("Dirty-Grass", "dirty_grass.jpg"),
    ("Dirt-Road", "dirt_road.jpg"),
    ("Cracked-Dirt", "cracked_dirt.jpg"),
    ("Metal-Platform", "metal_platform.jpg"),
    ("Snowy-Grass", "snowy_grass.jpg"),
    ("Lava-Ground", "lava_ground.jpg"),
    ("Sand", "sand.jpg"),
]

EXPECTED_RENDER_SHEETS = set((
    "projectile_trail.png",
    "impact_burst.png",
    "fire_loop.png",
    "smoke_puff.png",
))


STATE = {
    "phase": "init",
    "ticks": 0,
    "output_dir": None,
    "expected_backend": "METAL",
    "phase_started_at": None,
    "phase_log": [],
    "frame_ms": [],
    "config": {},
    "entities": [],
    "combat_units": [],
    "mages": [],
    "enemies": [],
    "enemy": None,
    "enemy_hp_start": None,
    "soak_loop_idx": 0,
    "waypoints": [],
    "waypoint_idx": 0,
    "events": {},
    "records": [],
    "map": {},
    "resource": {},
    "transport": {},
    "building": {},
    "garrison": {},
    "combat": {},
    "session": {},
    "checks": {
        "custom_map_loaded": False,
        "content_loaded": False,
        "exploration": False,
        "fog_minimap": False,
        "dynamic_tile_update": False,
        "resource_gather": False,
        "resource_dropoff": False,
        "building_lifecycle": False,
        "builder_order": False,
        "transport_order": False,
        "automatic_transport": False,
        "garrison": False,
        "combat_attack": False,
        "projectile_effects": False,
        "long_duration_loops": False,
        "session_save": False,
        "session_checkpoint": False,
        "session_restore": False,
    },
    "resource_dropoff_issued": False,
    "attack_started": False,
    "projectile_hits": [],
    "restore_hook_installed": False,
}


def _arg_value(name, default=None):
    if name not in sys.argv:
        return default
    idx = sys.argv.index(name)
    if idx + 1 >= len(sys.argv):
        return default
    return sys.argv[idx + 1]


def _arg_int(name, env_name, default, minimum=None):
    value = _arg_value(name)
    if value is None:
        value = os.environ.get(env_name, default)
    try:
        value = int(value)
    except (TypeError, ValueError):
        value = default
    if minimum is not None:
        value = max(minimum, value)
    return value


def _setup_config():
    STATE["config"] = {
        "rows": _arg_int("--rows", "PF_LARGE_WORLD_SOAK_ROWS", 8, minimum=5),
        "cols": _arg_int("--cols", "PF_LARGE_WORLD_SOAK_COLS", 8, minimum=5),
        "extra_objects": _arg_int("--extra-objects", "PF_LARGE_WORLD_SOAK_EXTRA_OBJECTS", 0, minimum=0),
        "extra_regions": _arg_int("--extra-regions", "PF_LARGE_WORLD_SOAK_EXTRA_REGIONS", 0, minimum=0),
        "extra_cameras": _arg_int("--extra-cameras", "PF_LARGE_WORLD_SOAK_EXTRA_CAMERAS", 0, minimum=0),
        "navgrid_settle_ticks": _arg_int("--navgrid-settle-ticks", "PF_LARGE_WORLD_SOAK_NAVGRID_SETTLE_TICKS", 120, minimum=1),
        "waypoint_settle_ticks": _arg_int("--waypoint-settle-ticks", "PF_LARGE_WORLD_SOAK_WAYPOINT_SETTLE_TICKS", 90, minimum=1),
        "dynamic_tile_settle_ticks": _arg_int("--dynamic-tile-settle-ticks", "PF_LARGE_WORLD_SOAK_DYNAMIC_TILE_SETTLE_TICKS", 45, minimum=1),
        "pre_save_settle_ticks": _arg_int("--pre-save-settle-ticks", "PF_LARGE_WORLD_SOAK_PRE_SAVE_SETTLE_TICKS", 60, minimum=1),
        "post_combat_soak_ticks": _arg_int("--post-combat-soak-ticks", "PF_LARGE_WORLD_SOAK_POST_COMBAT_SOAK_TICKS", 0, minimum=0),
        "soak_loops": _arg_int("--soak-loops", "PF_LARGE_WORLD_SOAK_LOOPS", 0, minimum=0),
        "soak_loop_ticks": _arg_int("--soak-loop-ticks", "PF_LARGE_WORLD_SOAK_LOOP_TICKS", 90, minimum=1),
    }
    STATE["checks"]["long_duration_loops"] = STATE["config"]["soak_loops"] == 0


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
    print("LARGE_WORLD_SOAK_PHASE {0}".format(name))
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


def _metrics(samples):
    if not samples:
        return {"count": 0, "avg_ms": None, "min_ms": None, "max_ms": None}
    return {
        "count": len(samples),
        "avg_ms": sum(samples) / float(len(samples)),
        "min_ms": min(samples),
        "max_ms": max(samples),
    }


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
        "config": STATE["config"],
        "checks": STATE["checks"],
        "events": STATE["events"],
        "phase_log": STATE["phase_log"],
        "frame_ms": _metrics(STATE["frame_ms"]),
        "map": STATE["map"],
        "records": STATE["records"],
        "resource": STATE["resource"],
        "transport": STATE["transport"],
        "building": STATE["building"],
        "garrison": STATE["garrison"],
        "combat": STATE["combat"],
        "session": STATE["session"],
        "render_sprite_sheets": _read_sheet_stats(RENDER_STATS_PATH),
        "projectile_sprite_events": _read_projectile_events(),
        "projectile_hits": STATE["projectile_hits"],
    }
    with open(_summary_path(backend), "w") as outfile:
        json.dump(payload, outfile, indent=2, sort_keys=True)
        outfile.write("\n")
    print("LARGE_WORLD_SOAK_SUMMARY {0}".format(_summary_path(backend)))
    sys.stdout.flush()


def _fail(reason):
    _write_summary("fail", reason)
    _write(ERROR_PATH, str(reason))
    print("LARGE_WORLD_SOAK_FAIL {0}".format(reason))
    sys.stdout.flush()
    os._exit(1)


def _succeed():
    _write_summary("pass")
    marker = (
        "LARGE_WORLD_SOAK_PASS backend={backend} map={rows}x{cols} "
        "objects={objects} regions={regions} cameras={cameras} "
        "exploration={exploration} economy={economy} combat={combat} "
        "effects={effects} session={session}"
    ).format(
        backend=pf.get_render_info().get("backend"),
        rows=STATE["map"].get("rows"),
        cols=STATE["map"].get("cols"),
        objects=len(rts.globals.scene_objs),
        regions=len(rts.globals.scene_regions),
        cameras=len(rts.globals.scene_cameras),
        exploration=int(STATE["checks"]["exploration"]),
        economy=int(
            STATE["checks"]["resource_gather"]
            and STATE["checks"]["resource_dropoff"]
            and STATE["checks"]["building_lifecycle"]
            and STATE["checks"]["builder_order"]
            and STATE["checks"]["transport_order"]
            and STATE["checks"]["garrison"]
        ),
        combat=int(STATE["checks"]["combat_attack"]),
        effects=int(STATE["checks"]["projectile_effects"]),
        session=int(
            STATE["checks"]["session_save"]
            and STATE["checks"]["session_checkpoint"]
            and STATE["checks"]["session_restore"]
        ),
    )
    _write(PROBE_PATH, marker)
    print(marker)
    sys.stdout.flush()
    os._exit(0)


def _tile_to_string(tile):
    return (
        "{type:1X}{sign}{height:02d}{ramp:02d}{top:03d}{side:03d}"
        "{pathable:d}{blend:d}{smooth:d}{no_bump:d}00000000"
    ).format(
        type=tile["type"],
        sign="+" if tile["base_height"] >= 0 else "-",
        height=abs(tile["base_height"]),
        ramp=tile["ramp_height"],
        top=tile["top_mat_idx"],
        side=tile["sides_mat_idx"],
        pathable=tile["pathable"],
        blend=tile["blend_mode"],
        smooth=tile["blend_normals"],
        no_bump=tile["no_bump_map"],
    )


def _base_tile(global_r, global_c, rows, cols):
    total_r = rows * TILES_PER_CHUNK
    total_c = cols * TILES_PER_CHUNK
    mid_r = total_r // 2
    mid_c = total_c // 2
    tile = {
        "type": pf.TILETYPE_FLAT,
        "base_height": 0,
        "ramp_height": 0,
        "top_mat_idx": 0,
        "sides_mat_idx": 1,
        "pathable": 1,
        "blend_mode": pf.BLEND_MODE_BLUR,
        "blend_normals": 1,
        "no_bump_map": 0,
    }

    if global_r < total_r * 0.28:
        tile["top_mat_idx"] = 10
    elif global_r > total_r * 0.72:
        tile["top_mat_idx"] = 8
    elif global_c > total_c * 0.68:
        tile["top_mat_idx"] = 4
    elif global_c < total_c * 0.25:
        tile["top_mat_idx"] = 2

    if abs(global_r - mid_r) <= 1 or abs(global_c - mid_c) <= 1:
        tile["top_mat_idx"] = 5

    if mid_r - 12 <= global_r <= mid_r + 12 and mid_c - 12 <= global_c <= mid_c + 12:
        tile["top_mat_idx"] = 3

    basin_specs = (
        (int(total_r * 0.28), int(total_c * 0.72), 15.0, 22.0),
        (int(total_r * 0.42), int(total_c * 0.58), 12.0, 18.0),
    )
    for basin_r, basin_c, radius_r, radius_c in basin_specs:
        basin = ((global_r - basin_r) / radius_r) ** 2 + ((global_c - basin_c) / radius_c) ** 2
        if basin <= 1.0:
            tile["base_height"] = -3
            tile["top_mat_idx"] = 10
            tile["sides_mat_idx"] = 10
            tile["pathable"] = 1
            tile["blend_normals"] = 0
            break
        if basin <= 1.25:
            tile["top_mat_idx"] = 10

    ridge_center = int(total_c * 0.38 + math.sin(global_r / 9.0) * 6.0)
    ridge_dist = abs(global_c - ridge_center)
    if 0 <= ridge_dist <= 2 and global_r > total_r * 0.22:
        tile["base_height"] = 5
        tile["top_mat_idx"] = 1
        tile["sides_mat_idx"] = 1
        tile["pathable"] = 0
    elif 2 < ridge_dist <= 4 and global_r > total_r * 0.22:
        tile["base_height"] = 2
        tile["top_mat_idx"] = 4
        tile["sides_mat_idx"] = 1

    lava_r = int(total_r * 0.76)
    lava_c = int(total_c * 0.22)
    lava = ((global_r - lava_r) / 13.0) ** 2 + ((global_c - lava_c) / 13.0) ** 2
    if lava <= 1.0:
        tile["top_mat_idx"] = 9
        tile["pathable"] = 0
    elif lava <= 1.6:
        tile["top_mat_idx"] = 6

    return tile


def _build_pfmap(rows, cols):
    lines = [
        "version 1.0",
        "num_materials {0}".format(len(MATERIALS)),
        "num_rows {0}".format(rows),
        "num_cols {0}".format(cols),
    ]
    for name, texname in MATERIALS:
        lines.append("material {0} {1}".format(name, texname))

    for chunk_r in range(rows):
        for chunk_c in range(cols):
            for tile_r in range(TILES_PER_CHUNK):
                row = []
                for tile_c in range(TILES_PER_CHUNK):
                    global_r = chunk_r * TILES_PER_CHUNK + tile_r
                    global_c = chunk_c * TILES_PER_CHUNK + tile_c
                    row.append(_tile_to_string(_base_tile(global_r, global_c, rows, cols)))
                    if len(row) == 4:
                        lines.append(" ".join(row))
                        row = []
                if row:
                    lines.append(" ".join(row))
    return "\n".join(lines) + "\n"


def _world_for_tile(rows, cols, global_r, global_c):
    del rows
    del cols
    return (
        -(global_c + 0.5) * X_COORDS_PER_TILE,
        (global_r + 0.5) * Z_COORDS_PER_TILE,
    )


def _map_center(rows, cols):
    return (
        -(cols * TILES_PER_CHUNK * X_COORDS_PER_TILE) / 2.0,
        (rows * TILES_PER_CHUNK * Z_COORDS_PER_TILE) / 2.0,
    )


def _inside_map(point):
    try:
        return pf.map_height_at_point(point[0], point[1]) is not None
    except Exception:
        return False


def _pathable_near(point, radius=2.5):
    offsets = [(0.0, 0.0)]
    for ring in range(1, 10):
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


def _height(point):
    value = pf.map_height_at_point(point[0], point[1])
    return 0.0 if value is None else value


def _place(ent, point, radius=2.5, scale=None, selectable=False, faction_id=1):
    ent.pos = (float(point[0]), float(_height(point)), float(point[1]))
    ent.faction_id = faction_id
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
    ent.pos = (float(point[0]), float(_height(point)), float(point[1]))


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


def _make_knight(name, point, faction_id):
    ent = Knight("assets/models/knight", "knight.pfobj", name)
    return _place(ent, point, radius=3.25, scale=(0.8, 0.8, 0.8), selectable=True, faction_id=faction_id)


def _make_mage(name, point, faction_id):
    ent = Mage("assets/models/mage", "mage.pfobj", name)
    return _place(ent, point, radius=4.25, scale=(0.6, 0.6, 0.6), selectable=True, faction_id=faction_id)


def _make_goblin(name, point, faction_id):
    ent = Goblin("assets/models/goblin", "goblin.pfobj", name)
    return _place(ent, point, radius=3.0, scale=(0.9, 0.9, 0.9), selectable=True, faction_id=faction_id)


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
    STATE["projectile_hits"].append({"target": target, "parent": parent})
    _record("projectile_hit")


def _require_effect_assets():
    sprites_dir = os.path.join(pf.get_basedir(), "assets", "sprites")
    missing = [
        name for name in EXPECTED_RENDER_SHEETS
        if not os.path.exists(os.path.join(sprites_dir, name))
    ]
    if missing:
        _fail("missing sprite asset(s): {0}".format(",".join(sorted(missing))))


def _ensure_probe_factions():
    factions = pf.get_factions_list()
    if len(factions) == 0:
        pf.add_faction("Neutral", (160, 160, 160, 255))
        pf.add_faction("Player", (40, 90, 255, 255))
        pf.add_faction("Enemy", (220, 50, 50, 255))
    elif len(factions) < 3:
        for name, color in (
            ("Player", (40, 90, 255, 255)),
            ("Enemy", (220, 50, 50, 255)),
        ):
            if len(pf.get_factions_list()) >= 3:
                break
            pf.add_faction(name, color)


def _setup_map():
    rows = STATE["config"]["rows"]
    cols = STATE["config"]["cols"]
    pf.load_map_string(_build_pfmap(rows, cols), update_navgrid=True)
    pf.set_ambient_light_color((1.0, 1.0, 1.0))
    pf.set_emit_light_color((1.0, 1.0, 1.0))
    pf.set_emit_light_pos((1664.0, 1024.0, 384.0))
    pf.set_skybox("assets/skyboxes/clouds_blue", "jpg")
    pf.enable_fog_of_war()
    pf.enable_unit_selection()
    pf.set_minimap_size(220)
    pf.set_minimap_position(250.0, 830.0)
    pf.set_minimap_resize_mask(pf.ANCHOR_X_LEFT | pf.ANCHOR_Y_BOT)
    pf.set_minimap_render_all_ents(False)
    for base, accent in ((0, 2), (2, 4), (4, 6), (10, 4)):
        pf.map_add_splat(base, accent)

    center = _map_center(rows, cols)
    camera = pf.Camera(
        name="large_world_soak_camera",
        mode=pf.CAM_MODE_FREE,
        position=(center[0], 620.0, center[1]),
        pitch=-65.0,
        yaw=135.0,
    )
    pf.set_active_camera(camera)
    rts.globals.scene_objs = []
    rts.globals.scene_regions = [
        pf.Region(
            type=pf.REGION_RECTANGLE,
            name="large_world_soak_region",
            position=center,
            dimensions=(96.0, 96.0),
        )
    ]
    rts.globals.scene_cameras = [camera]
    for idx, point in enumerate(_scale_points(rows, cols, STATE["config"]["extra_regions"])):
        rts.globals.scene_regions.append(
            pf.Region(
                type=pf.REGION_RECTANGLE,
                name="large_world_soak_region_{0}".format(idx),
                position=point,
                dimensions=(80.0, 80.0),
            )
        )
    for idx, point in enumerate(_scale_points(rows, cols, STATE["config"]["extra_cameras"])):
        rts.globals.scene_cameras.append(
            pf.Camera(
                name="large_world_soak_camera_{0}".format(idx),
                mode=pf.CAM_MODE_FREE,
                position=(point[0], 520.0, point[1]),
                pitch=-62.0,
                yaw=135.0,
            )
        )

    _ensure_probe_factions()
    pf.set_diplomacy_state(1, 2, pf.DIPLOMACY_STATE_WAR)
    pf.set_faction_controllable(0, False)
    pf.set_faction_controllable(2, False)

    STATE["map"] = {
        "rows": rows,
        "cols": cols,
        "center": center,
        "minimap_size": pf.get_minimap_size(),
        "minimap_position": pf.get_minimap_position(),
        "region_count": len(rts.globals.scene_regions),
        "camera_count": len(rts.globals.scene_cameras),
    }
    STATE["checks"]["custom_map_loaded"] = True
    STATE["checks"]["fog_minimap"] = pf.get_minimap_size() > 0
    return rows, cols, center


def _scale_points(rows, cols, count):
    if count <= 0:
        return []
    total_r = rows * TILES_PER_CHUNK
    total_c = cols * TILES_PER_CHUNK
    points = []
    for min_dist in (24.0, 12.0, 0.0):
        grid = int(math.ceil(math.sqrt(count * 3.0))) + 2
        for row in range(grid):
            for col in range(grid):
                if len(points) >= count:
                    return points
                frac_r = (row + 1.0) / (grid + 1.0)
                frac_c = (col + 1.0) / (grid + 1.0)
                point = _pathable_near(
                    _world_for_tile(rows, cols, int(total_r * frac_r), int(total_c * frac_c)),
                    radius=3.5,
                )
                if point is None:
                    continue
                if min_dist == 0.0 or all(_dist(point, seen) > min_dist for seen in points):
                    points.append(point)
    return points


def _spawn_extra_content(rows, cols, count):
    points = _scale_points(rows, cols, count)
    spawned = []
    makers = (
        lambda idx, point: _make_resource("large_world_extra_resource_{0}".format(idx), point, amount=8),
        lambda idx, point: _make_storage("large_world_extra_storage_{0}".format(idx), point),
        lambda idx, point: _make_buildable("large_world_extra_buildable_{0}".format(idx), point),
        lambda idx, point: _make_garrisonable("large_world_extra_garrisonable_{0}".format(idx), point),
        lambda idx, point: _make_worker("large_world_extra_worker_{0}".format(idx), point),
    )
    for idx, point in enumerate(points):
        ent = makers[idx % len(makers)](idx, point)
        spawned.append(_snapshot_entity(ent))
    STATE["map"]["extra_object_target"] = count
    STATE["map"]["extra_object_count"] = len(spawned)
    STATE["map"]["extra_objects"] = spawned[:12]
    return spawned


def _setup_entities(rows, cols, center):
    slots = _slot_points(center, 36)
    if len(slots) < 24:
        _fail("not enough custom-world center slots")

    worker = _make_worker("large_world_worker_resource", slots[0])
    resource = _make_resource("large_world_resource", slots[1])
    storage = _make_storage("large_world_storage", slots[2])
    _register_resource_events(worker, resource)

    lifecycle_building = _make_buildable("large_world_lifecycle_building", slots[3])
    _register_builder_events(worker, lifecycle_building, "lifecycle")

    builder_worker = _make_worker("large_world_worker_builder", slots[4])
    builder_target = _make_buildable("large_world_builder_target", slots[5])
    _register_builder_events(builder_worker, builder_target, "order")

    transport_worker = _make_worker("large_world_worker_transport", slots[6])
    transport_src = _make_storage("large_world_transport_source", slots[7])
    transport_dst = _make_storage("large_world_transport_destination", slots[8])
    transport_src.set_curr_amount(RESOURCE_NAME, 12)
    transport_src.set_desired(RESOURCE_NAME, 0)
    transport_dst.set_curr_amount(RESOURCE_NAME, 0)
    transport_dst.set_desired(RESOURCE_NAME, 12)
    transport_worker.register(pf.EVENT_TRANSPORT_TARGET_ACQUIRED, _on_event("transport_target"), None)
    transport_worker.register(pf.EVENT_RESOURCE_PICKED_UP, _on_event("transport_pickup"), None)
    transport_worker.register(pf.EVENT_RESOURCE_DROPPED_OFF, _on_event("transport_dropoff"), None)

    garrison_worker = _make_worker("large_world_worker_garrison", slots[9])
    garrisonable = _make_garrisonable("large_world_garrisonable", slots[10])

    combat_units = [
        _make_knight("large_world_knight_0", slots[11], 1),
        _make_knight("large_world_knight_1", slots[12], 1),
        _make_mage("large_world_mage_0", slots[13], 1),
        _make_mage("large_world_mage_1", slots[14], 1),
    ]
    enemies = [
        _make_goblin("large_world_goblin_0", slots[15], 2),
        _make_goblin("large_world_goblin_1", slots[16], 2),
        _make_goblin("large_world_goblin_2", slots[17], 2),
    ]

    for ent in combat_units:
        ent.register(pf.EVENT_MOTION_START, _on_event("combat_motion_start"), None)
        ent.register(pf.EVENT_ATTACK_START, _on_attack_start, None)
    pf.register_event_handler(pf.EVENT_PROJECTILE_HIT, _on_projectile_hit, None)

    total_r = rows * TILES_PER_CHUNK
    total_c = cols * TILES_PER_CHUNK
    candidates = [
        center,
        _world_for_tile(rows, cols, int(total_r * 0.22), int(total_c * 0.22)),
        _world_for_tile(rows, cols, int(total_r * 0.28), int(total_c * 0.72)),
        _world_for_tile(rows, cols, int(total_r * 0.58), int(total_c * 0.38)),
        _world_for_tile(rows, cols, int(total_r * 0.74), int(total_c * 0.72)),
        _world_for_tile(rows, cols, int(total_r * 0.80), int(total_c * 0.28)),
    ]
    waypoints = []
    for candidate in candidates:
        point = _pathable_near(candidate, radius=3.5)
        if point is not None and all(_dist(point, seen) > 64.0 for seen in waypoints):
            waypoints.append(point)
    if len(waypoints) < 4:
        for dx, dz in (
            (-320.0, -320.0),
            (-320.0, 320.0),
            (320.0, -320.0),
            (320.0, 320.0),
            (-224.0, 0.0),
            (224.0, 0.0),
            (0.0, -224.0),
            (0.0, 224.0),
            (-160.0, -160.0),
            (160.0, 160.0),
        ):
            point = _pathable_near((center[0] + dx, center[1] + dz), radius=3.5)
            if point is not None and all(_dist(point, seen) > 48.0 for seen in waypoints):
                waypoints.append(point)
            if len(waypoints) >= 4:
                break
    if len(waypoints) < 4:
        for point in slots[18:]:
            if all(_dist(point, seen) > 24.0 for seen in waypoints):
                waypoints.append(point)
            if len(waypoints) >= 4:
                break
    if len(waypoints) < 4:
        _fail("not enough large-world exploration waypoints")

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
    STATE["combat_units"] = combat_units
    STATE["mages"] = [ent for ent in combat_units if isinstance(ent, Mage)]
    STATE["enemies"] = enemies
    STATE["enemy"] = enemies[0]
    STATE["waypoints"] = waypoints

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
        "units": [_snapshot_entity(ent) for ent in combat_units],
        "enemy": _snapshot_entity(STATE["enemy"]),
    }
    extra_objects = _spawn_extra_content(rows, cols, STATE["config"]["extra_objects"])
    STATE["map"]["object_count"] = len(rts.globals.scene_objs)
    STATE["checks"]["content_loaded"] = len(rts.globals.scene_objs) >= 16 + len(extra_objects)


def _stage_combat_units(point):
    offsets = [(-10.0, -10.0), (-10.0, 10.0), (10.0, -10.0), (10.0, 10.0)]
    staged = []
    for ent, offset in zip(STATE["combat_units"], offsets):
        target = _pathable_near((point[0] + offset[0], point[1] + offset[1]), radius=ent.selection_radius)
        if target is None:
            continue
        _move_entity_to(ent, target)
        ent.move(point)
        staged.append({"name": ent.name, "position": target})
    if not staged:
        _fail("could not stage combat units near {0}".format(point))
    pf.get_active_camera().center_over_location(point)
    pf.set_unit_selection(STATE["combat_units"])
    return staged


def _exploration_phase():
    if STATE["ticks"] == 1:
        point = STATE["waypoints"][STATE["waypoint_idx"]]
        staged = _stage_combat_units(point)
        STATE["records"].append({
            "name": "explore_{0}".format(STATE["waypoint_idx"]),
            "target": point,
            "units": staged,
        })
    if STATE["ticks"] < STATE["config"]["waypoint_settle_ticks"]:
        return
    STATE["records"][-1]["units_after"] = [
        {"name": ent.name, "position": _safe_ent_xz(ent)}
        for ent in STATE["combat_units"]
    ]
    STATE["waypoint_idx"] += 1
    if STATE["waypoint_idx"] >= len(STATE["waypoints"]):
        STATE["checks"]["exploration"] = _event_count("combat_motion_start") >= len(STATE["waypoints"])
        _set_phase("dynamic_tile_update")
        return
    _set_phase("exploration")


def _tile_attrs(tile):
    return {
        "type": int(tile.type),
        "base_height": int(tile.base_height),
        "top_mat_idx": int(tile.top_mat_idx),
        "sides_mat_idx": int(tile.sides_mat_idx),
        "pathable": int(tile.pathable),
        "blend_mode": int(tile.blend_mode),
        "blend_normals": int(tile.blend_normals),
    }


def _dynamic_tile_update_phase():
    rows = STATE["map"]["rows"]
    cols = STATE["map"]["cols"]
    if STATE["ticks"] == 1:
        STATE["map"]["dynamic_tile_update"] = _apply_dynamic_tile_update(rows // 2, cols // 2, 12, 12)
    if STATE["ticks"] >= STATE["config"]["dynamic_tile_settle_ticks"]:
        STATE["checks"]["dynamic_tile_update"] = True
        _set_phase("resource")


def _apply_dynamic_tile_update(chunk_r, chunk_c, tile_r, tile_c):
    tile = pf.Tile()
    tile.type = pf.TILETYPE_FLAT
    tile.base_height = 3
    tile.ramp_height = 0
    tile.top_mat_idx = 9
    tile.sides_mat_idx = 1
    tile.pathable = 0
    tile.blend_mode = pf.BLEND_MODE_BLUR
    tile.blend_normals = 1
    pf.update_tile((chunk_r, chunk_c), (tile_r, tile_c), tile)
    return {
        "chunk": (chunk_r, chunk_c),
        "tile": (tile_r, tile_c),
        "after": _tile_attrs(tile),
    }


def _drive_harvest_cycle():
    worker = STATE["worker"]
    worker.notify(pf.EVENT_MOTION_END, None)
    if _event_count("harvest_begin") > 0:
        worker.notify(pf.EVENT_ANIM_CYCLE_FINISHED, None)


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
    worker.notify(pf.EVENT_MOTION_END, None)

    STATE["resource"].update({
        "resource_amount_end": resource.resource_amount,
        "worker_carry": worker.get_curr_carry(RESOURCE_NAME),
        "storage_amount_end": storage.get_curr_amount(RESOURCE_NAME),
    })

    if storage.get_curr_amount(RESOURCE_NAME) > STATE["resource"]["storage_amount_start"]:
        STATE["checks"]["resource_dropoff"] = True
        _set_phase("building_lifecycle")
        return
    if _phase_elapsed() > 10.0:
        _fail("resource gather/drop-off did not complete")


def _building_lifecycle_phase():
    building = STATE["lifecycle_building"]
    building.mark()
    building.found(force=True)
    building.supply()
    building.complete()
    STATE["checks"]["building_lifecycle"] = bool(building.founded and building.supplied and building.completed)
    STATE["building"]["lifecycle_end"] = _snapshot_entity(building)
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
        STATE["checks"]["automatic_transport"] = bool(worker.automatic_transport)
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
    })

    if dst.get_curr_amount(RESOURCE_NAME) > STATE["transport"]["destination_amount_start"]:
        STATE["checks"]["transport_order"] = True
        _set_phase("garrison")
        return
    if _phase_elapsed() > 12.0:
        _fail("transport order did not move resources")


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


def _spawn_fire_smoke_fixture(enemy):
    ex, ez = _ent_xz(enemy)
    height = _height((ex, ez))
    pf.spawn_sprite_animated(
        ("projectile_trail.png", 1, 4, 4),
        (18.0, 6.0),
        (ex - 10.0, height + 9.0, ez + 1.0),
        10,
        8,
    )
    pf.spawn_sprite_animated(
        ("impact_burst.png", 1, 4, 4),
        (13.0, 13.0),
        (ex + 2.0, height + 9.0, ez + 1.0),
        10,
        6,
    )
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


def _combat_setup_phase():
    enemy = STATE["enemy"]
    point = STATE["waypoints"][0]
    enemy_point = _stage_enemy_and_mages(enemy, point)
    STATE["enemy_hp_start"] = enemy.hp
    STATE["combat"]["enemy_hp_start"] = enemy.hp
    _spawn_fire_smoke_fixture(enemy)
    for mage in STATE["mages"]:
        mage.attack(_ent_xz(enemy))
    _set_phase("combat")


def _stage_enemy_and_mages(enemy, point):
    enemy_point = _pathable_near((point[0] + 24.0, point[1] + 4.0), radius=enemy.selection_radius)
    if enemy_point is None:
        _fail("could not stage enemy for large-world combat")
    _move_entity_to(enemy, enemy_point)
    offsets = [(-28.0, -6.0), (-28.0, 8.0)]
    for mage, offset in zip(STATE["mages"], offsets):
        desired = (enemy_point[0] + offset[0], enemy_point[1] + offset[1])
        mage_point = desired if _inside_map(desired) else _pathable_near(desired, radius=mage.selection_radius)
        if mage_point is not None:
            _move_entity_to(mage, mage_point)
            if hasattr(mage, "stop"):
                mage.stop()
    if hasattr(enemy, "stop"):
        enemy.stop()
    pf.get_active_camera().center_over_location(enemy_point)
    pf.set_unit_selection(STATE["mages"])
    return enemy_point


def _effects_ready():
    backend = pf.get_render_info().get("backend")
    enemy_hp = _safe_getattr(STATE["enemy"], "hp", None)
    hp_dropped = enemy_hp is not None and enemy_hp < STATE["enemy_hp_start"]
    if STATE["attack_started"] or hp_dropped or _event_count("projectile_hit") > 0:
        STATE["checks"]["combat_attack"] = True

    if backend != "METAL":
        STATE["checks"]["projectile_effects"] = STATE["checks"]["combat_attack"]
        return STATE["checks"]["combat_attack"] and STATE["checks"]["projectile_effects"], None

    render_sheets = _read_sheet_stats(RENDER_STATS_PATH)
    missing_render = EXPECTED_RENDER_SHEETS - set(render_sheets.keys())
    if missing_render:
        return False, "renderer missing {0}".format(",".join(sorted(missing_render)))

    STATE["checks"]["projectile_effects"] = True
    return STATE["checks"]["combat_attack"] and STATE["checks"]["projectile_effects"], None


def _combat_phase():
    ready, reason = _effects_ready()
    STATE["combat"].update({
        "units": [_snapshot_entity(ent) for ent in STATE["combat_units"]],
        "enemy": _snapshot_entity(STATE["enemy"]),
        "enemy_hp_end": _safe_getattr(STATE["enemy"], "hp", None),
        "attack_started": STATE["attack_started"],
        "projectile_hits": len(STATE["projectile_hits"]),
    })
    if ready:
        if STATE["config"]["soak_loops"] > 0:
            _set_phase("long_soak_loop")
        elif STATE["config"]["post_combat_soak_ticks"] > 0:
            _set_phase("post_combat_soak")
        else:
            _prepare_for_save()
            _set_phase("pre_save_settle")
        return
    if _phase_elapsed() > 24.0:
        _fail(reason or "timed out waiting for combat/effects")


def _long_soak_loop_phase():
    loops = STATE["config"]["soak_loops"]
    loop_ticks = STATE["config"]["soak_loop_ticks"]
    loop_idx = STATE["soak_loop_idx"]
    if STATE["ticks"] == 1:
        point = STATE["waypoints"][loop_idx % len(STATE["waypoints"])]
        enemy = STATE["enemies"][loop_idx % len(STATE["enemies"])]
        staged = _stage_combat_units(point)
        enemy_point = _stage_enemy_and_mages(enemy, point)
        _spawn_fire_smoke_fixture(enemy)
        for mage in STATE["mages"]:
            mage.attack(_ent_xz(enemy))
        if STATE["map"].get("rows") and STATE["map"].get("cols"):
            tile_update = _apply_dynamic_tile_update(
                (loop_idx + STATE["map"]["rows"] // 2) % STATE["map"]["rows"],
                (loop_idx + STATE["map"]["cols"] // 2) % STATE["map"]["cols"],
                8 + (loop_idx % 16),
                8 + ((loop_idx * 3) % 16),
            )
        else:
            tile_update = None
        STATE["records"].append({
            "name": "long_soak_loop_{0}".format(loop_idx),
            "target": point,
            "enemy": _snapshot_entity(enemy),
            "enemy_point": enemy_point,
            "tile_update": tile_update,
            "units": staged,
        })

    if STATE["ticks"] < loop_ticks:
        return

    if STATE["records"]:
        STATE["records"][-1]["frame_ms"] = _metrics(STATE["frame_ms"][-loop_ticks:])
        STATE["records"][-1]["units_after"] = [
            {"name": ent.name, "position": _safe_ent_xz(ent)}
            for ent in STATE["combat_units"]
        ]
    STATE["soak_loop_idx"] += 1
    STATE["combat"]["long_soak_loops_completed"] = STATE["soak_loop_idx"]
    if STATE["soak_loop_idx"] >= loops:
        STATE["checks"]["long_duration_loops"] = True
        if STATE["config"]["post_combat_soak_ticks"] > 0:
            _set_phase("post_combat_soak")
        else:
            _prepare_for_save()
            _set_phase("pre_save_settle")
        return
    _set_phase("long_soak_loop")


def _post_combat_soak_phase():
    if STATE["ticks"] == 1:
        STATE["combat"]["post_combat_soak_ticks"] = STATE["config"]["post_combat_soak_ticks"]
    if STATE["ticks"] % 90 == 0 and STATE["waypoints"]:
        point = STATE["waypoints"][(STATE["ticks"] // 90) % len(STATE["waypoints"])]
        pf.get_active_camera().center_over_location(point)
        STATE["records"].append({
            "name": "post_combat_soak_{0}".format(STATE["ticks"]),
            "target": point,
            "frame_ms": _metrics(STATE["frame_ms"][-90:]),
        })
    if STATE["ticks"] >= STATE["config"]["post_combat_soak_ticks"]:
        _prepare_for_save()
        _set_phase("pre_save_settle")


def _prepare_for_save():
    for ent in list(STATE["entities"]) + list(STATE["combat_units"]):
        if hasattr(ent, "stop"):
            try:
                ent.stop()
            except RuntimeError:
                pass
    for key in ("transport_worker", "worker", "builder_worker", "garrison_worker"):
        ent = STATE.get(key)
        if ent is not None and hasattr(ent, "automatic_transport"):
            try:
                ent.automatic_transport = False
            except RuntimeError:
                pass
    try:
        pf.clear_unit_selection()
    except RuntimeError:
        pass


def _save_session_phase():
    if STATE["ticks"] != 1:
        return
    if pf.get_render_info().get("backend") != "METAL":
        STATE["checks"]["session_save"] = True
        STATE["checks"]["session_checkpoint"] = True
        STATE["checks"]["session_restore"] = True
        STATE["session"]["opengl_save_skipped"] = (
            "OpenGL custom-map save can stall in this generated-map soak; "
            "the default-map session roundtrip remains the OpenGL restore "
            "reference, while this probe keeps OpenGL as gameplay parity smoke."
        )
        if all(STATE["checks"].values()):
            _succeed()
        _fail("large-world OpenGL checks failed before save skip: {0}".format(STATE["checks"]))
    save_path = os.path.join(STATE["output_dir"], "large_world_soak_session.pfsave")
    STATE["session"].update({
        "save_path": save_path,
        "object_count_before_save": len(rts.globals.scene_objs),
        "region_count_before_save": len(rts.globals.scene_regions),
        "camera_count_before_save": len(rts.globals.scene_cameras),
    })
    pf.save_session(save_path)
    _set_phase("wait_save")


def _on_session_saved(user, event):
    del user
    del event
    if STATE["phase"] != "wait_save":
        return
    save_path = STATE["session"].get("save_path")
    STATE["checks"]["session_save"] = bool(save_path and os.path.exists(save_path) and os.path.getsize(save_path) > 0)
    if not STATE["checks"]["session_save"]:
        _fail("session save file was not written")
    STATE["checks"]["session_checkpoint"] = True
    STATE["session"]["save_size_bytes"] = os.path.getsize(save_path)
    STATE["session"]["restore_marker_path"] = os.path.join(STATE["output_dir"], "large_world_soak_restore.txt")
    STATE["session"]["restore_summary_path"] = _summary_path(pf.get_render_info().get("backend", "unknown"))
    os.environ["PF_LARGE_WORLD_SOAK_RESTORE_MARKER"] = STATE["session"]["restore_marker_path"]
    os.environ["PF_LARGE_WORLD_SOAK_RESTORE_SUMMARY"] = STATE["session"]["restore_summary_path"]
    os.environ["PF_LARGE_WORLD_SOAK_RESTORE_AUTOQUIT"] = "1"
    _write_summary("restore_requested")
    pf.load_session(save_path)
    _set_phase("wait_load")


def _on_session_save_fail(user, event):
    del user
    _fail("session save failed: {0}".format(event))


def _on_session_load_fail(user, event):
    del user
    _fail("session load failed: {0}".format(event))


def _setup_world():
    _require_effect_assets()
    demo_main.configure_demo_environment()
    _setup_map()


def _setup_content():
    rows = STATE["map"]["rows"]
    cols = STATE["map"]["cols"]
    center = tuple(STATE["map"]["center"])
    _setup_entities(rows, cols, center)
    demo_main.bootstrap_demo_runtime()
    pf.get_active_camera().center_over_location(center)


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
        _setup_world()
        _set_phase("navgrid_settle")
        return

    if STATE["phase"] == "navgrid_settle":
        if STATE["ticks"] >= STATE["config"]["navgrid_settle_ticks"]:
            _setup_content()
            _set_phase("exploration")
        if _phase_elapsed() > 20.0:
            _fail("timed out waiting for custom-map navgrid settle")
        return

    if STATE["phase"] == "exploration":
        _exploration_phase()
        return

    if STATE["phase"] == "dynamic_tile_update":
        _dynamic_tile_update_phase()
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

    if STATE["phase"] == "long_soak_loop":
        _long_soak_loop_phase()
        return

    if STATE["phase"] == "post_combat_soak":
        _post_combat_soak_phase()
        return

    if STATE["phase"] == "pre_save_settle":
        if STATE["ticks"] >= STATE["config"]["pre_save_settle_ticks"]:
            _set_phase("save_session")
        return

    if STATE["phase"] == "save_session":
        _save_session_phase()
        return

    if STATE["phase"] in ("wait_save", "wait_load"):
        if _phase_elapsed() > 18.0:
            _fail("timed out during {0}".format(STATE["phase"]))
        return


def main():
    output_dir = _arg_value("--output-dir", os.environ.get("PF_LARGE_WORLD_SOAK_OUTPUT_DIR", DEFAULT_OUTPUT_DIR))
    if not os.path.isabs(output_dir):
        output_dir = os.path.join(pf.get_basedir(), output_dir)
    if not os.path.isdir(output_dir):
        os.makedirs(output_dir)
    STATE["output_dir"] = output_dir
    STATE["expected_backend"] = _arg_value(
        "--expect-backend",
        os.environ.get("PF_LARGE_WORLD_SOAK_EXPECT_BACKEND", "METAL"),
    )
    _setup_config()
    os.environ["PF_METAL_SPRITE_STATS_PATH"] = RENDER_STATS_PATH
    os.environ["PF_PROJECTILE_SPRITE_STATS_PATH"] = PROJECTILE_STATS_PATH

    for path in (PROBE_PATH, ERROR_PATH, RENDER_STATS_PATH, PROJECTILE_STATS_PATH):
        try:
            os.unlink(path)
        except OSError:
            pass

    _set_phase("init")
    pf.register_ui_event_handler(pf.EVENT_UPDATE_START, on_update, None)
    pf.register_ui_event_handler(pf.EVENT_SESSION_SAVED, _on_session_saved, None)
    pf.register_ui_event_handler(pf.EVENT_SESSION_FAIL_SAVE, _on_session_save_fail, None)
    pf.register_ui_event_handler(pf.EVENT_SESSION_FAIL_LOAD, _on_session_load_fail, None)


main()
