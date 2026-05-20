import os
import sys

import pf

sys.path.insert(0, pf.get_basedir() + "/scripts")
sys.path.insert(0, pf.get_basedir() + "/scripts/editor")

os.environ.setdefault("PF_EDITOR_LAUNCH_PROBE", "1")
os.environ.setdefault("PF_EDITOR_LAUNCH_PROBE_AUTOQUIT", "1")
os.environ.setdefault("PF_EDITOR_LAUNCH_PROBE_PATH", "/tmp/pf_metal_editor_launch_probe.txt")
os.environ.setdefault("PF_EDITOR_LAUNCH_PROBE_QUIT_AFTER", "8")

import editor.main  # noqa: F401
