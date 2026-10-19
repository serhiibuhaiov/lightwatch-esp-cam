"""Scenario steps against a fake Tuya device on this computer: switching on and off, a step for an ignored device,
the pause rule, failures, stop. Nothing here touches a real socket (see fake_tuya.py).
"""
import time

import pytest

from conftest import FAKE_NAME, device_list, fake_registered, put_devices
from test_scenarios import GHOST, TEST, editable, guard, listing, put, scenario, wait_state  # noqa: F401


@pytest.fixture(scope="module")
def fake(dev):
    with fake_registered(dev) as f:
        yield f


@pytest.fixture(autouse=True)
def calm(fake):
    fake.start()
    fake.silent = False
    fake.ignore_control = False
    fake.answer_delay = 0.0
    fake.set(d1=True)
    fake.commands.clear()
    yield
    fake.start()


def run(dev, name):
    r = dev.request("POST", "/api/scenarios/run", params={"name": name})
    assert r.status_code == 200, r.text
    return time.time()


def steps_of(dev, name):
    return next(s for s in listing(dev)["scenarios"] if s["name"] == name)["run_steps"]


def set_ignore(dev, flag):
    devices = device_list(dev)
    for d in devices:
        if d["name"] == FAKE_NAME:
            d["ignore"] = flag
    assert put_devices(dev, devices).status_code == 200


# ---- the action field ------------------------------------------------------------------------------------------

@pytest.mark.parametrize("bad", ["up", "ON", "", 1, True, None, ["on"]])
def test_an_unknown_action_is_rejected(dev, guard, bad):
    step = {"device": FAKE_NAME}
    if bad is not None:
        step["action"] = bad
    r = put(dev, guard + [scenario(TEST + "bad", [step])])
    if bad is None:  # absent: the default, on
        assert r.status_code == 200
    else:
        assert r.status_code == 400 and "action" in r.json()["error"]


def test_the_action_defaults_to_on_and_round_trips(dev, guard):
    doc = scenario(TEST + "act", [{"device": "a"}, {"device": "b", "action": "off"}, {"device": "c", "action": "on", "enabled": False}])
    assert put(dev, guard + [doc]).status_code == 200
    got = next(s for s in listing(dev)["scenarios"] if s["name"] == TEST + "act")
    assert [(s["device"], s["action"], s["enabled"]) for s in got["steps"]] == [("a", "on", True), ("b", "off", True), ("c", "on", False)]
    assert [s["action"] for s in editable(got)["steps"]] == ["on", "off", "on"]


# ---- switching ---------------------------------------------------------------------------------------------------

def test_a_step_switches_the_device_off_and_on_and_checks_the_state(dev, guard, fake):
    doc = scenario(TEST + "sw", [{"device": FAKE_NAME, "action": "off"}, {"device": FAKE_NAME, "action": "on"}], default_pause_s=0)
    assert put(dev, guard + [doc]).status_code == 200
    run(dev, TEST + "sw")
    wait_state(dev, TEST + "sw", ("done",))
    steps = steps_of(dev, TEST + "sw")
    assert [s["state"] for s in steps] == ["ok", "ok"] and [s["attempts"] for s in steps] == [1, 1]
    assert "switched off" in steps[0]["detail"] and "switched on" in steps[1]["detail"]
    assert [c for _, c in fake.commands] == [{"1": False}, {"1": True}]
    assert fake.get(1) is True
    assert fake.hmac_errors == 0


def test_the_command_is_sent_even_if_the_device_looks_to_be_in_that_state_already(dev, guard, fake):
    assert dev.request("POST", "/api/devices/status", params={"name": FAKE_NAME}, timeout=30).json()["dps"]["1"] is True  # the cache says on
    assert put(dev, guard + [scenario(TEST + "again", [{"device": FAKE_NAME, "action": "on"}])]).status_code == 200
    run(dev, TEST + "again")
    wait_state(dev, TEST + "again", ("done",))
    assert [c for _, c in fake.commands] == [{"1": True}]  # no skipping on the word of the cache
    assert steps_of(dev, TEST + "again")[0]["state"] == "ok"


def test_a_command_that_has_no_effect_fails_after_three_attempts(dev, guard, fake):
    fake.ignore_control = True
    fake.set(d1=False)
    assert put(dev, guard + [scenario(TEST + "noop", [{"device": FAKE_NAME, "action": "on"}, {"device": GHOST}])]).status_code == 200
    t = run(dev, TEST + "noop")
    wait_state(dev, TEST + "noop", ("done",), timeout=30)
    took = time.time() - t
    step = steps_of(dev, TEST + "noop")[0]
    assert step["state"] == "failed" and step["attempts"] == 3 and "does not show it on" in step["detail"]
    assert len(fake.commands) == 3  # one command per attempt
    assert 3.5 < took < 12, took  # two pauses of 2 s between the attempts
    assert steps_of(dev, TEST + "noop")[1]["state"] == "failed"  # the next step still ran (no such device)


def test_an_off_command_is_verified_too(dev, guard, fake):
    fake.ignore_control = True  # stays on
    assert put(dev, guard + [scenario(TEST + "offcheck", [{"device": FAKE_NAME, "action": "off"}])]).status_code == 200
    run(dev, TEST + "offcheck")
    wait_state(dev, TEST + "offcheck", ("done",), timeout=30)
    step = steps_of(dev, TEST + "offcheck")[0]
    assert step["state"] == "failed" and "does not show it off" in step["detail"]


def test_a_device_that_is_gone_fails_after_three_attempts_and_the_scenario_goes_on(dev, guard, fake):
    fake.stop()  # refused at once: three quick attempts, two pauses
    assert put(dev, guard + [scenario(TEST + "gone", [{"device": FAKE_NAME}, {"device": GHOST}])]).status_code == 200
    t = run(dev, TEST + "gone")
    wait_state(dev, TEST + "gone", ("done",), timeout=30)
    took = time.time() - t
    step = steps_of(dev, TEST + "gone")[0]
    assert step["state"] == "failed" and step["attempts"] == 3 and step["detail"].startswith("connect:")
    assert 3.5 < took < 9, took
    assert steps_of(dev, TEST + "gone")[1]["state"] == "failed"


# ---- the ignore flag -------------------------------------------------------------------------------------------

def test_a_step_for_an_ignored_device_switches_it(dev, guard, fake):
    # ignore only keeps the monitor away from the device: scenario steps (and buttons) still switch it
    try:
        set_ignore(dev, True)
        doc = scenario(TEST + "ign", [{"device": FAKE_NAME, "action": "off"}, {"device": GHOST}])
        assert put(dev, guard + [doc]).status_code == 200
        run(dev, TEST + "ign")
        wait_state(dev, TEST + "ign", ("done",))
        steps = steps_of(dev, TEST + "ign")
        assert steps[0]["state"] == "ok", steps[0]
        assert steps[1]["state"] == "failed"
        assert fake.get(1) is False and any(c == {"1": False} for _, c in fake.commands)
    finally:
        set_ignore(dev, False)


def test_after_un_ignoring_the_step_runs_again(dev, guard, fake):
    assert put(dev, guard + [scenario(TEST + "unign", [{"device": FAKE_NAME, "action": "off"}])]).status_code == 200
    run(dev, TEST + "unign")
    wait_state(dev, TEST + "unign", ("done",))
    assert steps_of(dev, TEST + "unign")[0]["state"] == "ok" and fake.get(1) is False


# ---- the pause rule, stop, and one run at a time ---------------------------------------------------------------

def test_the_pause_comes_after_a_step_that_switched_and_stop_ends_it(dev, guard, fake):
    doc = scenario(TEST + "pause", [{"device": FAKE_NAME, "action": "off", "pause_after_s": 60}, {"device": GHOST}])
    assert put(dev, guard + [doc]).status_code == 200
    run(dev, TEST + "pause")
    time.sleep(4)
    sc = next(s for s in listing(dev)["scenarios"] if s["name"] == TEST + "pause")
    assert sc["state"] == "running"  # in the 60 s pause
    steps = sc["run_steps"]
    assert steps[0]["state"] == "ok" and "pause 60 s" in steps[0]["detail"] and steps[1]["state"] == "pending"
    assert dev.request("POST", "/api/scenarios/run", params={"name": TEST + "pause"}).status_code == 409  # one run at a time
    t = time.time()
    assert dev.request("POST", "/api/scenarios/stop", params={"name": TEST + "pause"}).status_code == 200
    sc = wait_state(dev, TEST + "pause", ("stopped",), timeout=5)
    assert time.time() - t < 3  # the pause is cut at once
    assert [s["state"] for s in sc["run_steps"]] == ["ok", "pending"]  # the next step never started
    assert dev.request("POST", "/api/scenarios/stop", params={"name": TEST + "pause"}).status_code == 409  # nothing to stop now


def test_a_shorter_pause_is_kept(dev, guard, fake):
    doc = scenario(TEST + "short", [{"device": FAKE_NAME, "action": "off", "pause_after_s": 3}, {"device": GHOST}])
    assert put(dev, guard + [doc]).status_code == 200
    t = run(dev, TEST + "short")
    wait_state(dev, TEST + "short", ("done",), timeout=20)
    assert 3 <= time.time() - t < 9


def test_stop_works_between_steps_and_the_next_command_never_goes_out(dev, guard, fake):
    # a long pause after the first step: stopped during it, the second step (off) never runs
    doc = scenario(TEST + "w", [{"device": FAKE_NAME, "action": "on", "pause_after_s": 30}, {"device": FAKE_NAME, "action": "off"}])
    assert put(dev, guard + [doc]).status_code == 200
    run(dev, TEST + "w")
    time.sleep(3)
    assert dev.request("POST", "/api/scenarios/stop", params={"name": TEST + "w"}).status_code == 200
    wait_state(dev, TEST + "w", ("stopped",), timeout=5)
    assert fake.get(1) is True  # the "off" never went out
