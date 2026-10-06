#!/usr/bin/env bash
# Every IOPCIDevice call from the dext is validated before it is made, Close
# is fenced against calls in flight, and a dext that dies closes its PCI
# session first (dext/sources/pci_crash_close.h). dext_main.mm is compiled
# whole against DriverKit substitutes (linuxu/tests/pci_call_mocks.h); no
# DriverKit framework, installed driver or GPU is used.
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/pci-call-guards.XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/include/DriverKit" "$work/include/PCIDriverKit"
for header in IOService IOLib IODispatchQueue IOInterruptDispatchSource; do
  printf '#include "pci_call_mocks.h"\n' > "$work/include/DriverKit/$header.h"
done
for header in IOPCIDevice IOPCIFamilyDefinitions; do
  printf '#include "pci_call_mocks.h"\n' > "$work/include/PCIDriverKit/$header.h"
done
clang -w -std=gnu11 -g -O1 -fsanitize=undefined -fno-sanitize-recover=all -Ilinuxu/headers \
  -c linuxu/src/kmem/device_string.c -o "$work/device_string.o"
# No AddressSanitizer: the crash cases install their own fatal-signal
# handlers, as the dext does.
clang++ -x objective-c++ -std=c++17 -fno-exceptions -fno-objc-exceptions -fblocks \
  -g -O1 -fsanitize=undefined -fno-sanitize-recover=all -DLINUXU_DEXT=1 \
  -I"$work/include" -Ilinuxu/tests -Idext/sources -idirafter linuxu/headers \
  linuxu/tests/test_pci_call_guards.cpp -x none "$work/device_string.o" \
  -lpthread -o "$work/test"
"$work/test" guards
"$work/test" race
# The sanitizer's own deadly-signal handlers stay out of the way, as no
# handler is installed in the dext before its own.
UBSAN_OPTIONS=handle_segv=0:handle_sigbus=0:handle_sigill=0:handle_sigfpe=0:handle_abort=0:halt_on_error=1 \
  "$work/test" crash
