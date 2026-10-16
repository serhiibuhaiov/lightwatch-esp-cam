#!/usr/bin/env bash
# On-device tests. Needs DEVICE_URL and DEVICE_PASS (see tests/conftest.py).
#   tests/run.sh                  API tests (safe to repeat, config is restored)
#   tests/run.sh -m hardware      restart stress test (needs SERIAL_PORT or POWER_CMD_*)
set -euo pipefail
cd "$(dirname "$0")"
if [ ! -x .venv/bin/pytest ]; then
  python3 -m venv .venv
  .venv/bin/pip install -q -r requirements.txt
fi
exec .venv/bin/pytest "$@"
