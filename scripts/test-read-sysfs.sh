#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 -B linuxu/tests/test_read_sysfs.py
