#!/usr/bin/env bash
# The display agent's model (host/DisplayAgent.swift) against synthetic
# driver replies and EDIDs (linuxu/tests/test_display_agent.swift).
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp linuxu/tests/test_display_agent.swift "$work/main.swift"
xcrun swiftc -O host/DisplayAgent.swift "$work/main.swift" -o "$work/test_display_agent"
"$work/test_display_agent"
