"""Hours of a step and of a scenario's trigger, and the trigger "cron", against the fake Tuya device on this computer.

The windows are put around the board's own local time (config time.tz, worked out here with the same POSIX TZ string): one that
holds "now" with an hour to spare each side, one that starts two hours from now. No real socket is touched. The board must have
its clock (NTP); without it these tests are skipped. The cron tests wait for the next minute (up to ~70 s each).
"""
import contextlib
import os
import time

import pytest

from conftest import FAKE_NAME, fake_registered
from test_relay_trigger import last_id, query, relay, runs, switch, wait_until, watching
from test_scenarios import TEST, guard, listing, put, wait_state  # noqa: F401


@contextlib.contextmanager
def zone(tz):
    old = os.environ.get("TZ")
    os.environ["TZ"] = tz
    time.tzset()
    try:
        yield
    finally:
        if old is None:
            os.environ.pop("TZ", None)
        else:
            os.environ["TZ"] = old
        time.tzset()


def board_minute(dev):
    """The board's local time of day in minutes, from its time zone and this computer's clock (both on NTP)."""
    status = dev.get("/api/status").json()
    if not status.get("time_valid"):
        pytest.skip("the board has no clock yet (NTP)")
    with zone(dev.config(active=True)["time"]["tz"]):
        t = time.localtime()
    return t.tm_hour * 60 + t.tm_min


def hhmm(minute):
    minute %= 1440
    return f"{minute // 60:02d}:{minute % 60:02d}"


def inside(dev, **kw):
    m = board_minute(dev)
    return dict({"from": hhmm(m - 60), "to": hhmm(m + 60)}, **kw)


def outside(dev, **kw):
    m = board_minute(dev)
    return dict({"from": hhmm(m + 120), "to": hhmm(m + 180)}, **kw)


def scn(dev, name):
    return next(s for s in listing(dev)["scenarios"] if s["name"] == name)


def manual(name, steps, **kw):
    d = {"name": name, "enabled": False, "trigger": "manual", "default_pause_s": 0, "steps": steps}
    d.update(kw)
    return d


def run(dev, name, timeout=40):
    assert dev.request("POST", "/api/scenarios/run", params={"name": name}).status_code == 200
    return wait_state(dev, name, ("done", "stopped"), timeout=timeout)


@pytest.fixture(scope="module")
def fake(dev):
    with fake_registered(dev) as f:
        yield f


@pytest.fixture(autouse=True)
def calm(dev, fake):
    fake.start()
    fake.silent = False
    fake.ignore_control = False
    fake.set(d1=True)
    assert query(dev)["ok"]
    yield
    fake.start()


# ---- round trip ------------------------------------------------------------------------------------------------------

def test_hours_cron_and_attempts_round_trip(dev, guard):
    h = {"from": "22:00", "to": "06:00", "clock_wait_min": 0}
    docs = [
        {"name": TEST + "rt-cron", "enabled": False, "trigger": "cron", "when": {"cron": "*/15 6-22 * * MON-FRI"}, "hours": h, "default_pause_s": 0,
         "steps": [{"device": FAKE_NAME, "action": "on", "hours": {"from": "05:00", "to": "21:00"}},
                   {"action": "http", "url": "http://10.0.0.5/x", "attempts": 4, "retry_gap_s": 30}]},
    ]
    assert put(dev, guard + docs).status_code == 200
    s = scn(dev, TEST + "rt-cron")
    assert s["when"] == {"cron": "*/15 6-22 * * MON-FRI"} and s["hours"] == h, s
    assert s["hours_now"] in ("inside", "outside"), s
    assert s["steps"][0]["hours"] == {"from": "05:00", "to": "21:00", "clock_wait_min": 10}, s["steps"]
    assert s["steps"][1]["attempts"] == 4 and s["steps"][1]["retry_gap_s"] == 30, s["steps"]
    assert "hours" not in s["steps"][1] and "attempts" not in s["steps"][0]
    limits = listing(dev)["limits"]
    assert limits["cron"]["max_bytes"] == 100 and limits["hours"]["max_clock_wait_min"] == 1440
    assert limits["http_step"]["max_attempts"] == 20 and limits["http_step"]["max_retry_gap_s"] == 600


# ---- hours of a step -------------------------------------------------------------------------------------------------

def test_a_step_outside_its_hours_is_skipped_and_one_inside_goes(dev, guard, fake):
    name = TEST + "hstep"
    steps = [{"device": FAKE_NAME, "action": "off", "hours": outside(dev), "pause_after_s": 600},
             {"device": FAKE_NAME, "action": "off", "hours": inside(dev)}]
    assert put(dev, guard + [manual(name, steps)]).status_code == 200
    after = last_id(dev, name)  # the history keeps runs of this name from earlier sessions
    before = len(fake.commands)
    s = run(dev, name)
    first, second = s["run_steps"]
    assert first["state"] == "skipped" and first["attempts"] == 0 and first["detail"].startswith("outside the hours "), first
    assert second["state"] == "ok", second  # right after: no 600 s pause after the skipped step
    assert [c for _, c in fake.commands[before:]] == [{"1": False}]  # the second step only
    assert wait_until(lambda: runs(dev, name, after), 10)
    hist = runs(dev, name, after)[0]
    assert hist["skipped"] == 1 and hist["steps"][0]["state"] == "skipped", hist


# ---- hours of a trigger ----------------------------------------------------------------------------------------------

def test_a_relay_trigger_outside_its_hours_starts_nothing_inside_it_does(dev, guard, fake):
    name = TEST + "hrelay"
    doc = relay(name, to="off")
    assert put(dev, guard + [dict(doc, hours=outside(dev))]).status_code == 200
    watching(dev, name)
    assert scn(dev, name)["hours_now"] == "outside"
    after = last_id(dev, name)
    switch(dev, fake, False)
    time.sleep(8)
    assert runs(dev, name, after) == [] and scn(dev, name)["state"] == "idle"
    log = dev.get("/api/logs", params={"file": "current", "tail": 8192}).text
    assert f"[{name}] {FAKE_NAME}: the switch went OFF outside the hours" in log
    switch(dev, fake, True)
    assert put(dev, guard + [dict(doc, hours=inside(dev))]).status_code == 200
    watching(dev, name)
    assert scn(dev, name)["hours_now"] == "inside"
    switch(dev, fake, False)
    assert wait_until(lambda: runs(dev, name, after), 15), scn(dev, name)
    assert runs(dev, name, after)[0]["trigger"] == "relay-off"


def test_an_interval_tick_outside_its_hours_is_skipped(dev, guard):
    name = TEST + "hiv"
    doc = {"name": name, "enabled": True, "trigger": "interval", "when": {"every_s": 10}, "hours": outside(dev), "default_pause_s": 0,
           "steps": [{"action": "wait", "seconds": 1}]}
    assert put(dev, guard + [doc]).status_code == 200
    assert wait_until(lambda: scn(dev, name).get("interval", {}).get("skipped_hours", 0) >= 2, 40), scn(dev, name)
    iv = scn(dev, name)["interval"]
    assert iv["ok"] == 0 and iv["failed"] == 0, iv


def test_a_manual_start_ignores_the_scenarios_hours(dev, guard):
    name = TEST + "hman"
    doc = {"name": name, "enabled": False, "trigger": "power_on", "hours": outside(dev), "default_pause_s": 0,
           "steps": [{"action": "wait", "seconds": 1}]}
    assert put(dev, guard + [doc]).status_code == 200
    s = run(dev, name)
    assert s["state"] == "done" and s["run_trigger"] == "manual", s


# ---- cron ------------------------------------------------------------------------------------------------------------

def cron(name, expr="* * * * *", **kw):
    d = {"name": name, "enabled": True, "trigger": "cron", "when": {"cron": expr}, "default_pause_s": 0,
         "steps": [{"action": "wait", "seconds": 1}]}
    d.update(kw)
    return d


def test_cron_starts_the_scenario_at_the_minute(dev, guard):
    name = TEST + "cron"
    after = last_id(dev, name)  # the history keeps runs of this name from earlier sessions
    assert put(dev, guard + [cron(name)]).status_code == 200
    assert wait_until(lambda: scn(dev, name).get("watch", {}).get("next"), 10), scn(dev, name)
    w = scn(dev, name)["watch"]
    assert w["reason"] == "watching" and len(w["next"]) == 16 and w["next"][10] == "T", w
    assert wait_until(lambda: runs(dev, name, after), 75, step=2), scn(dev, name)
    hist = runs(dev, name, after)[0]
    assert hist["trigger"] == "cron" and hist["result"] == "done", hist
    log = dev.get("/api/logs", params={"file": "current", "tail": 16384}).text
    assert f"[{name}] cron '* * * * *' at " in log


def test_cron_outside_its_hours_starts_nothing(dev, guard):
    name = TEST + "cronh"
    after = last_id(dev, name)
    assert put(dev, guard + [cron(name, hours=outside(dev))]).status_code == 200
    time.sleep(70)
    assert runs(dev, name, after) == [], runs(dev, name, after)
    log = dev.get("/api/logs", params={"file": "current", "tail": 16384}).text
    assert f"[{name}] cron '* * * * *' at " in log and "outside the hours" in log
