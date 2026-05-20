import json
import os
import sys

import pf

sys.path.insert(0, pf.get_basedir() + "/scripts")

import rts.main as demo_main


PROBE_PATH = "/tmp/pf_metal_audio_probe.txt"
ERROR_PATH = "/tmp/pf_metal_audio_probe_error.txt"
DEFAULT_OUTPUT_DIR = "visual_parity_captures/audio-probe"
MUSIC_NAME = "audio_probe_tone"
EFFECT_NAME = "audio_probe_beep"

STATE = {
    "ticks": 0,
    "output_dir": None,
    "expected_backend": "METAL",
    "result": {},
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
    return os.path.join(STATE["output_dir"], "summary_metal_audio.json")


def _write_summary(status, reason=None):
    payload = {
        "status": status,
        "reason": reason,
        "backend": pf.get_render_info(),
        "ticks": STATE["ticks"],
        "result": STATE["result"],
    }
    with open(_summary_path(), "w") as outfile:
        json.dump(payload, outfile, indent=2, sort_keys=True)
        outfile.write("\n")


def _fail(reason):
    _write_summary("fail", reason)
    _write(ERROR_PATH, str(reason))
    print("METAL_AUDIO_PROBE_FAIL {0}".format(reason))
    sys.stdout.flush()
    os._exit(1)


def _succeed():
    _write_summary("pass")
    marker = "METAL_AUDIO_PROBE_PASS backend={0} music={1} effect={2}".format(
        pf.get_render_info().get("backend"),
        MUSIC_NAME,
        EFFECT_NAME,
    )
    _write(PROBE_PATH, marker)
    print(marker)
    print("METAL_AUDIO_PROBE_SUMMARY {0}".format(_summary_path()))
    sys.stdout.flush()
    os._exit(0)


def _exercise_audio():
    tracks = pf.get_all_music()
    STATE["result"]["tracks"] = tracks
    if MUSIC_NAME not in tracks:
        _fail("missing generated music fixture {0}; tracks={1}".format(MUSIC_NAME, tracks))

    pf.play_music(MUSIC_NAME)
    curr = pf.curr_music()
    STATE["result"]["curr_music"] = curr
    if curr != MUSIC_NAME:
        _fail("current music mismatch expected={0} actual={1}".format(MUSIC_NAME, curr))

    pf.play_global_effect(EFFECT_NAME, True, 0)
    STATE["result"]["global_effect"] = EFFECT_NAME

    pf.play_effect(EFFECT_NAME, (0.0, 0.0, 0.0))
    STATE["result"]["positional_effect"] = EFFECT_NAME


def on_update(user, event):
    del user
    del event

    STATE["ticks"] += 1
    if STATE["ticks"] == 30:
        backend = pf.get_render_info().get("backend")
        if STATE["expected_backend"] and backend != STATE["expected_backend"]:
            _fail("expected {0} backend, got {1}".format(STATE["expected_backend"], backend))
        _exercise_audio()
        return

    if STATE["ticks"] == 90:
        pf.play_music(None)
        _succeed()

    if STATE["ticks"] > 180:
        _fail("timed out waiting for audio probe completion")


def main():
    output_dir = _arg_value("--output-dir", os.environ.get("PF_AUDIO_PROBE_OUTPUT_DIR", DEFAULT_OUTPUT_DIR))
    if not os.path.isabs(output_dir):
        output_dir = os.path.join(pf.get_basedir(), output_dir)
    if not os.path.isdir(output_dir):
        os.makedirs(output_dir)
    STATE["output_dir"] = output_dir
    STATE["expected_backend"] = _arg_value("--expect-backend", os.environ.get("PF_AUDIO_PROBE_EXPECT_BACKEND", "METAL"))

    for path in (PROBE_PATH, ERROR_PATH):
        try:
            os.unlink(path)
        except OSError:
            pass

    demo_main.main()
    pf.register_event_handler(pf.EVENT_UPDATE_START, on_update, None)


main()
