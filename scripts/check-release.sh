#!/usr/bin/env bash
#
# scripts/check-release.sh FILE... — fail if a release build carries
# kmemcheck (make check-release passes the dext's library).
#
# kmemcheck (canaries and a table of every kmalloc, linuxu/src/kmem) is
# compiled in only with DEBUG defined (make DEBUG=1, Xcode's Debug
# configuration). Each FILE (an archive, object or dylib) must contain none
# of its symbols. First the patterns are checked against a debug build of
# kmemcheck, so a pattern that stopped matching fails here instead of
# passing silently.
set -euo pipefail
cd "$(dirname "$0")/.."

symbols='_kmemcheck_track|_kmemcheck_untrack|_kmemcheck_scan|_kmemcheck_recs|_kmemcheck_free_slots|_kmemcheck_pattern'

found() { # FILE -> prints the matching symbols
  nm -a "$1" 2>/dev/null | awk '{print $NF}' | grep -E "^($symbols)$" | sort -u || true
}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
clang -std=gnu11 -w -g -DDEBUG=1 -D__KERNEL__ -include linux/autoconf.h -Ilinuxu/headers \
  -c linuxu/src/kmem/kmemcheck.c -o "$work/kmemcheck.o"
if [ -z "$(found "$work/kmemcheck.o")" ]; then
  echo "check-release: the patterns do not match a debug build of kmemcheck; update them" >&2
  exit 1
fi

status=0
for file in "$@"; do
  [ -f "$file" ] || { echo "check-release: $file is missing" >&2; status=1; continue; }
  hits=$(found "$file")
  if [ -n "$hits" ]; then
    echo "check-release: $file carries kmemcheck:" >&2
    printf '  %s\n' $hits >&2
    status=1
  else
    echo "check-release: $file: no kmemcheck"
  fi
done
exit $status
