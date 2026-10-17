"""Power triggers (power_below / power_above) against a fake Tuya device on this computer: validation, when they fire and
when they must not, fire-once and re-arming, and when the monitor sends UPDATEDPS. See fake_tuya.py and, for a real
socket, test_power_trigger_real.py.

Power in the fake is a raw DP 19 value in 0.1 W: 300 = 30 W. The rules here use 10 s for `for_s`, the shortest allowed;
a sampler thread asks for the status of the fake every second so the history fills quickly (the monitor polls every 15 s).
"""
import threading
import time

import pytest

from conftest import FAKE_NAME, device_list, fake_registered, put_devices
from test_scenarios import TEST, editable, guard, listing, put, scenario  # noqa: F401

LOW, HIGH = 300, 3000  # 30 W and 300 W


@pytest.fixture(scope="module")
def fake(dev):
    with fake_registered(dev) as f:
        yield f


@pytest.fixture(autouse=True)
def calm(fake):
    fake.start()
    fake.silent = False
    fake.ignore_control = False
    fake.no_updatedps = False
    fake.set(d1=True, d19=LOW)
    fake.commands.clear()
    yield
    fake.start()
    fake.no_updatedps = False
    fake.ignore_control = False


def rule(name, kind="power_below", watts=100, for_s=10, warmup_s=0, steps=None, enabled=True, device=FAKE_NAME, **extra):
    when = {"device": device, "watts": watts, "for_s": for_s}
    if warmup_s is not None:
        when["warmup_s"] = warmup_s
    doc = {"name": name, "enabled": enabled, "trigger": kind, "when": when, "default_pause_s": 0,
           "steps": steps if steps is not None else [{"device": FAKE_NAME, "action": "off"}]}
    doc.update(extra)
    return doc


class Sampler:
    """Asks for the status of the fake every second, so the history gets a sample per second."""

    def __init__(self, dev):
        self.dev, self.stop_ = dev, threading.Event()
        self.t = threading.Thread(target=self.run, daemon=True)

    def run(self):
        while not self.stop_.is_set():
            try:
                self.dev.request("POST", "/api/devices/status", params={"name": FAKE_NAME}, timeout=20)
            except Exception:
                pass
            self.stop_.wait(1.0)

    def __enter__(self):
        self.t.start()
        return self

    def __exit__(self, *a):
        self.stop_.set()
        self.t.join(timeout=25)


def scn(dev, name):
    return next(s for s in listing(dev)["scenarios"] if s["name"] == name)


def wait_until(pred, timeout, step=0.5):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(step)
    return False


def reason(dev, name):
    return scn(dev, name).get("watch", {}).get("reason")


# ---- validation ----------------------------------------------------------------------------------------------------

def bad_rules():
    def mut(**kw):
        d = rule(TEST + "bad")
        for k, v in kw.items():
            if k.startswith("when_"):
                if v is KeyError:
                    d["when"].pop(k[5:], None)
                else:
                    d["when"][k[5:]] = v
            else:
                d[k] = v
        return d
    yield "no-when", {k: v for k, v in rule(TEST + "bad").items() if k != "when"}
    yield "when-not-object", mut(when=[1])
    yield "no-device", mut(when_device=KeyError)
    yield "bad-device", mut(when_device="A B")
    yield "device-number", mut(when_device=1)
    yield "no-watts", mut(when_watts=KeyError)
    yield "negative-watts", mut(when_watts=-1)
    yield "huge-watts", mut(when_watts=20001)
    yield "fraction-watts", mut(when_watts=1.5)
    yield "text-watts", mut(when_watts="100")
    yield "no-for", mut(when_for_s=KeyError)
    yield "short-for", mut(when_for_s=9)
    yield "long-for", mut(when_for_s=901)
    yield "text-for", mut(when_for_s="30")
    yield "negative-warmup", mut(when_warmup_s=-1)
    yield "long-warmup", mut(when_warmup_s=3601)
    yield "text-warmup", mut(when_warmup_s="5")
    yield "unknown-trigger", mut(trigger="power_between")


@pytest.mark.parametrize("label,doc", list(bad_rules()), ids=[l for l, _ in bad_rules()])
def test_invalid_rules_are_rejected(dev, guard, label, doc):
    before = listing(dev)["scenarios"]
    r = put(dev, guard + [doc])
    assert r.status_code == 400, (label, r.text)
    assert "error" in r.json()
    assert listing(dev)["scenarios"] == before


def test_a_rule_round_trips_and_the_warmup_has_a_default(dev, guard):
    docs = [rule(TEST + "a", watts=250, for_s=45, warmup_s=7), rule(TEST + "b", "power_above", watts=0, for_s=900, warmup_s=None, enabled=False)]
    assert put(dev, guard + docs).status_code == 200
    body = listing(dev)
    assert "power_below" in body["triggers"] and "power_above" in body["triggers"]
    a, b = scn(dev, TEST + "a"), scn(dev, TEST + "b")
    assert a["trigger"] == "power_below" and a["when"] == {"device": FAKE_NAME, "watts": 250, "for_s": 45, "warmup_s": 7}
    assert b["trigger"] == "power_above" and b["when"] == {"device": FAKE_NAME, "watts": 0, "for_s": 900, "warmup_s": 60}
    assert "when" not in scn(dev, "power-restored") or scn(dev, "power-restored")["trigger"] != "power_on"
    assert all(s["trigger"] != "power_on" or "when" not in s for s in body["scenarios"])


def test_a_rule_that_names_a_missing_device_is_saved_and_says_why_it_waits(dev, guard):
    assert put(dev, guard + [rule(TEST + "gone", device="zz-no-such-device")]).status_code == 200
    assert wait_until(lambda: reason(dev, TEST + "gone") == "no-device", 12)


# ---- firing ------------------------------------------------------------------------------------------------------------

def test_power_below_fires_only_after_the_whole_time(dev, guard, fake):
    fake.set(d19=HIGH)  # one high answer first, so the low run starts now and not in an earlier test's history
    assert dev.request("POST", "/api/devices/status", params={"name": FAKE_NAME}, timeout=20).json()["ok"]
    fake.set(d19=LOW)
    t0 = time.time()
    with Sampler(dev):
        assert put(dev, guard + [rule(TEST + "lo")]).status_code == 200
        assert wait_until(lambda: len(fake.commands) > 0, 45), "the rule never fired"
        fired = time.time() - t0
    assert fired >= 9, fired  # not before 10 s of low power
    assert fake.commands[0][1] == {"1": False}
    sc = scn(dev, TEST + "lo")
    assert sc["run_trigger"] == "power-below"
    assert wait_until(lambda: scn(dev, TEST + "lo")["state"] == "done", 20)


def test_power_above_fires_when_the_power_stays_high(dev, guard, fake):
    fake.set(d19=HIGH)
    t0 = time.time()
    with Sampler(dev):
        assert put(dev, guard + [rule(TEST + "hi", "power_above")]).status_code == 200
        assert wait_until(lambda: len(fake.commands) > 0, 45)
    assert time.time() - t0 >= 9
    assert scn(dev, TEST + "hi")["run_trigger"] == "power-above"


def test_power_below_does_not_fire_on_high_power_and_says_so(dev, guard, fake):
    fake.set(d19=HIGH)
    with Sampler(dev):
        assert put(dev, guard + [rule(TEST + "no")]).status_code == 200
        assert wait_until(lambda: reason(dev, TEST + "no") == "not-met", 15)
        time.sleep(12)
    assert fake.commands == []
    w = scn(dev, TEST + "no")["watch"]
    assert w["met"] is False and w["armed"] is True and abs(w["power_w"] - 300.0) < 0.5


def test_one_high_sample_restarts_the_count(dev, guard, fake):
    with Sampler(dev):
        assert put(dev, guard + [rule(TEST + "spike")]).status_code == 200
        time.sleep(5)
        fake.set(d19=HIGH)
        time.sleep(2.5)
        fake.set(d19=LOW)
        spike_end = time.time()
        assert wait_until(lambda: len(fake.commands) > 0, 45)
        fired = time.time() - spike_end
    assert fired >= 9, f"fired {fired:.1f} s after the spike: the count did not restart"


def test_the_relay_must_be_on(dev, guard, fake):
    fake.set(d1=False)
    with Sampler(dev):
        assert put(dev, guard + [rule(TEST + "off")]).status_code == 200
        assert wait_until(lambda: reason(dev, TEST + "off") == "relay-off", 15)
        time.sleep(12)
    assert fake.commands == []


def test_the_warmup_after_switching_on_delays_the_rule(dev, guard, fake):
    fake.set(d1=False)
    with Sampler(dev):
        assert put(dev, guard + [rule(TEST + "warm", warmup_s=8)]).status_code == 200
        time.sleep(4)
        fake.set(d1=True)  # switched on with low power: a heater that is still ramping up
        t_on = time.time()
        assert wait_until(lambda: reason(dev, TEST + "warm") == "warmup", 10)
        assert wait_until(lambda: len(fake.commands) > 0, 50)
        fired = time.time() - t_on
    assert fired >= 16.5, f"fired {fired:.1f} s after switching on: the warm-up (8 s) and the time (10 s) were not both kept"


def test_an_unanswered_query_restarts_the_count(dev, guard, fake):
    with Sampler(dev):
        assert put(dev, guard + [rule(TEST + "gap")]).status_code == 200
        time.sleep(6)
        fake.stop()  # the device does not answer for a while
        time.sleep(2.5)
        fake.start()
        back = time.time()
        assert wait_until(lambda: len(fake.commands) > 0, 50)
        fired = time.time() - back
    assert fired >= 9, f"fired {fired:.1f} s after the device came back: no data must not count as low power"


def test_it_fires_once_and_is_armed_again_only_after_the_condition_was_false(dev, guard, fake):
    fake.ignore_control = True  # the scenario cannot switch it off: the condition stays true
    with Sampler(dev):
        assert put(dev, guard + [rule(TEST + "once")]).status_code == 200
        assert wait_until(lambda: len(fake.commands) >= 3, 60)  # three attempts of one run
        time.sleep(12)
        assert len(fake.commands) == 3, len(fake.commands)  # not run again while the condition holds
        assert scn(dev, TEST + "once")["watch"]["armed"] is False
        fake.set(d19=HIGH)  # the condition is false for a moment ...
        assert wait_until(lambda: scn(dev, TEST + "once")["watch"]["armed"] is True, 15)
        fake.set(d19=LOW)  # ... and true again
        assert wait_until(lambda: len(fake.commands) >= 6, 60)
    assert len(fake.commands) in (6, 7, 8, 9)


def test_met_while_its_scenario_runs_it_waits_for_the_run_and_starts_once_more(dev, guard, fake):
    """The condition comes true during a manual run: the rule stays armed and `pending`, and when that run ends (the
    condition still holding) the scenario runs once more, by the rule."""
    name = TEST + "busy"
    steps = [{"device": FAKE_NAME, "action": "on", "pause_after_s": 40}, {"device": "zz-no-such-device"}]
    fake.set(d19=HIGH)  # the condition is false while the manual run starts
    assert put(dev, guard + [rule(name, steps=steps)]).status_code == 200
    try:
        with Sampler(dev):
            assert dev.request("POST", "/api/scenarios/run", params={"name": name}).status_code == 200
            assert wait_until(lambda: scn(dev, name).get("pause_left_s") is not None, 20), scn(dev, name)
            assert len(fake.commands) == 1
            fake.set(d19=LOW)  # "below 100 W for 10 s" comes true during the pause
            assert wait_until(lambda: scn(dev, name)["watch"]["pending"] is True, 25), scn(dev, name)
            s = scn(dev, name)
            assert s["state"] == "running" and s["run_trigger"] == "manual" and s["watch"]["armed"] is True, s
            assert len(fake.commands) == 1  # not started a second time while the first run goes on
            assert wait_until(lambda: scn(dev, name)["run_trigger"] == "power-below", 50), scn(dev, name)
            s = scn(dev, name)
            assert s["state"] in ("waiting", "running"), s
            assert s["watch"]["pending"] is False and s["watch"]["armed"] is False, s["watch"]
            assert wait_until(lambda: len(fake.commands) == 2, 15), fake.commands
    finally:
        dev.request("POST", "/api/scenarios/stop", params={"name": name})


def test_a_disabled_rule_never_fires(dev, guard, fake):
    with Sampler(dev):
        assert put(dev, guard + [rule(TEST + "dis", enabled=False)]).status_code == 200
        time.sleep(20)
    assert fake.commands == []
    assert "watch" not in scn(dev, TEST + "dis")


def test_an_ignored_device_is_never_fired_on(dev, guard, fake):
    devices = device_list(dev)
    for d in devices:
        if d["name"] == FAKE_NAME:
            d["ignore"] = True
    assert put_devices(dev, devices).status_code == 200
    try:
        assert put(dev, guard + [rule(TEST + "ign")]).status_code == 200
        assert wait_until(lambda: reason(dev, TEST + "ign") == "ignored", 12)
        time.sleep(8)
        assert fake.commands == []
    finally:
        for d in devices:
            if d["name"] == FAKE_NAME:
                d["ignore"] = False
        put_devices(dev, devices)


# ---- UPDATEDPS: only for a device an enabled rule waits for -----------------------------------------------------------------

def test_updatedps_is_sent_only_while_a_rule_waits_for_the_device(dev, guard, fake):
    gap = dev.get("/api/devices").json()["poll_interval_s"]
    fake.updatedps = 0
    time.sleep(gap + 5)  # a plain poll or two, no rule at all
    assert fake.updatedps == 0, "UPDATEDPS without any rule"

    assert put(dev, guard + [rule(TEST + "watch", watts=1, for_s=900)]).status_code == 200  # never fires: 1 W
    assert wait_until(lambda: fake.updatedps > 0, gap + 10), "no UPDATEDPS for a watched device"
    state = next(d for d in dev.get("/api/devices").json()["devices"] if d["name"] == FAKE_NAME)["state"]
    assert state["online"] is True and state["fails"] == 0

    assert put(dev, guard + [rule(TEST + "watch", watts=1, for_s=900, enabled=False)]).status_code == 200
    time.sleep(3)
    settled = fake.updatedps
    time.sleep(gap + 5)
    assert fake.updatedps == settled, "UPDATEDPS went on after the rule was switched off"


def test_a_rule_on_another_device_does_not_make_this_one_get_updatedps(dev, guard, fake):
    gap = dev.get("/api/devices").json()["poll_interval_s"]
    assert put(dev, guard + [rule(TEST + "other", device="zz-no-such-device", watts=1, for_s=900)]).status_code == 200
    fake.updatedps = 0
    time.sleep(gap + 6)
    assert fake.updatedps == 0


def test_a_device_that_does_not_know_updatedps_is_polled_plainly(dev, guard, fake):
    gap = dev.get("/api/devices").json()["poll_interval_s"]
    fake.no_updatedps = True
    fake.updatedps = 0
    assert put(dev, guard + [rule(TEST + "unsup", watts=1, for_s=900)]).status_code == 200
    time.sleep(gap * 2 + 8)  # at least two cycles
    state = next(d for d in dev.get("/api/devices").json()["devices"] if d["name"] == FAKE_NAME)["state"]
    assert state["online"] is True and state["fails"] == 0, state  # not marked offline: it falls back to a plain query
    assert 1 <= fake.updatedps <= 2, fake.updatedps  # tried once, then plain queries for a while
    samples = dev.get("/api/devices/history", params={"name": FAKE_NAME}).json()["samples"]
    assert samples[-1]["ok"] is True
