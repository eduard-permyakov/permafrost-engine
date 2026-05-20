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


PROBE_PATH = "/tmp/pf_metal_gameplay_effects_probe.txt"
ERROR_PATH = "/tmp/pf_metal_gameplay_effects_probe_error.txt"
RENDER_STATS_PATH = "/tmp/pf_metal_gameplay_effects_render_stats.txt"
PROJECTILE_STATS_PATH = "/tmp/pf_metal_gameplay_effects_projectile_stats.txt"
DEFAULT_OUTPUT_DIR = "visual_parity_captures/gameplay-effects-probe"

EXPECTED_RENDER_SHEETS = set((
    "projectile_trail.png",
    "impact_burst.png",
    "fire_loop.png",
    "smoke_puff.png",
))

STATE = {
    "phase": "init",
    "ticks": 0,
    "phase_started_at": None,
    "output_dir": None,
    "expected_backend": "METAL",
    "mage": None,
    "mages": [],
    "enemy": None,
    "enemy_hp": None,
    "attack_started": False,
    "capture": None,
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


def _set_phase(name):
    STATE["phase"] = name
    STATE["ticks"] = 0
    STATE["phase_started_at"] = time.monotonic()
    print("METAL_GAMEPLAY_EFFECTS_PHASE {0}".format(name))
    sys.stdout.flush()


def _phase_elapsed():
    return time.monotonic() - STATE["phase_started_at"]


def _summary_path():
    return os.path.join(STATE["output_dir"], "summary_metal.json")


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


def _entity_summary(ent, default_name):
    if ent is None:
        return None
    return {
        "uid": _safe_getattr(ent, "uid", None),
        "name": _safe_getattr(ent, "name", default_name),
        "position": _safe_ent_xz(ent),
    }


def _write_summary(status, reason=None):
    mage = STATE["mage"]
    enemy = STATE["enemy"]
    payload = {
        "status": status,
        "reason": reason,
        "backend": pf.get_render_info(),
        "mage": _entity_summary(mage, "mage"),
        "mages": [_entity_summary(ent, "mage") for ent in STATE["mages"]],
        "enemy": None if enemy is None else {
            "uid": _safe_getattr(enemy, "uid", None),
            "name": _safe_getattr(enemy, "name", "enemy"),
            "position": _safe_ent_xz(enemy),
            "hp_start": STATE["enemy_hp"],
            "hp_end": _safe_getattr(enemy, "hp", None),
        },
        "attack_started": STATE["attack_started"],
        "render_sprite_sheets": _read_sheet_stats(RENDER_STATS_PATH),
        "projectile_sprite_events": _read_projectile_events(),
        "capture": STATE["capture"],
    }
    with open(_summary_path(), "w") as outfile:
        json.dump(payload, outfile, indent=2, sort_keys=True)
        outfile.write("\n")
    print("METAL_GAMEPLAY_EFFECTS_SUMMARY {0}".format(_summary_path()))
    sys.stdout.flush()


def _fail(reason):
    _write_summary("fail", reason)
    _write(ERROR_PATH, str(reason))
    print("METAL_GAMEPLAY_EFFECTS_FAIL {0}".format(reason))
    sys.stdout.flush()
    os._exit(1)


def _capture_window_id():
    helper = os.path.join(pf.get_basedir(), "scripts/macos/pf_window_id_for_pid.swift")
    try:
        ret = subprocess.run(["/usr/bin/swift", helper, str(os.getpid())],
                             stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE,
                             universal_newlines=True,
                             timeout=3.0)
    except subprocess.TimeoutExpired:
        return None
    if ret.returncode != 0:
        return None
    window_id = ret.stdout.strip().splitlines()[-1]
    return window_id if window_id.isdigit() else None


def _capture(name):
    path = os.path.join(STATE["output_dir"], "metal_{0}.png".format(name))
    window_id = _capture_window_id()
    ret = 1
    if window_id is not None:
        try:
            ret = subprocess.run(["screencapture", "-x", "-o", "-l{0}".format(window_id), path],
                                 timeout=3.0).returncode
        except subprocess.TimeoutExpired:
            ret = 1
    if ret != 0:
        try:
            ret = subprocess.run(["screencapture", "-x", "-o", path], timeout=3.0).returncode
        except subprocess.TimeoutExpired:
            ret = 1
    if ret != 0:
        _fail("screencapture failed")
    return path


def _require_effect_assets():
    sprites_dir = os.path.join(pf.get_basedir(), "assets", "sprites")
    missing = [
        name for name in EXPECTED_RENDER_SHEETS
        if not os.path.exists(os.path.join(sprites_dir, name))
    ]
    if missing:
        _fail("missing sprite asset(s): {0}".format(",".join(sorted(missing))))


def _dist_xz(a, b):
    dx = a[0] - b[0]
    dz = a[1] - b[1]
    return math.sqrt(dx * dx + dz * dz)


def _ent_xz(ent):
    pos = ent.pos
    return (pos[0], pos[2])


def _is_mage(ent):
    name = getattr(ent, "name", "")
    cls_name = ent.__class__.__name__
    return "mage" in name.lower() or "mage" in cls_name.lower()


def _pathable_near(point, radius):
    offsets = (
        (0.0, 0.0), (6.0, 0.0), (-6.0, 0.0), (0.0, 6.0), (0.0, -6.0),
        (10.0, 10.0), (10.0, -10.0), (-10.0, 10.0), (-10.0, -10.0),
    )
    for dx, dz in offsets:
        target = pf.map_nearest_pathable((point[0] + dx, point[1] + dz), radius=radius)
        if target is not None:
            return target
    return None


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
        _fail("no friendly Mage found for gameplay effects probe")

    selected_mages = mages[:2]
    enemies = [
        ent for ent in scene
        if getattr(ent, "faction_id", None) not in (None, 1)
        and hasattr(ent, "hp")
        and getattr(ent, "selectable", False)
        and getattr(ent, "hp", 0) > 0
    ]
    if not enemies:
        _fail("no enemy target found for gameplay effects probe")

    enemy = min(enemies, key=lambda ent: _dist_xz(_ent_xz(ent), _ent_xz(selected_mages[0])))
    return selected_mages, enemy


def _set_entity_pos(ent, point):
    height = pf.map_height_at_point(point[0], point[1])
    if height is None:
        return False
    ent.pos = (point[0], height, point[1])
    return True


def _stage_combat(mages, enemy):
    mx, mz = _ent_xz(mages[0])
    enemy_point = _pathable_near((mx + 28.0, mz + 4.0), enemy.selection_radius)
    if enemy_point is None or not _set_entity_pos(enemy, enemy_point):
        _fail("could not stage enemy near Mage")

    ex, ez = _ent_xz(enemy)
    offsets = ((-26.0, -5.0), (-26.0, 7.0))
    staged = 0
    for mage, offset in zip(mages, offsets):
        mage_point = _pathable_near((ex + offset[0], ez + offset[1]), mage.selection_radius)
        if mage_point is None or not _set_entity_pos(mage, mage_point):
            continue
        if hasattr(mage, "stop"):
            mage.stop()
        staged += 1
    if staged == 0:
        _fail("could not stage Mage near enemy")

    if hasattr(enemy, "stop"):
        enemy.stop()
    pf.set_unit_selection(mages[:staged])
    pf.get_active_camera().center_over_location(_ent_xz(enemy))


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


def on_attack_start(user, event):
    del user
    del event
    STATE["attack_started"] = True


def _issue_attack():
    mages = STATE["mages"]
    enemy = STATE["enemy"]
    STATE["enemy_hp"] = enemy.hp
    for mage in mages:
        mage.register(pf.EVENT_ATTACK_START, on_attack_start, None)
        mage.attack(_ent_xz(enemy))
    _set_phase("combat")


def _effects_ready():
    render_sheets = _read_sheet_stats(RENDER_STATS_PATH)
    missing_render = EXPECTED_RENDER_SHEETS - set(render_sheets.keys())
    if missing_render:
        return False, "renderer missing {0}".format(",".join(sorted(missing_render)))

    projectile_events = _read_projectile_events()
    mage_parents = set(str(getattr(ent, "uid", "")) for ent in STATE["mages"])
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
    return True, None


def _succeed():
    STATE["capture"] = _capture("gameplay_effects")
    _write_summary("pass")
    marker = (
        "METAL_GAMEPLAY_EFFECTS_PASS backend={backend} "
        "trail=1 impact=1 fire=1 smoke=1 capture={capture}"
    ).format(
        backend=pf.get_render_info().get("backend"),
        capture=STATE["capture"],
    )
    _write(PROBE_PATH, marker)
    print(marker)
    sys.stdout.flush()
    os._exit(0)


def on_update(user, event):
    del user
    del event
    STATE["ticks"] += 1

    if STATE["phase"] == "init":
        backend = pf.get_render_info().get("backend")
        if STATE["expected_backend"] and backend != STATE["expected_backend"]:
            _fail("expected {0} backend, got {1}".format(STATE["expected_backend"], backend))
        _require_effect_assets()
        STATE["mages"], STATE["enemy"] = _choose_mages_and_enemy()
        STATE["mage"] = STATE["mages"][0]
        _stage_combat(STATE["mages"], STATE["enemy"])
        _spawn_fire_smoke_fixture(STATE["enemy"])
        _issue_attack()
        return

    if STATE["phase"] == "combat":
        ready, reason = _effects_ready()
        if ready:
            _succeed()
            return
        if _phase_elapsed() > 24.0:
            _fail(reason or "timed out waiting for gameplay effects")


def main():
    output_dir = _arg_value("--output-dir",
                            os.environ.get("PF_GAMEPLAY_EFFECTS_OUTPUT_DIR", DEFAULT_OUTPUT_DIR))
    if not os.path.isabs(output_dir):
        output_dir = os.path.join(pf.get_basedir(), output_dir)
    if not os.path.isdir(output_dir):
        os.makedirs(output_dir)
    STATE["output_dir"] = output_dir
    STATE["expected_backend"] = _arg_value(
        "--expect-backend",
        os.environ.get("PF_GAMEPLAY_EFFECTS_EXPECT_BACKEND", "METAL"),
    )
    os.environ["PF_METAL_SPRITE_STATS_PATH"] = RENDER_STATS_PATH
    os.environ["PF_PROJECTILE_SPRITE_STATS_PATH"] = PROJECTILE_STATS_PATH
    for path in (RENDER_STATS_PATH, PROJECTILE_STATS_PATH):
        try:
            os.unlink(path)
        except OSError:
            pass

    demo_main.main()
    pf.register_ui_event_handler(pf.EVENT_UPDATE_START, on_update, None)


main()
