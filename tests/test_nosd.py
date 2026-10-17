"""The board was booted WITHOUT a working SD card (opt-in: pytest -m nosd).

Procedure: power off, take the card out, power on, wait ~40 s, run
    tests/run.sh -m nosd
Expected firmware behaviour: it starts from the flash copy of the config, keeps
the log in RAM (served through the API), says so, and signals with the LEDs
(3 short pulses of the flash LED and the red LED every ~3 s - check by eye).
"""
import pytest

from conftest import LOG_LINE, with_value

pytestmark = pytest.mark.nosd


def test_status_reports_missing_card(dev):
    st = dev.get("/api/status").json()
    assert st["sd"] == "missing"
    assert st["file_logging"] is False
    assert st["config_source"] == "flash copy", st
    assert st["wifi"]["connected"] is True


def test_config_is_served_from_flash(dev, baseline):
    cfg = baseline["saved"]
    assert cfg["wifi"]["ssid"]
    assert cfg["device"]["web_password"] == ""


def test_log_list_without_card(dev):
    info = dev.log_files()
    assert info["sd"] == "missing"
    assert info["files"] == []


def test_ram_log_explains_the_problem(dev):
    r = dev.get("/api/logs", params={"file": "current"})
    assert r.status_code == 200
    assert "SD card not available" in r.text, r.text[-600:]
    assert "config loaded from flash copy" in r.text
    assert all(LOG_LINE.match(ln) for ln in r.text.splitlines() if ln)


def test_ram_log_tail_works(dev):
    r = dev.get("/api/logs", params={"file": "current", "tail": 512})
    assert r.status_code == 200
    assert 0 < len(r.content) <= 512


def test_old_log_files_are_503(dev):
    assert dev.get("/api/logs", params={"file": "log-0001.txt"}).status_code == 503


def test_ram_log_keeps_the_newest_lines(dev):
    """Fill the RAM buffer (32 KB in PSRAM, 8 KB without it) 1.5 times over; the newest lines must survive."""
    ram = dev.get("/api/status").json()["limits"]["logs"]["ram_bytes"]
    cur = dev.config()
    try:
        for i in range(ram // 40):  # each save logs a ~60 byte line: 1.5 times the buffer
            v = 100 + i
            assert dev.put(with_value(cur, "startup.scenario_delay_s", v)).status_code == 200
        text = dev.get("/api/logs", params={"file": "current"}).text
        assert "config saved (sd=0 flash=1)" in text
        assert text.count("config saved") > 20          # plenty of recent lines kept
        assert len(text.encode()) <= ram
        assert "boot, reset reason" not in text          # the oldest (boot) lines were evicted
    finally:
        dev.put(cur)


def test_put_saves_to_flash_only(dev, baseline):
    cur = baseline["saved"]
    new = cur["startup"]["scenario_delay_s"] + 1
    try:
        r = dev.put(with_value(cur, "startup.scenario_delay_s", new))
        assert r.status_code == 200
        body = r.json()
        assert body["sd"] is False and body["flash"] is True
    finally:
        dev.put(cur)
    assert dev.config() == cur
