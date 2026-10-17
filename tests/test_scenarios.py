"""Scenario API: /api/scenarios (list, editor), /api/scenarios/run|stop?name=...

The default tests never switch a real socket: the scenarios they run have no steps or only steps
for a device that does not exist ("no such device" fails at once, nothing is sent). Every PUT
repeats the scenarios that are already on the board and the fixture puts the original list back.

  SCENARIO=power-restored tests/run.sh -m scenario    really runs that scenario (switches real sockets)
"""
import copy
import os
import time

import pytest

GHOST = "zz-no-such-device"
TEST = "zz-test-"


def listing(dev):
    r = dev.get("/api/scenarios")
    assert r.status_code == 200, r.text
    return r.json()


def editable(sc):
    """A scenario as PUT wants it (no runtime fields; `when` of a power rule and `on_interrupt` kept, the live `watch` dropped)."""
    out = {"name": sc["name"], "enabled": sc["enabled"], "trigger": sc["trigger"], "default_pause_s": sc["default_pause_s"],
           "steps": [{k: v for k, v in st.items() if not (k == "pause_after_s" and v is None)} for st in sc["steps"]]}
    if "when" in sc:
        out["when"] = sc["when"]
    if "on_interrupt" in sc:
        out["on_interrupt"] = sc["on_interrupt"]
    return out


def put(dev, scenarios, **kw):
    return dev.request("PUT", "/api/scenarios", json={"scenarios": scenarios}, headers={"Content-Type": "application/json"}, **kw)


def scenario(name, steps=None, **kw):
    d = {"name": name, "enabled": False, "trigger": "manual", "default_pause_s": 5, "steps": steps if steps is not None else []}
    d.update(kw)
    return d


@pytest.fixture
def guard(dev):
    """The scenarios already on the board, restored after the test; `existing` is what every PUT must repeat."""
    existing = [editable(s) for s in listing(dev)["scenarios"]]
    yield existing
    r = put(dev, existing)
    assert r.status_code == 200, r.text


def wait_state(dev, name, states, timeout=15):
    end = time.time() + timeout
    while time.time() < end:
        sc = next(s for s in listing(dev)["scenarios"] if s["name"] == name)
        if sc["state"] in states:
            return sc
        time.sleep(0.4)
    raise AssertionError(f"{name} did not reach {states}: {sc}")


# ---- surface ----------------------------------------------------------------------------

def test_scenario_endpoints_need_auth_and_method(dev):
    assert dev.request("GET", "/api/scenarios", auth=False).status_code == 401
    assert dev.request("PUT", "/api/scenarios", auth=False, json={"scenarios": []}).status_code == 401
    for op in ("run", "stop"):
        assert dev.request("POST", f"/api/scenarios/{op}", auth=False, params={"name": "x"}).status_code == 401
        assert dev.get(f"/api/scenarios/{op}", params={"name": "x"}).status_code == 404  # POST only: a GET starts nothing
    assert dev.get("/api/scenarios/other").status_code == 404
    assert dev.request("PUT", "/api/scenarios/run", json={"scenarios": []}).status_code == 404  # never replaces the list


def test_listing_shape(dev):
    body = listing(dev)
    assert body["triggers"] == ["power_on", "manual", "power_below", "power_above"]
    assert body["max"] >= 1 and body["max_steps"] >= 1 and body["max_pause_s"] >= 60
    assert isinstance(body["sd"], bool) and isinstance(body["scenarios"], list)
    for sc in body["scenarios"]:
        assert {"name", "enabled", "trigger", "state", "default_pause_s", "steps"} <= set(sc)
        assert sc["trigger"] in body["triggers"]
        for st in sc["steps"]:
            assert set(st) == {"device", "action", "enabled", "pause_after_s"}


def test_run_and_stop_validate_before_looking_anything_up(dev):
    for op in ("run", "stop"):
        assert dev.request("POST", f"/api/scenarios/{op}").status_code == 400  # name is required
        assert dev.request("POST", f"/api/scenarios/{op}", params={"name": GHOST}).status_code == 404


def test_status_has_the_same_scenarios(dev):
    names = [s["name"] for s in listing(dev)["scenarios"]]
    assert [s["name"] for s in dev.get("/api/status").json()["scenarios"]] == names


# ---- validation: every bad document is rejected and changes nothing ----------------------------

def bad_docs():
    def ok(**kw):
        kw.setdefault("name", "zz-ok")
        kw.setdefault("steps", [{"device": "a"}])
        return scenario(kw.pop("name"), kw.pop("steps"), **kw)

    yield "not-an-object", []
    yield "no-array", {"scenarios": {}}
    yield "scenario-not-object", {"scenarios": [1]}
    yield "missing-name", {"scenarios": [{"trigger": "manual", "steps": []}]}
    yield "empty-name", {"scenarios": [ok(name="")]}
    yield "upper-name", {"scenarios": [ok(name="Zz")]}
    yield "long-name", {"scenarios": [ok(name="a" * 33)]}
    yield "slash-name", {"scenarios": [ok(name="a/b")]}
    yield "duplicate-name", {"scenarios": [ok(), ok()]}
    yield "enabled-not-bool", {"scenarios": [ok(enabled="yes")]}
    yield "missing-trigger", {"scenarios": [{"name": "zz-ok", "steps": []}]}
    yield "unknown-trigger", {"scenarios": [ok(trigger="sunrise")]}
    yield "trigger-not-string", {"scenarios": [ok(trigger=1)]}
    yield "pause-negative", {"scenarios": [ok(default_pause_s=-1)]}
    yield "pause-too-big", {"scenarios": [ok(default_pause_s=3601)]}
    yield "pause-text", {"scenarios": [ok(default_pause_s="30")]}
    yield "pause-fraction", {"scenarios": [ok(default_pause_s=1.5)]}
    yield "steps-missing", {"scenarios": [{"name": "zz-ok", "trigger": "manual"}]}
    yield "steps-not-array", {"scenarios": [ok(steps={})]}
    yield "step-not-object", {"scenarios": [ok(steps=["a"])]}
    yield "step-no-device", {"scenarios": [ok(steps=[{}])]}
    yield "step-bad-device", {"scenarios": [ok(steps=[{"device": "A B"}])]}
    yield "step-device-number", {"scenarios": [ok(steps=[{"device": 1}])]}
    yield "step-enabled-not-bool", {"scenarios": [ok(steps=[{"device": "a", "enabled": 1}])]}
    yield "step-pause-negative", {"scenarios": [ok(steps=[{"device": "a", "pause_after_s": -5}])]}
    yield "step-pause-too-big", {"scenarios": [ok(steps=[{"device": "a", "pause_after_s": 99999}])]}
    yield "step-pause-text", {"scenarios": [ok(steps=[{"device": "a", "pause_after_s": "5"}])]}
    yield "too-many-steps", {"scenarios": [ok(steps=[{"device": "a"}] * 25)]}
    yield "too-many-scenarios", {"scenarios": [scenario(f"zz-{i}") for i in range(25)]}  # over both limits (24 with PSRAM, 16 without)


@pytest.mark.parametrize("label,doc", list(bad_docs()), ids=[l for l, _ in bad_docs()])
def test_invalid_documents_are_rejected(dev, guard, label, doc):
    before = listing(dev)["scenarios"]
    r = dev.request("PUT", "/api/scenarios", json=doc, headers={"Content-Type": "application/json"})
    assert r.status_code == 400, (label, r.text)
    assert "error" in r.json()
    after = listing(dev)["scenarios"]
    assert after == before, (before, after)


def test_broken_json_and_wrong_content_type(dev, guard):
    r = dev.request("PUT", "/api/scenarios", data="{", headers={"Content-Type": "application/json"})
    assert r.status_code == 400
    r = dev.request("PUT", "/api/scenarios", data="scenarios=1")  # a form post: not JSON
    assert r.status_code in (400, 415)
    r = dev.request("PUT", "/api/scenarios", data="x" * 270000, headers={"Content-Type": "application/json"})  # over 262144 (and 49152)
    assert r.status_code == 413


# ---- editing -------------------------------------------------------------------------------

def test_round_trip_order_and_fields(dev, guard):
    steps = [{"device": "alpha", "enabled": True}, {"device": "beta", "enabled": False, "pause_after_s": 0},
             {"device": "gamma", "enabled": True, "pause_after_s": 120}]
    doc = scenario(TEST + "a", steps, trigger="power_on", default_pause_s=45)
    assert put(dev, guard + [doc]).status_code == 200
    got = next(s for s in listing(dev)["scenarios"] if s["name"] == TEST + "a")
    assert got["trigger"] == "power_on" and got["enabled"] is False and got["default_pause_s"] == 45
    assert [(s["device"], s["enabled"], s["pause_after_s"]) for s in got["steps"]] == [
        ("alpha", True, None), ("beta", False, 0), ("gamma", True, 120)]

    steps.reverse()  # the order is the order of the steps
    assert put(dev, guard + [dict(doc, steps=steps)]).status_code == 200
    got = next(s for s in listing(dev)["scenarios"] if s["name"] == TEST + "a")
    assert [s["device"] for s in got["steps"]] == ["gamma", "beta", "alpha"]

    assert put(dev, guard).status_code == 200  # a scenario that is not in the PUT is deleted
    assert TEST + "a" not in [s["name"] for s in listing(dev)["scenarios"]]
    assert dev.request("POST", "/api/scenarios/run", params={"name": TEST + "a"}).status_code == 404


def test_the_documented_maximum_fits_and_is_saved(dev, guard):
    """The maximum (24 scenarios x 24 steps with PSRAM, 16 x 24 without) with long names and every field set (up to ~58 KB of
    JSON): more than the 8 KB that the other PUTs accept. Saved, read back and the board keeps its memory."""
    limit = listing(dev)["max"]
    step = {"device": "zz-test-socket-with-a-long-name", "action": "off", "enabled": True, "pause_after_s": 300}
    mine = [scenario(f"{TEST}max{i}", [dict(step) for _ in range(24)]) for i in range(limit - len(guard))]
    heap = dev.get("/api/status").json()["heap_free"]
    r = put(dev, guard + mine)
    assert r.status_code == 200, r.text
    got = listing(dev)["scenarios"]
    psram = dev.get("/api/status").json()["memory"]["psram_total"] > 0
    assert len(got) == limit == (24 if psram else 16) and all(len(s["steps"]) == 24 for s in got if s["name"].startswith(TEST + "max"))
    assert dev.get("/api/status").json()["heap_free"] > heap - 20000
    assert put(dev, guard).status_code == 200


def test_removing_a_device_warns_about_the_scenarios_that_use_it(dev, guard):
    """A step names a device; when the device is removed (or renamed) the PUT still succeeds, but tells which steps
    would now fail. Only enabled steps count."""
    from conftest import device_list, put_devices
    original = device_list(dev)
    mine = {"name": "zz-warn", "ip": "192.168.8.252", "key": "0123456789abcdef", "version": "3.4", "dp": 1}
    try:
        assert put_devices(dev, original + [mine]).status_code == 200
        steps = [{"device": "zz-warn", "enabled": True}, {"device": "zz-warn-off", "enabled": False}]
        assert put(dev, guard + [scenario(TEST + "w", steps)]).status_code == 200
        r = put_devices(dev, original + [mine])
        assert r.status_code == 200 and r.json()["warnings"] == [], r.text
        r = put_devices(dev, original)  # zz-warn goes away
        w = r.json()["warnings"]
        assert r.status_code == 200 and len(w) == 1, r.text
        assert "zz-test-w" in w[0] and "step 1" in w[0] and "zz-warn" in w[0], w
    finally:
        put(dev, guard)
        put_devices(dev, original)


def test_a_disabled_power_on_scenario_is_not_listed_as_waiting(dev, guard):
    assert put(dev, guard + [scenario(TEST + "p", trigger="power_on", enabled=False)]).status_code == 200
    got = next(s for s in listing(dev)["scenarios"] if s["name"] == TEST + "p")
    assert got["state"] == "idle"


# ---- running, without touching a real socket ---------------------------------------------------

def test_a_scenario_without_steps_runs_and_finishes(dev, guard):
    assert put(dev, guard + [scenario(TEST + "e")]).status_code == 200
    r = dev.request("POST", "/api/scenarios/run", params={"name": TEST + "e"})
    assert r.status_code == 200 and r.json()["ok"] is True
    sc = wait_state(dev, TEST + "e", ("done",))
    assert sc["run_trigger"] == "manual"  # a disabled scenario can still be run by hand
    assert dev.request("POST", "/api/scenarios/stop", params={"name": TEST + "e"}).status_code == 409  # nothing to stop


def test_a_step_for_an_unknown_device_fails_and_the_scenario_goes_on(dev, guard):
    steps = [{"device": GHOST}, {"device": GHOST + "-2", "enabled": False}, {"device": GHOST + "-3"}]
    assert put(dev, guard + [scenario(TEST + "g", steps)]).status_code == 200
    assert dev.request("POST", "/api/scenarios/run", params={"name": TEST + "g"}).status_code == 200
    sc = wait_state(dev, TEST + "g", ("done",))
    assert [(x["state"]) for x in sc["run_steps"]] == ["failed", "disabled", "failed"]
    assert "no such device" in sc["run_steps"][0]["detail"]
    assert sc["progress_done"] == 2 and sc["progress_total"] == 2


def test_scenarios_run_side_by_side(dev, guard):
    docs = [scenario(TEST + n, [{"device": GHOST}]) for n in ("x", "y", "z")]
    assert put(dev, guard + docs).status_code == 200
    for d in docs:
        assert dev.request("POST", "/api/scenarios/run", params={"name": d["name"]}).status_code == 200
    for d in docs:
        assert wait_state(dev, d["name"], ("done",))["run_steps"][0]["state"] == "failed"


def test_the_board_stays_healthy_after_all_that(dev):
    st = dev.get("/api/status").json()
    assert st["heap_free"] > 20000


@pytest.mark.scenario
def test_real_scenario_run(dev):
    # Opt-in (SCENARIO=name pytest -m scenario): this really switches the sockets of that scenario.
    name = os.getenv("SCENARIO")
    if not name:
        pytest.skip("set SCENARIO=<name of a scenario on the board>")
    assert dev.request("POST", "/api/scenarios/run", params={"name": name}).status_code == 200
    sc = wait_state(dev, name, ("done", "stopped"), timeout=900)
    print(sc["run_steps"])
    assert sc["state"] == "done"
