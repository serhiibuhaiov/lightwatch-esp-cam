"""POST /api/ota: firmware update over HTTP into the other app slot, with the bootloader's rollback (see src/ota.h).

By default only refusals: nothing is switched and the board does not restart (an image that is written to the end and
then refused leaves the running firmware as it is). -m restart: real updates with .pio/build/esp32cam/firmware.bin (the
firmware the board runs, built on this computer), each a software restart: the slot changes, the new image waits for its
confirmation (60 s with Wi-Fi) and gets it; a restart asked for over the web confirms it at once.
"""
import hashlib
import os
import socket
import time

import pytest

BIN = os.path.join(os.path.dirname(__file__), "..", ".pio", "build", "esp32cam", "firmware.bin")
OK_HEADERS = {"Content-Type": "application/octet-stream", "X-OTA-Confirm": "1"}


def firmware(dev):
    r = dev.get("/api/status")
    assert r.status_code == 200, r.text
    return r.json()["firmware"], r.json()["uptime_s"]


def image():
    if not os.path.exists(BIN):
        pytest.skip("no .pio/build/esp32cam/firmware.bin: run pio run first")
    data = open(BIN, "rb").read()
    assert data[0] == 0xE9 and hashlib.sha256(data[:-32]).digest() == data[-32:]
    return data


def post(dev, body, headers=OK_HEADERS, timeout=120, **kw):
    return dev.request("POST", "/api/ota", data=body, headers=headers, timeout=timeout, **kw)


@pytest.fixture
def unchanged(dev):
    """The board neither restarts nor switches its slot, and no upload is left running."""
    before, up0 = firmware(dev)
    t0 = time.time()
    yield before
    after, up1 = firmware(dev)
    assert up1 >= up0 + int(time.time() - t0) - 2, "the board restarted"
    assert after["partition"] == before["partition"] and after["image_sha256"] == before["image_sha256"]
    assert after["update"]["running"] is False


def test_status_describes_the_running_firmware(dev):
    fw, _ = firmware(dev)
    assert fw["partition"] in ("app0", "app1") and fw["next_partition"] in ("app0", "app1") and fw["partition"] != fw["next_partition"]
    assert fw["state"] in ("valid", "pending_verify", "undefined", "new")
    assert len(fw["image_sha256"]) == 64 and fw["slot_bytes"] == 0x1E0000
    assert "rolled_back_from" in fw and set(fw["update"]) == {"running", "received", "total"}


def test_ota_needs_auth(dev, unchanged):
    assert post(dev, b"\xe9" + b"\0" * 100, auth=False).status_code == 401


def test_ota_needs_the_confirm_header(dev, unchanged):
    r = post(dev, b"\xe9" + b"\0" * 100, headers={"Content-Type": "application/octet-stream"})
    assert r.status_code == 400 and "X-OTA-Confirm" in r.json()["error"]
    r = post(dev, b"\xe9" + b"\0" * 100, headers={"Content-Type": "application/octet-stream", "X-OTA-Confirm": "yes"})
    assert r.status_code == 400


@pytest.mark.parametrize("ctype", ["application/json", "text/plain", "application/x-www-form-urlencoded"])
def test_ota_needs_an_octet_stream_body(dev, unchanged, ctype):
    r = post(dev, b"\xe9" + b"\0" * 100, headers={"Content-Type": ctype, "X-OTA-Confirm": "1"})
    assert r.status_code == 415, r.text


def test_ota_rejects_an_empty_body(dev, unchanged):
    r = post(dev, b"")
    assert r.status_code == 400 and "empty" in r.json()["error"]


def test_ota_rejects_a_bad_md5_header(dev, unchanged):
    r = post(dev, b"\xe9" + b"\0" * 100, headers=dict(OK_HEADERS, **{"X-OTA-MD5": "xyz"}))
    assert r.status_code == 400 and "MD5" in r.json()["error"]


def test_ota_rejects_what_is_not_an_esp32_image(dev, unchanged):
    r = post(dev, os.urandom(8192).replace(b"\xe9", b"\0"))
    assert r.status_code == 400, r.text
    assert "refused" in r.json()["error"] or "writing" in r.json()["error"]


def test_ota_rejects_a_cut_image(dev, unchanged):
    r = post(dev, image()[:65536])
    assert r.status_code == 400 and "refused" in r.json()["error"], r.text


def test_ota_rejects_an_image_larger_than_the_slot(dev, unchanged):
    slot = unchanged["slot_bytes"]
    r = post(dev, b"\xe9" + b"\0" * slot, timeout=300)  # the board reads the whole body before it answers
    assert r.status_code == 413, r.text


def test_ota_with_a_wrong_md5_writes_everything_and_still_keeps_the_old_firmware(dev, unchanged):
    data = image()
    r = post(dev, data, headers=dict(OK_HEADERS, **{"X-OTA-MD5": "0" * 32}), timeout=300)
    assert r.status_code == 400 and "MD5" in r.json()["error"], r.text
    log = dev.get("/api/logs", params={"file": "current", "tail": 4096}).text
    assert "firmware update started" in log and "the firmware was refused" in log


def test_a_second_upload_waits_its_turn_and_a_dropped_one_frees_the_slot(dev, unchanged):
    data = image()
    host, port = dev.url.split("//")[1].split(":")[0], 80
    auth = __import__("base64").b64encode(f"{dev.auth[0]}:{dev.auth[1]}".encode()).decode()
    s = socket.create_connection((host, port), timeout=10)
    try:
        s.sendall((f"POST /api/ota HTTP/1.1\r\nHost: {host}\r\nAuthorization: Basic {auth}\r\n"
                   f"Content-Type: application/octet-stream\r\nX-OTA-Confirm: 1\r\nContent-Length: {len(data)}\r\n\r\n").encode())
        s.sendall(data[:16384])
        end = time.time() + 10
        while time.time() < end and not firmware(dev)[0]["update"]["running"]:
            time.sleep(0.3)
        assert firmware(dev)[0]["update"]["running"] is True
        r = post(dev, data[:4096])
        assert r.status_code == 409, r.text
    finally:
        s.close()  # the connection goes away mid-upload: the board drops it
    end = time.time() + 10
    while time.time() < end and firmware(dev)[0]["update"]["running"]:
        time.sleep(0.3)
    assert firmware(dev)[0]["update"]["running"] is False
    log = dev.get("/api/logs", params={"file": "current", "tail": 4096}).text
    assert "firmware update dropped" in log and "the connection was closed" in log


# ---- real updates (-m restart) ----------------------------------------------------------------------------------

def _update(dev):
    """Uploads firmware.bin, waits for the board and returns its firmware status right after the start."""
    data = image()
    before, _ = firmware(dev)
    r = post(dev, data, headers=dict(OK_HEADERS, **{"X-OTA-MD5": hashlib.md5(data).hexdigest()}), timeout=300)
    assert r.status_code == 200 and r.json()["ok"] and r.json()["rebooting"] and r.json()["bytes"] == len(data), r.text
    time.sleep(6)
    end = time.time() + 120
    while time.time() < end:
        try:
            fw, up = firmware(dev)
            if up < 120:
                break
        except Exception:
            pass
        time.sleep(2)
    else:
        pytest.fail("the board did not come back after the update")
    assert fw["image_sha256"] == data[-32:].hex(), "another image runs"
    assert fw["partition"] == before["next_partition"] and fw["next_partition"] == before["partition"]
    return fw


@pytest.mark.restart
def test_an_update_switches_the_slot_and_the_new_firmware_confirms_itself(dev):
    fw = _update(dev)
    assert fw["state"] == "pending_verify" and fw["rolled_back_from"] is None
    boot = dev.get("/api/status").json()["boot_history"][-1]
    assert boot["reason"] == "SW"  # power_on scenarios do not start
    end = time.time() + 120
    while time.time() < end and fw["state"] == "pending_verify":
        assert fw["confirm_in_s"] is None or 0 <= fw["confirm_in_s"] <= 60
        time.sleep(3)
        fw = firmware(dev)[0]
    assert fw["state"] == "valid", fw
    log = dev.get("/api/logs", params={"file": "current", "tail": 16384}).text
    assert "this firmware is confirmed (Wi-Fi has been up for 60 s)" in log


@pytest.mark.restart
def test_a_restart_asked_for_over_the_web_confirms_a_new_firmware(dev):
    fw = _update(dev)
    assert fw["state"] == "pending_verify"
    cfg = dev.config()
    assert dev.put(cfg, params={"reboot": "1"}).status_code == 200
    time.sleep(6)
    assert dev.wait_online(90)
    after, up = firmware(dev)
    assert up < 120 and after["partition"] == fw["partition"] and after["image_sha256"] == fw["image_sha256"], "rolled back"
    assert after["state"] == "valid"
