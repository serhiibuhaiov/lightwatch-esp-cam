"""The OpenAPI description (web/openapi.json, served as /api/openapi.json) must say what the firmware does.

Static checks compare the file with the routes and query parameters in src/web_api.cpp; the live checks only send
requests that cannot change anything (reads, and writes that are rejected before they are applied).
"""
import json
import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SPEC_FILE = ROOT / "web" / "openapi.json"
CODE = (ROOT / "src" / "web_api.cpp").read_text()
METHODS = ("get", "post", "put")


@pytest.fixture(scope="module")
def spec():
    return json.loads(SPEC_FILE.read_text())


def routes_in_code():
    found = set()
    for path, method in re.findall(r'server\.on\("([^"]+)",\s*HTTP_(GET|POST|PUT)', CODE):
        if path in ("/", "/index.html"):  # the page itself
            continue
        found.add((path, method.lower()))
    return found


def routes_in_spec(spec):
    return {(p, m) for p, ops in spec["paths"].items() for m in ops if m in METHODS}


def test_spec_is_valid_openapi(spec):
    assert spec["openapi"].startswith("3.")
    assert spec["info"]["title"] and spec["info"]["version"]
    for path, ops in spec["paths"].items():
        for method, o in ops.items():
            assert method in METHODS, (path, method)
            assert o["summary"] and o["tags"] and "responses" in o, (path, method)
            for p in o.get("parameters", []):
                assert p["in"] == "query" and p["name"] and p["description"], (path, method, p)
            if method == "put":
                assert o["requestBody"]["content"]["application/json"]["example"], (path, method)


def test_every_route_in_the_firmware_is_described_and_the_other_way_round(spec):
    code, doc = routes_in_code(), routes_in_spec(spec)
    assert code - doc == set(), f"routes without a description: {sorted(code - doc)}"
    assert doc - code == set(), f"described, but not in the firmware: {sorted(doc - code)}"


def test_every_query_parameter_is_read_by_the_firmware(spec):
    names = {p["name"] for ops in spec["paths"].values() for o in ops.values() for p in o.get("parameters", [])}
    for n in sorted(names):
        assert f'"{n}"' in CODE, f"parameter {n} is in the description, but web_api.cpp never reads it"


def test_spec_needs_auth_and_is_served(dev, spec):
    assert dev.get("/api/openapi.json", auth=False).status_code == 401
    r = dev.get("/api/openapi.json")
    assert r.status_code == 200 and r.headers["Content-Type"].startswith("application/json")
    assert r.json() == spec, "the firmware serves another openapi.json than the one in the repository: flash it again"


def test_described_reads_exist(dev, spec):
    """Every GET answers (an unknown route would be a 404 with the message 'not found'); parameters are left out
    where they are required, so those answer 400."""
    for path, ops in spec["paths"].items():
        if "get" not in ops:
            continue
        r = dev.get(path)
        not_a_route = r.status_code == 404 and r.json().get("error") == "not found"
        assert not not_a_route, (path, r.text)
        assert r.status_code in (200, 400), (path, r.status_code, r.text)


def test_described_writes_are_rejected_without_the_required_input(dev, spec):
    """POST without `name` is a 400 and PUT without a body is a 400: nothing is changed. (reload has nothing to omit
    and would really reload, so it is covered by the route comparison only.)"""
    for path, ops in spec["paths"].items():
        for method in ("post", "put"):
            o = ops.get(method)
            if not o or path == "/api/devices/reload":
                continue
            r = dev.request(method.upper(), path, headers={"Content-Type": "application/json"} if method == "put" else None)
            assert r.status_code == 400, (method, path, r.status_code, r.text)
