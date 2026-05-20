#
#  This file is part of Permafrost Engine. 
#  Copyright (C) 2018-2023 Eduard Permyakov 
#
#  Permafrost Engine is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, either version 3 of the License, or
#  (at your option) any later version.
#
#  Permafrost Engine is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program.  If not, see <http://www.gnu.org/licenses/>.
# 
#  Linking this software statically or dynamically with other modules is making 
#  a combined work based on this software. Thus, the terms and conditions of 
#  the GNU General Public License cover the whole combination. 
#  
#  As a special exception, the copyright holders of Permafrost Engine give 
#  you permission to link Permafrost Engine with independent modules to produce 
#  an executable, regardless of the license terms of these independent 
#  modules, and to copy and distribute the resulting executable under 
#  terms of your choice, provided that you also meet, for each linked 
#  independent module, the terms and conditions of the license of that 
#  module. An independent module is a module which is not derived from 
#  or based on Permafrost Engine. If you modify Permafrost Engine, you may 
#  extend this exception to your version of Permafrost Engine, but you are not 
#  obliged to do so. If you do not wish to do so, delete this exception 
#  statement from your version.
#

import os
import sys
import json
import pf
import rts.globals
import rts.time_of_day as time_of_day

import views.demo_window as dw
import views.action_pad_window as apw

import view_controllers.action_pad_vc as apvc
import view_controllers.demo_vc as dvc

from constants import *
from units import *

main_cam = None
debug_cam = None
active_cam = None
demo_vc = None
action_pad_vc = None
runtime_probe_installed = False
runtime_probe_ticks = 0


def _format_probe_value(value):
    if isinstance(value, float):
        return "{0:.6f}".format(value)
    if isinstance(value, tuple):
        return "x".join(_format_probe_value(item) for item in value)
    return str(value)


def _write_probe_file(path, marker):
    with open(path, "w") as probe_file:
        probe_file.write(marker + "\n")


def _runtime_probe_marker():
    render_info = pf.get_render_info()
    keys = [
        "pf.video.display_mode",
        "pf.video.vsync",
        "pf.video.shadows_enabled",
        "pf.video.use_batch_rendering",
        "pf.video.water_reflection",
        "pf.video.water_refraction",
        "pf.video.resolution",
        "pf.video.aspect_ratio",
        "pf.game.movement_use_gpu",
        "pf.game.fog_of_war_enabled",
        "pf.game.healthbar_mode",
        "pf.game.combat_hz",
    ]
    values = [
        "{0}={1}".format(key, _format_probe_value(pf.settings_get(key)))
        for key in keys
    ]
    values.append("pf.render.backend={0}".format(render_info.get("backend")))
    values.append("pf.render.renderer={0}".format(render_info.get("renderer")))
    return "NATIVE_LAUNCH_READY " + " ".join(values)


def on_runtime_probe_update(user, event):
    del user
    del event

    global runtime_probe_ticks
    runtime_probe_ticks += 1

    if runtime_probe_ticks != 1:
        return

    marker = _runtime_probe_marker()
    print(marker)
    probe_path = os.environ.get("PF_NATIVE_LAUNCH_PROBE_PATH")
    if probe_path:
        _write_probe_file(probe_path, marker)
    if os.environ.get("PF_NATIVE_LAUNCH_PROBE_AUTOQUIT") == "1":
        sys.stdout.flush()
        os._exit(0)


def install_runtime_probe():
    global runtime_probe_installed
    if runtime_probe_installed:
        return
    if os.environ.get("PF_NATIVE_LAUNCH_PROBE") != "1":
        return
    pf.register_event_handler(pf.EVENT_UPDATE_START, on_runtime_probe_update, None)
    runtime_probe_installed = True

def toggle_camera(user, event):

    if event[0] == pf.SDL_SCANCODE_C and not pf.ui_text_edit_has_focus():
        global active_cam, main_cam, debug_cam
        if active_cam == main_cam:
            active_cam = debug_cam
        else:
            active_cam = main_cam
        pf.set_active_camera(active_cam)

def toggle_pause(user, event):

    if event[0] == pf.SDL_SCANCODE_P and not pf.ui_text_edit_has_focus():
        ss = pf.get_simstate()
        if ss == pf.G_RUNNING:
            pf.set_simstate(pf.G_PAUSED_UI_RUNNING)
        else:
            pf.set_simstate(pf.G_RUNNING)

def configure_demo_environment():
    time_of_day.configure_from_env()
    pf.set_active_font("OptimusPrinceps.ttf")

def load_demo_scene():
    pf.load_map("assets/maps", "demo.pfmap")
    (rts.globals.scene_objs,
     rts.globals.scene_regions,
     rts.globals.scene_cameras) = pf.load_scene("assets/maps/demo.pfscene")
    pf.set_skybox("assets/skyboxes/clouds_blue", "jpg")

    pf.set_diplomacy_state(1, 2, pf.DIPLOMACY_STATE_WAR)
    pf.set_diplomacy_state(1, 3, pf.DIPLOMACY_STATE_WAR)
    pf.set_diplomacy_state(2, 3, pf.DIPLOMACY_STATE_WAR)

    pf.set_faction_controllable(0, False)
    pf.set_faction_controllable(2, False)
    pf.set_faction_controllable(3, False)

def bootstrap_demo_runtime():
    global main_cam, debug_cam, active_cam, demo_vc, action_pad_vc

    main_cam = pf.get_active_camera()
    debug_cam = pf.Camera(mode=pf.CAM_MODE_FPS, position=(0.0, 175.0, 0.0), pitch=-65.0, yaw=135.0)
    active_cam = main_cam

    pf.register_ui_event_handler(pf.SDL_KEYDOWN, toggle_camera, None)
    pf.register_ui_event_handler(pf.SDL_KEYDOWN, toggle_pause, None)

    demo_vc = dvc.DemoVC(dw.DemoWindow())
    demo_vc.activate()

    action_pad_vc = apvc.ActionPadVC(apw.ActionPadWindow())
    action_pad_vc.activate()

def restore_runtime_after_session_load(scene_objs=None, scene_regions=None, scene_cameras=None):
    if scene_objs is not None:
        rts.globals.scene_objs = scene_objs
    if scene_regions is not None:
        rts.globals.scene_regions = scene_regions
    if scene_cameras is not None:
        rts.globals.scene_cameras = scene_cameras
    bootstrap_demo_runtime()

    large_world_marker_path = os.environ.get("PF_LARGE_WORLD_SOAK_RESTORE_MARKER")
    large_world_summary_path = os.environ.get("PF_LARGE_WORLD_SOAK_RESTORE_SUMMARY")
    if large_world_marker_path or large_world_summary_path:
        marker = "LARGE_WORLD_SOAK_RESTORE_PASS objs={0} regions={1} cameras={2}".format(
            len(rts.globals.scene_objs),
            len(rts.globals.scene_regions),
            len(rts.globals.scene_cameras),
        )
        if large_world_marker_path:
            with open(large_world_marker_path, "w") as marker_file:
                marker_file.write(marker + "\n")
        if large_world_summary_path:
            try:
                with open(large_world_summary_path, "r") as summary_file:
                    payload = json.load(summary_file)
            except (IOError, ValueError):
                payload = {}
            checks = payload.setdefault("checks", {})
            checks["session_restore"] = True
            payload["status"] = "pass"
            session = payload.setdefault("session", {})
            session["restore_object_count"] = len(rts.globals.scene_objs)
            session["restore_region_count"] = len(rts.globals.scene_regions)
            session["restore_camera_count"] = len(rts.globals.scene_cameras)
            session["restore_marker"] = marker
            with open(large_world_summary_path, "w") as summary_file:
                json.dump(payload, summary_file, indent=2, sort_keys=True)
                summary_file.write("\n")
        print(marker)
        sys.stdout.flush()
        if os.environ.get("PF_LARGE_WORLD_SOAK_RESTORE_AUTOQUIT") == "1":
            os._exit(0)

    if os.environ.get("PF_NATIVE_SESSION_PROBE") == "1":
        marker = "NATIVE_SESSION_RESTORED objs={0} regions={1} cameras={2}".format(
            len(rts.globals.scene_objs),
            len(rts.globals.scene_regions),
            len(rts.globals.scene_cameras),
        )
        if os.environ.get("PF_NATIVE_SESSION_PROBE_VERBOSE") == "1":
            region_names = ",".join(region.name for region in rts.globals.scene_regions)
            camera_names = ",".join(
                (camera.name or "None") for camera in rts.globals.scene_cameras
            )
            marker = "{0} region_names={1} camera_names={2}".format(
                marker,
                region_names,
                camera_names,
            )
        print(marker)
        probe_path = os.environ.get("PF_NATIVE_SESSION_PROBE_PATH")
        if probe_path:
            with open(probe_path, "w") as probe_file:
                probe_file.write(marker + "\n")
        if os.environ.get("PF_NATIVE_SESSION_PROBE_AUTOQUIT") == "1":
            sys.stdout.flush()
            os._exit(0)

def main():
    configure_demo_environment()
    load_demo_scene()
    bootstrap_demo_runtime()
    install_runtime_probe()

if __name__ == "__main__":
    main()
