import sys
import traceback

try:
    import pf

    sys.path.insert(0, pf.get_basedir() + "/scripts")
    sys.path.insert(0, pf.get_basedir() + "/scripts/editor")

    import editor.main  # noqa: F401
except BaseException:
    traceback.print_exc()
    raise
