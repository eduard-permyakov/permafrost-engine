import json
import os
import sys

import pf

sys.path.insert(0, pf.get_basedir() + "/scripts")

import common.constants
import rts.main as demo_main


ORIGINAL_PATH = "/tmp/pf_native_settings_original.json"
PROBE_PATH = "/tmp/pf_native_settings_verify_restore_probe.txt"
ERROR_PATH = "/tmp/pf_native_settings_verify_restore_error.txt"

STATE = {
    "phase": "verify",
    "ticks": 0,
}

for path in (PROBE_PATH, ERROR_PATH):
    if os.path.exists(path):
        os.remove(path)


def finish(marker):
    print(marker)
    with open(PROBE_PATH, "w") as probe_file:
        probe_file.write(marker + "\n")
    sys.stdout.flush()
    STATE["phase"] = "done"
    pf.global_event(pf.SDL_QUIT, None)


def fail(reason):
    with open(ERROR_PATH, "w") as errfile:
        errfile.write(str(reason) + "\n")
    sys.stdout.flush()
    os._exit(1)


def current_state():
    return {
        "pf.video.vsync": bool(pf.settings_get("pf.video.vsync")),
        "pf.game.healthbar_mode": int(pf.settings_get("pf.game.healthbar_mode")),
    }


def target_state():
    return {
        "pf.video.vsync": False,
        "pf.game.healthbar_mode": int(pf.HB_MODE_NEVER),
    }


def load_original_state():
    with open(ORIGINAL_PATH, "r") as original_file:
        raw = json.load(original_file)
    return {
        "pf.video.vsync": bool(raw["pf.video.vsync"]),
        "pf.game.healthbar_mode": int(raw["pf.game.healthbar_mode"]),
    }


def ui_restored_state(original):
    return {
        "pf.video.vsync": original["pf.video.vsync"],
        "pf.game.healthbar_mode": (
            int(pf.HB_MODE_NEVER)
            if original["pf.game.healthbar_mode"] == pf.HB_MODE_NEVER
            else int(pf.HB_MODE_DAMAGED)
        ),
    }


def apply_original_state(video_vc, game_vc, original):
    video_vc.view.vsync_idx = 0 if original["pf.video.vsync"] else 1
    game_vc.view.hb_idx = 0 if original["pf.game.healthbar_mode"] != pf.HB_MODE_NEVER else 1
    pf.global_event(common.constants.EVENT_SETTINGS_APPLY, None)


def on_update(user, event):
    del user
    del event

    STATE["ticks"] += 1
    settings_vc = demo_main.demo_vc._DemoVC__settings_vc
    video_vc, game_vc = settings_vc._TabBarVC__children

    if STATE["phase"] == "verify" and STATE["ticks"] >= 30:
        curr = current_state()
        expected = target_state()
        if curr != expected:
            fail("SETTINGS_RELAUNCH_MISMATCH expected={0} actual={1}".format(expected, curr))
        original = load_original_state()
        video_vc.view.vsync_idx = 0 if original["pf.video.vsync"] else 1
        pf.global_event(common.constants.EVENT_SETTINGS_APPLY, None)
        STATE["phase"] = "switch_to_game"
        STATE["ticks"] = 0
        return

    if STATE["phase"] == "switch_to_game" and STATE["ticks"] >= 30:
        original = load_original_state()
        curr = current_state()
        if curr["pf.video.vsync"] != original["pf.video.vsync"]:
            fail("VIDEO_SETTING_RESTORE_MISMATCH expected={0} actual={1}".format(original, curr))
        pf.global_event(common.constants.EVENT_SETTINGS_TAB_SEL_CHANGED, 1)
        STATE["phase"] = "restore_game"
        STATE["ticks"] = 0
        return

    if STATE["phase"] == "restore_game" and STATE["ticks"] >= 30:
        original = load_original_state()
        game_vc.view.hb_idx = 0 if original["pf.game.healthbar_mode"] != pf.HB_MODE_NEVER else 1
        pf.global_event(common.constants.EVENT_SETTINGS_APPLY, None)
        STATE["phase"] = "restore_verify"
        STATE["ticks"] = 0
        return

    if STATE["phase"] == "restore_verify" and STATE["ticks"] >= 30:
        curr = current_state()
        original = load_original_state()
        expected = ui_restored_state(original)
        if curr != expected:
            fail("SETTINGS_RESTORE_MISMATCH expected={0} actual={1}".format(expected, curr))
        pf.settings_set("pf.video.vsync", original["pf.video.vsync"])
        pf.settings_set("pf.game.healthbar_mode", original["pf.game.healthbar_mode"])
        exact = current_state()
        if exact != original:
            fail("SETTINGS_EXACT_RESTORE_MISMATCH expected={0} actual={1}".format(original, exact))
        finish("NATIVE_SETTINGS_RESTORED {0}".format(exact))
        return

    if STATE["ticks"] >= 300:
        fail("Timed out waiting for native settings verify/restore probe to finish")


demo_main.main()
demo_main.demo_vc._DemoVC__settings_vc.activate()
pf.register_ui_event_handler(pf.EVENT_UPDATE_START, on_update, None)
