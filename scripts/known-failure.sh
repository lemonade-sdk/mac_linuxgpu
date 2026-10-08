#!/usr/bin/env bash
# scripts/known-failure.sh <make target>: exits 0, printing SKIP and the
# reason, when tests/KNOWN_FAILURES.md lists the target; exits 1 otherwise,
# and the recipe runs the test. A recipe calls it as
#   bash scripts/known-failure.sh $@ || <the test>
# An entry marked (CI) is skipped only where CI is set (GitHub Actions sets
# CI=true); elsewhere the test runs. MAC_LINUXGPU_RUN_KNOWN_FAILURES=1 runs
# every listed test.
set -euo pipefail
cd "$(dirname "$0")/.."
target="${1:?usage: scripts/known-failure.sh <make target>}"
[[ -z "${MAC_LINUXGPU_RUN_KNOWN_FAILURES:-}" ]] || exit 1
[[ "$target" =~ ^[a-z0-9-]+$ ]] || exit 1
line=$(grep -m1 -E "^- \`$target\`( |$)" tests/KNOWN_FAILURES.md) || exit 1
case "$line" in
  "- \`$target\` (CI)"*) [[ -n "${CI:-}" ]] || exit 1 ;;
esac
echo "SKIP $target: known failure, see tests/KNOWN_FAILURES.md (${line#*— })"
