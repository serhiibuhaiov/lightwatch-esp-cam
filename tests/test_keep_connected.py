"""keep_connected: one connection kept open for every operation of a device, with a heartbeat while it is idle, against
the fake Tuya device on this computer (no real socket is touched). The heartbeat task sends a heartbeat on a connection idle for
10 s and closes it when the answer does not come in 2 s; it never connects itself. The board must reach this computer on TCP port 6668;
otherwise these tests skip.
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


def state(dev):
    return next(d for d in dev.get("/api/devices").json()["devices"] if d["name"] == FAKE_NAME)


def interval(dev):
    return dev.get("/api/devices").json()["poll_interval_s"]


def wait_for(cond, timeout, what):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return
        time.sleep(0.5)
    pytest.fail(what)


@pytest.fixture(autouse=True)
def kept(dev, fake):
    """Every test starts with the fake kept connected, as a real device would treat an idle connection (30 s)."""
    fake.start()
    fake.idle_timeout = 30.0
    fake.answer_heartbeat = True
    edit(dev, keep_connected=True, ignore=False)
    wait_for(lambda: fake.open_connections == 1, interval(dev) + 10, "the board did not open the kept connection")
    yield
    fake.idle_timeout = 10.0
    fake.answer_heartbeat = True
    edit(dev, keep_connected=False, ignore=False)


def test_a_kept_device_is_polled_over_one_connection_with_heartbeats_between(dev, fake):
    c0, q0, h0 = fake.connections, fake.queries, fake.heartbeats
    time.sleep(interval(dev) * 2.5)
    assert fake.connections == c0, "the board connected again"
    assert fake.open_connections == 1
    assert fake.queries - q0 >= 2, "the monitor did not poll over the kept connection"
    assert fake.heartbeats - h0 >= 2, "too few heartbeats while the connection was idle (one per 10 s of idling, polls every 15 s)"
    d = state(dev)
    assert d["keep_connected"] is True and d["state"]["online"] is True and d["state"]["fails"] == 0
    assert d["state"]["connections"] >= 1


def test_commands_and_manual_queries_use_the_kept_connection(dev, fake):
    c0, n0 = fake.connections, len(fake.commands)
    for op, want in (("off", False), ("on", True)):
        r = dev.request("POST", f"/api/devices/{op}", params={"name": FAKE_NAME}, timeout=30)
        assert r.status_code == 200 and r.json()["ok"], r.text
        assert fake.get(1) is want
    assert dev.request("POST", "/api/devices/status", params={"name": FAKE_NAME}, timeout=30).json()["ok"]
    assert len(fake.commands) == n0 + 2
    assert fake.connections == c0 and fake.open_connections == 1


def test_a_connection_the_device_closes_is_opened_again_without_a_failure(dev, fake):
    fake.idle_timeout = 3.0  # the fake now drops the connection 3 s after each request, before any heartbeat
    c0, q0 = fake.connections, fake.queries
    time.sleep(interval(dev) * 3.5)  # the open connection still waits with the old timeout until its next request
    assert fake.connections - c0 >= 2, "the board did not connect again after the device closed the connection"
    assert fake.queries - q0 >= 2
    d = state(dev)["state"]
    assert d["online"] is True and d["fails"] == 0, d  # a dropped idle connection is not a failed poll
    log = dev.get("/api/logs", params={"file": "current", "tail": 8192}).text
    assert f"[{FAKE_NAME}] the device closed the kept connection" in log


def test_a_command_over_a_dead_connection_is_sent_again_on_a_new_one(dev, fake):
    fake.mute_open_connections()  # like a socket that restarted: the old connection takes what is sent, never answers
    c0, n0 = fake.connections, len(fake.commands)
    t0 = time.time()
    r = dev.request("POST", "/api/devices/off", params={"name": FAKE_NAME}, timeout=40)
    took = time.time() - t0
    assert r.status_code == 200 and r.json()["ok"], r.text
    assert fake.get(1) is False and fake.connections == c0 + 1
    assert took >= 7, f"took {took:.1f} s: the dead connection did not time out first"
    assert any(c == {"1": False} for _, c in fake.commands[n0:])
    log = dev.get("/api/logs", params={"file": "current", "tail": 8192}).text
    assert f"[{FAKE_NAME}] no answer over the kept connection, connecting again" in log
    r = dev.request("POST", "/api/devices/on", params={"name": FAKE_NAME}, timeout=30)
    assert r.json()["ok"] and fake.connections == c0 + 1  # the new connection is kept and used


def test_a_poll_over_a_dead_connection_is_not_tried_a_third_time(dev, fake):
    # The session already tries once more on a new connection; another attempt of the monitor would hold the single monitor
    # for a third connect + timeout while the other devices wait. Muted right after a heartbeat, so that the poll (5 s later:
    # polls every 15 s, heartbeats after 10 s idle) and not the next heartbeat is what finds the connection dead.
    h0 = fake.heartbeats
    wait_for(lambda: fake.heartbeats > h0, interval(dev) + 12, "no heartbeat on the kept connection")
    fake.silent = True             # new connections are accepted and never answered
    fake.mute_open_connections()   # the kept one takes what is sent and never answers
    try:
        c0 = fake.connections
        wait_for(lambda: state(dev)["state"]["fails"] >= 1, interval(dev) + 20, "the poll over the dead connection did not fail")
        assert fake.connections == c0 + 1, f"{fake.connections - c0} new connections for one failed poll"
    finally:
        fake.silent = False


def test_without_keep_connected_the_kept_connection_is_closed_and_each_poll_connects(dev, fake):
    edit(dev, keep_connected=False)
    wait_for(lambda: fake.open_connections == 0, interval(dev) + 10, "the kept connection was not closed")
    c0 = fake.connections
    time.sleep(interval(dev) * 2.2)
    assert fake.connections - c0 >= 2  # a new connection for every poll, as before
    assert state(dev)["state"]["online"] is True


def test_an_ignored_kept_device_has_its_connection_closed(dev, fake):
    edit(dev, ignore=True)
    wait_for(lambda: fake.open_connections == 0, interval(dev) + 10, "the kept connection of an ignored device stayed open")
    c0 = fake.connections
    time.sleep(interval(dev) * 1.5)
    assert fake.connections == c0 and fake.open_connections == 0  # not polled, nothing kept
    # a command still switches it, over a connection of its own that is not kept
    for action in ("off", "on"):
        r = dev.request("POST", f"/api/devices/{action}", params={"name": FAKE_NAME}, timeout=30)
        assert r.status_code == 200 and r.json()["ok"], r.text
    assert fake.connections == c0 + 2 and fake.get(1) is True
    wait_for(lambda: fake.open_connections == 0, 5, "a command left a kept connection to an ignored device")


def test_a_connection_that_does_not_answer_the_heartbeat_is_closed_and_the_next_poll_connects(dev, fake):
    fake.answer_heartbeat = False
    c0 = fake.connections
    wait_for(lambda: fake.connections > c0, interval(dev) + 15, "the board did not connect again after the silent heartbeat")
    d = state(dev)["state"]
    assert d["online"] is True and d["fails"] == 0, d  # a closed idle connection is not a failed poll
    log = dev.get("/api/logs", params={"file": "current", "tail": 8192}).text
    assert f"[{FAKE_NAME}] no answer to the heartbeat in 2000 ms" in log


def test_polls_of_a_kept_device_do_not_end_busy_because_of_the_heartbeat(dev, fake):
    busy = lambda: dev.get("/api/status").json()["tuya"]["monitor"]["busy"]
    b0, q0 = busy(), fake.queries
    time.sleep(interval(dev) * 3.2)
    assert fake.queries - q0 >= 3
    assert busy() == b0, "a poll ended busy: it did not wait for the heartbeat"
