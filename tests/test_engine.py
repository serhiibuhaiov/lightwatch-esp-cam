"""The scenario engine under load, against fake Tuya devices on this computer (no real socket is touched):

- many runs at once share at most 4 places in the command lane, and a button press still gets through quickly;
- the attempts on a socket the monitor counts offline are counted (GET /api/status -> scenario_engine), which is the
  measurement that decides whether such attempts should be spaced out.

Run with -s to see the numbers.
"""
import time

import pytest

from conftest import DEAD_NAME, FAKE_NAME, fake_registered
from test_scenarios import TEST, guard, listing, put, scenario, wait_state  # noqa: F401


@pytest.fixture(scope="module")
def fakes(dev):
    with fake_registered(dev, with_dead=True) as f:
        yield f


def engine(dev):
    return dev.get("/api/status").json()["scenario_engine"]


def press_status(dev):
    """A status query as the page's "refresh" button sends it; (seconds until the answer, how long it was queued)."""
    t = time.time()
    r = dev.request("POST", "/api/devices/status", params={"name": FAKE_NAME}, timeout=60).json()
    assert r["ok"], r
    return time.time() - t, r["phases_ms"]["queued"] / 1000


def delta(after, before, key):
    return after[key] - before[key]


def test_twelve_runs_share_four_places_and_a_button_still_gets_through(dev, guard, fakes):
    fakes.start()
    fakes.set(d1=True)
    fakes.answer_delay = 0.3  # every job ~0.4 s: the runs really have to queue
    n = 12
    mine = [scenario(f"{TEST}load{i}", [{"device": FAKE_NAME, "action": "on"}, {"device": FAKE_NAME, "action": "on"}], default_pause_s=0)
            for i in range(n)]
    try:
        assert put(dev, guard + mine).status_code == 200
        before = engine(dev)
        for i in range(n):
            assert dev.request("POST", "/api/scenarios/run", params={"name": f"{TEST}load{i}"}).status_code == 200
        time.sleep(1.5)
        during = engine(dev)
        presses = [press_status(dev) for _ in range(3)]
        for i in range(n):
            wait_state(dev, f"{TEST}load{i}", ("done",), timeout=180)
        after = engine(dev)
    finally:
        fakes.answer_delay = 0.0
    print(f"\n12 runs x 2 steps: runs alive after 1.5 s {during['runs_alive']}, in flight {during['in_flight']}, "
          f"max in flight {after['max_in_flight_seen']}, slot waits +{delta(after, before, 'slot_waits')}, "
          f"longest slot wait {after['slot_wait_max_ms']} ms, queue full +{delta(after, before, 'queue_full')}; "
          f"button: " + ", ".join(f"{total:.1f} s (queued {queued:.1f} s)" for total, queued in presses))
    for i in range(n):
        assert [s["state"] for s in next(s for s in listing(dev)["scenarios"] if s["name"] == f"{TEST}load{i}")["run_steps"]] == ["ok", "ok"]
    assert during["runs_alive"] >= n // 2 and during["in_flight"] <= 4
    assert after["max_in_flight"] == 4 and after["max_in_flight_seen"] == 4 and after["in_flight"] == 0
    assert delta(after, before, "slot_waits") > 0
    assert delta(after, before, "jobs") == n * 4  # command + query per step
    # The button waits behind at most the 4 engine jobs (and the monitor's poll of the same socket), not behind all 48.
    assert all(queued < 4.0 for _, queued in presses), presses


def test_attempts_on_an_offline_socket_are_counted(dev, guard, fakes):
    fakes.start()
    # The monitor has to have seen the dead socket fail first (one poll cycle, plus a second one for a device that is new).
    end = time.time() + 60
    while time.time() < end:
        st = next(d for d in dev.get("/api/devices").json()["devices"] if d["name"] == DEAD_NAME)["state"]
        if st["known"] and not st["online"]:
            break
        time.sleep(1)
    else:
        pytest.fail("the monitor never counted the dead socket offline")
    n = 4
    mine = [scenario(f"{TEST}dead{i}", [{"device": DEAD_NAME}]) for i in range(n)]
    assert put(dev, guard + mine).status_code == 200
    before = engine(dev)
    for i in range(n):
        assert dev.request("POST", "/api/scenarios/run", params={"name": f"{TEST}dead{i}"}).status_code == 200
    time.sleep(2)
    presses = [press_status(dev) for _ in range(3)]
    for i in range(n):
        wait_state(dev, f"{TEST}dead{i}", ("done",), timeout=120)
    after = engine(dev)
    attempts = delta(after, before, "offline_attempts")
    lane_s = delta(after, before, "offline_attempt_ms") / 1000
    print(f"\n{n} runs on a dead socket: {attempts} attempts on a socket counted offline, {delta(after, before, 'offline_attempts_ok')} worked, "
          f"{lane_s:.1f} s of command lane; button meanwhile: " + ", ".join(f"{total:.1f} s (queued {queued:.1f} s)" for total, queued in presses))
    for i in range(n):
        step = next(s for s in listing(dev)["scenarios"] if s["name"] == f"{TEST}dead{i}")["run_steps"][0]
        assert step["state"] == "failed" and step["attempts"] == 3
    assert attempts == n * 3 and delta(after, before, "offline_attempts_ok") == 0
    assert n * 3 * 1.3 <= lane_s <= n * 3 * 3.5  # ~1.5 s connect timeout per attempt
