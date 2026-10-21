"""Outgoing HTTP requests: POST /api/http/test, and the HTTP steps of scenarios (action "http").

The default suite checks the validation (nothing is sent) and sends plain HTTP to a fake server on this computer: the
16 KB body arrives byte for byte, the status, the timeout, a refused connection, a name that does not resolve, and that
the log shows the host but never the path. HTTP steps: validation, the document round trip, runs against the fake
server (what arrives, the step's state and history, failures and retries) and a document near the PUT limit. `-m internet` sends HTTPS to public servers: the certificate check, that
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
from test_scenarios import TEST, guard, listing, put, wait_state  # noqa: F401
from test_interrupt import in_pause, reboot

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
                fake.requests.append({"method": self.command, "path": self.path, "headers": dict(self.headers), "body": body,
                                      "at": time.time()})
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


# ---- HTTP steps in scenarios ---------------------------------------------------------------------------------------------

def http_scenario(name, steps, **kw):
    d = {"name": name, "enabled": False, "trigger": "manual", "default_pause_s": 0, "steps": steps}
    d.update(kw)
    return d


def scn(dev, name, **params):
    r = dev.get("/api/scenarios", params=params)
    assert r.status_code == 200, r.text
    return next(s for s in r.json()["scenarios"] if s["name"] == name)


@pytest.mark.parametrize("step, words", [
    ({"action": "http"}, "url is required"),
    ({"action": "http", "url": "ftp://x/"}, "http:// or https://"),
    ({"action": "http", "url": "http://x/", "device": "towel"}, "device is not for an http step"),
    ({"action": "http", "url": "http://x/", "body": "a"}, "POST only"),
    ({"action": "http", "method": "PUT", "url": "http://x/"}, "method"),
    ({"action": "http", "method": "POST", "url": "http://x/", "body": "a" * 16385}, "longer than 16384"),
    ({"action": "http", "url": "http://x/", "timeout_ms": 500}, "timeout_ms"),
    ({"device": "towel", "action": "on", "url": "http://x/"}, "for an http step"),
    ({"device": "towel", "action": "get"}, "\"on\", \"off\", \"http\" or \"wait\""),
    ({"action": "http", "url": "http://x/", "attempts": 0}, "attempts must be an integer 1..20"),
    ({"action": "http", "url": "http://x/", "attempts": 21}, "attempts must be an integer 1..20"),
    ({"action": "http", "url": "http://x/", "retry_gap_s": 0}, "retry_gap_s must be an integer 1..600"),
    ({"action": "http", "url": "http://x/", "retry_gap_s": 601}, "retry_gap_s must be an integer 1..600"),
    ({"device": "towel", "action": "on", "retry_gap_s": 5}, "for an http step"),
])
def test_invalid_http_steps_are_rejected(dev, guard, step, words):
    r = put(dev, guard + [http_scenario(TEST + "hv", [step])])
    assert r.status_code == 400, r.text
    assert words in r.json()["error"] and "steps[0]" in r.json()["error"], r.text


def test_http_steps_round_trip_with_their_defaults(dev, guard):
    steps = [{"action": "http", "url": "https://hc-ping.com/0000", "pause_after_s": 5},
             {"action": "http", "method": "POST", "url": "https://api.telegram.org/bot1:x/sendMessage", "body": '{"text":"Привіт"}',
              "timeout_ms": 15000, "enabled": False},
             {"action": "http", "method": "POST", "url": "http://10.0.0.5/x", "content_type": "text/plain"}]
    assert put(dev, guard + [http_scenario(TEST + "hrt", steps)]).status_code == 200
    got = scn(dev, TEST + "hrt")["steps"]
    assert got[0] == {"action": "http", "method": "GET", "url": "https://hc-ping.com/0000", "timeout_ms": 10000, "enabled": True, "pause_after_s": 5}
    assert got[1] == {"action": "http", "method": "POST", "url": "https://api.telegram.org/bot1:x/sendMessage", "content_type": "application/json",
                      "body": '{"text":"Привіт"}', "timeout_ms": 15000, "enabled": False, "pause_after_s": None}
    assert got[2]["content_type"] == "text/plain" and got[2]["body"] == ""
    light = scn(dev, TEST + "hrt", state=1)
    assert "steps" not in light and light["state"] == "idle", light
    assert dev.get("/api/scenarios", params={"state": 2}).status_code == 400


def test_a_scenario_with_http_steps_sends_them_and_shows_only_the_host(dev, guard, fake):
    body = ascii_text(16384, seed=11)
    secret = "bot42:SECRET-" + ascii_text(10, seed=12).replace(" ", "x")
    steps = [{"action": "http", "url": f"{fake.base}/ok?{secret}"},
             {"action": "http", "method": "POST", "url": f"{fake.base}/echo/{secret}", "content_type": "text/plain", "body": body}]
    name = TEST + "hrun"
    assert put(dev, guard + [http_scenario(name, steps)]).status_code == 200
    before = len(fake.requests)
    assert dev.request("POST", "/api/scenarios/run", params={"name": name}).status_code == 200
    s = wait_state(dev, name, ("done",), timeout=30)
    got = fake.requests[before:]
    assert [(g["method"], g["path"]) for g in got] == [("GET", f"/ok?{secret}"), ("POST", f"/echo/{secret}")]
    assert got[1]["body"] == body.encode() and got[1]["headers"]["Content-Type"] == "text/plain"
    host = fake.base.split("//")[1]
    assert [x["http"] for x in s["run_steps"]] == [f"GET {host}", f"POST {host}"], s["run_steps"]
    assert all(x["state"] == "ok" and x["detail"].startswith("sent: answered 200 in ") for x in s["run_steps"]), s["run_steps"]
    hist = dev.get("/api/scenarios/history", params={"name": name, "limit": 1}).json()["runs"][0]
    assert [(x["http"], x["action"], x["state"]) for x in hist["steps"]] == [(f"GET {host}", "http", "ok"), (f"POST {host}", "http", "ok")]
    log = dev.get("/api/logs", params={"file": "current", "tail": 16384}).text
    for text in (json.dumps(hist), json.dumps(scn(dev, name, state=1)), log):
        assert secret not in text


def test_a_failing_http_step_is_tried_three_times_and_the_scenario_goes_on(dev, guard, fake):
    name = TEST + "hfail"
    steps = [{"action": "http", "url": f"{fake.base}/status/500", "pause_after_s": 600}, {"action": "http", "url": f"{fake.base}/ok"}]
    assert put(dev, guard + [http_scenario(name, steps)]).status_code == 200
    before = len(fake.requests)
    assert dev.request("POST", "/api/scenarios/run", params={"name": name}).status_code == 200
    s = wait_state(dev, name, ("done",), timeout=40)
    first, second = s["run_steps"]
    assert first["state"] == "failed" and first["attempts"] == 3 and first["detail"] == "http: the server answered 500", first
    assert second["state"] == "ok"  # no 600 s pause after a step that failed
    assert [g["path"] for g in fake.requests[before:]] == ["/status/500"] * 3 + ["/ok"]


def test_an_http_step_with_its_own_attempts_and_gap(dev, guard, fake):
    name = TEST + "hown"
    steps = [{"action": "http", "url": f"{fake.base}/status/500", "attempts": 5, "retry_gap_s": 3, "timeout_ms": 2000}]
    assert put(dev, guard + [http_scenario(name, steps)]).status_code == 200
    got = scn(dev, name)["steps"][0]
    assert got["attempts"] == 5 and got["retry_gap_s"] == 3 and got["timeout_ms"] == 2000, got
    before = len(fake.requests)
    started = time.time()
    assert dev.request("POST", "/api/scenarios/run", params={"name": name}).status_code == 200
    s = wait_state(dev, name, ("done",), timeout=60)
    took = time.time() - started
    assert s["run_steps"][0]["state"] == "failed" and s["run_steps"][0]["attempts"] == 5, s["run_steps"]
    assert [g["path"] for g in fake.requests[before:]] == ["/status/500"] * 5
    assert took >= 4 * 3, took  # four gaps of 3 s
    steps = [{"action": "http", "url": f"{fake.base}/status/500", "attempts": 1}]
    assert put(dev, guard + [http_scenario(name, steps)]).status_code == 200
    before = len(fake.requests)
    assert dev.request("POST", "/api/scenarios/run", params={"name": name}).status_code == 200
    s = wait_state(dev, name, ("done",), timeout=30)
    assert s["run_steps"][0]["attempts"] == 1 and len(fake.requests) - before == 1, s["run_steps"]


def test_a_document_near_the_put_limit_saves_reads_back_and_leaves_the_internal_ram(dev, guard):
    if dev.get("/api/status").json()["memory"]["psram_total"] == 0:
        pytest.skip("no PSRAM: the documents are limited to 48 KB")
    big = [{"action": "http", "method": "POST", "url": f"https://example.com/{i}", "body": ascii_text(16000, seed=100 + i)} for i in range(15)]
    doc = guard + [http_scenario(TEST + "hbig", big)]
    size = len(json.dumps({"scenarios": doc}).encode())
    assert 230000 < size < 262144, size
    t0 = time.time()
    r = put(dev, doc, timeout=60)
    print(f"\nPUT {size} bytes: {r.status_code} in {time.time() - t0:.1f} s")
    assert r.status_code == 200, r.text
    got = scn(dev, TEST + "hbig")["steps"]
    assert [g["body"] for g in got] == [b["body"] for b in big]
    m = dev.get("/api/status").json()["memory"]
    assert m["dram_free"] > 40000 and m["dram_largest"] > 20000, m


# ---- HTTP steps over a restart (-m restart) -------------------------------------------------------------------------------
# /runs.json keeps an HTTP step as its method and host only; a continued run takes the request from the definitions. The API
# cannot read /runs.json, so the third test shows it by its size (/api/status -> scenario_engine.runs_file_bytes).

@pytest.mark.restart
def test_an_http_step_of_a_continued_run_is_sent_as_the_definition_has_it(dev, guard, fake):
    name = TEST + "hres"
    tail = "second?tag=A&token=" + ascii_text(40, seed=21).replace(" ", "x")
    steps = [{"action": "http", "url": f"{fake.base}/ok", "pause_after_s": 45},
             {"action": "http", "method": "POST", "url": f"{fake.base}/{tail}", "content_type": "text/plain", "body": "the body of step 2"}]
    assert put(dev, guard + [http_scenario(name, steps)]).status_code == 200
    assert dev.request("POST", "/api/scenarios/run", params={"name": name}).status_code == 200
    in_pause(dev, name)
    time.sleep(5)  # /runs.json is written at most every 3 s
    before = len(fake.requests)
    reboot(dev)
    s = scn(dev, name)
    assert s["state"] == "running" and s["resumed"] == "SW at step 2" and s.get("pause_left_s"), s
    s = wait_state(dev, name, ("done",), timeout=70)
    assert [x["state"] for x in s["run_steps"]] == ["ok", "ok"], s["run_steps"]
    sent = fake.requests[before:]
    assert [(g["method"], g["path"], g["body"]) for g in sent] == [("POST", "/" + tail, b"the body of step 2")]
    time.sleep(1.5)
    rec = dev.get("/api/scenarios/history", params={"name": name, "limit": 1}).json()["runs"][0]
    assert rec["result"] == "done" and rec["resumed"] == "SW at step 2" and "token" not in json.dumps(rec), rec


@pytest.mark.restart
def test_an_edit_of_an_http_request_during_the_run_starts_it_again_with_the_new_request(dev, guard, fake):
    name = TEST + "hedit"
    steps = [{"action": "http", "url": f"{fake.base}/ok?v=1", "pause_after_s": 600}, {"action": "http", "url": f"{fake.base}/ok?v=2"}]
    assert put(dev, guard + [http_scenario(name, steps)]).status_code == 200
    assert dev.request("POST", "/api/scenarios/run", params={"name": name}).status_code == 200
    in_pause(dev, name)
    edited = [dict(steps[0], url=f"{fake.base}/ok?v=edited"), steps[1]]  # the URL only: the running copy keeps v=1
    assert put(dev, guard + [http_scenario(name, edited)]).status_code == 200
    time.sleep(5)
    before = len(fake.requests)
    reboot(dev)
    end = time.time() + 40
    while time.time() < end and not scn(dev, name).get("pause_left_s"):
        time.sleep(1)
    s = scn(dev, name)
    assert s["state"] == "running" and s["pause_left_s"] > 500, s  # started again from step 1
    assert [g["path"] for g in fake.requests[before:]] == ["/ok?v=edited"]
    time.sleep(1.5)
    rec = dev.get("/api/scenarios/history", params={"name": name, "limit": 1}).json()["runs"][0]
    assert rec["result"] == "interrupted" and "edited" in rec["reason"], rec
    assert dev.request("POST", "/api/scenarios/stop", params={"name": name}).status_code == 200


@pytest.mark.restart
def test_runs_json_does_not_keep_the_paths_of_the_urls(dev, guard, fake):
    """Three runs of 24 HTTP steps with 2 KB URLs, paused after step 1: their URLs alone are ~147 KB, /runs.json as saved is a
    small part of that, and all three are continued after a restart: the paths were not there."""
    names = [TEST + f"hpath{i}" for i in range(3)]
    marker = "/p/"
    def long_url(i, k):
        head = f"{fake.base}{marker}{i}-{k}/"
        return head + "a" * (2048 - len(head))
    docs = [http_scenario(n, [{"action": "http", "url": f"{fake.base}/ok", "pause_after_s": 300}] +
                          [{"action": "http", "url": long_url(i, k)} for k in range(23)]) for i, n in enumerate(names)]
    urls = sum(len(st["url"]) for d in docs for st in d["steps"])
    assert urls > 140000, urls
    assert put(dev, guard + docs, timeout=60).status_code == 200
    for n in names:
        assert dev.request("POST", "/api/scenarios/run", params={"name": n}).status_code == 200
    for n in names:
        in_pause(dev, n)
    time.sleep(5)
    saved = dev.get("/api/status").json()["scenario_engine"]["runs_file_bytes"]
    assert 0 < saved < 30000, saved
    try:
        before = len(fake.requests)
        reboot(dev)
        for n in names:
            s = scn(dev, n, state=1)
            assert s["state"] == "running" and s["resumed"] == "SW at step 2" and s.get("pause_left_s"), s
        assert not any(marker in g["path"] for g in fake.requests[before:])  # still in their pauses
        log = dev.get("/api/logs", params={"file": "current", "tail": 32768}).text
        assert "could not save the alive runs" not in log
    finally:
        for n in names:
            dev.request("POST", "/api/scenarios/stop", params={"name": n})

