#!/usr/bin/env bash
# The host's GPU-with-displays view (host/DeviceInfo.swift) from the
# properties the dext publishes (linuxu/tests/test_device_info.swift).
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp linuxu/tests/test_device_info.swift "$work/main.swift"
xcrun swiftc -O host/DeviceInfo.swift "$work/main.swift" -o "$work/test_device_info"
"$work/test_device_info"
