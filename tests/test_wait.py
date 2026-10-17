"""The step "wait": waits its seconds, always (also after a step that failed), after the pause of the step before it. Timed
with HTTP steps against the fake HTTP server on this computer (test_http.py): no socket is touched. Validation and the round
trip; the timing; a stop during a wait; -m restart: a run continued in the middle of a wait waits it again in full.
"""
import time

import pytest

from test_http import Fake, fake, http_scenario, scn  # noqa: F401
from test_interrupt import reboot
from test_scenarios import TEST, guard, put, wait_state  # noqa: F401


def wait(seconds, **kw):
    return dict({"action": "wait", "seconds": seconds}, **kw)


def hit(fake, path, **kw):
    return dict({"action": "http", "url": fake.base + path}, **kw)


def times(fake, since, path):
    return [g["at"] for g in fake.requests[since:] if g["path"] == path]


@pytest.mark.parametrize("step, words", [
    ({"action": "wait"}, "seconds must be an integer 1..3600"),
    ({"action": "wait", "seconds": 0}, "seconds must be an integer 1..3600"),
    ({"action": "wait", "seconds": 3601}, "seconds must be an integer 1..3600"),
    ({"action": "wait", "seconds": "5"}, "seconds must be an integer 1..3600"),
    ({"action": "wait", "seconds": 1.5}, "seconds must be an integer 1..3600"),
    ({"action": "wait", "seconds": 5, "device": "towel"}, "a wait step has only seconds"),
    ({"action": "wait", "seconds": 5, "pause_after_s": 3}, "a wait step has only seconds"),
    ({"action": "wait", "seconds": 5, "url": "http://x/"}, "a wait step has only seconds"),
    ({"device": "towel", "action": "on", "seconds": 5}, "seconds is for a wait step"),
])
def test_invalid_wait_steps_are_rejected(dev, guard, step, words):
    r = put(dev, guard + [http_scenario(TEST + "wv", [step])])
    assert r.status_code == 400 and words in r.json()["error"] and "steps[0]" in r.json()["error"], r.text


def test_a_wait_step_round_trips(dev, guard):
    assert put(dev, guard + [http_scenario(TEST + "wrt", [wait(90), wait(5, enabled=False)])]).status_code == 200
    assert scn(dev, TEST + "wrt")["steps"] == [{"action": "wait", "seconds": 90, "enabled": True},
                                               {"action": "wait", "seconds": 5, "enabled": False}]
    assert dev.get("/api/scenarios").json()["limits"]["max_wait_s"] == 3600


def test_a_wait_comes_after_the_pause_before_it_and_waits_also_after_a_failure(dev, guard, fake):
    name = TEST + "wrun"
    steps = [hit(fake, "/ok?w=1", pause_after_s=3), wait(5), hit(fake, "/status/500"), wait(4), hit(fake, "/ok?w=2")]
    assert put(dev, guard + [http_scenario(name, steps)]).status_code == 200
    since = len(fake.requests)
    assert dev.request("POST", "/api/scenarios/run", params={"name": name}).status_code == 200
    s = wait_state(dev, name, ("done",), timeout=40)
    t1, = times(fake, since, "/ok?w=1")
    fails = times(fake, since, "/status/500")
    t2, = times(fake, since, "/ok?w=2")
    assert len(fails) == 3
    assert 7 <= fails[0] - t1 <= 10, fails[0] - t1  # the 3 s pause of step 1, then the 5 s wait
    assert 3.5 <= t2 - fails[-1] <= 6, t2 - fails[-1]  # after the failed step: no pause, but the 4 s wait
    assert [x["state"] for x in s["run_steps"]] == ["ok", "ok", "failed", "ok", "ok"], s["run_steps"]
    assert s["run_steps"][1] == {"wait_s": 5, "state": "ok", "attempts": 1, "detail": "waited 5 s"}
    time.sleep(1.5)
    rec = dev.get("/api/scenarios/history", params={"name": name, "limit": 1}).json()["runs"][0]
    assert [(x["action"], x.get("seconds")) for x in rec["steps"]][1] == ("wait", 5)


def test_a_stop_ends_a_wait_at_once_and_the_next_step_never_runs(dev, guard, fake):
    name = TEST + "wstop"
    assert put(dev, guard + [http_scenario(name, [hit(fake, "/ok?ws=1"), wait(600), hit(fake, "/ok?ws=2")])]).status_code == 200
    since = len(fake.requests)
    assert dev.request("POST", "/api/scenarios/run", params={"name": name}).status_code == 200
    end = time.time() + 15
    while time.time() < end and not scn(dev, name, state=1).get("pause_left_s"):
        time.sleep(0.5)
    s = scn(dev, name, state=1)
    assert s["state"] == "running" and 590 <= s["pause_left_s"] <= 601 and s["run_steps"][1]["state"] == "running", s
    assert dev.request("POST", "/api/scenarios/stop", params={"name": name}).status_code == 200
    s = wait_state(dev, name, ("stopped",), timeout=5)
    assert [x["state"] for x in s["run_steps"]] == ["ok", "stopped", "pending"], s["run_steps"]
    time.sleep(2)
    assert times(fake, since, "/ok?ws=2") == []


@pytest.mark.restart
def test_a_run_continued_in_the_middle_of_a_wait_waits_it_again_in_full(dev, guard, fake):
    name = TEST + "wres"
    steps = [hit(fake, "/ok?wr=1"), wait(45), hit(fake, "/ok?wr=2")]
    assert put(dev, guard + [http_scenario(name, steps)]).status_code == 200  # on_interrupt: software = resume (the default)
    since = len(fake.requests)
    assert dev.request("POST", "/api/scenarios/run", params={"name": name}).status_code == 200
    time.sleep(20)  # 20 s into the 45 s wait; /runs.json was written when it began
    reboot(dev)
    back = time.time()
    s = scn(dev, name, state=1)
    assert s["state"] == "running" and s["resumed"] == "SW at step 2", s
    assert s["pause_left_s"] > 35, s  # in full again (45 s), not the ~20 s that were left
    assert s["run_steps"][1]["state"] == "running"
    s = wait_state(dev, name, ("done",), timeout=70)
    t2, = times(fake, since, "/ok?wr=2")
    assert t2 - back >= 38, t2 - back
