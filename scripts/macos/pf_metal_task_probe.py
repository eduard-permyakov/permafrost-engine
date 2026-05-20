import json
import os
import sys

import pf

sys.path.insert(0, pf.get_basedir() + "/scripts")

import rts.main as demo_main


PROBE_PATH = "/tmp/pf_metal_task_probe.txt"
ERROR_PATH = "/tmp/pf_metal_task_probe_error.txt"
DEFAULT_OUTPUT_DIR = "visual_parity_captures/task-probe"

STATE = {
    "ticks": 0,
    "output_dir": None,
    "expected_backend": "METAL",
    "steps": [],
    "task": None,
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
    return os.path.join(STATE["output_dir"], "summary_metal_task.json")


def _write_summary(status, reason=None):
    payload = {
        "status": status,
        "reason": reason,
        "backend": pf.get_render_info(),
        "steps": list(STATE["steps"]),
        "task_completed": bool(STATE["task"] and STATE["task"].completed),
        "ticks": STATE["ticks"],
    }
    with open(_summary_path(), "w") as outfile:
        json.dump(payload, outfile, indent=2, sort_keys=True)
        outfile.write("\n")


def _fail(reason):
    _write_summary("fail", reason)
    _write(ERROR_PATH, str(reason))
    print("METAL_TASK_PROBE_FAIL {0}".format(reason))
    sys.stdout.flush()
    os._exit(1)


def _succeed():
    _write_summary("pass")
    marker = "METAL_TASK_PROBE_PASS backend={0} steps={1}".format(
        pf.get_render_info().get("backend"),
        ",".join(STATE["steps"]),
    )
    _write(PROBE_PATH, marker)
    print(marker)
    print("METAL_TASK_PROBE_SUMMARY {0}".format(_summary_path()))
    sys.stdout.flush()
    os._exit(0)


class ProbeTask(pf.Task):
    def __run__(self):
        STATE["steps"].append("start")
        yield self.yield_()
        STATE["steps"].append("yield")
        yield self.sleep(20)
        STATE["steps"].append("sleep")
        yield self.await_event(pf.EVENT_UPDATE_START)
        STATE["steps"].append("event")
        return "done"


def on_update(user, event):
    del user
    del event
    STATE["ticks"] += 1

    if STATE["ticks"] == 1:
        backend = pf.get_render_info().get("backend")
        if STATE["expected_backend"] and backend != STATE["expected_backend"]:
            _fail("expected {0} backend, got {1}".format(STATE["expected_backend"], backend))
        task = ProbeTask()
        task.run()
        STATE["task"] = task
        return

    expected = ["start", "yield", "sleep", "event"]
    if STATE["task"] and STATE["task"].completed:
        if STATE["steps"] != expected:
            _fail("unexpected task steps expected={0} actual={1}".format(expected, STATE["steps"]))
        _succeed()

    if STATE["ticks"] > 180:
        _fail("timed out waiting for pf.Task probe completion steps={0}".format(STATE["steps"]))


def main():
    output_dir = _arg_value("--output-dir", os.environ.get("PF_TASK_PROBE_OUTPUT_DIR", DEFAULT_OUTPUT_DIR))
    if not os.path.isabs(output_dir):
        output_dir = os.path.join(pf.get_basedir(), output_dir)
    if not os.path.isdir(output_dir):
        os.makedirs(output_dir)
    STATE["output_dir"] = output_dir
    STATE["expected_backend"] = _arg_value("--expect-backend", os.environ.get("PF_TASK_PROBE_EXPECT_BACKEND", "METAL"))

    for path in (PROBE_PATH, ERROR_PATH):
        try:
            os.unlink(path)
        except OSError:
            pass

    demo_main.main()
    pf.register_ui_event_handler(pf.EVENT_UPDATE_START, on_update, None)


main()
