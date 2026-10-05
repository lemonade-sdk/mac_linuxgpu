#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -std=gnu11 -g -O1 -fsanitize=address,undefined -DLINUXU_DEXT=1 \
  -Ilinuxu/headers linuxu/tests/test_dext_compute_production.c \
  dext/sources/dext_compute.c -o "$test_dir/test_dext_compute_production"
"$test_dir/test_dext_compute_production" kfd-without-legacy-hqd
for stage in 1 2 3 4 5 6 7; do
  "$test_dir/test_dext_compute_production" startup "$stage" 0
  "$test_dir/test_dext_compute_production" startup "$stage" 1
done
for scenario in open-failure dispatch-timeout dispatch code-sync bounded-timeout bounded-oom \
  bounded-nospc create-nospc query-topology geometry legacy-one-hqd kfd-two-queues \
  kfd-open-failure kfd-stop kfd-destroy-retained kfd-death-recovered kfd-death-kept \
  kfd-fault-isolated \
  allocation-cleanup create-retained create-oom service-retained kick-poison \
  destroy-retained stop-retained stop-removed ordinary-errors close-retained device-spec \
  client-churn-close client-churn client-records-full host-window-no-session reset-hooks \
  reset-hooks-fail; do
  "$test_dir/test_dext_compute_production" "$scenario"
done
echo 'Production selector startup, ownership and quarantine checks passed'
