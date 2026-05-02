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

import pf
from constants import *
import map
import globals
import mouse_events
import scene

from math import cos, pi
import os
import subprocess
import sys
import time

import view_controllers.terrain_tab_vc as ttvc
import view_controllers.objects_tab_vc as otvc
import view_controllers.diplomacy_tab_vc as dtvc
import view_controllers.menu_vc as mvc
import common.view_controllers.tab_bar_vc as tbvc
import common.constants as common_constants

import views.tab_bar_window as tbw
import views.terrain_tab_window as ttw
import views.objects_tab_window as otw
import views.diplomacy_tab_window as dtw
import views.menu_window as mw

editor_probe_ticks = 0
editor_feature_probe_steps_done = set()
editor_workflow_probe_steps_done = set()
editor_workflow_probe_state = {}
editor_visual_probe_steps_done = set()
editor_visual_probe_state = {"captures": []}


def _write_probe_file(path, marker):
    with open(path, "w") as probe_file:
        probe_file.write(marker + "\n")


def _append_probe_trace(path, marker):
    if not path:
        return
    with open(path, "a") as probe_file:
        probe_file.write(marker + "\n")


def _editor_probe_marker():
    render_info = pf.get_render_info()
    return "EDITOR_LAUNCH_READY backend={0} renderer={1}".format(
        render_info.get("backend"),
        render_info.get("renderer"),
    )


def _editor_feature_probe_marker():
    render_info = pf.get_render_info()
    return "EDITOR_FEATURE_AUDIT_READY backend={0} renderer={1} factions={2}".format(
        render_info.get("backend"),
        render_info.get("renderer"),
        len(pf.get_factions_list()),
    )


def _editor_workflow_probe_marker():
    render_info = pf.get_render_info()
    return (
        "EDITOR_WORKFLOW_READY backend={0} renderer={1} saved_map={2} "
        "saved_scene={3} placed_objects={4} saved_objects={5}"
    ).format(
        render_info.get("backend"),
        render_info.get("renderer"),
        editor_workflow_probe_state.get("map_path"),
        editor_workflow_probe_state.get("scene_path"),
        editor_workflow_probe_state.get("placed_objects", 0),
        editor_workflow_probe_state.get("saved_objects", 0),
    )


def _editor_workflow_reload_probe_marker():
    render_info = pf.get_render_info()
    return "EDITOR_WORKFLOW_RELOAD_READY backend={0} renderer={1} loaded_objects={2}".format(
        render_info.get("backend"),
        render_info.get("renderer"),
        len(globals.active_objects_list),
    )


def _editor_visual_probe_marker():
    render_info = pf.get_render_info()
    return (
        "EDITOR_VISUAL_READY backend={0} renderer={1} captures={2} "
        "placed_objects={3} saved_objects={4}"
    ).format(
        render_info.get("backend"),
        render_info.get("renderer"),
        len(editor_visual_probe_state.get("captures", [])),
        editor_visual_probe_state.get("placed_objects", 0),
        editor_visual_probe_state.get("saved_objects", 0),
    )


def _editor_select_tab(index):
    tab_bar_vc.view.active_idx = index
    tab_bar_vc.view._TabBarWindow__show_active()
    pf.global_event(EVENT_TOP_TAB_SELECTION_CHANGED, index)


def on_editor_probe_update(user, event):
    del user
    del event

    global editor_probe_ticks
    editor_probe_ticks += 1

    quit_after = int(os.environ.get("PF_EDITOR_LAUNCH_PROBE_QUIT_AFTER", "8"))
    if editor_probe_ticks < quit_after:
        return

    marker = _editor_probe_marker()
    print(marker)
    probe_path = os.environ.get("PF_EDITOR_LAUNCH_PROBE_PATH")
    if probe_path:
        _write_probe_file(probe_path, marker)
    if os.environ.get("PF_EDITOR_LAUNCH_PROBE_AUTOQUIT") == "1":
        sys.stdout.flush()
        os._exit(0)


def _run_editor_feature_probe_step(name):
    marker = "EDITOR_FEATURE_STEP tick={0} name={1}".format(editor_probe_ticks, name)
    print(marker)
    sys.stdout.flush()
    _append_probe_trace(os.environ.get("PF_EDITOR_FEATURE_PROBE_TRACE_PATH"), marker)

    if name == "terrain_tab":
        pf.global_event(EVENT_TOP_TAB_SELECTION_CHANGED, 0)
    elif name == "objects_tab":
        pf.global_event(EVENT_TOP_TAB_SELECTION_CHANGED, 1)
    elif name == "objects_select_mode":
        objects_tab_vc.view.mode = objects_tab_vc.view.OBJECTS_MODE_SELECT
        pf.global_event(EVENT_OBJECTS_TAB_MODE_CHANGED, objects_tab_vc.view.mode)
    elif name == "objects_place_mode":
        objects_tab_vc.view.mode = objects_tab_vc.view.OBJECTS_MODE_PLACE
        pf.global_event(EVENT_OBJECTS_TAB_MODE_CHANGED, objects_tab_vc.view.mode)
    elif name == "diplomacy_tab":
        pf.global_event(EVENT_TOP_TAB_SELECTION_CHANGED, 2)
    elif name == "diplomacy_add_probe_faction":
        pf.global_event(EVENT_DIPLO_FAC_NEW, ("Editor Probe", (128, 64, 255, 255)))
    elif name == "terrain_large_brush":
        pf.global_event(EVENT_TOP_TAB_SELECTION_CHANGED, 0)
        terrain_tab_vc.view.brush_size_idx = 1
        pf.global_event(EVENT_TERRAIN_BRUSH_SIZE_CHANGED, terrain_tab_vc.view.brush_size_idx)
    elif name == "menu_show":
        menu.show()
    elif name == "settings_show":
        pf.global_event(EVENT_MENU_SETTINGS_SHOW, None)
    elif name == "settings_game_tab":
        pf.global_event(common_constants.EVENT_SETTINGS_TAB_SEL_CHANGED, 1)
    elif name == "settings_hide":
        pf.global_event(common_constants.EVENT_SETTINGS_HIDE, None)
    elif name == "perf_show":
        pf.global_event(EVENT_MENU_PERF_SHOW, None)
    elif name == "session_show":
        pf.global_event(EVENT_MENU_SESSION_SHOW, None)
    elif name == "load_cancel":
        pf.global_event(EVENT_MENU_LOAD, None)
        pf.global_event(EVENT_FILE_CHOOSER_CANCEL, None)
    elif name == "save_as_cancel":
        pf.global_event(EVENT_MENU_SAVE_AS, None)
        pf.global_event(EVENT_FILE_CHOOSER_CANCEL, None)

    marker = "EDITOR_FEATURE_STEP_DONE tick={0} name={1}".format(editor_probe_ticks, name)
    print(marker)
    sys.stdout.flush()
    _append_probe_trace(os.environ.get("PF_EDITOR_FEATURE_PROBE_TRACE_PATH"), marker)


def on_editor_feature_probe_update(user, event):
    del user
    del event

    global editor_probe_ticks
    editor_probe_ticks += 1

    steps = (
        (4, "terrain_tab"),
        (10, "objects_tab"),
        (16, "objects_select_mode"),
        (22, "objects_place_mode"),
        (28, "diplomacy_tab"),
        (34, "diplomacy_add_probe_faction"),
        (40, "terrain_large_brush"),
        (46, "menu_show"),
        (52, "settings_show"),
        (58, "settings_game_tab"),
        (64, "settings_hide"),
        (70, "perf_show"),
        (76, "session_show"),
        (82, "load_cancel"),
        (88, "save_as_cancel"),
    )

    for tick, name in steps:
        if editor_probe_ticks >= tick and name not in editor_feature_probe_steps_done:
            _run_editor_feature_probe_step(name)
            editor_feature_probe_steps_done.add(name)

    quit_after = int(os.environ.get("PF_EDITOR_FEATURE_PROBE_QUIT_AFTER", "110"))
    if editor_probe_ticks < quit_after:
        return

    marker = _editor_feature_probe_marker()
    print(marker)
    probe_path = os.environ.get("PF_EDITOR_FEATURE_PROBE_PATH")
    if probe_path:
        _write_probe_file(probe_path, marker)
    if os.environ.get("PF_EDITOR_FEATURE_PROBE_AUTOQUIT") == "1":
        sys.stdout.flush()
        os._exit(0)


def _editor_workflow_probe_output_dir():
    output_dir = os.environ.get("PF_EDITOR_WORKFLOW_PROBE_OUTPUT_DIR")
    if not output_dir:
        output_dir = "visual_parity_captures/2026-04-30-editor-workflow"
    if not os.path.isdir(output_dir):
        os.makedirs(output_dir)
    return output_dir


def _editor_workflow_probe_world_pos(chunk_coords, tile_coords):
    global_r = chunk_coords[0] * pf.TILES_PER_CHUNK_HEIGHT + tile_coords[0]
    global_c = chunk_coords[1] * pf.TILES_PER_CHUNK_WIDTH + tile_coords[1]
    x = -(global_c + 0.5) * pf.X_COORDS_PER_TILE
    z = (global_r + 0.5) * pf.Z_COORDS_PER_TILE
    y = pf.map_height_at_point(x, z)
    if y is None:
        y = 0.0
    return (x, y, z)


def _editor_workflow_probe_object_index(animated):
    for idx, meta in enumerate(scene.OBJECTS_LIST):
        if bool(meta["anim"]) == bool(animated):
            return idx
    raise RuntimeError("Editor workflow probe could not find an object with anim={0}".format(animated))


def _editor_workflow_probe_make_object(index, pos):
    obj = objects_tab_vc._ObjectsVC__object_at_index(index)
    obj.pos = pos
    obj.faction_id = pf.get_factions_list()[0]["id"]
    obj.selectable = True
    globals.active_objects_list.append(obj)
    return obj


def _editor_workflow_probe_scene_entity_count(path):
    with open(path, "r") as scene_file:
        for line in scene_file:
            parts = line.split()
            if len(parts) == 2 and parts[0] == "num_entities":
                return int(parts[1])
    raise RuntimeError("Editor workflow probe could not find num_entities in saved scene")


def _editor_visual_probe_output_dir():
    output_dir = os.environ.get("PF_EDITOR_VISUAL_PROBE_OUTPUT_DIR")
    if not output_dir:
        output_dir = "visual_parity_captures/2026-05-01-editor-visual-harness"
    if not os.path.isdir(output_dir):
        os.makedirs(output_dir)
    return output_dir


def _editor_visual_probe_capture_window_id():
    helper = os.path.join(pf.get_basedir(), "scripts/macos/pf_window_id_for_pid.swift")
    try:
        ret = subprocess.run(
            ["/usr/bin/swift", helper, str(os.getpid())],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            universal_newlines=True,
            timeout=3.0,
        )
    except subprocess.TimeoutExpired:
        return None
    if ret.returncode != 0:
        return None
    window_id = ret.stdout.strip().splitlines()[-1]
    return window_id if window_id.isdigit() else None


def _editor_visual_probe_activate_window():
    script = (
        'tell application "System Events"\n'
        '    set capture_process to first process whose unix id is {0}\n'
        '    set frontmost of capture_process to true\n'
        '    if (count of windows of capture_process) is greater than 0 then\n'
        '        perform action "AXRaise" of window 1 of capture_process\n'
        '    end if\n'
        'end tell\n'
    ).format(os.getpid())
    try:
        subprocess.run(["osascript", "-e", script], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=3.0)
    except subprocess.TimeoutExpired:
        pass


def _editor_visual_probe_capture(name):
    output_dir = _editor_visual_probe_output_dir()
    path = os.path.join(output_dir, "editor_{0}.png".format(name))
    ret = 1
    last_error = ""
    window_id = None
    for _ in range(5):
        _editor_visual_probe_activate_window()
        window_id = _editor_visual_probe_capture_window_id()
        if window_id is None:
            last_error = "no window id"
            time.sleep(0.15)
            continue
        try:
            capture = subprocess.run(
                ["screencapture", "-x", "-o", "-l{0}".format(window_id), path],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                universal_newlines=True,
                timeout=3.0,
            )
            ret = capture.returncode
            last_error = capture.stderr.strip()
            if ret == 0:
                break
        except subprocess.TimeoutExpired:
            last_error = "timeout"
            ret = 1
        time.sleep(0.15)
    if ret != 0:
        window_error = last_error
        for _ in range(3):
            _editor_visual_probe_activate_window()
            try:
                capture = subprocess.run(
                    ["screencapture", "-x", path],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    universal_newlines=True,
                    timeout=3.0,
                )
                ret = capture.returncode
                last_error = capture.stderr.strip()
                if ret == 0:
                    print("EDITOR_VISUAL_CAPTURE_FALLBACK window_id={0} stderr={1}".format(window_id, window_error))
                    sys.stdout.flush()
                    break
            except subprocess.TimeoutExpired:
                last_error = "timeout"
                ret = 1
            time.sleep(0.15)
    if ret != 0:
        print("EDITOR_VISUAL_CAPTURE_FAIL window_id={0} stderr={1}".format(window_id, last_error))
        sys.stdout.flush()
        raise RuntimeError("Editor visual probe could not capture {0}".format(name))
    editor_visual_probe_state["captures"].append(path)
    return path


def _run_editor_visual_probe_step(name):
    marker = "EDITOR_VISUAL_STEP tick={0} name={1}".format(editor_probe_ticks, name)
    print(marker)
    sys.stdout.flush()
    _append_probe_trace(os.environ.get("PF_EDITOR_VISUAL_PROBE_TRACE_PATH"), marker)

    if name == "setup_world":
        _editor_select_tab(0)
        terrain_tab_vc.view.brush_size_idx = 1
        terrain_tab_vc.view.selected_mat_idx = 3
        terrain_tab_vc.view.brush_type_idx = 0
        terrain_tab_vc.view.blend_textures = False
        terrain_tab_vc.view.blend_normals = False

        for row in range(2, 7):
            for col in range(2, 7):
                globals.active_map.update_tile_mat(
                    ((1, 1), (row, col)),
                    globals.active_map.materials[3],
                    pf.BLEND_MODE_NOBLEND,
                    0,
                )
        globals.active_map.update_tile(
            ((1, 1), (7, 4)),
            -1,
            pf.TILETYPE_FLAT,
            globals.active_map.materials[1],
            0,
            pf.BLEND_MODE_NOBLEND,
            0,
        )

        animated_idx = _editor_workflow_probe_object_index(True)
        static_idx = _editor_workflow_probe_object_index(False)
        animated = _editor_workflow_probe_make_object(
            animated_idx,
            _editor_workflow_probe_world_pos((1, 1), (3, 4)),
        )
        _editor_workflow_probe_make_object(
            static_idx,
            _editor_workflow_probe_world_pos((1, 1), (5, 4)),
        )
        animated.select()
        editor_visual_probe_state["placed_objects"] = len(globals.active_objects_list)
        editor_visual_probe_state["target"] = _editor_workflow_probe_world_pos((1, 1), (4, 4))
        pf.get_active_camera().center_over_location((
            editor_visual_probe_state["target"][0],
            editor_visual_probe_state["target"][2],
        ))
    elif name == "select_terrain":
        _editor_select_tab(0)
    elif name == "capture_terrain":
        _editor_visual_probe_capture("terrain")
    elif name == "select_objects":
        _editor_select_tab(1)
        objects_tab_vc.view.mode = objects_tab_vc.view.OBJECTS_MODE_SELECT
        pf.global_event(EVENT_OBJECTS_TAB_MODE_CHANGED, objects_tab_vc.view.mode)
    elif name == "capture_objects":
        _editor_visual_probe_capture("objects")
    elif name == "save_as_show":
        pf.global_event(EVENT_MENU_SAVE_AS, None)
    elif name == "save_as_confirm":
        output_dir = _editor_visual_probe_output_dir()
        map_path = os.path.join(output_dir, "editor_visual_probe.pfmap")
        scene_path = os.path.join(output_dir, "editor_visual_probe.pfscene")
        editor_visual_probe_state["map_path"] = map_path
        editor_visual_probe_state["scene_path"] = scene_path
        pf.global_event(EVENT_FILE_CHOOSER_OKAY, (map_path, scene_path))
    elif name == "validate_saved_files":
        map_path = editor_visual_probe_state["map_path"]
        scene_path = editor_visual_probe_state["scene_path"]
        saved_map = map.Map.from_filepath(map_path)
        if saved_map is None:
            raise RuntimeError("Editor visual probe failed to parse saved map")
        tile = saved_map.tile_at_coords((1, 1), (4, 4))
        if tile.top_mat_idx != 3:
            raise RuntimeError("Editor visual probe saved wrong visible terrain material")
        saved_objects = _editor_workflow_probe_scene_entity_count(scene_path)
        if saved_objects < editor_visual_probe_state["placed_objects"]:
            raise RuntimeError("Editor visual probe saved too few objects")
        editor_visual_probe_state["saved_objects"] = saved_objects

    marker = "EDITOR_VISUAL_STEP_DONE tick={0} name={1}".format(editor_probe_ticks, name)
    print(marker)
    sys.stdout.flush()
    _append_probe_trace(os.environ.get("PF_EDITOR_VISUAL_PROBE_TRACE_PATH"), marker)


def on_editor_visual_probe_update(user, event):
    del user
    del event

    global editor_probe_ticks
    editor_probe_ticks += 1

    steps = (
        (6, "setup_world"),
        (14, "select_terrain"),
        (20, "capture_terrain"),
        (28, "select_objects"),
        (34, "capture_objects"),
        (42, "save_as_show"),
        (48, "save_as_confirm"),
        (62, "validate_saved_files"),
    )

    for tick, name in steps:
        if editor_probe_ticks >= tick and name not in editor_visual_probe_steps_done:
            _run_editor_visual_probe_step(name)
            editor_visual_probe_steps_done.add(name)

    quit_after = int(os.environ.get("PF_EDITOR_VISUAL_PROBE_QUIT_AFTER", "78"))
    if editor_probe_ticks < quit_after:
        return

    marker = _editor_visual_probe_marker()
    print(marker)
    probe_path = os.environ.get("PF_EDITOR_VISUAL_PROBE_PATH")
    if probe_path:
        _write_probe_file(probe_path, marker)
    if os.environ.get("PF_EDITOR_VISUAL_PROBE_AUTOQUIT") == "1":
        sys.stdout.flush()
        os._exit(0)


def _run_editor_workflow_probe_step(name):
    marker = "EDITOR_WORKFLOW_STEP tick={0} name={1}".format(editor_probe_ticks, name)
    print(marker)
    sys.stdout.flush()
    _append_probe_trace(os.environ.get("PF_EDITOR_WORKFLOW_PROBE_TRACE_PATH"), marker)

    if name == "mutate":
        tile_coords = ((1, 1), (2, 3))
        globals.active_map.update_tile_mat(
            tile_coords,
            globals.active_map.materials[3],
            pf.BLEND_MODE_NOBLEND,
            0,
        )
        globals.active_map.update_tile(
            ((1, 1), (3, 3)),
            2,
            pf.TILETYPE_FLAT,
            globals.active_map.materials[1],
            0,
            pf.BLEND_MODE_NOBLEND,
            0,
        )

        animated_idx = _editor_workflow_probe_object_index(True)
        static_idx = _editor_workflow_probe_object_index(False)
        _editor_workflow_probe_make_object(animated_idx, _editor_workflow_probe_world_pos((1, 1), (2, 5)))
        _editor_workflow_probe_make_object(static_idx, _editor_workflow_probe_world_pos((1, 1), (5, 5)))
        editor_workflow_probe_state["placed_objects"] = len(globals.active_objects_list)
        editor_workflow_probe_state["mutated_tile"] = tile_coords
    elif name == "save_as_show":
        pf.global_event(EVENT_MENU_SAVE_AS, None)
    elif name == "save_as_confirm":
        output_dir = _editor_workflow_probe_output_dir()
        map_path = os.path.join(output_dir, "editor_workflow_probe.pfmap")
        scene_path = os.path.join(output_dir, "editor_workflow_probe.pfscene")
        editor_workflow_probe_state["map_path"] = map_path
        editor_workflow_probe_state["scene_path"] = scene_path
        pf.global_event(EVENT_FILE_CHOOSER_OKAY, (map_path, scene_path))
    elif name == "validate_saved_files":
        map_path = editor_workflow_probe_state["map_path"]
        scene_path = editor_workflow_probe_state["scene_path"]
        saved_map = map.Map.from_filepath(map_path)
        if saved_map is None:
            raise RuntimeError("Editor workflow probe failed to parse saved map")
        tile = saved_map.tile_at_coords(*editor_workflow_probe_state["mutated_tile"])
        if tile.top_mat_idx != 3:
            raise RuntimeError("Editor workflow probe saved wrong terrain material index")
        saved_objects = _editor_workflow_probe_scene_entity_count(scene_path)
        if saved_objects < editor_workflow_probe_state["placed_objects"]:
            raise RuntimeError("Editor workflow probe saved too few objects")
        editor_workflow_probe_state["saved_objects"] = saved_objects

    marker = "EDITOR_WORKFLOW_STEP_DONE tick={0} name={1}".format(editor_probe_ticks, name)
    print(marker)
    sys.stdout.flush()
    _append_probe_trace(os.environ.get("PF_EDITOR_WORKFLOW_PROBE_TRACE_PATH"), marker)


def on_editor_workflow_probe_update(user, event):
    del user
    del event

    global editor_probe_ticks
    editor_probe_ticks += 1

    if os.environ.get("PF_EDITOR_WORKFLOW_PROBE_RELOAD_ONLY") == "1":
        expected = int(os.environ.get("PF_EDITOR_WORKFLOW_PROBE_EXPECT_OBJECTS", "0"))
        if expected and len(globals.active_objects_list) < expected:
            raise RuntimeError("Editor workflow reload probe loaded too few objects")

        quit_after = int(os.environ.get("PF_EDITOR_WORKFLOW_PROBE_QUIT_AFTER", "12"))
        if editor_probe_ticks < quit_after:
            return

        marker = _editor_workflow_reload_probe_marker()
        print(marker)
        probe_path = os.environ.get("PF_EDITOR_WORKFLOW_PROBE_PATH")
        if probe_path:
            _write_probe_file(probe_path, marker)
        if os.environ.get("PF_EDITOR_WORKFLOW_PROBE_AUTOQUIT") == "1":
            sys.stdout.flush()
            os._exit(0)
        return

    steps = (
        (6, "mutate"),
        (12, "save_as_show"),
        (18, "save_as_confirm"),
        (30, "validate_saved_files"),
    )

    for tick, name in steps:
        if editor_probe_ticks >= tick and name not in editor_workflow_probe_steps_done:
            _run_editor_workflow_probe_step(name)
            editor_workflow_probe_steps_done.add(name)

    quit_after = int(os.environ.get("PF_EDITOR_WORKFLOW_PROBE_QUIT_AFTER", "45"))
    if editor_probe_ticks < quit_after:
        return

    marker = _editor_workflow_probe_marker()
    print(marker)
    probe_path = os.environ.get("PF_EDITOR_WORKFLOW_PROBE_PATH")
    if probe_path:
        _write_probe_file(probe_path, marker)
    if os.environ.get("PF_EDITOR_WORKFLOW_PROBE_AUTOQUIT") == "1":
        sys.stdout.flush()
        os._exit(0)


def install_editor_probe():
    if os.environ.get("PF_EDITOR_VISUAL_PROBE") == "1":
        pf.register_event_handler(pf.EVENT_UPDATE_START, on_editor_visual_probe_update, None)
    elif os.environ.get("PF_EDITOR_WORKFLOW_PROBE") == "1":
        pf.register_event_handler(pf.EVENT_UPDATE_START, on_editor_workflow_probe_update, None)
    elif os.environ.get("PF_EDITOR_FEATURE_PROBE") == "1":
        pf.register_event_handler(pf.EVENT_UPDATE_START, on_editor_feature_probe_update, None)
    elif os.environ.get("PF_EDITOR_LAUNCH_PROBE") == "1":
        pf.register_event_handler(pf.EVENT_UPDATE_START, on_editor_probe_update, None)

############################################################
# Global settings                                          #
############################################################

pf.set_ambient_light_color((1.0, 1.0, 1.0))
pf.set_emit_light_color((1.0, 1.0, 1.0))
pf.set_emit_light_pos((1664.0, 1024.0, 384.0))

pf.set_active_font("OptimusPrinceps.ttf")
pf.disable_unit_selection()
pf.disable_fog_of_war()

mouse_events.install()

############################################################
# Setup Map, Scene                                         #
############################################################

if len(sys.argv) > 1:
    pf.load_map(pf.get_basedir(), sys.argv[1], update_navgrid=False)
    globals.active_map = map.Map.from_filepath(pf.get_basedir() + "/" + sys.argv[1])
else:
    pf.load_map_string(globals.active_map.pfmap_str(), update_navgrid=False)

if len(sys.argv) > 2:
    loaded_scene = pf.load_scene(sys.argv[2], update_navgrid=False)
    globals.active_objects_list = loaded_scene[0]
    globals.scene_filename = sys.argv[2]
    for obj in globals.active_objects_list:
        obj.selectable = True
else:
    pf.add_faction(DEFAULT_FACTION_NAME, DEFAULT_FACTION_COLOR)

############################################################
# Setup UI                                                 #
############################################################

minimap_pos = pf.get_minimap_position()
pf.set_minimap_position(UI_LEFT_PANE_WIDTH + minimap_pos[0], minimap_pos[1])

terrain_tab_vc = ttvc.TerrainTabVC(ttw.TerrainTabWindow())
objects_tab_vc = otvc.ObjectsVC(otw.ObjectsTabWindow())
diplo_tab_vc = dtvc.DiplomacyVC(dtw.DiplomacyTabWindow())

tab_bar_vc = tbvc.TabBarVC(tbw.TabBarWindow(), EVENT_TOP_TAB_SELECTION_CHANGED)
tab_bar_vc.push_child("Terrain", terrain_tab_vc)
tab_bar_vc.push_child("Objects", objects_tab_vc)
tab_bar_vc.push_child("Diplomacy", diplo_tab_vc)
tab_bar_vc.activate()
tab_bar_vc.view.show()

menu = mw.Menu()
menuvc = mvc.MenuVC(menu)
menuvc.activate()

mb = mw.MenuButtonWindow(menu)
mb.show()

install_editor_probe()
