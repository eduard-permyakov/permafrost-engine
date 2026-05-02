import json
import os
import sys

import pf

sys.path.insert(0, pf.get_basedir() + "/scripts")

import rts.main as demo_main


PROBE_PATH = "/tmp/pf_metal_capability_inventory_probe.txt"
ERROR_PATH = "/tmp/pf_metal_capability_inventory_probe_error.txt"
DEFAULT_OUTPUT_DIR = "visual_parity_captures/capability-inventory-probe"

STATE = {
    "ticks": 0,
    "output_dir": None,
    "expected_backend": "METAL",
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


def _summary_path():
    return os.path.join(STATE["output_dir"], "summary_metal_capabilities.json")


def _fail(reason):
    _write(ERROR_PATH, str(reason))
    print("METAL_CAPABILITY_INVENTORY_FAIL {0}".format(reason))
    sys.stdout.flush()
    os._exit(1)


def _safe_call(fn, *args, **kwargs):
    try:
        return {
            "ok": True,
            "value": fn(*args, **kwargs),
        }
    except Exception as exc:
        return {
            "ok": False,
            "error_type": exc.__class__.__name__,
            "error": str(exc),
        }


def _setting(name):
    return _safe_call(pf.settings_get, name)


def _settings_snapshot():
    names = (
        "pf.video.vsync",
        "pf.video.shadows_enabled",
        "pf.video.use_batch_rendering",
        "pf.video.water_reflection",
        "pf.video.water_refraction",
        "pf.game.fog_of_war_enabled",
        "pf.game.healthbar_mode",
        "pf.game.movement_use_gpu",
        "pf.game.combat_hz",
    )
    return dict((name, _setting(name)) for name in names)


def _asset_counts():
    base = pf.get_basedir()
    groups = {
        "sounds": os.path.join(base, "assets", "sounds"),
        "music": os.path.join(base, "assets", "music"),
        "sprites": os.path.join(base, "assets", "sprites"),
        "models": os.path.join(base, "assets", "models"),
        "skyboxes": os.path.join(base, "assets", "skyboxes"),
    }
    ret = {}
    for name, path in groups.items():
        count = 0
        exists = os.path.isdir(path)
        if exists:
            for _root, _dirs, files in os.walk(path):
                count += len(files)
        ret[name] = {
            "exists": exists,
            "files": count,
        }
    return ret


def _api_presence():
    names = (
        "Task",
        "show_console",
        "play_music",
        "curr_music",
        "get_all_music",
        "play_effect",
        "play_global_effect",
        "spawn_projectile",
        "spawn_sprite_animated",
        "get_minimap_position",
        "set_minimap_position",
        "update_tile",
        "map_nearest_pathable",
        "map_nearest_pathable_water",
        "map_nearest_pathable_air",
        "settings_get",
        "settings_set",
        "settings_flush",
    )
    return dict((name, hasattr(pf, name)) for name in names)


def _task_fiber_status():
    if not hasattr(pf, "Task"):
        return {"available": False, "status": "missing"}

    class ProbeTask(pf.Task):
        def __run__(self):
            if False:
                yield self.yield_()
            return None

    try:
        task = ProbeTask()
        task.run()
        return {
            "available": True,
            "status": "run_supported",
            "completed": bool(task.completed),
        }
    except Exception as exc:
        return {
            "available": True,
            "status": "run_failed",
            "error_type": exc.__class__.__name__,
            "error": str(exc),
        }


def _gpu_movement_status():
    original = pf.settings_get("pf.game.movement_use_gpu")
    ret = {
        "original": bool(original),
        "requested_true": None,
        "after_request": None,
    }
    try:
        pf.settings_set("pf.game.movement_use_gpu", True, persist=False)
        ret["requested_true"] = True
        ret["after_request"] = bool(pf.settings_get("pf.game.movement_use_gpu"))
    except Exception as exc:
        ret["requested_true"] = False
        ret["error_type"] = exc.__class__.__name__
        ret["error"] = str(exc)
    finally:
        try:
            pf.settings_set("pf.game.movement_use_gpu", original, persist=False)
        except Exception as exc:
            ret["restore_error_type"] = exc.__class__.__name__
            ret["restore_error"] = str(exc)
    return ret


def _map_queries():
    return {
        "land": _safe_call(pf.map_nearest_pathable, (0.0, 0.0), radius=512.0),
        "water": _safe_call(pf.map_nearest_pathable_water, (0.0, 0.0), radius=1024.0),
        "air": _safe_call(pf.map_nearest_pathable_air, (0.0, 0.0), radius=512.0),
    }


def _audio_status():
    music = _safe_call(pf.get_all_music) if hasattr(pf, "get_all_music") else {"ok": False, "error": "missing API"}
    return {
        "api": {
            "play_music": hasattr(pf, "play_music"),
            "curr_music": hasattr(pf, "curr_music"),
            "get_all_music": hasattr(pf, "get_all_music"),
            "play_effect": hasattr(pf, "play_effect"),
            "play_global_effect": hasattr(pf, "play_global_effect"),
        },
        "loaded_music": music,
    }


def _write_summary():
    payload = {
        "python": {
            "version": sys.version,
            "major": sys.version_info[0],
            "minor": sys.version_info[1],
        },
        "backend": pf.get_render_info(),
        "render_settings": _safe_call(pf.get_render_settings),
        "settings": _settings_snapshot(),
        "api_presence": _api_presence(),
        "console_show": _safe_call(pf.show_console) if hasattr(pf, "show_console") else {"ok": False, "error": "missing API"},
        "task_fibers": _task_fiber_status(),
        "gpu_movement": _gpu_movement_status(),
        "map_queries": _map_queries(),
        "factions": _safe_call(pf.get_factions_list),
        "resources": _safe_call(pf.get_resource_list),
        "audio": _audio_status(),
        "assets": _asset_counts(),
    }
    with open(_summary_path(), "w") as outfile:
        json.dump(payload, outfile, indent=2, sort_keys=True)
        outfile.write("\n")
    marker = (
        "METAL_CAPABILITY_INVENTORY_READY backend={backend} "
        "gpu_movement_after_request={gpu} task_status={task}"
    ).format(
        backend=payload["backend"].get("backend"),
        gpu=payload["gpu_movement"].get("after_request"),
        task=payload["task_fibers"].get("status"),
    )
    _write(PROBE_PATH, marker)
    print(marker)
    print("METAL_CAPABILITY_INVENTORY_SUMMARY {0}".format(_summary_path()))
    sys.stdout.flush()


def on_update(user, event):
    del user
    del event
    STATE["ticks"] += 1
    if STATE["ticks"] < 30:
        return
    backend = pf.get_render_info().get("backend")
    if STATE["expected_backend"] and backend != STATE["expected_backend"]:
        _fail("expected {0} backend, got {1}".format(STATE["expected_backend"], backend))
    _write_summary()
    os._exit(0)


def main():
    output_dir = _arg_value("--output-dir",
                            os.environ.get("PF_CAPABILITY_INVENTORY_OUTPUT_DIR", DEFAULT_OUTPUT_DIR))
    if not os.path.isabs(output_dir):
        output_dir = os.path.join(pf.get_basedir(), output_dir)
    if not os.path.isdir(output_dir):
        os.makedirs(output_dir)
    STATE["output_dir"] = output_dir
    STATE["expected_backend"] = _arg_value(
        "--expect-backend",
        os.environ.get("PF_CAPABILITY_INVENTORY_EXPECT_BACKEND", "METAL"),
    )
    for path in (PROBE_PATH, ERROR_PATH):
        try:
            os.unlink(path)
        except OSError:
            pass

    demo_main.main()
    pf.register_ui_event_handler(pf.EVENT_UPDATE_START, on_update, None)


main()
