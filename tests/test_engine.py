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


def test_the_most_runs_with_the_most_steps_fit_in_memory(dev, guard, fakes):
    """All scenarios (24 with PSRAM) with 24 steps each, all alive at once (in a pause after their first step): the board
    keeps enough 8-bit internal RAM (DRAM: what the network and the SD card driver need; "internal" also counts IRAM
    that only takes 32-bit access), and the big answers still come. With PSRAM the runs live there and cost next to no DRAM;
    before they did (~1.4 KB a run), the second run of this test in a row ran DRAM out and writing to the card failed."""
    fakes.start()
    fakes.set(d1=True)
    limit = listing(dev)["max"]
    steps = [{"device": FAKE_NAME, "action": "on", "pause_after_s": 600}] + [{"device": f"zz-ghost-{k}"} for k in range(23)]
    names = [f"{TEST}big{i}" for i in range(limit - len(guard))]
    leftovers = [s["name"] for s in guard if s["name"].startswith("zz-")]
    assert not leftovers, f"leftover test scenarios on the board: {leftovers}"
    assert len(names) >= limit // 2, f"too many scenarios of the user's own for this test: {[s['name'] for s in guard]}"
    assert put(dev, guard + [scenario(n, steps) for n in names]).status_code == 200
    before = dev.get("/api/status").json()["memory"]
    try:
        for n in names:
            assert dev.request("POST", "/api/scenarios/run", params={"name": n}).status_code == 200
        end = time.time() + 180
        while time.time() < end:
            listed = {s["name"]: s for s in listing(dev)["scenarios"]}
            if all(listed[n].get("pause_left_s") for n in names):
                break
            time.sleep(2)
        else:
            pytest.fail("not every run reached its pause")
        r = dev.get("/api/scenarios", timeout=20)
        assert r.status_code == 200
        st = dev.get("/api/status").json()
        during, engine_ = st["memory"], st["scenario_engine"]
        print(f"\n{len(names)} runs x 24 steps alive: runs_alive {engine_['runs_alive']}, 8-bit DRAM free {before['dram_free']} -> "
              f"{during['dram_free']}, minimum since boot {during['dram_min']}, largest block {during['dram_largest']} "
              f"(internal with IRAM {during['internal_free']}), GET /api/scenarios {len(r.content)} bytes")
        assert engine_["runs_alive"] == len(names)
        assert during["dram_free"] > 40000 and during["dram_largest"] > 20000
        if during["psram_total"]:
            assert before["dram_free"] - during["dram_free"] < 8000, (before, during)
    finally:
        for n in names:
            dev.request("POST", "/api/scenarios/stop", params={"name": n})
    for n in names:
        wait_state(dev, n, ("stopped",), timeout=20)


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
