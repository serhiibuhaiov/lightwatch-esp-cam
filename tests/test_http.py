"""Outgoing HTTP requests (POST /api/http/test, the way an HTTP step of a scenario sends one).

The default suite checks the validation (nothing is sent) and sends plain HTTP to a fake server on this computer: the
16 KB body arrives byte for byte, the status, the timeout, a refused connection, a name that does not resolve, and that
the log shows the host but never the path. `-m internet` sends HTTPS to public servers: the certificate check, that
TLS keeps out of the internal RAM, and that a 16 KB body from PSRAM arrives intact over TLS.
"""
import contextlib
import hashlib
import json
import random
import socket
import string
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pytest

from fake_tuya import lan_ip_towards

H = {"Content-Type": "application/json"}


def send(dev, payload, timeout=40):
    body = payload if isinstance(payload, (str, bytes)) else json.dumps(payload)
    return dev.request("POST", "/api/http/test", data=body, headers=H, timeout=timeout)


def wait_until(pred, timeout, step=2.0):
    end = time.time() + timeout
    while time.time() < end:
        if pred():
            return True
        time.sleep(step)
    return pred()


def ascii_text(n, seed=1):
    rnd = random.Random(seed)
    return "".join(rnd.choice(string.ascii_letters + string.digits + " .,:;-_") for _ in range(n))


# ---- validation: nothing leaves the board ------------------------------------------------------------------------------

@pytest.mark.parametrize("payload, words", [
    ({}, "url is required"),
    ({"url": 5}, "url is required"),
    ({"method": "PUT", "url": "http://x/"}, "method"),
    ({"method": "get", "url": "http://x/"}, "method"),
    ({"url": "ftp://x/"}, "http:// or https://"),
    ({"url": "x.example/path"}, "http:// or https://"),
    ({"url": "https://"}, "no host"),
    ({"url": "http:///path"}, "no host"),
    ({"url": "http://x/a b"}, "spaces"),
    ({"url": "http://x/привіт"}, "non-ASCII"),
    ({"url": "http://x/" + "a" * 2040}, "longer than 2048"),
    ({"url": "http://x/", "body": "a"}, "POST only"),
    ({"url": "http://x/", "content_type": "text/plain"}, "POST only"),
    ({"method": "POST", "url": "http://x/", "body": "a" * 16385}, "longer than 16384"),
    ({"method": "POST", "url": "http://x/", "body": {"chat_id": 1}}, "body must be a string"),
    ({"method": "POST", "url": "http://x/", "content_type": ""}, "content_type"),
    ({"method": "POST", "url": "http://x/", "content_type": "a" * 101}, "content_type"),
    ({"url": "http://x/", "timeout_ms": 999}, "timeout_ms"),
    ({"url": "http://x/", "timeout_ms": 30001}, "timeout_ms"),
    ({"url": "http://x/", "timeout_ms": "5000"}, "timeout_ms"),
])
def test_invalid_requests_are_rejected(dev, payload, words):
    r = send(dev, payload)
    assert r.status_code == 400, r.text
    assert words in r.json()["error"], r.text


def test_the_body_must_be_a_json_object(dev):
    for body in ("not json", "[1,2]", '"x"'):
        r = send(dev, body)
        assert r.status_code == 400 and "JSON object" in r.json()["error"], (body, r.text)


def test_needs_auth_and_json(dev):
    assert dev.request("POST", "/api/http/test", auth=False, data="{}", headers=H).status_code == 401
    r = dev.request("POST", "/api/http/test", data='{"url":"http://x/"}',  # a form: the library parses fields, not a body
                    headers={"Content-Type": "application/x-www-form-urlencoded"})
    assert r.status_code == 415, r.text
    r = dev.request("POST", "/api/http/test", data="x" * 49153, headers=H)
    assert r.status_code == 413, r.text


# ---- a fake server on this computer ----------------------------------------------------------------------------------

class Fake:
    """Answers by path: /ok 200 "hello", /status/<n> that status, /slow after 3 s, /echo 200 with the sha256 of the body.
    Keeps every request (method, path, headers, body)."""

    def __init__(self):
        self.requests = []
        fake = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *a):
                pass

            def handle_any(self):
                n = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(n) if n else b""
                fake.requests.append({"method": self.command, "path": self.path, "headers": dict(self.headers), "body": body})
                code, out = 200, b"hello"
                if self.path.startswith("/status/"):
                    code, out = int(self.path.split("/")[2]), b"nope"
                elif self.path == "/slow":
                    time.sleep(3)
                elif self.path == "/echo":
                    out = hashlib.sha256(body).hexdigest().encode()
                elif self.path == "/long":
                    out = b"y" * 3000
                self.send_response(code)
                self.send_header("Content-Length", str(len(out)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(out)

            do_GET = do_POST = handle_any

        self.server = ThreadingHTTPServer(("0.0.0.0", 0), Handler)
        self.server.daemon_threads = True
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def stop(self):
        self.server.shutdown()
        self.server.server_close()


@pytest.fixture(scope="module")
def fake(dev):
    f = Fake()
    host = lan_ip_towards(dev.url.split("//")[1].split(":")[0])
    f.base = f"http://{host}:{f.port}"
    try:
        probe = send(dev, {"url": f.base + "/ok", "timeout_ms": 5000}).json()
        if not probe.get("ok"):
            pytest.skip(f"the board cannot reach this computer on port {f.port} (firewall?): {probe}")
        yield f
    finally:
        f.stop()


def test_get_answers_with_the_status_the_start_of_the_body_and_the_phases(dev, fake):
    r = send(dev, {"url": fake.base + "/ok?x=1"})
    assert r.status_code == 200, r.text
    j = r.json()
    assert j["ok"] is True and j["status"] == 200 and j["method"] == "GET", j
    assert j["response"] == "hello" and j["response_bytes"] == 5 and j["request_bytes"] == 0
    assert j["host"] == fake.base.split("//")[1]
    assert set(j["phases_ms"]) == {"queued", "connect", "send", "wait"}
    assert j["ms"] >= j["phases_ms"]["connect"]
    assert "error" not in j
    got = fake.requests[-1]
    assert got["method"] == "GET" and got["path"] == "/ok?x=1" and got["body"] == b""


def test_a_post_body_of_16_kb_arrives_byte_for_byte(dev, fake):
    body = ascii_text(16384)
    r = send(dev, {"method": "POST", "url": fake.base + "/echo", "content_type": "text/plain; charset=utf-8", "body": body})
    j = r.json()
    assert j["ok"] is True and j["request_bytes"] == 16384, j
    assert j["response"] == hashlib.sha256(body.encode()).hexdigest()
    got = fake.requests[-1]
    assert got["body"] == body.encode() and got["headers"]["Content-Type"] == "text/plain; charset=utf-8"


def test_a_post_without_a_content_type_sends_json(dev, fake):
    text = json.dumps({"chat_id": 1, "text": "Бойлер нагрів воду"}, ensure_ascii=False)
    j = send(dev, {"method": "POST", "url": fake.base + "/echo", "body": text}).json()
    assert j["ok"] is True, j
    got = fake.requests[-1]
    assert got["headers"]["Content-Type"] == "application/json" and got["body"] == text.encode()


def test_only_the_start_of_a_long_answer_is_kept(dev, fake):
    j = send(dev, {"url": fake.base + "/long"}).json()
    assert j["ok"] is True and j["response_bytes"] == 3000 and j["response"] == "y" * 256, j


def test_a_status_other_than_2xx_is_a_failure(dev, fake):
    for code in (301, 404, 500):
        j = send(dev, {"url": fake.base + f"/status/{code}"}).json()
        assert j["ok"] is False and j["status"] == code and j["error"] == "http" and j["response"] == "nope", j


def test_no_answer_within_the_timeout(dev, fake):
    j = send(dev, {"url": fake.base + "/slow", "timeout_ms": 1000}).json()
    assert j["ok"] is False and j["error"] == "timeout" and j["status"] is None, j
    assert j["ms"] < 3000


def test_a_refused_connection(dev, fake):
    s = socket.socket()
    s.bind(("0.0.0.0", 0))
    port = s.getsockname()[1]
    s.close()  # nobody listens there now
    host = fake.base.split("//")[1].split(":")[0]
    j = send(dev, {"url": f"http://{host}:{port}/", "timeout_ms": 3000}).json()
    assert j["ok"] is False and j["error"] in ("connect", "timeout") and j["status"] is None, j


def test_a_name_that_does_not_resolve(dev):
    j = send(dev, {"url": "http://no-such-host.invalid/", "timeout_ms": 5000}).json()
    assert j["ok"] is False and j["error"] == "dns", j


def test_the_log_shows_the_host_but_not_the_path_or_the_query(dev, fake):
    secret = "bot123456:SECRET-" + ascii_text(12, seed=7).replace(" ", "x")
    j = send(dev, {"url": f"{fake.base}/{secret}/sendMessage?chat_id=42&token={secret}"}).json()
    assert j["ok"] is True, j
    log = dev.get("/api/logs", params={"file": "current", "tail": 16384}).text
    assert fake.base.split("//")[1] in log
    assert secret not in log and "sendMessage" not in log


def test_requests_in_a_row_leave_the_internal_ram_where_it_was(dev, fake):
    before = dev.get("/api/status").json()["memory"]["dram_free"]
    for _ in range(10):
        assert send(dev, {"method": "POST", "url": fake.base + "/echo", "body": ascii_text(4000)}).json()["ok"] is True
    # The free RAM breathes (the monitor's polls, the web server, closed sockets waiting out TIME_WAIT): a leak stays.
    after = []
    assert wait_until(lambda: after.append(dev.get("/api/status").json()["memory"]["dram_free"]) or after[-1] > before - 4096, 60), \
        (before, after)


# ---- -m internet: HTTPS to public servers ------------------------------------------------------------------------------

TELEGRAM = "https://api.telegram.org/bot0:test/getMe"  # no such bot: 401/404 after a complete TLS handshake
ECHO = "https://httpbin.org/post"                       # answers with the body it got


@pytest.mark.internet
def test_https_checks_the_certificate_and_keeps_tls_out_of_the_internal_ram(dev):
    if dev.get("/api/status").json()["memory"]["psram_total"] == 0:
        pytest.skip("no PSRAM on this board: TLS takes internal RAM")
    for _ in range(3):
        j = send(dev, {"url": TELEGRAM, "timeout_ms": 15000}).json()
        print(f"\n{json.dumps({k: j[k] for k in ('status', 'ms', 'phases_ms', 'tls', 'memory')})}")
        assert j["status"] in (401, 404), j  # the server answered: the handshake and the certificate check worked
        t, m = j["tls"], j["memory"]
        assert t["psram"] is True and t["peak_psram"] > 16384 and t["peak_internal"] < 4096, t
        assert m["dram_before"] - m["dram_low"] < 16384, m  # ~48 KB of TLS would show here if it took internal RAM
        assert m["stack_free_min"] > 1024, m


@pytest.mark.internet
def test_a_16_kb_body_from_psram_arrives_intact_over_tls(dev):
    body = ascii_text(16384, seed=3)
    j = send(dev, {"method": "POST", "url": ECHO, "content_type": "text/plain", "body": body, "timeout_ms": 20000}).json()
    # Every TLS record carries a MAC: a record that lwIP sent damaged out of PSRAM (as plain answers of the web server
    # once were) would make the server drop the connection, never answer 200.
    assert j["ok"] is True and j["request_bytes"] == 16384, j
    assert body[:64] in j["response"], j  # httpbin repeats the body as "data" near the start of its answer


@pytest.mark.internet
def test_https_with_a_bad_certificate_fails(dev):
    j = send(dev, {"url": "https://self-signed.badssl.com/", "timeout_ms": 15000}).json()
    assert j["ok"] is False and j["error"] == "tls" and j["status"] is None, j
