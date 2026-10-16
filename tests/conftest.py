"""Shared fixtures for the on-device tests.

Configuration comes from the environment, nothing secret is stored in the repo:
  DEVICE_URL   e.g. http://192.168.8.100          (required)
  DEVICE_PASS  web_password of the device         (required)
  DEVICE_USER  web_user, default "admin"
"""
import contextlib
import copy
import json
import os
import re
import time

import pytest
import requests

# "2026-10-15 14:32:25 I time: ..." or "+3.032s I main: ..."
LOG_LINE = re.compile(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d|\+\d+\.\d{3}s) [DIWE] [^:\s]+: .*$")
LOG_NAME = re.compile(r"^log-\d{4}\.txt$")


class Device:
    def __init__(self, url, user, password):
        self.url = url.rstrip("/")
        self.auth = (user, password)

    def request(self, method, path, auth=True, timeout=10, **kw):
        # A fresh connection per call: the device closes connections and resets.
        return requests.request(method, self.url + path,
                                auth=self.auth if auth else None, timeout=timeout, **kw)

    def get(self, path, **kw):
        return self.request("GET", path, **kw)

    def put_raw(self, body, headers=None, params=None, **kw):
        h = {"Content-Type": "application/json"}
        h.update(headers or {})
        return self.request("PUT", "/api/config", data=body, headers=h, params=params, **kw)

    def put(self, cfg, **kw):
        import json
        return self.put_raw(json.dumps(cfg), **kw)

    def config(self, active=False):
        r = self.get("/api/config", params={"active": "1"} if active else None)
        assert r.status_code == 200, r.text
        return r.json()

    def pending(self):
        r = self.get("/api/config")
        assert r.status_code == 200
        return r.headers["X-Restart-Pending"] == "true"

    def wait_online(self, timeout=60):
        end = time.time() + timeout
        while time.time() < end:
            try:
                if self.get("/api/config", timeout=3).status_code == 200:
                    return True
            except requests.RequestException:
                pass
            time.sleep(1)
        return False

    def log_files(self):
        r = self.get("/api/logs")
        assert r.status_code == 200, r.text
        return r.json()


def with_value(cfg, dotted, value):
    """Deep copy of cfg with cfg['a']['b'] = value for dotted='a.b'."""
    out = copy.deepcopy(cfg)
    node = out
    parts = dotted.split(".")
    for p in parts[:-1]:
        node = node.setdefault(p, {})
    node[parts[-1]] = value
    return out


@pytest.fixture(scope="session")
def dev():
    url, password = os.getenv("DEVICE_URL"), os.getenv("DEVICE_PASS")
    if not url or not password:
        pytest.skip("set DEVICE_URL and DEVICE_PASS to run the on-device tests")
    d = Device(url, os.getenv("DEVICE_USER", "admin"), password)
    if not d.wait_online(15):
        pytest.exit(f"device {url} does not answer", returncode=2)
    return d


@pytest.fixture(scope="session")
def baseline(dev):
    """The saved config at the start of the session, restored at the end."""
    cfg = dev.config()
    pending = dev.pending()
    active = dev.config(active=True)
    yield {"saved": cfg, "pending": pending, "active": active}
    try:
        if dev.config() != cfg:
            dev.put(cfg)
    except requests.RequestException:
        pass


FAKE_NAME = "zz-fake"
FAKE_KEY = "0123456789abcdef"
DEAD_NAME = "zz-dead"
DEAD_IP = "192.168.8.251"  # nobody answers there: a socket that is off


def device_list(dev):
    """The configured devices as a PUT payload (no keys: the stored ones are kept, meters stay as they are)."""
    r = dev.get("/api/devices/config")
    assert r.status_code == 200, r.text
    return [{"name": d["name"], "ip": d["ip"], "version": d["version"], "dp": d["dp"], "ignore": d.get("ignore", False),
             "meters": {k: v for k, v in d["meters"].items() if v is not None}} for d in r.json()["devices"]]


def put_devices(dev, devices):
    return dev.request("PUT", "/api/devices/config", data=json.dumps({"devices": devices}),
                       headers={"Content-Type": "application/json"})


@contextlib.contextmanager
def fake_registered(dev, with_dead=False):
    """Starts a fake Tuya 3.4 device on this computer and registers it on the board as `zz-fake` (and, with
    `with_dead`, a device that nobody answers as `zz-dead`). The device list is put back at the end. Skips the
    test if the board cannot reach this computer (firewall)."""
    from fake_tuya import FakeTuya, lan_ip_towards
    original = device_list(dev)
    assert not any(d["name"] in (FAKE_NAME, DEAD_NAME) for d in original), "leftover test devices in /devices.json"
    fake = FakeTuya(key=FAKE_KEY.encode())
    try:
        fake.start()
    except OSError as e:
        pytest.skip(f"cannot listen on port 6668 here: {e}")
    added = [{"name": FAKE_NAME, "ip": lan_ip_towards(dev.url.split("//")[1].split(":")[0]), "key": FAKE_KEY, "version": "3.4", "dp": 1}]
    if with_dead:
        added.append({"name": DEAD_NAME, "ip": DEAD_IP, "key": FAKE_KEY, "version": "3.4", "dp": 1})
    try:
        r = put_devices(dev, original + added)
        assert r.status_code == 200, r.text
        probe = dev.request("POST", "/api/devices/status", params={"name": FAKE_NAME}, timeout=30).json()
        if not probe.get("ok"):
            pytest.skip(f"the board cannot reach the fake device on this computer (firewall?): {probe}")
        yield fake
    finally:
        put_devices(dev, original)
        fake.stop()
