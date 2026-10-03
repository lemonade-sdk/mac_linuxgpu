#!/usr/bin/env bash
# drm-selftest.py (the hardware CS self-test runner) against a fake selector.
set -euo pipefail
cd "$(dirname "$0")/.."
python3 -B linuxu/tests/test_drm_selftest.py
