"""Restart-during-writes stress test (opt-in: pytest -m hardware).

While a writer thread saves the config over and over (each save also writes a log
line, so the SD card is busy all the time), the board is reset at random moments.
After every reset the device must come back and the config must be intact. At the
end every log file must be readable and (almost) free of torn lines.

How the board is reset (first match wins):
  POWER_CMD_OFF + POWER_CMD_ON  shell commands that cut / restore power, e.g. for a
                                smart plug: curl 'http://plug/relay/0?turn=off'.
                                POWER_OFF_S (default 2) is the pause between them.
  SERIAL_PORT                   toggles RTS (the EN pin) of the USB-UART. This is a
                                hard reset, NOT a power cut: the SD card keeps its
                                power, so it is a weaker test of the card's own
                                state. Stop any serial monitor first.
Optional: CYCLES (10), MAX_DELAY seconds of run time before a reset (40).
With SERIAL_PORT set the boot log is captured and checked as well.
"""
import os
import random
import re
import subprocess
import threading
import time

import pytest
import requests

from conftest import LOG_LINE, with_value

pytestmark = pytest.mark.hardware

CYCLES = int(os.getenv("CYCLES", "10"))
MAX_DELAY = float(os.getenv("MAX_DELAY", "40"))
BAD_SERIAL = re.compile(r"Guru Meditation|abort\(\)|assert failed|Backtrace:|Brownout|stack overflow", re.I)


class SerialCapture:
    """Reads the port in the background and keeps all lines."""

    def __init__(self, port):
        import serial
        self.ser = serial.Serial()
        self.ser.port, self.ser.baudrate, self.ser.timeout = port, 115200, 0.2
        self.ser.dtr = False  # do not reset the board when opening
        self.ser.rts = False
        self.ser.exclusive = True  # fail loudly if a monitor holds the port
        self.ser.open()
        self.lines, self._lock, self._stop = [], threading.Lock(), False
        self._t = threading.Thread(target=self._run, daemon=True)
        self._t.start()

    def _run(self):
        buf = b""
        while not self._stop:
            try:
                buf += self.ser.read(256)
            except Exception:
                time.sleep(0.2)
                continue
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                with self._lock:
                    self.lines.append(line.decode("utf-8", "replace").rstrip("\r"))

    def mark(self):
        with self._lock:
            return len(self.lines)

    def since(self, mark):
        with self._lock:
            return list(self.lines[mark:])

    def hard_reset(self):
        self.ser.dtr = False
        self.ser.rts = True  # EN low
        time.sleep(0.15)
        self.ser.rts = False

    def close(self):
        self._stop = True
        self._t.join(1)
        self.ser.close()


class Rebooter:
    def __init__(self):
        self.off, self.on = os.getenv("POWER_CMD_OFF"), os.getenv("POWER_CMD_ON")
        port = os.getenv("SERIAL_PORT")
        if self.off and self.on:
            self.mode, self.capture = "power", None
        elif port:
            self.mode, self.capture = "rts", SerialCapture(port)
        else:
            pytest.skip("set SERIAL_PORT, or POWER_CMD_OFF and POWER_CMD_ON")

    def reset(self):
        if self.mode == "power":
            subprocess.run(self.off, shell=True, check=True, timeout=20)
            time.sleep(float(os.getenv("POWER_OFF_S", "2")))
            subprocess.run(self.on, shell=True, check=True, timeout=20)
        else:
            self.capture.hard_reset()


class Writer(threading.Thread):
    """Saves the config with an ever increasing startup.scenario_delay_s. A value is
    counted as acknowledged only after HTTP 200, so after any reset the device must
    hold the last acknowledged value or the one that was in flight."""

    def __init__(self, dev, cfg, start):
        super().__init__(daemon=True)
        self.dev, self.cfg, self.acked = dev, cfg, start
        self.paused, self.done = threading.Event(), threading.Event()
        self.paused.set()

    def run(self):
        while not self.done.is_set():
            if self.paused.is_set() or self.acked >= 3500:
                time.sleep(0.1)
                continue
            value = self.acked + 1
            try:
                r = self.dev.put(with_value(self.cfg, "startup.scenario_delay_s", value), timeout=4)
                if r.status_code == 200:
                    self.acked = value
            except requests.RequestException:
                pass
            time.sleep(0.2)


def fetch_all_logs(dev):
    info = dev.log_files()
    out = {}
    for f in info["files"]:
        r = dev.get("/api/logs", params={"file": f["name"]}, timeout=60)
        assert r.status_code == 200, f"{f['name']} unreadable: {r.status_code}"
        out[f["name"]] = r.text
    return out


def test_restarts_during_writes(dev, baseline):
    rebooter = Rebooter()
    saved = baseline["saved"]
    # Stress profile: write the log every second and from the first second on.
    stress = with_value(with_value(saved, "log.quiet_s", 0), "log.flush_s", 1)
    start = 100
    stress = with_value(stress, "startup.scenario_delay_s", start)

    assert dev.put(stress, params={"reboot": "1"}).status_code == 200
    time.sleep(3)
    assert dev.wait_online(60), "device did not come back after applying the stress profile"

    writer = Writer(dev, stress, start)
    writer.start()
    sources, problems = [], []
    try:
        for cycle in range(1, CYCLES + 1):
            writer.paused.clear()
            time.sleep(random.uniform(0.5, MAX_DELAY))

            mark = rebooter.capture.mark() if rebooter.capture else 0
            rebooter.reset()
            writer.paused.set()
            time.sleep(0.5)

            assert dev.wait_online(60), f"cycle {cycle}: device did not come back"
            now = dev.config()
            value = now["startup"]["scenario_delay_s"]
            assert value in (writer.acked, writer.acked + 1), \
                f"cycle {cycle}: config holds {value}, last acknowledged save was {writer.acked}"
            rest = lambda c: with_value(c, "startup.scenario_delay_s", 0)  # noqa: E731
            assert rest(now) == rest(stress), f"cycle {cycle}: config differs from the expected one"
            writer.acked = max(writer.acked, value)  # next saves continue from what is stored

            if rebooter.capture:
                lines = rebooter.capture.since(mark)
                text = "\n".join(lines)
                bad = BAD_SERIAL.search(text)
                assert not bad, f"cycle {cycle}: '{bad.group(0)}' on the serial port"
                src = re.search(r"config loaded from (.+)", text)
                assert src, f"cycle {cycle}: no 'config loaded' line in the boot log"
                sources.append(src.group(1).strip())
                assert "SD mounted" in text, f"cycle {cycle}: SD card was not mounted"
                if sources[-1] not in ("SD card", "SD card (recovered config.tmp)"):
                    problems.append(f"cycle {cycle}: config came from '{sources[-1]}'")
    finally:
        writer.done.set()
        writer.join(5)
        dev.wait_online(60)
        dev.put(saved, params={"reboot": "1"})  # also makes the normal profile active again
        time.sleep(3)
        dev.wait_online(60)
        if rebooter.capture:
            rebooter.capture.close()

    assert not problems, problems

    # Logs: every file readable; each reset may tear at most one line.
    logs = fetch_all_logs(dev)
    torn = [ln for text in logs.values() for ln in text.splitlines() if ln and not LOG_LINE.match(ln)]
    assert len(torn) <= CYCLES, f"{len(torn)} torn lines (allowed {CYCLES}), first: {torn[:3]}"
    assert dev.config() == saved
