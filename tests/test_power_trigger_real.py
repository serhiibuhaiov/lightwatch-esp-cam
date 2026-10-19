"""Power triggers on a REAL socket (opt-in): TUYA_DEVICE=toweldryer tests/run.sh -m power_trigger

The socket must be online, switched on and drawing some power (the towel dryer takes ~45 W). The tests switch it off and on
again through the triggers and the API and leave it switched ON at the end. They use the real cadence of the monitor
(a poll every monitor.interval_s) and the real UPDATEDPS (the socket needs updatedps:true and poll monitoring: UPDATEDPS
follows the device's flag, not the rules), so they also show how late the board reacts.
"""
import os
import time

import pytest

from test_scenarios import TEST, editable, listing, put, scenario  # noqa: F401

pytestmark = pytest.mark.power_trigger


@pytest.fixture(scope="module")
def target(dev):
    name = os.getenv("TUYA_DEVICE")
    if not name:
        pytest.skip("set TUYA_DEVICE to a socket that is on and draws power (e.g. toweldryer)")
    return name


def device(dev, name):
    return next(d for d in dev.get("/api/devices").json()["devices"] if d["name"] == name)


def relay(dev, name):
    return device(dev, name)["state"].get("dps", {}).get("1")


def command(dev, name, op):
    r = dev.request("POST", f"/api/devices/{op}", params={"name": name}, timeout=30).json()
    assert r["ok"], r
    # the cached state only changes with a status query: ask, so that relay() sees the new state at once
    dev.request("POST", "/api/devices/status", params={"name": name}, timeout=30)


def wait_until(pred, timeout, step=1.0):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return time.time()
        time.sleep(step)
    return None


@pytest.fixture
def socket_on(dev, target):
    """The socket is on and measured; whatever the test did, it is switched on again at the end."""
    st = device(dev, target)["state"]
    assert st.get("online"), f"{target} is not online"
    if not st.get("on"):
        command(dev, target, "on")
        time.sleep(40)  # let the first readings come in
    original = [editable(s) for s in listing(dev)["scenarios"]]
    yield
    put(dev, original)
    command(dev, target, "on")


def test_power_above_sees_a_fresh_reading_soon_after_switching_on(dev, target, socket_on):
    """Right after switching on, the socket's cached power is 0 for ~30 s. With UPDATEDPS the monitor sees the real power at its
    next poll, so a rule "above 20 W for 30 s" fires about 30 s + one poll interval after switching on, not 30 s + 33 s + ..."""
    interval = dev.get("/api/devices").json()["poll_interval_s"]
    command(dev, target, "off")
    time.sleep(interval + 8)  # the history gets a few clean "relay off" answers
    original = [editable(s) for s in listing(dev)["scenarios"]]
    command(dev, target, "on")
    t_on = time.time()
    assert relay(dev, target) is True
    doc = {"name": TEST + "real-above", "enabled": True, "trigger": "power_above", "default_pause_s": 0,
           "when": {"device": target, "watts": 20, "for_s": 30, "warmup_s": 0},
           "steps": [{"device": target, "action": "off"}]}
    assert put(dev, original + [doc]).status_code == 200
    t_off = wait_until(lambda: relay(dev, target) is False, 150, step=1.0)
    assert t_off, "the rule did not switch the socket off"
    took = t_off - t_on
    print(f"power_above (20 W, 30 s): the socket was switched off {took:.0f} s after it was switched on (poll interval {interval} s)")
    sc = next(s for s in listing(dev)["scenarios"] if s["name"] == TEST + "real-above")
    assert sc["run_trigger"] == "power-above" and sc["run_steps"][0]["state"] == "ok"
    assert 28 <= took <= 30 + 2 * interval + 5, took  # without UPDATEDPS it would be 30 s of cached zeros later


def test_power_below_fires_at_once_when_the_history_already_shows_it(dev, target, socket_on):
    """The socket has been drawing ~45 W (below 100 W) for a long time: the history already holds the condition, so the
    rule fires at the next look. Then the socket is off, the rule is armed again and does not fire on an off socket."""
    original = [editable(s) for s in listing(dev)["scenarios"]]
    assert relay(dev, target) is True
    # enough history of low power: wait until the history holds at least 40 s of answers with the relay on
    time.sleep(max(0, 45 - 0))
    doc = {"name": TEST + "real-below", "enabled": True, "trigger": "power_below", "default_pause_s": 0,
           "when": {"device": target, "watts": 100, "for_s": 30, "warmup_s": 0},
           "steps": [{"device": target, "action": "off"}]}
    t0 = time.time()
    assert put(dev, original + [doc]).status_code == 200
    t_off = wait_until(lambda: relay(dev, target) is False, 60, step=1.0)
    assert t_off, "the rule did not switch the socket off"
    print(f"power_below (100 W, 30 s): switched off {t_off - t0:.0f} s after the rule was saved")
    assert t_off - t0 <= 20
    sc = next(s for s in listing(dev)["scenarios"] if s["name"] == TEST + "real-below")
    assert sc["run_trigger"] == "power-below"
    time.sleep(25)  # it is off now: the rule must not do anything more
    assert relay(dev, target) is False
    w = next(s for s in listing(dev)["scenarios"] if s["name"] == TEST + "real-below")["watch"]
    assert w["reason"] == "relay-off" and w["armed"] is True
