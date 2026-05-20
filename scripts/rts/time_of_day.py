import os
import time

import pf


KEYFRAMES = {
    "baseline": {
        "ambient": (1.0, 1.0, 1.0),
        "emit": (1.0, 1.0, 1.0),
        "light_pos": (1664.0, 1024.0, 384.0),
    },
    "morning": {
        "ambient": (0.82, 0.78, 0.68),
        "emit": (1.0, 0.78, 0.52),
        "light_pos": (-1200.0, 720.0, 1050.0),
    },
    "afternoon": {
        "ambient": (1.0, 0.98, 0.92),
        "emit": (1.0, 0.96, 0.88),
        "light_pos": (1664.0, 1220.0, 384.0),
    },
    "evening": {
        "ambient": (0.68, 0.56, 0.50),
        "emit": (1.0, 0.58, 0.36),
        "light_pos": (1050.0, 680.0, -1250.0),
    },
    "night": {
        "ambient": (0.22, 0.26, 0.38),
        "emit": (0.34, 0.42, 0.66),
        "light_pos": (-320.0, 520.0, -900.0),
    },
}

DYNAMIC_SEQUENCE = ("morning", "afternoon", "evening", "night")

_installed_dynamic = False
_dynamic_start_time = None
_dynamic_day_length = 240.0
_current_state = {
    "phase": "baseline",
    "dynamic": False,
    "day_length_sec": None,
    "ambient_light_color": KEYFRAMES["baseline"]["ambient"],
    "emit_light_color": KEYFRAMES["baseline"]["emit"],
    "emit_light_pos": KEYFRAMES["baseline"]["light_pos"],
}


def _clamp01(value):
    return max(0.0, min(1.0, value))


def _lerp(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def _state_for_phase(phase):
    if phase not in KEYFRAMES:
        phase = "baseline"
    data = KEYFRAMES[phase]
    return {
        "phase": phase,
        "dynamic": False,
        "day_length_sec": None,
        "ambient_light_color": data["ambient"],
        "emit_light_color": data["emit"],
        "emit_light_pos": data["light_pos"],
    }


def _apply_state(state):
    global _current_state
    pf.set_ambient_light_color(state["ambient_light_color"])
    pf.set_emit_light_color(state["emit_light_color"])
    pf.set_emit_light_pos(state["emit_light_pos"])
    _current_state = dict(state)


def apply_phase(phase):
    _apply_state(_state_for_phase(phase))


def _float_env(name, default):
    value = os.environ.get(name)
    if not value:
        return default
    try:
        parsed = float(value)
    except ValueError:
        return default
    return max(parsed, 1.0)


def _dynamic_state(now):
    elapsed = (now - _dynamic_start_time) % _dynamic_day_length
    cycle_t = elapsed / _dynamic_day_length
    scaled = cycle_t * len(DYNAMIC_SEQUENCE)
    idx = int(scaled) % len(DYNAMIC_SEQUENCE)
    next_idx = (idx + 1) % len(DYNAMIC_SEQUENCE)
    local_t = _clamp01(scaled - int(scaled))

    phase = DYNAMIC_SEQUENCE[idx]
    next_phase = DYNAMIC_SEQUENCE[next_idx]
    curr = KEYFRAMES[phase]
    nxt = KEYFRAMES[next_phase]
    return {
        "phase": phase,
        "next_phase": next_phase,
        "phase_t": local_t,
        "dynamic": True,
        "day_length_sec": _dynamic_day_length,
        "ambient_light_color": _lerp(curr["ambient"], nxt["ambient"], local_t),
        "emit_light_color": _lerp(curr["emit"], nxt["emit"], local_t),
        "emit_light_pos": _lerp(curr["light_pos"], nxt["light_pos"], local_t),
    }


def _dynamic_update(user, event):
    del user
    del event
    _apply_state(_dynamic_state(time.time()))


def _phase_offset(phase):
    if phase not in DYNAMIC_SEQUENCE:
        return 0.0
    return float(DYNAMIC_SEQUENCE.index(phase)) / float(len(DYNAMIC_SEQUENCE))


def configure_from_env():
    global _installed_dynamic, _dynamic_start_time, _dynamic_day_length

    phase = os.environ.get("PF_RTS_TIME_OF_DAY_PHASE", "baseline")
    dynamic = os.environ.get("PF_RTS_TIME_OF_DAY_DYNAMIC") == "1"
    if not dynamic:
        apply_phase(phase)
        return

    _dynamic_day_length = _float_env("PF_RTS_DAY_LENGTH_SEC", 240.0)
    _dynamic_start_time = time.time() - _phase_offset(phase) * _dynamic_day_length
    _apply_state(_dynamic_state(time.time()))
    if not _installed_dynamic:
        pf.register_event_handler(pf.EVENT_UPDATE_START, _dynamic_update, None)
        _installed_dynamic = True


def current_state():
    return dict(_current_state)
