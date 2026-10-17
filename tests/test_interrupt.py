"""on_interrupt and the history of the runs, against the fake Tuya device on this computer (no real socket is touched).

The validation and the history API run in the default suite. The tests that restart the board (a software restart through
`PUT /api/config?reboot=1`: reset reason SW, so the power_on scenarios do not start) are opt-in:

  tests/run.sh -m restart        ~3 min, restarts the board 4 times
"""
import time

import pytest
import requests

from conftest import FAKE_NAME, fake_registered
from test_scenarios import TEST, guard, listing, put, scenario, wait_state  # noqa: F401


@pytest.fixture(scope="module")
def fake(dev):
    with fake_registered(dev) as f:
        yield f


def scn(dev, name):
    return next(s for s in listing(dev)["scenarios"] if s["name"] == name)


def history(dev, **params):
    r = dev.get("/api/scenarios/history", params=params)
    assert r.status_code == 200, r.text
    return r.json()


def reboot(dev):
    """A software restart (reset reason SW) and back on the network."""
    cfg = dev.get("/api/config").text  # the passwords come back empty: a PUT keeps the stored ones
    r = dev.request("PUT", "/api/config", params={"reboot": "1"}, data=cfg, headers={"Content-Type": "application/json"})
    assert r.status_code == 200 and r.json()["rebooting"], r.text
    time.sleep(4)
    end = time.time() + 90
    while time.time() < end:
        try:
            st = dev.get("/api/status", timeout=3).json()
            if st["uptime_s"] < 60 and st["wifi"]["connected"]:
                assert st["boot_history"][-1]["reason"] == "SW"
                return st
        except requests.RequestException:
            pass
        time.sleep(1)
    pytest.fail("the board did not come back after the restart")


def run(dev, name):
    assert dev.request("POST", "/api/scenarios/run", params={"name": name}).status_code == 200


def in_pause(dev, name, timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        s = scn(dev, name)
        if s.get("pause_left_s") is not None:
            return s
        time.sleep(0.5)
    pytest.fail(f"{name} never reached its pause: {s}")


OI = {"power": "restart", "software": "resume", "crash": "resume", "brownout": "resume", "resume_within_min": 120}


# ---- validation and the API (default suite) --------------------------------------------------------------------------

def bad_interrupts():
    yield "not-object", [1]
    yield "capital", {"software": "Resume"}
    yield "number", {"crash": 1}
    yield "unknown", {"power": "continue"}
    yield "negative-min", {"resume_within_min": -1}
    yield "long-min", {"resume_within_min": 1441}
    yield "text-min", {"resume_within_min": "5"}
    yield "fraction-min", {"resume_within_min": 1.5}


@pytest.mark.parametrize("label,oi", list(bad_interrupts()), ids=[x[0] for x in bad_interrupts()])
def test_invalid_on_interrupt_is_rejected(dev, guard, label, oi):
    r = put(dev, guard + [scenario(TEST + "oi", on_interrupt=oi)])
    assert r.status_code == 400, (label, r.text)
    assert "on_interrupt" in r.json()["error"]


def test_on_interrupt_round_trips_and_has_defaults(dev, guard):
    mine = {"power": "resume", "software": "drop", "crash": "restart", "brownout": "drop", "resume_within_min": 30}
    assert put(dev, guard + [scenario(TEST + "oi1", on_interrupt=mine), scenario(TEST + "oi2"),
                             scenario(TEST + "oi3", on_interrupt={"software": "restart"})]).status_code == 200
    assert scn(dev, TEST + "oi1")["on_interrupt"] == mine
    assert scn(dev, TEST + "oi2")["on_interrupt"] == OI
    assert scn(dev, TEST + "oi3")["on_interrupt"] == dict(OI, software="restart")


def test_history_validates_its_parameters(dev):
    for params in ({"limit": "0"}, {"limit": "51"}, {"limit": "x"}, {"before": "0"}, {"before": "-3"}, {"name": "A B"}, {"name": ""}):
        assert dev.get("/api/scenarios/history", params=params).status_code == 400, params
    assert dev.get("/api/scenarios/history", auth=False).status_code == 401


def test_a_finished_run_is_in_the_history_with_its_steps(dev, guard, fake):
    fake.set(d1=True)
    assert put(dev, guard + [scenario(TEST + "h", [{"device": FAKE_NAME, "action": "off"}, {"device": "zz-no-such-device"}])]).status_code == 200
    run(dev, TEST + "h")
    wait_state(dev, TEST + "h", ("done",), timeout=20)
    time.sleep(1.5)  # the engine writes the history line right after the run ends
    h = history(dev, name=TEST + "h", limit=1)
    rec = h["runs"][0]
    assert rec["name"] == TEST + "h" and rec["result"] == "done" and rec["trigger"] == "manual"
    assert rec["switched"] == 1 and rec["failed"] == 1 and rec["duration_s"] >= 0
    assert [(s["device"], s["action"], s["state"]) for s in rec["steps"]] == [(FAKE_NAME, "off", "ok"), ("zz-no-such-device", "on", "failed")]
    assert rec["steps"][0]["detail"] == "switched off; pause 5 s before the next step"
    assert rec["steps"][1]["detail"] == "no such device in /devices.json"
    assert rec["start"] is None or rec["start"] > 1700000000
    # newest first, pages with `before`
    run(dev, TEST + "h")
    wait_state(dev, TEST + "h", ("done",), timeout=20)
    time.sleep(1.5)
    page1 = history(dev, name=TEST + "h", limit=1)
    assert page1["more"] is True and page1["runs"][0]["id"] > rec["id"]
    page2 = history(dev, name=TEST + "h", limit=1, before=page1["runs"][0]["id"])
    assert page2["runs"][0]["id"] == rec["id"]
    assert all(r["name"] == TEST + "h" for r in history(dev, name=TEST + "h", limit=50)["runs"])


# ---- restarts (opt-in: -m restart) ------------------------------------------------------------------------------------

@pytest.mark.restart
def test_a_run_in_its_pause_is_continued_after_a_software_restart(dev, guard, fake):
    fake.set(d1=True)
    assert put(dev, guard + [scenario(TEST + "resume", [{"device": FAKE_NAME, "action": "off", "pause_after_s": 45},
                                                           {"device": FAKE_NAME, "action": "on"}])]).status_code == 200
    run(dev, TEST + "resume")
    in_pause(dev, TEST + "resume")
    paused_at = time.time()
    time.sleep(5)  # /runs.json is written at most every 3 s
    reboot(dev)
    s = scn(dev, TEST + "resume")
    assert s["state"] == "running" and s["resumed"] == "SW at step 2", s
    assert s["run_steps"][0]["state"] == "ok" and s["run_steps"][1]["state"] == "pending"
    left = s.get("pause_left_s")
    gone = time.time() - paused_at
    assert left is not None and 45 - gone - 8 <= left <= 45 - gone + 5, (left, gone)  # the pause goes on with what was left
    assert fake.get(1) is False
    wait_state(dev, TEST + "resume", ("done",), timeout=70)
    assert fake.get(1) is True
    time.sleep(1.5)
    rec = history(dev, name=TEST + "resume", limit=1)["runs"][0]
    assert rec["result"] == "done" and rec["resumed"] == "SW at step 2"
    assert [s["state"] for s in rec["steps"]] == ["ok", "ok"]
    assert rec["steps"][1]["at_s"] >= 44  # step times count the restart too


@pytest.mark.restart
def test_restart_starts_the_run_again_and_records_the_interrupted_one(dev, guard, fake):
    fake.set(d1=True)
    assert put(dev, guard + [scenario(TEST + "again", [{"device": FAKE_NAME, "action": "off", "pause_after_s": 600},
                                                          {"device": FAKE_NAME, "action": "on"}],
                                      on_interrupt=dict(OI, software="restart"))]).status_code == 200
    run(dev, TEST + "again")
    in_pause(dev, TEST + "again")
    time.sleep(5)
    fake.set(d1=True)  # if the run starts again, step 1 switches it off again
    reboot(dev)
    end = time.time() + 40
    while time.time() < end and not (fake.get(1) is False and scn(dev, TEST + "again").get("pause_left_s")):
        time.sleep(1)
    s = scn(dev, TEST + "again")
    assert s["state"] == "running" and s["run_steps"][0]["state"] == "ok" and s["pause_left_s"] > 500, s
    assert fake.get(1) is False
    time.sleep(1.5)
    rec = history(dev, name=TEST + "again", limit=1)["runs"][0]
    assert rec["result"] == "interrupted" and "on_interrupt.software = restart" in rec["reason"], rec
    assert dev.request("POST", "/api/scenarios/stop", params={"name": TEST + "again"}).status_code == 200


@pytest.mark.restart
def test_drop_leaves_the_run_interrupted(dev, guard, fake):
    fake.set(d1=True)
    assert put(dev, guard + [scenario(TEST + "drop", [{"device": FAKE_NAME, "action": "off", "pause_after_s": 600},
                                                         {"device": FAKE_NAME, "action": "on"}],
                                      on_interrupt=dict(OI, software="drop"))]).status_code == 200
    run(dev, TEST + "drop")
    in_pause(dev, TEST + "drop")
    time.sleep(5)
    reboot(dev)
    s = scn(dev, TEST + "drop")
    assert s["state"] == "interrupted" and "on_interrupt.software = drop" in s["interrupt_reason"], s
    assert [x["state"] for x in s["run_steps"]] == ["ok", "pending"]
    assert dev.request("POST", "/api/scenarios/stop", params={"name": TEST + "drop"}).status_code == 409
    time.sleep(1.5)
    assert history(dev, name=TEST + "drop", limit=1)["runs"][0]["result"] == "interrupted"
    assert fake.get(1) is False  # nothing ran after the restart


@pytest.mark.restart
def test_a_scenario_edited_during_the_run_is_started_again(dev, guard, fake):
    fake.set(d1=True)
    steps = [{"device": FAKE_NAME, "action": "off", "pause_after_s": 600}, {"device": FAKE_NAME, "action": "on"}]
    assert put(dev, guard + [scenario(TEST + "edit", steps)]).status_code == 200
    run(dev, TEST + "edit")
    in_pause(dev, TEST + "edit")
    edited = [dict(steps[0], pause_after_s=500), steps[1]]  # the running copy keeps 600 s
    assert put(dev, guard + [scenario(TEST + "edit", edited)]).status_code == 200
    time.sleep(5)
    fake.set(d1=True)
    reboot(dev)
    end = time.time() + 40
    while time.time() < end and not scn(dev, TEST + "edit").get("pause_left_s"):
        time.sleep(1)
    s = scn(dev, TEST + "edit")
    assert s["state"] == "running" and 400 < s["pause_left_s"] <= 500, s  # started again, with the new steps
    time.sleep(1.5)
    rec = history(dev, name=TEST + "edit", limit=1)["runs"][0]
    assert rec["result"] == "interrupted" and "edited" in rec["reason"], rec
    assert dev.request("POST", "/api/scenarios/stop", params={"name": TEST + "edit"}).status_code == 200


# ---- a power rule that fired before a restart does not fire again for the same condition (-m restart) -----------------

def _watch(dev, name):
    return scn(dev, name).get("watch", {})


def _runs(dev, name):
    return len(history(dev, name=name, limit=50)["runs"])


@pytest.mark.restart
def test_a_power_rule_whose_run_is_continued_fires_again_only_after_its_condition_went_away(dev, guard, fake):
    from test_power_trigger import HIGH, LOW, Sampler, rule, wait_until
    fake.start()
    fake.set(d1=True, d19=LOW)  # 30 W: "below 100 W" holds
    name = TEST + "rule1"
    steps = [{"device": FAKE_NAME, "action": "on", "pause_after_s": 40}, {"device": "zz-no-such-device"}]
    assert put(dev, guard + [rule(name, steps=steps)]).status_code == 200
    try:
        with Sampler(dev):
            in_pause(dev, name, timeout=60)
        s = scn(dev, name)
        assert s["run_trigger"] == "power-below" and s["watch"]["armed"] is False, s
        time.sleep(5)  # /runs.json written
        reboot(dev)
        s = scn(dev, name)
        assert s["state"] == "running" and s["resumed"], s
        assert s["watch"]["armed"] is False and s["watch"].get("restored") is True, s["watch"]
        before = _runs(dev, name)
        with Sampler(dev):
            wait_state(dev, name, ("done",), timeout=70)  # the continued run ends; the condition still holds
            time.sleep(30)  # long enough for "below 100 W for 10 s" to be met again from the fresh history
            s = scn(dev, name)
            assert s["state"] == "done" and s["watch"]["armed"] is False and s["watch"]["met"] is True, s
            assert _runs(dev, name) == before + 1  # only the continued run ended: no second one
            fake.set(d19=HIGH)  # the condition goes away: armed again
            assert wait_until(lambda: _watch(dev, name).get("armed") is True, 40), _watch(dev, name)
            assert "restored" not in _watch(dev, name)
            fake.set(d19=LOW)  # and it comes back: it fires again
            assert wait_until(lambda: scn(dev, name)["state"] in ("waiting", "running"), 40), scn(dev, name)
    finally:
        dev.request("POST", "/api/scenarios/stop", params={"name": name})
        fake.set(d19=LOW)


@pytest.mark.restart
def test_a_power_rule_whose_run_had_ended_stays_fired_over_a_restart(dev, guard, fake):
    from test_power_trigger import LOW, Sampler, rule
    fake.start()
    fake.set(d1=True, d19=LOW)
    name = TEST + "rule2"
    assert put(dev, guard + [rule(name, steps=[{"device": "zz-no-such-device"}])]).status_code == 200
    with Sampler(dev):
        end = time.time() + 60
        while time.time() < end and scn(dev, name)["state"] != "done":
            time.sleep(1)
    s = scn(dev, name)
    assert s["state"] == "done" and s["watch"]["armed"] is False, s
    time.sleep(5)  # /runs.json keeps the fired rule although no run is alive
    reboot(dev)
    s = scn(dev, name)
    assert s["watch"]["armed"] is False and s["watch"].get("restored") is True, s.get("watch")
    before = _runs(dev, name)
    with Sampler(dev):
        time.sleep(35)  # the condition holds the whole time
    s = scn(dev, name)
    assert s["state"] == "idle" and s["watch"]["armed"] is False and s["watch"]["met"] is True, s
    assert _runs(dev, name) == before

