"""Black-box tests of the web API on a real device. Safe to run repeatedly:
every test that changes the config puts it back."""
import json
import os
import threading
import time
from concurrent.futures import ThreadPoolExecutor

import pytest
import requests

from conftest import LOG_LINE, LOG_NAME, with_value


# ---- auth and routing ---------------------------------------------------------

@pytest.mark.parametrize("path", ["/api/config", "/api/logs"])
def test_requires_auth(dev, path):
    r = dev.get(path, auth=False)
    assert r.status_code == 401
    assert "Basic" in r.headers.get("WWW-Authenticate", "")
    assert r.json() == {"error": "unauthorized"}


def test_wrong_password_rejected(dev):
    r = requests.get(dev.url + "/api/config", auth=(dev.auth[0], dev.auth[1] + "x"), timeout=10)
    assert r.status_code == 401


def test_unknown_path_is_json_404(dev):
    r = dev.get("/nope", auth=False)
    assert r.status_code == 404
    assert "error" in r.json()


# ---- web UI ----------------------------------------------------------------------------

def test_ui_page_served_behind_auth(dev):
    assert dev.get("/", auth=False).status_code == 401
    for path in ("/", "/index.html"):
        r = dev.get(path)
        assert r.status_code == 200
        assert r.headers["Content-Type"].startswith("text/html")
        assert "<title>" in r.text and "/api/status" in r.text and "/api/config" in r.text
        assert 5000 < len(r.content) < 135000  # the page itself (requests unpacks it): keep an eye on its size
        assert r.headers.get("Content-Encoding") == "gzip"  # stored and sent gzip-compressed
        assert int(r.headers["Content-Length"]) < len(r.content) / 2
    # a client that does not take gzip gets a hint, not compressed bytes
    r = dev.get("/", headers={"Accept-Encoding": "identity"})
    assert r.status_code == 406 and "--compressed" in r.text


# ---- GET /api/status ------------------------------------------------------------------

def test_status(dev):
    r = dev.get("/api/status")
    assert r.status_code == 200
    st = r.json()
    assert st["sd"] in ("ok", "missing", "fault")
    assert st["file_logging"] is (st["sd"] == "ok")
    assert st["config_source"] in ("SD card", "SD card (recovered config.tmp)", "flash copy", "defaults")
    assert st["uptime_s"] >= 0 and st["heap_free"] > 20000
    assert {"connected", "ip", "rssi"} <= set(st["wifi"])
    for cls in ("commands", "monitor"):  # how long Tuya jobs waited (queue + device lock) since boot
        t = st["tuya"][cls]
        assert set(t) == {"jobs", "wait_avg_ms", "wait_max_ms", "wait_over_1s", "busy"}
        assert all(isinstance(v, int) and v >= 0 for v in t.values())
        assert t["wait_avg_ms"] <= t["wait_max_ms"]
    mon = st["monitor"]
    assert set(mon) == {"interval_s", "last_cycle_ms", "max_cycle_ms"} and mon["interval_s"] == dev.get("/api/devices").json()["poll_interval_s"]
    assert 0 < mon["last_cycle_ms"] <= mon["max_cycle_ms"]
    assert "scenario" not in st and isinstance(st["scenarios"], list)
    for sc in st["scenarios"]:
        assert {"name", "enabled", "trigger", "state"} <= set(sc)
        assert sc["trigger"] in ("power_on", "manual", "power_below", "power_above", "interval", "relay")
        assert sc["state"] in ("idle", "skipped", "waiting", "running", "done", "stopped")
        if sc["state"] == "waiting":
            assert isinstance(sc["starts_in_s"], int)
        if sc["state"] == "skipped":
            assert sc["skip_reason"].startswith("not a power-up")
    hist = st["boot_history"]
    assert 1 <= len(hist) <= 16
    assert [e.get("current") for e in hist].count(True) == 1 and hist[-1]["current"] is True
    for e in hist:
        assert e["reason"] in ("POWERON", "EXT", "SW", "PANIC", "INT_WDT", "TASK_WDT", "WDT", "DEEPSLEEP", "BROWNOUT", "SDIO", "UNKNOWN")
        assert e["stage"] in ("started", "storage", "config", "wifi-started", "got-ip", "alive-60s")
        assert e["time_quality"] in ("synced", "estimated", "unknown")
        assert (e["time"] is None) == (e["time_quality"] == "unknown")
        assert isinstance(e["uptime_min"], int) and e["uptime_min"] >= 0
    if st["time_valid"]:
        assert hist[-1]["time_quality"] == "synced" and hist[-1]["time"] > 1700000000
    assert dev.get("/api/status", auth=False).status_code == 401


def test_status_memory(dev):
    m = dev.get("/api/status").json()["memory"]
    assert set(m) == {"internal_free", "internal_min", "internal_largest", "dram_free", "dram_min", "dram_largest",
                      "dma_free", "dma_min", "dma_largest", "psram_total", "psram_free", "psram_largest"}
    assert 20000 < m["internal_min"] <= m["internal_free"] and 0 < m["internal_largest"] <= m["internal_free"]
    # 8-bit DRAM and DMA-capable RAM are part of the internal RAM (which also counts the 32-bit-only IRAM).
    for k in ("dram", "dma"):
        assert 0 < m[f"{k}_min"] <= m[f"{k}_free"] <= m["internal_free"] and 0 < m[f"{k}_largest"] <= m[f"{k}_free"], m
    assert 10000 < m["dram_min"]
    assert m["psram_total"] == 0 or (m["psram_total"] > 1 << 20 and m["psram_largest"] <= m["psram_free"] <= m["psram_total"])


def test_status_limits_match_what_the_other_routes_say(dev):
    """The page shows the limits from /api/status; they must be the ones the editors and the history work with."""
    lim = dev.get("/api/status").json()["limits"]
    sc, scenarios = lim["scenarios"], dev.get("/api/scenarios").json()
    assert sc == scenarios["limits"]
    assert (sc["max"], sc["max_steps"], sc["max_pause_s"]) == (scenarios["max"], scenarios["max_steps"], scenarios["max_pause_s"])
    pr = sc["power_rule"]
    assert pr["min_for_s"] <= pr["max_for_s"] <= 900 and pr["max_watts"] == 20000 and pr["max_warmup_s"] == 3600
    devices = dev.get("/api/devices").json()
    dv = lim["devices"]
    assert dv["max"] == devices["max"] == dev.get("/api/devices/config").json()["max"]
    assert dv["history_capacity"] == devices["history_capacity"]
    assert dv["poll_interval_s"] == devices["poll_interval_s"] and dv["history_span_s"] == dv["history_capacity"] * dv["poll_interval_s"]
    assert pr["max_for_s"] == min(900, (dv["history_capacity"] - 2) * dv["poll_interval_s"])
    assert lim["tuya_queues"] == {"commands": 16, "monitor": 4}
    psram = dev.get("/api/status").json()["memory"]["psram_total"] > 0
    assert sc["basis"] == ("psram" if psram else "internal")
    assert (sc["max"], sc["max_file_bytes"], sc["max_body_bytes"]) == ((24, 393216, 262144) if psram else (16, 49152, 49152))
    assert (sc["max_with_psram"], sc["max_without_psram"]) == (24, 16)
    assert lim["put_body_bytes"] == {"config": 8192, "devices": 8192, "scenarios": sc["max_body_bytes"], "http_test": 49152}
    assert lim["http"] == {"max_url_bytes": 2048, "max_body_bytes": 16384, "max_content_type_bytes": 100, "queue": 4,
                           "timeout_ms": 10000, "min_timeout_ms": 1000, "max_timeout_ms": 30000, "response_bytes": 256}
    assert lim["logs"] == {"ram_bytes": 32768 if psram else 8192, "tail_max_bytes": 32768}  # the log buffer is in PSRAM when there is some
    assert 1 <= len(dev.get("/api/status").json()["boot_history"]) <= lim["boot_history"]


def test_sd_is_healthy_in_the_normal_suite(dev):
    # The default suite expects a working card; the no-card case is `-m nosd`.
    assert dev.get("/api/status").json()["sd"] == "ok"
    assert dev.log_files()["sd"] == "ok"


# ---- GET /api/config ------------------------------------------------------------

def test_config_shape_and_secrets_hidden(dev, baseline):
    cfg = baseline["saved"]
    for section in ("device", "wifi", "startup", "monitor", "log", "time"):
        assert section in cfg
    assert "devices" not in cfg  # Tuya devices live in /devices.json on the SD card only
    assert cfg["device"]["web_password"] == ""
    assert cfg["wifi"]["password"] == ""
    assert cfg["wifi"]["ssid"]


def test_active_and_saved_views(dev, baseline):
    r = dev.get("/api/config")
    assert r.headers["X-Restart-Pending"] in ("true", "false")
    assert dev.config(active=True)["device"]["web_password"] == ""


# ---- PUT /api/config: rejected input must never change anything --------------------

BAD_VALUES = [
    ("monitor.interval_s", 0), ("monitor.interval_s", 99999),
    ("monitor.timeout_ms", 99), ("monitor.timeout_ms", 60001),
    ("monitor.retries", 11), ("monitor.retries", -1),
    ("monitor.grace_s", 3601),
    ("startup.scenario_delay_s", 3601),
    ("wifi.restart_after_min", 1441),
    ("log.flush_s", 0), ("log.flush_s", 3601),
    ("log.max_file_kb", 8), ("log.max_file_kb", 8193),
    ("log.keep_files", 0), ("log.keep_files", 51),
    ("log.quiet_s", 601),
    ("log.level", "verbose"), ("log.level", 5),
    ("monitor.interval_s", "30"), ("monitor.interval_s", 1.5), ("monitor.interval_s", True),
    ("device.name", ""), ("device.name", "-bad"), ("device.name", "bad-"),
    ("device.name", "has space"), ("device.name", "x" * 33), ("device.name", 123),
    ("device.web_user", ""),
    ("wifi.ssid", "s" * 33),
    ("time.ntp", "n" * 65),
]


@pytest.mark.parametrize("field,value", BAD_VALUES, ids=[f"{f}={str(v)[:12]}" for f, v in BAD_VALUES])
def test_put_rejects_bad_value(dev, baseline, field, value):
    r = dev.put(with_value(baseline["saved"], field, value))
    assert r.status_code == 400, r.text
    assert field.split(".")[-1] in r.json()["error"]
    assert dev.config() == baseline["saved"]


@pytest.mark.parametrize("body", ["{bad", "", "[]", "42", "null", '"text"'])
def test_put_rejects_malformed_documents(dev, baseline, body):
    r = dev.put_raw(body)
    assert r.status_code == 400, r.text
    assert dev.config() == baseline["saved"]


def test_put_without_json_content_type_gets_415(dev, baseline):
    r = dev.put_raw("{}", headers={"Content-Type": "application/x-www-form-urlencoded"})
    assert r.status_code == 415
    assert dev.config() == baseline["saved"]


def test_put_oversized_body_gets_413(dev, baseline):
    r = dev.put_raw('{"pad":"' + "a" * 9000 + '"}')
    assert r.status_code == 413
    assert dev.config() == baseline["saved"]


def test_put_requires_auth(dev, baseline):
    r = dev.put_raw(json.dumps(baseline["saved"]), auth=False)
    assert r.status_code == 401
    assert dev.config() == baseline["saved"]


# ---- PUT /api/config: accepted input ------------------------------------------------

def test_put_roundtrip_and_pending_flag(dev, baseline):
    saved, active = baseline["saved"], baseline["active"]
    cur = saved["startup"]["scenario_delay_s"]
    taken = {cur, active["startup"]["scenario_delay_s"]}  # must differ from both
    new = next(v for v in range(cur + 1, cur + 4) if v not in taken and v <= 3600) \
        if cur < 3598 else next(v for v in range(cur - 1, cur - 4, -1) if v not in taken)
    try:
        r = dev.put(with_value(saved, "startup.scenario_delay_s", new))
        assert r.status_code == 200, r.text
        body = r.json()
        assert body["ok"] and body["sd"] and body["flash"] and not body["rebooting"]
        assert body["restart_required"] is True

        assert dev.config()["startup"]["scenario_delay_s"] == new
        assert dev.pending() is True
        # the running firmware still uses what it started with
        assert dev.config(active=True) == active
    finally:
        r = dev.put(saved)
        assert r.status_code == 200, r.text
    assert dev.config() == saved
    assert r.json()["restart_required"] is baseline["pending"]
    assert dev.pending() is baseline["pending"]


def test_empty_passwords_keep_the_old_ones(dev, baseline):
    # The saved config has blank secrets, so a PUT of it must not lock us out.
    r = dev.put(baseline["saved"])
    assert r.status_code == 200
    assert dev.get("/api/config").status_code == 200  # same credentials still work


def test_devices_key_in_config_is_ignored(dev, baseline):
    # "devices" moved to /devices.json; an old config.json may still carry it. It is accepted
    # and dropped, never stored in config.json or in the flash copy.
    try:
        assert dev.put(with_value(baseline["saved"], "devices", [{"name": "x"}])).status_code == 200
        assert "devices" not in dev.config()
    finally:
        dev.put(baseline["saved"])
    assert dev.config() == baseline["saved"]


def test_put_is_full_replacement(dev, baseline):
    # An absent section falls back to the defaults; check on a harmless one.
    partial = {k: v for k, v in baseline["saved"].items() if k != "monitor"}
    try:
        assert dev.put(partial).status_code == 200
        assert dev.config()["monitor"] == {"interval_s": 30, "timeout_ms": 3000,
                                           "retries": 3, "grace_s": 120}
    finally:
        dev.put(baseline["saved"])
    assert dev.config() == baseline["saved"]


# ---- logs -------------------------------------------------------------------------

def test_log_list(dev):
    info = dev.log_files()
    names = [f["name"] for f in info["files"]]
    assert all(LOG_NAME.match(n) for n in names)
    assert names == sorted(names, reverse=True)
    assert all(isinstance(f["size"], int) for f in info["files"])
    if info["current"]:
        assert LOG_NAME.match(info["current"])


def assert_log_lines(text, allow_bad=0):
    bad = [ln for ln in text.splitlines() if ln and not LOG_LINE.match(ln)]
    assert len(bad) <= allow_bad, f"{len(bad)} malformed lines, first: {bad[:3]}"


def test_log_tail_current(dev):
    r = dev.get("/api/logs", params={"file": "current", "tail": 4096})
    assert r.status_code == 200
    assert len(r.content) <= 4096
    assert r.content, "the current log should not be empty a minute after boot"
    assert_log_lines(r.text)


@pytest.mark.parametrize("tail", [0, 1, 99999999])
def test_log_tail_edge_values(dev, tail):
    ram = dev.get("/api/status").json()["limits"]["logs"]["ram_bytes"]
    r = dev.get("/api/logs", params={"file": "current", "tail": tail})
    assert r.status_code == 200
    assert len(r.content) <= 32768 + ram  # card tail cap plus the RAM buffer
    assert_log_lines(r.text)


@pytest.mark.parametrize("tail", ["-5", "abc", "1e3", "99999999999"])
def test_log_tail_rejects_what_is_not_a_number(dev, tail):
    r = dev.get("/api/logs", params={"file": "current", "tail": tail})
    assert r.status_code == 400, r.text


def test_log_full_file_download_matches_listed_size(dev):
    info = dev.log_files()
    for f in info["files"]:
        r = dev.get("/api/logs", params={"file": f["name"]}, timeout=60)
        assert r.status_code == 200
        if f["name"] == info["current"]:
            assert len(r.content) >= f["size"]  # still growing
        else:
            assert len(r.content) == f["size"]
        assert_log_lines(r.text, allow_bad=1)  # a cut append may tear one line


@pytest.mark.parametrize("name", [
    "../config.json", "..%2Fconfig.json", "log-1.txt", "log-00001.txt", "log-0001.TXT",
    "log-0001.txt/../x", "current/..", "/etc/passwd", "config.json", "log-abcd.txt",
])
def test_log_name_validation(dev, name):
    r = dev.get("/api/logs", params={"file": name})
    assert r.status_code == 400, (name, r.status_code)


def test_missing_log_file_is_404(dev):
    names = {f["name"] for f in dev.log_files()["files"]}
    missing = "log-9999.txt"
    if missing in names:
        pytest.skip("log-9999.txt exists")
    assert dev.get("/api/logs", params={"file": missing}).status_code == 404


# ---- robustness ---------------------------------------------------------------------

def test_sequential_burst(dev):
    for _ in range(60):
        assert dev.get("/api/config").status_code == 200


def test_concurrent_load_without_failures_or_reset(dev):
    """Several clients read config and logs for a while; a reset or hang would
    show up as failed or slow requests."""
    stop = time.time() + float(os.getenv("LOAD_SECONDS", "20"))
    errors, count, lock = [], [0], threading.Lock()

    def worker(i):
        paths = [("/api/config", None),
                 ("/api/logs", {"file": "current", "tail": 8192}),
                 ("/api/logs", None)]
        n = 0
        while time.time() < stop:
            path, params = paths[(i + n) % len(paths)]
            n += 1
            try:
                r = dev.get(path, params=params, timeout=8)
                if r.status_code != 200:
                    errors.append((path, r.status_code))
            except requests.RequestException as e:
                errors.append((path, repr(e)))
            with lock:
                count[0] += 1

    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(worker, range(4)))
    assert count[0] > 20
    assert not errors, errors[:5]
    assert dev.get("/api/config").status_code == 200
