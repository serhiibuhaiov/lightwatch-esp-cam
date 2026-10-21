"""Energy since boot, counted two ways side by side (Devices::energy, lib/energy_meter): `integrated` (power x time from every
answer and push) and `reported` (the sum of the socket's energy data point). Against the fake Tuya socket and the fake HTTP
server on this computer (no real socket is touched, nothing leaves the LAN). The sums themselves are host-tested
(tests/native/test_energy_meter); here: the API, the history, what counts as a report, that a switched-off or lost socket
adds nothing, the placeholders, and that the socket's own `energy` reading is shown as before.
"""
import re
import time

import pytest

from conftest import FAKE_NAME, device_list, fake_registered, put_devices
from fake_tuya import lan_ip_towards
from test_http import Fake, send


@pytest.fixture(scope="module")
def sock(dev):
    with fake_registered(dev) as f:
        yield f


@pytest.fixture(scope="module")
def server(dev):
    f = Fake()
    f.base = f"http://{lan_ip_towards(dev.url.split('//')[1].split(':')[0])}:{f.port}"
    try:
        probe = send(dev, {"url": f.base + "/ok", "timeout_ms": 5000}).json()
        if not probe.get("ok"):
            pytest.skip(f"the board cannot reach this computer on port {f.port} (firewall?): {probe}")
        yield f
    finally:
        f.stop()


def query(dev):
    return dev.request("POST", "/api/devices/status", params={"name": FAKE_NAME}, timeout=20).json()


def edit(dev, **fields):
    devices = device_list(dev)
    for d in devices:
        if d["name"] == FAKE_NAME:
            d.update(fields)
    r = put_devices(dev, devices)
    assert r.status_code == 200, r.text


def energy(dev):
    d = next(d for d in dev.get("/api/devices").json()["devices"] if d["name"] == FAKE_NAME)
    return d["energy_since_boot"]


def details(dev, **params):
    r = dev.get("/api/devices/energy", params=dict({"name": FAKE_NAME}, **params))
    assert r.status_code == 200, r.text
    return r.json()


def interval(dev):
    return dev.get("/api/devices").json()["poll_interval_s"]


def wait_until(pred, timeout, step=0.5):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(step)
    return False


@pytest.fixture(autouse=True)
def known(dev, sock):
    """The socket online and on at 1000 W, without an energy reading, and the board knowing it."""
    sock.start()
    with sock.lock:
        sock.dps.pop("17", None)
    sock.set(d1=True, d19=10000, d18=4500, d20=2250)
    assert query(dev)["ok"]
    yield
    sock.start()


def test_the_listing_and_the_limits_name_the_energy_since_boot(dev):
    e = energy(dev)
    assert set(e) == {"integrated_wh", "reported_wh", "since_s", "covered_s", "power_w", "reports", "reports_counted", "last_report"}
    assert e["power_w"] == 1000
    lim = dev.get("/api/status").json()["limits"]["devices"]["energy"]
    assert lim["points"] == 1440 and lim["point_s"] == 60 and lim["reports"] == 128 and isinstance(lim["psram"], bool)
    for d in dev.get("/api/devices").json()["devices"]:  # every device has it, also those never counted
        assert "energy_since_boot" in d, d["name"]


def test_integrated_is_power_times_time(dev):
    e1, t1 = energy(dev), time.time()
    time.sleep(10)
    e2, t2 = energy(dev), time.time()
    expected = 1000 * (t2 - t1) / 3600  # 1000 W held: ~2.8 Wh
    assert abs((e2["integrated_wh"] - e1["integrated_wh"]) - expected) < 0.4, (e1, e2, expected)
    assert e2["covered_s"] >= e1["covered_s"] + 8


def test_a_switched_off_socket_adds_nothing_whatever_power_it_reports(dev, sock):
    sock.set(d1=False)  # the socket keeps reporting its old 1000 W
    assert query(dev)["ok"]
    e1 = energy(dev)
    assert e1["power_w"] == 0
    time.sleep(8)
    e2 = energy(dev)
    assert abs(e2["integrated_wh"] - e1["integrated_wh"]) <= 0.1, (e1, e2)
    assert e2["covered_s"] >= e1["covered_s"] + 6  # the time is counted, with 0 W


def test_a_lost_socket_adds_nothing_until_it_answers(dev, sock):
    sock.stop()
    for _ in range(2):  # two failed queries in a row: offline
        assert not query(dev)["ok"]
    assert wait_until(lambda: energy(dev)["power_w"] is None, 10)
    e1 = energy(dev)
    time.sleep(8)
    e2 = energy(dev)
    assert e2["integrated_wh"] == e1["integrated_wh"] and e2["covered_s"] == e1["covered_s"], (e1, e2)
    sock.start()
    assert query(dev)["ok"]
    assert energy(dev)["power_w"] == 1000


def test_a_read_value_counts_once_when_it_changed(dev, sock):
    sock.set(d17=5)
    assert query(dev)["ok"]
    e0 = energy(dev)
    sock.set(d17=7)
    assert query(dev)["ok"]
    e1 = energy(dev)
    assert e1["reported_wh"] == e0["reported_wh"] + 7 and e1["reports_counted"] == e0["reports_counted"] + 1, (e0, e1)
    assert e1["last_report"] == 7
    assert query(dev)["ok"]  # the same report read again
    e2 = energy(dev)
    assert e2["reported_wh"] == e1["reported_wh"] and e2["reports"] > e1["reports"], (e1, e2)
    last = details(dev)["reports"][-1]
    assert last["value"] == 7 and last["pushed"] is False and last["counted"] is False


def test_pushed_values_always_count(dev, sock):
    edit(dev, keep_connected=True)
    try:
        assert wait_until(lambda: sock.open_connections == 1, interval(dev) * 2 + 10), "the board did not open the kept connection"
        assert query(dev)["ok"]
        e0 = energy(dev)
        assert sock.push(d17=3) == 1
        assert wait_until(lambda: energy(dev)["reports_counted"] == e0["reports_counted"] + 1, 5)
        assert sock.push(d17=3) == 1  # the same value again is another report of the socket
        assert wait_until(lambda: energy(dev)["reports_counted"] == e0["reports_counted"] + 2, 5)
        assert query(dev)["ok"]  # a query reads the pushed value again: not counted
        e1 = energy(dev)
        assert e1["reported_wh"] == e0["reported_wh"] + 6 and e1["reports_counted"] == e0["reports_counted"] + 2, (e0, e1)
        pushed = [r for r in details(dev)["reports"] if r["pushed"]]
        assert len(pushed) >= 2 and all(r["counted"] and r["value"] == 3 for r in pushed[-2:])
        # a pushed power holds at once
        assert sock.push(d19=20000) == 1
        assert wait_until(lambda: energy(dev)["power_w"] == 2000, 5)
    finally:
        edit(dev, keep_connected=False)


def test_the_history_has_points_and_its_parameters_are_checked(dev):
    assert wait_until(lambda: len(details(dev)["points"]) >= 1, interval(dev) * 2 + 10), "no point in the history"
    j = details(dev, limit=1)
    assert j["point_s"] == 60 and j["points_capacity"] == 1440 and j["reports_capacity"] == 128
    assert len(j["points"]) <= 1
    if j["psram"]:
        p = j["points"][-1]
        assert set(p) == {"age_s", "time", "integrated_wh", "reported_wh", "power_w"}
    assert set(j["energy"]) == set(energy(dev))
    for bad in ("0", "1441", "x", "-1"):
        assert dev.get("/api/devices/energy", params={"name": FAKE_NAME, "limit": bad}).status_code == 400, bad
    assert dev.get("/api/devices/energy").status_code == 400
    assert dev.get("/api/devices/energy", params={"name": "zz-no-such-device"}).status_code == 404


def test_the_sockets_own_energy_reading_is_shown_as_before(dev, sock):
    sock.set(d17=42)
    assert query(dev)["ok"]
    d = next(d for d in dev.get("/api/devices").json()["devices"] if d["name"] == FAKE_NAME)
    assert d["state"]["dps"]["17"] == 42
    h = dev.get("/api/devices/history", params={"name": FAKE_NAME, "limit": 1}).json()["samples"][-1]
    assert h["energy"] == 42


def test_placeholders_give_the_energy_since_boot_in_kwh(dev, server, sock):
    sock.set(d17=9)
    assert query(dev)["ok"]
    n = len(server.requests)
    body = "{{%s.energy_integrated}}|{{%s.energy_reported}}|{{%s.energy}}|{{zz-no-such-device.energy_integrated}}" % ((FAKE_NAME,) * 3)
    j = send(dev, {"method": "POST", "url": server.base + "/echo", "content_type": "text/plain", "body": body}).json()
    assert j["ok"] is True, j
    assert len(server.requests) == n + 1
    integrated, reported, raw, missing = server.requests[-1]["body"].decode().split("|")
    assert re.fullmatch(r"\d+\.\d{3}", integrated) and re.fullmatch(r"\d+\.\d{3}", reported), (integrated, reported)
    e = energy(dev)
    assert abs(float(integrated) * 1000 - e["integrated_wh"]) < 3, (integrated, e)
    assert round(float(reported) * 1000) == e["reported_wh"], (reported, e)
    assert raw == "0.009" and missing == "-"
    r = send(dev, {"method": "POST", "url": server.base + "/echo", "body": "{{%s.energy_total}}" % FAKE_NAME})
    assert r.status_code == 400 and "unknown placeholder" in r.json()["error"], r.text
