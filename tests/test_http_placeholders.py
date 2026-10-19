"""Placeholders in the url and the body of HTTP steps ({{device.power}}, {{when.relay}}, {{time}}, {{scenario}}, {{trigger}}),
against the fake HTTP server and the fake Tuya socket on this computer (no real socket is touched, nothing leaves the LAN):
validation, the values and their units, "-" when a value is not there, the meters of the device, the run's own fields, and
that the definition keeps the template. The conversions themselves are host-tested (tests/native/test_http_template).
"""
import re
import time

import pytest

from conftest import FAKE_NAME, device_list, fake_registered, put_devices
from fake_tuya import lan_ip_towards
from test_http import Fake, send
from test_scenarios import TEST, guard, listing, put  # noqa: F401

TIME_RE = r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d"


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


@pytest.fixture(scope="module")
def sock(dev):
    with fake_registered(dev) as f:
        yield f


def query(dev):
    return dev.request("POST", "/api/devices/status", params={"name": FAKE_NAME}, timeout=20).json()


@pytest.fixture(autouse=True)
def known(dev, sock):
    """The socket online and on, with power 48 W, current 0.3 A, voltage 225 V and no energy reading, and the board knowing it."""
    sock.start()
    with sock.lock:
        sock.dps.pop("17", None)
    sock.set(d1=True, d19=480, d18=300, d20=2250)
    assert query(dev)["ok"]
    yield
    sock.start()


def edit(dev, **fields):
    devices = device_list(dev)
    for d in devices:
        if d["name"] == FAKE_NAME:
            d.update(fields)
    r = put_devices(dev, devices)
    assert r.status_code == 200, r.text


def echoed(dev, server, body, url_tail="/echo"):
    """POST /api/http/test with `body`: what reached the fake server (path, body)."""
    n = len(server.requests)
    j = send(dev, {"method": "POST", "url": server.base + url_tail, "content_type": "text/plain", "body": body}).json()
    assert j["ok"] is True, j
    assert len(server.requests) == n + 1
    got = server.requests[-1]
    return got["path"], got["body"].decode()


def http_step(server, body, url_tail="/echo"):
    return {"action": "http", "method": "POST", "url": server.base + url_tail, "content_type": "text/plain", "body": body}


def scn(dev, name):
    return next(s for s in listing(dev)["scenarios"] if s["name"] == name)


def wait_until(pred, timeout, step=0.5):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(step)
    return False


# ---- validation ----------------------------------------------------------------------------------------------------

@pytest.mark.parametrize("label, trigger, step, words", [
    ("unknown-field", "manual", {"url": "http://x/?p={{heater1.watts}}"}, "url: unknown placeholder"),
    ("spaces", "manual", {"method": "POST", "url": "http://x/", "body": "{{ heater1.power }}"}, "body: unknown placeholder"),
    ("open", "manual", {"method": "POST", "url": "http://x/", "body": "a {{time"}, "body: unknown placeholder"),
    ("in-host", "manual", {"url": "http://{{scenario}}.example/x"}, "in the host"),
    ("when-without-device", "manual", {"url": "http://x/?p={{when.power}}"}, "`when` device"),
    ("when-interval", "interval", {"url": "http://x/?p={{when.relay}}"}, "`when` device"),
])
def test_invalid_placeholders_are_rejected(dev, guard, label, trigger, step, words):
    doc = {"name": TEST + "bad", "enabled": False, "trigger": trigger, "default_pause_s": 0, "steps": [dict({"action": "http"}, **step)]}
    if trigger == "interval":
        doc["when"] = {"every_s": 60}
    r = put(dev, guard + [doc])
    assert r.status_code == 400, (label, r.text)
    assert words in r.json()["error"], (label, r.text)


def test_the_test_request_rejects_unknown_placeholders(dev):
    r = send(dev, {"method": "POST", "url": "http://x/", "body": "{{heater1.watts}}"})
    assert r.status_code == 400 and "body: unknown placeholder" in r.json()["error"], r.text


def test_a_trigger_with_a_device_may_use_when_and_the_template_is_kept(dev, guard, server):
    url = server.base + "/echo?p={{when.power}}&t={{time}}"
    body = '{"text":"{{when.relay}} {{scenario}} {{trigger}}","o":{"a":{"b":1}}}'
    doc = {"name": TEST + "keep", "enabled": False, "trigger": "relay", "when": {"device": FAKE_NAME, "to": "any"}, "default_pause_s": 0,
           "steps": [{"action": "http", "method": "POST", "url": url, "body": body}]}
    assert put(dev, guard + [doc]).status_code == 200, doc
    st = scn(dev, TEST + "keep")["steps"][0]
    assert st["url"] == url and st["body"] == body


# ---- values ----------------------------------------------------------------------------------------------------------

def test_the_readings_by_name_in_their_units(dev, server):
    f = FAKE_NAME
    path, body = echoed(dev, server, f"{{{{{f}.power}}}}|{{{{{f}.voltage}}}}|{{{{{f}.current}}}}|{{{{{f}.energy}}}}|{{{{{f}.relay}}}}",
                        f"/echo?p={{{{{f}.power}}}}&r={{{{{f}.relay}}}}")
    assert body == "48.0|225.0|0.300|-|on", body
    assert path == "/echo?p=48.0&r=on", path


def test_the_test_request_has_no_run(dev, server):
    _, body = echoed(dev, server, "{{time}}|{{scenario}}|{{trigger}}|{{when.power}}|{{zz-no-such-device.power}}|{{zz-no-such-device.relay}}")
    t, rest = body.split("|", 1)
    assert re.fullmatch(TIME_RE, t), body
    assert rest == "-|-|-|-|-", body


def test_an_offline_or_ignored_socket_gives_a_dash(dev, server, sock):
    sock.stop()
    for _ in range(2):
        query(dev)
    assert echoed(dev, server, f"{{{{{FAKE_NAME}.power}}}} {{{{{FAKE_NAME}.relay}}}}")[1] == "- -"
    sock.start()
    assert query(dev)["ok"]
    assert echoed(dev, server, f"{{{{{FAKE_NAME}.power}}}}")[1] == "48.0"
    edit(dev, ignore=True)
    try:
        query(dev)  # a query from the page still reads an ignored device: its value is not used
        assert echoed(dev, server, f"{{{{{FAKE_NAME}.power}}}} {{{{{FAKE_NAME}.relay}}}}")[1] == "- -"
    finally:
        edit(dev, ignore=False)


def test_the_meters_of_the_device_say_which_data_point(dev, server, sock):
    sock.set(d21=1234, d22=5)
    original = next(d for d in device_list(dev) if d["name"] == FAKE_NAME)["meters"]
    edit(dev, meters={"power": 21, "current": 22, "voltage": 0})
    try:
        assert query(dev)["ok"]
        assert echoed(dev, server, f"{{{{{FAKE_NAME}.power}}}} {{{{{FAKE_NAME}.current}}}} {{{{{FAKE_NAME}.voltage}}}}")[1] == "123.4 0.005 -"
    finally:
        edit(dev, meters=original)


def test_a_run_fills_in_its_scenario_trigger_and_when(dev, guard, server, sock):
    name = TEST + "ph"
    doc = {"name": name, "enabled": True, "trigger": "relay", "when": {"device": FAKE_NAME, "to": "any"}, "default_pause_s": 0,
           "steps": [http_step(server, "{{scenario}} {{trigger}} {{when.relay}} {{when.power}} {{time}}", "/echo?r={{when.relay}}")]}
    assert put(dev, guard + [doc]).status_code == 200
    assert wait_until(lambda: "watch" in scn(dev, name), 10)
    n = len(server.requests)
    assert dev.request("POST", "/api/scenarios/run", params={"name": name}).status_code == 200
    assert wait_until(lambda: len(server.requests) == n + 1, 15)
    got = server.requests[-1]
    words = got["body"].decode().split(" ")
    assert words[:4] == [name, "manual", "on", "48.0"] and re.fullmatch(TIME_RE, words[4]), got["body"]
    assert got["path"] == "/echo?r=on"
    assert wait_until(lambda: scn(dev, name)["state"] == "done", 15)
    sock.set(d1=False, d19=0)
    assert query(dev)["ok"]
    assert wait_until(lambda: len(server.requests) == n + 2, 15), scn(dev, name)
    got = server.requests[-1]
    assert got["body"].decode().split(" ")[:4] == [name, "relay-off", "off", "0.0"], got["body"]
    assert got["path"] == "/echo?r=off"
    rec = dev.get("/api/scenarios/history", params={"name": name, "limit": 1}).json()["runs"][0]
    assert rec["steps"][0]["http"] == "POST " + server.base.split("//")[1], rec  # the history names the host only, no values
