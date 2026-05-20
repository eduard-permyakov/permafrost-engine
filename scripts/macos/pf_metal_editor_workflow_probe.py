import os
import sys

import pf

sys.path.insert(0, pf.get_basedir() + "/scripts")
sys.path.insert(0, pf.get_basedir() + "/scripts/editor")

os.environ.setdefault("PF_EDITOR_WORKFLOW_PROBE", "1")
os.environ.setdefault("PF_EDITOR_WORKFLOW_PROBE_AUTOQUIT", "1")
os.environ.setdefault("PF_EDITOR_WORKFLOW_PROBE_PATH", "/tmp/pf_metal_editor_workflow_probe.txt")
os.environ.setdefault("PF_EDITOR_WORKFLOW_PROBE_TRACE_PATH", "/tmp/pf_metal_editor_workflow_probe_trace.txt")
os.environ.setdefault("PF_EDITOR_WORKFLOW_PROBE_QUIT_AFTER", "45")

import editor.main  # noqa: F401
