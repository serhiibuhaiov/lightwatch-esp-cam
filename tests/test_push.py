"""poll_s and status pushes: a device queried only once every poll_s, against the fake Tuya device on this computer (no real
socket is touched). Whatever a device sends on its own over its kept connection is read within 0.5 s (heartbeat task) and
merged into the cached state; between two queries the history gets that cached state every polling cycle while the kept
connection lives, and nothing without one. A lost kept connection is opened again by a query in the next cycle. The board
must reach this computer on TCP port 6668; otherwise these tests skip.
"""
import time

import pytest

from conftest import FAKE_NAME, device_list, fake_registered, put_devices


@pytest.fixture(scope="module")
def fake(dev):
    with fake_registered(dev) as f:
        yield f


def edit(dev, **fields):
    devices = device_list(dev)
    for d in devices:
        if d["name"] == FAKE_NAME:
            d.update(fields)
    r = put_devices(dev, devices)
    assert r.status_code == 200, r.text


def device(dev):
    return next(d for d in dev.get("/api/devices").json()["devices"] if d["name"] == FAKE_NAME)


def interval(dev):
    return dev.get("/api/devices").json()["poll_interval_s"]


def limits(dev):
    return dev.get("/api/status").json()["limits"]["devices"]


def history(dev, limit=10):
    r = dev.get("/api/devices/history", params={"name": FAKE_NAME, "limit": limit})
    assert r.status_code == 200, r.text
    return r.json()["samples"]


# Waits for something the monitor does once per cycle are two intervals long: offline devices (1.5 s per connect attempt)
# shift the moment a cycle gets to a device by several seconds.
def wait_for(cond, timeout, what, step=0.5):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return
        time.sleep(step)
    pytest.fail(what)


@pytest.fixture(autouse=True)
def pushing(dev, fake):
    """Every test starts with the fake kept connected and queried every 300 s, its connection open and the opening query done."""
    fake.start()
    fake.idle_timeout = 30.0
    fake.answer_heartbeat = True
    fake.set(d1=True, d19=480)
    edit(dev, keep_connected=True, poll_s=300, ignore=False)
    wait_for(lambda: fake.open_connections == 1, interval(dev) * 2 + 10, "the board did not open the kept connection")
    wait_for(lambda: device(dev)["state"].get("online") is True, interval(dev) * 2 + 10, "the device is not online")
    yield
    fake.idle_timeout = 10.0
    fake.answer_heartbeat = True
    fake.start()
    edit(dev, keep_connected=False, poll_s=None, ignore=False)
    # the next test starts with a new connection and its opening query
    wait_for(lambda: fake.open_connections == 0, interval(dev) * 2 + 10, "the kept connection was not closed after the test")


def test_limits_name_the_largest_poll_s(dev):
    lim = limits(dev)
    assert lim["poll_s_max"] == 3600 and "push_control_s" not in lim and "push_quiet_s" not in lim
    assert device(dev)["poll_s"] == 300 and "monitor" not in device(dev)


def test_a_pushed_switch_change_is_in_the_state_within_a_second_without_a_query(dev, fake):
    q0, c0 = fake.queries, fake.connections
    p0 = device(dev)["state"]["pushes"]
    for want in (False, True):
        t0 = time.time()
        assert fake.push(d1=want) == 1
        wait_for(lambda: device(dev)["state"].get("on") is want, 5, f"the pushed switch {want} did not reach the state", step=0.1)
        assert time.time() - t0 < 1.5, "the push took longer than the 0.5 s wake of the heartbeat task (+ HTTP)"
    st = device(dev)["state"]
    assert st["pushes"] - p0 >= 2 and st["push_age_s"] <= 2
    assert st["dps"]["19"] == 480  # a push carries only what changed: the rest of the state stays
    assert fake.queries == q0 and fake.connections == c0, "the board queried or connected for a push"
    log = dev.get("/api/logs", params={"file": "current", "tail": 8192}).text
    assert f"[{FAKE_NAME}] switch (dp 1) is now OFF (pushed)" in log


def test_between_queries_the_history_gets_the_pushed_readings(dev, fake):
    step = interval(dev)
    q0, c0, h0 = fake.queries, fake.connections, fake.heartbeats
    end = time.time() + step * 2.5
    power = 1000
    while time.time() < end:
        power += 10
        fake.push(d19=power)
        time.sleep(5)
    assert fake.queries == q0, "queried before poll_s"
    assert fake.connections == c0
    assert fake.heartbeats - h0 >= 3, "no heartbeats on the kept connection"
    samples = history(dev, 3)
    assert len(samples) == 3 and all(s["ok"] for s in samples)
    assert samples[-1]["power"] > 1000 and samples[-1]["on"] is True, samples
    assert samples[-1]["power"] != samples[0]["power"], "the history did not follow the pushes"
    assert dev.get("/api/status").json()["tuya"]["heartbeat_stack_free_min"] > 512


def test_a_device_is_queried_once_every_poll_s_whatever_it_pushes(dev, fake):
    step, every = interval(dev), 60
    edit(dev, poll_s=every)
    times, q = [], fake.queries
    end = time.time() + every * 2 + step * 2 + 15
    while len(times) < 2 and time.time() < end:
        fake.push(d19=600 + len(times))  # pushes do not put the query off (there is no "quiet" watchdog either)
        time.sleep(1)
        if fake.queries > q:
            q = fake.queries
            times.append(time.time())
    assert len(times) == 2, f"queries at {times}"
    gap = times[1] - times[0]
    assert every - step / 2 - 3 <= gap <= every + step + 5, f"{gap:.0f} s between two queries, poll_s {every}"
    assert fake.open_connections == 1  # over the same connection


def test_without_poll_s_it_is_queried_every_cycle_and_pushes_still_count(dev, fake):
    edit(dev, poll_s=None)
    q0, c0 = fake.queries, fake.connections
    time.sleep(interval(dev) * 2.2)
    assert fake.queries - q0 >= 2  # every cycle
    assert fake.connections == c0 and fake.open_connections == 1  # over the kept connection
    assert fake.push(d1=False) == 1
    wait_for(lambda: device(dev)["state"].get("on") is False, 5, "the pushed switch did not reach the state", step=0.1)
    assert device(dev)["poll_s"] is None


def test_without_a_kept_connection_there_are_no_cached_samples_between_queries(dev, fake):
    step = interval(dev)
    edit(dev, keep_connected=False, poll_s=60)
    wait_for(lambda: fake.open_connections == 0, step * 2 + 10, "the kept connection was not closed")
    t0 = time.time()
    time.sleep(step * 2.5)  # two and a half cycles: at most one query (every 60 s), no sample of the cache
    since = time.time() - t0
    fresh = [s for s in history(dev, 10) if s["age_s"] < since]
    assert len(fresh) <= 1, f"{len(fresh)} samples in {since:.0f} s without a kept connection: {fresh}"


def test_a_lost_connection_is_opened_again_by_a_query(dev, fake):
    fake.answer_heartbeat = False  # the board closes the connection when the heartbeat goes unanswered
    c0, q0 = fake.connections, fake.queries
    wait_for(lambda: fake.connections > c0, interval(dev) * 2 + 15, "the board did not connect again after the silent heartbeat")
    fake.answer_heartbeat = True
    wait_for(lambda: fake.queries > q0, 10, "no status query on the new connection")
    st = device(dev)["state"]
    assert st["online"] is True and st["fails"] == 0, st  # a closed idle connection is not a failed poll


def test_a_device_that_stops_answering_goes_offline_and_comes_back(dev, fake):
    step = interval(dev)
    fake.stop()
    wait_for(lambda: device(dev)["state"].get("online") is False, step * 3 + 20, "the stopped device did not go offline", step=1)
    samples = history(dev, 2)
    assert samples[-1]["ok"] is False, samples  # the failed queries are in the history, no cached samples while offline
    fake.start()
    wait_for(lambda: device(dev)["state"].get("online") is True, step * 2 + 20, "the device did not come back", step=1)
    wait_for(lambda: fake.open_connections == 1, 10, "no connection after it came back")


def test_ignore_closes_the_kept_connection(dev, fake):
    edit(dev, ignore=True)
    wait_for(lambda: fake.open_connections == 0, interval(dev) * 2 + 10, "the kept connection of an ignored device stayed open")
    assert fake.push(d1=False) == 0
