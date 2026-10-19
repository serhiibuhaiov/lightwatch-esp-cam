"""The relay trigger (`"trigger": "relay"`, `when: {device, to}`) against the fake Tuya device on this computer: a change of the
switch starts the scenario once, whoever made it (the socket, a query that found it changed, a push, a step of this board); the
first reading after an edit of the device and the first answer after it was offline are not changes; a change during the
run starts it once more afterwards; an ignored or disabled trigger starts nothing. No real socket is touched: the steps here
wait or switch the fake. The board looks every 3 s, so every wait here allows for one look and a query.
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
    """A status query from the page: the board sees the fake's switch as it is now."""
    return dev.request("POST", "/api/devices/status", params={"name": FAKE_NAME}, timeout=20).json()


@pytest.fixture(autouse=True)
def calm(dev, fake):
    """Every test starts with the fake online and on, and the board knowing it."""
    fake.start()
    fake.silent = False
    fake.ignore_control = False
    fake.set(d1=True)
    assert query(dev)["ok"]
    yield
    fake.start()


def relay(name, to="any", steps=None, enabled=True, device=FAKE_NAME):
    return {"name": name, "enabled": enabled, "trigger": "relay", "when": {"device": device, "to": to}, "default_pause_s": 0,
            "steps": steps if steps is not None else [{"action": "wait", "seconds": 1}]}


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
    """Saved and looked at once: changes from now on count."""
    assert wait_until(lambda: "watch" in scn(dev, name), 10), scn(dev, name)


def runs(dev, name, after=0):
    """The ended runs of `name` newer than the id `after` (the test names are used by other modules too)."""
    r = dev.get("/api/scenarios/history", params={"name": name, "limit": 20})
    assert r.status_code == 200, r.text
    return [x for x in r.json()["runs"] if x["id"] > after]


def last_id(dev, name):
    return max([x["id"] for x in runs(dev, name)], default=0)


def switch(dev, fake, on):
    fake.set(d1=on)
    assert query(dev)["ok"]


def edit(dev, **fields):
    devices = device_list(dev)
    for d in devices:
        if d["name"] == FAKE_NAME:
            d.update(fields)
    r = put_devices(dev, devices)
    assert r.status_code == 200, r.text


# ---- validation ----------------------------------------------------------------------------------------------------

def bad_relays():
    base = relay(TEST + "bad")
    yield "no-when", {k: v for k, v in base.items() if k != "when"}
    yield "when-not-object", dict(base, when=[1])
    yield "no-device", dict(base, when={"to": "on"})
    yield "bad-device", dict(base, when={"device": "A B", "to": "on"})
    yield "no-to", dict(base, when={"device": FAKE_NAME})
    yield "bad-to", dict(base, when={"device": FAKE_NAME, "to": "toggle"})
    yield "number-to", dict(base, when={"device": FAKE_NAME, "to": 1})


@pytest.mark.parametrize("label,doc", list(bad_relays()), ids=[l for l, _ in bad_relays()])
def test_invalid_relay_triggers_are_rejected(dev, guard, label, doc):
    before = [editable(s) for s in listing(dev)["scenarios"]]
    r = put(dev, guard + [doc])
    assert r.status_code == 400, (label, r.text)
    assert "when" in r.json()["error"]
    assert [editable(s) for s in listing(dev)["scenarios"]] == before


def test_a_relay_trigger_round_trips(dev, guard):
    docs = [relay(TEST + "a", "on"), relay(TEST + "b", "off", enabled=False), relay(TEST + "c", "any", device="zz-no-such-device")]
    assert put(dev, guard + docs).status_code == 200
    assert "relay" in listing(dev)["triggers"]
    for d in docs:
        assert scn(dev, d["name"])["when"] == d["when"]
    watching(dev, TEST + "a")
    w = scn(dev, TEST + "a")["watch"]
    assert w == {"reason": "watching", "relay": True, "pending": False}, w
    assert "watch" not in scn(dev, TEST + "b")  # disabled: not watched
    assert wait_until(lambda: scn(dev, TEST + "c").get("watch", {}).get("reason") == "no-device", 10)


# ---- firing ------------------------------------------------------------------------------------------------------------

def test_a_change_seen_by_a_query_starts_the_scenario(dev, guard, fake):
    old = last_id(dev, TEST + "q")
    assert put(dev, guard + [relay(TEST + "q")]).status_code == 200
    watching(dev, TEST + "q")
    switch(dev, fake, False)
    assert wait_until(lambda: scn(dev, TEST + "q").get("run_trigger") == "relay-off", 10), scn(dev, TEST + "q")
    assert wait_until(lambda: scn(dev, TEST + "q")["state"] == "done", 15)
    assert scn(dev, TEST + "q")["watch"]["relay"] is False
    switch(dev, fake, True)
    assert wait_until(lambda: len(runs(dev, TEST + "q", old)) == 2, 20)
    assert [r["trigger"] for r in runs(dev, TEST + "q", old)] == ["relay-on", "relay-off"]


def test_to_on_does_not_start_on_a_change_to_off(dev, guard, fake):
    assert put(dev, guard + [relay(TEST + "on", "on")]).status_code == 200
    watching(dev, TEST + "on")
    switch(dev, fake, False)
    time.sleep(8)
    assert scn(dev, TEST + "on")["state"] == "idle"
    switch(dev, fake, True)
    assert wait_until(lambda: scn(dev, TEST + "on").get("run_trigger") == "relay-on", 10)


def test_a_change_made_by_a_step_of_this_board_counts(dev, guard, fake):
    switcher = {"name": TEST + "sw", "enabled": False, "trigger": "manual", "default_pause_s": 0,
                "steps": [{"device": FAKE_NAME, "action": "off"}]}
    assert put(dev, guard + [switcher, relay(TEST + "off", "off")]).status_code == 200
    watching(dev, TEST + "off")
    assert dev.request("POST", "/api/scenarios/run", params={"name": TEST + "sw"}).status_code == 200
    assert wait_until(lambda: scn(dev, TEST + "off").get("run_trigger") == "relay-off", 20), scn(dev, TEST + "off")
    assert fake.get(1) is False


def test_a_change_during_its_run_starts_it_once_more(dev, guard, fake):
    old = last_id(dev, TEST + "busy")
    assert put(dev, guard + [relay(TEST + "busy", steps=[{"action": "wait", "seconds": 8}])]).status_code == 200
    watching(dev, TEST + "busy")
    switch(dev, fake, False)
    assert wait_until(lambda: scn(dev, TEST + "busy")["state"] == "running", 10)
    switch(dev, fake, True)
    switch(dev, fake, False)  # two changes during the run: one more run
    assert wait_until(lambda: scn(dev, TEST + "busy")["watch"]["pending"] is True, 10), scn(dev, TEST + "busy")
    assert wait_until(lambda: len(runs(dev, TEST + "busy", old)) == 2, 40)
    time.sleep(6)
    assert [r["trigger"] for r in runs(dev, TEST + "busy", old)] == ["relay-off", "relay-off"]
    assert scn(dev, TEST + "busy")["watch"]["pending"] is False


def test_a_push_starts_it_without_a_query(dev, guard, fake):
    edit(dev, keep_connected=True, poll_s=300)
    try:
        assert wait_until(lambda: fake.open_connections == 1, 45), "the board did not open the kept connection"
        assert put(dev, guard + [relay(TEST + "push", "off")]).status_code == 200
        watching(dev, TEST + "push")
        queries = fake.queries
        assert fake.push(d1=False) == 1
        assert wait_until(lambda: scn(dev, TEST + "push").get("run_trigger") == "relay-off", 8), scn(dev, TEST + "push")
        assert fake.queries == queries  # seen in the push, not in a query
    finally:
        edit(dev, keep_connected=False, poll_s=None)


# ---- not a change --------------------------------------------------------------------------------------------------------

def test_unanswered_queries_do_not_look_like_a_change(dev, guard, fake):
    assert put(dev, guard + [relay(TEST + "blip")]).status_code == 200
    watching(dev, TEST + "blip")
    fake.stop()
    assert query(dev)["ok"] is False
    fake.start()
    assert query(dev)["ok"]
    time.sleep(8)
    assert scn(dev, TEST + "blip")["state"] == "idle"


def test_the_first_answer_after_being_offline_is_not_a_change(dev, guard, fake):
    assert put(dev, guard + [relay(TEST + "away")]).status_code == 200
    watching(dev, TEST + "away")
    fake.stop()
    for _ in range(2):
        query(dev)
    state = next(d for d in dev.get("/api/devices").json()["devices"] if d["name"] == FAKE_NAME)["state"]
    assert state["online"] is False, state
    fake.set(d1=False)  # switched off while away
    fake.start()
    assert query(dev)["ok"]
    time.sleep(8)
    assert scn(dev, TEST + "away")["state"] == "idle"
    switch(dev, fake, True)  # a change seen while online
    assert wait_until(lambda: scn(dev, TEST + "away").get("run_trigger") == "relay-on", 10)


def test_an_ignored_device_starts_nothing_and_its_first_reading_after_the_edit_is_no_change(dev, guard, fake):
    assert put(dev, guard + [relay(TEST + "ign")]).status_code == 200
    watching(dev, TEST + "ign")
    edit(dev, ignore=True)
    try:
        assert wait_until(lambda: scn(dev, TEST + "ign")["watch"]["reason"] == "ignored", 10)
        switch(dev, fake, False)  # a query from the page still reads an ignored device
        switch(dev, fake, True)
        time.sleep(6)
        assert scn(dev, TEST + "ign")["state"] == "idle"
    finally:
        edit(dev, ignore=False)  # the state is reset: the next answer is a first reading
    fake.set(d1=False)
    assert query(dev)["ok"]
    time.sleep(8)
    assert scn(dev, TEST + "ign")["state"] == "idle"
    assert scn(dev, TEST + "ign")["watch"]["reason"] == "watching"


def test_a_disabled_scenario_picks_up_no_old_change(dev, guard, fake):
    doc = relay(TEST + "off", enabled=False)
    assert put(dev, guard + [doc]).status_code == 200
    switch(dev, fake, False)
    time.sleep(4)
    assert put(dev, guard + [dict(doc, enabled=True)]).status_code == 200
    watching(dev, TEST + "off")
    time.sleep(6)
    assert scn(dev, TEST + "off")["state"] == "idle"


# ---- restart (opt-in: -m restart) --------------------------------------------------------------------------------------

@pytest.mark.restart
def test_after_a_restart_the_first_reading_starts_nothing(dev, guard, fake):
    assert put(dev, guard + [relay(TEST + "boot")]).status_code == 200
    watching(dev, TEST + "boot")
    fake.set(d1=False)  # changed while the board is away
    reboot(dev)
    assert wait_until(lambda: scn(dev, TEST + "boot").get("watch", {}).get("relay") is False, 45), scn(dev, TEST + "boot")
    time.sleep(6)
    assert scn(dev, TEST + "boot")["state"] == "idle"
    switch(dev, fake, True)
    assert wait_until(lambda: scn(dev, TEST + "boot").get("run_trigger") == "relay-on", 10)
