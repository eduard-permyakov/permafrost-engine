import os
import sys

import pf

sys.path.insert(0, pf.get_basedir() + "/scripts")
sys.path.insert(0, pf.get_basedir() + "/scripts/editor")

os.environ.setdefault("PF_EDITOR_FEATURE_PROBE", "1")
os.environ.setdefault("PF_EDITOR_FEATURE_PROBE_AUTOQUIT", "1")
os.environ.setdefault("PF_EDITOR_FEATURE_PROBE_PATH", "/tmp/pf_metal_editor_feature_probe.txt")
os.environ.setdefault("PF_EDITOR_FEATURE_PROBE_TRACE_PATH", "/tmp/pf_metal_editor_feature_probe_trace.txt")
os.environ.setdefault("PF_EDITOR_FEATURE_PROBE_QUIT_AFTER", "110")

import editor.main  # noqa: F401
