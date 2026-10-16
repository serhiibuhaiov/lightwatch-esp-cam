"""Monitor behaviour against a fake Tuya device on this computer: the offline hysteresis, the history of readings,
the "ignore" flag, and how the timeouts combine. See fake_tuya.py: nothing here touches a real socket.

The board must be able to reach this computer on TCP port 6668; otherwise these tests skip.
"""
import time

import pytest

from conftest import DEAD_NAME, FAKE_NAME, device_list, fake_registered, put_devices

SAMPLE_KEYS = {"age_s", "time", "ok", "on", "power", "current", "voltage", "energy"}


@pytest.fixture(scope="module")
def fake(dev):
    with fake_registered(dev, with_dead=True) as f:
        yield f


def status(dev, name=FAKE_NAME, **params):
    r = dev.request("POST", "/api/devices/status", params={"name": name, **params}, timeout=40)
    assert r.status_code == 200, r.text
    return r.json()


def state(dev, name=FAKE_NAME):
    return next(d for d in dev.get("/api/devices").json()["devices"] if d["name"] == name)


def history(dev, name=FAKE_NAME, **params):
    return dev.get("/api/devices/history", params={"name": name, **params})


def interval(dev):
    return dev.get("/api/devices").json()["poll_interval_s"]


def edit(dev, name, **fields):
    devices = device_list(dev)
    for d in devices:
        if d["name"] == name:
            d.update(fields)
    r = put_devices(dev, devices)
    assert r.status_code == 200, r.text


@pytest.fixture(autouse=True)
def calm(fake):
    """Every test starts with a normal, reachable fake device."""
    fake.start()
    fake.silent = False
    fake.answer_delay = 0.0
    fake.ignore_control = False
    fake.set(d1=True, d18=300, d19=480, d20=2250)
    yield
    fake.start()
    fake.silent = False
    fake.answer_delay = 0.0


# ---- the fake itself, so the other results can be trusted --------------------------------------------

def test_the_board_talks_to_the_fake_device(dev, fake):
    s = status(dev)
    assert s["ok"] is True and s["dps"]["19"] == 480
    off = dev.request("POST", "/api/devices/off", params={"name": FAKE_NAME}, timeout=30).json()
    assert off["ok"] is True and fake.get(1) is False
    assert status(dev)["dps"]["1"] is False
    assert fake.hmac_errors == 0


# ---- hysteresis ----------------------------------------------------------------------------------------

def test_one_failed_query_does_not_make_an_online_device_offline(dev, fake):
    for _ in range(3):  # a poll of the monitor may fall in between: then try again
        assert status(dev)["ok"]
        assert state(dev)["state"]["online"] is True and state(dev)["state"]["fails"] == 0
        fake.stop()
        failed = status(dev)
        assert failed["ok"] is False and failed["error"] == "connect"
        st = state(dev)["state"]
        if st["fails"] == 1:
            assert st["online"] is True, st  # one miss: still online
            assert st["error"] == "connect"
            fake.start()
            assert status(dev)["ok"]
            st = state(dev)["state"]
            assert st["online"] is True and st["fails"] == 0 and "error" not in st  # one success clears it
            return
        fake.start()
    pytest.fail("the monitor kept falling in between")


def test_two_failed_queries_in_a_row_make_it_offline_and_one_success_brings_it_back(dev, fake):
    assert status(dev)["ok"]
    fake.stop()
    for _ in range(3):
        assert status(dev)["ok"] is False
    st = state(dev)["state"]
    assert st["fails"] >= 2 and st["online"] is False and st["error"] == "connect"
    log = dev.get("/api/logs", params={"file": "current", "tail": 16384}).text
    assert f"[{FAKE_NAME}] OFFLINE" in log
    fake.start()
    assert status(dev)["ok"]
    st = state(dev)["state"]
    assert st["online"] is True and st["fails"] == 0
    log = dev.get("/api/logs", params={"file": "current", "tail": 16384}).text
    assert f"[{FAKE_NAME}] online" in log


def test_the_state_always_follows_the_rule(dev, fake):
    """online == (fails < 2) for a device that was online: sampled while the fake goes away and comes back."""
    assert status(dev)["ok"]
    fake.stop()
    for _ in range(4):
        status(dev)
        st = state(dev)["state"]
        assert st["online"] == (st["fails"] < 2), st
    fake.start()


def test_a_device_that_never_answered_is_offline_at_once(dev, fake):
    assert status(dev, DEAD_NAME)["ok"] is False
    st = state(dev, DEAD_NAME)["state"]
    assert st["online"] is False and st["fails"] >= 1


def test_a_device_that_stays_offline_gets_a_diagnostic_line_in_the_log(dev, fake):
    """The monitor says why a device keeps failing (error, timings, a plain TCP probe), once a minute at most."""
    assert status(dev, DEAD_NAME)["ok"] is False
    end = time.time() + 100
    line = None
    while time.time() < end and not line:
        log = dev.get("/api/logs", params={"file": "current", "tail": 32768}).text
        line = next((l for l in log.splitlines() if f"[{DEAD_NAME}] still failing" in l), None)
        if not line:
            time.sleep(5)
    assert line, "no diagnostic line for the dead device"
    assert "TCP 192.168.8." in line and "no answer" in line and "Wi-Fi" in line, line  # nobody is at that address


# ---- history ---------------------------------------------------------------------------------------------

def test_history_endpoint_validation(dev, fake):
    assert dev.request("GET", "/api/devices/history", auth=False, params={"name": FAKE_NAME}).status_code == 401
    assert dev.get("/api/devices/history").status_code == 400
    assert dev.get("/api/devices/history", params={"name": "zz-no-such-device"}).status_code == 404
    for bad in ("0", "81", "-1", "abc", "1.5", ""):
        assert history(dev, limit=bad).status_code == 400, bad
    assert dev.request("POST", "/api/devices/history", params={"name": FAKE_NAME}).status_code in (404, 405)


def test_history_records_every_query_with_its_readings(dev, fake):
    sent = [(101, 11, 2201), (202, 22, 2302), (303, 33, 2403), (404, 44, 2504)]
    for power, current, voltage in sent:
        fake.set(d19=power, d18=current, d20=voltage)
        assert status(dev)["ok"]
    body = history(dev, limit=30).json()
    assert body["capacity"] == 80 and isinstance(body["time_valid"], bool) and body["poll_interval_s"] == interval(dev)
    samples = body["samples"]
    assert 4 <= len(samples) <= 30
    for s in samples:  # (earlier tests left failed samples in the ring: those have no values)
        assert set(s) == SAMPLE_KEYS
        if s["ok"]:
            assert isinstance(s["on"], bool) and s["energy"] is None  # the fake has no DP 17
        else:
            assert s["power"] is None and s["on"] is None
    assert samples[-1]["ok"] is True
    powers = [s["power"] for s in samples]
    it = iter(powers)
    assert all(p in it for p, _, _ in sent), powers  # our values, in the order we sent them (polls may add equal ones)
    last = samples[-1]
    assert (last["power"], last["current"], last["voltage"]) == (404, 44, 2504)
    ages = [s["age_s"] for s in samples]
    assert ages == sorted(ages, reverse=True) and ages[-1] <= 20  # newest last
    if body["time_valid"]:
        times = [s["time"] for s in samples]
        assert times == sorted(times) and abs(times[-1] - time.time()) < 60
    assert len(history(dev, limit=3).json()["samples"]) == 3


def test_history_keeps_the_last_80_in_order(dev, fake):
    for i in range(1, 91):
        fake.set(d19=1000 + i)
        assert status(dev)["ok"]
    samples = history(dev).json()["samples"]
    assert len(samples) == 80
    powers = [s["power"] for s in samples]
    assert powers == sorted(powers) and powers[-1] == 1090  # a ring: the oldest ones are gone, the order is kept
    assert powers[0] >= 1011 - 5  # only the newest ~80 of the 90 (plus a few polls) remain
    ages = [s["age_s"] for s in samples]
    assert ages == sorted(ages, reverse=True)


def test_a_failed_query_is_a_gap_in_the_history(dev, fake):
    assert status(dev)["ok"]
    fake.stop()
    failed = status(dev)
    assert failed["ok"] is False
    last = history(dev).json()["samples"][-1]
    assert last["ok"] is False and last["power"] is None and last["on"] is None and last["voltage"] is None
    fake.start()
    assert status(dev)["ok"]
    last = history(dev).json()["samples"][-1]
    assert last["ok"] is True and last["power"] is not None


def test_history_shows_the_relay(dev, fake):
    for value in (False, True):
        fake.set(d1=value)
        assert status(dev)["ok"]
        assert history(dev).json()["samples"][-1]["on"] is value


# ---- the ignore flag -------------------------------------------------------------------------------------

def test_the_monitor_polls_a_device_by_itself(dev, fake):
    before = fake.connections
    time.sleep(interval(dev) * 2.2)
    polls = fake.connections - before
    assert 2 <= polls <= 4, polls


def test_an_ignored_device_is_not_polled_but_can_still_be_used_by_hand(dev, fake):
    try:
        edit(dev, FAKE_NAME, ignore=True)
        d = state(dev)
        assert d["ignore"] is True and d["state"]["known"] is False  # the editor start it over: nothing known
        before = fake.connections
        time.sleep(interval(dev) * 1.6)
        assert fake.connections == before  # not one poll
        assert status(dev)["ok"] is True  # a manual action still works
        assert fake.connections == before + 1
        assert state(dev)["ignore"] is True
    finally:
        edit(dev, FAKE_NAME, ignore=False)
    before = fake.connections
    deadline = time.time() + interval(dev) * 2 + 5
    while time.time() < deadline and fake.connections == before:
        time.sleep(1)
    assert fake.connections > before, "the monitor did not poll the device again after it was un-ignored"
    assert state(dev)["ignore"] is False


# ---- timeouts ----------------------------------------------------------------------------------------------

def test_a_device_that_is_off_fails_after_the_connect_timeout(dev, fake):
    t = time.time()
    r = status(dev, DEAD_NAME)
    took = time.time() - t
    assert r["ok"] is False and r["error"] == "connect"
    # 1.5 s to connect, not the 8 s of the whole request. Time spent waiting for the device's lock (the monitor may be
    # polling this very address just now: up to one more connect timeout) is not part of the operation.
    own = took - r["phases_ms"]["queued"] / 1000
    assert 1.3 < own < 2.6 and took < 5, (took, own)
    assert 1400 <= r["phases_ms"]["connect"] <= 1700


def test_a_refused_connection_fails_at_once(dev, fake):
    fake.stop()
    t = time.time()
    r = status(dev)
    assert r["ok"] is False and r["error"] == "connect" and time.time() - t < 1.2


def test_a_device_that_accepts_but_never_answers_hits_the_total_timeout(dev, fake):
    fake.silent = True
    t = time.time()
    r = status(dev, timeout_ms=2500)
    took = time.time() - t
    assert r["ok"] is False and r["error"] in ("timeout", "handshake")
    assert 2.0 < took < 8.0, took  # the total, not connect + total
    assert r["ms"] < 3500


def test_a_slow_answer_within_the_timeout_still_works(dev, fake):
    fake.answer_delay = 1.5
    r = status(dev, timeout_ms=5000)
    assert r["ok"] is True and r["phases_ms"]["handshake"] >= 1400


def test_refresh_works_against_a_device_that_pushes_before_it_answers(dev, fake):
    fake.set(d19=777)
    r = dev.request("POST", "/api/devices/refresh", params={"name": FAKE_NAME, "dps": "18,19,20", "compare": "1"}, timeout=30).json()
    assert r["ok"] is True and r["dps"]["19"] == 777 and r["before"]["19"] == 777
    assert "cmd=0x8" in r["frames"] and "cmd=0x12" in r["frames"]


def test_the_fake_never_saw_a_bad_frame(dev, fake):
    assert fake.hmac_errors == 0


# ---- adding and removing devices while the board runs --------------------------------------------------------

def _without(dev, name):
    put = [d for d in device_list(dev) if d["name"] != name]
    r = put_devices(dev, put)
    assert r.status_code == 200, r.text


def _with_fake(dev, ip):
    r = put_devices(dev, device_list(dev) + [{"name": FAKE_NAME, "ip": ip, "key": "0123456789abcdef", "version": "3.4", "dp": 1}])
    assert r.status_code == 200, r.text


def test_a_removed_device_is_gone_and_a_re_added_one_starts_fresh(dev, fake):
    ip = next(d["ip"] for d in device_list(dev) if d["name"] == FAKE_NAME)
    for _ in range(3):
        assert status(dev)["ok"]
    assert len(history(dev).json()["samples"]) >= 3
    _without(dev, FAKE_NAME)
    try:
        assert history(dev).status_code == 404
        assert dev.request("POST", "/api/devices/status", params={"name": FAKE_NAME}).status_code == 404
        assert FAKE_NAME not in [d["name"] for d in dev.get("/api/devices").json()["devices"]]
    finally:
        _with_fake(dev, ip)
    d = state(dev)
    assert d["state"]["known"] is False and d["state"].get("fails", 0) == 0  # nothing carried over from before
    assert history(dev).json()["samples"] == []
    assert status(dev)["ok"]
    assert 1 <= len(history(dev).json()["samples"]) <= 2  # the new life only


def test_removing_a_device_while_a_request_to_it_is_running_is_safe(dev, fake):
    import threading
    ip = next(d["ip"] for d in device_list(dev) if d["name"] == FAKE_NAME)
    fake.answer_delay = 2.0
    result = {}
    t = threading.Thread(target=lambda: result.update(status(dev, timeout_ms=8000)))
    t.start()
    time.sleep(0.7)  # the request is in the middle of its handshake
    _without(dev, FAKE_NAME)
    try:
        t.join(timeout=20)
        assert result.get("ok") is True  # the job carries its own copy of the device: it finished normally
        assert FAKE_NAME not in [d["name"] for d in dev.get("/api/devices").json()["devices"]]
        assert history(dev).status_code == 404  # the late answer did not bring the device back
        st = dev.get("/api/status").json()
        assert st["heap_free"] > 20000
    finally:
        fake.answer_delay = 0.0
        _with_fake(dev, ip)
    assert status(dev)["ok"]
    assert 1 <= len(history(dev).json()["samples"]) <= 2


def test_a_scenario_that_names_a_removed_device_fails_that_step_at_once(dev, fake):
    from test_scenarios import TEST, listing, put, scenario, wait_state
    ip = next(d["ip"] for d in device_list(dev) if d["name"] == FAKE_NAME)
    existing = [{"name": s["name"], "enabled": s["enabled"], "trigger": s["trigger"], "default_pause_s": s["default_pause_s"],
                 "steps": [{k: v for k, v in st.items() if not (k == "pause_after_s" and v is None)} for st in s["steps"]]}
                for s in listing(dev)["scenarios"]]
    _without(dev, FAKE_NAME)
    try:
        assert put(dev, existing + [scenario(TEST + "rm", [{"device": FAKE_NAME}])]).status_code == 200  # saved: no check at save time
        t = time.time()
        assert dev.request("POST", "/api/scenarios/run", params={"name": TEST + "rm"}).status_code == 200
        wait_state(dev, TEST + "rm", ("done",), timeout=10)
        assert time.time() - t < 4  # "no such device" is not retried
        step = next(s for s in listing(dev)["scenarios"] if s["name"] == TEST + "rm")["run_steps"][0]
        assert step["state"] == "failed" and "no such device" in step["detail"]
    finally:
        put(dev, existing)
        _with_fake(dev, ip)


def test_adding_and_removing_a_device_many_times_does_not_leak(dev, fake):
    """The free heap "breathes" by about 5 KB (ring buffers and network buffers come and go), so it is compared
    over two batches with a margin: a real leak would show as a steady fall, not as a swing that comes back."""
    ip = next(d["ip"] for d in device_list(dev) if d["name"] == FAKE_NAME)

    def heap():
        time.sleep(1)
        return dev.get("/api/status").json()["heap_free"]

    assert status(dev)["ok"]
    start = heap()
    levels = [start]
    try:
        for _ in range(2):
            for _ in range(15):
                _without(dev, FAKE_NAME)
                _with_fake(dev, ip)
                assert status(dev)["ok"]
            levels.append(heap())
    finally:
        if FAKE_NAME not in [d["name"] for d in device_list(dev)]:
            _with_fake(dev, ip)
    print("heap", levels)
    assert min(levels[1:]) > start - 8192


# ---- an answer that comes back for a device that has changed meanwhile -----------------------------------------------

def test_an_answer_for_a_device_that_was_edited_meanwhile_is_dropped(dev, fake):
    import threading
    ip = next(d["ip"] for d in device_list(dev) if d["name"] == FAKE_NAME)
    fake.answer_delay = 2.0
    result = {}
    t = threading.Thread(target=lambda: result.update(status(dev, timeout_ms=8000)))
    t.start()
    time.sleep(0.7)  # the request is in its handshake, on the old address
    edit(dev, FAKE_NAME, ip="192.168.8.252")  # the same name now points somewhere else
    try:
        t.join(timeout=20)
        assert result.get("ok") is True  # the old socket did answer
        d = state(dev)
        # the answer of the old address must not become the state or a sample of the new one (the monitor may
        # have failed once against the new address by now: that is fine, an answer is not)
        assert d["state"].get("online") is not True, d
        assert not any(s["ok"] for s in history(dev).json()["samples"])
    finally:
        fake.answer_delay = 0.0
        edit(dev, FAKE_NAME, ip=ip)


def test_an_answer_for_a_device_that_was_removed_meanwhile_is_dropped(dev, fake):
    import threading
    ip = next(d["ip"] for d in device_list(dev) if d["name"] == FAKE_NAME)
    fake.answer_delay = 2.0
    result = {}
    t = threading.Thread(target=lambda: result.update(status(dev, timeout_ms=8000)))
    t.start()
    time.sleep(0.7)
    _without(dev, FAKE_NAME)
    try:
        t.join(timeout=20)
        assert result.get("ok") is True
        assert history(dev).status_code == 404
    finally:
        fake.answer_delay = 0.0
        _with_fake(dev, ip)
    # a device with the same name added again at another address starts empty, whatever the old request did
    assert state(dev)["state"]["known"] is False
    assert history(dev).json()["samples"] == []


# ---- the monitor treats a device that is added again as a new one --------------------------------------------------------

def test_a_device_added_again_is_polled_in_full_at_once(dev, fake):
    ip = next(d["ip"] for d in device_list(dev) if d["name"] == FAKE_NAME)
    gap = interval(dev)
    fake.stop()
    deadline = time.time() + gap * 2 + 20  # the monitor itself finds the device offline (no requests of ours)
    while time.time() < deadline:
        st = state(dev)["state"]
        if st["known"] and not st["online"]:
            break
        time.sleep(1)
    else:
        pytest.fail("the monitor did not find the device offline")
    # it is offline now and the monitor skips such a device every second cycle: remove it and add it again
    fake.start()
    before = fake.connections
    _without(dev, FAKE_NAME)
    _with_fake(dev, ip)
    t0 = time.time()
    while time.time() - t0 < gap * 2 and fake.connections == before:
        time.sleep(0.5)
    took = time.time() - t0
    assert fake.connections > before, "never polled again"
    assert took <= gap + 5, f"polled after {took:.0f} s: skipping left over from the device's previous life"


def test_a_poll_that_is_running_when_the_device_gets_ignored_leaves_no_state(dev, fake):
    ip = next(d["ip"] for d in device_list(dev) if d["name"] == FAKE_NAME)
    fake.answer_delay = 3.0
    before = fake.connections
    deadline = time.time() + interval(dev) * 2 + 5
    while time.time() < deadline and fake.connections == before:  # wait for a poll of the monitor to start
        time.sleep(0.2)
    assert fake.connections > before, "the monitor did not poll"
    try:
        edit(dev, FAKE_NAME, ignore=True)  # while that poll waits for the answer
        time.sleep(4.5)
        d = state(dev)
        assert d["ignore"] is True and d["state"]["known"] is False, d  # the late answer did not give it a state
        assert history(dev).json()["samples"] == []
    finally:
        fake.answer_delay = 0.0
        edit(dev, FAKE_NAME, ignore=False)
