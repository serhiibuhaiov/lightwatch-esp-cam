"""The trigger "interval": a scenario that starts every every_s seconds (a healthcheck ping), against the fake HTTP server on
this computer (test_http.py). Validation and the round trip; the fixed rate; the counters; the quiet log (one line when it
starts failing, one when it works again); its own history (short lines, the newest still in RAM, failed=1) apart from the
history of the other scenarios; a tick while the run still goes is skipped; a disabled one never runs. -m restart: its run
is not carried over a restart.
"""
import json
import time

import pytest

from test_http import Fake, fake, http_scenario, scn  # noqa: F401
from test_interrupt import reboot
from test_scenarios import TEST, guard, put  # noqa: F401


def interval(name, every_s, steps, **kw):
    return http_scenario(name, steps, trigger="interval", when={"every_s": every_s}, enabled=kw.pop("enabled", True), **kw)


def ping(fake, path="/ok", **kw):
    return dict({"action": "http", "url": fake.base + path}, **kw)


def hits(fake, since, path):
    return [g for g in fake.requests[since:] if g["path"] == path]


def wait_for(pred, timeout, step=0.5):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(step)
    return pred()


def history(dev, name, **params):
    r = dev.get("/api/scenarios/history", params=dict(name=name, **params))
    assert r.status_code == 200, r.text
    return r.json()


def log_tail(dev):
    return dev.get("/api/logs", params={"file": "current", "tail": 32768}).text


# ---- validation ------------------------------------------------------------------------------------------------------------

@pytest.mark.parametrize("when", [None, {}, {"every_s": 9}, {"every_s": 86401}, {"every_s": "60"}, {"every_s": 1.5}, [60]])
def test_an_interval_needs_every_s_10_to_86400(dev, guard, when):
    doc = http_scenario(TEST + "iv", [], trigger="interval")
    if when is not None:
        doc["when"] = when
    r = put(dev, guard + [doc])
    assert r.status_code == 400 and "every_s" in r.json()["error"], r.text


def test_an_interval_round_trips_and_shows_its_limits(dev, guard):
    assert put(dev, guard + [interval(TEST + "ivrt", 3600, [{"action": "http", "url": "https://hc-ping.com/x"}], enabled=False)]).status_code == 200
    s = scn(dev, TEST + "ivrt")
    assert s["trigger"] == "interval" and s["when"] == {"every_s": 3600} and s["enabled"] is False
    lim = dev.get("/api/scenarios").json()["limits"]["interval"]
    assert lim == {"min_every_s": 10, "max_every_s": 86400, "history_flush_s": 300, "history_file_bytes": 131072}


# ---- running ---------------------------------------------------------------------------------------------------------------

def test_it_runs_on_a_fixed_rate_quietly_and_keeps_a_history_of_its_own(dev, guard, fake):
    name = TEST + "ivok"
    path = "/ok?iv=" + name
    since = len(fake.requests)
    http_lines = log_tail(dev).count(f"http: GET {fake.base.split('//')[1]}:")
    assert put(dev, guard + [interval(name, 10, [ping(fake, path)])]).status_code == 200
    assert wait_for(lambda: scn(dev, name, state=1)["interval"]["ok"] >= 4, 50)  # ended runs, not just requests that arrived
    at = [g["at"] for g in hits(fake, since, path)]
    gaps = [b - a for a, b in zip(at, at[1:])]
    # A fixed rate of 10 s (looked at about once a second). A request may wait in the HTTP lane behind another one (a real
    # healthcheck on the board takes ~3 s), which moves one arrival but not the schedule: the average stays at 10 s.
    assert all(5.5 < g < 14.5 for g in gaps) and 9 < (at[-1] - at[0]) / len(gaps) < 11, gaps
    v = scn(dev, name, state=1)["interval"]
    assert v["ok"] >= 4 and v["failed"] == 0 and v["fails_in_row"] == 0 and 0 <= v["next_in_s"] <= 11, v
    h = history(dev, name, limit=50)
    assert h["interval"] is True and len(h["runs"]) >= 4, h
    line = h["runs"][0]
    assert set(line) == {"id", "name", "t", "ok", "s", "steps"} and line["ok"] is True and line["steps"] == [["ok", 1]], line
    assert [r["id"] for r in h["runs"]] == sorted((r["id"] for r in h["runs"]), reverse=True)
    assert len(json.dumps(line)) < 120
    general = dev.get("/api/scenarios/history", params={"limit": 50}).json()["runs"]
    assert not any(r["name"] == name for r in general)  # not in the history of the other scenarios
    log = log_tail(dev)
    assert f"[{name}] first run since boot: ok" in log
    assert f"[{name}] started" not in log and f"[{name}] finished" not in log  # those are DEBUG lines
    assert log.count(f"http: GET {fake.base.split('//')[1]}:") <= http_lines  # nor a line per request (<=: older lines may leave the 32 KB tail)


def test_failing_runs_are_logged_once_counted_and_filtered_and_then_it_works_again(dev, guard, fake):
    name = TEST + "ivbad"
    failing0 = log_tail(dev).count(f"[{name}] failing:")  # an earlier run of the suite may have left lines in the log
    failed0 = log_tail(dev).count(f"http: GET {fake.base.split('//')[1]}: FAILED")
    again0 = log_tail(dev).count(f"[{name}] works again after")
    assert put(dev, guard + [interval(name, 10, [ping(fake, "/status/500")])]).status_code == 200
    assert wait_for(lambda: (scn(dev, name, state=1).get("interval") or {}).get("fails_in_row", 0) >= 2, 40)
    v = scn(dev, name, state=1)["interval"]
    assert v["ok"] == 0 and v["failed"] >= 2 and v["last_failed"] is not None and "answered 500" in v["last_error"], v
    assert log_tail(dev).count(f"[{name}] failing:") == failing0 + 1  # one line for the whole series
    assert log_tail(dev).count(f"http: GET {fake.base.split('//')[1]}: FAILED") <= failed0  # no WARN per attempt
    failed = history(dev, name, failed=1)["runs"]
    assert len(failed) >= 2 and all(r["ok"] is False for r in failed), failed
    assert failed[0]["steps"][0][:2] == ["failed", 3] and "answered 500" in failed[0]["steps"][0][2], failed[0]
    assert put(dev, guard + [interval(name, 10, [ping(fake, "/ok")])]).status_code == 200  # fixed
    assert wait_for(lambda: scn(dev, name, state=1)["interval"]["fails_in_row"] == 0, 25)
    assert log_tail(dev).count(f"[{name}] works again after") == again0 + 1
    assert scn(dev, name, state=1)["interval"]["ok"] >= 1


def test_a_tick_while_the_run_still_goes_is_skipped(dev, guard, fake):
    name = TEST + "ivbusy"
    steps = [ping(fake, "/ok?busy=1", pause_after_s=25), ping(fake, "/ok?busy=2")]
    assert put(dev, guard + [interval(name, 10, steps)]).status_code == 200
    assert wait_for(lambda: (scn(dev, name, state=1).get("interval") or {}).get("skipped_busy", 0) >= 2, 40)
    s = scn(dev, name, state=1)
    assert s["state"] == "running", s
    dev.request("POST", "/api/scenarios/stop", params={"name": name})


def test_a_disabled_interval_never_runs_but_a_run_by_hand_goes_to_its_history(dev, guard, fake):
    name = TEST + "ivoff"
    path = "/ok?off=" + name
    since = len(fake.requests)
    assert put(dev, guard + [interval(name, 10, [ping(fake, path)], enabled=False)]).status_code == 200
    time.sleep(15)
    assert hits(fake, since, path) == []
    assert scn(dev, name, state=1)["interval"]["next_in_s"] is None
    assert dev.request("POST", "/api/scenarios/run", params={"name": name}).status_code == 200
    assert wait_for(lambda: len(hits(fake, since, path)) == 1, 10)
    assert wait_for(lambda: history(dev, name)["runs"], 10)
    assert history(dev, name)["runs"][0]["trigger"] == "manual"
    assert f"[{name}] start requested" not in log_tail(dev)


# ---- over a restart (-m restart) ---------------------------------------------------------------------------------------------

@pytest.mark.restart
def test_an_interval_run_is_not_carried_over_a_restart(dev, guard, fake):
    name = TEST + "ivrst"
    steps = [ping(fake, "/ok?rst=1", pause_after_s=600), ping(fake, "/ok?rst=2")]
    assert put(dev, guard + [interval(name, 3600, steps)]).status_code == 200
    assert wait_for(lambda: scn(dev, name, state=1).get("pause_left_s") is not None, 25)
    time.sleep(5)
    since = len(fake.requests)
    reboot(dev)
    s = scn(dev, name, state=1)
    assert "resumed" not in s and s["state"] in ("idle", "running", "done"), s
    general = dev.get("/api/scenarios/history", params={"limit": 20}).json()["runs"]
    assert not any(r["name"] == name for r in general)  # no "interrupted" line either
    assert wait_for(lambda: len(hits(fake, since, "/ok?rst=1")) == 1, 40)  # the first tick after the restart (Wi-Fi + 10 s)
    dev.request("POST", "/api/scenarios/stop", params={"name": name})
