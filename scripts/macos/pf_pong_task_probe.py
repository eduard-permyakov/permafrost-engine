import json
import os
import sys

import pf

sys.path.insert(0, pf.get_basedir() + "/scripts")


PROBE_PATH = "/tmp/pf_pong_task_probe.txt"
ERROR_PATH = "/tmp/pf_pong_task_probe_error.txt"
DEFAULT_OUTPUT_DIR = "visual_parity_captures/pong-task-probe"

STATE = {
    "ticks": 0,
    "output_dir": None,
    "expected_backend": "METAL",
    "ball_start": None,
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
    return os.path.join(STATE["output_dir"], "summary_pong_task.json")


def _write_summary(status, reason=None):
    import pong

    payload = {
        "status": status,
        "reason": reason,
        "backend": pf.get_render_info(),
        "ticks": STATE["ticks"],
        "ball_start": STATE["ball_start"],
        "ball_end": tuple(pong.ball.pos),
        "player_score": pong.player_score,
        "computer_score": pong.computer_score,
    }
    with open(_summary_path(), "w") as outfile:
        json.dump(payload, outfile, indent=2, sort_keys=True)
        outfile.write("\n")


def _fail(reason):
    _write_summary("fail", reason)
    _write(ERROR_PATH, str(reason))
    print("PONG_TASK_PROBE_FAIL {0}".format(reason))
    sys.stdout.flush()
    os._exit(1)


def _succeed():
    _write_summary("pass")
    marker = "PONG_TASK_PROBE_PASS backend={0}".format(pf.get_render_info().get("backend"))
    _write(PROBE_PATH, marker)
    print(marker)
    print("PONG_TASK_PROBE_SUMMARY {0}".format(_summary_path()))
    sys.stdout.flush()
    os._exit(0)


def on_update(user, event):
    del user
    del event

    import pong

    STATE["ticks"] += 1
    if STATE["ticks"] == 1:
        backend = pf.get_render_info().get("backend")
        if STATE["expected_backend"] and backend != STATE["expected_backend"]:
            _fail("expected {0} backend, got {1}".format(STATE["expected_backend"], backend))
        STATE["ball_start"] = tuple(pong.ball.pos)
        return

    if STATE["ticks"] < 90:
        return

    if tuple(pong.ball.pos) == tuple(STATE["ball_start"]):
        _fail("ball did not move; 30Hz task actor did not advance")
    _succeed()


def main():
    output_dir = _arg_value("--output-dir", os.environ.get("PF_PONG_TASK_OUTPUT_DIR", DEFAULT_OUTPUT_DIR))
    if not os.path.isabs(output_dir):
        output_dir = os.path.join(pf.get_basedir(), output_dir)
    if not os.path.isdir(output_dir):
        os.makedirs(output_dir)
    STATE["output_dir"] = output_dir
    STATE["expected_backend"] = _arg_value("--expect-backend", os.environ.get("PF_PONG_TASK_EXPECT_BACKEND", "METAL"))

    for path in (PROBE_PATH, ERROR_PATH):
        try:
            os.unlink(path)
        except OSError:
            pass

    import pong
    del pong

    pf.register_event_handler(pf.EVENT_UPDATE_START, on_update, None)


main()
