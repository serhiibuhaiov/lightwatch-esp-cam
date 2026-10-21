"""Tuya device API: /api/devices (list, reload) and /api/devices/status|on|off?name=...

The default tests need no Tuya device at all: they check the API surface and that
keys never leave the board. The `tuya` and `tuya_switch` tests talk to a real device
that must be listed in /devices.json on the SD card:

  TUYA_DEVICE=towel tests/run.sh -m tuya          read only (status queries)
  TUYA_DEVICE=towel tests/run.sh -m tuya_switch   switches it off and on, then restores the state
"""
import json
import os
import time
from concurrent.futures import ThreadPoolExecutor

import pytest

OPS = ("status", "on", "off")


def op(dev, name, device, timeout=20, **params):
    return dev.request("POST", f"/api/devices/{name}", params={"name": device, **params}, timeout=timeout)


# ---- API surface, no Tuya device needed -------------------------------------------------

def test_devices_endpoints_need_auth(dev):
    assert dev.request("GET", "/api/devices", auth=False).status_code == 401
    assert dev.request("POST", "/api/devices/reload", auth=False).status_code == 401
    for name in OPS:
        assert dev.request("POST", f"/api/devices/{name}", auth=False, params={"name": "x"}).status_code == 401


def test_devices_list_shape_and_keys_hidden(dev):
    r = dev.get("/api/devices")
    assert r.status_code == 200, r.text
    body = r.json()
    assert isinstance(body["devices"], list)
    assert body["count"] == len(body["devices"])
    for d in body["devices"]:
        assert set(d) == {"name", "ip", "version", "dp", "key_set", "ignore", "keep_connected", "updatedps", "poll_s", "meters", "state",
                          "energy_since_boot"}
        assert set(d["meters"]) == {"power", "current", "voltage", "energy"}
        assert {"known", "online", "fails"} <= set(d["state"])
        assert d["version"] in ("3.4", "3.5")
        assert d["key_set"] is True
    assert '"key"' not in r.text


def test_devices_are_not_in_the_main_config(dev, baseline):
    assert "devices" not in baseline["saved"]
    assert "devices" not in baseline["active"]


def static_part(devices):
    """The configured fields only: `state` changes with every poll, `energy_since_boot` with time."""
    return [{k: v for k, v in d.items() if k not in ("state", "energy_since_boot")} for d in devices]


def test_reload_is_idempotent(dev):
    before = dev.get("/api/devices").json()
    r = dev.request("POST", "/api/devices/reload")
    assert r.status_code == 200, r.text
    after = r.json()
    if "error" not in after:
        assert static_part(after["devices"]) == static_part(before["devices"])
    assert static_part(dev.get("/api/devices").json()["devices"]) == static_part(after["devices"])


@pytest.mark.parametrize("name", OPS)
def test_unknown_device_is_404(dev, name):
    assert op(dev, name, "no-such-device").status_code == 404


@pytest.mark.parametrize("name", OPS)
def test_name_is_required(dev, name):
    r = dev.request("POST", f"/api/devices/{name}")
    assert r.status_code == 400
    assert "name" in r.json()["error"]


def test_wrong_method_is_rejected(dev):
    assert dev.get("/api/devices/status", params={"name": "x"}).status_code in (404, 405)
    assert dev.request("POST", "/api/devices").status_code in (404, 405)


# ---- the editor: GET/PUT /api/devices/config ----------------------------------------------
# These tests change the real /devices.json on the card, so they never delete or rename a device
# that was there before (its key could not be restored: the API never returns keys). Every PUT
# repeats the existing devices without a key, which keeps their keys, and the fixture puts the
# original list back at the end.

FAKE_KEY = "0123456789abcdef"


def get_config(dev):
    r = dev.get("/api/devices/config")
    assert r.status_code == 200, r.text
    return r.json()


def put_config(dev, devices, **kw):
    # Always an explicit Content-Type: see the safety note in test_editor_needs_a_json_body.
    return dev.request("PUT", "/api/devices/config", data=json.dumps({"devices": devices}),
                       headers={"Content-Type": "application/json"}, **kw)


def existing(dev):
    """The current devices as a PUT payload: no key, so the stored keys are kept."""
    # meters are kept as they are: a PUT without them would reset them to the standard numbers
    # (and ignore, updatedps, poll_s: a PUT without them would reset the flags)
    return [{"name": d["name"], "ip": d["ip"], "version": d["version"], "dp": d["dp"], "ignore": d.get("ignore", False),
             "keep_connected": d.get("keep_connected", False), "updatedps": d.get("updatedps", True), "poll_s": d.get("poll_s"),
             "meters": {k: v for k, v in d["meters"].items() if v is not None}} for d in get_config(dev)["devices"]]


def fake(name="zz-test", **kw):
    d = {"name": name, "ip": "192.168.8.251", "key": FAKE_KEY, "version": "3.5", "dp": 2}
    d.update(kw)
    return d


@pytest.fixture
def guard(dev):
    original = existing(dev)
    yield original
    if existing(dev) != original:
        r = put_config(dev, original)
        assert r.status_code == 200, r.text


def test_editor_endpoints_need_auth(dev):
    assert dev.request("GET", "/api/devices/config", auth=False).status_code == 401
    assert dev.request("PUT", "/api/devices/config", auth=False, data="{}",
                       headers={"Content-Type": "application/json"}).status_code == 401


def test_editor_list_has_no_keys(dev):
    r = dev.get("/api/devices/config")
    assert r.status_code == 200
    body = r.json()
    assert body["max"] == 16
    for d in body["devices"]:
        assert set(d) == {"name", "ip", "version", "dp", "key_set", "ignore", "keep_connected", "updatedps", "poll_s", "meters"} and d["key_set"] is True
        assert set(d["meters"]) == {"power", "current", "voltage", "energy"}
    assert '"key"' not in r.text


BAD_EDITS = [
    ("name with a space", lambda e: e + [fake("Bad Name")]),
    ("empty name", lambda e: e + [fake("")]),
    ("name too long", lambda e: e + [fake("x" * 33)]),
    ("duplicate name", lambda e: e + [fake(), fake()]),
    ("bad ip", lambda e: e + [fake(ip="300.1.1.1")]),
    ("ip is text", lambda e: e + [fake(ip="not-an-ip")]),
    ("ip empty", lambda e: e + [fake(ip="")]),
    ("unknown version", lambda e: e + [fake(version="3.3")]),
    ("version missing", lambda e: e + [{k: v for k, v in fake().items() if k != "version"}]),
    ("dp zero", lambda e: e + [fake(dp=0)]),
    ("dp too big", lambda e: e + [fake(dp=256)]),
    ("dp is text", lambda e: e + [fake(dp="1")]),
    ("new device without a key", lambda e: e + [{k: v for k, v in fake().items() if k != "key"}]),
    ("new device with an empty key", lambda e: e + [fake(key="")]),
    ("key too short", lambda e: e + [fake(key="0123456789abcde")]),
    ("key too long", lambda e: e + [fake(key="0123456789abcdefg")]),
    ("more than 16 devices", lambda e: e + [fake(f"zz-{i}") for i in range(17)]),
    ("an entry that is not an object", lambda e: e + ["text"]),
    ("meters is not an object", lambda e: e + [fake(meters="x")]),
    ("meter dp too big", lambda e: e + [fake(meters={"power": 256})]),
    ("meter dp negative", lambda e: e + [fake(meters={"voltage": -1})]),
    ("meter dp is text", lambda e: e + [fake(meters={"energy": "23"})]),
]


@pytest.mark.parametrize("label,edit", BAD_EDITS, ids=[b[0] for b in BAD_EDITS])
def test_editor_rejects_bad_input_and_changes_nothing(dev, guard, label, edit):
    before = get_config(dev)
    r = put_config(dev, edit(existing(dev)))
    assert r.status_code == 400, r.text
    assert r.json()["error"]
    assert get_config(dev) == before


@pytest.mark.parametrize("body", ["{bad", "[]", "42", "null", '{"devices": "no"}', '{"devices": {"a": 1}}', '{}'])
def test_editor_rejects_malformed_documents(dev, guard, body):
    before = get_config(dev)
    r = dev.request("PUT", "/api/devices/config", data=body, headers={"Content-Type": "application/json"})
    assert r.status_code == 400, r.text
    assert get_config(dev) == before


def test_editor_needs_a_json_body(dev, guard):
    # SAFETY: every request here carries an explicit Content-Type and a body that is not a valid
    # device list. requests sends no Content-Type for a str body, which the firmware treats as JSON,
    # so a valid-looking body without a header would really be SAVED.
    assert dev.request("PUT", "/api/devices/config").status_code in (400, 415)
    r = dev.request("PUT", "/api/devices/config", data="devices=nothing",
                    headers={"Content-Type": "application/x-www-form-urlencoded"})
    assert r.status_code == 415
    assert dev.request("PUT", "/api/devices/config", data="x" * 9000,
                       headers={"Content-Type": "application/json"}).status_code == 413


def test_editor_add_change_rename_remove(dev, guard):
    base = guard
    # add: applied at once, without a restart, key hidden, state unknown until the first poll
    r = put_config(dev, existing(dev) + [fake()])
    assert r.status_code == 200 and r.json() == {"ok": True, "count": len(base) + 1, "warnings": []}  # no scenario names a missing device
    added = {d["name"]: d for d in get_config(dev)["devices"]}["zz-test"]
    assert added == {"name": "zz-test", "ip": "192.168.8.251", "version": "3.5", "dp": 2, "key_set": True, "ignore": False,
                     "keep_connected": False, "updatedps": True, "poll_s": None, "meters": {"power": None, "current": None, "voltage": None, "energy": None}}
    api = {d["name"]: d for d in dev.get("/api/devices").json()["devices"]}
    assert "zz-test" in api and "key" not in api["zz-test"]

    # it really went to the card: re-reading the file gives the same list
    assert dev.request("POST", "/api/devices/reload").status_code == 200
    assert "zz-test" in {d["name"] for d in get_config(dev)["devices"]}

    # change without sending the key: the stored key is kept
    edited = [d for d in existing(dev) if d["name"] != "zz-test"] + [
        {"name": "zz-test", "ip": "192.168.8.252", "version": "3.4", "dp": 3}]
    assert put_config(dev, edited).status_code == 200
    changed = {d["name"]: d for d in get_config(dev)["devices"]}["zz-test"]
    assert (changed["ip"], changed["version"], changed["dp"]) == ("192.168.8.252", "3.4", 3)

    # rename without a key is refused (the key cannot follow a new name), with a key it works
    renamed = [d for d in existing(dev) if d["name"] != "zz-test"]
    assert put_config(dev, renamed + [{"name": "zz-test2", "ip": "192.168.8.252", "version": "3.4", "dp": 3}]).status_code == 400
    assert put_config(dev, renamed + [fake("zz-test2")]).status_code == 200

    # remove: back to the original list
    assert put_config(dev, renamed).status_code == 200
    assert existing(dev) == base


@pytest.mark.parametrize("bad", ["yes", "true", 1, 0, [], {}])
def test_editor_rejects_a_non_boolean_ignore(dev, guard, bad):
    before = get_config(dev)
    r = put_config(dev, guard + [fake("zz-test", ignore=bad)])
    assert r.status_code == 400 and "ignore" in r.json()["error"]
    assert get_config(dev) == before


def test_editor_ignore_flag_round_trip_and_reload(dev, guard):
    r = put_config(dev, guard + [fake("zz-test", ignore=True), fake("zz-test2", ip="192.168.8.252")])
    assert r.status_code == 200, r.text
    cfg = {d["name"]: d for d in get_config(dev)["devices"]}
    assert cfg["zz-test"]["ignore"] is True and cfg["zz-test2"]["ignore"] is False  # the default is "not ignored"
    for g in guard:  # the other devices keep their flag
        assert cfg[g["name"]]["ignore"] == g["ignore"]
    listed = {d["name"]: d for d in dev.get("/api/devices").json()["devices"]}
    assert listed["zz-test"]["ignore"] is True and listed["zz-test2"]["ignore"] is False
    assert listed["zz-test"]["state"]["known"] is False  # an ignored device is never queried by the monitor
    # the flag is in the file: it survives a reload from the card
    assert dev.request("POST", "/api/devices/reload").status_code == 200
    assert {d["name"]: d["ignore"] for d in get_config(dev)["devices"]}["zz-test"] is True
    # and it can be switched off again
    r = put_config(dev, guard + [fake("zz-test", ignore=False)])
    assert r.status_code == 200 and {d["name"]: d["ignore"] for d in get_config(dev)["devices"]}["zz-test"] is False


@pytest.mark.parametrize("bad", ["yes", 1, 0, []])
def test_editor_rejects_a_non_boolean_keep_connected(dev, guard, bad):
    before = get_config(dev)
    r = put_config(dev, guard + [fake("zz-test", keep_connected=bad)])
    assert r.status_code == 400 and "keep_connected" in r.json()["error"]
    assert get_config(dev) == before


def test_editor_keep_connected_round_trip_and_reload(dev, guard):
    r = put_config(dev, guard + [fake("zz-test", keep_connected=True), fake("zz-test2", ip="192.168.8.252")])
    assert r.status_code == 200, r.text
    cfg = {d["name"]: d for d in get_config(dev)["devices"]}
    assert cfg["zz-test"]["keep_connected"] is True and cfg["zz-test2"]["keep_connected"] is False  # the default
    for g in guard:  # the other devices keep their flag
        assert cfg[g["name"]]["keep_connected"] == g["keep_connected"]
    assert {d["name"]: d for d in dev.get("/api/devices").json()["devices"]}["zz-test"]["keep_connected"] is True
    assert dev.request("POST", "/api/devices/reload").status_code == 200  # it is in the file
    assert {d["name"]: d["keep_connected"] for d in get_config(dev)["devices"]}["zz-test"] is True


@pytest.mark.parametrize("bad", ["no", 0, 1, []])
def test_editor_rejects_a_non_boolean_updatedps(dev, guard, bad):
    before = get_config(dev)
    r = put_config(dev, guard + [fake("zz-test", updatedps=bad)])
    assert r.status_code == 400 and "updatedps" in r.json()["error"]
    assert get_config(dev) == before


def test_editor_updatedps_flag_round_trip_and_reload(dev, guard):
    r = put_config(dev, guard + [fake("zz-test", updatedps=False), fake("zz-test2", ip="192.168.8.252")])
    assert r.status_code == 200, r.text
    cfg = {d["name"]: d for d in get_config(dev)["devices"]}
    assert cfg["zz-test"]["updatedps"] is False and cfg["zz-test2"]["updatedps"] is True  # the default is "send it"
    for g in guard:  # the other devices keep their flag
        assert cfg[g["name"]]["updatedps"] == g["updatedps"]
    assert {d["name"]: d for d in dev.get("/api/devices").json()["devices"]}["zz-test"]["updatedps"] is False
    assert dev.request("POST", "/api/devices/reload").status_code == 200  # it is in the file
    assert {d["name"]: d["updatedps"] for d in get_config(dev)["devices"]}["zz-test"] is False


@pytest.mark.parametrize("bad", ["15", 3601, -1, 1.5, True, []])
def test_editor_rejects_a_bad_poll_s(dev, guard, bad):
    before = get_config(dev)
    r = put_config(dev, guard + [fake("zz-test", poll_s=bad)])
    assert r.status_code == 400 and "poll_s" in r.json()["error"]
    assert get_config(dev) == before


def test_editor_poll_s_round_trip_and_reload(dev, guard):
    r = put_config(dev, guard + [fake("zz-test", poll_s=300), fake("zz-test2", ip="192.168.8.252", poll_s=None)])
    assert r.status_code == 200, r.text
    cfg = {d["name"]: d for d in get_config(dev)["devices"]}
    assert cfg["zz-test"]["poll_s"] == 300 and cfg["zz-test2"]["poll_s"] is None  # null: every monitor cycle
    for g in guard:  # the other devices keep theirs
        assert cfg[g["name"]]["poll_s"] == g["poll_s"]
    assert {d["name"]: d for d in dev.get("/api/devices").json()["devices"]}["zz-test"]["poll_s"] == 300
    assert dev.request("POST", "/api/devices/reload").status_code == 200  # it is in the file
    assert {d["name"]: d["poll_s"] for d in get_config(dev)["devices"]}["zz-test"] == 300


def test_editor_poll_s_0_needs_keep_connected(dev, guard):
    before = get_config(dev)
    r = put_config(dev, guard + [fake("zz-test", poll_s=0)])  # no polling and no kept connection: it would never get data
    assert r.status_code == 400 and "keep_connected" in r.json()["error"]
    assert get_config(dev) == before
    r = put_config(dev, guard + [fake("zz-test", poll_s=0, keep_connected=True)])
    assert r.status_code == 200, r.text
    assert dev.request("POST", "/api/devices/reload").status_code == 200  # it is in the file
    assert {d["name"]: d["poll_s"] for d in get_config(dev)["devices"]}["zz-test"] == 0


def test_editor_ignores_the_old_monitor_field(dev, guard):
    # "monitor" ("poll"/"push") of older firmware is not read any more: no error, nothing kept
    r = put_config(dev, guard + [fake("zz-test", monitor="push")])
    assert r.status_code == 200, r.text
    got = {d["name"]: d for d in get_config(dev)["devices"]}["zz-test"]
    assert "monitor" not in got and got["poll_s"] is None and got["keep_connected"] is False


def test_editor_meters_mapping_round_trip(dev, guard):
    # 0 = "none", absent = the standard number (reported as null), a number = that data point.
    r = put_config(dev, existing(dev) + [fake(meters={"voltage": 23, "power": 0, "current": 21})])
    assert r.status_code == 200, r.text
    got = {d["name"]: d for d in get_config(dev)["devices"]}["zz-test"]["meters"]
    assert got == {"power": 0, "current": 21, "voltage": 23, "energy": None}
    api = {d["name"]: d for d in dev.get("/api/devices").json()["devices"]}["zz-test"]["meters"]
    assert api == got
    # it is stored on the card, not only in memory
    assert dev.request("POST", "/api/devices/reload").status_code == 200
    again = {d["name"]: d for d in get_config(dev)["devices"]}["zz-test"]["meters"]
    assert again == got
    # clearing: a device sent without meters goes back to the standard numbers
    assert put_config(dev, [d for d in existing(dev) if d["name"] != "zz-test"] + [fake()]).status_code == 200
    cleared = {d["name"]: d for d in get_config(dev)["devices"]}["zz-test"]["meters"]
    assert cleared == {"power": None, "current": None, "voltage": None, "energy": None}


def test_editor_leaves_the_other_devices_alone(dev, guard):
    base = guard
    assert put_config(dev, existing(dev) + [fake()]).status_code == 200
    assert [d for d in existing(dev) if d["name"] != "zz-test"] == base
    assert put_config(dev, base).status_code == 200


# ---- a real device ------------------------------------------------------------------------

@pytest.fixture(scope="module")
def target(dev):
    name = os.getenv("TUYA_DEVICE")
    if not name:
        pytest.skip("set TUYA_DEVICE to a device name from /devices.json")
    listed = {d["name"]: d for d in dev.get("/api/devices").json()["devices"]}
    if name not in listed:
        pytest.fail(f"{name!r} is not in /devices.json, loaded: {sorted(listed)}")
    return listed[name]


def status(dev, target, **params):
    r = op(dev, "status", target["name"], **params)
    assert r.status_code == 200, r.text
    return r.json()


@pytest.mark.tuya
def test_status_returns_the_data_points(dev, target):
    s = status(dev, target)
    assert s["ok"] is True, s
    assert s["device"] == target["name"] and s["op"] == "status"
    assert s["retcode"] == 0
    assert isinstance(s["dps"], dict) and str(target["dp"]) in s["dps"], s
    assert isinstance(s["dps"][str(target["dp"])], bool)


@pytest.mark.tuya
def test_saving_the_list_keeps_the_keys(dev, target):
    # PUT the list back without any keys: the real device must still talk to us afterwards.
    original = existing(dev)
    assert put_config(dev, original).status_code == 200
    assert existing(dev) == original
    s = status(dev, target)
    assert s["ok"] is True, s


@pytest.mark.tuya
def test_status_is_repeatable(dev, target):
    times = []
    for _ in range(10):
        s = status(dev, target)
        assert s["ok"] is True, s
        times.append(s["ms"])
    print("status ms:", times)
    assert max(times) < 5000


@pytest.mark.tuya
def test_parallel_requests_are_queued(dev, target):
    with ThreadPoolExecutor(4) as pool:
        results = list(pool.map(lambda _: status(dev, target), range(4)))
    assert all(r["ok"] for r in results), results


@pytest.mark.tuya
def test_commands_and_polls_on_one_device_do_not_collide(dev, target):
    """Commands (their own lane) hammer one device while the monitor polls it (another lane): every command must
    succeed (a command waits for the device, it is never "busy"), and the device must not be marked offline."""
    def tuya(): return dev.get("/api/status").json()["tuya"]
    before = tuya()
    end = time.time() + 40  # longer than monitor.interval_s: at least two polls happen meanwhile

    def hammer(_):
        out = []
        while time.time() < end:
            out.append(status(dev, target))
        return out

    with ThreadPoolExecutor(3) as pool:
        results = [r for part in pool.map(hammer, range(3)) for r in part]
    after = tuya()
    # A real socket now and then does not accept a connection that comes right after the previous one (seen: one in
    # a few hundred back-to-back sessions, "connect" after 1.5 s): that is the device, not the board. What must not
    # happen is a command that fails for any other reason, in particular "busy" (they wait for the device).
    failed = [r for r in results if not r["ok"]]
    assert len(results) >= 20 and len(failed) <= max(2, len(results) // 50), failed[:3]
    assert all(r["error"] in ("connect", "handshake", "timeout", "closed") for r in failed), failed[:3]
    assert after["commands"]["busy"] == before["commands"]["busy"]  # commands wait for the device
    assert after["monitor"]["jobs"] > before["monitor"]["jobs"]      # the monitor lane kept working meanwhile
    assert status(dev, target)["ok"]
    st = listed(dev, target)["state"]
    assert st["online"] and st["fails"] == 0, st


@pytest.mark.tuya
def test_the_board_stays_healthy_after_many_operations(dev, target):
    heap_before = dev.get("/api/status").json()["heap_free"]
    for _ in range(30):
        assert status(dev, target)["ok"] is True
    heap_after = dev.get("/api/status").json()["heap_free"]
    print("heap", heap_before, "->", heap_after)
    assert heap_after > heap_before - 4096  # no leak per operation


@pytest.mark.tuya
def test_too_short_timeout_fails_cleanly(dev, target):
    s = status(dev, target, timeout_ms=500)
    assert s["ok"] in (True, False)  # a fast device may still answer; the point is: a clean JSON answer
    if not s["ok"]:
        assert s["error"] in ("timeout", "connect", "closed", "handshake")


def listed(dev, target):
    return {d["name"]: d for d in dev.get("/api/devices").json()["devices"]}[target["name"]]


@pytest.mark.tuya
def test_cached_state_after_a_status_query(dev, target):
    status(dev, target)
    st = listed(dev, target)["state"]
    assert st["known"] and st["online"] and st["fails"] == 0
    assert st["age_s"] <= 3 and st["latency_ms"] > 0
    assert isinstance(st["on"], bool) and st["dps"]


@pytest.mark.tuya
def test_background_poll_refreshes_the_state(dev, target):
    interval = dev.get("/api/devices").json()["poll_interval_s"]
    own = next(d for d in dev.get("/api/devices/config").json()["devices"] if d["name"] == target["name"]).get("poll_s")
    if own:  # the device's own "poll every N s" (poll_s 0: no polling at all, only pushes)
        interval = max(interval, own)
    elif own == 0:
        pytest.skip(f"{target['name']} has poll_s 0: the monitor does not poll it")
    deadline = time.time() + interval + 20
    first = listed(dev, target)["state"]["last_try_age_s"]
    while time.time() < deadline:
        time.sleep(2)
        age = listed(dev, target)["state"]["last_try_age_s"]
        if age < first:  # the age dropped: a new query ran without us asking
            return
        first = age
    pytest.fail(f"no background poll within {interval} s (monitor.interval_s, or the device's poll_s)")


@pytest.mark.tuya
def test_requests_and_answers_are_logged(dev, target):
    status(dev, target)
    log = dev.get("/api/logs", params={"file": "current", "tail": 16000}).text
    name = target["name"]
    assert f"[{name}] status {target['ip']}:6668 v{target['version']}" in log
    assert f"[{name}] tx cmd=0x10" in log and f"[{name}] rx cmd=0x10" in log
    assert f"[{name}] status: OK in" in log and "connect" in log and "handshake" in log
    assert '"dps"' in log or '"1":' in log


@pytest.mark.tuya
def test_result_carries_the_request_and_timings(dev, target):
    s = status(dev, target)
    assert s["request"] == "{}"
    assert set(s["phases_ms"]) == {"connect", "handshake", "command", "queued"}
    assert s["ms"] >= s["phases_ms"]["connect"]


@pytest.mark.tuya_switch
def test_switch_off_and_on_and_restore(dev, target):
    original = status(dev, target)["dps"][str(target["dp"])]
    key = str(target["dp"])
    try:
        for wanted, name in ((False, "off"), (True, "on"), (False, "off")):
            r = op(dev, name, target["name"])
            assert r.status_code == 200, r.text
            assert r.json()["ok"] is True, r.json()
            time.sleep(1)
            assert status(dev, target)["dps"][key] is wanted
    finally:
        op(dev, "on" if original else "off", target["name"])
    assert status(dev, target)["dps"][key] is original


@pytest.mark.tuya_switch
def test_switch_accepts_a_clock_that_is_not_synchronised(dev, target):
    # Without NTP the board would send "t" = seconds since boot instead of the Unix time. The
    # diagnostic parameter t= sends such values; the device must still switch.
    key = str(target["dp"])
    original = status(dev, target)["dps"][key]
    try:
        for wanted, name, t in ((not original, "on" if not original else "off", 45), (original, "on" if original else "off", 0)):
            r = op(dev, name, target["name"], t=t)
            assert r.status_code == 200 and r.json()["ok"] is True, r.text
            assert f'"t":{t},' in r.json()["request"]
            time.sleep(1)
            assert status(dev, target)["dps"][key] is wanted
    finally:
        op(dev, "on" if original else "off", target["name"])
    assert status(dev, target)["dps"][key] is original


def test_t_and_timeout_parameters_are_validated(dev):
    # SAFETY: an unknown device name, so even if validation were broken nothing could reach a real
    # device (the firmware checks the parameters before it looks the device up).
    for bad in ("-1", "x", "", "4294967296", "99999999999", "1.5", "+5", " 5"):
        r = op(dev, "on", "no-such-device", t=bad)
        assert r.status_code == 400, ("t", bad, r.text)
    assert op(dev, "status", "no-such-device", t="5").status_code == 400  # status has no time
    for good in ("0", "45", "4294967295"):
        assert op(dev, "on", "no-such-device", t=good).status_code == 404  # accepted, then the device is unknown
    for bad in ("100", "499", "30001", "99999", "abc", "", "-5"):
        r = op(dev, "status", "no-such-device", timeout_ms=bad)
        assert r.status_code == 400, ("timeout_ms", bad, r.text)
    assert op(dev, "status", "no-such-device", timeout_ms="500").status_code == 404
