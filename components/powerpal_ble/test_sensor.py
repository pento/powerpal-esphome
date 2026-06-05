"""Tests for the powerpal_ble sensor config validation.

Focused on ``_validate``, which resolves the dynamic ``live_power`` default and
guards the explicit ``live_power: true`` + no-power-sensor case. The module is
loaded by path so the suite needs no package/sys.path wiring; it only requires
``esphome`` to be importable.
"""

import importlib.util
import pathlib

import pytest
import esphome.config_validation as cv

_SENSOR_PY = pathlib.Path(__file__).with_name("sensor.py")
_SPEC = importlib.util.spec_from_file_location("powerpal_ble_sensor", _SENSOR_PY)
if _SPEC is None or _SPEC.loader is None:
    raise RuntimeError(f"could not load component module from {_SENSOR_PY}")
sensor = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(sensor)

CONF_POWER = sensor.CONF_POWER
CONF_ENERGY = sensor.CONF_ENERGY
CONF_LIVE_POWER = sensor.CONF_LIVE_POWER

# `_validate` only inspects key presence and the live_power flag, so the sensor
# configs themselves can be trivial placeholders.
_SENSOR = {"placeholder": True}


@pytest.mark.parametrize(
    "config, expected",
    [
        # Power sensor present, live_power unset -> live by default.
        ({CONF_POWER: _SENSOR}, True),
        # Energy-only, live_power unset -> off, and crucially without erroring.
        ({CONF_ENERGY: _SENSOR}, False),
        # Explicit values always win over the dynamic default.
        ({CONF_LIVE_POWER: True, CONF_POWER: _SENSOR}, True),
        ({CONF_LIVE_POWER: False, CONF_POWER: _SENSOR}, False),
        # Explicit false with no power sensor is fine (nothing to publish to).
        ({CONF_LIVE_POWER: False}, False),
    ],
)
def test_live_power_default(config, expected):
    result = sensor._validate(dict(config))
    assert result[CONF_LIVE_POWER] is expected


def test_explicit_live_power_without_power_sensor_is_invalid():
    with pytest.raises(cv.Invalid, match="requires a `power:` sensor") as exc_info:
        sensor._validate({CONF_LIVE_POWER: True})
    # The message should offer both fixes: add a power sensor, or disable
    # live_power (the most direct fix when the user doesn't need live updates).
    assert "live_power: false" in str(exc_info.value)
