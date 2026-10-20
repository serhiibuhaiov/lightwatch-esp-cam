"""The trigger "online" (`"trigger": "online"`, `when: {device, to, for_s}`) against the fake Tuya device on this computer: a
socket that went offline (after the monitor's hysteresis: two failed queries) or came back starts the scenario once; a
loss shorter than for_s is no event, nor is the way back; a trigger saved while the socket is offline takes that as its
base; after a boot or an edit of the device a socket that answers the first query is the base, one that does not comes
online as an event; an ignored device starts nothing.
No real socket is touched: the steps here only wait. The decision itself is host-tested (tests/native/test_online_rule).
"""
import time

import pytest

from conftest import FAKE_NAME, device_list, fake_registered, put_devices
from test_interrupt import reboot
from test_scenarios import TEST, editable, guard, listing, put  # noqa: F401


@pytest.fixture(scope="module")
def fake(dev):
    with fake_registered(dev) as f:
        yield f


def query(dev):
    return dev.request("POST", "/api/devices/status", params={"name": FAKE_NAME}, timeout=20).json()


def state(dev):
    return next(d for d in dev.get("/api/devices").json()["devices"] if d["name"] == FAKE_NAME)["state"]


def go_offline(dev, fake):
    """Two failed queries from the page: offline at once, without waiting for the monitor."""
    fake.stop()
    for _ in range(2):
        query(dev)
    assert state(dev)["online"] is False


def come_back(dev, fake):
    fake.start()
    assert query(dev)["ok"]


@pytest.fixture(autouse=True)
def calm(dev, fake):
    """Every test starts with the fake online and the board knowing it."""
    fake.start()
    fake.set(d1=True)
    assert query(dev)["ok"]
    yield
    fake.start()


def online(name, to="any", for_s=0, steps=None, enabled=True, device=FAKE_NAME):
    return {"name": name, "enabled": enabled, "trigger": "online", "when": {"device": device, "to": to, "for_s": for_s},
            "default_pause_s": 0, "steps": steps if steps is not None else [{"action": "wait", "seconds": 1}]}


def scn(dev, name):
    return next(s for s in listing(dev)["scenarios"] if s["name"] == name)


def wait_until(pred, timeout, step=0.5):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(step)
    return False


def watching(dev, name):
    assert wait_until(lambda: "watch" in scn(dev, name), 10), scn(dev, name)


def runs(dev, name, after=0):
    r = dev.get("/api/scenarios/history", params={"name": name, "limit": 20})
    assert r.status_code == 200, r.text
    return [x for x in r.json()["runs"] if x["id"] > after]


def last_id(dev, name):
    return max([x["id"] for x in runs(dev, name)], default=0)


def edit(dev, **fields):
    devices = device_list(dev)
    for d in devices:
        if d["name"] == FAKE_NAME:
            d.update(fields)
    r = put_devices(dev, devices)
    assert r.status_code == 200, r.text


# ---- validation ----------------------------------------------------------------------------------------------------

def bad_onlines():
    base = online(TEST + "bad")
    yield "no-when", {k: v for k, v in base.items() if k != "when"}
    yield "when-not-object", dict(base, when=[1])
    yield "no-device", dict(base, when={"to": "offline"})
    yield "no-to", dict(base, when={"device": FAKE_NAME})
    yield "relay-to", dict(base, when={"device": FAKE_NAME, "to": "off"})
    yield "negative-for", dict(base, when={"device": FAKE_NAME, "to": "any", "for_s": -1})
    yield "long-for", dict(base, when={"device": FAKE_NAME, "to": "any", "for_s": 3601})
    yield "text-for", dict(base, when={"device": FAKE_NAME, "to": "any", "for_s": "60"})


@pytest.mark.parametrize("label,doc", list(bad_onlines()), ids=[l for l, _ in bad_onlines()])
def test_invalid_online_triggers_are_rejected(dev, guard, label, doc):
    before = [editable(s) for s in listing(dev)["scenarios"]]
    r = put(dev, guard + [doc])
    assert r.status_code == 400, (label, r.text)
    assert "when" in r.json()["error"]
    assert [editable(s) for s in listing(dev)["scenarios"]] == before


def test_an_online_trigger_round_trips_with_for_s_0_by_default(dev, guard):
    doc = online(TEST + "rt", "offline", 120)
    plain = online(TEST + "rt0")
    del plain["when"]["for_s"]
    assert put(dev, guard + [doc, plain]).status_code == 200
    assert "online" in listing(dev)["triggers"]
    assert scn(dev, TEST + "rt")["when"] == {"device": FAKE_NAME, "to": "offline", "for_s": 120}
    assert scn(dev, TEST + "rt0")["when"]["for_s"] == 0
    watching(dev, TEST + "rt")
    w = scn(dev, TEST + "rt")["watch"]
    assert w["reason"] == "watching" and w["online"] is True and w["pending"] is False and w["held_s"] >= 0, w
    assert dev.get("/api/scenarios").json()["limits"]["online"]["max_for_s"] == 3600


# ---- firing --------------------------------------------------------------------------------------------------------------

def test_offline_and_back_each_start_it_once(dev, guard, fake):
    old = last_id(dev, TEST + "ob")
    assert put(dev, guard + [online(TEST + "ob")]).status_code == 200
    watching(dev, TEST + "ob")
    go_offline(dev, fake)
    assert wait_until(lambda: scn(dev, TEST + "ob").get("run_trigger") == "device-offline", 10), scn(dev, TEST + "ob")
    assert wait_until(lambda: scn(dev, TEST + "ob")["state"] == "done", 15)
    come_back(dev, fake)
    assert wait_until(lambda: len(runs(dev, TEST + "ob", old)) == 2, 20)
    time.sleep(6)
    assert [r["trigger"] for r in runs(dev, TEST + "ob", old)] == ["device-online", "device-offline"]


def test_to_offline_does_not_start_on_the_way_back(dev, guard, fake):
    old = last_id(dev, TEST + "off")
    assert put(dev, guard + [online(TEST + "off", "offline")]).status_code == 200
    watching(dev, TEST + "off")
    go_offline(dev, fake)
    assert wait_until(lambda: len(runs(dev, TEST + "off", old)) == 1, 20)
    come_back(dev, fake)
    time.sleep(8)
    assert [r["trigger"] for r in runs(dev, TEST + "off", old)] == ["device-offline"]


def test_a_loss_shorter_than_for_s_is_no_event_nor_is_the_way_back(dev, guard, fake):
    assert put(dev, guard + [online(TEST + "short", for_s=20)]).status_code == 200
    watching(dev, TEST + "short")
    go_offline(dev, fake)
    assert wait_until(lambda: scn(dev, TEST + "short")["watch"]["reason"] == "holding", 10), scn(dev, TEST + "short")
    time.sleep(5)
    come_back(dev, fake)
    time.sleep(28)  # longer than for_s after the way back
    s = scn(dev, TEST + "short")
    assert s["state"] == "idle" and s["watch"]["reason"] == "watching", s


def test_a_loss_that_holds_for_s_starts_it(dev, guard, fake):
    assert put(dev, guard + [online(TEST + "long", "offline", for_s=12)]).status_code == 200
    watching(dev, TEST + "long")
    go_offline(dev, fake)
    t0 = time.time()
    assert wait_until(lambda: scn(dev, TEST + "long").get("run_trigger") == "device-offline", 25), scn(dev, TEST + "long")
    assert time.time() - t0 >= 9  # not before for_s (the board looks every 3 s)


def test_one_failed_query_is_not_offline(dev, guard, fake):
    assert put(dev, guard + [online(TEST + "one")]).status_code == 200
    watching(dev, TEST + "one")
    fake.stop()
    assert query(dev)["ok"] is False
    assert state(dev)["online"] is True  # the hysteresis: one failure is not offline
    come_back(dev, fake)
    time.sleep(6)
    assert scn(dev, TEST + "one")["state"] == "idle"


def test_saved_while_offline_takes_that_as_its_base(dev, guard, fake):
    old = last_id(dev, TEST + "base")
    go_offline(dev, fake)
    assert put(dev, guard + [online(TEST + "base")]).status_code == 200
    watching(dev, TEST + "base")
    time.sleep(4)
    assert scn(dev, TEST + "base")["state"] == "idle" and scn(dev, TEST + "base")["watch"]["online"] is False
    come_back(dev, fake)
    assert wait_until(lambda: len(runs(dev, TEST + "base", old)) == 1, 20)
    assert runs(dev, TEST + "base", old)[0]["trigger"] == "device-online"


def test_an_edited_device_offline_at_its_first_query_comes_online_as_an_event(dev, guard, fake):
    old = last_id(dev, TEST + "edit")
    assert put(dev, guard + [online(TEST + "edit")]).status_code == 200
    watching(dev, TEST + "edit")
    fake.stop()
    edit(dev, ignore=True)
    assert wait_until(lambda: scn(dev, TEST + "edit")["watch"]["reason"] == "ignored", 10)
    edit(dev, ignore=False)  # the state is reset: the socket is new
    query(dev)
    assert state(dev)["online"] is False
    assert wait_until(lambda: scn(dev, TEST + "edit")["watch"]["reason"] == "not-seen-online", 10), scn(dev, TEST + "edit")
    come_back(dev, fake)  # its first query failed: its first online is an event
    assert wait_until(lambda: len(runs(dev, TEST + "edit", old)) == 1, 20), scn(dev, TEST + "edit")
    assert runs(dev, TEST + "edit", old)[0]["trigger"] == "device-online"


def test_an_event_during_its_run_starts_it_once_more(dev, guard, fake):
    old = last_id(dev, TEST + "busy")
    assert put(dev, guard + [online(TEST + "busy", steps=[{"action": "wait", "seconds": 10}])]).status_code == 200
    watching(dev, TEST + "busy")
    go_offline(dev, fake)
    assert wait_until(lambda: scn(dev, TEST + "busy")["state"] == "running", 10)
    come_back(dev, fake)
    assert wait_until(lambda: scn(dev, TEST + "busy")["watch"]["pending"] is True, 10), scn(dev, TEST + "busy")
    assert wait_until(lambda: len(runs(dev, TEST + "busy", old)) == 2, 40)
    assert [r["trigger"] for r in runs(dev, TEST + "busy", old)] == ["device-online", "device-offline"]


# ---- restart (opt-in: -m restart) --------------------------------------------------------------------------------------

@pytest.mark.restart
def test_after_a_restart_a_socket_that_answers_the_first_query_is_no_event(dev, guard, fake):
    assert put(dev, guard + [online(TEST + "ota")]).status_code == 200
    reboot(dev)  # like an OTA: the socket stays where it was
    assert wait_until(lambda: scn(dev, TEST + "ota").get("watch", {}).get("online") is True, 60), scn(dev, TEST + "ota")
    time.sleep(6)
    assert scn(dev, TEST + "ota")["state"] == "idle"  # its first online is the base
    go_offline(dev, fake)
    assert wait_until(lambda: scn(dev, TEST + "ota").get("run_trigger") == "device-offline", 10)


@pytest.mark.restart
def test_after_a_restart_a_socket_that_comes_up_later_is_an_event(dev, guard, fake):
    old = last_id(dev, TEST + "boot")
    assert put(dev, guard + [online(TEST + "boot")]).status_code == 200
    fake.stop()  # dead when the board comes up (up later than the board after a blackout)
    reboot(dev)
    assert wait_until(lambda: scn(dev, TEST + "boot").get("watch", {}).get("online") is False, 60), scn(dev, TEST + "boot")
    time.sleep(6)
    assert scn(dev, TEST + "boot")["state"] == "idle"  # never up: no "offline"
    fake.start()
    assert wait_until(lambda: len(runs(dev, TEST + "boot", old)) == 1, 60), scn(dev, TEST + "boot")
    assert runs(dev, TEST + "boot", old)[0]["trigger"] == "device-online"
