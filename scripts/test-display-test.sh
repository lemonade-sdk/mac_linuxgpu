#!/usr/bin/env bash
# display-test.py (the hardware display test runner) against a fake selector.
set -euo pipefail
cd "$(dirname "$0")/.."
python3 -B linuxu/tests/test_display_test.py
