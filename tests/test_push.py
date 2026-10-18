"""monitor:"push": a device that reports its changes itself over the kept connection, against the fake Tuya device on this
computer (no real socket is touched). The board reads the pushes within 0.5 s (heartbeat task), merges them into the cached
state, writes the cached state into the history every polling cycle, and queries the device only every push_control_s
(300 s), when nothing came for push_quiet_s (120 s), or when the connection is gone. The board must reach this computer on
TCP port 6668; otherwise these tests skip.
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


def wait_for(cond, timeout, what, step=0.5):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return
        time.sleep(step)
    pytest.fail(what)


@pytest.fixture(autouse=True)
def pushing(dev, fake):
    """Every test starts with the fake in push mode, its connection open and the opening query done."""
    fake.start()
    fake.idle_timeout = 30.0
    fake.answer_heartbeat = True
    fake.set(d1=True, d19=480)
    edit(dev, monitor="push", keep_connected=False, ignore=False)
    wait_for(lambda: fake.open_connections == 1, interval(dev) + 10, "the board did not open the connection of the push device")
    wait_for(lambda: device(dev)["state"].get("online") is True, interval(dev) + 10, "the push device is not online")
    yield
    fake.idle_timeout = 10.0
    fake.answer_heartbeat = True
    fake.start()
    edit(dev, monitor="poll", keep_connected=False, ignore=False)
    # the next test starts with a new connection and its opening query
    wait_for(lambda: fake.open_connections == 0, interval(dev) + 10, "the push connection was not closed after the test")


def test_limits_name_the_push_timings(dev):
    lim = limits(dev)
    assert lim["push_control_s"] == 300 and lim["push_quiet_s"] == 120


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


def test_while_pushes_come_it_is_not_polled_and_the_history_gets_the_pushed_readings(dev, fake):
    step = interval(dev)
    q0, c0, h0 = fake.queries, fake.connections, fake.heartbeats
    end = time.time() + step * 2.5
    power = 1000
    while time.time() < end:
        power += 10
        fake.push(d19=power)
        time.sleep(5)
    assert fake.queries == q0, "a push device with a live connection was polled"
    assert fake.connections == c0
    assert fake.heartbeats - h0 >= 3, "no heartbeats on the push connection"
    samples = history(dev, 3)
    assert len(samples) == 3 and all(s["ok"] for s in samples)
    assert samples[-1]["power"] > 1000 and samples[-1]["on"] is True, samples
    assert samples[-1]["power"] != samples[0]["power"], "the history did not follow the pushes"
    assert dev.get("/api/status").json()["tuya"]["heartbeat_stack_free_min"] > 512


def test_when_nothing_is_pushed_for_push_quiet_s_the_device_is_queried(dev, fake):
    quiet, step = limits(dev)["push_quiet_s"], interval(dev)
    fake.push(d19=700)
    q0, c0, t0 = fake.queries, fake.connections, time.time()
    wait_for(lambda: fake.queries > q0, quiet + step + 15, "no status query after the device went quiet", step=1)
    waited = time.time() - t0
    assert waited >= quiet - 5, f"queried after {waited:.0f} s, before push_quiet_s"
    assert fake.connections == c0 and fake.open_connections == 1  # over the same connection
    assert device(dev)["state"]["online"] is True


def test_a_lost_connection_is_opened_again_by_a_query(dev, fake):
    fake.answer_heartbeat = False  # the board closes the connection when the heartbeat goes unanswered
    c0, q0 = fake.connections, fake.queries
    wait_for(lambda: fake.connections > c0, interval(dev) + 15, "the board did not connect again after the silent heartbeat")
    fake.answer_heartbeat = True
    wait_for(lambda: fake.queries > q0, 10, "no status query on the new connection")
    st = device(dev)["state"]
    assert st["online"] is True and st["fails"] == 0, st  # a closed idle connection is not a failed poll


def test_a_device_that_stops_answering_goes_offline_and_comes_back(dev, fake):
    step = interval(dev)
    fake.stop()
    wait_for(lambda: device(dev)["state"].get("online") is False, step * 3 + 20, "the stopped push device did not go offline", step=1)
    samples = history(dev, 2)
    assert samples[-1]["ok"] is False, samples  # the failed queries are in the history, no cached samples while offline
    fake.start()
    wait_for(lambda: device(dev)["state"].get("online") is True, step * 2 + 20, "the push device did not come back", step=1)
    wait_for(lambda: fake.open_connections == 1, 10, "no connection after it came back")


def test_ignore_closes_the_push_connection(dev, fake):
    edit(dev, ignore=True)
    wait_for(lambda: fake.open_connections == 0, interval(dev) + 10, "the connection of an ignored push device stayed open")
    assert fake.push(d1=False) == 0


def test_back_to_poll_closes_the_connection_and_each_cycle_queries(dev, fake):
    edit(dev, monitor="poll")
    wait_for(lambda: fake.open_connections == 0, interval(dev) + 10, "the push connection was not closed in poll mode")
    q0 = fake.queries
    time.sleep(interval(dev) * 2.2)
    assert fake.queries - q0 >= 2  # polled every cycle again
    assert device(dev)["monitor"] == "poll"
